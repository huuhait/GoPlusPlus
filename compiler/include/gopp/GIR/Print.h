//===- gopp/GIR/Print.h - The GIR text form ---------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The printer behind `gopp build -emit=gir` and `gopp gir dump` (P04.01). The
// text is for people and FileCheck tests; the reader of the binary form
// (Serialize.h) prints the same text, which is how the round trip is tested.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_PRINT_H
#define GOPP_GIR_PRINT_H

#include "gopp/GIR/GIR.h"

namespace gopp {
namespace gir {

struct PrintOptions {
  /// Print `; file:line:col` after statements and terminators.
  bool Spans = true;
};

void print(llvm::raw_ostream &OS, const Module &M,
           const PrintOptions &Opts = {});
void print(llvm::raw_ostream &OS, const Module &M, const Function &F,
           const PrintOptions &Opts = {});
/// `_1`, `(*_1).0`, `_2[_3]`.
void print(llvm::raw_ostream &OS, const Place &P);

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_PRINT_H
