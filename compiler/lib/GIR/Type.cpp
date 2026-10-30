//===- Type.cpp - GIR types -----------------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Type.h"
#include "gopp/Basic/NewStack.h"

using namespace gopp;
using namespace gopp::gir;
using llvm::ArrayRef;

/// What makes two structural types the same type.
struct Context::Key {
  explicit Key(Type::Kind K) : K(K) {}
  Type::Kind K;
  unsigned Bits = 0;
  bool Signed = false, Fails = false;
  uint64_t Len = 0;
  Type *Elem = nullptr, *Ret = nullptr;
  ArrayRef<Type *> Elems;

  void profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(static_cast<unsigned>(K));
    ID.AddInteger(Bits);
    ID.AddBoolean(Signed);
    ID.AddBoolean(Fails);
    ID.AddInteger(Len);
    ID.AddPointer(Elem);
    ID.AddPointer(Ret);
    ID.AddInteger(Elems.size());
    for (Type *T : Elems)
      ID.AddPointer(T);
  }
};

void Type::Profile(llvm::FoldingSetNodeID &ID) const {
  Context::Key Kx(K);
  Kx.Bits = Bits;
  Kx.Signed = Signed;
  Kx.Fails = Fails;
  Kx.Len = Len;
  Kx.Elem = Elem;
  Kx.Ret = Ret;
  Kx.Elems = Elems;
  Kx.profile(ID);
}

Context::Context() {
  Unit = get(Key(Type::Kind::Unit));
  Bool = get(Key(Type::Kind::Bool));
  String = get(Key(Type::Kind::String));
}

Type *Context::get(const Key &K) {
  std::lock_guard<std::mutex> G(Mu);
  llvm::FoldingSetNodeID ID;
  K.profile(ID);
  void *Pos = nullptr;
  if (Type *T = Set.FindNodeOrInsertPos(ID, Pos))
    return T;
  Type *T = new (Alloc.Allocate<Type>()) Type(K.K);
  T->Bits = K.Bits;
  T->Signed = K.Signed;
  T->Fails = K.Fails;
  T->Len = K.Len;
  T->Elem = K.Elem;
  T->Ret = K.Ret;
  if (!K.Elems.empty()) {
    Type **Mem = Alloc.Allocate<Type *>(K.Elems.size());
    std::copy(K.Elems.begin(), K.Elems.end(), Mem);
    T->Elems = ArrayRef<Type *>(Mem, K.Elems.size());
  }
  Set.InsertNode(T, Pos);
  return T;
}

Type *Context::intTy(unsigned Bits, bool Signed) {
  assert((Bits == 8 || Bits == 16 || Bits == 32 || Bits == 64) &&
         "GIR integers are 8, 16, 32 or 64 bits");
  Key K(Type::Kind::Int);
  K.Bits = Bits;
  K.Signed = Signed;
  return get(K);
}

Type *Context::floatTy(unsigned Bits) {
  assert((Bits == 32 || Bits == 64) && "GIR floats are 32 or 64 bits");
  Key K(Type::Kind::Float);
  K.Bits = Bits;
  return get(K);
}

Type *Context::ptr(Type *Elem) {
  Key K(Type::Kind::Ptr);
  K.Elem = Elem;
  return get(K);
}

Type *Context::slice(Type *Elem) {
  Key K(Type::Kind::Slice);
  K.Elem = Elem;
  return get(K);
}

Type *Context::array(Type *Elem, uint64_t Len) {
  Key K(Type::Kind::Array);
  K.Elem = Elem;
  K.Len = Len;
  return get(K);
}

Type *Context::option(Type *Elem) {
  Key K(Type::Kind::Option);
  K.Elem = Elem;
  return get(K);
}

Type *Context::tuple(ArrayRef<Type *> Elems) {
  if (Elems.empty())
    return Unit;
  Key K(Type::Kind::Tuple);
  K.Elems = Elems;
  return get(K);
}

Type *Context::func(ArrayRef<Type *> Params, Type *Ret, bool Fails) {
  Key K(Type::Kind::Func);
  K.Elems = Params;
  K.Ret = Ret;
  K.Fails = Fails;
  return get(K);
}

Type *Context::structTy(llvm::StringRef Name) {
  std::lock_guard<std::mutex> G(Mu);
  auto It = Structs.find(Name);
  if (It != Structs.end())
    return It->second;
  Type *T = new (Alloc.Allocate<Type>()) Type(Type::Kind::Struct);
  char *Mem = Alloc.Allocate<char>(Name.size() + 1);
  std::copy(Name.begin(), Name.end(), Mem);
  Mem[Name.size()] = 0;
  T->Name = llvm::StringRef(Mem, Name.size());
  Structs[Name] = T;
  return T;
}

bool Context::setStructBody(Type *S, ArrayRef<Type *> Fields) {
  std::lock_guard<std::mutex> G(Mu);
  assert(S->getKind() == Type::Kind::Struct);
  if (S->Body)
    return false;
  if (!Fields.empty()) {
    Type **Mem = Alloc.Allocate<Type *>(Fields.size());
    std::copy(Fields.begin(), Fields.end(), Mem);
    S->Elems = ArrayRef<Type *>(Mem, Fields.size());
  }
  S->Body = true;
  return true;
}

unsigned Type::numFields() const {
  switch (K) {
  case Kind::String:
  case Kind::Slice:
    return 3;
  case Kind::Option:
    return 2;
  case Kind::Tuple:
  case Kind::Struct:
    return Elems.size();
  default:
    return 0;
  }
}

Type *Type::fieldType(unsigned I, Context &C) const {
  if (I >= numFields())
    return nullptr;
  switch (K) {
  case Kind::String:
    return I == 0 ? C.ptr(C.u8()) : C.u64();
  case Kind::Slice:
    return I == 0 ? C.ptr(Elem) : C.u64();
  case Kind::Option:
    return I == 0 ? Elem : C.boolean();
  default:
    return Elems[I];
  }
}

void Type::print(llvm::raw_ostream &OS) const {
  // The text recurses once per level of the type, and a chain of named types
  // makes a structural type thousands of levels deep: a new stack every
  // DeepWalk::Segment levels (Basic/NewStack.h; each thread counts its own
  // levels, which is all a segment needs).
  static thread_local DeepWalk Walk;
  // No stack for a level this deep: the text stops there, bounded.
  if (!Walk.level([&] { printOne(OS); }))
    OS << "...";
}

void Type::printOne(llvm::raw_ostream &OS) const {
  switch (K) {
  case Kind::Unit:
    OS << "()";
    return;
  case Kind::Bool:
    OS << "bool";
    return;
  case Kind::Int:
    OS << (Signed ? 'i' : 'u') << Bits;
    return;
  case Kind::Float:
    OS << 'f' << Bits;
    return;
  case Kind::String:
    OS << "string";
    return;
  case Kind::Ptr:
    OS << '*';
    Elem->print(OS);
    return;
  case Kind::Slice:
    OS << "[]";
    Elem->print(OS);
    return;
  case Kind::Array:
    OS << '[' << Len << ']';
    Elem->print(OS);
    return;
  case Kind::Option:
    OS << "Option<";
    Elem->print(OS);
    OS << '>';
    return;
  case Kind::Tuple: {
    OS << '(';
    bool First = true;
    for (Type *T : Elems) {
      if (!First)
        OS << ", ";
      First = false;
      T->print(OS);
    }
    OS << ')';
    return;
  }
  case Kind::Struct:
    OS << Name;
    return;
  case Kind::Func: {
    OS << "fn(";
    bool First = true;
    for (Type *T : Elems) {
      if (!First)
        OS << ", ";
      First = false;
      T->print(OS);
    }
    OS << ')';
    if (Fails)
      OS << '!';
    OS << " -> ";
    Ret->print(OS);
    return;
  }
  }
}

std::string Type::str() const {
  std::string S;
  llvm::raw_string_ostream OS(S);
  print(OS);
  return S;
}
