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
#include "llvm/ADT/DenseSet.h"

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
  /// The comparison of a `switch` tag with a case value through a `==`
  /// method (EXP-20), keyed by the case value. Not in Operators: the case
  /// value is an expression with an operator record of its own when it is
  /// `a + b`, and the key is that node.
  llvm::DenseMap<const ast::Expr *, OperatorUse> CaseOperators;
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
  /// The slice expression of every `s = s[i:j]` that shrinks the owned
  /// slice s in place (SLC-6, SLC-7: the kept elements move down and the
  /// capacity stays). Any other slice expression makes a borrowed slice or a
  /// view (SLC-8), whose type is in Types.
  llvm::DenseSet<const ast::SliceExpr *> Reslices;
  /// Every call that takes a place by `move` (an `append` base, a `move`
  /// argument or receiver) and stores its result back into that same place
  /// (OWN-R4: `p = append(p, ...)`, `m[k] = f(m[k])`): the store does not
  /// drop the old value, the call took it (P04.09).
  llvm::DenseSet<const ast::Expr *> SelfMoves;
  /// The first left side of every assignment that is a swap (OWN-R3a: the
  /// left sides are the right sides' places, `a[i], a[j] = a[j], a[i]`): the
  /// values move between the places (P04.09 R4). True for a swap of three or
  /// more places two of which may be one place at run time
  /// (Checker::swapMayAlias): Go's stores may then write one value twice, so
  /// its types must be cloneable (E0452, R18).
  llvm::DenseMap<const ast::Expr *, bool> Swaps;
  /// A copy the language makes (Checker::implicitClone) of a type that
  /// mentions a type parameter and is not cloneable as written: decided at
  /// each instance by the same rule (OWN-R6; InstanceWalk, E0453).
  struct ImplicitCloneUse {
    Type *T = nullptr;
    std::string What, Copied;
  };
  llvm::DenseMap<const ast::Node *, ImplicitCloneUse> ImplicitClones;
  /// Every call argument whose value its parameter takes as it is, although
  /// the two types differ (Checker::assignableAsIs: identical underlying
  /// types, one of them unnamed, as `struct{...}` into a struct type or
  /// `[2]T` into `type P [2]T`): no new value, unlike an Option made from a
  /// value, a view or an interface. The lowering passes such an argument
  /// where it is, as an argument of the parameter's own type (P04.18, review
  /// R14).
  llvm::DenseSet<const ast::Expr *> AsIsArgs;
  /// Every place where a value of a type that is no interface (a concrete
  /// type, or a type parameter, which may be one at an instance) becomes an
  /// interface value: an assignment to an interface or an `Option` of one (an
  /// argument, a result, a literal element, a send, `panic`), a conversion
  /// `I(x)`, and an operand compared with an interface. Keyed by the
  /// expression the checker checked, with the type each value had (several
  /// for the results of one call). Such a value needs the type descriptor of
  /// its dynamic type, which lists its whole method set, so the instance walk
  /// takes the methods of an instance of a generic type from here, for `gopp
  /// check` and the build alike (P04.08, M3).
  llvm::DenseMap<const ast::Expr *, llvm::SmallVector<Type *, 1>>
      IfaceConversions;

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

/// The spelling of unit types for the lowering (descriptor names, panic
/// messages, debug info; ADR-0025, UNI-5, UNI-12): for each canonical unit the
/// display form that comes first in the fixed order (fewest names, then text)
/// among the forms the package's own expressions make (Info::UnitDisplays) and
/// the package table (declarations and imports), so `KM/H` and never
/// `(5/18) M/S` when any spelling of it was written, whatever the order the
/// code was checked or lowered in. A unit that has no written form prints in
/// its canonical text.
class UnitSpeller {
public:
  UnitSpeller(TypeContext &Ctx, const Package *Pkg, const Info &I);
  /// `float64[KM]` for a unit type, `KM` for a unit argument. Units of the
  /// package Qual are written bare, others as `pkg.Name`.
  std::string text(const BasicType *T, const Package *Qual) const;
  /// A display form D as written (`U/KM`); Qual as in text.
  static std::string display(const UnitDisplay *D, const Package *Qual);

private:
  const Package *Pkg;
  llvm::DenseMap<const UnitCanon *, const UnitDisplay *> Best;
};

/// The unit U with the unit parameters Rep answers for replaced by their
/// units (a parameter Rep gives null for stays). Null if the result leaves
/// the range of units (UNI-2).
const UnitCanon *substituteUnit(
    TypeContext &Ctx, const UnitCanon *U,
    llvm::function_ref<const UnitCanon *(const TypeParamType *)> Rep);

/// The declared base units any unit of the program mentions, in the order of
/// their package path and name (the run-time unit descriptors have one
/// exponent for each, P04.24).
void unitBaseUnits(TypeContext &Ctx,
                   llvm::SmallVectorImpl<const Object *> &Out);

/// Resolves import paths. The loader implements it for source packages; the
/// foreign hook (P03.09) answers C/C++ header paths.
class Importer {
public:
  virtual ~Importer() = default;
  /// The imported package for Path, or null after reporting nothing: the
  /// checker reports "could not import".
  virtual Package *import(llvm::StringRef Path, std::string &Error) = 0;
};

/// One package-variable initialization, in the order Go runs them (the spec's
/// "package initialization": repeatedly the earliest variable in declaration
/// order that depends on no uninitialized variable). `var a, b = f()` is one
/// entry with two Lhs; `var a, b = 1, 2` is two.
struct InitEntry {
  llvm::ArrayRef<Var *> Lhs;
  const ast::Expr *Init = nullptr;
};

struct CheckedPackage {
  Package *Pkg = nullptr;
  /// The context and source manager the package was checked with, and its
  /// options (the lowering's MethodSets asks the checker's own lookup).
  TypeContext *Ctx = nullptr;
  const SourceManager *SM = nullptr;
  CheckOptions Opts;
  Info I;
  /// The initializations of the package-level variables that have an
  /// initializer, in initialization order (a variable without one is zero).
  std::vector<InitEntry> InitOrder;
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

/// The declared method an instantiated method comes from (itself for a method
/// of a plain type).
const Func *declaredMethod(const Func *F);

/// A method found in the method set of a type (MethodSets::lookup).
struct MethodHit {
  /// The method; null if the type has no such method in its method set (a
  /// pointer-receiver method of a value type, a move receiver, a field of
  /// that name, an ambiguous name).
  const Func *F = nullptr;
  /// The embedded fields to follow from the type to the receiver of F: field
  /// indexes (Selection::Index without the index of the method itself).
  llvm::SmallVector<int, 4> Path;
  /// A parent part of an `extends` chain stands before Path (the lowering of
  /// `extends` is P04.22).
  uint32_t ParentSteps = 0;
  /// Some embedded field on the way is a pointer.
  bool Indirect = false;
};

/// The checker's method lookup for the lowering of interfaces (P04.06): the
/// method sets that decide which method an interface call reaches. It runs
/// the checker's own lookup (Lookup.cpp), so the lowering never decides a
/// second time what `x.M` means; and the checker's own walk over the types
/// that hold a view (P04.07). Not thread safe; one per lowering.
class MethodSets {
public:
  /// ScanMethods (test-only): the checker's lookup scans a type's methods
  /// for each name instead of using its name index (a stress test's
  /// quadratic control).
  explicit MethodSets(const CheckedPackage &CP, bool ScanMethods = false);
  ~MethodSets();
  /// Test-only work count: the steps of the lookups so far (the methods the
  /// checker compared, the names collected), for the lowering's count.
  uint64_t steps() const;
  /// The methods of the interface type Iface (it, or a defined type over
  /// one), embedded interfaces flattened, in the type set's order (sorted
  /// by name; an unexported name belongs to its package).
  llvm::ArrayRef<Func *> interfaceMethods(Type *Iface);
  /// The method of the type Dyn's method set that has the name and package
  /// of M (Dyn is the dynamic type of an interface value: not addressable).
  MethodHit lookup(Type *Dyn, const Func *M);
  /// Every method in the method set of Dyn, in an order that depends only
  /// on the names, without operator methods, generic methods and methods
  /// with a `move` receiver.
  void methodSet(Type *Dyn, llvm::SmallVectorImpl<MethodHit> &Out);
  /// Can values of the dynamic type T be compared (`==` of two interface
  /// values holding T does not panic)?
  bool comparable(Type *T);
  /// Does a value of type T hold a view (OWN-V5)? The checker's own walk
  /// (Checker::containsView, the one that asks for `@View` on a struct whose
  /// field holds one): a view, or a slice, array, map, channel, Option or
  /// defined non-struct type of one, a `@View` struct and an instance of a
  /// generic struct by their fields (instances are expanded as the checker
  /// expands them). Pointer kinds, funcs and interfaces are not looked into
  /// (what they point to or box is the borrow checker's, P04.20). Memoized
  /// for the package (a walk that finds no view proves every struct it
  /// expanded holds none), so the questions cost linear work in all.
  bool holdsView(Type *T);
  /// Does the value of V hold a borrow (OWN-B1)? It is one (Var::isBorrow:
  /// borrowed, an `&` alias, a view or a borrowed `[]T`), or its type holds
  /// a view inside it (holdsView: a `@View` struct, an array, Option, slice,
  /// map or channel of views). A closure or method value that outlives the
  /// call may not hold it (CLO-3a; P04.07 review R7).
  bool holdsBorrow(const Var *V);
  /// The same at an instance of generic code, where V's type is Ty (P04.08,
  /// decision M6; GEN-5: the instance has the real category of each type
  /// argument): V borrows by its mode there (modeAt), and a `T` that is a
  /// view or holds one holds a borrow. A box binding that the instance
  /// copies out is a copy (the lowering points its view at a copy of its
  /// own, never into the box).
  bool holdsBorrowAt(const Var *V, Type *Ty);
  /// V's mode at an instance of generic code where its type is Ty: every
  /// decision of the checker that reads the variable's type, made again with
  /// the real type, by the rule plain code uses (review R5): a parameter or
  /// a value receiver borrows when Ty is owning (PAR-1,
  /// Checker::checkSigOwnership and checkMoveRecvSig); a range key or value
  /// and an Option's Get binding when Ty is owning (RNG-1, ADR-0018 point
  /// 11); a box binding when its case type, Ty's view target, is not a plain
  /// copy (TYP-9, Checker::bindsBox). A mode the syntax gives (`move`, `&`)
  /// and a variable the checker left owned keep theirs (a category is never
  /// better at the generic code than at an instance: OWN-C5).
  Var::Borrow modeAt(const Var *V, Type *Ty);
  /// Is T of the owning category (OWN-C1: a string, slice or map, or a type
  /// that holds one by value; an unconstrained type parameter too, OWN-C5)?
  /// What makes a parameter or a value receiver of type T borrowed (PAR-1).
  bool isOwning(Type *T);
  /// Is T of the shared-handle category (OWN-C3: an interface value, a func
  /// value, a channel, `Shared[*T]`, `Weak[*T]`, or a struct or array of
  /// them only)? Copying one retains; ending a copy releases (P04.09).
  bool isSharedHandle(Type *T);
  /// The `drop` method (OWN-R7) of the defined type T (the instance's, for
  /// an instance of a generic type), or null: only T itself, never a type
  /// that holds one.
  const Func *dropMethod(Type *T);
  /// Is T cloneable (OWN-R6: Checker::satisfiesClone, the one decision)?
  bool cloneable(Type *T);
  /// The `Clone() T` method the program wrote for the defined type T (OWN-R6:
  /// a value receiver), or null: the compiler's own clone then copies the
  /// fields.
  const Func *cloneMethod(Type *T);
  /// Test-only work count: the steps of holdsView's walks so far.
  uint64_t viewSteps() const;

  //===------------------------------------------------------------------===//
  // Generics (P04.08): the checker's own substitution and lookups, so the
  // instance of a generic body is decided by the code that decided the
  // generic body (GEN-5, GEN-12).
  //===------------------------------------------------------------------===//
  using TypeMap = llvm::DenseMap<const Type *, Type *>;
  /// T with the type parameters of Map replaced (Checker::substImpl). Every
  /// instance of a generic type the result reaches is expanded, so
  /// sema::under sees its fields, and its methods are instantiated.
  /// Memoized per (Map, T) by the caller; the result mentions no type
  /// parameter unless the substitution is too deep (mentionsTypeParam).
  Type *subst(Type *T, const TypeMap &Map);
  /// Expands every instance of a generic type that T reaches (its
  /// underlying type and methods exist), so sema::under works on them. The
  /// lowering calls it on every checker type it takes from Info.
  void expand(Type *T);
  /// Does T mention a type parameter (in an instance's arguments, a field, a
  /// signature, a constraint) or a type declared inside generic code? One
  /// memoized, iterative walk: linear over all the types asked.
  bool mentionsTypeParam(Type *T);
  /// Does T mention a type parameter for which Pick is true (as a type, or
  /// in a unit)? A type declared in generic code, or a generic type without
  /// arguments, counts as mentioning every one. The same iterative walk as
  /// mentionsTypeParam, memoized in the caller's Memo (one per Pick).
  bool mentionsTypeParamIf(Type *T,
                           llvm::function_ref<bool(const TypeParamType *)> Pick,
                           llvm::DenseMap<const Type *, bool> &Memo);
  /// The height of T (levels of nesting through its parts; a named type
  /// counts as its type arguments only) and its size as a tree (a part
  /// shared n times counts n times, saturating at SizeCap). A generic that
  /// instantiates itself with ever larger arguments grows one or the other,
  /// and the instance walk stops at a bound (GEN-5, E0449). One memoized
  /// post-order walk over the types as a DAG: a diamond of 2^n paths costs n.
  struct Extent {
    unsigned Height = 0;
    uint64_t Size = 0;
  };
  static constexpr uint64_t SizeCap = uint64_t(1) << 32;
  Extent extent(Type *T);
  /// The selection In (recorded for generic code, on a receiver of type
  /// In.Recv) resolved on Recv, the receiver's type at an instance: the
  /// field or method the plain code `x.f` would find (the same lookup, so a
  /// method of a type parameter's constraint is the concrete type's
  /// method). False if there is none (the caller reports). For an operator
  /// element of a constraint (In.Obj is an operator method) ElemSig is the
  /// element's signature at the instance: the method is then the one of the
  /// overload set with that signature (Checker::opAtInstance), and false
  /// means the built-in operator.
  bool resolveSelection(const Selection &In, Type *Recv, Selection &Out,
                        Type *ElemSig = nullptr);
  /// ADR-0018 point 11 and ADR-0020 on a type argument at an instance: why
  /// Option[Arg] or View[Arg] is not allowed, or null (the plain code's
  /// predicates, Checker::badOptionArg and badViewArg).
  const char *badOptionArg(Type *Arg);
  const char *badViewArg(Type *Arg);
  /// Go++'s spelling of a type in a message.
  std::string typeText(Type *T);
  /// Are the two types identical (Go's rule, `byte` and `uint8` too)?
  bool identical(Type *A, Type *B);
  /// Does a value of T have a zero value (TYP-15, OWN-R9)? The predicate of
  /// the checked results (OWN-P10): a result without one is unset when the
  /// call fails.
  bool hasZeroValue(Type *T);

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

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
