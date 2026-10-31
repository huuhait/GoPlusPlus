//===- Inline.cpp - Trivial inlining on GIR -------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Inline.h"

#include "gopp/Basic/WorkCount.h"

#include <vector>

using namespace gopp;
using namespace gopp::gir;

namespace {

thread_local uint64_t Steps = 0;

bool readsAddress(const Rvalue &R) { return R.K == Rvalue::AddrOf; }

/// Is F a trivial function (see Inline.h)?
bool isTrivial(const Function &F, const InlineOptions &Opts) {
  if (F.isDeclaration() || F.Blocks.size() != 1 || F.NoReturn)
    return false;
  const BasicBlock &BB = F.Blocks[0];
  if (BB.Cleanup || BB.Catch || BB.Term.K != Terminator::Return ||
      BB.Stmts.size() > Opts.MaxStatements)
    return false;
  for (const Statement &S : counted(Steps, BB.Stmts))
    if (S.K == Statement::Assign && readsAddress(S.RV))
      return false;
  // An owned temporary is a drop P04.09 must place in the callee (gir.md
  // section 7). The runtime call that makes one already keeps the callee
  // out; this keeps it out whatever made the value.
  for (const Local &L : counted(Steps, F.Locals))
    if (L.Owned)
      return false;
  return true;
}

/// The copy of a callee place in the caller: its local and the locals of its
/// index steps move by Base.
void remapPlace(Place &P, LocalId Base) {
  P.Local += Base;
  for (PlaceElem &E : P.Proj)
    if (E.K == PlaceElem::Index)
      E.Val += Base;
}

void remapOperand(Operand &O, LocalId Base) {
  if (O.K != Operand::Const)
    remapPlace(O.P, Base);
}

Statement remapStatement(const Statement &S, LocalId Base) {
  Statement R = S;
  switch (R.K) {
  case Statement::Assign:
    remapPlace(R.Dest, Base);
    remapOperand(R.RV.A, Base);
    remapOperand(R.RV.B, Base);
    for (Operand &O : R.RV.Ops)
      remapOperand(O, Base);
    if (R.RV.K == Rvalue::Len)
      remapPlace(R.RV.P, Base);
    break;
  case Statement::StorageLive:
  case Statement::StorageDead:
    R.L += Base;
    break;
  case Statement::Nop:
    break;
  }
  return R;
}

/// Inlines the call that ends block Bi of Caller into it.
void inlineCall(Function &Caller, size_t Bi, FuncId CalleeId,
                const Function &Callee) {
  const BasicBlock &Body = Callee.Blocks[0];
  BasicBlock &BB = Caller.Blocks[Bi];
  Terminator Call = std::move(BB.Term);
  // The inline site (P04.12): an `Inlined` scope at the call, in the call's
  // scope. The callee's own scopes are copied below it (Origin names the
  // callee's block), so a debugger shows the callee's lines inside the call
  // and the callee as an inlined frame.
  DebugScope Site;
  Site.K = DebugScope::Inlined;
  Site.Parent = Call.Scope;
  Site.Loc = Call.Loc;
  Site.Callee = CalleeId;
  ScopeId Inl = Caller.addScope(Site);
  std::vector<ScopeId> ScopeMap(Callee.Scopes.size() + 1, Inl);
  for (size_t K = 0; K < Callee.Scopes.size(); ++K) {
    ++Steps;
    DebugScope D = Callee.Scopes[K];
    D.Parent = ScopeMap[D.Parent];
    if (D.K == DebugScope::Block && !D.Origin)
      D.Origin = K + 1;
    ScopeMap[K + 1] = Caller.addScope(D);
  }
  LocalId Base = Caller.Locals.size();
  LocalId Index = 0;
  for (const Local &L : counted(Steps, Callee.Locals)) {
    Local N = L;
    // The result and the parameters become ordinary locals of the caller.
    N.K = L.K == Local::Return ? Local::Temp
                               : (L.K == Local::Arg ? Local::Var : L.K);
    N.Scope = ScopeMap[L.Scope];
    if (L.K == Local::Arg)
      N.ArgNo = Index;
    if (L.Origin == NoId)
      N.Origin = Index;
    ++Index;
    Caller.addLocal(std::move(N));
  }
  auto Assign = [&](Place Dest, Operand From, Span Loc) {
    Statement S;
    S.K = Statement::Assign;
    S.Dest = std::move(Dest);
    S.RV.K = Rvalue::Use;
    S.RV.Ty = S.Dest.Ty;
    S.RV.A = std::move(From);
    S.Loc = Loc;
    S.Scope = Call.Scope;
    S.Boundary = true;
    BB.Stmts.push_back(std::move(S));
  };
  // The arguments are copied before the body runs, as a call does.
  for (unsigned I = 0; I < Callee.numParams(); ++I) {
    ++Steps;
    const Local &P = Callee.Locals[1 + I];
    if (!P.Ty->isUnit())
      Assign(localPlace(Base + 1 + I, P.Ty), std::move(Call.Args[I]), Call.Loc);
  }
  // The callee's statements keep their own positions (a line of the callee's
  // file, which the caller's line table does not have), in the copies of the
  // callee's scopes: the code generator puts them in the callee's scope,
  // inlined at the call (P04.12). The copies of the parameters and the
  // result, and the jump back, are the call's: its position and scope.
  for (const Statement &S : counted(Steps, Body.Stmts)) {
    BB.Stmts.push_back(remapStatement(S, Base));
    BB.Stmts.back().Scope = ScopeMap[S.Scope];
  }
  if (!Callee.Locals[0].Ty->isUnit()) {
    Operand R;
    R.K = Operand::Move;
    R.P = localPlace(Base, Callee.Locals[0].Ty);
    Assign(std::move(Call.Dest), std::move(R), Call.Loc);
  }
  Terminator Go;
  Go.K = Terminator::Goto;
  Go.Target = Call.Target;
  Go.Loc = Call.Loc;
  Go.Scope = Call.Scope;
  Go.Set = true;
  BB.Term = std::move(Go);
}

} // namespace

uint64_t gir::inlineSteps() { return Steps; }

InlineStats gir::inlineTrivial(Module &M, const InlineOptions &Opts) {
  InlineStats Stats;
  std::vector<uint8_t> Trivial(M.Funcs.size(), 0);
  for (size_t I = 0; I < M.Funcs.size(); ++I) {
    ++Steps;
    Trivial[I] = isTrivial(*M.Funcs[I], Opts);
    Stats.Trivial += Trivial[I];
  }
  if (!Stats.Trivial)
    return Stats;
  for (auto &FP : M.Funcs) {
    Function &F = *FP;
    for (size_t Bi = 0; Bi < F.Blocks.size(); ++Bi) {
      ++Steps;
      const BasicBlock &BB = F.Blocks[Bi];
      const Terminator &T = BB.Term;
      if (BB.Cleanup || T.K != Terminator::Call || T.Target == NoId ||
          T.Unwind.K == UnwindAction::Cleanup || T.Discr.K != Operand::Const ||
          T.Discr.C.K != Constant::Func)
        continue;
      FuncId Callee = static_cast<FuncId>(T.Discr.C.Bits);
      if (Callee >= Trivial.size() || !Trivial[Callee])
        continue;
      inlineCall(F, Bi, Callee, *M.Funcs[Callee]);
      ++Stats.Inlined;
    }
  }
  return Stats;
}
