//===- GoABITest.cpp - The Go++ internal calling convention ---------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.18 (docs/design/calling-convention.md). GoAbi is the one place that
// decides how Go++ calls Go++:
//   * Kinds:  every kind on both sides of its boundary, on both targets;
//   * Registers: the budget (Policy) is what LLVM's swiftcc really returns in
//     registers: a function returning that many scalars is compiled for each
//     target and its assembly has no store through a hidden result pointer,
//     one more scalar and it has (the positive control);
//   * Decision: one cached answer per signature, whose IR arguments add up;
//     the end-to-end round trip (caller and callee, C and Go++) is the run
//     tests cabi-reverse-*.test and cc-params.test;
//   * Work: the flatten walk counts its steps (Work) and stays linear on a
//     deep chain, a diamond (2^N paths) and a wide struct, with a control.
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/GoABI.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/GIR/Type.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <gtest/gtest.h>

using namespace gopp;
using goabi::Kind;

namespace {

/// A GoAbi for one target, over a small GIR-to-LLVM type map (scalars and
/// structs: all the tests need).
struct Abi {
  gir::Context G;
  llvm::LLVMContext LC;
  std::unique_ptr<llvm::TargetMachine> TM;
  std::unique_ptr<llvm::DataLayout> DL;
  std::unique_ptr<goabi::GoAbi> A;
  uint64_t Work = 0;
  unsigned N = 0;

  explicit Abi(const char *Target) {
    initializeTargets();
    auto T = parseTarget(Target);
    EXPECT_TRUE(bool(T));
    auto M = createTargetMachine(*T);
    EXPECT_TRUE(bool(M));
    TM = std::move(*M);
    DL = std::make_unique<llvm::DataLayout>(TM->createDataLayout());
    A = std::make_unique<goabi::GoAbi>(
        LC, *DL, goabi::Policy::forTriple(TM->getTargetTriple()),
        [this](const gir::Type *Ty) { return mem(Ty); });
    A->Work = &Work;
  }

  llvm::Type *mem(const gir::Type *T) {
    if (T->isBool())
      return llvm::Type::getInt8Ty(LC);
    if (T->isInt())
      return llvm::Type::getIntNTy(LC, T->bits());
    if (T->isFloat())
      return T->bits() == 32 ? llvm::Type::getFloatTy(LC)
                             : llvm::Type::getDoubleTy(LC);
    if (T->isPtr())
      return llvm::PointerType::getUnqual(LC);
    if (T->getKind() == gir::Type::Kind::Array)
      return llvm::ArrayType::get(mem(T->elem()), T->length());
    llvm::SmallVector<llvm::Type *, 8> Es;
    for (unsigned I = 0; I < T->numFields(); ++I)
      Es.push_back(mem(T->fieldType(I, G)));
    return llvm::StructType::get(LC, Es);
  }

  gir::Type *st(std::vector<gir::Type *> Fs) {
    gir::Type *S = G.structTy("S" + std::to_string(N++));
    G.setStructBody(S, Fs);
    return S;
  }
  gir::Type *rep(gir::Type *T, unsigned K) {
    return st(std::vector<gir::Type *>(K, T));
  }
  const goabi::FnInfo &sig(std::vector<gir::Type *> Ps, gir::Type *R) {
    return A->classify(G.func(Ps, R ? R : G.unit()));
  }
};

const char *Targets[] = {"linux/amd64", "linux/arm64"};

} // namespace

TEST(GoAbiKinds, EveryKindOnBothSidesOfItsBoundary) {
  for (const char *Tg : Targets) {
    Abi X(Tg);
    gir::Type *I64 = X.G.i64(), *F64 = X.G.floatTy(64), *I8 = X.G.i8();
    // parameters: 4 scalars are Direct, 5 are Indirect
    {
      const auto &F = X.sig(
          {X.rep(I64, 4), X.rep(I64, 5), I64, X.G.unit(), X.st({})}, nullptr);
      EXPECT_EQ(F.Params[0].K, Kind::Direct) << Tg;
      EXPECT_EQ(F.Params[0].Count, 4u);
      EXPECT_EQ(F.Params[1].K, Kind::Indirect) << Tg;
      EXPECT_EQ(F.Params[1].Count, 1u);
      EXPECT_EQ(F.Params[2].K, Kind::Scalar);
      EXPECT_EQ(F.Params[3].K, Kind::Ignore);
      EXPECT_EQ(F.Params[4].K, Kind::Ignore) << "an empty struct";
      EXPECT_EQ(F.Ret.K, Kind::Ignore);
      // The Go++ convention: swiftcc on both targets (ADR-0030).
      EXPECT_EQ(F.CC, llvm::CallingConv::Swift) << Tg;
    }
    // an array of one element flattens, of two does not
    {
      gir::Type *A1 = X.st({X.G.array(I64, 1)}),
                *A2 = X.st({X.G.array(I64, 2)});
      const auto &F = X.sig({A1, A2}, nullptr);
      EXPECT_EQ(F.Params[0].K, Kind::Direct) << Tg;
      EXPECT_EQ(F.Params[1].K, Kind::Indirect) << Tg;
    }
    // the budget of a result: ints and floats count apart (swiftcc: 4 and 4
    // on amd64, 8 and 8 on arm64)
    bool Amd64 = std::string(Tg) == "linux/amd64";
    unsigned MaxI = Amd64 ? 4 : 8, MaxF = Amd64 ? 4 : 8;
    for (unsigned K = 1; K <= 18; ++K) {
      EXPECT_EQ(X.sig({}, X.rep(I64, K)).Ret.K,
                K == 1 ? Kind::Direct
                       : (K <= MaxI ? Kind::Direct : Kind::Indirect))
          << Tg << " " << K << " integers";
      EXPECT_EQ(X.sig({}, X.rep(F64, K)).Ret.K,
                K <= MaxF ? Kind::Direct : Kind::Indirect)
          << Tg << " " << K << " floats";
    }
    // four of each fit on both targets; the classes count apart, so the
    // whole budget of both fits at once, and one more integer does not
    gir::Type *Mix = X.st({I64, F64, I64, F64, I64, F64, I64, F64});
    EXPECT_EQ(X.sig({}, Mix).Ret.K, Kind::Direct) << Tg;
    EXPECT_EQ(X.sig({}, Mix).Type->getReturnType()->getStructNumElements(), 8u);
    std::vector<gir::Type *> Full(MaxI, I64);
    Full.insert(Full.end(), MaxF, F64);
    EXPECT_EQ(X.sig({}, X.st(Full)).Ret.K, Kind::Direct) << Tg;
    Full.push_back(I64);
    EXPECT_EQ(X.sig({}, X.st(Full)).Ret.K, Kind::Indirect) << Tg;
    // the Indirect result is a hidden first pointer
    const auto &Big = X.sig({I8}, X.rep(I64, 20));
    EXPECT_EQ(Big.Ret.K, Kind::Indirect);
    EXPECT_EQ(Big.SRet, 0);
    EXPECT_TRUE(Big.Type->getReturnType()->isVoidTy());
    EXPECT_EQ(Big.Params[0].First, 1u);
  }
}

TEST(GoAbiKinds, TheEnvironmentIsSwiftself) {
  // A closure's environment (P04.07): the swiftself register on both
  // targets, a parameter of its own before the others. A plain function has
  // none.
  for (const char *Tg : Targets) {
    Abi X(Tg);
    gir::Type *Sig = X.G.func({X.G.i64()}, X.G.i64());
    const auto &Plain = X.A->classify(Sig);
    const auto &Env = X.A->classify(Sig, true);
    EXPECT_EQ(Plain.Env, -1) << Tg;
    EXPECT_EQ(Plain.Type->getNumParams(), 1u) << Tg;
    EXPECT_EQ(Env.Env, 0) << Tg;
    EXPECT_EQ(Env.Params[0].First, 1u) << Tg;
    EXPECT_TRUE(Env.Attrs.hasParamAttr(0, llvm::Attribute::SwiftSelf)) << Tg;
    EXPECT_FALSE(Plain.Attrs.hasParamAttr(0, llvm::Attribute::SwiftSelf)) << Tg;
    EXPECT_NE(&Plain, &Env);
    EXPECT_EQ(&X.A->classify(Sig, true), &Env) << "cached";
  }
}

TEST(GoAbiKinds, NarrowIntegersAreExtended) {
  Abi X("linux/arm64");
  const auto &F =
      X.sig({X.G.i8(), X.G.u8(), X.G.boolean(), X.G.i64()}, X.G.i8());
  EXPECT_TRUE(F.Attrs.hasParamAttr(0, llvm::Attribute::SExt));
  EXPECT_TRUE(F.Attrs.hasParamAttr(1, llvm::Attribute::ZExt));
  EXPECT_TRUE(F.Attrs.hasParamAttr(2, llvm::Attribute::ZExt));
  EXPECT_FALSE(F.Attrs.hasParamAttr(3, llvm::Attribute::SExt));
  EXPECT_TRUE(F.Attrs.hasRetAttr(llvm::Attribute::SExt));
}

//===----------------------------------------------------------------------===//
// The budget is what LLVM does
//===----------------------------------------------------------------------===//

namespace {

/// The assembly of `{T x N} f()` returning a constant, with the target's
/// Go++ convention (Policy::CC).
std::string asmOfReturn(Abi &X, llvm::Type *Elem, unsigned K) {
  llvm::Module M("ret", X.LC);
  M.setDataLayout(X.TM->createDataLayout());
  M.setTargetTriple(X.TM->getTargetTriple());
  std::vector<llvm::Type *> Es(K, Elem);
  llvm::Type *R = K == 1 ? Elem : llvm::StructType::get(X.LC, Es);
  auto *F = llvm::Function::Create(llvm::FunctionType::get(R, false),
                                   llvm::GlobalValue::ExternalLinkage, "f", M);
  F->setCallingConv(goabi::Policy::forTriple(X.TM->getTargetTriple()).CC);
  llvm::IRBuilder<> B(llvm::BasicBlock::Create(X.LC, "", F));
  llvm::Value *V = llvm::PoisonValue::get(R);
  for (unsigned I = 0; I < K; ++I) {
    llvm::Constant *C = Elem->isDoubleTy()
                            ? llvm::ConstantFP::get(Elem, 1.0 + I)
                            : llvm::ConstantInt::get(Elem, 100 + I);
    V = K == 1 ? C : B.CreateInsertValue(V, C, I);
  }
  B.CreateRet(V);
  EXPECT_FALSE(llvm::verifyModule(M, &llvm::errs()));
  llvm::SmallString<1024> Buf;
  llvm::raw_svector_ostream OS(Buf);
  llvm::legacy::PassManager PM;
  EXPECT_FALSE(X.TM->addPassesToEmitFile(PM, OS, nullptr,
                                         llvm::CodeGenFileType::AssemblyFile));
  PM.run(M);
  return std::string(Buf);
}

/// Did the function store through a hidden result pointer? A constant result
/// in registers is moves into registers only (and loads of constants from
/// `(%rip)`); one demoted to memory stores through the pointer, which the
/// code may copy to another register first, so any access through a
/// register other than rip and rsp counts on x86-64, and any store on arm64.
bool demoted(const std::string &Asm, bool Amd64) {
  if (!Amd64)
    return Asm.find("\tstr\t") != std::string::npos ||
           Asm.find("\tstp\t") != std::string::npos;
  for (size_t At = Asm.find("(%r"); At != std::string::npos;
       At = Asm.find("(%r", At + 1))
    if (Asm.compare(At, 6, "(%rip)") != 0 && Asm.compare(At, 6, "(%rsp)") != 0)
      return true;
  return false;
}

} // namespace

TEST(GoAbiRegisters, ReturnRegistersAreWhatLLVMHas) {
  for (const char *Tg : Targets) {
    Abi X(Tg);
    bool Amd64 = std::string(Tg) == "linux/amd64";
    goabi::Policy P = goabi::Policy::forTriple(X.TM->getTargetTriple());
    // swiftcc on both (ADR-0030): amd64 rax, rdx, rcx, r8 and xmm0-3;
    // arm64 x0-x7 and v0-v7.
    EXPECT_EQ(P.CC, llvm::CallingConv::Swift) << Tg;
    EXPECT_EQ(P.RetInts, Amd64 ? 4u : 8u);
    EXPECT_EQ(P.RetFloats, Amd64 ? 4u : 8u);
    // Integers: every count up to the budget stays in registers, one more is
    // demoted to memory (LLVM's own fallback; the control that the check
    // sees it).
    llvm::Type *I64 = llvm::Type::getInt64Ty(X.LC);
    for (unsigned K = 1; K <= P.RetInts; ++K)
      EXPECT_FALSE(demoted(asmOfReturn(X, I64, K), Amd64))
          << Tg << " " << K << " integers";
    EXPECT_TRUE(demoted(asmOfReturn(X, I64, P.RetInts + 1), Amd64))
        << Tg << " one integer over the budget";
    // Floats: up to the budget, in the vector registers.
    llvm::Type *F64 = llvm::Type::getDoubleTy(X.LC);
    for (unsigned K = 1; K <= P.RetFloats; ++K) {
      std::string S = asmOfReturn(X, F64, K);
      EXPECT_FALSE(demoted(S, Amd64)) << Tg << " " << K << " floats";
      EXPECT_EQ(S.find("fld"), std::string::npos) << "no x87: " << Tg << K;
    }
    // One more double is not in a vector register: x87 on amd64, memory on
    // arm64.
    std::string Over = asmOfReturn(X, F64, P.RetFloats + 1);
    if (Amd64)
      EXPECT_NE(Over.find("fld"), std::string::npos);
    else
      EXPECT_TRUE(demoted(Over, Amd64));
  }
}

//===----------------------------------------------------------------------===//
// A caller and a callee made from one decision agree
//===----------------------------------------------------------------------===//

TEST(GoAbiDecision, OneDecisionPerSignatureAndItsIRAddsUp) {
  for (const char *Tg : Targets) {
    Abi X(Tg);
    gir::Type *I64 = X.G.i64(), *F32 = X.G.floatTy(32), *I8 = X.G.i8();
    std::vector<gir::Type *> Rets = {
        I64,           X.rep(I64, 2),  X.st({I64, F32, I8}),
        X.rep(I64, 5), X.rep(I64, 30), nullptr};
    for (gir::Type *R : Rets) {
      gir::Type *Sig =
          X.G.func({I8, X.rep(I64, 3), X.rep(I64, 9), F32}, R ? R : X.G.unit());
      const auto &FI = X.A->classify(Sig);
      llvm::Module M("rt", X.LC);
      M.setDataLayout(X.TM->createDataLayout());
      M.setTargetTriple(X.TM->getTargetTriple());
      auto *Callee = llvm::Function::Create(
          FI.Type, llvm::GlobalValue::ExternalLinkage, "callee", M);
      Callee->setCallingConv(FI.CC);
      Callee->setAttributes(FI.Attrs);
      // every IR argument of the callee has a type the decision named
      unsigned Seen = (FI.SRet >= 0) + (FI.Env >= 0);
      for (const auto &P : FI.Params)
        Seen += P.Count;
      EXPECT_EQ(Seen, FI.Type->getNumParams()) << Tg;
      EXPECT_EQ(&X.A->classify(Sig), &FI) << "one decision per type";
      EXPECT_EQ(FI.Ret.K == Kind::Indirect,
                FI.Type->getReturnType()->isVoidTy() && R && !R->isUnit())
          << Tg << " " << (R ? R->str() : "unit");
    }
  }
}

//===----------------------------------------------------------------------===//
// Near-linear work
//===----------------------------------------------------------------------===//

namespace {

/// The steps flatten takes on a type of the given shape of size N.
enum class Shape { Chain, Diamond, Wide };

uint64_t flattenWork(Shape S, unsigned N) {
  llvm::LLVMContext LC;
  llvm::DataLayout DL("e-m:e-i64:64-n32:64-S128");
  uint64_t W = 0;
  goabi::GoAbi A(LC, DL, goabi::Policy::forTriple(llvm::Triple("x86_64-linux")),
                 [](const gir::Type *) -> llvm::Type * { return nullptr; });
  A.Work = &W;
  llvm::Type *T = llvm::Type::getInt64Ty(LC);
  if (S == Shape::Wide) {
    std::vector<llvm::Type *> Es;
    for (unsigned I = 0; I < N; ++I)
      Es.push_back(llvm::StructType::get(
          LC, llvm::ArrayRef<llvm::Type *>(llvm::Type::getInt64Ty(LC))));
    T = llvm::StructType::get(LC, Es);
  } else {
    for (unsigned I = 0; I < N; ++I)
      T = S == Shape::Chain
              ? llvm::StructType::get(LC, llvm::ArrayRef<llvm::Type *>(T))
              : llvm::StructType::get(LC, llvm::ArrayRef<llvm::Type *>({T, T}));
  }
  llvm::SmallVector<goabi::Part, 4> Parts;
  unsigned Ints = 0, Floats = 0;
  A.flatten(T, 4, Parts, Ints, Floats);
  return W;
}

} // namespace

TEST(GoAbiWork, FlattenIsLinearInEveryDimension) {
  // Each dimension grows 16 times: the count must grow at least 10 times (the
  // counter sees the dimension) and at most 20 times (linear). The diamond has
  // 2^N paths and one visit per type.
  for (Shape S : {Shape::Chain, Shape::Diamond, Shape::Wide}) {
    uint64_t A = flattenWork(S, 50), B = flattenWork(S, 800);
    EXPECT_GE(B, 10 * A) << int(S);
    EXPECT_LE(B, 20 * A) << int(S);
  }
}

//===----------------------------------------------------------------------===//
// The checks of the lowered module (checkCalls)
//===----------------------------------------------------------------------===//

namespace {

/// checkCalls on the module of IR Body (after a header that declares a Go++
/// (swiftcc) and a C callee) for Triple; "" when it passes.
std::string checkIR(const std::string &Body,
                    const char *Triple = "x86_64-unknown-linux-gnu") {
  llvm::LLVMContext LC;
  llvm::SMDiagnostic Diag;
  std::string IR = std::string("target triple = \"") + Triple +
                   "\"\n"
                   "declare swiftcc void @goFn()\n"
                   "declare void @cFn()\n" +
                   Body;
  std::unique_ptr<llvm::Module> M = llvm::parseAssemblyString(IR, Diag, LC);
  if (!M)
    return "parse error: " + Diag.getMessage().str();
  EXPECT_FALSE(llvm::verifyModule(*M, &llvm::errs()));
  llvm::Error E = checkCalls(*M);
  return E ? llvm::toString(std::move(E)) : "";
}

} // namespace

TEST(GoAbiCalls, StaticDataHoldsGoppFunctions) {
  // P04.06: an itab, a method table and a type descriptor hold functions
  // that Go++ calls through pointers of Go++ function types, so each must
  // have the Go++ convention of the target, swiftcc on both. Both sides on
  // both targets, nested in a struct and an array.
  const char *Amd = "x86_64-unknown-linux-gnu";
  const char *Arm = "aarch64-unknown-linux-gnu";
  auto Table = [](const char *CC) {
    return std::string("define ") + CC +
           " void @thunk() {\n  ret void\n}\n"
           "@itab = internal constant { ptr, [2 x ptr] } "
           "{ ptr null, [2 x ptr] [ptr @thunk, ptr null] }\n";
  };
  EXPECT_EQ(checkIR(Table("swiftcc"), Amd), "");
  EXPECT_NE(checkIR(Table(""), Amd).find("static data itab holds thunk"),
            std::string::npos);
  EXPECT_NE(checkIR(Table("x86_regcallcc"), Amd), "");
  EXPECT_EQ(checkIR(Table("swiftcc"), Arm), "");
  EXPECT_NE(checkIR(Table(""), Arm), "");
  // A global whose initializer names another global, which holds the
  // function: found through the other global's own initializer.
  EXPECT_NE(checkIR("@inner = internal constant ptr @cFn\n"
                    "@outer = internal constant ptr @inner\n",
                    Amd)
                .find("static data inner holds cFn"),
            std::string::npos);
}

TEST(GoAbiCalls, ADirectCallUsesItsCalleesConvention) {
  // A direct call of a Go++ (swiftcc) function with the C convention, or
  // the reverse, is an internal error.
  EXPECT_EQ(checkIR("define void @f() {\n  call swiftcc void @goFn()\n"
                    "  call void @cFn()\n  ret void\n}\n"),
            "");
  EXPECT_NE(checkIR("define void @f() {\n  call void @goFn()\n  ret void\n}\n")
                .find("does not use its calling convention"),
            std::string::npos);
  EXPECT_NE(checkIR("define void @f() {\n  call swiftcc void @cFn()\n"
                    "  ret void\n}\n")
                .find("does not use its calling convention"),
            std::string::npos);
}
