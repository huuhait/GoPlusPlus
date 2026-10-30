//===- gopp/GIR/Serialize.h - The GIR bitstream form ------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// D4.15, docs/design/gir.md section 13. A module is written as an LLVM
// bitstream container: a magic number, an identification block with the
// compiler version, one module block (tables, function declarations, one
// block per function body, an index of the bodies) and a trailer that points
// at the index, so a reader can jump to one body. The same module always
// gives the same bytes. Only the compiler that wrote a file can read it.
//
// P06.02 writes these blocks into the `.gopx` sections; this API is the part
// that knows the format.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_SERIALIZE_H
#define GOPP_GIR_SERIALIZE_H

#include "gopp/GIR/GIR.h"

#include "llvm/Support/Error.h"

namespace gopp {
namespace gir {

/// The bytes of M. Deterministic.
std::string writeModule(const Module &M);

struct ReadOptions {
  /// Read only the body of this function (every function is still declared).
  /// Empty: read all bodies.
  llvm::StringRef OnlyFunction;
  /// The compiler version the file must have; empty: this compiler's.
  std::string Version;
};

/// Reads a module into Ctx and verifies it. Fails on a different compiler
/// version, a damaged file, or GIR that does not verify.
llvm::Expected<std::unique_ptr<Module>>
readModule(llvm::StringRef Bytes, Context &Ctx, const ReadOptions &Opts = {});

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_SERIALIZE_H
