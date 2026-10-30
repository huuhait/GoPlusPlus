//===- gopp/Mangle/SymbolName.h - Symbol names (D4.13) ----------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The names of the symbols the code generator emits, and their inverse. The
// format is specified in docs/spec/appendix-a-symbol-names.md; this header is
// its model.
//
//   - An entity (a function, a method, a closure, a type table, ...) is a
//     small tree of Nodes built in a Context. Nodes are interned: two nodes
//     with the same content are one pointer, so a type that is used a million
//     times is one node (a DAG), and nothing here depends on creation order.
//   - mangle() turns an entity into `_GP...` text. It is a function of the
//     content of the tree alone: the same entity gives the same bytes in
//     every package, build, thread and run.
//   - parseSymbol() reads such text back into a tree, and demangle() prints
//     it as readable text. Both reject malformed input with an error (never a
//     crash) and accept exactly the text mangle() writes, so
//     mangle(parseSymbol(S)) == S for every S that is accepted.
//
// Every walk is iterative (explicit stacks): a name may be nested a million
// levels deep, and a repeated subtree is written once and then referred to
// (a back reference), so a type that doubles at every level stays linear.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_MANGLE_SYMBOLNAME_H
#define GOPP_MANGLE_SYMBOLNAME_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"

#include <cstdint>
#include <string>

namespace gopp {
namespace sym {

/// Every kind of node. The order is not part of the format (the text is):
/// see kindPrefix().
enum class Kind : uint8_t {
  // Types: predeclared leaves.
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
  // Types: constructors.
  Pointer,
  Slice,
  Array,      ///< Num = length
  Map,        ///< key, element
  Option,     ///< Option[T]
  View,       ///< View[T]
  Borrowed,   ///< the borrowed slice of OWN-V2
  Chan,       ///< Sub = 'b' both, 's' send, 'r' receive
  Tuple,      ///< any number of types
  Signature,  ///< Num = flags (SigVariadic), Aux = move mask;
              ///< kids: parameter Tuple, result Tuple
  Struct,     ///< Num = 1 for `@Strict`; kids: Field...
  Field,      ///< Num = FieldFlags; A = package path (if FieldPkg), B = name,
              ///< C = tag (if FieldTag); kid: the type
  Interface,  ///< kids: IMethod... sorted by (package, name)
  IMethod,    ///< A = package path (empty: exported), B = name; kid: Signature
  Named,      ///< A = package path, B = name
  LocalNamed, ///< Num = ordinal in the scope, B = name; kid: the entity
              ///< (function, method, ...) that declares the type
  Universe,   ///< B = name of a predeclared type (`error`, `Unique`, ...)
  Instance,   ///< kids: the generic thing, then its type arguments
  TypeParam,  ///< Num = index; only for debugging names of generic code
  Unit,       ///< canonical unit: A = numerator (maybe `-`), B = denominator
              ///< (decimal, exact), kids: Dim / ParamDim sorted
  UnitType,   ///< Sub = letter of the numeric kind; kid: Unit (`float64[KM]`)
  Dim,        ///< A = package path, B = unit name, Aux = exponent (signed)
  ParamDim,   ///< Num = index of the unit type parameter, Aux = exponent
  BackRef,    ///< only in text; the parser resolves it to its target
  // Entities.
  Func,     ///< A = package path, B = name
  Method,   ///< B = name; kid: the receiver's named type (maybe an Instance)
  OpMethod, ///< an operator method (ADR-0024): Num = 1 for `@Right`; kids:
            ///< receiver, parameter Tuple (the overload set is told apart by
            ///< its parameters and `@Right`)
  Var,      ///< A = package path, B = name
  Closure,  ///< Num = ordinal (1 up) in the enclosing function; kid: the
            ///< enclosing entity (function, method, closure, variable)
  PkgInit,  ///< A = package path: the package's init entry point
  InitFunc, ///< A = package path, Num = ordinal of an `init` function
  TypeInfo, ///< kid: type
  VTable,   ///< kid: type (ADR-0023 type table)
  ITab,     ///< kids: concrete type, interface type
  EqGlue,   ///< kid: type
  HashGlue,
  DropGlue,
  CloneGlue,
  Data,        ///< B = a stable key (a content digest) of constant data
  CABIThunk,   ///< kid: entity (D4.4)
  MethodValue, ///< bound method value `x.M`
  MethodExpr,  ///< method expression `T.M`
  Promoted,    ///< wrapper of a promoted method of an `extends` child
  IfaceThunk,  ///< adapter of a method for an interface method table
  NumKinds
};

/// Signature::Num flags. A fallible signature has none: `func!(P) R` is
/// identical to `func(P) (R, error)` (ERR-2), so it is written as that.
enum : uint64_t { SigVariadic = 1 };
/// Field::Num flags: every mark of a field that the checker's type identity
/// compares (TYP-15, TYP-15a).
enum : uint64_t {
  FieldEmbedded = 1,
  FieldPkg = 2, ///< unexported name: A is the package
  FieldTag = 4,
  FieldRequired = 8,        ///< the effective `@Required` bit
  FieldOption = 16,         ///< `@Option`
  FieldParent = 32,         ///< the hidden parent part of an `extends` type
  FieldMarkedRequired = 64, ///< `@Required` as written
  FieldSetOnly = 128 ///< required only as "must be set": an instance's field
                     ///< declared with a type parameter (ADR-0016 point 8)
};

enum class NodeClass : uint8_t { Type, Entity, Other };

/// An immutable, interned node. Fields are used as the Kind says.
class Node : public llvm::FoldingSetNode {
public:
  Kind K;
  char Sub = 0;
  uint64_t Num = 0;
  uint64_t Aux = 0;
  llvm::StringRef A, B, C;
  llvm::ArrayRef<const Node *> Kids;
  /// Unique per distinct content within the Context (creation order; never
  /// part of a name).
  uint32_t Id = 0;
  /// Length of the text with no back references (saturating); a subtree
  /// becomes a back reference when it repeats and this is >= BackRefMin.
  uint64_t Size = 0;

  NodeClass cls() const;
  bool isType() const { return cls() == NodeClass::Type; }
  bool isEntity() const { return cls() == NodeClass::Entity; }
  void Profile(llvm::FoldingSetNodeID &ID) const;
};

/// The least expanded size of a repeated type that is written as a back
/// reference.
constexpr uint64_t BackRefMin = 8;

/// The largest unit exponent in magnitude the demangler accepts: the
/// checker's `units::MaxExp` (compiler/lib/Sema/Units.h, UNI-6). The test
/// `SemaSymbols.UnitExponentLimitIsTheCheckers` (a `static_assert`) keeps the
/// two equal.
constexpr int64_t MaxUnitExp = int64_t(1) << 20;

/// Owns and interns nodes. Not thread safe: use one per thread (or lock).
/// Nodes of different Contexts must not be mixed.
class Context {
public:
  Context();
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  size_t numNodes() const { return NextId; }
  /// A copy of S that lives as long as the Context.
  llvm::StringRef save(llvm::StringRef S);

  // ---- types -------------------------------------------------------------
  const Node *basic(Kind K); ///< Bool .. UnsafePointer
  const Node *pointer(const Node *T) { return make(Kind::Pointer, {T}); }
  const Node *slice(const Node *T) { return make(Kind::Slice, {T}); }
  const Node *array(const Node *T, uint64_t Len);
  const Node *map(const Node *K, const Node *V) {
    return make(Kind::Map, {K, V});
  }
  const Node *option(const Node *T) { return make(Kind::Option, {T}); }
  const Node *view(const Node *T) { return make(Kind::View, {T}); }
  const Node *borrowed(const Node *T) { return make(Kind::Borrowed, {T}); }
  /// Dir: 'b' both, 's' send only, 'r' receive only.
  const Node *chan(char Dir, const Node *T);
  const Node *tuple(llvm::ArrayRef<const Node *> Elems) {
    return make(Kind::Tuple, Elems);
  }
  const Node *signature(const Node *Params, const Node *Results,
                        uint64_t Flags = 0, uint64_t MoveMask = 0);
  const Node *structOf(llvm::ArrayRef<const Node *> Fields,
                       bool Strict = false);
  /// Pkg: the package of an unexported name (else empty).
  const Node *field(llvm::StringRef Name, llvm::StringRef Pkg, const Node *T,
                    uint64_t Flags = 0, llvm::StringRef Tag = {});
  /// Methods are sorted here by (package, name).
  const Node *interfaceOf(llvm::ArrayRef<const Node *> Methods);
  const Node *imethod(llvm::StringRef Name, llvm::StringRef Pkg,
                      const Node *Sig);
  const Node *named(llvm::StringRef Pkg, llvm::StringRef Name);
  const Node *localNamed(const Node *Scope, uint64_t Ordinal,
                         llvm::StringRef Name);
  const Node *universe(llvm::StringRef Name);
  /// A generic function, method or type with its type arguments.
  const Node *instance(const Node *Base, llvm::ArrayRef<const Node *> Args);
  const Node *typeParam(uint64_t Index);
  /// Dims are sorted here (named units by package and name, then parameters
  /// by index). Factor = Num/Den in lowest terms, decimal; Num may start
  /// with '-'.
  const Node *unit(llvm::StringRef Num, llvm::StringRef Den,
                   llvm::ArrayRef<const Node *> Dims);
  const Node *dim(llvm::StringRef Pkg, llvm::StringRef Name, int64_t Exp);
  const Node *paramDim(uint64_t Index, int64_t Exp);
  /// Letter: the Kind of the base type (Int .. Complex128).
  const Node *unitType(Kind Base, const Node *Unit);

  // ---- entities ----------------------------------------------------------
  const Node *func(llvm::StringRef Pkg, llvm::StringRef Name);
  /// A method of the named type (or instance) Recv. A name that is an
  /// operator spelling (ADR-0024) must pass Params, the tuple of its
  /// parameter types, and Right for a `@Right` method (EXP-26): operator
  /// methods are overloaded by both.
  const Node *method(const Node *Recv, llvm::StringRef Name,
                     const Node *Params = nullptr, bool Right = false);
  const Node *var(llvm::StringRef Pkg, llvm::StringRef Name);
  const Node *closure(const Node *Parent, uint64_t Ordinal);
  const Node *pkgInit(llvm::StringRef Pkg);
  const Node *initFunc(llvm::StringRef Pkg, uint64_t Ordinal);
  /// TypeInfo, VTable, EqGlue, HashGlue, DropGlue or CloneGlue of T.
  const Node *ofType(Kind K, const Node *T);
  const Node *itab(const Node *Concrete, const Node *Iface);
  const Node *data(llvm::StringRef Key);
  /// CABIThunk, MethodValue, MethodExpr, Promoted or IfaceThunk of Entity.
  const Node *wrapper(Kind K, const Node *Entity);

  /// The general constructor (used by the parser and the builders).
  const Node *make(Kind K, llvm::ArrayRef<const Node *> Kids, char Sub = 0,
                   uint64_t Num = 0, uint64_t Aux = 0, llvm::StringRef A = {},
                   llvm::StringRef B = {}, llvm::StringRef C = {});

private:
  llvm::BumpPtrAllocator Alloc;
  llvm::FoldingSet<Node> Set;
  uint32_t NextId = 0;
  std::string Scratch;
};

/// The prefix of every Go++ symbol. C names that start with it are reserved
/// (see isReservedName); a leading underscore and a capital letter is in the
/// namespace C reserves for the implementation anyway.
constexpr llvm::StringLiteral Prefix = "_GP";

/// Does Name start with the reserved prefix? The checker rejects an
/// `@Export` name or a C-ABI name that does (they would collide).
inline bool isReservedName(llvm::StringRef Name) {
  return Name.starts_with(Prefix);
}

/// The tag a kind is written with (`P` for Pointer, `Ti` for TypeInfo, ...).
const char *kindPrefix(Kind K);
/// The name of the kind as the appendix spells it (`Pointer`, `TypeInfo`).
const char *kindName(Kind K);

/// The text of the entity: Prefix + the entity. Only characters of
/// [A-Za-z0-9_$] are written. Entity must be an entity node.
std::string mangle(const Node *Entity);
/// The text of a type, with no prefix (used by tests and debugging).
std::string mangleType(const Node *T);

struct ParseError {
  std::string Message;
  size_t Pos = 0;
};

/// Reads the text of a symbol. Null (and Err set) if Text is not exactly what
/// mangle() writes for some entity. Nodes are made in Ctx.
const Node *parseSymbol(Context &Ctx, llvm::StringRef Text, ParseError &Err);
const Node *parseType(Context &Ctx, llvm::StringRef Text, ParseError &Err);

/// Readable text of a node. The output is cut at MaxOut bytes (Ok is false
/// then: a repeated type may be astronomically long once written out).
struct Printed {
  std::string Text;
  bool Ok = true;
};
Printed print(const Node *N, size_t MaxOut = 1 << 20);

struct DemangleResult {
  bool Ok = false;
  std::string Text;  ///< the readable name, if Ok
  std::string Error; ///< else the reason
  size_t ErrorPos = 0;
};
/// parseSymbol + print.
DemangleResult demangle(llvm::StringRef Symbol, size_t MaxOut = 1 << 20);

/// The text of the encoded identifier alone (tests).
std::string encodeIdent(llvm::StringRef Name);

} // namespace sym
} // namespace gopp

#endif // GOPP_MANGLE_SYMBOLNAME_H
