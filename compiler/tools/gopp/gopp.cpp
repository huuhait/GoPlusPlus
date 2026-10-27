//===- gopp.cpp - The gopp command line driver ----------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Subcommands are llvm::cl::SubCommand objects, as in llvm-pdbutil.
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Version.h"
#include "gopp/Build/HostToolchain.h"
#include "gopp/CodeGen/Linker.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/Import/ClangInfo.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

static cl::SubCommand VersionCmd("version",
                                 "Print the Go++, LLVM and target versions");
static cl::SubCommand
    HostToolchainCmd("host-toolchain",
                     "Print the discovered host C library (headers, crt "
                     "objects, dynamic linker)");

static int runVersion() {
  outs() << "gopp version " << gopp::getGoppVersion() << '\n'
         << "LLVM version " << gopp::getLLVMVersion() << '\n'
         << "Default target: " << gopp::getDefaultTargetTriple()
         << '\n'
         // The Clang and LLD libraries linked into gopp (D0.2).
         << "Clang: " << gopp::getClangVersion() << '\n'
         << "LLD: " << gopp::getLLDVersion() << '\n';
  return 0;
}

static int runHostToolchain() {
  Expected<gopp::HostToolchain> TC = gopp::probeHostToolchain();
  if (!TC) {
    errs() << "gopp: error: " << toString(TC.takeError()) << '\n';
    return 1;
  }
  TC->print(outs());
  return 0;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Go++ compiler driver\n");

  if (VersionCmd)
    return runVersion();
  if (HostToolchainCmd)
    return runHostToolchain();

  cl::PrintHelpMessage();
  return 1;
}
