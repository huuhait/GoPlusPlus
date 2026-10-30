//===- gopp/TestRun/Protocol.h - The result protocol ------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.19 (D4.14). The fast test runner has three parts that know each other
// only through this header (docs/design/test-runner.md):
//
//   batch compile driver (BatchCompile.h)   JobSpec  -> an object file
//   executor pool        (ExecutorPool.h)   object   -> RunResult
//   result protocol      (this file)        what is said between them, and
//                                           what lit and `gopp test` read
//
// Every message is one line of JSON ("JSON lines") with a version field "v".
// Bytes that are not text (a program's output) travel in base64, so the
// line is valid JSON whatever the program wrote.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_TESTRUN_PROTOCOL_H
#define GOPP_TESTRUN_PROTOCOL_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gopp::testrun {

/// Bumped when a field changes meaning. A reader refuses other versions.
inline constexpr int ProtocolVersion = 1;

/// How a program ended. The first three are the program's own result; the
/// others mean the runner could not give one.
enum class Outcome {
  Exited,        // exit status Code (a return from main, or `exit`)
  Signaled,      // killed by signal Signal (SIGSEGV, SIGABRT, ...)
  TimedOut,      // killed by the runner after the time limit
  CompileFailed, // the program does not compile; Message has the diagnostics
  LoadFailed,    // the object could not be loaded or linked in the JIT
  Internal,      // the runner itself failed (an executor would not start)
};

const char *outcomeName(Outcome O);
bool parseOutcome(llvm::StringRef Name, Outcome &O);

/// One program to compile and run (input of `gopp test-run`).
struct JobSpec {
  /// Names the job in the results; unique in a run.
  std::string Id;
  /// Source files, or one directory: what `gopp build` takes.
  std::vector<std::string> Inputs;
  /// Only the host target runs in the JIT.
  std::string Target = "host";
  /// Time limit of the run in ms; 0 = the runner's default.
  uint32_t TimeoutMs = 0;
  /// Standard output and standard error go to one file, in the order the
  /// program wrote them (a shell's `2>&1`); the result has both in Stdout.
  bool MergeOutput = false;
};

/// The result of one job (output of `gopp test-run`, one line each).
struct JobResult {
  std::string Id;
  Outcome O = Outcome::Internal;
  int ExitCode = 0; // Exited
  int Signal = 0;   // Signaled
  /// Captured exactly as written: standard output and standard error.
  std::string Stdout, Stderr;
  /// Diagnostics (CompileFailed), the JIT's error (LoadFailed), or what went
  /// wrong (Internal). Empty otherwise.
  std::string Message;
  /// Milliseconds spent compiling and running.
  double CompileMs = 0, RunMs = 0;
  /// The executor process ended with this program: it crashed, called
  /// `exit`, ran out of time, or changed process state the executor cannot
  /// put back (ExecutorPool.h). The next program gets a new process.
  bool ExecutorRestarted = false;

  bool exitedWith(int Code) const {
    return O == Outcome::Exited && ExitCode == Code;
  }
};

std::string encodeJobSpec(const JobSpec &J);
llvm::Expected<JobSpec> decodeJobSpec(llvm::StringRef Line);
std::string encodeJobResult(const JobResult &R);
llvm::Expected<JobResult> decodeJobResult(llvm::StringRef Line);

//===----------------------------------------------------------------------===//
// Between the pool and one executor process (a socket, JSON lines again)
//===----------------------------------------------------------------------===//

/// Pool -> executor: run this object file. The executor opens the output
/// files itself and writes the program's standard output and standard error
/// to them; the pool reads them afterwards.
struct RunRequest {
  std::string Id;
  std::string ObjectPath;
  /// StderrPath is empty when both streams go to StdoutPath (MergeOutput).
  std::string StdoutPath, StderrPath;
  /// The most bytes the program may write to one file (RLIMIT_FSIZE; it gets
  /// SIGXFSZ past it); 0 = no limit.
  uint64_t MaxOutputBytes = 0;
};

/// Executor -> pool, after main returned. A process that dies, or leaves
/// through `exit`, sends nothing: the pool sees the end of the socket and
/// asks the operating system how the process ended.
struct RunReply {
  std::string Id;
  bool Loaded = true; // false: Error says why the object did not load
  int ExitCode = 0;   // what main returned
  std::string Error;
  /// The program changed process state the executor could not put back (a
  /// lowered hard resource limit, a working directory that is gone): the pool
  /// replaces this executor before the next program.
  bool Recycle = false;
};

std::string encodeRunRequest(const RunRequest &R);
llvm::Expected<RunRequest> decodeRunRequest(llvm::StringRef Line);
std::string encodeRunReply(const RunReply &R);
llvm::Expected<RunReply> decodeRunReply(llvm::StringRef Line);
/// The first line an executor sends, when it can run programs.
std::string encodeReady();
bool isReady(llvm::StringRef Line);
/// The line that makes an executor leave cleanly.
std::string encodeQuit();
bool isQuit(llvm::StringRef Line);

} // namespace gopp::testrun

#endif // GOPP_TESTRUN_PROTOCOL_H
