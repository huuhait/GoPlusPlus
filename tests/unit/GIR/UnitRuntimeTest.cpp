//===- UnitRuntimeTest.cpp - Exact unit factors at run time, in the JIT ---===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.24 (TODO item 1, ADR-0025 Amendment, GEN-14): the run-time arithmetic
// of unit factors (gopp/GIR/UnitRuntime.h) is built from the module alone,
// lowered for the host and run in LLJIT, and every answer is checked against
// an exact reference made here with llvm::APInt: the product, the exact
// quotient, the gcd and the bounded quotient of numbers of 1 to 64 limbs; the
// product and the power of fractions and the 4096-bit bound (just inside
// gives the exact value, just past it the mark); the rounding to float32 and
// float64 (to nearest, ties to even, subnormals, overflow; checked against
// the two neighbours of the result, and equal to the compiler's own
// Real::toIEEE); the integer words of a factor (x * A + trunc(x * n / d) is
// trunc(x * p / q) modulo 2^64 for x up to 2^64 - 1, also where q passes 64
// bits); the descriptors unit_make builds from recipes (the merge of the
// pairs, the exponent and factor marks, a wide factor spilled to the heap
// chain and that chain freed by its glue); and the site groups (each kind of
// site, the marks, and a factor that is no number of the float type: 0, the
// site's panic, as plain code's E0435).
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/UnitRuntime.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/Sema/Constant.h"

#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"

#include "../Support/GIRTestSupport.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>

#include <random>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

using namespace gopp;
using namespace gopp::gir;
using llvm::APInt;

namespace {

using Limbs = std::vector<uint64_t>;

using BinFn = uint64_t(GOPP_GO_CC *)(uint64_t *, const uint64_t *, uint64_t,
                                     const uint64_t *, uint64_t);
using CmpFn = int64_t(GOPP_GO_CC *)(const uint64_t *, uint64_t,
                                    const uint64_t *, uint64_t);
using DivQFn = uint64_t(GOPP_GO_CC *)(uint64_t *, uint64_t, const uint64_t *,
                                      uint64_t, uint64_t *);
using FracMulFn = void(GOPP_GO_CC *)(uint64_t *, const uint64_t *, uint64_t,
                                     const uint64_t *, uint64_t);
using FracPowFn = void(GOPP_GO_CC *)(uint64_t *, const uint64_t *, int64_t);
using RatioFn = double(GOPP_GO_CC *)(const uint64_t *, uint64_t);
using IntFactorFn = void(GOPP_GO_CC *)(uint64_t *, const uint64_t *, uint64_t);
using MakeFn = void(GOPP_GO_CC *)(uint64_t *, uint64_t, uint64_t,
                                  const uint64_t *, const uint64_t *const *,
                                  uint64_t **);
using SpillFreeFn = void(GOPP_GO_CC *)(uint64_t **);
using SiteFFn = double(GOPP_GO_CC *)(const uint64_t *, const uint64_t *const *,
                                     uint64_t);
using SiteIFn = void(GOPP_GO_CC *)(uint64_t *, const uint64_t *,
                                   const uint64_t *const *, uint64_t);
using FracFn = const uint64_t *(GOPP_GO_CC *)(const uint64_t *);

/// The blocks of the spill chain the helpers allocate, and their sizes: the
/// stand-ins of gopp.mem_resize and gopp.mem_free (the helpers allocate anew:
// the old pointer is null).
std::map<void *, uint64_t> Allocs;
void *GOPP_GO_CC testAlloc(void *, uint64_t Size, bool Zero) {
  void *Q = Zero ? std::calloc(1, Size) : std::malloc(Size);
  Allocs[Q] = Size;
  return Q;
}
void GOPP_GO_CC testFree(void *P) {
  Allocs.erase(P);
  std::free(P);
}

/// The functions, JIT-compiled once for the whole file.
struct Arith {
  Context C;
  Module M{C, "units"};
  std::unique_ptr<llvm::orc::LLJIT> J;
  bool Ok = false;
  std::string Errors;
  BinFn Mul = nullptr, DivExact = nullptr, Gcd = nullptr;
  CmpFn Cmp = nullptr;
  DivQFn DivQ = nullptr;
  FracMulFn FracMul = nullptr;
  FracPowFn FracPow = nullptr;
  RatioFn Ratio = nullptr;
  IntFactorFn IntFactor = nullptr;
  MakeFn Make = nullptr;
  SpillFreeFn SpillFree = nullptr;
  SiteFFn SiteF = nullptr;
  SiteIFn SiteI = nullptr;
  FracFn FracOf = nullptr;

  Arith() {
    // (The allocator of the spill chain: two C++ functions bound below.)
    Type *PU8 = C.ptr(C.u8());
    FuncId Alloc = test::declare(M, "unit_test_alloc",
                                 C.func({PU8, C.u64(), C.boolean()}, PU8));
    FuncId Free = test::declare(M, "unit_test_free", C.func({PU8}, C.unit()));
    M.func(Alloc).CC = M.func(Free).CC = CallConv::Go;
    addUnitArith(
        M, [](llvm::StringRef N) { return N.str(); }, Linkage::External, Alloc,
        Free);
    Errors = test::errorsOf(M);
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
    if (!LM) {
      Errors += llvm::toString(LM.takeError());
      return;
    }
    auto Made = llvm::orc::LLJITBuilder().create();
    if (!Made) {
      Errors += llvm::toString(Made.takeError());
      return;
    }
    J = std::move(*Made);
    llvm::orc::SymbolMap Own;
    Own[J->mangleAndIntern("unit_test_alloc")] = {
        llvm::orc::ExecutorAddr::fromPtr(&testAlloc),
        llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    Own[J->mangleAndIntern("unit_test_free")] = {
        llvm::orc::ExecutorAddr::fromPtr(&testFree),
        llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable};
    if (llvm::Error E =
            J->getMainJITDylib().define(llvm::orc::absoluteSymbols(Own))) {
      Errors += llvm::toString(std::move(E));
      return;
    }
    if (llvm::Error E = J->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(*LM), std::move(Ctx)))) {
      Errors += llvm::toString(std::move(E));
      return;
    }
    Mul = find<BinFn>("unit_mul");
    DivExact = find<BinFn>("unit_divexact");
    Gcd = find<BinFn>("unit_gcd");
    Cmp = find<CmpFn>("unit_cmp");
    DivQ = find<DivQFn>("unit_divq");
    FracMul = find<FracMulFn>("unit_fracmul");
    FracPow = find<FracPowFn>("unit_fracpow");
    Ratio = find<RatioFn>("unit_ratio");
    IntFactor = find<IntFactorFn>("unit_intfactor");
    Make = find<MakeFn>("unit_make");
    SpillFree = find<SpillFreeFn>("unit_spill_free");
    SiteF = find<SiteFFn>("unit_site_f");
    SiteI = find<SiteIFn>("unit_site_i");
    FracOf = find<FracFn>("unit_frac");
    Ok = Mul && DivExact && Gcd && Cmp && DivQ && FracMul && FracPow && Ratio &&
         IntFactor && Make && SpillFree && SiteF && SiteI && FracOf;
  }
  template <class Fn> Fn find(const char *Name) {
    auto Addr = J->lookup(Name);
    if (!Addr) {
      Errors += llvm::toString(Addr.takeError());
      return nullptr;
    }
    return Addr->toPtr<Fn>();
  }
};

Arith &arith() {
  static Arith A;
  return A;
}

#define NEED_ARITH()                                                           \
  Arith &A = arith();                                                          \
  ASSERT_EQ(A.Errors, "");                                                     \
  if (!A.Ok)                                                                   \
  GTEST_SKIP() << "the host is not a supported target"

//===----------------------------------------------------------------------===//
// Exact references
//===----------------------------------------------------------------------===//

Limbs limbsOf(const APInt &V) {
  Limbs L;
  unsigned N = (V.getActiveBits() + 63) / 64;
  for (unsigned I = 0; I < N; ++I)
    L.push_back(V.extractBitsAsZExtValue(
        std::min(64u, V.getBitWidth() - 64 * I), 64 * I));
  return L;
}

APInt apOf(const uint64_t *P, uint64_t N, unsigned W = 0) {
  unsigned Bits = std::max<unsigned>(64 * std::max<uint64_t>(N, 1), W);
  APInt V(Bits, 0);
  for (uint64_t I = 0; I < N; ++I)
    V |= APInt(Bits, P[I]).shl(64 * I);
  return V;
}

/// A and B as one width wide enough for their product.
std::pair<APInt, APInt> same(const APInt &A, const APInt &B) {
  unsigned W = A.getBitWidth() + B.getBitWidth() + 2;
  return {A.zext(W), B.zext(W)};
}

bool eq(const APInt &A, const APInt &B) {
  auto [X, Y] = same(A, B);
  return X == Y;
}

APInt mul(const APInt &A, const APInt &B) {
  auto [X, Y] = same(A, B);
  return X * Y;
}

APInt add(const APInt &A, const APInt &B) {
  auto [X, Y] = same(A, B);
  return X + Y;
}

APInt gcd(const APInt &A, const APInt &B) {
  auto [X, Y] = same(A, B);
  return llvm::APIntOps::GreatestCommonDivisor(X, Y);
}

/// A positive number of exactly Bits bits; some of the patterns that break
/// carries: all ones, a single bit, alternating limbs.
APInt randBig(std::mt19937_64 &R, unsigned Bits) {
  unsigned N = (Bits + 63) / 64;
  APInt V(64 * N, 0);
  unsigned Shape = R() % 8;
  for (unsigned I = 0; I < N; ++I) {
    uint64_t L = R();
    if (Shape == 0)
      L = ~uint64_t(0);
    else if (Shape == 1)
      L = (I & 1) ? ~uint64_t(0) : 0;
    else if (Shape == 2)
      L = 0;
    V |= APInt(64 * N, L).shl(64 * I);
  }
  if (Bits < 64 * N)
    V = V.trunc(Bits).zext(64 * N);
  V.setBit(Bits - 1);
  if (Shape == 2)
    V.setBit(0);
  return V;
}

unsigned randBits(std::mt19937_64 &R, unsigned Max) {
  switch (R() % 4) {
  case 0:
    return 1 + R() % 64;
  case 1:
    return 64 * (1 + R() % (Max / 64)); // a full top limb
  default:
    return 1 + R() % Max;
  }
}

/// A fraction in memory (UnitRuntime.h) from N / D.
struct Frac {
  std::vector<uint64_t> W;
  Frac() : W(UnitFracWords + 8, 0) {}
  Frac(const APInt &N, const APInt &D) : Frac() {
    Limbs Ln = limbsOf(N), Ld = limbsOf(D);
    W[0] = Ln.size();
    W[1] = Ld.size();
    std::copy(Ln.begin(), Ln.end(), W.begin() + 2);
    std::copy(Ld.begin(), Ld.end(), W.begin() + 2 + Ln.size());
  }
  bool marked() const { return W[0] == 0; }
  APInt num() const { return apOf(&W[2], W[0]); }
  APInt den() const { return apOf(&W[2 + W[0]], W[1]); }
  uint64_t *data() { return W.data(); }
  const uint64_t *data() const { return W.data(); }
};

/// N / D reduced.
Frac reduced(APInt N, APInt D) {
  APInt G = gcd(N, D);
  auto [X, Gx] = same(N, G);
  auto [Y, Gy] = same(D, G);
  return Frac(X.udiv(Gx), Y.udiv(Gy));
}

/// Is the fraction F exactly N / D (both reduced)?
bool fracIs(const Frac &F, const APInt &N, const APInt &D) {
  if (F.marked())
    return false;
  return eq(F.num(), N) && eq(F.den(), D);
}

//===----------------------------------------------------------------------===//
// Numbers
//===----------------------------------------------------------------------===//

TEST(UnitRuntime, TheModuleIsValidGIR) {
  Arith &A = arith();
  EXPECT_EQ(A.Errors, "");
}

TEST(UnitRuntime, ProductQuotientAndGcdOfManyLimbs) {
  NEED_ARITH();
  std::mt19937_64 R(20261031);
  std::vector<uint64_t> Out(4 * UnitMaxLimbs + 8);
  for (unsigned K = 0; K < 300; ++K) {
    APInt X = randBig(R, randBits(R, 2048)), Y = randBig(R, randBits(R, 2048));
    Limbs Lx = limbsOf(X), Ly = limbsOf(Y);
    // The product.
    uint64_t N = A.Mul(Out.data(), Lx.data(), Lx.size(), Ly.data(), Ly.size());
    ASSERT_TRUE(eq(apOf(Out.data(), N), mul(X, Y))) << K;
    // The exact quotient of the product by either factor.
    Limbs P = limbsOf(mul(X, Y));
    uint64_t Nq =
        A.DivExact(Out.data(), P.data(), P.size(), Ly.data(), Ly.size());
    ASSERT_TRUE(eq(apOf(Out.data(), Nq), X)) << K;
    // The gcd of two multiples of a common factor.
    APInt G = randBig(R, 1 + R() % 300);
    Limbs Lg1 = limbsOf(mul(X, G)), Lg2 = limbsOf(mul(Y, G));
    uint64_t Ng =
        A.Gcd(Out.data(), Lg1.data(), Lg1.size(), Lg2.data(), Lg2.size());
    ASSERT_TRUE(eq(apOf(Out.data(), Ng), gcd(mul(X, G), mul(Y, G)))) << K;
    // The comparison.
    int64_t C = A.Cmp(Lx.data(), Lx.size(), Ly.data(), Ly.size());
    auto [Xs, Ys] = same(X, Y);
    EXPECT_EQ(C, Xs == Ys ? 0 : (Xs.ult(Ys) ? -1 : 1)) << K;
  }
  // The edges: 1, a full limb, gcd with one limb, coprime numbers, equal ones.
  uint64_t One = 1, Big = ~uint64_t(0);
  uint64_t N = A.Mul(Out.data(), &Big, 1, &Big, 1);
  EXPECT_TRUE(eq(apOf(Out.data(), N), mul(APInt(64, Big), APInt(64, Big))));
  N = A.Gcd(Out.data(), &One, 1, &Big, 1);
  EXPECT_EQ(N, 1u);
  EXPECT_EQ(Out[0], 1u);
  APInt X = randBig(R, 1000);
  Limbs Lx = limbsOf(X);
  N = A.Gcd(Out.data(), Lx.data(), Lx.size(), Lx.data(), Lx.size());
  EXPECT_TRUE(eq(apOf(Out.data(), N), X));
  // 2^k and 2^k * 3 share 2^k (the binary gcd's common power of two).
  APInt P2 = APInt::getOneBitSet(1200, 1100);
  Limbs L1 = limbsOf(P2), L2 = limbsOf(mul(P2, APInt(64, 3)));
  N = A.Gcd(Out.data(), L1.data(), L1.size(), L2.data(), L2.size());
  EXPECT_TRUE(eq(apOf(Out.data(), N), P2));
}

TEST(UnitRuntime, TheBoundedQuotientAndItsOverflow) {
  NEED_ARITH();
  std::mt19937_64 R(7);
  std::vector<uint64_t> Rb(UnitMaxLimbs + 8), Out(2);
  for (unsigned K = 0; K < 300; ++K) {
    APInt Y = randBig(R, randBits(R, 3000));
    uint64_t Q = R() >> (R() % 64);
    APInt Rm = randBig(R, std::max(1u, Y.getActiveBits() - 1));
    auto [Ys, Rs] = same(Y, Rm);
    if (Rs.uge(Ys))
      Rs = Rs.lshr(1);
    APInt V = mul(Y, APInt(64, Q));
    auto [Vs, Rs2] = same(V, Rs);
    V = Vs + Rs2;
    Limbs Lv = limbsOf(V), Ly = limbsOf(Y);
    std::copy(Lv.begin(), Lv.end(), Rb.begin());
    uint64_t Got =
        A.DivQ(Rb.data(), Lv.size(), Ly.data(), Ly.size(), Out.data());
    ASSERT_EQ(Out[1], 0u) << K;
    ASSERT_EQ(Got, Q) << K;
    ASSERT_TRUE(eq(apOf(Rb.data(), Out[0]), Rs)) << K;
  }
  // A quotient of 2^64 (and more) is the overflow, one of 2^64 - 1 is not.
  uint64_t Y = 3;
  APInt V = mul(APInt(64, 3), APInt::getOneBitSet(66, 64));
  Limbs Lv = limbsOf(V);
  std::copy(Lv.begin(), Lv.end(), Rb.begin());
  A.DivQ(Rb.data(), Lv.size(), &Y, 1, Out.data());
  EXPECT_EQ(Out[1], 1u);
  V = mul(APInt(64, 3), APInt(64, ~uint64_t(0)));
  Lv = limbsOf(V);
  std::copy(Lv.begin(), Lv.end(), Rb.begin());
  EXPECT_EQ(A.DivQ(Rb.data(), Lv.size(), &Y, 1, Out.data()), ~uint64_t(0));
  EXPECT_EQ(Out[1], 0u);
  EXPECT_EQ(Out[0], 0u);
}

//===----------------------------------------------------------------------===//
// Fractions
//===----------------------------------------------------------------------===//

TEST(UnitRuntime, FractionProductsAreReducedAndBounded) {
  NEED_ARITH();
  std::mt19937_64 R(11);
  for (unsigned K = 0; K < 300; ++K) {
    // Factors with a shared part, so the reduction has work to do.
    APInt S = randBig(R, 1 + R() % 200);
    APInt N1 = mul(randBig(R, randBits(R, 1500)), S),
          D1 = randBig(R, randBits(R, 1500));
    APInt N2 = randBig(R, randBits(R, 1500)),
          D2 = mul(randBig(R, randBits(R, 1500)), S);
    Frac F1 = reduced(N1, D1), F2 = reduced(N2, D2);
    bool I1 = R() & 1, I2 = R() & 1;
    Frac Out;
    A.FracMul(Out.data(), F1.data(), I1, F2.data(), I2);
    APInt Xn = I1 ? F1.den() : F1.num(), Xd = I1 ? F1.num() : F1.den();
    APInt Yn = I2 ? F2.den() : F2.num(), Yd = I2 ? F2.num() : F2.den();
    Frac Want = reduced(mul(Xn, Yn), mul(Xd, Yd));
    bool In = Want.num().getActiveBits() <= 4096 &&
              Want.den().getActiveBits() <= 4096;
    if (!In) {
      EXPECT_TRUE(Out.marked()) << K;
      continue;
    }
    ASSERT_TRUE(fracIs(Out, Want.num(), Want.den())) << K;
    // In place: dst is the first input.
    Frac Self = F1;
    A.FracMul(Self.data(), Self.data(), I1, F2.data(), I2);
    ASSERT_TRUE(fracIs(Self, Want.num(), Want.den())) << K;
    // A side of 1 (a copy, in place a no-op or, inverted, the general path),
    // either side, either way round.
    Frac One1(APInt(64, 1), APInt(64, 1));
    A.FracMul(Out.data(), F1.data(), I1, One1.data(), I2);
    ASSERT_TRUE(fracIs(Out, Xn, Xd)) << K;
    A.FracMul(Out.data(), One1.data(), I1, F2.data(), I2);
    ASSERT_TRUE(fracIs(Out, Yn, Yd)) << K;
    Self = F1;
    A.FracMul(Self.data(), Self.data(), I1, One1.data(), 0);
    ASSERT_TRUE(fracIs(Self, Xn, Xd)) << K;
    Self = F2;
    A.FracMul(Self.data(), One1.data(), 0, Self.data(), I2);
    ASSERT_TRUE(fracIs(Self, Yn, Yd)) << K;
  }
  // The bound: 2^4095 has 4096 bits (in), 2^4096 has 4097 (out).
  APInt One(64, 1), Two(64, 2);
  Frac Half = Frac(APInt::getOneBitSet(4095, 4094), One); // 2^4094
  Frac Out;
  A.FracMul(Out.data(), Half.data(), 0, Frac(Two, One).data(), 0);
  EXPECT_TRUE(fracIs(Out, APInt::getOneBitSet(4096, 4095), One)); // 2^4095
  Frac In = Out;
  A.FracMul(Out.data(), In.data(), 0, Frac(Two, One).data(), 0);
  EXPECT_TRUE(Out.marked()); // 2^4096: 4097 bits
  // ... and the inverse side of the bound, and a mark that stays a mark.
  A.FracMul(Out.data(), In.data(), 1, Frac(One, Two).data(), 0);
  EXPECT_TRUE(Out.marked());
  Frac Marked;
  A.FracMul(Out.data(), Marked.data(), 0, Frac(One, One).data(), 0);
  EXPECT_TRUE(Out.marked());
  // A product that passes the bound and cancels back stays inside when the
  // reduction comes first: 2^4000/3 * 3/2^4000 = 1.
  Frac Big = Frac(APInt::getOneBitSet(4001, 4000), APInt(64, 3));
  A.FracMul(Out.data(), Big.data(), 0, Big.data(), 1);
  EXPECT_TRUE(fracIs(Out, One, One));
}

TEST(UnitRuntime, FractionPowersAndTheirBound) {
  NEED_ARITH();
  std::mt19937_64 R(13);
  for (unsigned K = 0; K < 200; ++K) {
    APInt N = randBig(R, 1 + R() % 300), D = randBig(R, 1 + R() % 300);
    Frac F = reduced(N, D);
    int64_t E = int64_t(1 + R() % 40) * ((R() & 1) ? 1 : -1);
    Frac Out;
    A.FracPow(Out.data(), F.data(), E);
    APInt Pn(64, 1), Pd(64, 1);
    for (int64_t I = 0; I < (E < 0 ? -E : E); ++I) {
      Pn = mul(Pn, F.num());
      Pd = mul(Pd, F.den());
    }
    if (E < 0)
      std::swap(Pn, Pd);
    if (Pn.getActiveBits() > 4096 || Pd.getActiveBits() > 4096) {
      EXPECT_TRUE(Out.marked()) << K;
      continue;
    }
    ASSERT_TRUE(fracIs(Out, Pn, Pd)) << K << " e=" << E;
  }
  // 1000^411 has 4096 bits (in); 1000^412 has 4106 (out), as the checker's
  // bound (sema/units-generic-range.test, U^412 with U = KM).
  Frac KM(APInt(64, 1000), APInt(64, 1)), Out;
  A.FracPow(Out.data(), KM.data(), 411);
  EXPECT_FALSE(Out.marked());
  EXPECT_EQ(Out.num().getActiveBits(), 4096u);
  A.FracPow(Out.data(), KM.data(), -411);
  EXPECT_TRUE(fracIs(Out, APInt(64, 1), Out.den()));
  EXPECT_EQ(Out.den().getActiveBits(), 4096u);
  A.FracPow(Out.data(), KM.data(), -412);
  EXPECT_TRUE(Out.marked());
  // An exponent of 2^20 on 1 is 1, on 2 is the mark.
  Frac One(APInt(64, 1), APInt(64, 1)), Two(APInt(64, 2), APInt(64, 1));
  A.FracPow(Out.data(), One.data(), 1 << 20);
  EXPECT_TRUE(fracIs(Out, APInt(64, 1), APInt(64, 1)));
  A.FracPow(Out.data(), Two.data(), -(1 << 20));
  EXPECT_TRUE(Out.marked());
}

/// Does R round N/D once to nearest-even with P bits, in the range of the
/// format? Checked against its neighbours: the value lies within half an
/// ulp, and a tie went to the even one.
::testing::AssertionResult roundsOnce(double Got, const APInt &N,
                                      const APInt &D, unsigned P) {
  // The exact value of a double as a fraction 2^E * M.
  auto Exact = [](double V, APInt &Num, APInt &Den) {
    int E;
    double M = std::frexp(V, &E);
    uint64_t I = uint64_t(std::ldexp(M, 64)); // V = I * 2^(E - 64)
    int64_t S = int64_t(E) - 64;
    Num = APInt(64 + 2200, I);
    Den = APInt(64 + 2200, 1);
    if (S >= 0)
      Num <<= unsigned(S);
    else
      Den <<= unsigned(-S);
  };
  // The sign of (a/b - c/d).
  auto Sign = [](const APInt &A0, const APInt &B0, const APInt &C0,
                 const APInt &D0) {
    APInt L = mul(A0, D0), Rr = mul(C0, B0);
    auto [X, Y] = same(L, Rr);
    return X == Y ? 0 : (X.ult(Y) ? -1 : 1);
  };
  double Max = P == 24 ? double(std::numeric_limits<float>::max())
                       : std::numeric_limits<double>::max();
  auto Next = [&](double V, bool Up) {
    if (P == 24)
      return double(std::nextafter(float(V), Up ? INFINITY : 0.0f));
    return std::nextafter(V, Up ? INFINITY : 0.0);
  };
  if (std::isinf(Got)) {
    // Past the largest finite value plus half an ulp.
    double Ulp = Max - Next(Max, false);
    APInt Hn, Hd;
    Exact(Max, Hn, Hd);
    APInt Un, Ud;
    Exact(Ulp / 2, Un, Ud);
    APInt Sn = add(mul(Hn, Ud), mul(Un, Hd));
    APInt Sd = mul(Hd, Ud);
    if (Sign(N, D, Sn, Sd) < 0)
      return ::testing::AssertionFailure() << "inf below the overflow";
    return ::testing::AssertionSuccess();
  }
  if (Got == 0) {
    // Half the smallest subnormal, 2^-1075 or 2^-150 (no double holds it).
    APInt Mn(2200, 1), Md = APInt::getOneBitSet(2200, P == 24 ? 150 : 1075);
    if (Sign(N, D, Mn, Md) > 0)
      return ::testing::AssertionFailure() << "0 above half the smallest";
    return ::testing::AssertionSuccess();
  }
  APInt Gn, Gd;
  Exact(Got, Gn, Gd);
  for (bool Up : {false, true}) {
    double Nb = Next(Got, Up);
    if (std::isinf(Nb))
      continue;
    APInt Bn, Bd;
    Exact(Nb, Bn, Bd);
    // The midpoint (Got + Nb) / 2.
    APInt Mn = add(mul(Gn, Bd), mul(Bn, Gd));
    APInt Md = mul(mul(Gd, Bd), APInt(64, 2));
    int S = Sign(N, D, Mn, Md);
    if (Up ? S > 0 : S < 0)
      return ::testing::AssertionFailure()
             << "past the midpoint toward " << (Up ? "up" : "down");
    if (S == 0) {
      // A tie: the even one.
      uint64_t Bits;
      if (P == 24) {
        float F = float(Got);
        uint32_t B;
        std::memcpy(&B, &F, 4);
        Bits = B;
      } else {
        std::memcpy(&Bits, &Got, 8);
      }
      if (Bits & 1)
        return ::testing::AssertionFailure() << "a tie to odd";
    }
  }
  return ::testing::AssertionSuccess();
}

TEST(UnitRuntime, RatiosRoundOnceAsTheCompilerDoes) {
  NEED_ARITH();
  std::mt19937_64 R(17);
  auto Check = [&](const APInt &N0, const APInt &D0, const char *What) {
    Frac F = reduced(N0, D0);
    APInt N = F.num(), D = F.den();
    for (unsigned P : {24u, 53u}) {
      double Got = A.Ratio(F.data(), P);
      // The compiler's rounding of the same constant factor (plain code).
      unsigned W = std::max(N.getBitWidth(), D.getBitWidth()) + 1;
      double Want = sema::Real::fromRat(N.zext(W), D.zext(W)).toIEEE(P);
      uint64_t Gb, Wb;
      std::memcpy(&Gb, &Got, 8);
      std::memcpy(&Wb, &Want, 8);
      EXPECT_EQ(Gb, Wb) << What << " p=" << P << " got " << Got << " want "
                        << Want;
      EXPECT_TRUE(roundsOnce(Got, N, D, P)) << What << " p=" << P;
    }
  };
  for (unsigned K = 0; K < 400; ++K)
    Check(randBig(R, randBits(R, 4096)), randBig(R, randBits(R, 4096)),
          "random");
  // One limb each way: unit_ratio's 64-bit path, against the same reference.
  for (unsigned K = 0; K < 400; ++K)
    Check(APInt(64, (R() >> (R() % 64)) | 1),
          APInt(64, (R() >> (R() % 64)) | 1), "one limb");
  Check(APInt(64, ~uint64_t(0)), APInt(64, 1), "2^64 - 1");
  Check(APInt(64, 1), APInt(64, ~uint64_t(0)), "1 / (2^64 - 1)");
  APInt One(64, 1);
  // Ties: 1 + 2^-53 (to even: 1), 1 + 3 * 2^-53 (to even: up), and one bit
  // past the tie far below (2^-700): up. A rounding through 512 bits first
  // would make the last one a tie and give 1.
  APInt T = APInt::getOneBitSet(800, 700);
  Check(T + APInt::getOneBitSet(800, 647), T, "tie");
  Check(T + APInt::getOneBitSet(800, 647) * 3, T, "tie up");
  Check(T + APInt::getOneBitSet(800, 647) + 1, T, "past the tie");
  Check(T + APInt::getOneBitSet(800, 676), T, "f32 tie");
  Check(T + APInt::getOneBitSet(800, 676) + 1, T, "past the f32 tie");
  // The range: the largest finite values, their overflow, the subnormals,
  // half the smallest (to 0), just past it (to the smallest).
  Check(APInt::getOneBitSet(1100, 1023), One, "2^1023");
  Check(APInt::getOneBitSet(1100, 1024), One, "2^1024: inf");
  Check(APInt::getOneBitSet(200, 127), One, "2^127");
  Check(APInt::getOneBitSet(200, 128), One, "2^128: f32 inf");
  Check(APInt::getAllOnes(128), One, "2^128 - 1: f32 inf");
  Check(APInt::getAllOnes(1024), One, "2^1024 - 1: inf");
  Check(One, APInt::getOneBitSet(1100, 1074), "2^-1074");
  Check(One, APInt::getOneBitSet(1100, 1075), "2^-1075: 0");
  Check(One, APInt::getOneBitSet(1100, 1075) - 1, "just past 2^-1075");
  Check(APInt(64, 3), APInt::getOneBitSet(1100, 1076), "3 * 2^-1076");
  Check(One, APInt::getOneBitSet(200, 149), "2^-149");
  Check(One, APInt::getOneBitSet(200, 150), "2^-150: f32 0");
  Check(APInt(64, 3), APInt::getOneBitSet(200, 151), "3 * 2^-151");
  Check(One, APInt::getOneBitSet(200, 126), "2^-126");
  Check(APInt::getAllOnes(4096), One, "4096 bits");
  Check(One, APInt::getAllOnes(4096), "1 / 4096 bits");
  APInt Ten28(128, 1);
  for (int I = 0; I < 28; ++I)
    Ten28 *= 10;
  Check(APInt(64, 1602176634), Ten28, "eV in J");
  // The marked fraction is 0 (its site panics before using it).
  Frac Marked;
  EXPECT_EQ(A.Ratio(Marked.data(), 53), 0.0);
}

/// x * A + trunc(x * n / d) modulo 2^64, as unit_scale_int computes it.
uint64_t scaleWith(uint64_t X, const uint64_t *W) {
  unsigned __int128 P = (unsigned __int128)(X % W[2]) * W[1];
  return X * W[0] + (X / W[2]) * W[1] + uint64_t(P / W[2]);
}

/// trunc(x * p / q) modulo 2^64.
uint64_t scaleExact(uint64_t X, const APInt &P, const APInt &Q) {
  APInt V = mul(APInt(64, X), P);
  auto [Vs, Qs] = same(V, Q);
  return Vs.udiv(Qs).zextOrTrunc(64).getZExtValue();
}

TEST(UnitRuntime, IntegerWordsTruncateLikeTheExactProduct) {
  NEED_ARITH();
  std::mt19937_64 R(19);
  std::vector<uint64_t> Xs = {0,
                              1,
                              2,
                              999,
                              1000,
                              1001,
                              uint64_t(1) << 32,
                              uint64_t(1) << 63,
                              (uint64_t(1) << 63) - 1,
                              ~uint64_t(0),
                              ~uint64_t(0) - 1};
  auto Check = [&](const APInt &P0, const APInt &Q0, const char *What) {
    Frac F = reduced(P0, Q0);
    for (uint64_t Mode : {0u, 1u, 2u}) {
      uint64_t W[4] = {9, 9, 9, 9};
      A.IntFactor(W, F.data(), Mode);
      bool Inv = Mode == 1;
      if (Mode == 2) {
        auto [Ns, Ds] = same(F.num(), F.den());
        Inv = Ns.ult(Ds);
      }
      EXPECT_EQ(W[3], uint64_t(Inv)) << What;
      APInt P = Inv ? F.den() : F.num(), Q = Inv ? F.num() : F.den();
      ASSERT_NE(W[2], 0u) << What;
      EXPECT_LT(W[1], W[2]) << What;
      for (uint64_t X : Xs)
        ASSERT_EQ(scaleWith(X, W), scaleExact(X, P, Q))
            << What << " mode " << Mode << " x=" << X;
      for (unsigned K = 0; K < 40; ++K) {
        uint64_t X = R() >> (R() % 64);
        ASSERT_EQ(scaleWith(X, W), scaleExact(X, P, Q))
            << What << " mode " << Mode << " x=" << X;
      }
    }
  };
  for (unsigned K = 0; K < 150; ++K)
    Check(randBig(R, randBits(R, 600)), randBig(R, randBits(R, 600)), "random");
  // One limb each way: unit_intfactor's 64-bit path.
  for (unsigned K = 0; K < 100; ++K)
    Check(APInt(64, (R() >> (R() % 64)) | 1),
          APInt(64, (R() >> (R() % 64)) | 1), "one limb");
  APInt One(64, 1);
  APInt Ten28(128, 1);
  for (int I = 0; I < 28; ++I)
    Ten28 *= 10;
  Check(APInt(64, 1602176634), Ten28, "eV in J (q past 64 bits)");
  Check(APInt(64, 1000), One, "1000");
  Check(APInt(64, 127), APInt(64, 50), "127/50");
  Check(APInt::getAllOnes(4096), One, "a 4096-bit integer");
  Check(One, APInt::getAllOnes(4096), "1 over a 4096-bit integer");
  Check(APInt::getOneBitSet(130, 128) + 1, APInt::getOneBitSet(130, 128),
        "just above 1, q past 64 bits");
  Check(APInt::getOneBitSet(130, 128) - 1, APInt::getOneBitSet(130, 128),
        "just below 1, q past 64 bits");
  // (2^64 - 1)/2^64 and its neighbours: the approximation at the edge of the
  // denominators below 2^64.
  Check(APInt::getAllOnes(64).zext(70), APInt::getOneBitSet(70, 64), "edge");
  Check(APInt::getOneBitSet(200, 100) + 3, APInt::getOneBitSet(200, 164) - 1,
        "tiny");
  // The marked fraction gives d = 0 (the site's panic).
  Frac Marked;
  uint64_t W[4] = {9, 9, 9, 9};
  A.IntFactor(W, Marked.data(), 0);
  EXPECT_EQ(W[2], 0u);
}

//===----------------------------------------------------------------------===//
// Descriptors, recipes and site groups
//===----------------------------------------------------------------------===//

/// The words of a descriptor: {count, (id, exp) x count, the fraction}.
std::vector<uint64_t> desc(std::vector<std::pair<uint64_t, int64_t>> Pairs,
                           const Frac &F) {
  std::vector<uint64_t> W{Pairs.size()};
  for (auto &[Id, E] : Pairs) {
    W.push_back(Id);
    W.push_back(uint64_t(E));
  }
  W.insert(W.end(), F.W.begin(),
           F.W.begin() + 2 + (F.W[0] ? F.W[0] + F.W[1] : 0));
  return W;
}

/// A recipe: {np, (index, exp) x np, the base descriptor}.
std::vector<uint64_t> recipe(std::vector<std::pair<uint64_t, int64_t>> Params,
                             std::vector<uint64_t> Base) {
  std::vector<uint64_t> W{Params.size()};
  for (auto &[I, E] : Params) {
    W.push_back(I);
    W.push_back(uint64_t(E));
  }
  W.insert(W.end(), Base.begin(), Base.end());
  return W;
}

/// A site group: {static factor, ns, (kind, res, x, y) x ns}.
struct SiteSpec {
  uint64_t Kind;
  std::vector<uint64_t> Res, X, Y;
};
std::vector<uint64_t> group(const Frac &Static, std::vector<SiteSpec> Sites) {
  std::vector<uint64_t> W(Static.W.begin(),
                          Static.W.begin() + 2 +
                              (Static.W[0] ? Static.W[0] + Static.W[1] : 0));
  W.push_back(Sites.size());
  for (auto &S : Sites) {
    W.push_back(S.Kind);
    for (auto *R : {&S.Res, &S.X, &S.Y})
      W.insert(W.end(), R->begin(), R->end());
  }
  return W;
}

/// A frame slot of unit_make: room for Cap pairs and a fraction of
/// UnitSmallLimbs limbs.
struct Slot {
  std::vector<uint64_t> W;
  explicit Slot(unsigned Cap) : W(1 + 2 * Cap + 2 + UnitSmallLimbs, 0xdead) {}
  uint64_t count() const { return W[0]; }
  const uint64_t *frac() const { return &W[1 + 2 * W[0]]; }
};

/// The fraction at F (resolved if it was spilled) as a Frac.
Frac fracAt(Arith &A, const uint64_t *F) {
  const uint64_t *P = A.FracOf(F);
  Frac R;
  std::copy(P, P + 2 + (P[0] ? P[0] + P[1] : 0), R.W.begin());
  return R;
}

TEST(UnitRuntime, MakeBuildsDescriptorsFromRecipesAndSpillsWideFactors) {
  NEED_ARITH();
  APInt One(64, 1), K(64, 1000);
  Frac F1(One, One);
  // KM = 1000 M (id 5), S (id 9), passed as dicts[0] and dicts[1].
  auto KM = desc({{5, 1}}, Frac(K, One)), S = desc({{9, 1}}, F1);
  const uint64_t *Dicts[2] = {KM.data(), S.data()};
  uint64_t *Spill = nullptr;
  auto Make = [&](Slot &D, unsigned Cap, const std::vector<uint64_t> &R) {
    A.Make(D.W.data(), Cap, UnitSmallLimbs, R.data(), Dicts, &Spill);
  };
  // KM^3 / S^2: two pairs, 10^9 in place.
  Slot D1(4);
  Make(D1, 4, recipe({{0, 3}, {1, -2}}, desc({}, F1)));
  ASSERT_EQ(D1.count(), 2u);
  EXPECT_EQ(D1.W[1], 5u);
  EXPECT_EQ(int64_t(D1.W[2]), 3);
  EXPECT_EQ(D1.W[3], 9u);
  EXPECT_EQ(int64_t(D1.W[4]), -2);
  EXPECT_NE(D1.frac()[0], UnitCountOut); // in place
  EXPECT_TRUE(fracIs(fracAt(A, D1.frac()), APInt(64, 1000000000), One));
  // KM^8 with a base of M^-8: no base unit, 10^24 (two limbs) in place.
  Slot D2(4);
  Make(D2, 4, recipe({{0, 8}}, desc({{5, -8}}, F1)));
  APInt E24(128, 1);
  for (int I = 0; I < 24; ++I)
    E24 *= 10;
  ASSERT_EQ(D2.count(), 0u);
  EXPECT_TRUE(fracIs(fracAt(A, D2.frac()), E24, One));
  EXPECT_EQ(Spill, nullptr);
  // KM^30: 1000^30 has 299 bits (5 limbs and 1): spilled to the heap chain.
  Slot D3(4);
  Make(D3, 4, recipe({{0, 30}}, desc({}, F1)));
  APInt P30(320, 1);
  for (int I = 0; I < 30; ++I)
    P30 *= 1000;
  ASSERT_EQ(D3.count(), 1u);
  EXPECT_EQ(D3.frac()[0], UnitCountOut); // spilled
  EXPECT_TRUE(fracIs(fracAt(A, D3.frac()), P30, One));
  EXPECT_NE(Spill, nullptr);
  EXPECT_EQ(Allocs.size(), 1u);
  // A descriptor made from the spilled one is exact too (unit_frac):
  // 1000 M^31 / KM^30 is M with the factor 1/1000^29, spilled again.
  const uint64_t *Dicts2[2] = {D3.W.data(), S.data()};
  Slot D4(4);
  A.Make(D4.W.data(), 4, UnitSmallLimbs,
         recipe({{0, -1}}, desc({{5, 31}}, Frac(K, One))).data(), Dicts2,
         &Spill);
  APInt P29(320, 1);
  for (int I = 0; I < 29; ++I)
    P29 *= 1000;
  ASSERT_EQ(D4.count(), 1u);
  EXPECT_EQ(int64_t(D4.W[2]), 1);
  EXPECT_TRUE(fracIs(fracAt(A, D4.frac()), One, P29));
  EXPECT_EQ(Allocs.size(), 2u);
  // The marks: an exponent past 2^20 and more pairs than the slot has mark
  // the count; a factor past 4096 bits marks the factor, not the count; a
  // part out of range makes the whole so.
  Slot M1(4), M2(1), M3(4), M4(4);
  Make(M1, 4, recipe({{0, 1 << 20}}, desc({{5, 1}}, F1)));
  EXPECT_EQ(M1.count(), UnitCountOut);
  Make(M2, 1, recipe({{0, 1}, {1, 1}}, desc({}, F1)));
  EXPECT_EQ(M2.count(), UnitCountOut);
  Make(M3, 4, recipe({{0, 500}}, desc({}, F1)));
  ASSERT_EQ(M3.count(), 1u);
  EXPECT_EQ(M3.frac()[0], 0u);
  const uint64_t *Dicts3[2] = {M1.W.data(), S.data()};
  A.Make(M4.W.data(), 4, UnitSmallLimbs,
         recipe({{1, 1}, {0, 1}}, desc({}, F1)).data(), Dicts3, &Spill);
  EXPECT_EQ(M4.count(), UnitCountOut);
  // The chain is freed by its glue, which leaves it null.
  A.SpillFree(&Spill);
  EXPECT_EQ(Spill, nullptr);
  EXPECT_TRUE(Allocs.empty());
}

TEST(UnitRuntime, SiteGroupsCombineRoundAndMarkTheirFactors) {
  NEED_ARITH();
  APInt One(64, 1), K(64, 1000), H(64, 3600);
  Frac F1(One, One);
  // dicts: U = KM (id 5), V = M (id 5, factor 1), W = H (id 9), X out of
  // range.
  auto U = desc({{5, 1}}, Frac(K, One)), V = desc({{5, 1}}, F1),
       W = desc({{9, 1}}, Frac(H, One));
  // X is the one word of its mark at the end of a page whose next page is
  // not mapped: a helper that read a pair of it would fault.
  const long Page = sysconf(_SC_PAGESIZE);
  char *Mem =
      static_cast<char *>(mmap(nullptr, 2 * Page, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(Mem, MAP_FAILED);
  ASSERT_EQ(mprotect(Mem + Page, Page, PROT_NONE), 0);
  uint64_t *X = reinterpret_cast<uint64_t *>(Mem + Page) - 1;
  *X = UnitCountOut;
  const uint64_t *Dicts[4] = {U.data(), V.data(), W.data(), X};
  auto P = [](uint64_t I, int64_t E) {
    return std::vector<std::pair<uint64_t, int64_t>>{{I, E}};
  };
  auto Base = desc({}, F1);
  auto RU = recipe(P(0, 1), Base), RV = recipe(P(1, 1), Base),
       RW = recipe(P(2, 1), Base), RX = recipe(P(3, 1), Base);
  auto RUV = recipe({{0, 1}, {1, -1}}, Base); // U/V: no base unit, 1000
  auto RUW = recipe({{0, 1}, {2, -1}}, Base); // U/W: a base unit
  auto F = [&](const std::vector<uint64_t> &G, uint64_t Pr) {
    return A.SiteF(G.data(), Dicts, Pr);
  };
  // Quo U/V: c(res) c(y) / c(x) = 1000, times the static 2/3.
  Frac Static(APInt(64, 2), APInt(64, 3));
  auto Q = group(Static, {{1, RUV, RU, RV}});
  EXPECT_EQ(F(Q, 53),
            sema::Real::fromRat(APInt(64, 2000), APInt(64, 3)).toIEEE(53));
  EXPECT_EQ(F(Q, 24),
            sema::Real::fromRat(APInt(64, 2000), APInt(64, 3)).toIEEE(24));
  uint64_t Words[4];
  A.SiteI(Words, Q.data(), Dicts, 0);
  EXPECT_EQ(Words[0], 666u); // 2000/3 = 666 + 2/3
  EXPECT_EQ(Words[1], 2u);
  EXPECT_EQ(Words[2], 3u);
  // Mul and Conv; two sites multiply; a unit with a base unit is c = 1.
  auto M = group(F1, {{0, RUW, RU, RW}});
  EXPECT_EQ(F(M, 53), 1.0);
  // Quo (1000) then Conv c(U/V) / c(U/W) = 1000 / 1: 10^6.
  auto Two = group(F1, {{1, RUV, RU, RV}, {2, RUV, RUW, RUW}});
  EXPECT_EQ(F(Two, 53), 1e6);
  // Out of range: a unit out of range where its factor is needed, and a
  // static factor that is marked, give 0, the site's panic.
  EXPECT_EQ(F(group(F1, {{3, recipe({{3, 1}, {1, -1}}, Base), RU, RV}}), 53),
            0.0);
  // (The part out of range after one with pairs: no pair of it is read.)
  EXPECT_EQ(F(group(F1, {{3, recipe({{1, 1}, {3, 1}}, Base), RU, RV}}), 53),
            0.0);
  EXPECT_EQ(F(group(Frac(), {{1, RUV, RU, RV}}), 53), 0.0);
  A.SiteI(Words, group(Frac(), {{1, RUV, RU, RV}}).data(), Dicts, 0);
  EXPECT_EQ(Words[2], 0u);
  // The float range (R2, plain code's E0435): 2^200 is +inf in float32 and a
  // number in float64; 2^-200 is 0 in float32; FLT_MAX is inside.
  auto Big = group(Frac(APInt::getOneBitSet(201, 200), One), {});
  EXPECT_EQ(F(Big, 24), 0.0);
  EXPECT_EQ(F(Big, 53), std::ldexp(1.0, 200));
  auto Tiny = group(Frac(One, APInt::getOneBitSet(201, 200)), {});
  EXPECT_EQ(F(Tiny, 24), 0.0);
  EXPECT_EQ(F(Tiny, 53), std::ldexp(1.0, -200));
  // FLT_MAX = (2^24 - 1) 2^104.
  auto FMax = group(Frac(APInt(129, (1u << 24) - 1).shl(104), One), {});
  EXPECT_EQ(F(FMax, 24), double(std::numeric_limits<float>::max()));
  auto Sub = group(Frac(One, APInt::getOneBitSet(200, 149)), {});
  EXPECT_EQ(F(Sub, 24), double(std::numeric_limits<float>::denorm_min()));
  EXPECT_EQ(F(group(Frac(One, APInt::getOneBitSet(1100, 1075)), {}), 53), 0.0);
  munmap(Mem, 2 * Page);
}

} // namespace
