//===- HostToolchainTest.cpp - Host sysroot discovery tests ---------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Build/HostToolchain.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "gtest/gtest.h"

#include <map>

using namespace gopp;

namespace {

// Real output of `cc -E -v -x c /dev/null` on Debian bookworm (trimmed).
const char *DebianStderr = R"(Using built-in specs.
#include "..." search starts here:
#include <...> search starts here:
 /usr/lib/gcc/x86_64-linux-gnu/12/include
 /usr/local/include
 /usr/include/x86_64-linux-gnu
 /usr/include
End of search list.
)";

// Shape of the output through the Nix clang wrapper.
const char *NixStderr = R"(clang version 21.1.8
#include "..." search starts here:
#include <...> search starts here:
 /nix/store/aaa-clang-wrapper-21.1.8/resource-root/include
 /nix/store/bbb-glibc-2.42-51-dev/include
End of search list.
)";

/// Real compiler output, captured into tests/unit/Build/Inputs.
std::string readInput(llvm::StringRef Name) {
  auto Buffer = llvm::MemoryBuffer::getFile(std::string(GOPP_TEST_INPUTS_DIR) +
                                            "/" + Name);
  EXPECT_TRUE(static_cast<bool>(Buffer)) << "missing input " << Name.str();
  return Buffer ? (*Buffer)->getBuffer().str() : "";
}

/// Fake compiler: maps the joined argument list to canned output.
struct FakeCompiler {
  std::string Machine = "x86_64-linux-gnu";
  std::string LibDir = "/usr/lib/x86_64-linux-gnu";
  std::string Stderr = DebianStderr;
  /// stderr of `cc -###`; empty means "no link line" (use the fallback).
  std::string Trace;
  bool HasCrt1 = true;

  llvm::Expected<CommandOutput> operator()(llvm::ArrayRef<std::string> Argv) {
    std::string Arg = Argv.size() > 1 ? Argv[1] : "";
    if (Arg == "-dumpmachine")
      return CommandOutput{Machine + "\n", ""};
    if (Arg == "-E")
      return CommandOutput{"", Stderr};
    if (Arg == "-###")
      return CommandOutput{"", Trace};
    llvm::StringRef Name = Arg;
    if (Name.consume_front("-print-file-name=")) {
      if (Name == "crt1.o" && !HasCrt1)
        return CommandOutput{"crt1.o\n", ""}; // compilers echo the name
      return CommandOutput{LibDir + "/" + Name.str() + "\n", ""};
    }
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "unexpected argument");
  }
};

} // namespace

TEST(HostToolchain, ParseIncludeDirsDebian) {
  std::vector<std::string> Dirs = parseIncludeDirs(DebianStderr);
  ASSERT_EQ(Dirs.size(), 4u);
  EXPECT_EQ(Dirs[0], "/usr/lib/gcc/x86_64-linux-gnu/12/include");
  EXPECT_EQ(Dirs[3], "/usr/include");
}

TEST(HostToolchain, ParseIncludeDirsIgnoresQuoteSectionAndStripsFramework) {
  std::vector<std::string> Dirs = parseIncludeDirs(
      "#include \"...\" search starts here:\n /quote\n"
      "#include <...> search starts here:\n /a\n /fw (framework directory)\n"
      "End of search list.\n /after\n");
  ASSERT_EQ(Dirs.size(),
            2u); // quote-section dirs and text after the end are ignored
  EXPECT_EQ(Dirs[0], "/a");
  EXPECT_EQ(Dirs[1], "/fw");
}

TEST(HostToolchain, ParseIncludeDirsRealCompilers) {
  EXPECT_EQ(
      parseIncludeDirs(readInput("debian12-gcc12-E-v.txt")),
      (std::vector<std::string>{
          "/usr/lib/gcc/x86_64-linux-gnu/12/include", "/usr/local/include",
          "/usr/include/x86_64-linux-gnu", "/usr/include"}));
  for (const char *Name :
       {"debian12-clang14-E-v.txt", "nix-clang21-wrapper-E-v.txt",
        "nix-gcc15-wrapper-E-v.txt"}) {
    std::vector<std::string> Dirs = parseIncludeDirs(readInput(Name));
    ASSERT_FALSE(Dirs.empty()) << Name;
    for (const std::string &Dir : Dirs) {
      EXPECT_EQ(Dir.front(), '/') << Name << ": " << Dir;
      EXPECT_EQ(Dir.find(".."), std::string::npos) << Name << ": " << Dir;
    }
  }
  // Through the Nix wrappers the glibc headers come last (-idirafter).
  std::vector<std::string> Nix =
      parseIncludeDirs(readInput("nix-gcc15-wrapper-E-v.txt"));
  ASSERT_EQ(Nix.size(), 4u);
  EXPECT_NE(Nix[3].find("-glibc-2.42-51-dev/include"), std::string::npos);
}

TEST(HostToolchain, ParseIncludeDirsNormalizes) {
  EXPECT_EQ(parseIncludeDirs("#include <...> search starts here:\n"
                             " /usr/bin/../lib/gcc/x/12/include\n"
                             "End of search list.\n"),
            (std::vector<std::string>{"/usr/lib/gcc/x/12/include"}));
}

TEST(HostToolchain, ParseDynamicLinkerRealCompilers) {
  // gcc (collect2, `-dynamic-linker X`) and clang (quoted) on Debian.
  EXPECT_EQ(parseDynamicLinker(readInput("debian12-gcc12-trace.txt")),
            "/lib64/ld-linux-x86-64.so.2");
  EXPECT_EQ(parseDynamicLinker(readInput("debian12-clang14-trace.txt")),
            "/lib64/ld-linux-x86-64.so.2");
  // The Nix clang wrapper uses the `-dynamic-linker=X` form.
  EXPECT_EQ(parseDynamicLinker(readInput("nix-clang21-wrapper-trace.txt")),
            "/nix/store/fsd4vbp9zjd9iaicq2fb9lpc1h5j973y-glibc-2.42-100/lib/"
            "ld-linux-x86-64.so.2");
  EXPECT_NE(parseDynamicLinker(readInput("nix-gcc15-wrapper-trace.txt"))
                .find("-glibc-2.42-51/lib64/ld-linux-x86-64.so.2"),
            std::string::npos);
}

TEST(HostToolchain, ParseDynamicLinkerEdgeCases) {
  EXPECT_EQ(parseDynamicLinker(""), "");
  EXPECT_EQ(parseDynamicLinker("ld -o a.out a.o\n"), "");
  EXPECT_EQ(parseDynamicLinker("ld -dynamic-linker\n"), "");
  EXPECT_EQ(parseDynamicLinker("ld --dynamic-linker=/a -dynamic-linker /b\n"),
            "/b");
  EXPECT_EQ(parseDynamicLinker("ld \"-dynamic-linker\" \"/x/../lib/ld.so\"\n"),
            "/lib/ld.so");
}

TEST(HostToolchain, ParseIncludeDirsEmpty) {
  EXPECT_TRUE(parseIncludeDirs("nothing useful\n").empty());
}

TEST(HostToolchain, ParsePrintFileName) {
  EXPECT_EQ(parsePrintFileName("/usr/lib/crt1.o\n"), "/usr/lib/crt1.o");
  EXPECT_EQ(parsePrintFileName("crt1.o\n"), ""); // not found: name echoed
  EXPECT_EQ(parsePrintFileName(""), "");
  EXPECT_EQ(parsePrintFileName(
                "/usr/lib/gcc/x86_64-linux-gnu/12/../../../x86_64-linux-gnu/"
                "crt1.o\n"),
            "/usr/lib/x86_64-linux-gnu/crt1.o");
}

TEST(HostToolchain, DynamicLinkerFileName) {
  EXPECT_EQ(dynamicLinkerFileName("x86_64-linux-gnu"), "ld-linux-x86-64.so.2");
  EXPECT_EQ(dynamicLinkerFileName("aarch64-unknown-linux-gnu"),
            "ld-linux-aarch64.so.1");
  EXPECT_EQ(dynamicLinkerFileName("riscv64-linux-gnu"), "");
}

TEST(HostToolchain, ProbeWithCompilerDebian) {
  FakeCompiler Fake;
  llvm::Expected<HostToolchain> TC = probeWithCompiler(
      "cc", [&](llvm::ArrayRef<std::string> A) { return Fake(A); });
  ASSERT_TRUE(static_cast<bool>(TC)) << llvm::toString(TC.takeError());
  EXPECT_EQ(TC->Machine, "x86_64-linux-gnu");
  EXPECT_EQ(TC->Crt1, "/usr/lib/x86_64-linux-gnu/crt1.o");
  EXPECT_EQ(TC->Crti, "/usr/lib/x86_64-linux-gnu/crti.o");
  EXPECT_EQ(TC->Crtn, "/usr/lib/x86_64-linux-gnu/crtn.o");
  EXPECT_EQ(TC->DynamicLinker,
            "/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2");
  EXPECT_EQ(TC->IncludeDirs.size(), 4u);
}

TEST(HostToolchain, ProbeUsesTheDriverLinkLine) {
  // Debian: the program interpreter is /lib64/..., not the file that
  // -print-file-name finds under /usr/lib.
  FakeCompiler Fake;
  Fake.Trace = readInput("debian12-gcc12-trace.txt");
  llvm::Expected<HostToolchain> TC = probeWithCompiler(
      "cc", [&](llvm::ArrayRef<std::string> A) { return Fake(A); });
  ASSERT_TRUE(static_cast<bool>(TC)) << llvm::toString(TC.takeError());
  EXPECT_EQ(TC->DynamicLinker, "/lib64/ld-linux-x86-64.so.2");
}

TEST(HostToolchain, ProbeWithCompilerNix) {
  FakeCompiler Fake;
  Fake.LibDir = "/nix/store/ccc-glibc-2.42-51/lib";
  Fake.Stderr = NixStderr;
  llvm::Expected<HostToolchain> TC = probeWithCompiler(
      "cc", [&](llvm::ArrayRef<std::string> A) { return Fake(A); });
  ASSERT_TRUE(static_cast<bool>(TC)) << llvm::toString(TC.takeError());
  EXPECT_EQ(TC->Crt1, "/nix/store/ccc-glibc-2.42-51/lib/crt1.o");
  ASSERT_EQ(TC->IncludeDirs.size(), 2u);
  EXPECT_EQ(TC->IncludeDirs[1], "/nix/store/bbb-glibc-2.42-51-dev/include");
}

TEST(HostToolchain, ProbeFailsWhenCrtMissing) {
  FakeCompiler Fake;
  Fake.HasCrt1 = false;
  llvm::Expected<HostToolchain> TC = probeWithCompiler(
      "cc", [&](llvm::ArrayRef<std::string> A) { return Fake(A); });
  ASSERT_FALSE(static_cast<bool>(TC));
  std::string Msg = llvm::toString(TC.takeError());
  EXPECT_NE(Msg.find("crt1.o"), std::string::npos) << Msg;
  EXPECT_NE(Msg.find("libc6-dev"), std::string::npos) << Msg;
}

TEST(HostToolchain, ProbeFailsOnUnsupportedMachine) {
  FakeCompiler Fake;
  Fake.Machine = "riscv64-linux-gnu";
  llvm::Expected<HostToolchain> TC = probeWithCompiler(
      "cc", [&](llvm::ArrayRef<std::string> A) { return Fake(A); });
  ASSERT_FALSE(static_cast<bool>(TC));
  llvm::consumeError(TC.takeError());
}

TEST(HostToolchain, ProbeFromNixEnvironment) {
  std::map<std::string, bool> Files = {
      {"/nix/store/g/lib/crt1.o", true},
      {"/nix/store/g/lib/crti.o", true},
      {"/nix/store/g/lib/crtn.o", true},
      {"/nix/store/g/lib/ld-linux-x86-64.so.2", true}};
  auto Exists = [&](llvm::StringRef P) { return Files.count(P.str()) != 0; };
  llvm::Expected<HostToolchain> TC = probeFromNixEnvironment(
      "-frandom-seed=x -isystem /nix/store/g-dev/include -isystem /other",
      "-L/nix/store/none/lib -L/nix/store/g/lib", "x86_64-unknown-linux-gnu",
      Exists);
  ASSERT_TRUE(static_cast<bool>(TC)) << llvm::toString(TC.takeError());
  EXPECT_EQ(TC->Origin, HostToolchain::Source::NixEnvironment);
  EXPECT_EQ(TC->IncludeDirs.size(), 2u);
  EXPECT_EQ(TC->Crt1, "/nix/store/g/lib/crt1.o");
  EXPECT_EQ(TC->DynamicLinker, "/nix/store/g/lib/ld-linux-x86-64.so.2");
}

TEST(HostToolchain, ProbeFromNixEnvironmentIncomplete) {
  llvm::Expected<HostToolchain> TC =
      probeFromNixEnvironment("", "", "x86_64-unknown-linux-gnu",
                              [](llvm::StringRef) { return false; });
  ASSERT_FALSE(static_cast<bool>(TC));
  llvm::consumeError(TC.takeError());
}

TEST(HostToolchain, PrintFormat) {
  HostToolchain TC;
  TC.Compiler = "/usr/bin/cc";
  TC.IncludeDirs = {"/usr/include"};
  TC.Crt1 = "/usr/lib/crt1.o";
  TC.Crti = "/usr/lib/crti.o";
  TC.Crtn = "/usr/lib/crtn.o";
  TC.DynamicLinker = "/lib64/ld.so";
  std::string S;
  llvm::raw_string_ostream OS(S);
  TC.print(OS);
  EXPECT_NE(S.find("include-dir: /usr/include\n"), std::string::npos);
  EXPECT_NE(S.find("crt1: /usr/lib/crt1.o\n"), std::string::npos);
  EXPECT_NE(S.find("dynamic-linker: /lib64/ld.so\n"), std::string::npos);
}
