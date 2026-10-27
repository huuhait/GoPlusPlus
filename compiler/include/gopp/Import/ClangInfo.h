//===- gopp/Import/ClangInfo.h - Linked Clang queries -----------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_IMPORT_CLANGINFO_H
#define GOPP_IMPORT_CLANGINFO_H

#include <string>

namespace gopp {

/// The version of the Clang library linked into gopp, as Clang reports it
/// (e.g. "clang version 23.1.0").
std::string getClangVersion();

} // namespace gopp

#endif // GOPP_IMPORT_CLANGINFO_H
