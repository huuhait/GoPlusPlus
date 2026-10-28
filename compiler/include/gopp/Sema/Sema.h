//===- gopp/Sema/Sema.h - The type checker interface ------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Entry point of the checker (P03). The shape follows go/types and Clang's
// Sema:
//   - checkPackage() resolves and type-checks the files of one package and
//     returns a CheckedPackage: the Package (scope, objects), the typed side
//     table `Info` keyed by AST node, and the arena that owns the objects.
//   - Phase 1 (single threaded) collects the package-level objects and checks
//     their declarations and signatures lazily, with cycle detection (D3.4).
//     After it the package-level objects and their types are fixed, but not
//     all package-level state: a body marks an imported package as used
//     (PkgName::Used). P03.10 must make that write thread safe (an atomic
//     flag, or a per-body set merged after the bodies).
//   - Phase 2 checks every function body with a fresh body checker: its own
//     arena, own side-table fragment, own diagnostics. The diagnostics are
//     replayed in declaration order, so the output does not depend on the
//     order or the thread the bodies ran on (P03.10 runs them in parallel).
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_SEMA_H
#define GOPP_SEMA_SEMA_H

#include "gopp/AST/AST.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Sema/Types.h"

#include "llvm/ADT/DenseMap.h"

#include <memory>
#include <string>

namespace gopp {
namespace sema {

/// What an expression denotes (types2's operand modes).
enum class Mode : uint8_t {
  Invalid,
  NoValue,
  Builtin,
  TypeExpr,
  Constant,
  Variable,
  MapIndex,
  Value,
  NilValue,
  CommaOk,
  CommaErr,
};
const char *getModeName(Mode M);

struct TypeAndValue {
  Type *Ty = nullptr;
  const ConstValue *Val = nullptr;
  Mode M = Mode::Invalid;
};

/// The result of a selector lookup.
struct Selection {
  enum Kind : uint8_t { FieldVal, MethodVal, MethodExpr } K = FieldVal;
  Type *Recv = nullptr;
  Object *Obj = nullptr;
  llvm::ArrayRef<int> Index;
  bool Indirect = false;
  Type *Ty = nullptr;
};

struct Instance {
  llvm::ArrayRef<Type *> TArgs;
  Type *Ty = nullptr;
};

/// The typed side table (D3.4): the AST stays immutable.
struct Info {
  llvm::DenseMap<const ast::Expr *, TypeAndValue> Types;
  llvm::DenseMap<const ast::Ident *, Object *> Defs;
  llvm::DenseMap<const ast::Ident *, Object *> Uses;
  llvm::DenseMap<const ast::Node *, Object *> Implicits;
  llvm::DenseMap<const ast::SelectorExpr *, Selection> Selections;
  llvm::DenseMap<const ast::Expr *, Instance> Instances;

  const TypeAndValue *lookup(const ast::Expr *E) const {
    auto It = Types.find(E);
    return It == Types.end() ? nullptr : &It->second;
  }
  Type *typeOf(const ast::Expr *E) const {
    const TypeAndValue *TV = lookup(E);
    return TV ? TV->Ty : nullptr;
  }
  /// Moves all entries of Other into this table.
  void merge(Info &&Other);
};

struct CheckOptions {
  /// Go conformance mode: Go's `new` and no Go++ built-ins, and every
  /// ownership rule is off (the checker accepts exactly Go's programs).
  bool GoCompat = false;
  /// Run the type-level ownership checks (P03.07).
  bool Ownership = true;
  /// Check function bodies (phase 2).
  bool CheckBodies = true;
  /// Threads for phase 2 (1 = on the calling thread; P03.10 raises this).
  unsigned BodyThreads = 1;
  /// Report unused variables and imports.
  bool ReportUnused = true;
  /// `-lang`-like switch of the conformance suite: language version 1.N for
  /// the checks that depend on it (0 = latest).
  unsigned GoVersion = 0;
};

/// Resolves import paths. The loader implements it for source packages; the
/// foreign hook (P03.09) answers C/C++ header paths.
class Importer {
public:
  virtual ~Importer() = default;
  /// The imported package for Path, or null after reporting nothing: the
  /// checker reports "could not import".
  virtual Package *import(llvm::StringRef Path, std::string &Error) = 0;
};

struct CheckedPackage {
  Package *Pkg = nullptr;
  Info I;
  /// Owns the objects and scopes Pkg and I refer to. Body checkers' arenas
  /// are added here.
  std::vector<std::unique_ptr<Arena>> Mem;
  /// Names of the bodies in the order their diagnostics were replayed.
  unsigned NumBodies = 0;
};

/// Type-checks the files of one package. All files must be parsed. The
/// package is named by the first file's `package` clause; PkgPath is its
/// import path.
std::unique_ptr<CheckedPackage>
checkPackage(TypeContext &Ctx, const SourceManager &SM, llvm::StringRef PkgPath,
             llvm::ArrayRef<const ast::File *> Files, Importer &Imp,
             DiagnosticsEngine &Diags, const CheckOptions &Opts = {});

/// Prints the type of every expression and the object of every identifier of
/// the files, one per line (`gopp check -dump-types`).
void dumpTypes(llvm::raw_ostream &OS, const SourceManager &SM,
               const CheckedPackage &P,
               llvm::ArrayRef<const ast::File *> Files);

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_SEMA_H
