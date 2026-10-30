//===- DeepTest.cpp - Very long chains in the checker (P03.26) ------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The command line parser stops a tree taller than ParseOptions::MaxHeight
// (1024; `gopp check` does not count a selector on a selector), so the
// checker meets a long chain of calls, indexes, slices or assertions only
// here, with the limit raised. Every such chain is evaluated with a loop
// (Checker::prepareChain), not by recursion: 30000 links of each form. The
// checker runs on threads with a large stack (CheckStackBytes) as a safety
// net; under ASan a recursion per link would still need several times that
// stack, so in the sanitizer build this is the test that the loop is real.
// Star and paren chains are not here: the parser itself recurses on them.
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gopp/Sema/Sema.h"

#include "gtest/gtest.h"

#include <string>

using namespace gopp;

namespace {

class NoImports : public sema::Importer {
public:
  sema::Package *import(llvm::StringRef, std::string &Error) override {
    Error = "no such package";
    return nullptr;
  }
};

/// The errors of the program (parsed with a tree height limit of 1 million).
std::vector<std::string> errorsOf(const std::string &Src) {
  ParseOptions PO;
  PO.MaxHeight = 1u << 20;
  PO.MaxNesting = 1u << 20;
  test::Parsed P(Src, PO);
  sema::TypeContext Ctx;
  NoImports Imp;
  const ast::File *Files[] = {P.File};
  sema::CheckOptions CO;
  auto CP = sema::checkPackage(Ctx, P.Src.SM, "p", Files, Imp, P.Src.Diags, CO);
  EXPECT_TRUE(CP != nullptr);
  return P.errors();
}

std::string repeat(const char *Step, unsigned N) {
  std::string S;
  for (unsigned I = 0; I < N; ++I)
    S += Step;
  return S;
}

constexpr unsigned Links = 30000;

TEST(DeepChain, EveryPostfixForm) {
  // (One test: each form costs little, a process per form would cost more.)
  struct Form {
    const char *Name;
    std::string Src;
  } Forms[] = {
      {"selectors", "package p\ntype T struct {\n\tnext *T\n\tv    int\n}\n"
                    "func f(a *T) int { return a" +
                        repeat(".next", Links) + ".v }\n"},
      {"calls", "package p\ntype F func() F\nfunc f(a F) F { return a" +
                    repeat("()", Links) + " }\n"},
      {"indexes", "package p\ntype A []A\nfunc f(a A) int { return len(a" +
                      repeat("[0]", Links) + ") }\n"},
      {"slices", "package p\nfunc f(a []int) int { return len(a" +
                     repeat("[:]", Links) + ") }\n"},
      {"assertions", "package p\nfunc f(a any) any { return a" +
                         repeat(".(any)", Links) + " }\n"},
      {"mixed", "package p\ntype T struct {\n\ts *T\n\ta []*T\n\tf func() "
                "*T\n}\nfunc (t *T) me() *T { return t }\n"
                "func f(t *T) *T { return t" +
                    repeat(".s.a[0].me().f().s", Links / 8) + " }\n"},
  };
  for (const Form &F : Forms)
    EXPECT_EQ(errorsOf(F.Src), std::vector<std::string>{}) << F.Name;
}

class UnsafeOnly : public sema::Importer {
public:
  explicit UnsafeOnly(sema::TypeContext &Ctx) : Ctx(Ctx) {}
  sema::Package *import(llvm::StringRef Path, std::string &Error) override {
    if (Path == "unsafe")
      return Ctx.unsafePackage();
    Error = "no such package";
    return nullptr;
  }
  sema::TypeContext &Ctx;
};

/// The errors of the program parsed with the command line's limits.
std::vector<std::string> errorsAtTheLimit(const std::string &Src) {
  test::Parsed P(Src);
  if (P.numErrors())
    return P.errors(); // (not checked: the search for the parser's limit)
  sema::TypeContext Ctx;
  UnsafeOnly Imp(Ctx);
  const ast::File *Files[] = {P.File};
  sema::CheckOptions CO;
  auto CP = sema::checkPackage(Ctx, P.Src.SM, "p", Files, Imp, P.Src.Diags, CO);
  EXPECT_TRUE(CP != nullptr);
  return P.errors();
}

TEST(DeepChain, NestedFormsAtTheParserLimit) {
  // R13: the forms that cost the most stack per level (measured in the
  // sanitizer build: nested built-ins 16.3 KB, composite literals 14.3 KB,
  // calls and `()` methods 13.2 KB, conversions 12.0 KB, closures 9.3 KB,
  // `[]` methods 8.5 KB), as deep as the
  // parser allows (CheckStackBytes is sized from them): no error, no crash.
  // Each form is nested until the parser refuses one more level.
  struct Form {
    const char *Name, *Open, *Bottom, *Close;
  } Forms[] = {
      {"sizeof", "unsafe.Sizeof(", "a", ")"},
      {"alignof", "unsafe.Alignof(", "a", ")"},
      {"clone", "clone(", "s", ")"},
      {"min", "min(", "a", ", 1)"},
      {"append", "append(", "s", ", 1)"},
      {"real", "real(complex(", "1.0", ", 0))"},
      {"len", "len([]int{", "1", "})"},
      {"literal", "[]any{", "1", "}"},
      {"call", "g(", "a", ")"},
      {"conversion", "int(", "a", ")"},
      {"closure", "func() any { return ", "a", " }()"},
      {"operator-call", "o(", "o", ")"},  // a `()` method (ADR-0024)
      {"operator-index", "o[", "o", "]"}, // a `[]` method
  };
  for (const Form &F : Forms) {
    unsigned N = 1024;
    std::vector<std::string> Errs;
    for (; N > 0; N -= 4) {
      Errs = errorsAtTheLimit(
          "package p\nimport \"unsafe\"\nfunc g(x any) any { return x }\n"
          "func use(x ...any) {}\nvar _ = unsafe.Sizeof(0)\n"
          "type Op struct{ v int }\nfunc (o Op) () (x Op) Op { return o }\n"
          "func (o Op) [] (k Op) Op { return o }\n"
          "func f(a int, o Op) {\n\tvar s []int\n\tuse(s, " +
          repeat(F.Open, N) + F.Bottom + repeat(F.Close, N) + ")\n}\n");
      if (Errs.empty() ||
          (Errs[0].find("exceeded max") == std::string::npos &&
           Errs[0].find("too deeply nested") == std::string::npos))
        break; // (the parser's two limits)
    }
    EXPECT_EQ(Errs, std::vector<std::string>{}) << F.Name << " at " << N;
    EXPECT_GE(N, 250u) << F.Name; // at the limit (a closure is 4 levels)
  }
}

TEST(DeepChain, AnErrorAtTheBottomIsReportedOnce) {
  std::vector<std::string> Errs =
      errorsOf("package p\ntype F func() F\nfunc f() F { return nope" +
               repeat("()", Links) + " }\n");
  ASSERT_EQ(Errs.size(), 1u);
  EXPECT_NE(Errs[0].find("undefined: nope"), std::string::npos) << Errs[0];
}

} // namespace
