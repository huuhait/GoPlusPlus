//===- Stmt.cpp - Statements ----------------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's stmt.go, labels.go and return.go (BSD-style license,
// Copyright The Go Authors), with the Go++ range variable rules (RNG-*).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

static const ast::Expr *unparen(const ast::Expr *E) {
  while (auto *P = dyn_cast<ParenExpr>(E))
    E = P->X;
  return E;
}

namespace {
struct DelayedGuard {
  Checker &C;
  size_t Top;
  explicit DelayedGuard(Checker &C) : C(C), Top(C.Delayed.size()) {}
  ~DelayedGuard() { C.processDelayed(Top); }
};
} // namespace

//===----------------------------------------------------------------------===//
// Function bodies
//===----------------------------------------------------------------------===//

void Checker::funcBody(DeclInfo *D, StringRef Name, SignatureType *Sig,
                       const BlockStmt *Body, const ConstValue *Iota, Scope *Sc,
                       ArrayRef<Var *> Params, ArrayRef<Var *> Results,
                       Var *Recv, const ast::FuncType *FT) {
  (void)Name;
  (void)Params;
  (void)Recv;
  (void)FT;
  Env Saved = E;
  // A function literal in a header is an ordinary body, not the header.
  bool SavedHeader = InHeaderInit;
  InHeaderInit = false;
  auto SavedLocals = std::move(LocalVars);
  LocalVars.clear();
  E = Env();
  E.Cur = D;
  E.Sc = Sc;
  E.Iota = Iota;
  E.Sig = Sig;
  E.Results = Results;
  E.FuncId = Body;
  E.Pos = SourceLoc();
  size_t Top = Delayed.size();
  // Parameters belong to this function.
  for (Var *V : Params)
    V->FuncOwner = Body;
  for (Var *V : Results)
    V->FuncOwner = Body;
  if (Recv)
    Recv->FuncOwner = Body;
  stmtList(0, Body->List);
  if (E.HasLabel)
    labels(Body);
  if (!Results.empty() && !isTerminating(Body, ""))
    err(Body->Rbrace, diag::err_missing_return);
  processDelayed(Top);
  // Unused variables.
  std::stable_sort(LocalVars.begin(), LocalVars.end(),
                   [](Var *A, Var *B) { return A->Pos < B->Pos; });
  if (S.Opts.ReportUnused)
    for (Var *V : LocalVars)
      if (!V->Used && V->Name != "_" && !V->Name.empty())
        err(V->Pos, diag::err_declared_not_used, {std::string(V->Name)});
  LocalVars = std::move(SavedLocals);
  InHeaderInit = SavedHeader;
  E = Saved;
}

void Checker::checkBody(const BodyJob &J) {
  E = Env();
  E.Sc = J.Sc;
  E.Decl = J.F;
  funcBody(J.D, J.F->Name, J.Sig, J.Decl->Body, nullptr, J.Sc, J.Params,
           J.Results, J.Recv, J.Decl->Type);
  processDelayed();
}

//===----------------------------------------------------------------------===//
// Statement lists and blocks
//===----------------------------------------------------------------------===//

void Checker::stmtList(unsigned Ctx, ArrayRef<Stmt *> L) {
  unsigned Inner = Ctx & ~(FallthroughOk);
  bool Ok = Ctx & FallthroughOk;
  // Trailing empty statements are "invisible" to fallthrough analysis.
  size_t N = L.size();
  while (N > 0 && isa<EmptyStmt>(L[N - 1]))
    --N;
  for (size_t K = 0; K < L.size(); ++K) {
    unsigned C = Inner;
    if (Ok && K + 1 == N)
      C |= FallthroughOk;
    stmt(C, L[K]);
  }
}

void Checker::openBlock(const BlockStmt *B, const char *Comment) {
  openScope(B->getRange(), Comment);
}

void Checker::blockStmt(unsigned Ctx, const BlockStmt *B) {
  openBlock(B, "block");
  stmtList(Ctx, B->List);
  closeScope();
}

void Checker::simpleStmt(const Stmt *S0) {
  if (S0)
    stmt(0, S0);
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

static tok::Kind assignOpToBinary(tok::Kind K) {
  switch (K) {
  case tok::add_assign:
    return tok::add;
  case tok::sub_assign:
    return tok::sub;
  case tok::mul_assign:
    return tok::mul;
  case tok::quo_assign:
    return tok::quo;
  case tok::rem_assign:
    return tok::rem;
  case tok::and_assign:
    return tok::and_;
  case tok::or_assign:
    return tok::or_;
  case tok::xor_assign:
    return tok::xor_;
  case tok::shl_assign:
    return tok::shl;
  case tok::shr_assign:
    return tok::shr;
  case tok::and_not_assign:
    return tok::and_not;
  default:
    return tok::eof;
  }
}

void Checker::suspendedCall(const char *What, const ast::Expr *Call) {
  auto *C = dyn_cast<CallExpr>(unparen(Call));
  if (!C) {
    use(Call);
    err(Call, diag::err_go_defer_needs_call, {What});
    return;
  }
  Operand X;
  ExprKind K = rawExpr(X, Call, nullptr, false);
  const char *Msg = nullptr;
  switch (K) {
  case ExprKind::Conversion:
    Msg = "requires function call, not conversion";
    break;
  case ExprKind::Expression:
    Msg = "discards result of";
    break;
  case ExprKind::Statement:
    return;
  }
  err(X, diag::custom, {std::string(What) + " " + Msg + " " + str(X)});
}

void Checker::stmt(unsigned Ctx, const Stmt *S0) {
  DelayedGuard G(*this);
  unsigned Inner = Ctx & ~(FallthroughOk | FinalSwitchCase | InTypeSwitch);
  using K = ast::Node::Kind;
  switch (S0->getKind()) {
  case K::BadStmt:
  case K::EmptyStmt:
    return;
  case K::DeclStmt:
    declStmt(cast<DeclStmt>(S0));
    return;
  case K::LabeledStmt: {
    E.HasLabel = true;
    stmt(Ctx, cast<LabeledStmt>(S0)->Body);
    return;
  }
  case K::ExprStmt: {
    auto *ES = cast<ExprStmt>(S0);
    Operand X;
    ExprKind Kind = rawExpr(X, ES->X, nullptr, false);
    if (X.M == Mode::Invalid)
      return;
    const char *Msg = nullptr;
    switch (X.M) {
    case Mode::Builtin:
      Msg = "must be called";
      break;
    case Mode::TypeExpr:
      Msg = "is not an expression";
      break;
    default:
      if (Kind == ExprKind::Statement)
        return;
      Msg = "is not used";
    }
    err(X, diag::custom, {str(X) + " " + Msg});
    return;
  }
  case K::SendStmt: {
    auto *SS = cast<SendStmt>(S0);
    Operand Ch, Val;
    expr(Ch, SS->Chan);
    expr(Val, SS->Value);
    if (Ch.M == Mode::Invalid || Val.M == Mode::Invalid)
      return;
    Type *Elem = chanElem(S0->getBegin(), Ch, false);
    if (!Elem)
      return;
    assignment(Val, Elem, "send");
    if (own() && Val.M != Mode::Invalid) {
      if (isa<RefType>(Elem))
        checkRefStore(Val.E, Elem, Val.Ty);
      else
        needOwnership(Val, "send", Elem);
    }
    return;
  }
  case K::IncDecStmt: {
    auto *ID = cast<IncDecStmt>(S0);
    Operand X;
    expr(X, ID->X);
    if (X.M == Mode::Invalid)
      return;
    if (!allNumeric(X.Ty)) {
      const char *Op = ID->Op == tok::inc ? "++" : "--";
      err(ID->X, diag::err_inc_dec_non_numeric, {str(ID->X) + Op, str(X.Ty)});
      return;
    }
    assignVar(ID->X, nullptr, &X, "assignment");
    return;
  }
  case K::AssignStmt: {
    auto *AS = cast<AssignStmt>(S0);
    if (AS->Op == tok::assign) {
      assignVars(AS->Lhs, AS->Rhs);
      return;
    }
    if (AS->Op == tok::define) {
      shortVarDecl(AS->OpLoc, AS->Lhs, AS->Rhs);
      return;
    }
    if (AS->Lhs.size() != 1 || AS->Rhs.size() != 1) {
      err(AS->OpLoc, diag::custom,
          {"assignment operation requires single-valued expressions"});
      return;
    }
    Operand X;
    binary(X, nullptr, AS->Lhs[0], AS->Rhs[0], assignOpToBinary(AS->Op),
           AS->OpLoc);
    assignVar(AS->Lhs[0], nullptr, &X, "assignment operation");
    return;
  }
  case K::GoStmt:
    suspendedCall("go", cast<GoStmt>(S0)->Call);
    return;
  case K::DeferStmt:
    suspendedCall("defer", cast<DeferStmt>(S0)->Call);
    return;
  case K::ReturnStmt:
    returnStmt(cast<ReturnStmt>(S0));
    return;
  case K::BranchStmt: {
    auto *B = cast<BranchStmt>(S0);
    if (B->Label) {
      E.HasLabel = true;
      return; // checked by labels()
    }
    switch (B->Keyword) {
    case tok::kw_break:
      if (!(Ctx & BreakOk))
        err(S0, diag::err_not_in_loop_break);
      break;
    case tok::kw_continue:
      if (!(Ctx & ContinueOk))
        err(S0, diag::err_not_in_loop_continue);
      break;
    case tok::kw_fallthrough:
      if (!(Ctx & FallthroughOk)) {
        if (Ctx & FinalSwitchCase)
          err(S0, diag::err_fallthrough_final);
        else if (Ctx & InTypeSwitch)
          err(S0, diag::err_fallthrough_type_switch);
        else
          err(S0, diag::err_fallthrough_misplaced);
      }
      break;
    default:
      err(S0, diag::custom, {"invalid goto"});
    }
    return;
  }
  case K::BlockStmt:
    blockStmt(Inner, cast<BlockStmt>(S0));
    return;
  case K::IfStmt:
    ifStmt(Inner, cast<IfStmt>(S0));
    return;
  case K::SwitchStmt:
    switchStmt(Inner | BreakOk, cast<SwitchStmt>(S0));
    return;
  case K::TypeSwitchStmt:
    typeSwitchStmt(Inner | BreakOk | InTypeSwitch, cast<TypeSwitchStmt>(S0));
    return;
  case K::SelectStmt:
    selectStmt(Inner | BreakOk, cast<SelectStmt>(S0));
    return;
  case K::ForStmt:
    forStmt(Inner | BreakOk | ContinueOk, cast<ForStmt>(S0));
    return;
  case K::RangeStmt:
    rangeStmt(Inner | BreakOk | ContinueOk, cast<RangeStmt>(S0));
    return;
  default:
    err(S0, diag::custom, {"invalid statement"});
  }
}

/// The init statement of an if or switch (not a three-clause for, STM-4): its
/// temporaries live to the end of the whole statement (OWN-R2, STM-4), so a
/// variable it declares may hold a ref into one (OWN-V6).
void Checker::headerInit(const ast::Stmt *Init) {
  bool Saved = InHeaderInit;
  InHeaderInit = true;
  simpleStmt(Init);
  InHeaderInit = Saved;
}

void Checker::ifStmt(unsigned Ctx, const IfStmt *S0) {
  openScope(S0->getRange(), "if");
  headerInit(S0->Init);
  Operand X;
  expr(X, S0->Cond);
  if (X.M != Mode::Invalid && !allBoolean(X.Ty))
    err(S0->Cond, diag::custom, {"non-boolean condition in if statement"});
  blockStmt(Ctx, S0->Then);
  if (S0->Else)
    stmt(Ctx, S0->Else);
  closeScope();
}

void Checker::forStmt(unsigned Ctx, const ForStmt *S0) {
  openScope(S0->getRange(), "for");
  simpleStmt(S0->Init);
  if (S0->Cond) {
    Operand X;
    expr(X, S0->Cond);
    if (X.M != Mode::Invalid && !allBoolean(X.Ty))
      err(S0->Cond, diag::custom, {"non-boolean condition in for statement"});
  }
  if (S0->Post) {
    if (auto *AS = dyn_cast<AssignStmt>(S0->Post); AS && AS->Op == tok::define)
      err(S0->Post, diag::custom,
          {"cannot declare in post statement of for loop"});
    simpleStmt(S0->Post);
  }
  // The body shares the loop scope for scoping purposes of RNG-8 only.
  blockStmt(Ctx, S0->Body);
  closeScope();
}

void Checker::returnStmt(const ReturnStmt *S0) {
  ArrayRef<Var *> Res = E.Results;
  if (S0->Results.empty() && !Res.empty() && !Res[0]->Name.empty()) {
    for (Var *R : Res) {
      if (R->Name == "_")
        continue;
      Object *Alt = lookup(R->Name);
      if (Alt && Alt != R)
        err(S0, diag::err_shadowed_result, {std::string(R->Name)})
            .note(Alt->Pos, "inner declaration of " + std::string(R->Name));
    }
    return;
  }
  if (!S0->Results.empty()) {
    if (Res.empty()) {
      unsigned B = NumErrors;
      useExprs(S0->Results);
      if (B == NumErrors) {
        SmallVector<Type *, 4> Have;
        for (const ast::Expr *Ex : S0->Results)
          Have.push_back(I.typeOf(Ex));
        err(S0->Results[0], diag::err_too_many_arguments_return,
            {typesSummary(Have, false, false), "()"});
      }
      return;
    }
    initVars(Res, S0->Results, S0);
    if (own() && Res.size() == S0->Results.size())
      for (size_t K = 0; K < S0->Results.size(); ++K)
        checkReturnRef(S0->Results[K], Res[K]->Ty);
    return;
  }
  if (!Res.empty()) {
    SmallVector<Type *, 4> Want;
    for (Var *V : Res)
      Want.push_back(V->Ty);
    err(S0, diag::err_not_enough_arguments_return,
        {"()", typesSummary(Want, false, false)});
  }
}

//===----------------------------------------------------------------------===//
// switch
//===----------------------------------------------------------------------===//

void Checker::caseValues(Operand &X, ArrayRef<ast::Expr *> Values,
                         SmallVector<std::pair<ConstValue, Type *>, 8> &Seen,
                         SmallVector<SourceLoc, 8> &SeenPos) {
  for (const ast::Expr *Ex : Values) {
    Operand V;
    expr(V, Ex);
    if (X.M == Mode::Invalid || V.M == Mode::Invalid)
      continue;
    convertUntyped(V, X.Ty);
    if (V.M == Mode::Invalid)
      continue;
    Operand Res = V;
    Operand XX = X;
    bool Saved = SwitchCaseCmp;
    SwitchCaseCmp = true;
    comparison(Res, XX, tok::eql);
    SwitchCaseCmp = Saved;
    if (Res.M == Mode::Invalid)
      continue;
    if (V.M != Mode::Constant)
      continue;
    bool Dup = false;
    for (size_t K = 0; K < Seen.size(); ++K) {
      const auto &[Val, T] = Seen[K];
      if (Val.kind() == V.Val.kind() && !Val.isUnknown() &&
          ConstValue::compare(Val, tok::eql, V.Val) && identical(V.Ty, T)) {
        err(V, diag::err_duplicate_case, {str(V)})
            .note(SeenPos[K], "previous case");
        Dup = true;
        break;
      }
    }
    if (!Dup) {
      Seen.push_back({V.Val, V.Ty});
      SeenPos.push_back(V.pos());
    }
  }
}

void Checker::switchStmt(unsigned Ctx, const SwitchStmt *S0) {
  openScope(S0->getRange(), "switch");
  headerInit(S0->Init);
  Operand X;
  if (S0->Tag) {
    expr(X, S0->Tag);
    assignment(X, nullptr, "switch expression");
    if (X.M != Mode::Invalid && !comparableType(X.Ty, true, nullptr) &&
        !hasNil(X.Ty)) {
      err(S0->Tag, diag::err_switch_tag_not_comparable, {str(X)});
      X.M = Mode::Invalid;
    }
  } else {
    X.M = Mode::Constant;
    X.Ty = typ(BasicKind::Bool);
    X.Val = ConstValue::makeBool(true);
    X.E = nullptr;
  }
  // The synthetic `true` has no expression: use the tag position when needed.
  bool SeenDefault = false;
  SmallVector<std::pair<ConstValue, Type *>, 8> Seen;
  SmallVector<SourceLoc, 8> SeenPos;
  auto Clauses = S0->Body->List;
  for (size_t K = 0; K < Clauses.size(); ++K) {
    auto *CC = dyn_cast<CaseClause>(Clauses[K]);
    if (!CC) {
      err(Clauses[K], diag::custom, {"invalid case clause"});
      continue;
    }
    if (CC->IsDefault) {
      if (SeenDefault)
        err(CC, diag::err_misplaced_default);
      SeenDefault = true;
    }
    unsigned Inner = Ctx;
    if (K + 1 < Clauses.size())
      Inner |= FallthroughOk;
    else
      Inner |= FinalSwitchCase;
    if (!S0->Tag) {
      // `switch { case cond: }` compares each value with true.
      for (const ast::Expr *Ex : CC->List) {
        Operand V;
        expr(V, Ex);
        if (V.M == Mode::Invalid)
          continue;
        if (!allBoolean(V.Ty)) {
          err(Ex, diag::custom,
              {"invalid case " + str(Ex) + " in switch (mismatched types " +
               str(V.Ty) + " and bool)"});
        } else {
          convertUntyped(V, typ(BasicKind::Bool));
        }
      }
    } else {
      caseValues(X, CC->List, Seen, SeenPos);
    }
    openScope(CC->getRange(), "case");
    stmtList(Inner, CC->Body);
    closeScope();
  }
  closeScope();
}

static bool isNilIdent(Checker &C, const ast::Expr *Ex) {
  if (auto *Id = dyn_cast<ast::Ident>(unparen(Ex)))
    return llvm::isa_and_nonnull<Nil>(C.lookup(Id->Name.str()));
  return false;
}

void Checker::typeSwitchStmt(unsigned Ctx, const TypeSwitchStmt *S0) {
  openScope(S0->getRange(), "type switch");
  headerInit(S0->Init);
  const ast::Ident *Lhs = nullptr;
  const TypeAssertExpr *Guard = nullptr;
  if (auto *AS = dyn_cast<AssignStmt>(S0->Assign)) {
    if (AS->Lhs.size() == 1 && AS->Rhs.size() == 1) {
      Lhs = dyn_cast<ast::Ident>(AS->Lhs[0]);
      Guard = dyn_cast<TypeAssertExpr>(AS->Rhs[0]);
    }
  } else if (auto *ES = dyn_cast<ExprStmt>(S0->Assign)) {
    Guard = dyn_cast<TypeAssertExpr>(unparen(ES->X));
  }
  if (!Guard) {
    err(S0->Assign, diag::err_type_switch_outside);
    closeScope();
    return;
  }
  if (Lhs) {
    if (name(Lhs) == "_") {
      err(Lhs, diag::custom, {"no new variable on left side of :="});
      Lhs = nullptr;
    } else {
      recordDef(Lhs, nullptr);
    }
  }
  Operand SX;
  bool HaveSX = false;
  {
    Operand X;
    expr(X, Guard->X);
    if (X.M != Mode::Invalid) {
      if (isTypeParam(X.Ty))
        err(X, diag::custom,
            {"cannot use type switch on type parameter value " + str(X)});
      else if (isInterface(X.Ty)) {
        SX = X;
        HaveSX = true;
      } else
        err(X, diag::err_unexpected_type_switch, {str(X)});
    }
  }
  bool SeenDefault = false;
  SmallVector<Var *, 4> LhsVars;
  SmallVector<std::pair<Type *, SourceLoc>, 8> Seen;
  for (const Stmt *Cl : S0->Body->List) {
    auto *CC = dyn_cast<CaseClause>(Cl);
    if (!CC)
      continue;
    if (CC->IsDefault) {
      if (SeenDefault)
        err(CC, diag::err_misplaced_default);
      SeenDefault = true;
    }
    Type *T = nullptr;
    for (const ast::Expr *Ex : CC->List) {
      if (isNilIdent(*this, Ex)) {
        T = nullptr;
        Operand Dummy;
        expr(Dummy, Ex);
      } else {
        T = varType(Ex);
        if (!isValid(T))
          continue;
      }
      bool Dup = false;
      for (auto &[Seen1, Pos1] : Seen) {
        if ((T == nullptr && Seen1 == nullptr) ||
            (T && Seen1 && identical(T, Seen1))) {
          err(Ex, diag::err_duplicate_type_case,
              {T ? str(T) : std::string("nil")})
              .note(Pos1, "previous case");
          Dup = true;
          break;
        }
      }
      if (Dup)
        continue;
      // TYP-9: `case string:` also matches a ref string, so a later
      // `case ref string:` is never reached.
      if (own() && T)
        if (auto *R = dyn_cast<RefType>(T))
          for (auto &[Seen1, Pos1] : Seen)
            if (Seen1 && identical(Seen1, R->Elem)) {
              err(Ex, diag::err_own_case_ref_unreachable, {str(T), str(Seen1)})
                  .note(Pos1, "previous case");
              break;
            }
      Seen.push_back({T, Ex->getBegin()});
      if (HaveSX && T)
        typeAssertion(Ex, SX, T, true);
    }
    openScope(CC->getRange(), "case");
    if (Lhs) {
      Type *VT = T;
      if (CC->List.size() != 1 || !VT) {
        VT = HaveSX ? SX.Ty : invalid();
      }
      Var *V = newVar(name(Lhs), VT, Lhs->getBegin());
      declare(E.Sc, nullptr, V, CC->Colon);
      recordImplicit(CC, V);
      if (own() && isValid(VT) && isOwning(VT) && !isInterface(VT))
        V->Mode = Var::Borrowed; // TYP-9
      LhsVars.push_back(V);
    }
    stmtList(Ctx, CC->Body);
    closeScope();
  }
  if (Lhs) {
    bool Used = false;
    for (Var *V : LhsVars) {
      Used |= V->Used;
      V->Used = true;
    }
    if (!Used && S.Opts.ReportUnused)
      err(Lhs, diag::err_declared_not_used, {std::string(name(Lhs))});
  }
  closeScope();
}

//===----------------------------------------------------------------------===//
// select
//===----------------------------------------------------------------------===//

void Checker::selectStmt(unsigned Ctx, const SelectStmt *S0) {
  bool SeenDefault = false;
  for (const Stmt *Cl : S0->Body->List) {
    auto *CC = dyn_cast<CommClause>(Cl);
    if (!CC)
      continue;
    if (!CC->Comm) {
      if (SeenDefault)
        err(CC, diag::err_misplaced_default);
      SeenDefault = true;
    }
    bool Valid = false;
    const ast::Expr *Rhs = nullptr;
    if (!CC->Comm || isa<SendStmt>(CC->Comm))
      Valid = true;
    else if (auto *AS = dyn_cast<AssignStmt>(CC->Comm)) {
      if (AS->Rhs.size() == 1)
        Rhs = AS->Rhs[0];
    } else if (auto *ES = dyn_cast<ExprStmt>(CC->Comm))
      Rhs = ES->X;
    if (Rhs)
      if (auto *U = dyn_cast<UnaryExpr>(unparen(Rhs)); U && U->Op == tok::arrow)
        Valid = true;
    if (!Valid) {
      err(CC->Comm, diag::err_select_case_invalid);
      continue;
    }
    openScope(CC->getRange(), "case");
    if (CC->Comm)
      stmt(Ctx, CC->Comm);
    stmtList(Ctx, CC->Body);
    closeScope();
  }
}

//===----------------------------------------------------------------------===//
// range
//===----------------------------------------------------------------------===//

namespace {
struct RangeTypes {
  Type *Key = nullptr, *Val = nullptr;
  std::string Cause;
  bool Ok = false;
  bool IsFunc = false;
  bool IsInt = false;
  bool IsString = false;
};
} // namespace

static RangeTypes rangeKeyVal(Checker &C, Type *T) {
  RangeTypes R;
  auto Bad = [&](std::string Cause) {
    R.Key = R.Val = C.invalid();
    R.Cause = std::move(Cause);
    R.Ok = false;
    return R;
  };
  std::string Why;
  Type *U = C.commonUnder(
      T,
      [&](Type *, Type *Under) -> std::string {
        // A channel must permit receive operations.
        if (auto *Ch = dyn_cast_or_null<sema::ChanType>(Under))
          if (Ch->D == sema::ChanType::Send)
            return "receive from send-only channel";
        return "";
      },
      &Why);
  if (!U)
    return Bad(Why);
  if (auto *P = dyn_cast<PointerType>(U))
    if (auto *A = dyn_cast<sema::ArrayType>(C.under(P->Elem))) {
      R.Key = C.typ(BasicKind::Int);
      R.Val = A->Elem;
      R.Ok = true;
      return R;
    }
  if (auto *Rf = dyn_cast<RefType>(U))
    U = C.under(Rf->Elem);
  switch (U->getKind()) {
  case Type::Kind::Basic: {
    auto *B = cast<BasicType>(U);
    if (B->Info & IsString) {
      R.Key = C.typ(BasicKind::Int);
      R.Val = C.Ctx.runeType();
      R.IsString = true;
      R.Ok = true;
      return R;
    }
    if (B->Info & IsInteger) {
      R.Key = T;
      R.Val = nullptr;
      R.IsInt = true;
      R.Ok = true;
      return R;
    }
    break;
  }
  case Type::Kind::Array:
    R.Key = C.typ(BasicKind::Int);
    R.Val = cast<sema::ArrayType>(U)->Elem;
    R.Ok = true;
    return R;
  case Type::Kind::Slice:
    R.Key = C.typ(BasicKind::Int);
    R.Val = cast<SliceType>(U)->Elem;
    R.Ok = true;
    return R;
  case Type::Kind::Map:
    R.Key = cast<sema::MapType>(U)->Key;
    R.Val = cast<sema::MapType>(U)->Elem;
    R.Ok = true;
    return R;
  case Type::Kind::Chan: {
    auto *Ch = cast<sema::ChanType>(U);
    if (Ch->D == sema::ChanType::Send)
      return Bad("receive from send-only channel");
    R.Key = Ch->Elem;
    R.Ok = true;
    return R;
  }
  case Type::Kind::Signature: {
    auto *Sg = cast<SignatureType>(U);
    R.IsFunc = true;
    const char *Pre = "func must be func(yield func(...) bool): ";
    if (Sg->Params->size() != 1)
      return Bad(std::string(Pre) + "wrong argument count");
    if (Sg->Results->size() != 0)
      return Bad(std::string(Pre) + "unexpected results");
    std::string YieldWhy;
    Type *CbT = C.commonUnder(Sg->Params->Elems[0], nullptr, &YieldWhy);
    if (!CbT)
      return Bad(std::string(Pre) + "in yield type, " + YieldWhy);
    auto *Cb = dyn_cast<SignatureType>(CbT);
    if (!Cb)
      return Bad(std::string(Pre) + "argument is not func");
    if (Cb->Params->size() > 2)
      return Bad(std::string(Pre) + "yield func has too many parameters");
    if (Cb->Results->size() != 1 ||
        !C.identical(Cb->Results->Elems[0], C.typ(BasicKind::Bool))) {
      if (Cb->Results->size() == 1 && C.isBoolean(Cb->Results->Elems[0]))
        return Bad(std::string(Pre) +
                   "yield func returns user-defined boolean, not bool");
      return Bad(std::string(Pre) + "yield func does not return bool");
    }
    if (Cb->Params->size() >= 1)
      R.Key = Cb->Params->Elems[0];
    if (Cb->Params->size() >= 2)
      R.Val = Cb->Params->Elems[1];
    R.Ok = true;
    return R;
  }
  default:
    break;
  }
  R.Ok = false;
  return R;
}

void Checker::rangeStmt(unsigned Ctx, const RangeStmt *S0) {
  const ast::Expr *SKey = S0->Key, *SValue = S0->Value;
  bool IsDef = S0->Op == tok::define;
  Operand X;
  expr(X, S0->X);
  // The scope of the iteration variables opens after the range expression
  // (types2, go.dev/issue/51437).
  openScope(S0->getRange(), "range");
  Type *Key = nullptr, *Val = nullptr;
  RangeTypes RT;
  bool IsInt = false;
  if (X.M != Mode::Invalid) {
    RT = rangeKeyVal(*this, X.Ty);
    IsInt = RT.IsInt;
    if (!RT.Ok && !RT.Cause.empty())
      err(X, diag::custom, {"cannot range over " + str(X) + ": " + RT.Cause});
    else if (!RT.Ok)
      err(X, diag::err_range_cannot, {str(X)});
    else if (RT.IsFunc && ((!RT.Key && SKey) || (!RT.Val && SValue))) {
      if (!RT.Key)
        err(SKey, diag::custom,
            {"range over " + str(X) + " permits no iteration variables"});
      else
        err(SValue, diag::custom,
            {"range over " + str(X) + " permits only one iteration variable"});
    } else if (SValue && !RT.Val)
      err(SValue, diag::custom,
          {"range over " + str(X) + " permits only one iteration variable"});
    Key = RT.Key;
    Val = RT.Val;
  }
  // Go++ (RNG-1a): the `&` form aliases the element: slices, arrays, map
  // values, and iterators that yield pointers (RNG-6a).
  RangeStmt::AliasKind Alias = S0->Alias;
  bool AliasPointee = false;
  if (own() && Alias != RangeStmt::AliasKind::None && RT.Ok) {
    const char *Bad = nullptr;
    Type *U = coreType(X.Ty);
    if (RT.IsString)
      Bad = "a string";
    else if (RT.IsInt)
      Bad = "an integer";
    else if (U && isa<sema::ChanType>(U))
      Bad = "a channel";
    if (Bad) {
      err(S0->AliasLoc, diag::err_own_range_alias_invalid, {Bad});
    } else if (RT.IsFunc) {
      Type *Y = Alias == RangeStmt::AliasKind::Key ? Key : Val;
      if (Y && !isa<PointerType>(under(Y)))
        err(S0->AliasLoc, diag::err_own_range_alias_func);
      else
        AliasPointee = Y != nullptr;
    } else if (Alias == RangeStmt::AliasKind::Key) {
      err(S0->AliasLoc, diag::err_own_range_alias_index, {str(S0->X)});
    } else if (U && isa<sema::ArrayType>(U) && !isa<PointerType>(U)) {
      // An array held by value in a borrowed variable is borrowed storage.
      if (Var *BV = borrowedRoot(S0->X))
        err(S0->AliasLoc, diag::err_own_range_alias_borrowed,
            {str(S0->X), std::string(BV->Name)});
    }
  }
  // RNG-2 / RNG-6: `range move(xs)` and `range ch` give owned variables.
  bool OwnedVars = isBuiltinCall(S0->X, BuiltinId::Move) ||
                   (X.M != Mode::Invalid && coreType(X.Ty) &&
                    isa<sema::ChanType>(coreType(X.Ty)));

  const ast::Expr *Lhs[2] = {SKey, SValue};
  Type *Rhs[2] = {Key, Val};
  if (IsDef) {
    SmallVector<Var *, 2> Vars;
    for (int K = 0; K < 2; ++K) {
      if (!Lhs[K])
        continue;
      Var *Obj = nullptr;
      if (auto *Id = dyn_cast<ast::Ident>(Lhs[K])) {
        Obj = newVar(name(Id), nullptr, Id->getBegin());
        recordDef(Id, Obj);
        if (name(Id) != "_")
          Vars.push_back(Obj);
      } else {
        err(Lhs[K], diag::custom, {"cannot declare " + str(Lhs[K])});
        Obj = newVar("_", nullptr, Lhs[K]->getBegin());
      }
      Obj->LoopVar = true;
      Type *T = Rhs[K];
      if (!T || !isValid(T)) {
        Obj->Ty = invalid();
        Obj->Used = true;
        continue;
      }
      bool IsAlias =
          own() && ((Alias == RangeStmt::AliasKind::Key && K == 0) ||
                    (Alias == RangeStmt::AliasKind::Value && K == 1));
      // `for &d := range seq` with seq yielding *Doc: d is the Doc (RNG-6a).
      if (IsAlias && AliasPointee)
        if (auto *P = dyn_cast<PointerType>(under(T)))
          T = P->Elem;
      if (IsInt) {
        initVar(Obj, X, "range clause");
      } else {
        Operand Y;
        Y.M = Mode::Value;
        Y.E = Lhs[K];
        Y.Ty = T;
        // The elements are borrowed (RNG-1): no ownership is taken.
        bool Saved = NoOwnCheck;
        NoOwnCheck = true;
        initVar(Obj, Y, "assignment");
        NoOwnCheck = Saved;
        // OWN-R10: a range variable is a copy of the element.
        if (own() && !IsAlias && !OwnedVars && Obj->Ty && isValid(Obj->Ty) &&
            isPinned(Obj->Ty))
          err(Lhs[K], diag::err_own_pinned_copy,
              {str(S0->X) + " element", str(Obj->Ty)});
        if (own() && Obj->Ty && isValid(Obj->Ty)) {
          if (IsAlias)
            Obj->Mode = Var::Alias;
          else if (!OwnedVars && isOwning(Obj->Ty))
            Obj->Mode = Var::Borrowed;
        }
      }
    }
    if (!Vars.empty()) {
      for (Var *V : Vars) {
        declare(E.Sc, nullptr, V, S0->Body->getBegin());
        LocalVars.push_back(V);
      }
    } else {
      err(S0->OpLoc, diag::err_define_no_new_vars);
    }
  } else if (SKey) {
    for (int K = 0; K < 2; ++K) {
      if (!Lhs[K])
        continue;
      Type *T = Rhs[K];
      if (!T || !isValid(T))
        continue;
      if (IsInt) {
        assignVar(Lhs[K], nullptr, &X, "range clause");
        if (X.M != Mode::Invalid && !isInteger(X.Ty))
          err(Lhs[K], diag::custom,
              {"cannot use iteration variable of type " + str(X.Ty)});
      } else {
        Operand Y;
        Y.M = Mode::Value;
        Y.E = Lhs[K];
        Y.Ty = T;
        bool Saved = NoOwnCheck;
        NoOwnCheck = true;
        assignVar(Lhs[K], nullptr, &Y, "assignment");
        NoOwnCheck = Saved;
      }
    }
  } else if (IsInt) {
    assignment(X, nullptr, "range clause");
  }
  // RNG-3: the body must not visibly reallocate a ranged place.
  bool Frame = own() && X.M != Mode::Invalid && !OwnedVars &&
               placeKind(S0->X) != PlaceKind::Temp;
  if (Frame) {
    RangeFrame F;
    // An unstable range expression (`f()`) names no place to protect.
    if (samePlace(S0->X, S0->X))
      F.X = str(unparen(S0->X));
    F.XTy = X.Ty;
    if (SKey)
      F.Key = dyn_cast_or_null<Var>(identObj(SKey));
    RangeStack.push_back(std::move(F));
  }
  stmt(Ctx, S0->Body);
  if (Frame)
    RangeStack.pop_back();
  closeScope();
}

//===----------------------------------------------------------------------===//
// Labels (labels.go)
//===----------------------------------------------------------------------===//

namespace {
struct LabelInfo {
  const LabeledStmt *S;
  bool Used = false;
};
struct FwdJump {
  const BranchStmt *B;
  StringRef Name;
};
} // namespace

namespace {
class LabelChecker {
public:
  LabelChecker(Checker &C) : C(C) {}
  void run(const BlockStmt *Body) {
    collect(Body->List);
    SmallVector<FwdJump, 4> Fwd = blockBranches(nullptr, Body->List, Body);
    // Remaining forward jumps refer to undeclared labels.
    for (FwdJump &J : Fwd) {
      if (!Labels.count(J.Name))
        C.err(J.B->Label, diag::err_label_undeclared, {std::string(J.Name)});
    }
    for (auto &KV : Labels)
      if (!KV.second.Used)
        C.err(KV.second.S->Label, diag::err_label_not_used,
              {std::string(KV.first())});
  }

private:
  Checker &C;
  llvm::StringMap<LabelInfo> Labels;

  void collect(ArrayRef<Stmt *> L) {
    for (const Stmt *S : L)
      collectOne(S);
  }
  void collectOne(const Stmt *S) {
    using K = ast::Node::Kind;
    switch (S->getKind()) {
    case K::LabeledStmt: {
      auto *LS = cast<LabeledStmt>(S);
      StringRef N = LS->Label->Name.str();
      if (N != "_") {
        auto Ins = Labels.try_emplace(N, LabelInfo{LS, false});
        if (!Ins.second)
          C.err(LS->Label, diag::err_label_redeclared, {std::string(N)})
              .note(Ins.first->second.S->Label->getBegin(),
                    "previous declaration");
      }
      collectOne(LS->Body);
      break;
    }
    case K::BlockStmt:
      collect(cast<BlockStmt>(S)->List);
      break;
    case K::IfStmt: {
      auto *IS = cast<IfStmt>(S);
      collectOne(IS->Then);
      if (IS->Else)
        collectOne(IS->Else);
      break;
    }
    case K::SwitchStmt:
      collect(cast<SwitchStmt>(S)->Body->List);
      break;
    case K::TypeSwitchStmt:
      collect(cast<TypeSwitchStmt>(S)->Body->List);
      break;
    case K::SelectStmt:
      collect(cast<SelectStmt>(S)->Body->List);
      break;
    case K::CaseClause:
      collect(cast<CaseClause>(S)->Body);
      break;
    case K::CommClause:
      collect(cast<CommClause>(S)->Body);
      break;
    case K::ForStmt:
      collectOne(cast<ForStmt>(S)->Body);
      break;
    case K::RangeStmt:
      collectOne(cast<RangeStmt>(S)->Body);
      break;
    default:
      break;
    }
  }

  static bool isVarDecl(const Stmt *S) {
    if (auto *DS = dyn_cast<DeclStmt>(S))
      if (auto *GD = dyn_cast<GenDecl>(DS->D))
        return GD->Keyword == tok::kw_var;
    if (auto *AS = dyn_cast<AssignStmt>(S))
      return AS->Op == tok::define;
    return false;
  }

  /// Checks branches in a block; returns the forward jumps not resolved in it.
  SmallVector<FwdJump, 4> blockBranches(const LabeledStmt *Enclosing,
                                        ArrayRef<Stmt *> List,
                                        const Stmt *Parent) {
    SmallVector<FwdJump, 4> Fwd;
    llvm::StringMap<const LabeledStmt *> Declared;
    SourceLoc VarDeclPos;
    bool HaveVar = false;
    SmallVector<std::pair<StringRef, const BranchStmt *>, 4> Pending;
    (void)Parent;
    auto Jumped = [&](const BranchStmt *B, StringRef N) {
      Fwd.push_back({B, N});
    };
    std::function<void(const Stmt *, const LabeledStmt *)> Stmt1;
    auto Block = [&](ArrayRef<Stmt *> L, const LabeledStmt *Lab,
                     const Stmt *P) {
      SmallVector<FwdJump, 4> Inner = blockBranches(Lab, L, P);
      for (FwdJump &J : Inner)
        Fwd.push_back(J);
    };
    Stmt1 = [&](const Stmt *S, const LabeledStmt *Lab) {
      using K = ast::Node::Kind;
      if (isVarDecl(S)) {
        VarDeclPos = S->getBegin();
        HaveVar = true;
        // Pending forward gotos in this block jump over this declaration.
        for (auto &[N, B] : Pending) {
          (void)N;
          (void)B;
        }
        for (FwdJump &J : Fwd)
          if (J.B && !Declared.count(J.Name) &&
              JumpVarPos.find(J.B) == JumpVarPos.end())
            JumpVarPos[J.B] = S->getBegin();
      }
      switch (S->getKind()) {
      case K::DeclStmt:
        break;
      case K::LabeledStmt: {
        auto *LS = cast<LabeledStmt>(S);
        StringRef N = LS->Label->Name.str();
        Declared[N] = LS;
        // Resolve pending forward jumps to this label.
        SmallVector<FwdJump, 4> Rest;
        for (FwdJump &J : Fwd) {
          if (J.Name == N) {
            Labels[N].Used = true;
            auto It = JumpVarPos.find(J.B);
            if (It != JumpVarPos.end())
              C.err(J.B->Label, diag::err_goto_jumps_over,
                    {std::string(N),
                     std::to_string(C.S.SM.getLineNumber(It->second))});
          } else {
            Rest.push_back(J);
          }
        }
        Fwd = Rest;
        Stmt1(LS->Body, LS);
        break;
      }
      case K::BranchStmt: {
        auto *B = cast<BranchStmt>(S);
        if (!B->Label)
          break;
        StringRef N = B->Label->Name.str();
        switch (B->Keyword) {
        case tok::kw_break:
        case tok::kw_continue: {
          // Must refer to an enclosing labeled statement.
          bool Ok = false;
          for (const LabeledStmt *E = Enclosing; E; E = nullptr)
            (void)E;
          for (const LabeledStmt *E2 : EnclosingStack) {
            if (E2->Label->Name.str() != N)
              continue;
            const Stmt *Body = E2->Body;
            if (B->Keyword == tok::kw_break)
              Ok = isa<ForStmt>(Body) || isa<RangeStmt>(Body) ||
                   isa<SwitchStmt>(Body) || isa<TypeSwitchStmt>(Body) ||
                   isa<SelectStmt>(Body);
            else
              Ok = isa<ForStmt>(Body) || isa<RangeStmt>(Body);
            break;
          }
          auto It = Labels.find(N);
          if (It == Labels.end()) {
            C.err(B->Label, diag::err_label_undeclared, {std::string(N)});
          } else {
            It->second.Used = true;
            if (!Ok)
              C.err(B->Label,
                    B->Keyword == tok::kw_break
                        ? diag::err_invalid_break_label
                        : diag::err_invalid_continue_label,
                    {std::string(N)});
          }
          break;
        }
        case tok::kw_goto: {
          auto It = Labels.find(N);
          if (It == Labels.end()) {
            C.err(B->Label, diag::err_label_undeclared, {std::string(N)});
            break;
          }
          It->second.Used = true;
          auto D = Declared.find(N);
          if (D != Declared.end())
            break; // backward jump in the same block: fine
          Jumped(B, N);
          break;
        }
        default:
          break;
        }
        break;
      }
      case K::BlockStmt:
        Block(cast<BlockStmt>(S)->List, Lab, S);
        break;
      case K::IfStmt: {
        auto *IS = cast<IfStmt>(S);
        Block(IS->Then->List, Lab, IS->Then);
        if (IS->Else)
          Stmt1(IS->Else, nullptr);
        break;
      }
      case K::SwitchStmt:
        pushLab(Lab);
        for (const Stmt *Cl : cast<SwitchStmt>(S)->Body->List)
          Block(cast<CaseClause>(Cl)->Body, nullptr, Cl);
        popLab(Lab);
        break;
      case K::TypeSwitchStmt:
        pushLab(Lab);
        for (const Stmt *Cl : cast<TypeSwitchStmt>(S)->Body->List)
          Block(cast<CaseClause>(Cl)->Body, nullptr, Cl);
        popLab(Lab);
        break;
      case K::SelectStmt:
        pushLab(Lab);
        for (const Stmt *Cl : cast<SelectStmt>(S)->Body->List)
          Block(cast<CommClause>(Cl)->Body, nullptr, Cl);
        popLab(Lab);
        break;
      case K::ForStmt:
        pushLab(Lab);
        Block(cast<ForStmt>(S)->Body->List, nullptr, S);
        popLab(Lab);
        break;
      case K::RangeStmt:
        pushLab(Lab);
        Block(cast<RangeStmt>(S)->Body->List, nullptr, S);
        popLab(Lab);
        break;
      default:
        break;
      }
    };
    (void)VarDeclPos;
    (void)HaveVar;
    for (const Stmt *S : List)
      Stmt1(S, nullptr);
    // Jumps out of this block into an enclosing block are fine; jumps into a
    // nested block are caught below.
    SmallVector<FwdJump, 4> Out;
    for (FwdJump &J : Fwd) {
      // A label declared in a nested block of this block: jumping into it.
      auto It = Labels.find(J.Name);
      if (It != Labels.end() && !isDeclaredIn(List, J.Name) &&
          isNestedIn(List, J.Name)) {
        C.err(J.B->Label, diag::err_goto_jumps_into, {std::string(J.Name)});
        continue;
      }
      Out.push_back(J);
    }
    return Out;
  }

  llvm::DenseMap<const BranchStmt *, SourceLoc> JumpVarPos;
  SmallVector<const LabeledStmt *, 4> EnclosingStack;
  void pushLab(const LabeledStmt *L) {
    if (L)
      EnclosingStack.push_back(L);
  }
  void popLab(const LabeledStmt *L) {
    if (L)
      EnclosingStack.pop_back();
  }
  bool isDeclaredIn(ArrayRef<Stmt *> L, StringRef N) {
    for (const Stmt *S : L)
      if (auto *LS = dyn_cast<LabeledStmt>(S))
        if (LS->Label->Name.str() == N)
          return true;
    return false;
  }
  bool isNestedIn(ArrayRef<Stmt *> L, StringRef N) {
    // True if some statement of L (recursively) declares label N.
    for (const Stmt *S : L) {
      if (declares(S, N))
        return true;
    }
    return false;
  }
  bool declares(const Stmt *S, StringRef N) {
    using K = ast::Node::Kind;
    switch (S->getKind()) {
    case K::LabeledStmt: {
      auto *LS = cast<LabeledStmt>(S);
      return LS->Label->Name.str() == N || declares(LS->Body, N);
    }
    case K::BlockStmt:
      for (const Stmt *X : cast<BlockStmt>(S)->List)
        if (declares(X, N))
          return true;
      return false;
    case K::IfStmt: {
      auto *IS = cast<IfStmt>(S);
      return declares(IS->Then, N) || (IS->Else && declares(IS->Else, N));
    }
    case K::ForStmt:
      return declares(cast<ForStmt>(S)->Body, N);
    case K::RangeStmt:
      return declares(cast<RangeStmt>(S)->Body, N);
    case K::SwitchStmt:
      return declares(cast<SwitchStmt>(S)->Body, N);
    case K::TypeSwitchStmt:
      return declares(cast<TypeSwitchStmt>(S)->Body, N);
    case K::SelectStmt:
      return declares(cast<SelectStmt>(S)->Body, N);
    case K::CaseClause:
      for (const Stmt *X : cast<CaseClause>(S)->Body)
        if (declares(X, N))
          return true;
      return false;
    case K::CommClause:
      for (const Stmt *X : cast<CommClause>(S)->Body)
        if (declares(X, N))
          return true;
      return false;
    default:
      return false;
    }
  }
};
} // namespace

void Checker::labels(const BlockStmt *Body) {
  LabelChecker LC(*this);
  LC.run(Body);
}

//===----------------------------------------------------------------------===//
// Terminating statements (return.go)
//===----------------------------------------------------------------------===//

bool Checker::isTerminating(const Stmt *S0, StringRef Label) {
  using K = ast::Node::Kind;
  switch (S0->getKind()) {
  case K::ReturnStmt:
    return true;
  case K::BranchStmt: {
    auto *B = cast<BranchStmt>(S0);
    return B->Keyword == tok::kw_goto || B->Keyword == tok::kw_fallthrough;
  }
  case K::ExprStmt: {
    auto *Call = dyn_cast<CallExpr>(unparen(cast<ExprStmt>(S0)->X));
    if (!Call)
      return false;
    if (auto *Id = dyn_cast<ast::Ident>(unparen(Call->Fun))) {
      Object *O = nullptr;
      if (const Selection *Dummy = nullptr; !Dummy) {
        auto It = I.Uses.find(Id);
        if (It != I.Uses.end())
          O = It->second;
      }
      if (auto *B = dyn_cast_or_null<Builtin>(O))
        return B->Id == BuiltinId::Panic;
    }
    return false;
  }
  case K::BlockStmt:
    return isTerminatingList(cast<BlockStmt>(S0)->List, "");
  case K::LabeledStmt: {
    auto *LS = cast<LabeledStmt>(S0);
    return isTerminating(LS->Body, LS->Label->Name.str());
  }
  case K::IfStmt: {
    auto *IS = cast<IfStmt>(S0);
    return IS->Else && isTerminating(IS->Then, "") &&
           isTerminating(IS->Else, "");
  }
  case K::SwitchStmt:
    return isTerminatingSwitch(cast<SwitchStmt>(S0)->Body, Label);
  case K::TypeSwitchStmt:
    return isTerminatingSwitch(cast<TypeSwitchStmt>(S0)->Body, Label);
  case K::SelectStmt:
    for (const Stmt *Cl : cast<SelectStmt>(S0)->Body->List) {
      auto *CC = cast<CommClause>(Cl);
      if (!isTerminatingList(CC->Body, "") ||
          hasBreakList(CC->Body, Label, true))
        return false;
    }
    return true;
  case K::ForStmt: {
    auto *FS = cast<ForStmt>(S0);
    return !FS->Cond && !hasBreak(FS->Body, Label, true);
  }
  default:
    return false;
  }
}

bool Checker::isTerminatingList(ArrayRef<Stmt *> L, StringRef Label) {
  // Trailing empty statements are permitted.
  size_t N = L.size();
  while (N > 0 && isa<EmptyStmt>(L[N - 1]))
    --N;
  return N > 0 && isTerminating(L[N - 1], Label);
}

bool Checker::isTerminatingSwitch(const BlockStmt *Body, StringRef Label) {
  bool HasDefault = false;
  for (const Stmt *Cl : Body->List) {
    auto *CC = cast<CaseClause>(Cl);
    if (CC->IsDefault)
      HasDefault = true;
    if (!isTerminatingList(CC->Body, "") || hasBreakList(CC->Body, Label, true))
      return false;
  }
  return HasDefault;
}

bool Checker::hasBreak(const Stmt *S0, StringRef Label, bool Implicit) {
  using K = ast::Node::Kind;
  switch (S0->getKind()) {
  case K::BranchStmt: {
    auto *B = cast<BranchStmt>(S0);
    if (B->Keyword == tok::kw_break) {
      if (!B->Label)
        return Implicit;
      return !Label.empty() && B->Label->Name.str() == Label;
    }
    return false;
  }
  case K::BlockStmt:
    return hasBreakList(cast<BlockStmt>(S0)->List, Label, Implicit);
  case K::LabeledStmt:
    return hasBreak(cast<LabeledStmt>(S0)->Body, Label, Implicit);
  case K::IfStmt: {
    auto *IS = cast<IfStmt>(S0);
    return hasBreak(IS->Then, Label, Implicit) ||
           (IS->Else && hasBreak(IS->Else, Label, Implicit));
  }
  case K::CaseClause:
    return hasBreakList(cast<CaseClause>(S0)->Body, Label, Implicit);
  case K::CommClause:
    return hasBreakList(cast<CommClause>(S0)->Body, Label, Implicit);
  case K::SwitchStmt:
    if (!Label.empty() && hasBreak(cast<SwitchStmt>(S0)->Body, Label, false))
      return true;
    return false;
  case K::TypeSwitchStmt:
    if (!Label.empty() &&
        hasBreak(cast<TypeSwitchStmt>(S0)->Body, Label, false))
      return true;
    return false;
  case K::SelectStmt:
    if (!Label.empty() && hasBreak(cast<SelectStmt>(S0)->Body, Label, false))
      return true;
    return false;
  case K::ForStmt:
    if (!Label.empty() && hasBreak(cast<ForStmt>(S0)->Body, Label, false))
      return true;
    return false;
  case K::RangeStmt:
    if (!Label.empty() && hasBreak(cast<RangeStmt>(S0)->Body, Label, false))
      return true;
    return false;
  default:
    return false;
  }
}

bool Checker::hasBreakList(ArrayRef<Stmt *> L, StringRef Label, bool Implicit) {
  for (const Stmt *S0 : L)
    if (hasBreak(S0, Label, Implicit))
      return true;
  return false;
}
