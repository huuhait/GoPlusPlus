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
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>

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

class BasicType : public Type {
public:
  BasicType(BasicKind BK, unsigned Info, const char *Name)
      : Type(Kind::Basic), BK(BK), Info(static_cast<uint16_t>(Info)),
        Name(Name) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Basic; }
  BasicKind BK;
  uint16_t Info;
  const char *Name;
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

/// `ref string` / `ref []T` (D1.6). Elem is `string` or a SliceType.
class RefType : public Type, public llvm::FoldingSetNode {
public:
  explicit RefType(Type *Elem) : Type(Kind::Ref), Elem(Elem) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Ref; }
  void Profile(llvm::FoldingSetNodeID &ID) const { ID.AddPointer(Elem); }
  Type *Elem;
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
  explicit StructType(llvm::ArrayRef<Var *> Fields)
      : Type(Kind::Struct), Fields(Fields) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Struct; }
  void Profile(llvm::FoldingSetNodeID &ID) const;
  llvm::ArrayRef<Var *> Fields;
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
  bool isEmpty() const { return !AllTerms && Terms.empty(); }
  bool isAll() const {
    return AllTerms && !Comparable && !Copy && !Clone && Methods.empty();
  }
  bool hasTerms() const { return !AllTerms; }
};

class InterfaceType : public Type {
public:
  enum Special : uint8_t { None, Comparable, Copy, Clone };
  InterfaceType() : Type(Kind::Interface) {}
  static bool classof(const Type *T) { return T->getKind() == Kind::Interface; }
  llvm::ArrayRef<Func *> Methods;   // explicitly declared methods
  llvm::ArrayRef<Type *> Embeddeds; // embedded types and unions
  llvm::ArrayRef<SourceLoc> EmbedLocs;
  Special Sp = None;
  bool Implicit = false; // `[T int|string]` wrapped into an interface
  bool IsAny = false;    // the predeclared `any` (printed as such)
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
};

/// Predeclared generic pointer types of D1.7.
enum class PtrKind : uint8_t { None, Unique, Shared, Weak };

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
  bool Pinned = false; // @Pinned (OWN-R10)
  /// The predeclared generic `Option` (instances are OptionType, not Named).
  bool IsOption = false;
  /// "Has a zero value" (TYP-15, OWN-R9): 0 not known, 1 yes, 2 no. Stored
  /// only when exact (ZeroQuery in Ownership.cpp): the answer is a function
  /// of the type alone, so every thread that stores it stores the same value.
  std::atomic<uint8_t> ZeroState{0};
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
    Nil
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
  bool IsParam = false;
  bool Required = false; // a struct field marked `@Required` (TYP-15)
  /// The field of an instance whose declared type is a type parameter
  /// (`Val T @Required`): its @Required only means "must be set", never "never
  /// nil", since the generic code may store a nil T (ADR-0016 point 8).
  bool RequiredSetOnly = false;
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

class Label : public Object {
public:
  Label() : Object(Kind::Label) {}
  static bool classof(const Object *O) { return O->getKind() == Kind::Label; }
  bool Used = false;
};

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
  bool MoveRecv = false; // a `move` receiver (PAR-5)
  bool IsInterfaceMethod = false;
  bool Fails = false;
  bool IsBuiltinMethod = false; // x.Clone() / p.IsNil() of a pointer kind
  Func *Origin = nullptr;       // for an instantiated method
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

  PointerType *pointer(Type *Elem);
  SliceType *slice(Type *Elem);
  RefType *ref(Type *Elem);
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
    /// No position: a struct is uniqued, so a Var in it cannot belong to one
    /// body (see the rule at TypeContext::unique in Types.cpp).
  };
  StructType *structOf(llvm::ArrayRef<FieldSpec> Fields);
  /// Not uniqued: interfaces and unions are compared by their type sets.
  InterfaceType *newInterface(llvm::ArrayRef<Func *> Methods,
                              llvm::ArrayRef<Type *> Embeddeds,
                              llvm::ArrayRef<SourceLoc> EmbedLocs,
                              bool Implicit = false);
  UnionType *newUnion(llvm::ArrayRef<Term> Terms);
  NamedType *newNamed(TypeName *Obj, Type *Underlying = nullptr);
  TypeParamType *newTypeParam(TypeName *Obj, unsigned Index, Type *Bound);
  /// The instance Orig[TArgs]; created (unexpanded) on first use.
  NamedType *instance(NamedType *Orig, llvm::ArrayRef<Type *> TArgs);

  llvm::ArrayRef<Type *> copyTypes(llvm::ArrayRef<Type *> Ts);
  /// A method list owned by the context (see NamedType::Methods).
  llvm::SmallVector<Func *, 0> *newMethodList();
  /// Objects the context itself makes (universe, struct fields).
  Arena &arena() { return Objs; }
  /// Guards the lazy completion of instances and type sets (expandNamed,
  /// computeTypeSet, instantiatedMethod). Creating a type needs no lock from
  /// the caller: the tables and the allocator have their own (leaf) locks.
  std::recursive_mutex &lock() { return Mu; }
  /// The lock of the allocator behind arena(): held by the code that makes
  /// objects in it from several threads.
  std::mutex &allocLock() { return AllocMu; }

private:
  void initUniverse(bool GoCompat);
  std::deque<llvm::SmallVector<Func *, 0>> MethodLists;

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

/// Go-style text of a type for messages; types of package Qual are printed
/// without a package name.
std::string typeString(const Type *T, const Package *Qual = nullptr);

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_TYPES_H
