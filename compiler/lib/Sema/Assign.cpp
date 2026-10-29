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
    err(X, diag::err_sema_assign,
        {"cannot assign " + str(X) + " to " + (T ? str(T) : std::string("_")) +
         " in " + std::string(Context)});
    X.M = Mode::Invalid;
    return;
  }
  if (isUntyped(X.Ty)) {
    if (own() && X.isNil() && T && !isPointerKind(T))
      if (const char *Why = neverNilKind(T)) {
        err(X, diag::err_nil_never_nil, {str(T), Why});
        X.M = Mode::Invalid;
        return;
      }
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
      // ADR-0018: nil goes only into an Option and Go's nilable types.
      if (own() && X.isNil() && isPointerKind(Target)) {
        err(X, diag::err_ptr_never_nil, {str(Target)});
        X.M = Mode::Invalid;
        return;
      }
      if (own() && X.isNil() && T)
        if (const char *Why = neverNilKind(T)) {
          err(X, diag::err_nil_never_nil, {str(T), Why});
          X.M = Mode::Invalid;
          return;
        }
      std::string Msg = "cannot use " + str(X) + " as " + str(Target) +
                        " value in " + std::string(Context);
      if (Code == ReprCode::Truncated)
        Msg += " (truncated)";
      else if (Code == ReprCode::Overflow)
        Msg += " (overflows)";
      err(X, diag::err_sema_assign, {Msg});
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
      err(X, diag::err_sema_assign,
          {"cannot use generic function " + str(X.E) +
           " without instantiation in " + std::string(Context)});
      X.M = Mode::Invalid;
      return;
    }
  // P03.19: a use that only a loop's assumption narrows is the Option where
  // the Option fits the target too (unnarrowTentative).
  if (T)
    if (Type *OT = tentOption(X)) {
      Operand O = X;
      O.Ty = OT;
      if (assignableTo(O, T, nullptr))
        unnarrowTentative(X);
    }
  if (!T)
    return;
  std::string Cause;
  if (!assignableTo(X, T, &Cause)) {
    // ADR-0018 point 4: an unchecked Option used where its value is expected.
    if (own() && X.E)
      if (auto *Op = dyn_cast<OptionType>(under(X.Ty))) {
        Operand Val = X;
        Val.Ty = Op->Elem;
        if (assignableTo(Val, T, nullptr)) {
          err(X, diag::err_option_maybe_nil, {str(X.E), nilCheckHint(X.E)});
          X.M = Mode::Invalid;
          return;
        }
      }
    std::string Msg = "cannot use " + str(X) + " as " + str(T) + " value in " +
                      std::string(Context);
    if (!Cause.empty())
      Msg += ": " + Cause;
    err(X, diag::err_sema_assign, {Msg});
    X.M = Mode::Invalid;
    return;
  }
  // An `error` (which may be nil) converts to `type MyErr error`, an
  // interface with methods that is never nil, only where it is known set.
  checkNilableToNeverNil(X, T);
}

bool Checker::checkNilableToNeverNil(Operand &X, Type *T) {
  if (!own() || !X.E || X.M == Mode::Invalid || !X.Ty || !T || isTypeParam(T) ||
      !isa<InterfaceType>(under(T)) || !neverNilKind(T) || isUntyped(X.Ty) ||
      !isa<InterfaceType>(under(X.Ty)) || isTypeParam(X.Ty) ||
      neverNilKind(X.Ty) || knownNonNil(X.E, T, X.Ty))
    return false;
  err(X, diag::err_maybe_nil_to_never_nil, {str(X.E), str(T)});
  X.M = Mode::Invalid;
  return true;
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
    // P03.19: a type inferred from a use that only a loop's assumption
    // narrows is the Option's (a range over an integer needed the value).
    if (Context != "range clause")
      unnarrowTentative(X);
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
      bindsBox(Lhs->Ty)) {
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
  markLookupOk(Lhs); // `m[k] = v` writes the entry; it reads no value
  markNoNarrow(Lhs); // the variable itself is written, not its value
  const ast::Expr *SavedNoGuard = NoGuardExpr;
  NoGuardExpr = AssignReadsLhs ? nullptr : unparen(Lhs);
  expr(X, Lhs);
  NoGuardExpr = SavedNoGuard;
  // Narrowing ends at the assignment (ADR-0018 point 4), after the statement:
  // `n = n.Next` reads n first.
  if (own() && V)
    PendingKills.push_back(V);
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
        err(Lhs, diag::err_sema_assign,
            {"cannot assign to struct field " + str(Lhs) + " in map"});
        return invalid();
      }
      (void)Base;
    }
    err(Lhs, diag::err_cannot_assign, {str(Lhs)});
    return invalid();
  }
  checkContainerStore(Lhs); // P03.19: a map in a temporary, a box's contents
  return X.Ty;
}

Type *Checker::assignVar(const ast::Expr *Lhs, const ast::Expr *RhsExpr,
                         Operand *X, StringRef Context) {
  AssignReadsLhs = Context == "assignment operation";
  Type *T = lhsVar(Lhs); // nullptr for the blank identifier
  AssignReadsLhs = false;
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
  // CON-6a: `ch = nil` leaves a local channel nil (after the kill of the
  // statement, flowFlushKills).
  if (own() && T && Context == "assignment" && X->E && X->M != Mode::Invalid &&
      isNilValue(X->E))
    if (auto *Id = dyn_cast<ast::Ident>(unparen(Lhs)))
      if (auto *V = dyn_cast_or_null<Var>(I.Uses.lookup(Id));
          V && !V->IsPackageLevel && V->Pkg == S.Pkg && V->Ty &&
          isa<sema::ChanType>(under(V->Ty)))
        PendingNil.push_back(V);
  if (!T && Lhs) {
    // blank identifier (it needs no value type, P03.19)
    unnarrowTentative(*X);
    assignment(*X, nullptr, Context);
    return nullptr;
  }
  // ADR-0016 point 8: a never-nil @Required field is never set to nil.
  if (own() && X->isNil() && Lhs)
    if (const Var *F = selectedField(Lhs); F && isNeverNilField(F))
      err(*X, diag::err_required_nil_store, {str(Lhs), requiredWhy(F)});
  if (own() && !X->isNil() && Lhs && Context != "assignment operation")
    if (const Var *F = selectedField(Lhs))
      checkNeverNilStore(*X, F, str(Lhs));
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
    // Only report a mismatch error if there are no other errors on the rhs.
    if (ReturnStmt) {
      SmallVector<Operand, 4> Ops;
      exprList(OrigRhs, Ops);
      bool OkRhs = !OrigRhs.empty();
      for (Operand &O : Ops)
        OkRhs &= O.M != Mode::Invalid;
      if (OkRhs)
        returnError(ReturnStmt, Lhs, Ops);
    } else {
      unsigned B2 = NumErrors;
      useExprs(OrigRhs);
      if (NumErrors == B2 && !OrigRhs.empty())
        assignError(OrigRhs, L, R);
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
    if (own() && ReturnStmt && L >= 2)
      checkForwarded(OrigRhs[0], Rhs, Lhs.back()->Ty,
                     "the results of this function");
    if (own() && !ReturnStmt && L >= 2) {
      SmallVector<const ast::Expr *, 4> NoPlaces(L, nullptr);
      noteGuardedForm(Lhs, NoPlaces, OrigRhs[0], Rhs, CommaOk);
    }
    // ADR-0018 point 11: `v, ok := o.Get()` borrows an owning T from the
    // place o; `move(o).Get()` takes it (and leaves o nil).
    if (own() && !ReturnStmt && L == 2 && Lhs[0]->Ty && isOwning(Lhs[0]->Ty)) {
      auto *Call = dyn_cast<CallExpr>(unparen(OrigRhs[0]));
      auto *Sel = Call ? dyn_cast<SelectorExpr>(unparen(Call->Fun)) : nullptr;
      auto It = Sel ? I.Selections.find(Sel) : I.Selections.end();
      if (It != I.Selections.end())
        if (auto *F = dyn_cast_or_null<Func>(It->second.Obj);
            F && F->IsBuiltinMethod && F->Name == "Get" &&
            !isBuiltinCall(Sel->X, BuiltinId::Move) &&
            placeKind(Sel->X) != PlaceKind::Temp) {
          Lhs[0]->Mode = Var::Borrowed;
          Lhs[0]->OptionBinding = true;
          notePayloadBorrow(Lhs[0], Sel->X);
          noteBoxGet(Lhs[0], Sel->X); // TYP-9: an Option in a box
        }
    }
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
  // The places written by this statement refill what a `move` in it empties
  // (ADR-0016 point 5: a swap).
  SmallVector<const ast::Expr *, 4> SavedRefill = std::move(RefillLhs);
  RefillLhs.assign(Lhs.begin(), Lhs.end());
  SmallVector<const ast::Expr *, 4> SavedMoved = std::move(RefillMoved);
  RefillMoved.clear();
  struct RefillRestore {
    Checker &C;
    SmallVector<const ast::Expr *, 4> &Saved;
    SmallVector<const ast::Expr *, 4> &SavedMoved;
    ~RefillRestore() {
      C.RefillLhs = std::move(Saved);
      C.RefillMoved = std::move(SavedMoved);
    }
  } RefillGuard{*this, SavedRefill, SavedMoved};
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
    if (own() && L >= 2) {
      // The targets: a local variable, the blank identifier (null, no
      // place), or a place that cannot be checked later (null with a place).
      SmallVector<Var *, 4> Vs;
      SmallVector<const ast::Expr *, 4> Places;
      for (const ast::Expr *Ex : Lhs) {
        Var *V = nullptr;
        const ast::Expr *Pl = unparen(Ex);
        if (auto *Id = dyn_cast<ast::Ident>(Pl)) {
          if (name(Id) == "_")
            Pl = nullptr;
          else if (auto *O = dyn_cast_or_null<Var>(I.Uses.lookup(Id)))
            V = O;
        }
        Vs.push_back(V);
        Places.push_back(V ? nullptr : Pl);
      }
      noteGuardedForm(Vs, Places, OrigRhs[0], Rhs, CommaOk);
    }
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
        err(Lhs[K], diag::err_sema_assign,
            {std::string(N) + " repeated on left side of :="});
        HasErr = true;
        continue;
      }
    }
    if (Object *Alt = Sc->lookupLocal(N); Alt && N != "_") {
      recordUse(Id, Alt);
      if (auto *V = dyn_cast<Var>(Alt)) {
        LhsVars[K] = V;
        if (own()) { // `o, err := f()` assigns the existing o
          PendingKills.push_back(V);
          // A read-only binding (TYP-9, Get, OWN-R3b) cannot be assigned
          // this way either. After the error, forget what it borrowed.
          if (borrowedRoot(Id)) {
            checkWrite(Id, false, true);
            PayloadBorrows.erase(V);
          }
        }
      } else {
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
  // CON-6a: `ch := (chan int)(nil)` leaves a local channel nil, as `=` does.
  if (own() && !HasErr && Rhs.size() == LhsVars.size())
    for (size_t K = 0; K < LhsVars.size(); ++K) {
      Var *V = LhsVars[K];
      if (V && V->Name != "_" && !V->IsPackageLevel && V->Pkg == S.Pkg &&
          V->Ty && isa<sema::ChanType>(under(V->Ty)) && isNilValue(Rhs[K]))
        PendingNil.push_back(V);
    }
  processDelayed(Top);
  if (NewVars.empty() && !HasErr) {
    err(Pos, diag::err_define_no_new_vars);
    return;
  }
  SourceLoc ScopePos = Rhs.empty() ? Pos : Rhs.back()->getEnd();
  for (size_t K = 0; K < NewVars.size(); ++K)
    declare(Sc, nullptr, NewVars[K], ScopePos);
}
