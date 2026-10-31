//===- gopp.cpp - The gopp command line driver ----------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Subcommands are llvm::cl::SubCommand objects, as in llvm-pdbutil.
//
//===----------------------------------------------------------------------===//

#include "gopp/AST/ASTDumper.h"
#include "gopp/Basic/DiagnosticExplain.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/Basic/Version.h"
#include "gopp/Build/HostToolchain.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/Linker.h"
#include "gopp/CodeGen/Pipeline.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/Format/Format.h"
#include "gopp/GIR/Lower.h"
#include "gopp/GIR/Print.h"
#include "gopp/GIR/Serialize.h"
#include "gopp/GIR/Verify.h"
#include "gopp/Import/ClangInfo.h"
#include "gopp/Lex/Lexer.h"
#include "gopp/Mangle/SymbolName.h"
#include "gopp/Parse/Parser.h"
#include "gopp/Sema/ExampleMocks.h"
#include "gopp/Sema/Loader.h"
#include "gopp/TestRun/Executor.h"
#include "gopp/TestRun/Runner.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <algorithm>
#include <random>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

using namespace llvm;

static cl::SubCommand VersionCmd("version",
                                 "Print the Go++, LLVM and target versions");
static cl::SubCommand
    HostToolchainCmd("host-toolchain",
                     "Print the discovered host C library (headers, crt "
                     "objects, dynamic linker)");

static cl::SubCommand ParseCmd("parse",
                               "Parse Go++ files and report syntax errors");
static cl::list<std::string> ParseInputs(cl::Positional, cl::desc("<files>"),
                                         cl::sub(ParseCmd));
static cl::opt<bool> DumpAST("dump-ast", cl::desc("Print the syntax tree"),
                             cl::sub(ParseCmd));
static cl::opt<bool> DumpTokens("dump-tokens", cl::desc("Print the tokens"),
                                cl::sub(ParseCmd));
static cl::opt<bool>
    DumpComments("dump-comments",
                 cl::desc("With -dump-tokens: include comment tokens"),
                 cl::sub(ParseCmd));

static cl::opt<bool> LanguageRules(
    "language-rules",
    cl::desc("Report the Go++ rules the parser can check (&T{...}, redeclared "
             "loop variables, ...); -language-rules=false for Go sources"),
    cl::init(true), cl::sub(ParseCmd));
static cl::opt<unsigned>
    Jobs("j", cl::desc("Threads that parse the files (0 = one per core)"),
         cl::init(0), cl::sub(ParseCmd));
static cl::opt<bool>
    FixIt("fixit",
          cl::desc("Apply the fix-its of the diagnostics to the files in "
                   "place, then check the result (like clang -fixit)"),
          cl::sub(ParseCmd));
static cl::opt<bool> FixItStdout(
    "fixit-stdout",
    cl::desc("Like -fixit, but print the fixed text of the single input file "
             "to stdout and leave the file alone"),
    cl::sub(ParseCmd));

enum class DiagFormat { Text, JSON };
static cl::opt<DiagFormat>
    DiagFormatOpt("diag-format", cl::desc("Diagnostic output format"),
                  cl::values(clEnumValN(DiagFormat::Text, "text",
                                        "Clang-style text with carets"),
                             clEnumValN(DiagFormat::JSON, "json",
                                        "One JSON object per line")),
                  cl::init(DiagFormat::Text), cl::sub(ParseCmd));
static cl::opt<unsigned> ErrorLimit(
    "error-limit", cl::desc("Stop after this many errors (0 = no limit)"),
    cl::init(gopp::DiagnosticsEngine::DefaultErrorLimit), cl::sub(ParseCmd));

static cl::SubCommand CheckCmd("check",
                               "Type-check Go++ files or a package directory");
static cl::SubCommand
    VetCmd("vet", "Type-check like `check`, then report the vet warnings "
                  "(drop on a zero value, a ! that "
                  "panics outside main, defer in a loop, @Strict with no "
                  "required field)");
static cl::opt<bool>
    VetWerror("Werror", cl::desc("Exit with status 1 if there are warnings"),
              cl::sub(VetCmd));
static cl::list<std::string> CheckInputs(cl::Positional,
                                         cl::desc("<files or directory>"),
                                         cl::sub(CheckCmd), cl::sub(VetCmd));
static cl::opt<bool>
    CheckDumpTypes("dump-types",
                   cl::desc("Print the type of every expression and object"),
                   cl::sub(CheckCmd), cl::sub(VetCmd));
static cl::opt<bool>
    CheckGoMode("go",
                cl::desc("Check Go sources: Go's built-ins, no ownership "
                         "rules, no Go++ language rules"),
                cl::sub(CheckCmd), cl::sub(VetCmd));
static cl::opt<std::string> CheckOS("os",
                                    cl::desc("Target OS for build constraints"),
                                    cl::init("linux"), cl::sub(CheckCmd),
                                    cl::sub(VetCmd));
static cl::opt<std::string>
    CheckArch("arch", cl::desc("Target architecture for build constraints"),
              cl::init("amd64"), cl::sub(CheckCmd), cl::sub(VetCmd));
static cl::opt<std::string>
    CheckProfile("profile",
                 cl::desc("Build profile name (true in //gopp:build)"),
                 cl::init("dev"), cl::sub(CheckCmd), cl::sub(VetCmd));
static cl::list<std::string> CheckTags("tags", cl::CommaSeparated,
                                       cl::desc("Extra build tags"),
                                       cl::sub(CheckCmd), cl::sub(VetCmd));
static cl::opt<bool> CheckTests("test", cl::desc("Include _test.gopp files"),
                                cl::sub(CheckCmd), cl::sub(VetCmd));
static cl::opt<unsigned>
    CheckErrorLimit("check-error-limit",
                    cl::desc("Stop after this many errors (0 = no limit)"),
                    cl::init(gopp::DiagnosticsEngine::DefaultErrorLimit),
                    cl::sub(CheckCmd), cl::sub(VetCmd));
static cl::opt<DiagFormat> CheckDiagFormat(
    "check-diag-format", cl::desc("Diagnostic output format"),
    cl::values(clEnumValN(DiagFormat::Text, "text", "Clang-style text"),
               clEnumValN(DiagFormat::JSON, "json", "JSON lines")),
    cl::init(DiagFormat::Text), cl::sub(CheckCmd), cl::sub(VetCmd));

// TEST ONLY, hidden: the stand-ins for the standard library (P13) and for the
// C/C++ headers the spec examples import (P07, P08), for
// tests/sema/spec-examples-stubs.test. A normal `gopp check` has neither.
static cl::opt<std::string> CheckTestStubs(
    "test-stubs",
    cl::desc("TEST ONLY: look up the standard packages in this directory "
             "(declaration-only stubs) and mock zlib.h, <vector> and "
             "<algorithm>"),
    cl::Hidden, cl::sub(CheckCmd), cl::sub(VetCmd));

static cl::opt<unsigned> CheckJobs(
    "check-j",
    cl::desc("Threads that check function bodies (0 = one per core, at most "
             "8; 1 = the calling thread). The output does not depend on it."),
    cl::init(0), cl::sub(CheckCmd), cl::sub(VetCmd));

static cl::SubCommand ExplainCmd("explain",
                                 "Explain an error code: gopp explain E0042");
static cl::list<std::string>
    ExplainCodes(cl::Positional, cl::desc("<E0000 ...>"), cl::sub(ExplainCmd));
static cl::opt<bool> ExplainList("list",
                                 cl::desc("List every code with its message"),
                                 cl::sub(ExplainCmd));

static cl::SubCommand
    DemangleCmd("demangle",
                "Print Go++ symbol names (_GP...) as readable text; with no "
                "names, filter standard input like c++filt");
static cl::list<std::string> DemangleNames(cl::Positional,
                                           cl::desc("[symbol names]"),
                                           cl::sub(DemangleCmd));
static cl::opt<unsigned> DemangleMaxOut(
    "max-output",
    cl::desc("Longest readable name printed, in bytes (a repeated type may "
             "be astronomically long once written out)"),
    cl::init(1u << 20), cl::sub(DemangleCmd));

static cl::SubCommand
    FmtCmd("fmt", "Format Go++ files (gofmt's rules, extended to Go++)");
static cl::list<std::string> FmtInputs(cl::Positional,
                                       cl::desc("[files or directories]"),
                                       cl::sub(FmtCmd));
static cl::opt<bool> FmtList("l",
                             cl::desc("List files whose formatting differs"),
                             cl::sub(FmtCmd));
static cl::opt<bool> FmtWrite("w",
                              cl::desc("Write the result to the source file "
                                       "instead of stdout"),
                              cl::sub(FmtCmd));
static cl::opt<bool> FmtDiff("d", cl::desc("Print diffs instead of the result"),
                             cl::sub(FmtCmd));
static cl::opt<bool> FmtNoSortImports("no-sort-imports",
                                      cl::desc("Do not sort import groups"),
                                      cl::sub(FmtCmd));

static cl::SubCommand
    BuildCmd("build", "Compile a package: GIR, LLVM IR, an object or an "
                      "executable");
static cl::list<std::string> BuildInputs(cl::Positional,
                                         cl::desc("<files or directory>"),
                                         cl::sub(BuildCmd));
enum class Emit { GIR, GIRBitstream, LLVM, Asm, Obj, Exe };
static cl::opt<Emit> BuildEmit(
    "emit", cl::desc("What to produce"),
    cl::values(clEnumValN(Emit::GIR, "gir", "GIR text (stdout without -o)"),
               clEnumValN(Emit::GIRBitstream, "girb",
                          "GIR bitstream (D4.15); needs -o"),
               clEnumValN(Emit::LLVM, "llvm", "LLVM IR text"),
               clEnumValN(Emit::Asm, "asm", "assembly"),
               clEnumValN(Emit::Obj, "obj", "an object file"),
               clEnumValN(Emit::Exe, "exe", "a linked executable (default)")),
    cl::init(Emit::Exe), cl::sub(BuildCmd));
static cl::opt<bool> BuildDumpGIR("dump-gir", cl::desc("Same as -emit=gir"),
                                  cl::sub(BuildCmd));
static cl::opt<std::string>
    BuildTarget("target", cl::desc("Target: linux/amd64, linux/arm64 or host"),
                cl::init("host"), cl::sub(BuildCmd));
static cl::opt<std::string>
    BuildCrtDir("crt-dir",
                cl::desc("Directory with crt1.o, crti.o, crtn.o and libc, for "
                         "a target other than the host"),
                cl::sub(BuildCmd));
static cl::opt<std::string>
    BuildDynamicLinker("dynamic-linker",
                       cl::desc("Path of the dynamic linker the program asks "
                                "for at run time"),
                       cl::sub(BuildCmd));
static cl::list<std::string>
    BuildLinkObjs("link-obj", cl::Hidden,
                  cl::desc("TEST ONLY: link this object file into the "
                           "program (the C an @Extern(\"C\") test calls)"),
                  cl::value_desc("file"), cl::sub(BuildCmd));
static cl::opt<std::string>
    BuildOpt("opt",
             cl::desc("Optimization level, which picks the codegen pipeline "
                      "(D6.6): 0 (default; O0 code generation and a small "
                      "IR pass set), 1, 2, 3, s or z"),
             cl::value_desc("level"), cl::init("0"), cl::sub(BuildCmd));
static cl::opt<std::string> BuildPipeline(
    "pipeline",
    cl::desc("fastisel: plain O0 with FastISel and no GIR or IR pass (the "
             "measuring baseline of D6.8); default: the pipeline -opt picks"),
    cl::value_desc("name"), cl::init("default"), cl::sub(BuildCmd));
static cl::opt<std::string>
    BuildISel("isel", cl::Hidden,
              cl::desc("MEASURING: force the instruction selector of O0 code "
                       "generation: fastisel, globalisel or default"),
              cl::init("default"), cl::sub(BuildCmd));
static cl::opt<bool>
    BuildTimes("pipeline-times", cl::Hidden,
               cl::desc("MEASURING: print each stage as name=wall/cpu "
                        "seconds to standard error"),
               cl::sub(BuildCmd));
static cl::opt<bool> BuildNoDebug("no-debug", cl::desc("No DWARF"),
                                  cl::sub(BuildCmd));
static cl::opt<bool> BuildLinkageNames(
    "dwarf-linkage-names", cl::Hidden,
    cl::desc("Write the _GP symbol as DW_AT_linkage_name too (gdb and lldb "
             "then show it instead of the readable name)"),
    cl::sub(BuildCmd));
static cl::opt<bool> BuildNoLocs("gir-no-locs",
                                 cl::desc("With -emit=gir: leave out the "
                                          "source positions"),
                                 cl::sub(BuildCmd));
static cl::opt<std::string> BuildOutput("o", cl::desc("Output file"),
                                        cl::value_desc("file"),
                                        cl::sub(BuildCmd));
static cl::opt<bool> BuildTestIntrinsics(
    "test-intrinsics", cl::Hidden,
    cl::desc("TEST ONLY: @Extern(\"C\") functions named gopp_test_* are "
             "intrinsics that build slices and maps around a counting "
             "allocator (P04.25, ADR-0029)"),
    cl::sub(BuildCmd));
static cl::opt<std::string>
    BuildTestStubs("build-test-stubs", cl::Hidden,
                   cl::desc("TEST ONLY: standard packages from this "
                            "directory"),
                   cl::sub(BuildCmd));

static cl::SubCommand
    TestRunCmd("test-run", "Compile many programs in this process and run "
                           "them in executor processes (JIT); one JSON line "
                           "of result per program");
static cl::list<std::string>
    TestRunInputs(cl::Positional, cl::desc("<program files or directories>"),
                  cl::sub(TestRunCmd));
static cl::opt<std::string>
    TestRunManifest("manifest",
                    cl::desc("JSON lines of jobs (id, inputs, target, "
                             "timeout_ms); - is standard input"),
                    cl::value_desc("file"), cl::sub(TestRunCmd));
static cl::opt<std::string> TestRunOutput("o",
                                          cl::desc("Results file (default: "
                                                   "standard output)"),
                                          cl::value_desc("file"),
                                          cl::sub(TestRunCmd));
static cl::opt<unsigned>
    TestRunJobs("j", cl::desc("Compile threads (default: one per core)"),
                cl::init(0), cl::sub(TestRunCmd));
static cl::opt<unsigned> TestRunExecutors(
    "executors",
    cl::desc("Executor processes (default: a quarter of the cores, 2 to 16)"),
    cl::init(0), cl::sub(TestRunCmd));
static cl::opt<unsigned> TestRunTimeout("timeout-ms",
                                        cl::desc("Run time limit of a program"),
                                        cl::init(60000), cl::sub(TestRunCmd));
static cl::opt<std::string>
    TestRunWorkDir("work-dir",
                   cl::desc("Keep objects and outputs here (default: a "
                            "temporary directory, removed at the end)"),
                   cl::sub(TestRunCmd));
static cl::opt<uint64_t> TestRunMaxOutput(
    "max-output",
    cl::desc("Most bytes a program may write to one stream; past it the "
             "program gets SIGXFSZ (0 = no limit)"),
    cl::init(256ull << 20), cl::sub(TestRunCmd));
static cl::opt<bool>
    TestRunMergeOutput("merge-output",
                       cl::desc("Standard output and error of every program "
                                "in one stream, in the order written (2>&1)"),
                       cl::sub(TestRunCmd));
static cl::opt<bool> TestRunSummary("summary",
                                    cl::desc("Print a summary to stderr"),
                                    cl::sub(TestRunCmd));
// Test hooks (tests/run/runner-failures.test, runner-determinism.test).
static cl::opt<std::string>
    TestRunExecutor("executor", cl::Hidden,
                    cl::desc("TEST ONLY: start this program as the executor"),
                    cl::sub(TestRunCmd));
static cl::opt<unsigned> TestRunStartupTimeout(
    "startup-timeout-ms", cl::Hidden,
    cl::desc("TEST ONLY: how long an executor may take to start"),
    cl::init(20000), cl::sub(TestRunCmd));
static cl::opt<bool> TestRunShareExecutors(
    "share-executors", cl::Hidden,
    cl::desc("TEST ONLY: programs that keep C library state share executors "
             "too"),
    cl::init(false), cl::sub(TestRunCmd));
static cl::opt<std::string>
    TestRunSchedule("schedule", cl::Hidden,
                    cl::desc("TEST ONLY: the order programs are compiled and "
                             "run in: forward, reverse or shuffle:SEED"),
                    cl::init("forward"), cl::sub(TestRunCmd));
static cl::opt<std::string>
    TestRunTestStubs("build-test-stubs", cl::Hidden,
                     cl::desc("TEST ONLY: standard packages from this "
                              "directory"),
                     cl::sub(TestRunCmd));

static cl::SubCommand GirCmd("gir", "Work with GIR files: gopp gir dump "
                                    "<file.girb>");
static cl::list<std::string> GirArgs(cl::Positional, cl::desc("dump <file>"),
                                     cl::sub(GirCmd));

static int runVersion() {
  outs() << "gopp version " << gopp::getGoppVersion() << '\n'
         << "LLVM version " << gopp::getLLVMVersion() << '\n'
         << "Default target: " << gopp::getDefaultTargetTriple()
         << '\n'
         // The Clang and LLD libraries linked into gopp (D0.2).
         << "Clang: " << gopp::getClangVersion() << '\n'
         << "LLD: " << gopp::getLLDVersion() << '\n';
  return 0;
}

static int runHostToolchain() {
  Expected<gopp::HostToolchain> TC = gopp::probeHostToolchain();
  if (!TC) {
    errs() << "gopp: error: " << toString(TC.takeError()) << '\n';
    return 1;
  }
  TC->print(outs());
  return 0;
}

/// Passes diagnostics on and keeps a copy (for -fixit).
class TeeConsumer : public gopp::DiagnosticConsumer {
public:
  explicit TeeConsumer(gopp::DiagnosticConsumer &Out) : Out(Out) {}
  void handleDiagnostic(const gopp::Diagnostic &D) override {
    Seen.push_back(D);
    Out.handleDiagnostic(D);
  }
  void finish() override { Out.finish(); }
  std::vector<gopp::Diagnostic> Seen;

private:
  gopp::DiagnosticConsumer &Out;
};

/// Does Text have syntax errors?
static bool hasErrors(StringRef Text, const gopp::ParseOptions &PO) {
  gopp::SourceManager SM;
  Expected<gopp::FileID> F = SM.addMemoryBuffer("fixed", Text);
  if (!F) {
    consumeError(F.takeError());
    return true;
  }
  gopp::CollectingDiagnosticConsumer Quiet;
  gopp::DiagnosticsEngine Diags(SM, Quiet, 0);
  gopp::ast::ASTContext Ctx;
  gopp::parseFile(Ctx, SM, *F, Diags, PO);
  return Diags.hasErrors();
}

/// Replaces the contents of Path. Callers compute everything they need from
/// the old contents first: the SourceManager may have the file mapped.
static bool writeFile(StringRef Path, StringRef Text) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_None);
  if (!EC) {
    OS << Text;
    OS.close();
    EC = OS.error();
  }
  if (EC) {
    errs() << "gopp: error: " << Path << ": " << EC.message() << '\n';
    OS.clear_error();
    return false;
  }
  return true;
}

/// With several input files each dump starts with `==> path <==` (as tail
/// does); the dump of a single file has no header.
static void dumpHeader(const gopp::SourceManager &SM, gopp::FileID F,
                       size_t NumFiles) {
  if (NumFiles > 1)
    outs() << "==> " << SM.getFilename(F) << " <==\n";
}

/// `gopp parse`: all files are one package (one diagnostics engine, one error
/// limit), parsed in parallel with -j (the output does not depend on it,
/// except that past the error limit a parallel run still builds every tree).
/// Diagnostics go to stderr, dumps to stdout.
static int runParse() {
  if (ParseInputs.empty()) {
    errs() << "gopp: error: parse: no input files\n";
    return 1;
  }
  gopp::SourceManager SM;
  std::vector<gopp::FileID> Files;
  for (const std::string &Path : ParseInputs) {
    llvm::Expected<gopp::FileID> F = [&]() -> llvm::Expected<gopp::FileID> {
      if (Path != "-")
        return SM.addFile(Path);
      auto Buf = MemoryBuffer::getSTDIN();
      if (!Buf)
        return llvm::errorCodeToError(Buf.getError());
      return SM.addBuffer(std::move(*Buf));
    }();
    if (!F) {
      errs() << "gopp: error: " << toString(F.takeError()) << '\n';
      return 1;
    }
    Files.push_back(*F);
  }

  if (FixItStdout && Files.size() != 1) {
    errs()
        << "gopp: error: parse: -fixit-stdout needs exactly one input file\n";
    return 1;
  }

  std::unique_ptr<gopp::DiagnosticConsumer> Printer;
  if (DiagFormatOpt == DiagFormat::JSON)
    Printer = std::make_unique<gopp::JSONDiagnosticPrinter>(errs(), SM);
  else
    Printer = std::make_unique<gopp::TextDiagnosticPrinter>(errs(), SM);
  TeeConsumer Consumer(*Printer);
  gopp::DiagnosticsEngine Diags(SM, Consumer, ErrorLimit);

  gopp::ParseOptions PO;
  PO.CheckLanguageRules = LanguageRules;
  if (DumpTokens && !DumpAST) {
    // Lexing only: the lexer reports its own errors.
    for (gopp::FileID F : Files) {
      dumpHeader(SM, F, Files.size());
      gopp::dumpTokens(outs(), SM, F, Diags, DumpComments);
    }
  } else {
    std::vector<gopp::ParsedFile> Parsed =
        gopp::parseFiles(SM, Files, Diags, PO, Jobs);
    for (size_t I = 0; I < Files.size(); ++I) {
      if (DumpTokens || DumpAST)
        dumpHeader(SM, Files[I], Files.size());
      if (DumpTokens) {
        // The parse reported the lexer errors; the dump stays quiet.
        gopp::CollectingDiagnosticConsumer Quiet;
        gopp::DiagnosticsEngine QuietDiags(SM, Quiet, 0);
        gopp::dumpTokens(outs(), SM, Files[I], QuietDiags, DumpComments);
      }
      if (DumpAST)
        gopp::ast::dump(outs(), SM, Parsed[I].AST);
    }
  }
  Diags.finish();
  if (!FixIt && !FixItStdout)
    return Diags.hasErrors() ? 1 : 0;

  // Apply the fix-its, then parse the result again: the exit status says
  // whether the fixed files are clean. All texts are made before any file is
  // written (the SourceManager may have the files mapped).
  std::vector<std::string> Fixed(Files.size());
  std::vector<unsigned> Applied(Files.size(), 0);
  for (size_t I = 0; I < Files.size(); ++I)
    Fixed[I] = gopp::applyFixIts(SM, Files[I], Consumer.Seen, &Applied[I]);
  // Standard input cannot be rewritten: say so before any file is written.
  if (!FixItStdout)
    for (size_t I = 0; I < Files.size(); ++I)
      if (Applied[I] && ParseInputs[I] == "-") {
        errs() << "gopp: error: parse: -fixit cannot write standard input; "
                  "use -fixit-stdout\n";
        return 1;
      }
  bool HadErrors = false;
  for (size_t I = 0; I < Files.size(); ++I) {
    StringRef Path = SM.getFilename(Files[I]);
    if (FixItStdout) {
      outs() << Fixed[I];
    } else if (Applied[I]) {
      if (!writeFile(Path, Fixed[I]))
        return 1;
      errs() << "gopp: applied " << Applied[I] << " fix-it"
             << (Applied[I] == 1 ? "" : "s") << " to " << Path << '\n';
    }
    HadErrors |= hasErrors(Fixed[I], PO);
  }
  return HadErrors ? 1 : 0;
}

/// The exit status of `gopp check` and `gopp vet`: 1 on errors, and for
/// `gopp vet -Werror` on warnings too.
static int checkStatus(const gopp::sema::CheckedPackage *CP,
                       const gopp::DiagnosticsEngine &Diags, bool Vet) {
  if (!CP || Diags.hasErrors())
    return 1;
  return Vet && VetWerror && Diags.getNumWarnings() ? 1 : 0;
}

/// `gopp check <files|dir>`: loads the package (and what it imports from the
/// directory of the input), type-checks it and exits 1 on errors. `gopp vet`
/// is the same plus the vet lints on that package, which are warnings: they
/// leave the exit status alone unless -Werror is given.
static int runCheck(bool Vet) {
  if (CheckInputs.empty()) {
    errs() << "gopp: error: " << (Vet ? "vet" : "check")
           << ": no input files\n";
    return 1;
  }
  gopp::SourceManager SM;
  std::unique_ptr<gopp::DiagnosticConsumer> Printer;
  if (CheckDiagFormat == DiagFormat::JSON)
    Printer = std::make_unique<gopp::JSONDiagnosticPrinter>(errs(), SM);
  else
    Printer = std::make_unique<gopp::TextDiagnosticPrinter>(errs(), SM);
  gopp::DiagnosticsEngine Diags(SM, *Printer, CheckErrorLimit);

  gopp::sema::LoadOptions LO;
  LO.OS = CheckOS;
  LO.Arch = CheckArch;
  LO.Profile = CheckProfile;
  LO.Tags.assign(CheckTags.begin(), CheckTags.end());
  LO.IncludeTests = CheckTests;
  LO.LanguageRules = !CheckGoMode;
  LO.StdRoot = CheckTestStubs;
  gopp::sema::CheckOptions CO;
  CO.GoCompat = CheckGoMode;
  CO.Ownership = !CheckGoMode;
  CO.BodyThreads = CheckJobs;
  CO.Vet = Vet;
  gopp::sema::TypeContext Ctx(CheckGoMode);

  gopp::sema::CheckedPackage *CP = nullptr;
  gopp::sema::MockForeignProvider Foreign;
  std::unique_ptr<gopp::sema::ExampleHeaderMocks> TestMocks;
  if (!CheckTestStubs.empty())
    TestMocks = std::make_unique<gopp::sema::ExampleHeaderMocks>(Ctx, Foreign);
  if (CheckInputs.size() == 1 && llvm::sys::fs::is_directory(CheckInputs[0])) {
    LO.Root = CheckInputs[0];
    gopp::sema::Loader L(SM, Diags, Ctx, LO, CO);
    L.setForeignProvider(&Foreign);
    CP = L.loadDir(CheckInputs[0], "");
    if (CP && CheckDumpTypes)
      gopp::sema::dumpTypes(outs(), SM, *CP, L.filesOf(CP));
    Diags.finish();
    return checkStatus(CP, Diags, Vet);
  }
  LO.Root = llvm::sys::path::parent_path(CheckInputs[0]).str();
  if (LO.Root.empty())
    LO.Root = ".";
  gopp::sema::Loader L(SM, Diags, Ctx, LO, CO);
  L.setForeignProvider(&Foreign);
  CP = L.loadFiles(
      std::vector<std::string>(CheckInputs.begin(), CheckInputs.end()));
  if (CP && CheckDumpTypes)
    gopp::sema::dumpTypes(outs(), SM, *CP, L.filesOf(CP));
  Diags.finish();
  return checkStatus(CP, Diags, Vet);
}

/// Writes Bytes to OutFile, or stdout if it is empty.
static bool writeOutput(StringRef OutFile, StringRef Bytes) {
  if (OutFile.empty() || OutFile == "-") {
    outs() << Bytes;
    return true;
  }
  if (llvm::Error E =
          gopp::writeFileAtomic(OutFile, [&](llvm::raw_pwrite_stream &OS) {
            OS << Bytes;
            return llvm::Error::success();
          })) {
    errs() << "gopp: error: " << OutFile << ": " << toString(std::move(E))
           << '\n';
    return false;
  }
  return true;
}

static std::string SelfPath;

/// Reports a back-end failure as an error diagnostic.
static int reportCodegen(gopp::SourceManager &SM,
                         gopp::DiagnosticsEngine &Diags, llvm::Error E) {
  (void)SM;
  Diags.report(gopp::SourceLoc(), gopp::diag::err_codegen)
      << toString(std::move(E));
  Diags.finish();
  return 1;
}

/// `gopp build` from GIR on: LLVM IR, assembly, an object, an executable.
static int codegenAndLink(Emit What, gopp::SourceManager &SM,
                          gopp::DiagnosticsEngine &Diags,
                          gopp::gir::Module &M) {
  Expected<gopp::TargetSpec> T = gopp::parseTarget(BuildTarget);
  if (!T)
    return reportCodegen(SM, Diags, T.takeError());
  if (BuildPipeline != "default" && BuildPipeline != "fastisel") {
    errs() << "gopp: error: build: unknown -pipeline '" << BuildPipeline
           << "' (use fastisel)\n";
    return 1;
  }
  gopp::PipelineRequest Req;
  Req.Opt = BuildOpt;
  Req.Baseline = BuildPipeline == "fastisel";
  Req.Selector = BuildISel;
  Expected<gopp::PipelineSpec> Spec = gopp::selectPipeline(Req, *T);
  if (!Spec)
    return reportCodegen(SM, Diags, Spec.takeError());
  Expected<std::unique_ptr<llvm::TargetMachine>> TM =
      gopp::createTargetMachine(*T, *Spec);
  if (!TM)
    return reportCodegen(SM, Diags, TM.takeError());
  gopp::PipelineTimes Times;
  // Prints the stage times of this build when it ends, however it ends.
  struct PrintTimes {
    const gopp::PipelineSpec &Spec;
    gopp::PipelineTimes &T;
    ~PrintTimes() {
      if (!BuildTimes)
        return;
      errs() << "gopp: pipeline " << Spec.Name << ":";
      for (const auto &S : T.Stages)
        errs() << ' ' << S.Name << '=' << llvm::format("%.4f", S.Seconds) << '/'
               << llvm::format("%.4f", S.CpuSeconds);
      errs() << '\n';
    }
  } Report{*Spec, Times};
  auto Timed = [&](const char *Name, auto &&Fn) {
    gopp::StageTimer T(Name, &Times);
    return Fn();
  };
  gopp::runGIRPasses(M, *Spec, &Times);
  llvm::LLVMContext LCtx;
  gopp::LowerToLLVMOptions LO;
  LO.DebugInfo = !BuildNoDebug;
  LO.DwarfLinkageNames = BuildLinkageNames;
  LO.KeepVariables = Spec->Opt == gopp::OptLevel::O0 && !Spec->Baseline;
  Expected<std::unique_ptr<llvm::Module>> LM =
      Timed("lower", [&] { return gopp::lowerToLLVM(LCtx, M, **TM, LO); });
  if (!LM)
    return reportCodegen(SM, Diags, LM.takeError());
  if (llvm::Error E = gopp::runIRPasses(**LM, **TM, *Spec, &Times))
    return reportCodegen(SM, Diags, std::move(E));
  if (What == Emit::LLVM) {
    std::string Text;
    raw_string_ostream OS(Text);
    (*LM)->print(OS, nullptr);
    return writeOutput(BuildOutput, Text) ? 0 : 1;
  }
  if (What == Emit::Asm) {
    std::string Text;
    raw_string_ostream OS(Text);
    if (llvm::Error E = Timed(
            "codegen", [&] { return gopp::emitAssembly(**LM, **TM, OS); }))
      return reportCodegen(SM, Diags, std::move(E));
    return writeOutput(BuildOutput, Text) ? 0 : 1;
  }
  // The name of the result: -o, else the first input without its extension.
  std::string Stem = llvm::sys::path::stem(BuildInputs[0]).str();
  if (Stem.empty() || Stem == ".")
    Stem = "a";
  if (What == Emit::Obj) {
    std::string Out =
        BuildOutput.empty() ? Stem + ".o" : BuildOutput.getValue();
    if (llvm::Error E =
            Timed("codegen", [&] { return gopp::emitObject(**LM, **TM, Out); }))
      return reportCodegen(SM, Diags, std::move(E));
    return 0;
  }
  Expected<gopp::LinkInputs> In =
      gopp::findLinkInputs(*T, BuildCrtDir, BuildDynamicLinker);
  if (!In)
    return reportCodegen(SM, Diags, In.takeError());
  std::string Out = BuildOutput.empty() ? Stem : BuildOutput.getValue();
  llvm::SmallString<128> Obj;
  if (std::error_code EC = llvm::sys::fs::createTemporaryFile("gopp", "o", Obj))
    return reportCodegen(SM, Diags, llvm::errorCodeToError(EC));
  llvm::FileRemover Remove(Obj);
  if (llvm::Error E =
          Timed("codegen", [&] { return gopp::emitObject(**LM, **TM, Obj); }))
    return reportCodegen(SM, Diags, std::move(E));
  std::vector<std::string> Objects = {std::string(Obj)};
  for (const std::string &O : BuildLinkObjs)
    Objects.push_back(O);
  if (llvm::Error E = gopp::linkExecutable(SelfPath, *T, *In, Objects, Out))
    return reportCodegen(SM, Diags, std::move(E));
  return 0;
}

/// The part of `gopp build` after the package is checked.
static int finishBuild(Emit What, gopp::SourceManager &SM,
                       gopp::DiagnosticsEngine &Diags, gopp::sema::Loader &L,
                       gopp::sema::CheckedPackage *CP) {
  if (What == Emit::Exe && CP->Pkg && CP->Pkg->Name != "main") {
    Diags.finish();
    errs() << "gopp: error: build: package " << CP->Pkg->Name
           << " is not package main, so it is no program (-emit=obj, llvm, "
              "asm or gir build a library; linking libraries comes with P06)\n";
    return 1;
  }
  gopp::gir::Context GCtx;
  gopp::gir::LowerOptions LowerOpts;
  LowerOpts.TestIntrinsics = BuildTestIntrinsics;
  std::unique_ptr<gopp::gir::Module> M =
      gopp::gir::lowerPackage(GCtx, SM, *CP, L.filesOf(CP), Diags, LowerOpts);
  Diags.finish();
  if (!M)
    return 1;
  switch (What) {
  case Emit::GIR: {
    std::string Text;
    raw_string_ostream OS(Text);
    gopp::gir::PrintOptions PO;
    PO.Spans = !BuildNoLocs;
    gopp::gir::print(OS, *M, PO);
    return writeOutput(BuildOutput, Text) ? 0 : 1;
  }
  case Emit::GIRBitstream:
    if (BuildOutput.empty()) {
      errs() << "gopp: error: build: -emit=girb needs -o\n";
      return 1;
    }
    return writeOutput(BuildOutput, gopp::gir::writeModule(*M)) ? 0 : 1;
  default:
    break;
  }
  return codegenAndLink(What, SM, Diags, *M);
}

/// `gopp build`: load and check the package, lower it to GIR, then emit.
static int runBuild() {
  if (BuildInputs.empty()) {
    errs() << "gopp: error: build: no input files\n";
    return 1;
  }
  Emit What = BuildDumpGIR ? Emit::GIR : BuildEmit.getValue();
  gopp::SourceManager SM;
  gopp::TextDiagnosticPrinter Printer(errs(), SM);
  gopp::DiagnosticsEngine Diags(SM, Printer,
                                gopp::DiagnosticsEngine::DefaultErrorLimit);
  gopp::sema::LoadOptions LO;
  LO.StdRoot = BuildTestStubs;
  gopp::sema::CheckOptions CO;
  gopp::sema::TypeContext TCtx(false);
  gopp::sema::MockForeignProvider Foreign;
  bool IsDir =
      BuildInputs.size() == 1 && llvm::sys::fs::is_directory(BuildInputs[0]);
  if (IsDir) {
    LO.Root = BuildInputs[0];
  } else {
    LO.Root = llvm::sys::path::parent_path(BuildInputs[0]).str();
    if (LO.Root.empty())
      LO.Root = ".";
  }
  gopp::sema::Loader L(SM, Diags, TCtx, LO, CO);
  L.setForeignProvider(&Foreign);
  gopp::sema::CheckedPackage *CP =
      IsDir ? L.loadDir(BuildInputs[0], "")
            : L.loadFiles(std::vector<std::string>(BuildInputs.begin(),
                                                   BuildInputs.end()));
  if (!CP || Diags.hasErrors()) {
    Diags.finish();
    return 1;
  }
  return finishBuild(What, SM, Diags, L, CP);
}

/// `gopp test-run`: P04.19. Exit status 0 when every program was run, whatever
/// the programs did; 1 when the runner could not do its job (also for a
/// program that got no executor: outcome "internal").
static int runTestRun() {
  std::vector<gopp::testrun::JobSpec> Jobs;
  if (!TestRunManifest.empty()) {
    auto M = gopp::testrun::readManifest(TestRunManifest);
    if (!M) {
      errs() << "gopp: error: test-run: " << toString(M.takeError()) << '\n';
      return 1;
    }
    Jobs = std::move(*M);
  }
  StringSet<> Ids;
  for (const gopp::testrun::JobSpec &J : Jobs)
    Ids.insert(J.Id);
  for (const std::string &P : TestRunInputs) {
    if (!Ids.insert(P).second) {
      errs() << "gopp: error: test-run: '" << P << "' is given twice\n";
      return 1;
    }
    gopp::testrun::JobSpec J;
    J.Id = P;
    J.Inputs = {P};
    Jobs.push_back(std::move(J));
  }
  if (Jobs.empty()) {
    errs() << "gopp: error: test-run: no programs (give files or "
              "-manifest)\n";
    return 1;
  }
  if (TestRunMergeOutput)
    for (gopp::testrun::JobSpec &J : Jobs)
      J.MergeOutput = true;
  // The order the programs are handed to the compile threads (results are
  // the same in any order: runner-determinism.test).
  StringRef Sched = TestRunSchedule;
  if (Sched == "reverse") {
    std::reverse(Jobs.begin(), Jobs.end());
  } else if (Sched.consume_front("shuffle:")) {
    unsigned Seed = 0;
    if (Sched.getAsInteger(10, Seed)) {
      errs() << "gopp: error: test-run: bad -schedule\n";
      return 1;
    }
    std::mt19937 Rng(Seed);
    std::shuffle(Jobs.begin(), Jobs.end(), Rng);
  } else if (Sched != "forward") {
    errs() << "gopp: error: test-run: bad -schedule\n";
    return 1;
  }
  gopp::testrun::RunnerConfig Cfg;
  Cfg.Self = TestRunExecutor.empty() ? SelfPath : TestRunExecutor.getValue();
  Cfg.CompileThreads = TestRunJobs;
  Cfg.Executors = TestRunExecutors;
  Cfg.TimeoutMs = TestRunTimeout;
  Cfg.StartupTimeoutMs = TestRunStartupTimeout;
  Cfg.MaxOutputBytes = TestRunMaxOutput;
  Cfg.ShareExecutors = TestRunShareExecutors;
  Cfg.WorkDir = TestRunWorkDir;
  Cfg.KeepWorkDir = !TestRunWorkDir.empty();
  Cfg.Compile.StdRoot = TestRunTestStubs;

  std::unique_ptr<raw_fd_ostream> File;
  raw_ostream *Out = &outs();
  if (!TestRunOutput.empty() && TestRunOutput != "-") {
    std::error_code EC;
    File = std::make_unique<raw_fd_ostream>(TestRunOutput, EC);
    if (EC) {
      errs() << "gopp: error: " << TestRunOutput << ": " << EC.message()
             << '\n';
      return 1;
    }
    Out = File.get();
  }
  using gopp::testrun::Outcome;
  unsigned Counts[6] = {};
  gopp::testrun::RunStats Stats;
  llvm::Error E = gopp::testrun::runBatch(
      Jobs, Cfg,
      [&](gopp::testrun::JobResult &&R) {
        ++Counts[unsigned(R.O)];
        if (R.O == Outcome::Internal)
          errs() << "gopp: error: test-run: " << R.Id << ": " << R.Message
                 << '\n';
        // One line at a time, so a reader sees results as they come.
        *Out << gopp::testrun::encodeJobResult(R) << '\n';
        Out->flush();
      },
      &Stats);
  if (E) {
    errs() << "gopp: error: test-run: " << toString(std::move(E)) << '\n';
    return 1;
  }
  if (TestRunSummary) {
    errs() << "test-run: " << Jobs.size()
           << " programs: " << Counts[unsigned(Outcome::Exited)] << " exited, "
           << Counts[unsigned(Outcome::Signaled)] << " signaled, "
           << Counts[unsigned(Outcome::TimedOut)] << " timed out, "
           << Counts[unsigned(Outcome::CompileFailed)] << " did not compile, "
           << Counts[unsigned(Outcome::LoadFailed)] << " did not load, "
           << Counts[unsigned(Outcome::Internal)] << " internal errors; "
           << Stats.ProcessesStarted << " executor processes started\n";
  }
  return Counts[unsigned(Outcome::Internal)] ? 1 : 0;
}

/// `gopp gir dump file.girb`: the text form of a GIR bitstream.
static int runGir() {
  if (GirArgs.size() != 2 || GirArgs[0] != "dump") {
    errs() << "gopp: error: gir: usage: gopp gir dump <file.girb>\n";
    return 1;
  }
  auto Buf = MemoryBuffer::getFile(GirArgs[1]);
  if (!Buf) {
    errs() << "gopp: error: " << GirArgs[1] << ": " << Buf.getError().message()
           << '\n';
    return 1;
  }
  gopp::gir::Context Ctx;
  auto M = gopp::gir::readModule((*Buf)->getBuffer(), Ctx);
  if (!M) {
    errs() << "gopp: error: " << GirArgs[1] << ": " << toString(M.takeError())
           << '\n';
    return 1;
  }
  gopp::gir::print(outs(), **M);
  return 0;
}

/// `gopp explain E0042`: what the error means and an example. `-list` prints
/// every code. Exit status 1 for a code that does not exist.
static int runExplain() {
  if (ExplainList) {
    gopp::printDiagCodeList(outs());
    return 0;
  }
  if (ExplainCodes.empty()) {
    errs() << "gopp: error: explain: give an error code (E0042) or -list\n";
    return 1;
  }
  int Status = 0;
  bool First = true;
  for (const std::string &C : ExplainCodes) {
    gopp::diag::ID ID = gopp::lookupDiagCode(C);
    if (ID == gopp::diag::NUM_DIAGS) {
      errs() << "gopp: error: explain: " << C
             << " is not an error code (see gopp explain -list)\n";
      Status = 1;
      continue;
    }
    if (!First)
      outs() << "\n";
    First = false;
    gopp::printDiagExplanation(outs(), ID);
  }
  return Status;
}

/// `gopp demangle _GPF4mainE3Foo ...` prints one readable name per argument
/// (exit status 1 and a message for a name that is not a Go++ symbol). With no
/// argument it copies standard input, replacing every `_GP...` word.
static int runDemangle() {
  if (!DemangleNames.empty()) {
    int Status = 0;
    for (const std::string &N : DemangleNames) {
      gopp::sym::DemangleResult R = gopp::sym::demangle(N, DemangleMaxOut);
      if (R.Ok) {
        outs() << R.Text << '\n';
        continue;
      }
      errs() << "gopp: error: demangle: " << N << ": " << R.Error
             << " (at offset " << R.ErrorPos << ")\n";
      Status = 1;
    }
    return Status;
  }
  auto Buf = MemoryBuffer::getSTDIN();
  if (!Buf) {
    errs() << "gopp: error: demangle: " << Buf.getError().message() << '\n';
    return 1;
  }
  StringRef In = (*Buf)->getBuffer();
  auto isSym = [](char C) {
    return isAlnum(static_cast<unsigned char>(C)) || C == '_' || C == '$';
  };
  for (size_t I = 0; I < In.size();) {
    if (!isSym(In[I])) {
      outs() << In[I++];
      continue;
    }
    size_t J = I;
    while (J < In.size() && isSym(In[J]))
      ++J;
    StringRef Word = In.substr(I, J - I);
    gopp::sym::DemangleResult R;
    if (gopp::sym::isReservedName(Word))
      R = gopp::sym::demangle(Word, DemangleMaxOut);
    if (R.Ok)
      outs() << R.Text;
    else
      outs() << Word;
    I = J;
  }
  return 0;
}

/// The .gopp files below Dir, sorted; as gofmt, files whose name starts with
/// a dot are left out. Returns false (after a message) if Dir cannot be read.
static bool collectFiles(const std::string &Dir,
                         std::vector<std::string> &Files) {
  std::error_code EC;
  std::vector<std::string> Found;
  for (llvm::sys::fs::recursive_directory_iterator It(Dir, EC), End;
       It != End && !EC; It.increment(EC)) {
    if (llvm::sys::path::extension(It->path()) == ".gopp" &&
        !llvm::sys::path::filename(It->path()).starts_with(".") &&
        !llvm::sys::fs::is_directory(It->path()))
      Found.push_back(It->path());
  }
  if (EC) {
    errs() << "gopp: error: " << Dir << ": " << EC.message() << '\n';
    return false;
  }
  std::sort(Found.begin(), Found.end());
  Files.insert(Files.end(), Found.begin(), Found.end());
  return true;
}

/// `gopp fmt [-l] [-w] [-d] [files or directories]`. Like gofmt: with no flags
/// the result goes to stdout; with no files stdin is read. Exit status 2 if a
/// file has syntax errors.
static int runFmt() {
  std::vector<std::string> Paths;
  bool FromStdin =
      FmtInputs.empty() || (FmtInputs.size() == 1 && FmtInputs[0] == "-");
  if (FromStdin && FmtWrite) {
    errs() << "gopp: error: fmt: -w needs file names\n";
    return 2;
  }
  int Status = 0;
  if (!FromStdin) {
    for (const std::string &P : FmtInputs) {
      if (!llvm::sys::fs::is_directory(P))
        Paths.push_back(P);
      else if (!collectFiles(P, Paths))
        Status = 2;
    }
  }

  gopp::FormatOptions Opts;
  Opts.SortImports = !FmtNoSortImports;

  auto formatOne = [&](const std::string &Path) {
    gopp::SourceManager SM;
    llvm::Expected<gopp::FileID> F = [&]() -> llvm::Expected<gopp::FileID> {
      if (!FromStdin)
        return SM.addFile(Path);
      auto Buf = MemoryBuffer::getSTDIN();
      if (!Buf)
        return llvm::errorCodeToError(Buf.getError());
      // gofmt names standard input "<standard input>" everywhere.
      return SM.addMemoryBuffer("<standard input>", (*Buf)->getBuffer());
    }();
    if (!F) {
      errs() << "gopp: error: " << toString(F.takeError()) << '\n';
      Status = 2;
      return;
    }
    gopp::TextDiagnosticPrinter Printer(errs(), SM);
    gopp::DiagnosticsEngine Diags(SM, Printer, 0);
    std::string Out;
    bool Ok = gopp::formatFile(SM, *F, Diags, Out, Opts);
    Diags.finish();
    if (!Ok) {
      Status = 2;
      return;
    }
    StringRef Orig = SM.getBuffer(*F);
    std::string Name = FromStdin ? "<standard input>" : Path;
    bool Differs = Out != Orig;
    // The diff is made before -w rewrites the file: SM may have it mapped.
    std::string Diff = FmtDiff ? gopp::unifiedDiff(Name, Orig, Out) : "";
    if (FmtList && Differs)
      outs() << Name << '\n';
    if (FmtWrite && Differs && !writeFile(Path, Out)) {
      Status = 2;
      return;
    }
    outs() << Diff;
    if (!FmtList && !FmtWrite && !FmtDiff)
      outs() << Out;
  };

  if (FromStdin)
    formatOne("-");
  else
    for (const std::string &P : Paths)
      formatOne(P);
  return Status;
}

int main(int argc, char **argv) {
  // `gopp test-exec` is an executor process of `gopp test-run` (P04.19). It
  // runs programs, so it starts before InitLLVM: that would install crash
  // handlers that print a trace and change how a program's crash looks.
  if (argc == 2 && StringRef(argv[1]) == "test-exec")
    gopp::testrun::runExecutor(gopp::testrun::ExecutorControlFd);
  InitLLVM X(argc, argv);
  SelfPath = sys::fs::getMainExecutable(argv[0],
                                        reinterpret_cast<void *>(&runVersion));
  // `gopp lld ...` is the bundled LLD (D4.9): its arguments are the linker's,
  // not ours.
  if (argc >= 2 && StringRef(argv[1]) == "lld") {
    std::vector<std::string> Args(argv + 2, argv + argc);
    return gopp::runLLD(Args);
  }
#if defined(__GLIBC__)
  // glibc grows the heap of a thread that is not the main one (the checker
  // threads) one request at a time: about 2000 mprotect calls, 6% of the
  // check of a big file. A larger step fixes that; setting it turns off
  // glibc's dynamic mmap/trim thresholds, so they are set by hand. This is
  // the program's policy, not a library's (P03.26, M8, R7).
  mallopt(M_TOP_PAD, 16 << 20);
  mallopt(M_MMAP_THRESHOLD, 32 << 20);
  mallopt(M_TRIM_THRESHOLD, 64 << 20);
#endif
  cl::ParseCommandLineOptions(argc, argv, "Go++ compiler driver\n");

  if (VersionCmd)
    return runVersion();
  if (HostToolchainCmd)
    return runHostToolchain();
  if (ParseCmd)
    return runParse();
  if (CheckCmd)
    return runCheck(false);
  if (VetCmd)
    return runCheck(true);
  if (BuildCmd)
    return runBuild();
  if (TestRunCmd)
    return runTestRun();
  if (GirCmd)
    return runGir();
  if (ExplainCmd)
    return runExplain();
  if (FmtCmd)
    return runFmt();
  if (DemangleCmd)
    return runDemangle();

  cl::PrintHelpMessage();
  return 1;
}
