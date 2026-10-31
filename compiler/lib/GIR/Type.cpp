//===- Type.cpp - GIR types -----------------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Type.h"
#include "gopp/Basic/NewStack.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::gir;
using llvm::ArrayRef;

/// What makes two structural types the same type.
struct Context::Key {
  explicit Key(Type::Kind K) : K(K) {}
  Type::Kind K;
  unsigned Bits = 0;
  bool Signed = false, Fails = false, Env = false;
  CallConv CC = CallConv::Go;
  uint64_t Len = 0;
  Type *Elem = nullptr, *Ret = nullptr;
  ArrayRef<Type *> Elems;

  void profile(llvm::FoldingSetNodeID &ID) const {
    ID.AddInteger(static_cast<unsigned>(K));
    ID.AddInteger(Bits);
    ID.AddBoolean(Signed);
    ID.AddBoolean(Fails);
    ID.AddBoolean(Env);
    ID.AddInteger(static_cast<unsigned>(CC));
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
  Kx.Env = Env;
  Kx.CC = CC;
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
  Iface = structTy("gopp.iface");
  Type *P = ptr(intTy(8, false));
  setStructBody(Iface, {P, P});
  // The debugger shows an interface value as these two words (P04.12).
  setFieldNames(Iface, {"itab", "data"});
}

Type *Context::get(const Key &K) {
  std::lock_guard<std::mutex> G(Mu);
  llvm::FoldingSetNodeID ID;
  K.profile(ID);
  void *Pos = nullptr;
  if (Type *T = Set.FindNodeOrInsertPos(ID, Pos)) {
    auto C = Canon.find(T);
    return C == Canon.end() ? T : C->second;
  }
  Type *T = new (Alloc.Allocate<Type>()) Type(K.K);
  T->Bits = K.Bits;
  T->Signed = K.Signed;
  T->Fails = K.Fails;
  T->Env = K.Env;
  T->CC = K.CC;
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

Type *Context::func(ArrayRef<Type *> Params, Type *Ret, bool Fails, CallConv CC,
                    bool Env) {
  Key K(Type::Kind::Func);
  K.Elems = Params;
  K.Ret = Ret;
  K.Fails = Fails;
  K.Env = Env;
  K.CC = CC;
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

bool Context::setFieldNames(Type *S, ArrayRef<llvm::StringRef> Names) {
  std::lock_guard<std::mutex> G(Mu);
  assert(S->getKind() == Type::Kind::Struct);
  if (!S->Body || !S->FieldNames.empty() || Names.size() != S->Elems.size() ||
      Names.empty())
    return false;
  llvm::StringRef *Mem = Alloc.Allocate<llvm::StringRef>(Names.size());
  for (size_t I = 0; I < Names.size(); ++I) {
    char *Chars = Alloc.Allocate<char>(Names[I].size() + 1);
    std::copy(Names[I].begin(), Names[I].end(), Chars);
    Chars[Names[I].size()] = 0;
    Mem[I] = llvm::StringRef(Chars, Names[I].size());
  }
  S->FieldNames = ArrayRef<llvm::StringRef>(Mem, Names.size());
  return true;
}

bool Context::setTypeArgs(Type *S, ArrayRef<Type *> Args) {
  std::lock_guard<std::mutex> G(Mu);
  assert(S->getKind() == Type::Kind::Struct);
  if (!S->TypeArgs.empty() || Args.empty())
    return false;
  Type **Mem = Alloc.Allocate<Type *>(Args.size());
  std::copy(Args.begin(), Args.end(), Mem);
  S->TypeArgs = ArrayRef<Type *>(Mem, Args.size());
  return true;
}

Type *Context::recTy(llvm::StringRef Name) {
  std::lock_guard<std::mutex> G(Mu);
  auto It = Structs.find(Name);
  if (It != Structs.end())
    return It->second;
  Type *T = new (Alloc.Allocate<Type>()) Type(Type::Kind::Unit);
  T->Rec = true;
  char *Mem = Alloc.Allocate<char>(Name.size() + 1);
  std::copy(Name.begin(), Name.end(), Mem);
  Mem[Name.size()] = 0;
  T->Name = llvm::StringRef(Mem, Name.size());
  Structs[Name] = T;
  return T;
}

bool Context::setRecBody(Type *R, Type *Body) {
  std::lock_guard<std::mutex> G(Mu);
  assert(R->Rec && "not a self-naming type");
  if (R->Body || Body == R || Body->Rec || Canon.count(Body))
    return false;
  switch (Body->K) {
  case Type::Kind::Ptr:
  case Type::Kind::Slice:
  case Type::Kind::Array:
  case Type::Kind::Option:
  case Type::Kind::Tuple:
    break;
  default:
    return false;
  }
  R->K = Body->K;
  R->Bits = Body->Bits;
  R->Signed = Body->Signed;
  R->Fails = Body->Fails;
  R->Len = Body->Len;
  R->Elem = Body->Elem;
  R->Ret = Body->Ret;
  R->Elems = Body->Elems;
  R->Body = true;
  Canon[Body] = R;
  AnyCanon = true;
  return true;
}

Type *Context::canonical(Type *T) {
  if (!AnyCanon)
    return T;
  std::lock_guard<std::mutex> G(Mu);
  auto It = Canon.find(T);
  return It == Canon.end() ? T : It->second;
}

Layout gir::layoutOf(const Type *T) {
  llvm::DenseMap<const Type *, Layout> Memo;
  return layoutOf(T, Memo);
}

Layout gir::layoutOf(const Type *T,
                     llvm::DenseMap<const Type *, Layout> &Memo) {
  // Post-order over the parts, with an explicit stack (a type is as deep as
  // the source).
  llvm::SmallVector<std::pair<const Type *, unsigned>, 16> Stack;
  auto Parts = [](const Type *X, llvm::SmallVectorImpl<const Type *> &Out) {
    switch (X->getKind()) {
    case Type::Kind::Array:
    case Type::Kind::Option:
      Out.push_back(X->elem());
      break;
    case Type::Kind::Tuple:
    case Type::Kind::Struct:
      Out.append(X->elems().begin(), X->elems().end());
      break;
    default:
      break;
    }
  };
  Stack.push_back({T, 0});
  while (!Stack.empty()) {
    auto [Cur, Next] = Stack.back();
    if (Memo.count(Cur)) {
      Stack.pop_back();
      continue;
    }
    llvm::SmallVector<const Type *, 4> Ps;
    Parts(Cur, Ps);
    if (Next < Ps.size()) {
      Stack.back().second = Next + 1;
      if (!Memo.count(Ps[Next]))
        Stack.push_back({Ps[Next], 0});
      continue;
    }
    Layout L;
    switch (Cur->getKind()) {
    case Type::Kind::Unit:
      L = {0, 1};
      break;
    case Type::Kind::Bool:
      L = {1, 1};
      break;
    case Type::Kind::Int:
    case Type::Kind::Float:
      L = {Cur->bits() / 8, Cur->bits() / 8};
      break;
    case Type::Kind::Ptr:
    case Type::Kind::Func:
      L = {8, 8};
      break;
    case Type::Kind::String:
      L = {24, 8};
      break;
    case Type::Kind::Slice:
      L = {32, 8}; // ptr, len, cap and the allocator word (ADR-0029)
      break;
    case Type::Kind::Array: {
      Layout E = Memo[Cur->elem()];
      L = {E.Size * Cur->length(), E.Align};
      break;
    }
    case Type::Kind::Option: {
      Layout E = Memo[Cur->elem()];
      L.Align = E.Align;
      L.Size = (E.Size + 1 + E.Align - 1) / E.Align * E.Align;
      break;
    }
    case Type::Kind::Tuple:
    case Type::Kind::Struct: {
      uint64_t Off = 0, A = 1;
      for (const Type *P : Ps) {
        Layout E = Memo[P];
        Off = (Off + E.Align - 1) / E.Align * E.Align + E.Size;
        A = std::max(A, E.Align);
      }
      L = {(Off + A - 1) / A * A, A};
      break;
    }
    }
    Memo[Cur] = L;
    Stack.pop_back();
  }
  return Memo[T];
}

unsigned Type::numFields() const {
  switch (K) {
  case Kind::String:
    return 3;
  case Kind::Slice:
    return 4;
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
    return I == 0 ? C.ptr(Elem) : I == 3 ? C.ptr(C.u8()) : C.u64();
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
  if (Rec) {
    OS << Name;
    return;
  }
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
    OS << (CC == CallConv::C ? "fn \"C\"(" : Env ? "fn env(" : "fn(");
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
