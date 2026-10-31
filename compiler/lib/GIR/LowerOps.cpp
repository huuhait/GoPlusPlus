//===- LowerOps.cpp - The checked AST to GIR: operator methods ------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.23, ADR-0024, D4.19. The checker (Operators.cpp) resolves every
// operator expression and statement on a user type to ONE method and checks
// it as the ordinary call `recv.M(args)`; Info::Operators keeps that call
// (OperatorUse::Call) next to the node the source wrote. So an operator is
// lowered as the method call it is (lowerPlainCall: the receiver and the
// parameters pass as for any method, EXP-18) and this file only decides what
// the call does not say:
//
//   - the order of the operands. The call's receiver is evaluated first; a
//     `@Right` use (`2.0 * v`) has the right operand as its receiver, so the
//     left one is evaluated before it (opCall: First, recordOps' replay).
//   - a lazy `&&`/`||`: the right operand is a closure that the method calls
//     or not (lazyClosure).
//   - the tests around a call: `!=` is the negation of `==`, `<`, `<=`, `>`,
//     `>=` compare the `<=>` result with 0.
//   - the built-in `<=>` of numbers and strings (cmp.Compare, builtinCmp).
//   - the compound forms: `x op= y` calls the in-place method, or is `x = x
//     op y`, and `a[i] op= v` is one `[]` and one `[]=` call; `a`, `i` and
//     `x` are evaluated once (the replay of recordOps).
//   - a constraint element at an instance of generic code: the instance's
//     method, or for a built-in type the built-in operation (`Sum[int]` is
//     an `add`, no call).
//
// A use whose method is not found at the instance (opResolved) is a built-in
// operation: the ordinary lowering runs, with the instance's types.
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

namespace {

/// The comparison of the result of a `<=>` call with 0 that a token asks for.
bool signOp(tok::Kind K, BinOp &Op) {
  switch (K) {
  case tok::lss:
    Op = BinOp::Lt;
    return true;
  case tok::leq:
    Op = BinOp::Le;
    return true;
  case tok::gtr:
    Op = BinOp::Gt;
    return true;
  case tok::geq:
    Op = BinOp::Ge;
    return true;
  default:
    return false;
  }
}

} // namespace

//===----------------------------------------------------------------------===//
// The call
//===----------------------------------------------------------------------===//

bool Body::opResolved(const S::OperatorUse &U) {
  if (!U.Call)
    return false;
  auto *Sel = dyn_cast<ast::SelectorExpr>(U.Call->Fun);
  return Sel && selOf(Sel);
}

void Body::saveOp(const ast::Expr *E, LateOps &R) {
  if (!E || isConst(E) || isNilValue(E) || R.Saved.count(E))
    return;
  Operand V = lowerExpr(E);
  if (V.type())
    R.Saved[E] = localOf(std::move(V));
}

Operand Body::opCall(const ast::CallExpr *Call, const S::OperatorUse &U,
                     const ast::Expr *First, const ast::Node *At) {
  ++LowerSteps;
  auto *Sel = cast<ast::SelectorExpr>(Call->Fun);
  // The operands that the call does not evaluate in the source order or as
  // they are: kept here, and replayed by the lowering of the call.
  LateOps Pre;
  // `2.0 * v` with a `@Right` method: the call is `v.*(2.0)`, whose receiver
  // is evaluated first; the left operand (the argument) goes before it.
  if (U.Swapped && First)
    recordOps(First, Pre);
  // A view operand is read out (a pointer-free Copy value, OWN-V1): the
  // receiver is what the view looks at. (An argument is read by valueFor.)
  if (S::Type *RT = sTyOf(Sel->X); RT && !isConst(Sel->X))
    if (auto *RV = dyn_cast<S::RefType>(S::under(RT)); RV && !RV->Borrowed) {
      Operand V = lowerExpr(Sel->X);
      if (!V.type())
        return failed(Sel->X, zeroOfExpr(Call));
      Operand Val = viewValue(std::move(V), RT, Sel->X);
      if (!Val.type())
        return failed(Sel->X, zeroOfExpr(Call));
      Pre.Saved[Sel->X] = localOf(std::move(Val));
    }
  // The lazy right operand is a closure over what it uses.
  const ast::Expr *LazyArg = nullptr;
  if (U.Lazy && !Call->Args.empty()) {
    LazyArg = Call->Args[0];
    const S::Selection *SL = selOf(Sel);
    auto *F = SL ? dyn_cast_or_null<S::Func>(SL->Obj) : nullptr;
    if (!F)
      return failed(Call, zeroOfExpr(Call));
    S::SignatureType *Sig = sigOf(F);
    if (Sig->Params->Elems.empty())
      return failed(Call, zeroOfExpr(Call));
    Operand Clo = lazyClosure(LazyArg, Sig->Params->Elems[0]);
    if (!Clo.type())
      return failed(LazyArg, zeroOfExpr(Call));
    Ready[LazyArg] = std::move(Clo);
  }
  at(At);
  Operand R;
  if (Pre.Saved.empty()) {
    R = lowerPlainCall(Call);
  } else {
    // (The replay of an enclosing statement stays visible, one level: the
    // operands of this very call. Copying it would be quadratic in a chain.)
    Pre.Outer = Replay;
    if (L.Opts.CopyEnclosingReplays && Replay)
      for (const auto &KV : Replay->Saved) {
        ++LowerSteps;
        Pre.Saved.insert(KV);
      }
    ReplayScope Scope(*this, Pre);
    R = lowerPlainCall(Call);
  }
  if (LazyArg)
    Ready.erase(LazyArg);
  return R;
}

//===----------------------------------------------------------------------===//
// Expressions
//===----------------------------------------------------------------------===//

bool Body::lowerOperatorExpr(const ast::Expr *E, Operand &Out) {
  const S::OperatorUse *U = opUse(E);
  if (!U)
    return false;
  if (auto *B = dyn_cast<ast::BinaryExpr>(E)) {
    if (!U->Call) {
      // The built-in `<=>` (EXP-20).
      Out = builtinCmp(B->X, B->Y, sTyOf(B), B);
      return true;
    }
    Operand R;
    if (!opResolved(*U)) {
      // A constraint element at an instance whose type has no such method:
      // `<=>` is the built-in one, and so are `< <= > >=` through it (GEN-13:
      // `a < b` is `(a <=> b) < 0`, so a NaN follows cmp.Compare); the other
      // operators run as written.
      if (U->Op != S::OpKind::Cmp)
        return false;
      R = builtinCmp(B->X, B->Y,
                     U->Sign == tok::illegal
                         ? sTyOf(B)
                         : sigOf(U->Method)->Results->Elems[0],
                     B);
      if (!R.type() || U->Sign == tok::illegal) {
        Out = std::move(R);
        return true;
      }
    } else {
      R = opCall(U->Call, *U, U->Swapped ? B->X : nullptr, B);
    }
    if (!R.type()) {
      Out = R;
      return true;
    }
    BinOp Op;
    if (U->Negate) {
      at(B);
      R = copyOf(FB.temp(FB.unary(UnOp::Not, std::move(R))));
    } else if (U->Sign != tok::illegal && signOp(U->Sign, Op)) {
      at(B);
      Type *IT = R.type();
      R = copyOf(FB.temp(FB.binary(Op, std::move(R), constInt(IT, 0))));
    }
    Out = std::move(R);
    return true;
  }
  if (!U->Call)
    return false;
  if (!opResolved(*U))
    return false;
  // A unary operator, `*a`, `a[i]`, `a(x)`, `R(a)`, `<-q`: the call.
  Out = opCall(U->Call, *U, nullptr, E);
  return true;
}

Operand Body::builtinCmp(const ast::Expr *X, const ast::Expr *Y, S::Type *ResS,
                         const ast::Node *N) {
  // cmp.Compare (EXP-20): -1, 0 or 1; for floats a NaN is less than every
  // other value and equal to a NaN. The operands have the type of the one
  // that is not a constant.
  Type *Res = L.lowerType(ResS, N->getBegin());
  if (!Res || !Res->isInt())
    return failed(N, constInt(ctx().i64(), 0));
  S::Type *OpS = isConst(X) ? sTyOf(Y) : sTyOf(X);
  if (isConst(X) && isConst(Y)) {
    // `3 <=> 4`: the constants take their default types.
    OpS = sTyOf(X);
  }
  if (auto *Rf = dyn_cast_or_null<S::RefType>(OpS ? S::under(OpS) : nullptr))
    OpS = Rf->Elem;
  Operand XV = valueFor(X, OpS), YV = valueFor(Y, OpS);
  Type *T = XV.type();
  if (!T || !YV.type())
    return failed(N, constInt(Res, 0));
  at(N);
  Rt E(M, FB);
  Val A = E.snap(Val{&E, std::move(XV)}), B = E.snap(Val{&E, std::move(YV)});
  Var R = E.var(Res, "cmp");
  auto Num = [&](int64_t K) { return E.k(Res, uint64_t(K)); };
  if (T->getKind() == Type::Kind::String) {
    Place PA = placeOf(A.Op), PB = placeOf(B.Op);
    Operand C = callRt(RtFn::StrCmp,
                       {copyOf(FB.field(PA, 0)), copyOf(FB.field(PA, 1)),
                        copyOf(FB.field(PB, 0)), copyOf(FB.field(PB, 1))},
                       N);
    return E.to(Res, Val{&E, C}).Op;
  }
  if (!T->isInt() && !T->isFloat()) {
    unsupported(N, std::string("<=> on ") + T->str());
    return constInt(Res, 0);
  }
  R = Num(0);
  E.IfElse(
      A > B, [&] { R = Num(1); },
      [&] {
        E.IfElse(
            A < B, [&] { R = Num(-1); },
            [&] {
              if (!T->isFloat())
                return;
              // Neither greater, less nor equal: a NaN. Both: equal (0).
              // Only the left: less (-1). Only the right: greater (1).
              E.If(E.not_(A == B), [&] {
                E.IfElse(
                    A != A,
                    [&] {
                      R = Num(0);
                      E.If(B == B, [&] { R = Num(-1); });
                    },
                    [&] { R = Num(1); });
              });
            });
      });
  return copyOf(FB.local(R.L));
}

//===----------------------------------------------------------------------===//
// The right operand of a lazy `&&` or `||`
//===----------------------------------------------------------------------===//

Operand Body::lazyClosure(const ast::Expr *R, S::Type *FT) {
  Type *VT = L.lowerType(FT, R->getBegin());
  auto *Sig = dyn_cast_or_null<S::SignatureType>(FT ? S::under(FT) : nullptr);
  auto FIt =
      Facts ? Facts->Lits.find(R) : decltype(Facts->Lits)::const_iterator();
  if (!VT || !Sig || !Facts || FIt == Facts->Lits.end() || !VT->isFuncValue() ||
      Sig->Results->size() != 1) {
    if (VT)
      unsupported(R, "this lazy operator");
    return errorValue(ctx());
  }
  const LitFacts &LF = FIt->second;
  if (LF.Method || isa<ast::FuncLit>(R)) {
    unsupported(R, "a function value as the right operand of a lazy `&&` or "
                   "`||` (P04.27)");
    return errorValue(ctx());
  }
  if (!ScopeNode) {
    unsupported(R, "a lazy operator in the initializer of a blank variable "
                   "(P04.27)");
    return errorValue(ctx());
  }
  // What it may hold, as for a literal that does not escape (CLO-3a).
  for (const S::Var *V : LF.Captures) {
    ++EscapeSteps;
    if (!mayHold(V, LF.Escapes, R))
      return errorValue(ctx());
  }
  const sym::Node *CN = L.Syms.Ctx.closure(ScopeNode, LF.Ordinal);
  return makeClosure(
      R, LF, VT, CN, [&](Body &B) { B.lowerLazy(R, LF, Sig); }, FT);
}

void Body::lowerLazy(const ast::Expr *R, const LitFacts &LF,
                     S::SignatureType *Sig) {
  Function &F = FB.function();
  at(R);
  pushScope(false);
  FnSig = Sig;
  if (Sig->Params->Elems.size() + 1 != F.numParams()) {
    unsupported(R, "this lazy operator");
    return;
  }
  if (!bindCaptures(LF, R))
    return;
  S::Type *Res = Sig->Results->Elems[0];
  ResultTys.push_back(Res);
  Operand V = valueFor(R, Res);
  at(R);
  if (!F.Sig->ret()->isUnit())
    FB.assign(FB.local(FB.returnPlace()), FB.use(std::move(V)));
  exitScopes(0);
  FB.ret();
  Scopes.pop_back();
  finish();
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

void Body::lowerOpCallStmt(const ast::Stmt *St, const S::OperatorUse &U) {
  at(St);
  if (!U.Call)
    return;
  opCall(U.Call, U, nullptr, St);
}

void Body::lowerOpAssignStmt(const ast::AssignStmt *A,
                             const S::OperatorUse &U) {
  if (U.IndexSet) {
    lowerOpIndexCompound(A, U);
    return;
  }
  if (!U.Call) {
    unsupported(A, "this assignment");
    return;
  }
  if (U.InPlace) {
    // The method of a pointer receiver changes the place itself: no
    // allocation, and `x` is evaluated once (it is the receiver).
    at(A);
    opCall(U.Call, U, nullptr, A);
    return;
  }
  // `x = x op y`: x is evaluated once. Its operands are recorded first (Go's
  // phase 1), the call reads x under their replay, and the store finds the
  // same place.
  Dest D = recordDest(A->Lhs[0]);
  if (D.Blank)
    return;
  Operand R;
  {
    ReplayScope Scope(*this, D.Ops);
    R = opCall(U.Call, U, U.Swapped ? A->Lhs[0] : nullptr, A);
  }
  if (!R.type())
    return;
  store(D, std::move(R), A);
}

void Body::lowerOpIndexCompound(const ast::AssignStmt *A,
                                const S::OperatorUse &U) {
  auto *Ix = dyn_cast<ast::IndexExpr>(unparen(A->Lhs[0]));
  if (!Ix || !U.SetCall || !U.IndexSet || U.SetCall->Args.empty()) {
    unsupported(A, "this assignment");
    return;
  }
  // `a[i, j] op= v` is one `[]` and one `[]=` call, with `a` and the indexes
  // evaluated once (EXP-22): they are recorded now, and both calls replay
  // them.
  LateOps Ops;
  recordOps(Ix->X, Ops);
  for (const ast::Expr *Idx : Ix->Indices)
    saveOp(Idx, Ops);
  ReplayScope Scope(*this, Ops);
  at(A);
  Operand Mid;
  S::Type *MidS = nullptr;
  if (U.Call) {
    // The element's operator is a method: the `[]` read is its receiver (or,
    // `@Right`, its argument).
    Mid = opCall(U.Call, U, U.Swapped ? Ix : nullptr, A);
    if (const S::Selection *SL = selOf(cast<ast::SelectorExpr>(U.Call->Fun)))
      if (auto *F = dyn_cast_or_null<S::Func>(SL->Obj))
        if (sigOf(F)->Results->size() == 1)
          MidS = sigOf(F)->Results->Elems[0];
  } else {
    // The element's operator is built in (`m[1, 2] += 1.0` on a float64
    // element): the read, the operation, then the write.
    BinOp Op;
    if (!compoundOp(A->Op, Op)) {
      unsupported(A, "this assignment");
      return;
    }
    // (The type of `a[i]` is the result of its `[]` method; the checker
    // keeps no type for the index expression of this form.)
    if (U.Index && sigOf(U.Index)->Results->size() == 1)
      MidS = sigOf(U.Index)->Results->Elems[0];
    Type *G = L.lowerType(MidS, A->getBegin());
    if (!G)
      return;
    const ast::Expr *Rhs = A->Rhs[0];
    Operand X = lowerExpr(Ix);
    Operand Y = isShift(Op) ? lowerExpr(Rhs) : lowerAs(Rhs, G);
    Mid = arith(Op, std::move(X), std::move(Y), MidS, isConst(Rhs), A);
  }
  if (!Mid.type())
    return;
  // The write: the value goes in as the last argument.
  const S::Selection *SL = selOf(cast<ast::SelectorExpr>(U.SetCall->Fun));
  auto *SF = SL ? dyn_cast_or_null<S::Func>(SL->Obj) : nullptr;
  if (!SF) {
    unsupported(A, "this assignment");
    return;
  }
  S::SignatureType *SSig = sigOf(SF);
  S::Type *To = SSig->Params->Elems.back();
  Mid = convertResult(std::move(Mid), MidS, To, A);
  Ready[U.SetCall->Args.back()] = std::move(Mid);
  at(A);
  opCall(U.SetCall, U, nullptr, A);
  Ready.erase(U.SetCall->Args.back());
}

void Body::opSetStore(Dest &D, Operand V, const ast::Node *N) {
  const S::OperatorUse &U = *D.OpSet;
  ReplayScope Scope(*this, D.Ops);
  Ready[U.Call->Args.back()] = std::move(V);
  at(N);
  opCall(U.Call, U, nullptr, N);
  Ready.erase(U.Call->Args.back());
}

bool Body::opSwitchCase(const ast::Expr *Tag, LocalId TagLocal,
                        const ast::Expr *E, Operand &Cond) {
  auto UIt = I.CaseOperators.find(E);
  if (UIt == I.CaseOperators.end() || !opResolved(UIt->second))
    return false;
  const S::OperatorUse *U = &UIt->second;
  // The call `tag == value` (or, swapped, `value == tag`) names the tag by
  // the expression it was written with: it is already in a local.
  LateOps Pre;
  auto *Sel = cast<ast::SelectorExpr>(U->Call->Fun);
  Pre.Saved[U->Swapped ? U->Call->Args[0] : Sel->X] = TagLocal;
  (void)Tag;
  Pre.Outer = Replay;
  ReplayScope Scope(*this, Pre);
  Cond = opCall(U->Call, *U, nullptr, E);
  return true;
}
