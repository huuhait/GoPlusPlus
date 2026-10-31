//===- Runner.cpp - Compile and run a batch -------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/TestRun/Runner.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <mutex>
#include <optional>
#include <thread>
#include <unistd.h>

using namespace gopp::testrun;

namespace {

/// Removes the temporary work directory when the runner is stopped by SIGTERM,
/// SIGINT or SIGHUP (a lit run that is cancelled, a time-out of `timeout`),
/// then ends with the status a shell reports for that signal. The signals are
/// blocked in the calling thread (so in every thread started after it) and one
/// thread waits for them. SIGKILL cannot be caught: the callers that need
/// that to leave nothing give their own `-work-dir` and remove it.
class CleanupOnSignal {
  sigset_t Set, Old;
  std::string Dir;
  std::atomic<bool> Done{false};
  std::thread Waiter;

public:
  explicit CleanupOnSignal(std::string D) : Dir(std::move(D)) {
    sigemptyset(&Set);
    for (int Sig : {SIGTERM, SIGINT, SIGHUP})
      sigaddset(&Set, Sig);
    pthread_sigmask(SIG_BLOCK, &Set, &Old);
    Waiter = std::thread([this] {
      while (!Done) {
        timespec Tick = {0, 50 * 1000 * 1000};
        int Sig = sigtimedwait(&Set, nullptr, &Tick);
        if (Sig > 0) {
          // The pool's threads may still make a file here (the stdout and
          // stderr files of a program that starts just now), and a file made
          // between listing and removing the directory keeps it. The
          // directory is gone for good once a pass removes it, since nothing
          // can be made in a missing directory; so remove it until it is.
          for (unsigned Try = 0; Try < 1000 && llvm::sys::fs::exists(Dir);
               ++Try)
            llvm::sys::fs::remove_directories(Dir);
          _exit(128 + Sig);
        }
      }
    });
  }
  ~CleanupOnSignal() {
    Done = true;
    Waiter.join();
    pthread_sigmask(SIG_SETMASK, &Old, nullptr);
  }
};

} // namespace

llvm::Expected<std::vector<JobSpec>>
gopp::testrun::readManifest(llvm::StringRef Path) {
  auto Buf = llvm::MemoryBuffer::getFileOrSTDIN(Path);
  if (!Buf)
    return llvm::createStringError(Buf.getError(), "%s: %s", Path.str().c_str(),
                                   Buf.getError().message().c_str());
  std::vector<JobSpec> Jobs;
  llvm::StringSet<> Seen;
  llvm::StringRef Text = (*Buf)->getBuffer();
  unsigned N = 0;
  while (!Text.empty()) {
    auto [Line, Rest] = Text.split('\n');
    Text = Rest;
    ++N;
    if (Line.trim().empty())
      continue;
    llvm::Expected<JobSpec> J = decodeJobSpec(Line);
    if (!J)
      return llvm::createStringError(std::errc::invalid_argument, "%s:%u: %s",
                                     Path.str().c_str(), N,
                                     toString(J.takeError()).c_str());
    if (!Seen.insert(J->Id).second)
      return llvm::createStringError(std::errc::invalid_argument,
                                     "%s:%u: duplicate job id '%s'",
                                     Path.str().c_str(), N, J->Id.c_str());
    Jobs.push_back(std::move(*J));
  }
  return Jobs;
}

llvm::Error gopp::testrun::runBatch(llvm::ArrayRef<JobSpec> Jobs,
                                    RunnerConfig Cfg,
                                    llvm::function_ref<void(JobResult &&)> Sink,
                                    RunStats *Stats) {
  bool Temp = Cfg.WorkDir.empty();
  if (Temp) {
    llvm::SmallString<128> Dir;
    if (std::error_code EC =
            llvm::sys::fs::createUniqueDirectory("gopp-test-run", Dir))
      return llvm::createStringError(EC, "cannot make a work directory: %s",
                                     EC.message().c_str());
    Cfg.WorkDir = std::string(Dir);
  } else if (std::error_code EC =
                 llvm::sys::fs::create_directories(Cfg.WorkDir)) {
    return llvm::createStringError(EC, "%s: %s", Cfg.WorkDir.c_str(),
                                   EC.message().c_str());
  }
  Cfg.Compile.WorkDir = Cfg.WorkDir;
  std::optional<CleanupOnSignal> Cleanup;
  if (Temp)
    Cleanup.emplace(Cfg.WorkDir);

  std::mutex SinkMu;
  auto Report = [&](JobResult &&R) {
    std::lock_guard<std::mutex> L(SinkMu);
    Sink(std::move(R));
  };
  {
    PoolConfig PC;
    PC.Self = Cfg.Self;
    PC.Size =
        std::min<size_t>(Cfg.Executors ? Cfg.Executors : defaultExecutors(),
                         std::max<size_t>(Jobs.size(), 1));
    PC.WorkDir = Cfg.WorkDir;
    PC.DefaultTimeoutMs = Cfg.TimeoutMs;
    PC.StartupTimeoutMs = Cfg.StartupTimeoutMs;
    PC.MaxOutputBytes = Cfg.MaxOutputBytes;
    PC.ShareExecutors = Cfg.ShareExecutors;
    // The executors start now, while the first programs compile.
    ExecutorPool Pool(PC);
    compileBatch(Jobs, Cfg.Compile, Cfg.CompileThreads,
                 [&](size_t I, CompiledProgram &&C) {
                   const JobSpec &J = Jobs[I];
                   if (!C.Ok) {
                     JobResult R;
                     R.Id = J.Id;
                     R.O = Outcome::CompileFailed;
                     R.Message = std::move(C.Diagnostics);
                     R.CompileMs = C.Ms;
                     Report(std::move(R));
                     return;
                   }
                   PoolJob P;
                   P.Id = J.Id;
                   P.ObjectPath = std::move(C.ObjectPath);
                   P.TimeoutMs = J.TimeoutMs;
                   P.MergeOutput = J.MergeOutput;
                   double Ms = C.Ms;
                   P.Done = [&Report, Ms](JobResult &&R) {
                     R.CompileMs = Ms;
                     Report(std::move(R));
                   };
                   Pool.submit(std::move(P));
                 });
    Pool.stop();
    if (Stats)
      Stats->ProcessesStarted = Pool.processesStarted();
  }
  if (Temp && !Cfg.KeepWorkDir)
    llvm::sys::fs::remove_directories(Cfg.WorkDir);
  return llvm::Error::success();
}
