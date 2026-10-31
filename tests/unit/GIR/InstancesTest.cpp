//===- InstancesTest.cpp - The instance walk of generic code --------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.08, D4.3: the checker facade the instance walk decides with
// (MethodSets::extent and the Option and View predicates), with both sides of
// each rule, and the walk itself (lib/GIR/Instances.cpp) as `gopp check` runs
// it (gir::checkInstances) and as the lowering does.
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/GIR/GIR.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/Loader.h"
#include "gopp/Sema/Sema.h"

#include "llvm/Support/FileSystem.h"

#include <gtest/gtest.h>

#include <fstream>

using namespace gopp;
using namespace gopp::sema;

namespace {

/// One package main checked from Source.
struct Pkg {
  explicit Pkg(const std::string &Source) : Diags(SM, Collect, 0) {
    llvm::SmallString<128> D;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-inst", D));
    Dir = std::string(D);
    std::ofstream(Dir + "/main.gopp", std::ios::binary) << Source;
    LoadOptions LO;
    LO.Root = Dir;
    Ld = std::make_unique<Loader>(SM, Diags, Ctx, LO, CheckOptions());
    CP = Ld->loadDir(Dir, "");
  }
  ~Pkg() { llvm::sys::fs::remove_directories(Dir); }
  std::string errors() const {
    std::string E;
    for (const Diagnostic &D : Collect.Diags)
      if (D.Sev == Severity::Error)
        E += D.Message + "\n";
    return E;
  }
  /// The errors with their line and column: "line:col: message".
  std::string errorsAt() const {
    std::string E;
    for (const Diagnostic &D : Collect.Diags)
      if (D.Sev == Severity::Error)
        E += std::to_string(SM.getLineNumber(D.Loc)) + ":" +
             std::to_string(SM.getColumnNumber(D.Loc)) + ": " + D.Message +
             "\n";
    return E;
  }
  unsigned numErrors() const {
    unsigned N = 0;
    for (const Diagnostic &D : Collect.Diags)
      N += D.Sev == Severity::Error;
    return N;
  }
  /// `gopp check`'s walk.
  bool check(const gir::LowerOptions &Opts = {}) {
    return gir::checkInstances(*CP, Ld->filesOf(CP), Diags, Opts);
  }
  /// The build's walk, with the lowering.
  bool lower(const gir::LowerOptions &Opts = {}) {
    gir::Context C;
    return gir::lowerPackage(C, SM, *CP, Ld->filesOf(CP), Diags, Opts) !=
           nullptr;
  }
  std::string Dir;
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags;
  TypeContext Ctx;
  std::unique_ptr<Loader> Ld;
  CheckedPackage *CP = nullptr;
};

const char *Empty = "package main\nfunc main() {}\n";

} // namespace

TEST(GIRInstances, TheExtentOfATypeIsItsHeightAndItsSizeAsATree) {
  Pkg P(Empty);
  ASSERT_EQ(P.errors(), "");
  MethodSets MS(*P.CP);
  Type *Int = P.Ctx.typ(BasicKind::Int);
  MethodSets::Extent One = MS.extent(Int);
  EXPECT_EQ(One.Height, 1u);
  EXPECT_EQ(One.Size, 1u);
  Type *T = Int;
  for (int I = 0; I < 100; ++I)
    T = P.Ctx.slice(T);
  MethodSets::Extent Deep = MS.extent(T);
  EXPECT_EQ(Deep.Height, 101u);
  EXPECT_EQ(Deep.Size, 101u);
  // A diamond: each map holds the one before as key and as value, so the tree
  // has 2^n leaves; the memo visits n types and the size saturates.
  Type *D = Int;
  for (int I = 0; I < 200; ++I)
    D = P.Ctx.map(D, D);
  MethodSets::Extent Dia = MS.extent(D);
  EXPECT_EQ(Dia.Height, 201u);
  EXPECT_GE(Dia.Size, MethodSets::SizeCap);
}

// The walk's Option and View rules are the checker's predicates on the
// instance's types (Instances.cpp, meet). The View side is not reachable
// from source past the checker (its use tracking reports E0371 first), so it
// is tested here.
TEST(GIRInstances, TheOptionAndViewRulesAreTheCheckersPredicates) {
  Pkg P(Empty);
  ASSERT_EQ(P.errors(), "");
  MethodSets MS(*P.CP);
  Type *Int = P.Ctx.typ(BasicKind::Int);
  EXPECT_EQ(MS.badOptionArg(Int), nullptr);
  EXPECT_EQ(MS.badOptionArg(P.Ctx.pointer(Int)), nullptr);
  EXPECT_NE(MS.badOptionArg(P.Ctx.option(Int)), nullptr);
  EXPECT_NE(MS.badOptionArg(P.Ctx.slice(Int)), nullptr);
  EXPECT_EQ(MS.badViewArg(Int), nullptr);
  EXPECT_EQ(MS.badViewArg(P.Ctx.slice(Int)), nullptr);
  EXPECT_NE(MS.badViewArg(P.Ctx.pointer(Int)), nullptr);
  EXPECT_NE(MS.badViewArg(P.Ctx.ref(Int)), nullptr);
}

TEST(GIRInstances, AnOptionOfAnOptionIsOneErrorAtTheInstantiation) {
  Pkg P("package main\n"
        "func Count[T any](xs []T) int {\n"
        "\tvar a []Option[T]\n\tvar b []Option[T]\n\treturn len(xs) + len(a) + "
        "len(b)\n}\n"
        "func main() {\n\tprintln(Count([]Option[int]{}))\n}\n");
  ASSERT_EQ(P.errors(), "");
  EXPECT_FALSE(P.check());
  EXPECT_EQ(P.numErrors(), 1u) << P.errors(); // two uses, one error
  EXPECT_NE(P.errors().find("Option[Option[int]]"), std::string::npos);
}

TEST(GIRInstances, TheSameBodyIsFineForOtherTypeArguments) {
  Pkg P("package main\n"
        "func Count[T any](xs []T) int {\n"
        "\tvar a []Option[T]\n\treturn len(xs) + len(a)\n}\n"
        "func main() {\n\tprintln(Count([]int{1}), Count([]*int{}), "
        "Count([]string{}))\n}\n");
  ASSERT_EQ(P.errors(), "");
  EXPECT_TRUE(P.check());
  EXPECT_TRUE(P.lower()) << P.errors();
}

TEST(GIRInstances, AnInstanceOfAnInstanceIsMadeOnce) {
  // F calls G calls F with the same type argument: the walk ends (each
  // instance is made once) and finds the error where the chain starts.
  Pkg P("package main\n"
        "func F[T any](xs []T, n int) int {\n\tif n == 0 {\n\t\treturn 0\n\t}\n"
        "\treturn G(xs, n-1)\n}\n"
        "func G[T any](xs []T, n int) int {\n\tvar a []Option[T]\n"
        "\treturn len(a) + F(xs, n-1)\n}\n"
        "func main() {\n\tprintln(F([]int{}, 3))\n\tprintln(F([]Option[int]{}, "
        "3))\n}\n");
  ASSERT_EQ(P.errors(), "");
  EXPECT_FALSE(P.check());
  EXPECT_EQ(P.numErrors(), 1u) << P.errors();
  EXPECT_NE(P.errors().find("G[Option[int]]"), std::string::npos) << P.errors();
}

// M2: a limit is one error, never a silent stop, and the walk stops at it. A
// chain of N generics that each call the one below twice with two different
// bigger type arguments reaches 2^N instances.
static std::string chainOf(unsigned N) {
  std::string S = "package main\n\ntype P[A, B any] struct {\n\ta []A\n\tb "
                  "[]B\n}\n\nfunc F0[T any](x []T) int { return len(x) }\n";
  for (unsigned I = 1; I <= N; ++I)
    S += "func F" + std::to_string(I) + "[T any](x []T) int { return F" +
         std::to_string(I - 1) + "([]P[T, int]{}) + F" + std::to_string(I - 1) +
         "([]P[int, T]{}) }\n";
  return S + "func main() { println(F" + std::to_string(N) + "([]int{})) }\n";
}

TEST(GIRInstances, TheInstanceLimitIsOneErrorAndStopsTheWalk) {
  gir::LowerOptions Opts;
  Opts.MaxInstances = 200;
  {
    Pkg P(chainOf(40));
    ASSERT_EQ(P.errors(), "");
    EXPECT_FALSE(P.check(Opts));
    EXPECT_EQ(P.numErrors(), 1u) << P.errors();
    // (The limit is the source size here: more than the 200 asked for.)
    EXPECT_NE(P.errors().find("the package needs more than "),
              std::string::npos)
        << P.errors();
  }
  {
    // The build's walk says the same, once, and lowers nothing.
    Pkg P(chainOf(40));
    ASSERT_EQ(P.errors(), "");
    EXPECT_FALSE(P.lower(Opts));
    EXPECT_EQ(P.numErrors(), 1u) << P.errors();
  }
  {
    // The other side: 2^7 - 1 + 7 instances are under the limit.
    Pkg P(chainOf(6));
    ASSERT_EQ(P.errors(), "");
    EXPECT_TRUE(P.check(Opts)) << P.errors();
    EXPECT_TRUE(P.lower(Opts)) << P.errors();
  }
}

// The doubling chain of chainOf(N) with a body of Stmts statements in F0:
// 2^(N-1) instances of F0 (the first level's two type arguments are one), each
// as large as its body.
static std::string chainWithBody(unsigned N, unsigned Stmts) {
  const std::string Short = "func F0[T any](x []T) int { return len(x) }\n";
  std::string Body = "func F0[T any](x []T) int {\n\ts := 0\n";
  for (unsigned I = 0; I < Stmts; ++I)
    Body += "\ts += len(x) + " + std::to_string(I) + "\n";
  Body += "\treturn s\n}\n";
  std::string S = chainOf(N);
  return S.replace(S.find(Short), Short.size(), Body);
}

// Review R4: the code the instances hold is bounded too, not only their
// number: a short file of 2^n instances of one large body is one error
// (E0449, the same from `gopp check` and the build, at the same place) when
// the walk passes max(MaxWalkedNodes, 16 per byte of source) nodes, and the
// walk stops there (its steps stay near the budget). The other side: a
// smaller chain over the same body is under it.
TEST(GIRInstances, TheWorkBudgetIsOneErrorAndStopsTheWalk) {
  gir::LowerOptions Opts;
  Opts.MaxWalkedNodes = 1000; // the source's 16 per byte is the budget
  const std::string Big = chainWithBody(12, 40);
  // (2^11 instances of a 40-statement body: about 600000 nodes, against
  // 16 per byte of a file of under 4 KB.)
  ASSERT_LT(Big.size(), 4096u);
  std::string CheckErr;
  {
    Pkg P(Big);
    ASSERT_EQ(P.errors(), "");
    uint64_t Before = gir::instanceSteps();
    EXPECT_FALSE(P.check(Opts));
    uint64_t Steps = gir::instanceSteps() - Before;
    EXPECT_EQ(P.numErrors(), 1u) << P.errors();
    EXPECT_NE(P.errors().find("the instances of the package hold more than " +
                              std::to_string(16 * Big.size()) +
                              " nodes of code"),
              std::string::npos)
        << P.errors();
    EXPECT_LT(Steps, 4 * 16 * Big.size()) << "the walk went on past the budget";
    CheckErr = P.errorsAt();
  }
  {
    // The build's walk says the same, at the same place, and lowers nothing.
    Pkg P(Big);
    ASSERT_EQ(P.errors(), "");
    EXPECT_FALSE(P.lower(Opts));
    EXPECT_EQ(P.numErrors(), 1u) << P.errors();
    EXPECT_EQ(P.errorsAt(), CheckErr);
  }
  {
    // The other side: 2^5 instances of the body are under it.
    Pkg P(chainWithBody(6, 40));
    ASSERT_EQ(P.errors(), "");
    EXPECT_TRUE(P.check(Opts)) << P.errors();
    EXPECT_TRUE(P.lower(Opts)) << P.errors();
  }
}

// M3: `gopp check` finds the error of a method that only an itab needs, as
// the build does (Info::IfaceConversions).
TEST(GIRInstances, CheckFindsAMethodThatOnlyAnItabNeeds) {
  const char *Src =
      "package main\n"
      "type W[T any] struct{ n int }\n"
      "func (w W[T]) Show() int {\n\tvar o Option[T]\n\t_ = o\n\treturn "
      "w.n\n}\n"
      "type Shower interface{ Show() int }\n"
      "func main() {\n\tvar s Shower = W[%s]{1}\n\tprintln(s.Show())\n}\n";
  char Bad[512], Good[512];
  snprintf(Bad, sizeof Bad, Src, "Option[int]");
  snprintf(Good, sizeof Good, Src, "int");
  {
    Pkg P(Bad);
    ASSERT_EQ(P.errors(), "");
    EXPECT_FALSE(P.check());
    EXPECT_NE(
        P.errors().find("cannot instantiate W[Option[int]].Show: its body "
                        "makes Option[Option[int]]"),
        std::string::npos)
        << P.errors();
  }
  {
    Pkg P(Good);
    ASSERT_EQ(P.errors(), "");
    EXPECT_TRUE(P.check());
    EXPECT_TRUE(P.lower()) << P.errors();
  }
}
