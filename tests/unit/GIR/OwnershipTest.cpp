//===- OwnershipTest.cpp - owner states, use after move, drop elaboration -===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.09. OwnerStates (the persistent sets both passes keep per block), the
// use-after-move check on hand-built graphs (both sides: the bad shape is
// reported, the nearby good shape is not), drop elaboration of the same
// graphs, and the generated stress tests: thousands of sibling and nested
// owners, steps counted (never the clock) at N and 4N, each with a control
// that runs a quadratic walk and must fail the same check.
//
//===----------------------------------------------------------------------===//

#include "../Support/GIRTestSupport.h"

#include "gopp/GIR/DropElab.h"
#include "gopp/GIR/MoveCheck.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::test;

namespace {

TEST(OwnerStates, AddRemoveContainsAndHashConsing) {
  OwnerStates S;
  OwnerState A = S.add(OwnerStates::Empty, 3);
  OwnerState B = S.add(A, 5);
  EXPECT_TRUE(S.contains(B, 3));
  EXPECT_TRUE(S.contains(B, 5));
  EXPECT_FALSE(S.contains(B, 4));
  EXPECT_EQ(S.size(B), 2u);
  // The same set reached another way is the same state.
  EXPECT_EQ(S.add(S.add(OwnerStates::Empty, 5), 3), B);
  EXPECT_EQ(S.remove(B, 5), A);
  EXPECT_EQ(S.remove(S.remove(B, 5), 3), OwnerStates::Empty);
  llvm::SmallVector<LocalId, 4> M;
  S.members(B, M);
  ASSERT_EQ(M.size(), 2u);
  EXPECT_EQ(M[0], 5u); // the one made last first: the drop order
  EXPECT_EQ(M[1], 3u);
}

TEST(OwnerStates, UnionAndRanges) {
  OwnerStates S;
  OwnerState X = S.add(S.add(OwnerStates::Empty, 1), 4);
  OwnerState Y = S.add(S.add(OwnerStates::Empty, 2), 4);
  OwnerState U = S.unite(X, Y);
  EXPECT_EQ(S.size(U), 3u);
  EXPECT_EQ(S.unite(Y, X), U);
  EXPECT_EQ(S.unite(U, X), U);
  EXPECT_TRUE(S.anyInRange(U, 2, 3));
  EXPECT_FALSE(S.anyInRange(U, 5, 9));
  EXPECT_FALSE(S.anyInRange(U, 3, 4));
  llvm::SmallVector<LocalId, 4> R;
  S.rangeMembers(U, 2, 5, R);
  ASSERT_EQ(R.size(), 2u);
  EXPECT_EQ(R[0], 4u);
  EXPECT_EQ(R[1], 2u);
}

/// A function with a named string variable `s` and the shapes below.
struct MoveFixture {
  Context C;
  Module M{C, "test"};
  Type *Str = C.string();
  Type *Bool = C.boolean();
  FuncId F = declare(M, "f", C.func({Bool}, C.unit()));
  FuncBuilder B{M, M.func(F)};
  LocalId S, T;
  MoveFixture() {
    S = B.newLocal(Str, Local::Var, "s");
    T = B.newTemp(Str);
    M.func(F).Locals[S].Source = true;
  }
  Span at(uint32_t Line) {
    Span Sp;
    Sp.File = 1;
    Sp.Line = Line;
    Sp.Col = 1;
    return Sp;
  }
  void def() {
    B.setSpan(at(1));
    B.storageLive(S);
    B.assign(B.local(S), B.use(constStr(M, "x")));
  }
  void take(uint32_t Line) {
    B.setSpan(at(Line));
    B.assign(B.local(T), B.use(moveOf(B.local(S))));
  }
  void read(uint32_t Line) {
    B.setSpan(at(Line));
    B.assign(B.local(T), B.use(copyOf(B.local(S))));
  }
  std::vector<MoveError> run() { return checkMoves(M.func(F)); }
};

TEST(MoveCheck, UseAfterMoveIsReportedAndFreshValueIsNot) {
  {
    MoveFixture X; // bad: read after the move
    X.def();
    X.take(2);
    X.read(3);
    X.B.ret();
    auto E = X.run();
    ASSERT_EQ(E.size(), 1u);
    EXPECT_EQ(E[0].Use.Line, 3u);
    EXPECT_EQ(E[0].Moved.Line, 2u);
    EXPECT_EQ(E[0].Local, X.S);
  }
  {
    MoveFixture X; // good: written again between
    X.def();
    X.take(2);
    X.B.setSpan(X.at(3));
    X.B.assign(X.B.local(X.S), X.B.use(constStr(X.M, "y")));
    X.read(4);
    X.B.ret();
    EXPECT_TRUE(X.run().empty());
  }
  {
    MoveFixture X; // good: the move is the last use
    X.def();
    X.read(2);
    X.take(3);
    X.B.ret();
    EXPECT_TRUE(X.run().empty());
  }
}

TEST(MoveCheck, OneMovingBranchMakesTheJoinMoved) {
  for (bool BothSides : {false, true}) {
    MoveFixture X;
    X.def();
    BlockId Then = X.B.newBlock(), Else = X.B.newBlock(), Join = X.B.newBlock();
    X.B.branch(copyOf(X.B.local(1)), Then, Else);
    X.B.setBlock(Then);
    X.take(2);
    X.B.gotoBlock(Join);
    X.B.setBlock(Else);
    if (BothSides)
      X.take(3);
    X.B.gotoBlock(Join);
    X.B.setBlock(Join);
    X.read(4);
    X.B.ret();
    // Moved on one path or on both: either way the join reads a moved value.
    EXPECT_EQ(X.run().size(), 1u) << BothSides;
  }
  MoveFixture X; // good: neither branch moves
  X.def();
  BlockId Then = X.B.newBlock(), Join = X.B.newBlock();
  X.B.branch(copyOf(X.B.local(1)), Then, Join);
  X.B.setBlock(Then);
  X.read(2);
  X.B.gotoBlock(Join);
  X.B.setBlock(Join);
  X.read(3);
  X.B.ret();
  EXPECT_TRUE(X.run().empty());
}

TEST(MoveCheck, TheSecondTripOfALoopReadsTheMovedValue) {
  MoveFixture X;
  X.def();
  BlockId Head = X.B.newBlock(), Exit = X.B.newBlock();
  X.B.gotoBlock(Head);
  X.B.setBlock(Head);
  X.take(2); // reads s, then moves it
  X.B.branch(copyOf(X.B.local(1)), Head, Exit);
  X.B.setBlock(Exit);
  X.B.ret();
  EXPECT_EQ(X.run().size(), 1u);
}

TEST(MoveCheck, StorageLiveStartsAFreshVariable) {
  MoveFixture X; // good: a new variable each trip
  BlockId Head = X.B.newBlock(), Exit = X.B.newBlock();
  X.B.gotoBlock(Head);
  X.B.setBlock(Head);
  X.def();
  X.take(2);
  X.B.storageDead(X.S);
  X.B.branch(copyOf(X.B.local(1)), Head, Exit);
  X.B.setBlock(Exit);
  X.B.ret();
  EXPECT_TRUE(X.run().empty());
}

TEST(MoveCheck, APartMovedMakesTheWholeUnreadableButNotItsSibling) {
  Context C;
  Module M{C, "test"};
  Type *Str = C.string();
  Type *Pair = C.tuple({Str, Str});
  FuncId F = declare(M, "f", C.func({}, C.unit()));
  FuncBuilder B(M, M.func(F));
  LocalId P = B.newLocal(Pair, Local::Var, "p");
  LocalId T = B.newTemp(Str), W = B.newTemp(Pair);
  M.func(F).Locals[P].Source = true;
  B.storageLive(P);
  B.assign(B.local(T), B.use(moveOf(B.field(B.local(P), 0))));
  B.assign(B.local(T), B.use(copyOf(B.field(B.local(P), 1)))); // good
  EXPECT_TRUE(checkMoves(M.func(F)).empty());
  B.assign(B.local(W), B.use(copyOf(B.local(P)))); // the whole: bad
  B.ret();
  EXPECT_EQ(checkMoves(M.func(F)).size(), 1u);
}

/// N scoped variables, each moved in one arm of a diamond and dead at the
/// join: the states stay small.
void scopedDiamonds(MoveFixture &X, unsigned N) {
  X.def();
  X.B.storageDead(X.S);
  for (unsigned I = 0; I < N; ++I) {
    LocalId V = X.B.newLocal(X.Str, Local::Var, "v" + std::to_string(I));
    X.M.func(X.F).Locals[V].Source = true;
    BlockId Then = X.B.newBlock(), Else = X.B.newBlock(), Join = X.B.newBlock();
    X.B.storageLive(V);
    X.B.assign(X.B.local(V), X.B.use(constStr(X.M, "x")));
    X.B.branch(copyOf(X.B.local(1)), Then, Else);
    X.B.setBlock(Then);
    X.B.assign(X.B.local(X.T), X.B.use(moveOf(X.B.local(V))));
    X.B.gotoBlock(Join);
    X.B.setBlock(Else);
    X.B.gotoBlock(Join);
    X.B.setBlock(Join);
    X.B.storageDead(V);
  }
  X.B.ret();
}

/// Counting (the normal suite never compares clocks): N units cost at most C
/// per unit and 4N units at most 4.5 times as much (quadratic work is 16x).
::testing::AssertionResult linear(uint64_t S1, uint64_t S4, uint64_t N) {
  const uint64_t C = 40;
  if (S1 == 0 || S1 > C * N || S4 > C * 4 * N || 2 * S4 > 9 * S1)
    return ::testing::AssertionFailure()
           << "steps(N) = " << S1 << ", steps(4N) = " << S4 << ", N = " << N;
  return ::testing::AssertionSuccess();
}

uint64_t ownershipSteps() { return moveCheckSteps() + dropElabSteps(); }

TEST(OwnershipStress, MoveCheckOnThousandsOfScopedDiamonds) {
  const unsigned N = 2000;
  MoveFixture A, B;
  scopedDiamonds(A, N);
  scopedDiamonds(B, 4 * N);
  uint64_t S0 = ownershipSteps();
  EXPECT_TRUE(A.run().empty());
  uint64_t S1 = ownershipSteps();
  EXPECT_TRUE(B.run().empty());
  uint64_t S2 = ownershipSteps();
  EXPECT_TRUE(linear(S1 - S0, S2 - S1, N));
}

/// N variables alive at once, each moved and read again (so each read is a
/// question about a state of many moved paths).
void liveMoves(MoveFixture &X, unsigned N) {
  X.def();
  llvm::SmallVector<LocalId, 16> Vs;
  for (unsigned I = 0; I < N; ++I) {
    LocalId V = X.B.newLocal(X.Str, Local::Var, "v" + std::to_string(I));
    X.M.func(X.F).Locals[V].Source = true;
    X.B.storageLive(V);
    X.B.assign(X.B.local(V), X.B.use(constStr(X.M, "x")));
    X.B.assign(X.B.local(X.T), X.B.use(moveOf(X.B.local(V))));
    X.B.assign(X.B.local(V), X.B.use(constStr(X.M, "y")));
    X.B.assign(X.B.local(X.T), X.B.use(copyOf(X.B.local(V))));
    Vs.push_back(V);
  }
  X.B.ret();
}

/// The control (manager decision M6): the pass itself in its quadratic mode,
/// where every read looks at every moved path, fails the same count.
TEST(OwnershipStress, TheMoveCheckCountFailsItsRescanMode) {
  const unsigned N = 1000;
  for (bool Rescan : {false, true}) {
    MoveFixture A, B;
    liveMoves(A, N);
    liveMoves(B, 4 * N);
    uint64_t S0 = moveCheckSteps();
    EXPECT_TRUE(checkMoves(A.M.func(A.F), Rescan).empty());
    uint64_t S1 = moveCheckSteps();
    EXPECT_TRUE(checkMoves(B.M.func(B.F), Rescan).empty());
    uint64_t S2 = moveCheckSteps();
    EXPECT_EQ(bool(linear(S1 - S0, S2 - S1, N)), !Rescan) << Rescan;
  }
}

/// N variables alive at once, each moved in one arm of its own diamond: after
/// each join the maybe-moved set is the one before with one more path (P04.09
/// R9: `s_i := ...; if c { take(move(s_i)) }`, 32000 of them took 7.3 s). The
/// variables are all made first, then moved in the given order: forward,
/// reverse (the new path below every other, review round 2: 16000 took 28 s)
/// or a fixed shuffle.
enum class MoveOrder { Forward, Reverse, Shuffled };
void liveDiamonds(MoveFixture &X, unsigned N, MoveOrder Order) {
  X.def();
  X.B.storageDead(X.S);
  llvm::SmallVector<LocalId, 16> Vs;
  for (unsigned I = 0; I < N; ++I) {
    LocalId V = X.B.newLocal(X.Str, Local::Var, "v" + std::to_string(I));
    X.M.func(X.F).Locals[V].Source = true;
    X.B.storageLive(V);
    X.B.assign(X.B.local(V), X.B.use(constStr(X.M, "x")));
    Vs.push_back(V);
  }
  llvm::SmallVector<LocalId, 16> Moved(Vs.begin(), Vs.end());
  if (Order == MoveOrder::Reverse)
    std::reverse(Moved.begin(), Moved.end());
  if (Order == MoveOrder::Shuffled) {
    uint64_t R = 0x2545F4914F6CDD1Dull;
    for (size_t K = Moved.size(); K > 1; --K) {
      R ^= R << 13;
      R ^= R >> 7;
      R ^= R << 17;
      std::swap(Moved[K - 1], Moved[R % K]);
    }
  }
  for (LocalId V : Moved) {
    BlockId Then = X.B.newBlock(), Else = X.B.newBlock(), Join = X.B.newBlock();
    X.B.branch(copyOf(X.B.local(1)), Then, Else);
    X.B.setBlock(Then);
    X.B.assign(X.B.local(X.T), X.B.use(moveOf(X.B.local(V))));
    X.B.gotoBlock(Join);
    X.B.setBlock(Else);
    X.B.gotoBlock(Join);
    X.B.setBlock(Join);
  }
  for (size_t K = Vs.size(); K > 0; --K)
    X.B.storageDead(Vs[K - 1]);
  X.B.ret();
}

/// A join of a set with itself plus a path costs the nodes where the two
/// differ (MoveCheck's radix trie of paths), so this shape is linear in any
/// order; the control is the pass in the quadratic mode of exactly those
/// joins (every union walks both sets whole), which fails the same count.
/// (Two units per variable: its move and its join.)
TEST(OwnershipStress, LiveVariablesMovedOnBranchesAreLinear) {
  const unsigned N = 1000;
  for (MoveOrder Order :
       {MoveOrder::Forward, MoveOrder::Reverse, MoveOrder::Shuffled})
    for (bool FullJoins : {false, true}) {
      MoveFixture A, B;
      liveDiamonds(A, N, Order);
      liveDiamonds(B, 4 * N, Order);
      uint64_t S0 = ownershipSteps();
      EXPECT_TRUE(checkMoves(A.M.func(A.F), false, FullJoins).empty());
      uint64_t S1 = ownershipSteps();
      EXPECT_TRUE(checkMoves(B.M.func(B.F), false, FullJoins).empty());
      uint64_t S2 = ownershipSteps();
      EXPECT_EQ(bool(linear(S1 - S0, S2 - S1, 2 * N)), !FullJoins)
          << int(Order) << " " << FullJoins << " " << (S1 - S0) << " "
          << (S2 - S1);
    }
}

/// N owned temporaries made and dropped in sequence, each in a block of its
/// own: the elaborated function has N drops and the pass costs a bounded
/// amount each.
void ownedSequence(GoodModule &G, FuncId Glue, unsigned N, Function *&Out) {
  FuncId F = declare(G.M, "seq", G.C.func({}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  for (unsigned I = 0; I < N; ++I) {
    LocalId T = B.newTemp(G.C.string());
    G.fn(F).Locals[T].Owned = true;
    G.fn(F).Locals[T].Glue = Glue;
    B.storageLive(T);
    B.assign(B.local(T), B.use(constStr(G.M, "x")));
    B.storageDead(T);
    BlockId Next = B.newBlock();
    B.gotoBlock(Next);
    B.setBlock(Next);
  }
  B.ret();
  Out = &G.fn(F);
}

TEST(DropElab, MakesADropForEachOwnerAndIsLinear) {
  auto Run = [](unsigned N, uint64_t &Steps, unsigned &Drops,
                bool Rescan = false) {
    GoodModule G;
    FuncId Glue = declare(
        G.M, "glue", G.C.func({G.C.ptr(G.U8)}, G.Unit, false, CallConv::Go));
    {
      FuncBuilder GB(G.M, G.fn(Glue));
      GB.ret();
    }
    Function *Fn;
    ownedSequence(G, Glue, N, Fn);
    uint64_t Before = dropElabSteps();
    std::string Err;
    EXPECT_TRUE(elaborateDrops(G.M, *Fn, &Err, Rescan)) << Err;
    Steps = dropElabSteps() - Before;
    Drops = 0;
    for (const BasicBlock &BB : Fn->Blocks)
      if (BB.Term.K == Terminator::Drop)
        ++Drops;
    EXPECT_TRUE(verify(G.M).empty());
  };
  uint64_t S1, S4;
  unsigned D1, D4;
  Run(1000, S1, D1);
  Run(4000, S4, D4);
  EXPECT_EQ(D1, 1000u); // one drop per owner, no more
  EXPECT_EQ(D4, 4000u);
  EXPECT_TRUE(linear(S1, S4, 1000));
  // The control (manager decision M6): the pass in its quadratic mode (the
  // owner states computed again at every block) fails the same count.
  Run(300, S1, D1, /*Rescan=*/true);
  Run(1200, S4, D4, /*Rescan=*/true);
  EXPECT_FALSE(linear(S1, S4, 300));
}

} // namespace
