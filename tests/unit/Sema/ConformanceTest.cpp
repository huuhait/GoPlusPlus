//===- ConformanceTest.cpp - Go type checker conformance suite ------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Runs the ported types2 test files (tests/sema/conformance/testdata, from
// $GOROOT/src/internal/types/testdata, BSD license, see LICENSE-GO) through
// the checker in Go mode. The harness follows types2's check_test.go:
//   - `/* ERROR "msg" */` and `// ERROR "msg"` expect an error whose text
//     contains msg on the line of the token before the comment;
//   - `ERRORx` takes a regular expression instead of a substring;
//   - every reported error must be expected and every expectation matched.
// Only the line is compared (not the column).
//
// tests/sema/conformance/expected-failures.txt has one line per file that
// does not pass, `path | xfail | reason` or `path | exclude | reason`:
//   - xfail: a gap of the checker; the test fails when the file passes (so
//     the list only shrinks) or an unlisted file fails;
//   - exclude: the file tests something Go++ does not have by design (Go
//     version flags, cgo). It still runs, but it is not counted.
// A file with a first-line flag (`// -lang=...`) must be excluded, except the
// flags that select what Go++ always does (`-gotypesalias=1`).
// GOPP_CONFORMANCE_REPORT=1 prints the pass rates of the counted files.
//
//===----------------------------------------------------------------------===//

#include "gopp/Parse/Parser.h"
#include "gopp/Sema/Sema.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>

using namespace gopp;
using namespace gopp::sema;

namespace {

struct Expectation {
  unsigned Line;
  std::string Pattern;
  bool IsRegex;
  bool Matched = false;
};

struct Result {
  bool Pass = false;
  unsigned Expected = 0, Matched = 0, Unexpected = 0, Unmatched = 0;
  std::string Detail;
  bool Flagged = false; // the first line has flags (`// -lang=go1.17`)
};

class FakeImporter : public Importer {
public:
  FakeImporter(TypeContext &Ctx) : Ctx(Ctx) {}
  Package *import(llvm::StringRef Path, std::string &) override {
    if (Path == "unsafe")
      return Ctx.unsafePackage();
    // The Go standard library packages the files import exist (empty); any
    // other path cannot be imported, as with Go's test importer.
    static const char *Std[] = {"context", "database/sql",  "fmt",
                                "go/ast",  "html/template", "io",
                                "math",    "math/big",      "os",
                                "reflect", "regexp/syntax", "runtime",
                                "slices",  "sort",          "strconv",
                                "strings", "testing",       "text/template",
                                "time"};
    if (std::find(std::begin(Std), std::end(Std), Path) == std::end(Std))
      return nullptr;
    if (Package *Stub = loadStub(Path))
      return Stub;
    auto &P = Pkgs[Path.str()];
    if (!P) {
      P = std::make_unique<Package>();
      P->Path = Path;
      llvm::StringRef Name = llvm::sys::path::filename(Path);
      P->Name = Name;
      P->Fake = true;
      P->PkgScope = Mem.newScope(nullptr, {}, "package");
      Names.push_back(std::make_unique<std::string>(Name.str()));
      P->Name = *Names.back();
      Paths.push_back(std::make_unique<std::string>(Path.str()));
      P->Path = *Paths.back();
    }
    return P.get();
  }

private:
  /// The API of a Go standard library package the files use, written as Go
  /// source in tests/sema/conformance/stdstub (test-only; the real standard
  /// library is P13). Null when there is no stub for Path.
  Package *loadStub(llvm::StringRef Path) {
    auto It = Stubs.find(Path.str());
    if (It != Stubs.end())
      return It->second ? It->second->Checked->Pkg : nullptr;
    Stubs[Path.str()] = nullptr;
    std::string File =
        std::string(GOPP_CONFORMANCE_DIR) + "/stdstub/" + Path.str() + ".go";
    if (!llvm::sys::fs::exists(File))
      return nullptr;
    auto S = std::make_unique<Stub>();
    auto F = S->SM.addFile(File);
    if (!F) {
      llvm::consumeError(F.takeError());
      return nullptr;
    }
    S->Diags = std::make_unique<DiagnosticsEngine>(S->SM, S->Collect, 0);
    ParseOptions PO;
    PO.CheckLanguageRules = false;
    PO.AllowGenericMethods = false;
    PO.AllowNamespaceTypes = false;
    PO.AllowExtends = false;
    ast::File *AST = parseFile(S->AC, S->SM, *F, *S->Diags, PO);
    CheckOptions CO;
    CO.GoCompat = true;
    CO.Ownership = false;
    const ast::File *Files[] = {AST};
    S->Checked = checkPackage(Ctx, S->SM, Path, Files, *this, *S->Diags, CO);
    Package *Result = S->Checked->Pkg;
    Stubs[Path.str()] = std::move(S);
    return Result;
  }

  struct Stub {
    SourceManager SM;
    CollectingDiagnosticConsumer Collect;
    std::unique_ptr<DiagnosticsEngine> Diags;
    ast::ASTContext AC;
    std::unique_ptr<CheckedPackage> Checked;
  };
  std::map<std::string, std::unique_ptr<Stub>> Stubs;
  TypeContext &Ctx;
  Arena Mem;
  std::map<std::string, std::unique_ptr<Package>> Pkgs;
  std::vector<std::unique_ptr<std::string>> Names, Paths;
};

/// Unquotes the Go string or raw string at the start of S.
bool readQuoted(llvm::StringRef S, std::string &Out) {
  if (S.empty())
    return false;
  if (S[0] == '`') {
    size_t E = S.find('`', 1);
    if (E == llvm::StringRef::npos)
      return false;
    Out = S.substr(1, E - 1).str();
    return true;
  }
  if (S[0] != '"')
    return false;
  size_t I = 1;
  Out.clear();
  while (I < S.size() && S[I] != '"') {
    if (S[I] == '\\' && I + 1 < S.size()) {
      ++I;
      switch (S[I]) {
      case 'n':
        Out += '\n';
        break;
      case 't':
        Out += '\t';
        break;
      default:
        Out += S[I];
      }
    } else {
      Out += S[I];
    }
    ++I;
  }
  return I < S.size();
}

Result runFile(const std::string &Path) {
  Result R;
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  if (!Buf) {
    R.Detail = "cannot read";
    return R;
  }
  SourceManager SM;
  auto F = SM.addFile(Path);
  if (!F) {
    llvm::consumeError(F.takeError());
    R.Detail = "cannot add";
    return R;
  }
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  ast::ASTContext AC;
  ParseOptions PO;
  PO.CheckLanguageRules = false;
  PO.AllowGenericMethods = false;
  PO.AllowNamespaceTypes = false;
  PO.AllowExtends = false;
  ast::File *AST = parseFile(AC, SM, *F, Diags, PO);

  // Expectations from the comments.
  llvm::StringRef Text = SM.getBuffer(*F);
  std::vector<Expectation> Exp;
  std::vector<std::pair<uint32_t, uint32_t>> CommentRanges;
  for (const ast::Comment &C : AST->Comments) {
    uint32_t B = SM.getFileOffset(C.Loc);
    CommentRanges.push_back({B, B + C.Length});
  }
  std::sort(CommentRanges.begin(), CommentRanges.end());
  auto LineOfOffset = [&](uint32_t Off) {
    return SM.getLineNumber(SM.getLoc(*F, Off));
  };
  for (auto &[B, E] : CommentRanges) {
    llvm::StringRef C = Text.substr(B, E - B);
    llvm::StringRef Body =
        C.starts_with("//") ? C.drop_front(2) : C.drop_front(2).drop_back(2);
    Body = Body.ltrim();
    bool IsRegex = false;
    if (Body.starts_with("ERRORx "))
      IsRegex = true;
    else if (!Body.starts_with("ERROR "))
      continue;
    Body = Body.drop_front(IsRegex ? 7 : 6).ltrim();
    std::string Pat;
    if (!readQuoted(Body, Pat))
      continue;
    // The line of the token before the comment: skip whitespace and comments
    // backwards.
    int64_t Pos = static_cast<int64_t>(B) - 1;
    for (;;) {
      while (Pos >= 0 && (Text[Pos] == ' ' || Text[Pos] == '\t' ||
                          Text[Pos] == '\n' || Text[Pos] == '\r'))
        --Pos;
      if (Pos < 0)
        break;
      bool InComment = false;
      for (auto &[CB, CE] : CommentRanges)
        if (static_cast<uint32_t>(Pos) >= CB &&
            static_cast<uint32_t>(Pos) < CE) {
          Pos = static_cast<int64_t>(CB) - 1;
          InComment = true;
          break;
        }
      if (!InComment)
        break;
    }
    unsigned Line = Pos < 0 ? 1 : LineOfOffset(static_cast<uint32_t>(Pos));
    Exp.push_back({Line, Pat, IsRegex});
  }
  R.Expected = Exp.size();

  // Flags in the first line comment select Go versions and experiments.
  llvm::StringRef First = Text.take_until([](char C) { return C == '\n'; });
  // `-gotypesalias=1` and `-goexperiment=aliastypeparams` select behaviour
  // Go++ always has (alias types, generic aliases), so they do not count as
  // flags; any other flag selects something Go++ does not have.
  if (First.starts_with("//") && First.ltrim("/ ").starts_with("-")) {
    llvm::SmallVector<llvm::StringRef, 4> Flags;
    First.ltrim("/ ").split(Flags, ' ', -1, false);
    for (llvm::StringRef F : Flags)
      if (F != "-gotypesalias=1" && F != "-goexperiment=aliastypeparams")
        R.Flagged = true;
  }

  TypeContext Ctx(true);
  FakeImporter Imp(Ctx);
  CheckOptions CO;
  CO.GoCompat = true;
  CO.Ownership = false;
  const ast::File *Files[] = {AST};
  auto Checked = checkPackage(Ctx, SM, "p", Files, Imp, Diags, CO);

  // Match.
  std::vector<bool> UsedExp(Exp.size(), false);
  for (const Diagnostic &D : Collect.Diags) {
    if (D.Sev != Severity::Error)
      continue;
    unsigned Line = D.Loc.isValid() ? SM.getLineNumber(D.Loc) : 0;
    bool Found = false;
    for (size_t K = 0; K < Exp.size(); ++K) {
      if (Exp[K].Matched || Exp[K].Line != Line)
        continue;
      bool Ok;
      if (Exp[K].IsRegex) {
        llvm::Regex Re(Exp[K].Pattern);
        Ok = Re.isValid() && Re.match(D.Message);
      } else {
        Ok = D.Message.find(Exp[K].Pattern) != std::string::npos;
      }
      if (Ok) {
        Exp[K].Matched = true;
        ++R.Matched;
        Found = true;
        break;
      }
    }
    if (!Found) {
      ++R.Unexpected;
      if (R.Detail.size() < 600)
        R.Detail += "\n  unexpected " + std::to_string(Line) + ": " +
                    D.Message.substr(0, 140);
    }
  }
  for (Expectation &E : Exp)
    if (!E.Matched) {
      ++R.Unmatched;
      if (R.Detail.size() < 600)
        R.Detail += "\n  missing " + std::to_string(E.Line) + ": " + E.Pattern;
    }
  R.Pass = R.Unexpected == 0 && R.Unmatched == 0;
  return R;
}

struct Listed {
  bool Exclude = false;
  std::string Reason;
};

std::map<std::string, Listed> loadExpectedFailures() {
  std::map<std::string, Listed> M;
  std::ifstream In(std::string(GOPP_CONFORMANCE_DIR) +
                   "/expected-failures.txt");
  std::string Line;
  while (std::getline(In, Line)) {
    if (Line.empty() || Line[0] == '#')
      continue;
    llvm::SmallVector<llvm::StringRef, 3> Parts;
    llvm::StringRef(Line).split(Parts, " | ", 2);
    Listed L;
    L.Exclude = Parts.size() > 1 && Parts[1].trim() == "exclude";
    L.Reason = Parts.size() > 2 ? Parts[2].trim().str() : "";
    M[Parts[0].trim().str()] = L;
  }
  return M;
}

std::vector<std::string> listFiles() {
  std::vector<std::string> Out;
  std::string Root = std::string(GOPP_CONFORMANCE_DIR) + "/testdata";
  std::error_code EC;
  for (llvm::sys::fs::recursive_directory_iterator It(Root, EC), End;
       It != End && !EC; It.increment(EC))
    if (llvm::sys::path::extension(It->path()) == ".go")
      Out.push_back(It->path().substr(Root.size() + 1));
  std::sort(Out.begin(), Out.end());
  return Out;
}

bool isGenerics(const std::string &Rel, llvm::StringRef Text) {
  static const char *Names[] = {"typeparam",     "typeinst",   "typeinfer",
                                "generic",       "constraint", "inference",
                                "unions",        "typesets",   "linalg",
                                "funcinference", "tparam",     "instantiate"};
  for (const char *N : Names)
    if (Rel.find(N) != std::string::npos)
      return true;
  static llvm::Regex Re("\\[[A-Za-z_][A-Za-z_0-9]*(, *[A-Za-z_][A-Za-z_0-9]*)* "
                        "+(any|comparable|interface|~|\\*?[A-Za-z_][A-Za-z_0-9."
                        "]*(\\[[^]]*\\])? *[|,\\]])");
  return Re.match(Text);
}

struct Stats {
  unsigned Files = 0, Passed = 0, GFiles = 0, GPassed = 0;
  unsigned Annot = 0, AnnotMatched = 0, GAnnot = 0, GAnnotMatched = 0;
  unsigned Unexpected = 0;
  unsigned Excluded = 0, ExcludedPassing = 0;
};
Stats TheStats;

class ReportEnv : public ::testing::Environment {
public:
  void TearDown() override {
    if (!std::getenv("GOPP_CONFORMANCE_REPORT"))
      return;
    auto Pct = [](unsigned A, unsigned B) { return B ? 100.0 * A / B : 0.0; };
    printf("CONFORMANCE files: %u/%u pass (%.1f%%); annotations matched %u/%u "
           "(%.1f%%), unexpected errors %u\n",
           TheStats.Passed, TheStats.Files,
           Pct(TheStats.Passed, TheStats.Files), TheStats.AnnotMatched,
           TheStats.Annot, Pct(TheStats.AnnotMatched, TheStats.Annot),
           TheStats.Unexpected);
    printf("CONFORMANCE generics files: %u/%u pass (%.1f%%); annotations "
           "matched %u/%u (%.1f%%)\n",
           TheStats.GPassed, TheStats.GFiles,
           Pct(TheStats.GPassed, TheStats.GFiles), TheStats.GAnnotMatched,
           TheStats.GAnnot, Pct(TheStats.GAnnotMatched, TheStats.GAnnot));
    printf("CONFORMANCE excluded files (not counted): %u, of which %u pass "
           "anyway\n",
           TheStats.Excluded, TheStats.ExcludedPassing);
  }
};
const auto *RegisterEnv = ::testing::AddGlobalTestEnvironment(new ReportEnv);

class Conformance : public ::testing::TestWithParam<std::string> {};

TEST_P(Conformance, File) {
  static const auto Expected = loadExpectedFailures();
  std::string Rel = GetParam();
  std::string Path = std::string(GOPP_CONFORMANCE_DIR) + "/testdata/" + Rel;
  Result R = runFile(Path);
  auto Buf = llvm::MemoryBuffer::getFile(Path);
  bool Gen = isGenerics(Rel, Buf ? (*Buf)->getBuffer() : "");
  auto It = Expected.find(Rel);
  bool Excluded = It != Expected.end() && It->second.Exclude;
  if (R.Flagged)
    EXPECT_TRUE(Excluded) << Rel << " has Go version flags: list it as exclude";
  if (Excluded) {
    ++TheStats.Excluded;
    TheStats.ExcludedPassing += R.Pass;
    if (std::getenv("GOPP_CONFORMANCE_REPORT"))
      printf("CONFORMANCE EXCLUDED %s (%s)\n", Rel.c_str(),
             R.Pass ? "passes" : "fails");
    return;
  }
  ++TheStats.Files;
  TheStats.Annot += R.Expected;
  TheStats.AnnotMatched += R.Matched;
  TheStats.Unexpected += R.Unexpected;
  if (R.Pass)
    ++TheStats.Passed;
  if (Gen) {
    ++TheStats.GFiles;
    TheStats.GAnnot += R.Expected;
    TheStats.GAnnotMatched += R.Matched;
    if (R.Pass)
      ++TheStats.GPassed;
  }
  if (std::getenv("GOPP_CONFORMANCE_REPORT"))
    printf("CONFORMANCE %s %s expected=%u matched=%u unexpected=%u%s%s\n",
           R.Pass ? "PASS" : "FAIL", Rel.c_str(), R.Expected, R.Matched,
           R.Unexpected, Gen ? " generics" : "",
           std::getenv("GOPP_CONFORMANCE_DETAIL") ? R.Detail.c_str() : "");
  if (It != Expected.end())
    EXPECT_FALSE(R.Pass) << Rel
                         << " passes now: remove it from expected-failures.txt";
  else
    EXPECT_TRUE(R.Pass) << Rel << ": expected " << R.Expected << ", matched "
                        << R.Matched << ", unexpected " << R.Unexpected
                        << R.Detail;
}

INSTANTIATE_TEST_SUITE_P(Go, Conformance, ::testing::ValuesIn(listFiles()),
                         [](const ::testing::TestParamInfo<std::string> &I) {
                           std::string N = I.param;
                           for (char &C : N)
                             if (!std::isalnum(static_cast<unsigned char>(C)))
                               C = '_';
                           return N;
                         });

} // namespace
