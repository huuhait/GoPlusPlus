//===- CorpusTest.cpp - The checker on the fuzz corpus --------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The checker must not crash, hang or recurse without bound on any input the
// parser accepts, however broken: it gets every file of the P02 fuzz corpus
// and regressions, in Go++ and in Go mode, and the files cut at every 7th
// line (half-written programs). Under ASan/UBSan this is the memory-safety
// check of the checker.
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gopp/Sema/Sema.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "gtest/gtest.h"

#include <algorithm>

using namespace gopp;

namespace {

/// Imports fail ("could not import"): the corpus has no packages to load.
class NoImporter : public sema::Importer {
public:
  sema::Package *import(llvm::StringRef, std::string &) override {
    return nullptr;
  }
};

void check(llvm::StringRef Text, bool GoMode) {
  ParseOptions PO;
  PO.CheckLanguageRules = !GoMode;
  PO.AllowGenericMethods = !GoMode;
  PO.AllowNamespaceTypes = !GoMode;
  PO.AllowExtends = !GoMode;
  PO.AllowOperators = !GoMode;
  test::Parsed P(Text, PO);
  sema::TypeContext Ctx(GoMode);
  NoImporter Imp;
  sema::CheckOptions CO;
  CO.GoCompat = GoMode;
  CO.Ownership = !GoMode;
  const ast::File *Files[] = {P.File};
  auto CP = sema::checkPackage(Ctx, P.Src.SM, "p", Files, Imp, P.Src.Diags, CO);
  EXPECT_TRUE(CP != nullptr);
}

std::vector<std::string> filesIn(const char *Dir) {
  std::vector<std::string> Files;
  std::error_code EC;
  for (llvm::sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC;
       It.increment(EC))
    if (llvm::sys::path::filename(It->path()) != "README.md")
      Files.push_back(It->path());
  std::sort(Files.begin(), Files.end());
  return Files;
}

void runAll(const char *Dir) {
  for (const std::string &Path : filesIn(Dir)) {
    SCOPED_TRACE(Path);
    auto Buf = llvm::MemoryBuffer::getFile(Path);
    ASSERT_TRUE(Buf);
    llvm::StringRef Text = (*Buf)->getBuffer();
    check(Text, false);
    check(Text, true);
    // A program cut in the middle, both halves.
    size_t Lines = 0;
    for (size_t I = 0; I < Text.size(); ++I)
      if (Text[I] == '\n' && ++Lines % 7 == 0) {
        check(Text.substr(0, I), false);
        check(Text.substr(I), false);
      }
  }
}

TEST(SemaCorpus, SeedsDoNotCrash) { runAll(GOPP_FUZZ_CORPUS_DIR); }

TEST(SemaCorpus, RegressionsDoNotCrash) { runAll(GOPP_FUZZ_REGRESSIONS_DIR); }

TEST(SemaCorpus, TinyInputs) {
  for (const char *S :
       {"", "\n", "package", "package p", "{", "\xff\xfe", "package p\nfunc",
        "package p\nfunc f(", "package p\ntype", "package p\nvar x = ",
        "package p\nimport", "package p\nfunc (r T) m[P any]()",
        "package p\nfunc f() { for _, &x := range }"}) {
    check(S, false);
    check(S, true);
  }
}

} // namespace
