//===- gopp/CodeGen/Pipeline.h - Codegen pipelines per `opt` ----*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.13, D4.8, P06 D6.6. One place decides what a build does between GIR and
// the object file: `selectPipeline` turns the settings (`opt`, `lto`,
// `-pipeline=fastisel`) into a PipelineSpec, and `runGIRPasses` and
// `runIRPasses` run the stages the spec names. docs/design/pipelines.md says
// how profiles (P06 D6.12), `-trace` (D6.7), LTO (P15) and the split pass of
// ADR-0028 (P04.07) plug into it.
//
// The stages, in order:
//
//   GIR passes     trivial inlining (Inline.h), when the spec asks      (GIR)
//   lowerToLLVM    GIR -> llvm::Module (CodeGen.h); FMA fusion is decided here
//   IR passes      the curated opt-0 set, or LLVM's -O1/-O2/-O3/-Os/-Oz
//   pre-ISel hooks passes that run right before instruction selection
//   code generation  TargetMachine with the spec's optimization level and
//                    selector (emitObject / emitAssembly, Emit.cpp)
//
// The results of a program do not depend on the spec (the rule of
// docs/design/pipelines.md section 1): no stage may change what Go defines.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_CODEGEN_PIPELINE_H
#define GOPP_CODEGEN_PIPELINE_H

#include "gopp/CodeGen/Target.h"
#include "gopp/GIR/GIR.h"
#include "gopp/GIR/Inline.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TimeProfiler.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
class Module;
class TargetMachine;
} // namespace llvm

namespace gopp {

enum class OptLevel : uint8_t { O0, O1, O2, O3, Os, Oz };

/// The instruction selector of the back end. `Default` is LLVM's choice for
/// the target and optimization level.
enum class Selector : uint8_t { Default, FastISel, GlobalISel };

/// Link-time optimization (`lto thin` and `lto full`, P15). Only `None` is
/// built; the field is where P15 hooks in (selectPipeline refuses the rest).
enum class LTOMode : uint8_t { None, Thin, Full };

/// What runs on the LLVM module before code generation.
enum class IRPasses : uint8_t {
  None,     // nothing: the plain -O0 baseline
  Curated,  // the small opt-0 set (D6.6)
  Standard, // LLVM's per-module pipeline of Opt
};

/// What the user asked for. A build profile (P06 D6.12) fills it from its
/// settings; `gopp build -opt=N -pipeline=fastisel` fills it from the flags.
struct PipelineRequest {
  /// "0", "1", "2", "3", "s" or "z".
  std::string Opt = "0";
  /// `-pipeline=fastisel`: plain O0 and FastISel, no GIR or IR pass. The
  /// measuring baseline of D6.8 and the fallback of D6.9; not a profile
  /// setting.
  bool Baseline = false;
  /// "none", "thin" or "full".
  std::string LTO = "none";
  /// A measuring flag: "", "fastisel" or "globalisel" forces the selector.
  std::string Selector;
};

/// A pass that runs on the module right before code generation. P04.07
/// registers the ADR-0028 split pass here.
using PreISelHook = std::function<void(llvm::ModulePassManager &)>;

struct PipelineSpec {
  /// "opt0", "opt1", "opt2", "opt3", "opts", "optz" or "fastisel".
  std::string Name;
  OptLevel Opt = OptLevel::O0;
  bool Baseline = false;
  bool GIRInline = false;
  gir::InlineOptions Inline;
  IRPasses IR = IRPasses::None;
  /// The new-pass-manager text of the Curated set.
  std::string CuratedPasses;
  Selector Sel = Selector::Default;
  LTOMode LTO = LTOMode::None;
  /// Run in order, after the IR passes, before code generation.
  std::vector<PreISelHook> PreISel;

  /// The code generator's optimization level (TargetMachine).
  unsigned codeGenLevel() const;
};

/// The pipeline for Req on target T. An unknown `opt` value, `-pipeline=
/// fastisel` together with an `opt` other than 0, and `lto` other than none
/// (P15) are errors.
llvm::Expected<PipelineSpec> selectPipeline(const PipelineRequest &Req,
                                            const TargetSpec &T);

/// The pre-ISel pass every arm64 spec runs (docs/design/pipelines.md section
/// 4, ADR-0028 addendum). LLVM 23.1.0's AArch64 selectors (GlobalISel at O0,
/// SelectionDAG) select `fneg (llvm.fma a, b, c)` as FNMADD, which computes
/// `-(a*b) - c`: where `a*b + c` is exactly 0 that is +0, but Go's `-t` of
/// the fused t is -0 (FastISel and gc give -0). The pass puts an
/// `llvm.arithmetic.fence` between an `llvm.fma` and every negation of it
/// (`fneg`, and `fmul`/`fdiv` by -1.0, which the DAG combiner turns into
/// `fneg`), so no selector sees the pattern. GlobalISel cannot select the
/// fence and leaves such a function to SelectionDAG; the shape is rare.
struct FenceNegatedFmaPass : llvm::PassInfoMixin<FenceNegatedFmaPass> {
  llvm::PreservedAnalyses run(llvm::Module &M, llvm::ModuleAnalysisManager &);
};

/// The TargetMachine for T, with the optimization level and the instruction
/// selector of Spec (otherwise createTargetMachine, Target.h).
llvm::Expected<std::unique_ptr<llvm::TargetMachine>>
createTargetMachine(const TargetSpec &T, const PipelineSpec &Spec);

/// Wall time of the stages that ran, in order ("gir-inline", "ir-passes",
/// "pre-isel"; the driver adds "lower" and "codegen"). Every stage is also a
/// `llvm::TimeTraceScope` named "gopp.<stage>", so `-trace` (D6.7) shows them
/// next to the LLVM passes once it starts the time profiler.
struct PipelineTimes {
  struct Stage {
    std::string Name;
    double Seconds;    // wall time
    double CpuSeconds; // CPU time of the process: steady under machine load
  };
  std::vector<Stage> Stages;
};

/// Records a stage in Times (if not null) from construction to destruction,
/// and opens the trace scope `gopp.<Name>`.
class StageTimer {
public:
  StageTimer(const char *Name, PipelineTimes *Out);
  ~StageTimer();
  StageTimer(const StageTimer &) = delete;
  StageTimer &operator=(const StageTimer &) = delete;

private:
  const char *Name;
  PipelineTimes *Out;
  llvm::TimeTraceScope Scope;
  double Wall, Cpu;
};

/// Runs the GIR stage of Spec on M (before lowerToLLVM).
gir::InlineStats runGIRPasses(gir::Module &M, const PipelineSpec &Spec,
                              PipelineTimes *Times = nullptr);

/// Runs the IR stage of Spec on M, then its pre-ISel hooks.
llvm::Error runIRPasses(llvm::Module &M, llvm::TargetMachine &TM,
                        const PipelineSpec &Spec,
                        PipelineTimes *Times = nullptr);

} // namespace gopp

#endif // GOPP_CODEGEN_PIPELINE_H
