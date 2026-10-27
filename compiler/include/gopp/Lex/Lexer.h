//===- gopp/Lex/Lexer.h - The Go++ lexer ------------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hand-written scanner (D2.1), modelled on Go's go/scanner including its
// semicolon insertion. Differences from Go: the tokens '#' and '@', the
// reserved word 'macro', and no //line directives. 'ref' and 'move' are plain
// identifiers (LEX-12).
//
// The lexer never fails: bad input gives a diagnostic and a best-effort token
// (an unterminated string is still a string_lit token). It always makes
// progress and reads only inside the buffer, so NUL bytes, invalid UTF-8 and
// truncated input are safe.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_LEX_LEXER_H
#define GOPP_LEX_LEXER_H

#include "gopp/Basic/Diagnostics.h"
#include "gopp/Lex/Token.h"

namespace gopp {

struct LexerOptions {
  /// Return comments as tok::comment tokens (for gopp fmt and doc extraction).
  bool KeepComments = false;
  /// Insert semicolons at newlines (Go's rule). Only tests turn this off.
  bool InsertSemis = true;
};

class Lexer {
public:
  Lexer(const SourceManager &SM, FileID File, DiagnosticsEngine &Diags,
        LexerOptions Opts = {});

  /// The next token. At the end of input it returns tok::eof forever.
  Token lex();

  /// The source text of a token (empty for an automatic semicolon).
  llvm::StringRef getText(const Token &T) const;
  /// Number of errors this lexer reported.
  unsigned getNumErrors() const { return NumErrors; }

private:
  void next();
  uint8_t peekByte() const;
  void error(uint32_t Offs, diag::ID ID,
             llvm::ArrayRef<llvm::StringRef> Args = {});
  void errorWithFixIt(uint32_t Offs, diag::ID ID, llvm::StringRef Arg,
                      uint32_t Len, llvm::StringRef Replacement);
  /// An error whose primary range is [Offs, End).
  void errorRange(uint32_t Offs, uint32_t End, diag::ID ID);
  SourceLoc loc(uint32_t Offs) const {
    return Base.getLocWithOffset(static_cast<int64_t>(Offs));
  }

  uint32_t scanComment(uint32_t &NlOffset);
  void scanIdentifier();
  int digits(int Base, int64_t &Invalid);
  tok::Kind scanNumber();
  bool scanEscape(int32_t Quote);
  void scanRune();
  void scanString();
  void scanRawString();
  void skipWhitespace();
  tok::Kind switch2(tok::Kind T0, tok::Kind T1);
  tok::Kind switch3(tok::Kind T0, tok::Kind T1, int32_t Ch2, tok::Kind T2);
  tok::Kind switch4(tok::Kind T0, tok::Kind T1, int32_t Ch2, tok::Kind T2,
                    tok::Kind T3);

  DiagnosticsEngine &Diags;
  LexerOptions Opts;
  SourceLoc Base;
  llvm::StringRef Src;
  uint32_t Size;

  int32_t Ch = ' ';      // current character; -1 at the end of input
  uint32_t Offset = 0;   // offset of Ch
  uint32_t RdOffset = 0; // offset of the character after Ch
  bool InsertSemi = false;
  uint32_t NlPos = 0; // pending semicolon inside a /*...*/ comment, or 0
  bool HasNlPos = false;
  unsigned NumErrors = 0;
};

/// Prints one line per token: "line:col kind 'text'" (gopp parse
/// --dump-tokens).
void dumpTokens(llvm::raw_ostream &OS, const SourceManager &SM, FileID File,
                DiagnosticsEngine &Diags, bool KeepComments);

} // namespace gopp

#endif // GOPP_LEX_LEXER_H
