//===- gopp/Sema/Types.h - Types, objects and scopes ------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The type representation of the checker (P03.04, D3.8), shaped like Clang's
// ASTContext types and go/types:
//   - Types live in the arena of a TypeContext and are never freed one by one.
//   - Pointer, slice, array, map, chan, ref, tuple, signature, struct and
//     instantiated named types are *uniqued* with llvm::FoldingSet: two types
//     are identical exactly when the pointers are equal. Only interfaces and
//     unions (type sets) need a structural comparison (Checker::identical).
//   - A TypeContext can be shared by many checkers (one per package, and one
//     per function body when bodies are checked in parallel, P03.10): each
//     uniquing table is split in shards by hash, each with its own lock and
//     memory, and the other allocations have their own lock; reading a type
//     needs no lock because a type never changes after its creation, except
//     for the lazily resolved parts documented on NamedType and InterfaceType
//     (they are completed under TypeContext::lock() and published
//     atomically).
//   - Objects (variables, functions, ...) and scopes are not shared state:
//     a checker allocates them in its own arena (class Arena).
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_TYPES_H
#define GOPP_SEMA_TYPES_H

#include "gopp/Basic/SourceManager.h"
#include "gopp/Sema/Constant.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <vector>

namespace gopp {
namespace ast {
class FuncDecl;
class File;
class Ident;
} // namespace ast
namespace sema {

class Type;
class NamedType;
class TypeParamType;
class InterfaceType;
class SignatureType;
class Object;
class Var;
class Func;
class TypeName;
class Package;
class Scope;
class TypeContext;
class UnitCanon;
struct UnitRangeSum;
namespace units {
struct Namer;
}

//===----------------------------------------------------------------------===//
// Arena for the objects and scopes of one checker
//===----------------------------------------------------------------------===//

/// Memory of one checker: objects (trivially destructible), scopes and
/// constant values. Outlives the Info that points into it.
class Arena {
public:
  llvm::BumpPtrAllocator Alloc;
  std::deque<Scope> Scopes;
  std::deque<ConstValue> Consts;

  template <class T, class... Args> T *make(Args &&...A) {
    static_assert(std::is_trivially_destructible_v<T>);
    return new (Alloc.Allocate(sizeof(T), alignof(T)))
        T(std::forward<Args>(A)...);
  }
  template <class T> llvm::ArrayRef<T> copy(llvm::ArrayRef<T> Elts) {
    if (Elts.empty())
      return {};
    T *Mem = Alloc.Allocate<T>(Elts.size());
    std::uninitialized_copy(Elts.begin(), Elts.end(), Mem);
    return llvm::ArrayRef<T>(Mem, Elts.size());
  }
  llvm::StringRef copy(llvm::StringRef S) {
    char *Mem = Alloc.Allocate<char>(S.size() + 1);
    std::copy(S.begin(), S.end(), Mem);
    Mem[S.size()] = 0;
    return llvm::StringRef(Mem, S.size());
  }
  const ConstValue *constant(ConstValue V) {
    Consts.push_back(std::move(V));
    return &Consts.back();
  }
  Scope *newScope(Scope *Parent, SourceRange Extent, const char *Comment);
};

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

enum class BasicKind : uint8_t {
  Invalid,
  Bool,
  Int,
  Int8,
  Int16,
  Int32,
  Int64,
  Uint,
  Uint8,
  Uint16,
  Uint32,
  Uint64,
  Uintptr,
  Float32,
  Float64,
  Complex64,
  Complex128,
  String,
  UnsafePointer,
  UntypedBool,
  UntypedInt,
  UntypedRune,
  UntypedFloat,
  UntypedComplex,
  UntypedString,
  UntypedNil,
  /// A unit used as a type argument (`Vec2[KM]`, GEN-14): a basic type that
  /// carries only a unit (BasicType::Unit). It has no values.
  UnitArg,
  NumBasicKinds
};

enum BasicInfo : uint16_t {
  IsBoolean = 1 << 0,
  IsInteger = 1 << 1,
  IsUnsigned = 1 << 2,
  IsFloat = 1 << 3,
  IsComplex = 1 << 4,
  IsString = 1 << 5,
  IsUntyped = 1 << 6,
  IsOrdered = IsInteger | IsFloat | IsString,
  IsNumeric = IsInteger | IsFloat | IsComplex,
  IsConstType = IsBoolean | IsNumeric | IsString,
};

class Type {
public:
  enum class Kind : uint8_t {
    Basic,
    Pointer,
    Slice,
    Array,
    Map,
    Chan,
    Ref,
    Tuple,
    Signature,
    Struct,
    Interface,
    Union,
    Named,
    TypeParam,
    Option,
  };
  Kind getKind() const { return K; }

protected:
  explicit Type(Kind K) : K(K) {}

private:
  Kind K;
};

/// A unit of measure in canonical form (UNI-5): the base units (or unit type
/// parameters) with their exponents, sorted by name, and the exact factor
/// that turns one of the unit into the base units. Uniqued by
/// TypeContext::unitCanon, so two units are identical exactly when the
/// pointers are equal.
struct UnitDim {
  /// The declared base unit, or null for a unit type parameter.
  const Object *Unit = nullptr;
  /// The unit type parameter (GEN-14), or null.
  const TypeParamType *Param = nullptr;
  int32_t Exp = 0;
};

class UnitCanon {
public:
  /// Sorted by name, no zero exponent, no duplicate.
  llvm::ArrayRef<UnitDim> Dims;
  /// Exact (a rational): see Real::isRat.
  Real Factor;
  bool dimensionless() const { return Dims.empty(); }
  /// Does it mention a unit type parameter?
  bool HasParam = false;
};

/// A display form of a unit (UNI-5): the unit names as written, or as
/// combined by the arithmetic from the forms of the operands (`KM` divided by
/// `H` is `KM/H`), with equal names merged and cancelled. A part is a
/// declared unit (base or derived; UnitDim::Unit) or a unit parameter, and
/// the parts are sorted like the dimensions of a UnitCanon. Interned by
/// TypeContext::unitDisplay: equal forms are one pointer. Names (the number
/// of names, counted with their exponents) and then Key (the text with every
/// name qualified by its package path) are the fixed order that picks the
/// printed form when one type has several (UNI-5): never the order in which
/// the forms were made.
class UnitDisplay {
public:
  llvm::ArrayRef<UnitDim> Parts;
  uint64_t Names = 0;
  llvm::StringRef Key;
  /// The canonical form the parts make (units::displayCanon), once asked: a
  /// function of the parts alone, so it does not matter which thread stores
  /// it first.
  mutable std::atomic<const UnitCanon *> Canon{nullptr};
  /// Does A come before B in the fixed order (fewest names, then the smaller
  /// text)?
  static bool before(const UnitDisplay *A, const UnitDisplay *B) {
    if (A->Names != B->Names)
      return A->Names < B->Names;
    return A->Key < B->Key;
  }
};

/// One entry of a package's table of display forms (Package::UnitNames).
struct UnitNameEntry {
  const UnitCanon *Canon = nullptr;
  const UnitDisplay *Disp = nullptr;
};

class BasicType : public Type {
public:
  BasicType(BasicKind BK, unsigned Info, const char *Name)
      : Type(Kind::Basic), BK(BK), Info(static_cast<uint16_t>(Info)),
        Name(Name) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Basic; }
  BasicKind BK;
  uint16_t Info;
  const char *Name;
  /// `float64[KM]` (UNI-6): the unit; null for the predeclared types. The
  /// kind and the info are those of the base type, so every rule for
  /// numbers holds; identity needs the same unit too (TypeContext::unitType
  /// makes one type per (kind, unit)).
  const UnitCanon *Unit = nullptr;
};

class PointerType : public Type, public llvm::FoldingSetNode {
public:
  explicit PointerType(Type *Elem) : Type(Kind::Pointer), Elem(Elem) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Pointer; }
  void Profile(llvm::FoldingSetNodeID &ID) const { ID.AddPointer(Elem); }
  Type *Elem;
};

/// `Option[T]` (ADR-0018): the one "maybe" of Go++, for any T that does not
/// already have a nil (interfaces, funcs, maps, slices, channels) and is not an
/// Option. Its zero value is nil. A real type, uniqued like a pointer type, so
/// two Option[T] are identical exactly when the pointers are equal; there is
/// no NamedType behind it.
///
/// Layout note for P04: for a pointer kind (`*T`, `Unique[*T]`, `Shared[*T]`,
/// `Weak[*T]`, see usesNiche) the nil Option is the pointer's own empty bit
/// pattern, so Option[*T] has the size and layout of a C pointer; for every
/// other T it is T plus a hidden "is set" flag. Flow typing never changes this
/// type: a narrowed use is recorded in Info::Types as the type of T.
class OptionType : public Type, public llvm::FoldingSetNode {
public:
  explicit OptionType(Type *Elem) : Type(Kind::Option), Elem(Elem) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Option; }
  void Profile(llvm::FoldingSetNodeID &ID) const { ID.AddPointer(Elem); }
  /// True if nil is a pointer's empty bit pattern (no hidden flag).
  bool usesNiche() const;
  Type *Elem;
};

class SliceType : public Type, public llvm::FoldingSetNode {
public:
  explicit SliceType(Type *Elem) : Type(Kind::Slice), Elem(Elem) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Slice; }
  void Profile(llvm::FoldingSetNodeID &ID) const { ID.AddPointer(Elem); }
  Type *Elem;
};

/// `View[X]` (ADR-0020): a read-only borrow of an X, deep. The same type kind,
/// with Borrowed set, is the type of a slice made by slicing a slice or array
/// that may be written (OWN-V2, OWN-R3b item 6): a borrowed `[]T`, whose
/// elements are writable but which cannot grow, move or outlive its owner. It
/// is never written in source. isViewType (Checker) is the one question "is
/// this read-only".
class RefType : public Type, public llvm::FoldingSetNode {
public:
  explicit RefType(Type *Elem, bool Borrowed = false)
      : Type(Kind::Ref), Elem(Elem), Borrowed(Borrowed) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Ref; }
  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddPointer(Elem);
    ID.AddBoolean(Borrowed);
  }
  Type *Elem;
  bool Borrowed;
};

class ArrayType : public Type, public llvm::FoldingSetNode {
public:
  ArrayType(Type *Elem, int64_t Len)
      : Type(Kind::Array), Elem(Elem), Len(Len) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Array; }
  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddPointer(Elem);
    ID.AddInteger(Len);
  }
  Type *Elem;
  int64_t Len; // < 0 if unknown (an error was reported)
};

class MapType : public Type, public llvm::FoldingSetNode {
public:
  MapType(Type *Key, Type *Elem) : Type(Kind::Map), Key(Key), Elem(Elem) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Map; }
  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddPointer(Key);
    ID.AddPointer(Elem);
  }
  Type *Key, *Elem;
};

class ChanType : public Type, public llvm::FoldingSetNode {
public:
  enum Dir : uint8_t { Send = 1, Recv = 2, Both = 3 };
  ChanType(Dir D, Type *Elem) : Type(Kind::Chan), D(D), Elem(Elem) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Chan; }
  void Profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(static_cast<unsigned>(D));
    ID.AddPointer(Elem);
  }
  Dir D;
  Type *Elem;
};

class TupleType : public Type, public llvm::FoldingSetNode {
public:
  explicit TupleType(llvm::ArrayRef<Type *> Elems)
      : Type(Kind::Tuple), Elems(Elems) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Tuple; }
  void Profile(llvm::FoldingSetNodeID &ID) const {
    for (Type *T : Elems)
      ID.AddPointer(T);
  }
  llvm::ArrayRef<Type *> Elems;
  size_t size() const { return Elems.size(); }
};

/// Parameter and result types only; the names live in the declaration (the
/// Func's Params, or the objects made for a FuncLit). `MoveMask` has bit i set
/// for a `move` parameter (D1.5); `Fails` is the `!` mark (the result tuple
/// then already ends with `error`).
class SignatureType : public Type, public llvm::FoldingSetNode {
public:
  SignatureType(TupleType *Params, TupleType *Results, bool Variadic,
                bool Fails, uint64_t MoveMask,
                llvm::ArrayRef<TypeParamType *> TParams)
      : Type(Kind::Signature), Params(Params), Results(Results),
        Variadic(Variadic), Fails(Fails), MoveMask(MoveMask), TParams(TParams) {
  }
  static bool classof(const Type *T) { return T->getKind() == Kind::Signature; }
  void Profile(llvm::FoldingSetNodeID &ID) const;
  TupleType *Params, *Results;
  bool Variadic, Fails;
  uint64_t MoveMask;
  /// Type parameters of a generic function (empty otherwise).
  llvm::ArrayRef<TypeParamType *> TParams;
  bool isMove(unsigned I) const { return I < 64 && (MoveMask >> I) & 1; }
};

class StructType : public Type, public llvm::FoldingSetNode {
public:
  StructType(llvm::ArrayRef<Var *> Fields, bool Strict)
      : Type(Kind::Struct), Fields(Fields), Strict(Strict) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Struct; }
  void Profile(llvm::FoldingSetNodeID &ID) const;
  llvm::ArrayRef<Var *> Fields;
  /// The struct type of a `@Strict` declaration (TYP-15a, ADR-0019). The
  /// effective required bit of each field (Var::Required) already says what
  /// it means; this records the attribute itself (export data, P09, P14).
  bool Strict;
};

struct Term {
  bool Tilde = false;
  Type *T = nullptr;
};

/// Union of terms `~int | string`; a single term is a Union of size one only
/// where a tilde is involved.
class UnionType : public Type {
public:
  explicit UnionType(llvm::ArrayRef<Term> Terms)
      : Type(Kind::Union), Terms(Terms) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Union; }
  llvm::ArrayRef<Term> Terms;
};

/// The type set of an interface: its method set and its terms.
struct TypeSet {
  /// Sorted by name; unexported names of different packages are different.
  llvm::ArrayRef<Func *> Methods;
  /// If AllTerms the set has no restriction on types; else Terms (maybe empty,
  /// the empty set).
  bool AllTerms = true;
  llvm::ArrayRef<Term> Terms;
  bool Comparable = false; // the `comparable` constraint is part of the set
  bool Copy = false;       // Copy constraint (D1.3, GEN-1)
  bool Clone = false;      // Clone constraint (GEN-2)
  bool HasOps = false; // lists an operator element (TYP-27): constraint only
  Type *Extends = nullptr; // `extends P` (GEN-12)
  bool isEmpty() const { return !AllTerms && Terms.empty(); }
  bool isAll() const {
    return AllTerms && !Comparable && !Copy && !Clone && Methods.empty() &&
           !Extends;
  }
  bool hasTerms() const { return !AllTerms; }
};

class InterfaceType : public Type {
public:
  enum Special : uint8_t { None, Comparable, Copy, Clone, Unit };
  InterfaceType() : Type(Kind::Interface) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Interface; }
  llvm::ArrayRef<Func *> Methods;   // explicitly declared methods
  llvm::ArrayRef<Type *> Embeddeds; // embedded types and unions
  llvm::ArrayRef<SourceLoc> EmbedLocs;
  Special Sp = None;
  bool Implicit = false; // `[T int|string]` wrapped into an interface
  /// `extends P` (GEN-12): the type set is P and every type that extends P.
  /// Null if there is none; the location is for messages.
  Type *Extends = nullptr;
  SourceLoc ExtendsLoc;
  bool IsAny = false; // the predeclared `any` (printed as such)
  /// Made by substitution (an instance of a generic interface): its type set
  /// reports no errors. Which body computes it first is a matter of timing, so
  /// the error would land in a different place on each run; the declaration
  /// reports it, once, in phase 1 (types2 uses nopos for instances too).
  bool Substituted = false;
  /// Computed on demand (see Checker::typeSet) and published once complete,
  /// so that body threads read it without the lock.
  std::atomic<TypeSet *> Set{nullptr};
  bool Computing = false;     // under the context's lock
  TypeSet *Pending = nullptr; // the stand-in a recursive request gets
};

class TypeParamType : public Type {
public:
  TypeParamType(TypeName *Obj, unsigned Index)
      : Type(Kind::TypeParam), Obj(Obj), Index(Index) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::TypeParam; }
  TypeName *Obj;
  unsigned Index;
  Type *Bound = nullptr; // the constraint (an interface or a named interface)
  /// One thing the generic code does with this parameter that the plain
  /// code decides on the real type (EXP-15, TYP-19, ADR-0023 decision 16):
  /// it is recorded here as data, and every instantiation decides it on the
  /// real type argument with the predicate of the plain code
  /// (Checker::useWhy), so the two cannot differ (GEN-12).
  struct Use {
    enum Kind : uint8_t {
      Copy,    // `*p` used as a value: copied, moved, assigned through
      Clone,   // a value clone through a pointer or a view: `(*p).Clone()`
      Make,    // a value of T made or held: `var z T`, `T{}`, `make([]T, n)`
      Method,  // the receiver of Member bound, deferred or moved
      ViewArg, // `View[T]` written: T must be no pointer kind and no view
      // GEN-14: the code needs the unit of this unit parameter at run time
      // (its unit dictionary): a collapse site mentions it (Info::UnitSites),
      // or it is passed on to a generic whose code needs it. Never an error.
      UnitDict,
    };
    /// Where the receiver of a Method use is: `x.M` (a T value), `(*p).M`
    /// (through a pointer to T) or `p.M` (p a pointer, owning pointer or
    /// view of T).
    enum Place : uint8_t { Value, Deref, Pointer };
    Kind K = Copy;
    Place P = Value;
    /// A method value or a `go` / `defer` call (the receiver is copied),
    /// else a direct call of a `move` method (the receiver is moved).
    bool Taken = false;
    /// Given on by the generic code to another generic that has the use.
    bool Passed = false;
    llvm::StringRef Member; // Method: the method's name
    Use() = default;
    Use(Kind K, Place P = Value, bool Taken = false,
        llvm::StringRef Member = {})
        : K(K), P(P), Taken(Taken), Member(Member) {}
    auto tie() const { return std::make_tuple(K, P, Taken, Passed, Member); }
    bool operator<(const Use &O) const { return tie() < O.tie(); }
    bool operator==(const Use &O) const { return tie() == O.tie(); }
  };
  /// The parameter this one was copied from for one call's inference
  /// (Checker::renameTParams); the uses are those of the original.
  TypeParamType *RenamedFrom = nullptr;
  /// The uses of this parameter: an immutable sorted set, published once
  /// after all bodies of its package (Checker::untaggedCheck) or read from
  /// export data, so no reader depends on the order of the threads.
  struct UseList {
    llvm::ArrayRef<Use> Uses;
  };
  std::atomic<const UseList *> UseSet{nullptr};
  llvm::ArrayRef<Use> uses() const {
    const UseList *L = UseSet.load(std::memory_order_acquire);
    return L ? L->Uses : llvm::ArrayRef<Use>();
  }
};

/// Predeclared generic pointer types of D1.7.
enum class PtrKind : uint8_t { None, Unique, Shared, Weak };

/// The operator methods of one named type (ADR-0024), grouped by OpKind,
/// and the table of its nearest `extends` ancestor that has any (EXP-30).
/// It is a function of the declarations alone (Checker::opTable), so no
/// answer depends on which thread asks first.
struct OpTable {
  /// Sorted by OpKind, then declaration order. For an instance these are the
  /// instantiated methods.
  llvm::ArrayRef<Func *> Own;
  const OpTable *Next = nullptr;
  /// The methods of each kind that a value of the type has: Own of that kind
  /// then those behind it that Own does not hide (Checker::opMerged), kept
  /// here once made, so a use costs the list, not the depth of the chain.
  /// A function of the tables alone: a thread that loses the race to store
  /// has made the same list.
  static constexpr unsigned MaxKinds = 48;
  mutable std::atomic<const llvm::ArrayRef<Func *> *> Merged[MaxKinds] = {};
};

/// A defined type, or an instantiation of a generic one. Instances are
/// uniqued by (origin, type arguments). `Underlying` and the instantiated
/// methods of an instance are filled in lazily by the checker.
class NamedType : public Type, public llvm::FoldingSetNode {
public:
  explicit NamedType(TypeName *Obj) : Type(Kind::Named), Obj(Obj) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Named; }
  void Profile(llvm::FoldingSetNodeID &ID) const;
  TypeName *Obj;
  Type *Underlying = nullptr;
  NamedType *Orig = nullptr; // the generic type, for an instance
  /// A type declared inside a generic function or method: the type
  /// parameters of the enclosing declarations (in source order). It is
  /// implicitly parameterized by them, a vertex of the instantiation cycle
  /// check (Mono.cpp). Empty for every other type.
  llvm::ArrayRef<TypeParamType *> ImplicitTParams;
  llvm::ArrayRef<TypeParamType *> TParams;
  llvm::ArrayRef<Type *> TArgs;
  PtrKind Ptr = PtrKind::None;
  Type *FromRHS = nullptr;        // the declared right hand side
  NamedType *DirectRhs = nullptr; // `type A B`: the defined type B
  bool Resolving = false;
  /// Set with Resolving when the right hand side is an interface literal: the
  /// type is then an interface without methods yet (types2: an incomplete
  /// interface), so `T.m` inside its own method signatures is "undefined".
  bool ResolvingInterface = false;
  bool Pinned = false;   // @Pinned (OWN-R10)
  bool ViewAttr = false; // @View struct (OWN-V5)
  /// `@Abstract type Bird extends Animal {}` (TYP-19): the type may leave an
  /// `@Abstract` method open, and it is abstract itself.
  bool AbstractAttr = false;
  /// Checker::hasTypeTable (TYP-22): 0 not known, 1 no, 2 yes: the type or
  /// an ancestor has a `@Virtual` or `@Abstract` method. Stored once the
  /// types are final and only when exact, like CloneState.
  std::atomic<uint8_t> TableState{0};
  /// Checker::isAbstractType (TYP-19), on the origin: 0 not known, 1 no, 2 yes
  /// (an `@Abstract` method is open on its path), 3 yes only because of the
  /// `@Abstract` attribute (nothing is open). Stored like TableState.
  std::atomic<uint8_t> AbstractState{0};
  /// The first open `@Abstract` method by position (for messages); stored
  /// before AbstractState becomes 2.
  std::atomic<Func *> OpenWitness{nullptr};
  /// Place in the `extends` hierarchy (Hierarchy.cpp): the depth (1 for a
  /// root, 0 not known), the parent and a skew-binary jump pointer, so "does
  /// C extend P" takes O(log depth) steps, not one per level. Kept for
  /// instances too (an instance's parent is an instance). Written once the
  /// types are final, the depth last.
  std::atomic<uint32_t> HierDepth{0};
  std::atomic<NamedType *> HierParent{nullptr};
  std::atomic<NamedType *> HierJump{nullptr};
  /// A generic type on an instantiation cycle (Mono.cpp, set before any
  /// body is checked): its instances grow without end, so the walks that
  /// follow every instance a type reaches stop at them (Checker::onInstCycle).
  bool InstCycle = false;
  /// The predeclared generic `Option` (instances are OptionType, not Named).
  bool IsOption = false;
  /// The predeclared generic `View` (ADR-0020): an instance is a RefType.
  bool IsView = false;
  /// "Has a zero value" (TYP-15, OWN-R9): 0 not known, 1 yes, 2 no, 3 no only
  /// because of a type parameter (an instance may have one). Stored
  /// only when exact (ZeroQuery in Ownership.cpp): the answer is a function
  /// of the type alone, so every thread that stores it stores the same value.
  std::atomic<uint8_t> ZeroState{0};
  /// Generic declaration with ZeroState 3: the type parameters (bit per
  /// index) its "no zero value" rests on. Written before ZeroState; the same
  /// value whichever thread writes it.
  std::atomic<uint64_t> PolyMask{0};
  /// "Is pinned" (OWN-R10, Checker::isPinned) for the type as a whole: 0 not
  /// known, 1 no, 2 yes. Stored only when exact (nothing below it was still
  /// being declared): the answer is a function of the type alone, so every
  /// thread that stores it stores the same value.
  std::atomic<uint8_t> PinnedState{0};
  /// "Satisfies Clone" (Checker::satisfiesClone): 0 not known, 1 yes, 2 no.
  /// Stored once the types are final (Checker::typesFinal) and only when
  /// exact, like ZeroState.
  std::atomic<uint8_t> CloneState{0};
  /// Checker::opTable: the operator methods of the type and its ancestors.
  /// Null until asked; stored only once the types are final (like
  /// CloneState), and the first table stored stays.
  std::atomic<const OpTable *> OpsState{nullptr};
  /// Checker::pointerFree and Checker::viewCopyHasPointer (OWN-V1, OWN-V3):
  /// 0 not known, 1 no pointer is reached, 2 one is. Stored like CloneState.
  std::atomic<uint8_t> PtrFreeState{0};
  std::atomic<uint8_t> CopyPtrState{0};
  /// Checker::category: 0 not known, else 1 + the Category. Stored like
  /// CloneState.
  std::atomic<uint8_t> CategoryState{0};
  /// Generic declaration: the category of its underlying type with the type
  /// parameters left open: 0 not known, else 1 + the Category of the rest,
  /// and CatParams the parameters (bit per index, 63 for 63 and up) held by
  /// value, whose arguments' categories join it. CatParams is written before
  /// CatGeneric. An instance is answered from these, never expanded.
  std::atomic<uint8_t> CatGeneric{0};
  std::atomic<uint64_t> CatParams{0};
  /// Generic declaration: its type parameters (bit per index, 63 for 63 and
  /// up) that its underlying type holds by value as holdsByValue sees it
  /// (Checker::heldParams); HeldKnown is set after HeldParams, once the types
  /// are final. An instance holding a type parameter is answered from these.
  std::atomic<uint64_t> HeldParams{0};
  std::atomic<bool> HeldKnown{false};
  /// GEN-14, UNI-2: the bounds of the unit types the fields and methods of
  /// this generic declaration reach, through the generic types they hold
  /// (Checker::unitRangeSum), once the types are final.
  std::atomic<const UnitRangeSum *> URange{nullptr};
  /// Checker::gcLayout: the gc size and alignment of the type (Sizes.cpp).
  /// LayoutState 0 not known, 1 known; the two numbers are written before it.
  /// Stored only when exact (no by-value cycle was cut below the type and
  /// every type below it is declared): the answer is a function of the type
  /// alone, so every thread that stores it stores the same value.
  std::atomic<int64_t> LayoutSize{0};
  std::atomic<int64_t> LayoutAlign{0};
  std::atomic<uint8_t> LayoutState{0};
  /// Generic declaration: does it hold an instance of itself by value, so
  /// its instances grow without end (varParams)? 0 not known, 1 no, 2 yes.
  std::atomic<uint8_t> GrowState{0};
  /// Instance: Underlying is set. Written once, with release order, after
  /// Underlying: another body thread may read it (P03.10).
  std::atomic<bool> Expanded{false};
  bool Expanding = false; // under the context's lock: a re-entrant expansion
  /// Declared methods (of the origin), in declaration order.
  llvm::SmallVector<Func *, 0> *Methods = nullptr;
  /// Instance: methods instantiated so far, parallel to Orig->Methods.
  llvm::SmallVector<Func *, 0> *InstMethods = nullptr;
  NamedType *origin() { return Orig ? Orig : this; }
  const NamedType *origin() const { return Orig ? Orig : this; }
  bool isGeneric() const { return !Orig && !TParams.empty(); }
};

//===----------------------------------------------------------------------===//
// Objects
//===----------------------------------------------------------------------===//

class Object {
public:
  enum class Kind : uint8_t {
    PkgName,
    Const,
    TypeName,
    Var,
    Func,
    Label,
    Builtin,
    Nil,
    Unit
  };
  Kind getKind() const { return K; }
  llvm::StringRef Name;
  Package *Pkg = nullptr;
  Type *Ty = nullptr;
  SourceLoc Pos;
  Scope *Parent = nullptr;
  /// Scope position after which the object is visible (function-local).
  SourceLoc ScopePos;
  uint32_t Order = 0; // package-level declaration order
  uint8_t Color = 0;  // white=0, black=1 (resolved), >=2 grey (in progress)
  /// A C++ class template, or a function name with a template among its
  /// candidates (a foreign object only): `pkg.vec[int32]`, `pkg.f[T](x)`
  /// (ADR-0010 Q3).
  bool IsTemplate = false;
  bool exported() const;

protected:
  explicit Object(Kind K) : K(K) {}

private:
  Kind K;
};

class PkgName : public Object {
public:
  PkgName() : Object(Kind::PkgName) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::PkgName; }
  Package *Imported = nullptr;
  bool Used = false;
  bool Foreign = false; // imported from a C/C++ header (P03.09)
  SourceLoc ImportLoc;
};

class Const : public Object {
public:
  Const() : Object(Kind::Const) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::Const; }
  const ConstValue *Val = nullptr;
};

class TypeName : public Object {
public:
  TypeName() : Object(Kind::TypeName) {}
  static bool classof(const Object *O) {
    return O->getKind() == Kind::TypeName;
  }
  bool IsAlias = false;
  /// `type A[P any] = T`: the parameters of a generic alias (Ty is T).
  llvm::ArrayRef<TypeParamType *> AliasTParams;
};

class Var : public Object {
public:
  Var() : Object(Kind::Var) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::Var; }
  bool IsField = false;
  bool Embedded = false;
  /// The hidden first field of an `extends` type (TYP-16, ADR-0023): the
  /// parent part, named like the parent type. It is an embedded field, so
  /// the lookup finds the parent's fields and methods through it, and it is
  /// the first field, so the parent part comes first in the layout (TYP-22).
  /// A user-written embedded field is an error in a `.gopp` file (TYP-25).
  bool IsParent = false;
  /// The hidden `base` variable of a method of an `extends` type: the
  /// borrowed pointer to the parent part (TYP-20).
  bool IsBase = false;
  bool IsParam = false;
  /// The receiver of a method. A value receiver of a type with a type table
  /// may be the parent part of a child (EXP-15, ADR-0023 decision 16).
  bool IsRecv = false;
  /// The effective required bit of a struct field: marked `@Required`
  /// (TYP-15), or required by its `@Strict` struct (TYP-15a). Every rule
  /// reads this bit; it is decided in one place, effectiveRequired
  /// (Typexpr.cpp).
  bool Required = false;
  /// The field of an instance whose declared type is a type parameter
  /// (`Val T @Required`): its @Required only means "must be set", never "never
  /// nil", since the generic code may store a nil T (ADR-0016 point 8).
  bool RequiredSetOnly = false;
  /// The field attributes as written (ADR-0019): `@Required` and `@Option`.
  /// Only messages and the readers of the attributes (P09, P14) use them.
  bool MarkedRequired = false;
  bool MarkedOption = false;
  bool Used = false;
  bool IsPackageLevel = false;
  /// Ownership (P03.07): how the variable relates to its value.
  enum Borrow : uint8_t {
    Owned,    // an ordinary owner
    Borrowed, // OWN-R3b: read-only borrowed variable
    Alias,    // the `&d` range alias (RNG-1a)
    MoveParam // `move` parameter: an ordinary owned local
  } Mode = Owned;
  bool LoopVar = false;       // declared by a range/for clause (RNG-8)
  bool OptionBinding = false; // `v, ok := o.Get()` borrows from the Option
  bool Result = false;
  llvm::StringRef Tag; // struct field tag
  Var *Origin = nullptr;
  uint32_t ScopeDepth = 0;
  const void *FuncOwner = nullptr; // function whose local this is
};

enum class BuiltinId : uint8_t {
  Append,
  Cap,
  Clear,
  Close,
  Complex,
  Copy,
  Delete,
  Imag,
  Len,
  Make,
  Max,
  Min,
  New,
  Panic,
  Print,
  Println,
  Real,
  Recover,
  // Go conformance mode only: the checker test helpers of types2.
  Assert,
  Trace,
  // Go++ (EXP-1)
  Move,
  Clone,
  NewUnique,
  NewShared,
  NewWeak,
  // unsafe
  Alignof,
  Offsetof,
  Sizeof,
  Add,
  Slice,
  SliceData,
  String,
  StringData,
  // ADR-0025: `x.In[U]()` on a unit value (a member, not a function).
  UnitIn,
};

class Builtin : public Object {
public:
  Builtin() : Object(Kind::Builtin) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::Builtin; }
  BuiltinId Id = BuiltinId::Len;
};

class Nil : public Object {
public:
  Nil() : Object(Kind::Nil) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::Nil; }
};

/// A declared unit (UNI-1): a base unit (its own dimension) or a derived one.
/// It lives in the package block with types, functions and variables, and is
/// valid only inside the unit grammar.
class UnitObj : public Object {
public:
  UnitObj() : Object(Kind::Unit) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::Unit; }
  /// No `=`: the unit is a dimension of its own.
  bool IsBase = false;
  /// Set when the declaration is resolved; null for a unit that is invalid
  /// (a cycle, a bad factor): every use of it is silently invalid.
  const UnitCanon *Canon = nullptr;
  /// The text that `fmt` prints for the unit (`@Symbol`, UNI-13); empty: the
  /// name.
  llvm::StringRef Symbol;
  bool Resolved = false;
  /// The declaration was in error; uses of the unit are not reported again.
  bool Bad = false;
};

class Label : public Object {
public:
  Label() : Object(Kind::Label) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::Label; }
  bool Used = false;
};

/// The operator an operator method stands for (ADR-0024, EXP-16): its name
/// and its parameter count decide it (`-` with no parameter is Neg, with one
/// it is Sub). Convert is the `@Convert` attribute on an ordinary method; Bad
/// is a name the parser rejected (`=`, `.`, `,`, `!=`, unary `&`, ...).
enum class OpKind : uint8_t {
  None,
  Add,
  Sub,
  Mul,
  Quo,
  Rem,
  And,
  Or,
  Xor,
  Shl,
  Shr,
  AndNot,
  Neg,
  Pos,
  Compl,
  Not,
  Eq,
  Cmp,
  LAnd,
  LOr,
  Index,
  IndexSet,
  Call,
  Send,
  Recv,
  Inc,
  Dec,
  Deref,
  Range,
  AddAssign,
  SubAssign,
  MulAssign,
  QuoAssign,
  RemAssign,
  AndAssign,
  OrAssign,
  XorAssign,
  ShlAssign,
  ShrAssign,
  AndNotAssign,
  Convert,
  Bad,
};
static_assert(static_cast<unsigned>(OpKind::Bad) < OpTable::MaxKinds,
              "OpTable::Merged has a slot per OpKind");
/// The operator of the method name N with NumParams parameters, or None if N
/// is an ordinary name. A name that is an operator but has no form with that
/// many parameters gets the kind of its binary form (or its only form), so
/// the declaration check can say which row of the forms table it breaks.
OpKind opKindOf(llvm::StringRef N, unsigned NumParams);
/// The spelling of an operator kind: `+` for Add and Pos, `+=` for
/// AddAssign, `range` for Range.
const char *opSpelling(OpKind K);
bool isBinaryArith(OpKind K); // Add .. AndNot
bool isCompoundOp(OpKind K);  // AddAssign .. AndNotAssign
OpKind compoundToBinary(OpKind K);
OpKind binaryToCompound(OpKind K);
/// `+ * & | ^ ==`: one @Right method serves both orders (EXP-26).
bool isCommutativeOp(OpKind K);

class Func : public Object {
public:
  Func() : Object(Kind::Func) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::Func; }
  const ast::FuncDecl *Decl = nullptr;
  /// Parameter and result objects, in order (receiver separate).
  llvm::ArrayRef<Var *> Params;
  llvm::ArrayRef<Var *> Results;
  Var *Recv = nullptr;
  bool HasPtrRecv = false;
  /// The method mark of TYP-18: none (the method is final), `@Virtual`,
  /// `@Abstract` or `@Override`. An instantiated method has its origin's.
  enum class Mark : uint8_t { None, Virtual, Abstract, Override };
  Mark MethodMark = Mark::None;
  bool MoveRecv = false; // a `move` receiver (PAR-5)
  /// An operator method (ADR-0024, TYP-27): its Name is the operator (`+`,
  /// `[]=`, `range`) and Op the form that the name and the parameter count
  /// give; OpKind::Convert marks a `@Convert` method, OpKind::None an
  /// ordinary one. Set from the declaration, so an instantiated method and
  /// an imported one have it too. Operator methods are in the method list of
  /// their named type like every method (an overload set shares the name);
  /// an operator element of an interface is one of its methods.
  OpKind Op = OpKind::None;
  /// `@Right` (EXP-26): the method also serves `s op a`.
  bool Right = false;
  bool isOperator() const {
    return Op != OpKind::None && Op != OpKind::Convert;
  }
  bool IsInterfaceMethod = false;
  bool Fails = false;
  bool IsBuiltinMethod = false;  // x.Clone() / p.IsNil() of a pointer kind
  Func *Origin = nullptr;        // for an instantiated method
  NamedType *RecvInst = nullptr; // its instance, for an instantiated method
  /// Type parameters of the receiver base type (methods of generic types) and
  /// own type parameters (generic methods, D1.10).
  llvm::ArrayRef<TypeParamType *> RecvTParams;
  llvm::ArrayRef<TypeParamType *> OwnTParams;
  SignatureType *sig() const { return llvm::cast<SignatureType>(Ty); }
};

//===----------------------------------------------------------------------===//
// Scopes and packages
//===----------------------------------------------------------------------===//

class Scope {
public:
  Scope(Scope *Parent, SourceRange Extent, const char *Comment)
      : Parent(Parent), Extent(Extent), Comment(Comment) {}
  Scope *Parent;
  SourceRange Extent;
  const char *Comment; // "universe", "package", "file", "func", "block", ...
  bool IsFunc = false;
  llvm::SmallDenseMap<llvm::StringRef, Object *, 4> Elems;

  Object *lookupLocal(llvm::StringRef Name) const {
    auto It = Elems.find(Name);
    return It == Elems.end() ? nullptr : It->second;
  }
  /// Looks Name up in this scope and its parents; objects declared at or after
  /// Pos in a function-local scope are not visible yet (if Pos is valid).
  Object *lookup(llvm::StringRef Name, SourceLoc Pos = {},
                 Scope **Found = nullptr) const;
  /// Inserts Obj; returns the existing object with the same name if any.
  Object *insert(Object *Obj);
  bool isPackageScope() const;
};

/// The foreign (C/C++ header) hook point of D3.7: see gopp/Sema/Foreign.h.
class ForeignScope;

class Package {
public:
  llvm::StringRef Path;
  llvm::StringRef Name;
  Scope *PkgScope = nullptr;
  bool Fake = false;
  bool Complete = false;
  /// A package made from a C/C++ header: exported-name rule does not apply.
  bool IsForeign = false;
  ForeignScope *Foreign = nullptr;
  /// The display form that this package prints for a unit (UNI-5), by
  /// canonical form: the first in the fixed order among the forms of its
  /// declarations and of its imports (export data carries the table, so an
  /// importer prints the same names). Set once, before the bodies are
  /// checked; sorted by Canon for lookup (unitName).
  llvm::ArrayRef<UnitNameEntry> UnitNames;
  const UnitDisplay *unitName(const UnitCanon *C) const;
};

//===----------------------------------------------------------------------===//
// TypeContext
//===----------------------------------------------------------------------===//

class TypeContext {
public:
  /// GoCompat: the universe has Go's `new` and none of the Go++ built-ins
  /// (`move`, `clone`, `new_unique`, ...), for the Go conformance suite.
  explicit TypeContext(bool GoCompat = false);
  TypeContext(const TypeContext &) = delete;
  TypeContext &operator=(const TypeContext &) = delete;

  /// Predeclared types.
  BasicType *typ(BasicKind K) { return &Basics[static_cast<unsigned>(K)]; }
  BasicType *invalid() { return typ(BasicKind::Invalid); }
  Type *errorType() { return ErrorType; }
  Type *anyType() { return AnyType; }
  Type *comparableType() { return ComparableType; }
  Type *copyType() { return CopyType; }
  Type *cloneType() { return CloneType; }
  Type *byteType() { return ByteType; }
  Type *runeType() { return RuneType; }
  Package *unsafePackage() { return Unsafe; }
  Scope *universe() { return Universe; }

  /// The unit with these dimensions and factor (UNI-5). Dims must be sorted
  /// by UnitDim order, merged and without zero exponents (Units.cpp does
  /// that); Factor must be an exact rational.
  const UnitCanon *unitCanon(llvm::ArrayRef<UnitDim> Dims, const Real &Factor);
  /// The display form with these parts (sorted like UnitCanon::Dims, merged,
  /// no zero exponent: units::display makes them so); null for no parts.
  const UnitDisplay *unitDisplay(llvm::ArrayRef<UnitDim> Parts);
  /// The basic type of kind BK with unit U: BK itself if U is null.
  BasicType *unitType(BasicKind BK, const UnitCanon *U);
  /// The type argument that stands for the unit U: a unit parameter alone is
  /// the parameter itself, any other unit is a UnitArg type.
  Type *unitArgType(const UnitCanon *U);
  /// Set once a unit exists (a unit declaration, a unit read from export
  /// data, a unit type) or a type parameter has the constraint `unit`: in
  /// phase 1, before anything is decided about the program that has it.
  /// Until then no operand has a unit and no type argument is one, so the
  /// checks of EXP-31 to EXP-37 and GEN-14 are skipped and a program without
  /// units pays nothing. Only ever set.
  bool unitsSeen() const { return UnitsSeen.load(std::memory_order_relaxed); }
  void noteUnits() { UnitsSeen.store(true, std::memory_order_relaxed); }

  PointerType *pointer(Type *Elem);
  SliceType *slice(Type *Elem);
  RefType *ref(Type *Elem, bool Borrowed = false);
  OptionType *option(Type *Elem);
  ArrayType *array(Type *Elem, int64_t Len);
  MapType *map(Type *Key, Type *Elem);
  ChanType *chan(ChanType::Dir D, Type *Elem);
  TupleType *tuple(llvm::ArrayRef<Type *> Elems);
  SignatureType *signature(TupleType *Params, TupleType *Results, bool Variadic,
                           bool Fails, uint64_t MoveMask,
                           llvm::ArrayRef<TypeParamType *> TParams = {});
  /// Fields are copied; a new Var is made for each field of a new struct.
  struct FieldSpec {
    llvm::StringRef Name;
    Package *Pkg;
    Type *Ty;
    bool Embedded;
    llvm::StringRef Tag;
    bool Required = false;
    bool RequiredSetOnly = false;
    bool MarkedRequired = false;
    bool MarkedOption = false;
    bool IsParent = false;
    /// No position: a struct is uniqued, so a Var in it cannot belong to one
    /// body (see the rule at TypeContext::unique in Types.cpp).
  };
  /// Strict: the struct type of a `@Strict` declaration (StructType::Strict).
  StructType *structOf(llvm::ArrayRef<FieldSpec> Fields, bool Strict = false);
  /// Not uniqued: interfaces and unions are compared by their type sets.
  InterfaceType *newInterface(llvm::ArrayRef<Func *> Methods,
                              llvm::ArrayRef<Type *> Embeddeds,
                              llvm::ArrayRef<SourceLoc> EmbedLocs,
                              bool Implicit = false);
  UnionType *newUnion(llvm::ArrayRef<Term> Terms);
  NamedType *newNamed(TypeName *Obj, Type *Underlying = nullptr);
  TypeParamType *newTypeParam(TypeName *Obj, unsigned Index, Type *Bound);
  /// The shared stand-in for the Ordinal-th distinct type parameter, with
  /// constraint Bound, of a type asked about by a type-level query
  /// (Checker::canonicalParams): two type parameters with the same bound
  /// answer every such query alike, so the stand-ins let the bodies of many
  /// generic functions share the cached answers of one instance.
  TypeParamType *canonicalParam(Type *Bound, unsigned Ordinal);
  /// The instance Orig[TArgs]; created (unexpanded) on first use.
  NamedType *instance(NamedType *Orig, llvm::ArrayRef<Type *> TArgs);

  llvm::ArrayRef<Type *> copyTypes(llvm::ArrayRef<Type *> Ts);
  /// A method list owned by the context (see NamedType::Methods).
  llvm::SmallVector<Func *, 0> *newMethodList();
  /// An operator table owned by the context (see OpTable), and the shared
  /// empty table of a type with no operator method up its chain.
  const OpTable *newOpTable(llvm::ArrayRef<Func *> Own, const OpTable *Next);
  /// A list kept for an OpTable::Merged slot.
  const llvm::ArrayRef<Func *> *newOpList(llvm::ArrayRef<Func *> L);
  static const OpTable *emptyOps();
  /// Set once any package (declared or imported) has an operator method, a
  /// @Convert method or an operator element (ADR-0024). Until then no
  /// operator set is searched, so a program without operators pays nothing.
  /// Only ever set, before the bodies of the packages that can see one.
  void noteOperators() { OpsSeen.store(true, std::memory_order_relaxed); }
  bool operatorsSeen() const { return OpsSeen.load(std::memory_order_relaxed); }
  /// Objects the context itself makes (universe, struct fields).
  Arena &arena() { return Objs; }
  /// Guards the lazy completion of instances and type sets (expandNamed,
  /// computeTypeSet, instantiatedMethod). Creating a type needs no lock from
  /// the caller: the tables and the allocator have their own (leaf) locks.
  std::recursive_mutex &lock() { return Mu; }
  /// The lock of the allocator behind arena(): held by the code that makes
  /// objects in it from several threads.
  std::mutex &allocLock() { return AllocMu; }

  /// A per-type answer about the parent chain of an `extends` type, stored
  /// once for every type of the chain, so a chain of n types with a question
  /// on each costs n steps, not n squared (Checker::requiredAncestor). The
  /// answer is a function of the final types alone, so every thread stores
  /// the same one.
  struct MemberMemo {
    NamedType *Owner = nullptr; // the type of the chain that answers, or null
  };
  bool memberMemoGet(const NamedType *N, llvm::StringRef Key, MemberMemo &Out);
  void memberMemoPut(const NamedType *N, llvm::StringRef Key,
                     const MemberMemo &M);
  /// Every field and method name that any struct, interface or method list
  /// has (the names only, not whose they are): a name that was never noted
  /// is a member of no type, so a lookup on a chain ends at once. Noted when
  /// the member is made, before any lookup on a type that has it.
  void noteMemberName(llvm::StringRef Name);
  /// Publishes the uses of TP (TypeParamType::uses): sorted and without
  /// duplicates, copied into the context with their names.
  void setTParamUses(TypeParamType *TP,
                     llvm::ArrayRef<TypeParamType::Use> Uses);
  bool memberNameSeen(llvm::StringRef Name);
  /// The same for the case-folding lookup (the "did you mean" of an error):
  /// false only when no member name equals Name up to case.
  bool memberNameSeenFold(llvm::StringRef Name);

  /// The name index of the `extends` chains, on their origins
  /// (Checker::chainIndex, chainLookup, R19): for each root, the depths at
  /// which an origin of its tree declares a name, and for each origin its own
  /// members by name (methods first), its parent and a skew-binary jump
  /// pointer with their type arguments. A lookup goes straight to the
  /// ancestor at such a depth, so n lookups of n different names on a chain
  /// of n types cost n log n, not n squared. Each origin is entered once,
  /// after the types are final, with its ancestors; what is stored depends
  /// on the declarations alone, so no answer depends on the thread.
  struct ChainEntry {
    NamedType *Root = nullptr;
    uint32_t Depth = 0; // 1 for the root
    /// The types up to the root are all structs whose only embedded field is
    /// the parent part (else the general walk decides).
    bool Plain = false;
    NamedType *Parent = nullptr; // the parent's origin
    NamedType *Jump = nullptr;   // an ancestor origin (the root: itself)
    /// The type arguments of Parent and of Jump in terms of this origin's
    /// own type parameters.
    llvm::ArrayRef<Type *> ParentArgs, JumpArgs;
  };
  struct ChainMember {
    Object *Obj;
    int Idx; // its index among the type's methods or fields
  };
  bool chainEntry(const NamedType *N, ChainEntry &Out);
  /// Enters N (if it is not yet) with its members, Members[I] named
  /// Members[I].Obj->Name.
  void chainAdd(const NamedType *N, const ChainEntry &E,
                llvm::ArrayRef<ChainMember> Members);
  /// The deepest depth, at most Max, at which an origin under Root declares
  /// Name, or 0 (a binary search: an override at every level of a chain
  /// costs nothing more).
  uint32_t chainDepthAtMost(const NamedType *Root, llvm::StringRef Name,
                            uint32_t Max);
  void chainMembers(const NamedType *N, llvm::StringRef Name,
                    llvm::SmallVectorImpl<ChainMember> &Out);

private:
  void initUniverse(bool GoCompat);
  std::shared_mutex NamesMu;
  llvm::StringSet<> MemberNames; // also "\x04" + the ASCII lowercase
  bool NonAsciiNames = false;
  std::mutex MemoMu;
  llvm::DenseMap<const NamedType *, llvm::StringMap<MemberMemo>> MemberMemos;
  std::deque<llvm::SmallVector<Func *, 0>> MethodLists;
  std::deque<OpTable> OpTables;
  std::deque<llvm::ArrayRef<Func *>> OpLists;
  std::atomic<bool> OpsSeen{false};
  std::shared_mutex ChainMu;
  llvm::DenseMap<const NamedType *, ChainEntry> ChainTypes;
  using ChainKey = std::pair<const NamedType *, llvm::StringRef>;
  llvm::DenseMap<ChainKey, llvm::SmallVector<uint32_t, 1>> ChainDepthsOf;
  llvm::DenseMap<ChainKey, llvm::SmallVector<ChainMember, 1>> ChainMembersOf;

  llvm::ArrayRef<Type *> copyTypesLocked(llvm::ArrayRef<Type *> Ts);

  /// One part of a uniquing table: the types whose profile hash falls in it,
  /// the lock that guards them and the memory they are made in. Every table
  /// has NumShards shards, so threads that make or look up different types
  /// rarely take the same lock (P03.10).
  template <class T> struct alignas(64) Shard {
    std::mutex Mu;
    llvm::FoldingSet<T> Set;
    llvm::BumpPtrAllocator Alloc;
  };
  static constexpr unsigned NumShards = 16;
  template <class T> using Table = Shard<T>[NumShards];
  /// The node of Tab equal to ID, made with Make(allocator of its shard) if
  /// there is none (Types.cpp).
  template <class T, class MakeFn>
  T *unique(Table<T> &Tab, const llvm::FoldingSetNodeID &ID, MakeFn Make);

  llvm::BumpPtrAllocator Alloc;
  std::recursive_mutex Mu;
  /// Lock order: Mu, then a shard's lock, then AllocMu.
  std::mutex AllocMu; // Alloc, Objs, MethodLists
  Arena Objs;
  BasicType Basics[static_cast<unsigned>(BasicKind::NumBasicKinds)];
  Table<PointerType> Pointers;
  Table<SliceType> Slices;
  Table<RefType> Refs;
  Table<OptionType> Options;
  Table<ArrayType> Arrays;
  Table<MapType> Maps;
  Table<ChanType> Chans;
  Table<TupleType> Tuples;
  Table<SignatureType> Signatures;
  Table<StructType> Structs;
  Table<NamedType> Instances;
  std::atomic<bool> UnitsSeen{false};
  std::mutex UnitMu; // UnitCanons, UnitTypes, UnitDisplays; before AllocMu
  std::deque<UnitCanon> UnitStore;
  llvm::StringMap<UnitCanon *> UnitCanons;
  std::deque<UnitDisplay> UnitDisplayStore;
  llvm::StringMap<UnitDisplay *> UnitDisplays;
  llvm::DenseMap<std::pair<unsigned, const UnitCanon *>, BasicType *> UnitTypes;
  std::mutex CanonMu; // CanonParams; taken before AllocMu
  llvm::DenseMap<std::pair<Type *, unsigned>, TypeParamType *> CanonParams;
  Type *ErrorType = nullptr, *AnyType = nullptr, *ComparableType = nullptr,
       *CopyType = nullptr, *CloneType = nullptr, *ByteType = nullptr,
       *RuneType = nullptr;
  Package *Unsafe = nullptr;
  Scope *Universe = nullptr;
};

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

inline bool isExportedName(llvm::StringRef N) {
  if (N.empty())
    return false;
  unsigned char C = N[0];
  if (C < 0x80)
    return C >= 'A' && C <= 'Z';
  // Non-ASCII: decide by the first code point (upper-case letters of the
  // Latin-1 and Greek/Cyrillic blocks cover what the tests use).
  if (C == 0xC3 && N.size() > 1) {
    unsigned char D = N[1];
    return D >= 0x80 && D <= 0x9E && D != 0x97;
  }
  if (C == 0xCE && N.size() > 1) {
    unsigned char D = N[1];
    return D >= 0x91 && D <= 0xAB;
  }
  if (C == 0xD0 && N.size() > 1) {
    unsigned char D = N[1];
    return D >= 0x80 && D <= 0xAF && !(D >= 0xB0);
  }
  return false;
}

inline bool Object::exported() const { return isExportedName(Name); }

/// The underlying type of an already resolved type (a type parameter's is its
/// constraint interface). Instances must be expanded first: use
/// Checker::under.
Type *under(Type *T);

/// TYP-10: the fields of a struct type with the parent chain flattened: the
/// root ancestor's own fields first, then each child's, then the type's own.
/// The hidden parent fields themselves are not listed. Iterative, so a chain
/// of any depth is fine; the type and its ancestors must be resolved.
void fieldTable(Type *T, llvm::SmallVectorImpl<const Var *> &Out);

/// Go-style text of a type for messages; types of package Qual are printed
/// without a package name.
/// UN, if given, names a unit by a declared unit of the package or its imports
/// (units::Namer); without it a unit prints in its canonical form.
std::string typeString(const Type *T, const Package *Qual = nullptr,
                       const units::Namer *UN = nullptr);

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_TYPES_H
