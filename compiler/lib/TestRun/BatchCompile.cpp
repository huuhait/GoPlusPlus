//===- BatchCompile.cpp - Many compiles in one process --------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/TestRun/BatchCompile.h"

#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/ExampleMocks.h"
#include "gopp/Sema/Loader.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <chrono>

using namespace gopp;
using namespace gopp::testrun;

namespace {
class Stopwatch {
  std::chrono::steady_clock::time_point T0 = std::chrono::steady_clock::now();

public:
  double ms() const {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - T0)
        .count();
  }
};
} // namespace

static CompiledProgram fail(const Stopwatch &W, std::string Why) {
  CompiledProgram C;
  C.Diagnostics = std::move(Why);
  C.Ms = W.ms();
  return C;
}

CompiledProgram gopp::testrun::compileProgram(const JobSpec &Job,
                                              const CompileOptions &Opts,
                                              unsigned Serial) {
  Stopwatch W;
  if (Job.Inputs.empty())
    return fail(W, "no input files\n");
  llvm::Expected<TargetSpec> T = parseTarget(Job.Target);
  llvm::Expected<TargetSpec> Host = hostTarget();
  if (!T || !Host) {
    llvm::Error E = T ? Host.takeError() : T.takeError();
    return fail(W, toString(std::move(E)) + "\n");
  }
  if (T->name() != Host->name())
    return fail(W, "the fast runner runs programs of the host target (" +
                       Host->name() + ") only; " + T->name() +
                       " needs a real binary\n");

  // The same steps, in the same order, as `gopp build` (gopp.cpp runBuild).
  std::string Text;
  llvm::raw_string_ostream DiagOS(Text);
  SourceManager SM;
  TextDiagnosticPrinter Printer(DiagOS, SM);
  DiagnosticsEngine Diags(SM, Printer, DiagnosticsEngine::DefaultErrorLimit);
  sema::LoadOptions LO;
  LO.StdRoot = Opts.StdRoot;
  sema::CheckOptions CO;
  sema::TypeContext TCtx(false);
  sema::MockForeignProvider Foreign;
  bool IsDir =
      Job.Inputs.size() == 1 && llvm::sys::fs::is_directory(Job.Inputs[0]);
  if (IsDir) {
    LO.Root = Job.Inputs[0];
  } else {
    LO.Root = llvm::sys::path::parent_path(Job.Inputs[0]).str();
    if (LO.Root.empty())
      LO.Root = ".";
  }
  sema::Loader L(SM, Diags, TCtx, LO, CO);
  L.setForeignProvider(&Foreign);
  sema::CheckedPackage *CP = IsDir ? L.loadDir(Job.Inputs[0], "")
                                   : L.loadFiles(std::vector<std::string>(
                                         Job.Inputs.begin(), Job.Inputs.end()));
  if (!CP || Diags.hasErrors()) {
    Diags.finish();
    return fail(W, Text.empty() ? "the package does not load\n" : Text);
  }
  if (CP->Pkg && CP->Pkg->Name != "main") {
    Diags.finish();
    return fail(W, Text + "package " + CP->Pkg->Name.str() +
                       " is not package main, so it is no program\n");
  }
  gir::Context GCtx;
  std::unique_ptr<gir::Module> M =
      gir::lowerPackage(GCtx, SM, *CP, L.filesOf(CP), Diags);
  Diags.finish();
  if (!M)
    return fail(W, Text.empty() ? "lowering to GIR failed\n" : Text);

  llvm::Expected<std::unique_ptr<llvm::TargetMachine>> TM =
      createTargetMachine(*T);
  if (!TM)
    return fail(W, toString(TM.takeError()) + "\n");
  llvm::LLVMContext LCtx;
  LowerToLLVMOptions LLO;
  LLO.DebugInfo = Opts.DebugInfo;
  llvm::Expected<std::unique_ptr<llvm::Module>> LM =
      lowerToLLVM(LCtx, *M, **TM, LLO);
  if (!LM)
    return fail(W, toString(LM.takeError()) + "\n");
  CompiledProgram C;
  C.ObjectPath =
      (llvm::Twine(Opts.WorkDir) + "/" + llvm::Twine(Serial) + ".o").str();
  if (llvm::Error E = emitObject(**LM, **TM, C.ObjectPath))
    return fail(W, toString(std::move(E)) + "\n");
  C.Ok = true;
  C.Ms = W.ms();
  return C;
}

void gopp::testrun::compileBatch(
    llvm::ArrayRef<JobSpec> Jobs, const CompileOptions &Opts, unsigned Threads,
    llvm::function_ref<void(size_t, CompiledProgram &&)> OnDone) {
  // The registry of targets is filled once, before any thread asks for it.
  initializeTargets();
  llvm::DefaultThreadPool Pool(llvm::hardware_concurrency(Threads));
  for (size_t I = 0; I < Jobs.size(); ++I)
    Pool.async(
        [&, I] { OnDone(I, compileProgram(Jobs[I], Opts, unsigned(I))); });
  Pool.wait();
}
