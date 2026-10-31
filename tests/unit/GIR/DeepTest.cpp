//===- DeepTest.cpp - The deepest legal programs through GIR and LLVM -----===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01, P04.02 (stack discipline of P03.26). The GIR builder recurses once
// per level of the checked AST, which the parser bounds
// (ParseOptions::MaxHeight); the LLVM lowering does not recurse on the
// program at all (GIR is flat). Each form the M0 subset lowers is nested as
// deep as the parser allows, with the options `gopp build` uses, then
// checked, built into GIR, verified and lowered to LLVM IR (which the LLVM
// verifier checks). In the sanitizer build this is the test that the stack
// the builder runs on (gir::lowerPackage) is large enough: a left-deep
// `x + x + ...` chain at the limit needs 7.1 MB there.
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gopp/Basic/NewStack.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Lower.h"
#include "gopp/GIR/Verify.h"
#include "gopp/Sema/Sema.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

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

/// One nested form: Lead, then Open N times, Bottom, Close N times, Tail.
struct Form {
  const char *Name, *Lead, *Open, *Bottom, *Close, *Tail;
};

std::string repeat(const char *S, unsigned N) {
  std::string R;
  for (unsigned I = 0; I < N; ++I)
    R += S;
  return R;
}

std::string program(const Form &F, unsigned N) {
  return std::string("package main\n\nfunc g(x int) int { return x }\n\n"
                     "func h!(x int) int { return x }\n\n"
                     "func main() {\n\tx := 1\n\tb := true\n\t_ = b\n"
                     "\ta := [1]int{0}\n\t_ = a\n\t_ = x\n") +
         F.Lead + repeat(F.Open, N) + F.Bottom + repeat(F.Close, N) + F.Tail +
         "}\n";
}

/// The parse options of `gopp build` (sema::Loader).
ParseOptions buildParseOptions() {
  ParseOptions PO;
  PO.UncountedSelectors = true;
  return PO;
}

bool parses(const Form &F, unsigned N) {
  test::Parsed P(program(F, N), buildParseOptions());
  return P.numErrors() == 0;
}

/// Checks, builds and lowers a program (it must be valid).
void lowerProgram(const std::string &Text, const char *Name, unsigned Lo,
                  const llvm::TargetMachine &TM) {
  const Form F{Name, "", "", "", "", ""};
  test::Parsed P(Text, buildParseOptions());
  sema::TypeContext TC;
  NoImports Imp;
  const ast::File *Files[] = {P.File};
  auto CP = sema::checkPackage(TC, P.Src.SM, "p", Files, Imp, P.Src.Diags);
  ASSERT_TRUE(CP != nullptr) << F.Name;
  ASSERT_EQ(P.errors(), std::vector<std::string>{}) << F.Name << " at " << Lo;
  gir::Context GC;
  std::unique_ptr<gir::Module> M =
      gir::lowerPackage(GC, P.Src.SM, *CP, Files, P.Src.Diags);
  ASSERT_TRUE(M != nullptr)
      << F.Name << " at " << Lo << ": " << testing::PrintToString(P.errors());
  llvm::LLVMContext LC;
  auto LM = lowerToLLVM(LC, *M, TM);
  ASSERT_TRUE(static_cast<bool>(LM))
      << F.Name << " at " << Lo << ": " << toString(LM.takeError());
}

/// Checks, builds and lowers the deepest nesting of F the parser accepts.
void throughLLVM(const Form &F, const llvm::TargetMachine &TM) {
  // The deepest N the parser accepts (deeper is never accepted again).
  unsigned Lo = 1, Hi = 1100;
  ASSERT_TRUE(parses(F, Lo)) << F.Name;
  while (Lo < Hi) {
    unsigned Mid = (Lo + Hi + 1) / 2;
    if (parses(F, Mid))
      Lo = Mid;
    else
      Hi = Mid - 1;
  }
  ASSERT_FALSE(parses(F, Lo + 1)) << F.Name;
  EXPECT_GE(Lo, 250u) << F.Name; // at the limit, not stopped by something else
  lowerProgram(program(F, Lo), F.Name, Lo, TM);
}

const llvm::TargetMachine &amd64() {
  static std::unique_ptr<llvm::TargetMachine> TM = [] {
    auto T = parseTarget("linux/amd64");
    auto R = createTargetMachine(*T);
    return std::move(*R);
  }();
  return *TM;
}

TEST(DeepChain, ExpressionsThroughGIRAndLLVMAtTheParserLimit) {
  const Form Forms[] = {
      {"parens", "\ty := ", "(", "x", ")", "\n\tprintln(y)\n"},
      {"negations", "\ty := ", "-(", "x", ")", "\n\tprintln(y)\n"},
      {"right-nested sums", "\ty := ", "x + (", "x", ")", "\n\tprintln(y)\n"},
      {"left-deep sums", "\ty := ", "", "x", " + x", "\n\tprintln(y)\n"},
      {"calls", "\ty := ", "g(", "x", ")", "\n\tprintln(y)\n"},
      {"conversions", "\ty := ", "int(", "x", ")", "\n\tprintln(y)\n"},
      // P04.10: `!` calls (each a test and a cold edge).
      {"bang calls", "\ty := ", "h!(", "x", ")", "\n\tprintln(y)\n"},
      {"right-nested &&", "\tc := ", "b && (", "b", ")", "\n\tprintln(c)\n"},
      {"left-deep ||", "\tc := ", "", "b", " || b", "\n\tprintln(c)\n"},
      {"not", "\tc := ", "!(", "b", ")", "\n\tprintln(c)\n"},
      // P04.04: places and the checks in them.
      {"index chain", "\ty := ", "a[", "0", "]", "\n\tprintln(y)\n"},
      {"address and dereference", "\ty := ", "*(&", "x", ")",
       "\n\tprintln(y)\n"},
      {"min", "\ty := ", "min(x, ", "x", ")", "\n\tprintln(y)\n"},
      {"checked shifts", "\ty := ", "x << (", "x", ")", "\n\tprintln(y)\n"},
      {"checked divisions", "\ty := ", "x / (", "x", ")", "\n\tprintln(y)\n"},
  };
  for (const Form &F : Forms)
    throughLLVM(F, amd64());
}

TEST(DeepChain, StatementsThroughGIRAndLLVMAtTheParserLimit) {
  const Form Forms[] = {
      {"if", "", "\tif x == 1 {\n", "\tx++\n", "\t}\n", ""},
      {"if-else", "", "\tif x == 1 {\n\t} else {\n", "\tx++\n", "\t}\n", ""},
      {"for", "", "\tfor x < 9 {\n", "\tx++\n", "\tbreak\n\t}\n", ""},
      {"blocks", "\t", "{", "x++", "}", "\n"},
      // P04.04: switch inside a case (a Switch terminator per level) and
      // inside a default, with a fallthrough in the outer case.
      {"switch", "", "\tswitch x {\n\tcase 1:\n", "\tx++\n",
       "\tdefault:\n\t}\n", ""},
      {"tagless switch", "", "\tswitch {\n\tcase x == 1:\n", "\tx++\n",
       "\tdefault:\n\t}\n", ""},
      {"switch default", "", "\tswitch x {\n\tdefault:\n", "\tx++\n",
       "\tcase 1:\n\t}\n", ""},
  };
  for (const Form &F : Forms)
    throughLLVM(F, amd64());
}

// P04.04: the aggregates. Array and struct literals nested as deep as the
// parser allows, chains of field selections (selectors are uncounted by the
// parser: thousands of nested struct types, and a hundred thousand links
// through one self-pointing struct), and a package with twenty thousand
// variables initialized from each other.
static void aggregatesThroughGIRAndLLVM(unsigned N, unsigned K1, unsigned K2,
                                        unsigned V) {
  {
    std::string Ty, Open, Close;
    for (unsigned I = 0; I < N; ++I) {
      Ty += "[1]";
      Open += "{";
      Close += "}";
    }
    std::string Src = "package main\n\nfunc main() {\n\tv := " + Ty + "int" +
                      Open + "7" + Close + "\n\tw := v\n\tprintln(v == w)\n}\n";
    lowerProgram(Src, "nested array literals", N, amd64());
  }
  {
    // type T0 struct{ f int }; type Tk struct{ f Tk-1 }; v := Tn{...}; v.f.f...
    std::string Src = "package main\n\ntype T0 struct{ f int }\n";
    for (unsigned I = 1; I <= N; ++I)
      Src += "type T" + std::to_string(I) + " struct{ f T" +
             std::to_string(I - 1) + " }\n";
    std::string Lit = "T" + std::to_string(N) + "{";
    for (unsigned I = N; I-- > 0;)
      Lit += "T" + std::to_string(I) + "{";
    Lit += "3";
    for (unsigned I = 0; I <= N; ++I)
      Lit += "}";
    std::string Chain;
    for (unsigned I = 0; I < 1000 && I <= N; ++I)
      Chain += ".f";
    Src += "func main() {\n\tv := " + Lit + "\n\tw := v\n\tw" + Chain + " = v" +
           Chain + "\n\tprintln(v == w)\n}\n";
    lowerProgram(Src, "nested struct literals", N, amd64());
  }
  {
    // A field selection chain through pointers: p.f.f... is one place.
    std::string Src = "package main\n\ntype T0 struct{ n int }\n";
    for (unsigned I = 1; I <= N; ++I)
      Src += "type T" + std::to_string(I) + " struct{ f [1]T" +
             std::to_string(I - 1) + " }\n";
    std::string Chain;
    for (unsigned I = 0; I < N; ++I)
      Chain += ".f[0]";
    Src += "func main() {\n\tvar v T" + std::to_string(N) + "\n\tp := &v\n\tp" +
           Chain + ".n = 5\n\tprintln(v" + Chain + ".n, p" + Chain + ".n)\n}\n";
    lowerProgram(Src, "field and element chains", N, amd64());
  }
  {
    // A field chain far deeper than the nesting limit (the parser does not
    // count selectors): 3000 nested struct types, a pointer to the innermost
    // field taken through the chain.
    const unsigned K = K1;
    std::string Src = "package main\n\ntype A0 struct{ v int }\n", Sel;
    for (unsigned I = 1; I <= K; ++I)
      Src += "type A" + std::to_string(I) + " struct{ f A" +
             std::to_string(I - 1) + " }\n";
    for (unsigned I = 0; I < K; ++I)
      Sel += ".f";
    Src += "\nfunc main() {\n\tvar a A" + std::to_string(K) + "\n\tp := &a" +
           Sel + ".v\n\t*p = 5\n\tprintln(a" + Sel + ".v)\n}\n";
    lowerProgram(Src, "field chain", K, amd64());
  }
  {
    // One struct that points to itself: a selector chain of any length is
    // one type, so the lowering makes the place in a loop.
    const unsigned K = K2;
    std::string Chain;
    for (unsigned I = 0; I < K; ++I)
      Chain += ".f";
    std::string Src = "package main\n\ntype T struct {\n\tf *T\n\tv int\n}\n\n"
                      "func walk(p *T) int {\n\tp" +
                      Chain + ".v = 5\n\treturn p" + Chain +
                      ".v\n}\n\nfunc main() {}\n";
    lowerProgram(Src, "selector chain through a pointer", K, amd64());
  }
  {
    // Variables initialized from each other, in reverse order: the checker
    // declares it link by link on stacks of their own (runOnNewStack) and the
    // lowering does not recurse per link.
    std::string Src = "package main\n\n";
    for (unsigned I = V; I-- > 1;)
      Src += "var v" + std::to_string(I) + " = v" + std::to_string(I - 1) +
             " + 1\n";
    Src += "var v0 = 1\nfunc main() {\n\tprintln(v" + std::to_string(V - 1) +
           ")\n}\n";
    lowerProgram(Src, "init order chain", V, amd64());
  }
}

TEST(DeepChain, AggregatesThroughGIRAndLLVM) {
  ScopedNewStackSegment Seg(4);
  aggregatesThroughGIRAndLLVM(100, 300, 2000, 500);
}

TEST(DeepChain, DISABLED_AggregatesThroughGIRAndLLVMFullDepth) {
  aggregatesThroughGIRAndLLVM(200, 3000, 100000, 20000);
}

// R18: LLVM computes a struct's layout (and isSized) recursively through the
// structs it holds the first time it is asked; the code generator asks right
// after each body, so each ask is one level deep. A chain of 40000 structs
// overflowed the stack before (30000 passed); `setBody` of LLVM is quadratic
// in the chain, so this takes a dozen seconds (a quarter of the depth under
// the sanitizer).
TEST(DeepChain, DISABLED_StructLayoutOfAChainOfStructs) {
#if defined(__SANITIZE_ADDRESS__)
  const unsigned K = 10000;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
  const unsigned K = 10000;
#else
  const unsigned K = 40000;
#endif
#else
  const unsigned K = 40000;
#endif
  gir::Context GC;
  gir::Module HM(GC, "deep");
  gir::Type *S = GC.structTy("T0");
  GC.setStructBody(S, {GC.i64()});
  for (unsigned I = 1; I <= K; ++I) {
    gir::Type *Prev = S;
    S = GC.structTy("T" + std::to_string(I));
    GC.setStructBody(S, {Prev, GC.i64()});
  }
  gir::Global G;
  G.Name = "deep_global";
  G.Ty = S;
  HM.addGlobal(G);
  llvm::LLVMContext LC;
  auto LM = lowerToLLVM(LC, HM, amd64());
  EXPECT_TRUE(static_cast<bool>(LM)) << toString(LM.takeError());
}

// A chain of named types thousands of levels long (review R6): lowering the
// types (lowerType), the LLVM type of each (memTy) and the text of the GIR
// types walk the chain on stacks of their own, and the == glue cuts it into
// functions of 24 levels. Under ASan one recursion per link overflowed the
// stack between 10000 and 14000 links. An array chain,
// a struct chain and an Option chain, each compared with == and copied.
static void chainsOfNamedTypes(unsigned K, unsigned N) {
  // To GIR and verified only, as long as the stack would not reach (the
  // recursion overflowed under ASan between 10000 and 14000 links); then
  // through LLVM at a length that LLVM's own quadratic struct bodies allow.
  for (const char *Shape : {"[1]T", "struct{ f T"}) {
    bool Arr = Shape[0] == '[';
    std::string Src = Arr ? "package main\n\ntype T0 [1]int\n"
                          : "package main\n\ntype T0 struct{ f int }\n";
    for (unsigned I = 1; I <= K; ++I)
      Src += "type T" + std::to_string(I) + " " + Shape +
             std::to_string(I - 1) + (Arr ? "\n" : " }\n");
    Src += "func main() {\n\tvar a T" + std::to_string(K) +
           "\n\tb := a\n\tprintln(a == b)\n}\n";
    test::Parsed P(Src, buildParseOptions());
    sema::TypeContext TC;
    NoImports Imp;
    const ast::File *Files[] = {P.File};
    auto CP = sema::checkPackage(TC, P.Src.SM, "p", Files, Imp, P.Src.Diags);
    ASSERT_TRUE(CP != nullptr);
    gir::Context GC;
    std::unique_ptr<gir::Module> M =
        gir::lowerPackage(GC, P.Src.SM, *CP, Files, P.Src.Diags);
    ASSERT_TRUE(M != nullptr)
        << Shape << ": " << testing::PrintToString(P.errors());
    EXPECT_TRUE(gir::verify(*M, 5).empty()) << Shape;
    // The deep type prints (a structural array type is K levels of text).
    EXPECT_GT(M->Funcs.size(), 3u) << Shape;
  }
  {
    std::string Src = "package main\n\ntype T0 [1]int\n";
    for (unsigned I = 1; I <= N; ++I)
      Src +=
          "type T" + std::to_string(I) + " [1]T" + std::to_string(I - 1) + "\n";
    Src += "func main() {\n\tvar a T" + std::to_string(N) +
           "\n\tb := a\n\tb[0][0] = b[0][0]\n\tprintln(a == b)\n}\n";
    lowerProgram(Src, "array chain", N, amd64());
  }
  {
    std::string Src = "package main\n\ntype T0 struct{ f int }\n";
    for (unsigned I = 1; I <= N; ++I)
      Src += "type T" + std::to_string(I) + " struct{ f T" +
             std::to_string(I - 1) + "; g int8 }\n";
    Src += "func main() {\n\tvar a T" + std::to_string(N) +
           "\n\tb := a\n\tprintln(a == b, a != b)\n}\n";
    lowerProgram(Src, "struct chain", N, amd64());
  }
  {
    // A chain through Option and a pointer: Option[*T] is the pointer, so
    // the chain is of structs holding an Option of the next struct's field.
    std::string Src = "package main\n\ntype T0 struct{ f Option[int] }\n";
    for (unsigned I = 1; I <= 200; ++I)
      Src += "type T" + std::to_string(I) + " struct{ f Option[T" +
             std::to_string(I - 1) + "] }\n";
    Src += "func main() {\n\tvar a T200\n\tb := a\n\tprintln(a == b)\n}\n";
    lowerProgram(Src, "option chain", 200, amd64());
  }
}

// Smoke: a new stack every 4 levels, chains of 300.
TEST(DeepChain, ChainsOfNamedTypes) {
  ScopedNewStackSegment Seg(4);
  chainsOfNamedTypes(300, 300);
}

// The full depth (20000 and 3000 links): deep-chain.long.test.
TEST(DeepChain, DISABLED_ChainsOfNamedTypesFullDepth) {
  chainsOfNamedTypes(20000, 3000);
}

// R9: no stack to be had for a chain of types: lowering stops with the one
// error (and gives no module), the LLVM lowering of a good module returns the
// same message, and the text of a deep type is cut short. With normal
// resources the same program lowers.
TEST(DeepChain, NoStackForAChainOfTypes) {
  ScopedNewStackSegment Seg(64); // hop every 64 levels, not by stack left
  const unsigned K = 3000;
  std::string Src = "package main\n\ntype T0 [1]int\n";
  for (unsigned I = 1; I <= K; ++I)
    Src +=
        "type T" + std::to_string(I) + " [1]T" + std::to_string(I - 1) + "\n";
  Src += "func main() {\n\tvar a T" + std::to_string(K) +
         "\n\tb := a\n\tprintln(a == b)\n}\n";
  test::Parsed P(Src, buildParseOptions());
  sema::TypeContext TC;
  NoImports Imp;
  const ast::File *Files[] = {P.File};
  auto CP = sema::checkPackage(TC, P.Src.SM, "p", Files, Imp, P.Src.Diags);
  ASSERT_TRUE(CP != nullptr);
  ASSERT_EQ(P.errors(), std::vector<std::string>{});
  gir::Context GC;
  {
    test::Parsed Q(Src, buildParseOptions());
    sema::TypeContext TC2;
    const ast::File *Files2[] = {Q.File};
    auto CP2 = sema::checkPackage(TC2, Q.Src.SM, "p", Files2, Imp, Q.Src.Diags);
    ASSERT_TRUE(CP2 != nullptr);
    long Old = failNewStacksAfter(0);
    gir::Context GC2;
    auto M2 = gir::lowerPackage(GC2, Q.Src.SM, *CP2, Files2, Q.Src.Diags);
    failNewStacksAfter(Old);
    EXPECT_TRUE(M2 == nullptr);
    std::vector<std::string> Errs = Q.errors();
    ASSERT_EQ(Errs.size(), 1u) << testing::PrintToString(Errs);
    EXPECT_NE(Errs[0].find("nesting too deep: could not get stack for 64 "
                           "levels"),
              std::string::npos)
        << Errs[0];
  }
  std::unique_ptr<gir::Module> M =
      gir::lowerPackage(GC, P.Src.SM, *CP, Files, P.Src.Diags);
  ASSERT_TRUE(M != nullptr) << testing::PrintToString(P.errors());
  // A module whose global has a type 3000 levels deep, to LLVM with no
  // stacks: the same message, no crash; with stacks it lowers.
  {
    gir::Context GC3;
    gir::Module HM(GC3, "deep");
    gir::Type *Deep = GC3.i64();
    for (unsigned I = 0; I < K; ++I)
      Deep = GC3.array(Deep, 1);
    gir::Global G;
    G.Name = "deep_global";
    G.Ty = Deep;
    HM.addGlobal(G);
    {
      llvm::LLVMContext LC;
      long Old = failNewStacksAfter(0);
      auto LM = lowerToLLVM(LC, HM, amd64());
      failNewStacksAfter(Old);
      ASSERT_FALSE(static_cast<bool>(LM));
      std::string Msg = toString(LM.takeError());
      EXPECT_NE(Msg.find("nesting too deep: could not get stack for 64 "
                         "levels"),
                std::string::npos)
          << Msg;
    }
    llvm::LLVMContext LC;
    auto LM = lowerToLLVM(LC, HM, amd64());
    EXPECT_TRUE(static_cast<bool>(LM)) << toString(LM.takeError());
  }
  // A deep GIR type prints cut short when no stack can be had.
  gir::Type *T = GC.i64();
  for (unsigned I = 0; I < 300; ++I)
    T = GC.array(T, 1);
  long Old = failNewStacksAfter(0);
  std::string Text = T->str();
  failNewStacksAfter(Old);
  EXPECT_NE(Text.find("..."), std::string::npos);
  EXPECT_LT(Text.size(), 1000u);
  EXPECT_EQ(T->str().find("..."), std::string::npos);
}

} // namespace
