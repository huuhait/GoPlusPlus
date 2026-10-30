//===- gopp/GIR/Lower.h - The checked AST to GIR ----------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01 / P04.02. The builder: turns a checked package into a GIR module,
// reading only what the checker recorded (sema::Info: the type and constant
// of every expression, what every name denotes). It never looks a name up
// itself and never works out a type. The subset in M0 is functions of bool,
// integer and string values, `if`, `for`, calls, conversions between number
// types, `@Extern("C")` declarations and the built-in `println` / `print`;
// anything else is reported as err_gir_unsupported (docs/design/gir.md
// section 15 lists what each later task adds).
//
// The GIR verifier always runs on the result; a failure is err_gir_verify.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_LOWER_H
#define GOPP_GIR_LOWER_H

#include "gopp/Basic/Diagnostics.h"
#include "gopp/GIR/GIR.h"
#include "gopp/Sema/Sema.h"

namespace gopp {
namespace gir {

struct LowerOptions {
  /// Width of `int`, `uint`, `uintptr` and pointers. 64: linux/amd64 and
  /// linux/arm64.
  unsigned PointerBits = 64;
  /// Building a program: package main must have `func main`, and the module
  /// gets the C entry point `main` that calls `main.main`.
  bool Entry = true;
};

/// Lowers the package. Returns null after reporting the errors.
std::unique_ptr<Module> lowerPackage(Context &Ctx, const SourceManager &SM,
                                     const sema::CheckedPackage &CP,
                                     llvm::ArrayRef<const ast::File *> Files,
                                     DiagnosticsEngine &Diags,
                                     const LowerOptions &Opts = {});

/// What a function's symbol is made from.
enum class SymbolKind : uint8_t {
  Func,    // a function Name of package Pkg
  ExternC, // an @Extern("C") function: its C name is Name
  Entry,   // the C entry point that calls main.main (Name is unused)
  Runtime, // a function the compiler writes in GIR (println's helpers)
};

/// The symbol of a function, the one place that decides it (D4.13): M0 makes
/// `main.f` for package main, `<path>.f` for another package, the bare name
/// for @Extern("C"), `main` for the entry point and `gopp.<name>` for the
/// compiler's helpers. A Go++ symbol always has a dot, so it never collides
/// with a C name. P04.17 replaces the Func and Runtime cases with its
/// mangling.
std::string symbolOf(SymbolKind K, const sema::Package *Pkg,
                     llvm::StringRef Name);

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_LOWER_H
