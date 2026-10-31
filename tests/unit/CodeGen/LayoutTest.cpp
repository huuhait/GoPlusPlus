//===- LayoutTest.cpp - The checker's sizes are the code generator's ------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.04 (D4.5). `unsafe.Sizeof`, `Alignof` and `Offsetof` are constants the
// checker computes (Sizes.cpp) before any code exists; the program's data is
// laid out by LLVM's data layout of the target. If the two ever differ, a
// program reads another type's bytes. This test generates a table of types
// (every scalar, arrays, hundreds of structs of mixed fields, Options), asks
// the checker for each size, alignment and field offset, lowers the same
// variables to LLVM for both targets, and asks the DataLayout. Both sides: a
// positive control checks that the table really detects a different layout.
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/Loader.h"

#include "llvm/IR/DataLayout.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"

#include <gtest/gtest.h>

#include <fstream>
#include <random>

using namespace gopp;

namespace {

struct Entry {
  std::string Type;
  /// For a struct: the number of fields (Offsetof is checked for each).
  unsigned Fields = 0;
  /// A variable of a type without a zero value starts with this.
  const char *Init = "";
};

/// The checker's answers for variable g<N>: size, alignment, field offsets.
struct Answer {
  int64_t Size = -1, Align = -1;
  std::vector<int64_t> Offsets;
};

int64_t constOf(sema::CheckedPackage &CP, const std::string &Name) {
  auto *O = CP.Pkg->PkgScope->lookupLocal(Name);
  auto *K = O ? llvm::dyn_cast<sema::Const>(O) : nullptr;
  int64_t V = -1;
  if (!K || !K->Val || !K->Val->toInt().fitsInt64(V))
    return -1;
  return V;
}

/// Package main declaring the types and one variable of each, and constants
/// holding the answers.
std::string program(const std::vector<std::string> &Decls,
                    const std::vector<Entry> &Table, bool Calls = false) {
  std::string S = "package main\n\nimport \"unsafe\"\n\nvar dummy int32\n\n";
  for (const std::string &D : Decls)
    S += D + "\n";
  S += "\n";
  for (size_t I = 0; I < Table.size(); ++I) {
    std::string N = std::to_string(I);
    // (A type without a zero value is measured by a call that is not run.)
    std::string X = "g" + N;
    if (Calls) {
      S += "func g" + N + "() " + Table[I].Type + " { panic(0) }\n";
      X += "()";
    } else {
      S += "var g" + N + " " + Table[I].Type + Table[I].Init + "\n";
    }
    S += "const s" + N + " = unsafe.Sizeof(" + X + ")\n";
    S += "const a" + N + " = unsafe.Alignof(" + X + ")\n";
    for (unsigned K = 0; K < Table[I].Fields; ++K)
      S += "const o" + N + "_" + std::to_string(K) + " = unsafe.Offsetof(g" +
           N + ".f" + std::to_string(K) + ")\n";
  }
  S += "\nfunc main() {}\n";
  return S;
}

/// Checks Source (one file), then lowers it for Target (null: not at all).
struct Built {
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  std::unique_ptr<DiagnosticsEngine> Diags;
  sema::TypeContext TCtx;
  std::unique_ptr<sema::Loader> L;
  sema::CheckedPackage *CP = nullptr;
  gir::Context GC;
  std::unique_ptr<gir::Module> GM;
  std::string Dir;
  std::string Errors;

  Built(const std::string &Source, bool Lower) {
    llvm::SmallString<128> D;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-layout", D));
    Dir = std::string(D);
    {
      std::ofstream Out(Dir + "/main.gopp", std::ios::binary);
      Out << Source;
    }
    Diags = std::make_unique<DiagnosticsEngine>(SM, Collect, 0);
    sema::LoadOptions LO;
    LO.Root = Dir;
    L = std::make_unique<sema::Loader>(SM, *Diags, TCtx, LO);
    CP = L->loadDir(Dir, "");
    if (CP && !Diags->hasErrors() && Lower)
      GM = gir::lowerPackage(GC, SM, *CP, L->filesOf(CP), *Diags);
    for (const Diagnostic &Dg : Collect.Diags)
      Errors += Dg.Message + "\n";
  }
  ~Built() { llvm::sys::fs::remove_directories(Dir); }
};

/// A deterministic table: the scalars, arrays of them, and many structs.
void makeTable(std::vector<std::string> &Decls, std::vector<Entry> &Table) {
  const char *Scalars[] = {"bool",    "int8",    "int16",          "int32",
                           "int64",   "int",     "uint8",          "uint16",
                           "uint32",  "uint64",  "uint",           "uintptr",
                           "float32", "float64", "complex64",      "complex128",
                           "string",  "*int32",  "unsafe.Pointer", "byte",
                           "rune"};
  for (const char *S : Scalars)
    Table.push_back({S, 0, std::string(S) == "*int32" ? " = &dummy" : ""});
  const char *Arrays[] = {"[0]int8",     "[1]int8",       "[3]int16",
                          "[5]int32",    "[2]float64",    "[4]bool",
                          "[2][3]int16", "[3]complex64",  "[0]int64",
                          "[7]byte",     "[2]string",     "[3][2][2]uint8",
                          "[2]struct{}", "[1]complex128", "[9]float32"};
  for (const char *A : Arrays)
    Table.push_back({A, 0});
  Table.push_back({"struct{}", 0});
  // Structs: random field lists over scalars, arrays and the structs before.
  // (A raw pointer has no zero value, so a field of one is an Option.)
  std::vector<std::string> FieldTypes;
  for (const char *S : Scalars)
    FieldTypes.push_back(std::string(S) == "*int32" ? "Option[*int32]" : S);
  for (const char *A : Arrays)
    FieldTypes.push_back(A);
  std::mt19937 Rng(20261027);
  unsigned NS = 220;
  for (unsigned I = 0; I < NS; ++I) {
    unsigned N = Rng() % 7;
    std::string D = "type S" + std::to_string(I) + " struct {";
    for (unsigned K = 0; K < N; ++K) {
      std::string T;
      // Sometimes a struct declared before (nesting); else a scalar/array.
      if (I > 0 && Rng() % 5 == 0)
        T = "S" + std::to_string(Rng() % I);
      else
        T = FieldTypes[Rng() % FieldTypes.size()];
      D += " f" + std::to_string(K) + " " + T + ";";
    }
    D += " }";
    Decls.push_back(D);
    Table.push_back({"S" + std::to_string(I), N});
  }
  // The shapes that matter most, by hand: padding, trailing zero-size field,
  // a bool between words, and a struct of structs.
  Decls.push_back("type PadA struct { f0 bool; f1 int64; f2 int16 }");
  Table.push_back({"PadA", 3});
  Decls.push_back("type PadB struct { f0 int8; f1 int8; f2 int32; f3 int8 }");
  Table.push_back({"PadB", 4});
  Decls.push_back("type ZeroTail struct { f0 int64; f1 struct{} }");
  Table.push_back({"ZeroTail", 2});
  Decls.push_back("type ZeroTail2 struct { f0 int32; f1 struct{} }");
  Table.push_back({"ZeroTail2", 2});
  Decls.push_back("type ZeroMid struct { f0 int8; f1 struct{}; f2 int64 }");
  Table.push_back({"ZeroMid", 3});
  Decls.push_back("type Nest struct { f0 PadA; f1 PadB; f2 [3]PadA }");
  Table.push_back({"Nest", 3});
  Decls.push_back("type Cx struct { f0 bool; f1 complex64; f2 complex128 }");
  Table.push_back({"Cx", 3});
  // P04.05, P04.25: slices (four words, ADR-0029), maps (one pointer), the
  // self-naming types and structs that hold them.
  Decls.push_back("type WithSlice struct { f0 int8; f1 []int16; f2 "
                  "map[int]string; f3 bool; f4 [2][]byte }");
  Table.push_back({"WithSlice", 5});
  // The word after the slice moves the next field (ADR-0029): offsets 0, 32,
  // 40, 72 and 168, size 176.
  Decls.push_back("type SliceFirst struct { f0 []int; f1 int8; f2 []int8; "
                  "f3 [3][]int16; f4 map[int]int }");
  Table.push_back({"SliceFirst", 5});
  Decls.push_back("type RecS []RecS");
  Decls.push_back("type RecM map[string]RecM");
  Decls.push_back("type RecO Option[*RecO]");
  Decls.push_back("type RecA [2]Option[*RecA]");
  Decls.push_back("type RecT struct { f0 bool; f1 []RecT; f2 map[int]RecT }");
  Table.push_back({"RecT", 3});
  const char *Seq[] = {"[]int8",
                       "[]int64",
                       "[][]string",
                       "[]struct{}",
                       "[]PadA",
                       "map[int]int8",
                       "map[string]PadB",
                       "[3][]int32",
                       "[]int",
                       "[0][]int",
                       "[2][2][]string",
                       "RecS",
                       "RecM",
                       "RecO",
                       "RecA",
                       "[2]RecS",
                       "Option[RecA]"};
  for (const char *Q : Seq)
    Table.push_back({Q, 0});
  // Option[T]: the nil bit pattern for a pointer kind, else T and a flag.
  const char *Options[] = {
      "Option[int8]",     "Option[int16]",     "Option[int32]",
      "Option[int64]",    "Option[bool]",      "Option[float32]",
      "Option[float64]",  "Option[complex64]", "Option[complex128]",
      "Option[*int32]",   "Option[*PadA]",     "Option[PadA]",
      "Option[PadB]",     "Option[[3]int16]",  "Option[[0]int8]",
      "Option[struct{}]", "Option[string]",    "Option[Nest]",
      "Option[uintptr]",  "Option[byte]"};
  for (const char *O : Options)
    Table.push_back({O, 0});
}

std::vector<Answer> answers(sema::CheckedPackage &CP,
                            const std::vector<Entry> &Table) {
  std::vector<Answer> R;
  for (size_t I = 0; I < Table.size(); ++I) {
    Answer A;
    std::string N = std::to_string(I);
    A.Size = constOf(CP, "s" + N);
    A.Align = constOf(CP, "a" + N);
    for (unsigned K = 0; K < Table[I].Fields; ++K)
      A.Offsets.push_back(constOf(CP, "o" + N + "_" + std::to_string(K)));
    R.push_back(std::move(A));
  }
  return R;
}

TEST(Layout, TheCheckersSizesAreTheDataLayoutsOnBothTargets) {
  std::vector<std::string> Decls;
  std::vector<Entry> Table;
  makeTable(Decls, Table);
  Built B(program(Decls, Table), /*Lower=*/true);
  ASSERT_TRUE(B.CP && B.GM) << B.Errors;
  std::vector<Answer> Want = answers(*B.CP, Table);
  // (`dummy` is the first global.)
  ASSERT_EQ(B.GM->Globals.size(), Table.size() + 1);
  EXPECT_GT(Table.size(), 250u);
  // ADR-0029 / P04.25: the sizes are written down here too, so a change of
  // both sides together still fails: an owned slice is 32 bytes.
  for (size_t I = 0; I < Table.size(); ++I) {
    std::string T = Table[I].Type;
    if (T == "[]int" || T == "[]int8" || T == "[]int64")
      EXPECT_EQ(Want[I].Size, 32) << T;
    else if (T == "[2][2][]string")
      EXPECT_EQ(Want[I].Size, 128) << T;
    else if (T == "SliceFirst")
      EXPECT_EQ(Want[I].Size, 176) << T;
  }

  for (const char *Name : {"linux/amd64", "linux/arm64"}) {
    auto T = parseTarget(Name);
    ASSERT_TRUE(static_cast<bool>(T));
    auto TM = createTargetMachine(*T);
    ASSERT_TRUE(static_cast<bool>(TM));
    llvm::LLVMContext LC;
    auto LM = lowerToLLVM(LC, *B.GM, **TM);
    ASSERT_TRUE(static_cast<bool>(LM)) << llvm::toString(LM.takeError());
    const llvm::DataLayout &DL = (*LM)->getDataLayout();
    for (size_t I = 0; I < Table.size(); ++I) {
      const gir::Global &G = B.GM->Globals[I + 1];
      llvm::GlobalVariable *GV = (*LM)->getNamedGlobal(G.Name);
      ASSERT_NE(GV, nullptr) << Name << " " << Table[I].Type;
      llvm::Type *VT = GV->getValueType();
      EXPECT_EQ(static_cast<int64_t>(DL.getTypeAllocSize(VT)), Want[I].Size)
          << Name << " Sizeof(" << Table[I].Type << ")";
      EXPECT_EQ(static_cast<int64_t>(DL.getABITypeAlign(VT).value()),
                Want[I].Align)
          << Name << " Alignof(" << Table[I].Type << ")";
      if (Table[I].Fields) {
        auto *ST = llvm::dyn_cast<llvm::StructType>(VT);
        ASSERT_NE(ST, nullptr) << Table[I].Type;
        ASSERT_EQ(ST->getNumElements(), Table[I].Fields) << Table[I].Type;
        const llvm::StructLayout *SL = DL.getStructLayout(ST);
        for (unsigned K = 0; K < Table[I].Fields; ++K)
          EXPECT_EQ(static_cast<int64_t>(SL->getElementOffset(K)),
                    Want[I].Offsets[K])
              << Name << " Offsetof(" << Table[I].Type << ".f" << K << ")";
      }
    }
  }
}

TEST(Layout, ThePositiveControlSeesADifferentLayout) {
  // gc (GoCompat) puts one padding byte after a trailing zero-size field and
  // makes a string two words; the Go++ model (D4.5) does neither. A table
  // checked against the data layout under the gc model must fail, or the test
  // above proves nothing.
  std::vector<std::string> Decls = {
      "type ZeroTail struct { f0 int32; f1 struct{} }"};
  std::vector<Entry> Table = {{"ZeroTail", 2}, {"string", 0}};
  Built B(program(Decls, Table), /*Lower=*/true);
  ASSERT_TRUE(B.CP && B.GM) << B.Errors;
  std::vector<Answer> Want = answers(*B.CP, Table);
  EXPECT_EQ(Want[0].Size, 4);  // not 8: no gc padding byte
  EXPECT_EQ(Want[1].Size, 24); // {ptr, len, cap}
  auto T = parseTarget("linux/amd64");
  auto TM = createTargetMachine(*T);
  ASSERT_TRUE(static_cast<bool>(TM));
  llvm::LLVMContext LC;
  auto LM = lowerToLLVM(LC, *B.GM, **TM);
  ASSERT_TRUE(static_cast<bool>(LM)) << llvm::toString(LM.takeError());
  const llvm::DataLayout &DL = (*LM)->getDataLayout();
  llvm::Type *Z = (*LM)->getNamedGlobal(B.GM->Globals[1].Name)->getValueType();
  EXPECT_EQ(DL.getTypeAllocSize(Z), 4u);
  EXPECT_NE(DL.getTypeAllocSize(Z), 8u); // gc's answer is not LLVM's
}

TEST(Layout, WhatTheCodeGeneratorDoesNotLowerYetStillHasD45Sizes) {
  // The table of D4.5 for types that have no GIR yet; the checker alone.
  std::vector<std::string> Decls = {"type I interface { M() }",
                                    "type Pt struct{ x, y int32 }"};
  struct Row {
    const char *Type;
    int64_t Size, Align;
  };
  const Row Rows[] = {{"[]int", 32, 8}, // ADR-0029: ptr, len, cap, allocator
                      {"[]byte", 32, 8},
                      {"[3][]int", 96, 8},
                      {"func()", 16, 8},
                      {"func(int) int", 16, 8},
                      {"any", 16, 8},
                      {"error", 16, 8},
                      {"I", 16, 8},
                      {"map[int]string", 8, 8},
                      {"chan int", 8, 8},
                      {"View[string]", 16, 8},
                      {"View[[]int]", 16, 8},
                      {"View[int]", 8, 8},
                      {"View[Pt]", 8, 8},
                      {"Option[I]", 16, 8},
                      {"Option[func()]", 16, 8},
                      {"[2]func()", 32, 8},
                      {"string", 24, 8}};
  std::vector<Entry> Table;
  for (const Row &R : Rows)
    Table.push_back({R.Type, 0});
  Built B(program(Decls, Table, /*Calls=*/true), /*Lower=*/false);
  ASSERT_TRUE(B.CP) << B.Errors;
  ASSERT_EQ(B.Errors, "");
  std::vector<Answer> Got = answers(*B.CP, Table);
  for (size_t I = 0; I < Table.size(); ++I) {
    EXPECT_EQ(Got[I].Size, Rows[I].Size) << "Sizeof(" << Rows[I].Type << ")";
    EXPECT_EQ(Got[I].Align, Rows[I].Align) << "Alignof(" << Rows[I].Type << ")";
  }
}

} // namespace
