//===- gopp/Basic/Version.h - Go++ version ----------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BASIC_VERSION_H
#define GOPP_BASIC_VERSION_H

#include <string>

namespace gopp {

/// The Go++ version, e.g. "0.0.1".
std::string getGoppVersion();

} // namespace gopp

#endif // GOPP_BASIC_VERSION_H
