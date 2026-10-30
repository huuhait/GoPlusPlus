//===- Linker.cpp - Linking with the bundled LLD --------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// D4.9. The LLD inside gopp is started as `gopp lld ...`, a subprocess of the
// compiler, so a crash or an exit inside the linker cannot take the compiler
// down and there is one file to ship. The crt objects, the dynamic linker and
// the libc directory come from host sysroot discovery (P00.12), or from an
// explicit directory for a target other than the host.
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/Linker.h"

#include "gopp/Build/HostToolchain.h"
#include "gopp/CodeGen/CodeGen.h"

#include "lld/Common/Driver.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

// Declared in lld/Common/Version.h, defined in lldCommon. That header cannot be
// included: it needs the generated lld/Common/Version.inc, which the nixpkgs
// lld dev output does not install.
namespace lld {
std::string getLLDVersion();
} // namespace lld

LLD_HAS_DRIVER(elf)

using namespace gopp;
using llvm::Error;
using llvm::Expected;
using llvm::StringRef;

std::string gopp::getLLDVersion() { return lld::getLLDVersion(); }

/// Every piece of the link is there: the crt objects, each library in a
/// library directory (libX.so, or libX.a), a dynamic linker. The message says
/// what is missing and where it was looked for, never a crash or a bare
/// linker error.
static Error checkLinkInputs(const LinkInputs &In) {
  for (auto [Path, Name] :
       {std::pair<const std::string *, const char *>{&In.Crt1, "crt1.o"},
        {&In.Crti, "crti.o"},
        {&In.Crtn, "crtn.o"}}) {
    if (Path->empty())
      return llvm::createStringError(
          std::errc::no_such_file_or_directory,
          "the C library has no %s (install its development files: libc6-dev "
          "on Debian, glibc-devel on Fedora, glibc.dev on Nix)",
          Name);
    if (!llvm::sys::fs::exists(*Path))
      return llvm::createStringError(std::errc::no_such_file_or_directory,
                                     "%s does not exist: %s", Name,
                                     Path->c_str());
  }
  for (const std::string &L : In.Libs) {
    bool Found = false;
    for (const std::string &D : In.LibDirs)
      for (const char *Ext : {".so", ".a"}) {
        llvm::SmallString<128> P(D);
        llvm::sys::path::append(P, "lib" + L + Ext);
        Found |= llvm::sys::fs::exists(P);
      }
    if (!Found)
      return llvm::createStringError(
          std::errc::no_such_file_or_directory,
          "lib%s.so (or lib%s.a) is not in %s (install the C library "
          "development files)",
          L.c_str(), L.c_str(),
          In.LibDirs.empty() ? "any library directory"
                             : llvm::join(In.LibDirs, ", ").c_str());
  }
  if (In.DynamicLinker.empty())
    return llvm::createStringError(
        std::errc::invalid_argument,
        "no dynamic linker is known for this C library (pass "
        "-dynamic-linker=<path>)");
  return Error::success();
}

Expected<LinkInputs> gopp::findLinkInputs(const TargetSpec &T, StringRef CrtDir,
                                          StringRef DynamicLinker) {
  LinkInputs In;
  if (!CrtDir.empty()) {
    for (auto [Name, Field] :
         {std::pair<const char *, std::string *>{"crt1.o", &In.Crt1},
          {"crti.o", &In.Crti},
          {"crtn.o", &In.Crtn}}) {
      llvm::SmallString<128> P(CrtDir);
      llvm::sys::path::append(P, Name);
      *Field = std::string(P);
    }
    In.LibDirs.push_back(CrtDir.str());
    In.DynamicLinker =
        DynamicLinker.empty() ? T.DefaultDynamicLinker : DynamicLinker.str();
    if (Error E = checkLinkInputs(In))
      return std::move(E);
    return In;
  }
  Expected<TargetSpec> Host = hostTarget();
  if (!Host || Host->Arch != T.Arch || Host->OS != T.OS)
    return llvm::createStringError(
        std::errc::not_supported,
        "linking for %s on this host needs -crt-dir=<directory with crt1.o, "
        "crti.o, crtn.o and libc> (builds for other targets come with musl, "
        "P07.14)",
        T.name().c_str());
  llvm::consumeError(Host.takeError());
  Expected<HostToolchain> TC = probeHostToolchain();
  if (!TC)
    return TC.takeError();
  In.Crt1 = TC->Crt1;
  In.Crti = TC->Crti;
  In.Crtn = TC->Crtn;
  In.LibDirs.push_back(llvm::sys::path::parent_path(TC->Crt1).str());
  In.DynamicLinker = !DynamicLinker.empty()       ? DynamicLinker.str()
                     : !TC->DynamicLinker.empty() ? TC->DynamicLinker
                                                  : T.DefaultDynamicLinker;
  if (Error E = checkLinkInputs(In))
    return std::move(E);
  return In;
}

std::vector<std::string> gopp::lldArguments(const TargetSpec &T,
                                            const LinkInputs &In,
                                            llvm::ArrayRef<std::string> Objects,
                                            StringRef Output) {
  // A dynamic, non-PIE executable against the C library: crt1.o's _start
  // calls the `main` that the module defines. No crtbegin/crtend yet: they
  // matter for C++ static constructors and exception frame registration, and
  // arrive with the EH runtime (P04.07). The line is a function of the input
  // only, so the same objects give the same executable bytes: no build id,
  // no time, no environment. --eh-frame-hdr lets the unwinder find the
  // frames (D4.6); -z relro and the GNU hash table are what the system C
  // compiler's link line has.
  std::vector<std::string> A = {"-o",
                                Output.str(),
                                "-m",
                                T.Emulation,
                                "--eh-frame-hdr",
                                "--hash-style=gnu",
                                "--build-id=none",
                                "-z",
                                "relro",
                                "--dynamic-linker=" + In.DynamicLinker};
  A.push_back(In.Crt1);
  A.push_back(In.Crti);
  A.insert(A.end(), Objects.begin(), Objects.end());
  for (const std::string &D : In.LibDirs)
    A.push_back("-L" + D);
  for (const std::string &L : In.Libs)
    A.push_back("-l" + L);
  A.push_back(In.Crtn);
  return A;
}

int gopp::runLLD(llvm::ArrayRef<std::string> Argv) {
  std::vector<const char *> Args = {"ld.lld"};
  for (const std::string &A : Argv)
    Args.push_back(A.c_str());
  lld::Result R = lld::lldMain(Args, llvm::outs(), llvm::errs(),
                               {{lld::Gnu, &lld::elf::link}});
  llvm::outs().flush();
  llvm::errs().flush();
  return R.retCode;
}

Error gopp::linkExecutable(StringRef Self, const TargetSpec &T,
                           const LinkInputs &In,
                           llvm::ArrayRef<std::string> Objects,
                           StringRef Output) {
  std::vector<std::string> Argv = {Self.str(), "lld"};
  for (std::string &A : lldArguments(T, In, Objects, Output))
    Argv.push_back(std::move(A));
  std::vector<StringRef> Refs(Argv.begin(), Argv.end());
  std::string Msg;
  int Rc = llvm::sys::ExecuteAndWait(Self, Refs, std::nullopt, {}, 0, 0, &Msg);
  if (Rc != 0)
    return llvm::createStringError(std::errc::io_error,
                                   "the linker failed (exit status %d)%s%s", Rc,
                                   Msg.empty() ? "" : ": ", Msg.c_str());
  return Error::success();
}
