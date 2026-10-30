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

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gopp {
namespace gir {

class Type : public llvm::FoldingSetNode {
public:
  enum class Kind : uint8_t {
    Unit,   // the empty tuple: no value, size 0
    Bool,   // i1 in registers, one byte in memory
    Int,    // Bits (8, 16, 32, 64) and signedness
    Float,  // Bits (32, 64)
    String, // {ptr *u8, len u64, cap u64} (ADR-0026); fields 0, 1, 2
    Ptr,    // a pointer to Elem (every pointer kind of the language)
    Slice,  // {ptr *Elem, len u64, cap u64}; fields 0, 1, 2
    Array,  // [Len]Elem
    Tuple,  // Elems, laid out like a struct
    Struct, // nominal: Name, fields set once with Context::setStructBody
    Func,   // a function pointer: Elems = parameters, Ret, Fails
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
  llvm::StringRef name() const { return Name; }
  bool hasBody() const { return Body; }

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
  bool Body = false;
  unsigned Bits = 0;
  uint64_t Len = 0;
  Type *Elem = nullptr;
  Type *Ret = nullptr;
  llvm::ArrayRef<Type *> Elems;
  llvm::StringRef Name;
};

class Context {
public:
  Context();
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  Type *unit() { return Unit; }
  Type *boolean() { return Bool; }
  Type *string() { return String; }
  Type *intTy(unsigned Bits, bool Signed);
  Type *floatTy(unsigned Bits);
  Type *ptr(Type *Elem);
  Type *slice(Type *Elem);
  Type *array(Type *Elem, uint64_t Len);
  Type *option(Type *Elem);
  /// The empty list is Unit; a list of one stays a 1-tuple.
  Type *tuple(llvm::ArrayRef<Type *> Elems);
  Type *func(llvm::ArrayRef<Type *> Params, Type *Ret, bool Fails = false);
  /// The struct called Name (created without a body on first use).
  Type *structTy(llvm::StringRef Name);
  /// Sets the fields of a struct once; false if it already has a body.
  bool setStructBody(Type *S, llvm::ArrayRef<Type *> Fields);

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
  Type *Unit, *Bool, *String;
};

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_TYPE_H
