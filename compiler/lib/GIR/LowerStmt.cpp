//===- LowerStmt.cpp - The checked AST to GIR: statements
//------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Statements and control flow: assignment (parallel, op=, ++/--), variable
// declarations, if, for, expression switch with fallthrough, labels, break,
// continue, goto, return with named results, local type declarations, and the
// package init function.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"
#include "gopp/GIR/DropElab.h"
#include "gopp/GIR/MoveCheck.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::isa_and_nonnull;

//===----------------------------------------------------------------------===//
// Variables and destinations
//===----------------------------------------------------------------------===//

Body::VarSlot Body::declareVar(const ast::Ident *Id, Type *Ty, bool AllocNow) {
  auto D = I.Defs.find(Id);
  const S::Var *V = nullptr;
  if (D != I.Defs.end())
    V = dyn_cast_or_null<S::Var>(D->second);
  return declareSlot(V, Id, Id->Name.str(), Ty, AllocNow);
}

Body::VarSlot Body::declareSlot(const S::Var *V, const ast::Node *N,
                                StringRef Name, Type *Ty, bool AllocNow) {
  VarSlot R;
  Function &F = FB.function();
  if (V && Facts && Facts->Cells.count(V)) {
    // A literal that escapes captures V: it lives in a heap cell made where
    // it is declared (a loop that declares it again makes a new one, CLO-8),
    // and the variable is the pointer local's target.
    LocalId Cell = FB.newLocal(ctx().ptr(Ty), Local::Var, Name.str());
    F.Locals[Cell].Source = true;
    F.Locals[Cell].Deref = true;
    ownCell(Cell, tyOfObj(V));
    Aliases[V] = Cell;
    Scopes.back().Locals.push_back(Cell);
    FB.storageLive(Cell);
    if (AllocNow)
      allocCell(Cell, Ty, N);
    CellLog.push_back({Cell, Ty});
    R.Cell = Cell;
    R.P = FB.deref(FB.local(Cell));
    return R;
  }
  LocalId Loc = FB.newLocal(Ty, Local::Var, Name.str());
  F.Locals[Loc].Source = true;
  // A variable that owns its value drops it where it dies (P04.09); a
  // borrowed one (a range variable, an assertion binding) does not.
  if (V && modeOf(V) == S::Var::Owned)
    ownLocal(Loc, tyOfObj(V));
  if (V)
    Vars[V] = Loc;
  Scopes.back().Locals.push_back(Loc);
  FB.storageLive(Loc);
  R.P = FB.local(Loc);
  return R;
}

void Body::ownCell(LocalId Cell, S::Type *T) {
  CellTypes[Cell] = T;
  if (FuncId Rel = L.rt(RtFn::CellRelease, Pos); Rel != NoId) {
    Local &Lo = FB.function().Locals[Cell];
    Lo.Owned = true;
    Lo.Glue = Rel;
  }
}

void Body::allocCell(LocalId Cell, Type *Ty, const ast::Node *N, bool DropOld) {
  if (N)
    at(N);
  Type *PU8 = ctx().ptr(ctx().u8());
  // [count, drop glue of the value, the value] (CLO-2: the last reference
  // drops the variable, gopp.cell_release); the pointer is to the value.
  Operand Raw = callFn(L.rt(RtFn::MemResize, N ? N->getBegin() : Pos),
                       {zeroOf(PU8),
                        constInt(ctx().u64(), L.layout(Ty).Size + TI::RcHeader),
                        constBool(ctx(), true)});
  Rt E(M, FB);
  Val R = E.snap(Val{&E, Raw});
  E.store(R, ctx().u64(), E.u(1));
  auto CT = CellTypes.find(Cell);
  FuncId Glue = CT == CellTypes.end() ? NoId : L.dropGlue(CT->second, Pos);
  if (Glue != NoId)
    E.store(E.padd(R, E.u(8)), PU8,
            E.tmp(FB.cast(CastKind::PtrToPtr, E.func(Glue).Op, PU8)));
  FB.assign(FB.local(Cell),
            FB.cast(CastKind::PtrToPtr, E.snap(E.padd(R, E.u(TI::RcHeader))).Op,
                    ctx().ptr(Ty)));
  const Local &Lo = FB.function().Locals[Cell];
  if (DropOld && Lo.Owned)
    FB.function().Blocks[FB.block()].Stmts.back().DropOld = Lo.Glue;
}

void Body::recordOps(const ast::Expr *E, LateOps &R) {
  // The path from E to its root, outer to inner; a loop, as selectorPlace
  // (a selector chain can be far longer than the nesting limit).
  SmallVector<const ast::Expr *, 8> Path;
  const ast::Expr *Root = nullptr;
  for (const ast::Expr *X = E; X;) {
    X = unparen(X);
    if (isConst(X))
      break;
    switch (X->getKind()) {
    case ast::Node::Kind::Ident:
      X = nullptr; // a variable: read where the place is made
      break;
    case ast::Node::Kind::SelectorExpr: {
      auto *Sel = cast<ast::SelectorExpr>(X);
      const S::Selection *SL = selOf(Sel);
      if (!SL || SL->K != S::Selection::FieldVal) {
        Root = X;
        X = nullptr;
      } else {
        X = Sel->X;
      }
      break;
    }
    case ast::Node::Kind::IndexExpr: {
      auto *IE = cast<ast::IndexExpr>(X);
      if (IE->Indices.size() != 1 || hasOpMethod(IE)) {
        Root = X;
        X = nullptr;
      } else {
        Path.push_back(X);
        X = IE->X;
      }
      break;
    }
    case ast::Node::Kind::SliceExpr:
      Path.push_back(X);
      X = cast<ast::SliceExpr>(X)->X;
      break;
    case ast::Node::Kind::StarExpr:
      X = cast<ast::StarExpr>(X)->X;
      break;
    case ast::Node::Kind::UnaryExpr: {
      auto *U = cast<ast::UnaryExpr>(X);
      if (U->Op == tok::and_ && !hasOpMethod(U) &&
          !isa<ast::CompositeLit>(unparen(U->X))) {
        X = U->X; // &x: the address is taken where the place is made
      } else {
        Root = X;
        X = nullptr;
      }
      break;
    }
    case ast::Node::Kind::CallExpr: {
      // A conversion is no call: it may share its operand's storage
      // (`IS(s)[0]`, `(*[4]int)(s)[1]`), so it is made where the place is.
      auto *C = cast<ast::CallExpr>(X);
      const S::TypeAndValue *FT = I.lookup(unparen(C->Fun));
      if (FT && FT->M == S::Mode::TypeExpr && C->Args.size() == 1) {
        X = C->Args[0];
      } else {
        Root = X;
        X = nullptr;
      }
      break;
    }
    default:
      Root = X; // a value that is no place (a literal, ...)
      X = nullptr;
      break;
    }
    ++LowerSteps;
  }
  auto Save = [&](const ast::Expr *Op) {
    if (!Op || isConst(Op) || isNilValue(Op) || R.Saved.count(Op))
      return;
    Operand V = lowerExpr(Op);
    if (V.type())
      R.Saved[Op] = localOf(std::move(V));
  };
  // Inner to outer: Go's order (the root, then each index or bound).
  Save(Root);
  for (size_t K = Path.size(); K > 0; --K) {
    ++LowerSteps;
    if (auto *IE = dyn_cast<ast::IndexExpr>(Path[K - 1])) {
      Save(IE->Indices[0]);
    } else {
      auto *SE = cast<ast::SliceExpr>(Path[K - 1]);
      Save(SE->Lo);
      Save(SE->Hi);
      Save(SE->Max);
    }
  }
}

Body::Dest Body::recordDest(const ast::Expr *E) {
  E = unparen(E);
  Dest D;
  D.E = E;
  if (auto *Id = dyn_cast<ast::Ident>(E)) {
    if (Id->Name.str() == "_") {
      D.Blank = true;
      return D;
    }
    D.STy = sTyOf(E);
    if (narrowed(Id)) {
      // The variable itself is assigned: it is the Option, not its payload.
      auto U = I.Uses.find(Id);
      if (U != I.Uses.end())
        D.STy = tyOfObj(U->second);
    }
    return D;
  }
  if (auto *IE = dyn_cast<ast::IndexExpr>(E)) {
    // `a[i] = v` through the user's `[]=` (P04.23): not a place. Its
    // operands are evaluated now (phase 1), the call is the store.
    const S::OperatorUse *U = opUse(IE);
    if (U && U->Op == S::OpKind::IndexSet && U->Call && opResolved(*U)) {
      const S::Selection *SL = selOf(cast<ast::SelectorExpr>(U->Call->Fun));
      auto *F = SL ? dyn_cast_or_null<S::Func>(SL->Obj) : nullptr;
      if (F && !sigOf(F)->Params->Elems.empty()) {
        D.OpSet = U;
        D.STy = sigOf(F)->Params->Elems.back();
        recordOps(IE->X, D.Ops);
        for (const ast::Expr *Idx : IE->Indices)
          saveOp(Idx, D.Ops);
        return D;
      }
    }
  }
  if (auto *IE = dyn_cast<ast::IndexExpr>(E); IE && isMapIndex(IE))
    D.STy = mapTypeOf(sTyOf(IE->X))->Elem;
  else
    D.STy = sTyOf(E);
  recordOps(E, D.Ops);
  return D;
}

void Body::deriveDest(Dest &D) {
  ++LowerSteps;
  if (D.Blank || D.Derived)
    return;
  D.Derived = true;
  if (D.OpSet)
    return;
  ReplayScope Scope(*this, D.Ops);
  if (auto *Id = dyn_cast<ast::Ident>(D.E)) {
    D.P = identPlace(Id);
    return;
  }
  if (auto *IE = dyn_cast<ast::IndexExpr>(D.E); IE && isMapIndex(IE)) {
    lowerMapDest(IE, D);
    return;
  }
  D.P = lowerPlace(D.E, &D.Checks);
}

void Body::store(Dest &D, Operand V, const ast::Node *N, bool KeepOld,
                 bool Defer) {
  ++LowerSteps;
  if (D.Blank)
    return;
  if (D.OpSet) {
    opSetStore(D, std::move(V), N);
    return;
  }
  deriveDest(D);
  // Phase 2 of the assignment: the index checks of this target just before
  // its store (the spec: `x[1], x[3] = 4, 5` sets x[1], then panics for x[3]),
  // then its map entries.
  emitIndexChecks(D.Checks);
  D.Checks.clear();
  resolveDest(D, N);
  if (!D.P.Ty)
    return;
  at(N);
  // (A borrowed variable or parameter is re-pointed, never dropped: PAR-2.)
  const Local &Root0 = FB.function().Locals[D.P.Local];
  const bool Borrowed0 =
      !Root0.Owned &&
      (Root0.K == Local::Arg || (Root0.K == Local::Var && D.P.Proj.empty()));
  if (Defer && D.STy && !KeepOld && !Borrowed0 && L.needsDrop(D.STy)) {
    // The old value moves to a temporary that dies with the statement.
    Operand Old = copyOf(D.P);
    LocalId Tmp = FB.newTemp(Old.type());
    FB.assign(FB.local(Tmp), FB.use(std::move(Old)));
    FB.function().Blocks[FB.block()].Stmts.back().OldRead = true;
    ownTemp(copyOf(FB.local(Tmp)), D.STy);
    KeepOld = true;
  }
  FB.assign(D.P, FB.use(std::move(V)));
  // The value it replaces dies here (elaboration drops it first, P04.09).
  if (D.STy && !KeepOld && !Borrowed0)
    if (FuncId G = L.dropGlue(D.STy, N->getBegin()); G != NoId)
      FB.function().Blocks[FB.block()].Stmts.back().DropOld = G;
}

bool Body::multiValues(const ast::Expr *E, size_t Want,
                       SmallVector<Operand, 2> &Out,
                       ArrayRef<S::Type *> Targets) {
  // v, ok := m[k]
  auto *MapIE = dyn_cast<ast::IndexExpr>(unparen(E));
  auto *AssertE = dyn_cast<ast::TypeAssertExpr>(unparen(E));
  S::Type *Want0 = Targets.empty() ? nullptr : Targets[0];
  // `v, ok := move(m[k])` and `v, ok := clone(m[k])` (OWN-R3a, OWN-R9).
  const ast::IndexExpr *TakeIE = nullptr;
  bool TakeMove = false;
  if (auto *C = dyn_cast<ast::CallExpr>(unparen(E));
      C && Want == 2 && C->Args.size() == 1)
    if (auto *Id = dyn_cast<ast::Ident>(unparen(C->Fun)))
      if (auto U = I.Uses.find(Id); U != I.Uses.end())
        if (auto *B = dyn_cast<S::Builtin>(U->second);
            B && (B->Id == S::BuiltinId::Move || B->Id == S::BuiltinId::Clone))
          if (auto *IE = dyn_cast<ast::IndexExpr>(unparen(C->Args[0]));
              IE && isMapIndex(IE)) {
            TakeIE = IE;
            TakeMove = B->Id == S::BuiltinId::Move;
          }
  const bool CommaOk = Want == 2 && ((MapIE && isMapIndex(MapIE)) ||
                                     AssertE != nullptr || TakeIE);
  Operand T = TakeIE ? mapEntryOwned(TakeIE, true, TakeMove)
              : MapIE && Want == 2 && isMapIndex(MapIE) ? mapRead(MapIE, true)
              : AssertE && Want == 2 ? lowerTypeAssert(AssertE, true, Want0)
                                     : lowerExpr(unparen(E));
  Type *Ty = T.type();
  if (Ty->getKind() != Type::Kind::Tuple || Ty->numFields() != Want) {
    unsupported(E, "this multi-value expression");
    return false;
  }
  Place P = placeOf(std::move(T));
  at(E);
  // The checker types of the values: a call's results, or a comma-ok
  // form's value and its untyped bool.
  auto *TT = dyn_cast_or_null<S::TupleType>(sTyOf(E));
  for (size_t K = 0; K < Want; ++K) {
    Operand F = copyOf(FB.field(P, K));
    S::Type *From = nullptr;
    if (!CommaOk && TT && TT->Elems.size() == Want)
      From = TT->Elems[K];
    else if (CommaOk)
      From = K == 1   ? L.CP.Ctx->typ(S::BasicKind::Bool)
             : TakeIE ? mapTypeOf(sTyOf(TakeIE->X))->Elem
                      : sTyOf(E);
    // (A view a comma-ok assertion binds is lowerTypeAssert's: no
    // conversion goes to a view.)
    if (K < Targets.size())
      F = convertResult(std::move(F), From, Targets[K], E);
    // Each value goes to a new owner (P04.09): moved out of a call's results
    // (the rest end with the statement), a shared handle retained.
    S::Type *To = K < Targets.size() && Targets[K] ? Targets[K] : From;
    if (To)
      F = takeValue(std::move(F), To);
    Out.push_back(std::move(F));
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Assignment
//===----------------------------------------------------------------------===//

bool impl::compoundOp(tok::Kind T, BinOp &Op) {
  switch (T) {
  case tok::add_assign:
    Op = BinOp::Add;
    return true;
  case tok::sub_assign:
    Op = BinOp::Sub;
    return true;
  case tok::mul_assign:
    Op = BinOp::Mul;
    return true;
  case tok::quo_assign:
    Op = BinOp::Div;
    return true;
  case tok::rem_assign:
    Op = BinOp::Rem;
    return true;
  case tok::and_assign:
    Op = BinOp::And;
    return true;
  case tok::or_assign:
    Op = BinOp::Or;
    return true;
  case tok::xor_assign:
    Op = BinOp::Xor;
    return true;
  case tok::shl_assign:
    Op = BinOp::Shl;
    return true;
  case tok::shr_assign:
    Op = BinOp::Shr;
    return true;
  case tok::and_not_assign:
    Op = BinOp::AndNot;
    return true;
  default:
    return false;
  }
}

/// A right-hand value that a later store must not change: a read of a
/// variable is copied into a temporary.
static bool isStable(const Operand &V, FuncBuilder &FB) {
  return V.K == Operand::Const ||
         (V.P.Proj.empty() && FB.function().Locals[V.P.Local].K == Local::Temp);
}

bool Body::swapValues(const ast::AssignStmt *A, ArrayRef<S::Type *> Tys,
                      bool MayAlias, llvm::SmallVectorImpl<Operand> &Vals) {
  // A swap (OWN-R3a, Info::Swaps) moves the values between its places (P04.09
  // R4): every place and every check first (spec 4.2), then every value is
  // read, then the places that own their value are emptied, so a place named
  // twice at run time (`xs[i], xs[j] = xs[j], xs[i]` with i == j, a map key
  // twice) is read whole each time. The stores then fill the places. With two
  // places that is always one value per place. With three or more, two names
  // of one place can make Go's left-to-right stores write one value twice
  // and none of another (`a[i], a[j], a[k] = a[j], a[k], a[i]`, i == k):
  // when the checker cannot rule that out (MayAlias, R18: then every type is
  // cloneable, E0452/E0453), each place gets a clone at run time, the values
  // read are dropped once each, and the stores drop what they replace.
  const size_t N = A->Rhs.size();
  Rt E(M, FB);
  Type *PU8 = ctx().ptr(ctx().u8());
  struct Src {
    Val Addr; // null: an absent map entry (its zero value)
    Type *G = nullptr;
    S::Type *T = nullptr;
    bool Owns = false;
  };
  SmallVector<Src, 4> Srcs;
  for (size_t K = 0; K < N; ++K) {
    ++LowerSteps;
    const ast::Expr *R = unparen(A->Rhs[K]);
    Src S;
    S.T = Tys[K] ? Tys[K] : sTyOf(R);
    S.G = S.T ? L.lowerType(S.T, R->getBegin()) : nullptr;
    if (!S.G)
      return false;
    S.Owns = L.needsDrop(S.T);
    auto *IE = dyn_cast<ast::IndexExpr>(R);
    if (IE && isMapIndex(IE)) {
      // The entry's slot, or null when the key is absent (no insert).
      S::MapType *MT = mapTypeOf(sTyOf(IE->X));
      Val Mv = E.snap(Val{&E, mapValue(IE->X)});
      Operand Key = valueFor(IE->Indices[0], MT->Key);
      at(IE);
      Place KP = FB.temp(FB.use(Key));
      Operand Info = mapInfoPtr(MT, IE);
      Operand KA = bytesAddr(KP);
      mapKeyCheck(MT, Info, Mv.Op, KA, IE);
      S.Addr = E.snap(Val{&E, callRt(RtFn::MapGet, {Info, Mv.Op, KA}, IE)});
    } else {
      Place P = lowerPlace(R);
      if (!P.Ty)
        return false;
      at(A);
      S.Addr = E.snap(E.addrOf(P));
    }
    S.Addr = E.snap(E.to(PU8, S.Addr));
    Srcs.push_back(S);
  }
  at(A);
  // Every value, then the emptied places.
  SmallVector<Var, 4> Read;
  for (const Src &S : Srcs) {
    Var V = E.var(S.G, E.zero(S.G), "swap");
    E.If(S.Addr != E.null(PU8), [&] { V = E.load(S.Addr, S.G); });
    Read.push_back(V);
  }
  for (const Src &S : Srcs)
    if (S.Owns)
      E.If(S.Addr != E.null(PU8), [&] { E.store(S.Addr, S.G, E.zero(S.G)); });
  // An interface value lent to this function stays in no place it could
  // outlive the call from (R1).
  for (size_t K = 0; K < N; ++K)
    if (Srcs[K].Owns && Srcs[K].G->isIface())
      if (FuncId Take = L.rt(RtFn::IfaceTake, A->getBegin()); Take != NoId)
        E.call(Take,
               {E.to(PU8, E.addrOf(Read[K])), E.to(PU8, E.addrOf(Read[K]))});
  const bool Fix = N > 2 && MayAlias;
  SmallVector<Var, 4> Put;
  for (size_t K = 0; K < N; ++K)
    Put.push_back(Fix && Srcs[K].Owns ? E.var(Srcs[K].G, Read[K], "put")
                                      : Read[K]);
  if (Fix) {
    // Two names of one place among the sources.
    Var Alias = E.var(ctx().boolean(), E.boolean(false), "alias");
    for (size_t K = 1; K < N; ++K)
      for (size_t J = 0; J < K; ++J) {
        ++LowerSteps;
        if (Srcs[J].Owns && Srcs[K].Owns && Srcs[J].G == Srcs[K].G)
          E.If(E.land(Srcs[J].Addr != E.null(PU8),
                      [&] { return Srcs[J].Addr == Srcs[K].Addr; }),
               [&] { Alias = E.boolean(true); });
      }
    E.If(Alias, [&] {
      for (size_t K = 0; K < N; ++K) {
        if (!Srcs[K].Owns)
          continue;
        FuncId Clone = L.Methods.cloneable(Srcs[K].T)
                           ? L.cloneGlue(Srcs[K].T, A->getBegin())
                           : NoId;
        if (Clone == NoId) {
          // (The checker refuses such a swap of a type with no Clone.)
          L.internalError(A->getBegin(), "a swap that may name one place "
                                         "twice, of a type with no Clone");
          return;
        }
        E.call(Clone,
               {E.to(PU8, E.addrOf(Put[K])), E.to(PU8, E.addrOf(Read[K]))});
      }
      // Each value read is dropped once (its first read).
      for (size_t K = 0; K < N; ++K) {
        if (!Srcs[K].Owns)
          continue;
        FuncId Drop = L.dropGlue(Srcs[K].T, A->getBegin());
        if (Drop == NoId)
          continue;
        Var First = E.var(ctx().boolean(), E.boolean(true), "first");
        for (size_t J = 0; J < K; ++J)
          if (Srcs[J].Owns && Srcs[J].G == Srcs[K].G)
            E.If(Srcs[J].Addr == Srcs[K].Addr,
                 [&] { First = E.boolean(false); });
        E.If(First, [&] { E.call(Drop, {E.to(PU8, E.addrOf(Read[K]))}); });
      }
    });
  }
  for (size_t K = 0; K < N; ++K)
    Vals.push_back(copyOf(FB.local(Put[K].L)));
  return true;
}

void Body::lowerDefineOrAssign(const ast::AssignStmt *A) {
  bool Define = A->Op == tok::define;
  size_t N = A->Lhs.size();
  // 1. The operands of the left sides (index values, keys, a call: Go spec,
  //    "Assignment statements", phase 1), and which names are new.
  struct Target {
    Dest D;
    const ast::Ident *New = nullptr;
    S::Type *STy = nullptr;
  };
  SmallVector<Target, 2> Targets_;
  for (size_t K = 0; K < N; ++K) {
    Target T;
    const ast::Expr *Lhs = unparen(A->Lhs[K]);
    auto *Id = dyn_cast<ast::Ident>(Lhs);
    if (Define && Id && Id->Name.str() != "_") {
      auto Def = I.Defs.find(Id);
      if (Def != I.Defs.end() && Def->second) {
        T.New = Id;
        T.STy = tyOfObj(Def->second);
        Targets_.push_back(std::move(T));
        continue;
      }
    }
    T.D = recordDest(Lhs);
    T.STy = T.D.STy;
    Targets_.push_back(std::move(T));
  }
  // 2. The right sides, all before any store.
  SmallVector<Operand, 2> Vals;
  // The in-place shrinks `s = s[i:j]` the checker recorded (SLC-6): their
  // bounds are evaluated in their turn, the slice is read after the other
  // right sides (a late read: a call among them may grow it and move its
  // elements, as `p = append(p, ...)` moves p after them, EXP-3), and the
  // elements move after the stores.
  SmallVector<ShrinkMove, 1> Moves;
  SmallVector<std::pair<size_t, LateOps>, 1> Shrinks;
  // A swap of values that own something moves them between the places,
  // which may be one place at run time (swapValues, P04.09 R4).
  auto SwapIt = I.Swaps.find(A->Lhs[0]);
  bool Swap = !Define && A->Rhs.size() == N && N > 1 && SwapIt != I.Swaps.end();
  if (Swap) {
    bool Owns = false;
    for (size_t K = 0; K < N; ++K)
      Owns = Owns || (Targets_[K].STy && L.needsDrop(Targets_[K].STy));
    Swap = Owns;
  }
  if (Swap) {
    SmallVector<S::Type *, 2> Tys;
    for (const Target &T : Targets_)
      Tys.push_back(T.STy);
    if (!swapValues(A, Tys, SwapIt->second, Vals))
      return;
  } else if (A->Rhs.size() == N) {
    for (size_t K = 0; K < N; ++K) {
      S::Type *Want = Targets_[K].STy ? Targets_[K].STy : sTyOf(A->Rhs[K]);
      if (auto *SE = dyn_cast<ast::SliceExpr>(unparen(A->Rhs[K]));
          SE && I.Reslices.count(SE)) {
        Shrinks.push_back({K, LateOps()});
        recordOps(SE, Shrinks.back().second);
        Vals.push_back(Operand());
        continue;
      }
      Operand V = ownedValue(A->Rhs[K], Want);
      if (N > 1 && !isStable(V, FB)) {
        at(A);
        V = holdValue(std::move(V), Want);
      }
      Vals.push_back(std::move(V));
    }
    for (auto &[K, R] : Shrinks) {
      ++LowerSteps;
      ReplayScope Scope(*this, R);
      Moves.emplace_back();
      Operand V = lowerSliceExpr(cast<ast::SliceExpr>(unparen(A->Rhs[K])),
                                 &Moves.back());
      if (N > 1 && !isStable(V, FB)) {
        at(A);
        V = copyOf(FB.temp(FB.use(V)));
      }
      Vals[K] = std::move(V);
    }
  } else if (A->Rhs.size() == 1) {
    SmallVector<S::Type *, 2> To;
    for (Target &T : Targets_)
      To.push_back(T.STy);
    if (!multiValues(A->Rhs[0], N, Vals, To))
      return;
  } else {
    unsupported(A, "this assignment");
    return;
  }
  // 3. The places of the left sides: the containers and pointers along them
  //    as they are now, after the right sides and before any store.
  for (Target &T : Targets_)
    if (!T.New) {
      deriveDest(T.D);
      if (!T.D.ok())
        return;
    }
  // 4. Declare the new names and store, left to right.
  for (size_t K = 0; K < N; ++K) {
    Target &T = Targets_[K];
    at(A);
    if (T.New) {
      Type *Ty = L.lowerType(T.STy, T.New->getBegin());
      if (!Ty)
        return;
      // (A box binding a closure that outlives the call holds: ownBoxCopy.)
      auto D = I.Defs.find(T.New);
      Operand Val = ownBoxCopy(
          std::move(Vals[K]),
          D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::Var>(D->second),
          T.New);
      VarSlot Sl = declareVar(T.New, Ty);
      FB.assign(Sl.P, FB.use(std::move(Val)));
      if (D != I.Defs.end())
        noteBorrowingDecl(dyn_cast_or_null<S::Var>(D->second));
      continue;
    }
    // (An in-place shrink keeps its block: the elements that leave end in
    // step 5, nothing else does.)
    auto *Sl = dyn_cast<ast::SliceExpr>(
        unparen(A->Rhs.size() == N ? A->Rhs[K] : A->Rhs[0]));
    // (A swap of two leaves nothing to drop: the places were emptied, and
    // when they are one place both stores write the one value, swapValues.)
    store(T.D, Vals[K], A,
          (Swap && N == 2) ||
              (A->Rhs.size() == N && (I.SelfMoves.count(unparen(A->Rhs[K])) ||
                                      (Sl && I.Reslices.count(Sl)))),
          /*Defer=*/N > 1);
  }
  // 5. SLC-7: the kept elements of each in-place shrink go down to the start
  //    of the block after every store of the statement, so a store to an
  //    element (`s, s[0] = s[1:], 50`) lands where it was, in either order.
  for (ShrinkMove &Mv : Moves) {
    ++LowerSteps;
    if (!Mv.Dst.type())
      continue;
    at(A);
    Rt E(M, FB);
    if (Mv.Glue != NoId) {
      // The elements that leave the slice end here (OWN-R2).
      Val Ptr{&E, Mv.Ptr}, Lo{&E, Mv.Lo}, Hi{&E, Mv.Hi}, Len{&E, Mv.Len};
      auto Range = [&](Val From, Val To) {
        Var K = E.var(ctx().u64(), From, "k");
        E.While([&] { return E.bin(BinOp::Lt, K, To); },
                [&] {
                  E.call(Mv.Glue, {E.padd(Ptr, K * Mv.Esz)});
                  K = K + 1;
                });
      };
      Range(E.u(0), Lo);
      Range(Hi, Len);
    }
    Val Dst{&E, Mv.Dst}, Src{&E, Mv.Src};
    E.If(Dst != Src, [&] {
      callFn(L.libc("memmove", A->getBegin()), {Mv.Dst, Mv.Src, Mv.Bytes});
    });
  }
}

void Body::lowerOpAssign(const ast::AssignStmt *A) {
  BinOp Op;
  if (const S::OperatorUse *U = opUse(A);
      U && (U->IndexSet || opResolved(*U))) {
    lowerOpAssignStmt(A, *U);
    return;
  }
  if (!compoundOp(A->Op, Op) || A->Lhs.size() != 1 || A->Rhs.size() != 1) {
    unsupported(A, "this assignment");
    return;
  }
  Dest D = recordDest(A->Lhs[0]);
  if (D.Blank)
    return;
  S::Type *T = D.STy;
  Type *G = L.lowerType(T, A->getBegin());
  if (!G)
    return;
  at(A);
  // The right side first, then the place, its checks and its read (as gc:
  // the index is checked in phase 2, after the right side, and the place is
  // read, and found again in a container the right side grew, after a call
  // on the right has run).
  Operand Y = isShift(Op) ? lowerExpr(A->Rhs[0]) : lowerAs(A->Rhs[0], G);
  deriveDest(D);
  if (!D.ok())
    return;
  emitIndexChecks(D.Checks);
  D.Checks.clear();
  resolveDest(D, A);
  if (!D.P.Ty)
    return;
  at(A);
  // `s += x` on an owned string grows it in place (STM-8, P04.09): a string
  // that was moved from is zero and owns no buffer, so no other string
  // shares its bytes.
  if (Op == BinOp::Add && G->getKind() == Type::Kind::String &&
      isStringType(S::under(T))) {
    Rt E(M, FB);
    Place YP = placeOf(std::move(Y));
    at(A);
    callRt(RtFn::StrAppend,
           {E.addrOf(D.P).Op, copyOf(FB.field(YP, 0)), copyOf(FB.field(YP, 1))},
           A);
    return;
  }
  Operand X = copyOf(D.P);
  if (G->isInt() || G->isFloat()) {
    // The result goes straight into the place.
    if (arithChecks(Op, Y, G, isConst(A->Rhs[0]), A))
      FB.assign(D.P, FB.binary(Op, std::move(X), std::move(Y)));
    return;
  }
  Operand R = arith(Op, std::move(X), std::move(Y), T, isConst(A->Rhs[0]), A);
  store(D, std::move(R), A);
}

void Body::lowerAssign(const ast::AssignStmt *A) {
  at(A);
  if (A->Op == tok::define || A->Op == tok::assign)
    lowerDefineOrAssign(A);
  else
    lowerOpAssign(A);
}

void Body::lowerIncDec(const ast::IncDecStmt *S) {
  at(S);
  if (const S::OperatorUse *U = opUse(S); U && opResolved(*U)) {
    lowerOpCallStmt(S, *U);
    return;
  }
  Dest D = recordDest(S->X);
  if (D.Blank)
    return;
  deriveDest(D);
  if (!D.ok())
    return;
  emitIndexChecks(D.Checks);
  D.Checks.clear();
  resolveDest(D, S);
  if (!D.P.Ty)
    return;
  Type *T = D.P.Ty;
  if (!T->isInt() && !T->isFloat()) {
    unsupported(S, "++ on " + T->str());
    return;
  }
  Operand One = T->isInt() ? constInt(T, 1) : constFloat(T, 1.0);
  FB.assign(D.P, FB.binary(S->Op == tok::inc ? BinOp::Add : BinOp::Sub,
                           copyOf(D.P), One));
}

void Body::lowerValueSpec(const ast::ValueSpec *VS) {
  at(VS);
  size_t N = VS->Names.size();
  SmallVector<S::Type *, 2> STys;
  for (size_t K = 0; K < N; ++K) {
    auto D = I.Defs.find(VS->Names[K]);
    STys.push_back(D != I.Defs.end() && D->second ? tyOfObj(D->second)
                                                  : nullptr);
  }
  SmallVector<Operand, 2> Vals;
  if (VS->Values.empty()) {
    for (size_t K = 0; K < N; ++K) {
      Type *T = L.lowerType(STys[K], VS->Names[K]->getBegin());
      if (!T)
        return;
      Vals.push_back(zeroOf(T));
    }
  } else if (VS->Values.size() == N) {
    for (size_t K = 0; K < N; ++K) {
      S::Type *Want = STys[K] ? STys[K] : sTyOf(VS->Values[K]);
      Operand V = ownedValue(VS->Values[K], Want);
      if (N > 1 && !isStable(V, FB)) {
        at(VS);
        V = holdValue(std::move(V), Want);
      }
      Vals.push_back(std::move(V));
    }
  } else if (VS->Values.size() == 1) {
    if (!multiValues(VS->Values[0], N, Vals, STys))
      return;
  } else {
    unsupported(VS, "this declaration");
    return;
  }
  for (size_t K = 0; K < N; ++K) {
    if (VS->Names[K]->Name.str() == "_")
      continue;
    Type *T = L.lowerType(STys[K], VS->Names[K]->getBegin());
    if (!T)
      return;
    at(VS);
    auto D = I.Defs.find(VS->Names[K]);
    Operand Val = ownBoxCopy(
        std::move(Vals[K]),
        D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::Var>(D->second),
        VS->Names[K]);
    VarSlot Sl = declareVar(VS->Names[K], T);
    FB.assign(Sl.P, FB.use(std::move(Val)));
    if (D != I.Defs.end())
      noteBorrowingDecl(dyn_cast_or_null<S::Var>(D->second));
  }
}

void Body::noteBorrowingDecl(const S::Var *V) {
  // A view, a borrowed variable (a box binding of an assertion), or a value
  // that holds a view may point into a temporary of the statement.
  if (!V)
    return;
  S::Type *T = tyOfObj(V);
  if (modeOf(V) != S::Var::Owned || (T && isa<S::RefType>(S::under(T))) ||
      (T && L.Methods.holdsView(T)))
    ExtendTemps = true;
}

void Body::lowerTypeDecl(const ast::GenDecl *GD) {
  for (const ast::Spec *Sp : GD->Specs) {
    auto *TS = dyn_cast<ast::TypeSpec>(Sp);
    if (!TS || TS->AssignLoc.isValid())
      continue;
    if (TS->TypeParams && TS->TypeParams->numFields()) {
      unsupported(TS, "a generic type declared inside a function (P04.28)");
      continue;
    }
    if (CurInst) {
      unsupported(TS, "a type declared inside generic code (P04.28)");
      continue;
    }
    // (Its symbol was given before the bodies were lowered, with the next
    // ordinal of this function, so two types with one name in two blocks
    // have two: impl::declareLocalTypes.)
  }
}

//===----------------------------------------------------------------------===//
// Control flow
//===----------------------------------------------------------------------===//

void Body::lowerIf(const ast::IfStmt *If) {
  pushScope();
  if (If->Init)
    lowerStmt(If->Init, /*KeepTemps=*/true);
  Operand Cond = lowerAs(If->Cond, ctx().boolean());
  at(If);
  BlockId Then = FB.newBlock(), Else = If->Else ? FB.newBlock() : NoId,
          Join = FB.newBlock();
  FB.branch(Cond, Then, If->Else ? Else : Join);
  FB.setBlock(Then);
  lowerBlock(If->Then);
  if (!FB.terminated())
    FB.gotoBlock(Join);
  if (If->Else) {
    FB.setBlock(Else);
    lowerStmt(If->Else);
    if (!FB.terminated())
      FB.gotoBlock(Join);
  }
  FB.setBlock(Join);
  popScope();
}

void Body::lowerFor(const ast::ForStmt *For, std::string Label) {
  pushScope();
  size_t CellMark = CellLog.size();
  if (For->Init)
    lowerStmt(For->Init);
  // The variables of the clause that a literal which escapes captures: each
  // iteration has its own copy (CLO-8, Go 1.22), made before the post
  // statement runs (`continue` goes there too).
  SmallVector<std::pair<LocalId, Type *>, 2> LoopCells(
      CellLog.begin() + CellMark, CellLog.end());
  CellLog.resize(CellMark);
  const bool HasPost = For->Post || !LoopCells.empty();
  BlockId Head = FB.newBlock(), BodyB = FB.newBlock(),
          Post = HasPost ? FB.newBlock() : NoId, Exit = FB.newBlock();
  FB.gotoBlock(Head);
  FB.setBlock(Head);
  if (For->Cond) {
    Operand Cond = lowerAs(For->Cond, ctx().boolean());
    at(For);
    FB.branch(Cond, BodyB, Exit);
  } else {
    FB.gotoBlock(BodyB);
  }
  FB.setBlock(BodyB);
  BlockId Next = HasPost ? Post : Head;
  Targets.push_back({Exit, Next, Scopes.size(), std::move(Label)});
  lowerBlock(For->Body);
  Targets.pop_back();
  if (!FB.terminated())
    FB.gotoBlock(Next);
  if (HasPost) {
    FB.setBlock(Post);
    for (auto &[Cell, Ty] : LoopCells) {
      ++EscapeSteps;
      at(For);
      LocalId Old = FB.newTemp(FB.function().Locals[Cell].Ty);
      FB.assign(FB.local(Old), FB.use(copyOf(FB.local(Cell))));
      allocCell(Cell, Ty, For);
      // The new iteration's variable starts as a copy of the last one's
      // (CLO-8): the closures of that iteration keep theirs. An owned value
      // is cloned; then this function's reference to the old cell ends.
      Operand V = copyOf(FB.deref(FB.local(Old)));
      if (auto CT = CellTypes.find(Cell);
          CT != CellTypes.end() && L.needsDrop(CT->second)) {
        V = cloneValue(std::move(V), CT->second, For);
        if (V.K == Operand::Copy)
          V.K = Operand::Move;
      }
      at(For);
      FB.assign(FB.deref(FB.local(Cell)), FB.use(std::move(V)));
      Rt E(M, FB);
      callFn(L.rt(RtFn::CellRelease, For->getBegin()),
             {E.to(ctx().ptr(ctx().u8()), E.addrOf(FB.local(Old))).Op});
    }
    if (For->Post)
      lowerStmt(For->Post);
    if (!FB.terminated())
      FB.gotoBlock(Head);
  }
  FB.setBlock(Exit);
  popScope();
}

/// The tests of an expression switch: from the first case to the last, in
/// source order, then the default (or the exit).
void Body::lowerSwitchTests(const ast::SwitchStmt *Sw, ArrayRef<BlockId> Bodies,
                            BlockId Default, BlockId Exit) {
  ArrayRef<ast::Stmt *> Clauses = Sw->Body->List;
  BlockId Otherwise = Default != NoId ? Default : Exit;
  S::Type *TagS = Sw->Tag ? sTyOf(Sw->Tag) : nullptr;
  Type *TagTy = Sw->Tag ? tyOf(Sw->Tag) : nullptr;
  if (Sw->Tag && !TagTy)
    return;
  LocalId Tag = NoId;
  if (Sw->Tag) {
    Operand T = lowerExpr(Sw->Tag);
    Tag = localOf(std::move(T));
  }
  // A switch on an integer with constant cases is one Switch terminator.
  bool AllConst = Tag != NoId && TagTy->isInt();
  if (AllConst)
    for (const ast::Stmt *St : Clauses)
      for (const ast::Expr *E : cast<ast::CaseClause>(St)->List)
        // (A case value that makes the comparison rescale the tag, in units,
        // is tested below one by one.)
        if (!isConst(E) || I.CaseOperators.count(E) ||
            (unitsRecorded() &&
             (unitScaleOf(E, /*Tag=*/true, /*Own=*/false).Any ||
              (dictMode() && I.UnitSites.count(E)))))
          AllConst = false;
  if (AllConst) {
    SmallVector<SwitchCase, 8> Cases;
    for (size_t K = 0; K < Clauses.size(); ++K)
      for (const ast::Expr *E : cast<ast::CaseClause>(Clauses[K])->List) {
        Operand C = constOperand(*I.lookup(E)->Val, TagTy, E);
        if (C.K != Operand::Const)
          return;
        Cases.push_back({C.C.Bits, Bodies[K]});
      }
    at(Sw);
    FB.switchInt(copyOf(FB.local(Tag)), Cases, Otherwise);
    return;
  }
  for (size_t K = 0; K < Clauses.size(); ++K) {
    auto *CC = cast<ast::CaseClause>(Clauses[K]);
    for (const ast::Expr *E : CC->List) {
      Operand Cond;
      if (Tag != NoId && opSwitchCase(Sw->Tag, Tag, E, Cond)) {
        // (a `==` method: the tag against the case value)
      } else if (Tag != NoId) {
        Operand V = valueFor(E, TagS);
        Operand T = copyOf(FB.local(Tag));
        if (unitsRecorded()) {
          // The case value is in a finer unit than the tag: the tag is
          // converted for this comparison (ADR-0025, EXP-34).
          UnitScale U = unitScaleOf(E, /*Tag=*/true, /*Own=*/false);
          if (U.Bad)
            return;
          T = scaleValue(std::move(T), U, E);
        }
        Cond = equality(true, std::move(T), std::move(V), TagS, E);
      } else {
        Cond = lowerAs(E, ctx().boolean());
      }
      at(E);
      BlockId Next = FB.newBlock();
      FB.branch(Cond, Bodies[K], Next);
      FB.setBlock(Next);
    }
  }
  FB.gotoBlock(Otherwise);
}

void Body::lowerSwitch(const ast::SwitchStmt *Sw, std::string Label) {
  pushScope();
  if (Sw->Init)
    lowerStmt(Sw->Init, /*KeepTemps=*/true);
  ArrayRef<ast::Stmt *> Clauses = Sw->Body->List;
  SmallVector<BlockId, 8> Bodies;
  BlockId Default = NoId;
  for (size_t K = 0; K < Clauses.size(); ++K) {
    Bodies.push_back(FB.newBlock());
    if (cast<ast::CaseClause>(Clauses[K])->IsDefault)
      Default = Bodies.back();
  }
  BlockId Exit = FB.newBlock();
  lowerSwitchTests(Sw, Bodies, Default, Exit);
  Targets.push_back({Exit, NoId, Scopes.size(), std::move(Label)});
  BlockId SavedFall = Fallthrough;
  size_t SavedDepth = ClauseDepth;
  for (size_t K = 0; K < Clauses.size(); ++K) {
    auto *CC = cast<ast::CaseClause>(Clauses[K]);
    FB.setBlock(Bodies[K]);
    Fallthrough = K + 1 < Clauses.size() ? Bodies[K + 1] : NoId;
    pushScope(true, CC);
    ClauseDepth = Scopes.size();
    for (const ast::Stmt *St : CC->Body)
      lowerStmt(St);
    popScope();
    if (!FB.terminated())
      FB.gotoBlock(Exit);
  }
  Fallthrough = SavedFall;
  ClauseDepth = SavedDepth;
  Targets.pop_back();
  FB.setBlock(Exit);
  popScope();
}

BlockId Body::labelBlock(StringRef Name) {
  auto It = Labels.find(Name);
  if (It != Labels.end())
    return It->second;
  BlockId B = FB.newBlock();
  Labels[Name] = B;
  return B;
}

void Body::lowerLabeled(const ast::LabeledStmt *LS) {
  BlockId B = labelBlock(LS->Label->Name.str());
  LabelMarks[LS->Label->Name.str()] = Scopes.back().Locals.size();
  at(LS);
  if (!FB.terminated())
    FB.gotoBlock(B);
  FB.setBlock(B);
  PendingLabel = LS->Label->Name.str();
  lowerStmt(LS->Body);
}

SourceLoc Body::labelPos(StringRef Name) {
  if (!LabelsScanned || L.Opts.RescanLabels) {
    LabelsScanned = true;
    // The statements that hold statements (as the checker's label walk).
    SmallVector<const ast::Stmt *, 16> Work;
    if (RootBody)
      Work.push_back(RootBody);
    auto All = [&](ArrayRef<ast::Stmt *> List) {
      for (const ast::Stmt *S : List)
        Work.push_back(S);
    };
    using K = ast::Node::Kind;
    while (!Work.empty()) {
      const ast::Stmt *S = Work.pop_back_val();
      ++LowerSteps;
      if (!S)
        continue;
      switch (S->getKind()) {
      case K::LabeledStmt: {
        auto *LS = cast<ast::LabeledStmt>(S);
        LabelPositions.try_emplace(LS->Label->Name.str(), LS->getBegin());
        Work.push_back(LS->Body);
        break;
      }
      case K::BlockStmt:
        All(cast<ast::BlockStmt>(S)->List);
        break;
      case K::IfStmt:
        Work.push_back(cast<ast::IfStmt>(S)->Then);
        Work.push_back(cast<ast::IfStmt>(S)->Else);
        break;
      case K::SwitchStmt:
        All(cast<ast::SwitchStmt>(S)->Body->List);
        break;
      case K::TypeSwitchStmt:
        All(cast<ast::TypeSwitchStmt>(S)->Body->List);
        break;
      case K::SelectStmt:
        All(cast<ast::SelectStmt>(S)->Body->List);
        break;
      case K::CaseClause:
        All(cast<ast::CaseClause>(S)->Body);
        break;
      case K::CommClause:
        All(cast<ast::CommClause>(S)->Body);
        break;
      case K::ForStmt:
        Work.push_back(cast<ast::ForStmt>(S)->Body);
        break;
      case K::RangeStmt:
        Work.push_back(cast<ast::RangeStmt>(S)->Body);
        break;
      default:
        break;
      }
    }
  }
  return LabelPositions.lookup(Name);
}

void Body::leaveForGoto(StringRef Name) {
  if (FB.terminated())
    return;
  // The label is in the statement list that holds the goto or in one around
  // it (Go spec): the innermost list of the scopes that holds its position.
  SourceLoc To = labelPos(Name);
  size_t D = Scopes.size();
  for (; D > 0; --D) {
    ++LowerSteps;
    const Scope &Sc = Scopes[D - 1];
    if (Sc.ListBegin.isValid() && !(To < Sc.ListBegin) && To < Sc.ListEnd)
      break;
  }
  if (D == 0) {
    L.internalError(To, "a goto whose label is in no statement list around it");
    return;
  }
  // Every scope inside the label's list ends (its map iterations too, RNG-4).
  exitScopes(D);
  // In the list: a jump back ends what was made after the label (variables
  // it jumps back over, OWN-R2, and the temporaries of the statement); a jump
  // forward ends the temporaries of the statement that holds the goto (Go
  // jumps over no declaration).
  SmallVector<LocalId, 4> &Locs = Scopes[D - 1].Locals;
  auto LM = LabelMarks.find(Name);
  size_t Mark = LM != LabelMarks.end() ? LM->second : Scopes[D - 1].StmtMark;
  for (size_t K = Locs.size(); K > Mark; --K) {
    ++LowerSteps;
    FB.storageDead(Locs[K - 1]);
  }
}

void Body::lowerBranch(const ast::BranchStmt *B) {
  at(B);
  switch (B->Keyword) {
  case tok::kw_goto: {
    leaveForGoto(B->Label->Name.str());
    at(B);
    FB.gotoBlock(labelBlock(B->Label->Name.str()));
    startDeadBlock();
    return;
  }
  case tok::kw_fallthrough:
    if (Fallthrough == NoId) {
      unsupported(B, "this fallthrough");
      return;
    }
    exitScopes(ClauseDepth - 1);
    FB.gotoBlock(Fallthrough);
    startDeadBlock();
    return;
  case tok::kw_break:
  case tok::kw_continue: {
    bool Break = B->Keyword == tok::kw_break;
    const BranchTarget *T = nullptr;
    for (size_t K = Targets.size(); K > 0 && !T; --K) {
      const BranchTarget &C = Targets[K - 1];
      if (B->Label ? C.Label == B->Label->Name.str()
                   : (Break || C.Continue != NoId))
        T = &C;
    }
    if (!T || (!Break && T->Continue == NoId)) {
      unsupported(B, "this branch statement");
      return;
    }
    exitScopes(T->Depth);
    FB.gotoBlock(Break ? T->Break : T->Continue);
    startDeadBlock();
    return;
  }
  default:
    unsupported(B, "this branch statement");
    return;
  }
}

//===----------------------------------------------------------------------===//
// Return
//===----------------------------------------------------------------------===//

void Body::lowerReturn(const ast::ReturnStmt *R) {
  at(R);
  Function &F = FB.function();
  Type *Ret = F.Sig->ret();
  SmallVector<Operand, 2> Vals;
  // The number of results is the checker's: one func value or complex
  // number is a pair in GIR, and no list of two (P04.07).
  size_t Want = !NamedResults.empty() ? NamedResults.size()
                : Ret->isUnit()       ? 0
                : ResultTys.empty()   ? 1
                                      : ResultTys.size();
  if (FnSig && FnSig->Fails) {
    // `!` (ERR-3): the declared results, or them plus the failure.
    if (!lowerFailsReturn(R, Vals))
      return;
  } else if (R->Results.empty()) {
    // A bare return gives the named results.
    for (size_t K = 0; K < NamedResults.size(); ++K) {
      Operand V = copyOf(NamedResults[K]);
      // The result variable gives its value up (OWN-R3).
      if (K < ResultTys.size() && L.needsDrop(ResultTys[K]))
        V.K = Operand::Move;
      Vals.push_back(std::move(V));
    }
  } else {
    if (R->Results.size() == Want) {
      for (size_t K = 0; K < Want; ++K) {
        S::Type *Wanted =
            K < ResultTys.size() ? ResultTys[K] : sTyOf(R->Results[K]);
        Vals.push_back(ownedValue(R->Results[K], Wanted));
      }
    } else if (R->Results.size() == 1 && Want > 1) {
      // return f() with several results.
      SmallVector<S::Type *, 2> To;
      for (S::Type *T : ResultTys)
        To.push_back(T);
      if (!multiValues(R->Results[0], Want, Vals, To))
        return;
    } else {
      unsupported(R, "this return");
      return;
    }
  }
  at(R);
  if (!Ret->isUnit()) {
    if (Vals.size() == 1)
      FB.assign(FB.local(FB.returnPlace()), FB.use(Vals[0]));
    else
      FB.assign(FB.local(FB.returnPlace()),
                FB.aggregate(AggKind::Tuple, Ret, Vals));
  }
  exitScopes(0);
  FB.ret();
  startDeadBlock();
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

void Body::lowerBlock(const ast::BlockStmt *B) {
  pushScope(B != RootBody || ShadowBody, B);
  ShadowBody = false;
  for (const ast::Stmt *St : B->List)
    lowerStmt(St);
  popScope();
}

void Body::lowerStmt(const ast::Stmt *St, bool KeepTemps) {
  // The temporaries of a statement die at its end (OWN-R2): the owned ones
  // made while it ran, in the scope it ran in. A declaration of a variable
  // that borrows from them keeps them until the end of the block (OWN-V6,
  // TYP-9: lifetime extension); they die with the block's variables.
  const size_t Depth = Scopes.size(), Mark = Scopes.back().Locals.size();
  const size_t SavedStmtMark = Scopes.back().StmtMark;
  Scopes.back().StmtMark = Mark;
  const bool SavedExtend = ExtendTemps;
  ExtendTemps = false;
  lowerStmtImpl(St);
  if (Scopes.size() == Depth) {
    if (!ExtendTemps && !KeepTemps)
      dropTempsSince(Depth, Mark);
    Scopes.back().StmtMark = SavedStmtMark;
  }
  ExtendTemps = SavedExtend;
}

void Body::lowerStmtImpl(const ast::Stmt *St) {
  using K = ast::Node::Kind;
  std::string Label;
  Label.swap(PendingLabel);
  switch (St->getKind()) {
  case K::EmptyStmt:
    return;
  case K::BlockStmt:
    lowerBlock(cast<ast::BlockStmt>(St));
    return;
  case K::LabeledStmt:
    lowerLabeled(cast<ast::LabeledStmt>(St));
    return;
  case K::ExprStmt: {
    const ast::Expr *E = unparen(cast<ast::ExprStmt>(St)->X);
    if (auto *C = dyn_cast<ast::CallExpr>(E)) {
      at(St);
      lowerExpr(C);
      return;
    }
    // `<-q` through a `<-` method (CON-22).
    if (hasOpMethod(E)) {
      at(St);
      lowerExpr(E);
      return;
    }
    unsupported(St, "this expression statement");
    return;
  }
  case K::AssignStmt:
    lowerAssign(cast<ast::AssignStmt>(St));
    return;
  case K::IncDecStmt:
    lowerIncDec(cast<ast::IncDecStmt>(St));
    return;
  case K::IfStmt:
    lowerIf(cast<ast::IfStmt>(St));
    return;
  case K::ForStmt:
    lowerFor(cast<ast::ForStmt>(St), std::move(Label));
    return;
  case K::SwitchStmt:
    lowerSwitch(cast<ast::SwitchStmt>(St), std::move(Label));
    return;
  case K::BranchStmt:
    lowerBranch(cast<ast::BranchStmt>(St));
    return;
  case K::ReturnStmt:
    lowerReturn(cast<ast::ReturnStmt>(St));
    return;
  case K::DeclStmt: {
    auto *GD = dyn_cast_or_null<ast::GenDecl>(cast<ast::DeclStmt>(St)->D);
    if (!GD || GD->IsUnit) {
      unsupported(St, "this declaration");
      return;
    }
    if (GD->Keyword == tok::kw_type) {
      lowerTypeDecl(GD);
      return;
    }
    if (GD->Keyword == tok::kw_const)
      return; // folded by the checker
    if (GD->Keyword == tok::kw_var) {
      for (const ast::Spec *Sp : GD->Specs)
        lowerValueSpec(cast<ast::ValueSpec>(Sp));
      return;
    }
    unsupported(St, "this declaration");
    return;
  }
  case K::RangeStmt:
    lowerRange(cast<ast::RangeStmt>(St), std::move(Label));
    return;
  case K::TypeSwitchStmt:
    lowerTypeSwitch(cast<ast::TypeSwitchStmt>(St), std::move(Label));
    return;
  case K::DeferStmt:
    unsupported(St, "a defer statement (P04.26)");
    return;
  case K::SendStmt:
    // `q <- v` through a `<-` method (CON-22): an ordinary call.
    if (const S::OperatorUse *U = opUse(St); U && opResolved(*U)) {
      lowerOpCallStmt(St, *U);
      return;
    }
    unsupported(St, "concurrency statements (P12)");
    return;
  case K::GoStmt:
  case K::SelectStmt:
    unsupported(St, "concurrency statements (P12)");
    return;
  default:
    unsupported(St, std::string("the statement ") + St->getKindName());
    return;
  }
}

void Body::finish() {
  Function &F = FB.function();
  // The unit types of the variables, for the debugger (UNI-9: the local has
  // the base type; P04.24, DebugInfoBuilder::variable).
  if (L.CP.Ctx->unitsSeen()) {
    auto Name = [&](const S::Var *V, LocalId Loc) {
      // (A unit this instance does not know has no name: the instance's type
      // has it erased, `float64[U*KM]` is `float64[KM]` there.)
      if (erasedUnitOf(V))
        return;
      auto *B = dyn_cast_or_null<S::BasicType>(tyOfObj(V));
      if (!B || !B->Unit || B->BK == S::BasicKind::UnitArg ||
          !F.Locals[Loc].Source)
        return;
      F.Locals[Loc].UnitName = L.unitSpeller().text(
          B, L.CP.Pkg && L.CP.Pkg->Name == "main" ? L.CP.Pkg : nullptr);
    };
    for (auto &KV : Vars)
      Name(KV.first, KV.second);
    for (auto &KV : Aliases)
      Name(KV.first, KV.second);
  }
  for (BasicBlock &BB : F.Blocks)
    if (!BB.Term.Set) {
      BB.Term.K = Terminator::Unreachable;
      BB.Term.Set = true;
    }
  removeDeadBlocks(F);
  // A body that could not be lowered whole is not checked (its moves and
  // writes are partial); the module fails anyway.
  if (L.Reported != StartReported)
    return;
  // Use after move (OWN-R3), on the graph with its moves in place.
  for (const MoveError &E : checkMoves(F)) {
    SourceLoc At = L.locOf(E.Use);
    L.Failed = true;
    ++L.Reported;
    auto D = L.Diags.report(At, diag::err_use_after_move);
    D << F.Locals[E.Local].Name;
    if (SourceLoc From = L.locOf(E.Moved); From.isValid())
      D.note(From, "moved here");
  }
  if (L.Opts.CheckOnly || L.Failed)
    return;
  // The drops (P04.09): the one pass that knows which owners are live.
  std::string Err;
  if (!elaborateDrops(M, F, &Err))
    L.internalError(Pos, "drop elaboration of " + F.Name + ": " + Err);
}

void Body::prologueSince(size_t Mark) {
  // (Only the entry block holds the function's entry, V077.)
  if (FB.block() != 0)
    return;
  std::vector<Statement> &Stmts = FB.function().Blocks[0].Stmts;
  for (size_t K = Mark; K < Stmts.size(); ++K)
    Stmts[K].Prologue = true;
}

bool Body::bindResults(ArrayRef<ResultVar> Rs, SourceLoc At) {
  Function &F = FB.function();
  for (const ResultVar &R : Rs) {
    Type *T = L.lowerType(R.Ty, At);
    if (!T)
      return false;
    const size_t Mark = entryMark();
    if (R.V && Facts && Facts->Cells.count(R.V)) {
      // A named result that an escaping closure captures lives in a cell
      // (enterCells). Until it is made, the variable is a zeroed slot of the
      // frame, so a debugger never reads through a pointer not yet set.
      LocalId Init = FB.newLocal(T, Local::Var, R.Name.str() + ".v");
      LocalId Cell = FB.newLocal(ctx().ptr(T), Local::Var, R.Name.str());
      F.Locals[Cell].Source = true;
      F.Locals[Cell].Deref = true;
      ownCell(Cell, R.Ty);
      Aliases[R.V] = Cell;
      Scopes.back().Locals.push_back(Init);
      Scopes.back().Locals.push_back(Cell);
      FB.storageLive(Init);
      FB.assign(FB.local(Init), FB.use(zeroOf(T)));
      FB.storageLive(Cell);
      FB.assign(FB.local(Cell), FB.addrOf(FB.local(Init), true));
      prologueSince(Mark);
      EntryCells.push_back({Cell, T});
      NamedResults.push_back(FB.deref(FB.local(Cell)));
      continue;
    }
    VarSlot Sl = declareSlot(R.V, nullptr, R.Name, T);
    FB.assign(Sl.P, FB.use(zeroOf(T)));
    prologueSince(Mark);
    NamedResults.push_back(Sl.P);
  }
  return true;
}

void Body::bindParam(unsigned K, const S::Var *V, Lowering::Pass P,
                     SourceLoc At) {
  Function &F = FB.function();
  // A borrowed string or `[]T` arrives as the pair (ptr, len) (D4.4,
  // ADR-0029 point 7): a parameter or receiver that passOf makes a View.
  // The body works on a variable of its own made from it: a string with
  // capacity 0 (it owns no buffer), a `[]T` with the length as capacity
  // (SLC-12) and a null allocator word that nothing reads (a borrowed slice
  // is never grown, cloned in its allocator or freed). The pair and the
  // variable both carry the parameter's name (the debug-info hand-off, gir.md
  // section 15).
  // The debugger shows the parameter (P04.12): the value, or, for a Place, the
  // caller's place it points to (Local::Deref); for a View, the variable
  // made from the pair.
  F.Locals[FB.param(K)].Name = V->Name.str();
  switch (P) {
  case Lowering::Pass::Place:
    // A pointer to the caller's place (PAR-10).
    Aliases[V] = FB.param(K);
    F.Locals[FB.param(K)].Source = true;
    F.Locals[FB.param(K)].Deref = true;
    return;
  case Lowering::Pass::Value:
    Vars[V] = FB.param(K);
    F.Locals[FB.param(K)].Source = true;
    // A `move` parameter owns its value: the function drops it (P04.09).
    if (modeOf(V) == S::Var::Owned || modeOf(V) == S::Var::MoveParam)
      ownLocal(FB.param(K), tyOfObj(V));
    return;
  case Lowering::Pass::View:
    break;
  }
  // (The pair of a string and of a []byte are alike: the variable's type
  // tells them apart.)
  Type *GT = L.lowerType(tyOfObj(V), At);
  if (!GT)
    return;
  bool Str = GT->getKind() == Type::Kind::String;
  Rt E(M, FB);
  // The statements that make the variable are the function's entry
  // (Statement::Prologue, V077): a debugger stops after them at `break f`.
  const size_t Mark = entryMark();
  LocalId Loc = FB.newLocal(GT, Local::Var, V->Name.str());
  // The debugger's parameter is this variable, not the pair (P04.12).
  F.Locals[Loc].Source = true;
  F.Locals[Loc].ArgNo = K + 1;
  FB.storageLive(Loc);
  Val In = E.param(K);
  Val Ptr = E.fld(In, 0), Len = E.fld(In, 1);
  // (A borrowed string's capacity is its length, as a borrowed slice's: its
  // bytes are another's, which a clone copies; borrowedSeq.)
  if (Str)
    FB.assign(FB.local(Loc),
              FB.aggregate(AggKind::String, GT, {Ptr.Op, Len.Op, Len.Op}));
  else
    FB.assign(FB.local(Loc), FB.aggregate(AggKind::Slice, GT,
                                          {Ptr.Op, Len.Op, Len.Op,
                                           E.null(ctx().ptr(ctx().u8())).Op}));
  prologueSince(Mark);
  Scopes.back().Locals.push_back(Loc);
  Vars[V] = Loc;
}

void Body::cellParams(ArrayRef<S::Var *> Params) {
  // A parameter that a literal which escapes captures moves into a cell (a
  // Place parameter is the caller's storage, and an escaping closure may not
  // hold it: mayHold, CLO-3a). In the function's entry the cell's pointer
  // points at the parameter itself, so the variable a debugger shows is
  // right from the first instruction; enterCells makes the cell.
  Function &F = FB.function();
  for (S::Var *V : Params) {
    if (!Facts || !Facts->Cells.count(V) || Aliases.count(V))
      continue;
    auto It = Vars.find(V);
    if (It == Vars.end())
      continue;
    LocalId Param = It->second;
    Type *Ty = F.Locals[Param].Ty;
    const size_t Mark = entryMark();
    LocalId Cell = FB.newLocal(ctx().ptr(Ty), Local::Var, V->Name.str());
    F.Locals[Cell].Source = true;
    F.Locals[Cell].Deref = true;
    ownCell(Cell, tyOfObj(V));
    // The cell stands for the parameter in the debugger: its number is the
    // parameter's (a View's variable has it already; V074).
    const uint32_t Arg = F.Locals[Param].ArgNo;
    F.Locals[Cell].ArgNo = Arg ? Arg : (Param <= F.numParams() ? Param : 0);
    F.Locals[Param].Source = false;
    F.Locals[Param].ArgNo = 0;
    Scopes.back().Locals.push_back(Cell);
    FB.storageLive(Cell);
    FB.assign(FB.local(Cell), FB.addrOf(FB.local(Param), true));
    prologueSince(Mark);
    EntryCells.push_back({Cell, Ty});
    Vars.erase(V);
    Aliases[V] = Cell;
  }
}

void Body::enterCells(const ast::BlockStmt *Block) {
  // After the entry (the allocation is a call): each variable moves from the
  // slot its pointer points at into a new cell, the pointer last. This code
  // is on the first line of the body (review R4): LLVM ends the prologue
  // search at a call, so with no line (line 0) the call would get
  // `prologue_end` and the declaration's line, and `break f` would stop on
  // the declaration. On the body's first line, `prologue_end` is there, and
  // the debugger reads each variable through its pointer: the parameter's
  // own slot before the move, the cell after it.
  if (EntryCells.empty())
    return;
  FB.setSpan(L.spanOf(Block->List.empty() ? Block->Rbrace
                                          : Block->List.front()->getBegin()));
  for (auto &[Cell, Ty] : EntryCells) {
    ++EscapeSteps;
    LocalId Old = FB.newTemp(ctx().ptr(Ty));
    FB.assign(FB.local(Old), FB.use(copyOf(FB.local(Cell))));
    allocCell(Cell, Ty, nullptr);
    // The value moves into the cell: the parameter's own slot is left zero
    // (its drop at the return does nothing, P04.09). A cell may outlive the
    // call, so an interface value lent to it is boxed (takeValue, R1).
    Operand V = copyOf(FB.deref(FB.local(Old)));
    if (auto CT = CellTypes.find(Cell);
        CT != CellTypes.end() && L.needsDrop(CT->second))
      V = takeValue(std::move(V), CT->second, /*Move=*/true);
    FB.assign(FB.deref(FB.local(Cell)), FB.use(std::move(V)));
  }
  EntryCells.clear();
}

void Body::endBody(const ast::BlockStmt *Block) {
  if (FB.terminated())
    return;
  Function &F = FB.function();
  FB.setSpan(L.spanOf(Block->Rbrace));
  if (F.Sig->ret()->isUnit()) {
    exitScopes(0);
    FB.ret();
  } else if (FnSig && FnSig->Fails && FnSig->Results->size() == 1) {
    // `func f!()` ends with a nil error (ERR-3).
    FB.assign(FB.local(FB.returnPlace()), FB.use(zeroOf(F.Sig->ret())));
    exitScopes(0);
    FB.ret();
  } else {
    FB.unreachable();
  }
}

void Body::lower(const ast::FuncDecl *FD) {
  Function &F = FB.function();
  at(FD);
  pushScope(false);
  if (Obj) {
    ArrayRef<S::Var *> Params = Obj->Params;
    // The receiver of a method is parameter 0.
    const size_t Lead = Obj->Recv ? 1 : 0;
    const size_t Hidden = CurInst ? CurInst->DictParams.size() : 0;
    if (Params.size() + Lead + Hidden != F.numParams()) {
      unsupported(FD, "this parameter list");
      return;
    }
    // The unit descriptors of the instance (GEN-14), the last parameters.
    for (size_t K = 0; K < Hidden; ++K) {
      LocalId P = FB.param(Params.size() + Lead + K);
      F.Locals[P].Name = "unit";
      DictLocals[CurInst->DictParams[K]] = P;
      DictIndex[CurInst->DictParams[K]] = K;
    }
    // (At an instance of generic code, the types of the instance: P04.08.)
    FnSig = sigOf(Obj);
    if (Obj->Recv)
      bindParam(0, Obj->Recv, L.passOfRecvAt(Obj, tyOfObj(Obj->Recv)),
                FD->getBegin());
    for (size_t K = 0; K < Params.size(); ++K)
      bindParam(K + Lead, Params[K], L.passOf(FnSig, K), FD->getBegin());
    for (const S::Var *R : Obj->Results)
      ResultTys.push_back(tyOfObj(R));
    // Named results are variables, set to their zero values.
    SmallVector<ResultVar, 2> Rs;
    bool Named = false;
    for (const S::Var *R : Obj->Results) {
      Rs.push_back({R, tyOfObj(R), R->Name});
      Named |= !R->Name.empty();
    }
    if (Named && !bindResults(Rs, FD->getBegin()))
      return;
    cellParams(Params);
    if (Obj->Recv)
      cellParams(ArrayRef<S::Var *>(Obj->Recv));
    enterCells(FD->Body);
  }
  RootBody = FD->Body;
  // The unit descriptors and factors the body needs are made once, here at
  // the entry of the function (GEN-14): a block of their own that the entry
  // jumps through and the body's uses fill (Hoist).
  BlockId BodyStart = NoId;
  if (dictMode() && !DictLocals.empty()) {
    // (The spill chain of the descriptors this body makes: in the function's
    // scope before any return is lowered, so every exit drops it; owned only
    // if a descriptor slot uses it, below.)
    SpillLocal = FB.newLocal(ctx().ptr(ctx().u8()), Local::Var, "unitspill");
    Scopes.back().Locals.push_back(SpillLocal);
    FB.storageLive(SpillLocal);
    HoistBlock = FB.newBlock();
    HoistScope = FB.scope();
    BodyStart = FB.newBlock();
    FB.gotoBlock(HoistBlock);
    FB.setBlock(BodyStart);
  }
  lowerBlock(FD->Body);
  endBody(FD->Body);
  if (HoistBlock != NoId) {
    FB.setBlock(HoistBlock);
    FB.setSpan(L.spanOf(FD->getBegin()));
    FB.gotoBlock(BodyStart);
    HoistBlock = NoId;
  }
  if (SpillUsed) {
    Local &Lo = F.Locals[SpillLocal];
    Lo.Owned = true;
    Lo.Glue = L.UnitRt.SpillFree;
  }
  Scopes.pop_back();
  finish();
}

void Body::lowerInit(ArrayRef<S::InitEntry> Order, ArrayRef<FuncId> InitFuncs) {
  Function &F = FB.function();
  FB.setSpan(F.Loc);
  pushScope(false);
  const sym::Node *PkgScope = ScopeNode;
  for (const S::InitEntry &E : Order) {
    at(E.Init);
    // The literals of an initializer are the closures of its variable.
    EntityFacts EF;
    analyzeEntity({E.Init}, nullptr, {}, L.Keeps, L.FuncHolders, I, EF);
    Facts = &EF;
    if (!E.Lhs.empty())
      ScopeNode = L.Syms.Names.var(E.Lhs[0]);
    struct Restore {
      Body &B;
      const sym::Node *Scope;
      ~Restore() {
        B.Facts = nullptr;
        B.ScopeNode = Scope;
      }
    } Reset{*this, PkgScope};
    if (E.Lhs.size() == 1) {
      S::Var *V = E.Lhs[0];
      Operand Val = ownedValue(E.Init, tyOfObj(V));
      auto G = L.Globals.find(V);
      if (G != L.Globals.end()) {
        at(E.Init);
        FB.assign(FB.global(G->second), FB.use(Val));
      }
      continue;
    }
    SmallVector<Operand, 2> Vals;
    SmallVector<S::Type *, 2> To;
    for (S::Var *V : E.Lhs)
      To.push_back(tyOfObj(V));
    if (!multiValues(E.Init, E.Lhs.size(), Vals, To))
      continue;
    for (size_t K = 0; K < E.Lhs.size(); ++K) {
      auto G = L.Globals.find(E.Lhs[K]);
      if (G != L.Globals.end()) {
        at(E.Init);
        FB.assign(FB.global(G->second), FB.use(Vals[K]));
      }
    }
  }
  FB.setSpan(F.Loc);
  for (FuncId Id : InitFuncs)
    FB.call(constFunc(M, Id), {}, FB.local(FB.newTemp(ctx().unit())),
            {UnwindAction::Continue, NoId});
  exitScopes(0);
  FB.ret();
  Scopes.pop_back();
  finish();
}

//===----------------------------------------------------------------------===//
// Range (Go spec "For statements with range clause"; spec RNG-1 to RNG-6)
//===----------------------------------------------------------------------===//

void Body::endMapIteration(LocalId It) {
  FuncId F = L.rt(RtFn::MapIterDone, SourceLoc());
  if (F == NoId)
    return;
  Operand P = copyOf(FB.temp(FB.addrOf(FB.local(It))));
  FB.call(constFunc(M, F), {P}, FB.local(FB.newTemp(ctx().unit())),
          {UnwindAction::Continue, NoId});
}

/// `for k, v := range x` and `for k, v = range x` over an integer, an array,
/// a pointer to an array, a slice, a string, a View or a map. The range
/// expression is evaluated once, before the loop; a slice or string is
/// walked as it was then, so an `append` in the body does not extend the
/// loop. An array whose elements the loop copies is copied first, as in Go
/// (RNG-6); an array of borrowed elements, and the `&` form, walk the live
/// array. A string is decoded as UTF-8: a byte that does not start a valid
/// sequence is U+FFFD and one byte wide. `&v` (RNG-1a) is the element
/// itself: a pointer local whose uses are a Deref. A range over a map is a
/// running iteration of the map until the loop is left on any path (RNG-4).
void Body::lowerRange(const ast::RangeStmt *R, std::string Label) {
  ++LowerSteps;
  if (hasOpMethod(R)) {
    // `for v := range t` through a `range` method lowers as range-over-func.
    unsupported(R, "a range over a user type with a range method "
                   "(range over func, P04.27)");
    return;
  }
  pushScope();
  S::Type *XS = sTyOf(R->X);
  S::Type *U = XS ? S::under(XS) : nullptr;
  if (!U) {
    unsupported(R, "this range loop");
    popScope();
    return;
  }
  Rt E(M, FB);
  Type *U64 = ctx().u64(), *I64 = ctx().i64();
  bool Define = R->Op == tok::define;
  auto IsBlank = [&](const ast::Expr *T) {
    if (!T)
      return true;
    auto *Id = dyn_cast<ast::Ident>(unparen(T));
    return Id && Id->Name.str() == "_";
  };
  bool WantKey = !IsBlank(R->Key), WantVal = !IsBlank(R->Value);
  // RNG-1a: `&v` is the element itself (the checker allows it on the value
  // variable of a `:=` over a slice, an array or a map).
  bool Alias =
      Define && WantVal && R->Alias == ast::RangeStmt::AliasKind::Value;
  at(R);

  // What is walked.
  enum class Walk { Int, Array, PtrArray, Seq, Str, Map } W = Walk::Int;
  SeqInfo SI = seqInfo(XS);
  S::MapType *MT = mapTypeOf(XS);
  Type *IntG = nullptr;
  if (MT) {
    W = Walk::Map;
  } else if (SI.K == SeqKind::String || SI.K == SeqKind::ViewString) {
    W = Walk::Str;
  } else if (SI.K == SeqKind::Slice || SI.K == SeqKind::ViewSlice) {
    W = Walk::Seq;
  } else if (SI.K == SeqKind::Array) {
    W = Walk::Array;
  } else if (SI.K == SeqKind::PtrArray) {
    W = Walk::PtrArray;
  } else if (auto *B = dyn_cast<S::BasicType>(U);
             B && (B->BK != S::BasicKind::String)) {
    IntG = tyOf(R->X);
    if (!IntG || !IntG->isInt()) {
      unsupported(R, "this range loop");
      popScope();
      return;
    }
  } else {
    unsupported(R, "a range over this type (P04.27, P12)");
    popScope();
    return;
  }

  // The checker's value variable of `:=`: a borrowed one (owning elements,
  // RNG-1) borrows the live element, so an array of them is not copied.
  const S::Var *ValVar = nullptr;
  if (Define && WantVal)
    if (auto *Id = dyn_cast<ast::Ident>(unparen(R->Value))) {
      auto D = I.Defs.find(Id);
      if (D != I.Defs.end())
        ValVar = dyn_cast_or_null<S::Var>(D->second);
    }
  bool CopyArray =
      WantVal && !Alias && !(ValVar && modeOf(ValVar) == S::Var::Borrowed);

  // The range expression, once.
  LocalId IdxL = NoId, LenL = NoId;
  Place Seq;    // the header (Seq, Str) or the array (Array, PtrArray)
  Operand MapV; // Map
  uint64_t ArrLen = 0;
  if (W == Walk::Int) {
    Operand N = lowerAs(R->X, IntG);
    at(R);
    LenL = localOf(std::move(N));
  } else if (W == Walk::Map) {
    MapV = copyOf(FB.local(localOf(mapValue(R->X))));
  } else if (W == Walk::Seq || W == Walk::Str) {
    Place Q = SI.K == SeqKind::String || SI.K == SeqKind::Slice
                  ? placeOf(lowerExpr(R->X))
                  : seqPlace(lowerExpr(R->X), SI);
    at(R);
    Seq = FB.temp(FB.use(copyOf(Q)));
    LenL = localOf(copyOf(FB.temp(FB.len(Seq))));
  } else if (W == Walk::Array) {
    Place A = lowerPlace(R->X);
    if (!A.Ty)
      return popScope();
    at(R);
    ArrLen = A.Ty->length();
    if (CopyArray)
      Seq = FB.temp(FB.use(copyOf(A)));
    else // the live array, at the address it has now
      Seq = FB.deref(FB.local(localOf(copyOf(FB.temp(FB.addrOf(A, Alias))))));
  } else {
    Operand P = lowerExpr(R->X);
    if (!P.type()->isPtr() ||
        P.type()->elem()->getKind() != Type::Kind::Array) {
      unsupported(R, "this range loop");
      return popScope();
    }
    at(R);
    ArrLen = P.type()->elem()->length();
    Seq = FB.deref(FB.local(localOf(std::move(P))));
  }

  // The element type, and the map's key and value types.
  Type *KG = nullptr, *VG = nullptr;
  if (W == Walk::Map) {
    KG = L.lowerType(MT->Key, R->getBegin());
    VG = L.lowerType(MT->Elem, R->getBegin());
    if (!KG || !VG)
      return popScope();
  } else if (W != Walk::Int && W != Walk::Str) {
    VG = Seq.Ty->elem();
  }

  // The variables of `:=`, one per name, declared before the loop and set at
  // the start of each iteration; an `&` variable is a pointer to the element.
  LocalId ValLoc = NoId;
  VarSlot KeyS, ValS;
  // RNG-1, P04.09: a borrowed value variable (an owning element) is the
  // element itself, not a copy of its header: a nil map made through it
  // (`d.Index["x"] = 1`, `put(d)`, `d.Put()`) is made in the collection
  // (ADR-0018 amendment point 5). It is the `&` form's pointer, read only.
  const bool InPlace =
      Define && WantVal && !Alias && ValVar && VG &&
      modeOf(ValVar) == S::Var::Borrowed &&
      (W == Walk::Array || W == Walk::PtrArray || W == Walk::Map ||
       (W == Walk::Seq && SI.K == SeqKind::Slice)) &&
      L.lowerType(tyOfObj(ValVar), R->getBegin()) == VG &&
      !(Facts && Facts->Cells.count(ValVar));
  auto DeclareDef = [&](const ast::Expr *T, VarSlot &Sl) {
    if (IsBlank(T))
      return true;
    auto *Id = cast<ast::Ident>(unparen(T));
    auto D = I.Defs.find(Id);
    if (D == I.Defs.end() || !D->second) {
      unsupported(T, "this range variable");
      return false;
    }
    Type *Ty = L.lowerType(tyOfObj(D->second), Id->getBegin());
    if (!Ty)
      return false;
    Sl = declareVar(Id, Ty, /*AllocNow=*/false);
    return true;
  };
  if (Define) {
    if (!DeclareDef(R->Key, KeyS))
      return popScope();
    if (Alias || InPlace) {
      if (!ValVar || !VG) {
        unsupported(R->Value, "this `&` range variable");
        return popScope();
      }
      ValLoc = FB.newLocal(ctx().ptr(VG), Local::Var,
                           cast<ast::Ident>(unparen(R->Value))->Name.str());
      FB.function().Locals[ValLoc].Source = true;
      FB.function().Locals[ValLoc].Deref = true;
      Scopes.back().Locals.push_back(ValLoc);
      FB.storageLive(ValLoc);
      Aliases[ValVar] = ValLoc;
    } else if (!DeclareDef(R->Value, ValS)) {
      return popScope();
    }
  }

  // `move(m[k])` of the current entry moves the value variable (ADR-0016
  // point 5): the map variable, the key variable and the value's place.
  bool PushedRange = false;
  if (W == Walk::Map && Define && (ValS.P.Ty || InPlace) && R->Key)
    if (auto *MId = dyn_cast<ast::Ident>(unparen(R->X)))
      if (auto *KId = dyn_cast<ast::Ident>(unparen(R->Key))) {
        auto MU = I.Uses.find(MId);
        auto KD = I.Defs.find(KId);
        if (MU != I.Uses.end() && KD != I.Defs.end() && KD->second) {
          MapRanges.push_back({MU->second, KD->second,
                               InPlace ? FB.deref(FB.local(ValLoc)) : ValS.P});
          PushedRange = true;
        }
      }
  // The map iterator (its iteration ends with this scope, on every way out)
  // and the entry pointers.
  LocalId ItL = NoId;
  Place KP, VP;
  Operand InfoV;
  if (W == Walk::Map) {
    InfoV = copyOf(FB.local(localOf(mapInfoPtr(MT, R))));
    ItL = FB.newLocal(L.mapIterType(), Local::Var, "it");
    KP = FB.local(FB.newLocal(ctx().ptr(ctx().u8()), Local::Var, "kp"));
    VP = FB.local(FB.newLocal(ctx().ptr(ctx().u8()), Local::Var, "vp"));
    at(R);
    FB.assign(FB.local(ItL), FB.use(zeroOf(L.mapIterType())));
    // map_iter_next writes both before it returns true, and only then does
    // the body read them; null keeps an undefined value out of the IR (the
    // loop header's phi) all the same.
    FB.assign(KP, FB.use(E.null(KP.Ty).Op));
    FB.assign(VP, FB.use(E.null(VP.Ty).Op));
    callRt(RtFn::MapIterInit, {copyOf(FB.temp(FB.addrOf(FB.local(ItL)))), MapV},
           R);
    Scopes.back().MapIters.push_back({ItL, R});
  } else {
    IdxL = FB.newLocal(W == Walk::Int ? IntG : U64, Local::Var, "i");
    FB.assign(FB.local(IdxL),
              FB.use(W == Walk::Int ? constInt(IntG, 0) : constInt(U64, 0)));
  }
  LocalId NextL = W == Walk::Str ? FB.newLocal(U64, Local::Var, "next") : NoId;

  BlockId Head = FB.newBlock(), BodyB = FB.newBlock(), Post = FB.newBlock(),
          Exit = FB.newBlock();
  FB.gotoBlock(Head);
  FB.setBlock(Head);
  at(R);
  Operand Cond;
  if (W == Walk::Map) {
    Cond =
        callRt(RtFn::MapIterNext,
               {InfoV, copyOf(FB.temp(FB.addrOf(FB.local(ItL)))),
                copyOf(FB.temp(FB.addrOf(KP))), copyOf(FB.temp(FB.addrOf(VP)))},
               R);
  } else {
    Operand Bound = W == Walk::Array || W == Walk::PtrArray
                        ? constInt(U64, ArrLen)
                        : copyOf(FB.local(LenL));
    Cond = copyOf(FB.temp(FB.binary(BinOp::Lt, copyOf(FB.local(IdxL)), Bound)));
  }
  FB.branch(Cond, BodyB, Exit);
  FB.setBlock(BodyB);

  auto ToType = [&](Operand V, Type *Want) {
    if (V.type() == Want || !V.type()->isInt() || !Want->isInt())
      return V;
    return copyOf(FB.temp(FB.cast(CastKind::IntToInt, std::move(V), Want)));
  };
  // The element at a place as the variable T wants it (a View of it when the
  // variable is one: what is read through a view is a view).
  auto ElemFor = [&](Place P, const ast::Expr *T) -> Operand {
    S::Type *Want = nullptr;
    if (Define) {
      auto D = I.Defs.find(cast<ast::Ident>(unparen(T)));
      Want = D != I.Defs.end() && D->second ? tyOfObj(D->second) : nullptr;
    } else {
      Want = sTyOf(T);
    }
    if (Want && isa<S::RefType>(S::under(Want)) &&
        L.lowerType(Want, R->getBegin()) != P.Ty)
      return viewOfPlace(std::move(P), Want, R);
    return copyOf(P);
  };
  // This iteration's key, and its value (the place of the element, or the
  // value itself: a rune, an integer).
  Operand KeyV;
  Place ValP;
  Operand ValV;
  switch (W) {
  case Walk::Int:
    KeyV = copyOf(FB.local(IdxL));
    break;
  case Walk::Array:
  case Walk::PtrArray:
  case Walk::Seq:
    KeyV = ToType(copyOf(FB.local(IdxL)), I64);
    ValP = FB.index(Seq, IdxL);
    break;
  case Walk::Str: {
    Type *I32 = ctx().i32();
    Var Rn = E.var(I32, E.i32(0), "r");
    Val Idx = E.val(FB.local(IdxL));
    Val B0 = E.snap(E.val(FB.index(Seq, IdxL)));
    E.IfElse(
        B0 < 0x80,
        [&] {
          Rn = E.to(I32, B0);
          FB.assign(FB.local(NextL), FB.use((Idx + 1).Op));
        },
        [&] {
          Operand T = callRt(
              RtFn::DecodeRune,
              {copyOf(FB.field(Seq, 0)), copyOf(FB.field(Seq, 1)), Idx.Op}, R);
          Place TP = placeOf(T);
          Rn = E.val(FB.field(TP, 0));
          FB.assign(FB.local(NextL), FB.use(copyOf(FB.field(TP, 1))));
        });
    KeyV = ToType(copyOf(FB.local(IdxL)), I64);
    ValV = E.snap(Rn).Op;
    break;
  }
  case Walk::Map:
    KeyV = copyOf(E.at(E.snap(E.val(KP)), KG));
    ValP = E.at(E.snap(E.val(VP)), VG);
    break;
  }
  auto ValueOf = [&](const ast::Expr *T) {
    return ValP.Ty ? ElemFor(ValP, T) : ValV;
  };
  auto KeyOf = [&](const ast::Expr *T) {
    return W == Walk::Map ? ElemFor(placeOf(KeyV), T) : KeyV;
  };
  // The value of a variable of the clause for this iteration: an owned one
  // (a shared handle; RNG-1: an owning element is borrowed) gets a copy, and
  // the one of the iteration before is dropped as it is replaced (P04.09).
  auto SetVar = [&](VarSlot &Sl, const ast::Expr *T, Operand V) {
    const S::Var *SV = nullptr;
    if (auto D = I.Defs.find(cast<ast::Ident>(unparen(T))); D != I.Defs.end())
      SV = dyn_cast_or_null<S::Var>(D->second);
    S::Type *ST = SV ? tyOfObj(SV) : nullptr;
    const bool Owns = ST && L.needsDrop(ST) && modeOf(SV) == S::Var::Owned;
    if (Owns)
      V = takeValue(std::move(V), ST);
    FB.assign(Sl.P, FB.use(ToType(std::move(V), Sl.P.Ty)));
    if (Owns && Sl.Cell == NoId)
      if (FuncId G = L.dropGlue(ST, R->getBegin()); G != NoId)
        FB.function().Blocks[FB.block()].Stmts.back().DropOld = G;
  };
  if (Define) {
    at(R);
    // A variable a literal that escapes captures is new in each iteration
    // (the cell of the one before is released: CLO-2).
    if (KeyS.Cell != NoId)
      allocCell(KeyS.Cell, KeyS.P.Ty, R, /*DropOld=*/true);
    if (ValS.Cell != NoId)
      allocCell(ValS.Cell, ValS.P.Ty, R, /*DropOld=*/true);
    if (KeyS.P.Ty)
      SetVar(KeyS, R->Key, KeyOf(R->Key));
    if (Alias || InPlace)
      FB.assign(FB.local(ValLoc), FB.addrOf(ValP, true));
    else if (ValS.P.Ty)
      SetVar(ValS, R->Value, ValueOf(R->Value));
  } else if (WantKey || WantVal) {
    // `for k, v = range x`: each iteration is the assignment `k, v = key,
    // value` (Go spec): the operands of both places first (phase 1), the
    // values as they are now, then the stores left to right, each after its
    // own index checks (phase 2); `for i, a[i] = range` indexes with the i
    // of the previous iteration.
    Dest KD, VD;
    bool DoK = WantKey, DoV = WantVal;
    if (DoK) {
      KD = recordDest(R->Key);
      DoK = !KD.Blank;
    }
    if (DoV) {
      VD = recordDest(R->Value);
      DoV = !VD.Blank;
    }
    at(R);
    // (An owning key or element is cloned: the target gets a value of its
    // own and the collection keeps its, as with `v = xs[i].Clone()`; P04.09.)
    auto Snap = [&](Operand X, S::Type *From) {
      if (From && L.needsDrop(From) && !L.isSharedHandle(From) &&
          X.type() == L.lowerType(From, R->getBegin()))
        return cloneValue(std::move(X), From, R);
      return copyOf(FB.temp(FB.use(std::move(X))));
    };
    Operand K, V;
    if (DoK)
      K = Snap(KeyOf(R->Key), MT ? MT->Key : nullptr);
    if (DoV)
      V = Snap(ValueOf(R->Value), MT ? MT->Elem : SI.Elem);
    if (DoK)
      deriveDest(KD);
    if (DoV)
      deriveDest(VD);
    DoK = DoK && KD.ok();
    DoV = DoV && VD.ok();
    auto Store = [&](Dest &D, Operand X) {
      emitIndexChecks(D.Checks);
      D.Checks.clear();
      resolveDest(D, R);
      if (!D.P.Ty)
        return;
      at(R);
      // (The place's old value is dropped as it is replaced, and a shared
      // handle stored is a copy: P04.09.)
      const Local &Root = FB.function().Locals[D.P.Local];
      const bool Borrowed =
          !Root.Owned &&
          (Root.K == Local::Arg || (Root.K == Local::Var && D.P.Proj.empty()));
      const bool Owns = D.STy && L.needsDrop(D.STy) && !Borrowed;
      if (Owns)
        X = takeValue(std::move(X), D.STy);
      FB.assign(D.P, FB.use(ToType(std::move(X), D.P.Ty)));
      if (Owns)
        if (FuncId G = L.dropGlue(D.STy, R->getBegin()); G != NoId)
          FB.function().Blocks[FB.block()].Stmts.back().DropOld = G;
    };
    if (DoK)
      Store(KD, std::move(K));
    if (DoV)
      Store(VD, std::move(V));
  }

  BlockId Next = W == Walk::Map ? Head : Post;
  Targets.push_back({Exit, Next, Scopes.size(), std::move(Label)});
  lowerBlock(R->Body);
  Targets.pop_back();
  if (!FB.terminated())
    FB.gotoBlock(Next);
  if (W != Walk::Map) {
    FB.setBlock(Post);
    at(R);
    if (W == Walk::Str)
      FB.assign(FB.local(IdxL), FB.use(copyOf(FB.local(NextL))));
    else
      FB.assign(FB.local(IdxL),
                FB.binary(BinOp::Add, copyOf(FB.local(IdxL)),
                          constInt(FB.function().Locals[IdxL].Ty, 1)));
    FB.gotoBlock(Head);
  } else {
    // (Post is never entered: continue goes to the head.)
    FB.setBlock(Post);
    FB.unreachable();
  }
  FB.setBlock(Exit);
  if (Alias || InPlace)
    Aliases.erase(ValVar);
  if (PushedRange)
    MapRanges.pop_back();
  popScope(); // ends the map iteration on the way out of the loop
}
