//===- Types.cpp - Types, objects, scopes, universe -----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Types.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::ArrayRef;

//===----------------------------------------------------------------------===//
// Profiles
//===----------------------------------------------------------------------===//

void SignatureType::Profile(llvm::FoldingSetNodeID &ID) const {
  ID.AddPointer(Params);
  ID.AddPointer(Results);
  ID.AddBoolean(Variadic);
  ID.AddBoolean(Fails);
  ID.AddInteger(MoveMask);
  for (TypeParamType *T : TParams)
    ID.AddPointer(T);
}

void StructType::Profile(llvm::FoldingSetNodeID &ID) const {
  ID.AddBoolean(Strict);
  for (const Var *F : Fields) {
    ID.AddString(F->Name);
    // Unexported names of different packages are different (Go identity).
    ID.AddPointer(isExportedName(F->Name) ? nullptr : F->Pkg);
    ID.AddPointer(F->Ty);
    ID.AddBoolean(F->Embedded);
    ID.AddString(F->Tag);
    ID.AddBoolean(F->Required);
    ID.AddBoolean(F->RequiredSetOnly);
    ID.AddBoolean(F->MarkedRequired);
    ID.AddBoolean(F->MarkedOption);
  }
}

bool OptionType::usesNiche() const {
  if (llvm::isa<PointerType>(Elem))
    return true;
  auto *N = llvm::dyn_cast<NamedType>(Elem);
  if (!N)
    return false;
  // A defined pointer type (`type P *T`; its underlying type is set before
  // bodies are checked), or Unique / Shared / Weak.
  return N->Ptr != PtrKind::None ||
         (!N->Orig && N->Underlying && llvm::isa<PointerType>(N->Underlying));
}

void NamedType::Profile(llvm::FoldingSetNodeID &ID) const {
  ID.AddPointer(Orig);
  for (const Type *T : TArgs)
    ID.AddPointer(T);
}

//===----------------------------------------------------------------------===//
// Scope
//===----------------------------------------------------------------------===//

Object *Scope::lookup(llvm::StringRef Name, SourceLoc Pos,
                      Scope **Found) const {
  for (const Scope *S = this; S; S = S->Parent) {
    auto It = S->Elems.find(Name);
    if (It != S->Elems.end()) {
      Object *O = It->second;
      if (Pos.isValid() && O->ScopePos.isValid() && Pos < O->ScopePos)
        continue; // declared later in this block
      if (Found)
        *Found = const_cast<Scope *>(S);
      return O;
    }
  }
  return nullptr;
}

Object *Scope::insert(Object *Obj) {
  auto [It, Inserted] = Elems.try_emplace(Obj->Name, Obj);
  if (!Inserted)
    return It->second;
  if (!Obj->Parent)
    Obj->Parent = this;
  return nullptr;
}

bool Scope::isPackageScope() const {
  return Parent && !Parent->Parent && llvm::StringRef(Comment) == "package";
}

Scope *Arena::newScope(Scope *Parent, SourceRange Extent, const char *Comment) {
  Scopes.emplace_back(Parent, Extent, Comment);
  return &Scopes.back();
}

//===----------------------------------------------------------------------===//
// TypeContext
//===----------------------------------------------------------------------===//

static const struct {
  BasicKind K;
  unsigned Info;
  const char *Name;
} BasicTable[] = {
    {BasicKind::Invalid, 0, "invalid type"},
    {BasicKind::Bool, IsBoolean, "bool"},
    {BasicKind::Int, IsInteger, "int"},
    {BasicKind::Int8, IsInteger, "int8"},
    {BasicKind::Int16, IsInteger, "int16"},
    {BasicKind::Int32, IsInteger, "int32"},
    {BasicKind::Int64, IsInteger, "int64"},
    {BasicKind::Uint, IsInteger | IsUnsigned, "uint"},
    {BasicKind::Uint8, IsInteger | IsUnsigned, "uint8"},
    {BasicKind::Uint16, IsInteger | IsUnsigned, "uint16"},
    {BasicKind::Uint32, IsInteger | IsUnsigned, "uint32"},
    {BasicKind::Uint64, IsInteger | IsUnsigned, "uint64"},
    {BasicKind::Uintptr, IsInteger | IsUnsigned, "uintptr"},
    {BasicKind::Float32, IsFloat, "float32"},
    {BasicKind::Float64, IsFloat, "float64"},
    {BasicKind::Complex64, IsComplex, "complex64"},
    {BasicKind::Complex128, IsComplex, "complex128"},
    {BasicKind::String, IsString, "string"},
    {BasicKind::UnsafePointer, 0, "Pointer"},
    {BasicKind::UntypedBool, IsBoolean | IsUntyped, "untyped bool"},
    {BasicKind::UntypedInt, IsInteger | IsUntyped, "untyped int"},
    {BasicKind::UntypedRune, IsInteger | IsUntyped, "untyped rune"},
    {BasicKind::UntypedFloat, IsFloat | IsUntyped, "untyped float"},
    {BasicKind::UntypedComplex, IsComplex | IsUntyped, "untyped complex"},
    {BasicKind::UntypedString, IsString | IsUntyped, "untyped string"},
    {BasicKind::UntypedNil, IsUntyped, "untyped nil"},
};

static BasicType makeBasic(unsigned I) {
  return BasicType(BasicTable[I].K, BasicTable[I].Info, BasicTable[I].Name);
}

TypeContext::TypeContext(bool GoCompat)
    : Basics{makeBasic(0),  makeBasic(1),  makeBasic(2),  makeBasic(3),
             makeBasic(4),  makeBasic(5),  makeBasic(6),  makeBasic(7),
             makeBasic(8),  makeBasic(9),  makeBasic(10), makeBasic(11),
             makeBasic(12), makeBasic(13), makeBasic(14), makeBasic(15),
             makeBasic(16), makeBasic(17), makeBasic(18), makeBasic(19),
             makeBasic(20), makeBasic(21), makeBasic(22), makeBasic(23),
             makeBasic(24), makeBasic(25)} {
  static_assert(sizeof(BasicTable) / sizeof(BasicTable[0]) ==
                static_cast<unsigned>(BasicKind::NumBasicKinds));
  initUniverse(GoCompat);
}

ArrayRef<Type *> TypeContext::copyTypesLocked(ArrayRef<Type *> Ts) {
  if (Ts.empty())
    return {};
  Type **Mem = Alloc.Allocate<Type *>(Ts.size());
  std::uninitialized_copy(Ts.begin(), Ts.end(), Mem);
  return ArrayRef<Type *>(Mem, Ts.size());
}

ArrayRef<Type *> TypeContext::copyTypes(ArrayRef<Type *> Ts) {
  std::lock_guard<std::mutex> G(AllocMu);
  return copyTypesLocked(Ts);
}

llvm::SmallVector<Func *, 0> *TypeContext::newMethodList() {
  std::lock_guard<std::mutex> G(AllocMu);
  MethodLists.emplace_back();
  return &MethodLists.back();
}

namespace {
/// Memory in the allocator of a shard (struct fields and the arrays of a
/// type live next to the type).
template <class T>
ArrayRef<T> copyIn(llvm::BumpPtrAllocator &A, ArrayRef<T> Elts) {
  if (Elts.empty())
    return {};
  T *Mem = A.Allocate<T>(Elts.size());
  std::uninitialized_copy(Elts.begin(), Elts.end(), Mem);
  return ArrayRef<T>(Mem, Elts.size());
}
llvm::StringRef copyIn(llvm::BumpPtrAllocator &A, llvm::StringRef S) {
  char *Mem = A.Allocate<char>(S.size() + 1);
  std::copy(S.begin(), S.end(), Mem);
  Mem[S.size()] = 0;
  return llvm::StringRef(Mem, S.size());
}
} // namespace

/// RULE: a uniqued type must not carry body-specific data. Bodies are checked
/// in parallel and the first one to create a type owns the object that all the
/// others get back, so anything stored in it that is not in the profile (a
/// source position, a name written at one place, the package of an exported
/// field) would depend on thread timing and could leak into a diagnostic.
/// Keep such data in the side table (Info::Defs maps the identifier of a
/// struct field to the field's Var), never in the type. Today that is checked
/// for every uniqued type: pointers, slices, refs, arrays, maps, channels and
/// tuples hold types and numbers only; signatures hold no parameter names (they
/// live in the declaration) and type parameters are fresh per declaration;
/// instances hold the generic and its arguments; struct fields keep no Pos.
/// Test: ParallelCheck.UniquedStructsCarryNoBodyData.
///
/// Only the one shard the hash selects is locked. A FoldingSet picks its
/// bucket from the low bits of the same hash, so the shard comes from the high
/// bits.
template <class T, class MakeFn>
T *TypeContext::unique(Table<T> &Tab, const llvm::FoldingSetNodeID &ID,
                       MakeFn Make) {
  static_assert((NumShards & (NumShards - 1)) == 0, "a power of two");
  Shard<T> &Sh = Tab[(ID.ComputeHash() >> 24) & (NumShards - 1)];
  std::lock_guard<std::mutex> G(Sh.Mu);
  void *Pos;
  if (T *Found = Sh.Set.FindNodeOrInsertPos(ID, Pos))
    return Found;
  T *N = Make(Sh.Alloc);
  Sh.Set.InsertNode(N, Pos);
  return N;
}

#define GOPP_UNIQUE(Tab, Cls, ...)                                             \
  Cls Probe(__VA_ARGS__);                                                      \
  llvm::FoldingSetNodeID ID;                                                   \
  Probe.Profile(ID);                                                           \
  return unique(Tab, ID, [&](llvm::BumpPtrAllocator &A) {                      \
    return new (A.Allocate<Cls>()) Cls(Probe);                                 \
  });

PointerType *TypeContext::pointer(Type *Elem){
    GOPP_UNIQUE(Pointers, PointerType,
                Elem)} SliceType *TypeContext::slice(Type *Elem){
    GOPP_UNIQUE(Slices, SliceType, Elem)} RefType *TypeContext::ref(Type *Elem){
    GOPP_UNIQUE(Refs, RefType,
                Elem)} OptionType *TypeContext::option(Type *Elem){
    GOPP_UNIQUE(Options, OptionType,
                Elem)} ArrayType *TypeContext::array(Type *Elem, int64_t Len){
    GOPP_UNIQUE(Arrays, ArrayType, Elem,
                Len)} MapType *TypeContext::map(Type *Key, Type *Elem){
    GOPP_UNIQUE(Maps, MapType, Key,
                Elem)} ChanType *TypeContext::chan(ChanType::Dir D, Type *Elem){
    GOPP_UNIQUE(Chans, ChanType, D, Elem)}

TupleType *TypeContext::tuple(ArrayRef<Type *> Elems) {
  TupleType Probe(Elems);
  llvm::FoldingSetNodeID ID;
  Probe.Profile(ID);
  return unique(Tuples, ID, [&](llvm::BumpPtrAllocator &A) {
    return new (A.Allocate<TupleType>()) TupleType(copyIn(A, Elems));
  });
}

SignatureType *TypeContext::signature(TupleType *Params, TupleType *Results,
                                      bool Variadic, bool Fails,
                                      uint64_t MoveMask,
                                      ArrayRef<TypeParamType *> TParams) {
  SignatureType Probe(Params, Results, Variadic, Fails, MoveMask, TParams);
  llvm::FoldingSetNodeID ID;
  Probe.Profile(ID);
  return unique(Signatures, ID, [&](llvm::BumpPtrAllocator &A) {
    return new (A.Allocate<SignatureType>()) SignatureType(
        Params, Results, Variadic, Fails, MoveMask, copyIn(A, TParams));
  });
}

StructType *TypeContext::structOf(ArrayRef<FieldSpec> Fields, bool Strict) {
  // The profile is computed from the specs (the same as StructType::Profile
  // of the fields), so that a struct that exists already allocates nothing.
  llvm::FoldingSetNodeID ID;
  ID.AddBoolean(Strict);
  for (const FieldSpec &F : Fields) {
    ID.AddString(F.Name);
    ID.AddPointer(isExportedName(F.Name) ? nullptr : F.Pkg);
    ID.AddPointer(F.Ty);
    ID.AddBoolean(F.Embedded);
    ID.AddString(F.Tag);
    ID.AddBoolean(F.Required);
    ID.AddBoolean(F.RequiredSetOnly);
    ID.AddBoolean(F.MarkedRequired);
    ID.AddBoolean(F.MarkedOption);
  }
  return unique(Structs, ID, [&](llvm::BumpPtrAllocator &A) {
    llvm::SmallVector<Var *, 8> Vars;
    for (const FieldSpec &F : Fields) {
      static_assert(std::is_trivially_destructible_v<Var>);
      Var *V = new (A.Allocate<Var>()) Var();
      V->Name = copyIn(A, F.Name);
      // Only what the profile has (see the rule at unique()): the package of
      // an exported field is not part of the identity, so it is not kept.
      V->Pkg = isExportedName(F.Name) ? nullptr : F.Pkg;
      V->Ty = F.Ty;
      V->IsField = true;
      V->Embedded = F.Embedded;
      V->Tag = copyIn(A, F.Tag);
      V->Required = F.Required;
      V->RequiredSetOnly = F.RequiredSetOnly;
      V->MarkedRequired = F.MarkedRequired;
      V->MarkedOption = F.MarkedOption;
      Vars.push_back(V);
    }
    return new (A.Allocate<StructType>())
        StructType(copyIn(A, ArrayRef<Var *>(Vars)), Strict);
  });
}

InterfaceType *TypeContext::newInterface(ArrayRef<Func *> Methods,
                                         ArrayRef<Type *> Embeddeds,
                                         ArrayRef<SourceLoc> EmbedLocs,
                                         bool Implicit) {
  std::lock_guard<std::mutex> G(AllocMu);
  InterfaceType *I = new (Alloc.Allocate<InterfaceType>()) InterfaceType();
  I->Methods = Objs.copy(Methods);
  I->Embeddeds = copyTypesLocked(Embeddeds);
  I->EmbedLocs = Objs.copy(EmbedLocs);
  I->Implicit = Implicit;
  return I;
}

UnionType *TypeContext::newUnion(ArrayRef<Term> Terms) {
  std::lock_guard<std::mutex> G(AllocMu);
  return new (Alloc.Allocate<UnionType>()) UnionType(Objs.copy(Terms));
}

NamedType *TypeContext::newNamed(TypeName *Obj, Type *Underlying) {
  std::lock_guard<std::mutex> G(AllocMu);
  NamedType *N = new (Alloc.Allocate<NamedType>()) NamedType(Obj);
  N->Underlying = Underlying;
  if (Obj && !Obj->Ty)
    Obj->Ty = N;
  return N;
}

TypeParamType *TypeContext::newTypeParam(TypeName *Obj, unsigned Index,
                                         Type *Bound) {
  std::lock_guard<std::mutex> G(AllocMu);
  TypeParamType *T =
      new (Alloc.Allocate<TypeParamType>()) TypeParamType(Obj, Index);
  T->Bound = Bound;
  if (Obj && !Obj->Ty)
    Obj->Ty = T;
  return T;
}

TypeParamType *TypeContext::canonicalParam(Type *Bound, unsigned Ordinal) {
  std::lock_guard<std::mutex> G(CanonMu);
  TypeParamType *&P = CanonParams[{Bound, Ordinal}];
  if (!P) {
    TypeName *TN;
    {
      std::lock_guard<std::mutex> A(AllocMu);
      TN = Objs.make<TypeName>();
      TN->Name = "T";
    }
    P = newTypeParam(TN, Ordinal, Bound);
  }
  return P;
}

NamedType *TypeContext::instance(NamedType *Orig, ArrayRef<Type *> TArgs) {
  NamedType Probe(Orig->Obj);
  Probe.Orig = Orig;
  Probe.TArgs = TArgs;
  llvm::FoldingSetNodeID ID;
  Probe.Profile(ID);
  return unique(Instances, ID, [&](llvm::BumpPtrAllocator &A) {
    NamedType *N = new (A.Allocate<NamedType>()) NamedType(Orig->Obj);
    N->Orig = Orig;
    N->TArgs = copyIn(A, TArgs);
    N->Ptr = Orig->Ptr;
    return N;
  });
}

Type *sema::under(Type *T) {
  for (;;) {
    switch (T->getKind()) {
    case Type::Kind::Named: {
      auto *N = llvm::cast<NamedType>(T);
      if (!N->Underlying) {
        if (!N->DirectRhs)
          return T;
        T = N->DirectRhs;
        continue;
      }
      T = N->Underlying;
      continue;
    }
    case Type::Kind::TypeParam: {
      Type *B = llvm::cast<TypeParamType>(T)->Bound;
      if (!B)
        return T;
      T = B;
      if (llvm::isa<TypeParamType>(T))
        return T;
      continue;
    }
    default:
      return T;
    }
  }
}

//===----------------------------------------------------------------------===//
// Universe
//===----------------------------------------------------------------------===//

void TypeContext::initUniverse(bool GoCompat) {
  Universe = Objs.newScope(nullptr, {}, "universe");
  auto Def = [&](Object *O) {
    Universe->insert(O);
    return O;
  };
  auto DefType = [&](llvm::StringRef Name, Type *T) {
    TypeName *TN = Objs.make<TypeName>();
    TN->Name = Name;
    TN->Ty = T;
    Def(TN);
    return TN;
  };
  for (unsigned I = 1; I <= static_cast<unsigned>(BasicKind::String); ++I)
    DefType(BasicTable[I].Name, &Basics[I]);
  // Aliases.
  // byte and rune are distinct objects that are identical to uint8 and int32
  // (types2: universeByte, universeRune); they only print differently.
  ByteType = Objs.make<BasicType>(BasicKind::Uint8, typ(BasicKind::Uint8)->Info,
                                  "byte");
  RuneType = Objs.make<BasicType>(BasicKind::Int32, typ(BasicKind::Int32)->Info,
                                  "rune");
  DefType("byte", ByteType)->IsAlias = true;
  DefType("rune", RuneType)->IsAlias = true;

  // any, error, comparable, Copy, Clone.
  AnyType = newInterface({}, {}, {});
  llvm::cast<InterfaceType>(AnyType)->IsAny = true;
  DefType("any", AnyType)->IsAlias = true;
  {
    TypeName *TN = Objs.make<TypeName>();
    TN->Name = "error";
    NamedType *N = newNamed(TN);
    Func *M = Objs.make<Func>();
    M->Name = "Error";
    M->IsInterfaceMethod = true;
    M->Ty =
        signature(tuple({}), tuple({typ(BasicKind::String)}), false, false, 0);
    M->Recv = nullptr;
    Func *Ms[] = {M};
    N->Underlying = newInterface(Ms, {}, {});
    ErrorType = N;
    Def(TN);
  }
  auto Special = [&](llvm::StringRef Name, InterfaceType::Special Sp) {
    TypeName *TN = Objs.make<TypeName>();
    TN->Name = Name;
    NamedType *N = newNamed(TN);
    InterfaceType *I = newInterface({}, {}, {});
    I->Sp = Sp;
    N->Underlying = I;
    Def(TN);
    return N;
  };
  ComparableType = Special("comparable", InterfaceType::Comparable);
  if (!GoCompat) {
    CopyType = Special("Copy", InterfaceType::Copy);
    CloneType = Special("Clone", InterfaceType::Clone);
    // Unique[*T], Shared[*T], Weak[*T] (D1.7, ADR-0018).
    const std::pair<const char *, PtrKind> PK[] = {{"Unique", PtrKind::Unique},
                                                   {"Shared", PtrKind::Shared},
                                                   {"Weak", PtrKind::Weak}};
    for (auto &[Name, Kind] : PK) {
      TypeName *TN = Objs.make<TypeName>();
      TN->Name = Name;
      NamedType *N = newNamed(TN);
      N->Ptr = Kind;
      TypeName *PTN = Objs.make<TypeName>();
      PTN->Name = "T";
      TypeParamType *TP = newTypeParam(PTN, 0, AnyType);
      TypeParamType *TPs[] = {TP};
      N->TParams =
          ArrayRef<TypeParamType *>(Objs.copy(ArrayRef<TypeParamType *>(TPs)));
      N->Underlying = structOf({});
      Def(TN);
    }
    // Option[T] (ADR-0018): an instance is an OptionType, built by
    // Checker::instantiatedType.
    {
      TypeName *TN = Objs.make<TypeName>();
      TN->Name = "Option";
      NamedType *N = newNamed(TN);
      N->IsOption = true;
      TypeName *PTN = Objs.make<TypeName>();
      PTN->Name = "T";
      TypeParamType *TP = newTypeParam(PTN, 0, AnyType);
      TypeParamType *TPs[] = {TP};
      N->TParams =
          ArrayRef<TypeParamType *>(Objs.copy(ArrayRef<TypeParamType *>(TPs)));
      N->Underlying = structOf({});
      Def(TN);
    }
  }

  // Constants.
  auto DefConst = [&](llvm::StringRef Name, BasicKind K, ConstValue V) {
    Const *C = Objs.make<Const>();
    C->Name = Name;
    C->Ty = typ(K);
    C->Val = Objs.constant(std::move(V));
    Def(C);
  };
  DefConst("true", BasicKind::UntypedBool, ConstValue::makeBool(true));
  DefConst("false", BasicKind::UntypedBool, ConstValue::makeBool(false));
  DefConst("iota", BasicKind::UntypedInt, ConstValue::makeInt64(0));
  Nil *N = Objs.make<Nil>();
  N->Name = "nil";
  N->Ty = typ(BasicKind::UntypedNil);
  Def(N);

  auto DefBuiltin = [&](llvm::StringRef Name, BuiltinId Id, Scope *S) {
    Builtin *B = Objs.make<Builtin>();
    B->Name = Name;
    B->Id = Id;
    B->Ty = invalid();
    S->insert(B);
  };
  const std::pair<const char *, BuiltinId> BI[] = {
      {"append", BuiltinId::Append},   {"cap", BuiltinId::Cap},
      {"clear", BuiltinId::Clear},     {"close", BuiltinId::Close},
      {"complex", BuiltinId::Complex}, {"copy", BuiltinId::Copy},
      {"delete", BuiltinId::Delete},   {"imag", BuiltinId::Imag},
      {"len", BuiltinId::Len},         {"make", BuiltinId::Make},
      {"max", BuiltinId::Max},         {"min", BuiltinId::Min},
      {"panic", BuiltinId::Panic},     {"print", BuiltinId::Print},
      {"println", BuiltinId::Println}, {"real", BuiltinId::Real},
      {"recover", BuiltinId::Recover}};
  for (auto &[Name, Id] : BI)
    DefBuiltin(Name, Id, Universe);
  if (GoCompat) {
    DefBuiltin("new", BuiltinId::New, Universe);
    DefBuiltin("assert", BuiltinId::Assert, Universe);
    DefBuiltin("trace", BuiltinId::Trace, Universe);
  } else {
    DefBuiltin("move", BuiltinId::Move, Universe);
    DefBuiltin("clone", BuiltinId::Clone, Universe);
    DefBuiltin("new_unique", BuiltinId::NewUnique, Universe);
    DefBuiltin("new_shared", BuiltinId::NewShared, Universe);
    DefBuiltin("new_weak", BuiltinId::NewWeak, Universe);
  }

  // package unsafe
  Unsafe = Objs.make<Package>();
  Unsafe->Path = "unsafe";
  Unsafe->Name = "unsafe";
  Unsafe->Complete = true;
  Unsafe->PkgScope = Objs.newScope(Universe, {}, "package");
  {
    TypeName *TN = Objs.make<TypeName>();
    TN->Name = "Pointer";
    TN->Pkg = Unsafe;
    TN->Ty = typ(BasicKind::UnsafePointer);
    Unsafe->PkgScope->insert(TN);
  }
  const std::pair<const char *, BuiltinId> UB[] = {
      {"Alignof", BuiltinId::Alignof}, {"Offsetof", BuiltinId::Offsetof},
      {"Sizeof", BuiltinId::Sizeof},   {"Add", BuiltinId::Add},
      {"Slice", BuiltinId::Slice},     {"SliceData", BuiltinId::SliceData},
      {"String", BuiltinId::String},   {"StringData", BuiltinId::StringData}};
  for (auto &[Name, Id] : UB) {
    Builtin *B = Objs.make<Builtin>();
    B->Name = Name;
    B->Id = Id;
    B->Pkg = Unsafe;
    B->Ty = invalid();
    Unsafe->PkgScope->insert(B);
  }
}
