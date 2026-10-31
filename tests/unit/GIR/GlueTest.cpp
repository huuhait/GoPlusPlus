//===- GlueTest.cpp - Equality and hash glue, run through the JIT ---------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.04. The glue the compiler writes for == and for map keys (Glue.h) is
// made from the GIR type alone, lowered for the host and run in LLJIT. The
// cases are the ones the language fixes: equal values hash equal, an Option's
// flag padding and (when it is unset) its payload take no part, -0 and +0
// are one key and every NaN is one hash though NaN != NaN, struct padding is
// ignored, and each of these has a nearby case that must differ.
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Glue.h"
#include "gopp/Basic/NewStack.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Verify.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/TargetSelect.h"

#include "../Support/GIRTestSupport.h"
#include "llvm/Target/TargetMachine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::gir;

namespace {

struct Jit {
  // The glue is Go++ code: called with the Go++ convention (GOPP_GO_CC).
  using EqFn = bool(GOPP_GO_CC *)(const void *, const void *);
  using HashFn = uint64_t(GOPP_GO_CC *)(const void *, uint64_t);
  std::unique_ptr<llvm::orc::LLJIT> J;
  bool ok = false;

  /// Lowers M for the host, adds it to the JIT. False if the host is not a
  /// supported target (the test then has nothing to run on).
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
    EXPECT_TRUE(static_cast<bool>(Made)) << llvm::toString(Made.takeError());
    if (!Made)
      return;
    J = std::move(*Made);
    llvm::Error E = J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(*LM), std::move(Ctx)));
    EXPECT_FALSE(static_cast<bool>(E)) << llvm::toString(std::move(E));
    ok = !E;
  }
  template <class Fn> Fn find(const char *Name) {
    auto Addr = J->lookup(Name);
    EXPECT_TRUE(static_cast<bool>(Addr)) << llvm::toString(Addr.takeError());
    return Addr ? Addr->toPtr<Fn>() : nullptr;
  }
};

/// A module with the eq and hash glue of T under the names eq_<Tag> and
/// hash_<Tag>.
struct Glued {
  Context C;
  Module M{C, "glue"};
  void add(Type *T, const char *Tag) {
    ASSERT_TRUE(hasGlue(T));
    ASSERT_NE(addEqGlue(M, T, std::string("eq_") + Tag), NoId);
    ASSERT_NE(addHashGlue(M, T, std::string("hash_") + Tag), NoId);
  }
};

template <class T> struct Bytes {
  alignas(8) unsigned char B[sizeof(T) < 64 ? 64 : sizeof(T)];
  explicit Bytes(unsigned char Fill) { std::memset(B, Fill, sizeof B); }
  void *get() { return B; }
};

TEST(Glue, InterfacesHaveGlueAndAreFoundInsideOtherTypes) {
  // P04.06: an interface value is compared and hashed through the runtime
  // (gopp.iface_eq, gopp.iface_hash), so the glue of a type that holds one
  // exists, and hasIface tells the lowering to make those functions first.
  Context C;
  Type *I = C.iface();
  EXPECT_TRUE(I->isIface());
  EXPECT_TRUE(hasGlue(I));
  EXPECT_TRUE(hasIface(I));
  Type *S = C.structTy("glue.Holder");
  C.setStructBody(S, {C.i64(), I});
  EXPECT_TRUE(hasGlue(S));
  EXPECT_TRUE(hasIface(S));
  EXPECT_TRUE(hasIface(C.array(S, 3)));
  EXPECT_TRUE(hasIface(C.option(I)));
  // The other side: nothing in a plain type is an interface, and a pointer
  // or a string is not one.
  Type *P = C.structTy("glue.Plain");
  C.setStructBody(P, {C.i64(), C.string(), C.ptr(I)});
  EXPECT_FALSE(hasIface(P));
  EXPECT_FALSE(hasIface(C.array(C.i64(), 3)));
  EXPECT_FALSE(hasIface(C.tuple({C.floatTy(64), C.boolean()})));
}

TEST(Glue, OptionIgnoresFlagPaddingAndAnUnsetPayload) {
  Glued G;
  Type *O = G.C.option(G.C.i64());
  G.add(O, "o64");
  Jit J(G.M);
  if (!J.ok)
    GTEST_SKIP() << "the host is not a supported target";
  auto Eq = J.find<Jit::EqFn>("eq_o64");
  auto Hash = J.find<Jit::HashFn>("hash_o64");
  ASSERT_TRUE(Eq && Hash);
  // {payload i64, flag u8, 7 bytes of padding}
  auto Make = [](int64_t V, uint8_t Flag, unsigned char Junk) {
    Bytes<int64_t[2]> X(Junk);
    std::memcpy(X.B, &V, 8);
    X.B[8] = Flag;
    return X;
  };
  auto A = Make(5, 1, 0x00), B = Make(5, 1, 0xAB); // equal, other padding
  EXPECT_TRUE(Eq(A.get(), B.get()));
  EXPECT_EQ(Hash(A.get(), 7), Hash(B.get(), 7));
  auto C = Make(5, 0, 0x11), D = Make(9, 0, 0x22); // both unset
  EXPECT_TRUE(Eq(C.get(), D.get()));
  EXPECT_EQ(Hash(C.get(), 7), Hash(D.get(), 7));
  // The nearby cases that must differ.
  EXPECT_FALSE(Eq(A.get(), C.get())); // set against unset
  EXPECT_NE(Hash(A.get(), 7), Hash(C.get(), 7));
  auto E = Make(6, 1, 0x00); // set to another payload
  EXPECT_FALSE(Eq(A.get(), E.get()));
  EXPECT_NE(Hash(A.get(), 7), Hash(E.get(), 7));
  EXPECT_NE(Hash(A.get(), 7), Hash(A.get(), 8)); // the seed counts
}

TEST(Glue, FloatsZeroesAndNaNs) {
  Glued G;
  Type *F = G.C.floatTy(64);
  G.add(F, "f64");
  Type *OF = G.C.option(F);
  G.add(OF, "of64");
  Jit J(G.M);
  if (!J.ok)
    GTEST_SKIP() << "the host is not a supported target";
  auto Eq = J.find<Jit::EqFn>("eq_f64");
  auto Hash = J.find<Jit::HashFn>("hash_f64");
  auto OEq = J.find<Jit::EqFn>("eq_of64");
  auto OHash = J.find<Jit::HashFn>("hash_of64");
  ASSERT_TRUE(Eq && Hash && OEq && OHash);
  double Pz = 0.0, Nz = -0.0, One = 1.0, Two = 2.0;
  EXPECT_TRUE(Eq(&Pz, &Nz)); // -0 == +0
  EXPECT_EQ(Hash(&Pz, 1), Hash(&Nz, 1));
  EXPECT_FALSE(Eq(&One, &Two));
  EXPECT_NE(Hash(&One, 1), Hash(&Two, 1));
  // Two NaNs with different bits: never equal, one hash (so a map with NaN
  // keys behaves as Go's: every insert is a new key, but they share a bucket).
  uint64_t Q1 = 0x7ff8000000000001ull, Q2 = 0xfff8000000000abcull;
  double N1, N2;
  std::memcpy(&N1, &Q1, 8);
  std::memcpy(&N2, &Q2, 8);
  EXPECT_FALSE(Eq(&N1, &N1));
  EXPECT_FALSE(Eq(&N1, &N2));
  EXPECT_EQ(Hash(&N1, 1), Hash(&N2, 1));
  EXPECT_NE(Hash(&N1, 1), Hash(&One, 1));
  // Option[float64]: -0 inside a set Option is still +0.
  struct Opt {
    double V;
    uint8_t Flag;
    unsigned char Pad[7];
  };
  Opt A{0.0, 1, {1, 2, 3, 4, 5, 6, 7}}, B{-0.0, 1, {9, 9, 9, 9, 9, 9, 9}};
  EXPECT_TRUE(OEq(&A, &B));
  EXPECT_EQ(OHash(&A, 3), OHash(&B, 3));
  Opt U1{1.0, 0, {}}, U2{N1, 0, {}}; // unset: the payload is not looked at
  EXPECT_TRUE(OEq(&U1, &U2));
  EXPECT_EQ(OHash(&U1, 3), OHash(&U2, 3));
  Opt S{N1, 1, {}}; // a set NaN is not equal to itself
  EXPECT_FALSE(OEq(&S, &S));
}

TEST(Glue, StructPaddingArraysAndComplex) {
  Glued G;
  // struct { i32; f64; bool }: 4 bytes of padding after the i32 and 7 after
  // the bool.
  Type *S = G.C.structTy("t.S");
  G.C.setStructBody(S, {G.C.i32(), G.C.floatTy(64), G.C.boolean()});
  G.add(S, "s");
  Type *Arr = G.C.array(G.C.intTy(16, true), 7); // more than 4: a loop
  G.add(Arr, "arr");
  Type *Small = G.C.array(G.C.i8(), 3); // unrolled
  G.add(Small, "small");
  Type *Cplx = G.C.tuple({G.C.floatTy(32), G.C.floatTy(32)});
  G.add(Cplx, "c64");
  // A slice cannot be compared (a func value neither): no glue is made.
  EXPECT_FALSE(hasGlue(G.C.slice(G.C.i32())));
  EXPECT_EQ(addEqGlue(G.M, G.C.slice(G.C.i32()), "eq_sl"), NoId);
  EXPECT_FALSE(hasGlue(G.C.tuple({G.C.i32(), G.C.slice(G.C.i32())})));
  // A string can (P04.05).
  EXPECT_TRUE(hasGlue(G.C.string()));
  EXPECT_TRUE(hasGlue(G.C.array(G.C.string(), 3)));
  G.add(G.C.string(), "str");
  Jit J(G.M);
  if (!J.ok)
    GTEST_SKIP() << "the host is not a supported target";
  auto SEq = J.find<Jit::EqFn>("eq_s");
  auto SHash = J.find<Jit::HashFn>("hash_s");
  ASSERT_TRUE(SEq && SHash);
  struct St {
    int32_t A;
    double B;
    uint8_t C;
  };
  Bytes<St> X(0x00), Y(0xFF); // the padding differs
  for (Bytes<St> *P : {&X, &Y}) {
    St V{42, 2.5, 1};
    std::memcpy(reinterpret_cast<char *>(P->B) + offsetof(St, A), &V.A, 4);
    std::memcpy(reinterpret_cast<char *>(P->B) + offsetof(St, B), &V.B, 8);
    P->B[offsetof(St, C)] = 1;
  }
  EXPECT_TRUE(SEq(X.get(), Y.get()));
  EXPECT_EQ(SHash(X.get(), 1), SHash(Y.get(), 1));
  Y.B[offsetof(St, C)] = 0; // a field that differs
  EXPECT_FALSE(SEq(X.get(), Y.get()));
  EXPECT_NE(SHash(X.get(), 1), SHash(Y.get(), 1));

  auto AEq = J.find<Jit::EqFn>("eq_arr");
  auto AHash = J.find<Jit::HashFn>("hash_arr");
  int16_t A1[7] = {1, 2, 3, 4, 5, 6, 7}, A2[7] = {1, 2, 3, 4, 5, 6, 7};
  EXPECT_TRUE(AEq(A1, A2));
  EXPECT_EQ(AHash(A1, 1), AHash(A2, 1));
  A2[6] = 8;
  EXPECT_FALSE(AEq(A1, A2));
  EXPECT_NE(AHash(A1, 1), AHash(A2, 1));
  A2[6] = 7;
  A2[0] = 0;
  EXPECT_FALSE(AEq(A1, A2));

  auto SmEq = J.find<Jit::EqFn>("eq_small");
  auto SmHash = J.find<Jit::HashFn>("hash_small");
  int8_t B1[3] = {1, 2, 3}, B2[3] = {1, 2, 3};
  EXPECT_TRUE(SmEq(B1, B2));
  EXPECT_EQ(SmHash(B1, 1), SmHash(B2, 1));
  B2[1] = 0;
  EXPECT_FALSE(SmEq(B1, B2));

  auto CEq = J.find<Jit::EqFn>("eq_c64");
  auto CHash = J.find<Jit::HashFn>("hash_c64");
  float C1[2] = {0.0f, -0.0f}, C2[2] = {-0.0f, 0.0f}, C3[2] = {0.0f, 1.0f};
  EXPECT_TRUE(CEq(C1, C2));
  EXPECT_EQ(CHash(C1, 1), CHash(C2, 1));
  EXPECT_FALSE(CEq(C1, C3));

  // A string is its bytes (P04.05): the pointer, the capacity and the bytes
  // after the length do not matter; the length and every byte do.
  auto StEq = J.find<Jit::EqFn>("eq_str");
  auto StHash = J.find<Jit::HashFn>("hash_str");
  ASSERT_TRUE(StEq && StHash);
  struct Str {
    const char *P;
    uint64_t Len, Cap;
  };
  char T1[8] = "hello!!", T2[8] = "hello??";
  Str S1{T1, 5, 7}, S2{T2, 5, 5}, S3{T2, 4, 4}, S4{T1, 0, 0}, S5{nullptr, 0, 0};
  EXPECT_TRUE(StEq(&S1, &S2));
  EXPECT_EQ(StHash(&S1, 1), StHash(&S2, 1));
  EXPECT_FALSE(StEq(&S1, &S3)); // a prefix
  EXPECT_NE(StHash(&S1, 1), StHash(&S3, 1));
  EXPECT_TRUE(StEq(&S4, &S5)); // empty strings are equal, nil or not
  EXPECT_EQ(StHash(&S4, 1), StHash(&S5, 1));
  T2[4] = 'x';
  EXPECT_FALSE(StEq(&S1, &S2)); // the last byte
  EXPECT_NE(StHash(&S1, 1), StHash(&S2, 1));
}

// A chain of named types thousands of levels deep (review R6): the glue cuts
// it into functions of 24 levels each (Glue.cpp), and the answers are still the
// language's: equal values are equal and hash alike, a difference at the
// deepest field, in the middle and at the top is seen. Under ASan the old
// recursion overflowed at about 3000.
static void chainsOfNamedTypesThousandsDeep(unsigned N) {
  Glued G;
  Type *I = G.C.i64();
  // S0 { i64 }, Sk { S(k-1), i64 }: (k + 1) words, S0's at offset 0.
  Type *S = G.C.structTy("deep.S0");
  G.C.setStructBody(S, {I});
  for (unsigned K = 1; K <= N; ++K) {
    S = G.C.structTy("deep.S" + std::to_string(K));
    G.C.setStructBody(S, {G.C.structTy("deep.S" + std::to_string(K - 1)), I});
  }
  Type *Struct = S;
  // A0 [1]i64, Ak [1]A(k-1): one word.
  Type *Arr = G.C.array(I, 1);
  for (unsigned K = 1; K <= N; ++K)
    Arr = G.C.array(Arr, 1);
  // O0 Option<i64>, Ok Option<O(k-1)> (a hundred levels: the payload nests).
  Type *Opt = G.C.option(I);
  for (unsigned K = 1; K <= 100; ++K)
    Opt = G.C.option(Opt);
  G.add(Struct, "st");
  G.add(Arr, "arr");
  G.add(Opt, "opt");
  Jit J(G.M);
  if (!J.ok)
    GTEST_SKIP() << "the host is not a supported target";
  auto SEq = J.find<Jit::EqFn>("eq_st");
  auto SHash = J.find<Jit::HashFn>("hash_st");
  auto AEq = J.find<Jit::EqFn>("eq_arr");
  auto AHash = J.find<Jit::HashFn>("hash_arr");
  ASSERT_TRUE(SEq && SHash && AEq && AHash);
  std::vector<int64_t> X(N + 1), Y(N + 1);
  for (unsigned K = 0; K <= N; ++K)
    X[K] = Y[K] = K * 7 + 1;
  EXPECT_TRUE(SEq(X.data(), Y.data()));
  EXPECT_EQ(SHash(X.data(), 3), SHash(Y.data(), 3));
  for (unsigned At : {0u, N / 2, N}) { // deepest, middle, top
    Y[At] += 1;
    EXPECT_FALSE(SEq(X.data(), Y.data())) << At;
    EXPECT_NE(SHash(X.data(), 3), SHash(Y.data(), 3)) << At;
    Y[At] -= 1;
  }
  EXPECT_TRUE(SEq(X.data(), Y.data()));
  int64_t P = 5, Q = 5;
  EXPECT_TRUE(AEq(&P, &Q));
  EXPECT_EQ(AHash(&P, 3), AHash(&Q, 3));
  Q = 6;
  EXPECT_FALSE(AEq(&P, &Q));
  EXPECT_NE(AHash(&P, 3), AHash(&Q, 3));
}

TEST(Glue, ChainsOfNamedTypesThousandsDeep) {
  ScopedNewStackSegment Seg(4);
  chainsOfNamedTypesThousandsDeep(200);
}

TEST(Glue, DISABLED_ChainsOfNamedTypesThousandsDeepFull) {
  chainsOfNamedTypesThousandsDeep(1500);
}

// The same chains far beyond what the old recursion survived under ASan (it
// overflowed the stack at about 16000 levels): the glue is made and verified,
// no code is generated for it.
static void chainsOfNamedTypesTensOfThousandsDeep(unsigned N) {
  Glued G;
  Type *I = G.C.i64();
  Type *S = G.C.structTy("deep.S0");
  G.C.setStructBody(S, {I});
  Type *Arr = G.C.array(I, 1);
  for (unsigned K = 1; K <= N; ++K) {
    Type *Prev = S;
    S = G.C.structTy("deep.S" + std::to_string(K));
    G.C.setStructBody(S, {Prev, I});
    Arr = G.C.array(Arr, 1);
  }
  G.add(S, "st");
  G.add(Arr, "arr");
  EXPECT_TRUE(verify(G.M, 5).empty());
  // 24 levels per function: a chain of N has about N / 24 parts of each kind.
  EXPECT_GT(G.M.Funcs.size(), 4u * (N / 24) / 2);
  EXPECT_LT(G.M.Funcs.size(), 4u * (N / 12));
}

TEST(Glue, ChainsOfNamedTypesTensOfThousandsDeep) {
  ScopedNewStackSegment Seg(4);
  chainsOfNamedTypesTensOfThousandsDeep(2000);
}

TEST(Glue, DISABLED_ChainsOfNamedTypesTensOfThousandsDeepFull) {
  chainsOfNamedTypesTensOfThousandsDeep(60000);
}

} // namespace
