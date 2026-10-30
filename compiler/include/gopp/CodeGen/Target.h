//===- gopp/CodeGen/Target.h - LLVM target queries --------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.02 / P04.03. The targets Go++ builds for, as one table (D4.5, D4.9):
// the triple, the CPU and features, the data layout the language's layouts
// assume, and what the link step needs (the LLD emulation, the name of the
// dynamic linker). Nothing else in the compiler writes a triple.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_CODEGEN_TARGET_H
#define GOPP_CODEGEN_TARGET_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <string>

namespace llvm {
class TargetMachine;
}

namespace gopp {

/// The version of the LLVM that gopp is built against, e.g. "23.1.0".
std::string getLLVMVersion();

/// The default target triple of the host, e.g. "x86_64-unknown-linux-gnu".
std::string getDefaultTargetTriple();

/// A target Go++ can build for: Go's `GOOS/GOARCH` names plus what LLVM and
/// LLD need to know.
struct TargetSpec {
  std::string OS;   // "linux"
  std::string Arch; // "amd64" or "arm64" (the names the build tags use)
  std::string Triple;
  std::string CPU;
  std::string Features;
  /// The data layout string LLVM must give this target. Pinned: Go++'s
  /// layouts (D4.5) and the C ABI depend on it, so a newer LLVM that changes
  /// it is an error to look at, not something to follow silently.
  std::string DataLayout;
  /// LLD's `-m` emulation.
  std::string Emulation;
  /// The file name of the glibc dynamic linker (looked up in the sysroot, or
  /// /lib64 and /lib).
  std::string DynamicLinkerFile;
  /// Where a glibc system keeps it when the sysroot does not say.
  std::string DefaultDynamicLinker;
  unsigned PointerBits = 64;

  std::string name() const { return OS + "/" + Arch; }
};

/// "linux/amd64", "linux/arm64" (also `host`); an error that lists the
/// supported targets otherwise.
llvm::Expected<TargetSpec> parseTarget(llvm::StringRef Name);
/// The target the compiler runs on, if it is a supported one.
llvm::Expected<TargetSpec> hostTarget();

/// Registers the LLVM back ends of the supported targets (once).
void initializeTargets();

/// The TargetMachine for T: PIC code, the pinned CPU and features. Fails if the
/// data layout LLVM gives is not the pinned one.
llvm::Expected<std::unique_ptr<llvm::TargetMachine>>
createTargetMachine(const TargetSpec &T, unsigned OptLevel = 0);

} // namespace gopp

#endif // GOPP_CODEGEN_TARGET_H
