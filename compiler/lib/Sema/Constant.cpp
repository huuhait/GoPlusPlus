//===- Constant.cpp - Untyped constant arithmetic -------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Constant.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

using namespace gopp;
using namespace gopp::sema;
using llvm::APInt;

namespace {

APInt trim(const APInt &A) {
  unsigned W = std::max(1u, A.getSignificantBits());
  return W == A.getBitWidth() ? A : A.trunc(W);
}
APInt sext(const APInt &A, unsigned W) {
  return W > A.getBitWidth() ? A.sext(W) : A;
}
void unify(APInt &A, APInt &B) {
  unsigned W = std::max(A.getBitWidth(), B.getBitWidth()) + 1;
  A = sext(A, W);
  B = sext(B, W);
}
APInt pow10(unsigned N) {
  // 10^N by squaring; the width is large enough for the result.
  unsigned W = static_cast<unsigned>(N * 3.33) + 8;
  APInt Result(W, 1), Base(W, 10);
  while (N) {
    if (N & 1)
      Result *= Base;
    N >>= 1;
    if (N)
      Base *= Base;
  }
  return Result;
}

bool isDigitIn(char C, unsigned Radix) {
  unsigned D;
  if (C >= '0' && C <= '9')
    D = C - '0';
  else if (C >= 'a' && C <= 'f')
    D = C - 'a' + 10;
  else if (C >= 'A' && C <= 'F')
    D = C - 'A' + 10;
  else
    return false;
  return D < Radix;
}

APInt parseUnsigned(llvm::StringRef Digits, unsigned Radix) {
  if (Digits.empty())
    return APInt(1, 0);
  unsigned BitsPerDigit = Radix == 16 ? 4 : Radix == 8 ? 3 : Radix == 2 ? 1 : 4;
  unsigned W = static_cast<unsigned>(Digits.size()) * BitsPerDigit + 2;
  return APInt(W, Digits, static_cast<uint8_t>(Radix));
}

} // namespace

//===----------------------------------------------------------------------===//
// BigFloat
//===----------------------------------------------------------------------===//

BigFloat BigFloat::round(bool Neg, const APInt &M, int64_t E, bool Sticky) {
  BigFloat R;
  unsigned N = M.getActiveBits();
  if (N == 0)
    return R;
  R.Neg = Neg;
  if (N > Prec) {
    unsigned Sh = N - Prec;
    bool Half = M[Sh - 1];
    bool Rest = Sticky || (Sh > 1 && M.countr_zero() < Sh - 1);
    APInt Q = M.lshr(Sh).zextOrTrunc(Prec);
    if (Half && (Rest || Q[0])) {
      ++Q;
      if (Q.isZero()) { // carried out of the top: 1000...0
        Q = APInt::getOneBitSet(Prec, Prec - 1);
        ++Sh;
      }
    }
    R.Mant = Q;
    R.Exp = E + Sh;
  } else {
    unsigned Sh = Prec - N;
    R.Mant = M.zextOrTrunc(Prec).shl(Sh);
    R.Exp = E - Sh;
  }
  return R;
}

BigFloat BigFloat::fromInt(const APInt &S) {
  bool Neg = S.isNegative();
  APInt A = S.sext(S.getBitWidth() + 1);
  if (Neg)
    A = -A;
  return round(Neg, A, 0);
}

BigFloat BigFloat::fromDouble(double D) {
  if (D == 0 || !std::isfinite(D))
    return BigFloat();
  bool Neg = D < 0;
  int E;
  double M = std::frexp(std::fabs(D), &E);
  uint64_t Mant = static_cast<uint64_t>(std::ldexp(M, 53));
  return round(Neg, APInt(64, Mant), static_cast<int64_t>(E) - 53);
}

double BigFloat::toDouble() const {
  if (isZero())
    return 0;
  APInt T = Mant.lshr(Prec - 64).trunc(64);
  uint64_t U = T.getZExtValue();
  if (Mant.countr_zero() < Prec - 64)
    U |= 1;
  int64_t E = Exp + Prec - 64;
  E = std::max<int64_t>(-100000, std::min<int64_t>(100000, E));
  double D = std::ldexp(static_cast<double>(U), static_cast<int>(E));
  return Neg ? -D : D;
}

bool BigFloat::isInt() const {
  if (isZero() || Exp >= 0)
    return true;
  if (-Exp >= static_cast<int64_t>(Prec))
    return false;
  return Mant.countr_zero() >= static_cast<unsigned>(-Exp);
}

APInt BigFloat::truncToInt() const {
  if (isZero())
    return APInt(1, 0);
  APInt M(1, 0);
  if (Exp >= 0) {
    if (Exp > (1 << 22))
      return APInt(1, 0); // callers check overflows() first
    M = Mant.zext(Prec + static_cast<unsigned>(Exp) + 1).shl(Exp);
  } else if (-Exp >= static_cast<int64_t>(Prec)) {
    return APInt(1, 0);
  } else {
    M = Mant.lshr(static_cast<unsigned>(-Exp)).zext(Prec + 1);
  }
  return Neg ? -M : M;
}

bool BigFloat::toInt(APInt &Out) const {
  // An integer of more than 2^22 bits is far beyond every integer type, and
  // building it would take megabytes: it does not convert (truncated, as in
  // go/constant's ToInt of a huge value).
  if (!isInt() || Exp > (1 << 22))
    return false;
  Out = truncToInt();
  return true;
}

BigFloat BigFloat::neg() const {
  BigFloat R = *this;
  if (!R.isZero())
    R.Neg = !R.Neg;
  return R;
}
BigFloat BigFloat::abs() const {
  BigFloat R = *this;
  R.Neg = false;
  return R;
}

BigFloat BigFloat::add(const BigFloat &A, const BigFloat &B) {
  if (A.isZero())
    return B;
  if (B.isZero())
    return A;
  if (A.magnitudeExp() - B.magnitudeExp() > static_cast<int64_t>(Prec) + 2)
    return A;
  if (B.magnitudeExp() - A.magnitudeExp() > static_cast<int64_t>(Prec) + 2)
    return B;
  int64_t E = std::min(A.Exp, B.Exp);
  unsigned W = Prec + static_cast<unsigned>(std::max(A.Exp, B.Exp) - E) + 3;
  APInt MA = A.Mant.zext(W).shl(static_cast<unsigned>(A.Exp - E));
  APInt MB = B.Mant.zext(W).shl(static_cast<unsigned>(B.Exp - E));
  if (A.Neg)
    MA = -MA;
  if (B.Neg)
    MB = -MB;
  APInt S = MA + MB;
  bool Neg = S.isNegative();
  if (Neg)
    S = -S;
  return round(Neg, S, E);
}
BigFloat BigFloat::sub(const BigFloat &A, const BigFloat &B) {
  return add(A, B.neg());
}
BigFloat BigFloat::mul(const BigFloat &A, const BigFloat &B) {
  if (A.isZero() || B.isZero())
    return BigFloat();
  APInt M = A.Mant.zext(2 * Prec) * B.Mant.zext(2 * Prec);
  return round(A.Neg != B.Neg, M, A.Exp + B.Exp);
}
BigFloat BigFloat::quo(const BigFloat &A, const BigFloat &B) {
  if (A.isZero())
    return BigFloat();
  unsigned W = 2 * Prec + 4;
  APInt N = A.Mant.zext(W).shl(Prec + 2);
  APInt D = B.Mant.zext(W);
  APInt Q, R;
  APInt::udivrem(N, D, Q, R);
  return round(A.Neg != B.Neg, Q, A.Exp - B.Exp - (Prec + 2), !R.isZero());
}
int BigFloat::cmp(const BigFloat &A, const BigFloat &B) {
  int SA = A.sign(), SB = B.sign();
  if (SA != SB)
    return SA < SB ? -1 : 1;
  if (SA == 0)
    return 0;
  int C;
  if (A.magnitudeExp() != B.magnitudeExp())
    C = A.magnitudeExp() < B.magnitudeExp() ? -1 : 1;
  else
    C = A.Mant.ult(B.Mant) ? -1 : (A.Mant == B.Mant ? 0 : 1);
  return SA < 0 ? -C : C;
}

BigFloat BigFloat::ratio(const APInt &Num, const APInt &Den) {
  // Num/Den (both positive) rounded to Prec bits.
  unsigned Need = Prec + 4 + Den.getActiveBits();
  unsigned Sh = Need > Num.getActiveBits() ? Need - Num.getActiveBits() : 0;
  unsigned W = std::max(Num.getBitWidth(), Den.getBitWidth()) + Sh + 2;
  APInt N = Num.zext(W).shl(Sh);
  APInt D = Den.zext(W);
  APInt Q, R;
  APInt::udivrem(N, D, Q, R);
  return round(false, Q, -static_cast<int64_t>(Sh), !R.isZero());
}

/// Splits a Go float literal into D * 10^E (decimal) or D * 2^E (Hex).
static bool scanFloatLit(llvm::StringRef Text, APInt &D, bool &Hex,
                         int64_t &E) {
  std::string T;
  for (char C : Text)
    if (C != '_')
      T.push_back(C);
  llvm::StringRef S = T;
  Hex = S.starts_with("0x") || S.starts_with("0X");
  if (Hex)
    S = S.drop_front(2);
  std::string Digits;
  int64_t FracDigits = 0;
  bool SeenDot = false;
  size_t I = 0;
  unsigned Radix = Hex ? 16 : 10;
  for (; I < S.size(); ++I) {
    char C = S[I];
    if (C == '.') {
      if (SeenDot)
        return false;
      SeenDot = true;
    } else if (isDigitIn(C, Radix)) {
      Digits.push_back(C);
      if (SeenDot)
        ++FracDigits;
    } else {
      break;
    }
  }
  if (Digits.empty())
    return false;
  int64_t Exp = 0;
  if (I < S.size()) {
    char C = S[I];
    bool IsExp = Hex ? (C == 'p' || C == 'P') : (C == 'e' || C == 'E');
    if (!IsExp)
      return false;
    ++I;
    bool NegE = false;
    if (I < S.size() && (S[I] == '+' || S[I] == '-')) {
      NegE = S[I] == '-';
      ++I;
    }
    if (I >= S.size())
      return false;
    for (; I < S.size(); ++I) {
      if (S[I] < '0' || S[I] > '9')
        return false;
      if (Exp < (1LL << 40))
        Exp = Exp * 10 + (S[I] - '0');
    }
    if (NegE)
      Exp = -Exp;
  }
  D = parseUnsigned(Digits, Radix);
  E = Hex ? Exp - 4 * FracDigits : Exp - FracDigits;
  return true;
}

/// 10^N as a 512-bit BigFloat by squaring (the exact integer power of a large
/// N would take gigabytes).
static BigFloat bigPow10(uint64_t N) {
  BigFloat P = BigFloat::fromInt(APInt(64, 1)),
           B = BigFloat::fromInt(APInt(64, 10));
  for (; N; N >>= 1) {
    if (N & 1)
      P = BigFloat::mul(P, B);
    B = BigFloat::mul(B, B);
  }
  return P;
}

bool BigFloat::parse(llvm::StringRef Text, BigFloat &Out) {
  APInt D;
  bool Hex;
  int64_t E;
  if (!scanFloatLit(Text, D, Hex, E))
    return false;
  if (D.isZero()) {
    Out = BigFloat();
    return true;
  }
  if (Hex) {
    Out = round(false, D, E);
    return true;
  }
  int64_t E10 = E;
  // Beyond 10^700000000 the value is far outside the exponent range.
  constexpr int64_t FarExp = 700000000;
  if (E10 > FarExp) {
    Out = round(false, APInt(2, 1), BigFloat::MaxExp * 4);
    return true;
  }
  if (E10 < -FarExp) {
    Out = BigFloat();
    return true;
  }
  if (E10 > 400000 || E10 < -400000) {
    // 10^|E10| in 512 bits (the integer power would be huge).
    BigFloat P = bigPow10(static_cast<uint64_t>(E10 < 0 ? -E10 : E10));
    BigFloat M = round(false, D, 0);
    Out = E10 < 0 ? quo(M, P) : mul(M, P);
    return true;
  }
  if (E10 >= 0) {
    APInt P = pow10(static_cast<unsigned>(E10));
    unsigned W = D.getBitWidth() + P.getBitWidth();
    Out = round(false, D.zext(W) * P.zext(W), 0);
  } else {
    Out = ratio(D, pow10(static_cast<unsigned>(-E10)));
  }
  return true;
}

std::string BigFloat::str() const {
  if (isZero())
    return "0";
  char Buf[64];
  int64_t Mag = magnitudeExp();
  if (Mag > -1000 && Mag < 1000) {
    std::snprintf(Buf, sizeof Buf, "%.6g", toDouble());
    // go/constant: a non-integer whose %.6g form shows no '.' uses more
    // digits (go.dev/issue/56220).
    if (!isInt() && !std::strchr(Buf, '.')) {
      // Go's %g: the shortest digits that read back as the same double, in
      // exponent form when the exponent is < -4 or >= 6.
      double D = toDouble();
      int Digits = 1;
      for (; Digits < 17; ++Digits) {
        std::snprintf(Buf, sizeof Buf, "%.*e", Digits - 1, D);
        if (std::strtod(Buf, nullptr) == D)
          break;
      }
      std::snprintf(Buf, sizeof Buf, "%.*e", Digits - 1, D);
      int X = std::atoi(std::strchr(Buf, 'e') + 1);
      if (X >= -4 && X < 6)
        std::snprintf(Buf, sizeof Buf, "%.*f", std::max(0, Digits - 1 - X), D);
    }
    return Buf;
  }
  // Outside the range of double: scale by a power of ten.
  int64_t E10 = static_cast<int64_t>(std::floor(Mag * 0.30102999566398120));
  BigFloat P = bigPow10(static_cast<uint64_t>(std::abs(E10)));
  BigFloat Scaled = E10 < 0 ? mul(abs(), P) : quo(abs(), P);
  double M = Scaled.toDouble();
  while (M >= 10) {
    M /= 10;
    ++E10;
  }
  while (M < 1 && M > 0) {
    M *= 10;
    --E10;
  }
  std::snprintf(Buf, sizeof Buf, "%s%.6ge%+lld", Neg ? "-" : "", M,
                static_cast<long long>(E10));
  return Buf;
}

double BigFloat::toIEEE(unsigned P) const {
  if (isZero())
    return 0;
  // The normal exponent range of IEEE single and double.
  int64_t EMin = P == 24 ? -126 : -1022;
  int64_t EMax = P == 24 ? 127 : 1023;
  int64_t E = magnitudeExp(); // the value is in [2^E, 2^(E+1))
  double Inf = std::numeric_limits<double>::infinity();
  if (E > EMax)
    return Neg ? -Inf : Inf;
  // Bits kept: P for a normal result, fewer for a subnormal one.
  int64_t Keep = E >= EMin ? P : P - (EMin - E);
  if (Keep < 0)
    return 0; // below half the smallest subnormal
  unsigned Drop = Prec - static_cast<unsigned>(Keep);
  uint64_t Q = Keep == 0 ? 0 : Mant.lshr(Drop).getZExtValue();
  bool Half = Mant[Drop - 1];
  bool Rest = Drop > 1 && Mant.countr_zero() < Drop - 1;
  if (Half && (Rest || (Q & 1)))
    ++Q; // may carry into the next binade: still exact below
  double D = std::ldexp(static_cast<double>(Q), static_cast<int>(Exp + Drop));
  if (P == 24 && std::fabs(D) > std::numeric_limits<float>::max())
    D = Inf;
  return Neg ? -D : D;
}

//===----------------------------------------------------------------------===//
// Real
//===----------------------------------------------------------------------===//

namespace {
/// A * B for signed values of any widths.
APInt mulS(const APInt &A, const APInt &B) {
  unsigned W = A.getBitWidth() + B.getBitWidth();
  return A.sext(W) * B.sext(W);
}
/// The absolute value of a signed A, one bit wider.
APInt absS(const APInt &A) {
  APInt R = A.sext(A.getBitWidth() + 1);
  return R.isNegative() ? -R : R;
}
} // namespace

Real Real::big(const BigFloat &F) {
  Real R;
  R.Rat = false;
  R.F = F;
  return R;
}

Real Real::ratio(APInt N, APInt D) {
  if (N.isZero())
    return Real();
  unsigned W = std::max(N.getBitWidth(), D.getBitWidth()) + 1;
  bool Neg = N.isNegative();
  APInt AN = absS(N).zextOrTrunc(W), AD = D.sext(W);
  APInt G = llvm::APIntOps::GreatestCommonDivisor(AN, AD);
  if (!G.isOne()) {
    AN = AN.udiv(G);
    AD = AD.udiv(G);
  }
  if (AN.getActiveBits() > MaxRatBits || AD.getActiveBits() > MaxRatBits) {
    BigFloat F = BigFloat::ratio(AN, AD);
    return big(Neg ? F.neg() : F);
  }
  Real R;
  R.Num = trim(Neg ? -AN : AN);
  R.Den = trim(AD);
  return R;
}

Real Real::fromInt(const APInt &S) { return ratio(S, APInt(2, 1)); }

Real Real::fromFloat(const BigFloat &F) {
  if (F.isZero())
    return Real();
  unsigned TZ = F.Mant.countr_zero();
  APInt M = F.Mant.lshr(TZ);
  unsigned MB = M.getActiveBits();
  M = M.zextOrTrunc(MB + 1);
  int64_t E = F.Exp + TZ;
  if (E >= 0) {
    if (E + MB > MaxRatBits)
      return big(F);
    APInt N = M.zext(MB + 1 + static_cast<unsigned>(E)).shl(E);
    return ratio(F.Neg ? -N : N, APInt(2, 1));
  }
  if (-E > static_cast<int64_t>(MaxRatBits))
    return big(F);
  unsigned E2 = static_cast<unsigned>(-E);
  return ratio(F.Neg ? -M : M, APInt::getOneBitSet(E2 + 2, E2));
}

bool Real::parse(llvm::StringRef Text, Real &Out) {
  APInt D;
  bool Hex;
  int64_t E;
  if (!scanFloatLit(Text, D, Hex, E))
    return false;
  D = D.zext(D.getBitWidth() + 1); // signed, non-negative
  if (D.isZero()) {
    Out = Real();
    return true;
  }
  // Exact while the power stays within the rational limit (10^1200 has 3987
  // bits); larger exponents are rounded to a BigFloat, as go/constant does.
  if (Hex && E >= -static_cast<int64_t>(MaxRatBits) &&
      E <= static_cast<int64_t>(MaxRatBits)) {
    unsigned A = static_cast<unsigned>(E < 0 ? -E : E);
    APInt P = APInt::getOneBitSet(A + 2, A);
    Out = E >= 0 ? ratio(mulS(D, P), APInt(2, 1)) : ratio(D, P);
    return true;
  }
  if (!Hex && E >= -1200 && E <= 1200) {
    APInt P = pow10(static_cast<unsigned>(E < 0 ? -E : E));
    P = P.zext(P.getBitWidth() + 1);
    Out = E >= 0 ? ratio(mulS(D, P), APInt(2, 1)) : ratio(D, P);
    return true;
  }
  BigFloat F;
  if (!BigFloat::parse(Text, F))
    return false;
  Out = big(F);
  return true;
}

int Real::sign() const {
  if (!Rat)
    return F.sign();
  return Num.isZero() ? 0 : (Num.isNegative() ? -1 : 1);
}

bool Real::isInt() const { return Rat ? Den.isOne() : F.isInt(); }

bool Real::toInt(APInt &Out) const {
  if (!Rat)
    return F.toInt(Out);
  if (!Den.isOne())
    return false;
  Out = Num;
  return true;
}

BigFloat Real::toBigFloat() const {
  if (!Rat)
    return F;
  if (Num.isZero())
    return BigFloat();
  BigFloat R = BigFloat::ratio(absS(Num), Den);
  return Num.isNegative() ? R.neg() : R;
}

Real Real::neg() const {
  if (!Rat)
    return big(F.neg());
  Real R = *this;
  R.Num = trim(-Num.sext(Num.getBitWidth() + 1));
  return R;
}

Real Real::add(const Real &A, const Real &B) {
  if (A.Rat && B.Rat) {
    APInt X = mulS(A.Num, B.Den), Y = mulS(B.Num, A.Den);
    unsigned W = std::max(X.getBitWidth(), Y.getBitWidth()) + 1;
    return ratio(X.sext(W) + Y.sext(W), mulS(A.Den, B.Den));
  }
  return big(BigFloat::add(A.toBigFloat(), B.toBigFloat()));
}

Real Real::mul(const Real &A, const Real &B) {
  if (A.Rat && B.Rat)
    return ratio(mulS(A.Num, B.Num), mulS(A.Den, B.Den));
  return big(BigFloat::mul(A.toBigFloat(), B.toBigFloat()));
}

Real Real::quo(const Real &A, const Real &B) {
  if (A.Rat && B.Rat) {
    APInt N = mulS(A.Num, B.Den), D = mulS(A.Den, B.Num);
    if (D.isNegative()) {
      N = -N.sext(N.getBitWidth() + 1);
      D = -D.sext(D.getBitWidth() + 1);
    }
    return ratio(N, D);
  }
  return big(BigFloat::quo(A.toBigFloat(), B.toBigFloat()));
}

int Real::cmp(const Real &A, const Real &B) {
  if (A.Rat && B.Rat) {
    APInt X = mulS(A.Num, B.Den), Y = mulS(B.Num, A.Den);
    unsigned W = std::max(X.getBitWidth(), Y.getBitWidth());
    X = X.sext(W);
    Y = Y.sext(W);
    return X == Y ? 0 : (X.slt(Y) ? -1 : 1);
  }
  return BigFloat::cmp(A.toBigFloat(), B.toBigFloat());
}

//===----------------------------------------------------------------------===//
// ConstValue
//===----------------------------------------------------------------------===//

ConstValue ConstValue::makeBool(bool Bv) {
  ConstValue V;
  V.K = Bool;
  V.B = Bv;
  return V;
}
ConstValue ConstValue::makeString(std::string Sv) {
  ConstValue V;
  V.K = String;
  V.S = std::move(Sv);
  return V;
}
ConstValue ConstValue::makeInt(APInt Sv) {
  ConstValue V;
  V.K = Int;
  V.I = trim(Sv);
  return V;
}
ConstValue ConstValue::makeInt64(int64_t X) {
  return makeInt(APInt(64, static_cast<uint64_t>(X), true));
}
ConstValue ConstValue::makeUint64(uint64_t X) { return makeInt(APInt(65, X)); }
ConstValue ConstValue::makeFloat(Real Fv) {
  ConstValue V;
  if (Fv.overflows()) // go/constant: an infinite big.Float is unknown
    return V;
  V.K = Float;
  V.F = std::move(Fv);
  return V;
}
ConstValue ConstValue::makeFloat64(double D) {
  return makeFloat(Real::fromDouble(D));
}
ConstValue ConstValue::makeComplex(Real Re, Real Imv) {
  ConstValue V;
  V.K = Complex;
  V.F = std::move(Re);
  V.Im = std::move(Imv);
  return V;
}

static bool parseIntLit(llvm::StringRef T, bool LegacyOctal, APInt &Out) {
  std::string Clean;
  for (char C : T)
    if (C != '_')
      Clean.push_back(C);
  llvm::StringRef S = Clean;
  unsigned Radix = 10;
  if (S.size() >= 2 && S[0] == '0') {
    char P = S[1];
    if (P == 'x' || P == 'X') {
      Radix = 16;
      S = S.drop_front(2);
    } else if (P == 'b' || P == 'B') {
      Radix = 2;
      S = S.drop_front(2);
    } else if (P == 'o' || P == 'O') {
      Radix = 8;
      S = S.drop_front(2);
    } else if (LegacyOctal) {
      Radix = 8;
      S = S.drop_front(1);
    }
  }
  if (S.empty())
    return false;
  for (char C : S)
    if (!isDigitIn(C, Radix))
      return false;
  APInt U = parseUnsigned(S, Radix);
  Out = U.zext(U.getBitWidth() + 1);
  return true;
}

ConstValue ConstValue::fromLiteral(llvm::StringRef Text, tok::Kind K) {
  if ((Text.starts_with("-") || Text.starts_with("+")) &&
      (K == tok::int_lit || K == tok::float_lit || K == tok::imag_lit)) {
    ConstValue V = fromLiteral(Text.drop_front(), K);
    return Text[0] == '-' ? unaryOp(tok::sub, V, 0) : V;
  }
  switch (K) {
  case tok::int_lit: {
    APInt V;
    if (!parseIntLit(Text, true, V))
      return ConstValue();
    return makeInt(V);
  }
  case tok::float_lit: {
    Real F;
    if (!Real::parse(Text, F) || F.overflows())
      return ConstValue();
    return makeFloat(F);
  }
  case tok::imag_lit: {
    llvm::StringRef T = Text.drop_back(Text.ends_with("i") ? 1 : 0);
    Real F;
    bool Prefixed =
        T.size() > 1 && T[0] == '0' && llvm::StringRef("xXbBoO").contains(T[1]);
    bool Floaty = T.contains('.') ||
                  (!Prefixed && (T.contains('e') || T.contains('E'))) ||
                  (Prefixed && (T.contains('p') || T.contains('P')));
    if (!Floaty) {
      APInt V;
      if (!parseIntLit(T, false, V)) {
        // "0123i" is decimal.
        if (!parseIntLit(T.ltrim('0').empty() ? "0" : T.ltrim('0'), false, V))
          return ConstValue();
      }
      F = Real::fromInt(V);
    } else if (!Real::parse(T, F) || F.overflows()) {
      return ConstValue();
    }
    return makeComplex(Real(), F);
  }
  case tok::char_lit: {
    uint32_t Cp;
    if (!unquoteRune(Text, Cp))
      return ConstValue();
    return makeInt64(Cp);
  }
  case tok::string_lit: {
    std::string S;
    if (!unquoteString(Text, S))
      return ConstValue();
    return makeString(std::move(S));
  }
  default:
    return ConstValue();
  }
}

ConstValue ConstValue::toInt() const {
  switch (K) {
  case Int:
    return *this;
  case Float: {
    APInt V;
    if (F.overflows() || !F.toInt(V))
      return ConstValue();
    return makeInt(V);
  }
  case Complex: {
    APInt V;
    if (!Im.isZero() || F.overflows() || !F.toInt(V))
      return ConstValue();
    return makeInt(V);
  }
  default:
    return ConstValue();
  }
}
ConstValue ConstValue::toFloat() const {
  switch (K) {
  case Int:
    return makeFloat(Real::fromInt(I));
  case Float:
    return *this;
  case Complex:
    return Im.isZero() ? makeFloat(F) : ConstValue();
  default:
    return ConstValue();
  }
}
ConstValue ConstValue::toComplex() const {
  switch (K) {
  case Int:
    return makeComplex(Real::fromInt(I), Real());
  case Float:
    return makeComplex(F, Real());
  case Complex:
    return *this;
  default:
    return ConstValue();
  }
}
ConstValue ConstValue::real() const {
  switch (K) {
  case Int:
  case Float:
    return *this;
  case Complex:
    return makeFloat(F);
  default:
    return ConstValue();
  }
}
ConstValue ConstValue::imag() const {
  switch (K) {
  case Int:
    return makeInt64(0);
  case Float:
    return makeFloat(Real());
  case Complex:
    return makeFloat(Im);
  default:
    return ConstValue();
  }
}

int ConstValue::sign() const {
  switch (K) {
  case Int:
    return I.isZero() ? 0 : (I.isNegative() ? -1 : 1);
  case Float:
    return F.sign();
  case Complex:
    return (F.sign() | Im.sign()) != 0 ? 1 : 0;
  default:
    return 1;
  }
}
unsigned ConstValue::bitLen() const {
  if (K != Int)
    return 0;
  APInt A = I.sext(I.getBitWidth() + 1);
  if (A.isNegative())
    A = -A;
  return A.getActiveBits();
}
bool ConstValue::fitsInt64(int64_t &Out) const {
  if (K != Int || I.getSignificantBits() > 64)
    return false;
  Out = I.getSExtValue();
  return true;
}
bool ConstValue::fitsUint64(uint64_t &Out) const {
  if (K != Int || I.isNegative() || I.getActiveBits() > 64)
    return false;
  Out = I.getZExtValue();
  return true;
}
double ConstValue::float64() const {
  switch (K) {
  case Int:
    return BigFloat::fromInt(I).toIEEE(53);
  case Float:
  case Complex:
    return F.toIEEE(53);
  default:
    return 0;
  }
}
float ConstValue::float32() const {
  switch (K) {
  case Int:
    return static_cast<float>(BigFloat::fromInt(I).toIEEE(24));
  case Float:
  case Complex:
    return static_cast<float>(F.toIEEE(24));
  default:
    return 0;
  }
}

ConstValue ConstValue::roundFloat32() const {
  auto R = [](const Real &X) { return Real::fromDouble(X.toIEEE(24)); };
  if (K == Float)
    return makeFloat(R(F));
  if (K == Complex)
    return makeComplex(R(F), R(Im));
  return *this;
}
ConstValue ConstValue::roundFloat64() const {
  auto R = [](const Real &X) { return Real::fromDouble(X.toIEEE(53)); };
  if (K == Float)
    return makeFloat(R(F));
  if (K == Complex)
    return makeComplex(R(F), R(Im));
  return *this;
}

static std::string quoteGo(const std::string &S) {
  std::string Q = "\"";
  for (unsigned char C : S) {
    switch (C) {
    case '"':
      Q += "\\\"";
      break;
    case '\\':
      Q += "\\\\";
      break;
    case '\n':
      Q += "\\n";
      break;
    case '\t':
      Q += "\\t";
      break;
    case '\r':
      Q += "\\r";
      break;
    default:
      if (C < 0x20 || C == 0x7f) {
        char B[8];
        std::snprintf(B, sizeof B, "\\x%02x", C);
        Q += B;
      } else {
        Q.push_back(static_cast<char>(C));
      }
    }
  }
  return Q + "\"";
}

std::string ConstValue::str() const {
  switch (K) {
  case Unknown:
    return "unknown";
  case Bool:
    return B ? "true" : "false";
  case String: {
    std::string Q = quoteGo(S);
    // At most 72 characters (not bytes).
    size_t Chars = 0, I2 = 0;
    for (size_t J = 0; J < Q.size(); ++J)
      if ((static_cast<unsigned char>(Q[J]) & 0xC0) != 0x80)
        ++Chars;
    if (Chars > 72) {
      size_t N = 0;
      while (I2 < Q.size()) {
        if ((static_cast<unsigned char>(Q[I2]) & 0xC0) != 0x80) {
          if (N == 72 - 3)
            break;
          ++N;
        }
        ++I2;
      }
      Q = Q.substr(0, I2) + "...";
    }
    return Q;
  }
  case Int: {
    llvm::SmallString<64> Buf;
    I.toString(Buf, 10, true);
    return std::string(Buf);
  }
  case Float:
    return F.str();
  case Complex:
    return "(" + F.str() + " + " + Im.str() + "i)";
  }
  return "";
}
std::string ConstValue::exactStr() const { return str(); }

//===----------------------------------------------------------------------===//
// Operations
//===----------------------------------------------------------------------===//

void ConstValue::match(ConstValue &X, ConstValue &Y) {
  auto Rank = [](Kind K) {
    return K == Int ? 1 : K == Float ? 2 : K == Complex ? 3 : 0;
  };
  int RX = Rank(X.K), RY = Rank(Y.K);
  if (RX == 0 || RY == 0 || RX == RY)
    return;
  int R = std::max(RX, RY);
  auto Conv = [R](ConstValue &V) { V = R == 2 ? V.toFloat() : V.toComplex(); };
  if (RX < R)
    Conv(X);
  if (RY < R)
    Conv(Y);
}

ConstValue ConstValue::unaryOp(tok::Kind Op, const ConstValue &X,
                               unsigned Prec) {
  switch (Op) {
  case tok::add:
    return X.isNumeric() ? X : ConstValue();
  case tok::sub:
    switch (X.K) {
    case Int:
      return makeInt(-X.I.sext(X.I.getBitWidth() + 1));
    case Float:
      return makeFloat(X.F.neg());
    case Complex:
      return makeComplex(X.F.neg(), X.Im.neg());
    default:
      return ConstValue();
    }
  case tok::xor_: {
    if (X.K != Int)
      return ConstValue();
    unsigned W = std::max(X.I.getBitWidth(), Prec) + 2;
    APInt V = ~X.I.sext(W);
    if (Prec > 0) {
      APInt Mask = APInt::getLowBitsSet(W, Prec);
      V &= Mask;
    }
    return makeInt(V);
  }
  case tok::not_:
    return X.K == Bool ? makeBool(!X.B) : ConstValue();
  default:
    return ConstValue();
  }
}

ConstValue ConstValue::binaryOp(const ConstValue &X0, tok::Kind Op,
                                const ConstValue &Y0) {
  ConstValue X = X0, Y = Y0;
  match(X, Y);
  if (X.K != Y.K || X.K == Unknown)
    return ConstValue();
  switch (X.K) {
  case Bool:
    if (Op == tok::land)
      return makeBool(X.B && Y.B);
    if (Op == tok::lor)
      return makeBool(X.B || Y.B);
    return ConstValue();
  case String:
    return Op == tok::add ? makeString(X.S + Y.S) : ConstValue();
  case Int: {
    APInt A = X.I, B = Y.I;
    unify(A, B);
    switch (Op) {
    case tok::add:
      return makeInt(A + B);
    case tok::sub:
      return makeInt(A - B);
    case tok::mul: {
      unsigned W = X.I.getBitWidth() + Y.I.getBitWidth() + 1;
      return makeInt(X.I.sext(W) * Y.I.sext(W));
    }
    case tok::quo_assign:
    case tok::rem:
      if (B.isZero())
        return ConstValue();
      return makeInt(Op == tok::rem ? A.srem(B) : A.sdiv(B));
    case tok::quo: {
      if (B.isZero())
        return ConstValue();
      return makeFloat(Real::quo(Real::fromInt(X.I), Real::fromInt(Y.I)));
    }
    case tok::and_:
      return makeInt(A & B);
    case tok::or_:
      return makeInt(A | B);
    case tok::xor_:
      return makeInt(A ^ B);
    case tok::and_not:
      return makeInt(A & ~B);
    default:
      return ConstValue();
    }
  }
  case Float:
    switch (Op) {
    case tok::add:
      return makeFloat(Real::add(X.F, Y.F));
    case tok::sub:
      return makeFloat(Real::sub(X.F, Y.F));
    case tok::mul:
      return makeFloat(Real::mul(X.F, Y.F));
    case tok::quo:
      if (Y.F.isZero())
        return ConstValue();
      return makeFloat(Real::quo(X.F, Y.F));
    default:
      return ConstValue();
    }
  case Complex: {
    const Real &A = X.F, &B = X.Im, &C = Y.F, &D = Y.Im;
    switch (Op) {
    case tok::add:
      return makeComplex(Real::add(A, C), Real::add(B, D));
    case tok::sub:
      return makeComplex(Real::sub(A, C), Real::sub(B, D));
    case tok::mul:
      return makeComplex(Real::sub(Real::mul(A, C), Real::mul(B, D)),
                         Real::add(Real::mul(B, C), Real::mul(A, D)));
    case tok::quo: {
      Real S = Real::add(Real::mul(C, C), Real::mul(D, D));
      if (S.isZero())
        return ConstValue();
      Real Re = Real::add(Real::mul(A, C), Real::mul(B, D));
      Real Imx = Real::sub(Real::mul(B, C), Real::mul(A, D));
      return makeComplex(Real::quo(Re, S), Real::quo(Imx, S));
    }
    default:
      return ConstValue();
    }
  }
  default:
    return ConstValue();
  }
}

ConstValue ConstValue::shift(const ConstValue &X, tok::Kind Op,
                             unsigned Count) {
  if (X.K != Int)
    return ConstValue();
  if (Op == tok::shl)
    return makeInt(X.I.sext(X.I.getBitWidth() + Count + 1).shl(Count));
  if (Count >= X.I.getBitWidth())
    Count = X.I.getBitWidth() - 1;
  return makeInt(X.I.ashr(Count));
}

bool ConstValue::compare(const ConstValue &X0, tok::Kind Op,
                         const ConstValue &Y0) {
  ConstValue X = X0, Y = Y0;
  match(X, Y);
  if (X.K != Y.K)
    return false;
  int C = 0;
  switch (X.K) {
  case Bool:
    if (Op == tok::eql)
      return X.B == Y.B;
    if (Op == tok::neq)
      return X.B != Y.B;
    return false;
  case String:
    C = X.S.compare(Y.S);
    break;
  case Int: {
    APInt A = X.I, B = Y.I;
    unify(A, B);
    C = A == B ? 0 : (A.slt(B) ? -1 : 1);
    break;
  }
  case Float:
    C = Real::cmp(X.F, Y.F);
    break;
  case Complex: {
    bool Eq = Real::cmp(X.F, Y.F) == 0 && Real::cmp(X.Im, Y.Im) == 0;
    if (Op == tok::eql)
      return Eq;
    if (Op == tok::neq)
      return !Eq;
    return false;
  }
  default:
    return false;
  }
  switch (Op) {
  case tok::eql:
    return C == 0;
  case tok::neq:
    return C != 0;
  case tok::lss:
    return C < 0;
  case tok::leq:
    return C <= 0;
  case tok::gtr:
    return C > 0;
  case tok::geq:
    return C >= 0;
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Literals
//===----------------------------------------------------------------------===//

void sema::appendUTF8(std::string &Out, uint32_t Cp) {
  if (Cp > 0x10FFFF || (Cp >= 0xD800 && Cp < 0xE000))
    Cp = 0xFFFD;
  if (Cp < 0x80) {
    Out.push_back(static_cast<char>(Cp));
  } else if (Cp < 0x800) {
    Out.push_back(static_cast<char>(0xC0 | (Cp >> 6)));
    Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F)));
  } else if (Cp < 0x10000) {
    Out.push_back(static_cast<char>(0xE0 | (Cp >> 12)));
    Out.push_back(static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)));
    Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F)));
  } else {
    Out.push_back(static_cast<char>(0xF0 | (Cp >> 18)));
    Out.push_back(static_cast<char>(0x80 | ((Cp >> 12) & 0x3F)));
    Out.push_back(static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)));
    Out.push_back(static_cast<char>(0x80 | (Cp & 0x3F)));
  }
}

/// Reads one (possibly escaped) character of a quoted literal. IsByte is set
/// for \x and octal escapes, which are bytes, not code points.
static bool readChar(llvm::StringRef S, size_t &I, char Quote, uint32_t &Cp,
                     bool &IsByte) {
  IsByte = false;
  unsigned char C = S[I];
  if (C != '\\') {
    if (C < 0x80) {
      Cp = C;
      ++I;
      return true;
    }
    // Decode UTF-8.
    unsigned N = C >= 0xF0 ? 4 : C >= 0xE0 ? 3 : C >= 0xC0 ? 2 : 1;
    if (I + N > S.size()) {
      Cp = 0xFFFD;
      ++I;
      return true;
    }
    if (N == 1) {
      Cp = 0xFFFD;
      ++I;
      return true;
    }
    Cp = C & (0xFF >> (N + 1));
    for (unsigned K = 1; K < N; ++K)
      Cp = (Cp << 6) | (static_cast<unsigned char>(S[I + K]) & 0x3F);
    I += N;
    return true;
  }
  if (++I >= S.size())
    return false;
  char E = S[I++];
  auto Hex = [&](unsigned N) {
    uint32_t V = 0;
    for (unsigned K = 0; K < N; ++K) {
      if (I >= S.size() || !isDigitIn(S[I], 16))
        return false;
      char D = S[I++];
      V = V * 16 + (D <= '9' ? D - '0' : (D | 0x20) - 'a' + 10);
    }
    Cp = V;
    return true;
  };
  switch (E) {
  case 'a':
    Cp = 7;
    return true;
  case 'b':
    Cp = 8;
    return true;
  case 'f':
    Cp = 12;
    return true;
  case 'n':
    Cp = 10;
    return true;
  case 'r':
    Cp = 13;
    return true;
  case 't':
    Cp = 9;
    return true;
  case 'v':
    Cp = 11;
    return true;
  case '\\':
    Cp = '\\';
    return true;
  case '\'':
  case '"':
    if (E != Quote)
      return false;
    Cp = E;
    return true;
  case 'x':
    IsByte = true;
    return Hex(2);
  case 'u':
    return Hex(4);
  case 'U':
    return Hex(8);
  default:
    if (E >= '0' && E <= '7') {
      uint32_t V = E - '0';
      for (int K = 0; K < 2; ++K) {
        if (I >= S.size() || S[I] < '0' || S[I] > '7')
          return false;
        V = V * 8 + (S[I++] - '0');
      }
      if (V > 255)
        return false;
      IsByte = true;
      Cp = V;
      return true;
    }
    return false;
  }
}

bool sema::unquoteRune(llvm::StringRef Lit, uint32_t &Out) {
  if (Lit.size() < 3 || Lit.front() != '\'' || Lit.back() != '\'')
    return false;
  llvm::StringRef Body = Lit.slice(1, Lit.size() - 1);
  size_t I = 0;
  bool IsByte;
  if (!readChar(Body, I, '\'', Out, IsByte))
    return false;
  return I == Body.size();
}

bool sema::unquoteString(llvm::StringRef Lit, std::string &Out) {
  if (Lit.size() < 2)
    return false;
  char Q = Lit.front();
  if (Q != Lit.back())
    return false;
  llvm::StringRef Body = Lit.slice(1, Lit.size() - 1);
  Out.clear();
  if (Q == '`') {
    for (char C : Body)
      if (C != '\r')
        Out.push_back(C);
    return true;
  }
  if (Q != '"')
    return false;
  size_t I = 0;
  while (I < Body.size()) {
    if (Body[I] == '"')
      return false;
    unsigned char C = Body[I];
    if (C != '\\' && C >= 0x80) {
      Out.push_back(static_cast<char>(C)); // keep raw bytes
      ++I;
      continue;
    }
    uint32_t Cp;
    bool IsByte;
    if (!readChar(Body, I, '"', Cp, IsByte))
      return false;
    if (IsByte)
      Out.push_back(static_cast<char>(Cp));
    else
      appendUTF8(Out, Cp);
  }
  return true;
}
