//===- CABITest.cpp - The C ABI layer (P04.11) ----------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// docs/design/c-abi.md. What the run tests (tests/run/cabi-*.test) cannot
// show from outside:
//
//   - the classification of hand-written signatures on both Go++ targets, kind
//     by kind (Direct with each coerce type, Extend, Indirect byval / sret /
//     plain copy, Ignore), and both sides of the register, 16-byte and
//     homogeneous-aggregate boundaries;
//   - that the classification of scalars made without Clang (M3) is Clang's;
//   - the kinds no Go++ target produces: Expand (i686) and CoerceAndExpand
//     (swiftcc) classified by Clang, lowered by CABILower and, on the host,
//     run through a callee made by unpackCParams / packCReturn and a caller
//     made by lowerCArgs / lowerCResult (the two directions are inverse), and
//     the kinds that are refused (InAlloca, IndirectAliased, TargetSpecific);
//   - the three layouts of a type agree: GIR (gir::CTypeRules), LLVM's data
//     layout and Clang's record layout, on both targets;
//   - the cache keyed by signature, many threads asking at once, one Clang
//     world per target per process (M3);
//   - large structs reach Clang flat with the answer unchanged;
//   - gir::CTypeRules: both sides of each limit, and answers that do not
//     depend on the order of the questions;
//   - near-linear work (counted, M8) in every dimension of a signature and of
//     a program, each with a quadratic control.
//
//===----------------------------------------------------------------------===//

#include "gopp/Import/CABI.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/CodeGen/CABILower.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/GIR/CType.h"
#include "gopp/GIR/Lower.h"
#include "gopp/GIR/Verify.h"
#include "gopp/Sema/Loader.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <thread>

using namespace gopp;

namespace {

struct Fixture {
  gir::Context C;
  gir::Type *I8 = C.i8(), *U8 = C.u8(), *I16 = C.intTy(16, true),
            *I32 = C.i32(), *U32 = C.intTy(32, false), *I64 = C.i64(),
            *F32 = C.floatTy(32), *F64 = C.floatTy(64), *Bool = C.boolean(),
            *Ptr = C.ptr(C.u8());
  unsigned N = 0;
  gir::Type *st(std::initializer_list<gir::Type *> Fs) {
    return st(std::vector<gir::Type *>(Fs));
  }
  gir::Type *st(const std::vector<gir::Type *> &Fs) {
    gir::Type *S = C.structTy("S" + std::to_string(N++));
    C.setStructBody(S, Fs);
    return S;
  }
  gir::Type *fn(std::initializer_list<gir::Type *> Ps, gir::Type *R) {
    return C.func(std::vector<gir::Type *>(Ps), R ? R : C.unit());
  }
};

std::unique_ptr<cabi::Classifier> classifier(const char *Target) {
  auto T = parseTarget(Target);
  EXPECT_TRUE(bool(T));
  cabi::ClassifierOptions O;
  O.Triple = T->Triple;
  O.CPU = T->CPU;
  O.Features = T->Features;
  auto C = cabi::Classifier::create(O);
  EXPECT_TRUE(bool(C));
  return std::move(*C);
}

std::string lines(const cabi::CallInfo &CI) { return CI.str(); }

/// Every fact of an ArgInfo the lowering reads (str() leaves some out).
std::string everything(const cabi::ArgInfo &A) {
  std::string S = std::string(A.kindName()) +
                  " coerce=" + (A.HasCoerce ? A.Coerce.str() : "-") +
                  " off=" + std::to_string(A.Offset) +
                  " flat=" + std::to_string(A.Flatten) + " idx=";
  for (unsigned I : A.ExpandIdx)
    S += std::to_string(I) + ",";
  S += " inreg=" + std::to_string(A.InReg) + " sext=" + std::to_string(A.SExt) +
       " zext=" + std::to_string(A.ZExt) +
       " stack=" + std::to_string(A.StackAlign) +
       " pad=" + (A.HasPadding ? A.Padding.str() : "-") +
       std::to_string(A.PaddingInReg) + " align=" + std::to_string(A.Align) +
       " byval=" + std::to_string(A.ByVal) +
       " realign=" + std::to_string(A.Realign) +
       " as=" + std::to_string(A.AddrSpace) +
       " after=" + std::to_string(A.SRetAfterThis) +
       " inalloca=" + std::to_string(A.InAllocaField) +
       std::to_string(A.InAllocaIndirect) + std::to_string(A.InAllocaSRet);
  return S;
}

std::string everything(const cabi::CallInfo &CI) {
  std::string S = "ret " + everything(CI.Ret);
  for (const cabi::ArgInfo &A : CI.Args)
    S += "\narg " + everything(A);
  return S;
}

} // namespace

//===----------------------------------------------------------------------===//
// Classification, kind by kind, on the two Go++ targets
//===----------------------------------------------------------------------===//

TEST(CABIKinds, Amd64) {
  Fixture F;
  auto Cl = classifier("linux/amd64");
  // Scalars: a bool and a narrow integer are extended, the rest is direct.
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.I32, F.I8, F.Bool, F.U8, F.I16}, F.Bool))),
      "ret: extend i1 zeroext\n"
      "arg 0: direct i32\n"
      "arg 1: extend i8 signext\n"
      "arg 2: extend i1 zeroext\n"
      "arg 3: extend i8 zeroext\n"
      "arg 4: extend i16 signext");
  // Up to 16 bytes: the eightbytes in integer or SSE registers (a struct of
  // two eightbytes is flattened: one IR argument each); two floats in one
  // xmm register.
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.st({F.I32, F.I32})}, F.st({F.I32, F.I32})))),
      "ret: direct i64\narg 0: direct i64");
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.st({F.I64, F.I64})}, F.st({F.I64, F.I64})))),
      "ret: direct { i64, i64 } flatten\narg 0: direct { i64, i64 } flatten");
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.st({F.F32, F.F32})}, F.st({F.F32, F.F32})))),
      "ret: direct <2 x float>\narg 0: direct <2 x float>");
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.st({F.F64, F.I64})}, F.st({F.F64, F.I64})))),
      "ret: direct { double, i64 } flatten\n"
      "arg 0: direct { double, i64 } flatten");
  // A struct whose size is not a power of two is coerced to an odd integer.
  EXPECT_EQ(lines(Cl->classify(F.fn({F.st({F.C.array(F.I16, 3)})}, nullptr))),
            "ret: ignore\narg 0: direct i48");
  // Over 16 bytes: memory. A byval argument on the stack, a result through a
  // hidden pointer (sret).
  EXPECT_EQ(lines(Cl->classify(F.fn({F.st({F.I64, F.I64, F.I64})},
                                    F.st({F.I64, F.I64, F.I64})))),
            "ret: indirect align 8\narg 0: indirect byval align 8");
  // Empty struct: nothing is passed.
  EXPECT_EQ(lines(Cl->classify(F.fn({F.st({})}, F.st({})))),
            "ret: ignore\narg 0: ignore");
  // A struct {int32, uint8}: one eightbyte. (Option[int32] has this layout
  // but is no C type until P07, decision M6.)
  gir::Type *IU = F.st({F.I32, F.U8});
  EXPECT_EQ(lines(Cl->classify(F.fn({IU}, IU))),
            "ret: direct i64\narg 0: direct i64");
}

TEST(CABIKinds, Arm64) {
  Fixture F;
  auto Cl = classifier("linux/arm64");
  // Scalars are direct: AAPCS64 leaves the extension to the callee.
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.I32, F.I8, F.Bool}, F.Bool))),
      "ret: direct i1\narg 0: direct i32\narg 1: direct i8\narg 2: direct i1");
  // Up to 16 bytes: x0..x7, as an i64 or an array of two.
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.st({F.I32, F.I32})}, F.st({F.I32, F.I32})))),
      "ret: direct i64\narg 0: direct i64");
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.st({F.I64, F.I64})}, F.st({F.I64, F.I64})))),
      "ret: direct [2 x i64]\narg 0: direct [2 x i64]");
  // Homogeneous floating-point aggregates (HFA) of one to four members go in
  // consecutive vector registers: the argument is an array of the members.
  EXPECT_EQ(lines(Cl->classify(F.fn({F.st({F.F32, F.F32, F.F32})},
                                    F.st({F.F32, F.F32, F.F32})))),
            "ret: direct { float, float, float } flatten\n"
            "arg 0: direct [3 x float] alignstack 8");
  EXPECT_EQ(lines(Cl->classify(F.fn({F.st({F.F64, F.F64, F.F64, F.F64})},
                                    F.st({F.F64, F.F64, F.F64, F.F64})))),
            "ret: direct { double, double, double, double } flatten\n"
            "arg 0: direct [4 x double] alignstack 8");
  // A double and an integer is not an HFA.
  EXPECT_EQ(
      lines(Cl->classify(F.fn({F.st({F.F64, F.I64})}, F.st({F.F64, F.I64})))),
      "ret: direct [2 x i64]\narg 0: direct [2 x i64]");
  // Over 16 bytes: the caller makes a copy and passes its address (no byval);
  // a result goes through x8.
  EXPECT_EQ(lines(Cl->classify(F.fn({F.st({F.I64, F.I64, F.I64})},
                                    F.st({F.I64, F.I64, F.I64})))),
            "ret: indirect align 8\narg 0: indirect copy align 8");
  EXPECT_EQ(lines(Cl->classify(F.fn({F.st({})}, F.st({})))),
            "ret: ignore\narg 0: ignore");
}

TEST(CABIKinds, TheGoTargetsReachDirectExtendIndirectAndIgnore) {
  // Every ABIArgInfo kind a Go++ target can produce, from signatures made by
  // a seeded generator: both targets reach Direct, Indirect and Ignore;
  // amd64 reaches Extend (arm64 leaves the extension to the callee).
  Fixture F;
  std::mt19937 R(7);
  std::vector<gir::Type *> Scalars = {F.I8,  F.U8,  F.I16, F.I32,  F.U32,
                                      F.I64, F.F32, F.F64, F.Bool, F.Ptr};
  auto Pick = [&] { return Scalars[R() % Scalars.size()]; };
  std::vector<gir::Type *> Structs = {F.st({})};
  for (unsigned I = 0; I < 40; ++I) {
    std::vector<gir::Type *> Fs;
    for (unsigned K = R() % 6 + 1; K; --K)
      Fs.push_back(R() % 5 == 0 ? F.C.array(Pick(), R() % 5 + 1) : Pick());
    Structs.push_back(F.st(Fs));
  }
  std::vector<gir::Type *> Big;
  for (unsigned I = 0; I < 20; ++I)
    Big.push_back(F.I64);
  Structs.push_back(F.st(Big));
  for (const char *Tgt : {"linux/amd64", "linux/arm64"}) {
    auto Cl = classifier(Tgt);
    std::set<std::string> Seen;
    for (gir::Type *S : Structs)
      for (gir::Type *T : {S, Pick()}) {
        const cabi::CallInfo &CI = Cl->classify(F.fn({T, F.I8}, T));
        Seen.insert(CI.Ret.kindName());
        for (const cabi::ArgInfo &A : CI.Args)
          Seen.insert(A.kindName());
      }
    EXPECT_TRUE(Seen.count("direct")) << Tgt;
    EXPECT_TRUE(Seen.count("indirect")) << Tgt;
    EXPECT_TRUE(Seen.count("ignore")) << Tgt;
    EXPECT_EQ(Seen.count("extend") == 1, std::string(Tgt) == "linux/amd64")
        << Tgt;
    for (const std::string &K : Seen)
      EXPECT_TRUE(K == "direct" || K == "extend" || K == "indirect" ||
                  K == "ignore")
          << Tgt << " produced " << K;
  }
}

TEST(CABIKinds, SomeKindsAreReachableOnlyOffTheGoTargets) {
  Fixture F;
  // Expand: a struct of 32-bit scalars on i686.
  cabi::ClassifierOptions O;
  O.Triple = "i686-unknown-linux-gnu";
  auto Cl = cabi::Classifier::create(O);
  ASSERT_TRUE(bool(Cl));
  EXPECT_EQ(lines((*Cl)->classify(F.fn({F.st({F.I32, F.I32})}, nullptr))),
            "ret: ignore\narg 0: expand");
  // CoerceAndExpand: swiftcc.
  O.Triple = "x86_64-unknown-linux-gnu";
  O.Swift = true;
  auto Sw = cabi::Classifier::create(O);
  ASSERT_TRUE(bool(Sw));
  EXPECT_EQ(lines((*Sw)->classify(F.fn({F.st({F.F64, F.I64, F.I8})}, nullptr))),
            "ret: ignore\narg 0: coerce-and-expand { double, i64, i8 }");
}

TEST(CABIKinds, TheKindsNoTargetUsesAreRefused) {
  Fixture F;
  using K = cabi::ArgInfo::Kind;
  for (K Kind : {K::InAlloca, K::IndirectAliased, K::TargetSpecific}) {
    cabi::CallInfo CI;
    CI.Ret.K = K::Ignore;
    cabi::ArgInfo A;
    A.K = Kind;
    A.Ty = F.I32;
    CI.Args.push_back(A);
    llvm::Error E = cabi::checkSupported(CI);
    ASSERT_TRUE(bool(E)) << A.kindName();
    EXPECT_NE(llvm::toString(std::move(E)).find(A.kindName()),
              std::string::npos);
  }
  // And the kinds that are fine, and an Expand result (never valid).
  cabi::CallInfo Ok;
  Ok.Ret.K = K::Ignore;
  for (K Kind :
       {K::Direct, K::Extend, K::Indirect, K::Ignore, K::CoerceAndExpand}) {
    cabi::ArgInfo A;
    A.K = Kind;
    A.Ty = F.I32;
    Ok.Args.push_back(A);
  }
  EXPECT_FALSE(bool(cabi::checkSupported(Ok)));
  cabi::CallInfo ExpandRet;
  ExpandRet.Ret.K = K::Expand;
  ExpandRet.Ret.Ty = F.st({F.I32});
  EXPECT_TRUE(bool(cabi::checkSupported(ExpandRet)));
}

TEST(CABIKinds, Amd64RegistersRunOut) {
  // Both sides of each register rule (c2's table): a struct that needs two
  // integer registers goes to memory when one is left, a struct of one
  // eightbyte still fits the last one, a scalar stays Direct (LLVM puts it on
  // the stack), and a struct of two doubles goes to memory when the eight
  // vector registers are taken.
  Fixture F;
  auto Cl = classifier("linux/amd64");
  gir::Type *P = F.st({F.I64, F.I64}), *One = F.st({F.I64});
  auto Last = [&](std::vector<gir::Type *> Ps) {
    return Cl->classify(F.C.func(Ps, F.C.unit())).Args.back().str();
  };
  std::vector<gir::Type *> I4(4, F.I64), I5(5, F.I64), I6(6, F.I64),
      D8(8, F.F64);
  auto With = [](std::vector<gir::Type *> Ps, gir::Type *T) {
    Ps.push_back(T);
    return Ps;
  };
  EXPECT_EQ(Last(With(I4, P)), "direct { i64, i64 } flatten");
  EXPECT_EQ(Last(With(I5, P)), "indirect byval align 8");
  EXPECT_EQ(Last(With(I6, P)), "indirect byval align 8");
  EXPECT_EQ(Last(With(I6, F.I64)), "direct i64");
  EXPECT_EQ(Last(With(I5, One)), "direct i64");
  EXPECT_EQ(Last(With(I6, One)), "direct i64");
  EXPECT_EQ(Last(With(D8, F.F64)), "direct double");
  EXPECT_EQ(Last(With(D8, F.st({F.F64, F.F64}))), "indirect byval align 8");
  EXPECT_EQ(
      Last(With(std::vector<gir::Type *>(7, F.F64), F.st({F.F64, F.F64}))),
      "indirect byval align 8");
  EXPECT_EQ(
      Last(With(std::vector<gir::Type *>(6, F.F64), F.st({F.F64, F.F64}))),
      "direct { double, double } flatten");
}

TEST(CABIKinds, BothSidesOfTheBoundaries) {
  // 16 bytes is the edge of registers on both targets; four members of one
  // float type is the edge of a homogeneous aggregate on arm64, which also
  // sees through nesting and arrays (c2's table).
  Fixture F;
  auto Bytes = [&](unsigned N) { return F.st({F.C.array(F.I8, N)}); };
  auto Amd = classifier("linux/amd64");
  EXPECT_EQ(Amd->classify(F.fn({Bytes(16)}, nullptr)).Args[0].str(),
            "direct { i64, i64 } flatten");
  EXPECT_EQ(Amd->classify(F.fn({Bytes(17)}, nullptr)).Args[0].str(),
            "indirect byval align 8");
  EXPECT_EQ(Amd->classify(F.fn({Bytes(14)}, nullptr)).Args[0].str(),
            "direct { i64, i48 } flatten");
  EXPECT_EQ(
      Amd->classify(F.fn({F.st({F.I8, F.I16, F.Bool})}, nullptr)).Args[0].str(),
      "direct i48");
  EXPECT_EQ(Amd->classify(F.fn({}, Bytes(16))).Ret.str(true),
            "direct { i64, i64 } flatten");
  EXPECT_EQ(Amd->classify(F.fn({}, Bytes(17))).Ret.str(true),
            "indirect align 1");
  auto Arm = classifier("linux/arm64");
  EXPECT_EQ(Arm->classify(F.fn({Bytes(16)}, nullptr)).Args[0].str(),
            "direct [2 x i64]");
  EXPECT_EQ(Arm->classify(F.fn({Bytes(17)}, nullptr)).Args[0].str(),
            "indirect copy align 1");
  EXPECT_EQ(Arm->classify(F.fn({}, Bytes(16))).Ret.str(true),
            "direct [2 x i64]");
  EXPECT_EQ(Arm->classify(F.fn({}, Bytes(17))).Ret.str(true),
            "indirect align 1");
  auto ArmArg = [&](gir::Type *T) {
    return Arm->classify(F.fn({T}, nullptr)).Args[0].str();
  };
  EXPECT_EQ(ArmArg(F.st({F.C.array(F.F32, 4)})),
            "direct [4 x float] alignstack 8");
  EXPECT_EQ(ArmArg(F.st({F.C.array(F.F32, 5)})), "indirect copy align 4");
  EXPECT_EQ(ArmArg(F.st({F.st({F.F32, F.F32}), F.st({F.F32})})),
            "direct [3 x float] alignstack 8");
  EXPECT_EQ(ArmArg(F.st({F.F32, F.F64})), "direct [2 x i64]");
  EXPECT_EQ(ArmArg(F.st({F.F32, F.I32})), "direct i64");
  EXPECT_EQ(ArmArg(F.st({F.st({}), F.F64, F.F64})),
            "direct [2 x double] alignstack 8");
}

TEST(CABIKinds, ScalarsWithoutClangAreClangs) {
  // M3: a signature of scalars only is classified without Clang
  // (cabi::classifyScalars). Every fact of its answer must be Clang's, for
  // every scalar as a parameter and as the result, alone and after enough of
  // them to use up every register class, on both targets.
  Fixture F;
  std::vector<gir::Type *> Scalars = {
      F.Bool,    F.I8,  F.U8,  F.I16, F.C.intTy(16, false), F.I32, F.U32, F.I64,
      F.C.u64(), F.F32, F.F64, F.Ptr};
  for (const char *Tgt : {"linux/amd64", "linux/arm64"}) {
    auto Spec = parseTarget(Tgt);
    ASSERT_TRUE(bool(Spec));
    auto Cl = classifier(Tgt);
    unsigned N = 0;
    auto Same = [&](gir::Type *Sig) {
      std::optional<cabi::CallInfo> Mine =
          cabi::classifyScalars(Spec->Triple, Sig);
      ASSERT_TRUE(Mine.has_value()) << Sig->str();
      EXPECT_EQ(everything(*Mine), everything(Cl->classify(Sig)))
          << Tgt << " " << Sig->str();
      ++N;
    };
    for (gir::Type *T : Scalars) {
      Same(F.C.func({T}, F.C.unit()));
      Same(F.C.func({}, T));
      Same(F.C.func(std::vector<gir::Type *>(20, T), T));
    }
    std::vector<gir::Type *> Mixed;
    for (unsigned I = 0; I < 30; ++I)
      Mixed.push_back(Scalars[I % Scalars.size()]);
    Same(F.C.func(Mixed, F.Bool));
    EXPECT_EQ(N, 3 * Scalars.size() + 1);
  }
  // Not for an aggregate, nor for another target: Clang answers those.
  EXPECT_FALSE(cabi::classifyScalars("x86_64-unknown-linux-gnu",
                                     F.fn({F.st({F.I32})}, nullptr)));
  EXPECT_FALSE(cabi::classifyScalars("aarch64-unknown-linux-gnu",
                                     F.fn({}, F.C.option(F.I32))));
  EXPECT_FALSE(
      cabi::classifyScalars("i686-unknown-linux-gnu", F.fn({F.I32}, nullptr)));
}

//===----------------------------------------------------------------------===//
// Layouts: GIR, LLVM and Clang agree
//===----------------------------------------------------------------------===//

namespace {

/// The memory type of a GIR type in LLVM, as LowerToLLVM makes it.
struct MemTypes {
  explicit MemTypes(llvm::LLVMContext &C) : C(C) {}
  llvm::LLVMContext &C;
  llvm::DenseMap<const gir::Type *, llvm::Type *> M;
  llvm::Type *get(const gir::Type *T) {
    if (auto It = M.find(T); It != M.end())
      return It->second;
    llvm::Type *R = nullptr;
    switch (T->getKind()) {
    case gir::Type::Kind::Bool:
      R = llvm::Type::getInt8Ty(C);
      break;
    case gir::Type::Kind::Int:
      R = llvm::IntegerType::get(C, T->bits());
      break;
    case gir::Type::Kind::Float:
      R = T->bits() == 32 ? llvm::Type::getFloatTy(C)
                          : llvm::Type::getDoubleTy(C);
      break;
    case gir::Type::Kind::Array:
      R = llvm::ArrayType::get(get(T->elem()), T->length());
      break;
    case gir::Type::Kind::Option:
      R = llvm::StructType::get(C, {get(T->elem()), llvm::Type::getInt8Ty(C)});
      break;
    case gir::Type::Kind::Struct: {
      llvm::SmallVector<llvm::Type *, 4> Fs;
      for (const gir::Type *E : T->elems())
        Fs.push_back(get(E));
      R = llvm::StructType::get(C, Fs);
      break;
    }
    default:
      R = llvm::PointerType::getUnqual(C);
    }
    return M[T] = R;
  }
};

/// A seeded random C type of at most the given depth.
gir::Type *randomType(Fixture &F, std::mt19937 &R, unsigned Depth) {
  gir::Type *Sc[] = {F.I8,  F.U8,  F.I16, F.I32,  F.U32,
                     F.I64, F.F32, F.F64, F.Bool, F.Ptr};
  unsigned K = R() % 10;
  if (Depth == 0 || K < 4)
    return Sc[R() % 10];
  if (K == 4)
    return F.C.option(Sc[R() % 9]);
  if (K == 5)
    return F.C.array(randomType(F, R, Depth - 1), R() % 4 + 1);
  std::vector<gir::Type *> Fs;
  for (unsigned N = R() % 5; N; --N)
    Fs.push_back(randomType(F, R, Depth - 1));
  return F.st(Fs);
}

} // namespace

TEST(CABILayout, GirLlvmAndClangAgree) {
  initializeTargets();
  for (const char *Tgt : {"linux/amd64", "linux/arm64"}) {
    auto Spec = parseTarget(Tgt);
    ASSERT_TRUE(bool(Spec));
    llvm::DataLayout DL(Spec->DataLayout);
    auto Cl = classifier(Tgt);
    llvm::LLVMContext LC;
    MemTypes MT(LC);
    Fixture F;
    std::mt19937 R(11);
    gir::CTypeRules Rules;
    unsigned Structs = 0, Checked = 0;
    for (unsigned I = 0; I < 400; ++I) {
      gir::Type *T = randomType(F, R, 3);
      if (!Rules.check(T, gir::CUse::Field).ok())
        continue;
      gir::CTypeRules::Layout G = Rules.layout(T);
      llvm::Type *M = MT.get(T);
      cabi::ClangLayout CL = Cl->layoutOf(T);
      EXPECT_EQ(G.Size, DL.getTypeAllocSize(M)) << Tgt << " " << T->str();
      EXPECT_EQ(G.Align, DL.getABITypeAlign(M).value())
          << Tgt << " " << T->str();
      EXPECT_EQ(G.Size, CL.Size) << Tgt << " " << T->str();
      EXPECT_EQ(G.Align, CL.Align) << Tgt << " " << T->str();
      ++Checked;
      if (T->getKind() == gir::Type::Kind::Struct) {
        ++Structs;
        const llvm::StructLayout *SL =
            DL.getStructLayout(llvm::cast<llvm::StructType>(M));
        ASSERT_EQ(CL.Offsets.size(), T->elems().size());
        for (unsigned K = 0; K < CL.Offsets.size(); ++K)
          EXPECT_EQ(CL.Offsets[K], SL->getElementOffset(K))
              << Tgt << " " << T->str() << " field " << K;
      }
      if (T->isOption()) {
        const llvm::StructLayout *SL =
            DL.getStructLayout(llvm::cast<llvm::StructType>(M));
        ASSERT_EQ(CL.Offsets.size(), 2u);
        EXPECT_EQ(CL.Offsets[1], SL->getElementOffset(1));
      }
    }
    EXPECT_GT(Structs, 60u);
    EXPECT_GT(Checked, 250u);
  }
}

TEST(CABILayout, ThePositiveControlSeesAPackedLayout) {
  // The comparison above must be able to fail: the same struct, packed, has
  // another size.
  Fixture F;
  llvm::LLVMContext LC;
  auto Spec = parseTarget("linux/amd64");
  ASSERT_TRUE(bool(Spec));
  llvm::DataLayout DL(Spec->DataLayout);
  gir::Type *S = F.st({F.I8, F.I64, F.I8});
  gir::CTypeRules Rules;
  ASSERT_TRUE(Rules.check(S, gir::CUse::Field).ok());
  llvm::Type *Packed = llvm::StructType::get(LC,
                                             {llvm::Type::getInt8Ty(LC),
                                              llvm::Type::getInt64Ty(LC),
                                              llvm::Type::getInt8Ty(LC)},
                                             true);
  EXPECT_NE(Rules.layout(S).Size, DL.getTypeAllocSize(Packed));
  EXPECT_EQ(Rules.layout(S).Size, 24u);
  EXPECT_EQ(DL.getTypeAllocSize(Packed), 10u);
}

//===----------------------------------------------------------------------===//
// The cache and the threads
//===----------------------------------------------------------------------===//

TEST(CABICache, ASignatureIsClassifiedOnce) {
  Fixture F;
  auto Cl = classifier("linux/amd64");
  gir::Type *S = F.st({F.I64, F.I64, F.I64});
  gir::Type *Sig = F.fn({S, F.I32}, F.I32);
  const cabi::CallInfo &A = Cl->classify(Sig);
  EXPECT_EQ(Cl->stats().Classified, 1u);
  uint64_t Mapped = Cl->stats().TypesMapped;
  for (unsigned I = 0; I < 100; ++I)
    EXPECT_EQ(&Cl->classify(Sig), &A);
  EXPECT_EQ(Cl->stats().Classified, 1u);
  EXPECT_EQ(Cl->stats().Asked, 101u);
  EXPECT_EQ(Cl->stats().TypesMapped, Mapped);
  // Types are uniqued: the same signature built again is the same key.
  EXPECT_EQ(&Cl->classify(F.fn({S, F.I32}, F.I32)), &A);
  // A different signature over the same types maps no type again.
  Cl->classify(F.fn({F.I32}, S));
  EXPECT_EQ(Cl->stats().Classified, 2u);
  EXPECT_EQ(Cl->stats().TypesMapped, Mapped);
}

TEST(CABICache, TheAnswerDoesNotDependOnWhoAsksFirst) {
  // Eight threads ask for the same few hundred signatures in different
  // orders; the answers equal the ones a single thread gets.
  Fixture F;
  std::mt19937 R(3);
  std::vector<gir::Type *> Sigs;
  for (unsigned I = 0; I < 300; ++I) {
    std::vector<gir::Type *> Ps;
    for (unsigned N = R() % 5; N; --N)
      Ps.push_back(randomType(F, R, 2));
    gir::Type *Ret = randomType(F, R, 2);
    // A result must be a C result: an array is not.
    if (Ret->getKind() == gir::Type::Kind::Array)
      Ret = F.st({Ret});
    for (gir::Type *&P : Ps)
      if (P->getKind() == gir::Type::Kind::Array)
        P = F.st({P});
    Sigs.push_back(F.C.func(Ps, Ret));
  }
  for (const char *Tgt : {"linux/amd64", "linux/arm64"}) {
    auto Alone = classifier(Tgt);
    std::vector<std::string> Want;
    for (gir::Type *S : Sigs)
      Want.push_back(Alone->classify(S).str());
    auto Shared = classifier(Tgt);
    std::vector<std::thread> Ts;
    std::vector<std::vector<std::string>> Got(8);
    for (unsigned T = 0; T < 8; ++T)
      Ts.emplace_back([&, T] {
        std::vector<unsigned> Order(Sigs.size());
        for (unsigned I = 0; I < Order.size(); ++I)
          Order[I] = I;
        std::mt19937 TR(T);
        std::shuffle(Order.begin(), Order.end(), TR);
        Got[T].resize(Sigs.size());
        for (unsigned I : Order)
          Got[T][I] = Shared->classify(Sigs[I]).str();
      });
    for (std::thread &T : Ts)
      T.join();
    for (unsigned T = 0; T < 8; ++T)
      EXPECT_EQ(Got[T], Want) << Tgt;
    EXPECT_EQ(Shared->stats().Classified, Alone->stats().Classified);
    EXPECT_EQ(Shared->stats().Asked, 8 * Sigs.size());
  }
}

//===----------------------------------------------------------------------===//
// Flat records: a struct of no bytes or of more than 128 reaches Clang flat
//===----------------------------------------------------------------------===//

TEST(CABIFlat, FlatStructsClassifyLikeTheOriginals) {
  // ClassifierOptions::FlatSize (c1): a struct of more than 128 bytes reaches
  // Clang as a record of its size and alignment with one array field, so
  // Clang never walks its fields. That must not change one fact of an
  // answer, coerce types included, on either target: random structs from no
  // bytes to over 1000, nested, with arrays (also of length 0), homogeneous
  // float runs and empty structs, as a parameter (alone and after the
  // registers are used up) and as the result, with and without it. (This
  // test is why a struct of no bytes is not flattened: nested in an arm64
  // HFA result, its stand-in changed the coerce type.) The layout (layoutOf)
  // is the real record's in both.
  for (const char *Tgt : {"linux/amd64", "linux/arm64"}) {
    auto Spec = parseTarget(Tgt);
    ASSERT_TRUE(bool(Spec));
    cabi::ClassifierOptions O;
    O.Triple = Spec->Triple;
    O.CPU = Spec->CPU;
    O.Features = Spec->Features;
    auto Flat = cabi::Classifier::create(O);
    O.FlatSize = 0;
    auto Exact = cabi::Classifier::create(O);
    ASSERT_TRUE(Flat && Exact);
    Fixture F;
    std::mt19937 R(11);
    gir::Type *Sc[] = {F.I8,  F.U8,  F.I16, F.I32,  F.U32,
                       F.I64, F.F32, F.F64, F.Bool, F.Ptr};
    unsigned Big = 0, Zero = 0, Small = 0;
    for (int I = 0; I < 1500; ++I) {
      std::vector<gir::Type *> Fs;
      unsigned Fields = R() % 40;
      bool Hfa = R() % 3 == 0;
      for (unsigned J = 0; J < Fields; ++J) {
        gir::Type *E = Hfa ? F.F32 : Sc[R() % 10];
        if (R() % 6 == 0)
          E = F.C.array(E, R() % 5);
        else if (R() % 9 == 0)
          E = F.st({E, F.st({})});
        Fs.push_back(E);
      }
      if (R() % 10 == 0) { // a struct of zero-size things only
        Fs.clear();
        for (unsigned K = R() % 4; K; --K)
          Fs.push_back(R() % 2 ? F.st({}) : F.C.array(F.I64, 0));
      }
      gir::Type *S = F.st(Fs);
      std::vector<gir::Type *> Ps(R() % 2 ? 0 : 7, F.I64);
      Ps.push_back(S);
      gir::Type *Sig = F.C.func(Ps, S);
      EXPECT_EQ(everything((*Flat)->classify(Sig)),
                everything((*Exact)->classify(Sig)))
          << Tgt << " " << S->str();
      cabi::ClangLayout L = (*Exact)->layoutOf(S);
      EXPECT_EQ((*Flat)->layoutOf(S).Offsets, L.Offsets) << S->str();
      Big += L.Size > 128;
      Zero += L.Size == 0;
      Small += L.Size && L.Size <= 128;
    }
    // The table reaches both flat cases and the classified one.
    EXPECT_GT(Big, 100u) << Tgt;
    EXPECT_GT(Zero, 20u) << Tgt;
    EXPECT_GT(Small, 100u) << Tgt;
  }
}

TEST(CABIFlat, ADiamondAndAWideStructAreClassifiedAtOnce) {
  // Clang's homogeneous-aggregate and empty-record walks have no memo: a
  // struct that holds the struct below twice, 38 levels deep, has 2^38 paths
  // (arm64 visits them all). Flat, it is classified at once on both targets;
  // so is a struct of 20000 fields. (Clang's steps cannot be counted from
  // here: a regression is a test that does not finish.) gir::CTypeRules
  // refuses both before Clang sees them, and its field limit is what bounds
  // Clang's walk of the small structs that are not flattened (a diamond of
  // empty structs): this is the classifier's own guard for the large ones.
  using K = cabi::ArgInfo::Kind;
  for (const char *Tgt : {"linux/amd64", "linux/arm64"}) {
    Fixture F;
    auto Cl = classifier(Tgt);
    gir::Type *D = F.F32;
    for (int I = 0; I < 38; ++I)
      D = F.st({D, D});
    const cabi::CallInfo &A = Cl->classify(F.fn({D}, D));
    EXPECT_EQ(A.Ret.K, K::Indirect) << Tgt;
    EXPECT_EQ(A.Args[0].K, K::Indirect) << Tgt;
    gir::Type *W = F.st(std::vector<gir::Type *>(20000, F.I32));
    EXPECT_EQ(Cl->classify(F.fn({W}, nullptr)).Args[0].K, K::Indirect) << Tgt;
  }
}

//===----------------------------------------------------------------------===//
// The Clang world: once per target per process (M3)
//===----------------------------------------------------------------------===//

namespace {

/// A C declaration with this signature in a module of its own.
void declareC(gir::Module &M, const char *Name, gir::Type *Sig) {
  auto F = std::make_unique<gir::Function>();
  F->Name = Name;
  F->Sig = Sig;
  F->CC = gir::CallConv::C;
  F->NoUnwind = true;
  M.addFunction(std::move(F));
}

} // namespace

TEST(CABIWorld, OneWorldPerTargetPerProcess) {
  // A module that passes only scalars to C never starts Clang; the first one
  // that passes an aggregate makes the target's world, and every later
  // module (another gir::Context) and classifier of the target reuses it.
  // The target has a feature no other test asks for, so the world is this
  // test's own whatever ran before in the process.
  initializeTargets();
  auto Spec = parseTarget("linux/amd64");
  ASSERT_TRUE(bool(Spec));
  Spec->Features = "+cx16";
  auto TM = createTargetMachine(*Spec);
  ASSERT_TRUE(bool(TM)) << toString(TM.takeError());
  auto Lower = [&](gir::Module &M) {
    llvm::LLVMContext LC;
    auto LM = lowerToLLVM(LC, M, **TM);
    EXPECT_TRUE(bool(LM)) << (LM ? "" : toString(LM.takeError()));
  };
  unsigned Before = cabi::clangWorldsMade();
  {
    gir::Context C;
    gir::Module M(C, "scalars");
    declareC(M, "s1", C.func({C.i8(), C.boolean(), C.floatTy(64)}, C.i32()));
    declareC(M, "s2", C.func({C.ptr(C.u8())}, C.unit()));
    Lower(M);
  }
  EXPECT_EQ(cabi::clangWorldsMade(), Before) << "scalars started Clang";
  for (unsigned Round = 0; Round < 3; ++Round) {
    gir::Context C;
    gir::Module M(C, "aggregates");
    gir::Type *S = C.structTy("S");
    C.setStructBody(S, {C.i64(), C.i64(), C.i64()});
    declareC(M, "a", C.func({S, C.i8()}, S));
    Lower(M);
    EXPECT_EQ(cabi::clangWorldsMade(), Before + 1) << "round " << Round;
  }
  cabi::ClassifierOptions O;
  O.Triple = Spec->Triple;
  O.CPU = Spec->CPU;
  O.Features = Spec->Features;
  ASSERT_TRUE(bool(cabi::Classifier::create(O)));
  EXPECT_EQ(cabi::clangWorldsMade(), Before + 1);
}

TEST(CABIWorld, ModulesOnManyThreadsGetTheAnswersOfOne) {
  // Eight threads, each with a module of its own (its gir::Context and its
  // classifier), classify the same seeded signatures at once in shuffled
  // orders through the one world of the target: every answer equals the one
  // a single module gets, whichever thread made a record first.
  auto Sigs = [](Fixture &F, std::vector<gir::Type *> &Out) {
    std::mt19937 R(17);
    gir::Type *Sc[] = {F.I8, F.U8, F.I16, F.I32, F.I64, F.F32, F.F64, F.Bool};
    for (unsigned I = 0; I < 200; ++I) {
      std::vector<gir::Type *> Fs;
      for (unsigned K = R() % 5 + 1; K; --K)
        Fs.push_back(R() % 4 ? Sc[R() % 8] : F.C.array(Sc[R() % 8], R() % 6));
      gir::Type *S = F.st(Fs);
      Out.push_back(F.fn({S, Sc[R() % 8], S}, R() % 2 ? S : Sc[R() % 8]));
    }
  };
  for (const char *Tgt : {"linux/amd64", "linux/arm64"}) {
    std::vector<std::string> Want;
    {
      Fixture F;
      std::vector<gir::Type *> S;
      Sigs(F, S);
      auto Cl = classifier(Tgt);
      for (gir::Type *Sig : S)
        Want.push_back(Cl->classify(Sig).str());
    }
    std::vector<std::vector<std::string>> Got(8);
    std::vector<std::thread> Ts;
    for (unsigned T = 0; T < 8; ++T)
      Ts.emplace_back([&, T] {
        Fixture F;
        std::vector<gir::Type *> S;
        Sigs(F, S);
        auto Cl = classifier(Tgt);
        std::vector<unsigned> Order(S.size());
        for (unsigned I = 0; I < Order.size(); ++I)
          Order[I] = I;
        std::mt19937 R(T);
        std::shuffle(Order.begin(), Order.end(), R);
        Got[T].resize(S.size());
        for (unsigned I : Order)
          Got[T][I] = Cl->classify(S[I]).str();
      });
    for (std::thread &T : Ts)
      T.join();
    for (unsigned T = 0; T < 8; ++T)
      EXPECT_EQ(Got[T], Want) << Tgt << " thread " << T;
  }
}

TEST(CABIWorld, ModulesShareRecordsButNotTypes) {
  // The world keeps one C record per structure, whichever module asks; a
  // classifier keeps the GIR types of its own module. Modules with the same
  // shapes get the same answers, and a struct name another module gave
  // another shape gets the answer of its own shape.
  auto Answers = [](bool Swap) {
    Fixture F;
    auto Cl = classifier("linux/amd64");
    gir::Type *A = F.st({F.I32, F.I32}), *B = F.st({F.I64, F.I64, F.I64});
    if (Swap)
      std::swap(A, B); // S0 and S1 now name the other shapes
    std::vector<std::string> Out;
    for (gir::Type *T : {A, B})
      Out.push_back(Cl->classify(F.fn({T}, T)).str());
    Out.push_back(Cl->classify(F.fn({F.st({F.F64, F.F64})}, nullptr)).str());
    return Out;
  };
  std::vector<std::string> First = Answers(false);
  EXPECT_EQ(Answers(false), First);
  std::vector<std::string> Swapped = Answers(true);
  EXPECT_EQ(Swapped[0], First[1]);
  EXPECT_EQ(Swapped[1], First[0]);
  EXPECT_EQ(Swapped[2], First[2]);
}

//===----------------------------------------------------------------------===//
// Lowering: caller and callee made from one CallInfo
//===----------------------------------------------------------------------===//

namespace {

/// One module with a callee `callee(params...) -> result`, written by
/// unpackCParams/packCReturn to return its argument number RetArg, and
/// `void caller(ptr in, ptr out)`, written by lowerCArgs/lowerCResult:
/// it loads the parameters from the struct `in` (the parameter types in
/// order), calls the callee, and stores the result at `out`.
struct RoundTrip {
  std::unique_ptr<llvm::LLVMContext> Ctx =
      std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> M;
  llvm::StructType *InTy = nullptr;
  const gir::Type *Sig = nullptr;
  std::string Error;
  unsigned IRParams = 0;
  cabi::CallInfo Info;
  std::string IR;

  RoundTrip(const char *Triple, bool Swift, const gir::Type *Sig,
            unsigned RetArg)
      : Sig(Sig) {
    cabi::ClassifierOptions O;
    O.Triple = Triple;
    O.Swift = Swift;
    auto Cl = cabi::Classifier::create(O);
    if (!Cl) {
      Error = toString(Cl.takeError());
      return;
    }
    Info = (*Cl)->classify(Sig);
    if (llvm::Error E = cabi::checkSupported(Info)) {
      Error = toString(std::move(E));
      return;
    }
    llvm::Triple T(Triple);
    M = std::make_unique<llvm::Module>("rt", *Ctx);
    M->setTargetTriple(T);
    // The data layouts the targets pin (CodeGen/Target.cpp), and i686's.
    M->setDataLayout(T.isX86() && T.isArch64Bit()
                         ? "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:"
                           "128-f80:128-n8:16:32:64-S128"
                     : T.isAArch64()
                         ? "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:"
                           "16:32-i64:64-i128:128-n32:64-S128-Fn32"
                         : "e-m:e-p:32:32-p270:32:32-p271:32:32-p272:64:64-"
                           "i128:128-f64:32:64-f80:32-n8:16:32-S128");
    const llvm::DataLayout &DL = M->getDataLayout();
    MemTypes MT(*Ctx);
    auto MemFn = [&](const gir::Type *X) { return MT.get(X); };
    cabi::IRSignature S = cabi::irSignature(*Ctx, Info, MemFn);
    IRParams = S.Type->getNumParams();
    auto CC = Swift ? llvm::CallingConv::Swift : llvm::CallingConv::C;

    auto *Callee = llvm::Function::Create(
        S.Type, llvm::GlobalValue::ExternalLinkage, "callee", M.get());
    Callee->setAttributes(S.Attrs);
    Callee->setCallingConv(CC);
    {
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(*Ctx, "entry", Callee));
      cabi::Env E{B, DL, MemFn,
                  [&](llvm::Type *Ty, llvm::Align A, const llvm::Twine &N) {
                    auto *Al = B.CreateAlloca(Ty, nullptr, N);
                    Al->setAlignment(std::max(A, DL.getABITypeAlign(Ty)));
                    return Al;
                  }};
      std::vector<llvm::Argument *> Args;
      for (llvm::Argument &A : Callee->args())
        Args.push_back(&A);
      std::vector<cabi::Value> Vals = cabi::unpackCParams(E, Info, S, Args);
      cabi::packCReturn(E, Info, S, Args, Vals[RetArg]);
    }

    std::vector<llvm::Type *> InFs;
    for (const gir::Type *P : Sig->elems())
      InFs.push_back(MT.get(P));
    InTy = llvm::StructType::get(*Ctx, InFs);
    llvm::Type *Ptr = llvm::PointerType::getUnqual(*Ctx);
    auto *Caller = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(*Ctx), {Ptr, Ptr}, false),
        llvm::GlobalValue::ExternalLinkage, "caller", M.get());
    {
      llvm::IRBuilder<> B(llvm::BasicBlock::Create(*Ctx, "entry", Caller));
      cabi::Env E{B, DL, MemFn,
                  [&](llvm::Type *Ty, llvm::Align A, const llvm::Twine &N) {
                    auto *Al = B.CreateAlloca(Ty, nullptr, N);
                    Al->setAlignment(std::max(A, DL.getABITypeAlign(Ty)));
                    return Al;
                  }};
      const llvm::StructLayout *SL = DL.getStructLayout(InTy);
      std::vector<cabi::Value> Vals;
      for (unsigned I = 0; I < Sig->elems().size(); ++I) {
        const gir::Type *P = Sig->elems()[I];
        cabi::Value V;
        V.Ty = P;
        llvm::Value *Addr = B.CreateConstInBoundsGEP1_64(
            B.getInt8Ty(), Caller->getArg(0), SL->getElementOffset(I));
        llvm::Type *PT = MT.get(P);
        if (P->isScalar()) {
          llvm::Value *L = B.CreateLoad(PT, Addr);
          V.V = P->isBool() ? B.CreateICmpNE(L, B.getInt8(0)) : L;
        } else {
          V.V = Addr;
          V.Known = DL.getABITypeAlign(PT);
        }
        Vals.push_back(V);
      }
      cabi::Value Dest;
      Dest.Ty = Sig->ret();
      Dest.V = Caller->getArg(1);
      Dest.Known = DL.getABITypeAlign(MT.get(Sig->ret()));
      cabi::PreparedCall P = cabi::lowerCArgs(E, Info, S, Vals, Dest);
      llvm::CallInst *Call = B.CreateCall(S.Type, Callee, P.Args);
      Call->setAttributes(S.Attrs);
      Call->setCallingConv(CC);
      llvm::Value *R = cabi::lowerCResult(E, Info, S, P, Call, Dest);
      if (R) {
        if (Sig->ret()->isBool())
          R = B.CreateZExt(R, B.getInt8Ty());
        B.CreateStore(R, Dest.V);
      }
      B.CreateRetVoid();
    }
    std::string Problems;
    llvm::raw_string_ostream PS(Problems);
    if (llvm::verifyModule(*M, &PS))
      Error = "invalid IR: " + Problems;
    llvm::raw_string_ostream OS(IR);
    M->print(OS, nullptr);
  }
};

/// The scalar leaves of a type: offset, size, kind (to fill and compare
/// only what a C struct defines, not its padding).
struct LeafSpan {
  uint64_t Off, Size;
  bool IsBool;
};
void leaves(const gir::Type *T, const llvm::DataLayout &DL, MemTypes &MT,
            uint64_t Base, std::vector<LeafSpan> &Out) {
  switch (T->getKind()) {
  case gir::Type::Kind::Struct: {
    const llvm::StructLayout *SL =
        DL.getStructLayout(llvm::cast<llvm::StructType>(MT.get(T)));
    for (unsigned I = 0; I < T->elems().size(); ++I)
      leaves(T->elems()[I], DL, MT, Base + SL->getElementOffset(I), Out);
    return;
  }
  case gir::Type::Kind::Option: {
    const llvm::StructLayout *SL =
        DL.getStructLayout(llvm::cast<llvm::StructType>(MT.get(T)));
    leaves(T->elem(), DL, MT, Base, Out);
    Out.push_back({Base + SL->getElementOffset(1), 1, false});
    return;
  }
  case gir::Type::Kind::Array: {
    uint64_t Sz = DL.getTypeAllocSize(MT.get(T->elem()));
    for (uint64_t I = 0; I < T->length(); ++I)
      leaves(T->elem(), DL, MT, Base + I * Sz, Out);
    return;
  }
  default:
    Out.push_back({Base, DL.getTypeAllocSize(MT.get(T)), T->isBool()});
  }
}

/// Runs the round trip in a JIT (the host must be the target) and checks that
/// the result holds the bytes of argument RetArg.
::testing::AssertionResult runRoundTrip(RoundTrip &R, unsigned RetArg) {
  if (!R.Error.empty())
    return ::testing::AssertionFailure() << R.Error;
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  const llvm::DataLayout &DL = R.M->getDataLayout();
  MemTypes MT(*R.Ctx);
  std::vector<LeafSpan> Ls, RetLeaves, ArgLeaves;
  const llvm::StructLayout *SL = DL.getStructLayout(R.InTy);
  for (unsigned I = 0; I < R.Sig->elems().size(); ++I)
    leaves(R.Sig->elems()[I], DL, MT, SL->getElementOffset(I), Ls);
  leaves(R.Sig->ret(), DL, MT, 0, RetLeaves);
  leaves(R.Sig->elems()[RetArg], DL, MT, SL->getElementOffset(RetArg),
         ArgLeaves);
  EXPECT_EQ(ArgLeaves.size(), RetLeaves.size());

  alignas(16) unsigned char In[4096], Out[4096];
  if (DL.getTypeAllocSize(R.InTy) > sizeof In)
    return ::testing::AssertionFailure() << "test buffer too small";
  std::memset(Out, 0xEE, sizeof Out);
  for (size_t I = 0; I < sizeof In; ++I)
    In[I] = static_cast<unsigned char>(I * 37 + 11);
  for (const LeafSpan &L : Ls)
    if (L.IsBool)
      In[L.Off] = 1;

  auto Made = llvm::orc::LLJITBuilder().create();
  if (!Made)
    return ::testing::AssertionFailure() << toString(Made.takeError());
  auto J = std::move(*Made);
  if (llvm::Error E = J->addIRModule(
          llvm::orc::ThreadSafeModule(std::move(R.M), std::move(R.Ctx))))
    return ::testing::AssertionFailure() << toString(std::move(E));
  auto Addr = J->lookup("caller");
  if (!Addr)
    return ::testing::AssertionFailure() << toString(Addr.takeError());
  Addr->toPtr<void (*)(void *, void *)>()(In, Out);
  for (size_t I = 0; I < ArgLeaves.size() && I < RetLeaves.size(); ++I)
    if (std::memcmp(In + ArgLeaves[I].Off, Out + RetLeaves[I].Off,
                    ArgLeaves[I].Size) != 0)
      return ::testing::AssertionFailure()
             << "leaf " << I << " (offset " << ArgLeaves[I].Off
             << ") differs after the round trip";
  return ::testing::AssertionSuccess();
}

const char *hostTriple() {
  llvm::Triple H(llvm::sys::getProcessTriple());
  if (H.getArch() == llvm::Triple::x86_64)
    return "x86_64-unknown-linux-gnu";
  if (H.getArch() == llvm::Triple::aarch64)
    return "aarch64-unknown-linux-gnu";
  return nullptr;
}

/// The types of the round trips.
std::vector<gir::Type *> roundTripTypes(Fixture &F) {
  return {F.I32,
          F.I8,
          F.Bool,
          F.F32,
          F.F64,
          F.Ptr,
          F.st({}),
          F.st({F.I32, F.I32}),
          F.st({F.I64, F.I64}),
          F.st({F.I8, F.I8, F.I8}),
          F.st({F.I16, F.I16, F.I16}),
          F.st({F.F32, F.F32}),
          F.st({F.F32, F.F32, F.F32}),
          F.st({F.F64, F.F64, F.F64, F.F64}),
          F.st({F.F64, F.I64}),
          F.st({F.I32, F.F32}),
          F.st({F.I8, F.I64, F.I8}),
          F.st({F.I64, F.I64, F.I64}),
          F.st({F.C.array(F.I64, 17)}),
          F.st({F.C.array(F.F32, 4), F.I8}),
          F.st({F.Bool, F.Bool, F.U8}),
          F.C.option(F.I32),
          F.C.option(F.F64),
          F.st({F.st({F.F32, F.F32}), F.F32}),
          F.st({F.st({}), F.I32})};
}

} // namespace

TEST(CABIRoundTrip, CallerAndCalleeAreInverseOnTheHostTarget) {
  const char *Tgt = hostTriple();
  if (!Tgt)
    GTEST_SKIP() << "no host target to run the round trip on";
  Fixture F;
  unsigned Count = 0;
  for (gir::Type *T : roundTripTypes(F)) {
    // Alone, and after enough filler arguments to use up the integer and the
    // floating-point registers (so the type goes to the stack).
    for (unsigned Filler : {0u, 7u, 13u}) {
      std::vector<gir::Type *> Ps;
      for (unsigned I = 0; I < Filler; ++I)
        Ps.push_back(I % 3 == 2 ? F.F64 : I % 3 == 1 ? F.I64 : F.I32);
      Ps.push_back(T);
      Ps.push_back(F.I8);
      RoundTrip R(Tgt, false, F.C.func(Ps, T), Filler);
      EXPECT_TRUE(runRoundTrip(R, Filler)) << T->str() << " after " << Filler;
      ++Count;
    }
  }
  EXPECT_EQ(Count, 75u);
}

TEST(CABIRoundTrip, CoerceAndExpandThroughSwiftcc) {
  if (!hostTriple() || std::string(hostTriple()).rfind("x86_64", 0) != 0)
    GTEST_SKIP() << "the swiftcc round trip runs on x86-64";
  Fixture F;
  for (gir::Type *T :
       {F.st({F.F64, F.I64, F.I8}), F.st({F.I32, F.F32}), F.st({F.I8, F.I64}),
        F.st({F.F64, F.F64}), F.st({F.I32, F.I32, F.I32}),
        F.st({F.C.array(F.I16, 3)})}) {
    RoundTrip R("x86_64-unknown-linux-gnu", true, F.C.func({F.I32, T}, T), 1);
    ASSERT_TRUE(R.Error.empty()) << R.Error;
    bool Expands = false;
    for (const cabi::ArgInfo &A : R.Info.Args)
      Expands |= A.K == cabi::ArgInfo::Kind::CoerceAndExpand;
    EXPECT_TRUE(Expands) << T->str();
    EXPECT_TRUE(runRoundTrip(R, 1)) << T->str();
  }
}

TEST(CABIRoundTrip, ExpandOnI686IsOneArgumentPerField) {
  // i686 cannot run on the host: the IR is verified (RoundTrip does it) and
  // its shape checked.
  Fixture F;
  gir::Type *S = F.st({F.I32, F.F32, F.I32});
  RoundTrip R("i686-unknown-linux-gnu", false, F.fn({S, F.I32}, nullptr), 1);
  ASSERT_TRUE(R.Error.empty()) << R.Error;
  EXPECT_EQ(R.Info.Args[0].K, cabi::ArgInfo::Kind::Expand);
  // i32, float, i32 and the second argument.
  EXPECT_EQ(R.IRParams, 4u);
  EXPECT_NE(R.IR.find("@callee(i32 %0, float %1, i32 %2, i32 %3)"),
            std::string::npos)
      << R.IR;
}

TEST(CABIRoundTrip, EveryTargetGivesValidIR) {
  // arm64 on an x86 host, and the reverse: the IR is verified, not run.
  Fixture F;
  for (const char *Tgt :
       {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"})
    for (gir::Type *T : roundTripTypes(F)) {
      RoundTrip R(Tgt, false, F.C.func({F.I64, T, F.I8}, T), 1);
      EXPECT_TRUE(R.Error.empty()) << Tgt << " " << T->str() << "\n" << R.Error;
    }
}

TEST(CABIRoundTrip, AnIndirectAlignmentAboveTheTypesRaisesTheCopy) {
  // No Go++ type is aligned above 8, but a C type can be (`_Alignas(32)`,
  // P07): the lowering gives an indirect argument the alignment the ABI asks,
  // a byval of 32 and a copy of 64 here, by copying the value into a slot of
  // that alignment (c2's test, on a classification made by hand).
  using AK = cabi::ArgInfo::Kind;
  Fixture F;
  gir::Type *Big = F.st({F.I64, F.I64, F.I64});
  cabi::CallInfo CI;
  CI.Ret.K = AK::Ignore;
  CI.Ret.Ty = F.C.unit();
  cabi::ArgInfo A;
  A.K = AK::Indirect;
  A.Ty = Big;
  A.ByVal = true;
  A.Align = 32;
  CI.Args.push_back(A);
  A.ByVal = false;
  A.Align = 64;
  CI.Args.push_back(A);
  ASSERT_FALSE(bool(cabi::checkSupported(CI)));
  llvm::LLVMContext Ctx;
  llvm::Module M("align", Ctx);
  auto Spec = parseTarget("linux/amd64");
  ASSERT_TRUE(bool(Spec));
  M.setDataLayout(Spec->DataLayout);
  const llvm::DataLayout &DL = M.getDataLayout();
  MemTypes MT(Ctx);
  auto MemFn = [&](const gir::Type *T) { return MT.get(T); };
  cabi::IRSignature S = cabi::irSignature(Ctx, CI, MemFn);
  auto *Callee = llvm::Function::Create(
      S.Type, llvm::GlobalValue::ExternalLinkage, "callee", M);
  Callee->setAttributes(S.Attrs);
  llvm::Type *Ptr = llvm::PointerType::getUnqual(Ctx);
  auto *Caller = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Ctx), {Ptr, Ptr}, false),
      llvm::GlobalValue::ExternalLinkage, "caller", M);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(Ctx, "entry", Caller));
  cabi::Env E{B, DL, MemFn,
              [&](llvm::Type *Ty, llvm::Align Al, const llvm::Twine &N) {
                auto *Slot = B.CreateAlloca(Ty, nullptr, N);
                Slot->setAlignment(std::max(Al, DL.getABITypeAlign(Ty)));
                return Slot;
              }};
  std::vector<cabi::Value> Vals(2);
  for (unsigned I = 0; I < 2; ++I) {
    Vals[I].Ty = Big;
    Vals[I].V = Caller->getArg(I);
    Vals[I].Known = llvm::Align(8);
  }
  cabi::PreparedCall P = cabi::lowerCArgs(E, CI, S, Vals, cabi::Value());
  B.CreateCall(S.Type, Callee, P.Args)->setAttributes(S.Attrs);
  B.CreateRetVoid();
  std::string Problems;
  llvm::raw_string_ostream PS(Problems);
  ASSERT_FALSE(llvm::verifyModule(M, &PS)) << Problems;
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  M.print(OS, nullptr);
  EXPECT_NE(IR.find("ptr byval({ i64, i64, i64 }) align 32"), std::string::npos)
      << IR;
  EXPECT_NE(IR.find("ptr align 64 dead_on_return"), std::string::npos) << IR;
  EXPECT_NE(IR.find("alloca { i64, i64, i64 }, align 32"), std::string::npos)
      << IR;
  EXPECT_NE(IR.find("alloca { i64, i64, i64 }, align 64"), std::string::npos)
      << IR;
}

//===----------------------------------------------------------------------===//
// gir::CTypeRules: the limits, and answers that do not depend on the order
//===----------------------------------------------------------------------===//

namespace {

std::string verdict(const gir::CVerdict &V) {
  return std::to_string(V.K) + " " + V.Why;
}

/// A struct of N int32 fields, and a chain of N structs around an int32.
gir::Type *wideOf(Fixture &F, unsigned N) {
  return F.st(std::vector<gir::Type *>(N, F.I32));
}
gir::Type *chainOf(Fixture &F, unsigned N) {
  gir::Type *T = F.I32;
  for (unsigned I = 0; I < N; ++I)
    T = F.st({T});
  return T;
}

} // namespace

TEST(CTypeRules, TheLimitsBothSides) {
  Fixture F;
  gir::CTypeRules R;
  auto Ok = [&](gir::Type *T) { return R.check(T, gir::CUse::Param).ok(); };
  auto Why = [&](gir::Type *T) { return R.check(T, gir::CUse::Param).Why; };
  // Depth: 64 levels of structs, not 65.
  EXPECT_TRUE(Ok(chainOf(F, 64)));
  EXPECT_NE(Why(chainOf(F, 65)).find("nested more than 64 levels"),
            std::string::npos);
  // Fields written out: a struct of 4095 fields counts 4096, of 4096 counts
  // 4097. A diamond (each level holds the one below twice) is counted
  // written out: K0 {a, b int32} is 3, K10 is 4095, K11 is 8191.
  EXPECT_TRUE(Ok(wideOf(F, 4095)));
  EXPECT_NE(Why(wideOf(F, 4096)).find("more than 4096 fields"),
            std::string::npos);
  gir::Type *K = F.st({F.I32, F.I32});
  for (unsigned I = 1; I <= 10; ++I)
    K = F.st({K, K});
  EXPECT_TRUE(Ok(K));
  EXPECT_NE(Why(F.st({K, K})).find("more than 4096 fields"), std::string::npos);
  // An array counts its element once, whatever its length.
  EXPECT_TRUE(Ok(F.st({F.C.array(wideOf(F, 4000), 1000)})));
  // Size: 1 GiB is a C parameter, a byte more is not (as a field it is: the
  // parameter that holds it is refused).
  EXPECT_TRUE(Ok(F.st({F.C.array(F.I64, uint64_t(1) << 27)})));
  gir::Type *Huge = F.st({F.C.array(F.U8, (uint64_t(1) << 30) + 1)});
  EXPECT_NE(Why(Huge).find("larger than 1 GiB"), std::string::npos);
  EXPECT_TRUE(R.check(Huge, gir::CUse::Field).ok());
  // An array of zero-size elements is not a C type, whatever its length; a
  // zero-length array of a sized type is.
  EXPECT_FALSE(Ok(F.st({F.C.array(F.st({}), 2)})));
  EXPECT_FALSE(Ok(F.st({F.C.array(F.st({}), 0)})));
  EXPECT_NE(
      Why(F.st({F.C.array(F.C.array(F.I32, 0), 3)})).find("zero-size elements"),
      std::string::npos);
  EXPECT_TRUE(Ok(F.st({F.C.array(F.I32, 0), F.st({})})));
  // Option[T] by value is not a C type until P07 (decision M6), alone, in a
  // struct and as a result; Option[*T] is a pointer in GIR and is one.
  for (gir::Type *O : {F.C.option(F.I32), F.C.option(F.st({})),
                       F.C.option(F.st({F.I64, F.F64}))}) {
    for (gir::CUse U : {gir::CUse::Param, gir::CUse::Result}) {
      gir::CVerdict V = R.check(O, U);
      EXPECT_EQ(V.K, gir::CVerdict::Invalid) << O->str();
      EXPECT_NE(V.Why.find("Option[T] of a value has no C type until P07"),
                std::string::npos)
          << V.Why;
    }
    EXPECT_FALSE(Ok(F.st({F.I32, O})));
  }
  EXPECT_TRUE(Ok(F.C.ptr(F.I32)));
  EXPECT_TRUE(Ok(F.st({F.C.ptr(F.I32)})));
  // Complex numbers (Tuple{f, f} in GIR) say so, as a parameter and as a
  // field; a pair of results keeps the text about results; at the end of a
  // result the two cannot be told apart in GIR, and the text names both.
  gir::Type *Cx = F.C.tuple({F.F64, F.F64});
  EXPECT_NE(Why(Cx).find("complex numbers have no C mapping yet (P07)"),
            std::string::npos);
  EXPECT_EQ(Why(Cx).find("one result"), std::string::npos);
  EXPECT_NE(Why(F.st({Cx})).find("complex numbers"), std::string::npos);
  EXPECT_NE(R.check(Cx, gir::CUse::Result).Why.find("one result"),
            std::string::npos);
  EXPECT_NE(Why(F.C.tuple({F.I32, F.I32})).find("one result"),
            std::string::npos);
  // The zero-size text speaks of the user's type.
  EXPECT_NE(Why(F.st({F.C.array(F.st({}), 2)}))
                .find("an array of zero-size elements, [2]"),
            std::string::npos);
  EXPECT_EQ(Why(F.st({F.C.array(F.st({}), 2)})).find("Clang"),
            std::string::npos);
}

TEST(CTypeRules, TheOrderOfQuestionsChangesNoAnswer) {
  // The memo keeps only facts of a type itself (never the depth it was
  // reached at), so asking about an inner type first cannot change an
  // answer about an outer one: a chain of 70 is refused after its inner 30
  // were accepted (a memo of verdicts by depth accepts it). Several problems
  // in one type give one answer in any order (the depth first).
  Fixture F;
  std::vector<gir::Type *> L = {F.I32};
  for (unsigned I = 0; I < 100; ++I)
    L.push_back(F.st({L.back()}));
  gir::Type *Str = F.C.string();
  std::vector<gir::Type *> Asked = {
      L[30],
      L[64],
      L[65],
      L[70],
      L[100],
      F.st({Str, L[80]}),
      F.st({L[80], Str}),
      F.st({L[63]}),
      F.st({L[64]}),
      F.st({Str, wideOf(F, 5000)}),
      F.st({wideOf(F, 5000), F.C.array(F.st({}), 2)}),
      F.st({F.C.func({}, F.C.unit()), Str})};
  std::vector<std::string> Want;
  for (gir::Type *T : Asked) {
    gir::CTypeRules Fresh; // each alone
    Want.push_back(verdict(Fresh.check(T, gir::CUse::Param)));
  }
  EXPECT_EQ(Want[0].substr(0, 2), "0 ");
  EXPECT_NE(Want[3].find("nested"), std::string::npos);
  EXPECT_NE(Want[5].find("nested"), std::string::npos);
  EXPECT_EQ(Want[5], Want[6]);
  std::mt19937 R(5);
  for (unsigned Round = 0; Round < 40; ++Round) {
    gir::CTypeRules Shared;
    // Ask some inner types first, then all of them in a shuffled order.
    for (unsigned I = 0; I < 10; ++I)
      Shared.check(L[R() % L.size()], gir::CUse::Field);
    std::vector<unsigned> Order(Asked.size());
    for (unsigned I = 0; I < Order.size(); ++I)
      Order[I] = I;
    std::shuffle(Order.begin(), Order.end(), R);
    for (unsigned I : Order)
      EXPECT_EQ(verdict(Shared.check(Asked[I], gir::CUse::Param)), Want[I])
          << "round " << Round << ", type " << I;
  }
}

//===----------------------------------------------------------------------===//
// Near-linear work in every dimension of a signature (M8)
//===----------------------------------------------------------------------===//

namespace {

/// Steps of CTypeRules deciding one type built by Make for size N.
uint64_t ruleSteps(unsigned N, gir::Type *(*Make)(Fixture &, unsigned)) {
  Fixture F;
  gir::Type *T = Make(F, N);
  gir::CTypeRules Rules;
  uint64_t Steps = 0;
  Rules.Work = &Steps;
  // This counts the walk, whatever the verdict: at 4N a wide struct and the
  // diamond ladders are over the field limit, which has its own tests
  // (CTypeRules.TheLimitsBothSides, tests/gir/extern-deep.test).
  gir::CVerdict V = Rules.check(T, gir::CUse::Field);
  // Asked again, the answer is the same and free.
  uint64_t Before = Steps;
  gir::CVerdict Again = Rules.check(T, gir::CUse::Field);
  EXPECT_EQ(Again.K, V.K);
  EXPECT_EQ(Again.Why, V.Why);
  EXPECT_LE(Steps - Before, 1u);
  return Steps;
}

::testing::AssertionResult linear(uint64_t S1, uint64_t S4, uint64_t N) {
  if (S1 == 0 || S1 > 8 * N || S4 > 8 * 4 * N || 2 * S4 > 9 * S1)
    return ::testing::AssertionFailure()
           << "steps(N) = " << S1 << ", steps(4N) = " << S4 << ", N = " << N;
  return ::testing::AssertionSuccess();
}

/// A struct with N fields.
gir::Type *wide(Fixture &F, unsigned N) {
  return F.st(std::vector<gir::Type *>(N, F.I32));
}
/// A struct of N different structs.
gir::Type *manyStructs(Fixture &F, unsigned N) {
  std::vector<gir::Type *> Fs;
  for (unsigned I = 0; I < N; ++I)
    Fs.push_back(F.st({F.I8, F.F64}));
  return F.st(Fs);
}
/// A struct of N/40 chains of depth 40 (distinct structs).
gir::Type *chains(Fixture &F, unsigned N) {
  std::vector<gir::Type *> Fs;
  for (unsigned I = 0; I < N / 40; ++I) {
    gir::Type *T = F.I32;
    for (unsigned D = 0; D < 40; ++D)
      T = F.st({T});
    Fs.push_back(T);
  }
  return F.st(Fs);
}
/// Diamond ladders: each level holds the level below twice, so the value tree
/// has 2^60 leaves while the type DAG has 60 nodes.
gir::Type *diamonds(Fixture &F, unsigned N) {
  std::vector<gir::Type *> Top;
  for (unsigned I = 0; I < N / 60; ++I) {
    gir::Type *T = F.I32;
    for (unsigned D = 0; D < 60; ++D)
      T = F.st({T, T});
    Top.push_back(T);
  }
  return F.st(Top);
}
/// Arrays of arrays of a struct of N/8 fields.
gir::Type *arrays(Fixture &F, unsigned N) {
  gir::Type *T = F.st(std::vector<gir::Type *>(N / 8, F.I8));
  for (unsigned I = 0; I < 8; ++I)
    T = F.C.array(T, 2);
  return F.st({T});
}

} // namespace

TEST(CABIStress, TheTypeRulesWalkEachTypeOnce) {
  struct Dim {
    const char *Name;
    gir::Type *(*Make)(Fixture &, unsigned);
  } Dims[] = {{"wide", wide},
              {"manyStructs", manyStructs},
              {"chains", chains},
              {"diamonds", diamonds},
              {"arrays", arrays}};
  for (const Dim &D : Dims) {
    uint64_t S1 = ruleSteps(2400, D.Make), S4 = ruleSteps(9600, D.Make);
    EXPECT_TRUE(linear(S1, S4, 2400)) << D.Name;
  }
}

TEST(CABIStress, TheControlFailsAQuadraticWalk) {
  // The check above must be able to fail: a walk that visits every pair.
  auto Steps = [](unsigned N, bool Quadratic) {
    uint64_t S = 0;
    for (unsigned I = 0; I < N; ++I)
      for (unsigned J = 0; J < (Quadratic ? N : 1); ++J)
        ++S;
    return S;
  };
  EXPECT_TRUE(linear(Steps(2400, false), Steps(9600, false), 2400));
  EXPECT_FALSE(linear(Steps(2400, true), Steps(9600, true), 2400));
}

TEST(CABIStress, ManyParametersAndManySignatures) {
  // A signature of 4000 parameters, 4000 parameters of one shared struct, and
  // 4000 signatures over 4000 different structs: every type is mapped once
  // (TypesMapped counts them) and every signature classified once.
  Fixture F;
  auto Cl = classifier("linux/amd64");
  std::vector<gir::Type *> Ps;
  for (unsigned I = 0; I < 4000; ++I)
    Ps.push_back(I % 2 ? F.F64 : F.I32);
  const cabi::CallInfo &Wide = Cl->classify(F.C.func(Ps, F.I32));
  EXPECT_EQ(Wide.Args.size(), 4000u);
  EXPECT_LE(Cl->stats().TypesMapped, 4u);

  gir::Type *Shared = F.st({F.I64, F.F64});
  std::vector<gir::Type *> Same(4000, Shared);
  Cl->classify(F.C.func(Same, Shared));
  uint64_t Mapped = Cl->stats().TypesMapped;
  EXPECT_LE(Mapped, 8u);

  for (unsigned I = 0; I < 4000; ++I) {
    gir::Type *S = F.st({F.I32, F.I64});
    Cl->classify(F.C.func({S, F.I8, S}, S));
  }
  // One new type per signature (the struct), and int8 once.
  EXPECT_EQ(Cl->stats().TypesMapped - Mapped, 4001u);
  EXPECT_EQ(Cl->stats().Classified, 4002u);
}

namespace {

/// The C ABI work of a program from source to LLVM IR for amd64 (c2's
/// end-to-end stress): the verifier's steps (its V067 walks the C types with
/// gir::CTypeRules, as the front end does) and the code generator's
/// (LowerToLLVMOptions::CABIWork: aggregates checked against Clang's layout,
/// signatures classified, GIR types mapped to Clang, IR arguments made).
struct FromSource {
  uint64_t Verify = 0, CABI = 0;
  std::string Errors;
  bool Lowered = false;
};

FromSource lowerProgram(const std::string &Source) {
  FromSource R;
  llvm::SmallString<128> Dir;
  EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-cabi", Dir));
  {
    std::ofstream Out(std::string(Dir) + "/main.gopp", std::ios::binary);
    Out << Source;
  }
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  sema::LoadOptions LO;
  LO.Root = std::string(Dir);
  sema::TypeContext TCtx;
  sema::Loader L(SM, Diags, TCtx, LO, {});
  sema::CheckedPackage *CP = L.loadDir(std::string(Dir), "");
  gir::Context GC;
  std::unique_ptr<gir::Module> M;
  if (CP && !Diags.hasErrors())
    M = gir::lowerPackage(GC, SM, *CP, L.filesOf(CP), Diags);
  for (const Diagnostic &D : Collect.Diags)
    R.Errors += D.Message + "\n";
  if (M) {
    auto T = parseTarget("linux/amd64");
    auto TM = createTargetMachine(*T);
    llvm::LLVMContext LC;
    LowerToLLVMOptions O;
    O.CABIWork = &R.CABI;
    uint64_t V0 = gir::verifierSteps();
    auto LM = lowerToLLVM(LC, *M, **TM, O);
    R.Verify = gir::verifierSteps() - V0;
    if (LM)
      R.Lowered = true;
    else
      R.Errors += llvm::toString(LM.takeError());
  }
  llvm::sys::fs::remove_directories(Dir);
  return R;
}

/// Work(4N) at most 5 times work(N) (linear is 4, quadratic 16), and work(N)
/// at least N (the counter counts what grows).
::testing::AssertionResult linearFromSource(uint64_t Small, uint64_t Big,
                                            unsigned N) {
  if (Small < N || Big > 5 * Small)
    return ::testing::AssertionFailure()
           << "work(N) = " << Small << ", work(4N) = " << Big << ", N = " << N;
  return ::testing::AssertionSuccess();
}

std::string program(const std::string &Decls, const std::string &Use) {
  return "package main\n" + Decls + "\nfunc main() {\n" + Use + "}\n";
}

std::string wideProgram(unsigned N) {
  std::string D = "type W struct {\n";
  for (unsigned I = 0; I < N; ++I)
    D += "\tf" + std::to_string(I) + " int32\n";
  D += "}\n@Extern(\"C\")\nfunc wide(w W) W\n";
  return program(D, "\tvar w W\n\t_ = wide(w)\n");
}

std::string paramsProgram(unsigned N) {
  std::string D =
      "type P struct{ a int32; b float32 }\n@Extern(\"C\")\nfunc many(";
  std::string Use = "\tvar p P\n\tmany(";
  for (unsigned I = 0; I < N; ++I) {
    D += std::string(I ? ", " : "") + "p" + std::to_string(I) + " P";
    Use += std::string(I ? ", " : "") + "p";
  }
  return program(D + ")\n", Use + ")\n");
}

std::string declsProgram(unsigned N) {
  std::string D =
      "type P struct{ a, b int32 }\ntype V struct{ x, y float64 }\n";
  std::string Use = "\tvar p P\n\tvar v V\n";
  for (unsigned I = 0; I < N; ++I) {
    std::string F = "c" + std::to_string(I);
    D += "@Extern(\"C\")\nfunc " + F + "(p P, v V, n int8) P\n";
    Use += "\t_ = " + F + "(p, v, 1)\n";
  }
  return program(D, Use);
}

std::string deepProgram(unsigned N) {
  std::string D = "type D0 struct{ x int32 }\n";
  for (unsigned I = 1; I < N; ++I)
    D += "type D" + std::to_string(I) + " struct{ in D" +
         std::to_string(I - 1) + " }\n";
  std::string Last = "D" + std::to_string(N - 1);
  D += "@Extern(\"C\")\nfunc deep(d " + Last + ") " + Last + "\n";
  return program(D, "\tvar d " + Last + "\n\t_ = deep(d)\n");
}

/// A DAG of N levels (each holds the one below twice) and N sibling
/// declarations that take it: written out 2^N fields, one type per level.
std::string dagProgram(unsigned N) {
  std::string D = "type G0 struct{ x int8 }\n";
  for (unsigned I = 1; I < N; ++I)
    D += "type G" + std::to_string(I) + " struct{ a, b G" +
         std::to_string(I - 1) + " }\n";
  std::string Use;
  for (unsigned I = 0; I < N; ++I) {
    std::string G = "G" + std::to_string(I);
    D += "@Extern(\"C\")\nfunc dag" + std::to_string(I) + "(g " + G + ")\n";
    Use += "\tvar g" + std::to_string(I) + " " + G + "\n\tdag" +
           std::to_string(I) + "(g" + std::to_string(I) + ")\n";
  }
  return program(D, Use);
}

} // namespace

TEST(CABIStress, FromSourceEveryDimensionIsLinear) {
  // The whole C ABI path (V067, the layout check, Clang's classification,
  // the lowering of the calls) in each dimension of a program: fields of one
  // struct, parameters of one function, declarations, nesting (up to the 64
  // levels a C type may have) and a shared DAG (up to the field limit).
  struct Dim {
    const char *Name;
    std::string (*Make)(unsigned);
    unsigned N;
  } Dims[] = {{"fields", wideProgram, 250},
              {"parameters", paramsProgram, 100},
              {"declarations", declsProgram, 60},
              {"nesting", deepProgram, 15},
              {"dag", dagProgram, 2}};
  for (const Dim &D : Dims) {
    FromSource A = lowerProgram(D.Make(D.N)), B = lowerProgram(D.Make(4 * D.N));
    ASSERT_TRUE(A.Lowered && B.Lowered) << D.Name << ":\n"
                                        << A.Errors << B.Errors;
    EXPECT_TRUE(linearFromSource(A.Verify, B.Verify, D.N)) << D.Name;
    EXPECT_TRUE(linearFromSource(A.CABI, B.CABI, D.N)) << D.Name;
  }
}

TEST(CABIStress, TheFromSourceControlFailsAQuadraticCount) {
  // linearFromSource must be able to fail: a count that grows with N*N.
  EXPECT_TRUE(linearFromSource(100, 400, 100));
  EXPECT_FALSE(linearFromSource(100 * 100, 400 * 400, 100));
  EXPECT_FALSE(linearFromSource(10, 40, 100)); // counts less than the input
}

namespace {
/// Classifying N signatures, each over a struct of a shape no earlier call
/// made: the Clang world keeps one record per shape for the life of the
/// process (M3), so a repeated shape would time a cache.
double timeMapping(unsigned N) {
  static uint64_t NextShape = 0;
  Fixture F;
  auto Cl = classifier("linux/amd64");
  std::vector<gir::Type *> Sigs;
  for (unsigned I = 0; I < N; ++I) {
    gir::Type *S =
        F.st({F.I32, F.F64, F.C.array(F.I8, ++NextShape), F.I64, F.I8});
    Sigs.push_back(F.C.func({S, F.I8, S, F.I32}, S));
  }
  auto T0 = std::chrono::steady_clock::now();
  for (gir::Type *S : Sigs)
    Cl->classify(S);
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - T0)
      .count();
}
} // namespace

TEST(CABIStressLong,
     DISABLED_ClassifyingFourTimesTheSignaturesCostsAtMostTwelveTimes) {
  double Best1 = 1e9, Best4 = 1e9;
  for (unsigned Round = 0; Round < 5; ++Round) {
    Best1 = std::min(Best1, timeMapping(4000));
    Best4 = std::min(Best4, timeMapping(16000));
  }
  EXPECT_LT(Best4, 12 * Best1) << Best1 << " s, " << Best4 << " s";
}

TEST(CABIStressLong, DISABLED_TheTimingControlFailsAQuadraticLoop) {
  auto Quad = [](unsigned N) {
    volatile unsigned long Sink = 0;
    auto T0 = std::chrono::steady_clock::now();
    for (unsigned I = 0; I < N; ++I)
      for (unsigned J = 0; J < N; ++J)
        Sink = Sink + (I ^ J);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - T0)
        .count();
  };
  double Best1 = 1e9, Best4 = 1e9;
  for (unsigned Round = 0; Round < 5; ++Round) {
    Best1 = std::min(Best1, Quad(4000));
    Best4 = std::min(Best4, Quad(16000));
  }
  EXPECT_GE(Best4, 12 * Best1) << Best1 << " s, " << Best4 << " s";
}
