//===- PipelineTest.cpp - Codegen pipelines per `opt` ---------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.13. One place picks the pipeline (selectPipeline); these tests check the
// table it gives, what each stage does to the IR, the hooks other tasks plug
// in (pre-ISel passes, the trace scopes), that what a program computes is the
// same in every pipeline (run in LLJIT), that the lowering emits no flag that
// promises the optimizer more than Go does, and FMA contraction: fused on
// arm64 in every pipeline where gc fuses, not where gc does not, never on
// amd64, never through an inlined call, with an analysis whose work is
// counted (near-linear). Arm64SextOfSignextArgument guards the LLVM FastISel
// bug that made arm64 `opt 0` use GlobalISel (ADR-0028, P04.13 addendum). The
// run-time side, every program at every setting, is run/core-opt-matrix*.
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/Pipeline.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/Loader.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/TimeProfiler.h"
#include "llvm/Target/TargetMachine.h"

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <regex>

using namespace gopp;

namespace {

std::unique_ptr<gir::Module> lowerSource(gir::Context &C,
                                         const std::string &Source) {
  llvm::SmallString<128> Dir;
  EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-pl", Dir));
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
  std::unique_ptr<gir::Module> M;
  if (CP && !Diags.hasErrors())
    M = gir::lowerPackage(C, SM, *CP, L.filesOf(CP), Diags);
  llvm::sys::fs::remove_directories(Dir);
  return M;
}

/// Every setting a user can ask for, in the order of the table.
struct Setting {
  const char *Opt;
  bool Baseline;
  const char *Name;
};
const Setting AllSettings[] = {
    {"0", false, "opt0"},    {"1", false, "opt1"}, {"2", false, "opt2"},
    {"3", false, "opt3"},    {"s", false, "opts"}, {"z", false, "optz"},
    {"0", true, "fastisel"},
};

PipelineSpec specOf(const Setting &S, const char *Target = "linux/amd64") {
  PipelineRequest R;
  R.Opt = S.Opt;
  R.Baseline = S.Baseline;
  auto Spec = selectPipeline(R, *parseTarget(Target));
  EXPECT_TRUE(static_cast<bool>(Spec)) << toString(Spec.takeError());
  return std::move(*Spec);
}

//===----------------------------------------------------------------------===//
// The table
//===----------------------------------------------------------------------===//

TEST(Pipeline, TheTableOfPipelines) {
  for (const char *Target : {"linux/amd64", "linux/arm64"}) {
    for (const Setting &S : AllSettings) {
      PipelineSpec P = specOf(S, Target);
      EXPECT_EQ(P.Name, S.Name);
      EXPECT_EQ(P.Baseline, S.Baseline);
      EXPECT_EQ(P.LTO, LTOMode::None);
      if (S.Baseline) {
        // Plain O0 and FastISel: no GIR pass, no IR pass (D4.8).
        EXPECT_FALSE(P.GIRInline);
        EXPECT_EQ(P.IR, IRPasses::None);
        EXPECT_EQ(P.Sel, Selector::FastISel);
        EXPECT_EQ(P.codeGenLevel(), 0u);
      } else {
        EXPECT_TRUE(P.GIRInline) << S.Name;
        EXPECT_EQ(P.IR, std::string(S.Opt) == "0" ? IRPasses::Curated
                                                  : IRPasses::Standard);
      }
    }
    // The levels' code generator levels: O0, O1, O2, O3, and s and z as O2.
    EXPECT_EQ(specOf(AllSettings[0], Target).codeGenLevel(), 0u);
    EXPECT_EQ(specOf(AllSettings[1], Target).codeGenLevel(), 1u);
    EXPECT_EQ(specOf(AllSettings[2], Target).codeGenLevel(), 2u);
    EXPECT_EQ(specOf(AllSettings[3], Target).codeGenLevel(), 3u);
    EXPECT_EQ(specOf(AllSettings[4], Target).codeGenLevel(), 2u);
    EXPECT_EQ(specOf(AllSettings[5], Target).codeGenLevel(), 2u);
    // opt 0 is the curated set: SROA, instcombine, simplifycfg, always-inline.
    PipelineSpec P0 = specOf(AllSettings[0], Target);
    for (const char *Pass :
         {"always-inline", "sroa", "instcombine", "simplifycfg"})
      EXPECT_NE(P0.CuratedPasses.find(Pass), std::string::npos) << Pass;
    // The opt-0 selector (ADR-0028 addendum): FastISel on x86-64, GlobalISel
    // on arm64 (Arm64SextOfSignextArgument below says why).
    EXPECT_EQ(P0.Sel, std::string(Target) == "linux/arm64"
                          ? Selector::GlobalISel
                          : Selector::FastISel)
        << Target;
    // Above opt 0 LLVM chooses.
    EXPECT_EQ(specOf(AllSettings[2], Target).Sel, Selector::Default);
  }
}

TEST(Pipeline, RefusesWhatIsNotBuilt) {
  auto Try = [](PipelineRequest R) {
    auto S = selectPipeline(R, *parseTarget("linux/amd64"));
    return S ? std::string() : toString(S.takeError());
  };
  PipelineRequest R;
  R.Opt = "9";
  EXPECT_NE(Try(R).find("unknown opt level '9'"), std::string::npos);
  R.Opt = "";
  EXPECT_NE(Try(R).find("unknown opt level"), std::string::npos);
  R = {};
  R.Baseline = true;
  R.Opt = "2";
  EXPECT_NE(Try(R).find("cannot be combined"), std::string::npos);
  R = {};
  R.LTO = "thin";
  EXPECT_NE(Try(R).find("P15"), std::string::npos);
  R = {};
  R.Selector = "dag";
  EXPECT_NE(Try(R).find("unknown selector"), std::string::npos);
  // The nearby requests that are fine.
  R = {};
  EXPECT_EQ(Try(R), "");
  R.Baseline = true;
  EXPECT_EQ(Try(R), "");
  R = {};
  R.Opt = "3";
  R.Selector = "globalisel";
  EXPECT_EQ(Try(R), "");
}

TEST(Pipeline, TheSelectorReachesTheTargetMachine) {
  auto T = *parseTarget("linux/arm64");
  PipelineRequest R;
  R.Selector = "fastisel";
  auto F = createTargetMachine(T, *selectPipeline(R, T));
  ASSERT_TRUE(static_cast<bool>(F));
  EXPECT_FALSE((*F)->Options.EnableGlobalISel);
  EXPECT_TRUE((*F)->getO0WantsFastISel());
  R.Selector = "globalisel";
  auto G = createTargetMachine(T, *selectPipeline(R, T));
  ASSERT_TRUE(static_cast<bool>(G));
  EXPECT_TRUE((*G)->Options.EnableGlobalISel);
  // What nothing overrides: GlobalISel for arm64 `opt 0`, FastISel for its
  // baseline and for both pipelines on x86-64.
  auto Machine = [](const char *Target, const Setting &S) {
    auto TS = *parseTarget(Target);
    auto M = createTargetMachine(TS, specOf(S, Target));
    EXPECT_TRUE(static_cast<bool>(M));
    return std::move(*M);
  };
  EXPECT_TRUE(Machine("linux/arm64", AllSettings[0])->Options.EnableGlobalISel);
  EXPECT_FALSE(
      Machine("linux/arm64", AllSettings[6])->Options.EnableGlobalISel);
  for (const Setting *S : {&AllSettings[0], &AllSettings[6]}) {
    auto X = Machine("linux/amd64", *S);
    EXPECT_FALSE(X->Options.EnableGlobalISel) << S->Name;
    EXPECT_TRUE(X->getO0WantsFastISel()) << S->Name;
  }
  // The code generator's level is the one of the table.
  const llvm::CodeGenOptLevel Want[] = {
      llvm::CodeGenOptLevel::None,    llvm::CodeGenOptLevel::Less,
      llvm::CodeGenOptLevel::Default, llvm::CodeGenOptLevel::Aggressive,
      llvm::CodeGenOptLevel::Default, llvm::CodeGenOptLevel::Default,
      llvm::CodeGenOptLevel::None};
  for (size_t I = 0; I < std::size(AllSettings); ++I)
    for (const char *Target : {"linux/amd64", "linux/arm64"})
      EXPECT_EQ(Machine(Target, AllSettings[I])->getOptLevel(), Want[I])
          << AllSettings[I].Name << " " << Target;
}

//===----------------------------------------------------------------------===//
// The stages
//===----------------------------------------------------------------------===//

const char *Loops = "package main\n"
                    "func add3(a int, b int) int { return a*3 - b }\n"
                    "func f(n int) int {\n"
                    "\ts := 0\n"
                    "\tfor i := 0; i < n; i++ {\n"
                    "\t\ts += add3(i, s)\n"
                    "\t}\n"
                    "\treturn s\n"
                    "}\n"
                    "func main() { println(f(5)) }\n";

/// The LLVM module of Source for Target after the stages of S.
struct Built {
  gir::Context GC;
  llvm::LLVMContext LC;
  std::unique_ptr<llvm::TargetMachine> TM;
  std::unique_ptr<llvm::Module> M;
  PipelineTimes Times;
  PipelineSpec Spec;
  gir::InlineStats Inlined;

  /// KeepVariables: as `gopp build` sets it (LowerToLLVMOptions), the
  /// variables of opt 0 stay in memory for the debugger (P04.12).
  Built(const std::string &Source, const Setting &S,
        const char *Target = "linux/amd64", bool KeepVariables = false) {
    Spec = specOf(S, Target);
    auto GM = lowerSource(GC, Source);
    EXPECT_NE(GM, nullptr);
    if (!GM)
      return;
    auto T = *parseTarget(Target);
    TM = std::move(*createTargetMachine(T, Spec));
    Inlined = runGIRPasses(*GM, Spec, &Times);
    LowerToLLVMOptions LO;
    LO.KeepVariables =
        KeepVariables && Spec.Opt == OptLevel::O0 && !Spec.Baseline;
    auto LM = lowerToLLVM(LC, *GM, *TM, LO);
    EXPECT_TRUE(static_cast<bool>(LM)) << toString(LM.takeError());
    if (!LM)
      return;
    M = std::move(*LM);
    llvm::Error E = runIRPasses(*M, *TM, Spec, &Times);
    EXPECT_FALSE(static_cast<bool>(E)) << toString(std::move(E));
  }
  std::string ir() const {
    std::string S;
    llvm::raw_string_ostream OS(S);
    M->print(OS, nullptr);
    return S;
  }
  std::string assembly() const {
    std::string S;
    llvm::raw_string_ostream OS(S);
    llvm::Error E = emitAssembly(*M, *TM, OS);
    EXPECT_FALSE(static_cast<bool>(E)) << toString(std::move(E));
    return S;
  }
  /// The instructions with Opcode in the functions whose name contains In.
  unsigned count(unsigned Opcode, llvm::StringRef In = "") const {
    unsigned N = 0;
    for (const llvm::Function &F : *M)
      if (F.getName().contains(In))
        for (const llvm::BasicBlock &BB : F)
          for (const llvm::Instruction &I : BB)
            N += I.getOpcode() == Opcode;
    return N;
  }
};

TEST(Pipeline, Opt0PromotesLocalsAndInlinesTrivialCalls) {
  Built Base(Loops, AllSettings[6]);
  Built O0(Loops, AllSettings[0]);
  // The baseline keeps every local in memory and the call to add3; opt 0
  // promotes the locals (SROA) and has inlined add3 in GIR.
  EXPECT_GT(Base.count(llvm::Instruction::Alloca, "4mainE1f"), 0u);
  EXPECT_EQ(O0.count(llvm::Instruction::Alloca, "4mainE1f"), 0u) << O0.ir();
  EXPECT_NE(Base.ir().find("call i64 @_GPF4mainE4add3"), std::string::npos);
  EXPECT_EQ(O0.ir().find("call i64 @_GPF4mainE4add3"), std::string::npos)
      << O0.ir();
  EXPECT_EQ(Base.Inlined.Inlined, 0u);
  EXPECT_EQ(O0.Inlined.Inlined, 1u);
  // The stages that ran are recorded, in order.
  ASSERT_EQ(O0.Times.Stages.size(), 2u);
  EXPECT_EQ(O0.Times.Stages[0].Name, "gir-inline");
  EXPECT_EQ(O0.Times.Stages[1].Name, "ir-passes");
  EXPECT_TRUE(Base.Times.Stages.empty());
}

TEST(Pipeline, Opt0WithDwarfKeepsTheVariablesInMemory) {
  // P04.12 (M2 of the team build): `gopp build` at opt 0 keeps the variables
  // of the source in memory with volatile accesses, so a debugger prints them
  // at any line (docs/design/pipelines.md section 7); the temporaries are
  // registers as before and the call is still inlined.
  Built O0(Loops, AllSettings[0], "linux/amd64", /*KeepVariables=*/true);
  EXPECT_GT(O0.count(llvm::Instruction::Alloca, "4mainE1f"), 0u) << O0.ir();
  EXPECT_EQ(O0.ir().find("call i64 @_GPF4mainE4add3"), std::string::npos);
  EXPECT_NE(O0.ir().find("store volatile"), std::string::npos) << O0.ir();
  EXPECT_NE(O0.ir().find("load volatile"), std::string::npos);
  EXPECT_NE(O0.ir().find("#dbg_declare"), std::string::npos);
  // The other side: without it opt 0 promotes them (the test above), and
  // nothing is volatile. (`gopp build` sets it at opt 0 only:
  // codegen/pipelines.test checks the other settings.)
  Built Plain(Loops, AllSettings[0]);
  EXPECT_EQ(Plain.ir().find("volatile"), std::string::npos) << Plain.ir();
}

TEST(Pipeline, SizeLevelsMarkTheirFunctions) {
  Built Os(Loops, AllSettings[4]), Oz(Loops, AllSettings[5]),
      O2(Loops, AllSettings[2]);
  EXPECT_NE(Os.ir().find("optsize"), std::string::npos);
  EXPECT_EQ(Os.ir().find("minsize"), std::string::npos);
  EXPECT_NE(Oz.ir().find("minsize"), std::string::npos);
  EXPECT_EQ(O2.ir().find("optsize"), std::string::npos);
}

TEST(Pipeline, PreISelHooksRunAfterTheIRPasses) {
  struct Probe : llvm::PassInfoMixin<Probe> {
    std::vector<std::string> *Log;
    int Tag;
    unsigned *Allocas;
    llvm::PreservedAnalyses run(llvm::Module &M,
                                llvm::ModuleAnalysisManager &) {
      Log->push_back("hook" + std::to_string(Tag));
      for (llvm::Function &F : M)
        if (F.getName().contains("4mainE1f"))
          for (llvm::BasicBlock &BB : F)
            for (llvm::Instruction &I : BB)
              *Allocas += llvm::isa<llvm::AllocaInst>(I);
      return llvm::PreservedAnalyses::all();
    }
  };
  std::vector<std::string> Log;
  unsigned Allocas = 0;
  gir::Context GC;
  llvm::LLVMContext LC;
  auto GM = lowerSource(GC, Loops);
  ASSERT_NE(GM, nullptr);
  auto T = *parseTarget("linux/amd64");
  PipelineSpec Spec = specOf(AllSettings[0]);
  for (int Tag : {1, 2})
    Spec.PreISel.push_back([&Log, &Allocas, Tag](llvm::ModulePassManager &MPM) {
      MPM.addPass(Probe{{}, &Log, Tag, &Allocas});
    });
  auto TM = std::move(*createTargetMachine(T, Spec));
  runGIRPasses(*GM, Spec);
  auto LM = lowerToLLVM(LC, *GM, *TM);
  ASSERT_TRUE(static_cast<bool>(LM));
  PipelineTimes Times;
  ASSERT_FALSE(static_cast<bool>(runIRPasses(**LM, *TM, Spec, &Times)));
  EXPECT_EQ(Log, (std::vector<std::string>{"hook1", "hook2"}));
  // They saw the module after the opt-0 passes: no alloca in f.
  EXPECT_EQ(Allocas, 0u);
  ASSERT_EQ(Times.Stages.size(), 2u);
  EXPECT_EQ(Times.Stages[0].Name, "ir-passes");
  EXPECT_EQ(Times.Stages[1].Name, "pre-isel");
}

TEST(Pipeline, EveryStageIsATraceScope) {
  // `-trace` (D6.7) starts LLVM's time profiler; the stages show up in it.
  llvm::timeTraceProfilerInitialize(1, "pipeline-test");
  Built O0(Loops, AllSettings[0]);
  llvm::SmallString<0> Buf;
  {
    llvm::raw_svector_ostream OS(Buf);
    llvm::timeTraceProfilerWrite(OS);
  }
  std::string Trace(Buf.str());
  llvm::timeTraceProfilerCleanup();
  EXPECT_NE(Trace.find("gopp.gir-inline"), std::string::npos) << Trace;
  EXPECT_NE(Trace.find("gopp.ir-passes"), std::string::npos) << Trace;
}

//===----------------------------------------------------------------------===//
// The same results in every pipeline
//===----------------------------------------------------------------------===//

// Go defines wrap-around, shifts by any count, MinInt / -1 and a saturating
// conversion; the functions below compute them through loops an optimizer may
// rewrite. The expected values are computed here with unsigned arithmetic.
const char *Semantics =
    "package main\n"
    "func wrap(x int32, n int32) int32 {\n"
    "\ts := x\n"
    "\tfor i := int32(0); i < n; i++ {\n"
    "\t\ts += s*3 + i\n"
    "\t}\n"
    "\treturn s\n"
    "}\n"
    "func gt(x int64) bool { return x+1 > x }\n"
    "func count8(from int8) int {\n"
    "\tn := 0\n"
    "\tfor i := from; i > 0; i++ {\n"
    "\t\tn++\n"
    "\t}\n"
    "\treturn n\n"
    "}\n"
    "func shl(x int32, n uint) int32 { return x << n }\n"
    "func shr(x int32, n uint) int32 { return x >> n }\n"
    "func divmin(a int64, b int64) int64 { return a/b + a%b }\n"
    "func sat(f float64) int8 { return int8(f) }\n"
    "func main() {}\n";

TEST(Pipeline, SameResultsInEveryPipeline) {
  auto Host = hostTarget();
  if (!Host) {
    llvm::consumeError(Host.takeError());
    GTEST_SKIP() << "the host is not a supported target";
  }
  for (const Setting &S : AllSettings) {
    SCOPED_TRACE(S.Name);
    // The JIT needs a context of its own, so the module is lowered in it.
    gir::Context GC;
    auto GM = lowerSource(GC, Semantics);
    ASSERT_NE(GM, nullptr);
    PipelineSpec Spec = specOf(S, Host->name().c_str());
    auto TM = std::move(*createTargetMachine(*Host, Spec));
    runGIRPasses(*GM, Spec);
    auto Ctx = std::make_unique<llvm::LLVMContext>();
    auto LM = lowerToLLVM(*Ctx, *GM, *TM);
    ASSERT_TRUE(static_cast<bool>(LM)) << toString(LM.takeError());
    ASSERT_FALSE(static_cast<bool>(runIRPasses(**LM, *TM, Spec)));
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto J = std::move(*llvm::orc::LLJITBuilder().create());
    ASSERT_FALSE(static_cast<bool>(J->addIRModule(
        llvm::orc::ThreadSafeModule(std::move(*LM), std::move(Ctx)))));
    auto Find = [&](const char *Sym) {
      auto A = J->lookup(Sym);
      EXPECT_TRUE(static_cast<bool>(A)) << toString(A.takeError());
      return A ? A->getValue() : 0;
    };
    auto Wrap = reinterpret_cast<int32_t (*)(int32_t, int32_t)>(
        Find("_GPF4mainE4wrap"));
    auto Gt = reinterpret_cast<bool (*)(int64_t)>(Find("_GPF4mainE2gt"));
    auto Count8 =
        reinterpret_cast<int64_t (*)(int8_t)>(Find("_GPF4mainE6count8"));
    auto Shl = reinterpret_cast<int32_t (*)(int32_t, uint64_t)>(
        Find("_GPF4mainE3shl"));
    auto Shr = reinterpret_cast<int32_t (*)(int32_t, uint64_t)>(
        Find("_GPF4mainE3shr"));
    auto DivMin = reinterpret_cast<int64_t (*)(int64_t, int64_t)>(
        Find("_GPF4mainE6divmin"));
    auto Sat = reinterpret_cast<int8_t (*)(double)>(Find("_GPF4mainE3sat"));
    ASSERT_TRUE(Wrap && Gt && Count8 && Shl && Shr && DivMin && Sat);
    for (int32_t X : {0, 1, -7, 2147483000, INT32_MIN}) {
      uint32_t U = static_cast<uint32_t>(X);
      for (int32_t I = 0; I < 40; ++I)
        U += U * 3u + static_cast<uint32_t>(I);
      EXPECT_EQ(static_cast<uint32_t>(Wrap(X, 40)), U) << X;
    }
    EXPECT_FALSE(Gt(INT64_MAX));
    EXPECT_TRUE(Gt(5));
    EXPECT_EQ(Count8(120), 8);
    EXPECT_EQ(Count8(-3), 0);
    for (uint64_t N : {0u, 1u, 31u, 32u, 33u, 200u}) {
      EXPECT_EQ(Shl(-5, N),
                N >= 32 ? 0
                        : static_cast<int32_t>(static_cast<uint32_t>(-5) << N));
      EXPECT_EQ(Shr(-5, N), N >= 32 ? -1 : (-5 >> N));
    }
    EXPECT_EQ(DivMin(INT64_MIN, -1), INT64_MIN); // MinInt / -1 = MinInt, % = 0
    EXPECT_EQ(DivMin(-7, 2), -3 + -1);
    EXPECT_EQ(Sat(1e30), 127);
    EXPECT_EQ(Sat(-1e30), -128);
    EXPECT_EQ(Sat(std::nan("")), 0);
    EXPECT_EQ(Sat(-3.9), -3);
  }
}

//===----------------------------------------------------------------------===//
// FMA contraction in every pipeline
//===----------------------------------------------------------------------===//

const char *Fma =
    "package main\n"
    "func fused(x, y, z float64) float64 { t := x * y; return t + z }\n"
    "func fusedTwice(x, y, z float64) float64 { t := x * y; return (t + z) * "
    "(t - z) }\n"
    "func rounded(x, y, z float64) float64 { t := float64(x * y); return t + z "
    "}\n"
    "func direct(x, y, z float64) float64 { return x*y + z }\n"
    "func mul(a, b float64) float64 { return a * b }\n"
    "func viaCall(x, y, z float64) float64 { return mul(x, y) + z }\n"
    "func viaParam(x, y, z float64) float64 { return plus(x*y, z) }\n"
    "func plus(a, b float64) float64 { return a + b }\n"
    "func twoWrites(x, y, z float64, c bool) float64 {\n"
    "\tvar t float64\n"
    "\tif c {\n\t\tt = x * y\n\t} else {\n\t\tt = x * z\n\t}\n"
    "\treturn t + z\n"
    "}\n"
    "func addrTaken(x, y, z float64) float64 {\n"
    "\tt := x * y\n"
    "\tp := &t\n"
    "\t*p = *p + 0\n"
    "\treturn t + z\n"
    "}\n"
    // Written twice, read in a block the second write's block dominates: the
    // read sees the second product only (gc's SSA value).
    "func rewrittenCross(x, y, z float64, c bool) float64 {\n"
    "\tt := x * z\n"
    "\tt = x * y\n"
    "\tif c {\n\t\treturn t + z\n\t}\n"
    "\treturn 0\n"
    "}\n"
    // A loop body that writes and reads t, and a block after the loop that a
    // walk of the blocks reaches first and that writes t as well: the loop's
    // read still sees the loop's product (`return r * t` reads no product).
    "func loopBreak(x, y, a, b, z float64, n int) float64 {\n"
    "\tvar t float64\n"
    "\tr := 0.0\n"
    "\ti := 0\n"
    "\tfor {\n\t\tif i >= n {\n\t\t\tbreak\n\t\t}\n"
    "\t\tt = a * b\n\t\tr = t + z\n\t\ti++\n\t}\n"
    "\tt = x * y\n"
    "\treturn r * t\n"
    "}\n"
    // A write between the product and the read on one path (a phi in gc).
    "func writtenOnAPath(x, y, z float64, c bool) float64 {\n"
    "\tt := x * y\n"
    "\tif c {\n\t\tt = z\n\t}\n"
    "\treturn t + z\n"
    "}\n"
    // A write in the loop that a path reaches the sum only through the
    // product again: the sum reads the product.
    "func loopWriteAfter(x, y, z float64, n int) float64 {\n"
    "\tvar t float64\n\tr := 0.0\n"
    "\tfor i := 0; i < n; i++ {\n"
    "\t\tt = x * y\n\t\tif i&1 == 0 {\n\t\t\tr += t + z\n\t\t}\n"
    "\t\tif i == 5 {\n\t\t\tt = 0\n\t\t}\n\t}\n"
    "\treturn r + t\n"
    "}\n"
    // The other write returns: it never reaches the sum.
    "func earlyReturn(x, y, z float64, c bool) float64 {\n"
    "\tt := x * y\n\tif c {\n\t\tt = z\n\t\treturn t\n\t}\n"
    "\treturn t + z\n"
    "}\n"
    // The same write on the path from the product to the sum (a phi).
    "func loopWriteBetween(x, y, z float64, n int) float64 {\n"
    "\tvar t float64\n\tr := 0.0\n"
    "\tfor i := 0; i < n; i++ {\n"
    "\t\tt = x * y\n\t\tif i == 5 {\n\t\t\tt = 0\n\t\t}\n"
    "\t\tif i&1 == 0 {\n\t\t\tr += t + z\n\t\t}\n\t}\n"
    "\treturn r + t\n"
    "}\n"
    // A trivial function with a product in its result: the GIR inliner copies
    // it into callsScaleAdd, where it fuses as in the callee.
    "func scaleAdd(x, y, z float64) (r float64) {\n"
    "\tr = x * y\n\tr = r + z\n\treturn\n"
    "}\n"
    "func callsScaleAdd(x, y, z float64) float64 {\n"
    "\treturn scaleAdd(x, y, z) * 2\n"
    "}\n"
    "func main() {}\n";

/// The text of the LLVM function whose name ends in Name.
std::string functionText(const std::string &IR, const std::string &Name) {
  size_t At = IR.find("@_GPF4mainE" + std::to_string(Name.size()) + Name + "(");
  if (At == std::string::npos)
    return "";
  size_t Def = IR.rfind("define", At);
  size_t End = IR.find("\n}\n", At);
  return IR.substr(Def, End - Def);
}

bool fuses(const std::string &Text) {
  return Text.find("llvm.fma") != std::string::npos;
}

TEST(Pipeline, FmaIsTheSameInEveryPipeline) {
  for (const Setting &S : AllSettings) {
    SCOPED_TRACE(S.Name);
    Built Arm(Fma, S, "linux/arm64"), Amd(Fma, S, "linux/amd64");
    std::string A = Arm.ir(), X = Amd.ir();
    // gc fuses a product kept in a variable, also when it is read twice, and
    // a product in an expression; a variable written twice, read where only
    // the second write reaches; a loop's product read in the loop, whatever
    // the blocks after it write; a product in a result.
    for (const char *Yes :
         {"fused", "fusedTwice", "direct", "rewrittenCross", "loopBreak",
          "loopWriteAfter", "earlyReturn", "scaleAdd"})
      EXPECT_TRUE(fuses(functionText(A, Yes))) << Yes;
    // GIR inlining has put scaleAdd into its caller (and above opt 0 LLVM's
    // inliner may as well): the copy fuses as the callee does.
    if (!S.Baseline)
      EXPECT_TRUE(fuses(functionText(A, "callsScaleAdd")));
    // It does not fuse a conversion (it rounds), the result of a call (inlined
    // or not), an argument, a variable two branches write (a phi), a variable
    // a path between the product and the read writes, or a variable whose
    // address is taken.
    for (const char *No : {"rounded", "viaCall", "viaParam", "twoWrites",
                           "writtenOnAPath", "loopWriteBetween", "addrTaken"})
      EXPECT_FALSE(fuses(functionText(A, No))) << No;
    // The functions are all there to be looked at (O3 keeps external ones).
    for (const char *Fn : {"fused", "rounded", "viaCall", "twoWrites",
                           "loopBreak", "writtenOnAPath", "callsScaleAdd"})
      EXPECT_NE(functionText(A, Fn), "") << Fn;
    // Nothing is fused on amd64, and no float instruction asks LLVM to fuse
    // or to keep a conversion apart: the code generator is the only place that
    // decides, so no LLVM pass and no selector can change the result.
    EXPECT_FALSE(fuses(X));
    EXPECT_EQ(A.find("contract"), std::string::npos);
    EXPECT_EQ(A.find("arithmetic.fence"), std::string::npos);
    EXPECT_EQ(X.find("contract"), std::string::npos);
    // The back end keeps it: fmadd for the fused ones, a multiply and an add
    // for the others. (At optz the machine outliner moves bodies that are
    // alike, fused's and scaleAdd's, to a shared function.)
    bool Outlined = std::string(S.Name) == "optz";
    std::string Asm = Arm.assembly();
    auto Label = Asm.find("_GPF4mainE5fused:");
    ASSERT_NE(Label, std::string::npos);
    std::string Body = Asm.substr(Label, Asm.find(".Lfunc_end", Label) - Label);
    EXPECT_NE((Outlined ? Asm : Body).find("fmadd"), std::string::npos) << Body;
    auto RLabel = Asm.find("_GPF4mainE7rounded:");
    ASSERT_NE(RLabel, std::string::npos);
    std::string RBody =
        Asm.substr(RLabel, Asm.find(".Lfunc_end", RLabel) - RLabel);
    EXPECT_EQ(RBody.find("fmadd"), std::string::npos) << RBody;
    if (!Outlined)
      EXPECT_NE(RBody.find("fmul"), std::string::npos) << RBody;
    std::string XAsm = Amd.assembly();
    EXPECT_EQ(XAsm.find("vfmadd"), std::string::npos);
  }
}

//===----------------------------------------------------------------------===//
// What the lowering may not promise
//===----------------------------------------------------------------------===//

/// A program with every integer and float operation, narrow types too.
const char *Operations = R"(package main

func ints(a, b int32, c, d uint8, e, f int64, s uint) (int32, uint8, int64) {
	x := a + b - a*b
	x += x / (b | 1)
	x = x % (b | 1)
	x = x << s
	x = x >> s
	y := c + d - c*d
	y = y / (d | 1)
	y = y << s
	y = y >> s
	z := e + f - e*f
	z = z / (f | 1)
	z = z % (f | 1)
	z = z << s
	z = z >> s
	z = -z
	z = ^z
	z &^= e
	x++
	y--
	return x, y, z
}

func conv(a int32, b uint64, f float64, g float32) (int8, uint16, float64, float32, int64, uint64) {
	return int8(a), uint16(b), float64(b), float32(a), int64(f), uint64(g)
}

func floats(x, y float64, u, v float32) (float64, float32, bool) {
	return (x - y) / x, u*v - u, x < y || u >= v
}

func main() {
	a, b, c := ints(1, 2, 3, 4, 5, 6, 7)
	println(a, b, c)
}
)";

TEST(Pipeline, TheLoweringEmitsNoFlagThatGivesTheOptimizerMoreThanGoDoes) {
  // An `nsw`, `nuw`, `exact`, `nneg`, `disjoint` or `samesign` is a promise
  // the language does not make (Go wraps, shifts past the width, converts
  // and compares anything); a fast-math flag is the same for floats. What the
  // optimizer derives itself is its own (and proven); what the lowering
  // writes must carry none. Pointer arithmetic keeps its `inbounds` (every
  // index is checked first). run/core-opt-ir-flags.long.test scans the whole
  // run corpus the same way.
  std::regex Flag(
      R"(\b(add|sub|mul|shl|trunc|zext|uitofp|or|icmp|lshr|ashr|udiv|sdiv)\b[^\n]*\b(nsw|nuw|exact|nneg|disjoint|samesign)\b)");
  std::regex Fast(
      R"(\b(fadd|fsub|fmul|fdiv|frem|fneg|fcmp|fpext|fptrunc|call)\b[^\n]*\b(fast|nnan|ninf|nsz|arcp|contract|afn|reassoc)\b)");
  // The scans find what they look for.
  for (const char *Bad :
       {"%3 = add nsw i32 %1, %2", "%3 = shl nuw nsw i8 %1, 2",
        "%3 = trunc nuw i64 %1 to i8", "%3 = zext nneg i8 %1 to i64",
        "%3 = sdiv exact i32 %1, 2", "%3 = or disjoint i8 %1, 2",
        "%3 = icmp samesign ult i8 %1, 2"})
    EXPECT_TRUE(std::regex_search(Bad, Flag)) << Bad;
  for (const char *Bad :
       {"%3 = fadd fast double %1, %2", "%3 = fmul contract double %1, %2",
        "%3 = fcmp nnan olt double %1, %2",
        "%3 = call nsz double @llvm.fma.f64(double %1, double %2, double %3)"})
    EXPECT_TRUE(std::regex_search(Bad, Fast)) << Bad;
  EXPECT_FALSE(std::regex_search("%3 = add i32 %1, %2", Flag));
  EXPECT_FALSE(std::regex_search(
      "%3 = getelementptr inbounds nuw i8, ptr %1, i64 8", Flag));
  for (const char *Target : {"linux/amd64", "linux/arm64"}) {
    // The baseline runs no pass: this is the IR as the lowering writes it.
    Built B(Operations, AllSettings[6], Target);
    std::string IR = B.ir();
    std::smatch M;
    EXPECT_FALSE(std::regex_search(IR, M, Flag)) << Target << ": " << M.str();
    EXPECT_FALSE(std::regex_search(IR, M, Fast)) << Target << ": " << M.str();
    // The operations are there (the scan is not empty-handed).
    EXPECT_NE(IR.find("fptosi.sat"), std::string::npos);
    EXPECT_NE(IR.find("sdiv"), std::string::npos);
  }
}

//===----------------------------------------------------------------------===//
// The FMA analysis is near-linear (counted work, M8)
//===----------------------------------------------------------------------===//

/// Nested `if`s in a loop, each reading the product made above the loop.
std::string fmaNested(int N) {
  std::string S = "package main\n\nfunc f(a, b, c float64, n int) float64 {\n"
                  "\tt := a * b\n\tr := c\n\tfor i := 0; i < n; i++ {\n";
  for (int I = 0; I < N; ++I)
    S += "\t\tif r > " + std::to_string(I) + ".0 {\n\t\t\tr = t + r\n";
  for (int I = 0; I < N; ++I)
    S += "\t\t}\n";
  return S + "\t}\n\treturn r\n}\n\nfunc main() { println(f(1, 2, 3, 4)) }\n";
}

/// Sibling `if`/`else`s in a loop that write and read the product.
std::string fmaSiblings(int N) {
  std::string S = "package main\n\nfunc f(a, b, c float64, n int) float64 {\n"
                  "\tt := a * b\n\tr := c\n\tfor i := 0; i < n; i++ {\n";
  for (int I = 0; I < N; ++I)
    S += "\t\tif r < " + std::to_string(I) +
         ".0 {\n\t\t\tt = r * c\n\t\t} else {\n\t\t\tr = t - c\n\t\t}\n";
  return S + "\t}\n\treturn r + t\n}\n\nfunc main() { println(f(1, 2, 3, 4)) "
             "}\n";
}

/// One variable written N times, each product read in a later block.
std::string fmaWrites(int N) {
  std::string S = "package main\n\nfunc f(a, b, c float64) float64 {\n"
                  "\tr := c\n\tt := a\n";
  for (int I = 0; I < N; ++I)
    S += "\tt = a * b\n\tif r > " + std::to_string(I) +
         ".0 {\n\t\tr = t + r\n\t}\n";
  return S + "\treturn r\n}\n\nfunc main() { println(f(1, 2, 3)) }\n";
}

/// N variables in one loop, each written with a product, read in an `if` and
/// written again in another `if`: a write of each that only a path through
/// its product reaches, so each needs a walk, over a loop of N blocks.
std::string fmaLoopWrites(int N) {
  std::string S = "package main\n\nfunc f(a, b, c float64, n int) float64 {\n"
                  "\tr := c\n";
  for (int I = 0; I < N; ++I)
    S += "\tvar t" + std::to_string(I) + " float64\n";
  S += "\tfor i := 0; i < n; i++ {\n";
  for (int I = 0; I < N; ++I) {
    std::string T = "t" + std::to_string(I);
    S += "\t\t" + T + " = a * b\n\t\tif r > " + std::to_string(I) +
         ".0 {\n\t\t\tr += " + T + " + c\n\t\t}\n\t\tif r < " +
         std::to_string(I) + ".5 {\n\t\t\t" + T + " = 0\n\t\t}\n";
  }
  S += "\t}\n\treturn r";
  for (int I = 0; I < N; ++I)
    S += " + t" + std::to_string(I);
  return S + "\n}\n\nfunc main() { println(f(1, 2, 3, 4)) }\n";
}

/// N sibling loops one after the other, each with its own variable written
/// with a product, read in an `if` and written again in another (review R7:
/// a walk per variable, over its own loop only).
std::string fmaSiblingLoops(int N) {
  std::string S = "package main\n\nfunc f(a, b, c float64, n int) float64 {\n"
                  "\tr := c\n";
  for (int I = 0; I < N; ++I) {
    std::string T = "t" + std::to_string(I);
    S += "\tvar " + T + " float64\n\tfor i := 0; i < n; i++ {\n\t\t" + T +
         " = a * b\n\t\tif i%2 == 0 {\n\t\t\tr += " + T +
         " + c\n\t\t}\n\t\tif i%3 == 2 {\n\t\t\t" + T + " = 0\n\t\t}\n\t}\n";
  }
  return S + "\treturn r\n}\n\nfunc main() { println(f(1, 2, 3, 4)) }\n";
}

/// The arm64 IR of the program, and the analysis' work in Counts.
std::string fmaIR(const std::string &Source, LowerWorkCounts &Counts) {
  gir::Context GC;
  auto GM = lowerSource(GC, Source);
  EXPECT_NE(GM, nullptr);
  if (!GM)
    return "";
  llvm::LLVMContext LC;
  auto T = *parseTarget("linux/arm64");
  auto TM = createTargetMachine(T, specOf(AllSettings[0], "linux/arm64"));
  EXPECT_TRUE(static_cast<bool>(TM));
  LowerToLLVMOptions O;
  O.Counts = &Counts;
  auto L = lowerToLLVM(LC, *GM, **TM, O);
  EXPECT_TRUE(static_cast<bool>(L)) << toString(L.takeError());
  if (!L)
    return "";
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  (*L)->print(OS, nullptr);
  OS.flush();
  return IR;
}

size_t countFma(const std::string &IR) {
  size_t N = 0;
  for (size_t At = IR.find("call double @llvm.fma"); At != std::string::npos;
       At = IR.find("call double @llvm.fma", At + 1))
    ++N;
  return N;
}

/// The analysis' steps for the program (lowered for arm64, where it runs).
uint64_t fmaWork(const std::string &Source) {
  gir::Context GC;
  auto GM = lowerSource(GC, Source);
  EXPECT_NE(GM, nullptr);
  if (!GM)
    return 0;
  llvm::LLVMContext LC;
  auto T = *parseTarget("linux/arm64");
  auto TM = createTargetMachine(T, specOf(AllSettings[0], "linux/arm64"));
  EXPECT_TRUE(static_cast<bool>(TM));
  LowerWorkCounts Counts;
  LowerToLLVMOptions O;
  O.Counts = &Counts;
  auto L = lowerToLLVM(LC, *GM, **TM, O);
  EXPECT_TRUE(static_cast<bool>(L)) << toString(L.takeError());
  return Counts.FmaDomSteps + Counts.FmaSccSteps + Counts.FmaDefSteps +
         Counts.FmaWalkSteps;
}

/// Linear: at most C steps per unit, and 4N at most 4.5 times N.
::testing::AssertionResult fmaStepsAreLinear(uint64_t S1, uint64_t S4,
                                             uint64_t N) {
  const uint64_t C = 128;
  if (S1 == 0 || S1 > C * N || S4 > C * 4 * N || 2 * S4 > 9 * S1)
    return ::testing::AssertionFailure()
           << "steps(N) = " << S1 << ", steps(4N) = " << S4 << ", N = " << N;
  return ::testing::AssertionSuccess();
}

TEST(Pipeline, TheFmaAnalysisIsNearLinear) {
  struct Dim {
    const char *Name;
    std::string (*Make)(int);
    int N;
    int Blocks; // the blocks one N makes, in units of the others' 3
  } Dims[] = {{"nested", fmaNested, 60, 1},
              {"siblings", fmaSiblings, 500, 1},
              {"writes", fmaWrites, 500, 1},
              {"loopWrites", fmaLoopWrites, 200, 2},
              {"siblingLoops", fmaSiblingLoops, 200, 3}};
  for (const Dim &D : Dims) {
    uint64_t S1 = fmaWork(D.Make(D.N)), S4 = fmaWork(D.Make(4 * D.N));
    EXPECT_TRUE(fmaStepsAreLinear(S1, S4, D.N * D.Blocks)) << D.Name;
  }
}

TEST(Pipeline, TheFmaWalksAreBoundedByTheSizeOfTheFunction) {
  // 200 variables in one loop each need a walk of the loop: the walks share
  // a budget, so the steps stay linear (a read after it is spent counts as
  // written: conservative).
  gir::Context GC;
  auto GM = lowerSource(GC, fmaLoopWrites(200));
  ASSERT_NE(GM, nullptr);
  llvm::LLVMContext LC;
  auto T = *parseTarget("linux/arm64");
  auto TM = createTargetMachine(T, specOf(AllSettings[0], "linux/arm64"));
  ASSERT_TRUE(static_cast<bool>(TM));
  LowerWorkCounts Counts;
  LowerToLLVMOptions O;
  O.Counts = &Counts;
  auto L = lowerToLLVM(LC, *GM, **TM, O);
  ASSERT_TRUE(static_cast<bool>(L)) << toString(L.takeError());
  EXPECT_GT(Counts.FmaWalkSteps, 0u);
  // Without a budget the walks would be 200 times a loop of 600 blocks.
  EXPECT_LT(Counts.FmaWalkSteps, 100u * 200u * 16u);
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  (*L)->print(OS, nullptr);
  OS.flush();
  size_t Fused = 0;
  for (size_t At = IR.find("call double @llvm.fma"); At != std::string::npos;
       At = IR.find("call double @llvm.fma", At + 1))
    ++Fused;
  EXPECT_GT(Fused, 0u);
}

TEST(Pipeline, EverySiblingLoopFuses) {
  // gc fuses the product of each of 32 loops (the read sees the loop's own
  // write; the write after it reaches the read only through the product's
  // block). Each walk covers its own loop, not the loops after it, so the
  // budget is not spent and none is left unfused (review R7: 16 loops left
  // loops 0 to 5 unfused).
  for (int N : {16, 32}) {
    LowerWorkCounts Counts;
    std::string IR = fmaIR(fmaSiblingLoops(N), Counts);
    EXPECT_EQ(countFma(IR), size_t(N)) << N << " loops";
    EXPECT_GT(Counts.FmaWalkSteps, 0u);
  }
}

TEST(Pipeline, ControlAQuadraticFmaCountFailsTheCheck) {
  // A linear count passes; a quadratic one fails, also when it is small
  // enough to stay under the steps-per-unit bound (the 4N/N ratio fails it).
  EXPECT_TRUE(fmaStepsAreLinear(60 * 500, 60 * 2000, 500));
  EXPECT_FALSE(fmaStepsAreLinear(500ull * 500, 2000ull * 2000, 500));
  EXPECT_FALSE(
      fmaStepsAreLinear(500ull * 500 / 100, 2000ull * 2000 / 100, 500));
}

//===----------------------------------------------------------------------===//
// The LLVM bug behind the arm64 selector
//===----------------------------------------------------------------------===//

TEST(Pipeline, Arm64SextOfSignextArgument) {
  // LLVM 23.1.0: AArch64 FastISel selects `sext` of a `signext` i8 or i16
  // argument to i64 as a 32-bit move, which zero-extends: -1 becomes
  // 4294967295. GlobalISel and SelectionDAG are right, and so is x86-64
  // FastISel (movsbq, movswq). SROA turns an argument's stack slot into this
  // very pattern, so arm64 `opt 0` must not use FastISel (ADR-0028, P04.13
  // addendum). If the FastISel line below starts to fail, LLVM was fixed:
  // measure the selectors again and revisit the choice (P04.07's TODO); do
  // not delete the test.
  auto Build = [](const char *Target, const PipelineSpec &Spec) {
    auto T = *parseTarget(Target);
    auto TM = createTargetMachine(T, Spec);
    EXPECT_TRUE(static_cast<bool>(TM));
    llvm::LLVMContext Ctx;
    llvm::Module M("m", Ctx);
    M.setTargetTriple((*TM)->getTargetTriple());
    M.setDataLayout((*TM)->createDataLayout());
    llvm::IRBuilder<> B(Ctx);
    for (llvm::Type *Narrow : {B.getInt8Ty(), B.getInt16Ty()}) {
      auto *FT = llvm::FunctionType::get(B.getInt64Ty(),
                                         {B.getInt64Ty(), Narrow}, false);
      auto *F =
          llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage,
                                 Narrow->isIntegerTy(8) ? "f8" : "f16", M);
      F->addParamAttr(1, llvm::Attribute::SExt);
      B.SetInsertPoint(llvm::BasicBlock::Create(Ctx, "entry", F));
      B.CreateRet(B.CreateXor(F->getArg(0),
                              B.CreateSExt(F->getArg(1), B.getInt64Ty())));
    }
    std::string Asm;
    llvm::raw_string_ostream OS(Asm);
    EXPECT_FALSE(static_cast<bool>(emitAssembly(M, **TM, OS)));
    return Asm;
  };
  /// Does each function's code contain What (a sign extension)?
  auto Each = [](const std::string &Asm, const char *What) {
    for (const char *Fn : {"\nf8:", "\nf16:"}) {
      size_t B = Asm.find(Fn);
      if (B == std::string::npos ||
          Asm.substr(B, Asm.find(".Lfunc_end", B) - B).find(What) ==
              std::string::npos)
        return false;
    }
    return true;
  };
  // arm64: `opt 0` (GlobalISel: sxtb, sxth) and `opt 2` (SelectionDAG: sxtw
  // of the 32 bits that `signext` extended) sign-extend.
  for (const Setting *S : {&AllSettings[0], &AllSettings[2]}) {
    std::string Asm = Build("linux/arm64", specOf(*S, "linux/arm64"));
    EXPECT_TRUE(Each(Asm, "sxt")) << S->Name << Asm;
  }
  // FastISel, as the hidden -isel=fastisel asks for it, does not.
  PipelineRequest R;
  R.Selector = "fastisel";
  std::string Fast =
      Build("linux/arm64", *selectPipeline(R, *parseTarget("linux/arm64")));
  EXPECT_EQ(Fast.find("sxt"), std::string::npos)
      << "LLVM fixed AArch64 FastISel: revisit ADR-0028's P04.13 addendum\n"
      << Fast;
  // x86-64 FastISel, which `opt 0` uses there, is right: it extends the low
  // byte and the low half itself.
  std::string X = Build("linux/amd64", specOf(AllSettings[0], "linux/amd64"));
  EXPECT_NE(X.find("movsbq"), std::string::npos) << X;
  EXPECT_NE(X.find("movswq"), std::string::npos) << X;
}

TEST(Pipeline, Arm64FnegOfFmaIsFnmadd) {
  // LLVM 23.1.0: the AArch64 selectors (GlobalISel at O0, SelectionDAG at O2)
  // select `fneg (llvm.fma a, b, c)` as FNMADD, -(a*b) - c, which is +0 where
  // a*b + c is exactly 0 and -(+0) is -0 (review R8; FastISel is right). The
  // pre-ISel FenceNegatedFmaPass, in every arm64 spec, keeps them apart. If
  // the "LLVM still" lines start to fail, LLVM was fixed: the pass can go
  // (ADR-0028's P04.13 addendum, pipelines.md section 4); do not delete the
  // test.
  auto Build = [](const PipelineSpec &Spec, bool RunHooks) {
    auto T = *parseTarget("linux/arm64");
    auto TM = createTargetMachine(T, Spec);
    EXPECT_TRUE(static_cast<bool>(TM));
    llvm::LLVMContext Ctx;
    llvm::Module M("m", Ctx);
    M.setTargetTriple((*TM)->getTargetTriple());
    M.setDataLayout((*TM)->createDataLayout());
    llvm::IRBuilder<> B(Ctx);
    for (int K = 0; K < 2; ++K) {
      llvm::Type *D = B.getDoubleTy();
      auto *FT = llvm::FunctionType::get(D, {D, D, D}, false);
      auto *F = llvm::Function::Create(FT, llvm::GlobalValue::ExternalLinkage,
                                       K ? "viaMul" : "viaNeg", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(Ctx, "entry", F));
      llvm::Value *R =
          B.CreateIntrinsic(llvm::Intrinsic::fma, {D},
                            {F->getArg(0), F->getArg(1), F->getArg(2)});
      B.CreateRet(K ? B.CreateFMul(R, llvm::ConstantFP::get(D, -1.0))
                    : B.CreateFNeg(R));
    }
    PipelineSpec Plain = Spec;
    if (!RunHooks)
      Plain.PreISel.clear();
    EXPECT_FALSE(static_cast<bool>(runIRPasses(M, **TM, Plain)));
    std::string Asm;
    llvm::raw_string_ostream OS(Asm);
    EXPECT_FALSE(static_cast<bool>(emitAssembly(M, **TM, OS)));
    return Asm;
  };
  auto Count = [](const std::string &Asm, const char *What) {
    size_t N = 0;
    for (size_t At = Asm.find(What); At != std::string::npos;
         At = Asm.find(What, At + 1))
      ++N;
    return N;
  };
  for (const Setting *S : {&AllSettings[0], &AllSettings[2]}) {
    PipelineSpec Spec = specOf(*S, "linux/arm64");
    ASSERT_FALSE(Spec.PreISel.empty()) << S->Name;
    // LLVM still selects both negations of the fma as FNMADD...
    std::string Raw = Build(Spec, false);
    EXPECT_EQ(Count(Raw, "fnmadd"), 2u)
        << S->Name << ": LLVM no longer selects fneg(fma) as FNMADD; "
        << "FenceNegatedFmaPass may go (ADR-0028 P04.13 addendum)\n"
        << Raw;
    // ...and with the spec's pass, each is an FMADD and a negation.
    std::string Fenced = Build(Spec, true);
    EXPECT_EQ(Count(Fenced, "fnmadd"), 0u) << S->Name << Fenced;
    EXPECT_EQ(Count(Fenced, "fmadd"), 2u) << S->Name << Fenced;
  }
  // amd64 never fuses, so its specs have no such pass.
  EXPECT_TRUE(specOf(AllSettings[0], "linux/amd64").PreISel.empty());
}

} // namespace
