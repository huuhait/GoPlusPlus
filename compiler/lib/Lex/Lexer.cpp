//===- Lexer.cpp - The Go++ lexer -----------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a port of Go's go/scanner (scanner.go). The original is
//   Copyright 2009 The Go Authors. All rights reserved.
//   Use of this source code is governed by a BSD-style license (see NOTICE).
//
//===----------------------------------------------------------------------===//

#include "gopp/Lex/Lexer.h"
#include "gopp/Basic/Unicode.h"

#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Unicode.h"
#include "llvm/Support/raw_ostream.h"

using namespace gopp;

//===----------------------------------------------------------------------===//
// Token kind tables
//===----------------------------------------------------------------------===//

const char *tok::getName(Kind K) {
  switch (K) {
#define TOK(Name)                                                              \
  case Name:                                                                   \
    return #Name;
#define PUNCT(Name, Spell) TOK(Name)
#include "gopp/Lex/TokenKinds.def"
  case NUM_TOKENS:
    break;
  }
  return "<invalid>";
}

const char *tok::getSpelling(Kind K) {
  switch (K) {
#define PUNCT(Name, Spell)                                                     \
  case Name:                                                                   \
    return Spell;
#define KEYWORD(Name)                                                          \
  case kw_##Name:                                                              \
    return #Name;
#include "gopp/Lex/TokenKinds.def"
  default:
    break;
  }
  return nullptr;
}

bool tok::isKeyword(Kind K) {
  switch (K) {
#define KEYWORD(Name) case kw_##Name:
#include "gopp/Lex/TokenKinds.def"
    return true;
  default:
    return false;
  }
}

bool tok::isLiteral(Kind K) {
  switch (K) {
  case ident:
  case int_lit:
  case float_lit:
  case imag_lit:
  case char_lit:
  case string_lit:
    return true;
  default:
    return false;
  }
}

bool tok::isPunct(Kind K) {
  switch (K) {
#define PUNCT(Name, Spell) case Name:
#include "gopp/Lex/TokenKinds.def"
    return true;
  default:
    return false;
  }
}

tok::Kind tok::lookupKeyword(llvm::StringRef W) {
  return llvm::StringSwitch<Kind>(W)
#define KEYWORD(Name) .Case(#Name, kw_##Name)
#include "gopp/Lex/TokenKinds.def"
      .Default(ident);
}

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

namespace {

constexpr int32_t EOFChar = -1;

int32_t lower(int32_t C) { return ('a' - 'A') | C; }
bool isDecimal(int32_t C) { return '0' <= C && C <= '9'; }
bool isHex(int32_t C) {
  return isDecimal(C) || ('a' <= lower(C) && lower(C) <= 'f');
}

unsigned digitVal(int32_t C) {
  if ('0' <= C && C <= '9')
    return C - '0';
  if (C >= 0 && 'a' <= lower(C) && lower(C) <= 'f')
    return lower(C) - 'a' + 10;
  return 16; // larger than any legal digit value
}

/// Go's %#U: "U+0023 '#'" for printable runes, "U+0007" otherwise.
std::string formatRune(int32_t C) {
  if (C < 0)
    return "EOF";
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << "U+" << llvm::format_hex_no_prefix(static_cast<uint32_t>(C), 4, true);
  char Buf[8];
  unsigned N = 0;
  uint32_t V = C;
  if (V < 0x80) {
    Buf[N++] = V;
  } else if (V < 0x800) {
    Buf[N++] = 0xC0 | (V >> 6);
    Buf[N++] = 0x80 | (V & 0x3F);
  } else if (V < 0x10000) {
    Buf[N++] = 0xE0 | (V >> 12);
    Buf[N++] = 0x80 | ((V >> 6) & 0x3F);
    Buf[N++] = 0x80 | (V & 0x3F);
  } else {
    Buf[N++] = 0xF0 | (V >> 18);
    Buf[N++] = 0x80 | ((V >> 12) & 0x3F);
    Buf[N++] = 0x80 | ((V >> 6) & 0x3F);
    Buf[N++] = 0x80 | (V & 0x3F);
  }
  if (llvm::sys::unicode::isPrintable(V))
    OS << " '" << llvm::StringRef(Buf, N) << "'";
  return Out;
}

const char *litName(char Prefix) {
  switch (Prefix) {
  case 'x':
    return "hexadecimal literal";
  case 'o':
  case '0':
    return "octal literal";
  case 'b':
    return "binary literal";
  }
  return "decimal literal";
}

/// Index of the first invalid '_' separator in X, or -1.
int64_t invalidSep(llvm::StringRef X) {
  int32_t X1 = ' '; // prefix char, we only care if it's 'x'
  int32_t D = '.';  // digit, one of '_', '0' (a digit), or '.' (anything else)
  size_t I = 0;

  // a prefix counts as a digit
  if (X.size() >= 2 && X[0] == '0') {
    X1 = lower(static_cast<unsigned char>(X[1]));
    if (X1 == 'x' || X1 == 'o' || X1 == 'b') {
      D = '0';
      I = 2;
    }
  }

  // mantissa and exponent
  for (; I < X.size(); ++I) {
    int32_t P = D; // previous digit
    D = static_cast<unsigned char>(X[I]);
    if (D == '_') {
      if (P != '0')
        return I;
    } else if (isDecimal(D) || (X1 == 'x' && isHex(D))) {
      D = '0';
    } else {
      if (P == '_')
        return I - 1;
      D = '.';
    }
  }
  if (D == '_')
    return X.size() - 1;
  return -1;
}

} // namespace

//===----------------------------------------------------------------------===//
// Character reader
//===----------------------------------------------------------------------===//

Lexer::Lexer(const SourceManager &SM, FileID File, DiagnosticsEngine &Diags,
             LexerOptions Opts)
    : Diags(Diags), Opts(Opts), Base(SM.getStartLoc(File)),
      Src(SM.getBuffer(File)), Size(Src.size()) {
  next();
  if (Ch == static_cast<int32_t>(unicode::BOM))
    next(); // ignore BOM at file beginning
}

void Lexer::error(uint32_t Offs, diag::ID ID,
                  llvm::ArrayRef<llvm::StringRef> Args) {
  ++NumErrors;
  if (Diags.limitReached())
    return;
  DiagnosticBuilder B = Diags.report(loc(Offs), ID);
  for (llvm::StringRef A : Args)
    B << A;
}

void Lexer::errorWithFixIt(uint32_t Offs, diag::ID ID, llvm::StringRef Arg,
                           uint32_t Len, llvm::StringRef Replacement) {
  ++NumErrors;
  if (Diags.limitReached())
    return;
  Diags.report(loc(Offs), ID)
          .range(SourceRange(loc(Offs), loc(Offs + Len)))
          .fixit(SourceRange(loc(Offs), loc(Offs + Len)), Replacement)
      << Arg;
}

/// Reads the next character into Ch, reporting NUL, bad UTF-8 and BOMs.
void Lexer::next() {
  if (RdOffset < Size) {
    Offset = RdOffset;
    uint32_t R = static_cast<unsigned char>(Src[RdOffset]);
    unsigned W = 1;
    if (R == 0) {
      error(Offset, diag::err_lex_illegal_nul);
    } else if (R >= 0x80) {
      bool Invalid;
      W = unicode::decodeUTF8(Src.data() + RdOffset, Src.data() + Size, R,
                              Invalid);
      if (Invalid) {
        const unsigned char *In =
            reinterpret_cast<const unsigned char *>(Src.data()) + RdOffset;
        if (Offset == 0 && Size - RdOffset >= 2 &&
            ((In[0] == 0xFF && In[1] == 0xFE) ||
             (In[0] == 0xFE && In[1] == 0xFF))) {
          // U+FEFF BOM at start of file, encoded as big- or little-endian
          // UCS-2 (i.e. 2-byte UTF-16). Give a specific error.
          error(Offset, diag::err_lex_utf16);
          RdOffset += Size - RdOffset; // consume all input: no error cascade
        } else {
          error(Offset, diag::err_lex_illegal_utf8);
        }
      } else if (R == unicode::BOM && Offset > 0) {
        error(Offset, diag::err_lex_illegal_bom);
      }
    }
    RdOffset += W;
    Ch = static_cast<int32_t>(R);
  } else {
    Offset = Size;
    Ch = EOFChar;
  }
}

uint8_t Lexer::peekByte() const {
  return RdOffset < Size ? static_cast<uint8_t>(Src[RdOffset]) : 0;
}

llvm::StringRef Lexer::getText(const Token &T) const {
  if (T.Length == 0)
    return {};
  uint32_t Off = T.Loc.getRaw() - Base.getRaw();
  return Src.substr(Off, T.Length);
}

//===----------------------------------------------------------------------===//
// Comments, identifiers, numbers
//===----------------------------------------------------------------------===//

/// Scans a comment; the initial '/' is consumed and Ch is '/' or '*'. Returns
/// the offset where the comment text ends and sets NlOffset to the offset of
/// the first newline inside a /*...*/ comment (0 if there is none).
uint32_t Lexer::scanComment(uint32_t &NlOffset) {
  NlOffset = 0;
  if (Ch == '/') {
    // //-style comment: the final '\n' is not part of the comment
    next();
    while (Ch != '\n' && Ch >= 0)
      next();
    uint32_t End = Offset;
    if (End > 0 && Src[End - 1] == '\r')
      --End;
    return End;
  }

  // /*-style comment */
  uint32_t Offs = Offset - 1; // position of the initial '/'
  next();
  while (Ch >= 0) {
    int32_t C = Ch;
    if (C == '\n' && NlOffset == 0)
      NlOffset = Offset;
    next();
    if (C == '*' && Ch == '/') {
      next();
      return Offset;
    }
  }
  error(Offs, diag::err_lex_comment_unterminated);
  return Offset;
}

void Lexer::scanIdentifier() {
  // Ch is a letter. Consume ASCII letters and digits with a tight loop and fall
  // back to next() for everything else.
  for (;;) {
    while (RdOffset < Size) {
      unsigned char B = Src[RdOffset];
      if ((B | 0x20) - 'a' >= 26u && B != '_' && B - '0' >= 10u)
        break;
      Offset = RdOffset++;
      Ch = B;
    }
    next();
    if (Ch < 0 || !(unicode::isLetter(Ch) || unicode::isDigit(Ch)))
      return;
  }
}

/// digits accepts the sequence { digit | '_' }. If Base <= 10 it accepts any
/// decimal digit but records the offset of the first digit >= Base in Invalid.
/// Returns bit 0: digits present, bit 1: '_' present.
int Lexer::digits(int Base, int64_t &Invalid) {
  int DigSep = 0;
  if (Base <= 10) {
    int32_t Max = '0' + Base;
    while (isDecimal(Ch) || Ch == '_') {
      int DS = 1;
      if (Ch == '_')
        DS = 2;
      else if (Ch >= Max && Invalid < 0)
        Invalid = Offset;
      DigSep |= DS;
      next();
    }
  } else {
    while (isHex(Ch) || Ch == '_') {
      DigSep |= Ch == '_' ? 2 : 1;
      next();
    }
  }
  return DigSep;
}

tok::Kind Lexer::scanNumber() {
  uint32_t Offs = Offset;
  tok::Kind Tok = tok::illegal;

  int Base = 10;        // number base
  char Prefix = 0;      // one of 0 (decimal), '0' (0-octal), 'x', 'o', or 'b'
  int DigSep = 0;       // bit 0: digit present, bit 1: '_' present
  int64_t Invalid = -1; // offset of an invalid digit, or < 0

  // integer part
  if (Ch != '.') {
    Tok = tok::int_lit;
    if (Ch == '0') {
      next();
      switch (lower(Ch)) {
      case 'x':
        next();
        Base = 16, Prefix = 'x';
        break;
      case 'o':
        next();
        Base = 8, Prefix = 'o';
        break;
      case 'b':
        next();
        Base = 2, Prefix = 'b';
        break;
      default:
        Base = 8, Prefix = '0';
        DigSep = 1; // leading 0
      }
    }
    DigSep |= digits(Base, Invalid);
  }

  // fractional part
  if (Ch == '.') {
    Tok = tok::float_lit;
    if (Prefix == 'o' || Prefix == 'b')
      error(Offset, diag::err_lex_radix_point, {litName(Prefix)});
    next();
    DigSep |= digits(Base, Invalid);
  }

  if ((DigSep & 1) == 0)
    error(Offset, diag::err_lex_no_digits, {litName(Prefix)});

  // exponent
  int32_t E = lower(Ch);
  if (Ch >= 0 && (E == 'e' || E == 'p')) {
    std::string Exp(1, char(Ch));
    if (E == 'e' && Prefix != 0 && Prefix != '0')
      error(Offset, diag::err_lex_exponent_needs_decimal, {Exp});
    else if (E == 'p' && Prefix != 'x')
      error(Offset, diag::err_lex_exponent_needs_hex, {Exp});
    next();
    Tok = tok::float_lit;
    if (Ch == '+' || Ch == '-')
      next();
    int64_t Unused = 0; // base 10: never records an invalid digit
    int DS = digits(10, Unused);
    DigSep |= DS;
    if ((DS & 1) == 0)
      error(Offset, diag::err_lex_exponent_no_digits);
  } else if (Prefix == 'x' && Tok == tok::float_lit) {
    error(Offset, diag::err_lex_hex_mantissa_needs_p);
  }

  // suffix 'i'
  if (Ch == 'i') {
    Tok = tok::imag_lit;
    next();
  }

  llvm::StringRef Lit = Src.slice(Offs, Offset);
  if (Tok == tok::int_lit && Invalid >= 0) {
    std::string Digit(1, Lit[Invalid - Offs]);
    error(Invalid, diag::err_lex_invalid_digit, {Digit, litName(Prefix)});
  }
  if (DigSep & 2) {
    int64_t I = invalidSep(Lit);
    if (I >= 0)
      error(Offs + I, diag::err_lex_bad_separator);
  }
  return Tok;
}

//===----------------------------------------------------------------------===//
// Strings and runes
//===----------------------------------------------------------------------===//

/// Parses an escape sequence where Quote is the accepted escaped quote. On a
/// syntax error it stops at the offending character (without consuming it).
bool Lexer::scanEscape(int32_t Quote) {
  uint32_t Offs = Offset;

  unsigned N;
  uint32_t Base, Max;
  switch (Ch) {
  case 'a':
  case 'b':
  case 'f':
  case 'n':
  case 'r':
  case 't':
  case 'v':
  case '\\':
    next();
    return true;
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
    N = 3, Base = 8, Max = 255;
    break;
  case 'x':
    next();
    N = 2, Base = 16, Max = 255;
    break;
  case 'u':
    next();
    N = 4, Base = 16, Max = unicode::MaxRune;
    break;
  case 'U':
    next();
    N = 8, Base = 16, Max = unicode::MaxRune;
    break;
  default:
    if (Ch == Quote) {
      next();
      return true;
    }
    error(Offs, Ch < 0 ? diag::err_lex_escape_unterminated
                       : diag::err_lex_escape_unknown);
    return false;
  }

  uint32_t X = 0;
  while (N > 0) {
    uint32_t D = digitVal(Ch);
    if (D >= Base) {
      if (Ch < 0)
        error(Offset, diag::err_lex_escape_unterminated);
      else
        error(Offset, diag::err_lex_escape_illegal_char, {formatRune(Ch)});
      return false;
    }
    X = X * Base + D;
    next();
    --N;
  }

  if (X > Max || (0xD800 <= X && X < 0xE000)) {
    error(Offs, diag::err_lex_escape_bad_code_point);
    return false;
  }
  return true;
}

void Lexer::scanRune() {
  // '\'' opening already consumed
  uint32_t Offs = Offset - 1;

  bool Valid = true;
  unsigned N = 0;
  for (;;) {
    int32_t C = Ch;
    if (C == '\n' || C < 0) {
      // only report error if we don't have one already
      if (Valid) {
        error(Offs, diag::err_lex_rune_unterminated);
        Valid = false;
      }
      break;
    }
    next();
    if (C == '\'')
      break;
    ++N;
    if (C == '\\') {
      if (!scanEscape('\''))
        Valid = false;
      // continue to read to closing quote
    }
  }

  if (Valid && N != 1)
    error(Offs, diag::err_lex_rune_illegal);
}

void Lexer::scanString() {
  // '"' opening already consumed
  uint32_t Offs = Offset - 1;
  for (;;) {
    int32_t C = Ch;
    if (C == '\n' || C < 0) {
      error(Offs, diag::err_lex_string_unterminated);
      break;
    }
    next();
    if (C == '"')
      break;
    if (C == '\\')
      scanEscape('"');
  }
}

void Lexer::scanRawString() {
  // '`' opening already consumed
  uint32_t Offs = Offset - 1;
  for (;;) {
    int32_t C = Ch;
    if (C < 0) {
      error(Offs, diag::err_lex_raw_string_unterminated);
      break;
    }
    next();
    if (C == '`')
      break;
  }
}

void Lexer::skipWhitespace() {
  while (Ch == ' ' || Ch == '\t' || (Ch == '\n' && !InsertSemi) || Ch == '\r')
    next();
}

//===----------------------------------------------------------------------===//
// Operators
//===----------------------------------------------------------------------===//

tok::Kind Lexer::switch2(tok::Kind T0, tok::Kind T1) {
  if (Ch == '=') {
    next();
    return T1;
  }
  return T0;
}

tok::Kind Lexer::switch3(tok::Kind T0, tok::Kind T1, int32_t Ch2,
                         tok::Kind T2) {
  if (Ch == '=') {
    next();
    return T1;
  }
  if (Ch == Ch2) {
    next();
    return T2;
  }
  return T0;
}

tok::Kind Lexer::switch4(tok::Kind T0, tok::Kind T1, int32_t Ch2, tok::Kind T2,
                         tok::Kind T3) {
  if (Ch == '=') {
    next();
    return T1;
  }
  if (Ch == Ch2) {
    next();
    if (Ch == '=') {
      next();
      return T3;
    }
    return T2;
  }
  return T0;
}

//===----------------------------------------------------------------------===//
// lex
//===----------------------------------------------------------------------===//

Token Lexer::lex() {
  Token T;
  for (;;) { // scanAgain
    if (HasNlPos) {
      // Artificial ';' after a /*...*/ comment that contains a newline, at the
      // position of its first newline.
      HasNlPos = false;
      T.Kind = tok::semi;
      T.Flags = Token::AutoSemi;
      T.Loc = loc(NlPos);
      T.Length = 0;
      return T;
    }

    skipWhitespace();

    uint32_t Start = Offset; // current token start
    T.Loc = loc(Start);
    bool NewInsertSemi = false;
    int32_t C = Ch;
    tok::Kind K = tok::illegal;

    if (C >= 0 && unicode::isLetter(C)) {
      scanIdentifier();
      llvm::StringRef Lit = Src.slice(Start, Offset);
      if (Lit.size() > 1) {
        // keywords are longer than one letter - avoid lookup otherwise
        K = tok::lookupKeyword(Lit);
        switch (K) {
        case tok::ident:
        case tok::kw_break:
        case tok::kw_continue:
        case tok::kw_fallthrough:
        case tok::kw_return:
          NewInsertSemi = true;
          break;
        default:
          break;
        }
      } else {
        NewInsertSemi = true;
        K = tok::ident;
      }
    } else if (isDecimal(C) || (C == '.' && isDecimal(peekByte()))) {
      NewInsertSemi = true;
      K = scanNumber();
    } else {
      next(); // always make progress
      switch (C) {
      case EOFChar:
        if (InsertSemi) {
          InsertSemi = false; // EOF consumed
          T.Kind = tok::semi;
          T.Flags = Token::AutoSemi;
          T.Length = 0;
          return T;
        }
        K = tok::eof;
        break;
      case '\n':
        // we only reach here if InsertSemi was set in the first place and
        // exited early from skipWhitespace()
        InsertSemi = false; // newline consumed
        T.Kind = tok::semi;
        T.Flags = Token::AutoSemi;
        T.Length = 0;
        return T;
      case '"':
        NewInsertSemi = true;
        K = tok::string_lit;
        scanString();
        break;
      case '\'':
        NewInsertSemi = true;
        K = tok::char_lit;
        scanRune();
        break;
      case '`':
        NewInsertSemi = true;
        K = tok::string_lit;
        scanRawString();
        break;
      case ':':
        K = switch2(tok::colon, tok::define);
        break;
      case '.':
        // fractions starting with a '.' are handled by the number case above
        K = tok::period;
        if (Ch == '.' && peekByte() == '.') {
          next();
          next(); // consume last '.'
          K = tok::ellipsis;
        }
        break;
      case ',':
        K = tok::comma;
        break;
      case ';':
        K = tok::semi;
        break;
      case '(':
        K = tok::l_paren;
        break;
      case ')':
        NewInsertSemi = true;
        K = tok::r_paren;
        break;
      case '[':
        K = tok::l_brack;
        break;
      case ']':
        NewInsertSemi = true;
        K = tok::r_brack;
        break;
      case '{':
        K = tok::l_brace;
        break;
      case '}':
        NewInsertSemi = true;
        K = tok::r_brace;
        break;
      case '+':
        K = switch3(tok::add, tok::add_assign, '+', tok::inc);
        if (K == tok::inc)
          NewInsertSemi = true;
        break;
      case '-':
        K = switch3(tok::sub, tok::sub_assign, '-', tok::dec);
        if (K == tok::dec)
          NewInsertSemi = true;
        break;
      case '*':
        K = switch2(tok::mul, tok::mul_assign);
        break;
      case '/':
        if (Ch == '/' || Ch == '*') {
          // comment
          uint32_t NlOffset;
          uint32_t End = scanComment(NlOffset);
          if (InsertSemi && NlOffset != 0) {
            // For /*...*/ containing \n, return the comment, then an
            // artificial semicolon at the first newline.
            NlPos = NlOffset;
            HasNlPos = true;
            InsertSemi = false;
          } else {
            NewInsertSemi = InsertSemi; // preserve InsertSemi info
          }
          if (!Opts.KeepComments)
            continue; // skip the comment; InsertSemi keeps its meaning
          K = tok::comment;
          T.Kind = K;
          T.Length = End - Start;
          if (Opts.InsertSemis)
            InsertSemi = NewInsertSemi;
          return T;
        }
        K = switch2(tok::quo, tok::quo_assign);
        break;
      case '%':
        K = switch2(tok::rem, tok::rem_assign);
        break;
      case '^':
        K = switch2(tok::xor_, tok::xor_assign);
        break;
      case '<':
        if (Ch == '-') {
          next();
          K = tok::arrow;
        } else {
          K = switch4(tok::lss, tok::leq, '<', tok::shl, tok::shl_assign);
        }
        break;
      case '>':
        K = switch4(tok::gtr, tok::geq, '>', tok::shr, tok::shr_assign);
        break;
      case '=':
        K = switch2(tok::assign, tok::eql);
        break;
      case '!':
        K = switch2(tok::not_, tok::neq);
        break;
      case '&':
        if (Ch == '^') {
          next();
          K = switch2(tok::and_not, tok::and_not_assign);
        } else {
          K = switch3(tok::and_, tok::and_assign, '&', tok::land);
        }
        break;
      case '|':
        K = switch3(tok::or_, tok::or_assign, '|', tok::lor);
        break;
      case '~':
        K = tok::tilde;
        break;
      case '#':
        K = tok::hash;
        break;
      case '@':
        K = tok::at;
        break;
      default:
        // next() reports unexpected BOMs: don't repeat
        if (C != static_cast<int32_t>(unicode::BOM)) {
          // Curly quotation marks are easily introduced by copy and paste.
          if (C == 0x201C || C == 0x201D) {
            errorWithFixIt(Start, diag::err_lex_curly_quote,
                           C == 0x201C ? "\xE2\x80\x9C" : "\xE2\x80\x9D", 3,
                           "\"");
          } else {
            error(Start, diag::err_lex_illegal_char, {formatRune(C)});
          }
        }
        NewInsertSemi = InsertSemi; // preserve InsertSemi info
        K = tok::illegal;
        break;
      }
    }

    if (Opts.InsertSemis)
      InsertSemi = NewInsertSemi;

    T.Kind = K;
    T.Length = Offset - Start;
    return T;
  }
}

//===----------------------------------------------------------------------===//
// Token dump
//===----------------------------------------------------------------------===//

void gopp::dumpTokens(llvm::raw_ostream &OS, const SourceManager &SM,
                      FileID File, DiagnosticsEngine &Diags,
                      bool KeepComments) {
  LexerOptions Opts;
  Opts.KeepComments = KeepComments;
  Lexer L(SM, File, Diags, Opts);
  for (;;) {
    Token T = L.lex();
    PresumedLoc P = SM.getPresumedLoc(T.Loc);
    OS << P.Line << ':' << P.Column << ' ' << tok::getName(T.Kind);
    if (T.isAutoSemi()) {
      OS << " '\\n'";
    } else if (T.Kind != tok::eof) {
      OS << " '";
      for (char C : L.getText(T)) {
        unsigned char U = C;
        if (C == '\n')
          OS << "\\n";
        else if (C == '\t')
          OS << "\\t";
        else if (C == '\r')
          OS << "\\r";
        else if (C == '\\')
          OS << "\\\\";
        else if (U < 0x20 || U == 0x7F)
          OS << "\\x" << llvm::format_hex_no_prefix(U, 2, true);
        else
          OS << C;
      }
      OS << "'";
    }
    OS << '\n';
    if (T.Kind == tok::eof)
      break;
  }
}
