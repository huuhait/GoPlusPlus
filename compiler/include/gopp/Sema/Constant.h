//===- gopp/Sema/Constant.h - Untyped constant arithmetic -------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Constant values, in the shape of Go's go/constant (P03.02, D3.2, ADR-0009).
//   - Integers are llvm::APInt, signed, as wide as the value needs.
//   - Floats are Real: an exact rational while numerator and denominator fit
//     in Real::MaxRatBits bits, else a BigFloat (a binary float with a
//     512-bit mantissa; the Go spec asks for at least 256 bits). go/constant
//     does the same with big.Rat and big.Float.
//   - Complex numbers are two Reals.
// A ConstValue is a plain value type; the checker keeps the ones it needs in
// a pool, so that the type table can point to them.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_CONSTANT_H
#define GOPP_SEMA_CONSTANT_H

#include "gopp/Lex/Token.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace gopp {
namespace sema {

/// value = (Neg ? -1 : 1) * Mant * 2^Exp. Mant has exactly Prec bits and its
/// top bit set, or the value is zero (Mant == 0).
class BigFloat {
public:
  static constexpr unsigned Prec = 512;
  /// Exponents beyond this are "infinite" (overflow in the checker).
  static constexpr int64_t MaxExp = 0x7fffffff; // big.Float: math.MaxInt32

  BigFloat() : Mant(1, 0) {}
  static BigFloat fromInt(const llvm::APInt &SignedInt);
  static BigFloat fromDouble(double D);
  /// Parses a Go float literal (decimal or hexadecimal, '_' allowed).
  /// Returns false if the text is malformed.
  static bool parse(llvm::StringRef Text, BigFloat &Out);
  /// Num / Den for positive integers.
  static BigFloat ratio(const llvm::APInt &Num, const llvm::APInt &Den);

  bool isZero() const { return Mant.isZero(); }
  int sign() const { return isZero() ? 0 : (Neg ? -1 : 1); }
  /// Binary exponent of the leading bit: value is in [2^e, 2^(e+1)).
  int64_t magnitudeExp() const { return Exp + Prec - 1; }
  bool isInt() const;
  /// The value as an integer if it has no fraction.
  bool toInt(llvm::APInt &Out) const;
  /// Truncates toward zero.
  llvm::APInt truncToInt() const;
  double toDouble() const;
  bool overflows() const { return !isZero() && magnitudeExp() > MaxExp; }

  BigFloat neg() const;
  BigFloat abs() const;
  static BigFloat add(const BigFloat &A, const BigFloat &B);
  static BigFloat sub(const BigFloat &A, const BigFloat &B);
  static BigFloat mul(const BigFloat &A, const BigFloat &B);
  /// B must not be zero.
  static BigFloat quo(const BigFloat &A, const BigFloat &B);
  static int cmp(const BigFloat &A, const BigFloat &B);

  /// "%.6g"-like text.
  std::string str() const;
  /// The value rounded to nearest-even to an IEEE binary format with P
  /// mantissa bits: single (24) or double (53), subnormals included; +-inf
  /// if it is too large.
  double toIEEE(unsigned P) const;

  bool Neg = false;
  llvm::APInt Mant;
  int64_t Exp = 0;

private:
  /// Rounds the exact value (-1)^Neg * M * 2^E to Prec bits.
  static BigFloat round(bool Neg, const llvm::APInt &M, int64_t E,
                        bool Sticky = false);
};

/// An untyped float value: the exact rational Num/Den (Den > 0, reduced)
/// while both fit in MaxRatBits bits, else a BigFloat. Operations are exact
/// as long as the operands and the result are rationals.
class Real {
public:
  static constexpr unsigned MaxRatBits = 4096;

  Real() : Num(1, 0), Den(2, 1) {}
  static Real fromInt(const llvm::APInt &SignedInt);
  /// Exact when the binary exponent is small enough, else the BigFloat.
  static Real fromFloat(const BigFloat &F);
  static Real fromDouble(double D) {
    return fromFloat(BigFloat::fromDouble(D));
  }
  /// Parses a Go float literal (decimal or hexadecimal, '_' allowed).
  /// Returns false if the text is malformed.
  static bool parse(llvm::StringRef Text, Real &Out);

  bool isRat() const { return Rat; }
  /// The exact representation, for serialization (Serialize.h): Num / Den
  /// when isRat(), else bigValue(). fromRat and fromBig are the inverses.
  const llvm::APInt &ratNum() const { return Num; }
  const llvm::APInt &ratDen() const { return Den; }
  const BigFloat &bigValue() const { return F; }
  static Real fromRat(llvm::APInt Num, llvm::APInt Den) {
    Real R;
    R.Num = std::move(Num);
    R.Den = std::move(Den);
    return R;
  }
  static Real fromBig(const BigFloat &F) { return big(F); }
  bool isZero() const { return Rat ? Num.isZero() : F.isZero(); }
  int sign() const;
  bool isInt() const;
  /// The value as an integer if it has no fraction.
  bool toInt(llvm::APInt &Out) const;
  BigFloat toBigFloat() const;
  double toIEEE(unsigned P) const { return toBigFloat().toIEEE(P); }
  bool overflows() const { return !Rat && F.overflows(); }

  Real neg() const;
  static Real add(const Real &A, const Real &B);
  static Real sub(const Real &A, const Real &B) { return add(A, B.neg()); }
  static Real mul(const Real &A, const Real &B);
  /// B must not be zero.
  static Real quo(const Real &A, const Real &B);
  static int cmp(const Real &A, const Real &B);
  std::string str() const { return toBigFloat().str(); }

private:
  /// Num / Den reduced (Den > 0); the BigFloat if it does not fit.
  static Real ratio(llvm::APInt Num, llvm::APInt Den);
  static Real big(const BigFloat &F);
  bool Rat = true;
  llvm::APInt Num, Den; // if Rat: signed, Den > 0
  BigFloat F;           // if !Rat
};

class ConstValue {
public:
  enum Kind : uint8_t { Unknown, Bool, String, Int, Float, Complex };

  ConstValue() = default;
  static ConstValue makeUnknown() { return ConstValue(); }
  static ConstValue makeBool(bool B);
  static ConstValue makeString(std::string S);
  static ConstValue makeInt(llvm::APInt SignedInt);
  static ConstValue makeInt64(int64_t V);
  static ConstValue makeUint64(uint64_t V);
  static ConstValue makeFloat(Real F);
  static ConstValue makeFloat64(double D);
  static ConstValue makeComplex(Real Re, Real Im);
  /// From the text of a literal token (int, float, imaginary, char, string).
  /// Unknown if the text is malformed.
  static ConstValue fromLiteral(llvm::StringRef Text, tok::Kind K);

  Kind kind() const { return K; }
  bool isUnknown() const { return K == Unknown; }
  bool isNumeric() const { return K == Int || K == Float || K == Complex; }

  bool boolVal() const { return B; }
  const std::string &stringVal() const { return S; }
  const llvm::APInt &intVal() const { return I; }
  /// The value of a Float, or the real part of a Complex.
  const Real &floatVal() const { return F; }
  const Real &imagVal() const { return Im; }
  /// A Float or Complex value too large for any float type.
  bool overflows() const { return F.overflows() || Im.overflows(); }

  /// Conversions that keep the kind if they cannot be done (Unknown result).
  /// toInt succeeds for an integral Float or Complex with zero imaginary part.
  ConstValue toInt() const;
  ConstValue toFloat() const;
  ConstValue toComplex() const;
  ConstValue real() const;
  ConstValue imag() const;

  int sign() const;
  /// Number of bits of |x| for an Int.
  unsigned bitLen() const;
  bool fitsInt64(int64_t &Out) const;
  bool fitsUint64(uint64_t &Out) const;
  /// Rounded to float64 / float32 (+-inf if too large).
  double float64() const;
  float float32() const;

  /// Go's constant.Value.String (short form for messages).
  std::string str() const;
  /// The exact decimal text of an Int, the same as str() for the others.
  std::string exactStr() const;

  /// Unary operators: add, sub, xor_ (bitwise not), not_. For xor_ on an
  /// unsigned type, Prec is the width of the type (else 0).
  static ConstValue unaryOp(tok::Kind Op, const ConstValue &X, unsigned Prec);
  /// Binary operators. Both operands must have the same kind (see match). Use
  /// quo_assign for integer division. Returns Unknown on division by zero.
  static ConstValue binaryOp(const ConstValue &X, tok::Kind Op,
                             const ConstValue &Y);
  /// shl or shr by Count.
  static ConstValue shift(const ConstValue &X, tok::Kind Op, unsigned Count);
  /// Relational operators (eql, neq, lss, leq, gtr, geq).
  static bool compare(const ConstValue &X, tok::Kind Op, const ConstValue &Y);
  /// Converts X and Y to the larger of their numeric kinds.
  static void match(ConstValue &X, ConstValue &Y);

  /// Rounds a Float or Complex to 32 or 64 bits.
  ConstValue roundFloat32() const;
  ConstValue roundFloat64() const;

private:
  Kind K = Unknown;
  bool B = false;
  std::string S;
  llvm::APInt I;
  Real F, Im;
};

/// Decodes a Go string literal ("..." or `...`) to its bytes. False if
/// malformed.
bool unquoteString(llvm::StringRef Lit, std::string &Out);
/// Decodes a rune literal to its code point. False if malformed.
bool unquoteRune(llvm::StringRef Lit, uint32_t &Out);
/// Appends the UTF-8 encoding of a code point (U+FFFD for invalid ones).
void appendUTF8(std::string &Out, uint32_t Cp);

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_CONSTANT_H
