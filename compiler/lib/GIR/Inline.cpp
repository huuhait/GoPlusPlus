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
void inlineCall(Function &Caller, size_t Bi, const Function &Callee) {
  const BasicBlock &Body = Callee.Blocks[0];
  LocalId Base = Caller.Locals.size();
  for (const Local &L : counted(Steps, Callee.Locals)) {
    Local N = L;
    // The result and the parameters become ordinary locals of the caller.
    N.K = L.K == Local::Return ? Local::Temp
                               : (L.K == Local::Arg ? Local::Var : L.K);
    Caller.addLocal(std::move(N));
  }
  BasicBlock &BB = Caller.Blocks[Bi];
  Terminator Call = std::move(BB.Term);
  auto Assign = [&](Place Dest, Operand From, Span Loc) {
    Statement S;
    S.K = Statement::Assign;
    S.Dest = std::move(Dest);
    S.RV.K = Rvalue::Use;
    S.RV.Ty = S.Dest.Ty;
    S.RV.A = std::move(From);
    S.Loc = Loc;
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
  // The callee's statements belong to the call site's position: a Span names
  // a line of the callee's file, and the caller's line table is another
  // file's. (A real `inlinedAt` scope shows the callee's lines: P04.12.)
  for (const Statement &S : counted(Steps, Body.Stmts)) {
    BB.Stmts.push_back(remapStatement(S, Base));
    BB.Stmts.back().Loc = Call.Loc;
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
      inlineCall(F, Bi, *M.Funcs[Callee]);
      ++Stats.Inlined;
    }
  }
  return Stats;
}
