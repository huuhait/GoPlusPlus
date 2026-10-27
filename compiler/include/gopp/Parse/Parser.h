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

namespace gopp {

struct ParseOptions {
  /// Maximum nesting of expressions, types, statements and blocks.
  unsigned MaxNesting = 1000;
  /// Maximum height of the tree of one top-level declaration, counting
  /// nesting and every level of an operator, selector, index or call chain
  /// (`a+b+c...` is a left-deep tree). Every later pass walks the tree
  /// recursively, so this keeps their stack use bounded.
  unsigned MaxHeight = 1024;
};

/// Parses File (a file of SM) into Ctx. The returned File lives in Ctx; it
/// refers to SM's buffer, so SM must outlive Ctx. Never returns null.
ast::File *parseFile(ast::ASTContext &Ctx, const SourceManager &SM, FileID File,
                     DiagnosticsEngine &Diags, ParseOptions Opts = {});

} // namespace gopp

#endif // GOPP_PARSE_PARSER_H
