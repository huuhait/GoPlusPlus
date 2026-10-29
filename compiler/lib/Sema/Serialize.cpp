//===- Serialize.cpp - Type and object serialization (D3.8) ---------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Record layouts (operands; `T` is a type id, `S` a string id, `P` a package
// reference id, 0 meaning none):
//   STRING        blob
//   PKGREF        S(path) S(name)
//   BASIC         kind alias(0 | 1 byte | 2 rune)
//   UNIVERSE      S(name)                      error, any, comparable, ...
//   POINTER/SLICE/REF  T(elem)
//   ARRAY         T(elem) len
//   MAP           T(key) T(elem)
//   CHAN          dir T(elem)
//   TUPLE         n T...
//   SIGNATURE     T(params) T(results) variadic fails moveMask n T(tparam)...
//   STRUCT        n strict { S(name) P T embedded parent S(tag) required
//                 setOnly markedRequired markedOption }...
//   INTERFACE     implicit special nm ne { S(name) P T(sig) fails }... T...
//                 T(extends, 0 for none)
//   UNION         n { tilde T }...
//   NAMED_DECL    P S(name) ptrKind flags zeroState
//                 flags: bit 0 @Pinned, bit 1 @View, bit 2 @Abstract (the
//                 type attribute), bits 3-4 abstract (0 not known, 1 no, 2 yes)
//   NAMED_DEF     T(named) T(underlying) n T(tparam)... nm
//                 { S(name) T(sig) flags nr T... no T... }...
//                 method flags: bit 0 pointer receiver, 1 move receiver,
//                 2 fails, 3 builtin, bits 4-5 the mark (0 none, 1 @Virtual,
//                 2 @Abstract, 3 @Override)
//   NAMED_INST    T(origin) n T(arg)...
//   TPARAM_DECL   S(name) index
//   TPARAM_DEF    T(tparam) T(bound)
//   PKG_BEGIN     S(name) S(path)
//   OBJ_CONST     S(name) T hasValue value...
//   OBJ_TYPE      S(name) T alias n T(tparam)...
//   OBJ_VAR       S(name) T
//   OBJ_FUNC      S(name) T(sig) fails np S... nr S...
//   PKG_END
// A named type or type parameter gets its id from the DECL record and its
// contents from a later DEF record, so types that refer to themselves (a list
// node pointing to the node, a method taking its own type) need no special
// case. The DEF of a type parameter (its bound) is written after the type
// that declared it is complete: type parameters that share one bound
// (`[P, Q interface{ f(Q) P }]`) keep sharing it.
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Serialize.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::sema;
using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::isa_and_nonnull;
using llvm::SmallVector;
using llvm::StringRef;

//===----------------------------------------------------------------------===//
// MemoryRecordStream
//===----------------------------------------------------------------------===//

void MemoryRecordStream::write(unsigned Code, ArrayRef<uint64_t> Ops,
                               StringRef Blob) {
  Record R;
  R.Code = Code;
  R.Ops.assign(Ops.begin(), Ops.end());
  R.Blob = Blob.str();
  Recs.push_back(std::move(R));
}

bool MemoryRecordStream::read(Record &R) {
  if (Pos >= Recs.size())
    return false;
  R = Recs[Pos++];
  return true;
}

static void putVarint(std::string &Out, uint64_t V) {
  do {
    uint8_t B = V & 0x7f;
    V >>= 7;
    Out.push_back(static_cast<char>(V ? B | 0x80 : B));
  } while (V);
}

static bool getVarint(StringRef &In, uint64_t &V) {
  V = 0;
  for (unsigned Shift = 0; Shift < 70; Shift += 7) {
    if (In.empty())
      return false;
    uint8_t B = static_cast<uint8_t>(In.front());
    In = In.drop_front();
    V |= uint64_t(B & 0x7f) << Shift;
    if (!(B & 0x80))
      return true;
  }
  return false;
}

std::string MemoryRecordStream::toBytes() const {
  std::string Out;
  for (const Record &R : Recs) {
    putVarint(Out, R.Code);
    putVarint(Out, R.Ops.size());
    for (uint64_t Op : R.Ops)
      putVarint(Out, Op);
    putVarint(Out, R.Blob.size());
    Out += R.Blob;
  }
  return Out;
}

bool MemoryRecordStream::fromBytes(StringRef Bytes) {
  Recs.clear();
  Pos = 0;
  while (!Bytes.empty()) {
    Record R;
    uint64_t Code, N, Len;
    if (!getVarint(Bytes, Code) || !getVarint(Bytes, N) || N > Bytes.size())
      return false;
    R.Code = static_cast<unsigned>(Code);
    for (uint64_t I = 0; I < N; ++I) {
      uint64_t Op;
      if (!getVarint(Bytes, Op))
        return false;
      R.Ops.push_back(Op);
    }
    if (!getVarint(Bytes, Len) || Len > Bytes.size())
      return false;
    R.Blob = Bytes.substr(0, Len).str();
    Bytes = Bytes.drop_front(Len);
    Recs.push_back(std::move(R));
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Writer
//===----------------------------------------------------------------------===//

namespace {
void putAPInt(const llvm::APInt &V, std::vector<uint64_t> &Ops) {
  Ops.push_back(V.getBitWidth());
  Ops.push_back(V.getNumWords());
  for (unsigned I = 0; I < V.getNumWords(); ++I)
    Ops.push_back(V.getRawData()[I]);
}

void putReal(const Real &R, std::vector<uint64_t> &Ops) {
  Ops.push_back(R.isRat());
  if (R.isRat()) {
    putAPInt(R.ratNum(), Ops);
    putAPInt(R.ratDen(), Ops);
  } else {
    const BigFloat &F = R.bigValue();
    Ops.push_back(F.Neg);
    Ops.push_back(static_cast<uint64_t>(F.Exp));
    putAPInt(F.Mant, Ops);
  }
}
} // namespace

uint64_t TypeWriter::string(StringRef S) {
  if (S.empty())
    return 0;
  auto It = Strings.find(S);
  if (It != Strings.end())
    return It->second;
  W.write(REC_STRING, {}, S);
  uint64_t Id = Strings.size() + 1;
  Strings[S] = Id;
  return Id;
}

uint64_t TypeWriter::package(const Package *P) {
  if (!P)
    return 0;
  auto It = Pkgs.find(P);
  if (It != Pkgs.end())
    return It->second;
  uint64_t Ops[] = {string(P->Path), string(P->Name)};
  W.write(REC_PKGREF, Ops);
  uint64_t Id = Pkgs.size() + 1;
  Pkgs[P] = Id;
  return Id;
}

void TypeWriter::declareTypeParam(const TypeParamType *T) {
  uint64_t Ops[] = {string(T->Obj ? T->Obj->Name : StringRef()), T->Index};
  W.write(REC_TPARAM_DECL, Ops);
  Ids[T] = NextType++;
}

uint64_t TypeWriter::type(const Type *T) {
  ++Depth;
  uint64_t Id = typeImpl(T);
  if (--Depth == 0) {
    // The bounds of the type parameters declared on the way.
    ++Depth;
    while (!PendingTParams.empty()) {
      const TypeParamType *TP = PendingTParams.front();
      PendingTParams.pop_front();
      // The uses of the parameter by the generic code (GEN-12): decided on
      // the type arguments of the importing package.
      std::vector<uint64_t> Ops{Ids[TP], type(TP->Bound), TP->uses().size()};
      for (const TypeParamType::Use &U : TP->uses()) {
        Ops.push_back(U.K);
        Ops.push_back(U.P);
        Ops.push_back(U.Taken | (U.Passed << 1));
        Ops.push_back(string(U.Member));
      }
      W.write(REC_TPARAM_DEF, Ops);
    }
    --Depth;
  }
  return Id;
}

uint64_t TypeWriter::typeImpl(const Type *T) {
  if (!T)
    return 0;
  if (auto It = Ids.find(T); It != Ids.end())
    return It->second;
  // Emits a record that makes a type, after its operands were written.
  auto Emit = [&](RecordCode C, const std::vector<uint64_t> &Ops) {
    W.write(C, Ops);
    return Ids[T] = NextType++;
  };
  switch (T->getKind()) {
  case Type::Kind::Basic: {
    auto *B = cast<BasicType>(T);
    StringRef N = B->Name;
    return Emit(REC_BASIC,
                {static_cast<uint64_t>(B->BK), N == "byte"   ? uint64_t(1)
                                               : N == "rune" ? uint64_t(2)
                                                             : uint64_t(0)});
  }
  case Type::Kind::Pointer:
    return Emit(REC_POINTER, {type(cast<PointerType>(T)->Elem)});
  case Type::Kind::Slice:
    return Emit(REC_SLICE, {type(cast<SliceType>(T)->Elem)});
  case Type::Kind::Ref:
    // A View[X] (0), or the borrowed []T of slicing (1, OWN-V2).
    return Emit(REC_REF, {type(cast<RefType>(T)->Elem),
                          cast<RefType>(T)->Borrowed ? uint64_t(1) : 0});
  case Type::Kind::Option:
    return Emit(REC_OPTION, {type(cast<OptionType>(T)->Elem)});
  case Type::Kind::Array: {
    auto *A = cast<ArrayType>(T);
    uint64_t E = type(A->Elem);
    return Emit(REC_ARRAY, {E, static_cast<uint64_t>(A->Len)});
  }
  case Type::Kind::Map: {
    auto *M = cast<MapType>(T);
    uint64_t K = type(M->Key), E = type(M->Elem);
    return Emit(REC_MAP, {K, E});
  }
  case Type::Kind::Chan: {
    auto *C = cast<ChanType>(T);
    uint64_t E = type(C->Elem);
    return Emit(REC_CHAN, {static_cast<uint64_t>(C->D), E});
  }
  case Type::Kind::Tuple: {
    std::vector<uint64_t> Ops{cast<TupleType>(T)->Elems.size()};
    for (Type *E : cast<TupleType>(T)->Elems)
      Ops.push_back(type(E));
    return Emit(REC_TUPLE, Ops);
  }
  case Type::Kind::Signature: {
    auto *S = cast<SignatureType>(T);
    std::vector<uint64_t> TPs;
    for (TypeParamType *TP : S->TParams)
      TPs.push_back(type(TP));
    uint64_t P = type(S->Params), R = type(S->Results);
    std::vector<uint64_t> Ops{P,        R,           S->Variadic,
                              S->Fails, S->MoveMask, TPs.size()};
    Ops.insert(Ops.end(), TPs.begin(), TPs.end());
    return Emit(REC_SIGNATURE, Ops);
  }
  case Type::Kind::Struct: {
    auto *St = cast<StructType>(T);
    // TYP-15a: the effective required bits, and the attributes as written
    // (`@Strict` on the struct, `@Required` and `@Option` on each field).
    std::vector<uint64_t> Ops{St->Fields.size(), St->Strict};
    for (const Var *F : St->Fields) {
      uint64_t Ty = type(F->Ty);
      Ops.push_back(string(F->Name));
      Ops.push_back(isExportedName(F->Name) ? 0 : package(F->Pkg));
      Ops.push_back(Ty);
      Ops.push_back(F->Embedded);
      Ops.push_back(F->IsParent);
      Ops.push_back(string(F->Tag));
      Ops.push_back(F->Required);
      Ops.push_back(F->RequiredSetOnly);
      Ops.push_back(F->MarkedRequired);
      Ops.push_back(F->MarkedOption);
    }
    return Emit(REC_STRUCT, Ops);
  }
  case Type::Kind::Interface: {
    auto *I = cast<InterfaceType>(T);
    if (I->IsAny) {
      uint64_t Ops[] = {string("any")};
      W.write(REC_UNIVERSE, Ops);
      return Ids[T] = NextType++;
    }
    std::vector<uint64_t> Ms, Es;
    for (const Func *M : I->Methods) {
      uint64_t Sig = type(M->Ty);
      Ms.push_back(string(M->Name));
      Ms.push_back(M->exported() ? 0 : package(M->Pkg));
      Ms.push_back(Sig);
      Ms.push_back(M->Fails);
    }
    for (Type *E : I->Embeddeds)
      Es.push_back(type(E));
    uint64_t Ext = I->Extends ? type(I->Extends) : 0; // GEN-12
    std::vector<uint64_t> Ops{I->Implicit, static_cast<uint64_t>(I->Sp),
                              I->Methods.size(), I->Embeddeds.size()};
    Ops.insert(Ops.end(), Ms.begin(), Ms.end());
    Ops.insert(Ops.end(), Es.begin(), Es.end());
    Ops.push_back(Ext);
    return Emit(REC_INTERFACE, Ops);
  }
  case Type::Kind::Union: {
    auto *U = cast<UnionType>(T);
    std::vector<uint64_t> Ops{U->Terms.size()};
    for (const Term &Tm : U->Terms) {
      uint64_t Ty = type(Tm.T);
      Ops.push_back(Tm.Tilde);
      Ops.push_back(Ty);
    }
    return Emit(REC_UNION, Ops);
  }
  case Type::Kind::TypeParam: {
    auto *TP = cast<TypeParamType>(T);
    declareTypeParam(TP);
    PendingTParams.push_back(TP);
    return Ids[T];
  }
  case Type::Kind::Named: {
    auto *N = cast<NamedType>(T);
    // The universe's types (error, comparable, Unique, ...) are named by
    // their name; an instance of one is an instance like any other.
    if (!N->Orig && N->Obj && !N->Obj->Pkg) {
      uint64_t Ops[] = {string(N->Obj->Name)};
      W.write(REC_UNIVERSE, Ops);
      return Ids[T] = NextType++;
    }
    if (N->Orig) {
      std::vector<uint64_t> Ops;
      uint64_t Orig = type(N->Orig);
      Ops.push_back(Orig);
      Ops.push_back(N->TArgs.size());
      for (Type *A : N->TArgs)
        Ops.push_back(type(A));
      return Emit(REC_NAMED_INST, Ops);
    }
    // State 3 (no zero value only through a type parameter) and the state of
    // a generic declaration are not exported: the importer works them out
    // again (the exported bytes must not depend on which queries ran).
    uint64_t Zero = N->ZeroState.load(std::memory_order_acquire);
    if (Zero == 3 || N->isGeneric())
      Zero = 0;
    uint64_t DeclOps[] = {
        package(N->Obj ? N->Obj->Pkg : nullptr),
        string(N->Obj ? N->Obj->Name : StringRef()),
        static_cast<uint64_t>(N->Ptr),
        uint64_t(N->Pinned) | uint64_t(N->ViewAttr) << 1 |
            uint64_t(N->AbstractAttr) << 2 |
            uint64_t(N->AbstractState.load(std::memory_order_acquire)) << 3,
        Zero};
    W.write(REC_NAMED_DECL, DeclOps);
    uint64_t Id = Ids[T] = NextType++;
    std::vector<uint64_t> Ops{Id, 0, N->TParams.size()};
    for (TypeParamType *TP : N->TParams)
      Ops.push_back(type(TP));
    Ops[1] = type(N->Underlying);
    size_t NumMethods = N->Methods ? N->Methods->size() : 0;
    Ops.push_back(NumMethods);
    for (size_t I = 0; I < NumMethods; ++I) {
      const Func *F = (*N->Methods)[I];
      uint64_t Sig = type(F->Ty);
      Ops.push_back(string(F->Name));
      Ops.push_back(Sig);
      Ops.push_back(F->HasPtrRecv | (F->MoveRecv << 1) | (F->Fails << 2) |
                    (F->IsBuiltinMethod << 3) | (uint64_t(F->MethodMark) << 4));
      Ops.push_back(F->RecvTParams.size());
      for (TypeParamType *TP : F->RecvTParams)
        Ops.push_back(type(TP));
      Ops.push_back(F->OwnTParams.size());
      for (TypeParamType *TP : F->OwnTParams)
        Ops.push_back(type(TP));
    }
    W.write(REC_NAMED_DEF, Ops);
    return Id;
  }
  }
  return 0;
}

void TypeWriter::constValue(const ConstValue &V, std::vector<uint64_t> &Ops) {
  Ops.push_back(V.kind());
  switch (V.kind()) {
  case ConstValue::Unknown:
    break;
  case ConstValue::Bool:
    Ops.push_back(V.boolVal());
    break;
  case ConstValue::String:
    Ops.push_back(string(V.stringVal()));
    break;
  case ConstValue::Int:
    putAPInt(V.intVal(), Ops);
    break;
  case ConstValue::Float:
    putReal(V.floatVal(), Ops);
    break;
  case ConstValue::Complex:
    putReal(V.floatVal(), Ops);
    putReal(V.imagVal(), Ops);
    break;
  }
}

void TypeWriter::object(const Object *O) {
  switch (O->getKind()) {
  case Object::Kind::Const: {
    auto *C = cast<Const>(O);
    uint64_t Ty = type(C->Ty);
    std::vector<uint64_t> Ops{string(C->Name), Ty, C->Val != nullptr};
    constValue(C->Val ? *C->Val : ConstValue(), Ops);
    W.write(REC_OBJ_CONST, Ops);
    break;
  }
  case Object::Kind::TypeName: {
    auto *TN = cast<TypeName>(O);
    std::vector<uint64_t> TPs;
    for (TypeParamType *TP : TN->AliasTParams)
      TPs.push_back(type(TP));
    uint64_t Ty = type(TN->Ty);
    std::vector<uint64_t> Ops{string(TN->Name), Ty, TN->IsAlias, TPs.size()};
    Ops.insert(Ops.end(), TPs.begin(), TPs.end());
    W.write(REC_OBJ_TYPE, Ops);
    break;
  }
  case Object::Kind::Var: {
    uint64_t Ty = type(O->Ty);
    uint64_t Ops[] = {string(O->Name), Ty};
    W.write(REC_OBJ_VAR, Ops);
    break;
  }
  case Object::Kind::Func: {
    auto *F = cast<Func>(O);
    uint64_t Sig = type(F->Ty);
    std::vector<uint64_t> Ops{string(F->Name), Sig, F->Fails, F->Params.size()};
    for (const Var *P : F->Params)
      Ops.push_back(string(P->Name));
    Ops.push_back(F->Results.size());
    for (const Var *R : F->Results)
      Ops.push_back(string(R->Name));
    W.write(REC_OBJ_FUNC, Ops);
    break;
  }
  default:
    break;
  }
}

/// The package-level objects of a package (only the exported ones if
/// OnlyExported), sorted by name.
static std::vector<const Object *> packageObjects(const Package &P,
                                                  bool OnlyExported) {
  std::vector<const Object *> Objs;
  if (P.PkgScope)
    for (auto &KV : P.PkgScope->Elems) {
      const Object *O = KV.second;
      switch (O->getKind()) {
      case Object::Kind::Const:
      case Object::Kind::TypeName:
      case Object::Kind::Var:
      case Object::Kind::Func:
        if (O->exported() || !OnlyExported)
          Objs.push_back(O);
        break;
      default:
        break;
      }
    }
  std::sort(Objs.begin(), Objs.end(),
            [](const Object *A, const Object *B) { return A->Name < B->Name; });
  return Objs;
}

void TypeWriter::writePackage(const Package &P, bool OnlyExported) {
  uint64_t Ops[] = {string(P.Name), string(P.Path)};
  W.write(REC_PKG_BEGIN, Ops);
  for (const Object *O : packageObjects(P, OnlyExported))
    object(O);
  W.write(REC_PKG_END, {});
}

//===----------------------------------------------------------------------===//
// Reader
//===----------------------------------------------------------------------===//

bool TypeReader::fail(const char *Msg) {
  if (Error.empty())
    Error = Msg;
  return false;
}

Type *TypeReader::type(uint64_t Id) const {
  return Id < Types.size() ? Types[Id] : nullptr;
}

const std::string *TypeReader::str(uint64_t Id) const {
  return Id < Strings.size() ? &Strings[Id] : nullptr;
}

bool TypeReader::constValue(const std::vector<uint64_t> &Ops, size_t &I,
                            ConstValue &V) {
  auto Next = [&](uint64_t &Out) {
    if (I >= Ops.size())
      return false;
    Out = Ops[I++];
    return true;
  };
  auto GetInt = [&](llvm::APInt &Out) {
    uint64_t Width, Words;
    if (!Next(Width) || !Next(Words) || Words > Ops.size() - I || Width == 0 ||
        Width > (1u << 24))
      return false;
    std::vector<uint64_t> W(Ops.begin() + I, Ops.begin() + I + Words);
    I += Words;
    if (W.size() != (Width + 63) / 64)
      return false;
    Out = llvm::APInt(static_cast<unsigned>(Width), W);
    return true;
  };
  // The invariants of Real and BigFloat (Constant.h) hold, or the value is
  // rejected: the arithmetic on constants relies on them.
  auto GetReal = [&](Real &Out) {
    uint64_t Rat;
    if (!Next(Rat) || Rat > 1)
      return false;
    if (Rat) {
      llvm::APInt N, D;
      if (!GetInt(N) || !GetInt(D) || D.isZero() || D.isNegative())
        return false;
      Out = Real::fromRat(N, D);
      return true;
    }
    uint64_t Neg, Exp;
    BigFloat F;
    if (!Next(Neg) || Neg > 1 || !Next(Exp) || !GetInt(F.Mant) ||
        F.Mant.getBitWidth() != BigFloat::Prec ||
        (!F.Mant.isZero() && !F.Mant.isSignBitSet()))
      return false;
    F.Neg = Neg;
    F.Exp = static_cast<int64_t>(Exp);
    Out = Real::fromBig(F);
    return true;
  };
  uint64_t K;
  if (!Next(K))
    return false;
  switch (K) {
  case ConstValue::Unknown:
    V = ConstValue();
    return true;
  case ConstValue::Bool: {
    uint64_t B;
    if (!Next(B) || B > 1)
      return false;
    V = ConstValue::makeBool(B);
    return true;
  }
  case ConstValue::String: {
    uint64_t S;
    if (!Next(S) || !str(S))
      return false;
    V = ConstValue::makeString(*str(S));
    return true;
  }
  case ConstValue::Int: {
    llvm::APInt A;
    if (!GetInt(A))
      return false;
    V = ConstValue::makeInt(A);
    return true;
  }
  case ConstValue::Float: {
    Real R;
    if (!GetReal(R))
      return false;
    V = ConstValue::makeFloat(R);
    return true;
  }
  case ConstValue::Complex: {
    Real Re, Im;
    if (!GetReal(Re) || !GetReal(Im))
      return false;
    V = ConstValue::makeComplex(Re, Im);
    return true;
  }
  }
  return false;
}

/// A type that can be the underlying type of a named type: not a name, a
/// type parameter or a tuple (null stands for an invalid declaration).
static bool isUnderlyingType(const Type *T) {
  return !T ||
         !(isa<NamedType>(T) || isa<TypeParamType>(T) || isa<TupleType>(T));
}

/// A signature whose flags agree with its tuples: a variadic one ends with a
/// slice parameter, a `!` one with an error result.
static bool isWellFormedSignature(TypeContext &Ctx, const SignatureType *S) {
  if (S->Variadic &&
      (S->Params->Elems.empty() || !isa<SliceType>(S->Params->Elems.back())))
    return false;
  if (S->Fails && (S->Results->Elems.empty() ||
                   S->Results->Elems.back() != Ctx.errorType()))
    return false;
  return true;
}

bool TypeReader::record(const Record &Rec) {
  const std::vector<uint64_t> &Ops = Rec.Ops;
  size_t I = 0;
  bool Bad = false;
  unsigned Depth = 0; // of the deepest type the record refers to
  auto Next = [&]() -> uint64_t {
    if (I >= Ops.size()) {
      Bad = true;
      return 0;
    }
    return Ops[I++];
  };
  auto Flag = [&]() -> bool {
    uint64_t F = Next();
    if (F > 1)
      Bad = true;
    return F;
  };
  auto Ty = [&]() -> Type * {
    uint64_t Id = Next();
    Type *T = type(Id);
    if (Id != 0 && !T)
      Bad = true;
    if (T)
      Depth = std::max(Depth, Depths[Id]);
    return T;
  };
  auto Str = [&]() -> StringRef {
    const std::string *S = str(Next());
    if (!S) {
      Bad = true;
      return {};
    }
    return *S;
  };
  auto Pk = [&]() -> Package * {
    uint64_t Id = Next();
    if (Id >= Pkgs.size())
      Bad = true;
    return Id < Pkgs.size() ? Pkgs[Id] : nullptr;
  };
  auto TParams = [&](SmallVector<TypeParamType *, 4> &Out) {
    uint64_t N = Next();
    for (uint64_t K = 0; K < N && !Bad; ++K) {
      auto *TP = dyn_cast_or_null<TypeParamType>(Ty());
      if (!TP)
        Bad = true;
      Out.push_back(TP);
    }
  };
  auto Produce = [&](Type *T) {
    if (Bad || !T)
      return fail("malformed type record");
    if (Depth + 1 > MaxTypeDepth)
      return fail("type nested too deeply");
    Types.push_back(T);
    Depths.push_back(Depth + 1);
    return true;
  };
  /// Declares O in the package scope; a name declared twice is an error.
  auto Declare = [&](Object *O) {
    O->Parent = Pkg->PkgScope;
    if (Pkg->PkgScope->insert(O))
      return fail("object declared twice");
    return true;
  };

  switch (Rec.Code) {
  case REC_STRING:
    Strings.push_back(Rec.Blob);
    return true;
  case REC_PKGREF: {
    StringRef Path = Str();
    StringRef Name = Str();
    if (Bad)
      return fail("malformed package reference");
    Package *P = Pkg && Pkg->Path == Path ? Pkg : PR.resolve(Path);
    if (!P) {
      Error = "unknown package " + Path.str();
      return false;
    }
    (void)Name;
    Pkgs.push_back(P);
    return true;
  }
  case REC_BASIC: {
    uint64_t K = Next(), Alias = Next();
    if (Bad || K >= static_cast<uint64_t>(BasicKind::NumBasicKinds) ||
        Alias > 2)
      return fail("malformed basic type");
    return Produce(Alias == 1   ? Ctx.byteType()
                   : Alias == 2 ? Ctx.runeType()
                                : Ctx.typ(static_cast<BasicKind>(K)));
  }
  case REC_UNIVERSE: {
    StringRef N = Str();
    Object *O = Bad ? nullptr : Ctx.universe()->lookupLocal(N);
    return Produce(O && isa<TypeName>(O) ? O->Ty : nullptr);
  }
  case REC_POINTER: {
    Type *E = Ty();
    return Produce(E ? Ctx.pointer(E) : nullptr);
  }
  case REC_SLICE: {
    Type *E = Ty();
    return Produce(E ? Ctx.slice(E) : nullptr);
  }
  case REC_REF: {
    Type *E = Ty();
    uint64_t Borrowed = Next();
    return Produce(E && Borrowed <= 1 ? Ctx.ref(E, Borrowed == 1) : nullptr);
  }
  case REC_OPTION: {
    Type *E = Ty();
    return Produce(E ? Ctx.option(E) : nullptr);
  }
  case REC_ARRAY: {
    Type *E = Ty();
    int64_t Len = static_cast<int64_t>(Next());
    return Produce(E && Len >= -1 ? Ctx.array(E, Len) : nullptr);
  }
  case REC_MAP: {
    Type *K = Ty(), *E = Ty();
    return Produce(K && E ? Ctx.map(K, E) : nullptr);
  }
  case REC_CHAN: {
    uint64_t D = Next();
    Type *E = Ty();
    return Produce(E && D >= 1 && D <= 3
                       ? Ctx.chan(static_cast<ChanType::Dir>(D), E)
                       : nullptr);
  }
  case REC_TUPLE: {
    uint64_t N = Next();
    SmallVector<Type *, 8> Es;
    for (uint64_t K = 0; K < N && !Bad; ++K) {
      Es.push_back(Ty());
      if (!Es.back())
        Bad = true;
    }
    return Produce(Bad ? nullptr : Ctx.tuple(Es));
  }
  case REC_SIGNATURE: {
    auto *P = dyn_cast_or_null<TupleType>(Ty());
    auto *R = dyn_cast_or_null<TupleType>(Ty());
    bool Variadic = Flag(), Fails = Flag();
    uint64_t Mask = Next();
    SmallVector<TypeParamType *, 4> TPs;
    TParams(TPs);
    if (!P || !R || Bad)
      return fail("malformed signature");
    SignatureType *S = Ctx.signature(P, R, Variadic, Fails, Mask, TPs);
    if (!isWellFormedSignature(Ctx, S))
      return fail("malformed signature");
    return Produce(S);
  }
  case REC_STRUCT: {
    uint64_t N = Next();
    bool Strict = Flag();
    std::vector<TypeContext::FieldSpec> Fs;
    for (uint64_t K = 0; K < N && !Bad; ++K) {
      TypeContext::FieldSpec F;
      F.Name = Str();
      F.Pkg = Pk();
      F.Ty = Ty();
      F.Embedded = Flag();
      F.IsParent = Flag();
      F.Tag = Str();
      F.Required = Flag();
      F.RequiredSetOnly = Flag();
      F.MarkedRequired = Flag();
      F.MarkedOption = Flag();
      if (!F.Ty)
        Bad = true;
      // The parent part is the first field and an embedded one (TYP-16).
      if (F.IsParent && (K != 0 || !F.Embedded))
        Bad = true;
      Fs.push_back(F);
    }
    return Produce(Bad ? nullptr : Ctx.structOf(Fs, Strict));
  }
  case REC_INTERFACE: {
    bool Implicit = Flag();
    uint64_t Special = Next(), NM = Next(), NE = Next();
    SmallVector<Func *, 8> Ms;
    for (uint64_t K = 0; K < NM && !Bad; ++K) {
      Func *F = Mem.make<Func>();
      F->Name = Mem.copy(Str());
      F->Pkg = Pk();
      F->Ty = Ty();
      F->Fails = Flag();
      F->IsInterfaceMethod = true;
      if (!isa_and_nonnull<SignatureType>(F->Ty))
        Bad = true;
      Ms.push_back(F);
    }
    SmallVector<Type *, 4> Es;
    for (uint64_t K = 0; K < NE && !Bad; ++K) {
      Es.push_back(Ty());
      if (!Es.back())
        Bad = true;
    }
    if (Bad || Special > InterfaceType::Clone)
      return fail("malformed interface");
    std::vector<SourceLoc> Locs(Es.size());
    Type *Ext = Ty(); // GEN-12; null if there is none
    if (Bad)
      return fail("malformed interface");
    InterfaceType *It = Ctx.newInterface(Ms, Es, Locs, Implicit);
    It->Sp = static_cast<InterfaceType::Special>(Special);
    It->Extends = Ext;
    return Produce(It);
  }
  case REC_UNION: {
    uint64_t N = Next();
    std::vector<Term> Ts;
    for (uint64_t K = 0; K < N && !Bad; ++K) {
      Term T;
      T.Tilde = Flag();
      T.T = Ty();
      if (!T.T)
        Bad = true;
      Ts.push_back(T);
    }
    return Produce(Bad ? nullptr : Ctx.newUnion(Ts));
  }
  case REC_NAMED_DECL: {
    Package *P = Pk();
    StringRef Name = Str();
    uint64_t Ptr = Next();
    uint64_t Flags =
        Next(); // bit 0 @Pinned, 1 @View, 2 @Abstract, 3-4 abstract
    uint64_t Zero = Next();
    if (Bad || Ptr > static_cast<uint64_t>(PtrKind::Weak) || Flags > 31 ||
        (Flags >> 3) > 3 || Zero > 2)
      return fail("malformed named type");
    TypeName *TN = Mem.make<TypeName>();
    TN->Name = Mem.copy(Name);
    TN->Pkg = P;
    NamedType *N = Ctx.newNamed(TN);
    N->Ptr = static_cast<PtrKind>(Ptr);
    N->Pinned = Flags & 1;
    N->ViewAttr = Flags & 2;
    N->AbstractAttr = Flags & 4;
    N->AbstractState.store(static_cast<uint8_t>(Flags >> 3),
                           std::memory_order_release);
    N->ZeroState.store(static_cast<uint8_t>(Zero), std::memory_order_release);
    Undefined.insert(N);
    return Produce(N);
  }
  case REC_NAMED_DEF: {
    auto *N = dyn_cast_or_null<NamedType>(Ty());
    Type *Under = Ty();
    SmallVector<TypeParamType *, 4> TPs;
    TParams(TPs);
    // Only a type this stream declared, once: a corrupt stream must not
    // change a type of the universe or of another package.
    if (!N || Bad || !Undefined.erase(N) || !isUnderlyingType(Under))
      return fail("malformed named type definition");
    N->TParams = Mem.copy(ArrayRef<TypeParamType *>(TPs));
    N->Underlying = Under;
    uint64_t NM = Next();
    N->Methods = Ctx.newMethodList();
    for (uint64_t K = 0; K < NM && !Bad; ++K) {
      Func *F = Mem.make<Func>();
      F->Name = Mem.copy(Str());
      F->Pkg = N->Obj->Pkg;
      F->Ty = Ty();
      uint64_t Flags = Next();
      F->HasPtrRecv = Flags & 1;
      F->MoveRecv = Flags & 2;
      F->Fails = Flags & 4;
      F->IsBuiltinMethod = Flags & 8;
      F->MethodMark = static_cast<Func::Mark>((Flags >> 4) & 3);
      SmallVector<TypeParamType *, 4> RTPs, OTPs;
      TParams(RTPs);
      TParams(OTPs);
      F->RecvTParams = Mem.copy(ArrayRef<TypeParamType *>(RTPs));
      F->OwnTParams = Mem.copy(ArrayRef<TypeParamType *>(OTPs));
      if (Flags > 63 || !isa_and_nonnull<SignatureType>(F->Ty)) {
        Bad = true;
        break;
      }
      Var *Recv = Mem.make<Var>();
      Recv->Pkg = F->Pkg;
      Recv->IsParam = true;
      Recv->Ty = F->HasPtrRecv ? static_cast<Type *>(Ctx.pointer(N)) : N;
      F->Recv = Recv;
      Ctx.noteMemberName(F->Name);
      N->Methods->push_back(F);
    }
    if (Bad)
      return fail("malformed method list");
    return true;
  }
  case REC_NAMED_INST: {
    auto *O = dyn_cast_or_null<NamedType>(Ty());
    uint64_t N = Next();
    SmallVector<Type *, 4> As;
    for (uint64_t K = 0; K < N && !Bad; ++K) {
      As.push_back(Ty());
      if (!As.back())
        Bad = true;
    }
    // The origin is a generic type, not an instance itself.
    return Produce(O && !O->Orig && !Bad ? Ctx.instance(O, As) : nullptr);
  }
  case REC_TPARAM_DECL: {
    StringRef Name = Str();
    uint64_t Index = Next();
    if (Bad || Index > UINT32_MAX)
      return fail("malformed type parameter");
    TypeName *TN = Mem.make<TypeName>();
    TN->Name = Mem.copy(Name);
    TN->Pkg = Pkg;
    TypeParamType *TP =
        Ctx.newTypeParam(TN, static_cast<unsigned>(Index), nullptr);
    Undefined.insert(TP);
    return Produce(TP);
  }
  case REC_TPARAM_DEF: {
    auto *TP = dyn_cast_or_null<TypeParamType>(Ty());
    Type *Bound = Ty();
    uint64_t NUses = Next();
    std::vector<TypeParamType::Use> Uses;
    for (uint64_t K = 0; K < NUses && !Bad; ++K) {
      uint64_t Kind = Next(), Place = Next(), Flags = Next();
      StringRef Member = Str();
      if (Kind > TypeParamType::Use::Method ||
          Place > TypeParamType::Use::Pointer || Flags > 3) {
        Bad = true;
        break;
      }
      TypeParamType::Use U;
      U.K = static_cast<TypeParamType::Use::Kind>(Kind);
      U.P = static_cast<TypeParamType::Use::Place>(Place);
      U.Taken = Flags & 1;
      U.Passed = Flags & 2;
      U.Member = Member;
      Uses.push_back(U);
    }
    if (!TP || Bad || !Undefined.erase(TP) ||
        !std::is_sorted(Uses.begin(), Uses.end()) ||
        std::adjacent_find(Uses.begin(), Uses.end()) != Uses.end())
      return fail("malformed type parameter definition");
    TP->Bound = Bound;
    Ctx.setTParamUses(TP, Uses);
    return true;
  }
  case REC_PKG_BEGIN: {
    StringRef Name = Str(), Path = Str();
    if (Bad || Pkg)
      return fail("malformed package header");
    Pkg = Mem.make<Package>();
    Pkg->Name = Mem.copy(Name);
    Pkg->Path = Mem.copy(Path);
    Pkg->PkgScope = Mem.newScope(Ctx.universe(), {}, "package");
    return true;
  }
  case REC_OBJ_CONST: {
    if (!Pkg)
      return fail("object before the package header");
    StringRef Name = Str();
    Type *T = Ty();
    bool HasValue = Flag();
    ConstValue V;
    if (Bad || !constValue(Ops, I, V))
      return fail("malformed constant");
    Const *C = Mem.make<Const>();
    C->Name = Mem.copy(Name);
    C->Pkg = Pkg;
    C->Ty = T;
    C->Val = HasValue ? Mem.constant(std::move(V)) : nullptr;
    return Declare(C);
  }
  case REC_OBJ_TYPE: {
    if (!Pkg)
      return fail("object before the package header");
    StringRef Name = Str();
    Type *T = Ty();
    bool Alias = Flag();
    SmallVector<TypeParamType *, 4> TPs;
    TParams(TPs);
    if (Bad)
      return fail("malformed type declaration");
    // A defined type of this package is declared by its own object; any
    // other type (an alias, an invalid declaration) gets a new one.
    TypeName *TN = nullptr;
    if (auto *N = dyn_cast_or_null<NamedType>(T);
        !Alias && N && N->Obj && !N->Orig && N->Obj->Pkg == Pkg &&
        N->Obj->Name == Name && !N->Obj->Parent)
      TN = N->Obj;
    if (!TN) {
      TN = Mem.make<TypeName>();
      TN->Name = Mem.copy(Name);
      TN->Pkg = Pkg;
      TN->Ty = T;
      TN->IsAlias = Alias;
      TN->AliasTParams = Mem.copy(ArrayRef<TypeParamType *>(TPs));
    }
    return Declare(TN);
  }
  case REC_OBJ_VAR: {
    if (!Pkg)
      return fail("object before the package header");
    StringRef Name = Str();
    Type *T = Ty();
    if (Bad)
      return fail("malformed variable");
    Var *V = Mem.make<Var>();
    V->Name = Mem.copy(Name);
    V->Pkg = Pkg;
    V->Ty = T;
    V->IsPackageLevel = true;
    V->Used = true;
    return Declare(V);
  }
  case REC_OBJ_FUNC: {
    if (!Pkg)
      return fail("object before the package header");
    StringRef Name = Str();
    auto *Sig = dyn_cast_or_null<SignatureType>(Ty());
    bool Fails = Flag();
    if (Bad || !Sig)
      return fail("malformed function");
    Func *F = Mem.make<Func>();
    F->Name = Mem.copy(Name);
    F->Pkg = Pkg;
    F->Ty = Sig;
    F->Fails = Fails;
    F->OwnTParams = Sig->TParams;
    // One name per element of the tuple, but for the implicit error result
    // of a `!` function.
    auto Vars = [&](TupleType *Tup, bool Result) {
      uint64_t N = Next();
      SmallVector<Var *, 8> Vs;
      if (N != Tup->size() - (Result && Sig->Fails ? 1 : 0))
        Bad = true;
      for (uint64_t K = 0; K < N && !Bad; ++K) {
        Var *V = Mem.make<Var>();
        V->Name = Mem.copy(Str());
        V->Pkg = Pkg;
        V->Ty = Tup->Elems[K];
        V->IsParam = true;
        V->Used = true;
        V->Result = Result;
        Vs.push_back(V);
      }
      return Mem.copy(ArrayRef<Var *>(Vs));
    };
    F->Params = Vars(Sig->Params, false);
    F->Results = Vars(Sig->Results, true);
    if (Bad)
      return fail("malformed function parameters");
    return Declare(F);
  }
  default:
    return fail("unknown record code");
  }
}

Package *TypeReader::readPackage(std::string &Err) {
  Record Rec;
  bool Done = false;
  while (!Done && R.read(Rec)) {
    if (Rec.Code == REC_PKG_END) {
      Done = true;
      break;
    }
    if (!record(Rec)) {
      Err = Error;
      return nullptr;
    }
  }
  if (!Done || !Pkg) {
    Err = Error.empty() ? "truncated stream" : Error;
    return nullptr;
  }
  // Every declared named type and type parameter got its definition.
  if (!Undefined.empty()) {
    Err = "a named type or type parameter has no definition";
    return nullptr;
  }
  Pkg->Complete = true;
  return Pkg;
}

//===----------------------------------------------------------------------===//
// Dump
//===----------------------------------------------------------------------===//

void sema::dumpPackageAPI(llvm::raw_ostream &OS, const Package &P,
                          bool OnlyExported) {
  auto Sig = [&](const Type *T) {
    std::string S = typeString(T, &P);
    return S.substr(S.rfind("func", 0) == 0 ? 4 : 0);
  };
  for (const Object *O : packageObjects(P, OnlyExported)) {
    switch (O->getKind()) {
    case Object::Kind::Const: {
      auto *C = cast<Const>(O);
      OS << "const " << O->Name << ' ' << typeString(O->Ty, &P) << " = "
         << (C->Val ? C->Val->exactStr() : "?") << '\n';
      break;
    }
    case Object::Kind::Var:
      OS << "var " << O->Name << ' ' << typeString(O->Ty, &P) << '\n';
      break;
    case Object::Kind::Func:
      OS << "func " << O->Name << Sig(O->Ty) << '\n';
      break;
    case Object::Kind::TypeName: {
      auto *TN = cast<TypeName>(O);
      auto *N = dyn_cast_or_null<NamedType>(O->Ty);
      if (TN->IsAlias || !N || N->Obj != TN) {
        // An alias, or a declaration whose type is not its own (invalid).
        OS << "type " << O->Name;
        if (!TN->AliasTParams.empty()) {
          std::string TPs;
          for (const TypeParamType *TP : TN->AliasTParams)
            TPs += (TPs.empty() ? "" : ", ") +
                   (TP && TP->Obj ? std::string(TP->Obj->Name) : "?");
          OS << '[' << TPs << ']';
        }
        OS << (TN->IsAlias ? " = " : " ") << typeString(O->Ty, &P) << '\n';
        break;
      }
      OS << (N->Pinned ? "@Pinned " : "") << "type " << typeString(N, &P) << ' '
         << typeString(N->Underlying, &P) << '\n';
      if (N->Methods)
        for (const Func *M : *N->Methods)
          OS << "func (" << (M->HasPtrRecv ? "*" : "") << typeString(N, &P)
             << ") " << (M->MoveRecv ? "move " : "") << M->Name << Sig(M->Ty)
             << '\n';
      break;
    }
    default:
      break;
    }
  }
}
