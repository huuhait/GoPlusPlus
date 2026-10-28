//===- gopp/Basic/DiagnosticExplain.h - gopp explain ------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The text behind `gopp explain E0042` (D3.11, P03.14): for every diagnostic
// a few sentences on what is wrong and how to fix it, and an example file
// that makes the compiler report it. The data is DiagnosticExplain.def; the
// unit test ExplainTest.cpp compiles every example.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BASIC_DIAGNOSTICEXPLAIN_H
#define GOPP_BASIC_DIAGNOSTICEXPLAIN_H

#include "gopp/Basic/Diagnostics.h"

namespace gopp {

struct DiagExplanation {
  const char *Text;
  /// A whole source file (`gopp check` it). Some entries describe a file
  /// that cannot be written as text (bytes that are not UTF-8) in a comment.
  const char *Example;
};

/// The explanation of ID; null for `custom`.
const DiagExplanation *getDiagExplanation(diag::ID ID);

/// True for a family code: one code with the format "%0" that stands for many
/// messages. The explain page shows typical messages for it.
bool isDiagFamily(diag::ID ID);

/// Prints the page of `gopp explain`: code, name, severity, the message of
/// the table, the explanation and the example.
void printDiagExplanation(llvm::raw_ostream &OS, diag::ID ID);

/// Prints one line per code: `E0042 err_name  message`.
void printDiagCodeList(llvm::raw_ostream &OS);

} // namespace gopp

#endif // GOPP_BASIC_DIAGNOSTICEXPLAIN_H
