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

/// The CPU time of this process, in seconds. Other processes on a busy
/// machine take the CPU away without adding to this clock; what is left is
/// cache and memory contention. (The process, not the calling thread:
/// lowerPackage runs on a thread of its own with a bigger stack, which the
/// clock of the waiting caller would not see; the tests run nothing else.)
static double processCpuSeconds() {
  timespec Ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &Ts);
  return static_cast<double>(Ts.tv_sec) +
         1e-9 * static_cast<double>(Ts.tv_nsec);
}

/// The time of one run of Run, in seconds (process CPU time, not wall time).
template <class Fn> double best1(Fn Run) {
  double T0 = processCpuSeconds();
  Run();
  return processCpuSeconds() - T0;
}

/// The time that a run of Scale times the units may take when the run of N
/// took T1: 3 * Scale times (linear is Scale, quadratic Scale squared). No
/// floor: bestOfBoth makes T1 long enough to mean something.
double nearLinearLimit(double T1, double Scale = 4) { return 3 * Scale * T1; }

/// Timing (long suite only, M8): the times of Small and Big (the same work at
/// Scale times the size). Reps is calibrated: it starts from the best of
/// several probe runs and doubles until Small takes at least 20 ms, so the
/// times are far above the clock's noise. Each time is the least of five
/// runs of Small and Big taken in turn, so a machine that gets busier or
/// quieter during the test changes both. Big runs only while the pair is at
/// or over nearLinearLimit (five times at most): a busy machine only adds
/// time, so more runs could only lower Big, and the comparison with the
/// limit comes out as with five runs of each; Small, which a quadratic walk
/// needs to be too slow to pass, always gets its five. The times are of the
/// process's CPU (best1), so other processes do not add to them.
template <class FnA, class FnB>
std::pair<double, double> bestOfBothOnce(FnA Small, FnB Big, double Scale = 4) {
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
  int RunsOfBig = 0;
  auto RunBig = [&] {
    B = std::min(B, Many(Big));
    ++RunsOfBig;
  };
  for (int I = 0; I < 5; ++I) {
    A = std::min(A, Many(Small));
    if (RunsOfBig == 0 || B >= nearLinearLimit(A, Scale))
      RunBig();
  }
  while (RunsOfBig < 5 && B >= nearLinearLimit(A, Scale))
    RunBig();
  return {A, B};
}

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
                                 [&] { walkUnits(Big, true); }, 8);
  EXPECT_GE(Q8, nearLinearLimit(Q1, 8))
      << "a quadratic walk passed: T(N) = " << Q1 << ", T(8N) = " << Q8;
  auto [L1, L8] = bestOfBothOnce([&] { walkUnits(Small, false); },
                                 [&] { walkUnits(Big, false); }, 8);
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
  std::unique_ptr<gir::Module> lower(gir::Context &C,
                                     const LowerOptions &Opts = {}) {
    return lowerPackage(C, SM, *CP, Ld->filesOf(CP), Diags, Opts);
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

//===----------------------------------------------------------------------===//
// P04.05: strings, slices, views and maps
//===----------------------------------------------------------------------===//

// Normal suite (M8): the work is counted, not timed. Lowering N units must
// make GIR the verifier walks in at most C steps per unit, and four times the
// units at most 4.5 times the steps (verifierSteps counts the elements of
// every walk it does). Each dimension a program can grow in has its own
// generator, so a quadratic spot shows in the dimension that has it: a long
// `+` chain, a big literal, a deep chain of slicing, many map operations.
// The control feeds stepsAreLinear a program whose work is the product of two
// dimensions.

/// x + x + ... (N terms).
std::string concatChainOfN(unsigned N) {
  std::string S = "package main\n\nfunc main() {\n\tx := \"ab\" + "
                  "string(rune(65+len(\"\")))\n\ts := x";
  for (unsigned I = 0; I < N; ++I)
    S += " + x";
  return S + "\n\tprintln(len(s))\n}\n";
}

/// N statements of `s += x`: the same dimension, as a sequence.
std::string concatStatementsOfN(unsigned N) {
  std::string S =
      "package main\n\nfunc main() {\n\tx := \"ab\" + \"\"\n\ts := x + \"\"\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\ts += x\n";
  return S + "\tprintln(len(s))\n}\n";
}

/// A slice literal, a map literal and a string-slice literal of N elements.
std::string literalsOfN(unsigned N) {
  std::string L, M, T;
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    L += (I ? ", " : "") + K;
    M += (I ? ", " : "") + K + ": " + K;
    T += std::string(I ? ", " : "") + "\"s" + K + "\"";
  }
  return "package main\n\nfunc main() {\n\tl := []int{" + L +
         "}\n\tm := map[int]int{" + M + "}\n\tt := []string{" + T +
         "}\n\tprintln(len(l), len(m), len(t))\n}\n";
}

/// a[1:][1:]...[1:] (N times) on a slice.
std::string slicingChainOfN(unsigned N) {
  std::string S =
      "package main\n\nfunc main() {\n\ta := make([]int, 10)\n\tv := a";
  for (unsigned I = 0; I < N; ++I)
    S += "[0:]";
  return S + "\n\tprintln(len(v))\n}\n";
}

/// N statements that slice (two and three index) and index.
std::string slicingStatementsOfN(unsigned N) {
  // (the three-index form is for arrays only, SLC-13)
  std::string S = "package main\n\nfunc main() {\n\ta := make([]int, "
                  "10)\n\tvar r [10]int\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tt += len(a[" + std::to_string(I % 3) +
         ":]) + len(r[1:4:" + std::to_string(5 + I % 4) + "]) + a[" +
         std::to_string(I % 10) + "]\n";
  return S + "\tprintln(t)\n}\n";
}

/// N map writes, reads, deletes and comma-ok reads.
std::string mapOpsOfN(unsigned N) {
  std::string S = "package main\n\nfunc main() {\n\tm := map[int]string{}\n\tn "
                  ":= map[string]int{}\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    S += "\tm[" + K + "] = \"v" + K + "\"\n\tn[m[" + K +
         "] + \"\"]++\n\tif _, ok := m[" + std::to_string(I / 2) +
         "]; ok {\n\t\tt += len(m[" + K + "])\n\t}\n\tdelete(m, " +
         std::to_string(I / 3) + ")\n";
  }
  return S + "\tprintln(t, len(m), len(n))\n}\n";
}

/// N ranges over a map that end every way a loop can (RNG-4: each way out
/// ends the iteration): break, `continue` of the loop around it, `return`,
/// and `goto` out to N labels (the labels of a function are found by one walk
/// of its statements, at its first goto out of a map range).
std::string mapRangesOfN(unsigned N) {
  std::string S = "package main\n\nfunc find(m map[int]int, k int) int {\n"
                  "\tfor a, b := range m {\n\t\tif a == k {\n\t\t\treturn "
                  "b\n\t\t}\n\t}\n\treturn -1\n}\n\nfunc main() {\n\tm := "
                  "map[int]int{1: 2, 3: 4}\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    S += "\tfor k := range m {\n\t\tif k > 2 {\n\t\t\tgoto l" + K +
         "\n\t\t}\n\t\tt += k\n\t\tbreak\n\t}\nl" + K +
         ":\n\tfor i := 0; i < 2; i++ {\n\t\tfor range m {\n\t\t\tcontinue l" +
         K + "\n\t\t}\n\t}\n\tt += find(m, " + std::to_string(I % 4) + ")\n";
  }
  return S + "\tprintln(t)\n}\n";
}

/// N appends and copies and conversions.
std::string sliceOpsOfN(unsigned N) {
  std::string S = "package main\n\nfunc main() {\n\tvar s []int\n\tb := "
                  "[]byte(\"x\")\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\ts = append(s, " + std::to_string(I) +
         ", 1)\n\tt += copy(s, s[1:]) + len(string(b)) + "
         "len([]rune(string(b)))\n";
  return S + "\tprintln(t, len(s))\n}\n";
}

/// P04.25: one function with N borrowed slice parameters (each arrives as the
/// pair (ptr, len) and the body gets a slice of its own with a null word), one
/// call that passes N owned slices (each call argument makes its pair), and
/// the function again with `move` parameters (each passes all four words).
std::string borrowedSliceParamsOfN(unsigned N) {
  std::string Ps, Body, As, Mv, MvAs;
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    Ps += std::string(I ? ", " : "") + "a" + K;
    Body += "\tt += len(a" + K + ")\n";
    As += std::string(I ? ", " : "") + "s";
    Mv += std::string(I ? ", " : "") + "move m" + K + " []int";
    MvAs += std::string(I ? ", " : "") + "[]int{" + K + "}";
  }
  return "package main\n\nfunc f(" + Ps + " []int) int {\n\tt := 0\n" + Body +
         "\treturn t\n}\n\nfunc g(" + Mv +
         ") int {\n\treturn 1\n}\n\nfunc main() {\n\ts := "
         "[]int{1, 2}\n\tprintln(f(" +
         As + "), g(" + MvAs + "))\n}\n";
}

/// N calls of a function that takes a borrowed slice, an owned slice and
/// returns one (append through a `move` parameter): the word is passed on N
/// times.
std::string wordPassedOnOfN(unsigned N) {
  std::string S = "package main\n\nfunc sum(xs []int) int {\n\treturn "
                  "len(xs)\n}\n\nfunc grow(move xs []int) []int {\n\t"
                  "return append(xs, 1)\n}\n\nfunc main() {\n\tvar s "
                  "[]int\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\ts = grow(s)\n\tt += sum(s)\n";
  return S + "\tprintln(t, len(s))\n}\n";
}

/// The steps the verifier counts on the lowered program of N units.
uint64_t lowerAndCount(unsigned N, std::string (*Make)(unsigned)) {
  Checked P(Make(N));
  EXPECT_EQ(P.errors(), "") << "N = " << N;
  gir::Context C;
  auto M = P.lower(C);
  EXPECT_NE(M, nullptr) << P.errors();
  if (!M)
    return 0;
  uint64_t A = verifierSteps();
  EXPECT_TRUE(verify(*M).empty());
  return verifierSteps() - A;
}

/// The lowered program of N units and the one of 4N: linear steps. The
/// runtime functions are a fixed part, which the per-unit limit allows for.
void expectLoweredStepsLinear(unsigned N, std::string (*Make)(unsigned)) {
  uint64_t S1 = lowerAndCount(N, Make), S4 = lowerAndCount(4 * N, Make);
  const uint64_t C = 400; // GIR steps one unit may make, runtime included
  EXPECT_TRUE(S1 != 0 && S1 <= C * N && S4 <= C * 4 * N && 2 * S4 <= 9 * S1)
      << "steps(N) = " << S1 << ", steps(4N) = " << S4 << ", N = " << N;
}

TEST(GIRStress, LoweringALongConcatChainIsLinear) {
  expectLoweredStepsLinear(240, concatChainOfN); // 4N: the parser's depth
                                                 // limit is 1024
}
TEST(GIRStress, LoweringManyConcatStatementsIsLinear) {
  expectLoweredStepsLinear(600, concatStatementsOfN);
}
TEST(GIRStress, LoweringBigLiteralsIsLinear) {
  expectLoweredStepsLinear(600, literalsOfN);
}
TEST(GIRStress, LoweringADeepSlicingChainIsLinear) {
  expectLoweredStepsLinear(240, slicingChainOfN);
}
TEST(GIRStress, LoweringManySlicingStatementsIsLinear) {
  expectLoweredStepsLinear(300, slicingStatementsOfN);
}
TEST(GIRStress, LoweringManyMapOperationsIsLinear) {
  expectLoweredStepsLinear(200, mapOpsOfN);
}
TEST(GIRStress, LoweringManyAppendsAndConversionsIsLinear) {
  expectLoweredStepsLinear(300, sliceOpsOfN);
}
TEST(GIRStress, LoweringManyBorrowedSliceParametersIsLinear) {
  expectLoweredStepsLinear(300, borrowedSliceParamsOfN);
}
TEST(GIRStress, LoweringTheAllocatorWordPassedOnManyTimesIsLinear) {
  expectLoweredStepsLinear(300, wordPassedOnOfN);
}
TEST(GIRStress, LoweringManyMapRangesAndTheirExitsIsLinear) {
  expectLoweredStepsLinear(200, mapRangesOfN);
}

// The control: the same check on a program whose output grows with the
// product of two dimensions (each of N statements has an N-term chain) must
// fail it, so the check is able to see a quadratic lowering.
std::string concatSquareOfN(unsigned N) {
  std::string Chain = "x";
  for (unsigned I = 0; I < N; ++I)
    Chain += " + x";
  std::string S =
      "package main\n\nfunc main() {\n\tx := \"ab\" + \"\"\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tt += len(" + Chain + ")\n";
  return S + "\tprintln(t)\n}\n";
}

TEST(GIRStress, TheLoweringStepCheckFailsAQuadraticProgram) {
  uint64_t S1 = lowerAndCount(20, concatSquareOfN),
           S4 = lowerAndCount(80, concatSquareOfN);
  EXPECT_GT(2 * S4, 9 * S1) << S1 << " -> " << S4;
}

// P04.05 review R10: the lowering walks that make no GIR statement each (the
// late reads of an assignment's targets, `op=`, `++`, a range clause, the
// stores and shrink moves of a tuple, labelPos, exitScopes, the scopes a
// goto leaves) count their steps (gir::loweringSteps), since the verifier's
// steps measure the GIR they made, not the work that made it. One generator
// per dimension; each control grows its dimension 16 times: the count must
// grow at least 10 times (the counter sees the dimension) and stay at most
// C * 16N (linear).

/// One statement with N targets of every kind (a tuple assignment).
std::string tupleTargetsOfN(unsigned N) {
  std::string S = "package main\n\ntype T struct{ f int }\n\nfunc main() "
                  "{\n\ta := make([]int, 8)\n\tm := map[int]int{}\n\tmm "
                  ":= map[int]map[int]int{}\n\tts := make([]T, 8)\n\t";
  std::string L, R;
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I % 8);
    const char *Sep = I ? ", " : "";
    switch (I % 4) {
    case 0:
      L += Sep + ("a[" + K + "]");
      break;
    case 1:
      L += Sep + ("m[" + std::to_string(I) + "]");
      break;
    case 2:
      L += Sep + ("mm[" + K + "][" + std::to_string(I) + "]");
      break;
    default:
      L += Sep + ("ts[" + K + "].f");
      break;
    }
    R += Sep + std::to_string(I);
  }
  return S + L + " = " + R +
         "\n\tprintln(len(a), len(m), len(mm), len(ts))\n}\n";
}

/// One target whose path is N deep: x[i][i]...[i] = f().
std::string deepTargetOfN(unsigned N) {
  std::string Ty, Path;
  for (unsigned I = 0; I < N; ++I) {
    Ty += "[]";
    Path += "[i]";
  }
  return "package main\n\nfunc f() int { return 1 }\n\nfunc main() {\n\t"
         "var x " +
         Ty + "int\n\ti := len(x)\n\tif i > 0 {\n\t\tx" + Path +
         " = f()\n\t\tx" + Path + " += f()\n\t}\n}\n";
}

/// N statements of `op=`, `++`, append and borrowed map arguments.
std::string lateStatementsOfN(unsigned N) {
  std::string S = "package main\n\nfunc put(m map[int]int, k int) { m[k] = k "
                  "}\n\nfunc main() {\n\ta := make([]int, 8)\n\tmm := "
                  "map[int]map[int]int{}\n\tvar s []int\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I % 8);
    S += "\ta[" + K + "] += len(s)\n\tmm[" + K + "][" + K +
         "]++\n\ts = append(s, a[" + K + "])\n\tput(mm[" + K + "], " + K +
         ")\n";
  }
  return S + "\tprintln(len(s), len(mm))\n}\n";
}

/// N range loops whose targets are places, over a slice, a string and a map.
std::string rangeTargetsOfN(unsigned N) {
  std::string S = "package main\n\nfunc main() {\n\ta := make([]int, 8)\n\t"
                  "m := map[int]int{1: 2}\n\tk := 0\n\tr := 'x'\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tfor k, a[k&7] = range a {\n\t}\n\tfor k, r = range \"ab\" "
         "{\n\t}\n\tfor k, m[k] = range m {\n\t\tbreak\n\t}\n";
  return S + "\tprintln(k, r, len(m))\n}\n";
}

/// N ways out of nested blocks with locals (exitScopes): break, continue and
/// return.
std::string exitsOfN(unsigned N) {
  std::string S = "package main\n\nfunc f(n int) int {\n\tt := 0\n\tfor i "
                  ":= 0; i < n; i++ {\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    S += "\t\tif i == " + K +
         " {\n\t\t\tx := i\n\t\t\t{\n\t\t\t\ty "
         ":= x\n\t\t\t\tif y > 3 {\n\t\t\t\t\tbreak\n\t\t\t\t}\n\t"
         "\t\t\tif y > 2 {\n\t\t\t\t\tcontinue\n\t\t\t\t}\n\t\t\t"
         "\tif y > 1 {\n\t\t\t\t\treturn y\n\t\t\t\t}\n\t\t\t\tt "
         "+= y\n\t\t\t}\n\t\t}\n";
  }
  return S + "\t}\n\treturn t\n}\n\nfunc main() { println(f(9)) }\n";
}

/// The steps the lowering walks counted on the program of N units.
uint64_t lowerAndCountWalks(unsigned N, std::string (*Make)(unsigned),
                            const LowerOptions &Opts = {}) {
  Checked P(Make(N));
  EXPECT_EQ(P.errors(), "") << "N = " << N;
  gir::Context C;
  uint64_t A = loweringSteps();
  auto M = P.lower(C, Opts);
  EXPECT_NE(M, nullptr) << P.errors();
  return loweringSteps() - A;
}

/// Steps S1 for N units and S16 for 16N: the dimension is counted (S16 at
/// least 10 * S1) and linear (S16 at most C * 16N).
::testing::AssertionResult walksFollow(uint64_t S1, uint64_t S16, unsigned N) {
  const uint64_t C = 64; // walk steps one unit may take (at most 41 now)
  if (S1 == 0 || S16 < 10 * S1 || S16 > C * 16 * N)
    return ::testing::AssertionFailure()
           << "steps(N) = " << S1 << ", steps(16N) = " << S16 << ", N = " << N;
  return ::testing::AssertionSuccess();
}

void expectWalksFollow(unsigned N, std::string (*Make)(unsigned)) {
  uint64_t S1 = lowerAndCountWalks(N, Make);
  uint64_t S16 = lowerAndCountWalks(16 * N, Make);
  EXPECT_TRUE(walksFollow(S1, S16, N));
}

TEST(GIRStress, TheLoweringCountsTupleTargets) {
  expectWalksFollow(20, tupleTargetsOfN);
}
TEST(GIRStress, TheLoweringCountsTheDepthOfATarget) {
  expectWalksFollow(30, deepTargetOfN); // 16N: below the parser's limit
}
TEST(GIRStress, TheLoweringCountsLateReads) {
  expectWalksFollow(20, lateStatementsOfN);
}
TEST(GIRStress, TheLoweringCountsRangeTargets) {
  expectWalksFollow(20, rangeTargetsOfN);
}
TEST(GIRStress, TheLoweringCountsScopeExits) {
  expectWalksFollow(20, exitsOfN);
}
TEST(GIRStress, TheLoweringCountsLabelsAndGotos) {
  expectWalksFollow(20, mapRangesOfN);
}

// The control: labelPos walking the statements again at every goto (the
// quadratic walk, LowerOptions::RescanLabels) must fail the same check.
TEST(GIRStress, TheWalkCheckFailsAQuadraticLabelWalk) {
  LowerOptions Rescan;
  Rescan.RescanLabels = true;
  uint64_t S1 = lowerAndCountWalks(20, mapRangesOfN, Rescan);
  uint64_t S16 = lowerAndCountWalks(320, mapRangesOfN, Rescan);
  EXPECT_FALSE(walksFollow(S1, S16, 20)) << S1 << " -> " << S16;
}

// P04.06: the walks of interfaces. A chain of N interfaces, each embedding
// the one before and adding a method, satisfied by one type of N methods and
// used as the top one (flattened once: N interfaces visited, an itab of N
// methods made by a binary search in the type's sorted method set).
std::string ifaceChainOfN(unsigned N) {
  std::string S = "package main\n\ntype I0 interface{ M0() int }\n";
  for (unsigned I = 1; I < N; ++I)
    S += "type I" + std::to_string(I) + " interface {\n\tI" +
         std::to_string(I - 1) + "\n\tM" + std::to_string(I) + "() int\n}\n";
  S += "type T struct{}\n";
  for (unsigned I = 0; I < N; ++I)
    S += "func (T) M" + std::to_string(I) + "() int { return " +
         std::to_string(I) + " }\n";
  S += "\nfunc main() {\n\tvar x I" + std::to_string(N - 1) +
       " = T{}\n\tprintln(x.M0() + x.M" + std::to_string(N - 1) + "())\n}\n";
  return S;
}

// The control: the same chain with a variable of every interface of it
// (each flattened by itself: k interfaces for the k-th) is quadratic in N,
// and the count must see it.
std::string ifaceChainAllOfN(unsigned N) {
  std::string S = ifaceChainOfN(N);
  S.resize(S.rfind("\nfunc main()"));
  S += "\nfunc main() {\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tvar x" + std::to_string(I) + " I" + std::to_string(I) +
         " = T{}\n\t_ = x" + std::to_string(I) + "\n";
  return S + "}\n";
}

// A type switch of N cases (N types) and N sibling assertions.
std::string typeSwitchOfN(unsigned N) {
  std::string S = "package main\n\n";
  for (unsigned I = 0; I < N; ++I)
    S += "type T" + std::to_string(I) + " struct{ A int }\n";
  S += "\nfunc f(x any) int {\n\tswitch x.(type) {\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tcase T" + std::to_string(I) + ":\n\t\treturn " + std::to_string(I) +
         "\n";
  S += "\t}\n\treturn -1\n}\n\nfunc main() {\n\tvar a any = T0{}\n\tn := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tif _, ok := a.(T" + std::to_string(I) + "); ok {\n\t\tn++\n\t}\n";
  return S + "\tprintln(f(a), n)\n}\n";
}

TEST(GIRStress, TheLoweringCountsAChainOfEmbeddedInterfaces) {
  expectWalksFollow(30, ifaceChainOfN);
}
TEST(GIRStress, TheLoweringCountsTypeSwitchCasesAndAssertions) {
  expectWalksFollow(20, typeSwitchOfN);
}
TEST(GIRStress, TheInterfaceWalkCheckFailsAQuadraticChain) {
  uint64_t S1 = lowerAndCountWalks(20, ifaceChainAllOfN);
  uint64_t S16 = lowerAndCountWalks(320, ifaceChainAllOfN);
  EXPECT_FALSE(walksFollow(S1, S16, 20)) << S1 << " -> " << S16;
}
TEST(GIRStress, LoweringInterfacesIsLinearInTheGIRItMakes) {
  expectLoweredStepsLinear(60, ifaceChainOfN);
  expectLoweredStepsLinear(60, typeSwitchOfN);
}

// The other dimensions of interfaces, each grown alone: an interface of N
// methods that a type implements and N calls through it (a call finds its
// itab slot by one lookup; the control scans for it, LowerOptions::
// ScanItabSlots, and must fail the same check), N types converted to one
// interface (a descriptor, an itab and a thunk each), N comma-ok assertions
// and conversions between interfaces, one type of N methods asserted to N
// interfaces of one method each, an interface that embeds N others, and a
// type nested N deep in an `any` (its name is built once).

/// An interface of N methods, a type that has them, and N calls through it.
std::string ifaceMethodsOfN(unsigned N) {
  std::string S = "package main\n\ntype I interface {\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tM" + std::to_string(I) + "() int\n";
  S += "}\n\ntype T struct{ x int }\n\n";
  for (unsigned I = 0; I < N; ++I)
    S += "func (t T) M" + std::to_string(I) + "() int { return t.x + " +
         std::to_string(I) + " }\n";
  S += "\nfunc main() {\n\tvar i I = T{1}\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tt += i.M" + std::to_string(I) + "()\n";
  return S + "\tprintln(t)\n}\n";
}

/// N types with a method, each converted to one interface and called.
std::string typesToIfaceOfN(unsigned N) {
  std::string S = "package main\n\ntype I interface{ M() int }\n\n";
  for (unsigned I = 0; I < N; ++I)
    S += "type T" + std::to_string(I) + " struct{ x int }\n\nfunc (t T" +
         std::to_string(I) + ") M() int { return t.x + " + std::to_string(I) +
         " }\n";
  S += "\nfunc main() {\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tt += I(T" + std::to_string(I) + "{1}).M()\n";
  return S + "\tprintln(t)\n}\n";
}

/// N conversions between interfaces and N assertions, comma-ok.
std::string assertionsOfN(unsigned N) {
  std::string S = "package main\n\ntype A interface{ M() int }\ntype B "
                  "interface {\n\tA\n\tN() int\n}\n\ntype T struct{ x "
                  "int }\n\nfunc (t T) M() int { return t.x }\nfunc (t T) "
                  "N() int { return 2 }\n\nfunc main() {\n\tvar b B = "
                  "T{1}\n\tvar x any = b\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tvar a" + std::to_string(I) + " A = b\n\tt += a" +
         std::to_string(I) +
         ".M()\n\tif v, ok := x.(B); ok {\n\t\tt += v.N()\n\t}\n\tif v, ok := "
         "x.(T); ok {\n\t\tt += v.x\n\t}\n";
  return S + "\tprintln(t)\n}\n";
}

/// One type with N methods and N interfaces of one method each; the value is
/// converted to each and asserted from an `any`.
std::string ifaceInterfacesOfN(unsigned N) {
  std::string S = "package main\n\ntype T struct{ n int }\n\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    S += "func (t T) M" + K + "() int { return t.n + " + K + " }\n\ntype I" +
         K + " interface{ M" + K + "() int }\n\n";
  }
  S += "func main() {\n\tvar x any = T{1}\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    S += "\tif v, ok := x.(I" + K + "); ok {\n\t\tt += v.M" + K + "()\n\t}\n";
  }
  return S + "\tprintln(t)\n}\n";
}

/// An interface that embeds N others, one method each, and a call of each.
std::string ifaceEmbedsOfN(unsigned N) {
  std::string S = "package main\n\ntype T struct{ n int }\n\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    S += "func (t T) M" + K + "() int { return " + K + " }\n\ntype E" + K +
         " interface{ M" + K + "() int }\n\n";
  }
  S += "type All interface {\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tE" + std::to_string(I) + "\n";
  S += "}\n\nfunc main() {\n\tvar a All = T{1}\n\tt := 0\n";
  for (unsigned I = 0; I < N; ++I)
    S += "\tt += a.M" + std::to_string(I) + "()\n";
  return S + "\tprintln(t)\n}\n";
}

/// A type nested N deep ([][]...[]int) in an `any`, asserted back.
std::string ifaceDeepTypeOfN(unsigned N) {
  std::string Ty;
  for (unsigned I = 0; I < N; ++I)
    Ty += "[]";
  return "package main\n\nfunc main() {\n\tvar x " + Ty +
         "int\n\tvar a any = x\n\t_, ok := a.(" + Ty +
         "int)\n\tprintln(ok)\n}\n";
}

/// N types and N interfaces, every type has every method: the size of what
/// is lowered is the product, so the GIR check must fail (the control).
std::string ifaceProductOfN(unsigned N) {
  std::string S = "package main\n\n";
  for (unsigned J = 0; J < N; ++J)
    S += "type I" + std::to_string(J) + " interface{ M" + std::to_string(J) +
         "() int }\n\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string K = std::to_string(I);
    S += "type T" + K + " struct{ n int }\n\n";
    for (unsigned J = 0; J < N; ++J)
      S += "func (t T" + K + ") M" + std::to_string(J) + "() int { return " +
           std::to_string(J) + " }\n\n";
  }
  S += "func main() {\n\tt := 0\n\tvar x any\n";
  for (unsigned I = 0; I < N; ++I) {
    S += "\tx = T" + std::to_string(I) + "{1}\n";
    S += "\tif v, ok := x.(I" + std::to_string(I) + "); ok {\n\t\tt += v.M" +
         std::to_string(I) + "()\n\t}\n";
  }
  return S + "\tprintln(t)\n}\n";
}

TEST(GIRStress, TheLoweringCountsItabsAndDescriptors) {
  expectWalksFollow(20, ifaceMethodsOfN);
  expectWalksFollow(20, typesToIfaceOfN);
}
TEST(GIRStress, TheLoweringCountsAssertionsAndConversions) {
  expectWalksFollow(20, assertionsOfN);
}
/// A type of N methods boxed in an `any` (its descriptor lists all of them:
/// the checker's method set, one lookup per name, review R2).
std::string boxedMethodsOfN(unsigned N) {
  std::string S = "package main\n\ntype T struct{ n int }\n\n";
  for (unsigned I = 0; I < N; ++I)
    S += "func (t T) M" + std::to_string(I) + "() int { return t.n + " +
         std::to_string(I) + " }\n";
  return S + "\nfunc main() {\n\tvar a any = T{1}\n\t_ = a\n}\n";
}

TEST(GIRStress, TheLoweringCountsTheMethodSetOfABoxedType) {
  expectWalksFollow(20, boxedMethodsOfN);
}
// The control: the checker's lookup by name scanning the type's methods (the
// quadratic method set of review R2, LowerOptions::ScanMethodLookups) must
// fail the same check, which counts the lookups' steps.
TEST(GIRStress, TheMethodSetCheckFailsAScanForEachName) {
  LowerOptions Scan;
  Scan.ScanMethodLookups = true;
  uint64_t S1 = lowerAndCountWalks(20, boxedMethodsOfN, Scan);
  uint64_t S16 = lowerAndCountWalks(320, boxedMethodsOfN, Scan);
  EXPECT_FALSE(walksFollow(S1, S16, 20)) << S1 << " -> " << S16;
}
TEST(GIRStress, TheItabCheckFailsAScanForTheSlot) {
  LowerOptions Scan;
  Scan.ScanItabSlots = true;
  uint64_t S1 = lowerAndCountWalks(20, ifaceMethodsOfN, Scan);
  uint64_t S16 = lowerAndCountWalks(320, ifaceMethodsOfN, Scan);
  EXPECT_FALSE(walksFollow(S1, S16, 20)) << S1 << " -> " << S16;
}
TEST(GIRStress, LoweringManyInterfacesOfOneTypeIsLinear) {
  expectLoweredStepsLinear(100, ifaceInterfacesOfN);
}
TEST(GIRStress, LoweringAnInterfaceThatEmbedsManyIsLinear) {
  expectLoweredStepsLinear(100, ifaceEmbedsOfN);
}
TEST(GIRStress, LoweringADeepTypeInAnInterfaceIsLinear) {
  expectLoweredStepsLinear(200, ifaceDeepTypeOfN);
}
TEST(GIRStress, TheInterfaceProductControlFailsTheLinearCheck) {
  uint64_t S1 = lowerAndCount(10, ifaceProductOfN),
           S4 = lowerAndCount(40, ifaceProductOfN);
  EXPECT_GT(2 * S4, 9 * S1) << S1 << " -> " << S4;
}

// P04.05 review R15: the map-entry arguments of one call make GIR linear in
// their number (one record each, one runtime call that resolves the
// aliasing). One call with N entry arguments; the dimension is counted on
// the GIR made (the verifier's steps, past the size of the program with one
// such argument, which holds the runtime) and on the lowering's steps. The
// control emits GIR for every pair (LowerOptions::PairwiseEntryArgs) and
// must fail the same check.
std::string entryArgsOfN(unsigned N) {
  std::string P, A;
  for (unsigned I = 0; I < N; ++I) {
    P += (I ? ", a" : "a") + std::to_string(I);
    A += (I ? ", mm[" : "mm[") + std::to_string(I % 7) + "]";
  }
  return "package main\n\nfunc f(" + P +
         " map[int]int) {\n\ta0[1] = 1\n}\n\nfunc main() {\n\tmm := "
         "map[int]map[int]int{}\n\tf(" +
         A + ")\n\tprintln(len(mm))\n}\n";
}

/// The verifier's steps on the lowered program of N entry arguments.
uint64_t entryArgsGIR(unsigned N, const LowerOptions &Opts) {
  Checked P(entryArgsOfN(N));
  EXPECT_EQ(P.errors(), "") << "N = " << N;
  gir::Context C;
  auto M = P.lower(C, Opts);
  EXPECT_NE(M, nullptr) << P.errors();
  if (!M)
    return 0;
  uint64_t A = verifierSteps();
  EXPECT_TRUE(verify(*M).empty());
  return verifierSteps() - A;
}

/// The GIR of N and of 16N entry arguments, past the program of one.
::testing::AssertionResult entryArgsFollow(unsigned N,
                                           const LowerOptions &Opts) {
  uint64_t B = entryArgsGIR(1, Opts), S1 = entryArgsGIR(N, Opts),
           S16 = entryArgsGIR(16 * N, Opts);
  return walksFollow(S1 - B, S16 - B, N);
}

TEST(GIRStress, TheLoweringOfEntryArgumentsIsLinear) {
  EXPECT_TRUE(entryArgsFollow(20, {}));
  expectWalksFollow(20, entryArgsOfN);
}

TEST(GIRStress, TheEntryArgumentCheckFailsAPairwiseEmission) {
  LowerOptions Pairwise;
  Pairwise.PairwiseEntryArgs = true;
  EXPECT_FALSE(entryArgsFollow(20, Pairwise));
}

// The timed version is long (tests/gir/core-stress-str.long.test).
// (In three parts, one lit file each, so the sanitizer build runs them side
// by side: tests/gir/core-stress-str*.long.test.)
TEST(GIRStressLong, DISABLED_LoweringConcatsAndLiterals) {
  expectLoweringNearLinear(240, concatChainOfN);
  expectLoweringNearLinear(3000, literalsOfN);
}
TEST(GIRStressLong, DISABLED_LoweringSlicing) {
  expectLoweringNearLinear(240, slicingChainOfN);
  expectLoweringNearLinear(1000, slicingStatementsOfN);
}
TEST(GIRStressLong, DISABLED_LoweringMapsAndAppends) {
  expectLoweringNearLinear(1000, mapOpsOfN);
  expectLoweringNearLinear(1000, mapRangesOfN);
  expectLoweringNearLinear(1000, sliceOpsOfN);
}

// R15, timed: 4N entry arguments of one call lower in at most 12 times the
// time of N; the pairwise emission must fail the same check at 8 times the
// size (linear 8, quadratic 64, limit 24), measured once, as the other
// controls.
void expectEntryArgsTiming(unsigned N, bool Pairwise) {
  unsigned Scale = Pairwise ? 8 : 4;
  Checked P1(entryArgsOfN(N)), PS(entryArgsOfN(Scale * N));
  LowerOptions Opts;
  Opts.PairwiseEntryArgs = Pairwise;
  auto Lower = [&Opts](Checked &P) {
    return [&P, &Opts] {
      gir::Context C;
      auto M = P.lower(C, Opts);
      EXPECT_NE(M, nullptr);
    };
  };
  if (Pairwise) {
    auto [T1, T8] = bestOfBothOnce(Lower(P1), Lower(PS), 8);
    EXPECT_GE(T8, nearLinearLimit(T1, 8)) << T1 << " s, " << T8 << " s";
  } else {
    auto [T1, T4] = bestOfBoth(Lower(P1), Lower(PS));
    EXPECT_LT(T4, nearLinearLimit(T1)) << T1 << " s, " << T4 << " s";
  }
}

TEST(GIRStressLong, DISABLED_LoweringManyEntryArguments) {
  expectEntryArgsTiming(250, false);
}
// The control is sized for itself: at 100 arguments one lowering of the small
// side takes about 20 ms, the floor bestOfBothOnce calibrates to, so the five
// rounds of the 64 times bigger side cost 8 s in the ASan build (at 250: 114
// ms a run and 23 s), and the ratio stays as far over the limit (about 40
// against 24 there, 41 in the default build).
TEST(GIRStressLong, DISABLED_TheEntryArgumentTimingFailsAPairwiseEmission) {
  expectEntryArgsTiming(100, true);
}

// The control of the lowering timing checks: measured once (bestOfBothOnce;
// bestOfBoth measures again while the pair is over the limit, which for a
// control only looks for a pass), at eight times the size as the other
// controls (linear 8, quadratic 64, limit 24: at four times, 16 against 12
// left too little room on a busy machine).
TEST(GIRStressLong, DISABLED_TheLoweringTimingCheckFailsAQuadraticProgram) {
  Checked P1(concatSquareOfN(100)), P8(concatSquareOfN(800));
  auto Lower = [](Checked &P) {
    return [&P] {
      gir::Context C;
      auto M = P.lower(C);
      EXPECT_NE(M, nullptr);
    };
  };
  auto [T1, T8] = bestOfBothOnce(Lower(P1), Lower(P8), 8);
  EXPECT_GE(T8, nearLinearLimit(T1, 8)) << T1 << " s, " << T8 << " s";
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
