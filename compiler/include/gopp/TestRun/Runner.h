//===- gopp/TestRun/Runner.h - Compile, run, report -------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.19 (D4.14). The three parts joined: every compile that finishes hands
// its object to the executor pool at once, so compiling and running overlap.
// This is `gopp test-run`. Callers that need other wiring (P06's cache that
// skips the compile of an unchanged program; `gopp test`, which compiles a
// package once and runs many tests from it) use BatchCompile.h and
// ExecutorPool.h directly.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_TESTRUN_RUNNER_H
#define GOPP_TESTRUN_RUNNER_H

#include "gopp/TestRun/BatchCompile.h"
#include "gopp/TestRun/ExecutorPool.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

namespace gopp::testrun {

struct RunnerConfig {
  /// The `gopp` to start executors from.
  std::string Self;
  /// 0 = one per core.
  unsigned CompileThreads = 0;
  /// 0 = defaultExecutors(), and never more than there are programs.
  unsigned Executors = 0;
  /// Run time limit when a job gives none.
  unsigned TimeoutMs = 60000;
  /// See PoolConfig.
  unsigned StartupTimeoutMs = 20000;
  uint64_t MaxOutputBytes = 256ull << 20;
  /// TEST ONLY, see PoolConfig.
  bool ShareExecutors = false;
  /// Objects and output files; empty = a new temporary directory, removed at
  /// the end.
  std::string WorkDir;
  bool KeepWorkDir = false;
  CompileOptions Compile;
};

/// Reads jobs, one JobSpec JSON line each ("-" = standard input). Blank lines
/// are skipped. Fails on a bad line or a duplicate id.
llvm::Expected<std::vector<JobSpec>> readManifest(llvm::StringRef Path);

/// What a run cost, for `-summary` and the tests.
struct RunStats {
  /// Executor processes started: the first ones, and one for every program
  /// that ended its executor and was followed by another job.
  unsigned ProcessesStarted = 0;
};

/// Compiles and runs every job. Sink gets each result as soon as it exists,
/// one call at a time (no two overlap), in completion order. Returns when
/// every job has a result.
llvm::Error runBatch(llvm::ArrayRef<JobSpec> Jobs, RunnerConfig Cfg,
                     llvm::function_ref<void(JobResult &&)> Sink,
                     RunStats *Stats = nullptr);

} // namespace gopp::testrun

#endif // GOPP_TESTRUN_RUNNER_H
