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

/// A checked use of an operator method (ADR-0024), keyed in Info::Operators
/// by the expression or statement the source wrote. The checker checked it
/// as the method call Call (`a.op(b)`, or `b.op(a)` when Swapped) through the
/// ordinary call path (EXP-18), so the lowering (P04.23) lowers that call
/// (its Selections give the receiver path) and needs no second search.
struct OperatorUse {
  /// The operator the source wrote: Add for `a + b` and for `a += b`, Eq for
  /// `a != b`, Cmp for `a < b`, Index for `a[i]`, IndexSet for `a[i] = v`.
  OpKind Op = OpKind::None;
  /// The method called. For an operand that is a type parameter it is the
  /// element of its constraint (the instance's method is found at P04.23).
  Func *Method = nullptr;
  /// The method call the use was checked as; null for the built-in `<=>`.
  const ast::CallExpr *Call = nullptr;
  /// `a[i] op= v` (keyed by the statement): Method is the binary method of
  /// the element type and Call its call (both null when the element type's
  /// operator is built in), Index the `[]` read (its call is the use of the
  /// IndexExpr), IndexSet the `[]=` write, whose call SetCall takes the
  /// result of the middle step as its last argument; the three calls share
  /// `a` and the index expressions, which are evaluated once. `a[i] = v`
  /// (keyed by the IndexExpr): IndexSet only.
  Func *Index = nullptr;
  Func *IndexSet = nullptr;
  const ast::CallExpr *SetCall = nullptr;
  /// A lazy `&&`/`||`: the right operand is passed as a closure (EXP-21).
  bool Lazy = false;
  /// `@Right`: the receiver is the right operand (EXP-26); the operands are
  /// evaluated in the source order.
  bool Swapped = false;
  /// `a != b`: the negation of the `==` call.
  bool Negate = false;
  /// `a < b`, `a <= b`, `a > b`, `a >= b`: the sign test of the `<=>` call
  /// (the comparison token); tok::illegal otherwise.
  tok::Kind Sign = tok::illegal;
  /// `a op= b` calls the in-place pointer-receiver method (EXP-19); false
  /// for `a = a op b`.
  bool InPlace = false;
  /// The built-in `<=>` of numbers and strings: -1, 0 or 1 as cmp.Compare,
  /// NaN as in cmp.Compare (EXP-20).
  bool Builtin = false;
  /// The method is an element of the constraint of a type parameter (GEN-13).
  bool ViaConstraint = false;
  /// The method is declared on an `extends` ancestor of the receiver
  /// (EXP-30).
  bool Inherited = false;
};

/// A scale change that the units checker decided (ADR-0025, EXP-32 to
/// EXP-36), for the lowering (P04.24): the value of the expression is
/// multiplied by Factor, an exact rational > 0 (the factor of the unit it has
/// over the factor of the unit it gets). Keyed by the operand that is
/// rescaled: an operand of `+ - % <=>`, a comparison, `min`, `max`, a `case`
/// value or the right side of `x op= y` that goes to the finer unit (or to
/// the unit of x), the operand of a conversion `T[U](x)`, the receiver of
/// `x.In[U]()`; and, for a product or quotient whose dimension is empty
/// (`d / 500.0[M]`), the expression itself (its value is multiplied by the
/// factor that remains). A constant is folded instead (its value is already
/// in the new unit). Truncate: an integer kind with a ratio that is no
/// integer, which only an explicit conversion may do (it truncates toward
/// zero, EXP-32). Tag: the entry of a `case` value whose comparison converts
/// the switch tag (the coarser side) to the unit of the case value; the tag is
/// one expression for all the cases, so its scale change is kept per case.
struct UnitConv {
  Real Factor;
  bool Truncate = false;
  bool Tag = false;
};

/// An operation of unit-generic code whose value depends on whether a unit
/// has an empty dimension at the instance (GEN-14, for the lowering P04.24).
/// A value whose unit has an empty dimension at the instance is the plain
/// number, as in plain code (EXP-33); every other value is the number in its
/// unit. With c(E) = the factor of E where E has an empty dimension at the
/// instance and 1 elsewhere (a unit parameter may itself be empty: a caller
/// may pass `V/W`), the value is multiplied by:
///   Mul   `x * y`: c(Res) / (c(X) * c(Y));
///   Quo   `x / y`: c(Res) * c(Y) / c(X);
///   Conv  a scale change of the keyed operand from X to Res, beside its
///         UnitConvs factor: c(Res) / c(X) (X null: a number given the unit
///         Res, `T[Res](v)`); Tag as in UnitConv;
///   Const a constant whose number is in the unit Res: the number times
///         c(Res) (a literal, a folded constant expression, a named constant,
///         a constant that a scale change folded).
/// Res null is a plain result (its static factor is in UnitConvs). The units
/// are written over the unit parameters of the generic; c comes from its unit
/// dictionary. A node has at most one site of each role: its own value
/// (Mul, Quo or Const) and its scale change as an operand (Conv).
struct UnitSite {
  enum Kind : uint8_t { Mul, Quo, Conv, Const };
  Kind K = Conv;
  bool Tag = false;
  const UnitCanon *Res = nullptr, *X = nullptr, *Y = nullptr;
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
  /// Every operator expression or statement that resolved to an operator
  /// method, and every built-in `<=>`, keyed by its node (BinaryExpr,
  /// UnaryExpr, StarExpr, IndexExpr, CallExpr, AssignStmt, IncDecStmt,
  /// SendStmt, RangeStmt, a `case` value) (ADR-0024).
  llvm::DenseMap<const ast::Node *, OperatorUse> Operators;
  /// Every scale change of a value with a unit (ADR-0025); an expression with
  /// no entry is not rescaled.
  llvm::DenseMap<const ast::Expr *, UnitConv> UnitConvs;
  /// The collapse sites of unit-generic code (GEN-14), keyed like UnitConvs:
  /// at most one per role (UnitSite).
  llvm::DenseMap<const ast::Expr *, llvm::SmallVector<UnitSite, 1>> UnitSites;
  /// The display form (UNI-5) of each expression that makes one: a unit
  /// literal, a conversion `T[U](x)`, `x.In[U]()`, and an operator whose
  /// result unit comes from its operands (`d / t` is `KM/H`). The lowering
  /// picks the printed form of a type among these and the package tables by
  /// the fixed order (P04.24).
  llvm::DenseMap<const ast::Expr *, const UnitDisplay *> UnitDisplays;

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
