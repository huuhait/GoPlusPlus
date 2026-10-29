//===- Flow.cpp - Flow typing of Option (ADR-0018 point 4) ----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An Option local or parameter `o` has the type of its value T at the places
// where the checker knows it is set (docs/spec/04-ownership.md OWN-P10):
// after `o != nil` or `!o.IsNil()`, after an early return, break or continue
// on `o == nil`, in the operands of `&&` and `||`, in the body of `for o !=
// nil` and in the post statement of `for n := head; n != nil; n = n.Next`.
// Narrowing ends at an assignment to `o`. Fields and elements are never
// narrowed (they could change between the check and the use), and neither is
// a variable captured by a function literal inside it (CLO-9: it is not a
// local there), one that a function literal assigns, or one whose address is
// taken.
//
// The checker walks the statements in order with one state, Cur: the Option
// variables known to be set (FlowState). Where paths meet (after an if, a
// switch, a loop, at a break target) only what every path knows is kept; a
// path that cannot continue (return, break, continue, goto, panic) is Dead
// and drops out of the meet. The state at a loop top is the meet of the state
// before the loop and the states at its back edges (the end of the body, each
// `continue`, the post statement): see flowLoopHead and flowLoopBack. A label
// that a goto names starts with nothing known. A function literal starts
// empty. The state belongs to one body checker, so it does not depend on the
// thread.
//
// The result is recorded in the side table as the type of each use:
// Info::Types holds T for a narrowed use of `o` (Info::Uses still maps it to
// the Var, whose type is Option[T]); see Info::narrowedType. GIR lowering
// (P04) and the language server (P14) read it from there. The same machinery
// (facts of a condition, kill on assignment, meet at joins) checks the values
// of `v, err := f()` and of the comma-ok forms (P03.18, at the end of this
// file), and a local of type error, any, map or channel checked with `!= nil`
// is a fact too (TYP-15's known non-nil values).
//
// References into the payload. `o = nil` or `move(o)` leaves zero bytes in
// the payload storage, and for a T with no zero value (a never-nil pointer
// inside) a reference kept there would then read nil. So every construct
// that makes a reference to the storage of a place which can outlive the
// statement calls rejectPayloadAddress, which rejects it while the place is
// inside a checked payload by value: `&x` (Expr.cpp), a pointer-receiver method
// value or go / defer call (Call.cpp), slicing an array (Index.cpp) and an `&`
// range variable over an array (Stmt.cpp). A variable that borrows a part of
// the payload (a plain range variable over owning elements of an array, a Get
// binding) counts as that part (PayloadBorrows). Not covered here, because
// they are other rules: a reference into a buffer the payload owns (a
// string, a slice, a map, an interface box) dangles after `o = nil`, which
// is UB-1 as for any owned local; a borrowed variable used after `o = nil`
// is UB-2a; a reference that comes out of a call (a method that returns
// `ref []T` or `*T` into its receiver) is UB-1 (OWN-V7 draws the same line).
// An immediate pointer-receiver call `o.M()` ends with the statement.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "gopp/AST/ASTDumper.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::isa_and_nonnull;

static const ast::Expr *unparen(const ast::Expr *E) {
  while (auto *P = dyn_cast<ParenExpr>(E))
    E = P->X;
  return E;
}

//===----------------------------------------------------------------------===//
// Which variables can be narrowed
//===----------------------------------------------------------------------===//

/// The predeclared nil (not a variable that shadows it).
bool Checker::isNilLit(const ast::Expr *Ex) {
  auto *Id = dyn_cast<ast::Ident>(unparen(Ex));
  return Id && name(Id) == "nil" &&
         llvm::isa_and_nonnull<Nil>(E.Sc->lookup("nil", E.Pos));
}

bool Checker::isNilValue(const ast::Expr *Ex) {
  Ex = unparen(Ex);
  if (isNilLit(Ex))
    return true;
  // `(chan int)(nil)`: a conversion of nil.
  auto *C = dyn_cast<CallExpr>(Ex);
  if (!C || C->Args.size() != 1 || !isNilLit(C->Args[0]))
    return false;
  const TypeAndValue *F = I.lookup(C->Fun);
  return F && F->M == Mode::TypeExpr;
}

/// CON-6a. Every way a local channel can stop being nil, and where each is
/// handled: an assignment of anything (the kills of the statement, also
/// `ch, x := f()`, a range clause, a select receive); a join with a path that
/// assigned (the meet keeps only what every path knows); a loop that assigns
/// it (flowLoopHead); a goto label (the state resets); `&ch`, a
/// pointer-receiver call or method value on ch (an implicit `&ch`), an
/// assignment inside a function literal (narrowable) and any capture by one
/// (ClosureNames): never tracked; a check `ch != nil` (flowAddTo: the path is
/// not known nil); a move. A package variable, a field, a parameter, a named
/// result and an `&` range alias are never tracked.
bool Checker::knownNilChan(const ast::Expr *Ex) {
  if (!own() || Cur.Dead)
    return false;
  auto *Id = dyn_cast<ast::Ident>(unparen(Ex));
  if (!Id)
    return false;
  auto *V = dyn_cast_or_null<Var>(I.Uses.lookup(Id));
  // What may change a local under the flow facts (also an implicit `&ch` of
  // a pointer-receiver call or method value) is decided in narrowable.
  if (!narrowable(V) || V->IsParam || V->Result || ClosureNames.count(V->Name))
    return false;
  return Cur.Nil.count(V) && !Cur.N.count(V);
}

bool Checker::narrowable(const Var *V) {
  // A package variable can change under our feet; so can a variable that a
  // function literal assigns, or whose address is taken (by name, over the
  // whole function). Inside a function literal a captured variable is not a
  // local (CLO-9): another call or goroutine may assign it.
  // An `&` range alias is the element itself (RNG-1a): the collection can
  // change under it (`xs[0] = nil`, a call), so it is a place, not a local.
  // A plain range variable is a copy (or a borrow that RNG-1 keeps unchanged).
  // A plain range variable over owning elements borrows the element, and
  // `move(xs[i])` (also inside a call) leaves the zero value there (RNG-3):
  // it is a place too. A trivial element is a copy and narrows.
  return V && !V->IsPackageLevel && !V->IsField && V->Mode != Var::Alias &&
         !(V->LoopVar && V->Mode == Var::Borrowed) &&
         V->FuncOwner == E.FuncId && !NoNarrowNames.count(V->Name) &&
         !takesImplicitAddress(V);
}

/// `x.M()` / `x.M` / `go x.M()` with a pointer-receiver M takes the address of
/// the variable x as `&x` does (the callee can write x). The scan of the body
/// (AssignScan) records the selector names used on each name; the type of x,
/// known here, tells which of them are pointer-receiver methods.
bool Checker::takesImplicitAddress(const Var *V) {
  auto It = MethodSels.find(V->Name);
  if (It == MethodSels.end() || !V->Ty || !isValid(V->Ty) ||
      isa<PointerType>(under(V->Ty)))
    return false;
  for (auto &Sel : It->second) {
    LookupResult R = lookupFieldOrMethod(V->Ty, true, S.Pkg, Sel.first());
    if (auto *F = dyn_cast_or_null<Func>(R.Obj); F && F->HasPtrRecv)
      return true;
  }
  return false;
}

Type *Checker::narrowedType(const ast::Ident *Id, Var *V, Type *T) {
  if (!own() || !T || Id == NoNarrowExpr)
    return T;
  auto *Op = dyn_cast<OptionType>(under(T));
  if (!Op || !isNarrowed(V))
    return T;
  // Known only if the loop around assumed it: decided at its end, unless the
  // use turns out not to need it (unnarrowTentative).
  if (!Cur.Dead)
    if (auto It = Cur.TentN.find(V); It != Cur.TentN.end()) {
      TentNIndex[Id] = unsigned(TentNUses.size());
      TentNUses.push_back({Id, V, It->second});
    }
  return Op->Elem;
}

Type *Checker::tentOption(const Operand &X) {
  if (!own() || !X.E || X.M == Mode::Invalid)
    return nullptr;
  auto *Id = dyn_cast<ast::Ident>(unparen(X.E));
  if (!Id)
    return nullptr;
  auto It = TentNIndex.find(Id);
  return It == TentNIndex.end() ? nullptr : TentNUses[It->second].V->Ty;
}

bool Checker::unnarrowTentative(Operand &X) {
  Type *OptTy = tentOption(X);
  if (!OptTy)
    return false;
  auto It = TentNIndex.find(llvm::cast<ast::Ident>(unparen(X.E)));
  TentNUses[It->second].Regions.clear(); // needs no assumption now
  TentNIndex.erase(It);
  // The use and its parentheses have the Option's type again.
  for (const ast::Expr *Ex = X.E;;) {
    if (auto TV = I.Types.find(Ex); TV != I.Types.end())
      TV->second.Ty = OptTy;
    auto *P = dyn_cast<ParenExpr>(Ex);
    if (!P)
      break;
    Ex = P->X;
  }
  X.Ty = OptTy;
  return true;
}

Var *Checker::optionVarOf(const ast::Expr *Ex) {
  auto *Id = dyn_cast<ast::Ident>(unparen(Ex));
  if (!Id)
    return nullptr;
  auto *V = dyn_cast_or_null<Var>(I.Uses.lookup(Id));
  if (!V || !V->Ty || !isa<OptionType>(under(V->Ty)) || !narrowable(V))
    return nullptr;
  return V;
}

Var *Checker::guardVarOf(const ast::Expr *Ex) {
  auto *Id = dyn_cast<ast::Ident>(unparen(Ex));
  if (!Id)
    return nullptr;
  auto *V = dyn_cast_or_null<Var>(I.Uses.lookup(Id));
  if (!V || !V->Ty || !narrowable(V))
    return nullptr;
  if (identical(V->Ty, Ctx.errorType()) || allBoolean(V->Ty))
    return V;
  return nullptr;
}

Var *Checker::nilCheckVarOf(const ast::Expr *Ex) {
  if (Var *V = optionVarOf(Ex))
    return V;
  auto *Id = dyn_cast<ast::Ident>(unparen(Ex));
  if (!Id)
    return nullptr;
  auto *V = dyn_cast_or_null<Var>(I.Uses.lookup(Id));
  if (!V || !V->Ty || !narrowable(V) || !hasNil(V->Ty))
    return nullptr;
  Type *U = under(V->Ty);
  if (isa<InterfaceType>(U) || isa<sema::MapType>(U) || isa<sema::ChanType>(U))
    return V;
  return nullptr;
}

Var *Checker::payloadOption(const ast::Expr *Ex) {
  bool Through = false;
  Var *V = rootVar(Ex, Through);
  if (Through || !V)
    return nullptr;
  // A variable that borrows storage inside the payload stands for it. It was
  // recorded while o was narrowed, and it still points into o's storage
  // where o is no longer narrowed (a loop that assigns o, a goto label).
  if (auto It = PayloadBorrows.find(V); It != PayloadBorrows.end())
    return It->second;
  if (!V->Ty || !isa<OptionType>(under(V->Ty)) || !isNarrowed(V))
    return nullptr;
  return V;
}

void Checker::notePayloadBorrow(Var *Borrower, const ast::Expr *Ex) {
  if (Var *V = payloadOption(Ex))
    PayloadBorrows[Borrower] = V;
}

bool Checker::rejectPayloadAddress(const ast::Expr *Ex, const char *What) {
  Var *V = payloadOption(Ex);
  if (!V)
    return false;
  err(Ex, diag::err_option_maybe_nil,
      {std::string(V->Name), std::string(What) +
                                 " keeps the address of the "
                                 "payload of a checked "
                                 "Option: copy " +
                                 std::string(V->Name) + " first"});
  return true;
}

std::string Checker::nilCheckHint(const ast::Expr *Ex) {
  std::string S0 = str(unparen(Ex));
  if (auto *Id = dyn_cast<ast::Ident>(unparen(Ex)))
    if (narrowable(dyn_cast_or_null<Var>(I.Uses.lookup(Id))))
      return "check " + S0 + " != nil first";
  // A name for the copy that does not hide the variable itself.
  const char *Copy = S0 == "v" ? "x" : "v";
  if (auto *Id = dyn_cast<ast::Ident>(unparen(Ex)))
    if (auto *V = dyn_cast_or_null<Var>(I.Uses.lookup(Id));
        V && V->LoopVar && V->Mode == Var::Borrowed)
      return std::string("the range variable borrows its element, and "
                         "move(...) of the element can empty it: range over "
                         "move(the collection) to own the elements, or copy "
                         "one (`if ") +
             Copy + " := clone(" + S0 + "); " + Copy + " != nil { ... }`)";
  // A field, an element, a call or a variable that does not narrow here.
  return std::string("check a local copy: `if ") + Copy + " := " + S0 + "; " +
         Copy + " != nil { ... }`";
}

void Checker::markNoNarrow(const ast::Expr *Ex) {
  if (!own() || !Ex)
    return;
  Ex = unparen(Ex);
  if (isa<ast::Ident>(Ex))
    NoNarrowExpr = Ex;
}

//===----------------------------------------------------------------------===//
// Facts of a condition
//===----------------------------------------------------------------------===//

Checker::FlowState Checker::flowJoin(const FlowState &A, const FlowState &B) {
  if (A.Dead)
    return B;
  if (B.Dead)
    return A;
  FlowState R;
  for (const Var *V : A.Nil)
    if (B.Nil.count(V))
      R.Nil.insert(V);
  for (const Var *V : A.N)
    if (B.N.count(V)) {
      R.N.insert(V);
      // Known on both paths; it holds only if the assumptions of both do.
      auto TA = A.TentN.find(V), TB = B.TentN.find(V);
      if (TA != A.TentN.end() || TB != B.TentN.end()) {
        SmallVector<unsigned, 2> Rs;
        if (TA != A.TentN.end())
          Rs.append(TA->second.begin(), TA->second.end());
        if (TB != B.TentN.end())
          Rs.append(TB->second.begin(), TB->second.end());
        llvm::sort(Rs);
        Rs.erase(std::unique(Rs.begin(), Rs.end()), Rs.end());
        R.TentN[V] = std::move(Rs);
      }
    }
  // A value that may be invalid on either path may be invalid after. Where
  // the paths name different guards, no guard makes it valid.
  for (auto &KV : A.Pend) {
    auto It = B.Pend.find(KV.first);
    R.Pend[KV.first] =
        It == B.Pend.end() || It->second == KV.second ? KV.second : nullptr;
  }
  for (auto &KV : B.Pend)
    if (!A.Pend.count(KV.first))
      R.Pend[KV.first] = KV.second;
  // A value is tentative after the join only if it is tentative on every path
  // where it is pending; it is real if any of their regions makes it real.
  for (auto &KV : R.Pend) {
    const Var *V = KV.first;
    auto TA = A.Tent.find(V), TB = B.Tent.find(V);
    bool InA = A.Pend.count(V), InB = B.Pend.count(V);
    if ((InA && TA == A.Tent.end()) || (InB && TB == B.Tent.end()))
      continue;
    SmallVector<unsigned, 2> Rs;
    if (InA)
      Rs.append(TA->second.begin(), TA->second.end());
    if (InB)
      Rs.append(TB->second.begin(), TB->second.end());
    llvm::sort(Rs);
    Rs.erase(std::unique(Rs.begin(), Rs.end()), Rs.end());
    R.Tent[V] = std::move(Rs);
  }
  return R;
}

void Checker::flowAddTo(FlowState &S,
                        const llvm::SmallPtrSetImpl<const Var *> &Vars) {
  for (const Var *V : Vars) {
    // A checked result named by a fact is valid; an Option or a nilable local
    // is known set. (A channel checked non-nil is not known nil on this path.)
    S.Nil.erase(V);
    if (S.Pend.erase(V)) {
      S.Tent.erase(V);
    } else {
      S.N.insert(V);
      S.TentN.erase(V); // a fact of its own, not an assumption
    }
  }
}

void Checker::flowAdd(const llvm::SmallPtrSetImpl<const Var *> &Vars) {
  flowAddTo(Cur, Vars);
}

void Checker::addGuarded(const Var *G,
                         llvm::SmallPtrSetImpl<const Var *> &Into) {
  for (auto &KV : Cur.Pend)
    if (KV.second == G)
      Into.insert(KV.first);
}

void Checker::flowFacts(const ast::Expr *Cond, FlowFacts &Out) {
  if (!own() || !Cond)
    return;
  flowFactsRec(Cond, Out);
  // `o != nil && take(move(o))`: o is nil again after the move, so the
  // condition says nothing about o afterwards.
  llvm::StringSet<> Moved;
  collectMoved(Cond, Moved);
  if (Moved.empty())
    return;
  for (auto *Set : {&Out.True, &Out.False}) {
    SmallVector<const Var *, 2> Drop;
    for (const Var *V : *Set)
      if (Moved.count(V->Name))
        Drop.push_back(V);
    for (const Var *V : Drop)
      Set->erase(V);
  }
}

void Checker::flowFactsRec(const ast::Expr *Cond, FlowFacts &Out) {
  Cond = unparen(Cond);
  // `ok`: the values that came with ok are valid when it is true.
  if (isa<ast::Ident>(Cond)) {
    if (Var *G = guardVarOf(Cond); G && allBoolean(G->Ty))
      addGuarded(G, Out.True);
    return;
  }
  if (auto *U = dyn_cast<UnaryExpr>(Cond)) {
    if (U->Op == tok::not_) {
      FlowFacts In;
      flowFactsRec(U->X, In);
      Out.True = In.False;
      Out.False = In.True;
    }
    return;
  }
  if (auto *B = dyn_cast<BinaryExpr>(Cond)) {
    if (B->Op == tok::eql || B->Op == tok::neq) {
      // `o != nil` is true when o is set; `o == nil` is false then.
      const ast::Expr *Other = nullptr;
      if (isNilLit(B->Y))
        Other = B->X;
      else if (isNilLit(B->X))
        Other = B->Y;
      if (Other) {
        if (Var *V = nilCheckVarOf(Other))
          (B->Op == tok::neq ? Out.True : Out.False).insert(V);
        // The values that came with err are valid where `err == nil`.
        if (Var *G = guardVarOf(Other); G && !allBoolean(G->Ty))
          addGuarded(G, B->Op == tok::eql ? Out.True : Out.False);
      }
      return;
    }
    if (B->Op == tok::land || B->Op == tok::lor) {
      FlowFacts L, R;
      flowFactsRec(B->X, L);
      flowFactsRec(B->Y, R);
      if (B->Op == tok::land) {
        // True: both are. False: the left is false, or the left is true and
        // the right false.
        Out.True = L.True;
        for (const Var *V : R.True)
          Out.True.insert(V);
        for (const Var *V : L.False)
          if (R.False.count(V) || L.True.count(V))
            Out.False.insert(V);
      } else {
        // False: both are. True: the left is true, or the left is false and
        // the right true.
        Out.False = L.False;
        for (const Var *V : R.False)
          Out.False.insert(V);
        for (const Var *V : L.True)
          if (R.True.count(V) || L.False.count(V))
            Out.True.insert(V);
      }
    }
    return;
  }
  // `o.IsNil()` is false when o is set (Option's own IsNil: the selection
  // tells it from a method of the value with that name).
  if (auto *C = dyn_cast<CallExpr>(Cond)) {
    auto *Sel = dyn_cast<SelectorExpr>(unparen(C->Fun));
    if (!Sel || !C->Args.empty() || name(Sel->Sel) != "IsNil")
      return;
    auto It = I.Selections.find(Sel);
    if (It == I.Selections.end() || !It->second.Recv ||
        !isa<OptionType>(under(It->second.Recv)))
      return;
    if (Var *V = optionVarOf(Sel->X))
      Out.False.insert(V);
  }
}

//===----------------------------------------------------------------------===//
// Assignments
//===----------------------------------------------------------------------===//

namespace {
/// Names assigned (or address-taken) in a subtree, by name: a conservative
/// over-approximation that needs no scopes (a shadowing variable of the same
/// name only makes the analysis more cautious).
struct AssignScan {
  llvm::StringSet<> Assigned;      // anywhere
  llvm::StringSet<> InClosure;     // inside a function literal
  llvm::StringSet<> AddrTaken;     // &name anywhere
  llvm::StringSet<> Gotos;         // goto targets
  llvm::StringSet<> Moved;         // move(name): the variable is nil afterwards
  llvm::StringSet<> ClosureIdents; // every name used inside a function literal
  /// Selector names used on a plain name: `x.M` (see takesImplicitAddress).
  llvm::StringMap<llvm::StringSet<>> MethodSels;
  /// `a, b, g = rhs` with one right-hand side: a possible checked-result or
  /// comma-ok form (values Vals, guard Guard = the last target; "_" for a
  /// target that is not a plain name, a field, an element, `*p`). Whether it
  /// is one is known only when the checker classifies the typed statement
  /// (noteGuardedForm records it in RealForms under the right side Rhs).
  /// Define: a `:=` (it can rebind a variable only in its own scope);
  /// InClosure: inside a function literal. ValInner / GuardInner: the name
  /// was declared earlier inside the scanned statements, in a scope around
  /// the form, so the form binds that inner variable and not the one of the
  /// same name outside.
  struct Form {
    SmallVector<std::string, 2> Vals;
    SmallVector<bool, 2> ValInner;
    std::string Guard;
    bool GuardInner = false;
    bool Define = false, InClosure = false;
    const ast::Expr *Rhs = nullptr; // unparenthesized
    /// The form assigns the variable Nm from outside the scanned statements.
    bool bindsOuter(StringRef Nm) const {
      if (Define)
        return false;
      for (size_t K = 0; K < Vals.size(); ++K)
        if (Vals[K] == Nm && !ValInner[K])
          return true;
      return false;
    }
  };
  SmallVector<Form, 4> Forms;
  llvm::StringSet<> Other; // assigned outside the roles of a form
  /// The names declared inside the scanned statements, one set per open
  /// scope.
  SmallVector<llvm::StringSet<>, 8> Scopes;
  bool declaredInside(StringRef Nm) const {
    for (const llvm::StringSet<> &Sc : Scopes)
      if (Sc.count(Nm))
        return true;
    return false;
  }
  void declare(const ast::Expr *E) {
    while (auto *P = dyn_cast_or_null<ParenExpr>(E))
      E = P->X;
    if (auto *Id = dyn_cast_or_null<ast::Ident>(E); Id && !Scopes.empty())
      Scopes.back().insert(Id->Name.str());
  }
  void addLhs(const ast::Expr *E, llvm::StringSet<> &Into) {
    while (auto *P = dyn_cast_or_null<ParenExpr>(E))
      E = P->X;
    if (auto *Id = dyn_cast_or_null<ast::Ident>(E))
      Into.insert(Id->Name.str());
  }
  void walk(const ast::Node *N, unsigned ClosureDepth) {
    if (!N)
      return;
    bool Scope = isa<BlockStmt>(N) || isa<IfStmt>(N) || isa<ForStmt>(N) ||
                 isa<RangeStmt>(N) || isa<SwitchStmt>(N) ||
                 isa<TypeSwitchStmt>(N) || isa<CaseClause>(N) ||
                 isa<CommClause>(N);
    if (Scope)
      Scopes.emplace_back();
    if (ClosureDepth)
      if (auto *Id = dyn_cast<ast::Ident>(N))
        ClosureIdents.insert(Id->Name.str());
    if (auto *AS = dyn_cast<AssignStmt>(N)) {
      bool IsForm = AS->Lhs.size() >= 2 && AS->Rhs.size() == 1 &&
                    (AS->Op == tok::assign || AS->Op == tok::define);
      if (IsForm) {
        Form F;
        F.Define = AS->Op == tok::define;
        F.InClosure = ClosureDepth != 0;
        F.Rhs = unparen(AS->Rhs[0]);
        for (size_t K = 0; K < AS->Lhs.size(); ++K) {
          auto *Id = dyn_cast<ast::Ident>(unparen(AS->Lhs[K]));
          std::string Nm = Id ? std::string(Id->Name.str()) : std::string("_");
          bool Inner = Id && declaredInside(Nm);
          if (K + 1 == AS->Lhs.size()) {
            F.Guard = Nm;
            F.GuardInner = Inner;
          } else {
            F.Vals.push_back(Nm);
            F.ValInner.push_back(Inner);
          }
        }
        Forms.push_back(std::move(F));
      }
      for (const ast::Expr *L : AS->Lhs) {
        // (A form that is not a checked one stores valid values: it is
        // counted as a possible form, which is the cautious side.)
        if (!IsForm)
          addLhs(L, Other);
        addLhs(L, Assigned);
        if (ClosureDepth)
          addLhs(L, InClosure);
      }
    } else if (auto *ID = dyn_cast<IncDecStmt>(N)) {
      addLhs(ID->X, Assigned);
      addLhs(ID->X, Other);
      if (ClosureDepth)
        addLhs(ID->X, InClosure);
    } else if (auto *RS = dyn_cast<RangeStmt>(N)) {
      if (RS->Op == tok::define) {
        declare(RS->Key);
        declare(RS->Value);
      } else {
        addLhs(RS->Key, Assigned);
        addLhs(RS->Value, Assigned);
        addLhs(RS->Key, Other);
        addLhs(RS->Value, Other);
        if (ClosureDepth) {
          addLhs(RS->Key, InClosure);
          addLhs(RS->Value, InClosure);
        }
      }
    } else if (auto *U = dyn_cast<UnaryExpr>(N)) {
      // `&o` can write o through the pointer at any time.
      if (U->Op == tok::and_) {
        addLhs(U->X, Assigned);
        addLhs(U->X, AddrTaken);
        addLhs(U->X, Other);
      }
    } else if (auto *C = dyn_cast<CallExpr>(N)) {
      if (auto *F = dyn_cast<ast::Ident>(unparen(C->Fun));
          F && F->Name.str() == "move" && C->Args.size() == 1) {
        addLhs(C->Args[0], Moved);
        // A function literal that moves o may run at any time (CLO-9): o is
        // not a local there, exactly as when it assigns o.
        if (ClosureDepth)
          addLhs(C->Args[0], InClosure);
      }
    } else if (auto *Sx = dyn_cast<SelectorExpr>(N)) {
      if (auto *Id = dyn_cast<ast::Ident>(unparen(Sx->X)); Id && Sx->Sel)
        MethodSels[Id->Name.str()].insert(Sx->Sel->Name.str());
    } else if (auto *Br = dyn_cast<BranchStmt>(N)) {
      if (Br->Keyword == tok::kw_goto && Br->Label)
        Gotos.insert(Br->Label->Name.str());
    }
    unsigned D = ClosureDepth + (isa<FuncLit>(N) ? 1 : 0);
    ast::forEachChild(N, [&](const char *, const ast::Node *C) { walk(C, D); });
    // A declaration is in scope after its statement.
    if (auto *AS = dyn_cast<AssignStmt>(N); AS && AS->Op == tok::define)
      for (const ast::Expr *L : AS->Lhs)
        declare(L);
    if (auto *DS = dyn_cast<DeclStmt>(N))
      if (auto *GD = dyn_cast<GenDecl>(DS->D))
        for (const ast::Spec *Sp : GD->Specs)
          if (auto *VS = dyn_cast<ValueSpec>(Sp))
            for (const ast::Ident *Id : VS->Names)
              declare(Id);
    if (Scope)
      Scopes.pop_back();
  }
};
} // namespace

void Checker::flowBeginBody(const BlockStmt *Body) {
  NoNarrowNames.clear();
  ClosureNames.clear();
  MethodSels.clear();
  GotoLabels.clear();
  if (!own() || !Body)
    return;
  AssignScan Scan;
  Scan.walk(Body, 0);
  // The checked results belong to the outermost body (a function literal sees
  // them as captured variables). E is still the enclosing function's here.
  if (!E.FuncId) {
    GuardedVars.clear();
    GuardText.clear();
    CapturedUses.clear();
    RefTaken.clear();
    Rebindings.clear();
    FlowRegions.clear();
    RealForms.clear();
  }
  for (auto &KV : Scan.InClosure)
    NoNarrowNames.insert(KV.first());
  for (auto &KV : Scan.AddrTaken)
    NoNarrowNames.insert(KV.first());
  for (auto &KV : Scan.ClosureIdents)
    ClosureNames.insert(KV.first());
  for (auto &KV : Scan.Gotos)
    GotoLabels.insert(KV.first());
  for (auto &KV : Scan.MethodSels)
    for (auto &Sel : KV.second)
      MethodSels[KV.first()].insert(Sel.first());
  // A goto can bring back any value that a form of this body binds.
  LabelForms.clear();
  LabelRegion = -1;
  if (Scan.Gotos.empty())
    return;
  llvm::SmallPtrSet<const ast::Expr *, 4> Region;
  for (const AssignScan::Form &F : Scan.Forms)
    if (!F.InClosure) {
      Region.insert(F.Rhs);
      for (const std::string &V : F.Vals)
        LabelForms.push_back(V);
    }
  LabelRegion = static_cast<int>(FlowRegions.size());
  FlowRegions.emplace_back();
  FlowRegions.back().Forms = std::move(Region);
}

void Checker::collectAssigned(const ast::Node *N, llvm::StringSet<> &Names) {
  AssignScan Scan;
  Scan.walk(N, 0);
  for (auto &KV : Scan.Assigned)
    Names.insert(KV.first());
  // A move leaves the variable nil: a later iteration must not assume it set.
  for (auto &KV : Scan.Moved)
    Names.insert(KV.first());
}

void Checker::flowMoved(const ast::Expr *Arg) {
  if (!own())
    return;
  if (Var *V = nilCheckVarOf(Arg)) {
    MoveLog.push_back(V);
    if (!Cur.Dead) {
      Cur.N.erase(V);
      Cur.Nil.erase(V);
    }
  }
}

void Checker::collectMoved(const ast::Node *N, llvm::StringSet<> &Names) {
  AssignScan Scan;
  Scan.walk(N, 0);
  for (auto &KV : Scan.Moved)
    Names.insert(KV.first());
}

void Checker::flowKillNames(const llvm::StringSet<> &Names) {
  if (Names.empty() || Cur.Dead)
    return;
  SmallVector<const Var *, 4> Drop;
  for (const Var *V : Cur.N)
    if (Names.count(V->Name))
      Drop.push_back(V);
  for (const Var *V : Drop)
    Cur.N.erase(V);
  Drop.clear();
  for (const Var *V : Cur.Nil)
    if (Names.count(V->Name))
      Drop.push_back(V);
  for (const Var *V : Drop)
    Cur.Nil.erase(V);
}

void Checker::flowFlushKills() {
  for (const Var *V : PendingKills) {
    Cur.N.erase(V);
    Cur.Nil.erase(V);
    // What V guarded is not tied to it any more; V itself is a fresh value.
    Cur.Pend.erase(V);
    Cur.Tent.erase(V);
    for (auto &KV : Cur.Pend)
      if (KV.second == V) {
        KV.second = nullptr;
        // (A value this statement binds again keeps the text of its form.)
        if (!llvm::any_of(PendingGuards,
                          [&](const auto &PG) { return PG.first == KV.first; }))
          untieText(KV.first, V, "was assigned again");
      }
  }
  PendingKills.clear();
  for (const Var *V : PendingNil)
    Cur.Nil.insert(V);
  PendingNil.clear();
  for (auto &PG : PendingGuards) {
    GuardedVars.insert(PG.first);
    Cur.Pend[PG.first] = PG.second;
    Cur.Tent.erase(PG.first);
  }
  PendingGuards.clear();
}

//===----------------------------------------------------------------------===//
// Jumps
//===----------------------------------------------------------------------===//

void Checker::flowPushTarget(bool IsLoop) {
  Targets.emplace_back();
  Targets.back().IsLoop = IsLoop;
  Targets.back().Label = std::move(PendingLabel);
  PendingLabel.clear();
}

Checker::FlowTarget Checker::flowPopTarget() {
  FlowTarget T = std::move(Targets.back());
  Targets.pop_back();
  return T;
}

void Checker::flowBranch(const ast::BranchStmt *B) {
  if (B->Keyword == tok::kw_break) {
    // An unlabeled break leaves the innermost for, switch or select.
    for (size_t K = Targets.size(); K-- > 0;) {
      FlowTarget &T = Targets[K];
      if (B->Label && T.Label != B->Label->Name.str())
        continue;
      T.Break = T.HasBreak ? flowJoin(T.Break, Cur) : Cur;
      T.HasBreak = true;
      break;
    }
  } else if (B->Keyword == tok::kw_continue) {
    // A back edge to the loop (its post statement, then its head): the
    // state here is part of the meet that decides the loop head.
    for (size_t K = Targets.size(); K-- > 0;) {
      FlowTarget &T = Targets[K];
      if (!T.IsLoop || (B->Label && T.Label != B->Label->Name.str()))
        continue;
      T.Cont = T.HasCont ? flowJoin(T.Cont, Cur) : Cur;
      T.HasCont = true;
      break;
    }
  } else if (B->Keyword == tok::kw_fallthrough) {
    FallState = Cur;
    HasFall = true;
  }
  // The label a goto names resets the state.
  flowDead();
}

//===----------------------------------------------------------------------===//
// Checked results and comma-ok values (P03.18, ADR-0018 amendment point 4)
//
// `v, err := f()` for a call whose type ends in `error`, and the comma-ok
// forms `v, ok := m[k]`, `<-ch`, `x.(T)` and `o.Get()`, give v a value that is
// valid only where err is nil / ok is true, when v's type has no zero value
// (a failing call leaves nothing in v; a missing key or a closed channel has
// nothing to give). This is the Option machinery read the other way round:
// FlowState::Pend holds the locals that may be invalid here, each with the
// guard (its err / ok) whose good state makes it valid. A condition on the
// guard is a fact about those values (flowFactsRec), so early exits, `&&` and
// `||`, switches, loops and joins work as they do for an Option; after a
// join a value is pending if it is pending on either path.
//
// Every way to break the rule (lesson 3), each handled in one place:
//  1. v assigned again: a plain assignment stores a valid value; a form makes
//     v pending again, tied to its new guard (flowFlushKills: the kills of
//     the statement, then its guards).
//  2. The guard assigned again (`err = nil`, `x, err := g()`, `err++`, a
//     range clause): the kill unties every value pending on it, so a check
//     of the new err says nothing about an old v (flowFlushKills).
//  3. The guard assigned in a function literal, or its address taken: it is
//     not narrowable, so no fact comes out of it (guardVarOf) and a form ties
//     nothing to it (noteGuardedForm).
//  4. Shadowing: facts and ties are by Var; the scans of 5 and 6 go by name,
//     and a loop scan knows which names the loop declares itself.
//  5. A loop: a later pass comes back to the head (and to a for loop's post
//     statement) with what the loop rebinds in a form, so each such value is
//     pending there, tied to its guard only if every form that binds it names
//     that guard and nothing else assigns it; a pending value whose guard the
//     loop assigns otherwise is untied (flowRebound). This also covers a
//     variable that becomes a checked result only inside the loop (`v := &x;
//     for { use(v); v, err = f() }`).
//  6. A goto: the label starts with nothing known, and every value that a
//     form of the body binds is pending with no guard (flowReset).
//     In 5 and 6 the head or label comes before the forms are checked, so
//     which multi-value assignments are checked forms is not known yet: the
//     scan takes every one as possible, and the value is pending there
//     tentatively (FlowState::Tent, one region per loop or label). Only the
//     checker's own states decide, at the end of the body (a read of a
//     tentative value is reported then, flowDecideTentative, in body order):
//     a loop region is real if a back edge of the loop has the value pending
//     for real, or through a region that is (flowLoopBack, the loop fixpoint
//     of P03.19); a label region is real if one of its forms is a checked
//     form that binds the variable (noteGuardedForm records the checker's
//     classification of the typed statement in RealForms).
//  7. A function literal may run at any time: a captured checked result is
//     never known valid in it (CLO-9, as for an Option), whether the form
//     that binds it comes before or after the literal. Such uses are
//     reported at the end of the outermost body, when every form has been
//     seen (flowEndBody).
//  8. A reference into v's storage that outlives its statement (`&v`, `&v.F`,
//     a pointer-receiver method value or go / defer call, a slice of an array
//     in v, an `&` range variable over one: noteReference, at the places
//     that also guard an Option's payload) would see the value a failed form
//     leaves when v is bound again: such a form is an error (flowEndBody, in
//     either order; a form that declares v makes a new variable).
//  9. v stored where nothing can check it (a field, an element, a package or
//     a captured variable: `s.F, err = f()`), or handed on as they are (the
//     arguments of a call `h(f())`, the results of a function `return f()`,
//     `return o.Get()`, in a plain or a `!` function) into a receiver that is
//     not the same kind of checked form: an error at the store
//     (noteGuardedForm, checkForwarded).
// 10. The error or ok dropped (`v, _ := f()`), or kept where it cannot be
//     checked (a package variable, a variable of another type): v is pending
//     with no guard until it is assigned again.
// 11. `&v`, `move(v)`, `v.F = x` and `v.F += x` read v and are checked like
//     any use; only the variable itself as an assignment target is not a
//     read (NoGuardExpr). The place is empty after `move(v)`, which is the
//     use-after-move rule of P04.09.
//===----------------------------------------------------------------------===//

Checker::GuardKind Checker::guardKindOf(const ast::Expr *Rhs,
                                        ArrayRef<Operand> Ops, bool CommaOk) {
  if (!own() || !Rhs || Ops.size() < 2)
    return GuardKind::None;
  if (CommaOk)
    return GuardKind::Ok;
  auto *Call = dyn_cast<CallExpr>(unparen(Rhs));
  if (!Call)
    return GuardKind::None;
  // Only a last result identical to `error` is a guard. A type parameter
  // that is instantiated with `error` (`call[error](loadN)`) is not seen here:
  // the P04.08 per-instance check (also run by `gopp check`) covers it.
  if (Ops.back().Ty && identical(Ops.back().Ty, Ctx.errorType()))
    return GuardKind::Err;
  // `v, ok := o.Get()` of an Option.
  if (Ops.size() == 2)
    if (auto *Sel = dyn_cast<SelectorExpr>(unparen(Call->Fun))) {
      auto It = I.Selections.find(Sel);
      if (It != I.Selections.end())
        if (auto *F = dyn_cast_or_null<Func>(It->second.Obj);
            F && F->IsBuiltinMethod && F->Name == "Get")
          return GuardKind::Ok;
    }
  return GuardKind::None;
}

/// `return f()`, `return o.Get()` and `h(f())` hand the values of a checked
/// form on as they are. That is safe only when the receiver is itself the
/// same kind of checked form for them: a result list whose last result is
/// `error`, at the place of the call's error, so that the caller's
/// `v, err :=` (or `v := f!()`) checks them. No receiver is a comma-ok form,
/// and the parameters of a call are plain variables of the callee. Otherwise
/// a value that nothing checked is handed on as valid (ERR-3, OWN-P10).
void Checker::checkForwarded(const ast::Expr *Call, ArrayRef<Operand> Ops,
                             Type *LastInto, const char *Into) {
  GuardKind K = guardKindOf(Call, Ops, false);
  if (K == GuardKind::None)
    return;
  if (K == GuardKind::Err && LastInto && identical(LastInto, Ctx.errorType()))
    return;
  bool Err = K == GuardKind::Err;
  std::string Place(Into);
  if (Err && LastInto)
    Place += " (they do not end in error)";
  for (size_t Kx = 0; Kx + 1 < Ops.size(); ++Kx)
    if (Ops[Kx].Ty && isValid(Ops[Kx].Ty) && !hasZeroValue(Ops[Kx].Ty)) {
      err(Call, diag::err_unchecked_store,
          {str(Call), Place, Err ? "err == nil" : "ok is true"});
      return;
    }
}

void Checker::noteGuardedForm(ArrayRef<Var *> Vars,
                              ArrayRef<const ast::Expr *> Places,
                              const ast::Expr *Rhs, ArrayRef<Operand> Ops,
                              bool CommaOk) {
  GuardKind K = guardKindOf(Rhs, Ops, CommaOk);
  if (K == GuardKind::None || Vars.size() != Ops.size() ||
      Places.size() != Ops.size())
    return;
  size_t N = Ops.size();
  Var *G = Vars[N - 1];
  bool Err = K == GuardKind::Err;
  bool GoodGuard =
      G && G->Name != "_" && !G->IsPackageLevel && narrowable(G) &&
      (Err ? identical(G->Ty, Ctx.errorType()) : G->Ty && allBoolean(G->Ty));
  for (size_t Kx = 0; Kx + 1 < N; ++Kx) {
    Type *T = Ops[Kx].Ty;
    if (!T || !isValid(T) || hasZeroValue(T))
      continue;
    Var *V = Vars[Kx];
    bool Blank = V ? V->Name == "_" : !Places[Kx];
    if (Blank)
      continue;
    std::string What = Err ? "err == nil" : "ok is true";
    if (G && GoodGuard)
      What = Err ? std::string(G->Name) + " == nil"
                 : std::string(G->Name) + " is true";
    // Only a local can be checked later; an `&` range alias is the element
    // itself (RNG-1a), so it is a place too.
    if (!V || V->IsPackageLevel || V->FuncOwner != E.FuncId ||
        V->Mode == Var::Alias) {
      std::string Place = Places[Kx] ? str(Places[Kx]) : std::string(V->Name);
      err(Places[Kx] ? Places[Kx] : Rhs, diag::err_unchecked_store,
          {str(Rhs), Place, What});
      continue;
    }
    PendingGuards.push_back({V, GoodGuard ? G : nullptr});
    RealForms.insert({unparen(Rhs), V});
    // The form binds an existing variable again (not one it declares): a
    // reference into it taken anywhere could see the value after a failure.
    // (A read-only binding cannot be assigned at all; that is reported.)
    if (!V->OptionBinding && V->Mode != Var::Borrowed &&
        E.Sc->lookup(V->Name, Rhs->getBegin()) == V)
      Rebindings.push_back({Rhs, V, str(Rhs), What});
    if (GoodGuard)
      GuardText[V] = guardHint(G);
    else if (G && G->Name != "_")
      GuardText[V] = std::string("nothing can check it: its ") +
                     (Err ? "error" : "ok") + " goes into " +
                     std::string(G->Name) + ", which is not a local " +
                     (Err ? "error" : "bool") +
                     " variable that only this function assigns";
    else
      GuardText[V] = std::string("its ") + (Err ? "error" : "ok") +
                     " result is dropped: keep it in a variable and check it";
  }
}

std::string Checker::guardHint(const Var *G) {
  std::string GN(G->Name);
  if (allBoolean(G->Ty))
    return "it is valid only where " + GN + " is true: check `if !" + GN +
           " { return }` first";
  return "it is valid only where " + GN + " == nil: check `if " + GN +
         " != nil { return " + GN + " }` first";
}

void Checker::guardedUse(const ast::Ident *Id, Var *V) {
  if (!own() || Id == NoGuardExpr || !V || V->IsPackageLevel || V->IsField)
    return;
  if (V->FuncOwner != E.FuncId) {
    // Captured by a function literal: decided at the end of the outermost
    // body, when every form is known (point 7).
    if (V->Ty && isValid(V->Ty) && !hasZeroValue(V->Ty))
      CapturedUses.push_back({Id, V});
    return;
  }
  if (Cur.Dead || !Cur.Pend.count(V))
    return;
  // `x.f op= y` checks x twice.
  if (Id == LastGuardErr)
    return;
  LastGuardErr = Id;
  std::string Why = GuardText.lookup(V);
  if (Why.empty())
    Why = "check it first";
  // Pending only if a form of the loop or label region turns out to be a
  // checked one: decided at the end of the body (points 5, 6).
  if (auto It = Cur.Tent.find(V); It != Cur.Tent.end()) {
    TentUses.push_back({Id, V, std::move(Why), It->second});
    return;
  }
  err(Id, diag::err_unchecked_value, {std::string(V->Name), str(V->Ty), Why});
}

void Checker::noteReference(const ast::Expr *Ex) {
  if (!own() || !Ex)
    return;
  bool Through = false;
  if (Var *V = rootVar(Ex, Through); V && !Through && !V->IsPackageLevel)
    RefTaken.try_emplace(V, Ex->getBegin());
}

void Checker::flowEndBody() {
  for (const Rebinding &R : Rebindings)
    if (auto It = RefTaken.find(R.V); It != RefTaken.end()) {
      std::string Name(R.V->Name);
      err(R.At, diag::err_unchecked_store,
          {R.Rhs,
           Name + " (a reference into " + Name + " is kept at line " +
               std::to_string(S.SM.getLineNumber(It->second)) + ")",
           R.What});
    }
  Rebindings.clear();
  RefTaken.clear();
  for (auto &[Id, V] : CapturedUses)
    if (GuardedVars.count(V)) {
      std::string Name(V->Name);
      const char *Copy = Name == "x" ? "v" : "x";
      err(Id, diag::err_unchecked_value,
          {Name, str(V->Ty),
           "a function literal can run at any time, so a checked result is "
           "never known valid in it (CLO-9): after the check, copy it into a "
           "local (`" +
               std::string(Copy) + " := " + Name + "`) and use the copy"});
    }
  CapturedUses.clear();
}

/// Whether V is pending for real at the head of region R: for a label region,
/// if one of its forms is a checked form that binds V (RealForms); for a
/// loop, if a back edge has V pending for real, or pending only through the
/// tentative entry of a region that is real itself. Pending is the least
/// fixpoint (a region never supports itself), so this is whether such a
/// region can be reached from R over those edges: a walk with a set of the
/// regions seen, linear in the regions however deep the loops nest.
bool Checker::regionPendReal(unsigned R, const Var *V) {
  llvm::DenseSet<unsigned> Seen;
  SmallVector<unsigned, 8> Work{R};
  while (!Work.empty()) {
    unsigned Q = Work.pop_back_val();
    if (!Seen.insert(Q).second)
      continue;
    const FlowRegion &Reg = FlowRegions[Q];
    if (!Reg.IsLoop) {
      if (llvm::any_of(Reg.Forms, [&](const ast::Expr *Rhs) {
            return RealForms.count({Rhs, V}) != 0;
          }))
        return true;
      continue;
    }
    if (Reg.PendReal.count(V))
      return true;
    if (auto It = Reg.PendEdge.find(V); It != Reg.PendEdge.end())
      Work.append(It->second.begin(), It->second.end());
  }
  return false;
}

/// Whether the assumption of loop region R that V stays known set fails: a
/// back edge does not know V, or knows it only through the assumption of a
/// region that fails. Known set is the greatest fixpoint (a region may rest
/// on itself), so failing is the least one: whether a region whose back edge
/// does not know V can be reached from R (the same linear walk).
bool Checker::regionNBad(unsigned R, const Var *V) {
  llvm::DenseSet<unsigned> Seen;
  SmallVector<unsigned, 8> Work{R};
  while (!Work.empty()) {
    unsigned Q = Work.pop_back_val();
    if (!Seen.insert(Q).second)
      continue;
    const FlowRegion &Reg = FlowRegions[Q];
    if (Reg.NBad.count(V))
      return true;
    if (auto It = Reg.NEdge.find(V); It != Reg.NEdge.end())
      Work.append(It->second.begin(), It->second.end());
  }
  return false;
}

bool Checker::regionFrontier(ArrayRef<unsigned> Rs, const Var *V, bool Pend,
                             llvm::SmallVectorImpl<unsigned> &Out) {
  // The same walk as regionPendReal / regionNBad, from every region of Rs at
  // once. A closed loop's list is final (and collapsed when it closed), so
  // the walk only goes through the open regions it reaches.
  llvm::SmallDenseSet<unsigned, 8> Seen;
  SmallVector<unsigned, 8> Work(Rs.begin(), Rs.end());
  while (!Work.empty()) {
    unsigned Q = Work.pop_back_val();
    if (!Seen.insert(Q).second)
      continue;
    const FlowRegion &Reg = FlowRegions[Q];
    if (Pend && !Reg.IsLoop) {
      if (llvm::any_of(Reg.Forms, [&](const ast::Expr *Rhs) {
            return RealForms.count({Rhs, V}) != 0;
          }))
        return true;
      Out.push_back(Q); // decided at the end of the body
      continue;
    }
    if (Pend ? Reg.PendReal.count(V) : Reg.NBad.count(V))
      return true;
    if (!Reg.Closed)
      Out.push_back(Q);
    const auto &Edges = Pend ? Reg.PendEdge : Reg.NEdge;
    if (auto It = Edges.find(V); It != Edges.end())
      Work.append(It->second.begin(), It->second.end());
  }
  llvm::sort(Out);
  Out.erase(std::unique(Out.begin(), Out.end()), Out.end());
  return false;
}

void Checker::flowDecideTentative(size_t Top, size_t TopN) {
  for (size_t K = Top; K < TentUses.size(); ++K) {
    const TentUse &U = TentUses[K];
    if (llvm::any_of(U.Regions,
                     [&](unsigned R) { return regionPendReal(R, U.V); }))
      err(U.Id, diag::err_unchecked_value,
          {std::string(U.V->Name), str(U.V->Ty), U.Why});
  }
  TentUses.resize(Top);
  llvm::SmallPtrSet<const ast::Ident *, 4> Seen;
  for (size_t K = TopN; K < TentNUses.size(); ++K) {
    const TentNUse &U = TentNUses[K];
    TentNIndex.erase(U.Id);
    if (llvm::any_of(U.Regions,
                     [&](unsigned R) { return regionNBad(R, U.V); }) &&
        Seen.insert(U.Id).second) {
      std::string Nm(U.V->Name);
      err(U.Id, diag::err_option_maybe_nil,
          {Nm, nilCheckHint(U.Id) + " (the loop assigns " + Nm +
                   ", and a later pass comes back to this use with it "
                   "unchecked)"});
    }
  }
  TentNUses.resize(TopN);
}

Var *Checker::checkableLocal(StringRef Name, SourceLoc Pos) {
  auto *V = dyn_cast_or_null<Var>(E.Sc->lookup(Name, Pos));
  if (!V || V->IsPackageLevel || V->IsField || V->FuncOwner != E.FuncId ||
      !V->Ty || !isValid(V->Ty) || hasZeroValue(V->Ty))
    return nullptr;
  return V;
}

void Checker::untieText(const Var *V, const Var *G, StringRef What) {
  std::string GN(G->Name);
  GuardText[V] = GN + " " + What.str() + ", so it no longer tells whether " +
                 std::string(V->Name) + " is valid: check " + GN +
                 " before that";
}

void Checker::flowRebound(ArrayRef<const ast::Node *> Ns, SourceLoc Pos,
                          int Region) {
  if (!own() || Cur.Dead)
    return;
  AssignScan Scan;
  for (const ast::Node *N : Ns)
    Scan.walk(N, 0);
  if (Scan.Forms.empty() && Cur.Pend.empty())
    return;
  // Which multi-value assignments of Ns are checked forms is known only when
  // the checker classifies them (noteGuardedForm). One thing is sure before:
  // when the last target is a variable of a declared type that neither an
  // `error` value nor an untyped bool ok is assignable to (the checker's own
  // assignability, so `Option[bool]` or `Option[IE]` takes one, ADR-0018
  // point 3), the assignment is a plain one: a checked form there is a type
  // error, reported at the assignment. Its targets are assigned otherwise.
  llvm::erase_if(Scan.Forms, [&](const AssignScan::Form &F) {
    if (F.Guard == "_" || F.GuardInner)
      return false;
    auto *G = dyn_cast_or_null<Var>(E.Sc->lookup(F.Guard, Pos));
    if (!G || !G->Ty || !isValid(G->Ty) || isa<TypeParamType>(G->Ty))
      return false;
    Operand ErrV, OkV;
    ErrV.M = OkV.M = Mode::Value;
    ErrV.Ty = Ctx.errorType();
    OkV.Ty = typ(BasicKind::UntypedBool);
    if (assignableTo(ErrV, G->Ty, nullptr) || assignableTo(OkV, G->Ty, nullptr))
      return false;
    for (const std::string &Nm : F.Vals)
      Scan.Other.insert(Nm);
    Scan.Other.insert(F.Guard);
    return true;
  });
  // Every other one may be a checked form: a value it binds from outside is
  // pending here tentatively, for the region of the loop (point 5). A
  // `:=` there, or a name declared inside, binds an inner variable; a form
  // inside a function literal binds no local of this body (an error, E0351).
  llvm::StringSet<> Rebound;
  for (const AssignScan::Form &F : Scan.Forms)
    if (!F.InClosure)
      for (const std::string &Nm : F.Vals)
        if (Nm != "_" && F.bindsOuter(Nm))
          Rebound.insert(Nm);
  // A pending value whose guard N assigns, and that N does not rebind: the
  // new guard says nothing about it.
  for (auto &KV : Cur.Pend)
    if (KV.second && Scan.Assigned.count(KV.second->Name) &&
        !Rebound.count(KV.first->Name)) {
      untieText(KV.first, KV.second, "is assigned again in the loop");
      KV.second = nullptr;
    }
  for (auto &Entry : Rebound) {
    StringRef Nm = Entry.first();
    Var *V = checkableLocal(Nm, Pos);
    if (!V)
      continue;
    // Tied to the guard only if every form that binds v names that same
    // guard from outside, no form binds that guard without v or as a value,
    // and nothing else assigns it.
    std::string Guard;
    bool Conflict = false;
    for (const AssignScan::Form &F : Scan.Forms)
      if (!F.InClosure && F.bindsOuter(Nm)) {
        Conflict = Conflict || F.GuardInner;
        if (Guard.empty())
          Guard = F.Guard;
        else if (Guard != F.Guard)
          Conflict = true;
      }
    const Var *GV = nullptr;
    if (!Conflict && Guard != "_" && !Scan.Other.count(Guard)) {
      bool Stable = true;
      for (const AssignScan::Form &F : Scan.Forms)
        if ((F.Guard == Guard && !F.GuardInner && !F.Define &&
             !F.bindsOuter(Nm)) ||
            F.bindsOuter(Guard))
          Stable = false;
      auto *O = dyn_cast_or_null<Var>(E.Sc->lookup(Guard, Pos));
      if (Stable && O && narrowable(O) &&
          (identical(O->Ty, Ctx.errorType()) || allBoolean(O->Ty)))
        GV = O;
    }
    // Pending before with another guard: no one guard makes it valid.
    auto It = Cur.Pend.find(V);
    bool Was = It != Cur.Pend.end();
    if (Was && It->second != GV)
      GV = nullptr;
    Cur.Pend[V] = GV;
    // A value pending for real before stays real; a new or tentative one is
    // tentative for this region too: the back edges of the loop decide it
    // (flowLoopBack).
    if (!Was || Cur.Tent.count(V)) {
      SmallVector<unsigned, 2> &Rs = Cur.Tent[V];
      if (!llvm::is_contained(Rs, unsigned(Region)))
        Rs.push_back(unsigned(Region));
      FlowRegions[Region].PendEdge.try_emplace(V);
    }
    GuardText[V] =
        GV ? guardHint(GV)
           : "the loop assigns it again from a call that can fail or a "
             "comma-ok form, so a later pass may read a value that nothing "
             "checked: check it right after that assignment";
  }
}

/// The head of a loop is the meet of the state before it and the states at its
/// back edges. The back edges are known only after the body is checked, so
/// the head starts from the entry state with the assumptions the body can
/// break, each tagged with the loop's region: a local the loop assigns is
/// assumed to keep what the entry knows (known set), and a checked result the
/// loop rebinds with a possible checked form is assumed pending. Uses that
/// rest on an assumption are recorded; flowLoopBack decides the assumptions
/// from the real states at the back edges, and flowDecideTentative reports
/// the uses at the end of the body. Returns the region (-1: no flow typing).
int Checker::flowLoopHead(const ast::Node *Loop, SourceLoc BodyPos) {
  if (!own() || Cur.Dead)
    return -1;
  // The key and value that `for k, v = range` assigns come in at the top of
  // each iteration, not over a back edge.
  if (auto *RS = dyn_cast<RangeStmt>(Loop); RS && RS->Op != tok::define) {
    llvm::StringSet<> Names;
    if (auto *Id = dyn_cast_or_null<ast::Ident>(RS->Key))
      Names.insert(Id->Name.str());
    if (auto *Id = dyn_cast_or_null<ast::Ident>(RS->Value))
      Names.insert(Id->Name.str());
    flowKillNames(Names);
  }
  int Region = static_cast<int>(FlowRegions.size());
  FlowRegions.emplace_back();
  FlowRegions.back().IsLoop = true;
  llvm::StringSet<> Assigned, Moved;
  collectAssigned(Loop, Assigned);
  // A channel that the loop assigns may be made by an earlier pass.
  Cur.Nil.remove_if([&](const Var *V) { return Assigned.count(V->Name) != 0; });
  // A move in the loop leaves the variable nil for the next pass (the use
  // that finds it nil is reported where it is, with the move in its text).
  collectMoved(Loop, Moved);
  flowKillNames(Moved);
  for (const Var *V : Cur.N)
    if (Assigned.count(V->Name)) {
      SmallVector<unsigned, 2> &Rs = Cur.TentN[V];
      Rs.push_back(unsigned(Region));
      llvm::sort(Rs);
      Rs.erase(std::unique(Rs.begin(), Rs.end()), Rs.end());
      FlowRegions[Region].NEdge.try_emplace(V);
    }
  // A for loop's init statement runs once, before the head.
  if (auto *F = dyn_cast<ForStmt>(Loop))
    flowRebound({F->Cond, F->Post, F->Body}, BodyPos, Region);
  else
    flowRebound({Loop}, BodyPos, Region);
  return Region;
}

/// Back is the state at the end of one iteration: the end of the body, every
/// `continue`, and the post statement. A value the loop assumed pending is
/// pending at the head if it is pending there; one it assumed known set is not
/// known at the head if it is not known there.
void Checker::flowLoopBack(int Region, const FlowState &Back) {
  if (Region < 0 || Back.Dead)
    return;
  FlowRegion &Reg = FlowRegions[Region];
  for (auto &KV : Reg.PendEdge) {
    const Var *V = KV.first;
    if (!Back.Pend.count(V))
      continue;
    auto T = Back.Tent.find(V);
    if (T == Back.Tent.end())
      Reg.PendReal.insert(V);
    else
      KV.second = T->second;
  }
  for (auto &KV : Reg.NEdge) {
    const Var *V = KV.first;
    if (!Back.N.count(V)) {
      Reg.NBad.insert(V);
      continue;
    }
    if (auto T = Back.TentN.find(V); T != Back.TentN.end())
      KV.second = T->second;
  }
}

/// The loop and the loops inside it are decided by now (also an inner one a
/// `break` to an outer label comes from); a loop around it may still make a
/// region fail later, and the uses wait for that (flowDecideTentative). A
/// failed region stays failed, so the state after the loop can drop a fact
/// that rests on one at once: the code after the loop sees the variable
/// unchecked, as before the loop fixpoint, and no type there depends on it.
void Checker::flowLoopClean(int Region, FlowState &S) {
  if (Region < 0)
    return;
  // The loop is closed: no back edge comes any more. Decide its region once
  // for each variable: a failure (or, for a pending value, support) is
  // recorded in the region itself; otherwise its list becomes the open
  // regions it rests on, so a closed region is one step from them (R17).
  FlowRegion &Reg = FlowRegions[Region];
  Reg.Closed = true;
  SmallVector<unsigned, 4> Out;
  for (bool Pend : {false, true}) {
    auto &Edges = Pend ? Reg.PendEdge : Reg.NEdge;
    SmallVector<const Var *, 4> Vars;
    for (auto &KV : Edges)
      Vars.push_back(KV.first);
    for (const Var *V : Vars) {
      Out.clear();
      unsigned Self = unsigned(Region);
      if (regionFrontier(Self, V, Pend, Out)) {
        (Pend ? Reg.PendReal : Reg.NBad).insert(V);
        Edges[V].clear();
      } else {
        Edges[V].assign(Out.begin(), Out.end());
      }
    }
  }
  if (S.Dead)
    return;
  // The state after the loop rests on the open regions only. A failed region
  // stays failed, so a fact that rests on one is dropped at once: the code
  // after the loop sees the variable unchecked, as before the loop fixpoint,
  // and no type there depends on it. A fact that rests on nothing open holds
  // for good; a pending value that rests on nothing open is pending on
  // nothing (an empty list: no use of it is reported).
  SmallVector<const Var *, 2> Failed, Holds;
  for (auto &KV : S.TentN) {
    Out.clear();
    if (regionFrontier(KV.second, KV.first, false, Out)) {
      if (S.N.count(KV.first))
        Failed.push_back(KV.first);
      continue;
    }
    KV.second.assign(Out.begin(), Out.end());
    if (KV.second.empty())
      Holds.push_back(KV.first);
  }
  for (const Var *V : Failed) {
    S.N.erase(V);
    S.TentN.erase(V);
  }
  for (const Var *V : Holds)
    S.TentN.erase(V); // a fact of its own now
  for (auto &KV : S.Tent) {
    Out.clear();
    if (!regionFrontier(KV.second, KV.first, true, Out)) {
      KV.second.assign(Out.begin(), Out.end());
      continue;
    }
    // Real: one region that makes it so is enough.
    for (unsigned R : KV.second) {
      Out.clear();
      if (regionFrontier(R, KV.first, true, Out)) {
        KV.second.assign(1, R);
        break;
      }
    }
  }
}

void Checker::flowReset(SourceLoc Pos) {
  Cur = FlowState();
  for (const Var *V : GuardedVars)
    Cur.Pend[V] = nullptr;
  // A value that a multi-value assignment of the body binds is pending
  // tentatively, until the checker knows which of them are checked forms
  // (point 6).
  for (const std::string &Val : LabelForms)
    if (Var *V = checkableLocal(Val, Pos);
        V && LabelRegion >= 0 && !Cur.Pend.count(V)) {
      Cur.Pend[V] = nullptr;
      Cur.Tent[V] = {unsigned(LabelRegion)};
    }
  for (auto &KV : Cur.Pend)
    GuardText[KV.first] =
        "a goto can come back to the label above with a value that a call "
        "that can fail or a comma-ok form gave it: check it after the label";
}

//===----------------------------------------------------------------------===//
// Never-nil @Required fields (TYP-15's list of known non-nil values)
//===----------------------------------------------------------------------===//

bool Checker::knownNonNil(const ast::Expr *Ex, Type *Target, Type *SrcTy) {
  Ex = unparen(Ex);
  Type *S = SrcTy ? SrcTy : I.typeOf(Ex);
  if (!S || !isValid(S))
    return true; // reported elsewhere
  // One result of a call: a call says nothing about its results.
  if (SrcTy && isa<CallExpr>(Ex) && !isBuiltinCall(Ex, BuiltinId::Make)) {
    const TypeAndValue *CT = I.lookup(Ex);
    if (CT && isa<TupleType>(CT->Ty))
      return false;
  }
  Type *TU = Target ? under(Target) : nullptr;
  bool ToIface = TU && isa<InterfaceType>(TU);
  // A literal, make(...), a function literal.
  if (isa<CompositeLit>(Ex) || isa<FuncLit>(Ex))
    return true;
  if (auto *C = dyn_cast<CallExpr>(Ex)) {
    if (isBuiltinCall(Ex, BuiltinId::Make))
      return true;
    // A conversion keeps the question for its operand.
    const TypeAndValue *FT = I.lookup(C->Fun);
    if (FT && FT->M == Mode::TypeExpr && C->Args.size() == 1) {
      Type *CT = I.typeOf(Ex);
      if (CT && isa<InterfaceType>(under(CT)))
        return knownNonNil(C->Args[0], CT);
    }
  }
  // A read of another never-nil field.
  if (const Var *F = selectedField(Ex); F && isNeverNilField(F))
    return true;
  // A local checked with != nil.
  if (auto *Id = dyn_cast<ast::Ident>(Ex)) {
    auto *V = dyn_cast_or_null<Var>(I.Uses.lookup(Id));
    // (Not one that only a loop's assumption makes known.)
    if (V && narrowable(V) && Cur.N.count(V) && !Cur.TentN.count(V))
      return true;
    if (V && Cur.Dead && narrowable(V))
      return true;
  }
  // Any value of a type that has no nil of its own, converted to an
  // interface (error, any): a pointer, a func, an interface with methods, or
  // a concrete value (a nil slice or map inside an interface is not a nil
  // interface).
  if (ToIface) {
    Type *SU = under(S);
    if (isa<OptionType>(SU) || S == typ(BasicKind::UntypedNil))
      return false;
    if (isa<InterfaceType>(SU))
      return neverNilKind(S) != nullptr;
    return true;
  }
  return false;
}

void Checker::checkNeverNilStore(const Operand &X, const Var *Fld,
                                 const std::string &Place) {
  if (!own() || !X.E || X.M == Mode::Invalid || X.isNil() ||
      !isNeverNilField(Fld))
    return;
  if (knownNonNil(X.E, Fld->Ty, X.Ty))
    return;
  err(X, diag::err_required_maybe_nil,
      {str(X.E), Place, nilCheckHint(X.E), requiredWhy(Fld)});
}
