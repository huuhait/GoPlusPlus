//===- TypeCheckThroughput.cpp - Type-check throughput (P03, D3.10) -------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Measures how fast the checker type-checks a package, in lines per second per
// core (D3.10 budget: at least 250k lines/s per core). The input is generated
// in memory: N copies of a block of declarations (struct with methods, generic
// function, slices, maps, loops, closures, constants), so the numbers do not
// depend on files on disk. Lexing and parsing are done once, outside the timed
// region.
//
//   gopp-bench-check [-funcs N] [-runs N] [-threads=1,2,4,8,16]
//
// The first result is the best of N runs of checkPackage on one thread (CPU
// time): the number of the D3.10 budget. -threads then measures phase 2 (the
// function bodies) on that many threads, as wall-clock time, and prints the
// speedup over one thread (P03.10). Only a Release build counts: the Dev build
// has -O1 and assertions.
//
//===----------------------------------------------------------------------===//

#include "gopp/AST/ASTContext.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/Parse/Parser.h"
#include "gopp/Sema/Sema.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>

using namespace llvm;

static cl::opt<unsigned> Funcs("funcs", cl::desc("Number of generated blocks"),
                               cl::init(3000));
static cl::opt<unsigned> Runs("runs", cl::desc("Runs; the best counts"),
                              cl::init(5));
static cl::list<unsigned> Threads("threads", cl::CommaSeparated,
                                  cl::desc("Thread counts to measure"));

namespace {

class NoImports : public gopp::sema::Importer {
public:
  gopp::sema::Package *import(StringRef, std::string &Error) override {
    Error = "imports are not supported in the benchmark";
    return nullptr;
  }
};

/// One block of about 38 lines; the names get the suffix N.
std::string block(unsigned N) {
  std::string S = std::to_string(N);
  std::string B;
  B += "type Point" + S + " struct {\n\tX, Y int\n}\n";
  B += "func (p Point" + S + ") Sum() int { return p.X + p.Y }\n";
  B += "func (p *Point" + S +
       ") Move(dx, dy int) {\n\tp.X += dx\n\tp.Y += dy\n}\n";
  B += "const Limit" + S + " = 1 << 10\n";
  B += "func Map" + S + "[T, U any](xs []T, f func(T) U) []U {\n";
  B += "\tout := make([]U, 0, len(xs))\n\tfor _, x := range xs {\n";
  B += "\t\tout = append(out, f(x))\n\t}\n\treturn out\n}\n";
  B += "func Work" + S + "(n int) (int, error) {\n";
  B += "\tpts := []Point" + S + "{{1, 2}, {3, 4}}\n";
  B += "\tcounts := map[int]int{}\n";
  B += "\ttotal := 0\n";
  B += "\tfor i := 0; i < n && i < Limit" + S + "; i++ {\n";
  B += "\t\tp := pts[i%len(pts)]\n";
  B += "\t\tp.Move(i, -i)\n";
  B += "\t\tcounts[p.X] += p.Sum()\n";
  B += "\t\tif total > 100 {\n\t\t\ttotal -= 100\n\t\t} else {\n\t\t\ttotal += "
       "i * 2\n\t\t}\n";
  B += "\t}\n";
  B += "\tsums := Map" + S + "(pts, func(p Point" + S +
       ") int { return p.Sum() })\n";
  B += "\tfor _, s := range sums {\n\t\ttotal += s\n\t}\n";
  B += "\tswitch {\n\tcase total < 0:\n\t\treturn 0, "
       "nil\n\tdefault:\n\t\treturn total + len(counts), nil\n\t}\n";
  B += "}\n";
  return B;
}

double cpuSeconds() {
  std::chrono::nanoseconds User, Sys;
  sys::TimePoint<> Wall;
  sys::Process::GetTimeUsage(Wall, User, Sys);
  return std::chrono::duration<double>(User + Sys).count();
}

double wallSeconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv,
                              "Go++ type-check throughput benchmark\n");
  std::string Src = "package bench\n";
  for (unsigned I = 0; I < Funcs; ++I)
    Src += block(I);
  uint64_t Lines =
      static_cast<uint64_t>(std::count(Src.begin(), Src.end(), '\n'));

  gopp::SourceManager SM;
  auto F = SM.addBuffer(MemoryBuffer::getMemBufferCopy(Src, "bench.gopp"));
  if (!F) {
    errs() << "gopp-bench-check: cannot add the buffer\n";
    return 1;
  }
  gopp::CollectingDiagnosticConsumer Quiet;
  gopp::DiagnosticsEngine ParseDiags(SM, Quiet, 0);
  gopp::ast::ASTContext AC;
  gopp::ParseOptions PO;
  gopp::ast::File *AST = gopp::parseFile(AC, SM, *F, ParseDiags, PO);
  if (ParseDiags.hasErrors()) {
    errs() << "gopp-bench-check: the generated source has syntax errors\n";
    return 1;
  }
  const gopp::ast::File *Files[] = {AST};

  // One run of checkPackage with the given number of body threads.
  uint64_t Errors = 0;
  auto Once = [&](unsigned Threads, double (*Clock)()) {
    gopp::CollectingDiagnosticConsumer Sink;
    gopp::DiagnosticsEngine Diags(SM, Sink, 0);
    gopp::sema::TypeContext Ctx;
    NoImports Imp;
    gopp::sema::CheckOptions Opts;
    Opts.BodyThreads = Threads;
    double T0 = Clock();
    auto CP =
        gopp::sema::checkPackage(Ctx, SM, "bench", Files, Imp, Diags, Opts);
    double T = Clock() - T0;
    Errors = Sink.Diags.size();
    if (!CP) {
      errs() << "gopp-bench-check: checkPackage failed\n";
      exit(1);
    }
    return T;
  };
  double Best = 1e30;
  for (unsigned R = 0; R < Runs; ++R)
    Best = std::min(Best, Once(1, cpuSeconds));
  if (Errors) {
    errs() << "gopp-bench-check: the generated source has " << Errors
           << " type errors\n";
    return 1;
  }
  outs() << "corpus: " << Funcs << " blocks, " << Lines << " lines\n";
  outs() << format(
      "check (1 thread): best of %u: %.3f s  %.0f k lines/s/core\n",
      Runs.getValue(), Best, Lines / Best / 1e3);
  if (Threads.empty())
    return 0;
  // Wall-clock time per thread count; the speedup is over the same function
  // run with one thread.
  double Base = 0;
  for (unsigned T : Threads) {
    double W = 1e30;
    for (unsigned R = 0; R < Runs; ++R)
      W = std::min(W, Once(T, wallSeconds));
    if (!Base)
      Base = W;
    outs() << format("check (%u threads): wall %.4f s  %.0f k lines/s  "
                     "speedup %.2fx\n",
                     T, W, Lines / W / 1e3, Base / W);
  }
  return 0;
}
