//===- gopp/CodeGen/CodeGen.h - GIR to objects and programs -----*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.02 / P04.03. The only library that includes LLVM IR headers (D0.2). The
// steps of `gopp build`, each usable alone:
//
//   lowerToLLVM     GIR module -> llvm::Module   (LowerToLLVM.cpp)
//   emitObject / emitAssembly                    (Emit.cpp)
//   findLinkInputs, lldArguments, linkExecutable (Linker.cpp; LLD runs as
//                                                 `gopp lld`)
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_CODEGEN_CODEGEN_H
#define GOPP_CODEGEN_CODEGEN_H

#include "gopp/CodeGen/Target.h"
#include "gopp/GIR/GIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
class raw_ostream;
class raw_pwrite_stream;
} // namespace llvm

namespace gopp {

/// The work of the FMA analysis (test-only, manager decision M8 of P04.04):
/// the near-linear tests count these steps, never time.
struct LowerWorkCounts {
  uint64_t FmaDomSteps = 0;  // the dominator tree and its scoped walk
  uint64_t FmaSccSteps = 0;  // the strongly connected components
  uint64_t FmaDefSteps = 0;  // the index of the writes
  uint64_t FmaWalkSteps = 0; // the walks that tell which writes lie between
};

struct LowerToLLVMOptions {
  /// DWARF line tables (D4.7). Variables and types come with P04.12.
  bool DebugInfo = true;
  /// The target's CPU and features, for the C ABI classification (P04.11);
  /// lowerToLLVM fills them from the TargetMachine.
  std::string TargetCPU, TargetFeatures;
  /// Test-only work counter (M8) of the C ABI (P04.11): the aggregates whose
  /// layout is checked against Clang's, the signatures classified and the
  /// GIR types mapped to Clang, the IR arguments of the calls of C.
  uint64_t *CABIWork = nullptr;
  /// If set, the lowering adds its work to it (tests only).
  LowerWorkCounts *Counts = nullptr;
};

/// Lowers a GIR module. The result has the target's triple and data layout.
/// The GIR verifier runs first and the LLVM verifier last; either failure is
/// an internal error. Fails (with the construct named) on GIR the code
/// generator cannot lower yet: drops before P04.09, cleanup edges and
/// `terminate` unwind actions before P04.07.
llvm::Expected<std::unique_ptr<llvm::Module>>
lowerToLLVM(llvm::LLVMContext &Ctx, const gir::Module &M,
            const llvm::TargetMachine &TM, const LowerToLLVMOptions &Opts = {});

/// Replaces Path with what Write produces, all or nothing: Write fills a
/// temporary file in Path's directory, and the rename onto Path happens only
/// if Write succeeded. On failure Path is as it was and no temporary file
/// stays. Every `-emit` kind that writes `-o` goes through this (R4).
llvm::Error writeFileAtomic(
    llvm::StringRef Path,
    llvm::function_ref<llvm::Error(llvm::raw_pwrite_stream &)> Write);

/// Writes the module as an object file (the LLVM module is verified first;
/// an invalid one leaves Path untouched) / as assembly text.
llvm::Error emitObject(llvm::Module &M, llvm::TargetMachine &TM,
                       llvm::StringRef Path);
llvm::Error emitAssembly(llvm::Module &M, llvm::TargetMachine &TM,
                         llvm::raw_ostream &OS);

/// What the link step needs besides the object file (P00.12, D4.9).
struct LinkInputs {
  std::string Crt1, Crti, Crtn;
  std::string DynamicLinker;
  /// Directories searched for libc (`-L`).
  std::vector<std::string> LibDirs;
  std::vector<std::string> Libs = {"c"};
};

/// Fills Inputs for the target from the host probe (a native build), or from
/// CrtDir (a directory with crt1.o, crti.o, crtn.o and libc; used to link for
/// a target other than the host, and by the tests). DynamicLinker, if not
/// empty, replaces the one found.
llvm::Expected<LinkInputs> findLinkInputs(const TargetSpec &T,
                                          llvm::StringRef CrtDir,
                                          llvm::StringRef DynamicLinker);

/// The `ld.lld` command line for linking Objects into Output.
std::vector<std::string> lldArguments(const TargetSpec &T, const LinkInputs &In,
                                      llvm::ArrayRef<std::string> Objects,
                                      llvm::StringRef Output);

/// Runs the bundled LLD in this process: `gopp lld ...` (D4.9). Argv is the
/// arguments after `lld`. Returns the exit code.
int runLLD(llvm::ArrayRef<std::string> Argv);

/// Links with the LLD bundled in the program Self (`Self lld ...` runs as a
/// subprocess, so a linker crash or exit cannot take the compiler with it).
llvm::Error linkExecutable(llvm::StringRef Self, const TargetSpec &T,
                           const LinkInputs &In,
                           llvm::ArrayRef<std::string> Objects,
                           llvm::StringRef Output);

} // namespace gopp

#endif // GOPP_CODEGEN_CODEGEN_H
