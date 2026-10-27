//===- gopp/Lex/Token.h - Tokens --------------------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_LEX_TOKEN_H
#define GOPP_LEX_TOKEN_H

#include "gopp/Basic/SourceManager.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace gopp {
namespace tok {

enum Kind : uint8_t {
#define TOK(Name) Name,
#define PUNCT(Name, Spell) Name,
#include "gopp/Lex/TokenKinds.def"
  NUM_TOKENS
};

/// The enumerator name, e.g. "l_paren" or "kw_func".
const char *getName(Kind K);
/// The fixed spelling ("(", "func") or null for literals and special tokens.
const char *getSpelling(Kind K);
bool isKeyword(Kind K);
bool isLiteral(Kind K);
/// Operators and delimiters (everything that has a fixed non-word spelling).
bool isPunct(Kind K);
/// Maps a word to its keyword kind, or tok::ident.
Kind lookupKeyword(llvm::StringRef Word);

} // namespace tok

/// A token is a kind plus a span of the source. Literal and identifier text is
/// read from the source (Lexer::getText); nothing is copied. Literal text is
/// raw: it still contains any '\r' bytes.
struct Token {
  enum : uint8_t {
    /// A semicolon that the lexer inserted at a newline or at the end of the
    /// file (Go's rule). It has length 0 and sits on the newline / EOF.
    AutoSemi = 1,
  };

  tok::Kind Kind = tok::eof;
  uint8_t Flags = 0;
  SourceLoc Loc;
  uint32_t Length = 0;

  bool is(tok::Kind K) const { return Kind == K; }
  bool isNot(tok::Kind K) const { return Kind != K; }
  bool isAutoSemi() const { return Flags & AutoSemi; }
  SourceLoc getLoc() const { return Loc; }
  SourceLoc getEndLoc() const { return Loc.getLocWithOffset(Length); }
  SourceRange getRange() const { return SourceRange(Loc, getEndLoc()); }
};

} // namespace gopp

#endif // GOPP_LEX_TOKEN_H
