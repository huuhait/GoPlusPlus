//===- ClangInfo.cpp - Linked Clang queries -------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Import/ClangInfo.h"

#include "clang/Basic/Version.h"

std::string gopp::getClangVersion() { return clang::getClangFullVersion(); }
