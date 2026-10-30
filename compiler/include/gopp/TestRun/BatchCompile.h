//===- gopp/TestRun/BatchCompile.h - The batch compile driver ---*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.19 (D4.14). Part one of the fast test runner: many programs compiled
// by ONE process on many threads. A compile is the same pipeline as
// `gopp build -emit=obj` (load, check, GIR, LLVM, object) with no command
// line state, so any number can run at once: every compile owns its source
// manager, diagnostics, type context and LLVM context. The only shared things
// are read-only (the target table).
//
// The driver knows nothing about running programs: it produces object files
// and reports them. Later users (`gopp test`, the differential corpus, the
// build cache of P06) call it the same way, and P06 puts the cached runtime
// and std in front of it: a cache hit never reaches this code.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_TESTRUN_BATCHCOMPILE_H
#define GOPP_TESTRUN_BATCHCOMPILE_H

#include "gopp/TestRun/Protocol.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <string>

namespace gopp::testrun {

struct CompileOptions {
  /// Where object files are written (must exist): `<Serial>.o`.
  std::string WorkDir;
  /// DWARF line tables. Off: a test run needs none and they cost time.
  bool DebugInfo = false;
  /// TEST ONLY: standard packages from this directory (`gopp build
  /// -build-test-stubs`).
  std::string StdRoot;
};

struct CompiledProgram {
  bool Ok = false;
  /// The object file (Ok).
  std::string ObjectPath;
  /// The diagnostics, or why the target is not supported (not Ok).
  std::string Diagnostics;
  double Ms = 0;
};

/// Compiles one job for the host. Thread safe; Serial must differ between
/// calls that run at once (it names the object file).
CompiledProgram compileProgram(const JobSpec &Job, const CompileOptions &Opts,
                               unsigned Serial);

/// Compiles every job on Threads threads (0 = one per core). OnDone is called
/// from a worker thread as each compile ends, with the job's index; calls
/// may overlap, so it must be thread safe. Returns when all are done.
void compileBatch(llvm::ArrayRef<JobSpec> Jobs, const CompileOptions &Opts,
                  unsigned Threads,
                  llvm::function_ref<void(size_t, CompiledProgram &&)> OnDone);

} // namespace gopp::testrun

#endif // GOPP_TESTRUN_BATCHCOMPILE_H
