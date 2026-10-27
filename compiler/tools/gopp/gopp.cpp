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
#include "gopp/Import/ClangInfo.h"
#include "gopp/Lex/Lexer.h"
#include "gopp/Parse/Parser.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/MemoryBuffer.h"
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

/// `gopp parse`: all files are one package (one diagnostics engine, one error
/// limit). Diagnostics go to stderr, dumps to stdout.
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

  std::unique_ptr<gopp::DiagnosticConsumer> Consumer;
  if (DiagFormatOpt == DiagFormat::JSON)
    Consumer = std::make_unique<gopp::JSONDiagnosticPrinter>(errs(), SM);
  else
    Consumer = std::make_unique<gopp::TextDiagnosticPrinter>(errs(), SM);
  gopp::DiagnosticsEngine Diags(SM, *Consumer, ErrorLimit);

  for (gopp::FileID F : Files) {
    if (DumpTokens && !DumpAST) {
      // Lexing only: the lexer reports its own errors.
      gopp::dumpTokens(outs(), SM, F, Diags, DumpComments);
      continue;
    }
    if (DumpTokens) {
      // The parse below reports the lexer errors; the dump stays quiet.
      gopp::CollectingDiagnosticConsumer Quiet;
      gopp::DiagnosticsEngine QuietDiags(SM, Quiet, 0);
      gopp::dumpTokens(outs(), SM, F, QuietDiags, DumpComments);
    }
    gopp::ast::ASTContext Ctx;
    gopp::ast::File *AST = gopp::parseFile(Ctx, SM, F, Diags);
    if (DumpAST)
      gopp::ast::dump(outs(), SM, AST);
  }
  Diags.finish();
  return Diags.hasErrors() ? 1 : 0;
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

  cl::PrintHelpMessage();
  return 1;
}
