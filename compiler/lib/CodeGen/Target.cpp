//===- Target.cpp - LLVM target queries -----------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/Target.h"

#include "llvm/Config/llvm-config.h"
#include "llvm/TargetParser/Host.h"

std::string gopp::getLLVMVersion() { return LLVM_VERSION_STRING; }

std::string gopp::getDefaultTargetTriple() {
  return llvm::sys::getDefaultTargetTriple();
}
