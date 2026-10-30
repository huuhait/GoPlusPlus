//===- Pipeline.cpp - Codegen pipelines per `opt` -------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/Pipeline.h"

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/OptimizationLevel.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Target/TargetMachine.h"

#include <chrono>
#include <ctime>

using namespace gopp;
using llvm::Error;
using llvm::Expected;
using llvm::StringRef;

unsigned PipelineSpec::codeGenLevel() const {
  switch (Opt) {
  case OptLevel::O0:
    return 0;
  case OptLevel::O1:
    return 1;
  case OptLevel::O2:
  case OptLevel::Os: // clang's choice: Os and Oz generate code as O2
  case OptLevel::Oz:
    return 2;
  case OptLevel::O3:
    return 3;
  }
  return 0;
}

/// The small IR pass set of `opt 0` (D6.6): inline what is marked always,
/// SROA (which includes mem2reg), instcombine, simplifycfg. The order and the
/// options are those measured in ADR-0028.
static const char *CuratedPassText =
    "always-inline,function(sroa,instcombine<no-verify-fixpoint>,simplifycfg)";

Expected<PipelineSpec> gopp::selectPipeline(const PipelineRequest &Req,
                                            const TargetSpec &T) {
  PipelineSpec S;
  struct Level {
    const char *Text, *Name;
    OptLevel L;
  };
  static const Level Levels[] = {
      {"0", "opt0", OptLevel::O0}, {"1", "opt1", OptLevel::O1},
      {"2", "opt2", OptLevel::O2}, {"3", "opt3", OptLevel::O3},
      {"s", "opts", OptLevel::Os}, {"z", "optz", OptLevel::Oz},
  };
  const Level *Chosen = nullptr;
  for (const Level &L : Levels)
    if (Req.Opt == L.Text)
      Chosen = &L;
  if (!Chosen)
    return llvm::createStringError(
        std::errc::invalid_argument,
        "unknown opt level '%s' (use 0, 1, 2, 3, s or z)", Req.Opt.c_str());
  if (Req.LTO != "none")
    return llvm::createStringError(
        std::errc::not_supported,
        "lto '%s' is not built yet (it comes with P15); use lto none",
        Req.LTO.c_str());
  if (Req.Baseline && Chosen->L != OptLevel::O0)
    return llvm::createStringError(
        std::errc::invalid_argument,
        "-pipeline=fastisel is plain O0 code generation and cannot be "
        "combined with -opt=%s",
        Req.Opt.c_str());

  S.Opt = Chosen->L;
  S.Name = Req.Baseline ? "fastisel" : Chosen->Name;
  S.Baseline = Req.Baseline;
  if (Req.Baseline) {
    // Plain O0 and FastISel: no GIR pass, no IR pass (D4.8). On arm64 the
    // selector is FastISel too, since the baseline is what to measure
    // against and ADR-0028 measures FastISel there. The AArch64 FastISel bug
    // below does not reach it: with no SROA every argument goes to its stack
    // slot first, so no `sext` reads a `signext` argument.
    S.Sel = Selector::FastISel;
  } else if (S.Opt == OptLevel::O0) {
    S.GIRInline = true;
    S.IR = IRPasses::Curated;
    S.CuratedPasses = CuratedPassText;
    // The selector of `opt 0` (ADR-0028 addendum, P04.13): FastISel on
    // x86-64 (ADR-0028 decision 2), GlobalISel on arm64. FastISel is a few
    // percent cheaper there, but LLVM 23.1.0's AArch64 FastISel selects
    // `sext` of a `signext` i8/i16 argument to i64 as a zero-extension, and
    // SROA makes that pattern (Pipeline.Arm64SextOfSignextArgument fails
    // when LLVM is fixed: then measure again).
    S.Sel = T.Arch == "arm64" ? Selector::GlobalISel : Selector::FastISel;
  } else {
    S.GIRInline = true;
    S.IR = IRPasses::Standard;
  }
  if (Req.Selector == "fastisel")
    S.Sel = Selector::FastISel;
  else if (Req.Selector == "globalisel")
    S.Sel = Selector::GlobalISel;
  else if (!Req.Selector.empty() && Req.Selector != "default")
    return llvm::createStringError(
        std::errc::invalid_argument,
        "unknown selector '%s' (use fastisel, globalisel or default)",
        Req.Selector.c_str());
  // The code generator fuses only on arm64 (D4.12), so only there can a
  // negation of an llvm.fma meet the AArch64 FNMADD patterns.
  if (T.Arch == "arm64")
    S.PreISel.push_back([](llvm::ModulePassManager &MPM) {
      MPM.addPass(FenceNegatedFmaPass());
    });
  return std::move(S);
}

/// Is U a negation of V: `fneg V`, `V * -1.0`, `-1.0 * V` or `V / -1.0`?
static bool negates(const llvm::User *U, const llvm::Value *V) {
  if (const auto *N = llvm::dyn_cast<llvm::UnaryOperator>(U))
    return N->getOpcode() == llvm::Instruction::FNeg;
  const auto *B = llvm::dyn_cast<llvm::BinaryOperator>(U);
  if (!B)
    return false;
  auto MinusOne = [](const llvm::Value *C) {
    const auto *F = llvm::dyn_cast<llvm::ConstantFP>(C);
    return F && F->isExactlyValue(-1.0);
  };
  const llvm::Value *L = B->getOperand(0), *R = B->getOperand(1);
  if (B->getOpcode() == llvm::Instruction::FMul)
    return (L == V && MinusOne(R)) || (R == V && MinusOne(L));
  if (B->getOpcode() == llvm::Instruction::FDiv)
    return L == V && MinusOne(R);
  return false;
}

llvm::PreservedAnalyses
FenceNegatedFmaPass::run(llvm::Module &M, llvm::ModuleAnalysisManager &) {
  bool Changed = false;
  llvm::SmallVector<llvm::User *, 4> Negations;
  for (llvm::Function &F : M)
    for (llvm::BasicBlock &BB : F)
      for (llvm::Instruction &I : BB) {
        auto *Call = llvm::dyn_cast<llvm::IntrinsicInst>(&I);
        if (!Call || Call->getIntrinsicID() != llvm::Intrinsic::fma)
          continue;
        Negations.clear();
        for (llvm::User *U : Call->users())
          if (negates(U, Call))
            Negations.push_back(U);
        if (Negations.empty())
          continue;
        llvm::IRBuilder<> B(Call->getNextNode());
        B.SetCurrentDebugLocation(Call->getDebugLoc());
        llvm::Value *Fence = B.CreateArithmeticFence(Call, Call->getType());
        for (llvm::User *U : Negations)
          U->replaceUsesOfWith(Call, Fence);
        Changed = true;
      }
  return Changed ? llvm::PreservedAnalyses::none()
                 : llvm::PreservedAnalyses::all();
}

Expected<std::unique_ptr<llvm::TargetMachine>>
gopp::createTargetMachine(const TargetSpec &T, const PipelineSpec &Spec) {
  Expected<std::unique_ptr<llvm::TargetMachine>> TM =
      createTargetMachine(T, Spec.codeGenLevel());
  if (!TM)
    return TM;
  switch (Spec.Sel) {
  case Selector::Default:
    break;
  case Selector::FastISel:
    // `setGlobalISel(false)` is what makes aarch64 -O0 use FastISel; x86-64
    // uses it already. Only O0 code generation has a FastISel to ask for.
    (*TM)->setGlobalISel(false);
    (*TM)->setO0WantsFastISel(true);
    break;
  case Selector::GlobalISel:
    (*TM)->setGlobalISel(true);
    (*TM)->setGlobalISelAbort(llvm::GlobalISelAbortMode::Disable);
    break;
  }
  return TM;
}

static double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static double cpuSeconds() {
  timespec TS;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &TS);
  return TS.tv_sec + TS.tv_nsec * 1e-9;
}

StageTimer::StageTimer(const char *Name, PipelineTimes *Out)
    : Name(Name), Out(Out), Scope(("gopp." + std::string(Name)).c_str()),
      Wall(wallSeconds()), Cpu(cpuSeconds()) {}

StageTimer::~StageTimer() {
  if (Out)
    Out->Stages.push_back({Name, wallSeconds() - Wall, cpuSeconds() - Cpu});
}

gir::InlineStats gopp::runGIRPasses(gir::Module &M, const PipelineSpec &Spec,
                                    PipelineTimes *Times) {
  if (!Spec.GIRInline)
    return {};
  StageTimer T("gir-inline", Times);
  return gir::inlineTrivial(M, Spec.Inline);
}

namespace {

/// The analysis managers a pass pipeline needs, wired as clang does.
struct PassContext {
  llvm::LoopAnalysisManager LAM;
  llvm::FunctionAnalysisManager FAM;
  llvm::CGSCCAnalysisManager CGAM;
  llvm::ModuleAnalysisManager MAM;
  llvm::PassBuilder PB;
  explicit PassContext(llvm::TargetMachine &TM) : PB(&TM) {
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  }
};

/// LLVM's levels are O0 to O3. `s` and `z` are the O2 pipeline on functions
/// marked `optsize` (and `minsize` for z), as clang does it.
llvm::OptimizationLevel levelOf(OptLevel L) {
  switch (L) {
  case OptLevel::O0:
    return llvm::OptimizationLevel::O0;
  case OptLevel::O1:
    return llvm::OptimizationLevel::O1;
  case OptLevel::O2:
  case OptLevel::Os:
  case OptLevel::Oz:
    return llvm::OptimizationLevel::O2;
  case OptLevel::O3:
    return llvm::OptimizationLevel::O3;
  }
  return llvm::OptimizationLevel::O0;
}

void markForSize(llvm::Module &M, OptLevel L) {
  if (L != OptLevel::Os && L != OptLevel::Oz)
    return;
  for (llvm::Function &F : M) {
    if (F.isDeclaration())
      continue;
    F.addFnAttr(llvm::Attribute::OptimizeForSize);
    if (L == OptLevel::Oz)
      F.addFnAttr(llvm::Attribute::MinSize);
  }
}

} // namespace

Error gopp::runIRPasses(llvm::Module &M, llvm::TargetMachine &TM,
                        const PipelineSpec &Spec, PipelineTimes *Times) {
  if (Spec.IR != IRPasses::None) {
    StageTimer T("ir-passes", Times);
    PassContext PC(TM);
    llvm::ModulePassManager MPM;
    if (Spec.IR == IRPasses::Curated) {
      if (Error E = PC.PB.parsePassPipeline(MPM, Spec.CuratedPasses))
        return E;
    } else {
      markForSize(M, Spec.Opt);
      MPM = PC.PB.buildPerModuleDefaultPipeline(levelOf(Spec.Opt));
    }
    MPM.run(M, PC.MAM);
  }
  if (!Spec.PreISel.empty()) {
    StageTimer T("pre-isel", Times);
    PassContext PC(TM);
    llvm::ModulePassManager Pre;
    for (const PreISelHook &H : Spec.PreISel)
      H(Pre);
    Pre.run(M, PC.MAM);
  }
  return Error::success();
}
