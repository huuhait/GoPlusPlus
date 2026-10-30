//===- TestRunTest.cpp - The fast test runner (P04.19) --------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The three parts of the runner, each alone and then together:
//   * the result protocol: every byte value survives a round trip, a wrong
//     version is refused;
//   * the batch compile driver: bad programs give diagnostics, the same
//     program compiled on many threads at once gives the same bytes;
//   * the executor pool: output captured exactly, an exit status, a panic's
//     exit 2, SIGSEGV and abort, a time-out and an object that does not link
//     each end only their own program, and the next program runs. The number
//     of executor processes started is the work counted by the stress test:
//     it follows the number of crashes and the recycling limit, not the
//     number of programs.
//
//===----------------------------------------------------------------------===//

#include "gopp/TestRun/BatchCompile.h"
#include "gopp/TestRun/ExecutorPool.h"
#include "gopp/TestRun/Protocol.h"
#include "gopp/TestRun/Runner.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"

#include <gtest/gtest.h>

#include <csignal>
#include <fstream>
#include <map>
#include <mutex>

using namespace gopp::testrun;

namespace {

/// A temporary directory with source files, removed at the end.
class Dir {
  llvm::SmallString<128> Path;

public:
  Dir() { EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-tr", Path)); }
  ~Dir() { llvm::sys::fs::remove_directories(Path); }
  std::string path() const { return std::string(Path); }
  std::string write(const std::string &Name, const std::string &Text) {
    std::string P = path() + "/" + Name;
    std::ofstream Out(P, std::ios::binary);
    Out << Text;
    return P;
  }
};

JobSpec job(const std::string &Id, const std::string &File) {
  JobSpec J;
  J.Id = Id;
  J.Inputs = {File};
  return J;
}

/// Runs the jobs through the whole runner; results by id.
std::map<std::string, JobResult> runAll(const std::vector<JobSpec> &Jobs,
                                        unsigned TimeoutMs = 20000) {
  RunnerConfig Cfg;
  Cfg.Self = GOPP_TEST_GOPP_BIN;
  Cfg.TimeoutMs = TimeoutMs;
  std::map<std::string, JobResult> Out;
  llvm::Error E = runBatch(Jobs, Cfg, [&](JobResult &&R) {
    std::string Id = R.Id;
    Out[Id] = std::move(R);
  });
  EXPECT_FALSE(bool(E)) << llvm::toString(std::move(E));
  return Out;
}

const char *Hello = "package main\nfunc main() { println(\"hi\", 42) }\n";

} // namespace

//===----------------------------------------------------------------------===//
// Protocol
//===----------------------------------------------------------------------===//

TEST(TestRunProtocol, ResultKeepsEveryByteValue) {
  JobResult R;
  R.Id = "a/b.gopp";
  R.O = Outcome::Signaled;
  R.Signal = 11;
  for (int B = 0; B < 256; ++B) {
    R.Stdout.push_back(char(B));
    R.Stderr.push_back(char(255 - B));
  }
  R.Message = "bad \xff byte"; // text is fixed up to valid UTF-8
  R.CompileMs = 1.5;
  R.RunMs = 2.25;
  R.ExecutorRestarted = true;
  std::string Line = encodeJobResult(R);
  EXPECT_EQ(Line.find('\n'), std::string::npos) << "one line";
  auto Back = decodeJobResult(Line);
  ASSERT_TRUE(bool(Back)) << llvm::toString(Back.takeError());
  EXPECT_EQ(Back->Id, R.Id);
  EXPECT_EQ(Back->O, Outcome::Signaled);
  EXPECT_EQ(Back->Signal, 11);
  EXPECT_EQ(Back->Stdout, R.Stdout);
  EXPECT_EQ(Back->Stderr, R.Stderr);
  EXPECT_TRUE(Back->ExecutorRestarted);
  EXPECT_DOUBLE_EQ(Back->RunMs, 2.25);
}

TEST(TestRunProtocol, SpecAndControlMessagesRoundTrip) {
  JobSpec J;
  J.Id = "x";
  J.Inputs = {"a.gopp", "b.gopp"};
  J.TimeoutMs = 1500;
  auto B = decodeJobSpec(encodeJobSpec(J));
  ASSERT_TRUE(bool(B));
  EXPECT_EQ(B->Inputs, J.Inputs);
  EXPECT_EQ(B->TimeoutMs, 1500u);
  EXPECT_EQ(B->Target, "host");

  EXPECT_FALSE(B->MergeOutput);
  J.MergeOutput = true;
  auto M = decodeJobSpec(encodeJobSpec(J));
  ASSERT_TRUE(bool(M));
  EXPECT_TRUE(M->MergeOutput);

  RunRequest Rq{"id", "/o.o", "/o.out", "/o.err", 1u << 20};
  auto Q = decodeRunRequest(encodeRunRequest(Rq));
  ASSERT_TRUE(bool(Q));
  EXPECT_EQ(Q->ObjectPath, "/o.o");
  EXPECT_EQ(Q->StderrPath, "/o.err");
  EXPECT_EQ(Q->MaxOutputBytes, 1u << 20);
  // Merged output: no file of its own for standard error.
  RunRequest Merged{"id", "/o.o", "/o.out", "", 0};
  auto QM = decodeRunRequest(encodeRunRequest(Merged));
  ASSERT_TRUE(bool(QM));
  EXPECT_EQ(QM->StderrPath, "");
  EXPECT_EQ(QM->MaxOutputBytes, 0u);
  RunReply Rp;
  Rp.Id = "id";
  Rp.ExitCode = 7;
  auto P = decodeRunReply(encodeRunReply(Rp));
  ASSERT_TRUE(bool(P));
  EXPECT_EQ(P->ExitCode, 7);
  EXPECT_TRUE(P->Loaded);
  EXPECT_FALSE(P->Recycle);
  Rp.Recycle = true;
  auto PR = decodeRunReply(encodeRunReply(Rp));
  ASSERT_TRUE(bool(PR));
  EXPECT_TRUE(PR->Recycle);
  EXPECT_TRUE(isReady(encodeReady()));
  EXPECT_TRUE(isQuit(encodeQuit()));
  EXPECT_FALSE(isQuit(encodeReady()));
}

TEST(TestRunProtocol, RefusesWhatItDoesNotKnow) {
  // The other side of the round trip: bad input is an error, not a guess.
  EXPECT_FALSE(
      bool(decodeJobResult("{\"v\":2,\"id\":\"a\",\"outcome\":\"exited\"}")));
  EXPECT_FALSE(
      bool(decodeJobResult("{\"v\":1,\"id\":\"a\",\"outcome\":\"melted\"}")));
  EXPECT_FALSE(bool(decodeJobResult("not json")));
  EXPECT_FALSE(bool(decodeJobResult("[1]")));
  EXPECT_FALSE(bool(decodeJobSpec("{\"v\":1,\"id\":\"a\",\"inputs\":[]}")));
  EXPECT_FALSE(bool(decodeJobSpec("{\"v\":1,\"inputs\":[\"a\"]}")));
  EXPECT_FALSE(
      bool(decodeRunRequest("{\"v\":1,\"cmd\":\"run\",\"id\":\"a\"}")));
  llvm::consumeError(decodeJobResult("not json").takeError());
}

//===----------------------------------------------------------------------===//
// Batch compile driver
//===----------------------------------------------------------------------===//

TEST(TestRunCompile, BadProgramGivesDiagnosticsAndGoodOneAnObject) {
  Dir D;
  CompileOptions Opts;
  Opts.WorkDir = D.path();
  CompiledProgram Bad = compileProgram(
      job("bad", D.write("bad.gopp", "package main\nfunc main() { x := }\n")),
      Opts, 0);
  EXPECT_FALSE(Bad.Ok);
  EXPECT_NE(Bad.Diagnostics.find("error["), std::string::npos)
      << Bad.Diagnostics;
  CompiledProgram Lib = compileProgram(
      job("lib", D.write("lib.gopp", "package lib\nfunc F() {}\n")), Opts, 1);
  EXPECT_FALSE(Lib.Ok);
  EXPECT_NE(Lib.Diagnostics.find("not package main"), std::string::npos);
  CompiledProgram Good =
      compileProgram(job("ok", D.write("ok.gopp", Hello)), Opts, 2);
  ASSERT_TRUE(Good.Ok) << Good.Diagnostics;
  EXPECT_TRUE(llvm::sys::fs::exists(Good.ObjectPath));
  JobSpec Arm = job("arm", D.write("a.gopp", Hello));
  Arm.Target = "linux/arm64";
  CompiledProgram A = compileProgram(Arm, Opts, 3);
  EXPECT_FALSE(A.Ok) << "the JIT runs the host target only";
}

TEST(TestRunCompile, SameProgramOnManyThreadsGivesTheSameBytes) {
  Dir D;
  std::string Src = D.write("p.gopp", Hello);
  std::vector<JobSpec> Jobs;
  for (int I = 0; I < 48; ++I)
    Jobs.push_back(job("j" + std::to_string(I), Src));
  CompileOptions Opts;
  Opts.WorkDir = D.path();
  std::mutex Mu;
  std::vector<std::string> Objects(Jobs.size());
  compileBatch(Jobs, Opts, 16, [&](size_t I, CompiledProgram &&C) {
    std::lock_guard<std::mutex> L(Mu);
    EXPECT_TRUE(C.Ok) << C.Diagnostics;
    Objects[I] = C.ObjectPath;
  });
  auto First = llvm::MemoryBuffer::getFile(Objects[0]);
  ASSERT_TRUE(bool(First));
  for (const std::string &O : Objects) {
    auto B = llvm::MemoryBuffer::getFile(O);
    ASSERT_TRUE(bool(B)) << O;
    EXPECT_EQ((*B)->getBuffer(), (*First)->getBuffer());
  }
}

//===----------------------------------------------------------------------===//
// Executor pool, through the whole runner
//===----------------------------------------------------------------------===//

TEST(TestRunPool, OutputStatusAndCrashesAreEachProgramsOwn) {
  Dir D;
  std::vector<JobSpec> Jobs;
  Jobs.push_back(job("hello", D.write("hello.gopp", Hello)));
  Jobs.push_back(job("exit7", D.write("exit7.gopp", R"(package main
@Extern("C")
func exit(code int32)
func main() {
	println("bye")
	exit(7)
	println("not reached")
}
)")));
  Jobs.push_back(job("panic", D.write("panic.gopp", R"(package main
func main() {
	println("before")
	var x [3]int
	i := 5
	println(x[i%7])
}
)")));
  Jobs.push_back(job("segv", D.write("segv.gopp", R"(package main
import "unsafe"
func main() {
	println("before")
	var a uintptr = 8
	p := (*int)(unsafe.Pointer(a))
	*p = 1
	println("after")
}
)")));
  Jobs.push_back(job("abort", D.write("abort.gopp", R"(package main
@Extern("C")
func abort()
func main() {
	println("x")
	abort()
}
)")));
  Jobs.push_back(job(
      "loop", D.write("loop.gopp", "package main\nfunc main() { for {} }\n")));
  Jobs.back().TimeoutMs = 300;
  Jobs.push_back(job("nolink", D.write("nolink.gopp", R"(package main
@Extern("C")
func no_such_function_xyz()
func main() { no_such_function_xyz() }
)")));
  Jobs.push_back(
      job("bad", D.write("bad.gopp", "package main\nfunc main() { x := }\n")));
  Jobs.push_back(job("after", D.write("after.gopp", Hello)));

  auto R = runAll(Jobs);
  ASSERT_EQ(R.size(), Jobs.size());
  EXPECT_TRUE(R["hello"].exitedWith(0));
  EXPECT_EQ(R["hello"].Stdout, "hi 42\n");
  EXPECT_EQ(R["hello"].Stderr, "");
  EXPECT_TRUE(R["exit7"].exitedWith(7));
  EXPECT_EQ(R["exit7"].Stdout, "bye\n");
  EXPECT_TRUE(R["exit7"].ExecutorRestarted) << "exit ends the executor";
  EXPECT_TRUE(R["panic"].exitedWith(2)) << R["panic"].Message;
  EXPECT_EQ(R["panic"].Stdout, "before\n");
  EXPECT_EQ(R["panic"].Stderr.substr(0, 31), "panic: runtime error: index out");
  EXPECT_EQ(R["segv"].O, Outcome::Signaled);
  EXPECT_EQ(R["segv"].Signal, SIGSEGV);
  EXPECT_EQ(R["segv"].Stdout, "before\n") << "output before the crash is kept";
  EXPECT_EQ(R["abort"].O, Outcome::Signaled);
  EXPECT_EQ(R["abort"].Signal, SIGABRT);
  EXPECT_EQ(R["abort"].Stdout, "x\n");
  EXPECT_EQ(R["loop"].O, Outcome::TimedOut);
  EXPECT_EQ(R["nolink"].O, Outcome::LoadFailed);
  // The missing symbols, sorted, and nothing that depends on the executor.
  EXPECT_EQ(R["nolink"].Message, "undefined symbols: no_such_function_xyz");
  EXPECT_EQ(R["bad"].O, Outcome::CompileFailed);
  // The other side: the programs that did not fail are not touched by the
  // ones that did, whatever order the pool ran them in.
  EXPECT_TRUE(R["after"].exitedWith(0));
  EXPECT_EQ(R["after"].Stdout, "hi 42\n");
}

TEST(TestRunPool, OutputIsExactNotTextConverted) {
  Dir D;
  // Bytes that are not UTF-8 and a NUL, written with `write` directly.
  std::string Src = R"(package main
@Extern("C")
func write(fd int32, p *byte, n uint) int
func main() {
	b := [4]byte{0, 255, 10, 13}
	write(1, &b[0], 4)
	write(2, &b[1], 2)
}
)";
  auto R = runAll({job("bytes", D.write("b.gopp", Src))});
  ASSERT_TRUE(R["bytes"].exitedWith(0)) << R["bytes"].Message;
  EXPECT_EQ(R["bytes"].Stdout, std::string("\0\xff\n\r", 4));
  EXPECT_EQ(R["bytes"].Stderr, "\xff\n");
}

TEST(TestRunPool, ProcessesStartedFollowCrashesNotPrograms) {
  // The work counted: executor processes started. 120 programs on 2
  // executors, 6 of them crash: 2 first processes + 6 restarts. With the
  // recycling limit at 50 and no crash, 120 programs need 2 + 2 recycles.
  Dir D;
  CompileOptions CO;
  CO.WorkDir = D.path();
  CompiledProgram Ok =
      compileProgram(job("ok", D.write("ok.gopp", Hello)), CO, 0);
  CompiledProgram Crash = compileProgram(
      job("c", D.write("c.gopp", "package main\n@Extern(\"C\")\nfunc "
                                 "abort()\nfunc main() { abort() }\n")),
      CO, 1);
  ASSERT_TRUE(Ok.Ok && Crash.Ok);

  auto Run = [&](unsigned Programs, unsigned CrashEvery, unsigned Limit) {
    PoolConfig PC;
    PC.Self = GOPP_TEST_GOPP_BIN;
    PC.Size = 2;
    PC.WorkDir = D.path();
    PC.MaxJobsPerExecutor = Limit;
    std::atomic<unsigned> Good{0}, Crashed{0}, Wrong{0};
    unsigned Started;
    {
      ExecutorPool Pool(PC);
      for (unsigned I = 0; I < Programs; ++I) {
        bool C = CrashEvery && I % CrashEvery == CrashEvery - 1;
        PoolJob J;
        J.Id = std::to_string(I);
        J.ObjectPath = C ? Crash.ObjectPath : Ok.ObjectPath;
        J.Done = [&, C](JobResult &&R) {
          if (C && R.O == Outcome::Signaled)
            ++Crashed;
          else if (!C && R.exitedWith(0) && R.Stdout == "hi 42\n")
            ++Good;
          else
            ++Wrong;
        };
        Pool.submit(std::move(J));
      }
      Pool.wait();
      Started = Pool.processesStarted();
    }
    EXPECT_EQ(Wrong.load(), 0u);
    EXPECT_EQ(Good.load() + Crashed.load(), Programs);
    return std::make_pair(Started, Crashed.load());
  };
  auto [S1, C1] = Run(120, 20, 1000);
  EXPECT_EQ(C1, 6u);
  // 2 first processes, one restart per crash. Every crash ends a process, and
  // a new one starts only if more jobs come for that executor: a crash that is
  // an executor's last job starts nothing, and there are two executors.
  EXPECT_GE(S1, 2u + 4u);
  EXPECT_LE(S1, 2u + 6u);
  auto [S2, C2] = Run(120, 0, 50);
  EXPECT_EQ(C2, 0u);
  // The same number of programs without crashes: recycling only. Control: a
  // run that is twice as long recycles about twice as often, not more.
  EXPECT_GE(S2, 2u + 1u);
  EXPECT_LE(S2, 2u + 3u);
  auto [S3, C3] = Run(240, 0, 50);
  EXPECT_LE(S3, 2u + 5u);
  EXPECT_GE(S3, S2);
}

TEST(TestRunPool, ProgramsThatKeepCLibraryStateAreToldFromThePureOnes) {
  // R1: the pool decides from the object's undefined symbols. Both sides:
  // programs that use only the allowlist (write, exit, abort, mem*, libm) are
  // state-free; anything else (rand, a data symbol) is not.
  Dir D;
  CompileOptions CO;
  CO.WorkDir = D.path();
  auto Keeps = [&](const std::string &Name, const std::string &Src) {
    CompiledProgram C =
        compileProgram(job(Name, D.write(Name + ".gopp", Src)), CO, 0);
    EXPECT_TRUE(C.Ok) << C.Diagnostics;
    return programKeepsState(C.ObjectPath);
  };
  EXPECT_FALSE(Keeps("hello", Hello));
  EXPECT_FALSE(Keeps("math",
                     "package main\n@Extern(\"C\")\nfunc sin(x "
                     "float64) float64\n@Extern(\"C\")\nfunc sqrtf(x "
                     "float32) float32\nfunc main() { println(sin(1.0), "
                     "sqrtf(2.0)) }\n"));
  EXPECT_TRUE(Keeps("rand", "package main\n@Extern(\"C\")\nfunc rand() "
                            "int32\nfunc main() { println(rand()) }\n"));
  EXPECT_TRUE(Keeps("getenv", "package main\n@Extern(\"C\")\nfunc getpid() "
                              "int32\nfunc main() { println(getpid()) }\n"));
  EXPECT_TRUE(programKeepsState(D.path() + "/no-such-object.o"));
  EXPECT_TRUE(programKeepsState(D.write("junk.o", "not an object")));
}

TEST(TestRunPool, AnExecutorThatDoesNotStartFailsEveryJobTheSameWay) {
  // No executor can start (the program is missing): every job gets the same
  // `internal` result, the first failure's text, and the pool stops trying
  // after MaxStartFailures failures in a row instead of once per job.
  Dir D;
  PoolConfig PC;
  PC.Self = D.path() + "/no-such-gopp";
  PC.Size = 2;
  PC.WorkDir = D.path();
  std::mutex Mu;
  std::vector<JobResult> Results;
  unsigned Started;
  {
    ExecutorPool Pool(PC);
    for (int I = 0; I < 20; ++I) {
      PoolJob J;
      J.Id = std::to_string(I);
      J.ObjectPath = D.path() + "/none.o";
      J.Done = [&](JobResult &&R) {
        std::lock_guard<std::mutex> L(Mu);
        Results.push_back(std::move(R));
      };
      Pool.submit(std::move(J));
    }
    Pool.stop();
    Started = Pool.processesStarted();
  }
  ASSERT_EQ(Results.size(), 20u);
  for (const JobResult &R : Results) {
    EXPECT_EQ(R.O, Outcome::Internal);
    EXPECT_EQ(R.Message, Results[0].Message);
  }
  EXPECT_EQ(Results[0].Message.rfind("the executor does not start: cannot "
                                     "start ",
                                     0),
            0u)
      << Results[0].Message;
  EXPECT_EQ(Started, 0u);
}
