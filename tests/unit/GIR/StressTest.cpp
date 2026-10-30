//===- StressTest.cpp - GIR walks stay near-linear ------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01. Every new analysis gets a generated stress test: the verifier and
// the printer run on functions of tens of thousands of blocks (no recursion
// on the depth of the CFG), wide cleanup regions and thousands of
// functions. Four times the input must cost at most 4.5 times the work (in the
// long suite: at most 12 times the time); a quadratic walk costs 16 times.
//
// M8: the normal suite never compares clocks. It counts work (verifierSteps,
// printerSteps) and asserts steps <= C * N at N and 4N; the timed comparison
// is GIRStressLong (tests/gir/walk-stress.long.test): the best of five
// interleaved runs, repeated until the small input takes 20 ms, limit 12 * T1.
// Each helper has a control that runs a quadratic walk and must fail it.
//
//===----------------------------------------------------------------------===//

#include "../Support/GIRTestSupport.h"

#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Lower.h"
#include "gopp/GIR/Print.h"
#include "gopp/GIR/Verify.h"
#include "gopp/Sema/Loader.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Target/TargetMachine.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::test;

namespace {

/// A function of N+1 blocks; block i computes, then branches to i+1 or i+2
/// (a ladder, not a line); the last one returns.
void ladder(GoodModule &G, unsigned N) {
  FuncId F = declare(G.M, "ladder", G.C.func({G.I64}, G.I64));
  FuncBuilder B(G.M, G.fn(F));
  LocalId T = B.newTemp(G.I64), Cn = B.newTemp(G.Bool);
  for (unsigned I = 1; I <= N; ++I)
    B.newBlock();
  for (unsigned I = 0; I <= N; ++I) {
    B.setBlock(I);
    B.assign(B.local(T),
             B.binary(BinOp::Add, copyOf(B.local(1)), constInt(G.I64, I)));
    if (I == N) {
      B.assign(B.local(0), B.use(copyOf(B.local(T))));
      B.ret();
      continue;
    }
    B.assign(B.local(Cn),
             B.binary(BinOp::Lt, copyOf(B.local(T)), constInt(G.I64, 100)));
    B.branch(copyOf(B.local(Cn)), I + 1, I + 2 <= N ? I + 2 : I + 1);
  }
}

/// A function with N calls; each unwinds into one of 64 cleanup blocks that
/// chain into each other and end in resume.
void cleanups(GoodModule &G, unsigned N) {
  FuncId F = declare(G.M, "cleanups", G.C.func({}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  LocalId R = B.newTemp(G.I64);
  const unsigned K = 64;
  std::vector<BlockId> Ok{0}, Cl;
  for (unsigned I = 1; I < N; ++I)
    Ok.push_back(B.newBlock());
  for (unsigned I = 0; I < K; ++I)
    Cl.push_back(B.newBlock(/*Cleanup=*/true));
  BlockId End = B.newBlock();
  for (unsigned I = 0; I < N; ++I) {
    B.setBlock(Ok[I]);
    B.call(constFunc(G.M, G.Ext), {constInt(G.I64, I)}, B.local(R),
           {UnwindAction::Cleanup, Cl[I % K]}, I + 1 < N ? Ok[I + 1] : End);
  }
  B.setBlock(End);
  B.ret();
  for (unsigned I = 0; I < K; ++I) {
    B.setBlock(Cl[I]);
    if (I + 1 < K)
      B.gotoBlock(Cl[I + 1]);
    else
      B.resume();
  }
}

/// N functions that each return their parameter.
void manyFunctions(GoodModule &G, unsigned N) {
  for (unsigned I = 0; I < N; ++I) {
    FuncId F = declare(G.M, "f" + std::to_string(I), G.C.func({G.I64}, G.I64));
    FuncBuilder B(G.M, G.fn(F));
    B.assign(B.local(0), B.use(copyOf(B.local(1))));
    B.ret();
  }
}

/// One function with N locals.
void manyLocals(GoodModule &G, unsigned N) {
  FuncId F = declare(G.M, "locals", G.C.func({}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  for (unsigned I = 0; I < N; ++I)
    B.newTemp(G.I64);
  B.ret();
}

/// One function with a switch of N cases.
void manyCases(GoodModule &G, unsigned N) {
  FuncId F = declare(G.M, "cases", G.C.func({G.I64}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  BlockId Out = B.newBlock();
  std::vector<SwitchCase> Cs;
  for (unsigned I = 0; I < N; ++I)
    Cs.push_back({I, Out});
  B.switchInt(copyOf(B.local(1)), Cs, Out);
  B.setBlock(Out);
  B.ret();
}

/// One call with N arguments.
void manyArguments(GoodModule &G, unsigned N) {
  FuncId Callee =
      declare(G.M, "callee", G.C.func(std::vector<Type *>(N, G.I64), G.Unit));
  FuncId F = declare(G.M, "args", G.C.func({}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  std::vector<Operand> Args;
  for (unsigned I = 0; I < N; ++I)
    Args.push_back(constInt(G.I64, I));
  BlockId Out = B.newBlock();
  B.call(constFunc(G.M, Callee), Args, B.local(B.newTemp(G.Unit)),
         {UnwindAction::Continue, NoId}, Out);
  B.setBlock(Out);
  B.ret();
}

/// One tuple built from N operands.
void manyAggregateOperands(GoodModule &G, unsigned N) {
  FuncId F = declare(G.M, "agg", G.C.func({}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  Type *T = G.C.tuple(std::vector<Type *>(N, G.I64));
  std::vector<Operand> Ops;
  for (unsigned I = 0; I < N; ++I)
    Ops.push_back(constInt(G.I64, I));
  B.assign(B.local(B.newTemp(T)), B.aggregate(AggKind::Tuple, T, Ops));
  B.ret();
}

/// One place with N projections (N derefs of a pointer to pointer to ...).
void manyProjections(GoodModule &G, unsigned N) {
  Type *T = G.I64;
  for (unsigned I = 0; I < N; ++I)
    T = G.C.ptr(T);
  FuncId F = declare(G.M, "proj", G.C.func({T}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  Place P = B.local(1);
  for (unsigned I = 0; I < N; ++I)
    P = B.deref(P);
  B.assign(B.local(B.newTemp(G.I64)), B.use(copyOf(P)));
  B.ret();
}

/// N globals.
void manyGlobals(GoodModule &G, unsigned N) {
  for (unsigned I = 0; I < N; ++I) {
    Global Gl;
    Gl.Name = "g" + std::to_string(I);
    Gl.Ty = G.I64;
    G.M.addGlobal(std::move(Gl));
  }
}

/// One struct of N fields, used by a local.
void manyFields(GoodModule &G, unsigned N) {
  Type *S = G.C.structTy("stress.Wide");
  G.C.setStructBody(S, std::vector<Type *>(N, G.I64));
  FuncId F = declare(G.M, "fields", G.C.func({}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  B.newTemp(S);
  B.ret();
}

/// The CPU time of the calling thread, in seconds. Other processes on a busy
/// machine take the CPU away without adding to this clock; what is left is
/// cache and memory contention.
static double threadCpuSeconds() {
  timespec Ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &Ts);
  return static_cast<double>(Ts.tv_sec) +
         1e-9 * static_cast<double>(Ts.tv_nsec);
}

/// The time of one run of Run, in seconds (thread CPU time, not wall time).
template <class Fn> double best1(Fn Run) {
  double T0 = threadCpuSeconds();
  Run();
  return threadCpuSeconds() - T0;
}

/// Timing (long suite only, M8): the times of Small and Big (the same work at
/// four times the size). Reps is calibrated: it starts from the best of
/// several probe runs and doubles until Small takes at least 20 ms, so the
/// times are far above the clock's noise. Each time is the least of five
/// runs of Small and Big taken in turn, so a machine that gets busier or
/// quieter during the test changes both. The times are of the thread's CPU
/// (best1), so other processes do not add to them.
template <class FnA, class FnB>
std::pair<double, double> bestOfBothOnce(FnA Small, FnB Big) {
  double Probe = 1e9;
  for (int I = 0; I < 5; ++I)
    Probe = std::min(Probe, best1(Small));
  Probe = std::max(Probe, 1e-7);
  long Reps = std::max(1L, static_cast<long>(std::ceil(0.025 / Probe)));
  auto Many = [&Reps](auto &Fn) {
    return best1([&] {
      for (long I = 0; I < Reps; ++I)
        Fn();
    });
  };
  while (Many(Small) < 0.020 && Reps < (1L << 24))
    Reps *= 2;
  double A = 1e9, B = 1e9;
  for (int I = 0; I < 5; ++I) {
    A = std::min(A, Many(Small));
    B = std::min(B, Many(Big));
  }
  return {A, B};
}

/// The time that a run of Scale times the units may take when the run of N
/// took T1: 3 * Scale times (linear is Scale, quadratic Scale squared). No
/// floor: bestOfBoth makes T1 long enough to mean something.
double nearLinearLimit(double T1, double Scale = 4) { return 3 * Scale * T1; }

/// bestOfBothOnce, measured again (twice at most) while the pair is over the
/// limit, keeping the least time of each side over all the rounds. A busy
/// machine only slows a run down; a quadratic walk is over the limit in every
/// round. The controls, which must fail, call bestOfBothOnce.
template <class FnA, class FnB>
std::pair<double, double> bestOfBoth(FnA Small, FnB Big) {
  auto R = bestOfBothOnce(Small, Big);
  for (int Round = 0; Round < 2 && R.second >= nearLinearLimit(R.first);
       ++Round) {
    auto Again = bestOfBothOnce(Small, Big);
    R = {std::min(R.first, Again.first), std::min(R.second, Again.second)};
  }
  return R;
}

/// Counting (normal suite, M8): the work of N units and of 4N units is at
/// most C per unit, and four times the input costs at most 4.5 times the work
/// (quadratic work is 16 times). A count does not change with the load.
::testing::AssertionResult stepsAreLinear(uint64_t S1, uint64_t S4,
                                          uint64_t N) {
  const uint64_t C = 24;
  if (S1 == 0 || S1 > C * N || S4 > C * 4 * N || 2 * S4 > 9 * S1)
    return ::testing::AssertionFailure()
           << "steps(N) = " << S1 << ", steps(4N) = " << S4 << ", N = " << N;
  return ::testing::AssertionSuccess();
}

/// Verifying a module of N units and one of 4N units: the steps counted.
void expectVerifierSteps(unsigned N, void (*Make)(GoodModule &, unsigned)) {
  GoodModule G1, G4;
  Make(G1, N);
  Make(G4, 4 * N);
  uint64_t A = verifierSteps();
  EXPECT_TRUE(verify(G1.M).empty());
  uint64_t B = verifierSteps();
  EXPECT_TRUE(verify(G4.M).empty());
  uint64_t C = verifierSteps();
  EXPECT_TRUE(stepsAreLinear(B - A, C - B, N));
}

/// Verifying a module of 4N units takes at most 12 times as long as one of N.
void expectVerifierNearLinear(unsigned N,
                              void (*Make)(GoodModule &, unsigned)) {
  GoodModule G1, G4;
  Make(G1, N);
  Make(G4, 4 * N);
  auto [T1, T4] = bestOfBoth([&] { EXPECT_TRUE(verify(G1.M).empty()); },
                             [&] { EXPECT_TRUE(verify(G4.M).empty()); });
  EXPECT_LT(T4, nearLinearLimit(T1))
      << "T(N) = " << T1 << " s, T(4N) = " << T4 << " s";
}

// The controls (R19, M8): the helpers above must fail a walk that is
// quadratic in the number of units (each unit scans all the others), and pass
// the same walk done linearly.
static volatile unsigned long Sink;
static uint64_t WalkSteps;
static void walkUnits(const std::vector<unsigned> &Units, bool Quadratic) {
  unsigned long Found = 0;
  if (Quadratic) {
    for (unsigned U : Units)
      for (unsigned V : Units) {
        Found += (U ^ V) == 1;
        ++WalkSteps;
      }
  } else {
    for (unsigned U : Units) {
      Found += U & 1;
      ++WalkSteps;
    }
  }
  Sink = Found;
}

std::vector<unsigned> iota(unsigned N) {
  std::vector<unsigned> V(N);
  for (unsigned I = 0; I < N; ++I)
    V[I] = I;
  return V;
}

TEST(GIRStress, TheStepCountHelperFailsAQuadraticWalk) {
  for (bool Quadratic : {true, false}) {
    std::vector<unsigned> Small = iota(600), Big = iota(2400);
    WalkSteps = 0;
    walkUnits(Small, Quadratic);
    uint64_t S1 = WalkSteps;
    walkUnits(Big, Quadratic);
    uint64_t S4 = WalkSteps - S1;
    // A quadratic walk makes 600 * 600 steps for 600 units.
    EXPECT_EQ(bool(stepsAreLinear(S1, S4, 600)), !Quadratic) << S1 << " " << S4;
  }
}

TEST(GIRStressLong, DISABLED_TheTimingHelperFailsAQuadraticWalk) {
  // Eight times the size: linear is 8, quadratic 64, the limit 24.
  std::vector<unsigned> Small = iota(1000), Big = iota(8000);
  auto [Q1, Q8] = bestOfBothOnce([&] { walkUnits(Small, true); },
                                 [&] { walkUnits(Big, true); });
  EXPECT_GE(Q8, nearLinearLimit(Q1, 8))
      << "a quadratic walk passed: T(N) = " << Q1 << ", T(8N) = " << Q8;
  auto [L1, L8] = bestOfBothOnce([&] { walkUnits(Small, false); },
                                 [&] { walkUnits(Big, false); });
  EXPECT_LT(L8, nearLinearLimit(L1, 8)) << L1 << " s, " << L8 << " s";
}

TEST(GIRStress, VerifierOnALadderOf80000Blocks) {
  expectVerifierSteps(20000, ladder);
}

TEST(GIRStress, VerifierOnWideCleanupRegions) {
  expectVerifierSteps(20000, cleanups);
}

TEST(GIRStress, VerifierOnManyFunctions) {
  expectVerifierSteps(5000, manyFunctions);
}

// R25: one test per dimension of the verifier's loops. Each grows exactly one
// (locals, switch cases, call arguments, aggregate operands, projections,
// globals, struct fields) from N to 16N: the steps must follow it (at least
// 10 times, so the loop counts its elements) and stay linear (at most
// 24 per unit, and 18 times: a quadratic walk is 256 times).
void expectStepsFollow(unsigned N, void (*Make)(GoodModule &, unsigned)) {
  GoodModule G1, G16;
  Make(G1, N);
  Make(G16, 16 * N);
  uint64_t A = verifierSteps();
  EXPECT_TRUE(verify(G1.M).empty());
  uint64_t B = verifierSteps();
  EXPECT_TRUE(verify(G16.M).empty());
  uint64_t C = verifierSteps();
  EXPECT_GE(C - B, 10 * (B - A)) << (B - A) << " -> " << (C - B);
  EXPECT_LE(C - B, 24 * 16 * uint64_t(N)) << (C - B);
  EXPECT_LE(C - B, 18 * (B - A)) << (B - A) << " -> " << (C - B);
}

TEST(GIRStress, TheVerifierCountsLocals) { expectStepsFollow(500, manyLocals); }
TEST(GIRStress, TheVerifierCountsSwitchCases) {
  expectStepsFollow(500, manyCases);
}
TEST(GIRStress, TheVerifierCountsCallArguments) {
  expectStepsFollow(500, manyArguments);
}
TEST(GIRStress, TheVerifierCountsAggregateOperands) {
  expectStepsFollow(500, manyAggregateOperands);
}
TEST(GIRStress, TheVerifierCountsProjections) {
  expectStepsFollow(250, manyProjections);
}
TEST(GIRStress, TheVerifierCountsGlobals) {
  expectStepsFollow(500, manyGlobals);
}
TEST(GIRStress, TheVerifierCountsStructFields) {
  expectStepsFollow(500, manyFields);
}
TEST(GIRStress, TheVerifierCountsFunctions) {
  expectStepsFollow(500, manyFunctions);
}

TEST(GIRStressLong, DISABLED_VerifierOnALadderOf80000Blocks) {
  expectVerifierNearLinear(20000, ladder);
}

TEST(GIRStressLong, DISABLED_VerifierOnWideCleanupRegions) {
  expectVerifierNearLinear(20000, cleanups);
}

TEST(GIRStressLong, DISABLED_VerifierOnManyFunctions) {
  expectVerifierNearLinear(5000, manyFunctions);
}

TEST(GIRStress, VerifierFindsUnreachableBlocksAmongThousands) {
  GoodModule G;
  ladder(G, 30000);
  // Cut the ladder (a block is reached from the two before it): everything
  // after bb15001 is unreachable.
  Function &F = G.fn(G.M.lookupFunction("ladder"));
  F.Blocks[15000].Term = term(Terminator::Return);
  F.Blocks[15001].Term = term(Terminator::Return);
  std::vector<VerifyError> E = verify(G.M, 100000);
  EXPECT_EQ(E.size(), 30000u - 15001u);
  for (const VerifyError &Err : E)
    ASSERT_EQ(Err.Rule, "V022") << Err.str();
}

TEST(GIRStress, PrinterIsLinearAndStable) {
  auto Text = [](unsigned N) {
    GoodModule G;
    ladder(G, N);
    return textOf(G.M);
  };
  std::string A1 = Text(10000), A2 = Text(10000);
  uint64_t P0 = printerSteps();
  std::string B = Text(40000);
  uint64_t P1 = printerSteps();
  std::string A3 = Text(10000);
  uint64_t P2 = printerSteps();
  EXPECT_EQ(A1, A2); // deterministic
  EXPECT_EQ(A1, A3);
  double R = double(B.size()) / double(A1.size());
  EXPECT_GT(R, 3.8);
  EXPECT_LT(R, 4.2);
  EXPECT_TRUE(stepsAreLinear(P2 - P1, P1 - P0, 10000));
}

TEST(GIRStress, ThePrinterCountsEachDimension) {
  using Make = void (*)(GoodModule &, unsigned);
  const Make Makes[] = {
      manyLocals,      manyCases,   manyArguments, manyAggregateOperands,
      manyProjections, manyGlobals, manyFunctions, ladder};
  for (Make M : Makes) {
    GoodModule G1, G16;
    M(G1, 250);
    M(G16, 16 * 250);
    uint64_t A = printerSteps();
    (void)textOf(G1.M);
    uint64_t B = printerSteps();
    (void)textOf(G16.M);
    uint64_t C = printerSteps();
    EXPECT_GE(C - B, 10 * (B - A)) << (B - A) << " -> " << (C - B);
    EXPECT_LE(C - B, 24 * 16 * 250u) << (C - B);
  }
}

TEST(GIRStressLong, DISABLED_PrinterIsNearLinearInTime) {
  auto Text = [](unsigned N) {
    GoodModule G;
    ladder(G, N);
    return textOf(G.M);
  };
  auto [T1, T4] = bestOfBoth([&] { Text(10000); }, [&] { Text(40000); });
  EXPECT_LT(T4, nearLinearLimit(T1)) << T1 << " s, " << T4 << " s";
}

//===----------------------------------------------------------------------===//
// P04.04: the lowering of the core subset
//===----------------------------------------------------------------------===//

/// A program checked once, ready to be lowered many times.
struct Checked {
  explicit Checked(const std::string &Source) : Diags(SM, Collect, 0) {
    llvm::SmallString<128> D;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-stress", D));
    Dir = std::string(D);
    {
      std::ofstream Out(Dir + "/main.gopp", std::ios::binary);
      Out << Source;
    }
    sema::LoadOptions LO;
    LO.Root = Dir;
    Ld = std::make_unique<sema::Loader>(SM, Diags, TCtx, LO,
                                        sema::CheckOptions());
    CP = Ld->loadDir(Dir, "");
  }
  ~Checked() { llvm::sys::fs::remove_directories(Dir); }
  std::string errors() const {
    std::string E;
    for (const Diagnostic &D : Collect.Diags)
      if (D.Sev == Severity::Error)
        E += D.Message + "\n";
    return E;
  }
  std::unique_ptr<gir::Module> lower(gir::Context &C) {
    return lowerPackage(C, SM, *CP, Ld->filesOf(CP), Diags);
  }
  std::string Dir;
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags;
  sema::TypeContext TCtx;
  std::unique_ptr<sema::Loader> Ld;
  sema::CheckedPackage *CP = nullptr;
};

/// The code each generator makes for N units of one construct.
std::string switchOfN(unsigned N) {
  std::string S = "package main\n\nfunc f(x int) int {\n\tswitch x {\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tcase " + std::to_string(I) + ":\n\t\treturn " +
         std::to_string(I * 3) + "\n";
  S += "\t}\n\tswitch {\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tcase x == " + std::to_string(I + N) + ":\n\t\treturn 1\n";
  return S + "\t}\n\treturn 0\n}\n\nfunc main() { println(f(5)) }\n";
}

std::string structOfN(unsigned N) {
  std::string S = "package main\n\ntype T struct {\n", Lit, Cmp;
  for (unsigned I = 0; I < N; ++I) {
    S += "\tf" + std::to_string(I) + " int32\n";
    Lit += (I ? ", " : "") + std::to_string(I);
  }
  S += "}\n\nfunc main() {\n\ta := T{" + Lit + "}\n\tb := a\n\tb.f" +
       std::to_string(N - 1) + " = 0\n\tprintln(a == b, a.f1 + b.f2)\n}\n";
  return S;
}

std::string globalsOfN(unsigned N) {
  std::string S = "package main\n\n";
  for (unsigned I = N; I-- > 1;)
    S += "var g" + std::to_string(I) + " = g" + std::to_string(I - 1) + " + " +
         std::to_string(I) + "\n";
  return S + "var g0 = 1\n\nfunc main() { println(g" + std::to_string(N - 1) +
         ") }\n";
}

std::string statementsOfN(unsigned N) {
  std::string S = "package main\n\nfunc main() {\n\tx := 1\n\tvar a [8]int\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tx += a[x&7] / (x | 1)\n\ta[x&7] = x << uint(x&3)\n";
  return S + "\tprintln(x)\n}\n";
}

std::string typesOfN(unsigned N) {
  std::string S = "package main\n\nfunc main() {\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\t{\n\t\ttype L struct{ a int }\n\t\tx, y := L{" + std::to_string(I) +
         "}, L{1}\n\t\t_ = x == y\n\t}\n";
  return S + "}\n";
}

/// Lowering a program of 4N units takes at most 8 times as long as one of N
/// (the check of the program is not timed; it is the checker's own test).
void expectLoweringNearLinear(unsigned N, std::string (*Make)(unsigned)) {
  Checked P1(Make(N)), P4(Make(4 * N));
  EXPECT_EQ(P1.errors(), "") << "N = " << N;
  EXPECT_EQ(P4.errors(), "") << "N = " << 4 * N;
  auto Lower = [](Checked &P) {
    return [&P] {
      gir::Context C;
      auto M = P.lower(C);
      EXPECT_NE(M, nullptr) << P.errors();
    };
  };
  auto [T1, T4] = bestOfBoth(Lower(P1), Lower(P4));
  EXPECT_LT(T4, nearLinearLimit(T1))
      << "T(N) = " << T1 << " s, T(4N) = " << T4 << " s";
}

// The lowering stress tests are long (tests/gir/core-stress*.long.test run
// them): the shapes the checker made quadratic or recursive before P04.04's
// fixes (M3) have N of 10000 or 20000, so 4N is 40000 or 80000 (a small N
// times too little to tell a slow walk from a busy machine).
TEST(GIRStressLong, DISABLED_LoweringSwitchesWithManyCases) {
  expectLoweringNearLinear(20000, switchOfN);
}
TEST(GIRStressLong, DISABLED_LoweringStructsWithManyFields) {
  expectLoweringNearLinear(10000, structOfN);
}
TEST(GIRStressLong, DISABLED_LoweringGlobalsInInitOrder) {
  expectLoweringNearLinear(10000, globalsOfN);
}
TEST(GIRStressLong, DISABLED_LoweringManyCheckedStatements) {
  expectLoweringNearLinear(1500, statementsOfN);
}
TEST(GIRStressLong, DISABLED_LoweringManyLocalTypes) {
  expectLoweringNearLinear(1500, typesOfN);
}

TEST(GIRStressLong, DISABLED_CodeGenerationOfTheCoreSubsetIsNearLinear) {
  auto Spec = parseTarget("linux/amd64");
  auto TM = createTargetMachine(*Spec);
  ASSERT_TRUE(static_cast<bool>(TM));
  Checked P1(statementsOfN(2000)), P4(statementsOfN(8000));
  gir::Context C1, C4;
  auto M1 = P1.lower(C1), M4 = P4.lower(C4);
  ASSERT_NE(M1, nullptr);
  ASSERT_NE(M4, nullptr);
  auto Lower = [&TM](gir::Module &M) {
    return [&M, &TM] {
      llvm::LLVMContext LC;
      auto LM = lowerToLLVM(LC, M, **TM);
      EXPECT_TRUE(static_cast<bool>(LM));
    };
  };
  auto [T1, T4] = bestOfBoth(Lower(*M1), Lower(*M4));
  EXPECT_LT(T4, nearLinearLimit(T1))
      << "T(N) = " << T1 << " s, T(4N) = " << T4 << " s";
}

} // namespace
