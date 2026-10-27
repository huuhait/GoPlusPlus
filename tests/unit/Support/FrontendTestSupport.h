//===- FrontendTestSupport.h - Helpers for front-end unit tests -----------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_TESTS_FRONTENDTESTSUPPORT_H
#define GOPP_TESTS_FRONTENDTESTSUPPORT_H

#include "gopp/AST/ASTContext.h"
#include "gopp/AST/ASTDumper.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/Lex/Lexer.h"
#include "gopp/Parse/Parser.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace gopp::test {

/// One source text in its own SourceManager, with an unlimited collecting
/// diagnostics engine.
struct TestSource {
  explicit TestSource(llvm::StringRef Text, llvm::StringRef Name = "test.gopp",
                      unsigned ErrorLimit = 0)
      : Diags(SM, Consumer, ErrorLimit) {
    llvm::Expected<FileID> F = SM.addMemoryBuffer(Name, Text);
    if (!F)
      llvm::report_fatal_error("cannot add buffer");
    File = *F;
  }
  SourceManager SM;
  CollectingDiagnosticConsumer Consumer;
  DiagnosticsEngine Diags;
  FileID File;

  unsigned offsetOf(SourceLoc L) const { return SM.getFileOffset(L); }
  /// "offset: message" for every error, in order.
  std::vector<std::string> errors() const {
    std::vector<std::string> Out;
    for (const Diagnostic &D : Consumer.Diags)
      if (D.Sev == Severity::Error)
        Out.push_back((D.Loc.isValid() ? std::to_string(offsetOf(D.Loc))
                                       : std::string("-")) +
                      ": " + D.Message);
    return Out;
  }
  unsigned numErrors() const { return Diags.getNumErrors(); }
};

struct LexedToken {
  Token Tok;
  std::string Text;
  unsigned Offset;
};

/// Lexes the whole text (up to and including eof).
inline std::vector<LexedToken> lexAll(TestSource &S, LexerOptions Opts = {}) {
  Lexer L(S.SM, S.File, S.Diags, Opts);
  std::vector<LexedToken> Out;
  for (;;) {
    Token T = L.lex();
    Out.push_back({T, L.getText(T).str(), S.offsetOf(T.Loc)});
    if (T.is(tok::eof))
      break;
  }
  return Out;
}

/// A parsed source text: the tree is valid as long as this object lives.
struct Parsed {
  explicit Parsed(llvm::StringRef Text, ParseOptions Opts = {},
                  unsigned ErrorLimit = 0)
      : Src(Text, "test.gopp", ErrorLimit) {
    File = parseFile(Ctx, Src.SM, Src.File, Src.Diags, Opts);
  }
  TestSource Src;
  ast::ASTContext Ctx;
  ast::File *File = nullptr;

  std::vector<std::string> errors() const { return Src.errors(); }
  unsigned numErrors() const { return Src.numErrors(); }
  std::string dump() const {
    std::string Out;
    llvm::raw_string_ostream OS(Out);
    ast::dump(OS, Src.SM, File);
    return Out;
  }
};

} // namespace gopp::test

#endif // GOPP_TESTS_FRONTENDTESTSUPPORT_H
