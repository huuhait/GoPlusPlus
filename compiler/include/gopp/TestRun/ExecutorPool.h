//===- gopp/TestRun/ExecutorPool.h - A pool of executor processes -*- C++ -*-=//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.19 (D4.14). Part two of the fast test runner: N executor processes
// (Executor.h), each running one program at a time. The pool gives every
// finished object file to a free executor and turns what happens into a
// JobResult:
//
//   main returned          Exited, the status main returned
//   the process left       Exited with its status (`exit(7)`, a panic's 2)
//   the process was killed Signaled (SIGSEGV from `unsafe`, `abort`, SIGXFSZ
//                          past the output limit)
//   no answer in time      TimedOut (the pool kills it)
//   the object won't link  LoadFailed (the missing symbols, sorted)
//   no executor starts     Internal (the same text for every such job)
//
// After the process of an executor ends, the next job starts a new one, so a
// crash costs one program, never the run. A program that returns leaves its
// executor to the next one; the executor puts back the process state the
// program changed (Executor.h), and asks for a new process when it cannot.
// The pool knows nothing about the compiler: it takes object files.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_TESTRUN_EXECUTORPOOL_H
#define GOPP_TESTRUN_EXECUTORPOOL_H

#include "gopp/TestRun/Protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gopp::testrun {

/// Whether a program may leave state in its process that the next program
/// would see: false only when every symbol the object at Path leaves
/// undefined (what it takes from the C library) is on a short allowlist of
/// functions without hidden state (`write`, `exit`, `abort`, `mem*`,
/// `strlen`, `malloc` and `free`, the libm functions). Anything else (`rand`,
/// `strtok`, `localtime`, `setlocale`, `getopt`, `pthread_create`, a data
/// symbol such as `stdout`, ...) may keep C library state, and an object that
/// cannot be read is treated the same way. The pool gives such a program an
/// executor of its own: it ends with the program. The rule is in
/// docs/design/test-runner.md.
bool programKeepsState(const std::string &Path);

/// The pool size when none is asked for: a quarter of the cores, between 2 and
/// 16. A program runs for milliseconds, and every executor costs a process
/// start (about 25 ms of CPU: the shared LLVM library is mapped and the JIT
/// made), so more executors than the work needs lose time.
unsigned defaultExecutors();

struct PoolConfig {
  /// The program that runs as `<Self> test-exec` (the running `gopp`).
  std::string Self;
  /// Executor processes; 0 = defaultExecutors().
  unsigned Size = 0;
  /// A directory for the output files of the runs (must exist).
  std::string WorkDir;
  /// Time limit of a run, when the job gives none.
  unsigned DefaultTimeoutMs = 60000;
  /// An executor that has not said "ready" after this long did not start.
  unsigned StartupTimeoutMs = 20000;
  /// After this many failed starts in a row no executor is started any more:
  /// every job that needs one gets the first failure's text at once.
  unsigned MaxStartFailures = 3;
  /// The most bytes a program may write to one output file; past it the
  /// program gets SIGXFSZ (a program that prints forever must not fill the
  /// disk, and the pool reads each file into memory). 0 = no limit.
  uint64_t MaxOutputBytes = 256ull << 20;
  /// An executor is replaced after this many programs: the C library state
  /// and the memory that leaking programs left do not pile up.
  unsigned MaxJobsPerExecutor = 64;
  /// TEST ONLY: also share an executor between programs that keep state
  /// (programKeepsState), to test what the executor puts back.
  bool ShareExecutors = false;
};

struct PoolJob {
  std::string Id;
  std::string ObjectPath;
  unsigned TimeoutMs = 0; // 0 = the pool's default
  /// Both streams in one file, in the order they were written (Stdout).
  bool MergeOutput = false;
  /// Called from a pool thread with the program's result (O, ExitCode,
  /// Signal, Stdout, Stderr, Message, RunMs, ExecutorRestarted are set).
  std::function<void(JobResult &&)> Done;
};

class ExecutorPool {
public:
  explicit ExecutorPool(PoolConfig Cfg);
  /// Waits for the queued jobs, then ends the executors.
  ~ExecutorPool();
  ExecutorPool(const ExecutorPool &) = delete;
  ExecutorPool &operator=(const ExecutorPool &) = delete;

  void submit(PoolJob J);
  /// Returns when every submitted job has been reported.
  void wait();
  /// wait(), then ends the executors and the pool's threads. The destructor
  /// does it if nobody did; after it, processesStarted() is final.
  void stop();
  unsigned size() const { return unsigned(Threads.size()); }
  /// How many executor processes were started (the first ones, and the
  /// restarts).
  unsigned processesStarted() const;

  /// Failed executor starts, shared by the workers (see MaxStartFailures).
  class StartGate {
    std::mutex Mu;
    unsigned InARow = 0;
    std::string First;

  public:
    /// True (with the first failure's text in Why) once starts have failed
    /// Max times in a row.
    bool closed(unsigned Max, std::string &Why);
    /// Records a start; a failure's Why becomes the first failure's text.
    void record(bool Ok, std::string &Why);
  };

private:
  class Worker;
  void loop();

  PoolConfig Cfg;
  StartGate Gate;
  std::mutex Mu;
  std::condition_variable HaveWork, Idle;
  std::deque<PoolJob> Queue;
  unsigned Active = 0;
  std::atomic<unsigned> Started{0};
  std::atomic<unsigned> Seq{0};
  bool Stopping = false;
  std::vector<std::thread> Threads;
};

} // namespace gopp::testrun

#endif // GOPP_TESTRUN_EXECUTORPOOL_H
