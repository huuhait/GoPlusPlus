//===- FuzzCorpusTest.cpp - Replay of the fuzz corpus ---------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every file of tests/fuzz/corpus (the seeds) and tests/fuzz/regressions (one
// file per fixed crash) must pass the fuzz checks. This is the regular-build
// side of P02.10: no libFuzzer needed.
//
//===----------------------------------------------------------------------===//

#include "../../fuzz/FuzzCheck.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "gtest/gtest.h"

#include <algorithm>

namespace {

std::vector<std::string> filesIn(const char *Dir) {
  std::vector<std::string> Files;
  std::error_code EC;
  for (llvm::sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC;
       It.increment(EC)) {
    if (llvm::sys::path::filename(It->path()) != "README.md")
      Files.push_back(It->path());
  }
  std::sort(Files.begin(), Files.end());
  return Files;
}

void replay(const char *Dir, size_t AtLeast) {
  std::vector<std::string> Files = filesIn(Dir);
  EXPECT_GE(Files.size(), AtLeast) << Dir;
  for (const std::string &Path : Files) {
    auto Buf = llvm::MemoryBuffer::getFile(Path);
    ASSERT_TRUE(Buf) << Path;
    std::string Why = gopp::fuzz::checkAll((*Buf)->getBuffer());
    EXPECT_EQ(Why, "") << Path;
  }
}

TEST(FuzzCorpus, SeedsPassTheChecks) { replay(GOPP_FUZZ_CORPUS_DIR, 20); }

TEST(FuzzCorpus, RegressionsPassTheChecks) {
  replay(GOPP_FUZZ_REGRESSIONS_DIR, 0);
}

TEST(FuzzCorpus, EmptyAndTinyInputs) {
  for (const char *S : {"", "\n", "package", "package p", "{", "\xff\xfe",
                        "package p\x00", "/*", "`", "'", "\""})
    EXPECT_EQ(gopp::fuzz::checkAll(llvm::StringRef(S)), "");
}

} // namespace
