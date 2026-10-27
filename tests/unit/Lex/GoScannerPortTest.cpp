//===- GoScannerPortTest.cpp - Go's go/scanner tests, ported --------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Test cases ported from Go's src/go/scanner/scanner_test.go:
//   Copyright 2009 The Go Authors. All rights reserved.
//   Use of this source code is governed by a BSD-style license (see NOTICE).
// Changes: '#' and '@' are valid tokens in Go++; //line directives and the
// CR-stripping of literal text are not part of the lexer.
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gtest/gtest.h"

#include <map>
#include <sstream>

using namespace gopp;
using namespace gopp::test;

namespace {

// Go's token strings, used by the ported tables.
std::string goTokString(const LexedToken &T) {
  switch (T.Tok.Kind) {
  case tok::ident:
    return "IDENT";
  case tok::int_lit:
    return "INT";
  case tok::float_lit:
    return "FLOAT";
  case tok::imag_lit:
    return "IMAG";
  case tok::char_lit:
    return "CHAR";
  case tok::string_lit:
    return "STRING";
  case tok::comment:
    return "COMMENT";
  case tok::eof:
    return "EOF";
  case tok::illegal:
    return "ILLEGAL";
  default:
    return tok::getSpelling(T.Tok.Kind);
  }
}

enum Class { Special, Literal, Operator, Keyword };

Class tokenClass(tok::Kind K) {
  if (tok::isLiteral(K))
    return Literal;
  if (tok::isKeyword(K))
    return Keyword;
  if (tok::isPunct(K))
    return Operator;
  return Special;
}

struct Elt {
  tok::Kind Kind;
  std::string Lit;
  Class Cls;
};

const std::vector<Elt> &tokens() {
  static const std::vector<Elt> Tokens = {
      // Special tokens
      {tok::comment, "/* a comment */", Special},
      {tok::comment, "// a comment \n", Special},
      {tok::comment, "/*\r*/", Special},
      {tok::comment, "/**\r/*/", Special}, // issue 11151
      {tok::comment, "/**\r\r/*/", Special},
      {tok::comment, "//\r\n", Special},

      // Identifiers and basic type literals
      {tok::ident, "foobar", Literal},
      {tok::ident, "a۰۱۸", Literal},
      {tok::ident, "foo६४", Literal},
      {tok::ident, "bar９８７６", Literal},
      {tok::ident, "ŝ", Literal},    // was bug (issue 4000)
      {tok::ident, "ŝfoo", Literal}, // was bug (issue 4000)
      {tok::int_lit, "0", Literal},
      {tok::int_lit, "1", Literal},
      {tok::int_lit, "123456789012345678890", Literal},
      {tok::int_lit, "01234567", Literal},
      {tok::int_lit, "0xcafebabe", Literal},
      {tok::float_lit, "0.", Literal},
      {tok::float_lit, ".0", Literal},
      {tok::float_lit, "3.14159265", Literal},
      {tok::float_lit, "1e0", Literal},
      {tok::float_lit, "1e+100", Literal},
      {tok::float_lit, "1e-100", Literal},
      {tok::float_lit, "2.71828e-1000", Literal},
      {tok::imag_lit, "0i", Literal},
      {tok::imag_lit, "1i", Literal},
      {tok::imag_lit, "012345678901234567889i", Literal},
      {tok::imag_lit, "123456789012345678890i", Literal},
      {tok::imag_lit, "0.i", Literal},
      {tok::imag_lit, ".0i", Literal},
      {tok::imag_lit, "3.14159265i", Literal},
      {tok::imag_lit, "1e0i", Literal},
      {tok::imag_lit, "1e+100i", Literal},
      {tok::imag_lit, "1e-100i", Literal},
      {tok::imag_lit, "2.71828e-1000i", Literal},
      {tok::char_lit, "'a'", Literal},
      {tok::char_lit, "'\\000'", Literal},
      {tok::char_lit, "'\\xFF'", Literal},
      {tok::char_lit, "'\\uff16'", Literal},
      {tok::char_lit, "'\\U0000ff16'", Literal},
      {tok::string_lit, "`foobar`", Literal},
      {tok::string_lit, "`foo\n\t                        bar`", Literal},
      {tok::string_lit, "`\r`", Literal},
      {tok::string_lit, "`foo\r\nbar`", Literal},

      // Operators and delimiters
      {tok::add, "+", Operator},
      {tok::sub, "-", Operator},
      {tok::mul, "*", Operator},
      {tok::quo, "/", Operator},
      {tok::rem, "%", Operator},

      {tok::and_, "&", Operator},
      {tok::or_, "|", Operator},
      {tok::xor_, "^", Operator},
      {tok::shl, "<<", Operator},
      {tok::shr, ">>", Operator},
      {tok::and_not, "&^", Operator},

      {tok::add_assign, "+=", Operator},
      {tok::sub_assign, "-=", Operator},
      {tok::mul_assign, "*=", Operator},
      {tok::quo_assign, "/=", Operator},
      {tok::rem_assign, "%=", Operator},

      {tok::and_assign, "&=", Operator},
      {tok::or_assign, "|=", Operator},
      {tok::xor_assign, "^=", Operator},
      {tok::shl_assign, "<<=", Operator},
      {tok::shr_assign, ">>=", Operator},
      {tok::and_not_assign, "&^=", Operator},

      {tok::land, "&&", Operator},
      {tok::lor, "||", Operator},
      {tok::arrow, "<-", Operator},
      {tok::inc, "++", Operator},
      {tok::dec, "--", Operator},

      {tok::eql, "==", Operator},
      {tok::lss, "<", Operator},
      {tok::gtr, ">", Operator},
      {tok::assign, "=", Operator},
      {tok::not_, "!", Operator},

      {tok::neq, "!=", Operator},
      {tok::leq, "<=", Operator},
      {tok::geq, ">=", Operator},
      {tok::define, ":=", Operator},
      {tok::ellipsis, "...", Operator},

      {tok::l_paren, "(", Operator},
      {tok::l_brack, "[", Operator},
      {tok::l_brace, "{", Operator},
      {tok::comma, ",", Operator},
      {tok::period, ".", Operator},

      {tok::r_paren, ")", Operator},
      {tok::r_brack, "]", Operator},
      {tok::r_brace, "}", Operator},
      {tok::semi, ";", Operator},
      {tok::colon, ":", Operator},
      {tok::tilde, "~", Operator},

      // Go++ additions
      {tok::hash, "#", Operator},
      {tok::at, "@", Operator},

      // Keywords
      {tok::kw_break, "break", Keyword},
      {tok::kw_case, "case", Keyword},
      {tok::kw_chan, "chan", Keyword},
      {tok::kw_const, "const", Keyword},
      {tok::kw_continue, "continue", Keyword},

      {tok::kw_default, "default", Keyword},
      {tok::kw_defer, "defer", Keyword},
      {tok::kw_else, "else", Keyword},
      {tok::kw_fallthrough, "fallthrough", Keyword},
      {tok::kw_for, "for", Keyword},

      {tok::kw_func, "func", Keyword},
      {tok::kw_go, "go", Keyword},
      {tok::kw_goto, "goto", Keyword},
      {tok::kw_if, "if", Keyword},
      {tok::kw_import, "import", Keyword},

      {tok::kw_interface, "interface", Keyword},
      {tok::kw_map, "map", Keyword},
      {tok::kw_package, "package", Keyword},
      {tok::kw_range, "range", Keyword},
      {tok::kw_return, "return", Keyword},

      {tok::kw_select, "select", Keyword},
      {tok::kw_struct, "struct", Keyword},
      {tok::kw_switch, "switch", Keyword},
      {tok::kw_type, "type", Keyword},
      {tok::kw_var, "var", Keyword},
      {tok::kw_macro, "macro", Keyword},
  };
  return Tokens;
}

const std::string Whitespace = "  \t  \n\n\n"; // to separate tokens

unsigned newlineCount(const std::string &S) {
  return std::count(S.begin(), S.end(), '\n');
}

// Verify that lexing provides the correct results (TestScan).
TEST(GoScannerPort, Scan) {
  std::string Source;
  for (const Elt &E : tokens())
    Source += E.Lit + Whitespace;
  TestSource S(Source);
  LexerOptions Opts;
  Opts.KeepComments = true;
  Opts.InsertSemis = false;
  std::vector<LexedToken> Toks = lexAll(S, Opts);

  unsigned Offset = 0, Line = 1;
  ASSERT_EQ(Toks.size(), tokens().size() + 1);
  for (size_t I = 0; I < Toks.size(); ++I) {
    const LexedToken &T = Toks[I];
    SCOPED_TRACE("token " + std::to_string(I));
    PresumedLoc P = S.SM.getPresumedLoc(T.Tok.Loc);
    EXPECT_EQ(T.Offset, Offset) << T.Text;
    EXPECT_EQ(P.Line, Line) << T.Text;
    EXPECT_EQ(P.Column, 1u) << T.Text; // every token starts a line
    if (I == tokens().size()) {
      EXPECT_EQ(T.Tok.Kind, tok::eof);
      break;
    }
    const Elt &E = tokens()[I];
    EXPECT_EQ(T.Tok.Kind, E.Kind) << T.Text;
    EXPECT_EQ(tokenClass(T.Tok.Kind), E.Cls) << T.Text;
    std::string Want = E.Lit;
    if (E.Kind == tok::comment && Want[1] == '/') {
      // a //-comment does not contain its newline (nor the CR before it)
      Want.pop_back();
      if (!Want.empty() && Want.back() == '\r')
        Want.pop_back();
    }
    EXPECT_EQ(T.Text, Want);
    Offset += E.Lit.size() + Whitespace.size();
    Line += newlineCount(E.Lit) + newlineCount(Whitespace);
  }
  EXPECT_EQ(S.numErrors(), 0u) << ::testing::PrintToString(S.errors());
}

const std::pair<const char *, const char *> SemicolonTests[] = {
    {"", ""},
    {"\xEF\xBB\xBF;", ";"}, // first BOM is ignored
    {";", ";"},
    {"foo\n", "IDENT ;"},
    {"123\n", "INT ;"},
    {"1.2\n", "FLOAT ;"},
    {"'x'\n", "CHAR ;"},
    {"\"x\"\n", "STRING ;"},
    {"`x`\n", "STRING ;"},

    {"+\n", "+"},
    {"-\n", "-"},
    {"*\n", "*"},
    {"/\n", "/"},
    {"%\n", "%"},

    {"&\n", "&"},
    {"|\n", "|"},
    {"^\n", "^"},
    {"<<\n", "<<"},
    {">>\n", ">>"},
    {"&^\n", "&^"},

    {"+=\n", "+="},
    {"-=\n", "-="},
    {"*=\n", "*="},
    {"/=\n", "/="},
    {"%=\n", "%="},

    {"&=\n", "&="},
    {"|=\n", "|="},
    {"^=\n", "^="},
    {"<<=\n", "<<="},
    {">>=\n", ">>="},
    {"&^=\n", "&^="},

    {"&&\n", "&&"},
    {"||\n", "||"},
    {"<-\n", "<-"},
    {"++\n", "++ ;"},
    {"--\n", "-- ;"},

    {"==\n", "=="},
    {"<\n", "<"},
    {">\n", ">"},
    {"=\n", "="},
    {"!\n", "!"},

    {"!=\n", "!="},
    {"<=\n", "<="},
    {">=\n", ">="},
    {":=\n", ":="},
    {"...\n", "..."},

    {"(\n", "("},
    {"[\n", "["},
    {"{\n", "{"},
    {",\n", ","},
    {".\n", "."},

    {")\n", ") ;"},
    {"]\n", "] ;"},
    {"}\n", "} ;"},
    {";\n", ";"},
    {":\n", ":"},

    // Go++: '#' and '@' behave like operators (no semicolon after them)
    {"#\n", "#"},
    {"@\n", "@"},
    {"@Pinned\n", "@ IDENT ;"},
    {"@Derive(JSON)\n", "@ IDENT ( IDENT ) ;"},
    {"f!\n(x)\n", "IDENT ! ( IDENT ) ;"},
    {"call!()\n", "IDENT ! ( ) ;"},
    {"macro\n", "macro"},

    {"break\n", "break ;"},
    {"case\n", "case"},
    {"chan\n", "chan"},
    {"const\n", "const"},
    {"continue\n", "continue ;"},

    {"default\n", "default"},
    {"defer\n", "defer"},
    {"else\n", "else"},
    {"fallthrough\n", "fallthrough ;"},
    {"for\n", "for"},

    {"func\n", "func"},
    {"go\n", "go"},
    {"goto\n", "goto"},
    {"if\n", "if"},
    {"import\n", "import"},

    {"interface\n", "interface"},
    {"map\n", "map"},
    {"package\n", "package"},
    {"range\n", "range"},
    {"return\n", "return ;"},

    {"select\n", "select"},
    {"struct\n", "struct"},
    {"switch\n", "switch"},
    {"type\n", "type"},
    {"var\n", "var"},

    {"foo//comment\n", "IDENT COMMENT ;"},
    {"foo//comment", "IDENT COMMENT ;"},
    {"foo/*comment*/\n", "IDENT COMMENT ;"},
    {"foo/*\n*/", "IDENT COMMENT ;"},
    {"foo/*comment*/    \n", "IDENT COMMENT ;"},
    {"foo/*\n*/    ", "IDENT COMMENT ;"},

    {"foo    // comment\n", "IDENT COMMENT ;"},
    {"foo    // comment", "IDENT COMMENT ;"},
    {"foo    /*comment*/\n", "IDENT COMMENT ;"},
    {"foo    /*\n*/", "IDENT COMMENT ;"},
    {"foo    /*  */ /* \n */ bar/**/\n",
     "IDENT COMMENT COMMENT ; IDENT COMMENT ;"},
    {"foo    /*0*/ /*1*/ /*2*/\n", "IDENT COMMENT COMMENT COMMENT ;"},

    {"foo    /*comment*/    \n", "IDENT COMMENT ;"},
    {"foo    /*0*/ /*1*/ /*2*/    \n", "IDENT COMMENT COMMENT COMMENT ;"},
    {"foo\t/**/ /*-------------*/       /*----\n*/bar       /*  \n*/baa\n",
     "IDENT COMMENT COMMENT COMMENT ; IDENT COMMENT ; IDENT ;"},
    {"foo    /* an EOF terminates a line */", "IDENT COMMENT ;"},
    {"foo    /* an EOF terminates a line */ /*", "IDENT COMMENT COMMENT ;"},
    {"foo    /* an EOF terminates a line */ //", "IDENT COMMENT COMMENT ;"},

    {"package main\n\nfunc main() {\n\tif {\n\t\treturn /* */ }\n}\n",
     "package IDENT ; func IDENT ( ) { if { return COMMENT } ; } ;"},
    {"package main", "package IDENT ;"},
};

std::string replaceAll(std::string S, const std::string &From,
                       const std::string &To) {
  for (size_t P = 0; (P = S.find(From, P)) != std::string::npos;)
    S.replace(P, From.size(), To), P += To.size();
  return S;
}

void checkSemi(const std::string &Input, std::string Want, bool Comments) {
  if (!Comments) {
    Want = replaceAll(Want, "COMMENT ", "");
    Want = replaceAll(Want, " COMMENT", ""); // if at end
    Want = replaceAll(Want, "COMMENT", "");  // if sole token
  }
  TestSource S(Input);
  LexerOptions Opts;
  Opts.KeepComments = Comments;
  std::vector<LexedToken> Toks = lexAll(S, Opts);
  std::string Got;
  for (const LexedToken &T : Toks) {
    if (T.Tok.is(tok::eof))
      break;
    if (T.Tok.isAutoSemi()) {
      // Artificial semicolon: its position is EOF or that of a newline.
      EXPECT_TRUE(T.Offset == Input.size() || Input[T.Offset] == '\n')
          << "scanning <<" << Input << ">> got a semicolon at offset "
          << T.Offset;
    }
    if (!Got.empty())
      Got += ' ';
    Got += T.Tok.isAutoSemi() ? ";" : goTokString(T);
  }
  EXPECT_EQ(Got, Want) << "scanning <<" << Input << ">>";
}

TEST(GoScannerPort, Semicolons) {
  for (auto &[Input, Want] : SemicolonTests) {
    std::string In = Input;
    checkSemi(In, Want, false);
    checkSemi(In, Want, true);
    // if the input ended in newlines, the input must tokenize the same with or
    // without those newlines
    for (size_t I = In.size(); I-- > 0 && In[I] == '\n';) {
      checkSemi(In.substr(0, I), Want, false);
      checkSemi(In.substr(0, I), Want, true);
    }
  }
}

struct ErrCase {
  std::string Src;
  tok::Kind Kind;
  unsigned Pos;
  std::string Lit;
  std::string Err;
};

const std::vector<ErrCase> &errorCases() {
  static const std::vector<ErrCase> Cases = {
      {"\a", tok::illegal, 0, "", "illegal character U+0007"},
      {"$", tok::illegal, 0, "", "illegal character U+0024 '$'"},
      {"…", tok::illegal, 0, "", "illegal character U+2026 '…'"},
      {"..", tok::period, 0, ".", ""}, // two periods, not invalid token
      {"' '", tok::char_lit, 0, "' '", ""},
      {"''", tok::char_lit, 0, "''", "illegal rune literal"},
      {"'12'", tok::char_lit, 0, "'12'", "illegal rune literal"},
      {"'123'", tok::char_lit, 0, "'123'", "illegal rune literal"},
      {"'\\0'", tok::char_lit, 3, "'\\0'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\07'", tok::char_lit, 4, "'\\07'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\8'", tok::char_lit, 2, "'\\8'", "unknown escape sequence"},
      {"'\\08'", tok::char_lit, 3, "'\\08'",
       "illegal character U+0038 '8' in escape sequence"},
      {"'\\x'", tok::char_lit, 3, "'\\x'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\x0'", tok::char_lit, 4, "'\\x0'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\x0g'", tok::char_lit, 4, "'\\x0g'",
       "illegal character U+0067 'g' in escape sequence"},
      {"'\\u'", tok::char_lit, 3, "'\\u'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\u0'", tok::char_lit, 4, "'\\u0'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\u00'", tok::char_lit, 5, "'\\u00'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\u000'", tok::char_lit, 6, "'\\u000'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\u000", tok::char_lit, 6, "'\\u000",
       "escape sequence not terminated"},
      {"'\\u0000'", tok::char_lit, 0, "'\\u0000'", ""},
      {"'\\U'", tok::char_lit, 3, "'\\U'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\U0'", tok::char_lit, 4, "'\\U0'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\U00'", tok::char_lit, 5, "'\\U00'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\U000'", tok::char_lit, 6, "'\\U000'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\U0000'", tok::char_lit, 7, "'\\U0000'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\U00000'", tok::char_lit, 8, "'\\U00000'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\U000000'", tok::char_lit, 9, "'\\U000000'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\U0000000'", tok::char_lit, 10, "'\\U0000000'",
       "illegal character U+0027 ''' in escape sequence"},
      {"'\\U0000000", tok::char_lit, 10, "'\\U0000000",
       "escape sequence not terminated"},
      {"'\\U00000000'", tok::char_lit, 0, "'\\U00000000'", ""},
      {"'\\Uffffffff'", tok::char_lit, 2, "'\\Uffffffff'",
       "escape sequence is invalid Unicode code point"},
      {"'", tok::char_lit, 0, "'", "rune literal not terminated"},
      {"'\\", tok::char_lit, 2, "'\\", "escape sequence not terminated"},
      {"'\n", tok::char_lit, 0, "'", "rune literal not terminated"},
      {"'\n   ", tok::char_lit, 0, "'", "rune literal not terminated"},
      {"\"\"", tok::string_lit, 0, "\"\"", ""},
      {"\"abc", tok::string_lit, 0, "\"abc", "string literal not terminated"},
      {"\"abc\n", tok::string_lit, 0, "\"abc", "string literal not terminated"},
      {"\"abc\n   ", tok::string_lit, 0, "\"abc",
       "string literal not terminated"},
      {"``", tok::string_lit, 0, "``", ""},
      {"`", tok::string_lit, 0, "`", "raw string literal not terminated"},
      {"/**/", tok::comment, 0, "/**/", ""},
      {"/*", tok::comment, 0, "/*", "comment not terminated"},
      {"077", tok::int_lit, 0, "077", ""},
      {"078.", tok::float_lit, 0, "078.", ""},
      {"07801234567.", tok::float_lit, 0, "07801234567.", ""},
      {"078e0", tok::float_lit, 0, "078e0", ""},
      {"0E", tok::float_lit, 2, "0E", "exponent has no digits"}, // issue 17621
      {"078", tok::int_lit, 2, "078", "invalid digit '8' in octal literal"},
      {"07090000008", tok::int_lit, 3, "07090000008",
       "invalid digit '9' in octal literal"},
      {"0x", tok::int_lit, 2, "0x", "hexadecimal literal has no digits"},
      {std::string("\"abc\x00"
                   "def\"",
                   9),
       tok::string_lit, 4,
       std::string("\"abc\x00"
                   "def\"",
                   9),
       "illegal character NUL"},
      {"\"abc\x80"
       "def\"",
       tok::string_lit, 4,
       "\"abc\x80"
       "def\"",
       "illegal UTF-8 encoding"},
      // only the first BOM is ignored
      {"\xEF\xBB\xBF\xEF\xBB\xBF", tok::illegal, 3, "\xEF\xBB\xBF",
       "illegal byte order mark"},
      {"//\xEF\xBB\xBF", tok::comment, 2, "//\xEF\xBB\xBF",
       "illegal byte order mark"},
      {"'\xEF\xBB\xBF'", tok::char_lit, 1, "'\xEF\xBB\xBF'",
       "illegal byte order mark"},
      {"\"abc\xEF\xBB\xBF"
       "def\"",
       tok::string_lit, 4,
       "\"abc\xEF\xBB\xBF"
       "def\"",
       "illegal byte order mark"},
      {std::string("abc\x00"
                   "def",
                   7),
       tok::ident, 3, "abc", "illegal character NUL"},
      {std::string("abc\x00", 4), tok::ident, 3, "abc",
       "illegal character NUL"},
      {"\xE2\x80\x9C"
       "abc\xE2\x80\x9D",
       tok::illegal, 0, "",
       "curly quotation mark '\xE2\x80\x9C' (use neutral '\"')"},
  };
  return Cases;
}

TEST(GoScannerPort, ScanErrors) {
  for (const ErrCase &E : errorCases()) {
    SCOPED_TRACE(E.Src);
    TestSource S(E.Src);
    LexerOptions Opts;
    Opts.KeepComments = true;
    Opts.InsertSemis = false;
    Lexer L(S.SM, S.File, S.Diags, Opts);
    Token T = L.lex();
    EXPECT_EQ(T.Kind, E.Kind) << tok::getName(T.Kind);
    if (T.Kind != tok::illegal)
      EXPECT_EQ(L.getText(T).str(), E.Lit);
    std::vector<std::string> Errs = S.errors();
    if (E.Err.empty()) {
      EXPECT_TRUE(Errs.empty()) << ::testing::PrintToString(Errs);
    } else {
      ASSERT_EQ(Errs.size(), 1u) << ::testing::PrintToString(Errs);
      std::string Want = std::to_string(E.Pos) + ": " + E.Err;
      // P02.07: Go's "illegal rune literal" for 'ab' (a string in single
      // quotes) is the tailored message with a fix-it.
      if (E.Err == "illegal rune literal" &&
          Errs[0].find("single quotes make a rune") != std::string::npos)
        Want = std::to_string(E.Pos) +
               ": single quotes make a rune: use double quotes for a string";
      EXPECT_EQ(Errs[0], Want);
    }
  }
}

TEST(GoScannerPort, UTF16) {
  // BOM + "package p" encoded as UTF-16 BE and LE.
  const std::string Srcs[] = {
      std::string("\xfe\xff\x00p\x00"
                  "a\x00"
                  "c\x00k\x00"
                  "a\x00g\x00"
                  "e\x00 \x00p",
                  20),
      std::string("\xff\xfep\x00"
                  "a\x00"
                  "c\x00k\x00"
                  "a\x00g\x00"
                  "e\x00 \x00p\x00",
                  20),
  };
  for (const std::string &Src : Srcs) {
    TestSource S(Src);
    Lexer L(S.SM, S.File, S.Diags);
    L.lex();
    // two errors: one from the decoder, one from the scanner
    std::vector<std::string> Want = {
        "0: illegal UTF-8 encoding (got UTF-16)",
        "0: illegal character U+FFFD '\xEF\xBF\xBD'"};
    EXPECT_EQ(S.errors(), Want);
  }
}

// Verify that no comments show up when skipping comments (issue 10213).
TEST(GoScannerPort, Issue10213) {
  const char *Src = R"(
		var (
			A = 1 // foo
		)

		var (
			B = 2
			// foo
		)

		var C = 3 // foo

		var D = 4
		// foo

		func anycode() {
		// foo
		}
	)";
  TestSource S(Src);
  for (const LexedToken &T : lexAll(S)) {
    EXPECT_NE(T.Tok.Kind, tok::comment);
    if (!T.Text.empty()) {
      Class C = tokenClass(T.Tok.Kind);
      EXPECT_TRUE(C == Keyword || C == Literal || T.Tok.Kind == tok::eof ||
                  C == Operator)
          << T.Text;
    }
  }
}

TEST(GoScannerPort, Issue28112) {
  // stand-alone ".." immediately before EOF tests the EOF behavior
  TestSource S("... .. 0.. ..");
  std::vector<tok::Kind> Want = {tok::ellipsis,  tok::period, tok::period,
                                 tok::float_lit, tok::period, tok::period,
                                 tok::period,    tok::eof};
  std::vector<tok::Kind> Got;
  for (const LexedToken &T : lexAll(S)) {
    Got.push_back(T.Tok.Kind);
    if (tokenClass(T.Tok.Kind) == Literal)
      EXPECT_FALSE(T.Text.empty());
  }
  EXPECT_EQ(Got, Want);
}

struct NumCase {
  tok::Kind Kind;
  const char *Src;
  const char *Tokens;
  const char *Err;
};

TEST(GoScannerPort, Numbers) {
  const NumCase Cases[] = {
      // binaries
      {tok::int_lit, "0b0", "0b0", ""},
      {tok::int_lit, "0b1010", "0b1010", ""},
      {tok::int_lit, "0B1110", "0B1110", ""},

      {tok::int_lit, "0b", "0b", "binary literal has no digits"},
      {tok::int_lit, "0b0190", "0b0190", "invalid digit '9' in binary literal"},
      {tok::int_lit, "0b01a0", "0b01 a0", ""}, // only accept 0-9

      {tok::float_lit, "0b.", "0b.", "invalid radix point in binary literal"},
      {tok::float_lit, "0b.1", "0b.1", "invalid radix point in binary literal"},
      {tok::float_lit, "0b1.0", "0b1.0",
       "invalid radix point in binary literal"},
      {tok::float_lit, "0b1e10", "0b1e10",
       "'e' exponent requires decimal mantissa"},
      {tok::float_lit, "0b1P-1", "0b1P-1",
       "'P' exponent requires hexadecimal mantissa"},

      {tok::imag_lit, "0b10i", "0b10i", ""},
      {tok::imag_lit, "0b10.0i", "0b10.0i",
       "invalid radix point in binary literal"},

      // octals
      {tok::int_lit, "0o0", "0o0", ""},
      {tok::int_lit, "0o1234", "0o1234", ""},
      {tok::int_lit, "0O1234", "0O1234", ""},

      {tok::int_lit, "0o", "0o", "octal literal has no digits"},
      {tok::int_lit, "0o8123", "0o8123", "invalid digit '8' in octal literal"},
      {tok::int_lit, "0o1293", "0o1293", "invalid digit '9' in octal literal"},
      {tok::int_lit, "0o12a3", "0o12 a3", ""}, // only accept 0-9

      {tok::float_lit, "0o.", "0o.", "invalid radix point in octal literal"},
      {tok::float_lit, "0o.2", "0o.2", "invalid radix point in octal literal"},
      {tok::float_lit, "0o1.2", "0o1.2",
       "invalid radix point in octal literal"},
      {tok::float_lit, "0o1E+2", "0o1E+2",
       "'E' exponent requires decimal mantissa"},
      {tok::float_lit, "0o1p10", "0o1p10",
       "'p' exponent requires hexadecimal mantissa"},

      {tok::imag_lit, "0o10i", "0o10i", ""},
      {tok::imag_lit, "0o10e0i", "0o10e0i",
       "'e' exponent requires decimal mantissa"},

      // 0-octals
      {tok::int_lit, "0", "0", ""},
      {tok::int_lit, "0123", "0123", ""},

      {tok::int_lit, "08123", "08123", "invalid digit '8' in octal literal"},
      {tok::int_lit, "01293", "01293", "invalid digit '9' in octal literal"},
      {tok::int_lit, "0F.", "0 F .", ""}, // only accept 0-9
      {tok::int_lit, "0123F.", "0123 F .", ""},
      {tok::int_lit, "0123456x", "0123456 x", ""},

      // decimals
      {tok::int_lit, "1", "1", ""},
      {tok::int_lit, "1234", "1234", ""},

      {tok::int_lit, "1f", "1 f", ""}, // only accept 0-9

      {tok::imag_lit, "0i", "0i", ""},
      {tok::imag_lit, "0678i", "0678i", ""},

      // decimal floats
      {tok::float_lit, "0.", "0.", ""},
      {tok::float_lit, "123.", "123.", ""},
      {tok::float_lit, "0123.", "0123.", ""},

      {tok::float_lit, ".0", ".0", ""},
      {tok::float_lit, ".123", ".123", ""},
      {tok::float_lit, ".0123", ".0123", ""},

      {tok::float_lit, "0.0", "0.0", ""},
      {tok::float_lit, "123.123", "123.123", ""},
      {tok::float_lit, "0123.0123", "0123.0123", ""},

      {tok::float_lit, "0e0", "0e0", ""},
      {tok::float_lit, "123e+0", "123e+0", ""},
      {tok::float_lit, "0123E-1", "0123E-1", ""},

      {tok::float_lit, "0.e+1", "0.e+1", ""},
      {tok::float_lit, "123.E-10", "123.E-10", ""},
      {tok::float_lit, "0123.e123", "0123.e123", ""},

      {tok::float_lit, ".0e-1", ".0e-1", ""},
      {tok::float_lit, ".123E+10", ".123E+10", ""},
      {tok::float_lit, ".0123E123", ".0123E123", ""},

      {tok::float_lit, "0.0e1", "0.0e1", ""},
      {tok::float_lit, "123.123E-10", "123.123E-10", ""},
      {tok::float_lit, "0123.0123e+456", "0123.0123e+456", ""},

      {tok::float_lit, "0e", "0e", "exponent has no digits"},
      {tok::float_lit, "0E+", "0E+", "exponent has no digits"},
      {tok::float_lit, "1e+f", "1e+ f", "exponent has no digits"},
      {tok::float_lit, "0p0", "0p0",
       "'p' exponent requires hexadecimal mantissa"},
      {tok::float_lit, "1.0P-1", "1.0P-1",
       "'P' exponent requires hexadecimal mantissa"},

      {tok::imag_lit, "0.i", "0.i", ""},
      {tok::imag_lit, ".123i", ".123i", ""},
      {tok::imag_lit, "123.123i", "123.123i", ""},
      {tok::imag_lit, "123e+0i", "123e+0i", ""},
      {tok::imag_lit, "123.E-10i", "123.E-10i", ""},
      {tok::imag_lit, ".123E+10i", ".123E+10i", ""},

      // hexadecimals
      {tok::int_lit, "0x0", "0x0", ""},
      {tok::int_lit, "0x1234", "0x1234", ""},
      {tok::int_lit, "0xcafef00d", "0xcafef00d", ""},
      {tok::int_lit, "0XCAFEF00D", "0XCAFEF00D", ""},

      {tok::int_lit, "0x", "0x", "hexadecimal literal has no digits"},
      {tok::int_lit, "0x1g", "0x1 g", ""},

      {tok::imag_lit, "0xf00i", "0xf00i", ""},

      // hexadecimal floats
      {tok::float_lit, "0x0p0", "0x0p0", ""},
      {tok::float_lit, "0x12efp-123", "0x12efp-123", ""},
      {tok::float_lit, "0xABCD.p+0", "0xABCD.p+0", ""},
      {tok::float_lit, "0x.0189P-0", "0x.0189P-0", ""},
      {tok::float_lit, "0x1.ffffp+1023", "0x1.ffffp+1023", ""},

      {tok::float_lit, "0x.", "0x.", "hexadecimal literal has no digits"},
      {tok::float_lit, "0x0.", "0x0.",
       "hexadecimal mantissa requires a 'p' exponent"},
      {tok::float_lit, "0x.0", "0x.0",
       "hexadecimal mantissa requires a 'p' exponent"},
      {tok::float_lit, "0x1.1", "0x1.1",
       "hexadecimal mantissa requires a 'p' exponent"},
      {tok::float_lit, "0x1.1e0", "0x1.1e0",
       "hexadecimal mantissa requires a 'p' exponent"},
      {tok::float_lit, "0x1.2gp1a", "0x1.2 gp1a",
       "hexadecimal mantissa requires a 'p' exponent"},
      {tok::float_lit, "0x0p", "0x0p", "exponent has no digits"},
      {tok::float_lit, "0xeP-", "0xeP-", "exponent has no digits"},
      {tok::float_lit, "0x1234PAB", "0x1234P AB", "exponent has no digits"},
      {tok::float_lit, "0x1.2p1a", "0x1.2p1 a", ""},

      {tok::imag_lit, "0xf00.bap+12i", "0xf00.bap+12i", ""},

      // separators
      {tok::int_lit, "0b_1000_0001", "0b_1000_0001", ""},
      {tok::int_lit, "0o_600", "0o_600", ""},
      {tok::int_lit, "0_466", "0_466", ""},
      {tok::int_lit, "1_000", "1_000", ""},
      {tok::float_lit, "1_000.000_1", "1_000.000_1", ""},
      {tok::imag_lit, "10e+1_2_3i", "10e+1_2_3i", ""},
      {tok::int_lit, "0x_f00d", "0x_f00d", ""},
      {tok::float_lit, "0x_f00d.0p1_2", "0x_f00d.0p1_2", ""},

      {tok::int_lit, "0b__1000", "0b__1000",
       "'_' must separate successive digits"},
      {tok::int_lit, "0o60___0", "0o60___0",
       "'_' must separate successive digits"},
      {tok::int_lit, "0466_", "0466_", "'_' must separate successive digits"},
      {tok::float_lit, "1_.", "1_.", "'_' must separate successive digits"},
      {tok::float_lit, "0._1", "0._1", "'_' must separate successive digits"},
      {tok::float_lit, "2.7_e0", "2.7_e0",
       "'_' must separate successive digits"},
      {tok::imag_lit, "10e+12_i", "10e+12_i",
       "'_' must separate successive digits"},
      {tok::int_lit, "0x___0", "0x___0", "'_' must separate successive digits"},
      {tok::float_lit, "0x1.0_p0", "0x1.0_p0",
       "'_' must separate successive digits"},
  };
  for (const NumCase &C : Cases) {
    SCOPED_TRACE(C.Src);
    TestSource S(C.Src);
    Lexer L(S.SM, S.File, S.Diags);
    std::istringstream Want(C.Tokens);
    std::string W;
    unsigned I = 0;
    size_t ErrsSeen = 0;
    while (Want >> W) {
      Token T = L.lex();
      std::string Lit = L.getText(T).str();
      // lit for tokens where the text is not the spelling
      if (T.is(tok::period))
        Lit = ".";
      else if (T.is(tok::add))
        Lit = "+";
      else if (T.is(tok::sub))
        Lit = "-";
      if (I == 0) {
        EXPECT_EQ(T.Kind, C.Kind) << tok::getName(T.Kind);
        std::vector<std::string> Errs = S.errors();
        std::string First =
            Errs.empty() ? "" : Errs[0].substr(Errs[0].find(": ") + 2);
        EXPECT_EQ(First, C.Err);
      }
      ErrsSeen = S.errors().size();
      EXPECT_EQ(Lit, W);
      ++I;
    }
    (void)ErrsSeen;
    // make sure we read all
    Token T = L.lex();
    if (T.is(tok::semi))
      T = L.lex();
    EXPECT_EQ(T.Kind, tok::eof) << tok::getName(T.Kind);
  }
}

} // namespace
