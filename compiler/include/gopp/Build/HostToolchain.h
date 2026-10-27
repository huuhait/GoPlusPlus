//===- gopp/Build/HostToolchain.h - Host sysroot discovery ------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// D0.12: the Clang and LLD embedded in gopp do not use the Nix clang wrapper,
// so they cannot find libc headers, crt objects or the dynamic linker by
// themselves (NixOS has no /usr/include). We ask the system C compiler once
// and record the answers.
//
// Strategy, in order:
//   1. Ask the system C compiler (`cc`, run with LC_ALL=C): `-dumpmachine`,
//      `-print-file-name=crt1.o` (and crti.o, crtn.o), `-###` for the
//      dynamic linker its own link line uses (else
//      `-print-file-name=<loader>`), `-E -v -x c /dev/null` for the include
//      dirs. Paths are lexically normalized (no `..`).
//   2. If there is no `cc` but NIX_CFLAGS_COMPILE / NIX_LDFLAGS are set, read
//      them instead.
//   3. Otherwise fail with an error. The bundled static musl fallback of
//      D0.12 does not exist yet (the runtime arrives in P04/P05), so in P00
//      the fallback is "report a clear error and exit non-zero", never a
//      silent guess.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BUILD_HOSTTOOLCHAIN_H
#define GOPP_BUILD_HOSTTOOLCHAIN_H

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace gopp {

/// Everything the link step needs to know about the host C library.
struct HostToolchain {
  enum class Source { CompilerQuery, NixEnvironment };

  Source Origin = Source::CompilerQuery;
  /// The C compiler that was asked (empty for NixEnvironment).
  std::string Compiler;
  /// Machine triple reported by the compiler (empty for NixEnvironment).
  std::string Machine;
  std::vector<std::string> IncludeDirs;
  std::string Crt1, Crti, Crtn;
  std::string DynamicLinker;

  /// Prints `key: value` lines (stable format, used by lit tests).
  void print(llvm::raw_ostream &OS) const;
};

/// Output of one compiler invocation.
struct CommandOutput {
  std::string Stdout;
  std::string Stderr;
};

/// Runs `argv` (argv[0] is the program) and captures its output. Injectable so
/// the probe can be tested with canned compiler output.
using CommandRunner = llvm::function_ref<llvm::Expected<CommandOutput>(
    llvm::ArrayRef<std::string>)>;

/// Include dirs from the stderr of `cc -E -v -x c /dev/null`: the lines
/// between "#include <...> search starts here:" and "End of search list.",
/// normalized.
std::vector<std::string> parseIncludeDirs(llvm::StringRef CompilerStderr);

/// The dynamic linker on the link line that `cc -### ...` prints (stderr).
/// Understands `-dynamic-linker X`, `-dynamic-linker=X` and the `--`
/// spellings; the last one wins, as in ld. Returns "" if there is none.
std::string parseDynamicLinker(llvm::StringRef DriverTrace);

/// Result of `cc -print-file-name=Name`. Compilers echo `Name` back when the
/// file is not found, so only an absolute path counts. Returns "" if not found.
std::string parsePrintFileName(llvm::StringRef Stdout);

/// The file name of the glibc dynamic linker for a machine triple such as
/// "x86_64-linux-gnu"; "" if the architecture is not supported yet.
std::string dynamicLinkerFileName(llvm::StringRef MachineTriple);

/// Fallback 2: derive the answer from the Nix cc-wrapper variables.
llvm::Expected<HostToolchain>
probeFromNixEnvironment(llvm::StringRef NixCFlagsCompile,
                        llvm::StringRef NixLdFlags, llvm::StringRef Machine,
                        llvm::function_ref<bool(llvm::StringRef)> FileExists);

/// Strategy 1 with an injected runner.
llvm::Expected<HostToolchain> probeWithCompiler(llvm::StringRef Compiler,
                                                CommandRunner Run);

/// The full probe on the real system (strategies 1 and 2).
llvm::Expected<HostToolchain> probeHostToolchain();

} // namespace gopp

#endif // GOPP_BUILD_HOSTTOOLCHAIN_H
