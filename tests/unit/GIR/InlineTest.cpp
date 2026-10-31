//===- InlineTest.cpp - Trivial inlining on GIR ---------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.13. The one GIR pass of the opt-0 pipeline (Inline.h): what it inlines
// and, as important, what it leaves alone (both sides); that the module stays
// valid; that a program computes the same before and after (run in LLJIT, the
// call's destination aliasing an argument included); and that the pass is
// near-linear, counted as work (M8) with a control that must fail the check.
//
//===----------------------------------------------------------------------===//

#include "../Support/GIRTestSupport.h"

#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Inline.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::test;

namespace {

/// A module with no function that cannot be lowered (GoodModule has a
/// cleanup block), for the tests that run code.
struct Mini {
  Context C;
  Module M{C, "mini"};
  Type *I64 = C.i64(), *Bool = C.boolean();
  Function &fn(FuncId Id) { return M.func(Id); }
};

/// add3(a, b) = a*3 - b: a trivial function.
template <class GM> FuncId makeAdd3(GM &G, const char *Name = "add3") {
  FuncId F = declare(G.M, Name, G.C.func({G.I64, G.I64}, G.I64));
  FuncBuilder B(G.M, G.fn(F));
  Place T =
      B.temp(B.binary(BinOp::Mul, copyOf(B.local(1)), constInt(G.I64, 3)));
  Place U = B.temp(B.binary(BinOp::Sub, copyOf(T), copyOf(B.local(2))));
  B.assign(B.local(0), B.use(copyOf(U)));
  B.ret();
  return F;
}

/// caller(x, y) = add3(x, y) * 10 + add3(y, x)
template <class GM> FuncId makeCaller(GM &G, FuncId Callee) {
  FuncId F = declare(G.M, "caller", G.C.func({G.I64, G.I64}, G.I64));
  FuncBuilder B(G.M, G.fn(F));
  LocalId R1 = B.newTemp(G.I64), R2 = B.newTemp(G.I64);
  B.call(constFunc(G.M, Callee), {copyOf(B.local(1)), copyOf(B.local(2))},
         B.local(R1));
  B.call(constFunc(G.M, Callee), {copyOf(B.local(2)), copyOf(B.local(1))},
         B.local(R2));
  Place M =
      B.temp(B.binary(BinOp::Mul, copyOf(B.local(R1)), constInt(G.I64, 10)));
  B.assign(B.local(0), B.binary(BinOp::Add, copyOf(M), copyOf(B.local(R2))));
  B.ret();
  return F;
}

unsigned callsIn(const Function &F) {
  unsigned N = 0;
  for (const BasicBlock &BB : F.Blocks)
    N += BB.Term.K == Terminator::Call;
  return N;
}

struct Jit {
  std::unique_ptr<llvm::orc::LLJIT> J;
  explicit Jit(const Module &M) {
    initializeTargets();
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto Spec = hostTarget();
    if (!Spec) {
      llvm::consumeError(Spec.takeError());
      return;
    }
    auto TM = createTargetMachine(*Spec);
    if (!TM) {
      llvm::consumeError(TM.takeError());
      return;
    }
    auto Ctx = std::make_unique<llvm::LLVMContext>();
    auto LM = lowerToLLVM(*Ctx, M, **TM);
    EXPECT_TRUE(static_cast<bool>(LM)) << llvm::toString(LM.takeError());
    if (!LM)
      return;
    auto Made = llvm::orc::LLJITBuilder().create();
    if (!Made) {
      llvm::consumeError(Made.takeError());
      return;
    }
    J = std::move(*Made);
    llvm::Error E = J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(*LM), std::move(Ctx)));
    if (E) {
      llvm::consumeError(std::move(E));
      J.reset();
    }
  }
  template <class Fn> Fn find(const char *Name) {
    auto Addr = J->lookup(Name);
    EXPECT_TRUE(static_cast<bool>(Addr)) << llvm::toString(Addr.takeError());
    return Addr ? Addr->toPtr<Fn>() : nullptr;
  }
};

TEST(GIRInline, InlinesATrivialCallee) {
  GoodModule G;
  FuncId Callee = makeAdd3(G);
  FuncId Caller = makeCaller(G, Callee);
  ASSERT_EQ(callsIn(G.fn(Caller)), 2u);
  InlineStats S = inlineTrivial(G.M);
  EXPECT_EQ(S.Inlined, 2u);
  EXPECT_GE(S.Trivial, 1u);
  EXPECT_EQ(callsIn(G.fn(Caller)), 0u);
  EXPECT_GIR_VALID(G.M);
  // The callee stays (it may be called through a pointer or from outside).
  EXPECT_FALSE(G.fn(Callee).isDeclaration());
  // A second run finds nothing to do.
  EXPECT_EQ(inlineTrivial(G.M).Inlined, 0u);
}

TEST(GIRInline, ParameterAndResultCopiesAreBoundaries) {
  GoodModule G;
  FuncId Callee = makeAdd3(G);
  FuncId Caller = makeCaller(G, Callee);
  inlineTrivial(G.M);
  // Every copy the pass made is marked, no statement of the callee is.
  unsigned Marked = 0, Plain = 0;
  for (const BasicBlock &BB : G.fn(Caller).Blocks)
    for (const Statement &S : BB.Stmts)
      (S.Boundary ? Marked : Plain)++;
  EXPECT_EQ(Marked, 2u * (2 + 1)); // two parameter copies and a result copy
  EXPECT_GT(Plain, 0u);
  for (const Statement &S : G.fn(Callee).Blocks[0].Stmts)
    EXPECT_FALSE(S.Boundary);
}

TEST(GIRInline, SitesKeepTheCalleesPositionsAndNameTheCall) {
  // P04.12: each inlined call is an inline site (an `Inlined` scope at the
  // call, in the call's scope); the callee's statements keep their positions
  // in the callee and lie in the site, its locals are copies that name their
  // origin; the copies of the arguments and the result are the call's.
  GoodModule G;
  uint32_t CalleeFile = G.M.addFile("callee.gopp") + 1;
  uint32_t CallerFile = G.M.addFile("caller.gopp") + 1;
  FuncId Callee = makeAdd3(G);
  G.fn(Callee).Loc = {CalleeFile, 10, 1};
  G.fn(Callee).Locals[1].Name = "a";
  G.fn(Callee).Locals[1].Source = true;
  unsigned Line = 11;
  for (Statement &S : G.fn(Callee).Blocks[0].Stmts)
    S.Loc = {CalleeFile, Line++, 3};
  FuncId Caller = makeCaller(G, Callee);
  G.fn(Caller).Loc = {CallerFile, 1, 1};
  unsigned CallLine = 4;
  for (BasicBlock &BB : G.fn(Caller).Blocks)
    if (BB.Term.K == Terminator::Call)
      BB.Term.Loc = {CallerFile, CallLine += 2, 9};
  size_t CalleeLocals = G.fn(Callee).Locals.size();
  size_t CallerLocals = G.fn(Caller).Locals.size();
  inlineTrivial(G.M);
  EXPECT_GIR_VALID(G.M);
  const Function &F = G.fn(Caller);
  ASSERT_EQ(F.Scopes.size(), 2u); // the callee has no block of its own
  for (size_t I = 0; I < 2; ++I) {
    const DebugScope &Site = F.Scopes[I];
    EXPECT_EQ(Site.K, DebugScope::Inlined);
    EXPECT_EQ(Site.Callee, Callee);
    EXPECT_EQ(Site.Parent, 0u);
    EXPECT_EQ(Site.Loc, (Span{CallerFile, unsigned(6 + 2 * I), 9}));
    // The callee's locals are copied behind each other, in the site.
    LocalId First = CallerLocals + I * CalleeLocals;
    for (size_t L = 0; L < CalleeLocals; ++L) {
      const Local &Copy = F.Locals[First + L];
      EXPECT_EQ(Copy.Scope, I + 1);
      EXPECT_EQ(Copy.Origin, L);
      EXPECT_EQ(Copy.Name, G.fn(Callee).Locals[L].Name);
    }
    // The parameter became a variable of the caller and stays a source one,
    // with its position among the parameters.
    EXPECT_EQ(F.Locals[First + 1].K, Local::Var);
    EXPECT_TRUE(F.Locals[First + 1].Source);
    EXPECT_EQ(F.Locals[First + 1].ArgNo, 1u);
    EXPECT_NE(F.Locals[First].K, Local::Return);
    EXPECT_EQ(F.Locals[First].ArgNo, 0u);
  }
  for (LocalId L = 0; L < CallerLocals; ++L) {
    EXPECT_EQ(F.Locals[L].Origin, NoId);
    EXPECT_EQ(F.Locals[L].Scope, 0u);
  }
  unsigned FromCallee = 0, FromCall = 0;
  for (const BasicBlock &BB : F.Blocks) {
    EXPECT_EQ(BB.Term.Scope, 0u);
    for (const Statement &S : BB.Stmts) {
      if (S.Scope == 0 && S.Boundary) {
        // A copy of an argument or of the result: the call's position.
        EXPECT_EQ(S.Loc.File, CallerFile);
        ++FromCall;
      } else if (S.Scope != 0) {
        EXPECT_FALSE(S.Boundary);
        EXPECT_EQ(S.Loc.File, CalleeFile);
        EXPECT_GE(S.Loc.Line, 11u);
        ++FromCallee;
      }
    }
  }
  EXPECT_EQ(FromCallee, 2u * 3); // add3 has three statements
  EXPECT_EQ(FromCall, 2u * 3);
  // The callee itself is untouched.
  EXPECT_TRUE(G.fn(Callee).Scopes.empty());
  for (const Statement &S : G.fn(Callee).Blocks[0].Stmts)
    EXPECT_EQ(S.Scope, 0u);
  for (const Local &L : G.fn(Callee).Locals)
    EXPECT_EQ(L.Origin, NoId);
}

TEST(GIRInline, LeavesWhatIsNotTrivial) {
  // A declaration, a callee with a call, an assertion, a second block, an
  // address-of, an owned temporary, and one over the statement limit: none
  // is inlined.
  GoodModule G;
  FuncId Caller = declare(G.M, "caller", G.C.func({G.I64}, G.I64));
  std::vector<FuncId> Callees;
  Callees.push_back(G.Ext); // a declaration
  {
    FuncId F = declare(G.M, "withCall", G.C.func({G.I64}, G.I64));
    FuncBuilder B(G.M, G.fn(F));
    B.call(constFunc(G.M, G.Ext), {copyOf(B.local(1))}, B.local(0));
    B.ret();
    Callees.push_back(F);
  }
  {
    FuncId F = declare(G.M, "withAssert", G.C.func({G.I64}, G.I64));
    FuncBuilder B(G.M, G.fn(F));
    Place C =
        B.temp(B.binary(BinOp::Ne, copyOf(B.local(1)), constInt(G.I64, 0)));
    B.assertThat(copyOf(C), true, AssertKind::DivByZero);
    B.assign(B.local(0),
             B.binary(BinOp::Div, constInt(G.I64, 100), copyOf(B.local(1))));
    B.ret();
    Callees.push_back(F);
  }
  {
    FuncId F = declare(G.M, "withBranch", G.C.func({G.Bool}, G.I64));
    (void)F;
  }
  {
    FuncId F = declare(G.M, "withAddr", G.C.func({G.I64}, G.I64));
    FuncBuilder B(G.M, G.fn(F));
    Place P = B.temp(B.addrOf(B.local(1)));
    B.assign(B.local(0), B.use(copyOf(B.deref(P))));
    B.ret();
    Callees.push_back(F);
  }
  {
    // An owned temporary (P04.05; a drop of P04.09), even with no call.
    FuncId F = declare(G.M, "withOwned", G.C.func({G.I64}, G.I64));
    FuncBuilder B(G.M, G.fn(F));
    LocalId T = B.newTemp(G.C.string());
    G.fn(F).Locals[T].Owned = true;
    B.assign(B.local(T), B.use(constStr(G.M, "s")));
    B.assign(B.local(0), B.use(copyOf(B.local(1))));
    B.ret();
    Callees.push_back(F);
  }
  {
    FuncId F = declare(G.M, "tooLong", G.C.func({G.I64}, G.I64));
    FuncBuilder B(G.M, G.fn(F));
    for (unsigned I = 0; I < InlineOptions().MaxStatements + 1; ++I)
      B.assign(B.local(0),
               B.binary(BinOp::Add, copyOf(B.local(1)), constInt(G.I64, I)));
    B.ret();
    Callees.push_back(F);
  }
  {
    FuncBuilder B(G.M, G.fn(Caller));
    for (FuncId C : Callees) {
      const Type *Sig = G.fn(C).Sig;
      if (Sig->elems().size() != 1 || Sig->elems()[0] != G.I64)
        continue;
      B.call(constFunc(G.M, C), {copyOf(B.local(1))}, B.local(B.newTemp(G.I64)),
             {UnwindAction::Continue, NoId});
    }
    B.ret();
  }
  unsigned Before = callsIn(G.fn(Caller));
  ASSERT_GE(Before, 6u);
  InlineStats S = inlineTrivial(G.M);
  EXPECT_EQ(S.Inlined, 0u);
  EXPECT_EQ(callsIn(G.fn(Caller)), Before);
  EXPECT_GIR_VALID(G.M);
}

TEST(GIRInline, TheInlineHintsDecideBothSides) {
  // P04.23: @NoInline keeps a trivial callee a call, @Inline inlines one over
  // the statement limit, and the same bodies without a mark behave as before.
  // Returns the calls left in the caller of both.
  auto Run = [](Function::InlineHint ShortHint, Function::InlineHint LongHint) {
    GoodModule G;
    FuncId Short = makeAdd3(G, "short");
    FuncId Long = declare(G.M, "long", G.C.func({G.I64}, G.I64));
    {
      FuncBuilder B(G.M, G.fn(Long));
      for (unsigned I = 0; I < InlineOptions().MaxStatements + 5; ++I)
        B.assign(B.local(0),
                 B.binary(BinOp::Add, copyOf(B.local(1)), constInt(G.I64, I)));
      B.ret();
    }
    G.fn(Short).Inline = ShortHint;
    G.fn(Long).Inline = LongHint;
    FuncId Caller = declare(G.M, "caller2", G.C.func({G.I64}, G.I64));
    {
      FuncBuilder B(G.M, G.fn(Caller));
      B.call(constFunc(G.M, Short), {copyOf(B.local(1)), copyOf(B.local(1))},
             B.local(B.newTemp(G.I64)), {UnwindAction::Continue, NoId});
      B.call(constFunc(G.M, Long), {copyOf(B.local(1))},
             B.local(B.newTemp(G.I64)), {UnwindAction::Continue, NoId});
      B.ret();
    }
    inlineTrivial(G.M);
    EXPECT_GIR_VALID(G.M);
    return callsIn(G.fn(Caller));
  };
  EXPECT_EQ(Run(Function::InlineHint::None, Function::InlineHint::None), 1u);
  // ^ the trivial one is inlined, the long one stays
  EXPECT_EQ(Run(Function::InlineHint::Never, Function::InlineHint::None), 2u);
  // ^ @NoInline: both stay
  EXPECT_EQ(Run(Function::InlineHint::None, Function::InlineHint::Always), 0u);
  // ^ @Inline: the long one is inlined too
  EXPECT_EQ(Run(Function::InlineHint::Never, Function::InlineHint::Always), 1u);
  // ^ @NoInline stays a call, @Inline is inlined
}

TEST(GIRInline, LeavesACallWhoseUnwindEdgeIsACleanupBlock) {
  // Inlining would leave the cleanup block without an edge into it (V022).
  GoodModule G;
  FuncId Callee = makeAdd3(G, "add3");
  FuncId F = declare(G.M, "withCleanup", G.C.func({G.I64}, G.Unit));
  {
    FuncBuilder B(G.M, G.fn(F));
    LocalId R = B.newTemp(G.I64);
    BlockId Cl = B.newBlock(/*Cleanup=*/true);
    B.call(constFunc(G.M, Callee), {copyOf(B.local(1)), copyOf(B.local(1))},
           B.local(R), {UnwindAction::Cleanup, Cl});
    B.ret();
    B.setBlock(Cl);
    B.resume();
  }
  EXPECT_EQ(inlineTrivial(G.M).Inlined, 0u);
  EXPECT_EQ(callsIn(G.fn(F)), 1u);
  EXPECT_GIR_VALID(G.M);
}

TEST(GIRInline, SameResultsBeforeAndAfter) {
  // add3(x, y)*10 + add3(y, x), then `x = add3(x, x)` (the destination is an
  // argument: the arguments are copied before the result is written).
  auto Build = [](Mini &G) {
    FuncId Callee = makeAdd3(G);
    makeCaller(G, Callee);
    FuncId F = declare(G.M, "alias", G.C.func({G.I64}, G.I64));
    FuncBuilder B(G.M, G.fn(F));
    B.call(constFunc(G.M, Callee), {copyOf(B.local(1)), copyOf(B.local(1))},
           B.local(1));
    B.call(constFunc(G.M, Callee), {copyOf(B.local(1)), constInt(G.I64, 5)},
           B.local(0));
    B.ret();
  };
  Mini Plain, Inlined;
  Build(Plain);
  Build(Inlined);
  ASSERT_GT(inlineTrivial(Inlined.M).Inlined, 0u);
  EXPECT_GIR_VALID(Inlined.M);
  Jit A(Plain.M), B(Inlined.M);
  if (!A.J || !B.J)
    GTEST_SKIP() << "the host is not a supported target";
  // (Go++ functions: the Go++ convention of the host, GOPP_GO_CC.)
  using Fn2 = int64_t(GOPP_GO_CC *)(int64_t, int64_t);
  using Fn1 = int64_t(GOPP_GO_CC *)(int64_t);
  auto CallerA = A.find<Fn2>("caller"), CallerB = B.find<Fn2>("caller");
  auto AliasA = A.find<Fn1>("alias"), AliasB = B.find<Fn1>("alias");
  ASSERT_TRUE(CallerA && CallerB && AliasA && AliasB);
  for (int64_t X : {int64_t(0), int64_t(1), int64_t(-7), int64_t(123456789),
                    INT64_MAX, INT64_MIN})
    for (int64_t Y :
         {int64_t(0), int64_t(3), int64_t(-9), INT64_MIN, INT64_MAX}) {
      EXPECT_EQ(CallerA(X, Y), CallerB(X, Y)) << X << ' ' << Y;
    }
  for (int64_t X : {int64_t(0), int64_t(1), int64_t(-7), int64_t(123456789),
                    INT64_MAX, INT64_MIN})
    EXPECT_EQ(AliasA(X), AliasB(X)) << X;
  EXPECT_EQ(CallerB(4, 5), (4 * 3 - 5) * 10 + (5 * 3 - 4));
}

//===----------------------------------------------------------------------===//
// Work: near-linear (M8), with a control
//===----------------------------------------------------------------------===//

::testing::AssertionResult stepsAreLinear(uint64_t S1, uint64_t S4,
                                          uint64_t N) {
  const uint64_t C = 24;
  if (S1 == 0 || S1 > C * N || S4 > C * 4 * N || 2 * S4 > 9 * S1)
    return ::testing::AssertionFailure()
           << "steps(N) = " << S1 << ", steps(4N) = " << S4 << ", N = " << N;
  return ::testing::AssertionSuccess();
}

/// One caller with N calls of one trivial callee.
void manyCalls(GoodModule &G, unsigned N) {
  FuncId Callee = makeAdd3(G);
  FuncId F = declare(G.M, "many", G.C.func({G.I64}, G.I64));
  FuncBuilder B(G.M, G.fn(F));
  LocalId R = B.newTemp(G.I64);
  for (unsigned I = 0; I < N; ++I)
    B.call(constFunc(G.M, Callee), {copyOf(B.local(1)), constInt(G.I64, I)},
           B.local(R));
  B.assign(B.local(0), B.use(copyOf(B.local(R))));
  B.ret();
}

/// N callers with one call each.
void manyCallers(GoodModule &G, unsigned N) {
  FuncId Callee = makeAdd3(G);
  for (unsigned I = 0; I < N; ++I) {
    FuncId F = declare(G.M, "c" + std::to_string(I), G.C.func({G.I64}, G.I64));
    FuncBuilder B(G.M, G.fn(F));
    B.call(constFunc(G.M, Callee), {copyOf(B.local(1)), constInt(G.I64, I)},
           B.local(0));
    B.ret();
  }
}

/// N trivial functions that nobody calls.
void manyTrivial(GoodModule &G, unsigned N) {
  for (unsigned I = 0; I < N; ++I)
    makeAdd3(G, ("t" + std::to_string(I)).c_str());
}

/// One call of a callee with N parameters.
void manyParameters(GoodModule &G, unsigned N) {
  std::vector<Type *> Ps(N, G.I64);
  FuncId Callee = declare(G.M, "wide", G.C.func(Ps, G.I64));
  {
    FuncBuilder B(G.M, G.fn(Callee));
    B.assign(B.local(0), B.use(copyOf(B.local(N))));
    B.ret();
  }
  FuncId F = declare(G.M, "callWide", G.C.func({}, G.I64));
  FuncBuilder B(G.M, G.fn(F));
  std::vector<Operand> Args;
  for (unsigned I = 0; I < N; ++I)
    Args.push_back(constInt(G.I64, I));
  B.call(constFunc(G.M, Callee), Args, B.local(0));
  B.ret();
}

void expectInlineSteps(unsigned N, void (*Make)(GoodModule &, unsigned),
                       bool ExpectInlined = true) {
  GoodModule G1, G4;
  Make(G1, N);
  Make(G4, 4 * N);
  uint64_t A = inlineSteps();
  uint64_t I1 = inlineTrivial(G1.M).Inlined;
  uint64_t B = inlineSteps();
  uint64_t I4 = inlineTrivial(G4.M).Inlined;
  uint64_t C = inlineSteps();
  EXPECT_EQ(I1 > 0, ExpectInlined);
  EXPECT_EQ(I4 > 0, ExpectInlined);
  EXPECT_TRUE(stepsAreLinear(B - A, C - B, N));
  EXPECT_GIR_VALID(G4.M);
}

TEST(GIRInlineStress, ManyCallsOfOneCallee) {
  expectInlineSteps(2000, manyCalls);
}
TEST(GIRInlineStress, ManyCallers) { expectInlineSteps(2000, manyCallers); }
TEST(GIRInlineStress, ManyTrivialFunctions) {
  expectInlineSteps(2000, manyTrivial, /*ExpectInlined=*/false);
}
TEST(GIRInlineStress, ManyParameters) {
  expectInlineSteps(2000, manyParameters);
}

TEST(GIRInlineStress, ControlAQuadraticCountFailsTheCheck) {
  // The check itself: a count that grows with N*N is refused, a linear one
  // accepted.
  EXPECT_TRUE(stepsAreLinear(5 * 1000, 5 * 4000, 1000));
  EXPECT_FALSE(stepsAreLinear(1000ull * 1000, 16ull * 1000 * 1000, 1000));
}

} // namespace
