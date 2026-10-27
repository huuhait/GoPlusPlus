//===- Version.cpp - Go++ version -----------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Version.h"

#ifndef GOPP_VERSION_STRING
#error "GOPP_VERSION_STRING must be defined by the build system"
#endif

std::string gopp::getGoppVersion() { return GOPP_VERSION_STRING; }
