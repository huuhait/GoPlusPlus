//===- UnicodeTest.cpp - UTF-8 decoding and character classes -------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Unicode.h"

#include "gtest/gtest.h"

#include <cstdio>
#include <string>

using namespace gopp;

namespace {

struct Decoded {
  uint32_t CP;
  unsigned Len;
  bool Invalid;
};

Decoded dec(const std::string &S) {
  Decoded D{};
  D.Len = unicode::decodeUTF8(S.data(), S.data() + S.size(), D.CP, D.Invalid);
  return D;
}

/// The scalar value CP in UTF-8.
std::string utf8(uint32_t CP) {
  std::string S;
  if (CP < 0x80) {
    S += static_cast<char>(CP);
  } else if (CP < 0x800) {
    S += static_cast<char>(0xC0 | (CP >> 6));
    S += static_cast<char>(0x80 | (CP & 0x3F));
  } else if (CP < 0x10000) {
    S += static_cast<char>(0xE0 | (CP >> 12));
    S += static_cast<char>(0x80 | ((CP >> 6) & 0x3F));
    S += static_cast<char>(0x80 | (CP & 0x3F));
  } else {
    S += static_cast<char>(0xF0 | (CP >> 18));
    S += static_cast<char>(0x80 | ((CP >> 12) & 0x3F));
    S += static_cast<char>(0x80 | ((CP >> 6) & 0x3F));
    S += static_cast<char>(0x80 | (CP & 0x3F));
  }
  return S;
}

// What Go's strconv.Quote does with each code point, written by Go
// (tools/gen-unicode -quote): ranges in order that cover every code point.
// Kind 'P': as it is; 'E': a backslash escape (\a ... \v, \", \\); 'X':
// \xNN; 'u': \uXXXX; 'U': \UXXXXXXXX; '-': a surrogate (no UTF-8 holds it).
struct GoQuoteRange {
  uint32_t Lo, Hi;
  char Kind;
};
#include "GoQuote.inc"

TEST(Unicode, DecodesValidSequences) {
  EXPECT_EQ(dec("a").CP, 0x61u);
  EXPECT_EQ(dec("\xC3\xA9").CP, 0xE9u);
  EXPECT_EQ(dec("\xE2\x82\xAC").CP, 0x20ACu);
  EXPECT_EQ(dec("\xF0\x9F\x98\x80").CP, 0x1F600u);
  EXPECT_EQ(dec("\xF0\x9F\x98\x80").Len, 4u);
  EXPECT_FALSE(dec("\xF4\x8F\xBF\xBF").Invalid); // U+10FFFF
  Decoded R = dec("\xEF\xBF\xBD");               // a real U+FFFD is not invalid
  EXPECT_EQ(R.CP, unicode::ReplacementChar);
  EXPECT_FALSE(R.Invalid);
}

TEST(Unicode, RejectsInvalidSequencesLikeGo) {
  const char *Bad[] = {
      "\x80",             // continuation byte alone
      "\xBF",             //
      "\xC0\x80",         // overlong NUL
      "\xC1\xBF",         // overlong
      "\xE0\x80\x80",     // overlong
      "\xF0\x80\x80\x80", // overlong
      "\xED\xA0\x80",     // surrogate U+D800
      "\xED\xBF\xBF",     // surrogate U+DFFF
      "\xF4\x90\x80\x80", // > U+10FFFF
      "\xF5\x80\x80\x80", //
      "\xFF",             //
      "\xFE",             //
      "\xC3",             // truncated
      "\xE2\x82",         // truncated
      "\xF0\x9F\x98",     // truncated
      "\xC3\x28",         // bad continuation
  };
  for (const char *B : Bad) {
    std::string S(B);
    Decoded D = dec(S);
    EXPECT_TRUE(D.Invalid) << "case " << (int)(unsigned char)B[0];
    EXPECT_EQ(D.Len, 1u);
    EXPECT_EQ(D.CP, unicode::ReplacementChar);
  }
}

TEST(Unicode, EveryScalarRoundTrips) {
  for (uint32_t CP = 0; CP <= unicode::MaxRune; CP += (CP < 0x10000 ? 1 : 97)) {
    if (CP >= 0xD800 && CP <= 0xDFFF)
      continue;
    std::string S = utf8(CP);
    Decoded D = dec(S);
    ASSERT_EQ(D.CP, CP);
    ASSERT_EQ(D.Len, S.size());
    ASSERT_FALSE(D.Invalid);
  }
}

TEST(Unicode, Letters) {
  EXPECT_TRUE(unicode::isLetter('a'));
  EXPECT_TRUE(unicode::isLetter('Z'));
  EXPECT_TRUE(unicode::isLetter('_'));
  EXPECT_FALSE(unicode::isLetter('0'));
  EXPECT_FALSE(unicode::isLetter('$'));
  EXPECT_FALSE(unicode::isLetter(' '));
  EXPECT_TRUE(unicode::isLetter(0xE9));    // e acute
  EXPECT_TRUE(unicode::isLetter(0x4E16));  // CJK
  EXPECT_TRUE(unicode::isLetter(0x3B1));   // alpha
  EXPECT_TRUE(unicode::isLetter(0x1D7C));  // letter in a high plane block
  EXPECT_FALSE(unicode::isLetter(0xD7));   // multiplication sign
  EXPECT_FALSE(unicode::isLetter(0x2018)); // curly quote
  EXPECT_FALSE(unicode::isLetter(0x1F600));
  EXPECT_FALSE(unicode::isLetter(0xFFFD));
  EXPECT_FALSE(unicode::isLetter(unicode::MaxRune));
}

TEST(Unicode, Digits) {
  EXPECT_TRUE(unicode::isDigit('0'));
  EXPECT_TRUE(unicode::isDigit('9'));
  EXPECT_FALSE(unicode::isDigit('a'));
  EXPECT_TRUE(unicode::isDigit(0x661));   // Arabic-Indic one
  EXPECT_TRUE(unicode::isDigit(0x966));   // Devanagari zero
  EXPECT_FALSE(unicode::isDigit(0xB2));   // superscript two is No, not Nd
  EXPECT_FALSE(unicode::isDigit(0x2160)); // Roman numeral
}

TEST(Unicode, Printable) {
  // strconv.IsPrint: the ASCII space and no other space; no control, format
  // character or unassigned code point.
  EXPECT_TRUE(unicode::isPrint(' '));
  EXPECT_TRUE(unicode::isPrint('~'));
  EXPECT_FALSE(unicode::isPrint(0x7F));
  EXPECT_FALSE(unicode::isPrint(0x85));
  EXPECT_FALSE(unicode::isPrint(0xA0)); // no-break space
  EXPECT_TRUE(unicode::isPrint(0xA1));
  EXPECT_FALSE(unicode::isPrint(0xAD)); // soft hyphen
  EXPECT_TRUE(unicode::isPrint(0xFC));
  EXPECT_FALSE(unicode::isPrint(0x200B)); // zero-width space
  EXPECT_FALSE(unicode::isPrint(0x2028));
  EXPECT_TRUE(unicode::isPrint(0xFFFD));
  EXPECT_TRUE(unicode::isPrint(0x1F600));
  EXPECT_FALSE(unicode::isPrint(0xE0001));
  EXPECT_FALSE(unicode::isPrint(unicode::MaxRune));
}

TEST(Unicode, QuoteAndIsPrintAgreeWithGoOnEveryCodePoint) {
  // GoQuote.inc says what Go's strconv.Quote does with each code point: each
  // one is quoted here and compared, and isPrint (strconv.IsPrint, which
  // Quote asks) with it.
  static const char Named[] = "abtnvfr"; // \a (U+0007) ... \r (U+000D)
  uint32_t Next = 0;
  char Buf[16];
  for (const GoQuoteRange &R : GoQuoteRanges) {
    ASSERT_EQ(R.Lo, Next) << "the ranges cover every code point in order";
    ASSERT_LE(R.Lo, R.Hi);
    for (uint32_t CP = R.Lo; CP <= R.Hi; ++CP) {
      if (R.Kind == '-') {
        ASSERT_TRUE(CP >= 0xD800 && CP <= 0xDFFF) << std::hex << CP;
        continue;
      }
      std::string S = utf8(CP), Want;
      const unsigned V = CP;
      switch (R.Kind) {
      case 'P':
        Want = S;
        break;
      case 'E':
        ASSERT_TRUE(CP == '"' || CP == '\\' || (CP >= 7 && CP <= 13)) << CP;
        Want = "\\";
        Want += CP >= 7 && CP <= 13 ? Named[CP - 7] : static_cast<char>(CP);
        break;
      case 'X':
        std::snprintf(Buf, sizeof Buf, "\\x%02x", V);
        Want = Buf;
        break;
      case 'u':
        std::snprintf(Buf, sizeof Buf, "\\u%04x", V);
        Want = Buf;
        break;
      case 'U':
        std::snprintf(Buf, sizeof Buf, "\\U%08x", V);
        Want = Buf;
        break;
      default:
        FAIL() << "kind " << R.Kind;
      }
      ASSERT_EQ(unicode::quote(S), "\"" + Want + "\"") << std::hex << CP;
      ASSERT_EQ(unicode::isPrint(CP), R.Kind == 'P' || CP == '"' || CP == '\\')
          << std::hex << CP;
    }
    Next = R.Hi + 1;
  }
  EXPECT_EQ(Next, unicode::MaxRune + 1);
}

TEST(Unicode, QuoteEscapesEachByteThatIsNotUTF8) {
  // strconv.Quote's own outputs: a byte that is not part of valid UTF-8 is
  // \xNN, one byte at a time, also where the bytes would make a surrogate,
  // an overlong form or a rune above U+10FFFF; a real U+FFFD stays as it is.
  // (The tag of review R9: a no-break space, a zero-width space, a byte
  // 0xff, and a printable rune.)
  struct {
    std::string In;
    const char *Want;
  } Cases[] = {
      {"", R"("")"},
      {"x\xffq", R"("x\xffq")"},
      {"\xed\xa0\x80", R"("\xed\xa0\x80")"},
      {"\xe2\x82\xc3\xa9", "\"\\xe2\\x82\xc3\xa9\""},
      {"\xc0\x80", R"("\xc0\x80")"},
      {"\xf4\x90\x80\x80", R"("\xf4\x90\x80\x80")"},
      {"\xef\xbf\xbd", "\"\xef\xbf\xbd\""},
      {std::string("a\0b\x7f", 4), R"("a\x00b\x7f")"},
      {"x\xc2\xa0y\xe2\x80\x8bz\xffq \xc3\xbc",
       "\"x\\u00a0y\\u200bz\\xffq \xc3\xbc\""},
      {"\xc2\xad\xc2\x85\xe2\x80\xa8\xf3\xa0\x80\x81\xf0\x9f\x98\x80",
       "\"\\u00ad\\u0085\\u2028\\U000e0001\xf0\x9f\x98\x80\""},
      {"\a\b\f\n\r\t\v\"\\", R"("\a\b\f\n\r\t\v\"\\")"},
  };
  for (auto &C : Cases)
    EXPECT_EQ(unicode::quote(C.In), C.Want);
}

} // namespace
