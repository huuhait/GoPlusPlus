//===- UnicodeTest.cpp - UTF-8 decoding and character classes -------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Unicode.h"

#include "gtest/gtest.h"

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

} // namespace
