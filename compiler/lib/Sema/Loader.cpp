//===- Loader.cpp - The package loader ------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Loader.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cctype>

using namespace gopp;
using namespace gopp::sema;
using llvm::SmallVector;
using llvm::StringRef;

//===----------------------------------------------------------------------===//
// Build constraints
//===----------------------------------------------------------------------===//

static const char *const KnownOS[] = {
    "aix",     "android", "darwin",  "dragonfly", "freebsd", "hurd",
    "illumos", "ios",     "js",      "linux",     "nacl",    "netbsd",
    "openbsd", "plan9",   "solaris", "wasip1",    "windows"};
static const char *const KnownArch[] = {
    "386",     "amd64",   "arm",    "arm64",    "loong64",
    "mips",    "mipsle",  "mips64", "mips64le", "ppc64",
    "ppc64le", "riscv64", "s390x",  "sparc64",  "wasm"};

static bool isKnown(StringRef S, const char *const *Begin, size_t N) {
  for (size_t I = 0; I < N; ++I)
    if (S == Begin[I])
      return true;
  return false;
}
static bool isOS(StringRef S) {
  return isKnown(S, KnownOS, std::size(KnownOS));
}
static bool isArch(StringRef S) {
  return isKnown(S, KnownArch, std::size(KnownArch));
}

namespace {
/// Recursive descent parser for `a && (b || !c)`.
struct ConstraintParser {
  StringRef S;
  size_t P = 0;
  const Loader &L;
  std::string Err;

  void ws() {
    while (P < S.size() && (S[P] == ' ' || S[P] == '\t'))
      ++P;
  }
  bool tag(StringRef T) const {
    const LoadOptions &O = L.options();
    if (T == O.OS || T == O.Arch || T == O.Profile || T == "gopp")
      return true;
    if (T == "unix")
      return O.OS == "linux" || O.OS == "darwin" || O.OS == "freebsd" ||
             O.OS == "netbsd" || O.OS == "openbsd" || O.OS == "solaris" ||
             O.OS == "aix" || O.OS == "android" || O.OS == "ios";
    return std::find(O.Tags.begin(), O.Tags.end(), T) != O.Tags.end();
  }
  bool orExpr() {
    bool V = andExpr();
    for (;;) {
      ws();
      if (S.substr(P).starts_with("||")) {
        P += 2;
        bool R = andExpr();
        V = V || R;
      } else {
        return V;
      }
    }
  }
  bool andExpr() {
    bool V = notExpr();
    for (;;) {
      ws();
      if (S.substr(P).starts_with("&&")) {
        P += 2;
        bool R = notExpr();
        V = V && R;
      } else {
        return V;
      }
    }
  }
  bool notExpr() {
    ws();
    if (P < S.size() && S[P] == '!') {
      ++P;
      return !notExpr();
    }
    return atom();
  }
  bool atom() {
    ws();
    if (P < S.size() && S[P] == '(') {
      ++P;
      bool V = orExpr();
      ws();
      if (P < S.size() && S[P] == ')')
        ++P;
      else if (Err.empty())
        Err = "missing )";
      return V;
    }
    size_t B = P;
    while (P < S.size() && (std::isalnum(static_cast<unsigned char>(S[P])) ||
                            S[P] == '_' || S[P] == '.'))
      ++P;
    if (B == P) {
      if (Err.empty())
        Err = P < S.size() ? "unexpected '" + std::string(1, S[P]) + "'"
                           : "unexpected end of expression";
      if (P < S.size())
        ++P;
      return false;
    }
    return tag(S.substr(B, P - B));
  }
};
} // namespace

bool Loader::evalConstraint(StringRef Expr, bool &Result,
                            std::string &Error) const {
  ConstraintParser CP{Expr, 0, *this, {}};
  bool V = CP.orExpr();
  CP.ws();
  if (CP.Err.empty() && CP.P != Expr.size())
    CP.Err = "unexpected '" + std::string(1, Expr[CP.P]) + "'";
  if (!CP.Err.empty()) {
    Error = CP.Err;
    return false;
  }
  Result = V;
  return true;
}

/// The `//gopp:build` line in the comments before the package clause.
static bool findConstraint(StringRef Content, std::string &Expr) {
  size_t Pos = 0;
  while (Pos < Content.size()) {
    size_t Eol = Content.find('\n', Pos);
    StringRef Line = Content.substr(
        Pos, Eol == StringRef::npos ? StringRef::npos : Eol - Pos);
    Line = Line.trim();
    if (Line.empty()) {
      // blank line
    } else if (Line.starts_with("//")) {
      StringRef Body = Line.drop_front(2);
      if (Body.starts_with("gopp:build ") || Body == "gopp:build") {
        Expr = Body.drop_front(10).trim().str();
        return true;
      }
    } else if (Line.starts_with("/*")) {
      size_t End = Content.find("*/", Pos);
      if (End == StringRef::npos)
        return false;
      Pos = End + 2;
      continue;
    } else {
      return false; // first non-comment line
    }
    if (Eol == StringRef::npos)
      break;
    Pos = Eol + 1;
  }
  return false;
}

bool Loader::matchFile(StringRef Name, StringRef Content, bool &Selected,
                       bool &IsTest, std::string &Error) const {
  Selected = false;
  IsTest = false;
  if (Name.starts_with("_") || Name.starts_with("."))
    return true;
  if (!Name.ends_with(".gopp"))
    return true;
  StringRef Stem = Name.drop_back(5);
  if (Stem.ends_with("_test")) {
    IsTest = true;
    Stem = Stem.drop_back(5);
  }
  // _os, _arch, _os_arch suffixes.
  SmallVector<StringRef, 4> Parts;
  Stem.split(Parts, '_');
  if (Parts.size() >= 2) {
    StringRef Last = Parts.back();
    StringRef Prev = Parts.size() >= 3 ? Parts[Parts.size() - 2] : StringRef();
    if (isArch(Last)) {
      if (Last != LO.Arch) {
        return true;
      }
      if (!Prev.empty() && isOS(Prev) && Prev != LO.OS)
        return true;
    } else if (isOS(Last)) {
      if (Last != LO.OS)
        return true;
    }
  }
  std::string Expr;
  if (findConstraint(Content, Expr)) {
    bool V = false;
    std::string E;
    if (!evalConstraint(Expr, V, E)) {
      Error = E;
      return false;
    }
    if (!V)
      return true;
  }
  Selected = true;
  return true;
}

//===----------------------------------------------------------------------===//
// Loading
//===----------------------------------------------------------------------===//

Loader::Loader(SourceManager &SM, DiagnosticsEngine &Diags, TypeContext &Ctx,
               LoadOptions LO, CheckOptions CO)
    : SM(SM), Diags(Diags), Ctx(Ctx), LO(std::move(LO)), CO(CO) {}

llvm::ArrayRef<const ast::File *>
Loader::filesOf(const CheckedPackage *P) const {
  for (auto &KV : Loaded)
    if (KV.second->Checked.get() == P)
      return KV.second->Files;
  return {};
}

bool Loader::selectFiles(StringRef Dir, std::vector<std::string> &Files) {
  std::error_code EC;
  std::vector<std::string> Found;
  for (llvm::sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC;
       It.increment(EC)) {
    StringRef Path = It->path();
    StringRef Name = llvm::sys::path::filename(Path);
    if (!Name.ends_with(".gopp") || llvm::sys::fs::is_directory(Path))
      continue;
    auto Buf = llvm::MemoryBuffer::getFile(Path);
    if (!Buf) {
      Diags.report({}, diag::err_load_cannot_read)
          << Path << Buf.getError().message();
      continue;
    }
    bool Selected, IsTest;
    std::string Err;
    if (!matchFile(Name, (*Buf)->getBuffer(), Selected, IsTest, Err)) {
      Diags.report({}, diag::err_load_bad_build_constraint)
          << (std::string(Path) + ": " + Err);
      continue;
    }
    if (!Selected || (IsTest && !LO.IncludeTests))
      continue;
    Found.push_back(Path.str());
  }
  if (EC) {
    Diags.report({}, diag::err_load_cannot_read) << Dir << EC.message();
    return false;
  }
  std::sort(Found.begin(), Found.end());
  Files = std::move(Found);
  return true;
}

std::string Loader::dirOf(StringRef ImportPath) const {
  StringRef P = ImportPath;
  if (!LO.ModulePath.empty()) {
    if (P == LO.ModulePath)
      P = "";
    else if (P.starts_with(LO.ModulePath) && P.size() > LO.ModulePath.size() &&
             P[LO.ModulePath.size()] == '/')
      P = P.drop_front(LO.ModulePath.size() + 1);
  }
  llvm::SmallString<128> D(LO.Root);
  if (!P.empty())
    llvm::sys::path::append(D, P);
  return std::string(D);
}

bool Loader::internalAllowed(StringRef Importer, StringRef Path) const {
  // The last `internal` element decides.
  size_t Pos = StringRef::npos;
  StringRef Rest = Path;
  size_t Off = 0;
  for (;;) {
    size_t Slash = Rest.find('/');
    StringRef Elem = Rest.substr(0, Slash);
    if (Elem == "internal")
      Pos = Off;
    if (Slash == StringRef::npos)
      break;
    Off += Slash + 1;
    Rest = Rest.substr(Slash + 1);
  }
  if (Pos == StringRef::npos)
    return true;
  StringRef Parent = Path.substr(0, Pos == 0 ? 0 : Pos - 1);
  if (Pos == 0)
    return true; // module-root internal: the whole module
  if (Importer == Parent)
    return true;
  return Importer.starts_with(Parent) && Importer.size() > Parent.size() &&
         Importer[Parent.size()] == '/';
}

Package *Loader::import(StringRef Path, std::string &Error) {
  // Foreign headers.
  if (Path.starts_with("<") || Path.ends_with(".h") || Path.ends_with(".hpp")) {
    auto It = ForeignPkgs.find(Path.str());
    if (It != ForeignPkgs.end())
      return It->second;
    if (!Foreign) {
      Error = formatDiagnostic(getDiagFormat(diag::err_foreign_unsupported),
                               {Path.str()});
      return nullptr;
    }
    ForeignMem.push_back(std::make_unique<Arena>());
    Package *P = Foreign->header(Path, *ForeignMem.back(), Error);
    if (P)
      ForeignPkgs[Path.str()] = P;
    return P;
  }
  if (Path == "unsafe")
    return Ctx.unsafePackage();
  StringRef Importer = Stack.empty() ? StringRef() : StringRef(Stack.back());
  if (!internalAllowed(Importer, Path)) {
    Diags.report({}, diag::err_load_internal_import) << Path;
    Error = "use of internal package " + Path.str() + " not allowed";
    return nullptr;
  }
  // Import cycles.
  auto Pos = std::find(Stack.begin(), Stack.end(), Path.str());
  if (Pos != Stack.end()) {
    std::string Chain;
    for (auto I = Pos; I != Stack.end(); ++I)
      Chain += *I + " -> ";
    Chain += Path.str();
    Diags.report({}, diag::err_load_import_cycle) << Chain;
    Error = "import cycle not allowed: " + Chain;
    return nullptr;
  }
  auto It = Loaded.find(Path.str());
  if (It != Loaded.end())
    return It->second->Checked ? It->second->Checked->Pkg : nullptr;
  auto F = Failed.find(Path.str());
  if (F != Failed.end()) {
    Error = "package failed to load";
    return nullptr;
  }
  std::string Dir = dirOf(Path);
  if (!llvm::sys::fs::is_directory(Dir) && !LO.StdRoot.empty()) {
    llvm::SmallString<128> Std(LO.StdRoot);
    llvm::sys::path::append(Std, Path);
    if (llvm::sys::fs::is_directory(Std))
      Dir = std::string(Std);
  }
  if (!llvm::sys::fs::is_directory(Dir)) {
    Error = formatDiagnostic(getDiagFormat(diag::err_load_no_such_package),
                             {Path.str(), Dir});
    Failed[Path.str()] = nullptr;
    return nullptr;
  }
  CheckedPackage *CP = loadDir(Dir, Path);
  if (!CP) {
    Failed[Path.str()] = nullptr;
    Error = "package " + Path.str() + " has no usable files";
    return nullptr;
  }
  return CP->Pkg;
}

CheckedPackage *Loader::loadDir(StringRef Dir, StringRef ImportPath) {
  std::vector<std::string> Files;
  if (!selectFiles(Dir, Files))
    return nullptr;
  if (Files.empty()) {
    Diags.report({}, diag::err_load_no_go_files) << Dir;
    return nullptr;
  }
  std::string IP = ImportPath.str();
  if (IP.empty())
    IP = ".";
  return loadFilesImpl(std::move(Files), IP, Dir);
}

CheckedPackage *Loader::loadFiles(llvm::ArrayRef<std::string> Paths,
                                  StringRef ImportPath) {
  return loadFilesImpl(std::vector<std::string>(Paths.begin(), Paths.end()),
                       ImportPath, "");
}

CheckedPackage *Loader::loadFilesImpl(std::vector<std::string> Paths,
                                      StringRef ImportPath, StringRef Dir) {
  auto E = std::make_unique<Entry>();
  E->ImportPath = ImportPath.str();
  E->Dir = Dir.str();
  std::vector<FileID> Ids;
  for (const std::string &P : Paths) {
    llvm::Expected<FileID> F = SM.addFile(P);
    if (!F) {
      Diags.report({}, diag::err_load_cannot_read)
          << P << llvm::toString(F.takeError());
      return nullptr;
    }
    Ids.push_back(*F);
  }
  ParseOptions PO;
  PO.CheckLanguageRules = LO.LanguageRules;
  PO.AllowGenericMethods = !CO.GoCompat;
  PO.AllowNamespaceTypes = !CO.GoCompat;
  PO.AllowExtends = !CO.GoCompat;
  PO.AllowOperators = !CO.GoCompat;
  PO.UncountedSelectors = true; // the checker walks a selector chain by loop
  E->Parsed = parseFiles(SM, Ids, Diags, PO, 1);
  for (ParsedFile &PF : E->Parsed)
    E->Files.push_back(PF.AST);
  // One package name per directory.
  StringRef Name;
  for (const ast::File *F : E->Files) {
    if (!F->Name)
      continue;
    StringRef N = F->Name->Name.str();
    if (Name.empty())
      Name = N;
    else if (N != Name) {
      Diags.report(F->Name->getBegin(), diag::err_load_mixed_packages)
          << Name << N << (Dir.empty() ? StringRef(".") : Dir);
      return nullptr;
    }
  }
  Entry *Raw = E.get();
  Loaded[ImportPath.str()] = std::move(E);
  // Vet looks at the package that was asked for, not at what it imports.
  CheckOptions PkgCO = CO;
  PkgCO.Vet = CO.Vet && Stack.empty();
  Stack.push_back(ImportPath.str());
  Raw->Checked =
      checkPackage(Ctx, SM, ImportPath, Raw->Files, *this, Diags, PkgCO);
  Stack.pop_back();
  Order.push_back(Raw->Checked.get());
  return Raw->Checked.get();
}
