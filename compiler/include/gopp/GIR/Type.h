//===- gopp/GIR/Type.h - GIR types ------------------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01, docs/design/gir.md section 3. GIR has its own types, not the
// checker's: they are fixed-width (`int` is already i64 here), have no names
// except for structs, and are cheap to serialize (D4.15) and to substitute in
// monomorphization (D4.3). A Context uniques them, so two types are the same
// type exactly when the pointers are equal (structs by name). The checker's
// types are turned into these in one place: gir::TypeLowering (Lower.cpp).
//
// The Context can be shared by threads that build different functions: the
// uniquing tables are behind one lock, and nothing a type prints or
// serializes as depends on the order the types were created in.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_TYPE_H
#define GOPP_GIR_TYPE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gopp {
namespace gir {

/// How a function takes its parameters and gives its result (D4.4). It is
/// part of a function type (P04.18): a call through a pointer uses the
/// convention of the pointer's type, and a function's own convention is its
/// signature's (V075), so a pointer to a Go++ function cannot reach a C
/// caller, nor a C function a Go++ caller, without a thunk.
enum class CallConv : uint8_t {
  Go, // the internal convention (CodeGen/GoABI.h, calling-convention.md)
  C,  // the exact C ABI (CodeGen/CABILower.h, c-abi.md)
};

class Type : public llvm::FoldingSetNode {
public:
  enum class Kind : uint8_t {
    Unit,   // the empty tuple: no value, size 0
    Bool,   // i1 in registers, one byte in memory
    Int,    // Bits (8, 16, 32, 64) and signedness
    Float,  // Bits (32, 64)
    String, // {ptr *u8, len u64, cap u64} (ADR-0026); fields 0, 1, 2
    Ptr,    // a pointer to Elem (every pointer kind of the language)
    Slice,  // {ptr *Elem, len u64, cap u64, alloc *u8} (ADR-0029); fields 0-3.
            // alloc is the allocator object that owns the buffer (null: the
            // global allocator). A borrowed []T is a Tuple(ptr, len) instead.
    Array,  // [Len]Elem
    Tuple,  // Elems, laid out like a struct
    Struct, // nominal: Name, fields set once with Context::setStructBody
    Func,   // a function pointer: Elems = parameters, Ret, Fails, its
            // calling convention (callConv)
    Option, // Option[T] of a type that has no nil of its own: T followed by a
            // one-byte "is set" flag, padded (D4.5); fields 0 (T) and 1 (bool).
            // Its == and hash ignore the flag's padding and, when the flag is
            // clear, the payload (Glue.h). Option of a pointer kind is the
            // pointer: no Option type.
  };

  Kind getKind() const { return K; }
  bool isUnit() const { return K == Kind::Unit; }
  bool isBool() const { return K == Kind::Bool; }
  bool isInt() const { return K == Kind::Int; }
  bool isFloat() const { return K == Kind::Float; }
  bool isPtr() const { return K == Kind::Ptr; }
  bool isOption() const { return K == Kind::Option; }
  /// The type of every interface value (Context::iface): a struct of two
  /// pointers, the itab and the data word (P04.06).
  bool isIface() const { return K == Kind::Struct && Name == "gopp.iface"; }
  /// Types whose value is one scalar in a register.
  bool isScalar() const {
    return K == Kind::Bool || K == Kind::Int || K == Kind::Float ||
           K == Kind::Ptr || K == Kind::Func;
  }

  unsigned bits() const { return Bits; }
  bool isSigned() const { return Signed; }
  Type *elem() const { return Elem; }
  uint64_t length() const { return Len; }
  llvm::ArrayRef<Type *> elems() const { return Elems; }
  Type *ret() const { return Ret; }
  bool fails() const { return Fails; }
  /// A Func type whose first parameter is the closure's environment, a
  /// `*u8` that the Go++ convention passes in the `swiftself` register, no
  /// argument's (P04.07, calling-convention.md). The code part of a func
  /// value has one; a top-level function does not.
  bool hasEnv() const { return Env; }
  /// The GIR type of a func value: the pair (code with an environment,
  /// environment). `Option[func]` has the same type; its nil is a null code
  /// pointer.
  bool isFuncValue() const {
    return K == Kind::Tuple && Elems.size() == 2 && Elems[0]->K == Kind::Func &&
           Elems[0]->Env;
  }
  /// The calling convention of a Func type (Go for every other kind).
  CallConv callConv() const { return CC; }
  llvm::StringRef name() const { return Name; }
  bool hasBody() const { return Body; }
  /// The source name of field I of a struct, or empty when none was recorded
  /// (a tuple, a runtime struct). Debug information only (P04.12): it is not
  /// part of the type's identity or its printed form; the bitstream keeps it.
  llvm::StringRef fieldName(unsigned I) const {
    return I < FieldNames.size() ? FieldNames[I] : llvm::StringRef();
  }
  /// The type arguments of a struct that is an instance of a generic type
  /// (`Pair[int, string]`: int64 and string), for the debugger's name of it
  /// (DebugInfo spell). Empty for every other type. Not part of the type's
  /// identity (its name is); the bitstream keeps it.
  llvm::ArrayRef<Type *> typeArgs() const { return TypeArgs; }
  /// A self-naming type (Context::recTy): a named type whose body is any
  /// structural kind, made for a type that reaches itself through no struct
  /// (`type S []S`, `type A [2]*A`). Once its body is set it behaves as the
  /// kind of its body (getKind, elem, length, ... answer for the body), but
  /// it is nominal: two of them are the same type only when they have the
  /// same name, and printing, the bitstream and the verifier use the name.
  bool isRec() const { return Rec; }
  /// Is the type known by a name (a struct or a self-naming type)?
  bool isNominal() const { return K == Kind::Struct || Rec; }

  /// The type of field I of a String, a Slice, an Option, a Tuple or a Struct
  /// (null if there is no such field).
  Type *fieldType(unsigned I, class Context &C) const;
  unsigned numFields() const;

  void Profile(llvm::FoldingSetNodeID &ID) const;
  void print(llvm::raw_ostream &OS) const;
  std::string str() const;

private:
  friend class Context;
  explicit Type(Kind K) : K(K) {}
  void printOne(llvm::raw_ostream &OS) const;
  Kind K;
  bool Signed = false;
  bool Fails = false;
  bool Env = false;
  CallConv CC = CallConv::Go;
  bool Body = false;
  bool Rec = false;
  unsigned Bits = 0;
  uint64_t Len = 0;
  Type *Elem = nullptr;
  Type *Ret = nullptr;
  llvm::ArrayRef<Type *> Elems;
  llvm::StringRef Name;
  llvm::ArrayRef<llvm::StringRef> FieldNames;
  llvm::ArrayRef<Type *> TypeArgs;
};

/// The size and alignment of a value in memory, in bytes, as the code
/// generator lays it out for linux/amd64 and linux/arm64 (D4.5; both have
/// 64-bit pointers and natural alignment): a bool is one byte, a string and a
/// slice 24 (pointer, length, capacity), a struct, a tuple and an Option(T) (T,
/// then a flag byte) are laid out in order with natural alignment, and the size
/// is a multiple of the alignment. `tests/unit/CodeGen/LayoutTest.cpp` compares
/// it with LLVM's data layout. Iterative: a chain of named types is as deep as
/// the source.
struct Layout {
  uint64_t Size = 0, Align = 1;
};
Layout layoutOf(const Type *T);
/// The same with a memo the caller keeps between calls (a chain of N nested
/// types asked for one by one costs N, not N squared).
Layout layoutOf(const Type *T, llvm::DenseMap<const Type *, Layout> &Memo);

class Context {
public:
  Context();
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  Type *unit() { return Unit; }
  Type *boolean() { return Bool; }
  Type *string() { return String; }
  /// An interface value (P04.06): `struct gopp.iface { itab *u8, data *u8 }`.
  /// One type for every interface type; the itab says which methods it has,
  /// and the lowering knows the static type. `Option[I]` has the same type,
  /// its nil is a null itab (D4.5). Glue (Glue.h) compares and hashes it by
  /// the dynamic type.
  Type *iface() { return Iface; }
  Type *intTy(unsigned Bits, bool Signed);
  Type *floatTy(unsigned Bits);
  Type *ptr(Type *Elem);
  Type *slice(Type *Elem);
  Type *array(Type *Elem, uint64_t Len);
  Type *option(Type *Elem);
  /// The empty list is Unit; a list of one stays a 1-tuple.
  Type *tuple(llvm::ArrayRef<Type *> Elems);
  /// Env: Params[0] is the closure environment, a `*u8` (Type::hasEnv).
  Type *func(llvm::ArrayRef<Type *> Params, Type *Ret, bool Fails = false,
             CallConv CC = CallConv::Go, bool Env = false);
  /// The Func type Sig with the calling convention CC.
  Type *withCallConv(Type *Sig, CallConv CC) {
    return func(Sig->elems(), Sig->ret(), Sig->fails(), CC, Sig->hasEnv());
  }
  /// The struct called Name (created without a body on first use).
  Type *structTy(llvm::StringRef Name);
  /// Sets the fields of a struct once; false if it already has a body.
  bool setStructBody(Type *S, llvm::ArrayRef<Type *> Fields);
  /// Records the source names of the fields of a struct that has a body, for
  /// the debugger (Type::fieldName). Once: false if S already has names or
  /// the count differs from the fields'.
  bool setFieldNames(Type *S, llvm::ArrayRef<llvm::StringRef> Names);
  /// Records the type arguments of a struct that is an instance of a generic
  /// type (Type::typeArgs). Once: false if S already has them or Args is
  /// empty.
  bool setTypeArgs(Type *S, llvm::ArrayRef<Type *> Args);

  /// The self-naming type called Name (created without a body on first
  /// use). Its parts may name it: `type S []S` is recTy("S") whose body is
  /// `slice(S)`. Until its body is set it has no kind (Unit).
  Type *recTy(llvm::StringRef Name);
  /// Gives R the body Body, a structural type made with R inside it (Ptr,
  /// Slice, Array, Option or Tuple). From then on R answers for the kind,
  /// element and length of Body, and Body itself is the same type as R: a
  /// structural type that is built later with the same parts (`slice(S)`)
  /// comes back as R, so `[]S` and `S` are one type, as the checker says
  /// (`S` has the underlying type `[]S`). False if R already has a body or
  /// Body is a body of another self-naming type.
  bool setRecBody(Type *R, Type *Body);
  /// T, or the self-naming type whose body T is (setRecBody).
  Type *canonical(Type *T);

  /// What makes two structural types the same type (Type.cpp).
  struct Key;

  // Common shorthands.
  Type *i8() { return intTy(8, true); }
  Type *u8() { return intTy(8, false); }
  Type *i32() { return intTy(32, true); }
  Type *i64() { return intTy(64, true); }
  Type *u64() { return intTy(64, false); }

private:
  Type *get(const Key &K);
  std::mutex Mu;
  llvm::BumpPtrAllocator Alloc;
  llvm::FoldingSet<Type> Set;
  llvm::StringMap<Type *> Structs;
  /// The structural body of each self-naming type (setRecBody) to the type.
  llvm::DenseMap<Type *, Type *> Canon;
  std::atomic<bool> AnyCanon{false};
  Type *Unit, *Bool, *String, *Iface;
};

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_TYPE_H
