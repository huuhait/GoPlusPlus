//===- Predicates.cpp - Type predicates, identity -------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's predicates.go, under.go and typeterm.go (BSD-style
// license, Copyright The Go Authors).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

Type *Checker::under(Type *T) {
  if (!T)
    return invalid();
  for (unsigned Depth = 0; Depth < 100; ++Depth) {
    switch (T->getKind()) {
    case Type::Kind::Named: {
      auto *N = cast<NamedType>(T);
      if (N->Orig && !N->Expanded)
        expandNamed(N);
      if (!N->Underlying) {
        if (!N->DirectRhs)
          return invalid();
        T = N->DirectRhs;
        continue;
      }
      T = N->Underlying;
      continue;
    }
    case Type::Kind::TypeParam: {
      Type *B = cast<TypeParamType>(T)->Bound;
      if (!B)
        return Ctx.anyType();
      T = B;
      if (isa<TypeParamType>(T))
        return Ctx.anyType();
      continue;
    }
    default:
      return T;
    }
  }
  return invalid();
}

bool Checker::isBasic(Type *T, unsigned Info) {
  auto *B = dyn_cast<BasicType>(under(T));
  return B && (B->Info & Info) != 0;
}

bool Checker::underIs(Type *T, const std::function<bool(Type *)> &F) {
  if (auto *P = dyn_cast<TypeParamType>(T)) {
    TypeSet *TS = typeSetOf(P);
    if (!TS || TS->AllTerms)
      return F(nullptr);
    if (TS->Terms.empty())
      return F(nullptr);
    for (const Term &Tm : TS->Terms)
      if (!F(under(Tm.T)))
        return false;
    return true;
  }
  return F(under(T));
}

bool Checker::typeParamIs(TypeParamType *P,
                          const std::function<bool(const Term *)> &F) {
  TypeSet *TS = typeSetOf(P);
  if (!TS || TS->AllTerms || TS->Terms.empty())
    return F(nullptr);
  for (const Term &Tm : TS->Terms)
    if (!F(&Tm))
      return false;
  return true;
}

Type *
Checker::commonUnder(Type *T,
                     const std::function<std::string(Type *, Type *)> &Cond,
                     std::string *Err) {
  Type *CT = nullptr, *CU = nullptr;
  std::string Msg;
  auto Visit = [&](Type *Tt, Type *U) {
    if (Cond) {
      Msg = Cond(Tt, U);
      if (!Msg.empty())
        return false;
    }
    if (!U) {
      Msg = "no specific type";
      return false;
    }
    if (!CU) {
      CT = Tt;
      CU = U;
      return true;
    }
    if (auto *C1 = dyn_cast<ChanType>(CU))
      if (auto *C2 = dyn_cast<ChanType>(U)) {
        if (!identical(C1->Elem, C2->Elem)) {
          Msg = "channels " + str(CT) + " and " + str(Tt) +
                " have different element types";
          return false;
        }
        if (C1->D == C2->D)
          return true;
        if (C1->D == ChanType::Both) {
          CT = Tt;
          CU = U;
          return true;
        }
        if (C2->D != ChanType::Both) {
          Msg = "channels " + str(CT) + " and " + str(Tt) +
                " have conflicting directions";
          return false;
        }
        return true;
      }
    if (!identical(CU, U)) {
      Msg = str(CT) + " and " + str(Tt) + " have different underlying types";
      return false;
    }
    return true;
  };
  bool Ok;
  if (auto *P = dyn_cast<TypeParamType>(T))
    Ok = typeParamIs(P, [&](const Term *Tm) {
      return Tm ? Visit(Tm->T, under(Tm->T)) : Visit(nullptr, nullptr);
    });
  else
    Ok = Visit(T, under(T));
  if (!Ok) {
    if (Err)
      *Err = Msg;
    return nullptr;
  }
  return CU;
}

Type *Checker::chanElem(SourceLoc Pos, const Operand &X, bool Recv) {
  std::string Cause;
  Type *U = commonUnder(
      X.Ty,
      [&](Type *T, Type *U) -> std::string {
        if (!U)
          return "no specific channel type";
        auto *Ch = dyn_cast<ChanType>(U);
        if (!Ch)
          return "non-channel " + str(T);
        if (Recv && Ch->D == ChanType::Send)
          return "send-only channel " + str(T);
        if (!Recv && Ch->D == ChanType::Recv)
          return "receive-only channel " + str(T);
        return "";
      },
      &Cause);
  if (U)
    return cast<ChanType>(U)->Elem;
  std::string Verb = Recv ? "invalid operation: cannot receive from "
                          : "invalid operation: cannot send to ";
  if (isTypeParam(X.Ty))
    err(Pos, diag::err_sema_predicate, {Verb + str(X) + ": " + Cause});
  else
    err(Pos, diag::err_sema_predicate, {Verb + Cause + " " + str(X)});
  return nullptr;
}

Type *Checker::sliceElem(const Operand &X, std::string &Err) {
  Type *Elem = nullptr;
  bool Ok = underIs(X.Ty, [&](Type *U) {
    if (auto *Rf = U ? dyn_cast<RefType>(U) : nullptr)
      U = under(Rf->Elem); // Go++: ref []T
    auto *Sl = U ? dyn_cast<SliceType>(U) : nullptr;
    if (!Sl) {
      Err = X.isNil() ? "argument must be a slice; have untyped nil"
                      : "argument must be a slice; have " + str(X);
      return false;
    }
    if (!Elem) {
      Elem = Sl->Elem;
    } else if (!identical(Elem, Sl->Elem)) {
      Err = "mismatched slice element types " + str(Elem) + " and " +
            str(Sl->Elem) + " in " + str(X);
      return false;
    }
    return true;
  });
  return Ok ? Elem : nullptr;
}

bool Checker::allBasic(Type *T, unsigned Info) {
  if (auto *P = dyn_cast<TypeParamType>(T))
    return underIs(P, [&](Type *U) {
      auto *B = U ? dyn_cast<BasicType>(U) : nullptr;
      return B && (B->Info & Info) != 0;
    });
  return isBasic(T, Info);
}

bool Checker::isTyped(Type *T) {
  auto *B = dyn_cast<BasicType>(T);
  return !B || !(B->Info & IsUntyped);
}

bool Checker::isUntypedNumeric(Type *T) {
  auto *B = dyn_cast<BasicType>(T);
  return B && (B->Info & IsUntyped) && (B->Info & IsNumeric);
}

bool Checker::isNamed(const Type *T) const {
  return isa<BasicType>(T) || isa<NamedType>(T) || isa<TypeParamType>(T);
}
bool Checker::hasName(const Type *T) const { return isNamed(T); }

bool Checker::isInterface(Type *T) { return isa<InterfaceType>(under(T)); }
bool Checker::isNonTypeParamInterface(Type *T) {
  return !isTypeParam(T) && isInterface(T);
}
bool Checker::isInterfacePtr(Type *T) {
  auto *P = dyn_cast<PointerType>(under(T));
  return P && isInterface(P->Elem);
}
bool Checker::isPointer(Type *T) { return isa<PointerType>(under(T)); }
bool Checker::isUnsafePointer(Type *T) {
  auto *B = dyn_cast<BasicType>(under(T));
  return B && B->BK == BasicKind::UnsafePointer;
}
bool Checker::isUintptr(Type *T) {
  auto *B = dyn_cast<BasicType>(under(T));
  return B && B->BK == BasicKind::Uintptr;
}
bool Checker::isBytesOrRunes(Type *T) {
  auto *S = dyn_cast<SliceType>(under(T));
  if (!S)
    return false;
  auto *B = dyn_cast<BasicType>(under(S->Elem));
  return B && (B->BK == BasicKind::Uint8 || B->BK == BasicKind::Int32);
}
bool Checker::isGeneric(Type *T) {
  auto *N = dyn_cast<NamedType>(T);
  return N && N->isGeneric();
}
bool Checker::isEmptyInterface(Type *T) {
  auto *I = dyn_cast<InterfaceType>(under(T));
  if (!I)
    return false;
  return typeSet(I)->isAll();
}

NamedType *Checker::asNamed(Type *T) { return dyn_cast<NamedType>(T); }

Type *Checker::deref(Type *T, bool &IsPtr) {
  if (auto *P = dyn_cast<PointerType>(T)) {
    IsPtr = true;
    return P->Elem;
  }
  IsPtr = false;
  return T;
}

Type *Checker::derefStructPtr(Type *T) {
  if (auto *P = dyn_cast<PointerType>(under(T)))
    if (isa<StructType>(under(P->Elem)))
      return P->Elem;
  return T;
}

Type *Checker::defaultType(Type *T) {
  if (auto *B = dyn_cast<BasicType>(T)) {
    switch (B->BK) {
    case BasicKind::UntypedBool:
      return typ(BasicKind::Bool);
    case BasicKind::UntypedInt:
      return typ(BasicKind::Int);
    case BasicKind::UntypedRune:
      return Ctx.runeType();
    case BasicKind::UntypedFloat:
      return typ(BasicKind::Float64);
    case BasicKind::UntypedComplex:
      return typ(BasicKind::Complex128);
    case BasicKind::UntypedString:
      return typ(BasicKind::String);
    default:
      break;
    }
  }
  return T;
}

Type *Checker::maxType(Type *X, Type *Y) {
  if (X == Y)
    return X;
  if (isUntypedNumeric(X) && isUntypedNumeric(Y))
    return static_cast<unsigned>(cast<BasicType>(X)->BK) >
                   static_cast<unsigned>(cast<BasicType>(Y)->BK)
               ? X
               : Y;
  return nullptr;
}

bool Checker::hasNil(Type *T) {
  if (own()) {
    // ADR-0018: pointers are never nil; Option[T] is the "maybe".
    if (isPointerKind(T))
      return false;
    if (isa<OptionType>(T))
      return true;
  } else if (auto *N = dyn_cast<NamedType>(T)) {
    if (N->origin()->Ptr != PtrKind::None)
      return true;
  }
  Type *U = under(T);
  switch (U->getKind()) {
  case Type::Kind::Basic:
    return cast<BasicType>(U)->BK == BasicKind::UnsafePointer;
  case Type::Kind::Option:
    return own();
  case Type::Kind::Pointer:
    return !own();
  case Type::Kind::Slice:
  case Type::Kind::Signature:
  case Type::Kind::Map:
  case Type::Kind::Chan:
    return true;
  case Type::Kind::Interface:
    if (!isTypeParam(T))
      return true;
    return underIs(T, [&](Type *V) { return V && hasNil(V); });
  default:
    return false;
  }
}

bool Checker::isPointerKind(Type *T) {
  if (auto *N = dyn_cast<NamedType>(T))
    if (N->origin()->Ptr != PtrKind::None)
      return true;
  return isa<PointerType>(under(T));
}

bool Checker::comparableType(Type *T, bool Dynamic,
                             SmallVector<const Type *, 4> *Seen,
                             std::string *Cause) {
  SmallVector<const Type *, 4> Local;
  if (!Seen)
    Seen = &Local;
  DepthGuard DG(RecDepth);
  if (RecDepth > MaxRecDepth)
    return true;
  for (const Type *S : *Seen)
    if (S == T)
      return true;
  Seen->push_back(T);
  Type *U = under(T);
  switch (U->getKind()) {
  case Type::Kind::Basic:
    return cast<BasicType>(U)->BK != BasicKind::UntypedNil;
  case Type::Kind::Pointer:
  case Type::Kind::Chan:
  case Type::Kind::Ref:
    return true;
  case Type::Kind::Option:
    // ADR-0018: Option[T] compares like T (both nil, or both set and equal).
    if (!comparableType(cast<OptionType>(U)->Elem, Dynamic, Seen)) {
      if (Cause)
        *Cause = str(T) +
                 " cannot be compared: " + str(cast<OptionType>(U)->Elem) +
                 " is not comparable";
      return false;
    }
    return true;
  case Type::Kind::Struct:
    for (Var *F : cast<StructType>(U)->Fields)
      if (!comparableType(F->Ty, Dynamic, Seen)) {
        if (Cause)
          *Cause = "struct containing " + str(F->Ty) + " cannot be compared";
        return false;
      }
    return true;
  case Type::Kind::Array:
    if (!comparableType(cast<ArrayType>(U)->Elem, Dynamic, Seen)) {
      if (Cause)
        *Cause = str(T) + " cannot be compared";
      return false;
    }
    return true;
  case Type::Kind::Interface: {
    auto *I = cast<InterfaceType>(U);
    if ((Dynamic && !isTypeParam(T)) || typeSetIsComparable(typeSet(I)))
      return true;
    if (Cause)
      *Cause = typeSet(I)->isEmpty() ? "empty type set"
                                     : "incomparable types in type set";
    return false;
  }
  default:
    return false;
  }
}

//===----------------------------------------------------------------------===//
// Identity
//===----------------------------------------------------------------------===//

namespace {
struct IdState {
  bool IgnoreTags;
  unsigned Depth = 0;
};
} // namespace

bool Checker::identical(const Type *X, const Type *Y) {
  return identicalImpl(X, Y, false, nullptr);
}
bool Checker::identicalIgnoreTags(const Type *X, const Type *Y) {
  return identicalImpl(X, Y, true, nullptr);
}

bool Checker::identicalImpl(const Type *X0, const Type *Y0, bool IgnoreTags,
                            void *Seen) {
  if (X0 == Y0)
    return true;
  if (!X0 || !Y0)
    return false;
  Type *X = const_cast<Type *>(X0), *Y = const_cast<Type *>(Y0);
  if (X->getKind() != Y->getKind())
    return false;
  auto Rec = [&](const Type *A, const Type *B) {
    return identicalImpl(A, B, IgnoreTags, Seen);
  };
  switch (X->getKind()) {
  case Type::Kind::Basic:
    return cast<BasicType>(X)->BK == cast<BasicType>(Y)->BK;
  case Type::Kind::TypeParam:
    return false;
  case Type::Kind::Array: {
    auto *A = cast<ArrayType>(X), *B = cast<ArrayType>(Y);
    return A->Len >= 0 && A->Len == B->Len && Rec(A->Elem, B->Elem);
  }
  case Type::Kind::Slice:
    return Rec(cast<SliceType>(X)->Elem, cast<SliceType>(Y)->Elem);
  case Type::Kind::Ref:
    return Rec(cast<RefType>(X)->Elem, cast<RefType>(Y)->Elem);
  case Type::Kind::Option:
    return Rec(cast<OptionType>(X)->Elem, cast<OptionType>(Y)->Elem);
  case Type::Kind::Pointer:
    return Rec(cast<PointerType>(X)->Elem, cast<PointerType>(Y)->Elem);
  case Type::Kind::Map: {
    auto *A = cast<MapType>(X), *B = cast<MapType>(Y);
    return Rec(A->Key, B->Key) && Rec(A->Elem, B->Elem);
  }
  case Type::Kind::Chan: {
    auto *A = cast<ChanType>(X), *B = cast<ChanType>(Y);
    return A->D == B->D && Rec(A->Elem, B->Elem);
  }
  case Type::Kind::Tuple: {
    auto *A = cast<TupleType>(X), *B = cast<TupleType>(Y);
    if (A->size() != B->size())
      return false;
    for (size_t I = 0; I < A->size(); ++I)
      if (!Rec(A->Elems[I], B->Elems[I]))
        return false;
    return true;
  }
  case Type::Kind::Signature: {
    auto *A = cast<SignatureType>(X), *B = cast<SignatureType>(Y);
    if (A->Variadic != B->Variadic || A->MoveMask != B->MoveMask ||
        A->TParams.size() != B->TParams.size())
      return false;
    if (!A->TParams.empty()) {
      // Generic signatures: identical up to renaming of the type parameters.
      auto Sub = [&](SignatureType *S, ArrayRef<TypeParamType *> From,
                     ArrayRef<TypeParamType *> To) {
        SmallVector<Type *, 4> ToT(To.begin(), To.end());
        return subst(S, From, ToT);
      };
      Type *A2 = Sub(A, A->TParams, B->TParams);
      auto *AS = cast<SignatureType>(A2);
      return Rec(AS->Params, B->Params) && Rec(AS->Results, B->Results);
    }
    return Rec(A->Params, B->Params) && Rec(A->Results, B->Results);
  }
  case Type::Kind::Struct: {
    auto *A = cast<StructType>(X), *B = cast<StructType>(Y);
    if (A->Fields.size() != B->Fields.size())
      return false;
    for (size_t I = 0; I < A->Fields.size(); ++I) {
      Var *F = A->Fields[I], *G = B->Fields[I];
      // `@Required` is part of the identity even where tags are ignored
      // (conversions): `User(struct{ID int}{})` would skip the field.
      if (F->Embedded != G->Embedded || F->Required != G->Required ||
          (!IgnoreTags && F->Tag != G->Tag) || F->Name != G->Name ||
          (!isExportedName(F->Name) && F->Pkg != G->Pkg) || !Rec(F->Ty, G->Ty))
        return false;
    }
    return true;
  }
  case Type::Kind::Union: {
    auto *A = cast<UnionType>(X), *B = cast<UnionType>(Y);
    if (A->Terms.size() != B->Terms.size())
      return false;
    for (size_t I = 0; I < A->Terms.size(); ++I)
      if (A->Terms[I].Tilde != B->Terms[I].Tilde ||
          !Rec(A->Terms[I].T, B->Terms[I].T))
        return false;
    return true;
  }
  case Type::Kind::Interface: {
    auto *A = cast<InterfaceType>(X), *B = cast<InterfaceType>(Y);
    // Recursive interfaces: a pair already being compared is assumed equal.
    using Pairs = SmallVector<std::pair<const Type *, const Type *>, 4>;
    Pairs Local;
    Pairs *Stack = static_cast<Pairs *>(Seen);
    if (!Stack)
      Stack = &Local;
    for (auto &P : *Stack)
      if (P.first == X && P.second == Y)
        return true;
    Stack->push_back({X, Y});
    struct Pop {
      Pairs *S;
      ~Pop() { S->pop_back(); }
    } PopOnExit{Stack};
    auto Rec = [&](const Type *A2, const Type *B2) {
      return identicalImpl(A2, B2, IgnoreTags, Stack);
    };
    TypeSet *SA = typeSet(A), *SB = typeSet(B);
    if (SA->AllTerms != SB->AllTerms || SA->Comparable != SB->Comparable ||
        SA->Copy != SB->Copy || SA->Clone != SB->Clone ||
        SA->Methods.size() != SB->Methods.size() ||
        SA->Terms.size() != SB->Terms.size())
      return false;
    for (size_t I = 0; I < SA->Terms.size(); ++I) {
      bool Found = false;
      for (size_t J = 0; J < SB->Terms.size() && !Found; ++J)
        Found = SA->Terms[I].Tilde == SB->Terms[J].Tilde &&
                Rec(SA->Terms[I].T, SB->Terms[J].T);
      if (!Found)
        return false;
    }
    for (size_t I = 0; I < SA->Methods.size(); ++I) {
      Func *F = SA->Methods[I], *G = SB->Methods[I];
      if (F->Name != G->Name ||
          (!isExportedName(F->Name) && F->Pkg != G->Pkg) || !Rec(F->Ty, G->Ty))
        return false;
    }
    return true;
  }
  case Type::Kind::Named: {
    auto *A = cast<NamedType>(X), *B = cast<NamedType>(Y);
    if (A->Orig && B->Orig && A->Orig == B->Orig &&
        A->TArgs.size() == B->TArgs.size()) {
      for (size_t I = 0; I < A->TArgs.size(); ++I)
        if (!Rec(A->TArgs[I], B->TArgs[I]))
          return false;
      return true;
    }
    return false;
  }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Core types
//===----------------------------------------------------------------------===//

Type *Checker::coreType(Type *T) {
  if (!isTypeParam(T)) {
    Type *U = under(T);
    return U;
  }
  Type *Su = nullptr;
  bool Ok = underIs(T, [&](Type *U) {
    if (!U)
      return false;
    if (!Su) {
      Su = U;
      return true;
    }
    if (identical(Su, U))
      return true;
    // Channels with identical element types and compatible directions.
    auto *C1 = dyn_cast<ChanType>(Su);
    auto *C2 = dyn_cast<ChanType>(U);
    if (C1 && C2 && identical(C1->Elem, C2->Elem)) {
      if (C1->D == ChanType::Both) {
        Su = U;
        return true;
      }
      if (C2->D == ChanType::Both || C1->D == C2->D)
        return true;
    }
    return false;
  });
  return Ok ? Su : nullptr;
}

unsigned long long Checker::sizeofType(Type *T) {
  DepthGuard DG(RecDepth);
  if (RecDepth > MaxRecDepth)
    return 8;
  Type *U = under(T);
  switch (U->getKind()) {
  case Type::Kind::Basic:
    switch (cast<BasicType>(U)->BK) {
    case BasicKind::Bool:
    case BasicKind::Int8:
    case BasicKind::Uint8:
      return 1;
    case BasicKind::Int16:
    case BasicKind::Uint16:
      return 2;
    case BasicKind::Int32:
    case BasicKind::Uint32:
    case BasicKind::Float32:
      return 4;
    case BasicKind::Complex128:
    case BasicKind::String:
      return 16;
    default:
      return 8;
    }
  case Type::Kind::Slice:
    return 24;
  case Type::Kind::Interface:
    return 16;
  case Type::Kind::Array: {
    auto *A = cast<ArrayType>(U);
    return A->Len < 0 ? 0 : sizeofType(A->Elem) * A->Len;
  }
  case Type::Kind::Option: {
    // ADR-0018: a pointer kind's nil is its empty bit pattern; any other T
    // gets a hidden "is set" flag.
    auto *O = cast<OptionType>(U);
    unsigned long long ES = sizeofType(O->Elem);
    if (O->usesNiche())
      return ES;
    unsigned long long A = std::min<unsigned long long>(ES, 8);
    A = A == 0 ? 1 : A;
    return (ES + 1 + A - 1) / A * A;
  }
  case Type::Kind::Struct: {
    unsigned long long Off = 0, MaxA = 1;
    for (Var *F : cast<StructType>(U)->Fields) {
      unsigned long long FS = sizeofType(F->Ty);
      unsigned long long A = std::min<unsigned long long>(FS, 8);
      A = A == 0 ? 1 : A;
      if (isa<StructType>(under(F->Ty)) || isa<ArrayType>(under(F->Ty)))
        A = 1; // approximation: alignment of aggregates is not tracked
      Off = (Off + A - 1) / A * A + FS;
      MaxA = std::max(MaxA, A);
    }
    return (Off + MaxA - 1) / MaxA * MaxA;
  }
  default:
    return 8;
  }
}
