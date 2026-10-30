//===- ExecutorPool.cpp - Executor processes and their supervision --------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/TestRun/ExecutorPool.h"

#include "Lines.h"
#include "gopp/TestRun/Executor.h"

#include "llvm/ADT/StringSet.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Threading.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>

extern char **environ;

using namespace gopp::testrun;
using Clock = std::chrono::steady_clock;

namespace {

double msSince(Clock::time_point T0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - T0).count();
}

std::string readFile(const std::string &Path) {
  auto B = llvm::MemoryBuffer::getFile(Path);
  if (!B)
    return std::string();
  return (*B)->getBuffer().str();
}

/// The C library functions a program may call and still share its executor
/// with the next program: they keep no state of the process. The run corpus's
/// `nm -u` gives write, exit, abort, memmove and memset; the rest is what the
/// runtime and plain C code can reasonably need. Each libm function is also
/// allowed with the `f` suffix.
bool stateFreeSymbol(llvm::StringRef Name) {
  static const llvm::StringSet<> Allowed = [] {
    llvm::StringSet<> S;
    for (const char *N :
         {"write", "exit", "_exit", "abort", "memcpy", "memmove", "memset",
          "memcmp", "bcmp", "strlen", "malloc", "calloc", "realloc", "free"})
      S.insert(N);
    // libm. (`errno` is set by some of them; the executor clears it.)
    for (const char *N :
         {"sin",    "cos",     "tan",       "asin",   "acos",  "atan",
          "atan2",  "sinh",    "cosh",      "tanh",   "asinh", "acosh",
          "atanh",  "exp",     "exp2",      "expm1",  "log",   "log2",
          "log10",  "log1p",   "pow",       "sqrt",   "cbrt",  "hypot",
          "floor",  "ceil",    "trunc",     "round",  "rint",  "nearbyint",
          "fabs",   "fmod",    "remainder", "fma",    "fmin",  "fmax",
          "fdim",   "ldexp",   "scalbn",    "frexp",  "modf",  "copysign",
          "lround", "llround", "lrint",     "llrint", "erf",   "erfc",
          "tgamma"}) { // not lgamma: it writes the global `signgam`
      S.insert(N);
      S.insert(std::string(N) + "f");
    }
    return S;
  }();
  return Allowed.contains(Name);
}

} // namespace

bool gopp::testrun::programKeepsState(const std::string &Path) {
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  if (!Buf)
    return true;
  auto Obj =
      llvm::object::ObjectFile::createObjectFile((*Buf)->getMemBufferRef());
  if (!Obj) {
    llvm::consumeError(Obj.takeError());
    return true;
  }
  for (const llvm::object::SymbolRef &Sym : (*Obj)->symbols()) {
    auto Flags = Sym.getFlags();
    if (!Flags || !(*Flags & llvm::object::SymbolRef::SF_Undefined))
      continue;
    auto Name = Sym.getName();
    if (!Name) {
      llvm::consumeError(Name.takeError());
      return true;
    }
    if (!Name->empty() && !stateFreeSymbol(*Name))
      return true;
  }
  return false;
}

bool ExecutorPool::StartGate::closed(unsigned Max, std::string &Why) {
  std::lock_guard<std::mutex> L(Mu);
  if (InARow < Max)
    return false;
  Why = First;
  return true;
}

void ExecutorPool::StartGate::record(bool Ok, std::string &Why) {
  std::lock_guard<std::mutex> L(Mu);
  if (Ok) {
    InARow = 0;
    return;
  }
  ++InARow;
  if (First.empty())
    First = Why;
  Why = First;
}

/// One executor process and the thread's view of it.
class ExecutorPool::Worker {
  const PoolConfig &Cfg;
  StartGate &Gate;
  pid_t Pid = -1;
  int Fd = -1;
  LineBuffer In;
  unsigned JobsDone = 0;
  std::atomic<unsigned> &Started;

  /// A running executor that said "ready", or false with why not.
  bool ensureStarted(std::string &Why) {
    if (Pid >= 0)
      return true;
    if (Gate.closed(Cfg.MaxStartFailures, Why))
      return false;
    bool Ok = spawn(Why);
    if (!Ok)
      Why = "the executor does not start: " + Why;
    Gate.record(Ok, Why);
    return Ok;
  }

  /// Ends the executor (it is started again for the next job).
  void retire() {
    writeLine(Fd, encodeQuit());
    describeEnd(true);
    JobsDone = 0;
  }

  bool spawn(std::string &Why) {
    int SV[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, SV) != 0) {
      Why = std::string("socketpair: ") + std::strerror(errno);
      return false;
    }
    // The child's end must not be fd 3 already (dup2 onto itself keeps
    // CLOEXEC), so it is moved out of the way first.
    int Child = ::fcntl(SV[1], F_DUPFD_CLOEXEC, 10);
    ::close(SV[1]);
    posix_spawn_file_actions_t FA;
    posix_spawn_file_actions_init(&FA);
    posix_spawn_file_actions_adddup2(&FA, Child, ExecutorControlFd);
    // stdin is /dev/null for the executor itself; its stdout and stderr are
    // the pool's (only the executor's own failures write there; a program's
    // output goes to files).
    std::string Self = Cfg.Self;
    char *Argv[] = {Self.data(), const_cast<char *>("test-exec"), nullptr};
    int RC = posix_spawn(&Pid, Self.c_str(), &FA, nullptr, Argv, environ);
    posix_spawn_file_actions_destroy(&FA);
    ::close(Child);
    if (RC != 0) {
      ::close(SV[0]);
      Pid = -1;
      Why = "cannot start " + Self + ": " + std::strerror(RC);
      return false;
    }
    Fd = SV[0];
    In = LineBuffer();
    ++Started;
    // The executor says "ready" when its JIT is up. Every wait has a limit,
    // so a broken executor cannot hang the pool.
    std::string Line;
    auto Deadline =
        Clock::now() + std::chrono::milliseconds(Cfg.StartupTimeoutMs);
    for (;;) {
      while (In.next(Line))
        if (isReady(Line))
          return true;
      int Left = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                         Deadline - Clock::now())
                         .count());
      if (Left <= 0) {
        describeEnd(true);
        Why = "it was not ready after " + std::to_string(Cfg.StartupTimeoutMs) +
              " ms";
        return false;
      }
      pollfd P{Fd, POLLIN, 0};
      int N = ::poll(&P, 1, Left);
      if (N < 0 && errno == EINTR)
        continue;
      if (N > 0 && In.fill(Fd))
        continue;
      if (N != 0) {
        Why = "it ended before it was ready (" + describeEnd(true) + ")";
        return false;
      }
    }
  }

  /// After the socket closed: true once the process has ended (it stays
  /// waitable for describeEnd), false if it still runs at the deadline. A
  /// process that dies closes the socket on its way out, so this is at once;
  /// a program that closed the socket itself may run on.
  bool endsBy(Clock::time_point Deadline) {
    for (;;) {
      siginfo_t Info{};
      if (::waitid(P_PID, Pid, &Info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
          Info.si_pid == Pid)
        return true;
      if (Clock::now() >= Deadline)
        return false;
      ::usleep(1000);
    }
  }

  /// Reaps the process (killing it first if Kill) and says how it ended.
  std::string describeEnd(bool Kill, int *Code = nullptr, int *Sig = nullptr) {
    if (Pid < 0)
      return "no process";
    if (Kill)
      ::kill(Pid, SIGKILL);
    int St = 0;
    while (::waitpid(Pid, &St, 0) < 0 && errno == EINTR) {
    }
    Pid = -1;
    ::close(Fd);
    Fd = -1;
    if (WIFEXITED(St)) {
      if (Code)
        *Code = WEXITSTATUS(St);
      return "exit status " + std::to_string(WEXITSTATUS(St));
    }
    if (Sig)
      *Sig = WTERMSIG(St);
    return "signal " + std::to_string(WTERMSIG(St));
  }

public:
  Worker(const PoolConfig &Cfg, StartGate &Gate, std::atomic<unsigned> &Started)
      : Cfg(Cfg), Gate(Gate), Started(Started) {}

  ~Worker() {
    if (Pid < 0)
      return;
    writeLine(Fd, encodeQuit());
    // It leaves by itself; if not, it is killed.
    for (int I = 0; I < 200; ++I) {
      int St;
      if (::waitpid(Pid, &St, WNOHANG) == Pid) {
        ::close(Fd);
        return;
      }
      ::usleep(5000);
    }
    describeEnd(true);
  }

  /// Starts the process now, so its start-up overlaps with the compiles.
  void warm() {
    std::string Why;
    ensureStarted(Why);
  }

  JobResult run(const PoolJob &J, unsigned Seq) {
    JobResult R;
    R.Id = J.Id;
    auto T0 = Clock::now();
    if (Pid >= 0 && JobsDone >= Cfg.MaxJobsPerExecutor)
      retire();
    std::string Why;
    if (!ensureStarted(Why)) {
      R.O = Outcome::Internal;
      R.Message = Why;
      return R;
    }
    std::string OutPath = Cfg.WorkDir + "/" + std::to_string(Seq) + ".stdout";
    std::string ErrPath =
        J.MergeOutput ? ""
                      : Cfg.WorkDir + "/" + std::to_string(Seq) + ".stderr";
    RunRequest Rq{J.Id, J.ObjectPath, OutPath, ErrPath, Cfg.MaxOutputBytes};
    if (!writeLine(Fd, encodeRunRequest(Rq))) {
      // An idle executor that died (killed from outside): start a new one.
      describeEnd(true);
      if (!ensureStarted(Why) || !writeLine(Fd, encodeRunRequest(Rq))) {
        R.O = Outcome::Internal;
        R.Message = Why.empty() ? "cannot talk to an executor" : Why;
        if (Pid >= 0)
          describeEnd(true);
        return R;
      }
    }
    ++JobsDone;
    // A program that may leave C library state behind ends its executor (the
    // next job starts a new one). Not one that did not load: it never ran.
    bool Fresh = !Cfg.ShareExecutors && programKeepsState(J.ObjectPath);
    unsigned Limit = J.TimeoutMs ? J.TimeoutMs : Cfg.DefaultTimeoutMs;
    auto Deadline = Clock::now() + std::chrono::milliseconds(Limit);
    std::string Line;
    bool Replied = false;
    RunReply Reply;
    bool TimedOut = false;
    for (;;) {
      if (In.next(Line)) {
        auto Got = decodeRunReply(Line);
        if (Got) {
          Reply = std::move(*Got);
          Replied = true;
        } else {
          llvm::consumeError(Got.takeError());
        }
        break;
      }
      int Left = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                         Deadline - Clock::now())
                         .count());
      if (Left <= 0) {
        TimedOut = true;
        break;
      }
      pollfd P{Fd, POLLIN, 0};
      int N = ::poll(&P, 1, Left);
      if (N < 0 && errno == EINTR)
        continue;
      if (N == 0) {
        TimedOut = true;
        break;
      }
      if (!In.fill(Fd))
        break; // the process ended without an answer
    }
    R.RunMs = msSince(T0);
    if (Replied && Reply.Id != J.Id) {
      // Not this program's answer: nothing the executor says can be trusted.
      describeEnd(true);
      R.O = Outcome::Internal;
      R.Message =
          "the executor answered for another program ('" + Reply.Id + "')";
      R.ExecutorRestarted = true;
    } else if (Replied) {
      if (!Reply.Loaded) {
        R.O = Outcome::LoadFailed;
        R.Message = Reply.Error;
      } else {
        R.O = Outcome::Exited;
        R.ExitCode = Reply.ExitCode;
      }
      if (Reply.Recycle || (Fresh && Reply.Loaded)) {
        // The program changed what the executor cannot put back, or may have
        // (programKeepsState).
        retire();
        R.ExecutorRestarted = true;
      }
    } else if (TimedOut) {
      describeEnd(true);
      R.O = Outcome::TimedOut;
      R.ExecutorRestarted = true;
    } else if (!endsBy(Deadline)) {
      // The program closed the socket and runs on.
      describeEnd(true);
      R.O = Outcome::TimedOut;
      R.ExecutorRestarted = true;
    } else {
      // No answer: how the process ended is the program's result.
      int Code = 0, Sig = 0;
      describeEnd(false, &Code, &Sig);
      R.ExecutorRestarted = true;
      if (Sig) {
        R.O = Outcome::Signaled;
        R.Signal = Sig;
      } else {
        R.O = Outcome::Exited;
        R.ExitCode = Code;
      }
    }
    R.Stdout = readFile(OutPath);
    llvm::sys::fs::remove(OutPath);
    if (!J.MergeOutput) {
      R.Stderr = readFile(ErrPath);
      llvm::sys::fs::remove(ErrPath);
    }
    return R;
  }
};

unsigned gopp::testrun::defaultExecutors() {
  unsigned Cores = llvm::hardware_concurrency().compute_thread_count();
  return std::clamp(Cores / 4, 2u, 16u);
}

ExecutorPool::ExecutorPool(PoolConfig C) : Cfg(std::move(C)) {
  if (Cfg.Size == 0)
    Cfg.Size = defaultExecutors();
  for (unsigned I = 0; I < Cfg.Size; ++I)
    Threads.emplace_back([this] { loop(); });
}

ExecutorPool::~ExecutorPool() { stop(); }

void ExecutorPool::stop() {
  wait();
  {
    std::lock_guard<std::mutex> L(Mu);
    Stopping = true;
  }
  HaveWork.notify_all();
  for (std::thread &T : Threads)
    if (T.joinable())
      T.join();
}

unsigned ExecutorPool::processesStarted() const { return Started.load(); }

void ExecutorPool::submit(PoolJob J) {
  {
    std::lock_guard<std::mutex> L(Mu);
    Queue.push_back(std::move(J));
  }
  HaveWork.notify_one();
}

void ExecutorPool::wait() {
  std::unique_lock<std::mutex> L(Mu);
  Idle.wait(L, [this] { return Queue.empty() && Active == 0; });
}

void ExecutorPool::loop() {
  Worker W(Cfg, Gate, Started);
  W.warm();
  for (;;) {
    PoolJob J;
    {
      std::unique_lock<std::mutex> L(Mu);
      HaveWork.wait(L, [this] { return Stopping || !Queue.empty(); });
      if (Queue.empty())
        return; // Stopping
      J = std::move(Queue.front());
      Queue.pop_front();
      ++Active;
    }
    JobResult R = W.run(J, Seq++);
    J.Done(std::move(R));
    {
      std::lock_guard<std::mutex> L(Mu);
      --Active;
    }
    Idle.notify_all();
  }
}
