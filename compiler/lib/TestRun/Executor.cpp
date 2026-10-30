//===- Executor.cpp - Loads and runs objects in an ORC JIT ----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/TestRun/Executor.h"

#include "Lines.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/TestRun/Protocol.h"

#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/TargetSelect.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern char **environ;

using namespace gopp;
using namespace gopp::testrun;
using namespace llvm;
using namespace llvm::orc;

namespace {

/// `exit` as the program sees it in the JIT: what a real binary does (flush
/// the C streams, leave with the status), without running the destructors of
/// the executor's own libraries first.
void jitExit(int Code) {
  std::fflush(nullptr);
  _exit(Code);
}

/// The descriptors the executor keeps for itself. They live at 900 and up (or
/// as high as the descriptor limit allows), so the program finds 0, 1 and 2
/// open and 3 and up free, as in a binary.
struct OwnFds {
  int Control = -1;       // the socket to the pool
  int Null = -1;          // /dev/null, standard input of every program
  int Out = -1, Err = -1; // the executor's own standard output and error

  /// A copy of Fd at the lowest free number at or above the highest floor
  /// that works; -1 if none does.
  static int copyHigh(int Fd) {
    for (int Min : {900, 100, 10}) {
      int N = ::fcntl(Fd, F_DUPFD_CLOEXEC, Min);
      if (N >= 0)
        return N;
    }
    return -1;
  }

  bool take(int ControlFd) {
    Control = copyHigh(ControlFd);
    if (Control < 0)
      return false;
    ::close(ControlFd);
    int DevNull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (DevNull < 0)
      return false;
    Null = copyHigh(DevNull);
    ::close(DevNull);
    Out = copyHigh(1);
    Err = copyHigh(2);
    if (Null < 0 || Out < 0 || Err < 0)
      return false;
    ::dup2(Null, 0);
    closeProgramFds();
    return true;
  }

  /// Closes every descriptor from 3 up that is not one of the executor's own:
  /// what the program opened and did not close (below or above the executor's
  /// descriptors), or what the executor inherited (a binary started by a test
  /// starts with 0, 1 and 2 only).
  void closeProgramFds() const {
    int Own4[4] = {Control, Null, Out, Err};
    std::sort(Own4, Own4 + 4);
    unsigned Next = 3;
    bool Ok = true;
    for (int Keep : Own4) {
      if (Keep < 0)
        continue;
      if (unsigned(Keep) > Next)
        Ok &= ::close_range(Next, unsigned(Keep) - 1, 0) == 0;
      Next = unsigned(Keep) + 1;
    }
    Ok &= ::close_range(Next, ~0U, 0) == 0;
    if (!Ok)
      closeByScan();
  }

private:
  bool isOwn(int Fd) const {
    return Fd == Control || Fd == Null || Fd == Out || Fd == Err;
  }

  /// close_range is Linux 5.9 and up: on an older kernel the descriptors are
  /// read from /proc/self/fd.
  void closeByScan() const {
    std::vector<int> Open;
    if (DIR *D = ::opendir("/proc/self/fd")) {
      while (const dirent *E = ::readdir(D)) {
        int Fd = std::atoi(E->d_name);
        if (Fd >= 3 && Fd != ::dirfd(D) && !isOwn(Fd))
          Open.push_back(Fd);
      }
      ::closedir(D);
    }
    for (int Fd : Open)
      ::close(Fd);
  }
};

/// The default action for every signal, nothing blocked, nothing pending, no
/// alternate stack: what a process starts with. Setting SIG_IGN first
/// discards a pending signal (POSIX), as the end of a program's own process
/// would.
void resetSignals() {
  struct sigaction Ign = {}, Dfl = {};
  Ign.sa_handler = SIG_IGN;
  Dfl.sa_handler = SIG_DFL;
  for (int Sig = 1; Sig < NSIG; ++Sig) {
    if (Sig == SIGKILL || Sig == SIGSTOP)
      continue;
    ::sigaction(Sig, &Ign, nullptr); // fails harmlessly for glibc's own ones
    ::sigaction(Sig, &Dfl, nullptr);
  }
  sigset_t None;
  sigemptyset(&None);
  ::sigprocmask(SIG_SETMASK, &None, nullptr);
  stack_t St{};
  St.ss_flags = SS_DISABLE;
  ::sigaltstack(&St, nullptr);
}

/// What a program can change in its process and the next program in the same
/// executor must not see (manager decision M2 of P04.19). Taken when the
/// executor starts; put back after every program that returns from main.
/// Not covered here: C library state (stdio, locale, rand, timers, threads,
/// ...). The pool does not share an executor with a program that could change
/// it (programKeepsState in ExecutorPool.h), and this executor ends after a
/// program that left a thread. See docs/design/test-runner.md.
class ProcessState {
  /// glibc's getrlimit takes an enum in C++.
  static decltype(RLIMIT_CPU) resource(int R) {
    return static_cast<decltype(RLIMIT_CPU)>(R);
  }

  mode_t Umask = 022;
  std::string Cwd;
  std::vector<std::string> Env;
  struct rlimit Limits[RLIM_NLIMITS] = {};

public:
  void capture() {
    Umask = ::umask(0);
    ::umask(Umask);
    // Empty if it cannot be read (deleted): then every program that
    // returns makes the pool replace the executor (restore fails).
    std::vector<char> Buf(4096);
    const char *Got;
    while (!(Got = ::getcwd(Buf.data(), Buf.size())) && errno == ERANGE)
      Buf.resize(Buf.size() * 2);
    if (Got)
      Cwd = Got;
    for (char **E = environ; E && *E; ++E)
      Env.push_back(*E);
    for (int R = 0; R < RLIM_NLIMITS; ++R)
      ::getrlimit(resource(R), &Limits[R]);
  }

  /// Puts everything back. False if something could not be: then the
  /// executor must not run another program.
  bool restore() const {
    bool Ok = true;
    struct itimerval Zero = {};
    for (int Which : {ITIMER_REAL, ITIMER_VIRTUAL, ITIMER_PROF})
      ::setitimer(Which, &Zero, nullptr);
    // After the timers: a signal a timer raised just now is discarded too.
    resetSignals();
    ::umask(Umask);
    errno = 0; // a libm function that failed leaves it set
    if (Cwd.empty() || ::chdir(Cwd.c_str()) != 0)
      Ok = false;
    // The strings are copied: putenv keeps the pointer it is given, and the
    // program may have written into the ones it read with getenv.
    ::clearenv();
    for (const std::string &E : Env)
      ::putenv(::strdup(E.c_str()));
    for (int R = 0; R < RLIM_NLIMITS; ++R) {
      struct rlimit Now;
      if (::getrlimit(resource(R), &Now) != 0)
        continue;
      if (Now.rlim_cur == Limits[R].rlim_cur &&
          Now.rlim_max == Limits[R].rlim_max)
        continue;
      // A lowered hard limit cannot be raised again without privilege.
      if (::setrlimit(resource(R), &Limits[R]) != 0)
        Ok = false;
    }
    return Ok;
  }
};

/// Why an object does not link. The text must not depend on the order of
/// ORC's hash sets or on the executor's history (the same program gives the
/// same result however it is scheduled), so the missing symbols are sorted
/// and ORC's "failed to materialize" (a hash-ordered set of the program's own
/// symbols, with the name of its JITDylib) is left out.
struct LinkProblems {
  std::set<std::string> Missing, Other;

  void add(Error E) {
    handleAllErrors(
        std::move(E),
        [&](const SymbolsNotFound &S) {
          for (const SymbolStringPtr &N : S.getSymbols())
            Missing.insert((*N).str());
        },
        [&](const FailedToMaterialize &) {},
        [&](const ErrorInfoBase &B) { Other.insert(B.message()); });
  }

  std::string text() const {
    std::string T;
    if (!Missing.empty()) {
      T = "undefined symbols:";
      for (const std::string &N : Missing)
        T += " " + N;
    }
    for (const std::string &O : Other)
      T += (T.empty() ? "" : "\n") + O;
    return T.empty() ? "the object does not link" : T;
  }
};

/// The threads of this process (one directory entry per thread). A program
/// that started a thread leaves it running, with code that is about to be
/// freed: the executor must end.
unsigned threadCount() {
  unsigned N = 0;
  if (DIR *D = ::opendir("/proc/self/task")) {
    while (const dirent *E = ::readdir(D))
      if (E->d_name[0] != '.')
        ++N;
    ::closedir(D);
  }
  return N ? N : 1;
}

class Executor {
  std::unique_ptr<LLJIT> J;
  const OwnFds &Own;
  const ProcessState &Start;
  pid_t Pid = ::getpid();
  LinkProblems *Problems = nullptr; // the load in progress
  unsigned Count = 0;

  /// The program's standard input (/dev/null), output and error (the
  /// request's files; one file for both when the request merges them).
  bool openStreams(const RunRequest &Rq, std::string &Why) {
    const int Flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
    int Out = ::open(Rq.StdoutPath.c_str(), Flags, 0644);
    int Err = Rq.StderrPath.empty()
                  ? Out
                  : ::open(Rq.StderrPath.c_str(), Flags, 0644);
    if (Out < 0 || Err < 0) {
      Why =
          std::string("cannot open the output files: ") + std::strerror(errno);
      if (Out >= 0)
        ::close(Out);
      if (Err >= 0 && Err != Out)
        ::close(Err);
      return false;
    }
    std::fflush(nullptr);
    ::dup2(Own.Null, 0);
    ::dup2(Out, 1);
    ::dup2(Err, 2);
    ::close(Out);
    if (Err != Out)
      ::close(Err);
    return true;
  }

  /// After main returned: the executor's own streams again, the program's
  /// descriptors closed.
  void closeStreams() {
    std::fflush(nullptr);
    ::dup2(Own.Null, 0);
    ::dup2(Own.Out, 1);
    ::dup2(Own.Err, 2);
    Own.closeProgramFds();
  }

public:
  Executor(const OwnFds &Own, const ProcessState &Start)
      : Own(Own), Start(Start) {}

  Error init() {
    initializeTargets();
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    auto Made = LLJITBuilder().setNumCompileThreads(0).create();
    if (!Made)
      return Made.takeError();
    J = std::move(*Made);
    J->getExecutionSession().setErrorReporter([this](Error E) {
      if (Problems)
        Problems->add(std::move(E));
      else
        consumeError(std::move(E));
    });
    return Error::success();
  }

  RunReply run(const RunRequest &Rq) {
    RunReply R;
    R.Id = Rq.Id;
    R.Loaded = false;
    auto Obj = MemoryBuffer::getFile(Rq.ObjectPath);
    if (!Obj) {
      R.Error = Rq.ObjectPath + ": " + Obj.getError().message();
      return R;
    }
    auto JDOr = J->createJITDylib("job" + std::to_string(Count++));
    if (!JDOr) {
      R.Error = toString(JDOr.takeError());
      return R;
    }
    JITDylib &JD = *JDOr;
    ExecutionSession &ES = J->getExecutionSession();
    // The names a program may use: its own, then the process's (libc), with
    // `exit` replaced.
    LinkProblems LP;
    Problems = &LP;
    int (*Main)() = nullptr;
    auto Gen = DynamicLibrarySearchGenerator::GetForCurrentProcess(
        J->getDataLayout().getGlobalPrefix());
    SymbolMap OwnSyms;
    OwnSyms[J->mangleAndIntern("exit")] = {ExecutorAddr::fromPtr(&jitExit),
                                           JITSymbolFlags::Exported |
                                               JITSymbolFlags::Callable};
    if (!Gen) {
      LP.add(Gen.takeError());
    } else {
      JD.addGenerator(std::move(*Gen));
      if (Error E = JD.define(absoluteSymbols(std::move(OwnSyms))))
        LP.add(std::move(E));
      else if (Error E = J->addObjectFile(JD, std::move(*Obj)))
        LP.add(std::move(E));
      else if (auto Addr = J->lookup(JD, "main"))
        Main = Addr->toPtr<int (*)()>();
      else
        LP.add(Addr.takeError());
    }
    Problems = nullptr;
    std::string Why;
    if (!Main || !openStreams(Rq, Why)) {
      R.Error = Main ? Why : LP.text();
      if (Error E = ES.removeJITDylib(JD)) {
        consumeError(std::move(E));
        R.Recycle = true;
      }
      return R;
    }
    if (Rq.MaxOutputBytes) {
      struct rlimit L;
      ::getrlimit(RLIMIT_FSIZE, &L);
      L.rlim_cur = std::min<rlim_t>(Rq.MaxOutputBytes, L.rlim_max);
      ::setrlimit(RLIMIT_FSIZE, &L);
    }
    R.ExitCode = Main() & 0xFF; // what crt1 passes to exit
    if (::getpid() != Pid) {
      // A child the program forked returned from main: it ends here, as a
      // child of a real binary would, and never talks to the pool.
      std::fflush(nullptr);
      _exit(R.ExitCode);
    }
    R.Loaded = true;
    closeStreams();
    R.Recycle = !Start.restore();
    if (threadCount() != 1) {
      // A thread may still run the program's code: leave it in place, the
      // executor ends after this answer.
      R.Recycle = true;
      return R;
    }
    // The program's code and data go; the executor stays for the next one.
    if (Error E = ES.removeJITDylib(JD)) {
      consumeError(std::move(E));
      R.Recycle = true;
    }
    return R;
  }
};

} // namespace

void gopp::testrun::runExecutor(int Fd) {
  resetSignals();
  // Without the pool there is nothing to run for: leave when it does.
  ::prctl(PR_SET_PDEATHSIG, SIGKILL);
  // A crash of a program must not leave a core file of the whole executor.
  struct rlimit NoCore = {0, 0};
  ::setrlimit(RLIMIT_CORE, &NoCore);
  OwnFds Own;
  if (!Own.take(Fd)) {
    std::fprintf(stderr, "gopp test-exec: cannot set up descriptors: %s\n",
                 std::strerror(errno));
    _exit(70);
  }
  ProcessState Start;
  Start.capture();
  Executor E(Own, Start);
  if (Error Err = E.init()) {
    errs() << "gopp test-exec: " << toString(std::move(Err)) << "\n";
    _exit(70);
  }
  if (!writeLine(Own.Control, encodeReady()))
    _exit(71);
  LineBuffer In;
  std::string Line;
  for (;;) {
    while (!In.next(Line)) {
      if (!In.fill(Own.Control))
        _exit(0); // the pool is gone
    }
    if (isQuit(Line))
      _exit(0);
    Expected<RunRequest> Rq = decodeRunRequest(Line);
    if (!Rq) {
      errs() << "gopp test-exec: " << toString(Rq.takeError()) << "\n";
      _exit(72);
    }
    RunReply Reply = E.run(*Rq);
    // A program that closed the socket cannot be answered; the status of the
    // process is then its answer, as for a program that calls exit.
    if (!writeLine(Own.Control, encodeRunReply(Reply)))
      _exit(Reply.Loaded ? Reply.ExitCode : 71);
  }
}
