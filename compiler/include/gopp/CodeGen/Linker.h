//===- gopp/CodeGen/Linker.h - Linked LLD queries ---------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_CODEGEN_LINKER_H
#define GOPP_CODEGEN_LINKER_H

#include <string>

namespace gopp {

/// The version of the LLD library linked into gopp, as LLD reports it
/// (e.g. "LLD 23.1.0").
std::string getLLDVersion();

} // namespace gopp

#endif // GOPP_CODEGEN_LINKER_H
