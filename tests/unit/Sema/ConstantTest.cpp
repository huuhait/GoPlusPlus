//===- ConstantTest.cpp - Constant arithmetic tests (P03.02) --------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The tables below are ported from go/constant's value_test.go (Copyright 2013
// The Go Authors. All rights reserved. Use of this source code is governed by
// a BSD-style license, see NOTICE). Cases that expect a Go panic are left out.
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Constant.h"

#include "llvm/ADT/StringExtras.h"
#include "gtest/gtest.h"

#include <cmath>

using namespace gopp;
using namespace gopp::sema;
using llvm::SmallVector;
using llvm::StringRef;

static const char *const intTests[] = {
    R"GOPP(0_123 = 0123)GOPP",
    R"GOPP(0123_456 = 0123456)GOPP",
    R"GOPP(1_234 = 1234)GOPP",
    R"GOPP(1_234_567 = 1234567)GOPP",
    R"GOPP(0X_0 = 0)GOPP",
    R"GOPP(0X_1234 = 0x1234)GOPP",
    R"GOPP(0X_CAFE_f00d = 0xcafef00d)GOPP",
    R"GOPP(0o0 = 0)GOPP",
    R"GOPP(0o1234 = 01234)GOPP",
    R"GOPP(0o01234567 = 01234567)GOPP",
    R"GOPP(0O0 = 0)GOPP",
    R"GOPP(0O1234 = 01234)GOPP",
    R"GOPP(0O01234567 = 01234567)GOPP",
    R"GOPP(0o_0 = 0)GOPP",
    R"GOPP(0o_1234 = 01234)GOPP",
    R"GOPP(0o0123_4567 = 01234567)GOPP",
    R"GOPP(0O_0 = 0)GOPP",
    R"GOPP(0O_1234 = 01234)GOPP",
    R"GOPP(0O0123_4567 = 01234567)GOPP",
    R"GOPP(0b0 = 0)GOPP",
    R"GOPP(0b1011 = 0xb)GOPP",
    R"GOPP(0b00101101 = 0x2d)GOPP",
    R"GOPP(0B0 = 0)GOPP",
    R"GOPP(0B1011 = 0xb)GOPP",
    R"GOPP(0B00101101 = 0x2d)GOPP",
    R"GOPP(0b_0 = 0)GOPP",
    R"GOPP(0b10_11 = 0xb)GOPP",
    R"GOPP(0b_0010_1101 = 0x2d)GOPP",
};

static const char *const floatTests[] = {
    R"GOPP(1_2_3. = 123.)GOPP",
    R"GOPP(0_123. = 123.)GOPP",
    R"GOPP(0_0e0 = 0.)GOPP",
    R"GOPP(1_2_3e0 = 123.)GOPP",
    R"GOPP(0_123e0 = 123.)GOPP",
    R"GOPP(0e-0_0 = 0.)GOPP",
    R"GOPP(1_2_3E+0 = 123.)GOPP",
    R"GOPP(0123E1_2_3 = 123e123)GOPP",
    R"GOPP(0.e+1 = 0.)GOPP",
    R"GOPP(123.E-1_0 = 123e-10)GOPP",
    R"GOPP(01_23.e123 = 123e123)GOPP",
    R"GOPP(.0e-1 = .0)GOPP",
    R"GOPP(.123E+10 = .123e10)GOPP",
    R"GOPP(.0123E123 = .0123e123)GOPP",
    R"GOPP(1_2_3.123 = 123.123)GOPP",
    R"GOPP(0123.01_23 = 123.0123)GOPP",
    R"GOPP(1e-1000000000 = 0)GOPP",
    R"GOPP(1e+1000000000 = ?)GOPP",
    R"GOPP(6e5518446744 = ?)GOPP",
    R"GOPP(-6e5518446744 = ?)GOPP",
    R"GOPP(0x0.p+0 = 0.)GOPP",
    R"GOPP(0Xdeadcafe.p-10 = 0xdeadcafe/1024)GOPP",
    R"GOPP(0x1234.P84 = 0x1234000000000000000000000)GOPP",
    R"GOPP(0x.1p-0 = 1/16)GOPP",
    R"GOPP(0X.deadcafep4 = 0xdeadcafe/0x10000000)GOPP",
    R"GOPP(0x.1234P+12 = 0x1234/0x10)GOPP",
    R"GOPP(0x0p0 = 0.)GOPP",
    R"GOPP(0Xdeadcafep+1 = 0x1bd5b95fc)GOPP",
    R"GOPP(0x1234P-10 = 0x1234/1024)GOPP",
    R"GOPP(0x0.0p0 = 0.)GOPP",
    R"GOPP(0Xdead.cafep+1 = 0x1bd5b95fc/0x10000)GOPP",
    R"GOPP(0x12.34P-10 = 0x1234/0x40000)GOPP",
    R"GOPP(0Xdead_cafep+1 = 0xdeadcafep+1)GOPP",
    R"GOPP(0x_1234P-10 = 0x1234p-10)GOPP",
    R"GOPP(0X_dead_cafe.p-10 = 0xdeadcafe.p-10)GOPP",
    R"GOPP(0x12_34.P1_2_3 = 0x1234.p123)GOPP",
};

static const char *const imagTests[] = {
    R"GOPP(1_234i = 1234i)GOPP",
    R"GOPP(1_234_567i = 1234567i)GOPP",
    R"GOPP(0.i = 0i)GOPP",
    R"GOPP(123.i = 123i)GOPP",
    R"GOPP(0123.i = 123i)GOPP",
    R"GOPP(0.e+1i = 0i)GOPP",
    R"GOPP(123.E-1_0i = 123e-10i)GOPP",
    R"GOPP(01_23.e123i = 123e123i)GOPP",
    R"GOPP(1e-1000000000i = 0i)GOPP",
    R"GOPP(1e+1000000000i = ?)GOPP",
    R"GOPP(6e5518446744i = ?)GOPP",
    R"GOPP(-6e5518446744i = ?)GOPP",
};

static const char *const opTests[] = {
    R"GOPP(+ 0 = 0)GOPP",
    R"GOPP(+ ? = ?)GOPP",
    R"GOPP(- 1 = -1)GOPP",
    R"GOPP(- ? = ?)GOPP",
    R"GOPP(^ 0 = -1)GOPP",
    R"GOPP(^ ? = ?)GOPP",
    R"GOPP(! true = false)GOPP",
    R"GOPP(! false = true)GOPP",
    R"GOPP(! ? = ?)GOPP",
    R"GOPP("" + "" = "")GOPP",
    R"GOPP("foo" + "" = "foo")GOPP",
    R"GOPP("" + "bar" = "bar")GOPP",
    R"GOPP("foo" + "bar" = "foobar")GOPP",
    R"GOPP(0 + 0 = 0)GOPP",
    R"GOPP(0 + 0.1 = 0.1)GOPP",
    R"GOPP(0 + 0.1i = 0.1i)GOPP",
    R"GOPP(0.1 + 0.9 = 1)GOPP",
    R"GOPP(1e100 + 1e100 = 2e100)GOPP",
    R"GOPP(? + 0 = ?)GOPP",
    R"GOPP(0 + ? = ?)GOPP",
    R"GOPP(0 - 0 = 0)GOPP",
    R"GOPP(0 - 0.1 = -0.1)GOPP",
    R"GOPP(0 - 0.1i = -0.1i)GOPP",
    R"GOPP(1e100 - 1e100 = 0)GOPP",
    R"GOPP(? - 0 = ?)GOPP",
    R"GOPP(0 - ? = ?)GOPP",
    R"GOPP(0 * 0 = 0)GOPP",
    R"GOPP(1 * 0.1 = 0.1)GOPP",
    R"GOPP(1 * 0.1i = 0.1i)GOPP",
    R"GOPP(1i * 1i = -1)GOPP",
    R"GOPP(? * 0 = ?)GOPP",
    R"GOPP(0 * ? = ?)GOPP",
    R"GOPP(0 * 1e+1000000000 = ?)GOPP",
    R"GOPP(10 / 2 = 5)GOPP",
    R"GOPP(5 / 3 = 5/3)GOPP",
    R"GOPP(5i / 3i = 5/3)GOPP",
    R"GOPP(? / 0 = ?)GOPP",
    R"GOPP(0 / ? = ?)GOPP",
    R"GOPP(0 * 1e+1000000000i = ?)GOPP",
    R"GOPP(10 % 3 = 1)GOPP",
    R"GOPP(? % 0 = ?)GOPP",
    R"GOPP(0 % ? = ?)GOPP",
    R"GOPP(0 & 0 = 0)GOPP",
    R"GOPP(12345 & 0 = 0)GOPP",
    R"GOPP(0xff & 0xf = 0xf)GOPP",
    R"GOPP(? & 0 = ?)GOPP",
    R"GOPP(0 & ? = ?)GOPP",
    R"GOPP(0 | 0 = 0)GOPP",
    R"GOPP(12345 | 0 = 12345)GOPP",
    R"GOPP(0xb | 0xa0 = 0xab)GOPP",
    R"GOPP(? | 0 = ?)GOPP",
    R"GOPP(0 | ? = ?)GOPP",
    R"GOPP(0 ^ 0 = 0)GOPP",
    R"GOPP(1 ^ -1 = -2)GOPP",
    R"GOPP(? ^ 0 = ?)GOPP",
    R"GOPP(0 ^ ? = ?)GOPP",
    R"GOPP(0 &^ 0 = 0)GOPP",
    R"GOPP(0xf &^ 1 = 0xe)GOPP",
    R"GOPP(1 &^ 0xf = 0)GOPP",
    R"GOPP(0 << 0 = 0)GOPP",
    R"GOPP(1 << 10 = 1024)GOPP",
    R"GOPP(0 >> 0 = 0)GOPP",
    R"GOPP(1024 >> 10 == 1)GOPP",
    R"GOPP(? << 0 == ?)GOPP",
    R"GOPP(? >> 10 == ?)GOPP",
    R"GOPP(false == false = true)GOPP",
    R"GOPP(false == true = false)GOPP",
    R"GOPP(true == false = false)GOPP",
    R"GOPP(true == true = true)GOPP",
    R"GOPP(false != false = false)GOPP",
    R"GOPP(false != true = true)GOPP",
    R"GOPP(true != false = true)GOPP",
    R"GOPP(true != true = false)GOPP",
    R"GOPP("foo" == "bar" = false)GOPP",
    R"GOPP("foo" != "bar" = true)GOPP",
    R"GOPP("foo" < "bar" = false)GOPP",
    R"GOPP("foo" <= "bar" = false)GOPP",
    R"GOPP("foo" > "bar" = true)GOPP",
    R"GOPP("foo" >= "bar" = true)GOPP",
    R"GOPP(0 == 0 = true)GOPP",
    R"GOPP(0 != 0 = false)GOPP",
    R"GOPP(0 < 10 = true)GOPP",
    R"GOPP(10 <= 10 = true)GOPP",
    R"GOPP(0 > 10 = false)GOPP",
    R"GOPP(10 >= 10 = true)GOPP",
    R"GOPP(1/123456789 == 1/123456789 == true)GOPP",
    R"GOPP(1/123456789 != 1/123456789 == false)GOPP",
    R"GOPP(1/123456789 < 1/123456788 == true)GOPP",
    R"GOPP(1/123456788 <= 1/123456789 == false)GOPP",
    R"GOPP(0.11 > 0.11 = false)GOPP",
    R"GOPP(0.11 >= 0.11 = true)GOPP",
    R"GOPP(? == 0 = false)GOPP",
    R"GOPP(? != 0 = false)GOPP",
    R"GOPP(? < 10 = false)GOPP",
    R"GOPP(? <= 10 = false)GOPP",
    R"GOPP(? > 10 = false)GOPP",
    R"GOPP(? >= 10 = false)GOPP",
    R"GOPP(0 == ? = false)GOPP",
    R"GOPP(0 != ? = false)GOPP",
    R"GOPP(0 < ? = false)GOPP",
    R"GOPP(10 <= ? = false)GOPP",
    R"GOPP(0 > ? = false)GOPP",
    R"GOPP(10 >= ? = false)GOPP",
};

namespace {

ConstValue lit(StringRef S, tok::Kind K) {
  return ConstValue::fromLiteral(S, K);
}

/// go/constant's test helper `val`.
ConstValue val(StringRef L) {
  if (L.empty() || L == "?")
    return ConstValue();
  if (L == "true")
    return ConstValue::makeBool(true);
  if (L == "false")
    return ConstValue::makeBool(false);
  auto [A, B] = L.split('/');
  if (!B.empty() || L.contains('/'))
    return ConstValue::binaryOp(lit(A, tok::int_lit), tok::quo,
                                lit(B, tok::int_lit));
  tok::Kind K = tok::int_lit;
  if (L[0] == '"' || L[0] == '`')
    K = tok::string_lit;
  else if (L[0] == '\'')
    K = tok::char_lit;
  else if (L.back() == 'i')
    K = tok::imag_lit;
  else if (!L.starts_with("0x") && L.find_first_of("./Ee") != StringRef::npos)
    K = tok::float_lit;
  std::string Copy = L.str();
  if (K == tok::string_lit)
    for (char &C : Copy)
      if (C == '_')
        C = ' ';
  return lit(Copy, K);
}

bool eql(const ConstValue &X, const ConstValue &Y) {
  if (X.isUnknown() || Y.isUnknown())
    return X.isUnknown() == Y.isUnknown();
  return ConstValue::compare(X, tok::eql, Y);
}

tok::Kind opKind(StringRef S) {
  static const std::pair<const char *, tok::Kind> Tab[] = {
      {"!", tok::not_}, {"+", tok::add},  {"-", tok::sub},
      {"*", tok::mul},  {"/", tok::quo},  {"%", tok::rem},
      {"<<", tok::shl}, {">>", tok::shr}, {"&", tok::and_},
      {"|", tok::or_},  {"^", tok::xor_}, {"&^", tok::and_not},
      {"==", tok::eql}, {"!=", tok::neq}, {"<", tok::lss},
      {"<=", tok::leq}, {">", tok::gtr},  {">=", tok::geq}};
  for (auto &E : Tab)
    if (S == E.first)
      return E.second;
  ADD_FAILURE() << "unknown operator " << S.str();
  return tok::eof;
}

void testNumbers(tok::Kind K, llvm::ArrayRef<const char *> Tests) {
  for (const char *T : Tests) {
    StringRef Test = T;
    auto [A, B] = Test.split(" = ");
    ConstValue X = lit(A, K), Y;
    if (B != "?") {
      auto [N, D] = B.split('/');
      if (K == tok::float_lit && B.contains('/'))
        Y = ConstValue::binaryOp(lit(N, tok::int_lit), tok::quo,
                                 lit(D, tok::int_lit));
      else
        Y = lit(B, K);
      ASSERT_FALSE(Y.isUnknown()) << "invalid test case: " << Test.str();
    }
    ASSERT_EQ(X.kind(), Y.kind()) << Test.str();
    if (Y.isUnknown())
      continue;
    EXPECT_TRUE(ConstValue::compare(X, tok::eql, Y))
        << Test.str() << ": " << X.str() << " != " << Y.str();
  }
}

} // namespace

TEST(Constant, IntLiterals) { testNumbers(tok::int_lit, intTests); }
TEST(Constant, FloatLiterals) { testNumbers(tok::float_lit, floatTests); }
TEST(Constant, ImagLiterals) { testNumbers(tok::imag_lit, imagTests); }

TEST(Constant, Ops) {
  for (const char *T : opTests) {
    SmallVector<StringRef, 8> A;
    StringRef(T).split(A, ' ');
    unsigned I = 0;
    bool Unary = A.size() == 4;
    ASSERT_TRUE(Unary || A.size() == 5) << T;
    ConstValue X;
    if (!Unary) {
      X = val(A[0]);
      I = 1;
    }
    tok::Kind Op = opKind(A[I]);
    ConstValue Y = val(A[I + 1]);
    ConstValue Got;
    if (Unary) {
      Got = ConstValue::unaryOp(Op, Y, 0);
    } else if (Op >= tok::eql && Op <= tok::geq) {
      Got = ConstValue::makeBool(ConstValue::compare(X, Op, Y));
    } else if (Op == tok::shl || Op == tok::shr) {
      int64_t S = 0;
      Y.fitsInt64(S);
      Got = ConstValue::shift(X, Op, static_cast<unsigned>(S));
    } else {
      Got = ConstValue::binaryOp(X, Op, Y);
    }
    ConstValue Want = val(A[I + 3]);
    EXPECT_TRUE(eql(Got, Want))
        << T << ": got " << Got.str() << " want " << Want.str();
  }
}

TEST(Constant, Precision) {
  // 256-bit integers and more.
  ConstValue One = ConstValue::makeInt64(1);
  ConstValue Big = ConstValue::shift(One, tok::shl, 300);
  EXPECT_EQ(Big.bitLen(), 301u);
  ConstValue Minus = ConstValue::binaryOp(Big, tok::sub, One);
  EXPECT_EQ(Minus.bitLen(), 300u);
  EXPECT_FALSE(ConstValue::compare(Big, tok::eql, Minus));
  // 1<<300 in a float keeps all of its bits and 2^-300 is exact.
  ConstValue F = Big.toFloat();
  ConstValue Inv = ConstValue::binaryOp(One.toFloat(), tok::quo, F);
  ConstValue Back = ConstValue::binaryOp(Inv, tok::mul, F);
  EXPECT_TRUE(ConstValue::compare(Back, tok::eql, One));
  // Untyped float constants are exact rationals, as in go/constant: 1/3*3
  // is 1 and 0.3 - 0.1 - 0.2 is 0.
  ConstValue Third =
      ConstValue::binaryOp(One, tok::quo, ConstValue::makeInt64(3));
  ConstValue Prod =
      ConstValue::binaryOp(Third, tok::mul, ConstValue::makeInt64(3));
  EXPECT_TRUE(ConstValue::compare(Prod, tok::eql, One)) << Prod.str();
  ConstValue P3 = lit("0.3", tok::float_lit), P1 = lit("0.1", tok::float_lit),
             P2 = lit("0.2", tok::float_lit);
  ConstValue Rest = ConstValue::binaryOp(ConstValue::binaryOp(P3, tok::sub, P1),
                                         tok::sub, P2);
  EXPECT_EQ(Rest.sign(), 0) << Rest.str();
  EXPECT_TRUE(ConstValue::compare(ConstValue::binaryOp(P1, tok::add, P2),
                                  tok::eql, P3));
  EXPECT_TRUE(ConstValue::compare(
      ConstValue::binaryOp(lit("1e-400", tok::float_lit), tok::mul,
                           lit("1e400", tok::float_lit)),
      tok::eql, One));
  // Past the rational limit the value is a 512-bit float.
  ConstValue Huge = lit("1e5000", tok::float_lit);
  EXPECT_FALSE(Huge.floatVal().isRat());
  EXPECT_EQ(Huge.str(), "1e+5000");
  // go/constant prints 6 significant digits, also outside the double range,
  // and more digits for a non-integer that %.6g would show as an integer.
  EXPECT_EQ(lit("1.23456789e5000", tok::float_lit).str(), "1.23457e+5000");
  EXPECT_EQ(lit("-1.23456789e-5000", tok::float_lit).str(), "-1.23457e-5000");
  EXPECT_EQ(lit("1000000.5", tok::float_lit).str(), "1.0000005e+06");
}

TEST(Constant, HugeExponents) {
  // The exponent range of big.Float (2^31): 1e100000000 is a finite float
  // that prints at once (10^n in 512 bits, not as an exact integer) and does
  // not convert to an integer: no integer type holds it, and the exact value
  // would take megabytes (`var i int = 1e100000000` is "truncated", as in Go).
  ConstValue X = lit("1e100000000", tok::float_lit);
  EXPECT_EQ(X.kind(), ConstValue::Float);
  EXPECT_EQ(X.str(), "1e+100000000");
  EXPECT_EQ(X.toInt().kind(), ConstValue::Unknown);
  EXPECT_EQ(lit("1e-100000000", tok::float_lit).str(), "1e-100000000");
  // A product past the range is infinite, which go/constant makes unknown.
  ConstValue Y = ConstValue::binaryOp(X, tok::mul, X);
  for (int I = 0; I < 30 && Y.kind() == ConstValue::Float; ++I)
    Y = ConstValue::binaryOp(Y, tok::mul, Y);
  EXPECT_EQ(Y.kind(), ConstValue::Unknown);
  // 1e2000 is still exact enough to be an integer.
  EXPECT_EQ(lit("1e2000", tok::float_lit).toInt().kind(), ConstValue::Int);
}

TEST(Constant, RoundingToFloat32AndFloat64) {
  // Correctly rounded once, also for float32 (no double rounding through
  // float64) and for subnormals.
  EXPECT_EQ(lit("0.1", tok::float_lit).float64(), 0.1);
  EXPECT_EQ(lit("0.1", tok::float_lit).float32(), 0.1f);
  EXPECT_EQ(lit("1e-45", tok::float_lit).float32(), 1e-45f);
  EXPECT_EQ(lit("4.9e-324", tok::float_lit).float64(), 4.9e-324);
  // 1 + 2^-24 + 2^-60 rounds up in float32; through float64 it would be the
  // tie 1 + 2^-24 and round to even (1).
  ConstValue X = ConstValue::binaryOp(
      ConstValue::binaryOp(lit("1", tok::float_lit), tok::add,
                           lit("0x1p-24", tok::float_lit)),
      tok::add, lit("0x1p-60", tok::float_lit));
  EXPECT_EQ(X.float32(), 1.0f + 0x1p-23f);
  EXPECT_TRUE(std::isinf(lit("3.5e38", tok::float_lit).float32()));
  EXPECT_FALSE(std::isinf(lit("3.4e38", tok::float_lit).float32()));
}

TEST(Constant, Strings) {
  EXPECT_EQ(val("\"foo\"").str(), "\"foo\"");
  EXPECT_EQ(lit("1e9999", tok::float_lit).str(), "1e+9999");
  EXPECT_EQ(lit("2.1", tok::float_lit).str(), "2.1");
  EXPECT_EQ(lit("10i", tok::imag_lit).str(), "(0 + 10i)");
  EXPECT_EQ(lit("'a'", tok::char_lit).str(), "97");
  EXPECT_EQ(lit("\"a\\tb\\u00e9\"", tok::string_lit).stringVal(),
            "a\tb\xc3\xa9");
  std::string Long = "\"" + std::string(68, 'x') + "xxx\"";
  EXPECT_EQ(lit(Long, tok::string_lit).str(),
            "\"" + std::string(68, 'x') + "...");
}
