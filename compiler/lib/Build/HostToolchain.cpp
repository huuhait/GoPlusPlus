//===- HostToolchain.cpp - Host sysroot discovery -------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Build/HostToolchain.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include <cstdlib>

extern char **environ;

using namespace gopp;

namespace {

/// Removes `.` and `..` components: GCC answers with paths like
/// /usr/lib/gcc/x86_64-linux-gnu/12/../../../x86_64-linux-gnu/crt1.o.
std::string normalizePath(llvm::StringRef Path) {
  llvm::SmallString<256> Normal(Path);
  llvm::sys::path::remove_dots(Normal, /*remove_dot_dot=*/true);
  return std::string(Normal);
}

} // namespace

void HostToolchain::print(llvm::raw_ostream &OS) const {
  OS << "source: "
     << (Origin == Source::CompilerQuery ? "compiler" : "nix-environment")
     << '\n';
  if (!Compiler.empty())
    OS << "compiler: " << Compiler << '\n';
  if (!Machine.empty())
    OS << "machine: " << Machine << '\n';
  for (const std::string &Dir : IncludeDirs)
    OS << "include-dir: " << Dir << '\n';
  OS << "crt1: " << Crt1 << '\n';
  OS << "crti: " << Crti << '\n';
  OS << "crtn: " << Crtn << '\n';
  OS << "dynamic-linker: " << DynamicLinker << '\n';
}

std::vector<std::string> gopp::parseIncludeDirs(llvm::StringRef Stderr) {
  std::vector<std::string> Dirs;
  bool In = false;
  llvm::SmallVector<llvm::StringRef, 32> Lines;
  Stderr.split(Lines, '\n');
  for (llvm::StringRef Line : Lines) {
    Line = Line.rtrim();
    if (Line.starts_with("#include <...> search starts here:")) {
      In = true;
      continue;
    }
    if (Line.starts_with("End of search list."))
      break;
    // Directory lines are indented by one space.
    if (In && Line.starts_with(" ")) {
      llvm::StringRef Dir = Line.trim();
      Dir.consume_back(" (framework directory)");
      Dirs.push_back(normalizePath(Dir));
    }
  }
  return Dirs;
}

std::string gopp::parsePrintFileName(llvm::StringRef Stdout) {
  llvm::StringRef Line = Stdout.split('\n').first.trim();
  if (!llvm::sys::path::is_absolute(Line))
    return "";
  return normalizePath(Line);
}

std::string gopp::parseDynamicLinker(llvm::StringRef DriverTrace) {
  std::string Found;
  llvm::SmallVector<llvm::StringRef, 8> Lines;
  DriverTrace.split(Lines, '\n');
  for (llvm::StringRef Line : Lines) {
    if (!Line.contains("dynamic-linker"))
      continue;
    // The driver quotes its arguments like a shell does.
    llvm::BumpPtrAllocator Alloc;
    llvm::StringSaver Saver(Alloc);
    llvm::SmallVector<const char *, 64> Words;
    llvm::cl::TokenizeGNUCommandLine(Line, Saver, Words);
    for (size_t I = 0; I < Words.size(); ++I) {
      llvm::StringRef Word = Words[I];
      if (!Word.consume_front("--") && !Word.consume_front("-"))
        continue;
      if (!Word.consume_front("dynamic-linker"))
        continue;
      if (Word.empty() && I + 1 < Words.size())
        Found = normalizePath(Words[++I]);
      else if (Word.consume_front("=") && !Word.empty())
        Found = normalizePath(Word);
    }
  }
  return Found;
}

std::string gopp::dynamicLinkerFileName(llvm::StringRef MachineTriple) {
  switch (llvm::Triple(MachineTriple).getArch()) {
  case llvm::Triple::x86_64:
    return "ld-linux-x86-64.so.2";
  case llvm::Triple::aarch64:
    return "ld-linux-aarch64.so.1";
  default:
    return "";
  }
}

llvm::Expected<HostToolchain> gopp::probeWithCompiler(llvm::StringRef Compiler,
                                                      CommandRunner Run) {
  HostToolchain TC;
  TC.Origin = HostToolchain::Source::CompilerQuery;
  TC.Compiler = Compiler.str();

  auto Query =
      [&](llvm::ArrayRef<std::string> Args) -> llvm::Expected<CommandOutput> {
    std::vector<std::string> Argv{TC.Compiler};
    Argv.insert(Argv.end(), Args.begin(), Args.end());
    return Run(Argv);
  };
  auto FindFile = [&](llvm::StringRef Name) -> llvm::Expected<std::string> {
    auto Out = Query({("-print-file-name=" + Name).str()});
    if (!Out)
      return Out.takeError();
    std::string Path = parsePrintFileName(Out->Stdout);
    if (Path.empty())
      return llvm::createStringError(
          llvm::inconvertibleErrorCode(),
          "'%s' cannot find %s (is the C library development package, "
          "e.g. libc6-dev, installed?)",
          TC.Compiler.c_str(), Name.str().c_str());
    return Path;
  };

  auto Machine = Query({"-dumpmachine"});
  if (!Machine)
    return Machine.takeError();
  TC.Machine = llvm::StringRef(Machine->Stdout).trim().str();

  std::string LoaderName = dynamicLinkerFileName(TC.Machine);
  if (LoaderName.empty())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "unsupported host machine '%s' (supported: x86_64, aarch64)",
        TC.Machine.c_str());

  struct Item {
    llvm::StringRef Name;
    std::string *Dest;
  } Items[] = {
      {"crt1.o", &TC.Crt1}, {"crti.o", &TC.Crti}, {"crtn.o", &TC.Crtn}};
  for (Item &I : Items) {
    auto Path = FindFile(I.Name);
    if (!Path)
      return Path.takeError();
    *I.Dest = *Path;
  }

  // The dynamic linker the compiler's own link line uses: the program
  // interpreter of the binaries it builds (/lib64/ld-linux-x86-64.so.2 on
  // Debian; the glibc store path through the Nix wrapper). If the compiler
  // cannot print its link line, ask where the loader file is instead.
  if (auto Trace = Query({"-###", "-x", "c", "/dev/null", "-o", "/dev/null"}))
    TC.DynamicLinker = parseDynamicLinker(Trace->Stderr);
  else
    llvm::consumeError(Trace.takeError());
  if (TC.DynamicLinker.empty()) {
    auto Path = FindFile(LoaderName);
    if (!Path)
      return Path.takeError();
    TC.DynamicLinker = *Path;
  }

  auto Preprocess = Query({"-E", "-v", "-x", "c", "/dev/null"});
  if (!Preprocess)
    return Preprocess.takeError();
  TC.IncludeDirs = parseIncludeDirs(Preprocess->Stderr);
  if (TC.IncludeDirs.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "'%s' reported no include directories",
                                   TC.Compiler.c_str());
  return TC;
}

llvm::Expected<HostToolchain> gopp::probeFromNixEnvironment(
    llvm::StringRef NixCFlagsCompile, llvm::StringRef NixLdFlags,
    llvm::StringRef Machine,
    llvm::function_ref<bool(llvm::StringRef)> FileExists) {
  HostToolchain TC;
  TC.Origin = HostToolchain::Source::NixEnvironment;

  // NIX_CFLAGS_COMPILE: "... -isystem /nix/store/...-glibc-dev/include ..."
  llvm::SmallVector<llvm::StringRef, 32> Words;
  NixCFlagsCompile.split(Words, ' ', -1, /*KeepEmpty=*/false);
  for (size_t I = 0; I + 1 < Words.size(); ++I)
    if (Words[I] == "-isystem")
      TC.IncludeDirs.push_back(Words[I + 1].str());

  // NIX_LDFLAGS: "... -L/nix/store/...-glibc/lib ..."
  std::string LoaderName = dynamicLinkerFileName(Machine);
  if (LoaderName.empty())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "unsupported host machine '%s'",
                                   Machine.str().c_str());
  Words.clear();
  NixLdFlags.split(Words, ' ', -1, /*KeepEmpty=*/false);
  for (llvm::StringRef W : Words) {
    if (!W.consume_front("-L"))
      continue;
    auto Try = [&](llvm::StringRef Name, std::string &Dest) {
      if (!Dest.empty())
        return;
      llvm::SmallString<256> P(W);
      llvm::sys::path::append(P, Name);
      if (FileExists(P))
        Dest = std::string(P);
    };
    Try("crt1.o", TC.Crt1);
    Try("crti.o", TC.Crti);
    Try("crtn.o", TC.Crtn);
    Try(LoaderName, TC.DynamicLinker);
  }
  if (TC.IncludeDirs.empty() || TC.Crt1.empty() || TC.Crti.empty() ||
      TC.Crtn.empty() || TC.DynamicLinker.empty())
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(),
        "NIX_CFLAGS_COMPILE/NIX_LDFLAGS do not describe a complete C library");
  return TC;
}

namespace {

/// Runs a program and captures stdout and stderr through temporary files.
llvm::Expected<CommandOutput> runCommand(llvm::ArrayRef<std::string> Argv) {
  llvm::ErrorOr<std::string> Exe = llvm::sys::findProgramByName(Argv[0]);
  if (!Exe)
    return llvm::createStringError(Exe.getError(), "cannot find '%s' in PATH",
                                   Argv[0].c_str());

  llvm::SmallString<128> OutPath, ErrPath;
  if (std::error_code EC =
          llvm::sys::fs::createTemporaryFile("gopp-probe", "out", OutPath))
    return llvm::createStringError(EC, "cannot create temporary file");
  llvm::FileRemover OutRemover(OutPath);
  if (std::error_code EC =
          llvm::sys::fs::createTemporaryFile("gopp-probe", "err", ErrPath))
    return llvm::createStringError(EC, "cannot create temporary file");
  llvm::FileRemover ErrRemover(ErrPath);

  // The parsers match English compiler output ("search starts here:"), so
  // the child runs with LC_ALL=C and no other locale variables.
  std::vector<llvm::StringRef> Env;
  for (char **E = environ; E && *E; ++E) {
    llvm::StringRef Var(*E);
    if (!Var.starts_with("LC_ALL=") && !Var.starts_with("LC_MESSAGES=") &&
        !Var.starts_with("LANG=") && !Var.starts_with("LANGUAGE="))
      Env.push_back(Var);
  }
  Env.push_back("LC_ALL=C");

  std::vector<llvm::StringRef> Args(Argv.begin(), Argv.end());
  // An empty redirect path means /dev/null.
  std::optional<llvm::StringRef> Redirects[] = {
      llvm::StringRef(""), llvm::StringRef(OutPath), llvm::StringRef(ErrPath)};
  std::string ErrMsg;
  int RC = llvm::sys::ExecuteAndWait(*Exe, Args, Env, Redirects,
                                     /*SecondsToWait=*/60,
                                     /*MemoryLimit=*/0, &ErrMsg);
  if (RC != 0)
    return llvm::createStringError(
        llvm::inconvertibleErrorCode(), "'%s' failed (exit code %d)%s%s",
        Argv[0].c_str(), RC, ErrMsg.empty() ? "" : ": ", ErrMsg.c_str());

  CommandOutput Result;
  auto Out = llvm::MemoryBuffer::getFile(OutPath);
  auto Err = llvm::MemoryBuffer::getFile(ErrPath);
  if (!Out || !Err)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "cannot read output of '%s'",
                                   Argv[0].c_str());
  Result.Stdout = (*Out)->getBuffer().str();
  Result.Stderr = (*Err)->getBuffer().str();
  return Result;
}

} // namespace

llvm::Expected<HostToolchain> gopp::probeHostToolchain() {
  // `cc` first (the common case), then `$CC`-less NixOS environments.
  if (llvm::sys::findProgramByName("cc"))
    return probeWithCompiler("cc", runCommand);

  const char *CFlags = std::getenv("NIX_CFLAGS_COMPILE");
  const char *LdFlags = std::getenv("NIX_LDFLAGS");
  if (CFlags && LdFlags)
    return probeFromNixEnvironment(
        CFlags, LdFlags, llvm::sys::getProcessTriple(),
        [](llvm::StringRef P) { return llvm::sys::fs::exists(P); });

  return llvm::createStringError(
      llvm::inconvertibleErrorCode(),
      "no C compiler ('cc') in PATH and no NIX_CFLAGS_COMPILE/NIX_LDFLAGS; "
      "the bundled musl fallback is not available yet");
}
