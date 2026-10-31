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

LocalId Body::declareVar(const ast::Ident *Id, Type *Ty) {
  auto D = I.Defs.find(Id);
  LocalId Loc = FB.newLocal(Ty, Local::Var, Id->Name.str());
  FB.function().Locals[Loc].Source = true;
  if (D != I.Defs.end())
    if (auto *V = dyn_cast_or_null<S::Var>(D->second))
      Vars[V] = Loc;
  Scopes.back().Locals.push_back(Loc);
  FB.storageLive(Loc);
  return Loc;
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
      auto It = I.Selections.find(Sel);
      if (It == I.Selections.end() || It->second.K != S::Selection::FieldVal) {
        Root = X;
        X = nullptr;
      } else {
        X = Sel->X;
      }
      break;
    }
    case ast::Node::Kind::IndexExpr: {
      auto *IE = cast<ast::IndexExpr>(X);
      if (IE->Indices.size() != 1 || I.Operators.count(IE)) {
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
      if (U->Op == tok::and_ && !I.Operators.count(U) &&
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
    if (I.narrowedType(Id)) {
      // The variable itself is assigned: it is the Option, not its payload.
      auto U = I.Uses.find(Id);
      if (U != I.Uses.end())
        D.STy = U->second->Ty;
    }
    return D;
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

void Body::store(Dest &D, Operand V, const ast::Node *N) {
  ++LowerSteps;
  if (D.Blank)
    return;
  deriveDest(D);
  // Phase 2 of the assignment: the index checks of this target just before
  // its store (the spec: `x[1], x[3] = 4, 5` sets x[1], then panics for x[3]),
  // then its map entries.
  emitIndexChecks(D.Checks);
  D.Checks.clear();
  resolveDest(D, N);
  if (!D.P.Ty)
    return;
  // The value it replaces is not dropped yet: it leaks until drop
  // elaboration (P04.09).
  at(N);
  FB.assign(D.P, FB.use(std::move(V)));
}

bool Body::multiValues(const ast::Expr *E, size_t Want,
                       SmallVector<Operand, 2> &Out,
                       ArrayRef<S::Type *> Targets) {
  // v, ok := m[k]
  auto *MapIE = dyn_cast<ast::IndexExpr>(unparen(E));
  auto *AssertE = dyn_cast<ast::TypeAssertExpr>(unparen(E));
  S::Type *Want0 = Targets.empty() ? nullptr : Targets[0];
  const bool CommaOk =
      Want == 2 && ((MapIE && isMapIndex(MapIE)) || AssertE != nullptr);
  Operand T = MapIE && Want == 2 && isMapIndex(MapIE) ? mapRead(MapIE, true)
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
      From = K == 0 ? sTyOf(E) : L.CP.Ctx->typ(S::BasicKind::Bool);
    // (A view a comma-ok assertion binds is lowerTypeAssert's: no
    // conversion goes to a view.)
    if (K < Targets.size())
      F = convertResult(std::move(F), From, Targets[K], E);
    Out.push_back(std::move(F));
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Assignment
//===----------------------------------------------------------------------===//

static bool compoundOp(tok::Kind T, BinOp &Op) {
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
        T.STy = Def->second->Ty;
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
  if (A->Rhs.size() == N) {
    for (size_t K = 0; K < N; ++K) {
      S::Type *Want = Targets_[K].STy ? Targets_[K].STy : sTyOf(A->Rhs[K]);
      if (auto *SE = dyn_cast<ast::SliceExpr>(unparen(A->Rhs[K]));
          SE && I.Reslices.count(SE)) {
        Shrinks.push_back({K, LateOps()});
        recordOps(SE, Shrinks.back().second);
        Vals.push_back(Operand());
        continue;
      }
      Operand V = valueFor(A->Rhs[K], Want);
      if (N > 1 && !isStable(V, FB)) {
        at(A);
        V = copyOf(FB.temp(FB.use(V)));
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
      LocalId Loc = declareVar(T.New, Ty);
      FB.assign(FB.local(Loc), FB.use(Vals[K]));
      continue;
    }
    store(T.D, Vals[K], A);
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
    Val Dst{&E, Mv.Dst}, Src{&E, Mv.Src};
    E.If(Dst != Src, [&] {
      callFn(L.libc("memmove", A->getBegin()), {Mv.Dst, Mv.Src, Mv.Bytes});
    });
  }
}

void Body::lowerOpAssign(const ast::AssignStmt *A) {
  BinOp Op;
  if (!compoundOp(A->Op, Op) || A->Lhs.size() != 1 || A->Rhs.size() != 1 ||
      I.Operators.count(A)) {
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
    STys.push_back(D != I.Defs.end() && D->second ? D->second->Ty : nullptr);
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
      Operand V = valueFor(VS->Values[K], Want);
      if (N > 1 && !isStable(V, FB)) {
        at(VS);
        V = copyOf(FB.temp(FB.use(V)));
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
    LocalId Loc = declareVar(VS->Names[K], T);
    FB.assign(FB.local(Loc), FB.use(Vals[K]));
  }
}

void Body::lowerTypeDecl(const ast::GenDecl *GD) {
  for (const ast::Spec *Sp : GD->Specs) {
    auto *TS = dyn_cast<ast::TypeSpec>(Sp);
    if (!TS || TS->AssignLoc.isValid())
      continue;
    if (TS->TypeParams && TS->TypeParams->numFields()) {
      unsupported(TS, "a generic type (P04.08)");
      continue;
    }
    auto D = I.Defs.find(TS->Name);
    auto *TN =
        D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::TypeName>(D->second);
    // Each declaration gets the next ordinal of this function, so two types
    // with one name (in two blocks) get two symbols.
    if (TN && ScopeNode)
      L.declareLocalType(TN, ScopeNode, LocalTypes);
    ++LocalTypes;
  }
}

//===----------------------------------------------------------------------===//
// Control flow
//===----------------------------------------------------------------------===//

void Body::lowerIf(const ast::IfStmt *If) {
  pushScope();
  if (If->Init)
    lowerStmt(If->Init);
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
  if (For->Init)
    lowerStmt(For->Init);
  BlockId Head = FB.newBlock(), BodyB = FB.newBlock(),
          Post = For->Post ? FB.newBlock() : NoId, Exit = FB.newBlock();
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
  BlockId Next = For->Post ? Post : Head;
  Targets.push_back({Exit, Next, Scopes.size(), std::move(Label)});
  lowerBlock(For->Body);
  Targets.pop_back();
  if (!FB.terminated())
    FB.gotoBlock(Next);
  if (For->Post) {
    FB.setBlock(Post);
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
        if (!isConst(E))
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
      if (Tag != NoId) {
        Operand V = valueFor(E, TagS);
        Cond = equality(true, copyOf(FB.local(Tag)), std::move(V), TagS, E);
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
    lowerStmt(Sw->Init);
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
    pushScope();
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

void Body::lowerBranch(const ast::BranchStmt *B) {
  at(B);
  switch (B->Keyword) {
  case tok::kw_goto: {
    // A goto out of a range over a map ends that iteration (RNG-4). A label
    // is in the same block as the goto or in one around it (Go spec), so the
    // goto leaves exactly the ranges whose statement does not hold the label.
    bool InMapRange = false;
    for (const Scope &Sc : Scopes) {
      ++LowerSteps;
      InMapRange |= !Sc.MapIters.empty();
    }
    SourceLoc To = InMapRange ? labelPos(B->Label->Name.str()) : SourceLoc();
    if (To.isValid())
      for (size_t S = Scopes.size(); S > 0; --S)
        for (size_t K = Scopes[S - 1].MapIters.size(); K > 0; --K) {
          ++LowerSteps;
          const MapIteration &MI = Scopes[S - 1].MapIters[K - 1];
          if (To < MI.R->getBegin() || MI.R->getEnd() <= To)
            endMapIteration(MI.It);
        }
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
  size_t Want =
      NamedResults.empty() ? (Ret->isUnit() ? 0 : 1) : NamedResults.size();
  if (Ret->getKind() == Type::Kind::Tuple && NamedResults.empty())
    Want = Ret->numFields();
  if (R->Results.empty()) {
    // A bare return gives the named results.
    for (LocalId N : NamedResults)
      Vals.push_back(copyOf(FB.local(N)));
  } else {
    ArrayRef<S::Var *> Rs = Obj ? Obj->Results : ArrayRef<S::Var *>();
    if (R->Results.size() == Want) {
      for (size_t K = 0; K < Want; ++K) {
        S::Type *Wanted = K < Rs.size() ? Rs[K]->Ty : sTyOf(R->Results[K]);
        Vals.push_back(valueFor(R->Results[K], Wanted));
      }
    } else if (R->Results.size() == 1 && Want > 1) {
      // return f() with several results.
      SmallVector<S::Type *, 2> To;
      for (S::Var *V : Rs)
        To.push_back(V->Ty);
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
  pushScope(B != RootBody);
  for (const ast::Stmt *St : B->List)
    lowerStmt(St);
  popScope();
}

void Body::lowerStmt(const ast::Stmt *St) {
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
      lowerCall(C);
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
    unsupported(St, "a defer statement (P04.07)");
    return;
  case K::GoStmt:
  case K::SelectStmt:
  case K::SendStmt:
    unsupported(St, "concurrency statements (P12)");
    return;
  default:
    unsupported(St, std::string("the statement ") + St->getKindName());
    return;
  }
}

void Body::finish() {
  Function &F = FB.function();
  for (BasicBlock &BB : F.Blocks)
    if (!BB.Term.Set) {
      BB.Term.K = Terminator::Unreachable;
      BB.Term.Set = true;
    }
  removeDeadBlocks(F);
}

void Body::lower(const ast::FuncDecl *FD) {
  Function &F = FB.function();
  at(FD);
  pushScope(false);
  if (Obj) {
    ArrayRef<S::Var *> Params = Obj->Params;
    // The receiver of a method is parameter 0.
    const size_t Lead = Obj->Recv ? 1 : 0;
    if (Params.size() + Lead != F.numParams()) {
      unsupported(FD, "this parameter list");
      return;
    }
    // A borrowed []T arrives as (ptr, len) (ADR-0029 point 7): a parameter
    // without `move`, and a value receiver of a slice type (PAR-1). The body
    // works on a []T of its own, with a null allocator word that nothing
    // reads (a borrowed slice is never grown, cloned or freed) and the
    // length as capacity (SLC-12).
    auto OwnSlice = [&](unsigned K, const S::Var *V) {
      Rt E(M, FB);
      Type *ST = ctx().slice(F.Locals[FB.param(K)].Ty->elems()[0]->elem());
      LocalId Loc = FB.newLocal(ST, Local::Var, V->Name.str());
      // The debugger's parameter is this slice, not the pair (P04.12).
      F.Locals[Loc].Source = true;
      F.Locals[Loc].ArgNo = K + 1;
      FB.storageLive(Loc);
      Val P = E.param(K);
      Val Ptr = E.fld(P, 0), Len = E.fld(P, 1);
      FB.assign(FB.local(Loc),
                FB.aggregate(AggKind::Slice, ST,
                             {Ptr.Op, Len.Op, Len.Op,
                              E.null(ctx().ptr(ctx().u8())).Op}));
      Scopes.back().Locals.push_back(Loc);
      Vars[V] = Loc;
    };
    if (Obj->Recv) {
      F.Locals[FB.param(0)].Name = Obj->Recv->Name.str();
      if (F.Locals[FB.param(0)].Ty->getKind() == Type::Kind::Tuple &&
          L.recvBorrowedSlice(Obj)) {
        OwnSlice(0, Obj->Recv);
      } else {
        Vars[Obj->Recv] = FB.param(0);
        F.Locals[FB.param(0)].Source = true;
      }
    }
    for (size_t K = 0; K < Params.size(); ++K) {
      // A borrowed map is a pointer to the caller's header (PAR-10).
      F.Locals[FB.param(K + Lead)].Name = Params[K]->Name.str();
      if (L.mapByPointer(Obj->sig(), K)) {
        Aliases[Params[K]] = FB.param(K + Lead);
        F.Locals[FB.param(K + Lead)].Source = true;
      } else if (F.Locals[FB.param(K + Lead)].Ty->getKind() ==
                     Type::Kind::Tuple &&
                 L.sliceBorrowed(Obj->sig(), K)) {
        OwnSlice(K + Lead, Params[K]);
      } else {
        Vars[Params[K]] = FB.param(K + Lead);
        F.Locals[FB.param(K + Lead)].Source = true;
      }
    }
    // Named results are variables, set to their zero values.
    bool Named = false;
    for (const S::Var *R : Obj->Results)
      if (!R->Name.empty())
        Named = true;
    if (Named)
      for (const S::Var *R : Obj->Results) {
        Type *T = L.lowerType(R->Ty, FD->getBegin());
        if (!T)
          return;
        LocalId Loc = FB.newLocal(T, Local::Var, R->Name.str());
        F.Locals[Loc].Source = true;
        Vars[R] = Loc;
        NamedResults.push_back(Loc);
        Scopes.back().Locals.push_back(Loc);
        FB.storageLive(Loc);
        FB.assign(FB.local(Loc), FB.use(zeroOf(T)));
      }
  }
  RootBody = FD->Body;
  lowerBlock(FD->Body);
  if (!FB.terminated()) {
    FB.setSpan(L.spanOf(FD->Body->Rbrace));
    if (F.Sig->ret()->isUnit()) {
      exitScopes(0);
      FB.ret();
    } else {
      FB.unreachable();
    }
  }
  Scopes.pop_back();
  finish();
}

void Body::lowerInit(ArrayRef<S::InitEntry> Order, ArrayRef<FuncId> InitFuncs) {
  Function &F = FB.function();
  FB.setSpan(F.Loc);
  pushScope(false);
  for (const S::InitEntry &E : Order) {
    at(E.Init);
    if (E.Lhs.size() == 1) {
      S::Var *V = E.Lhs[0];
      Operand Val = valueFor(E.Init, V->Ty);
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
      To.push_back(V->Ty);
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
    unsupported(R, "a range over this type (P04.07, P12)");
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
      WantVal && !Alias && !(ValVar && ValVar->Mode == S::Var::Borrowed);

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
  LocalId KeyLoc = NoId, ValLoc = NoId;
  auto DeclareDef = [&](const ast::Expr *T, LocalId &Loc) {
    if (IsBlank(T))
      return true;
    auto *Id = cast<ast::Ident>(unparen(T));
    auto D = I.Defs.find(Id);
    if (D == I.Defs.end() || !D->second) {
      unsupported(T, "this range variable");
      return false;
    }
    Type *Ty = L.lowerType(D->second->Ty, Id->getBegin());
    if (!Ty)
      return false;
    Loc = declareVar(Id, Ty);
    return true;
  };
  if (Define) {
    if (!DeclareDef(R->Key, KeyLoc))
      return popScope();
    if (Alias) {
      if (!ValVar || !VG) {
        unsupported(R->Value, "this `&` range variable");
        return popScope();
      }
      ValLoc = FB.newLocal(ctx().ptr(VG), Local::Var,
                           cast<ast::Ident>(unparen(R->Value))->Name.str());
      FB.function().Locals[ValLoc].Source = true;
      Scopes.back().Locals.push_back(ValLoc);
      FB.storageLive(ValLoc);
      Aliases[ValVar] = ValLoc;
    } else if (!DeclareDef(R->Value, ValLoc)) {
      return popScope();
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
      Want = D != I.Defs.end() && D->second ? D->second->Ty : nullptr;
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
  if (Define) {
    at(R);
    if (KeyLoc != NoId)
      FB.assign(FB.local(KeyLoc),
                FB.use(ToType(KeyOf(R->Key), FB.function().Locals[KeyLoc].Ty)));
    if (Alias)
      FB.assign(FB.local(ValLoc), FB.addrOf(ValP, true));
    else if (ValLoc != NoId)
      FB.assign(
          FB.local(ValLoc),
          FB.use(ToType(ValueOf(R->Value), FB.function().Locals[ValLoc].Ty)));
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
    Operand K, V;
    if (DoK)
      K = copyOf(FB.temp(FB.use(KeyOf(R->Key))));
    if (DoV)
      V = copyOf(FB.temp(FB.use(ValueOf(R->Value))));
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
      FB.assign(D.P, FB.use(ToType(std::move(X), D.P.Ty)));
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
  if (Alias)
    Aliases.erase(ValVar);
  popScope(); // ends the map iteration on the way out of the loop
}
