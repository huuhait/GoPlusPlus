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
//     After it the package-level objects and their types are fixed.
//   - Phase 2 checks every function body with a fresh body checker, on
//     CheckOptions::BodyThreads threads (D3.4, P03.10). The checker writes to
//     its thread's worker: an arena, a side table and a set of the imports it
//     used (one thread writes straight into the package's table). Each body's
//     diagnostics are kept apart and replayed in declaration order, so the
//     output does not depend on the order or the thread the bodies ran on.
//     A body writes nothing that another body can read: shared state (the
//     TypeContext, the SourceManager's line tables, the lazily completed
//     parts of types) is internally locked, and everything else a body writes
//     is its own.
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

namespace llvm {
class ThreadPoolInterface;
}

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
  /// A member of an `extends` ancestor: Index is preceded by this many parent
  /// parts (field 0 at each level), which are not spelled out.
  uint32_t ParentSteps = 0;
  bool Indirect = false;
  Type *Ty = nullptr;
  /// `base.M`: the receiver is the parent part, and the call is static, to
  /// the parent's version of M (TYP-20, EXP-14).
  bool Base = false;
};

struct Instance {
  llvm::ArrayRef<Type *> TArgs;
  Type *Ty = nullptr;
};

/// What a `call!()` does when the call fails (ERR-6, ERR-11a). Decided by the
/// innermost enclosing function: GIR lowering (P04) and `gopp vet` read it.
enum class BangKind : uint8_t {
  Return,   // the enclosing function can fail: it returns the error
  Panic,    // anywhere else: panic with errors.MustError
  DeferSet, // `defer f!()` in a function that can fail: sets the error if nil
};

/// The typed side table (D3.4): the AST stays immutable.
struct Info {
  llvm::DenseMap<const ast::Expr *, TypeAndValue> Types;
  llvm::DenseMap<const ast::Ident *, Object *> Defs;
  llvm::DenseMap<const ast::Ident *, Object *> Uses;
  llvm::DenseMap<const ast::Node *, Object *> Implicits;
  llvm::DenseMap<const ast::SelectorExpr *, Selection> Selections;
  llvm::DenseMap<const ast::Expr *, Instance> Instances;
  /// Every `f!(...)` call that type-checked, and what it does on failure.
  llvm::DenseMap<const ast::CallExpr *, BangKind> Bangs;

  const TypeAndValue *lookup(const ast::Expr *E) const {
    auto It = Types.find(E);
    return It == Types.end() ? nullptr : &It->second;
  }
  Type *typeOf(const ast::Expr *E) const {
    const TypeAndValue *TV = lookup(E);
    return TV ? TV->Ty : nullptr;
  }
  /// Flow typing (ADR-0018 point 4): the narrowed type of a use of an Option
  /// local or parameter. Types[Id] holds the type of the value T at a place
  /// where the variable is known to be set, while Uses[Id] is the Var, whose
  /// type is Option[T]. Returns T for such a use, null otherwise.
  Type *narrowedType(const ast::Ident *Id) const {
    auto U = Uses.find(Id);
    auto T = Types.find(static_cast<const ast::Expr *>(Id));
    if (U == Uses.end() || T == Types.end() || !U->second->Ty ||
        !T->second.Ty || T->second.Ty == U->second->Ty)
      return nullptr;
    // Option[T], or a defined type over one.
    Type *VT = U->second->Ty;
    if (auto *N = llvm::dyn_cast<NamedType>(VT); N && !N->Orig)
      VT = N->Underlying;
    return llvm::isa_and_nonnull<OptionType>(VT) ? T->second.Ty : nullptr;
  }

  /// Adds all entries of the Others (tables of disjoint AST nodes).
  void merge(llvm::ArrayRef<Info *> Others);
};

struct CheckOptions {
  /// Go conformance mode: Go's `new` and no Go++ built-ins, and every
  /// ownership rule is off (the checker accepts exactly Go's programs).
  bool GoCompat = false;
  /// Run the type-level ownership checks (P03.07).
  bool Ownership = true;
  /// Check function bodies (phase 2).
  bool CheckBodies = true;
  /// Threads for phase 2: 1 = on the calling thread, 0 = one per core up to
  /// MaxAutoBodyThreads. The diagnostics and the side table are the same for
  /// every value (D3.4).
  unsigned BodyThreads = 1;
  /// The most threads BodyThreads = 0 picks: more threads are slower, because
  /// new types are created under the type tables' locks (bench/README.md).
  static constexpr unsigned MaxAutoBodyThreads = 8;
  /// Packages with fewer bodies than MinBodiesPerThread per extra thread use
  /// fewer threads: a thread costs more than a few small bodies.
  unsigned MinBodiesPerThread = 16;
  /// A pool shared with other work (the build driver's); null: phase 2 makes
  /// its own pool of BodyThreads threads.
  llvm::ThreadPoolInterface *Pool = nullptr;
  /// Report unused variables and imports.
  bool ReportUnused = true;
  /// Run the `gopp vet` lints after checking (P03.12). Findings are warnings.
  bool Vet = false;
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

/// The `gopp vet` lints (P03.12), run by checkPackage when CheckOptions::Vet
/// is set: `drop` methods that may run on a zero value (a heuristic), a `!`
/// that panics outside package main, and `defer` in a loop body. Reports
/// warnings in source order.
void vetPackage(const SourceManager &SM, const CheckedPackage &P,
                llvm::ArrayRef<const ast::File *> Files,
                DiagnosticsEngine &Diags);

/// Prints the type of every expression and the object of every identifier of
/// the files, one per line (`gopp check -dump-types`).
void dumpTypes(llvm::raw_ostream &OS, const SourceManager &SM,
               const CheckedPackage &P,
               llvm::ArrayRef<const ast::File *> Files);

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_SEMA_H
