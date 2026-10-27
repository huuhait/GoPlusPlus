//===- LexerExtrasTest.cpp - Go++ additions and robustness of the lexer ---===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gtest/gtest.h"

#include <random>
#include <sstream>

using namespace gopp;
using namespace gopp::test;

namespace {

std::string kinds(llvm::StringRef Text) {
  TestSource S(Text);
  std::string Out;
  for (const LexedToken &T : lexAll(S)) {
    if (!Out.empty())
      Out += ' ';
    Out += tok::getName(T.Tok.Kind);
  }
  return Out;
}

TEST(LexerExtras, HashAndAtAreTokens) {
  EXPECT_EQ(kinds("#Name(x)"), "hash ident l_paren ident r_paren semi eof");
  EXPECT_EQ(kinds("@Name"), "at ident semi eof");
  EXPECT_EQ(kinds("#a.b"), "hash ident period ident semi eof");
}

TEST(LexerExtras, MacroIsTheOnlyNewKeyword) {
  // Like Go's "func", the keyword does not end a statement.
  EXPECT_EQ(kinds("macro"), "kw_macro eof");
  // ref and move are ordinary identifiers.
  EXPECT_EQ(kinds("ref move unique"), "ident ident ident semi eof");
  EXPECT_EQ(kinds("Macro"), "ident semi eof");
}

TEST(LexerExtras, HashAndAtDoNotInsertSemicolons) {
  // A newline after '#' or '@' is not a statement end.
  EXPECT_EQ(kinds("#\nx"), "hash ident semi eof");
  EXPECT_EQ(kinds("x @\n"), "ident at eof");
}

TEST(LexerExtras, BangForms) {
  EXPECT_EQ(kinds("f!(x)"), "ident not_ l_paren ident r_paren semi eof");
  EXPECT_EQ(kinds("a != b"), "ident neq ident semi eof");
  EXPECT_EQ(kinds("f! = 1"), "ident not_ assign int_lit semi eof");
}

TEST(LexerExtras, BOMOnlyAtStart) {
  {
    TestSource S("\xEF\xBB\xBFx");
    auto T = lexAll(S);
    EXPECT_EQ(S.numErrors(), 0u);
    EXPECT_EQ(T[0].Text, "x");
  }
  {
    TestSource S("x \xEF\xBB\xBF y");
    lexAll(S);
    EXPECT_EQ(S.numErrors(), 1u);
  }
}

TEST(LexerExtras, NulAndInvalidUTF8AreReported) {
  {
    TestSource S(std::string("a\0b", 3));
    lexAll(S);
    EXPECT_GE(S.numErrors(), 1u);
  }
  {
    TestSource S("var \xC3 = 1");
    lexAll(S);
    EXPECT_GE(S.numErrors(), 1u);
  }
  {
    // Inside a string and a comment, bad bytes are reported but lexing goes on.
    TestSource S("\"a\xFF\" // \xFE\n/* \xFD */ x");
    auto T = lexAll(S);
    EXPECT_GE(S.numErrors(), 3u);
    EXPECT_EQ(T.back().Tok.Kind, tok::eof);
  }
}

TEST(LexerExtras, UnterminatedLiteralsAreReported) {
  for (const char *Src :
       {"\"abc", "`abc", "'a", "/* x", "'", "\"\\", "'\\x", "0x", "1e", "0b2",
        "1_", "0o8", "\"\\u12\"", "'ab'", "''"}) {
    SCOPED_TRACE(Src);
    TestSource S(Src);
    auto T = lexAll(S);
    EXPECT_GE(S.numErrors(), 1u);
    EXPECT_EQ(T.back().Tok.Kind, tok::eof);
  }
}

TEST(LexerExtras, OverflowingNumbersLexWithoutValueChecks) {
  // The lexer does not evaluate literals, so any digit run is one token.
  std::string Big(100000, '9');
  TestSource S(Big);
  auto T = lexAll(S);
  EXPECT_EQ(S.numErrors(), 0u);
  EXPECT_EQ(T[0].Tok.Kind, tok::int_lit);
  EXPECT_EQ(T[0].Text.size(), Big.size());
  TestSource S2("1e99999999999999999999 0x1p-99999999999999999999");
  lexAll(S2);
  EXPECT_EQ(S2.numErrors(), 0u);
}

TEST(LexerExtras, HugeInputs) {
  // 4 MiB of identifiers and a long single line.
  std::string Text;
  for (int I = 0; I < 400000; ++I)
    Text += "abcdefghi ";
  TestSource S(Text);
  auto T = lexAll(S);
  EXPECT_EQ(T.size(), 400000u + 2); // idents, the final semicolon and eof
  EXPECT_EQ(S.numErrors(), 0u);
}

TEST(LexerExtras, EmptyAndWhitespaceOnly) {
  EXPECT_EQ(kinds(""), "eof");
  EXPECT_EQ(kinds("  \t\r\n\n"), "eof");
  EXPECT_EQ(kinds("// c"), "eof");
}

TEST(LexerExtras, ErrorLimitStopsReporting) {
  TestSource S(std::string(100, '\x01'), "test.gopp", /*ErrorLimit=*/3);
  lexAll(S);
  EXPECT_TRUE(S.Diags.limitReached());
  // 3 errors plus the single "too many errors".
  EXPECT_EQ(S.Consumer.Diags.size(), 4u);
}

TEST(LexerExtras, KeepCommentsOption) {
  TestSource S("a // x\n/* y */ b");
  LexerOptions O;
  O.KeepComments = true;
  auto T = lexAll(S, O);
  unsigned Comments = 0;
  for (auto &L : T)
    Comments += L.Tok.is(tok::comment);
  EXPECT_EQ(Comments, 2u);
}

TEST(LexerExtras, DirectiveCommentIsAnOrdinaryComment) {
  // LEX-11: //gopp:build is a comment to the lexer; the build tool reads it.
  TestSource S("//gopp:build linux && debug\npackage main\n");
  LexerOptions O;
  O.KeepComments = true;
  auto T = lexAll(S, O);
  ASSERT_GE(T.size(), 2u);
  EXPECT_TRUE(T[0].Tok.is(tok::comment));
  EXPECT_EQ(T[0].Text, "//gopp:build linux && debug");
  EXPECT_TRUE(T[1].Tok.is(tok::kw_package));
  EXPECT_EQ(S.numErrors(), 0u);
  EXPECT_EQ(kinds("//gopp:build linux\npackage main\n"),
            "kw_package ident semi eof");
}

TEST(LexerExtras, RandomBytesNeverCrashAndAlwaysTerminate) {
  std::mt19937 Rng(12345);
  // A mix that reaches every lexer state: operators, quotes, digits, bytes.
  const char Alphabet[] =
      "abc_09 \t\n\r\"`'\\/*+-<>=!&|^%.,;:()[]{}#@~xXeEpP_\xE2\x80\x9C\xFF\x80";
  for (int Iter = 0; Iter < 2000; ++Iter) {
    std::string Text;
    unsigned Len = Rng() % 64;
    for (unsigned I = 0; I < Len; ++I)
      Text += Alphabet[Rng() % (sizeof(Alphabet) - 1)];
    if (Iter % 7 == 0)
      Text += static_cast<char>(Rng() & 0xFF);
    TestSource S(Text);
    auto T = lexAll(S);
    ASSERT_EQ(T.back().Tok.Kind, tok::eof);
    // Tokens come in increasing position and stay inside the buffer.
    unsigned Prev = 0;
    for (auto &L : T) {
      ASSERT_GE(L.Offset, Prev) << Text;
      ASSERT_LE(L.Offset, Text.size());
      Prev = L.Offset;
    }
  }
}

TEST(LexerExtras, TokenTextCoversTheSource) {
  // Concatenating the non-semicolon token texts (comments kept) reproduces the
  // source minus whitespace, for a well-formed input.
  const std::string Src = "x := a.b[1] + 0x1F // c\n/* d */ y <<= \"s\\n\"\n";
  TestSource S(Src);
  LexerOptions O;
  O.KeepComments = true;
  std::string Joined;
  for (auto &L : lexAll(S, O))
    if (!L.Tok.is(tok::semi) && !L.Tok.is(tok::eof))
      for (char C : L.Text)
        if (C != ' ')
          Joined += C;
  std::string Expected;
  for (char C : Src)
    if (C != ' ' && C != '\n')
      Expected += C;
  EXPECT_EQ(S.numErrors(), 0u);
  // The // comment text excludes its newline, so both sides agree.
  EXPECT_EQ(Joined, Expected);
}

} // namespace
