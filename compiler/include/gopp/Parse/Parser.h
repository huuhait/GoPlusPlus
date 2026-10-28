//===- gopp/Parse/Parser.h - The Go++ parser --------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Hand-written recursive-descent parser (D2.1), modelled on Go's go/parser.
// It parses all of Go++ v0 (see docs/spec): the Go grammar with generics plus
// `ref` types, the `move` parameter mark, the `&` range mark, the `!` forms,
// macro invocations (#Name(...)), attributes (@Name(...)) and macro
// declarations. Macros are not interpreted: their arguments are kept as token
// trees (D2.6).
//
// parseFile never fails and never crashes: for any byte sequence it returns a
// File (with Bad* nodes where the source was wrong) and reports errors to the
// DiagnosticsEngine. Recursion is bounded (ParseOptions::MaxNesting,
// MaxHeight), so deeply nested input gives an error instead of a stack
// overflow. This is the entry point of the fuzzer (P02.10).
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_PARSE_PARSER_H
#define GOPP_PARSE_PARSER_H

#include "gopp/AST/ASTContext.h"
#include "gopp/Basic/Diagnostics.h"

#include "llvm/ADT/ArrayRef.h"

#include <memory>
#include <vector>

namespace llvm {
class ThreadPoolInterface;
}

namespace gopp {

struct ParseOptions {
  /// Maximum nesting of expressions, types, statements and blocks.
  unsigned MaxNesting = 1000;
  /// Maximum height of the tree of one top-level declaration, counting
  /// nesting and every level of an operator, selector, index or call chain
  /// (`a+b+c...` is a left-deep tree). Every later pass walks the tree
  /// recursively, so this keeps their stack use bounded.
  unsigned MaxHeight = 1024;
  /// Report the Go++ rules that need only the syntax to check: `&T{...}`
  /// (OWN-P4), redeclaring a loop variable (RNG-8), a `!` function with an
  /// `error` result (ERR-15) and `//gopp:cflags` in a source file (LEX-11).
  /// They do not change the tree, so tools that read code that may break them
  /// (`gopp fmt`, Go corpora) turn them off.
  bool CheckLanguageRules = true;
  /// Accept type parameters on methods (generic methods, GEN-6, D1.10). Go
  /// rejects them; the types2 conformance suite (Go mode) turns this off.
  bool AllowGenericMethods = true;
};

/// Parses File (a file of SM) into Ctx. The returned File lives in Ctx; it
/// refers to SM's buffer, so SM must outlive Ctx. Never returns null.
ast::File *parseFile(ast::ASTContext &Ctx, const SourceManager &SM, FileID File,
                     DiagnosticsEngine &Diags, ParseOptions Opts = {});

/// One parsed file of a package: the tree and the arena it lives in.
struct ParsedFile {
  std::unique_ptr<ast::ASTContext> Ctx;
  ast::File *AST = nullptr;
};

/// Parses the files of one package (D2.9), in input order. With NumThreads != 1
/// the files are parsed in parallel on an llvm::ThreadPool (0 = one thread per
/// core, at most one per file). Every file reports into a private engine and
/// its diagnostics are replayed into Diags in file order, so the output (and
/// the error limit) is the same as parsing them one after the other.
///
/// Threads only touch the SourceManager entries of their own file (its lazy
/// line table), so the FileIDs must be distinct and no other code may use SM
/// while this runs. Past the error limit a parallel run still builds every
/// tree; only the diagnostics are cut.
std::vector<ParsedFile> parseFiles(const SourceManager &SM,
                                   llvm::ArrayRef<FileID> Files,
                                   DiagnosticsEngine &Diags,
                                   ParseOptions Opts = {},
                                   unsigned NumThreads = 0);

/// The same on a thread pool the caller owns, so that a build that parses many
/// packages does not create (and tear down) a pool per package. Only the tasks
/// of this call are waited for, so the pool can be shared with other work.
std::vector<ParsedFile> parseFiles(const SourceManager &SM,
                                   llvm::ArrayRef<FileID> Files,
                                   DiagnosticsEngine &Diags, ParseOptions Opts,
                                   llvm::ThreadPoolInterface &Pool);

} // namespace gopp

#endif // GOPP_PARSE_PARSER_H
