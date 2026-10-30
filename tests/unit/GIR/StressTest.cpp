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
// functions, and four times the input must take at most eight times as long
// (a quadratic walk takes sixteen). Each time is the best of three runs, to
// ignore a noisy machine; below 50 ms the ratio is noise.
//
//===----------------------------------------------------------------------===//

#include "../Support/GIRTestSupport.h"

#include <algorithm>
#include <chrono>

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

/// The best of three wall-clock times of Run, in seconds.
template <class Fn> double best(Fn Run) {
  double B = 1e9;
  for (int I = 0; I < 3; ++I) {
    auto T0 = std::chrono::steady_clock::now();
    Run();
    B = std::min(
        B, std::chrono::duration<double>(std::chrono::steady_clock::now() - T0)
               .count());
  }
  return B;
}

/// Verifying a module of 4N units takes at most 8 times as long as one of N.
void expectVerifierNearLinear(unsigned N,
                              void (*Make)(GoodModule &, unsigned)) {
  auto Time = [&](unsigned Size) {
    GoodModule G;
    Make(G, Size);
    EXPECT_GIR_VALID(G.M);
    return best([&] { EXPECT_TRUE(verify(G.M).empty()); });
  };
  double T1 = Time(N), T4 = Time(4 * N);
  EXPECT_LT(T4, std::max(8 * T1, 0.05))
      << "T(N) = " << T1 << " s, T(4N) = " << T4 << " s";
}

TEST(GIRStress, VerifierOnALadderOf80000Blocks) {
  expectVerifierNearLinear(20000, ladder);
}

TEST(GIRStress, VerifierOnWideCleanupRegions) {
  expectVerifierNearLinear(20000, cleanups);
}

TEST(GIRStress, VerifierOnManyFunctions) {
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
  std::string A1 = Text(10000), A2 = Text(10000), B = Text(40000);
  EXPECT_EQ(A1, A2); // deterministic
  double R = double(B.size()) / double(A1.size());
  EXPECT_GT(R, 3.8);
  EXPECT_LT(R, 4.2);
  double T1 = best([&] { Text(10000); }), T4 = best([&] { Text(40000); });
  EXPECT_LT(T4, std::max(8 * T1, 0.05)) << T1 << " s, " << T4 << " s";
}

} // namespace
