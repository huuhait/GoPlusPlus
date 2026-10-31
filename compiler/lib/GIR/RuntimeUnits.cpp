//===- RuntimeUnits.cpp - Exact unit factors at run time ------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The run-time unit arithmetic of generic code (GEN-14, ADR-0025 Amendment,
// P04.24; the layouts are in gopp/GIR/UnitRuntime.h): numbers of 64-bit limbs,
// exact fractions of at most 4096 bits each way, their rounding to a float,
// the integer scaling by one, and the unit descriptors made of them.
//
// The work is done once per site, at the entry of a function (Body::Hoist), so
// a loop pays none of it. Its cost follows the sizes of the factors: a factor
// of one limb each way takes the 64-bit fast path of unit_fracmul; a product
// of an n-limb and a 1-limb number is n steps (a gcd with a 1-limb number is
// one pass of unit_mod1 and a 64-bit gcd); only two numbers of many limbs each
// take the binary gcd (bits times limbs) and the schoolbook product. Every
// buffer is bounded by UnitMaxLimbs: nothing grows with the program.
//
//===----------------------------------------------------------------------===//

#include "RtEmit.h"
#include "gopp/GIR/UnitRuntime.h"

#include <limits>

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;

namespace {

/// Scratch numbers: the bound, the extra limbs of a shift or a product of two
/// numbers whose bits sum to 4097, and some room.
constexpr unsigned Scratch = UnitMaxLimbs + 8;
constexpr uint64_t MaxBits = 64 * UnitMaxLimbs;

struct Emitter {
  Module &M;
  Context &C;
  llvm::function_ref<std::string(llvm::StringRef)> Name;
  Linkage Link;
  UnitArith A;
  FuncId Trim = NoId, Copy = NoId, Ctz = NoId;
  FuncId Alloc = NoId, Free = NoId; // gopp.mem_resize, gopp.mem_free
  Type *U64, *I64, *PU8, *F64, *Pair, *Void;

  Emitter(Module &M, llvm::function_ref<std::string(llvm::StringRef)> Name,
          Linkage Link)
      : M(M), C(M.Ctx), Name(Name), Link(Link) {
    U64 = C.u64();
    I64 = C.i64();
    PU8 = C.ptr(C.u8());
    F64 = C.floatTy(64);
    Pair = C.tuple({U64, U64});
    Void = C.unit();
  }

  FuncId fn(llvm::StringRef Base, std::initializer_list<Type *> Params,
            Type *Ret) {
    auto F = std::make_unique<Function>();
    F->Name = Name(Base);
    F->Sig = C.func(Params, Ret);
    F->Link = Link;
    F->CC = CallConv::Go;
    return M.addFunction(std::move(F));
  }

  // Limb I of the number at P.
  static Val ld(Rt &R, Val P, Val I) { return R.load(R.padd(P, I * 8), R.U64); }
  static void st(Rt &R, Val P, Val I, Val V) {
    R.store(R.padd(P, I * 8), R.U64, V);
  }
  /// A number buffer of Words limbs in the frame of the function. A function
  /// with a large one is never inlined: its buffer would stay in the frame of
  /// the caller (a recursive generic would carry it at every level).
  Val buf(Rt &R, unsigned Words) {
    if (Words > 8)
      R.F.Inline = Function::InlineHint::Never;
    Var X = R.var(C.array(U64, Words));
    return R.to(PU8, R.addrOf(X));
  }
  /// 1 if B, else 0.
  static Val b2u(Rt &R, Val B) {
    Var X = R.var(R.U64, R.u(0));
    R.If(B, [&] { X = R.u(1); });
    return R.snap(X);
  }
  Val fl(Rt &R, double D) { return Val{&R, constFloat(F64, D)}; }

  void small();
  void numbers();
  void fractions();
  void descriptors();
};

//===----------------------------------------------------------------------===//
// 64-bit helpers
//===----------------------------------------------------------------------===//

void Emitter::small() {
  // unit_gcd64(a, b): Euclid's algorithm.
  A.Gcd64 = fn("unit_gcd64", {U64, U64}, U64);
  {
    Rt R(M, M.func(A.Gcd64));
    Var X = R.var(U64, R.param(0)), Y = R.var(U64, R.param(1));
    R.While([&] { return R.snap(Y.v() != 0); },
            [&] {
              Val T = R.snap(X.v() % Y.v());
              X = Y.v();
              Y = T;
            });
    R.Return(X.v());
    R.finish();
  }

  // unit_mul128(a, b) (hi, lo): the 128-bit product.
  A.Mul128 = fn("unit_mul128", {U64, U64}, Pair);
  {
    Rt R(M, M.func(A.Mul128));
    Val X = R.param(0), Y = R.param(1);
    const uint64_t M32 = 0xffffffffull;
    Val Al = R.snap(X & M32), Ah = R.snap(X >> 32);
    Val Bl = R.snap(Y & M32), Bh = R.snap(Y >> 32);
    Val LL = R.snap(Al * Bl), LH = R.snap(Al * Bh);
    Val HL = R.snap(Ah * Bl), HH = R.snap(Ah * Bh);
    Val Mid = R.snap(R.snap(LL >> 32) + R.snap(LH & M32) + R.snap(HL & M32));
    Val Lo = R.snap(R.snap(LL & M32) | R.snap(Mid << 32));
    Val Hi =
        R.snap(HH + R.snap(LH >> 32) + R.snap(HL >> 32) + R.snap(Mid >> 32));
    R.Return(R.tuple(Pair, {Hi, Lo}));
    R.finish();
  }

  // unit_divmod128(hi, lo, d) (q, rem): (hi*2^64 + lo) / d, hi < d.
  A.DivMod128 = fn("unit_divmod128", {U64, U64, U64}, Pair);
  {
    Rt R(M, M.func(A.DivMod128));
    Val Lo = R.param(1), D = R.param(2);
    Var Rem = R.var(U64, R.param(0)), Q = R.var(U64, R.u(0));
    Var N = R.var(U64, R.u(64));
    R.While([&] { return R.snap(N.v() > R.u(0)); },
            [&] {
              N = R.snap(N.v() - 1);
              Val Carry = R.snap(Rem.v() >> 63);
              Val Bit = R.snap(R.snap(Lo >> N.v()) & 1);
              Rem = R.snap(R.snap(Rem.v() << 1) | Bit);
              Q = R.snap(Q.v() << 1);
              R.If(R.snap(R.lor(R.snap(Carry != 0),
                                [&] { return R.snap(Rem.v() >= D); })),
                   [&] {
                     Rem = R.snap(Rem.v() - D);
                     Q = R.snap(Q.v() | 1);
                   });
            });
    R.Return(R.tuple(Pair, {Q.v(), Rem.v()}));
    R.finish();
  }

  // unit_muldiv(r, n, d): floor(r * n / d) for r < d, the product in 128 bits.
  A.MulDiv = fn("unit_muldiv", {U64, U64, U64}, U64);
  {
    Rt R(M, M.func(A.MulDiv));
    Val Rr = R.param(0), N = R.param(1), D = R.param(2);
    Val P = R.call(A.Mul128, {Rr, N});
    Val Hi = R.snap(R.fld(P, 0)), Lo = R.snap(R.fld(P, 1));
    R.If(R.snap(Hi == 0), [&] { R.Return(R.snap(Lo / D)); });
    Val QR = R.call(A.DivMod128, {Hi, Lo, D});
    R.Return(R.snap(R.fld(QR, 0)));
    R.finish();
  }

  // unit_bitlen(x): the number of bits of x >= 1.
  A.BitLen = fn("unit_bitlen", {U64}, U64);
  {
    Rt R(M, M.func(A.BitLen));
    Var X = R.var(U64, R.param(0)), N = R.var(U64, R.u(1));
    for (unsigned S : {32, 16, 8, 4, 2, 1})
      R.If(R.snap(R.snap(X.v() >> S) != 0), [&] {
        X = R.snap(X.v() >> S);
        N = R.snap(N.v() + S);
      });
    R.Return(N.v());
    R.finish();
  }

  // unit_pow2(k): 2^k as a double (every power from 2^-1074 to 2^1023 is
  // exact, and so is every partial product on the way).
  A.Pow2 = fn("unit_pow2", {I64}, F64);
  {
    Rt R(M, M.func(A.Pow2));
    Val K = R.param(0);
    Var Left = R.var(I64, K), Base = R.var(F64, fl(R, 2.0));
    Var Res = R.var(F64, fl(R, 1.0));
    R.If(R.snap(K < R.i(0)), [&] {
      Left = R.snap(R.i(0) - K);
      Base = fl(R, 0.5);
    });
    R.While([&] { return R.snap(Left.v() > R.i(0)); },
            [&] {
              R.If(R.snap(R.snap(Left.v() & R.i(1)) != R.i(0)),
                   [&] { Res = R.bin(BinOp::Mul, Res.v(), Base.v()); });
              Base = R.bin(BinOp::Mul, Base.v(), Base.v());
              Left = R.snap(Left.v() >> R.i(1));
            });
    R.Return(Res.v());
    R.finish();
  }
}

//===----------------------------------------------------------------------===//
// Numbers
//===----------------------------------------------------------------------===//

void Emitter::numbers() {
  // unit_trim(a, n): the length without the top zero limbs.
  Trim = fn("unit_trim", {PU8, U64}, U64);
  {
    Rt R(M, M.func(Trim));
    Val P = R.param(0);
    Var N = R.var(U64, R.param(1));
    R.While(
        [&] {
          return R.land(R.snap(N.v() > 0), [&] {
            return R.snap(ld(R, P, R.snap(N.v() - 1)) == 0);
          });
        },
        [&] { N = R.snap(N.v() - 1); });
    R.Return(N.v());
    R.finish();
  }
  // unit_copy(dst, a, n).
  Copy = fn("unit_copy", {PU8, PU8, U64}, Void);
  {
    Rt R(M, M.func(Copy));
    Val D = R.param(0), S = R.param(1), N = R.param(2);
    Var I = R.var(U64, R.u(0));
    R.While([&] { return R.snap(I.v() < N); },
            [&] {
              st(R, D, I.v(), ld(R, S, I.v()));
              I = R.snap(I.v() + 1);
            });
    R.finish();
  }

  // unit_bits(a, na): 64 * (na - 1) + the bits of the top limb.
  A.Bits = fn("unit_bits", {PU8, U64}, U64);
  {
    Rt R(M, M.func(A.Bits));
    Val P = R.param(0), N = R.param(1);
    R.If(R.snap(N == 0), [&] { R.Return(R.u(0)); });
    Val Top = R.call(A.BitLen, {ld(R, P, R.snap(N - 1))});
    R.Return(R.snap(R.snap(R.snap(N - 1) * 64) + Top));
    R.finish();
  }

  // unit_ctz(a, na): the trailing zero bits of a > 0.
  Ctz = fn("unit_ctz", {PU8, U64}, U64);
  {
    Rt R(M, M.func(Ctz));
    Val P = R.param(0);
    Var I = R.var(U64, R.u(0));
    R.While([&] { return R.snap(ld(R, P, I.v()) == 0); },
            [&] { I = R.snap(I.v() + 1); });
    Var X = R.var(U64, ld(R, P, I.v())), N = R.var(U64, R.u(0));
    for (unsigned S : {32, 16, 8, 4, 2, 1})
      R.If(R.snap(R.snap(X.v() & ((uint64_t(1) << S) - 1)) == 0), [&] {
        X = R.snap(X.v() >> S);
        N = R.snap(N.v() + S);
      });
    R.Return(R.snap(R.snap(I.v() * 64) + N.v()));
    R.finish();
  }

  // unit_cmp(a, na, b, nb): the sign of a - b.
  A.Cmp = fn("unit_cmp", {PU8, U64, PU8, U64}, I64);
  {
    Rt R(M, M.func(A.Cmp));
    Val Pa = R.param(0), Na = R.param(1), Pb = R.param(2), Nb = R.param(3);
    R.If(R.snap(Na < Nb), [&] { R.Return(R.i(-1)); });
    R.If(R.snap(Na > Nb), [&] { R.Return(R.i(1)); });
    Var I = R.var(U64, Na);
    R.While([&] { return R.snap(I.v() > 0); },
            [&] {
              I = R.snap(I.v() - 1);
              Val X = ld(R, Pa, I.v()), Y = ld(R, Pb, I.v());
              R.If(R.snap(X < Y), [&] { R.Return(R.i(-1)); });
              R.If(R.snap(X > Y), [&] { R.Return(R.i(1)); });
            });
    R.Return(R.i(0));
    R.finish();
  }

  // unit_sub(a, na, b, nb): a -= b in place (a >= b), the new length.
  A.Sub = fn("unit_sub", {PU8, U64, PU8, U64}, U64);
  {
    Rt R(M, M.func(A.Sub));
    Val Pa = R.param(0), Na = R.param(1), Pb = R.param(2), Nb = R.param(3);
    Var I = R.var(U64, R.u(0)), Borrow = R.var(U64, R.u(0));
    R.While(
        [&] {
          return R.land(R.snap(I.v() < Na), [&] {
            return R.lor(R.snap(I.v() < Nb),
                         [&] { return R.snap(Borrow.v() != 0); });
          });
        },
        [&] {
          Val X = ld(R, Pa, I.v());
          Var Y = R.var(U64, R.u(0));
          R.If(R.snap(I.v() < Nb), [&] { Y = ld(R, Pb, I.v()); });
          Val D = R.snap(X - Y.v());
          Val B1 = b2u(R, R.snap(X < Y.v()));
          Val D2 = R.snap(D - Borrow.v());
          Val B2 = b2u(R, R.snap(D < Borrow.v()));
          st(R, Pa, I.v(), D2);
          Borrow = R.snap(B1 | B2);
          I = R.snap(I.v() + 1);
        });
    R.Return(R.call(Trim, {Pa, Na}));
    R.finish();
  }

  // unit_shr(a, na, k): a >>= k in place, the new length.
  A.Shr = fn("unit_shr", {PU8, U64, U64}, U64);
  {
    Rt R(M, M.func(A.Shr));
    Val P = R.param(0), N = R.param(1), K = R.param(2);
    Val W = R.snap(K / 64), S = R.snap(K % 64);
    R.If(R.snap(W >= N), [&] { R.Return(R.u(0)); });
    Val Len = R.snap(N - W);
    Var I = R.var(U64, R.u(0));
    R.While([&] { return R.snap(I.v() < Len); },
            [&] {
              Val J = R.snap(I.v() + W);
              Var X = R.var(U64, R.snap(ld(R, P, J) >> S));
              R.If(R.land(R.snap(S != 0),
                          [&] { return R.snap(R.snap(J + 1) < N); }),
                   [&] {
                     Val Hi = ld(R, P, R.snap(J + 1));
                     X = R.snap(X.v() | R.snap(Hi << R.snap(R.u(64) - S)));
                   });
              st(R, P, I.v(), X.v());
              I = R.snap(I.v() + 1);
            });
    R.Return(R.call(Trim, {P, Len}));
    R.finish();
  }

  // unit_shl(dst, a, na, k): dst = a << k (room for na + k/64 + 1 limbs).
  A.Shl = fn("unit_shl", {PU8, PU8, U64, U64}, U64);
  {
    Rt R(M, M.func(A.Shl));
    Val D = R.param(0), P = R.param(1), N = R.param(2), K = R.param(3);
    R.If(R.snap(N == 0), [&] { R.Return(R.u(0)); });
    Val W = R.snap(K / 64), S = R.snap(K % 64);
    Var I = R.var(U64, R.u(0));
    R.While([&] { return R.snap(I.v() < W); },
            [&] {
              st(R, D, I.v(), R.u(0));
              I = R.snap(I.v() + 1);
            });
    R.If(R.snap(S == 0), [&] {
      R.call(Copy, {R.padd(D, R.snap(W * 8)), P, N});
      R.Return(R.snap(N + W));
    });
    Var Carry = R.var(U64, R.u(0));
    I = R.u(0);
    R.While([&] { return R.snap(I.v() < N); },
            [&] {
              Val X = ld(R, P, I.v());
              st(R, D, R.snap(I.v() + W), R.snap(R.snap(X << S) | Carry.v()));
              Carry = R.snap(X >> R.snap(R.u(64) - S));
              I = R.snap(I.v() + 1);
            });
    st(R, D, R.snap(N + W), Carry.v());
    R.Return(R.call(Trim, {D, R.snap(R.snap(N + W) + 1)}));
    R.finish();
  }

  // unit_mul(dst, a, na, b, nb): the schoolbook product.
  A.Mul = fn("unit_mul", {PU8, PU8, U64, PU8, U64}, U64);
  {
    Rt R(M, M.func(A.Mul));
    Val D = R.param(0), Pa = R.param(1), Na = R.param(2), Pb = R.param(3),
        Nb = R.param(4);
    R.If(R.lor(R.snap(Na == 0), [&] { return R.snap(Nb == 0); }),
         [&] { R.Return(R.u(0)); });
    Val Len = R.snap(Na + Nb);
    Var I = R.var(U64, R.u(0));
    R.While([&] { return R.snap(I.v() < Len); },
            [&] {
              st(R, D, I.v(), R.u(0));
              I = R.snap(I.v() + 1);
            });
    I = R.u(0);
    R.While([&] { return R.snap(I.v() < Na); },
            [&] {
              Val Ai = ld(R, Pa, I.v());
              Var Carry = R.var(U64, R.u(0)), J = R.var(U64, R.u(0));
              R.While([&] { return R.snap(J.v() < Nb); },
                      [&] {
                        Val T = R.call(A.Mul128, {Ai, ld(R, Pb, J.v())});
                        Val Hi = R.snap(R.fld(T, 0)), Lo = R.snap(R.fld(T, 1));
                        Val K = R.snap(I.v() + J.v());
                        Val S = R.snap(ld(R, D, K) + Lo);
                        Val C1 = b2u(R, R.snap(S < Lo));
                        Val S2 = R.snap(S + Carry.v());
                        Val C2 = b2u(R, R.snap(S2 < S));
                        st(R, D, K, S2);
                        Carry = R.snap(R.snap(Hi + C1) + C2);
                        J = R.snap(J.v() + 1);
                      });
              st(R, D, R.snap(I.v() + Nb), Carry.v());
              I = R.snap(I.v() + 1);
            });
    R.Return(R.call(Trim, {D, Len}));
    R.finish();
  }

  // unit_mod1(a, na, m): a mod m, one 128-bit step per limb from the top.
  A.Mod1 = fn("unit_mod1", {PU8, U64, U64}, U64);
  {
    Rt R(M, M.func(A.Mod1));
    Val P = R.param(0), N = R.param(1), Md = R.param(2);
    Var I = R.var(U64, N), Rem = R.var(U64, R.u(0));
    R.While([&] { return R.snap(I.v() > 0); },
            [&] {
              I = R.snap(I.v() - 1);
              Val QR = R.call(A.DivMod128, {Rem.v(), ld(R, P, I.v()), Md});
              Rem = R.snap(R.fld(QR, 1));
            });
    R.Return(Rem.v());
    R.finish();
  }

  // unit_divexact(dst, a, na, b, nb): a / b where b divides a, from the low
  // limb up (the inverse of b's low limb modulo 2^64, b made odd first).
  A.DivExact = fn("unit_divexact", {PU8, PU8, U64, PU8, U64}, U64);
  {
    Rt R(M, M.func(A.DivExact));
    Val D = R.param(0), Pa = R.param(1), Na = R.param(2), Pb = R.param(3),
        Nb = R.param(4);
    R.If(R.snap(Na == 0), [&] { R.Return(R.u(0)); });
    R.If(
        R.land(R.snap(Nb == 1), [&] { return R.snap(ld(R, Pb, R.u(0)) == 1); }),
        [&] {
          R.call(Copy, {D, Pa, Na});
          R.Return(Na);
        });
    Val X = buf(R, Scratch), Y = buf(R, Scratch);
    R.call(Copy, {X, Pa, Na});
    R.call(Copy, {Y, Pb, Nb});
    Val T = R.call(Ctz, {Pb, Nb});
    Val NX = R.call(A.Shr, {X, Na, T}), NY = R.call(A.Shr, {Y, Nb, T});
    Val Y0 = ld(R, Y, R.u(0));
    // Newton: Y0 * Y0 = 1 modulo 8, and every step doubles the bits.
    Var Inv = R.var(U64, Y0);
    for (int K = 0; K < 5; ++K)
      Inv = R.snap(Inv.v() * R.snap(R.u(2) - R.snap(Y0 * Inv.v())));
    Val Nq = R.snap(R.snap(NX - NY) + 1);
    Var I = R.var(U64, R.u(0));
    R.While([&] { return R.snap(I.v() < Nq); },
            [&] {
              Val Q = R.snap(ld(R, X, I.v()) * Inv.v());
              st(R, D, I.v(), Q);
              // X -= Q * Y << (64 I)
              Var Cy = R.var(U64, R.u(0)), J = R.var(U64, R.u(0));
              R.While([&] { return R.snap(J.v() < NY); },
                      [&] {
                        Val P = R.call(A.Mul128, {Q, ld(R, Y, J.v())});
                        Val Hi = R.snap(R.fld(P, 0)), Lo = R.snap(R.fld(P, 1));
                        Val Lo2 = R.snap(Lo + Cy.v());
                        Val C1 = b2u(R, R.snap(Lo2 < Lo));
                        Val K = R.snap(I.v() + J.v());
                        Val Tx = ld(R, X, K);
                        st(R, X, K, R.snap(Tx - Lo2));
                        Val C2 = b2u(R, R.snap(Tx < Lo2));
                        Cy = R.snap(R.snap(Hi + C1) + C2);
                        J = R.snap(J.v() + 1);
                      });
              Var K = R.var(U64, R.snap(I.v() + NY));
              R.While(
                  [&] {
                    return R.land(R.snap(Cy.v() != 0),
                                  [&] { return R.snap(K.v() < NX); });
                  },
                  [&] {
                    Val Tx = ld(R, X, K.v());
                    st(R, X, K.v(), R.snap(Tx - Cy.v()));
                    Cy = b2u(R, R.snap(Tx < Cy.v()));
                    K = R.snap(K.v() + 1);
                  });
              I = R.snap(I.v() + 1);
            });
    R.Return(R.call(Trim, {D, Nq}));
    R.finish();
  }

  // unit_gcd(dst, a, na, b, nb): a 1-limb operand reduces the other with
  // unit_mod1 and ends in unit_gcd64; else the binary gcd, which drops to
  // that as soon as one side has one limb.
  A.Gcd = fn("unit_gcd", {PU8, PU8, U64, PU8, U64}, U64);
  {
    Rt R(M, M.func(A.Gcd));
    Val D = R.param(0), Pa = R.param(1), Na = R.param(2), Pb = R.param(3),
        Nb = R.param(4);
    auto One = [&](Val Big, Val NBig, Val Small) {
      Val Rm = R.call(A.Mod1, {Big, NBig, Small});
      st(R, D, R.u(0), R.call(A.Gcd64, {Small, Rm}));
      R.Return(R.u(1));
    };
    R.If(R.snap(Nb == 1), [&] { One(Pa, Na, ld(R, Pb, R.u(0))); });
    R.If(R.snap(Na == 1), [&] { One(Pb, Nb, ld(R, Pa, R.u(0))); });
    Val X = buf(R, Scratch), Y = buf(R, Scratch);
    R.call(Copy, {X, Pa, Na});
    R.call(Copy, {Y, Pb, Nb});
    Val Za = R.call(Ctz, {Pa, Na}), Zb = R.call(Ctz, {Pb, Nb});
    Var K = R.var(U64, Za);
    R.If(R.snap(Zb < Za), [&] { K = Zb; });
    Var Pu = R.var(PU8, X), Pv = R.var(PU8, Y);
    Var Nu = R.var(U64, R.call(A.Shr, {X, Na, Za}));
    Var Nv = R.var(U64, R.call(A.Shr, {Y, Nb, Zb}));
    Val G1 = buf(R, 1);
    Var Gp = R.var(PU8, G1), Ng = R.var(U64, R.u(1));
    // Both odd from here on.
    R.While([&] { return R.boolean(true); },
            [&] {
              R.If(R.snap(Nu.v() == 1), [&] {
                Val U0 = ld(R, Pu.v(), R.u(0));
                Val Rm = R.call(A.Mod1, {Pv.v(), Nv.v(), U0});
                st(R, G1, R.u(0), R.call(A.Gcd64, {U0, Rm}));
                R.Break();
              });
              R.If(R.snap(Nv.v() == 1), [&] {
                Val V0 = ld(R, Pv.v(), R.u(0));
                Val Rm = R.call(A.Mod1, {Pu.v(), Nu.v(), V0});
                st(R, G1, R.u(0), R.call(A.Gcd64, {V0, Rm}));
                R.Break();
              });
              Val Cmp = R.call(A.Cmp, {Pu.v(), Nu.v(), Pv.v(), Nv.v()});
              R.If(R.snap(Cmp == R.i(0)), [&] {
                Gp = Pu.v();
                Ng = Nu.v();
                R.Break();
              });
              R.If(R.snap(Cmp < R.i(0)), [&] {
                Val Tp = R.snap(Pu.v()), Tn = R.snap(Nu.v());
                Pu = Pv.v();
                Nu = Nv.v();
                Pv = Tp;
                Nv = Tn;
              });
              Val N1 = R.call(A.Sub, {Pu.v(), Nu.v(), Pv.v(), Nv.v()});
              Val Z = R.call(Ctz, {Pu.v(), N1});
              Nu = R.call(A.Shr, {Pu.v(), N1, Z});
            });
    R.Return(R.call(A.Shl, {D, Gp.v(), Ng.v(), K.v()}));
    R.finish();
  }

  // unit_divq(r, nr, y, ny, out): the quotient when it has at most 64 bits,
  // by shift and subtract over its bits only.
  A.DivQ = fn("unit_divq", {PU8, U64, PU8, U64, PU8}, U64);
  {
    Rt R(M, M.func(A.DivQ));
    Val Pr = R.param(0), Nr = R.param(1), Py = R.param(2), Ny = R.param(3),
        Out = R.param(4);
    st(R, Out, R.u(0), Nr);
    st(R, Out, R.u(1), R.u(0));
    Val Br = R.call(A.Bits, {Pr, Nr}), By = R.call(A.Bits, {Py, Ny});
    R.If(R.snap(Br < By), [&] { R.Return(R.u(0)); });
    Val K = R.snap(Br - By);
    R.If(R.snap(K > 64), [&] {
      st(R, Out, R.u(1), R.u(1));
      R.Return(R.u(0));
    });
    Val Ys = buf(R, Scratch);
    Var Nys = R.var(U64, R.call(A.Shl, {Ys, Py, Ny, K}));
    Var NR = R.var(U64, Nr), Q = R.var(U64, R.u(0)), I = R.var(U64, K);
    R.While([&] { return R.boolean(true); },
            [&] {
              Val Cmp = R.call(A.Cmp, {Pr, NR.v(), Ys, Nys.v()});
              R.If(R.snap(Cmp >= R.i(0)), [&] {
                R.If(R.snap(I.v() == 64), [&] {
                  st(R, Out, R.u(1), R.u(1));
                  R.Return(R.u(0));
                });
                NR = R.call(A.Sub, {Pr, NR.v(), Ys, Nys.v()});
                Q = R.snap(Q.v() | R.snap(R.u(1) << I.v()));
              });
              R.If(R.snap(I.v() == 0), [&] { R.Break(); });
              I = R.snap(I.v() - 1);
              Nys = R.call(A.Shr, {Ys, Nys.v(), R.u(1)});
            });
    st(R, Out, R.u(0), NR.v());
    R.Return(Q.v());
    R.finish();
  }
}

//===----------------------------------------------------------------------===//
// Fractions
//===----------------------------------------------------------------------===//

void Emitter::fractions() {
  // unit_fracput(dst, n, nn, d, nd): writes a fraction (n and d not in dst).
  FuncId Put = fn("unit_fracput", {PU8, PU8, U64, PU8, U64}, Void);
  {
    Rt R(M, M.func(Put));
    Val D = R.param(0), N = R.param(1), Nn = R.param(2), Dd = R.param(3),
        Nd = R.param(4);
    st(R, D, R.u(0), Nn);
    st(R, D, R.u(1), Nd);
    R.call(Copy, {R.padd(D, R.u(16)), N, Nn});
    R.call(Copy, {R.padd(D, R.snap(R.u(16) + R.snap(Nn * 8))), Dd, Nd});
    R.finish();
  }

  // unit_fracmul_big(dst, a, ainv, b, binv): the general product, with its
  // scratch numbers in a frame of its own (unit_fracmul's is small).
  FuncId FracMulBig = fn("unit_fracmul_big", {PU8, PU8, U64, PU8, U64}, Void);
  {
    Rt R(M, M.func(FracMulBig));
    Val D = R.param(0), Pa = R.param(1), Ia = R.param(2), Pb = R.param(3),
        Ib = R.param(4);
    Val Na = ld(R, Pa, R.u(0)), Nb = ld(R, Pb, R.u(0));
    R.If(R.lor(R.snap(Na == 0), [&] { return R.snap(Nb == 0); }), [&] {
      st(R, D, R.u(0), R.u(0));
      R.Return();
    });
    // The parts, the inverse swapped: n1/d1 * n2/d2.
    struct Parts {
      Var N, Nn, Dd, Nd;
    };
    auto Get = [&](Val P, Val Inv) {
      Val Nn = ld(R, P, R.u(0)), Nd = ld(R, P, R.u(1));
      Val Np = R.padd(P, R.u(16)), Dp = R.padd(P, R.snap(R.u(16) + Nn * 8));
      Parts X{R.var(PU8, Np), R.var(U64, Nn), R.var(PU8, Dp), R.var(U64, Nd)};
      R.If(R.snap(Inv != 0), [&] {
        X.N = Dp;
        X.Nn = Nd;
        X.Dd = Np;
        X.Nd = Nn;
      });
      return X;
    };
    Parts X = Get(Pa, Ia), Y = Get(Pb, Ib);
    // g1 = gcd(n1, d2), g2 = gcd(n2, d1); the product of the reduced parts
    // is reduced.
    Val G = buf(R, Scratch), SA = buf(R, Scratch), SB = buf(R, Scratch),
        SC = buf(R, Scratch), SE = buf(R, Scratch);
    auto Reduce = [&](Var &P1, Var &N1, Var &P2, Var &N2, Val S1, Val S2) {
      Val Ng = R.call(A.Gcd, {G, P1.v(), N1.v(), P2.v(), N2.v()});
      Val IsOne = R.land(R.snap(Ng == 1),
                         [&] { return R.snap(ld(R, G, R.u(0)) == 1); });
      R.If(R.not_(IsOne), [&] {
        N1 = R.call(A.DivExact, {S1, P1.v(), N1.v(), G, Ng});
        P1 = S1;
        N2 = R.call(A.DivExact, {S2, P2.v(), N2.v(), G, Ng});
        P2 = S2;
      });
    };
    Reduce(X.N, X.Nn, Y.Dd, Y.Nd, SA, SB);
    Reduce(Y.N, Y.Nn, X.Dd, X.Nd, SC, SE);
    auto Mark = [&] {
      st(R, D, R.u(0), R.u(0));
      R.Return();
    };
    auto TooBig = [&](Var &P1, Var &N1, Var &P2, Var &N2) {
      Val B = R.snap(R.call(A.Bits, {P1.v(), N1.v()}) +
                     R.call(A.Bits, {P2.v(), N2.v()}));
      R.If(R.snap(B > MaxBits + 1), Mark);
    };
    TooBig(X.N, X.Nn, Y.N, Y.Nn);
    TooBig(X.Dd, X.Nd, Y.Dd, Y.Nd);
    Val SN = buf(R, Scratch), SD = buf(R, Scratch);
    Val Nn = R.call(A.Mul, {SN, X.N.v(), X.Nn.v(), Y.N.v(), Y.Nn.v()});
    Val Nd = R.call(A.Mul, {SD, X.Dd.v(), X.Nd.v(), Y.Dd.v(), Y.Nd.v()});
    R.If(R.lor(R.snap(Nn > UnitMaxLimbs),
               [&] { return R.snap(Nd > UnitMaxLimbs); }),
         Mark);
    R.call(Put, {D, SN, Nn, SD, Nd});
    R.finish();
  }

  // unit_fracmul(dst, a, ainv, b, binv): the marks, and four 1-limb parts in
  // 64-bit words; else unit_fracmul_big.
  A.FracMul = fn("unit_fracmul", {PU8, PU8, U64, PU8, U64}, Void);
  {
    Rt R(M, M.func(A.FracMul));
    Val D = R.param(0), Pa = R.param(1), Ia = R.param(2), Pb = R.param(3),
        Ib = R.param(4);
    Val Na = ld(R, Pa, R.u(0)), Nb = ld(R, Pb, R.u(0));
    R.If(R.lor(R.snap(Na == 0), [&] { return R.snap(Nb == 0); }), [&] {
      st(R, D, R.u(0), R.u(0));
      R.Return();
    });
    // b = 1 (most factors of a site are): dst = a is nothing to do.
    auto IsOne = [&](Val P) {
      return R.land(
          R.snap(R.snap(ld(R, P, R.u(0)) | ld(R, P, R.u(1))) == 1), [&] {
            return R.snap(R.snap(ld(R, P, R.u(2)) | ld(R, P, R.u(3))) == 1);
          });
    };
    R.If(R.land(R.land(R.snap(R.to(U64, D) == R.to(U64, Pa)),
                       [&] { return R.snap(Ia == 0); }),
                [&] { return IsOne(Pb); }),
         [&] { R.Return(); });
    // The parts, the inverse swapped: n1/d1 * n2/d2.
    struct Parts {
      Var N, Nn, Dd, Nd;
    };
    auto Get = [&](Val P, Val Inv) {
      Val Nn = ld(R, P, R.u(0)), Nd = ld(R, P, R.u(1));
      Val Np = R.padd(P, R.u(16)), Dp = R.padd(P, R.snap(R.u(16) + Nn * 8));
      Parts X{R.var(PU8, Np), R.var(U64, Nn), R.var(PU8, Dp), R.var(U64, Nd)};
      R.If(R.snap(Inv != 0), [&] {
        X.N = Dp;
        X.Nn = Nd;
        X.Dd = Np;
        X.Nd = Nn;
      });
      return X;
    };
    Parts X = Get(Pa, Ia), Y = Get(Pb, Ib);
    // One side 1: the other, copied (not onto itself: that is the general
    // path's, through its scratch).
    auto Ptr = [&](Val P) { return R.to(U64, P); };
    R.If(R.land(IsOne(Pb), [&] { return R.snap(Ptr(D) != Ptr(Pa)); }), [&] {
      R.call(Put, {D, X.N.v(), X.Nn.v(), X.Dd.v(), X.Nd.v()});
      R.Return();
    });
    R.If(R.land(IsOne(Pa), [&] { return R.snap(Ptr(D) != Ptr(Pb)); }), [&] {
      R.call(Put, {D, Y.N.v(), Y.Nn.v(), Y.Dd.v(), Y.Nd.v()});
      R.Return();
    });
    // Fast path: four 1-limb parts (no gcd where a side is 1).
    Val Small = R.land(R.snap(R.snap(X.Nn.v() | X.Nd.v()) == 1), [&] {
      return R.snap(R.snap(Y.Nn.v() | Y.Nd.v()) == 1);
    });
    R.If(Small, [&] {
      Val N1 = ld(R, X.N.v(), R.u(0)), D1 = ld(R, X.Dd.v(), R.u(0));
      Val N2 = ld(R, Y.N.v(), R.u(0)), D2 = ld(R, Y.Dd.v(), R.u(0));
      Var G1 = R.var(U64, R.u(1)), G2 = R.var(U64, R.u(1));
      R.If(R.land(R.snap(R.snap(N1 | D1) != 1),
                  [&] { return R.snap(R.snap(N2 | D2) != 1); }),
           [&] {
             G1 = R.call(A.Gcd64, {N1, D2});
             G2 = R.call(A.Gcd64, {N2, D1});
           });
      Val Pn = R.call(A.Mul128, {R.snap(N1 / G1.v()), R.snap(N2 / G2.v())});
      Val Pd = R.call(A.Mul128, {R.snap(D1 / G2.v()), R.snap(D2 / G1.v())});
      Val NH = R.snap(R.fld(Pn, 0)), NL = R.snap(R.fld(Pn, 1));
      Val DH = R.snap(R.fld(Pd, 0)), DL = R.snap(R.fld(Pd, 1));
      Var Nn = R.var(U64, R.u(1)), Nd = R.var(U64, R.u(1));
      R.If(R.snap(NH != 0), [&] { Nn = R.u(2); });
      R.If(R.snap(DH != 0), [&] { Nd = R.u(2); });
      st(R, D, R.u(0), Nn.v());
      st(R, D, R.u(1), Nd.v());
      st(R, D, R.u(2), NL);
      R.If(R.snap(NH != 0), [&] { st(R, D, R.u(3), NH); });
      Val Dp = R.padd(D, R.snap(R.u(16) + R.snap(Nn.v() * 8)));
      st(R, Dp, R.u(0), DL);
      R.If(R.snap(DH != 0), [&] { st(R, Dp, R.u(1), DH); });
      R.Return();
    });
    R.call(FracMulBig, {D, Pa, Ia, Pb, Ib});
    R.finish();
  }

  // unit_powl(dst, a, na, e): a^e (e >= 1), 0 if a power on the way passes
  // the bound (each is at most the result).
  FuncId PowL = fn("unit_powl", {PU8, PU8, U64, U64}, U64);
  {
    Rt R(M, M.func(PowL));
    Val D = R.param(0), P = R.param(1), N = R.param(2), E0 = R.param(3);
    R.If(R.land(R.snap(N == 1), [&] { return R.snap(ld(R, P, R.u(0)) == 1); }),
         [&] {
           st(R, D, R.u(0), R.u(1));
           R.Return(R.u(1));
         });
    Val B1 = buf(R, Scratch), B2 = buf(R, Scratch), B3 = buf(R, Scratch);
    st(R, B1, R.u(0), R.u(1));
    R.call(Copy, {B2, P, N});
    Var Rp = R.var(PU8, B1), Pp = R.var(PU8, B2), Tp = R.var(PU8, B3);
    Var Nr = R.var(U64, R.u(1)), Np = R.var(U64, N), E = R.var(U64, E0);
    auto Step = [&](Var &Dst, Var &ND, Var &X, Var &NX, Var &Y, Var &NY) {
      Val B = R.snap(R.call(A.Bits, {X.v(), NX.v()}) +
                     R.call(A.Bits, {Y.v(), NY.v()}));
      R.If(R.snap(B > MaxBits + 1), [&] { R.Return(R.u(0)); });
      Val Nt = R.call(A.Mul, {Tp.v(), X.v(), NX.v(), Y.v(), NY.v()});
      R.If(R.snap(Nt > UnitMaxLimbs), [&] { R.Return(R.u(0)); });
      Val Old = R.snap(Dst.v());
      Dst = Tp.v();
      Tp = Old;
      ND = Nt;
    };
    R.While([&] { return R.snap(E.v() > 0); },
            [&] {
              R.If(R.snap(R.snap(E.v() & 1) != 0),
                   [&] { Step(Rp, Nr, Rp, Nr, Pp, Np); });
              E = R.snap(E.v() >> 1);
              R.If(R.snap(E.v() > 0), [&] { Step(Pp, Np, Pp, Np, Pp, Np); });
            });
    R.call(Copy, {D, Rp.v(), Nr.v()});
    R.Return(Nr.v());
    R.finish();
  }

  // unit_fracpow(dst, b, e): the numerator and the denominator raised apart
  // (a reduced fraction stays reduced).
  A.FracPow = fn("unit_fracpow", {PU8, PU8, I64}, Void);
  {
    Rt R(M, M.func(A.FracPow));
    Val D = R.param(0), P = R.param(1), E = R.param(2);
    auto Mark = [&] {
      st(R, D, R.u(0), R.u(0));
      R.Return();
    };
    Val Nn = ld(R, P, R.u(0));
    R.If(R.snap(Nn == 0), Mark);
    Val Nd = ld(R, P, R.u(1));
    Val Np = R.padd(P, R.u(16)), Dp = R.padd(P, R.snap(R.u(16) + Nn * 8));
    Var Abs = R.var(I64, E);
    R.If(R.snap(E < R.i(0)), [&] { Abs = R.snap(R.i(0) - E); });
    Val Eu = R.to(U64, Abs.v());
    Val SN = buf(R, Scratch), SD = buf(R, Scratch);
    Val N1 = R.call(PowL, {SN, Np, Nn, Eu});
    R.If(R.snap(N1 == 0), Mark);
    Val N2 = R.call(PowL, {SD, Dp, Nd, Eu});
    R.If(R.snap(N2 == 0), Mark);
    R.IfElse(
        R.snap(E < R.i(0)), [&] { R.call(Put, {D, SD, N2, SN, N1}); },
        [&] { R.call(Put, {D, SN, N1, SD, N2}); });
    R.finish();
  }

  // unit_ratio_big(f, p): n / d rounded once (Real::toIEEE). E =
  // floor(log2(n/d)), the bits kept (fewer below the normal range), q = floor(n
  // 2^(keep-E) / d) with one more bit than kept, the remainder as the sticky
  // bit.
  FuncId RatioBig = fn("unit_ratio_big", {PU8, U64}, F64);
  {
    Rt R(M, M.func(RatioBig));
    Val P = R.param(0), Prec = R.param(1);
    Val Nn = ld(R, P, R.u(0));
    R.If(R.snap(Nn == 0), [&] { R.Return(fl(R, 0.0)); });
    Val Nd = ld(R, P, R.u(1));
    Val Np = R.padd(P, R.u(16)), Dp = R.padd(P, R.snap(R.u(16) + Nn * 8));
    Val Bn = R.call(A.Bits, {Np, Nn}), Bd = R.call(A.Bits, {Dp, Nd});
    Val E0 = R.snap(R.to(I64, Bn) - R.to(I64, Bd));
    Val T = buf(R, Scratch);
    Var Ge = R.var(R.Bool, R.boolean(false));
    R.IfElse(
        R.snap(E0 >= R.i(0)),
        [&] {
          Val Nt = R.call(A.Shl, {T, Dp, Nd, R.to(U64, E0)});
          Ge = R.snap(R.call(A.Cmp, {Np, Nn, T, Nt}) >= R.i(0));
        },
        [&] {
          Val Nt = R.call(A.Shl, {T, Np, Nn, R.to(U64, R.snap(R.i(0) - E0))});
          Ge = R.snap(R.call(A.Cmp, {T, Nt, Dp, Nd}) >= R.i(0));
        });
    Var E = R.var(I64, E0);
    R.If(R.not_(Ge.v()), [&] { E = R.snap(E0 - R.i(1)); });
    Var EMin = R.var(I64, R.i(-1022)), EMax = R.var(I64, R.i(1023));
    R.If(R.snap(Prec == 24), [&] {
      EMin = R.i(-126);
      EMax = R.i(127);
    });
    R.If(R.snap(E.v() > EMax.v()),
         [&] { R.Return(fl(R, std::numeric_limits<double>::infinity())); });
    Val Pi = R.to(I64, Prec);
    Var Keep = R.var(I64, Pi);
    R.If(R.snap(E.v() < EMin.v()),
         [&] { Keep = R.snap(Pi - R.snap(EMin.v() - E.v())); });
    R.If(R.snap(Keep.v() < R.i(0)), [&] { R.Return(fl(R, 0.0)); });
    Val S = R.snap(Keep.v() - E.v());
    Val X = buf(R, Scratch), Yb = buf(R, Scratch);
    Var Nx = R.var(U64, R.u(0)), Yp = R.var(PU8, Dp), Ny = R.var(U64, Nd);
    R.IfElse(
        R.snap(S >= R.i(0)),
        [&] { Nx = R.call(A.Shl, {X, Np, Nn, R.to(U64, S)}); },
        [&] {
          R.call(Copy, {X, Np, Nn});
          Nx = Nn;
          Ny = R.call(A.Shl, {Yb, Dp, Nd, R.to(U64, R.snap(R.i(0) - S))});
          Yp = Yb;
        });
    Val Out = buf(R, 2);
    Val Q2 = R.call(A.DivQ, {X, Nx.v(), Yp.v(), Ny.v(), Out});
    Val Sticky = R.snap(ld(R, Out, R.u(0)) != 0);
    Var Q = R.var(U64, R.snap(Q2 >> 1));
    Val Half = R.snap(R.snap(Q2 & 1) != 0);
    Val Up = R.land(Half, [&] {
      return R.lor(Sticky, [&] { return R.snap(R.snap(Q.v() & 1) != 0); });
    });
    R.If(Up, [&] { Q = R.snap(Q.v() + 1); });
    Val Pw = R.call(A.Pow2, {R.snap(R.snap(E.v() - Keep.v()) + R.i(1))});
    Val V = R.bin(BinOp::Mul, R.to(F64, Q.v()), Pw);
    // (A float32 that the rounding carried to 2^128 is +inf, as the compiler
    // says.)
    R.If(R.land(R.snap(Prec == 24),
                [&] {
                  return R.bin(BinOp::Gt, V,
                               fl(R, std::numeric_limits<float>::max()));
                }),
         [&] { R.Return(fl(R, std::numeric_limits<double>::infinity())); });
    R.Return(V);
    R.finish();
  }

  // unit_ratio(f, p): a numerator and a denominator of one limb each take
  // 64-bit words (the value is within 2^+-64, so no subnormal and no
  // overflow): q = n 2^s / d with p + 2 or p + 3 bits, then rounded to p
  // bits with the sticky bit; else unit_ratio_big.
  A.Ratio = fn("unit_ratio", {PU8, U64}, F64);
  {
    Rt R(M, M.func(A.Ratio));
    Val P = R.param(0), Prec = R.param(1);
    Val Nn = ld(R, P, R.u(0));
    R.If(R.snap(Nn != 1), [&] { R.Return(R.call(RatioBig, {P, Prec})); });
    R.If(R.snap(ld(R, P, R.u(1)) != 1),
         [&] { R.Return(R.call(RatioBig, {P, Prec})); });
    Val N = ld(R, P, R.u(2)), D = ld(R, P, R.u(3));
    Val Bn = R.call(A.BitLen, {N}), Bd = R.call(A.BitLen, {D});
    Val S = R.snap(R.snap(R.to(I64, Prec) + R.i(2)) + R.snap(R.to(I64, Bd)) -
                   R.to(I64, Bn));
    Var Q = R.var(U64, R.u(0)), Sticky = R.var(R.Bool, R.boolean(false));
    R.IfElse(
        R.snap(S >= R.i(0)),
        [&] {
          Var Hi = R.var(U64, R.u(0)), Lo = R.var(U64, R.u(0));
          Val Su = R.to(U64, S);
          R.IfElse(
              R.snap(Su >= 64), [&] { Hi = R.snap(N << R.snap(Su - 64)); },
              [&] {
                R.If(R.snap(Su == 0), [&] { Lo = N; });
                R.If(R.snap(Su != 0), [&] {
                  Hi = R.snap(N >> R.snap(R.u(64) - Su));
                  Lo = R.snap(N << Su);
                });
              });
          Val QR = R.call(A.DivMod128, {Hi.v(), Lo.v(), D});
          Q = R.snap(R.fld(QR, 0));
          Sticky = R.snap(R.fld(QR, 1) != 0);
        },
        [&] {
          Val T = R.to(U64, R.snap(R.i(0) - S));
          Val N2 = R.snap(N >> T);
          Val Lost = R.snap(N & R.snap(R.snap(R.u(1) << T) - 1));
          Q = R.snap(N2 / D);
          Sticky = R.snap(R.lor(R.snap(R.snap(N2 % D) != 0),
                                [&] { return R.snap(Lost != 0); }));
        });
    Val Sh = R.snap(R.call(A.BitLen, {Q.v()}) - Prec);
    Val Low = R.snap(Q.v() & R.snap(R.snap(R.u(1) << Sh) - 1));
    Val Half = R.snap(R.u(1) << R.snap(Sh - 1));
    Var Q2 = R.var(U64, R.snap(Q.v() >> Sh));
    Val Up = R.lor(R.snap(Low > Half), [&] {
      return R.land(R.snap(Low == Half), [&] {
        return R.lor(Sticky.v(),
                     [&] { return R.snap(R.snap(Q2.v() & 1) != 0); });
      });
    });
    R.If(Up, [&] { Q2 = R.snap(Q2.v() + 1); });
    Val Pw = R.call(A.Pow2, {R.snap(R.to(I64, Sh) - S)});
    R.Return(R.bin(BinOp::Mul, R.to(F64, Q2.v()), Pw));
    R.finish();
  }

  // unit_intfactor_big(out, f, mode): the general case.
  FuncId IntFactorBig = fn("unit_intfactor_big", {PU8, PU8, U64}, Void);
  {
    Rt R(M, M.func(IntFactorBig));
    Val Out = R.param(0), P = R.param(1), Mode = R.param(2);
    auto Put4 = [&](Val A0, Val N, Val D, Val Inv) {
      st(R, Out, R.u(0), A0);
      st(R, Out, R.u(1), N);
      st(R, Out, R.u(2), D);
      st(R, Out, R.u(3), Inv);
    };
    Val Nn = ld(R, P, R.u(0));
    R.If(R.snap(Nn == 0), [&] {
      Put4(R.u(0), R.u(0), R.u(0), R.u(0));
      R.Return();
    });
    Val Nd = ld(R, P, R.u(1));
    Val Np = R.padd(P, R.u(16)), Dp = R.padd(P, R.snap(R.u(16) + Nn * 8));
    Var Inv = R.var(U64, R.u(0));
    R.If(R.snap(Mode == 1), [&] { Inv = R.u(1); });
    R.If(R.snap(Mode == 2), [&] {
      R.If(R.snap(R.call(A.Cmp, {Np, Nn, Dp, Nd}) < R.i(0)),
           [&] { Inv = R.u(1); });
    });
    // p / q
    Var Pp = R.var(PU8, Np), Pn = R.var(U64, Nn), Qp = R.var(PU8, Dp),
        Qn = R.var(U64, Nd);
    R.If(R.snap(Inv.v() != 0), [&] {
      Pp = Dp;
      Pn = Nd;
      Qp = Np;
      Qn = Nn;
    });
    // An integer: A = p mod 2^64.
    R.If(R.land(R.snap(Qn.v() == 1),
                [&] { return R.snap(ld(R, Qp.v(), R.u(0)) == 1); }),
         [&] {
           Put4(ld(R, Pp.v(), R.u(0)), R.u(0), R.u(1), Inv.v());
           R.Return();
         });
    // p = A q + B: long division, the quotient's low 64 bits kept.
    Val Rb = buf(R, Scratch), Ys = buf(R, Scratch);
    R.call(Copy, {Rb, Pp.v(), Pn.v()});
    Var Nr = R.var(U64, Pn.v()), Aq = R.var(U64, R.u(0));
    Val Bp = R.call(A.Bits, {Pp.v(), Pn.v()}),
        Bq = R.call(A.Bits, {Qp.v(), Qn.v()});
    R.If(R.snap(Bp >= Bq), [&] {
      Val K = R.snap(Bp - Bq);
      Var Nys = R.var(U64, R.call(A.Shl, {Ys, Qp.v(), Qn.v(), K}));
      Var I = R.var(U64, K);
      R.While([&] { return R.boolean(true); },
              [&] {
                Val Cmp = R.call(A.Cmp, {Rb, Nr.v(), Ys, Nys.v()});
                R.If(R.snap(Cmp >= R.i(0)), [&] {
                  Nr = R.call(A.Sub, {Rb, Nr.v(), Ys, Nys.v()});
                  R.If(R.snap(I.v() < 64),
                       [&] { Aq = R.snap(Aq.v() | R.snap(R.u(1) << I.v())); });
                });
                R.If(R.snap(I.v() == 0), [&] { R.Break(); });
                I = R.snap(I.v() - 1);
                Nys = R.call(A.Shr, {Ys, Nys.v(), R.u(1)});
              });
    });
    // q in one limb: n / d = B / q exactly.
    R.If(R.snap(Qn.v() == 1), [&] {
      Var B = R.var(U64, R.u(0));
      R.If(R.snap(Nr.v() != 0), [&] { B = ld(R, Rb, R.u(0)); });
      Put4(Aq.v(), B.v(), ld(R, Qp.v(), R.u(0)), Inv.v());
      R.Return();
    });
    R.If(R.snap(Nr.v() == 0), [&] {
      Put4(Aq.v(), R.u(0), R.u(1), Inv.v());
      R.Return();
    });
    // q past 64 bits: the best lower approximation L of x = B/q with a
    // denominator of at most N = 2^64 - 1, by the Stern-Brocot walk with
    // jumps (Euclid on Rv = B ld - ln q and Sv = un q - B ud, from L = 0/1 and
    // U = 1/1): no fraction with such a denominator lies between L and x, so
    // floor(y L) = floor(y x) for every integer y <= N.
    Val Rv = Rb, Sv = buf(R, Scratch), Tmp = buf(R, 2);
    R.call(Copy, {Sv, Qp.v(), Qn.v()});
    Var NR = R.var(U64, Nr.v());
    Var NS = R.var(U64, R.call(A.Sub, {Sv, Qn.v(), Rv, Nr.v()}));
    Var Ln = R.var(U64, R.u(0)), Ld = R.var(U64, R.u(1)),
        Un = R.var(U64, R.u(1)), Ud = R.var(U64, R.u(1));
    const uint64_t Top = ~uint64_t(0);
    R.While([&] { return R.boolean(true); },
            [&] {
              Val CapL = R.snap(R.snap(R.u(Top) - Ld.v()) / Ud.v());
              Val T1 = R.call(A.DivQ, {Rv, NR.v(), Sv, NS.v(), Tmp});
              Val Over1 = R.snap(ld(R, Tmp, R.u(1)) != 0);
              R.If(R.lor(Over1, [&] { return R.snap(T1 > CapL); }), [&] {
                Ln = R.snap(Ln.v() + R.snap(CapL * Un.v()));
                Ld = R.snap(Ld.v() + R.snap(CapL * Ud.v()));
                R.Break();
              });
              NR = ld(R, Tmp, R.u(0));
              Ln = R.snap(Ln.v() + R.snap(T1 * Un.v()));
              Ld = R.snap(Ld.v() + R.snap(T1 * Ud.v()));
              R.If(R.snap(NR.v() == 0), [&] { R.Break(); });
              Val CapU = R.snap(R.snap(R.u(Top) - Ud.v()) / Ld.v());
              Val T2 = R.call(A.DivQ, {Sv, NS.v(), Rv, NR.v(), Tmp});
              Val Over2 = R.snap(ld(R, Tmp, R.u(1)) != 0);
              R.If(R.lor(Over2, [&] { return R.snap(T2 > CapU); }),
                   [&] { R.Break(); });
              NS = ld(R, Tmp, R.u(0));
              Un = R.snap(Un.v() + R.snap(T2 * Ln.v()));
              Ud = R.snap(Ud.v() + R.snap(T2 * Ld.v()));
              R.If(R.snap(NS.v() == 0), [&] { R.Break(); });
            });
    Put4(Aq.v(), Ln.v(), Ld.v(), Inv.v());
    R.finish();
  }
  // unit_intfactor(out, f, mode): one limb each way in 64-bit words, A = p /
  // q, n = p % q, d = q; else unit_intfactor_big.
  A.IntFactor = fn("unit_intfactor", {PU8, PU8, U64}, Void);
  {
    Rt R(M, M.func(A.IntFactor));
    Val Out = R.param(0), P = R.param(1), Mode = R.param(2);
    Val Small = R.land(R.snap(ld(R, P, R.u(0)) == 1),
                       [&] { return R.snap(ld(R, P, R.u(1)) == 1); });
    R.If(R.not_(Small), [&] {
      R.call(IntFactorBig, {Out, P, Mode});
      R.Return();
    });
    Var Pv = R.var(U64, ld(R, P, R.u(2))), Qv = R.var(U64, ld(R, P, R.u(3)));
    Var Inv = R.var(U64, R.u(0));
    R.If(R.lor(R.snap(Mode == 1),
               [&] {
                 return R.land(R.snap(Mode == 2),
                               [&] { return R.snap(Pv.v() < Qv.v()); });
               }),
         [&] {
           Val T = R.snap(Pv.v());
           Pv = Qv.v();
           Qv = T;
           Inv = R.u(1);
         });
    st(R, Out, R.u(0), R.snap(Pv.v() / Qv.v()));
    st(R, Out, R.u(1), R.snap(Pv.v() % Qv.v()));
    st(R, Out, R.u(2), Qv.v());
    st(R, Out, R.u(3), Inv.v());
    R.finish();
  }
}

//===----------------------------------------------------------------------===//
// Descriptors
//===----------------------------------------------------------------------===//

void Emitter::descriptors() {
  const int64_t MaxExp = 1 << 20;
  const uint64_t Out = UnitCountOut;
  auto Cnt = [&](Rt &R, Val P) { return R.load(P, U64); };
  auto PairId = [&](Rt &R, Val P, Val I) {
    return R.load(R.padd(P, R.snap(R.u(8) + I * 16)), U64);
  };
  auto PairExp = [&](Rt &R, Val P, Val I) {
    return R.load(R.padd(P, R.snap(R.u(16) + I * 16)), I64);
  };
  // The words of a fraction in place: {0, 0} marked, else 2 + nn + nd.
  auto FracWords = [&](Rt &R, Val F) {
    Var W = R.var(U64, R.u(2));
    Val Nn = ld(R, F, R.u(0));
    R.If(R.land(R.snap(Nn != 0), [&] { return R.snap(Nn != Out); }),
         [&] { W = R.snap(R.snap(Nn + ld(R, F, R.u(1))) + 2); });
    return R.snap(W.v());
  };

  // unit_frac(f): the fraction itself where f is one spilled to the heap
  // ({~0, pointer}), else f.
  A.Frac = fn("unit_frac", {PU8}, PU8);
  {
    Rt R(M, M.func(A.Frac));
    Val F = R.param(0);
    R.If(R.snap(ld(R, F, R.u(0)) == Out),
         [&] { R.Return(R.load(R.padd(F, R.u(8)), PU8)); });
    R.Return(F);
    R.finish();
  }

  // unit_expof(d, id): the exponent of the base unit id in the descriptor d.
  FuncId ExpOf = fn("unit_expof", {PU8, U64}, I64);
  {
    Rt R(M, M.func(ExpOf));
    Val D = R.param(0), Id = R.param(1);
    Val N = Cnt(R, D);
    Var I = R.var(U64, R.u(0));
    R.While([&] { return R.snap(I.v() < N); },
            [&] {
              R.If(R.snap(PairId(R, D, I.v()) == Id),
                   [&] { R.Return(PairExp(R, D, I.v())); });
              I = R.snap(I.v() + 1);
            });
    R.Return(R.i(0));
    R.finish();
  }

  // A unit recipe (gopp/GIR/UnitRuntime.h): {np, (dict index, exp) x np,
  // the base descriptor}. The descriptor of parameter K of a recipe.
  auto Param = [&](Rt &R, Val U, Val Dicts, Val K) {
    Val Idx = ld(R, U, R.snap(R.snap(K * 2) + 1));
    return R.load(R.padd(Dicts, R.snap(Idx * 8)), PU8);
  };
  auto PExp = [&](Rt &R, Val U, Val K) {
    return R.to(I64, ld(R, U, R.snap(R.snap(K * 2) + 2)));
  };
  auto Base = [&](Rt &R, Val U) {
    return R.padd(U, R.snap(R.snap(R.snap(ld(R, U, R.u(0)) * 2) + 1) * 8));
  };

  // unit_total(u, dicts, id): the exponent of the base unit id in the unit
  // of the recipe u.
  FuncId Total = fn("unit_total", {PU8, PU8, U64}, I64);
  {
    Rt R(M, M.func(Total));
    Val U = R.param(0), Dicts = R.param(1), Id = R.param(2);
    Val Np = ld(R, U, R.u(0));
    Var T = R.var(I64, R.call(ExpOf, {Base(R, U), Id}));
    Var K = R.var(U64, R.u(0));
    R.While([&] { return R.snap(K.v() < Np); },
            [&] {
              Val E = R.call(ExpOf, {Param(R, U, Dicts, K.v()), Id});
              T = R.snap(T.v() + R.snap(PExp(R, U, K.v()) * E));
              K = R.snap(K.v() + 1);
            });
    R.Return(T.v());
    R.finish();
  }

  // unit_check(u, dicts) u64: 2 if the unit of the recipe is out of range (a
  // part marked, or an exponent past +-2^20), 0 if it has a base unit, 1 if
  // it has none.
  FuncId Check = fn("unit_check", {PU8, PU8}, U64);
  {
    Rt R(M, M.func(Check));
    Val U = R.param(0), Dicts = R.param(1);
    Val Np = ld(R, U, R.u(0));
    Var Dim = R.var(R.Bool, R.boolean(false));
    // Every part first: a part out of range has no pairs to read.
    R.If(R.snap(Cnt(R, Base(R, U)) == Out), [&] { R.Return(R.u(2)); });
    {
      Var K = R.var(U64, R.u(0));
      R.While([&] { return R.snap(K.v() < Np); },
              [&] {
                R.If(R.snap(Cnt(R, Param(R, U, Dicts, K.v())) == Out),
                     [&] { R.Return(R.u(2)); });
                K = R.snap(K.v() + 1);
              });
    }
    // The ids of one descriptor, each checked against the whole unit.
    auto Scan = [&](Val D) {
      Val N = Cnt(R, D);
      Var I = R.var(U64, R.u(0));
      R.While([&] { return R.snap(I.v() < N); },
              [&] {
                Val T = R.call(Total, {U, Dicts, PairId(R, D, I.v())});
                R.If(R.lor(R.snap(T > R.i(MaxExp)),
                           [&] { return R.snap(T < R.i(-MaxExp)); }),
                     [&] { R.Return(R.u(2)); });
                R.If(R.snap(T != R.i(0)), [&] { Dim = R.boolean(true); });
                I = R.snap(I.v() + 1);
              });
    };
    Scan(Base(R, U));
    Var K = R.var(U64, R.u(0));
    R.While([&] { return R.snap(K.v() < Np); },
            [&] {
              Scan(Param(R, U, Dicts, K.v()));
              K = R.snap(K.v() + 1);
            });
    R.If(Dim.v(), [&] { R.Return(R.u(0)); });
    R.Return(R.u(1));
    R.finish();
  }

  // unit_factor(dst, u, dicts): the factor of the unit of the recipe u, the
  // base's times each parameter's to its exponent (a fraction at most at the
  // bound; marked past it).
  FuncId Factor = fn("unit_factor", {PU8, PU8, PU8}, Void);
  {
    Rt R(M, M.func(Factor));
    Val D = R.param(0), U = R.param(1), Dicts = R.param(2);
    Val One = buf(R, 4);
    for (unsigned W = 0; W < 4; ++W)
      st(R, One, R.u(W), R.u(1));
    Val B = Base(R, U);
    Val Bf = R.padd(B, R.snap(R.u(8) + R.snap(Cnt(R, B) * 16)));
    R.call(A.FracMul, {D, Bf, R.u(0), One, R.u(0)});
    Val Np = ld(R, U, R.u(0));
    Val Pw = buf(R, UnitFracWords);
    Var K = R.var(U64, R.u(0));
    R.While(
        [&] { return R.snap(K.v() < Np); },
        [&] {
          Val P = Param(R, U, Dicts, K.v());
          Val Pf = R.call(A.Frac,
                          {R.padd(P, R.snap(R.u(8) + R.snap(Cnt(R, P) * 16)))});
          Val E = PExp(R, U, K.v());
          R.IfElse(
              R.lor(R.snap(E == R.i(1)), [&] { return R.snap(E == R.i(-1)); }),
              [&] {
                Var Inv = R.var(U64, R.u(0));
                R.If(R.snap(E < R.i(0)), [&] { Inv = R.u(1); });
                R.call(A.FracMul, {D, D, R.u(0), Pf, Inv.v()});
              },
              [&] {
                R.call(A.FracPow, {Pw, Pf, E});
                R.call(A.FracMul, {D, D, R.u(0), Pw, R.u(0)});
              });
          K = R.snap(K.v() + 1);
        });
    R.finish();
  }

  // unit_make(dst, cap, small, u, dicts, spill): the descriptor of the unit
  // of the recipe u, in a frame slot with room for cap pairs and a fraction
  // of small limbs; a wider fraction goes to a heap block linked into the
  // chain at *spill (the function's owned spill, freed at its exits).
  A.Make = fn("unit_make", {PU8, U64, U64, PU8, PU8, PU8}, Void);
  {
    Rt R(M, M.func(A.Make));
    Val Dst = R.param(0), Cap = R.param(1), Small = R.param(2), U = R.param(3),
        Dicts = R.param(4), Spill = R.param(5);
    auto MarkOut = [&] {
      R.store(Dst, U64, R.u(Out));
      R.Return();
    };
    Val Np = ld(R, U, R.u(0));
    R.If(R.snap(Cnt(R, Base(R, U)) == Out), MarkOut);
    {
      Var K = R.var(U64, R.u(0));
      R.While([&] { return R.snap(K.v() < Np); },
              [&] {
                R.If(R.snap(Cnt(R, Param(R, U, Dicts, K.v())) == Out), MarkOut);
                K = R.snap(K.v() + 1);
              });
    }
    // The pairs in id order: the least id above the last one, from every
    // part, each time (the parts are short).
    Var Last = R.var(U64, R.u(0)), Have = R.var(R.Bool, R.boolean(false));
    Var N = R.var(U64, R.u(0));
    R.While([&] { return R.boolean(true); },
            [&] {
              Var Min = R.var(U64, R.u(~uint64_t(0)));
              Var Found = R.var(R.Bool, R.boolean(false));
              auto Look = [&](Val D) {
                Val C0 = Cnt(R, D);
                Var I = R.var(U64, R.u(0));
                R.While(
                    [&] { return R.snap(I.v() < C0); },
                    [&] {
                      Val Id = PairId(R, D, I.v());
                      Val Above = R.lor(R.not_(Have.v()),
                                        [&] { return R.snap(Id > Last.v()); });
                      R.If(R.land(Above, [&] { return R.snap(Id <= Min.v()); }),
                           [&] {
                             Min = Id;
                             Found = R.boolean(true);
                           });
                      I = R.snap(I.v() + 1);
                    });
              };
              Look(Base(R, U));
              Var K = R.var(U64, R.u(0));
              R.While([&] { return R.snap(K.v() < Np); },
                      [&] {
                        Look(Param(R, U, Dicts, K.v()));
                        K = R.snap(K.v() + 1);
                      });
              R.If(R.not_(Found.v()), [&] { R.Break(); });
              Last = Min.v();
              Have = R.boolean(true);
              Val T = R.call(Total, {U, Dicts, Min.v()});
              R.If(R.lor(R.snap(T > R.i(MaxExp)),
                         [&] { return R.snap(T < R.i(-MaxExp)); }),
                   MarkOut);
              R.If(R.snap(T != R.i(0)), [&] {
                R.If(R.snap(N.v() >= Cap), MarkOut);
                R.store(R.padd(Dst, R.snap(R.u(8) + N.v() * 16)), U64, Min.v());
                R.store(R.padd(Dst, R.snap(R.u(16) + N.v() * 16)), I64, T);
                N = R.snap(N.v() + 1);
              });
            });
    R.store(Dst, U64, N.v());
    Val Fd = R.padd(Dst, R.snap(R.u(8) + N.v() * 16));
    Val S = buf(R, UnitFracWords);
    R.call(Factor, {S, U, Dicts});
    Val W = FracWords(R, S);
    R.IfElse(
        R.lor(R.snap(ld(R, S, R.u(0)) == 0),
              [&] { return R.snap(R.snap(W - 2) <= Small); }),
        [&] { R.call(Copy, {Fd, S, W}); },
        [&] {
          Val Blk = R.call(Alloc, {R.null(PU8), R.snap(R.snap(W + 1) * 8),
                                   R.boolean(false)});
          R.store(Blk, PU8, R.load(Spill, PU8));
          R.store(Spill, PU8, Blk);
          Val F = R.padd(Blk, R.u(8));
          R.call(Copy, {F, S, W});
          st(R, Fd, R.u(0), R.u(Out));
          R.store(R.padd(Fd, R.u(8)), PU8, F);
        });
    R.finish();
  }

  // unit_spill_free(spill): the drop glue of the spill chain (a drop glue is
  // fn(*u8): spill points to the head pointer).
  A.SpillFree = fn("unit_spill_free", {PU8}, Void);
  {
    Rt R(M, M.func(A.SpillFree));
    Val Pp = R.param(0);
    Var P = R.var(PU8, R.load(Pp, PU8));
    R.While([&] { return R.snap(R.to(U64, P.v()) != 0); },
            [&] {
              Val Next = R.load(P.v(), PU8);
              R.call(Free, {P.v()});
              P = Next;
            });
    R.store(Pp, PU8, R.null(PU8));
    R.finish();
  }

  // unit_sites(dst, g, dicts): the factor of a group of collapse sites (a
  // static factor, then each site: kind, and the recipes of res, x and y):
  // the static factor times, for each site, Mul c(res) / (c(x) c(y)), Quo
  // c(res) c(y) / c(x), Conv c(res) / c(x), Const c(res), where c(D) is the
  // factor of D if it has no base unit, 1 if it has, marked if D is out of
  // range.
  FuncId Sites = fn("unit_sites", {PU8, PU8, PU8}, Void);
  {
    Rt R(M, M.func(Sites));
    Val D = R.param(0), G = R.param(1), Dicts = R.param(2);
    Val One = buf(R, 4), Marked = buf(R, 1);
    for (unsigned W = 0; W < 4; ++W)
      st(R, One, R.u(W), R.u(1));
    st(R, Marked, R.u(0), R.u(0));
    R.call(A.FracMul, {D, G, R.u(0), One, R.u(0)});
    Val Sw = FracWords(R, G);
    Val Ns = ld(R, G, Sw);
    Var P = R.var(PU8, R.padd(G, R.snap(R.snap(Sw + 1) * 8)));
    Val Tr = buf(R, UnitFracWords), Tx = buf(R, UnitFracWords),
        Ty = buf(R, UnitFracWords);
    // The size of a recipe: 1 + 2 np, and its base descriptor.
    auto Size = [&](Val U) {
      Val B = Base(R, U);
      Val Cn = Cnt(R, B);
      Var S = R.var(U64, R.u(1));
      R.If(R.snap(Cn != Out), [&] {
        Val Bf = R.padd(B, R.snap(R.u(8) + R.snap(Cn * 16)));
        S = R.snap(R.snap(R.u(1) + R.snap(Cn * 2)) + FracWords(R, Bf));
      });
      return R.snap(R.snap(R.snap(ld(R, U, R.u(0)) * 2) + 1) + S.v());
    };
    // c(D) of the recipe at P, the next recipe after it.
    auto Cof = [&](Val T) {
      Val U = R.snap(P.v());
      Val St = R.call(Check, {U, Dicts});
      Var F = R.var(PU8, One);
      R.If(R.snap(St == 2), [&] { F = Marked; });
      R.If(R.snap(St == 1), [&] {
        R.call(Factor, {T, U, Dicts});
        F = T;
      });
      P = R.padd(U, R.snap(Size(U) * 8));
      return R.snap(F.v());
    };
    Var K = R.var(U64, R.u(0));
    R.While([&] { return R.snap(K.v() < Ns); },
            [&] {
              Val Kind = ld(R, P.v(), R.u(0));
              P = R.padd(P.v(), R.u(8));
              Val Cr = Cof(Tr), Cx = Cof(Tx), Cy = Cof(Ty);
              auto Mul = [&](Val F, uint64_t Inv) {
                R.call(A.FracMul, {D, D, R.u(0), F, R.u(Inv)});
              };
              Mul(Cr, 0);
              R.If(R.snap(Kind == 0), [&] {
                Mul(Cx, 1);
                Mul(Cy, 1);
              });
              R.If(R.snap(Kind == 1), [&] {
                Mul(Cy, 0);
                Mul(Cx, 1);
              });
              R.If(R.snap(Kind == 2), [&] { Mul(Cx, 1); });
              K = R.snap(K.v() + 1);
            });
    R.finish();
  }

  // unit_site_f(g, dicts, p) f64: the factor of the group rounded once to p
  // bits; 0 for a factor out of range, also one that is no number of the
  // float type (+inf or 0 rounded: plain code's E0435, unitFactorFits).
  A.SiteF = fn("unit_site_f", {PU8, PU8, U64}, F64);
  {
    Rt R(M, M.func(A.SiteF));
    Val S = buf(R, UnitFracWords);
    R.call(Sites, {S, R.param(0), R.param(1)});
    Val V = R.call(A.Ratio, {S, R.param(2)});
    R.If(R.bin(BinOp::Eq, V, fl(R, std::numeric_limits<double>::infinity())),
         [&] { R.Return(fl(R, 0.0)); });
    R.Return(V);
    R.finish();
  }

  // unit_site_i(out, g, dicts, mode): the integer words of the group's
  // factor (unit_intfactor).
  A.SiteI = fn("unit_site_i", {PU8, PU8, PU8, U64}, Void);
  {
    Rt R(M, M.func(A.SiteI));
    Val S = buf(R, UnitFracWords);
    R.call(Sites, {S, R.param(1), R.param(2)});
    R.call(A.IntFactor, {R.param(0), S, R.param(3)});
    R.finish();
  }
}

} // namespace

UnitArith
gir::addUnitArith(Module &M,
                  llvm::function_ref<std::string(llvm::StringRef)> Name,
                  Linkage Link, FuncId Alloc, FuncId Free) {
  Emitter E(M, Name, Link);
  E.Alloc = Alloc;
  E.Free = Free;
  E.small();
  E.numbers();
  E.fractions();
  E.descriptors();
  return E.A;
}
