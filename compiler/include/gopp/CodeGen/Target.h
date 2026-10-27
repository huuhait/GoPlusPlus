//===- gopp/CodeGen/Target.h - LLVM target queries --------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_CODEGEN_TARGET_H
#define GOPP_CODEGEN_TARGET_H

#include <string>

namespace gopp {

/// The version of the LLVM that gopp is built against, e.g. "23.1.0".
std::string getLLVMVersion();

/// The default target triple of the host, e.g. "x86_64-unknown-linux-gnu".
std::string getDefaultTargetTriple();

} // namespace gopp

#endif // GOPP_CODEGEN_TARGET_H
