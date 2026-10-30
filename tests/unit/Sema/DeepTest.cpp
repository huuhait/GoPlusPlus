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

#include "gopp/Basic/NewStack.h"
#include "gopp/Sema/Sema.h"

#include "llvm/Support/FormatVariadic.h"

#include "gtest/gtest.h"

#include <iostream>

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
      // P03.25 (ADR-0025): `x.In[U]()` is a selector, an index and a call.
      {"unit-in", "package p\nunit M\nunit KM = 1000 M\n"
                  "func f(a float64[M]) float64[KM] { return a" +
                      repeat(".In[KM]().In[M]()", Links / 6) + ".In[KM]() }\n"},
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

// R13: a chain of declarations whose every initializer is nested to the
// parser's limit. A declaration hops to a stack of its own when the stack
// left is less than one worst declaration (DeclReserveBytes), not every N
// links, so a deep initializer reached after a hop has the whole of a fresh
// stack. Calls, parentheses, literals and closures (each as deep as the parser
// allows; K links) must check clean, in the normal and the sanitizer build.
TEST(DeepChain, ChainOfInitializersAtTheParserLimit) {
  struct Form {
    const char *Name, *Open, *Close;
  } Forms[] = {
      {"call", "g(", ")"},
      {"paren", "(", ")"},
      {"literal", "[]any{", "}"},
      {"closure", "func() any { return ", " }()"},
  };
  const unsigned K = 24;
  unsigned long Before = newStackThreads();
  for (const Form &F : Forms) {
    unsigned N = 1024;
    std::vector<std::string> Errs;
    for (; N > 0; N -= 4) {
      std::string Src = "package p\nfunc g(x any) any { return x }\n";
      for (unsigned I = 0; I < K; ++I)
        Src += "var v" + std::to_string(I) + " any = " + repeat(F.Open, N) +
               "v" + std::to_string(I + 1) + repeat(F.Close, N) + "\n";
      Src += "var v" + std::to_string(K) + " any = 1\n";
      Errs = errorsAtTheLimit(Src);
      if (Errs.empty() ||
          (Errs[0].find("exceeded max") == std::string::npos &&
           Errs[0].find("too deeply nested") == std::string::npos))
        break;
    }
    EXPECT_EQ(Errs, std::vector<std::string>{}) << F.Name << " at " << N;
    EXPECT_GE(N, 250u) << F.Name;
  }
  // The initializers use most of a stack each: the chains hopped.
  EXPECT_GT(newStackThreads(), Before);
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

TEST(DeepChain, UnitFormsAtTheParserLimit) {
  // P03.25 (M4): the unit forms nested as deep as the parser allows, on the
  // checker stack sized from the worst form above: conversions that scale,
  // `min` over units, inference of a unit parameter, unit products, unit
  // literals in literals, `.In` under parentheses. The unit rules run after
  // the operands are checked, out of line, so a level costs what the form
  // costs without units.
  struct Form {
    const char *Name, *Open, *Bottom, *Close;
  } Forms[] = {
      {"conversion", "float64[KM](", "d", ")"},
      {"min", "min(", "d", ", 1.0[M])"},
      {"inference", "id(", "d", ")"},
      {"explicit", "id[M](", "d", ")"},
      {"product", "(d*", "d", ")"},
      {"sum", "(1.0[KM] + ", "d", ")"},
      {"literal", "[]any{", "1.0[M]", "}"},
      {"in", "(", "d", ").In[KM]()"},
  };
  for (const Form &F : Forms) {
    unsigned N = 1024;
    std::vector<std::string> Errs;
    for (; N > 0; N -= 4) {
      Errs = errorsAtTheLimit(
          "package p\nunit M\nunit KM = 1000 M\n"
          "func use(x ...any) {}\n"
          "func id[U unit](x float64[U]) float64[U] { return x }\n"
          "func f(d float64[M]) {\n\tuse(" +
          repeat(F.Open, N) + F.Bottom + repeat(F.Close, N) + ")\n}\n");
      if (Errs.empty() ||
          (Errs[0].find("exceeded max") == std::string::npos &&
           Errs[0].find("too deeply nested") == std::string::npos))
        break; // (the parser's two limits)
    }
    EXPECT_EQ(Errs, std::vector<std::string>{}) << F.Name << " at " << N;
    EXPECT_GE(N, 250u) << F.Name;
  }
}

TEST(DeepChain, AnErrorAtTheBottomIsReportedOnce) {
  std::vector<std::string> Errs =
      errorsOf("package p\ntype F func() F\nfunc f() F { return nope" +
               repeat("()", Links) + " }\n");
  ASSERT_EQ(Errs.size(), 1u);
  EXPECT_NE(Errs[0].find("undefined: nope"), std::string::npos) << Errs[0];
}

/// The text of each cycle error of the program.
std::vector<std::string> cycleErrors(const std::string &Src) {
  std::vector<std::string> Out;
  for (const std::string &E : errorsOf(Src))
    if (E.find("initialization cycle") != std::string::npos)
      Out.push_back(E);
  return Out;
}

// An initialization cycle of 20000 variables, of constants, or of variables
// and functions is declared and reported (once) without a crash: objDecl
// follows it link by link on stacks of their own (runOnNewStack), and the
// cycle search has an explicit stack. Both sides: a short ring has its
// message.
static void initializationCycles(unsigned N) {
  auto Ring = [](unsigned K, const char *Fmt) {
    std::string Src = "package p\n";
    for (unsigned I = 0; I < K; ++I)
      Src += llvm::formatv(Fmt, I, (I + 1) % K).str();
    return Src;
  };
  for (unsigned K : {3u, N}) {
    std::vector<std::string> Var =
        cycleErrors(Ring(K, "var v{0} = v{1} + 1\n"));
    ASSERT_EQ(Var.size(), 1u) << K;
    EXPECT_NE(Var[0].find("initialization cycle for v0"), std::string::npos);
    std::vector<std::string> Const =
        cycleErrors(Ring(K, "const c{0} = c{1} + 1\n"));
    ASSERT_EQ(Const.size(), 1u) << K;
    EXPECT_NE(Const[0].find("initialization cycle for c0"), std::string::npos);
    std::vector<std::string> Mix = cycleErrors(
        Ring(K, "var v{0} = f{0}()\nfunc f{0}() int { return v{1} }\n"));
    ASSERT_EQ(Mix.size(), 1u) << K;
    EXPECT_NE(Mix[0].find("initialization cycle for v0"), std::string::npos);
  }
  // A chain whose end is a ring, the chain declared first: the search for a
  // cycle runs for the ring only.
  std::string Tail = "package p\n";
  for (unsigned I = 0; I + 1 < N; ++I)
    Tail +=
        "var t" + std::to_string(I) + " = t" + std::to_string(I + 1) + " + 1\n";
  Tail += "var t" + std::to_string(N - 1) + " = r0\n";
  Tail += "var r0 = r1 + 1\nvar r1 = r2 + 1\nvar r2 = r0 + 1\n";
  std::vector<std::string> T = cycleErrors(Tail);
  ASSERT_EQ(T.size(), 1u);
  EXPECT_NE(T[0].find("initialization cycle for r0"), std::string::npos);
}

// Smoke: segments of 4 levels, so rings and chains of 300 hop many times.
TEST(DeepChain, InitializationCycles) {
  ScopedNewStackSegment Seg(4);
  initializationCycles(300);
}

// The full depth (20000 links, 64 levels a stack): deep-chain.long.test.
TEST(DeepChain, DISABLED_InitializationCyclesFullDepth) {
  initializationCycles(20000);
}

/// Lets the next N threads be made, then fails every request; restores the
/// setting when it goes out of scope.
struct StackBudget {
  long Old;
  explicit StackBudget(long N) : Old(failNewStacksAfter(N)) {}
  ~StackBudget() { failNewStacksAfter(Old); }
};

// R9: when the system gives no more stacks (a limit on address space or
// threads), a chain that needs them stops at its declaration with one clear
// error, never a crash, and the same chain with normal resources checks
// clean. The failure is made on purpose through the test hook.
static void noStackForADeepChain(unsigned N) {
  const unsigned Seg = newStackSegment();
  std::string Src = "package p\n";
  for (unsigned I = 0; I + 1 < N; ++I)
    Src +=
        "var v" + std::to_string(I) + " = v" + std::to_string(I + 1) + " + 1\n";
  Src += "var v" + std::to_string(N - 1) + " = 1\n";
  {
    StackBudget B(3); // three stacks of Seg links, then none
    std::vector<std::string> Errs = errorsOf(Src);
    ASSERT_EQ(Errs.size(), 1u) << testing::PrintToString(Errs);
    EXPECT_NE(Errs[0].find("nesting too deep: could not get stack for " +
                           std::to_string(4 * Seg) + " levels"),
              std::string::npos)
        << Errs[0];
  }
  {
    StackBudget B(0); // none at all: the first hop, Seg links down
    std::vector<std::string> Errs = errorsOf(Src);
    ASSERT_EQ(Errs.size(), 1u) << testing::PrintToString(Errs);
    EXPECT_NE(Errs[0].find("could not get stack for " + std::to_string(Seg) +
                           " levels"),
              std::string::npos)
        << Errs[0];
  }
  EXPECT_EQ(errorsOf(Src), std::vector<std::string>{}); // normal resources
  // A chain short of one stack needs none.
  StackBudget B(0);
  std::string Short = "package p\n";
  for (unsigned I = 0; I + 2 < Seg; ++I)
    Short +=
        "var v" + std::to_string(I) + " = v" + std::to_string(I + 1) + " + 1\n";
  Short += "var v" + std::to_string(Seg - 2) + " = 1\n";
  EXPECT_EQ(errorsOf(Short), std::vector<std::string>{});
}

TEST(DeepChain, NoStackForADeepChain) {
  ScopedNewStackSegment Seg(4);
  noStackForADeepChain(300);
}

TEST(DeepChain, DISABLED_NoStackForADeepChainFullDepth) {
  ScopedNewStackSegment Seg(64); // a hop every 64 levels
  noStackForADeepChain(20000);
}

// R13: hopping by stack left, a chain of 20000 cheap links needs a few threads
// (it was one per 64 links, 312). Prints the count.
TEST(DeepChain, DISABLED_CheapChainNeedsFewThreads) {
  const unsigned N = 20000;
  std::string Src = "package p\n";
  for (unsigned I = 0; I + 1 < N; ++I)
    Src +=
        "var v" + std::to_string(I) + " = v" + std::to_string(I + 1) + " + 1\n";
  Src += "var v" + std::to_string(N - 1) + " = 1\n";
  unsigned long Before = newStackThreads();
  EXPECT_EQ(errorsOf(Src), std::vector<std::string>{});
  unsigned long Used = newStackThreads() - Before;
  std::cerr << "threads for a chain of " << N << " links: " << Used << "\n";
  EXPECT_LE(Used, 60u);
}

} // namespace
