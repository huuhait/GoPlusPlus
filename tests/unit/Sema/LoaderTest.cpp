//===- LoaderTest.cpp - Package loader tests (P03.01) ---------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Build constraints, file-name suffixes, _test files, internal/ packages,
// import cycles and the foreign (mock) header packages. Each test makes a
// small package tree in a temporary directory.
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Loader.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace gopp;
using namespace gopp::sema;

namespace {

/// A temporary directory with files written into it.
class Tree {
public:
  Tree() {
    llvm::SmallString<128> P;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-loader", P));
    Root = std::string(P);
  }
  ~Tree() { llvm::sys::fs::remove_directories(Root); }
  void write(const std::string &Rel, const std::string &Text) {
    std::string Path = Root + "/" + Rel;
    llvm::sys::fs::create_directories(llvm::sys::path::parent_path(Path));
    std::ofstream Out(Path);
    Out << Text;
  }
  std::string Root;
};

struct Result {
  bool Loaded = false;
  std::vector<std::string> Errors;
  std::vector<std::string> Files; // selected files of the root package
  bool has(const std::string &Sub) const {
    for (const std::string &E : Errors)
      if (E.find(Sub) != std::string::npos)
        return true;
    return false;
  }
};

Result load(Tree &T, LoadOptions LO = {}, ForeignProvider *FP = nullptr) {
  Result R;
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  TypeContext Ctx;
  LO.Root = T.Root;
  Loader L(SM, Diags, Ctx, LO, {});
  if (FP)
    L.setForeignProvider(FP);
  CheckedPackage *CP = L.loadDir(T.Root, "");
  R.Loaded = CP != nullptr;
  for (const Diagnostic &D : Collect.Diags)
    if (D.Sev == Severity::Error)
      R.Errors.push_back(D.Message);
  std::vector<std::string> Sel;
  if (L.selectFiles(T.Root, Sel))
    for (const std::string &F : Sel)
      R.Files.push_back(llvm::sys::path::filename(F).str());
  return R;
}

TEST(Loader, FileNameSuffixesSelectOSAndArch) {
  Tree T;
  T.write("a.gopp", "package p\n");
  T.write("a_linux.gopp", "package p\n");
  T.write("a_windows.gopp", "package p\n");
  T.write("a_amd64.gopp", "package p\n");
  T.write("a_arm64.gopp", "package p\n");
  T.write("a_linux_amd64.gopp", "package p\n");
  T.write("a_windows_amd64.gopp", "package p\n");
  Result R = load(T);
  EXPECT_TRUE(R.Errors.empty());
  EXPECT_EQ(R.Files,
            (std::vector<std::string>{"a.gopp", "a_amd64.gopp", "a_linux.gopp",
                                      "a_linux_amd64.gopp"}));
}

TEST(Loader, TestFilesOnlyOnRequest) {
  Tree T;
  T.write("a.gopp", "package p\n");
  T.write("a_test.gopp", "package p\n");
  EXPECT_EQ(load(T).Files, (std::vector<std::string>{"a.gopp"}));
  LoadOptions LO;
  LO.IncludeTests = true;
  EXPECT_EQ(load(T, LO).Files,
            (std::vector<std::string>{"a.gopp", "a_test.gopp"}));
}

TEST(Loader, BuildConstraintsUseProfileAndTags) {
  Tree T;
  T.write("always.gopp", "package p\n");
  T.write("dev.gopp", "//gopp:build dev\n\npackage p\n");
  T.write("release.gopp", "//gopp:build release\n\npackage p\n");
  T.write("notdev.gopp", "//gopp:build !dev && linux\n\npackage p\n");
  T.write("tagged.gopp",
          "//gopp:build (fast || slow) && !windows\n\npackage p\n");
  Result R = load(T);
  EXPECT_EQ(R.Files, (std::vector<std::string>{"always.gopp", "dev.gopp"}));
  LoadOptions LO;
  LO.Profile = "release";
  LO.Tags = {"fast"};
  EXPECT_EQ(load(T, LO).Files,
            (std::vector<std::string>{"always.gopp", "notdev.gopp",
                                      "release.gopp", "tagged.gopp"}));
}

TEST(Loader, BadConstraintIsReported) {
  Tree T;
  T.write("a.gopp", "//gopp:build (dev &&\n\npackage p\n");
  Result R = load(T);
  EXPECT_FALSE(R.Errors.empty());
}

TEST(Loader, ImportsAreResolvedAndChecked) {
  Tree T;
  T.write("main.gopp", "package main\nimport \"lib\"\nvar X int = lib.F()\n");
  T.write("lib/lib.gopp", "package lib\nfunc F() int { return 1 }\n");
  Result R = load(T);
  EXPECT_TRUE(R.Loaded);
  EXPECT_TRUE(R.Errors.empty());
}

TEST(Loader, TypeErrorInImportedPackageIsReported) {
  Tree T;
  T.write("main.gopp", "package main\nimport \"lib\"\nvar X = lib.F()\n");
  T.write("lib/lib.gopp", "package lib\nfunc F() int { return \"s\" }\n");
  Result R = load(T);
  EXPECT_FALSE(R.Errors.empty());
}

TEST(Loader, ImportCycleIsAnError) {
  Tree T;
  T.write("main.gopp", "package main\nimport \"a\"\nvar _ = a.X\n");
  T.write("a/a.gopp", "package a\nimport \"b\"\nvar X = b.Y\n");
  T.write("b/b.gopp", "package b\nimport \"a\"\nvar Y = a.X\n");
  EXPECT_TRUE(load(T).has("import cycle"));
}

TEST(Loader, MissingPackageIsAnError) {
  Tree T;
  T.write("main.gopp", "package main\nimport \"nothere\"\nvar _ = nothere.X\n");
  EXPECT_FALSE(load(T).Errors.empty());
}

TEST(Loader, DirectoryWithoutSourcesIsAnError) {
  Tree T;
  T.write("readme.txt", "x");
  Result R = load(T);
  EXPECT_FALSE(R.Loaded);
  EXPECT_TRUE(R.has("no Go++ source files"));
}

TEST(Loader, MixedPackageNamesAreAnError) {
  Tree T;
  T.write("a.gopp", "package a\n");
  T.write("b.gopp", "package b\n");
  EXPECT_TRUE(load(T).has("package"));
}

TEST(Loader, InternalPackagesAreVisibleToTheirParentOnly) {
  Tree T;
  T.write("main.gopp", "package main\nimport \"x/internal/h\"\nvar _ = h.V\n");
  T.write("x/internal/h/h.gopp", "package h\nvar V = 1\n");
  // main is outside x/: not allowed.
  EXPECT_TRUE(load(T).has("internal"));

  Tree T2;
  T2.write("x/main.gopp", "package x\nimport \"x/internal/h\"\nvar _ = h.V\n");
  T2.write("x/internal/h/h.gopp", "package h\nvar V = 1\n");
  LoadOptions LO;
  LO.ModulePath = "";
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  TypeContext Ctx;
  LO.Root = T2.Root;
  Loader L(SM, Diags, Ctx, LO, {});
  EXPECT_NE(L.loadDir(T2.Root + "/x", "x"), nullptr);
  EXPECT_FALSE(Diags.hasErrors());
}

TEST(Loader, ForeignHeaderPackagesComeFromTheProvider) {
  Tree T;
  T.write("main.gopp",
          "package main\nimport \"<vec>\"\nvar N int = vec.size(2)\n"
          "var Bad string = vec.size(2)\n");
  MockForeignProvider Provider;
  Arena Mem;
  TypeContext Ctx; // only to build the signature below
  auto *F = Mem.make<Func>();
  F->Name = "size";
  F->Ty = Ctx.signature(Ctx.tuple({Ctx.typ(BasicKind::Int)}),
                        Ctx.tuple({Ctx.typ(BasicKind::Int)}), false, false, 0);
  Provider.scopeOf("<vec>").add(F);
  // The signature belongs to another TypeContext than the checker's, which is
  // fine for a type made of basic types only.
  Result R = load(T, {}, &Provider);
  EXPECT_TRUE(R.Loaded);
  ASSERT_EQ(R.Errors.size(), 1u);
  EXPECT_NE(R.Errors[0].find("cannot use"), std::string::npos);
}

TEST(Loader, ForeignHeaderWithoutProviderIsAnError) {
  Tree T;
  T.write("main.gopp", "package main\nimport \"<vec>\"\nvar _ = vec.X\n");
  EXPECT_FALSE(load(T).Errors.empty());
}

TEST(Loader, ForeignNamesIgnoreTheExportRule) {
  Tree T;
  T.write("main.gopp",
          "package main\nimport \"<vec>\"\nvar N int = vec.lower(1)\n");
  MockForeignProvider Provider;
  Arena Mem;
  TypeContext Ctx;
  auto *F = Mem.make<Func>();
  F->Name = "lower";
  F->Ty = Ctx.signature(Ctx.tuple({Ctx.typ(BasicKind::Int)}),
                        Ctx.tuple({Ctx.typ(BasicKind::Int)}), false, false, 0);
  Provider.scopeOf("<vec>").add(F);
  EXPECT_TRUE(load(T, {}, &Provider).Errors.empty());
  // The same name in a source package is unexported.
  Tree T2;
  T2.write("main.gopp",
           "package main\nimport \"lib\"\nvar N int = lib.lower(1)\n");
  T2.write("lib/lib.gopp", "package lib\nfunc lower(x int) int { return x }\n");
  EXPECT_FALSE(load(T2).Errors.empty());
}

} // namespace
