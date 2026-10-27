//===- Linker.cpp - Linked LLD queries ------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/Linker.h"

#include <string>

// Declared in lld/Common/Version.h, defined in lldCommon. That header cannot be
// included: it needs the generated lld/Common/Version.inc, which the nixpkgs
// lld dev output does not install.
namespace lld {
std::string getLLDVersion();
} // namespace lld

std::string gopp::getLLDVersion() { return lld::getLLDVersion(); }
