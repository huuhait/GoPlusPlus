//===- Escape.cpp - Which closures escape, which variables need a cell ----===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Escape.h"

#include "gopp/AST/ASTDumper.h"

#include "llvm/Support/Casting.h"

#include <cstdint>

namespace gopp {
namespace gir {
namespace impl {

/// (LowerImpl.h) The steps of the closure analysis, read by gir::escapeSteps.
extern thread_local uint64_t EscapeSteps;

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::SmallVector;

namespace {

/// Calls Visit(Node, Exit) for every node under Root, parents before children
/// (Exit false) and again after the children (Exit true), with Path holding
/// the nodes from Root to the one visited. One explicit stack: any depth.
template <class F> void walk(const ast::Node *Root, F &&Visit) {
  struct Item {
    const ast::Node *N;
    bool Exit;
  };
  SmallVector<Item, 64> Stack{{Root, false}};
  SmallVector<const ast::Node *, 8> Kids;
  while (!Stack.empty()) {
    Item It = Stack.pop_back_val();
    ++EscapeSteps;
    Visit(It.N, It.Exit);
    if (It.Exit)
      continue;
    Stack.push_back({It.N, true});
    Kids.clear();
    ast::forEachChild(
        It.N, [&](const char *, const ast::Node *C) { Kids.push_back(C); });
    for (auto K = Kids.rbegin(); K != Kids.rend(); ++K)
      Stack.push_back({*K, false});
  }
}

const ast::Expr *unparen(const ast::Expr *E) {
  while (auto *P = dyn_cast_or_null<ast::ParenExpr>(E))
    E = P->X;
  return E;
}

bool isNil(const S::Info &I, const ast::Expr *E) {
  const S::TypeAndValue *TV = I.lookup(E);
  if (!TV)
    return false;
  if (TV->M == S::Mode::NilValue)
    return true;
  auto *B = dyn_cast_or_null<S::BasicType>(TV->Ty);
  return B && B->BK == S::BasicKind::UntypedNil;
}

/// How the value of an expression (the last node of Path) is used.
struct UseKind {
  enum K { Good, Bad, Arg } Kind = Bad;
  const S::Func *Callee = nullptr; // Arg: the function and the parameter
  size_t Index = 0;
};

/// The use of the expression Path.back(): called, compared with nil, stored
/// into (an identifier on the left of `=`), or passed to a parameter of a
/// function of the package; anything else is Bad.
UseKind classify(const llvm::SmallVectorImpl<const ast::Node *> &Path,
                 const S::Info &I) {
  UseKind U;
  size_t P = Path.size() - 1;
  while (P > 0 && isa<ast::ParenExpr>(Path[P - 1]))
    --P;
  if (P == 0)
    return U;
  const ast::Node *Child = Path[P], *Par = Path[P - 1];
  if (auto *CE = dyn_cast<ast::CallExpr>(Par)) {
    if (CE->Fun == Child) {
      U.Kind = UseKind::Good;
      return U;
    }
    for (size_t K = 0; K < CE->Args.size(); ++K) {
      if (CE->Args[K] != Child)
        continue;
      auto *Id = dyn_cast<ast::Ident>(unparen(CE->Fun));
      auto It = Id ? I.Uses.find(Id) : I.Uses.end();
      auto *F = It == I.Uses.end() ? nullptr : dyn_cast<S::Func>(It->second);
      if (F && !F->Recv && !F->IsInterfaceMethod &&
          !CE->EllipsisLoc.isValid()) {
        U.Kind = UseKind::Arg;
        U.Callee = F;
        U.Index = K;
      }
      return U;
    }
    return U;
  }
  if (auto *BE = dyn_cast<ast::BinaryExpr>(Par)) {
    if (BE->Op == tok::eql || BE->Op == tok::neq) {
      const ast::Expr *Other = BE->X == Child ? BE->Y : BE->X;
      if (isNil(I, Other))
        U.Kind = UseKind::Good;
    }
    return U;
  }
  if (auto *AS = dyn_cast<ast::AssignStmt>(Par)) {
    if (AS->Op == tok::assign)
      for (const ast::Expr *L : AS->Lhs)
        if (L == Child)
          U.Kind = UseKind::Good;
    return U;
  }
  return U;
}

/// Is the last node of Path the function of a call?
bool isCallee(const llvm::SmallVectorImpl<const ast::Node *> &Path) {
  size_t P = Path.size() - 1;
  while (P > 0 && isa<ast::ParenExpr>(Path[P - 1]))
    --P;
  auto *CE = P ? dyn_cast<ast::CallExpr>(Path[P - 1]) : nullptr;
  return CE && CE->Fun == Path[P];
}

bool isLocalVar(const S::Var *V);

/// The local variable at the root of the receiver X of a method value of a
/// pointer method, which binds X's address (CLO-5): through fields held by
/// value (`x.f`), indexes (`a[i]`, `s[i]`) and slicing (`a[i:j][k]`), never
/// through a pointer (what a pointer points to is P04.20's). Null when X is
/// reached through a pointer or starts at no local variable (a package
/// variable, a call). The variable is what the method value holds, decided
/// like a captured variable (Body::mayHold): a cell when the value escapes,
/// or refused when it is a borrow (a slice of a local array, a view).
const S::Var *receiverRoot(const ast::Expr *X, const S::Info &I, TypeAt At) {
  // (The type of an expression at the instance being analyzed.)
  auto TypeOf = [&](const S::TypeAndValue *TV) -> S::Type * {
    S::Type *T = TV ? TV->Ty : nullptr;
    return T && At ? At(T) : T;
  };
  for (;;) {
    ++EscapeSteps;
    X = unparen(X);
    if (auto *Id = dyn_cast<ast::Ident>(X)) {
      auto U = I.Uses.find(Id);
      auto *V = U == I.Uses.end() ? nullptr : dyn_cast<S::Var>(U->second);
      return isLocalVar(V) ? V : nullptr;
    }
    if (auto *Sel = dyn_cast<ast::SelectorExpr>(X)) {
      // (A field through a pointer, also an embedded one, is in the pointee.)
      auto SI = I.Selections.find(Sel);
      S::Type *BT = TypeOf(I.lookup(Sel->X));
      if (SI == I.Selections.end() || SI->second.K != S::Selection::FieldVal ||
          SI->second.Indirect || !BT || isa<S::PointerType>(S::under(BT)))
        return nullptr;
      X = Sel->X;
      continue;
    }
    const ast::Expr *Base = nullptr;
    if (auto *IE = dyn_cast<ast::IndexExpr>(X))
      Base = IE->X;
    else if (auto *SE = dyn_cast<ast::SliceExpr>(X))
      Base = SE->X;
    S::Type *BT = Base ? TypeOf(I.lookup(Base)) : nullptr;
    S::Type *U = BT ? S::under(BT) : nullptr;
    // (A map entry, a string and a pointer to an array hold no element of a
    // variable's own.)
    if (!U ||
        !(isa<S::ArrayType>(U) || isa<S::SliceType>(U) || isa<S::RefType>(U)))
      return nullptr;
    X = Base;
  }
}

bool isLocalVar(const S::Var *V) {
  return V && !V->IsPackageLevel && !V->IsField && V->FuncOwner;
}

} // namespace

//===----------------------------------------------------------------------===//
// FuncHolderTypes
//===----------------------------------------------------------------------===//

bool FuncHolderTypes::mayContainFuncValue(S::Type *T) {
  if (!T)
    return false;
  if (auto It = Memo.find(T); It != Memo.end())
    return It->second;
  // Every type T reaches that is not decided yet, in order, with the edges
  // back to the types that reach it; then "holds one" spreads back along
  // them from the types that hold one by themselves. Every type seen is
  // decided once (a ring of types too), so a package's types cost linear
  // work over all the questions, whatever their order.
  SmallVector<S::Type *, 16> Order{T};
  llvm::DenseMap<S::Type *, unsigned> Index{{T, 0}};
  SmallVector<SmallVector<unsigned, 2>, 16> Users(1);
  SmallVector<bool, 16> Holds{false};
  SmallVector<unsigned, 16> Work;
  auto Hold = [&](unsigned K) {
    if (!Holds[K]) {
      Holds[K] = true;
      Work.push_back(K);
    }
  };
  for (unsigned K = 0; K < Order.size(); ++K) {
    ++EscapeSteps;
    auto Edge = [&](S::Type *E) {
      ++EscapeSteps;
      if (!E)
        return;
      if (auto M = Memo.find(E); M != Memo.end()) {
        if (M->second)
          Hold(K);
        return;
      }
      auto [It, New] = Index.try_emplace(E, Order.size());
      if (New) {
        Order.push_back(E);
        Users.emplace_back();
        Holds.push_back(false);
      }
      Users[It->second].push_back(K);
    };
    S::Type *U = S::under(Order[K]);
    switch (U->getKind()) {
    case S::Type::Kind::Signature:
    case S::Type::Kind::Interface: // (it can box a func value)
    case S::Type::Kind::Union:
    case S::Type::Kind::TypeParam: // (it may be instantiated with one)
    case S::Type::Kind::Named:     // (not resolved: say yes)
      Hold(K);
      break;
    case S::Type::Kind::Basic:
      break;
    case S::Type::Kind::Pointer:
      Edge(cast<S::PointerType>(U)->Elem);
      break;
    case S::Type::Kind::Slice:
      Edge(cast<S::SliceType>(U)->Elem);
      break;
    case S::Type::Kind::Array:
      Edge(cast<S::ArrayType>(U)->Elem);
      break;
    case S::Type::Kind::Map:
      Edge(cast<S::MapType>(U)->Key);
      Edge(cast<S::MapType>(U)->Elem);
      break;
    case S::Type::Kind::Chan:
      Edge(cast<S::ChanType>(U)->Elem);
      break;
    case S::Type::Kind::Ref:
      Edge(cast<S::RefType>(U)->Elem);
      break;
    case S::Type::Kind::Option:
      Edge(cast<S::OptionType>(U)->Elem);
      break;
    case S::Type::Kind::Tuple:
      for (S::Type *E : cast<S::TupleType>(U)->Elems)
        Edge(E);
      break;
    case S::Type::Kind::Struct:
      for (const S::Var *F : cast<S::StructType>(U)->Fields)
        Edge(F->Ty);
      break;
    }
  }
  while (!Work.empty()) {
    unsigned K = Work.pop_back_val();
    for (unsigned User : Users[K]) {
      ++EscapeSteps;
      Hold(User);
    }
  }
  for (unsigned K = 0; K < Order.size(); ++K)
    Memo[Order[K]] = Holds[K];
  return Holds[0];
}

//===----------------------------------------------------------------------===//
// KeepTable
//===----------------------------------------------------------------------===//

void KeepTable::compute(llvm::ArrayRef<const ast::File *> Files,
                        const S::Info &I) {
  struct Fn {
    const ast::FuncDecl *FD;
    const S::Func *Obj;
  };
  SmallVector<Fn, 16> Fns;
  unsigned Total = 0;
  for (const ast::File *File : Files)
    for (const ast::Decl *D : File->Decls) {
      auto *FD = dyn_cast<ast::FuncDecl>(D);
      if (!FD || !FD->Body || FD->Recv || !FD->Name)
        continue;
      auto Def = I.Defs.find(FD->Name);
      auto *Obj =
          Def == I.Defs.end() ? nullptr : dyn_cast<S::Func>(Def->second);
      if (!Obj || Base.count(Obj))
        continue;
      Base[Obj] = Total;
      Total += Obj->Params.size();
      Fns.push_back({FD, Obj});
    }
  Kept.assign(Total, 0);
  // Slot -> the slots whose keeping depends on it (the callee's parameter
  // keeps what is passed to it).
  SmallVector<SmallVector<unsigned, 2>, 16> Rev(Total);
  SmallVector<unsigned, 16> Work;
  for (const Fn &F : Fns) {
    llvm::DenseMap<const S::Var *, unsigned> Slot;
    for (size_t K = 0; K < F.Obj->Params.size(); ++K) {
      ++EscapeSteps;
      Slot[F.Obj->Params[K]] = Base[F.Obj] + K;
    }
    unsigned LitDepth = 0;
    SmallVector<const ast::Node *, 32> Path;
    walk(F.FD->Body, [&](const ast::Node *N, bool Exit) {
      if (Exit) {
        if (isa<ast::FuncLit>(N))
          --LitDepth;
        Path.pop_back();
        return;
      }
      Path.push_back(N);
      if (isa<ast::FuncLit>(N))
        ++LitDepth;
      auto *Id = dyn_cast<ast::Ident>(N);
      if (!Id)
        return;
      auto U = I.Uses.find(Id);
      auto *V = U == I.Uses.end() ? nullptr : dyn_cast<S::Var>(U->second);
      auto S = V ? Slot.find(V) : Slot.end();
      if (S == Slot.end() || Kept[S->second])
        return;
      UseKind K = LitDepth ? UseKind() : classify(Path, I);
      if (K.Kind == UseKind::Good)
        return;
      if (K.Kind == UseKind::Arg) {
        auto B = Base.find(K.Callee);
        if (B != Base.end() && K.Index < K.Callee->Params.size() &&
            !K.Callee->sig()->Variadic) {
          Rev[B->second + K.Index].push_back(S->second);
          return;
        }
      }
      Kept[S->second] = 1;
      Work.push_back(S->second);
    });
  }
  while (!Work.empty()) {
    unsigned S = Work.pop_back_val();
    for (unsigned Dep : Rev[S]) {
      ++EscapeSteps;
      if (!Kept[Dep]) {
        Kept[Dep] = 1;
        Work.push_back(Dep);
      }
    }
  }
}

bool KeepTable::keeps(const S::Func *F, size_t K) const {
  auto It = Base.find(F);
  if (It == Base.end() || K >= F->Params.size())
    return true;
  return Kept[It->second + K];
}

//===----------------------------------------------------------------------===//
// Closures of one function
//===----------------------------------------------------------------------===//

namespace {

struct VarFacts {
  unsigned Scope = ~0u; // the innermost scope of its declaration; ~0u unknown
  bool Bad = false;
  SmallVector<std::pair<const S::Func *, size_t>, 2> Edges;
};

struct Frame {
  const ast::FuncLit *Lit;
  const ast::BlockStmt *Body;
  llvm::DenseSet<const S::Var *> Seen;
  unsigned NextOrdinal = 0; // of the literals directly inside this one
};

bool opensScope(const ast::Node *N) {
  switch (N->getKind()) {
  case ast::Node::Kind::BlockStmt:
  case ast::Node::Kind::ForStmt:
  case ast::Node::Kind::RangeStmt:
  case ast::Node::Kind::IfStmt:
  case ast::Node::Kind::SwitchStmt:
  case ast::Node::Kind::TypeSwitchStmt:
  case ast::Node::Kind::CaseClause:
  case ast::Node::Kind::CommClause:
  case ast::Node::Kind::FuncLit:
    return true;
  default:
    return false;
  }
}

} // namespace

void analyzeEntity(llvm::ArrayRef<const ast::Node *> Roots,
                   const ast::BlockStmt *Body,
                   llvm::ArrayRef<const S::Var *> Outer, const KeepTable &Keeps,
                   FuncHolderTypes &Holders, const S::Info &I, EntityFacts &Out,
                   TypeAt At) {
  // (A variable's type at the instance being analyzed.)
  auto VarType = [&](const S::Var *V) {
    return V->Ty && At ? At(V->Ty) : V->Ty;
  };
  llvm::DenseMap<const S::Var *, VarFacts> Vars;
  // Scope intervals (an Euler tour): scope 0 is the entity itself.
  SmallVector<unsigned, 32> In{0}, OutT{~0u};
  SmallVector<unsigned, 16> ScopeStack{0};
  unsigned Tick = 1;
  for (const S::Var *V : Outer)
    Vars[V].Scope = 0;
  // The literal or method value each variable is the one initializer of, and
  // how the value of each is used.
  struct Ctx {
    enum K { Direct, Arg, Holder, Other } Kind = Other;
    const S::Func *Callee = nullptr;
    size_t Index = 0;
    const S::Var *Var = nullptr;
  };
  llvm::DenseMap<const ast::Expr *, Ctx> Ctxs;
  llvm::DenseMap<const S::Var *, const ast::Expr *> HolderOf;
  SmallVector<Frame, 8> Frames;
  SmallVector<const ast::Expr *, 8> Order;
  unsigned RootOrdinal = 0;
  SmallVector<const ast::Node *, 32> Path;

  auto bodyNow = [&]() -> const ast::BlockStmt * {
    return Frames.empty() ? Body : Frames.back().Body;
  };
  // How the value of the literal or method value E (Path.back()) is used:
  // called on the spot, passed to a parameter, the one initializer of a
  // local variable, or anything else.
  auto contextOf = [&](const ast::Expr *E) {
    Ctx C;
    UseKind U = classify(Path, I);
    size_t P = Path.size() - 1;
    while (P > 0 && isa<ast::ParenExpr>(Path[P - 1]))
      --P;
    const ast::Node *Par = P ? Path[P - 1] : nullptr;
    if (U.Kind == UseKind::Good) {
      // Good for an identifier also means a store into it; for a value only
      // a call counts.
      auto *CE = dyn_cast_or_null<ast::CallExpr>(Par);
      if (CE && CE->Fun == Path[P])
        C.Kind = Ctx::Direct;
    } else if (U.Kind == UseKind::Arg) {
      C.Kind = Ctx::Arg;
      C.Callee = U.Callee;
      C.Index = U.Index;
    } else {
      const ast::Ident *Name = nullptr;
      if (auto *AS = dyn_cast_or_null<ast::AssignStmt>(Par)) {
        if (AS->Op == tok::define && AS->Lhs.size() == AS->Rhs.size())
          for (size_t K = 0; K < AS->Rhs.size(); ++K)
            if (AS->Rhs[K] == Path[P])
              Name = dyn_cast<ast::Ident>(AS->Lhs[K]);
      } else if (auto *VS = dyn_cast_or_null<ast::ValueSpec>(Par)) {
        if (VS->Names.size() == VS->Values.size())
          for (size_t K = 0; K < VS->Values.size(); ++K)
            if (VS->Values[K] == Path[P])
              Name = VS->Names[K];
      }
      auto D = Name ? I.Defs.find(Name) : I.Defs.end();
      auto *V = D == I.Defs.end() ? nullptr : dyn_cast<S::Var>(D->second);
      // (Only a variable whose every use is followed can hold it: the same
      // predicate as the walk below.)
      if (V && isLocalVar(V) && Holders.mayContainFuncValue(VarType(V))) {
        C.Kind = Ctx::Holder;
        C.Var = V;
        HolderOf[V] = E;
      }
    }
    Ctxs[E] = C;
  };

  for (const ast::Node *Root : Roots) {
    walk(Root, [&](const ast::Node *N, bool Exit) {
      if (Exit) {
        Path.pop_back();
        if (opensScope(N)) {
          OutT[ScopeStack.back()] = Tick++;
          ScopeStack.pop_back();
        }
        if (isa<ast::FuncLit>(N)) {
          Frame F = std::move(Frames.back());
          Frames.pop_back();
          // What the literal captures, its parent captures too, unless the
          // variable is the parent's own.
          LitFacts &LF = Out.Lits[F.Lit];
          for (const S::Var *V : LF.Captures) {
            ++EscapeSteps;
            if (V->FuncOwner == bodyNow())
              continue;
            if (!Frames.empty() && Frames.back().Seen.insert(V).second)
              Out.Lits[Frames.back().Lit].Captures.push_back(V);
          }
        }
        return;
      }
      Path.push_back(N);
      if (opensScope(N)) {
        ScopeStack.push_back(In.size());
        In.push_back(Tick++);
        OutT.push_back(~0u);
      }
      if (auto *Lit = dyn_cast<ast::FuncLit>(N)) {
        LitFacts &LF = Out.Lits[Lit];
        LF.Parent = Frames.empty() ? nullptr : Frames.back().Lit;
        // 1, 2, ... in source order among the literals directly inside the
        // parent (appendix A.5).
        LF.Ordinal =
            Frames.empty() ? ++RootOrdinal : ++Frames.back().NextOrdinal;
        Order.push_back(Lit);
        Frames.push_back({Lit, Lit->Body, {}, 0});
        contextOf(Lit);
        return;
      }
      if (auto *Sel = dyn_cast<ast::SelectorExpr>(N)) {
        // A method value `x.M` (not called on the spot) is a closure over its
        // receiver (CLO-5), a node like a literal. A pointer method binds the
        // address of x: the method value holds the variable whose storage
        // that address is in (a cell if the value escapes).
        auto SI = I.Selections.find(Sel);
        auto *F = SI == I.Selections.end()
                      ? nullptr
                      : dyn_cast_or_null<S::Func>(SI->second.Obj);
        if (!F || F->IsBuiltinMethod ||
            SI->second.K != S::Selection::MethodVal || isCallee(Path))
          return;
        LitFacts &MF = Out.Lits[Sel];
        MF.Method = true;
        MF.Parent = Frames.empty() ? nullptr : Frames.back().Lit;
        if (F->HasPtrRecv && !F->IsInterfaceMethod)
          if (const S::Var *Root = receiverRoot(Sel->X, I, At))
            MF.Captures.push_back(Root);
        Order.push_back(Sel);
        contextOf(Sel);
        return;
      }
      auto *Id = dyn_cast<ast::Ident>(N);
      if (!Id)
        return;
      if (auto D = I.Defs.find(Id); D != I.Defs.end())
        if (auto *V = dyn_cast_or_null<S::Var>(D->second))
          Vars[V].Scope = ScopeStack.back();
      auto U = I.Uses.find(Id);
      auto *V = U == I.Uses.end() ? nullptr : dyn_cast<S::Var>(U->second);
      if (!isLocalVar(V))
        return;
      if (V->FuncOwner != bodyNow() && !Frames.empty() &&
          Frames.back().Seen.insert(V).second)
        Out.Lits[Frames.back().Lit].Captures.push_back(V);
      // A variable that may hold a closure or a method value (it may be a
      // holder, R1): how each use treats it.
      if (!Holders.mayContainFuncValue(VarType(V)))
        return;
      UseKind K = classify(Path, I);
      VarFacts &VF = Vars[V];
      if (K.Kind == UseKind::Bad)
        VF.Bad = true;
      else if (K.Kind == UseKind::Arg)
        VF.Edges.push_back({K.Callee, K.Index});
    });
  }

  // The verdicts. A literal or method value is local when its value is
  // called on the spot, passed to a parameter that is not kept, or held by a
  // variable that is only used that way and is declared inside the scope of
  // everything it captures (CLO-1, CLO-3).
  auto Inside = [&](unsigned Inner, unsigned OuterS) {
    return Inner != ~0u && OuterS != ~0u && In[OuterS] <= In[Inner] &&
           OutT[Inner] <= OutT[OuterS];
  };
  SmallVector<const ast::Expr *, 16> Work;
  for (const ast::Expr *Lit : Order) {
    ++EscapeSteps;
    LitFacts &LF = Out.Lits[Lit];
    const Ctx &C = Ctxs[Lit];
    bool Local = false;
    switch (C.Kind) {
    case Ctx::Direct:
      Local = true;
      break;
    case Ctx::Arg:
      Local = !Keeps.keeps(C.Callee, C.Index);
      break;
    case Ctx::Holder: {
      const VarFacts &VF = Vars[C.Var];
      Local = !VF.Bad && VF.Scope != ~0u;
      for (auto &[F, K] : VF.Edges) {
        ++EscapeSteps;
        Local = Local && !Keeps.keeps(F, K);
      }
      for (const S::Var *Cap : LF.Captures) {
        ++EscapeSteps;
        Local = Local && Inside(VF.Scope, Vars[Cap].Scope);
      }
      break;
    }
    case Ctx::Other:
      break;
    }
    LF.Escapes = !Local;
    if (LF.Escapes)
      Work.push_back(Lit);
  }
  // A literal (or method value) held by a variable that an escaping one
  // captures escapes too (the variable then lives in a cell; the held one's
  // frame environment would dangle).
  while (!Work.empty()) {
    const ast::Expr *Lit = Work.pop_back_val();
    for (const S::Var *V : Out.Lits[Lit].Captures) {
      ++EscapeSteps;
      Out.Cells.insert(V);
      auto H = HolderOf.find(V);
      if (H == HolderOf.end())
        continue;
      LitFacts &HF = Out.Lits[H->second];
      if (!HF.Escapes) {
        HF.Escapes = true;
        Work.push_back(H->second);
      }
    }
  }
}

} // namespace impl
} // namespace gir
} // namespace gopp
