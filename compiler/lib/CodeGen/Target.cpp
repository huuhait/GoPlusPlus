//===- Target.cpp - LLVM target queries -----------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/Target.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <mutex>

// The back ends of the targets in the table, and no others.
extern "C" {
void LLVMInitializeX86TargetInfo();
void LLVMInitializeX86Target();
void LLVMInitializeX86TargetMC();
void LLVMInitializeX86AsmPrinter();
void LLVMInitializeAArch64TargetInfo();
void LLVMInitializeAArch64Target();
void LLVMInitializeAArch64TargetMC();
void LLVMInitializeAArch64AsmPrinter();
}

using namespace gopp;
using llvm::Error;
using llvm::Expected;
using llvm::StringRef;

std::string gopp::getLLVMVersion() { return LLVM_VERSION_STRING; }

std::string gopp::getDefaultTargetTriple() {
  return llvm::sys::getDefaultTargetTriple();
}

static TargetSpec linuxAmd64() {
  TargetSpec T;
  T.OS = "linux";
  T.Arch = "amd64";
  T.Triple = "x86_64-unknown-linux-gnu";
  T.CPU = "x86-64";
  T.DataLayout =
      "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:"
      "32:64-S128";
  T.Emulation = "elf_x86_64";
  T.DynamicLinkerFile = "ld-linux-x86-64.so.2";
  T.DefaultDynamicLinker = "/lib64/ld-linux-x86-64.so.2";
  return T;
}

static TargetSpec linuxArm64() {
  TargetSpec T;
  T.OS = "linux";
  T.Arch = "arm64";
  T.Triple = "aarch64-unknown-linux-gnu";
  T.CPU = "generic";
  T.DataLayout =
      "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:"
      "128-n32:64-S128-Fn32";
  T.Emulation = "aarch64linux";
  T.DynamicLinkerFile = "ld-linux-aarch64.so.1";
  T.DefaultDynamicLinker = "/lib/ld-linux-aarch64.so.1";
  return T;
}

Expected<TargetSpec> gopp::parseTarget(StringRef Name) {
  if (Name == "host")
    return hostTarget();
  if (Name == "linux/amd64")
    return linuxAmd64();
  if (Name == "linux/arm64")
    return linuxArm64();
  return llvm::createStringError(
      std::errc::invalid_argument,
      "unsupported target '%s' (supported: linux/amd64, linux/arm64)",
      Name.str().c_str());
}

Expected<TargetSpec> gopp::hostTarget() {
  llvm::Triple H(llvm::sys::getDefaultTargetTriple());
  if (H.isOSLinux() && H.getArch() == llvm::Triple::x86_64)
    return linuxAmd64();
  if (H.isOSLinux() && H.getArch() == llvm::Triple::aarch64)
    return linuxArm64();
  return llvm::createStringError(
      std::errc::not_supported,
      "the host '%s' is not a supported target (linux/amd64, linux/arm64)",
      H.str().c_str());
}

void gopp::initializeTargets() {
  static std::once_flag Once;
  std::call_once(Once, [] {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86Target();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmPrinter();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64Target();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmPrinter();
  });
}

Expected<std::unique_ptr<llvm::TargetMachine>>
gopp::createTargetMachine(const TargetSpec &T, unsigned OptLevel) {
  initializeTargets();
  std::string Err;
  llvm::Triple Tr(T.Triple);
  const llvm::Target *Tg = llvm::TargetRegistry::lookupTarget(Tr, Err);
  if (!Tg)
    return llvm::createStringError(std::errc::not_supported,
                                   "no LLVM back end for %s: %s",
                                   T.Triple.c_str(), Err.c_str());
  llvm::TargetOptions Opts;
  llvm::CodeGenOptLevel OL = OptLevel == 0   ? llvm::CodeGenOptLevel::None
                             : OptLevel == 1 ? llvm::CodeGenOptLevel::Less
                             : OptLevel == 2
                                 ? llvm::CodeGenOptLevel::Default
                                 : llvm::CodeGenOptLevel::Aggressive;
  std::unique_ptr<llvm::TargetMachine> TM(Tg->createTargetMachine(
      Tr, T.CPU, T.Features, Opts, llvm::Reloc::PIC_, std::nullopt, OL));
  if (!TM)
    return llvm::createStringError(std::errc::not_supported,
                                   "cannot create a target machine for %s",
                                   T.Triple.c_str());
  std::string DL = TM->createDataLayout().getStringRepresentation();
  if (DL != T.DataLayout)
    return llvm::createStringError(
        std::errc::not_supported,
        "LLVM %s gives %s the data layout '%s', not the pinned '%s': Go++'s "
        "layouts (D4.5) assume the pinned one; check the change before "
        "updating TargetSpec::DataLayout",
        LLVM_VERSION_STRING, T.Triple.c_str(), DL.c_str(),
        T.DataLayout.c_str());
  return std::move(TM);
}
