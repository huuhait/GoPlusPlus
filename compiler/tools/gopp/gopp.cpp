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
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/Basic/Version.h"
#include "gopp/Build/HostToolchain.h"
#include "gopp/CodeGen/Linker.h"
#include "gopp/CodeGen/Target.h"
#include "gopp/Format/Format.h"
#include "gopp/Import/ClangInfo.h"
#include "gopp/Lex/Lexer.h"
#include "gopp/Parse/Parser.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

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
  InitLLVM X(argc, argv);
  cl::ParseCommandLineOptions(argc, argv, "Go++ compiler driver\n");

  if (VersionCmd)
    return runVersion();
  if (HostToolchainCmd)
    return runHostToolchain();
  if (ParseCmd)
    return runParse();
  if (FmtCmd)
    return runFmt();

  cl::PrintHelpMessage();
  return 1;
}
