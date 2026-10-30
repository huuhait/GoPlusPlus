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

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

//===----------------------------------------------------------------------===//
// Variables and destinations
//===----------------------------------------------------------------------===//

LocalId Body::declareVar(const ast::Ident *Id, Type *Ty) {
  auto D = I.Defs.find(Id);
  LocalId Loc = FB.newLocal(Ty, Local::Var, Id->Name.str());
  if (D != I.Defs.end())
    if (auto *V = dyn_cast_or_null<S::Var>(D->second))
      Vars[V] = Loc;
  Scopes.back().Locals.push_back(Loc);
  FB.storageLive(Loc);
  return Loc;
}

Body::Dest Body::lowerDest(const ast::Expr *E) {
  E = unparen(E);
  Dest D;
  if (auto *Id = dyn_cast<ast::Ident>(E)) {
    if (Id->Name.str() == "_") {
      D.Blank = true;
      return D;
    }
    D.STy = sTyOf(E);
    D.P = identPlace(Id);
    if (S::Type *N = I.narrowedType(Id)) {
      (void)N;
      // The variable itself is assigned: it is the Option, not its payload.
      auto U = I.Uses.find(Id);
      if (U != I.Uses.end())
        D.STy = U->second->Ty;
    }
    return D;
  }
  D.STy = sTyOf(E);
  D.P = lowerPlace(E, &D.Checks);
  return D;
}

void Body::store(const Dest &D, Operand V, const ast::Node *N) {
  if (D.Blank || !D.P.Ty)
    return;
  // A string stored in a field or an element would drop the old one (P04.09).
  if (D.P.Ty->getKind() == Type::Kind::String && !D.P.Proj.empty() &&
      V.K != Operand::Const) {
    unsupported(N, "storing a string that is not a constant in a struct or "
                   "array (P04.09)");
    return;
  }
  at(N);
  FB.assign(D.P, FB.use(std::move(V)));
}

bool Body::multiValues(const ast::Expr *E, size_t Want,
                       SmallVector<Operand, 2> &Out) {
  Operand T = lowerExpr(unparen(E));
  Type *Ty = T.type();
  if (Ty->getKind() != Type::Kind::Tuple || Ty->numFields() != Want) {
    unsupported(E, "this multi-value expression");
    return false;
  }
  Place P = placeOf(std::move(T));
  at(E);
  for (size_t K = 0; K < Want; ++K) {
    Operand F = copyOf(FB.field(P, K));
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
  // 1. The places of the left sides (index operands and pointers are
  //    evaluated now, before the right sides: Go spec, "Assignment
  //    statements"), and which names are new.
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
    T.D = lowerDest(Lhs);
    T.STy = T.D.STy;
    if (!T.D.Blank && !T.D.P.Ty)
      return;
    Targets_.push_back(std::move(T));
  }
  // 2. The right sides, all before any store.
  SmallVector<Operand, 2> Vals;
  if (A->Rhs.size() == N) {
    for (size_t K = 0; K < N; ++K) {
      S::Type *Want = Targets_[K].STy ? Targets_[K].STy : sTyOf(A->Rhs[K]);
      Operand V = valueFor(A->Rhs[K], Want);
      if (N > 1 && !isStable(V, FB)) {
        at(A);
        V = copyOf(FB.temp(FB.use(V)));
      }
      Vals.push_back(std::move(V));
    }
  } else if (A->Rhs.size() == 1) {
    if (!multiValues(A->Rhs[0], N, Vals))
      return;
  } else {
    unsupported(A, "this assignment");
    return;
  }
  // 3. Declare the new names and store.
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
    // Phase 2 of the assignment: the index checks of this target, then its
    // store, left to right (the spec: `x[1], x[3] = 4, 5` sets x[1], then
    // panics for x[3]).
    emitIndexChecks(T.D.Checks);
    store(T.D, Vals[K], A);
  }
}

void Body::lowerOpAssign(const ast::AssignStmt *A) {
  BinOp Op;
  if (!compoundOp(A->Op, Op) || A->Lhs.size() != 1 || A->Rhs.size() != 1 ||
      I.Operators.count(A)) {
    unsupported(A, "this assignment");
    return;
  }
  Dest D = lowerDest(A->Lhs[0]);
  if (D.Blank || !D.P.Ty)
    return;
  S::Type *T = D.STy;
  Type *G = L.lowerType(T, A->getBegin());
  if (!G)
    return;
  at(A);
  // The right side first, then the checks of the place, then the read of the
  // place (as gc: the index is checked in phase 2, after the right side, and
  // the place is read after a call on the right has run).
  Operand Y = isShift(Op) ? lowerExpr(A->Rhs[0]) : lowerAs(A->Rhs[0], G);
  emitIndexChecks(D.Checks);
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
  Dest D = lowerDest(S->X);
  if (D.Blank || !D.P.Ty)
    return;
  emitIndexChecks(D.Checks);
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
    if (!multiValues(VS->Values[0], N, Vals))
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

void Body::lowerBranch(const ast::BranchStmt *B) {
  at(B);
  switch (B->Keyword) {
  case tok::kw_goto:
    FB.gotoBlock(labelBlock(B->Label->Name.str()));
    startDeadBlock();
    return;
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
      if (!multiValues(R->Results[0], Want, Vals))
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
  pushScope();
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
    unsupported(St, "a range loop (P04.05)");
    return;
  case K::TypeSwitchStmt:
    unsupported(St, "a type switch (P04.06)");
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
  pushScope();
  if (Obj) {
    ArrayRef<S::Var *> Params = Obj->Params;
    if (Params.size() != F.numParams()) {
      unsupported(FD, "this parameter list");
      return;
    }
    for (size_t K = 0; K < Params.size(); ++K) {
      Vars[Params[K]] = FB.param(K);
      F.Locals[FB.param(K)].Name = Params[K]->Name.str();
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
        Vars[R] = Loc;
        NamedResults.push_back(Loc);
        Scopes.back().Locals.push_back(Loc);
        FB.storageLive(Loc);
        FB.assign(FB.local(Loc), FB.use(zeroOf(T)));
      }
  }
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
  pushScope();
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
    if (!multiValues(E.Init, E.Lhs.size(), Vals))
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
