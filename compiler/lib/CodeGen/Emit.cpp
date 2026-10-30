//===- Emit.cpp - Objects and assembly from an LLVM module ----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/CodeGen.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

using namespace gopp;

static llvm::Error verify(llvm::Module &M) {
  std::string Problems;
  llvm::raw_string_ostream PS(Problems);
  if (llvm::verifyModule(M, &PS))
    return llvm::createStringError(std::errc::invalid_argument,
                                   "the generated LLVM IR is invalid: %s",
                                   Problems.c_str());
  return llvm::Error::success();
}

static llvm::Error emit(llvm::Module &M, llvm::TargetMachine &TM,
                        llvm::raw_pwrite_stream &OS, llvm::CodeGenFileType FT) {
  llvm::legacy::PassManager PM;
  if (TM.addPassesToEmitFile(PM, OS, nullptr, FT))
    return llvm::createStringError(std::errc::not_supported,
                                   "the target cannot emit this file type");
  PM.run(M);
  return llvm::Error::success();
}

llvm::Error gopp::writeFileAtomic(
    llvm::StringRef Path,
    llvm::function_ref<llvm::Error(llvm::raw_pwrite_stream &)> Write) {
  // Everything is made in memory first: a failure touches nothing.
  llvm::SmallString<0> Buf;
  llvm::raw_svector_ostream BufOS(Buf);
  if (llvm::Error E = Write(BufOS))
    return E;
  auto Put = [&](llvm::raw_ostream &OS) {
    OS << Buf;
    return llvm::Error::success();
  };
  // "-" and /dev/null: LLVM's writeToOutput knows them.
  if (Path == "-" || Path == "/dev/null")
    return llvm::writeToOutput(Path, Put);
  // A symlink: write the file it points to, keep the link.
  llvm::SmallString<256> Real;
  if (!llvm::sys::fs::real_path(Path, Real))
    Path = Real;
  // A device, a pipe: write it in place; renaming over it would replace it.
  llvm::sys::fs::file_status St;
  if (!llvm::sys::fs::status(Path, St) && llvm::sys::fs::exists(St) &&
      !llvm::sys::fs::is_regular_file(St)) {
    std::error_code EC;
    llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_None);
    if (EC)
      return llvm::createStringError(EC, "cannot write %s: %s",
                                     Path.str().c_str(), EC.message().c_str());
    OS << Buf;
    OS.close();
    if (OS.has_error()) {
      std::error_code E2 = OS.error();
      OS.clear_error();
      return llvm::createStringError(E2, "cannot write %s: %s",
                                     Path.str().c_str(), E2.message().c_str());
    }
    return llvm::Error::success();
  }
  // A regular file, or none: a temporary file next to it, then a rename.
  return llvm::writeToOutput(Path, Put);
}

llvm::Error gopp::emitObject(llvm::Module &M, llvm::TargetMachine &TM,
                             llvm::StringRef Path) {
  // Nothing is written, and an old Path is not touched, unless the module is
  // valid and the whole object is made.
  if (llvm::Error E = verify(M))
    return E;
  return writeFileAtomic(Path, [&](llvm::raw_pwrite_stream &OS) {
    return emit(M, TM, OS, llvm::CodeGenFileType::ObjectFile);
  });
}

llvm::Error gopp::emitAssembly(llvm::Module &M, llvm::TargetMachine &TM,
                               llvm::raw_ostream &OS) {
  llvm::SmallString<0> Buf;
  llvm::raw_svector_ostream SOS(Buf);
  if (llvm::Error E = verify(M))
    return E;
  if (llvm::Error E = emit(M, TM, SOS, llvm::CodeGenFileType::AssemblyFile))
    return E;
  OS << Buf;
  return llvm::Error::success();
}
