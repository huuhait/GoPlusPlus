//===- ParseThroughput.cpp - Lex + parse throughput (P02.11) --------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Measures how fast the front end lexes and parses source text, in lines per
// second per core, on the Go standard library ($(go env GOROOT)/src, read at
// run time; the files are not copied into the repository). All files are read
// into memory first, so the numbers contain no I/O.
//
//   gopp-bench-parse [-dir DIR] [-runs N] [-threads N] [-with-testdata]
//
// Three measurements, each the best of N runs:
//   serial    one thread parses all files of all packages (CPU time); this is
//             the number of the D2.9 budget
//   pkg-par   the files of one package are parsed in parallel (D2.9) by
//             gopp::parseFiles on a shared pool, one package after the other
//   all-par   every file at once on the pool (packages in parallel as well)
// The parallel runs are wall clock; "per thread" is the total divided by the
// thread count. Files below a "testdata" directory are left out (many have
// syntax errors on purpose), unless -with-testdata is given. Only a Release
// build counts: the Dev build has -O1 and assertions.
//
//===----------------------------------------------------------------------===//

#include "gopp/AST/ASTContext.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/Parse/Parser.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <chrono>
#include <map>

using namespace llvm;

static cl::opt<std::string>
    Dir("dir", cl::desc("Directory to scan (default: $(go env GOROOT)/src)"));
static cl::opt<unsigned> Runs("runs",
                              cl::desc("Runs per measurement; the best counts"),
                              cl::init(5));
static cl::opt<unsigned>
    Threads("threads",
            cl::desc("Threads of the parallel measurement (0: all cores)"),
            cl::init(0));
static cl::opt<bool>
    WithTestdata("with-testdata",
                 cl::desc("Also parse files below testdata directories"));

namespace {

struct Package {
  std::string Dir;
  std::vector<gopp::FileID> Files;
  uint64_t Lines = 0;
  uint64_t BiggestFile = 0; // lines of the biggest file
};

uint64_t countLines(StringRef S) {
  uint64_t N = S.count('\n');
  if (!S.empty() && S.back() != '\n')
    ++N;
  return N;
}

std::string goRoot() {
  auto Go = sys::findProgramByName("go");
  if (!Go)
    return "";
  SmallString<128> Out;
  int FD;
  if (sys::fs::createTemporaryFile("gorootsrc", "txt", FD, Out))
    return "";
  sys::fs::file_t F = sys::fs::convertFDToNativeFile(FD);
  (void)F;
  sys::fs::closeFile(F);
  std::optional<StringRef> Redirects[] = {std::nullopt, StringRef(Out),
                                          std::nullopt};
  int RC = sys::ExecuteAndWait(*Go, {"go", "env", "GOROOT"}, std::nullopt,
                               Redirects);
  std::string Result;
  if (RC == 0)
    if (auto Buf = MemoryBuffer::getFile(Out))
      Result = (*Buf)->getBuffer().trim().str();
  sys::fs::remove(Out);
  return Result.empty() ? "" : Result + "/src";
}

/// Parses one file as a build would (the default error limit); the
/// diagnostics are dropped (recovery is part of the cost).
uint64_t parseOne(const gopp::SourceManager &SM, gopp::FileID F) {
  gopp::CollectingDiagnosticConsumer Quiet;
  gopp::DiagnosticsEngine Diags(SM, Quiet);
  gopp::ast::ASTContext Ctx;
  gopp::ParseOptions Opts;
  Opts.CheckLanguageRules = false;
  gopp::ast::File *AST = gopp::parseFile(Ctx, SM, F, Diags, Opts);
  return AST->Decls.size(); // keeps the call from being optimized away
}

double seconds(std::chrono::steady_clock::time_point A,
               std::chrono::steady_clock::time_point B) {
  return std::chrono::duration<double>(B - A).count();
}

/// CPU time of this process (user + system).
double cpuSeconds() {
  std::chrono::nanoseconds User, Sys;
  sys::TimePoint<> Wall;
  sys::Process::GetTimeUsage(Wall, User, Sys);
  return std::chrono::duration<double>(User + Sys).count();
}

} // namespace

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "Go++ parse throughput benchmark\n");

  std::string Root = Dir;
  if (Root.empty())
    Root = goRoot();
  if (Root.empty()) {
    errs() << "gopp-bench-parse: no -dir and `go env GOROOT` failed\n";
    return 1;
  }

  // Read every file into memory.
  gopp::SourceManager SM;
  std::map<std::string, Package> Packages;
  uint64_t TotalLines = 0, TotalBytes = 0, NFiles = 0;
  std::error_code EC;
  for (sys::fs::recursive_directory_iterator It(Root, EC), End;
       It != End && !EC; It.increment(EC)) {
    StringRef Path = It->path();
    if (!Path.ends_with(".go"))
      continue;
    if (!WithTestdata && Path.contains("/testdata/"))
      continue;
    auto Buf = MemoryBuffer::getFile(Path);
    if (!Buf)
      continue;
    uint64_t Lines = countLines((*Buf)->getBuffer());
    uint64_t Bytes = (*Buf)->getBufferSize();
    Expected<gopp::FileID> F = SM.addBuffer(std::move(*Buf));
    if (!F) {
      consumeError(F.takeError());
      continue;
    }
    Package &P = Packages[sys::path::parent_path(Path).str()];
    P.Dir = sys::path::parent_path(Path).str();
    P.Files.push_back(*F);
    P.Lines += Lines;
    P.BiggestFile = std::max(P.BiggestFile, Lines);
    TotalLines += Lines;
    TotalBytes += Bytes;
    ++NFiles;
  }
  if (EC || NFiles == 0) {
    errs() << "gopp-bench-parse: no .go files below " << Root << "\n";
    return 1;
  }

  const unsigned NRuns = Runs;
  unsigned NThreads =
      Threads ? Threads : hardware_concurrency().compute_thread_count();
  outs() << "corpus: " << Root << ": " << NFiles << " files, "
         << Packages.size() << " packages, " << TotalLines << " lines, "
         << TotalBytes / 1024 << " KiB\n";

  // Serial: all files, one thread. CPU time, so that a busy machine does not
  // make the per-core number worse than it is.
  double BestSerial = 1e30;
  uint64_t Sink = 0;
  for (unsigned R = 0; R < NRuns; ++R) {
    double T0 = cpuSeconds();
    for (auto &[Name, P] : Packages)
      for (gopp::FileID F : P.Files)
        Sink += parseOne(SM, F);
    BestSerial = std::min(BestSerial, cpuSeconds() - T0);
  }
  double SerialRate = TotalLines / BestSerial;
  outs() << format("serial   (1 thread):   best of %u: %.3f s  %.2f M "
                   "lines/s/core  %.1f MiB/s\n",
                   NRuns, BestSerial, SerialRate / 1e6,
                   TotalBytes / BestSerial / (1024.0 * 1024.0));

  // Parallel, wall clock, on one pool for the whole run (gopp::parseFiles, as
  // the build driver will use it):
  //   pkg-par  one package after the other, its files in parallel (D2.9);
  //   all-par  every file of the corpus at once (packages in parallel too).
  ThreadPoolStrategy Strategy = hardware_concurrency(NThreads);
  DefaultThreadPool Pool(Strategy);
  gopp::ParseOptions PO;
  PO.CheckLanguageRules = false; // plain Go
  auto RunParallel = [&](const char *Name, auto &&Parse) {
    double Best = 1e30;
    for (unsigned R = 0; R < NRuns; ++R) {
      auto T0 = std::chrono::steady_clock::now();
      Sink += Parse();
      Best = std::min(Best, seconds(T0, std::chrono::steady_clock::now()));
    }
    double Rate = TotalLines / Best;
    outs() << format("%s  (%u threads): best of %u: %.3f s  %.2f M lines/s "
                     "total, %.2f M lines/s/thread, speedup %.1fx\n",
                     Name, NThreads, NRuns, Best, Rate / 1e6,
                     Rate / NThreads / 1e6, BestSerial / Best);
  };
  RunParallel("pkg-par", [&] {
    uint64_t N = 0;
    for (auto &[Name, P] : Packages) {
      gopp::CollectingDiagnosticConsumer Quiet;
      gopp::DiagnosticsEngine Diags(SM, Quiet);
      for (gopp::ParsedFile &F : gopp::parseFiles(SM, P.Files, Diags, PO, Pool))
        N += F.AST->Decls.size();
    }
    return N;
  });
  // With one package after the other, the biggest file of each package is
  // the longest chain: no thread count can beat this speedup.
  uint64_t Chain = 0;
  for (auto &[Name, P] : Packages)
    Chain += P.BiggestFile;
  outs() << format("         package barrier: the biggest file of every "
                   "package adds up to %.1f%% of all lines, so the speedup is "
                   "at most %.1fx\n",
                   100.0 * Chain / TotalLines, (double)TotalLines / Chain);
  std::vector<gopp::FileID> All;
  for (auto &[Name, P] : Packages)
    All.insert(All.end(), P.Files.begin(), P.Files.end());
  RunParallel("all-par", [&] {
    gopp::CollectingDiagnosticConsumer Quiet;
    gopp::DiagnosticsEngine Diags(SM, Quiet, 0);
    uint64_t N = 0;
    for (gopp::ParsedFile &F : gopp::parseFiles(SM, All, Diags, PO, Pool))
      N += F.AST->Decls.size();
    return N;
  });
  outs() << "(checksum " << Sink << ")\n";
  outs() << "budget: >= 1.00 M lines/s/core: "
         << (SerialRate >= 1e6 ? "met" : "NOT met") << "\n";
  return 0;
}
