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
// and drops out of the meet. A loop starts from the state before it minus the
// names the loop assigns (a later iteration comes back to the top). A label
// that a goto names starts with nothing known. A function literal starts
// empty. The state belongs to one body checker, so it does not depend on the
// thread.
//
// The result is recorded in the side table as the type of each use:
// Info::Types holds T for a narrowed use of `o` (Info::Uses still maps it to
// the Var, whose type is Option[T]); see Info::narrowedType. GIR lowering
// (P04) and the language server (P14) read it from there. The same machinery
// (facts of a condition, kill on assignment, meet at joins) is what P04.09's
// flow checks need for comma-ok (`!ok` is a move) and for `v, err := f()`.
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
         V->FuncOwner == E.FuncId && !NoNarrowNames.count(V->Name);
}

Type *Checker::narrowedType(const ast::Ident *Id, Var *V, Type *T) {
  if (!own() || !T || Id == NoNarrowExpr)
    return T;
  auto *Op = dyn_cast<OptionType>(under(T));
  if (!Op || !isNarrowed(V))
    return T;
  return Op->Elem;
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
  for (const Var *V : A.N)
    if (B.N.count(V))
      R.N.insert(V);
  return R;
}

void Checker::flowAdd(const llvm::SmallPtrSetImpl<const Var *> &Vars) {
  for (const Var *V : Vars)
    Cur.N.insert(V);
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
      if (Other)
        if (Var *V = optionVarOf(Other))
          (B->Op == tok::neq ? Out.True : Out.False).insert(V);
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
  llvm::StringSet<> Assigned;  // anywhere
  llvm::StringSet<> InClosure; // inside a function literal
  llvm::StringSet<> AddrTaken; // &name anywhere
  llvm::StringSet<> Gotos;     // goto targets
  llvm::StringSet<> Moved;     // move(name): the variable is nil afterwards
  void addLhs(const ast::Expr *E, llvm::StringSet<> &Into) {
    while (auto *P = dyn_cast_or_null<ParenExpr>(E))
      E = P->X;
    if (auto *Id = dyn_cast_or_null<ast::Ident>(E))
      Into.insert(Id->Name.str());
  }
  void walk(const ast::Node *N, unsigned ClosureDepth) {
    if (!N)
      return;
    if (auto *AS = dyn_cast<AssignStmt>(N)) {
      for (const ast::Expr *L : AS->Lhs) {
        addLhs(L, Assigned);
        if (ClosureDepth)
          addLhs(L, InClosure);
      }
    } else if (auto *ID = dyn_cast<IncDecStmt>(N)) {
      addLhs(ID->X, Assigned);
      if (ClosureDepth)
        addLhs(ID->X, InClosure);
    } else if (auto *RS = dyn_cast<RangeStmt>(N)) {
      if (RS->Op != tok::define) {
        addLhs(RS->Key, Assigned);
        addLhs(RS->Value, Assigned);
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
    } else if (auto *Br = dyn_cast<BranchStmt>(N)) {
      if (Br->Keyword == tok::kw_goto && Br->Label)
        Gotos.insert(Br->Label->Name.str());
    }
    unsigned D = ClosureDepth + (isa<FuncLit>(N) ? 1 : 0);
    ast::forEachChild(N, [&](const char *, const ast::Node *C) { walk(C, D); });
  }
};
} // namespace

void Checker::flowBeginBody(const BlockStmt *Body) {
  NoNarrowNames.clear();
  GotoLabels.clear();
  if (!own() || !Body)
    return;
  AssignScan Scan;
  Scan.walk(Body, 0);
  for (auto &KV : Scan.InClosure)
    NoNarrowNames.insert(KV.first());
  for (auto &KV : Scan.AddrTaken)
    NoNarrowNames.insert(KV.first());
  for (auto &KV : Scan.Gotos)
    GotoLabels.insert(KV.first());
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
  if (Var *V = optionVarOf(Arg)) {
    MoveLog.push_back(V);
    if (!Cur.Dead)
      Cur.N.erase(V);
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
}

void Checker::flowFlushKills() {
  for (const Var *V : PendingKills)
    Cur.N.erase(V);
  PendingKills.clear();
}

//===----------------------------------------------------------------------===//
// Jumps
//===----------------------------------------------------------------------===//

void Checker::flowPushTarget() {
  Targets.emplace_back();
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
  } else if (B->Keyword == tok::kw_fallthrough) {
    FallState = Cur;
    HasFall = true;
  }
  // A continue goes back to a loop head and post statement that assume only
  // what holds on every path (see forStmt); the label a goto names resets
  // the state.
  flowDead();
}
