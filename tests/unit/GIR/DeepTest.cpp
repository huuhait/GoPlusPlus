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

#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Lower.h"
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
                     "func main() {\n\tx := 1\n\tb := true\n\t_ = b\n"
                     "\t_ = x\n") +
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
  test::Parsed P(program(F, Lo), buildParseOptions());
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
      {"right-nested &&", "\tc := ", "b && (", "b", ")", "\n\tprintln(c)\n"},
      {"left-deep ||", "\tc := ", "", "b", " || b", "\n\tprintln(c)\n"},
      {"not", "\tc := ", "!(", "b", ")", "\n\tprintln(c)\n"},
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
  };
  for (const Form &F : Forms)
    throughLLVM(F, amd64());
}

} // namespace
