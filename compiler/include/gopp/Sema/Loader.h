//===- gopp/Sema/Loader.h - The package loader ------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P03.01. A directory is a package. The loader:
//   - selects the files of the directory: `.gopp` files, minus `_test` files,
//     minus files whose `_os`, `_arch` or `_os_arch` suffix or `//gopp:build`
//     constraint is false for the target (and the build profile name);
//   - maps import paths to directories below a root;
//   - applies the `internal/` rule and reports import cycles;
//   - records `"<header>"` imports as opaque foreign packages (P03.09
//     replaces the provider; P03 ships a mock);
//   - checks every imported package before the importing one.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_LOADER_H
#define GOPP_SEMA_LOADER_H

#include "gopp/Parse/Parser.h"
#include "gopp/Sema/Sema.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gopp {
namespace sema {

struct LoadOptions {
  /// Directory import paths are resolved in.
  std::string Root = ".";
  /// Import path of Root ("" means paths are relative to Root).
  std::string ModulePath;
  std::string OS = "linux";
  std::string Arch = "amd64";
  /// Build profile name (`dev`, `release`, ...): true in `//gopp:build`.
  std::string Profile = "dev";
  std::vector<std::string> Tags;
  /// Include `_test.gopp` files.
  bool IncludeTests = false;
  /// Turn the parser's Go++ language rules off (for Go sources).
  bool LanguageRules = true;
};

/// Provides the packages made from C/C++ headers (P03.09).
class ForeignProvider {
public:
  virtual ~ForeignProvider() = default;
  /// The package for the header path (e.g. `<vector>`), or null with Error.
  virtual Package *header(llvm::StringRef Path, Arena &Mem,
                          std::string &Error) = 0;
};

/// A ForeignScope with objects added by hand, for tests.
class MockForeignScope : public ForeignScope {
public:
  Object *lookup(llvm::StringRef Name) override {
    auto It = Objs.find(Name);
    return It == Objs.end() ? nullptr : It->second;
  }
  void add(Object *O) { Objs[O->Name] = O; }

private:
  llvm::StringMap<Object *> Objs;
};

/// Makes foreign packages with a MockForeignScope per header.
class MockForeignProvider : public ForeignProvider {
public:
  Package *header(llvm::StringRef Path, Arena &Mem,
                  std::string &Error) override;
  /// The scope of a header (created on demand), to add objects to.
  MockForeignScope &scopeOf(llvm::StringRef Path);

private:
  std::map<std::string, std::unique_ptr<MockForeignScope>> Scopes;
};

class Loader : public Importer {
public:
  Loader(SourceManager &SM, DiagnosticsEngine &Diags, TypeContext &Ctx,
         LoadOptions LO, CheckOptions CO = {});

  void setForeignProvider(ForeignProvider *P) { Foreign = P; }

  /// Loads and checks the package in directory Dir. ImportPath is its import
  /// path (empty: derived from Dir relative to the root). Null if the
  /// directory has no files or cannot be read (reported).
  CheckedPackage *loadDir(llvm::StringRef Dir, llvm::StringRef ImportPath = {});
  /// Loads and checks the given files as one package.
  CheckedPackage *
  loadFiles(llvm::ArrayRef<std::string> Paths,
            llvm::StringRef ImportPath = "command-line-arguments");

  Package *import(llvm::StringRef Path, std::string &Error) override;

  /// The files of Dir the build constraints select, sorted.
  bool selectFiles(llvm::StringRef Dir, std::vector<std::string> &Files);

  /// True if the file name's `_os`/`_arch` suffix and the constraint at the
  /// top of Content hold; IsTest is set for `_test` files. Error is set (and
  /// false returned with IsTest unchanged) for a malformed constraint.
  bool matchFile(llvm::StringRef Name, llvm::StringRef Content, bool &Selected,
                 bool &IsTest, std::string &Error) const;
  /// Evaluates a `//gopp:build` expression.
  bool evalConstraint(llvm::StringRef Expr, bool &Result,
                      std::string &Error) const;

  const LoadOptions &options() const { return LO; }
  /// The packages checked so far, in dependency order.
  const std::vector<CheckedPackage *> &packages() const { return Order; }
  /// The parsed files of all packages, for dumps.
  llvm::ArrayRef<const ast::File *> filesOf(const CheckedPackage *P) const;

private:
  struct Entry {
    std::string ImportPath;
    std::string Dir;
    std::unique_ptr<CheckedPackage> Checked;
    std::vector<ParsedFile> Parsed;
    std::vector<const ast::File *> Files;
    Arena *Mem = nullptr;
  };
  CheckedPackage *loadFilesImpl(std::vector<std::string> Paths,
                                llvm::StringRef ImportPath,
                                llvm::StringRef Dir);
  bool internalAllowed(llvm::StringRef Importer, llvm::StringRef Path) const;
  std::string dirOf(llvm::StringRef ImportPath) const;

  SourceManager &SM;
  DiagnosticsEngine &Diags;
  TypeContext &Ctx;
  LoadOptions LO;
  CheckOptions CO;
  ForeignProvider *Foreign = nullptr;
  std::map<std::string, std::unique_ptr<Entry>> Loaded;
  std::map<std::string, Package *> Failed;
  std::vector<std::string> Stack; // import paths being loaded
  std::vector<CheckedPackage *> Order;
  std::vector<std::unique_ptr<Arena>> ForeignMem;
  std::map<std::string, Package *> ForeignPkgs;
};

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_LOADER_H
