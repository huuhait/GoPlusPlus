//===- Assign.cpp - Assignments and initialization ------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's assignments.go (BSD-style license, Copyright The Go
// Authors). The ownership hooks (OWN-R3a, OWN-R3b) are the Go++ additions.
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

static std::string measure(unsigned N, const char *Unit) {
  return std::to_string(N) + " " + Unit + (N != 1 ? "s" : "");
}

void Checker::assignment(Operand &X, Type *T, StringRef Context) {
  singleValue(X);
  switch (X.M) {
  case Mode::Invalid:
    return;
  case Mode::Constant:
  case Mode::Variable:
  case Mode::MapIndex:
  case Mode::Value:
  case Mode::NilValue:
  case Mode::CommaOk:
  case Mode::CommaErr:
    break;
  default:
    err(X, diag::custom,
        {"cannot assign " + str(X) + " to " + (T ? str(T) : std::string("_")) +
         " in " + std::string(Context)});
    X.M = Mode::Invalid;
    return;
  }
  if (isUntyped(X.Ty)) {
    Type *Target = T;
    if (!T || isNonTypeParamInterface(T)) {
      if (!T && X.Ty == typ(BasicKind::UntypedNil)) {
        err(X, diag::err_use_of_untyped_nil, {std::string(Context)});
        X.M = Mode::Invalid;
        return;
      }
      Target = defaultType(X.Ty);
    }
    ConstValue NV;
    ReprCode Code;
    Type *NewT = implicitTypeAndValue(X, Target, &NV, &Code);
    if (!NewT) {
      std::string Msg = "cannot use " + str(X) + " as " + str(Target) +
                        " value in " + std::string(Context);
      if (Code == ReprCode::Truncated)
        Msg += " (truncated)";
      else if (Code == ReprCode::Overflow)
        Msg += " (overflows)";
      err(X, diag::custom, {Msg});
      X.M = Mode::Invalid;
      return;
    }
    if (X.M == Mode::Constant && !NV.isUnknown() && NewT != X.Ty) {
      X.Val = NV;
      updateExprVal(X.E, NV);
    }
    if (NewT != X.Ty) {
      X.Ty = NewT;
      updateExprType(X.E, NewT, false);
    }
  }
  if (auto *Sg = dyn_cast_or_null<SignatureType>(under(X.Ty)))
    if (!Sg->TParams.empty()) {
      err(X, diag::custom,
          {"cannot use generic function " + str(X.E) +
           " without instantiation in " + std::string(Context)});
      X.M = Mode::Invalid;
      return;
    }
  if (!T)
    return;
  std::string Cause;
  if (!assignableTo(X, T, &Cause)) {
    std::string Msg = "cannot use " + str(X) + " as " + str(T) + " value in " +
                      std::string(Context);
    if (!Cause.empty())
      Msg += ": " + Cause;
    err(X, diag::custom, {Msg});
    X.M = Mode::Invalid;
  }
}

void Checker::initConst(Const *Lhs, Operand &X) {
  if (X.M == Mode::Invalid || !X.Ty || !isValid(X.Ty) ||
      (Lhs->Ty && !isValid(Lhs->Ty))) {
    if (!Lhs->Ty)
      Lhs->Ty = invalid();
    X.M = Mode::Invalid;
    return;
  }
  if (X.M != Mode::Constant) {
    err(X, diag::err_not_constant, {str(X)});
    if (!Lhs->Ty)
      Lhs->Ty = invalid();
    X.M = Mode::Invalid;
    return;
  }
  if (!Lhs->Ty)
    Lhs->Ty = X.Ty;
  assignment(X, Lhs->Ty, "constant declaration");
  if (X.M == Mode::Invalid)
    return;
  Lhs->Val = Mem.constant(X.Val);
}

void Checker::initVar(Var *Lhs, Operand &X, StringRef Context) {
  if (X.M == Mode::Invalid || !X.Ty || !isValid(X.Ty) ||
      (Lhs->Ty && !isValid(Lhs->Ty))) {
    if (!Lhs->Ty)
      Lhs->Ty = invalid();
    X.M = Mode::Invalid;
    return;
  }
  if (!Lhs->Ty) {
    Type *T = X.Ty;
    if (isUntyped(T)) {
      if (T == typ(BasicKind::UntypedNil)) {
        err(X, diag::err_use_of_untyped_nil, {std::string(Context)});
        Lhs->Ty = invalid();
        X.M = Mode::Invalid;
        return;
      }
      T = defaultType(T);
    }
    Lhs->Ty = T;
  }
  assignment(X, Lhs->Ty, Context);
  if (!own() || X.M == Mode::Invalid)
    return;
  bool IsReturn = Context == "return statement";
  // TYP-9: `s := v.(string)` binds a borrowed variable to the shared box. Lhs
  // is new here when it is not in a scope yet.
  if (!IsReturn && !Lhs->Parent && isa<TypeAssertExpr>(unparen(X.E)) &&
      isOwning(Lhs->Ty) && !isInterface(Lhs->Ty)) {
    Lhs->Mode = Var::Borrowed;
    return;
  }
  // A ref destination borrows: no ownership is taken (TYP-2), but the ref
  // must not point into a temporary (OWN-V6; returns: checkReturnRef).
  if (isa<RefType>(Lhs->Ty)) {
    // A variable declared in a statement header lives no longer than the
    // header's temporaries (OWN-R2, STM-4).
    bool DeclaredInHeader = InHeaderInit && !Lhs->Parent;
    if (!IsReturn && !DeclaredInHeader)
      checkRefStore(X.E, Lhs->Ty, X.Ty);
    return;
  }
  // The blank identifier keeps nothing: `_, ok := m[k]` moves no value.
  if (!IsReturn && Lhs->Name == "_")
    return;
  needOwnership(X, IsReturn ? "return" : "initialize from", Lhs->Ty);
}

Type *Checker::lhsVar(const ast::Expr *Lhs) {
  // Assigning to a variable does not count as a use.
  Var *V = nullptr;
  bool WasUsed = false;
  if (auto *Id = dyn_cast<ast::Ident>(unparen(Lhs))) {
    if (name(Id) == "_") {
      recordDef(Id, nullptr);
      return nullptr;
    }
    if (Object *O = E.Sc->lookup(name(Id), E.Pos))
      if ((V = dyn_cast<Var>(O))) {
        if (V->Pkg == S.Pkg && !V->IsPackageLevel) {
          WasUsed = V->Used;
        } else
          V = nullptr;
      }
  }
  Operand X;
  expr(X, Lhs);
  if (V)
    // RNG-1a: a write to an `&` alias is the point of the loop: it counts as
    // a use.
    V->Used = WasUsed || V->Mode == Var::Alias;
  if (X.M == Mode::Invalid)
    return invalid();
  switch (X.M) {
  case Mode::Invalid:
    return invalid();
  case Mode::Variable:
  case Mode::MapIndex:
    break;
  default:
    if (auto *Sel = dyn_cast<SelectorExpr>(unparen(X.E))) {
      Operand Base;
      // x.f = v with x a non-addressable map element / call result.
      const TypeAndValue *TV = I.lookup(Sel->X);
      if (TV && TV->M == Mode::MapIndex) {
        err(Lhs, diag::custom,
            {"cannot assign to struct field " + str(Lhs) + " in map"});
        return invalid();
      }
      (void)Base;
    }
    err(Lhs, diag::err_cannot_assign, {str(Lhs)});
    return invalid();
  }
  return X.Ty;
}

Type *Checker::assignVar(const ast::Expr *Lhs, const ast::Expr *RhsExpr,
                         Operand *X, StringRef Context) {
  Type *T = lhsVar(Lhs); // nullptr for the blank identifier
  if (T && !isValid(T)) {
    if (X)
      X->M = Mode::Invalid;
    else
      use(RhsExpr);
    return nullptr;
  }
  Operand Local;
  if (!X) {
    X = &Local;
    ReverseDesc = str(Lhs);
    exprWithHint(*X, RhsExpr, T);
    ReverseDesc.clear();
  }
  if (!T && Lhs) {
    // blank identifier
    assignment(*X, nullptr, Context);
    return nullptr;
  }
  bool OpAssign = Context == "assignment operation";
  // PAR-2: a borrowed string / []T variable is a local ref, so it can be
  // re-pointed at a ref.
  bool Repoint = false, Shrink = false;
  if (own() && !OpAssign && X->M != Mode::Invalid && X->Ty &&
      isa<RefType>(X->Ty)) {
    Type *RefElem = cast<RefType>(X->Ty)->Elem;
    if (auto *Id = dyn_cast<ast::Ident>(unparen(Lhs)))
      if (auto *V = dyn_cast_or_null<Var>(I.Uses.lookup(Id)))
        if (V->Mode == Var::Borrowed && identical(RefElem, T)) {
          X->Ty = T;
          Repoint = true;
        }
    // SLC-6: `s = s[i:j]` on the same owned slice shrinks it in place.
    if (!Repoint && X->E)
      if (auto *Sl = dyn_cast<SliceExpr>(unparen(X->E)))
        if (isa<SliceType>(under(T)) && identical(RefElem, T) &&
            samePlace(Sl->X, Lhs)) {
          X->Ty = T;
          Shrink = true;
        }
  }
  if (isa<SelectorExpr>(unparen(Lhs)) && checkPointerField(*X, T))
    return T;
  assignment(*X, T, Context);
  if (!own())
    return T;
  // `xs = append(xs, ...)` on a borrowed xs: append reported it (PAR-1).
  bool SelfAppend = false;
  if (X->E && isBuiltinCall(X->E, BuiltinId::Append))
    if (auto *Call = cast<CallExpr>(unparen(X->E)); !Call->Args.empty())
      SelfAppend = isImplicitMove(Call->Args[0], Call) &&
                   placeKind(Call->Args[0]) == PlaceKind::BorrowedVar;
  if (!SelfAppend)
    checkWrite(Lhs, Repoint);
  checkRangeMutation(Lhs, Shrink     ? "re-slice"
                          : OpAssign ? "grow"
                                     : "assign to");
  if (X->M == Mode::Invalid || Repoint || Shrink || OpAssign)
    return T;
  if (isa<RefType>(T))
    checkRefStore(X->E, T, X->Ty);
  else
    needOwnership(*X, "assign from", T);
  return T;
}

void Checker::assignError(ArrayRef<ast::Expr *> Rhs, unsigned L, unsigned R) {
  std::string Vars = measure(L, "variable"), Vals = measure(R, "value");
  const ast::Expr *Rhs0 = Rhs[0];
  if (Rhs.size() == 1)
    if (auto *Call = dyn_cast<CallExpr>(unparen(Rhs0))) {
      err(Rhs0, diag::err_returns_mismatch, {Vars, str(Call->Fun), Vals});
      return;
    }
  err(Rhs0, diag::err_assign_mismatch, {Vars, Vals});
}

void Checker::returnError(const Stmt *At, ArrayRef<Var *> Lhs,
                          ArrayRef<Operand> Rhs) {
  size_t L = Lhs.size(), R = Rhs.size();
  SourceLoc Loc = At->getBegin();
  bool TooMany = R > L;
  if (TooMany)
    Loc = Rhs[L].pos();
  else if (R > 0)
    Loc = Rhs[R - 1].pos();
  SmallVector<Type *, 4> Have, Want;
  for (const Operand &O : Rhs)
    Have.push_back(O.Ty);
  for (Var *V : Lhs)
    Want.push_back(V->Ty);
  err(Loc,
      TooMany ? diag::err_too_many_arguments_return
              : diag::err_not_enough_arguments_return,
      {typesSummary(Have, false, false), typesSummary(Want, false, false)});
}

void Checker::initVars(ArrayRef<Var *> Lhs, ArrayRef<ast::Expr *> OrigRhs,
                       const Stmt *ReturnStmt) {
  const char *Context = ReturnStmt ? "return statement" : "assignment";
  size_t L = Lhs.size(), R = OrigRhs.size();
  bool IsCall = R == 1 && isa<CallExpr>(unparen(OrigRhs[0]));
  if (L == R && !IsCall) {
    for (size_t K = 0; K < L; ++K) {
      Operand X;
      ReverseDesc = Lhs[K]->Name.empty() || Lhs[K]->Name == "_"
                        ? "result variable"
                        : std::string(Lhs[K]->Name);
      exprWithHint(X, OrigRhs[K], Lhs[K]->Ty);
      ReverseDesc.clear();
      initVar(Lhs[K], X, Context);
    }
    return;
  }
  if (R != 1) {
    unsigned Before = NumErrors;
    for (Var *V : Lhs)
      if (V->Ty && !isValid(V->Ty))
        ++Before; // do not report a mismatch when the lhs is already broken
    unsigned B2 = NumErrors;
    useExprs(OrigRhs);
    bool OkRhs = NumErrors == B2;
    if (Before == NumErrors && OkRhs && !OrigRhs.empty()) {
      if (ReturnStmt) {
        SmallVector<Operand, 4> Ops;
        for (const ast::Expr *Ex : OrigRhs) {
          Operand O;
          O.E = Ex;
          O.Ty = I.typeOf(Ex);
          Ops.push_back(O);
        }
        returnError(ReturnStmt, Lhs, Ops);
      } else {
        assignError(OrigRhs, L, R);
      }
    }
    for (Var *V : Lhs)
      if (!V->Ty)
        V->Ty = invalid();
    return;
  }
  SmallVector<Operand, 4> Rhs;
  bool CommaOk = false;
  multiExpr(Rhs, OrigRhs[0], L == 2 && !ReturnStmt, &CommaOk);
  R = Rhs.size();
  if (L == R) {
    for (size_t K = 0; K < L; ++K)
      initVar(Lhs[K], Rhs[K], Context);
    return;
  }
  if (Rhs[0].M != Mode::Invalid) {
    if (ReturnStmt)
      returnError(ReturnStmt, Lhs, Rhs);
    else
      assignError(OrigRhs, L, R);
  }
  for (Var *V : Lhs) {
    V->Used = true;
    if (!V->Ty)
      V->Ty = invalid();
  }
}

void Checker::assignVars(ArrayRef<ast::Expr *> Lhs,
                         ArrayRef<ast::Expr *> OrigRhs) {
  size_t L = Lhs.size(), R = OrigRhs.size();
  bool IsCall = R == 1 && isa<CallExpr>(unparen(OrigRhs[0]));
  if (own() && L == R) {
    // OWN-R4: `p = append(p, ...)` and `b = f(b)` with a `move` parameter
    // move p implicitly: the place receives the result in the same statement.
    for (size_t K = 0; K < L; ++K)
      if (isa<CallExpr>(unparen(OrigRhs[K])))
        AppendTargets[unparen(OrigRhs[K])] = Lhs[K];
  }
  if (L == R && !IsCall) {
    // OWN-R3a: a swap (`a[i], a[j] = a[j], a[i]`) reads and writes the same
    // places, so it moves the values instead of copying them.
    bool Swap = false;
    if (own() && L > 1) {
      // Every place must be stable: `xs[f()], xs[f()] = xs[f()], xs[f()]`
      // may name four different places.
      SmallVector<std::string, 4> Ls, Rs;
      bool Stable = true;
      for (size_t K = 0; K < L; ++K) {
        Stable = Stable && samePlace(Lhs[K], Lhs[K]) &&
                 samePlace(OrigRhs[K], OrigRhs[K]);
        Ls.push_back(str(unparen(Lhs[K])));
        Rs.push_back(str(unparen(OrigRhs[K])));
      }
      std::sort(Ls.begin(), Ls.end());
      std::sort(Rs.begin(), Rs.end());
      Swap = Stable && Ls == Rs;
    }
    bool Saved = NoOwnCheck;
    NoOwnCheck = NoOwnCheck || Swap;
    for (size_t K = 0; K < L; ++K)
      assignVar(Lhs[K], OrigRhs[K], nullptr, "assignment");
    NoOwnCheck = Saved;
    return;
  }
  if (R != 1) {
    unsigned B = NumErrors;
    useLHS(Lhs);
    useExprs(OrigRhs);
    if (B == NumErrors)
      assignError(OrigRhs, L, R);
    return;
  }
  SmallVector<Operand, 4> Rhs;
  bool CommaOk = false;
  multiExpr(Rhs, OrigRhs[0], L == 2, &CommaOk);
  R = Rhs.size();
  if (L == R) {
    for (size_t K = 0; K < L; ++K)
      assignVar(Lhs[K], nullptr, &Rhs[K], "assignment");
    return;
  }
  if (Rhs[0].M != Mode::Invalid)
    assignError(OrigRhs, L, R);
  useLHS(Lhs);
}

void Checker::shortVarDecl(SourceLoc Pos, ArrayRef<ast::Expr *> Lhs,
                           ArrayRef<ast::Expr *> Rhs) {
  size_t Top = Delayed.size();
  Scope *Sc = E.Sc;
  llvm::StringSet<> Seen;
  SmallVector<Var *, 4> LhsVars(Lhs.size(), nullptr);
  SmallVector<Var *, 4> NewVars;
  SmallVector<const ast::Ident *, 4> NewIds;
  bool HasErr = false;
  for (size_t K = 0; K < Lhs.size(); ++K) {
    auto *Id = dyn_cast<ast::Ident>(Lhs[K]);
    if (!Id) {
      useLHS(Lhs[K] ? ArrayRef<const ast::Expr *>(&Lhs[K], 1)
                    : ArrayRef<const ast::Expr *>());
      err(Lhs[K], diag::err_non_name_on_left_of_define, {str(Lhs[K])});
      HasErr = true;
      continue;
    }
    StringRef N = name(Id);
    if (N != "_") {
      if (!Seen.insert(N).second) {
        err(Lhs[K], diag::custom,
            {std::string(N) + " repeated on left side of :="});
        HasErr = true;
        continue;
      }
    }
    if (Object *Alt = Sc->lookupLocal(N); Alt && N != "_") {
      recordUse(Id, Alt);
      if (auto *V = dyn_cast<Var>(Alt))
        LhsVars[K] = V;
      else {
        err(Lhs[K], diag::err_cannot_assign_to, {str(Lhs[K])});
        HasErr = true;
      }
      continue;
    }
    Var *V = newVar(N, nullptr, Id->getBegin());
    LhsVars[K] = V;
    if (N != "_") {
      NewVars.push_back(V);
      NewIds.push_back(Id);
    }
    recordDef(Id, V);
    LocalVars.push_back(V);
  }
  for (size_t K = 0; K < LhsVars.size(); ++K)
    if (!LhsVars[K]) {
      LhsVars[K] = newVar("_", nullptr, Lhs[K]->getBegin());
    }
  initVars(LhsVars, Rhs, nullptr);
  processDelayed(Top);
  if (NewVars.empty() && !HasErr) {
    err(Pos, diag::err_define_no_new_vars);
    return;
  }
  SourceLoc ScopePos = Rhs.empty() ? Pos : Rhs.back()->getEnd();
  for (size_t K = 0; K < NewVars.size(); ++K)
    declare(Sc, nullptr, NewVars[K], ScopePos);
}
