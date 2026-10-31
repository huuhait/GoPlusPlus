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
  /// Test-only: labelPos walks the function's statements again at every
  /// goto out of a map range, the quadratic walk the stress test's control
  /// must catch (tests/unit/GIR/StressTest.cpp).
  bool RescanLabels = false;
  /// Test-only: a call emits GIR for every pair of its map-entry arguments
  /// (the quadratic emission the stress test's control must catch).
  bool PairwiseEntryArgs = false;
  /// Test-only (P04.25, ADR-0029): `@Extern("C")` declarations named
  /// `gopp_test_*` are compiler intrinsics that build a slice or a map around
  /// a counting allocator object (two exist; a unit test may pass its own),
  /// read its counters and call the runtime's free and clone routines (the
  /// ones P04.09's drop glue and `Clone` will call). Removed once P05.03 has
  /// real arenas, or kept for runtime unit tests only.
  bool TestIntrinsics = false;
  /// Test-only: an interface method call finds its itab slot by a scan of the
  /// interface's methods, the quadratic search the stress test's control must
  /// catch.
  bool ScanItabSlots = false;
  /// Test-only: the checker's lookup of a method by name scans the type's
  /// methods (sema::MethodSets), the quadratic walk the stress test's
  /// control must catch.
  bool ScanMethodLookups = false;
  /// Test-only: the instance walk finds an instance already made by a scan of
  /// all the instances so far (Instances.cpp) instead of by its symbol: the
  /// quadratic walk the stress test's control must catch.
  bool ScanInstances = false;
  /// Test-only: the instance walk takes the method set of an instance type at
  /// every place one of its values becomes an interface value, instead of
  /// once: the quadratic walk the stress test's control must catch.
  bool RedescribeTypes = false;
  /// The most instances of generic code one package may have, or its source
  /// size in bytes if that is more (GEN-5, P04.08: a chain of generics that
  /// each call the next twice with two bigger type arguments has 2^n, a
  /// written chain of n generic functions n). Past it is one error (E0449)
  /// and no more instances. A test may lower it.
  size_t MaxInstances = 100000;
  /// The most nodes of code the instance walk visits (the plain code and the
  /// body of every instance, which the lowering lowers too), or 16 per byte
  /// of the package's source if that is more (GEN-5, P04.08): what the
  /// instances hold is bounded, not only their number (2^16 instances of a
  /// 1000-statement body are a 20 KB file). Plain code walks under one node
  /// per byte. Past it is one error (E0449) and no more instances. A test
  /// may lower it.
  uint64_t MaxWalkedNodes = 4000000;
};

/// Test-only (M8): the steps the lowering walks that make no GIR statement
/// each (the late reads of assignment targets, range clauses, the label
/// walk, the scopes left by a branch) have taken on this thread so far. A
/// stress test checks that it grows in step with the input.
uint64_t loweringSteps();

/// Test-only: the steps of the closure analysis (which literals capture what,
/// which escape, which parameters a function keeps) on this thread so far.
uint64_t escapeSteps();
/// Test-only (M8): the steps of the instance walk (every expression of the
/// walked code, every instance requested and every type met) of the
/// lowerings this thread asked for. A stress test checks that it grows in
/// step with the number of instances.
uint64_t instanceSteps();

/// Lowers the package. Returns null after reporting the errors.
std::unique_ptr<Module> lowerPackage(Context &Ctx, const SourceManager &SM,
                                     const sema::CheckedPackage &CP,
                                     llvm::ArrayRef<const ast::File *> Files,
                                     DiagnosticsEngine &Diags,
                                     const LowerOptions &Opts = {});

/// The per-instance checks of generic code (P04.08, D4.3) without any GIR:
/// `gopp check` calls it after the package type-checks. It walks the same
/// instances `lowerPackage` lowers (one walk, lib/GIR/Instances.h) and reports
/// the same errors: an `Option[X]` made inside a generic body with a nilable
/// or Option X (E0446), a `View[X]` with a pointer kind (E0447), a call whose
/// last result is a type parameter that is `error` while another result has
/// no zero value (E0448), and an instance past the limits (E0449). Only
/// Opts.MaxInstances and Opts.MaxWalkedNodes are read. False after reporting.
bool checkInstances(const sema::CheckedPackage &CP,
                    llvm::ArrayRef<const ast::File *> Files,
                    DiagnosticsEngine &Diags, const LowerOptions &Opts = {});

/// What a symbol is made from.
enum class SymbolKind : uint8_t {
  Func,     // a function Name of package Pkg
  Var,      // a package variable Name of package Pkg
  InitFunc, // the Ordinal-th `init` function of package Pkg
  ExternC,  // an @Extern("C") function: its C name is Name
  Entry,    // the C entry point that calls main.main (Name is unused)
  Runtime,  // a function the compiler writes in GIR (println's helpers)
};

/// The symbol of an entity, the one place that decides it (D4.13): the
/// mangled `_GP` name (gopp/Mangle) of the function, variable or `init`
/// function; the bare name for @Extern("C") (appendix A.8); `main` for the
/// C entry point; the mangled `gopp.<name>` for the compiler's helpers.
/// Entities that carry a checker type or object (a function, a variable, the
/// glue of a type, a type declared inside a function) are named by
/// sym::SemaNames through gir::Symbols (lib/GIR/Symbols.h), which the
/// lowering uses; this is the same decision for the ones that need none.
std::string symbolOf(SymbolKind K, const sema::Package *Pkg,
                     llvm::StringRef Name, uint64_t Ordinal = 0);

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_LOWER_H
