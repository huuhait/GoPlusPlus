//===- gopp/TestRun/Executor.h - The executor process ----------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.19 (D4.14). The program side of the fast test runner: `gopp test-exec`.
// An executor is a process with an ORC JIT (LLJIT) in it. It loads the object
// files it is asked for, one at a time, links them in memory against the
// symbols of its own process (libc: `write`, `exit`, ...), calls `main` on its
// main thread and reports what main returned. A program that crashes, calls
// `exit` or panics (exit 2) ends the executor; the pool (ExecutorPool.h) sees
// that and starts another one.
//
// A program that returns from main leaves the executor for the next program,
// so the executor puts back what a program can change in its process before
// it answers (docs/design/test-runner.md, "One process, many programs"):
// standard input, output and error, the descriptors the program opened, the
// signal dispositions, mask and pending signals, the interval timers, the
// umask, the working directory, the environment and the resource limits. What
// it cannot put back makes it ask the pool for a new process.
//
// Why a process and not a thread: the program may do anything a program can
// do. The same JIT code in another process is what LLVM's own
// llvm-jitlink-executor and ORC's ExecutorProcessControl are for; here the
// compile happens in the driver and only the finished object travels, so the
// executor needs no more than LLJIT and a socket.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_TESTRUN_EXECUTOR_H
#define GOPP_TESTRUN_EXECUTOR_H

namespace gopp::testrun {

/// The descriptor the pool hands the executor its end of the socket on. The
/// executor moves the socket to a high number first thing, so descriptor 3
/// is free for the program, as in a binary.
inline constexpr int ExecutorControlFd = 3;

/// The main loop of an executor: reads requests from Fd, answers on it, until
/// "quit" or the end of the stream, then ends the process with `_exit` (the
/// programs' code is gone, so nothing they registered may run at exit). Call
/// it before anything else in the process has installed signal handlers or
/// started threads (it resets the handlers to their defaults, so a crash in
/// the program kills the process the way it would kill a real binary).
[[noreturn]] void runExecutor(int Fd);

} // namespace gopp::testrun

#endif // GOPP_TESTRUN_EXECUTOR_H
