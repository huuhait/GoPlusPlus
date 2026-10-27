//===- gopp/Format/Format.h - gopp fmt --------------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The formatter of `gopp fmt` (D2.8): gofmt's rules, extended to the Go++
// syntax. It is a port of go/printer and text/tabwriter (Copyright 2009 The
// Go Authors; BSD-style license, see NOTICE): the same layout algorithm, run on
// the Go++ AST and the comment groups of the parser (the comment map of D2.3).
//
//   - One style, no options: tabs for indentation, spaces for alignment.
//   - `ref`, `move`, `&` in range clauses, `!`, attributes, macro declarations
//     and calls are printed in their canonical form. Macro arguments (token
//     trees) are copied verbatim.
//   - Import groups are sorted, except groups that contain a header import
//     (C/C++ header order can matter, D2.8).
//   - Invariant: parse(print(parse(src))) == parse(src).
//
// Not ported from go/printer: the reformatting of doc comments (go/doc/comment)
// and the //go:build line fixes. Comments are never lost.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_FORMAT_FORMAT_H
#define GOPP_FORMAT_FORMAT_H

#include "gopp/AST/AST.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"

#include <functional>
#include <string>

namespace gopp {

struct FormatOptions {
  /// Sort import groups that have no header import (gofmt does).
  bool SortImports = true;
  /// Is the (unquoted) import path a C/C++ header? Empty means
  /// isHeaderImportPath. A build tool that knows the nearest gopp.mod installs
  /// the compiler's rule here (D2.8; P01 D1.15, P10 D10.1).
  std::function<bool(llvm::StringRef)> IsHeader;
};

/// True if an import path is a C/C++ header without a gopp.mod: `<...>` or a
/// path that ends in .h, .hh, .hpp or .hxx (P01 D1.15; P10 refines this with
/// the nearest gopp.mod).
bool isHeaderImportPath(llvm::StringRef Path);

/// Formats file F of SM. Returns false (and reports to Diags, formatting
/// nothing) if the text has syntax errors. The Go++ rules that need only the
/// syntax (`&T{...}` and the like, ParseOptions::CheckLanguageRules) are not
/// checked: like type errors for gofmt, they do not stop formatting. The
/// result of a successful call parses to the same AST as the input. Sorting
/// imports may add the sorted text to SM as a new file.
bool formatFile(SourceManager &SM, FileID F, DiagnosticsEngine &Diags,
                std::string &Out, const FormatOptions &Opts = {});

/// The second half of formatFile: formats a tree that was parsed from F with
/// no error. Returns false (err_fmt_unreported_syntax_error) if the tree has a
/// Bad node, which the parser always reports; this guard keeps `gopp fmt` from
/// printing a Bad node into a file whatever happens to the diagnostics.
bool formatParsedFile(SourceManager &SM, FileID F, ast::File *AST,
                      DiagnosticsEngine &Diags, std::string &Out,
                      const FormatOptions &Opts = {});

/// Formats text that is not in a SourceManager yet (tests, the fuzzer).
/// Returns false if there are syntax errors; Errors, if given, then holds the
/// diagnostics as text.
bool formatText(llvm::StringRef Name, llvm::StringRef Text, std::string &Out,
                std::string *Errors = nullptr, const FormatOptions &Opts = {});

/// Formats an already parsed file (no import sorting, no error check): the
/// printer alone. The AST must come from SM/F and be free of errors.
std::string printFile(const SourceManager &SM, FileID F, const ast::File *AST);

/// A unified diff (3 lines of context) from Old to New for `gopp fmt -d`;
/// empty if they are equal. Name is the file name.
std::string unifiedDiff(llvm::StringRef Name, llvm::StringRef Old,
                        llvm::StringRef New);

} // namespace gopp

#endif // GOPP_FORMAT_FORMAT_H
