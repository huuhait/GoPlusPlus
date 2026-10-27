//===- VersionTest.cpp - Version unit tests -------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Version.h"
#include "gopp/CodeGen/Linker.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/Import/ClangInfo.h"

#include "gtest/gtest.h"

TEST(Version, GoppVersionIsNotEmpty) {
  EXPECT_FALSE(gopp::getGoppVersion().empty());
}

TEST(Version, LLVMVersionIsAtLeast21) {
  std::string V = gopp::getLLVMVersion();
  EXPECT_GE(std::stoi(V), 21) << V;
}

TEST(Version, DefaultTripleHasArchitecture) {
  EXPECT_NE(gopp::getDefaultTargetTriple().find('-'), std::string::npos);
}

// Clang and LLD are linked (statically in Release) and are the same pinned
// version as LLVM (D0.2).
TEST(Version, ClangMatchesLLVM) {
  std::string Clang = gopp::getClangVersion();
  EXPECT_NE(Clang.find("clang version " + gopp::getLLVMVersion()),
            std::string::npos)
      << Clang;
}

TEST(Version, LLDMatchesLLVM) {
  std::string LLD = gopp::getLLDVersion();
  EXPECT_NE(LLD.find("LLD " + gopp::getLLVMVersion()), std::string::npos)
      << LLD;
}
