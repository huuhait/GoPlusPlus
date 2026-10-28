//===- Infer.cpp - Substitution, instantiation and type inference ---------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported (simplified) from types2's subst.go, instantiate.go, infer.go and
// unify.go (BSD-style license, Copyright The Go Authors).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

//===----------------------------------------------------------------------===//
// Substitution
//===----------------------------------------------------------------------===//

Type *Checker::subst(Type *T, ArrayRef<TypeParamType *> From,
                     ArrayRef<Type *> To) {
  if (From.empty() || From.size() != To.size())
    return T;
  llvm::DenseMap<const Type *, Type *> M;
  for (size_t I = 0; I < From.size(); ++I)
    M[From[I]] = To[I];
  return substImpl(T, M, 0);
}

Type *Checker::substImpl(Type *T, const llvm::DenseMap<const Type *, Type *> &M,
                         unsigned Depth) {
  if (!T || Depth > 64)
    return T;
  auto Rec = [&](Type *X) { return substImpl(X, M, Depth + 1); };
  switch (T->getKind()) {
  case Type::Kind::Basic:
    return T;
  case Type::Kind::TypeParam: {
    auto It = M.find(T);
    return It == M.end() ? T : It->second;
  }
  case Type::Kind::Pointer: {
    auto *P = cast<PointerType>(T);
    Type *E2 = Rec(P->Elem);
    return E2 == P->Elem ? T : Ctx.pointer(E2);
  }
  case Type::Kind::Slice: {
    auto *P = cast<SliceType>(T);
    Type *E2 = Rec(P->Elem);
    return E2 == P->Elem ? T : Ctx.slice(E2);
  }
  case Type::Kind::Ref: {
    auto *P = cast<RefType>(T);
    Type *E2 = Rec(P->Elem);
    return E2 == P->Elem ? T : Ctx.ref(E2);
  }
  case Type::Kind::Option: {
    auto *P = cast<OptionType>(T);
    Type *E2 = Rec(P->Elem);
    return E2 == P->Elem ? T : Ctx.option(E2);
  }
  case Type::Kind::Array: {
    auto *P = cast<ArrayType>(T);
    Type *E2 = Rec(P->Elem);
    return E2 == P->Elem ? T : Ctx.array(E2, P->Len);
  }
  case Type::Kind::Map: {
    auto *P = cast<MapType>(T);
    Type *K2 = Rec(P->Key), *E2 = Rec(P->Elem);
    return (K2 == P->Key && E2 == P->Elem) ? T : Ctx.map(K2, E2);
  }
  case Type::Kind::Chan: {
    auto *P = cast<sema::ChanType>(T);
    Type *E2 = Rec(P->Elem);
    return E2 == P->Elem ? T : Ctx.chan(P->D, E2);
  }
  case Type::Kind::Tuple: {
    auto *P = cast<TupleType>(T);
    SmallVector<Type *, 8> Out;
    bool Changed = false;
    for (Type *X : P->Elems) {
      Type *Y = Rec(X);
      Changed |= Y != X;
      Out.push_back(Y);
    }
    return Changed ? Ctx.tuple(Out) : T;
  }
  case Type::Kind::Signature: {
    auto *P = cast<SignatureType>(T);
    auto *Pa = cast<TupleType>(Rec(P->Params));
    auto *Re = cast<TupleType>(Rec(P->Results));
    if (Pa == P->Params && Re == P->Results)
      return T;
    return Ctx.signature(Pa, Re, P->Variadic, P->Fails, P->MoveMask,
                         P->TParams);
  }
  case Type::Kind::Struct: {
    auto *P = cast<StructType>(T);
    SmallVector<TypeContext::FieldSpec, 8> Fs;
    bool Changed = false;
    for (Var *F : P->Fields) {
      Type *Y = Rec(F->Ty);
      Changed |= Y != F->Ty;
      // `Val T @Required` stays "must be set" only in every instance.
      bool SetOnly =
          F->Required && (F->RequiredSetOnly || isa<TypeParamType>(F->Ty));
      Fs.push_back(
          {F->Name, F->Pkg, Y, F->Embedded, F->Tag, F->Required, SetOnly});
    }
    return Changed ? Ctx.structOf(Fs) : T;
  }
  case Type::Kind::Union: {
    auto *P = cast<UnionType>(T);
    SmallVector<Term, 4> Out;
    bool Changed = false;
    for (const Term &Tm : P->Terms) {
      Type *Y = Rec(Tm.T);
      Changed |= Y != Tm.T;
      Out.push_back({Tm.Tilde, Y});
    }
    return Changed ? Ctx.newUnion(Mem.copy(ArrayRef<Term>(Out))) : T;
  }
  case Type::Kind::Interface: {
    auto *P = cast<InterfaceType>(T);
    bool Changed = false;
    SmallVector<Func *, 8> Ms;
    for (Func *Mth : P->Methods) {
      Type *Y = Rec(Mth->Ty);
      if (Y != Mth->Ty) {
        Changed = true;
        Func *N = newFunc(Mth->Name, Y, Mth->Pos);
        N->Pkg = Mth->Pkg;
        N->IsInterfaceMethod = true;
        N->Origin = Mth;
        N->Fails = Mth->Fails;
        Ms.push_back(N);
      } else {
        Ms.push_back(Mth);
      }
    }
    SmallVector<Type *, 4> Es;
    for (Type *X : P->Embeddeds) {
      Type *Y = Rec(X);
      Changed |= Y != X;
      Es.push_back(Y);
    }
    if (!Changed)
      return T;
    InterfaceType *N =
        Ctx.newInterface(Mem.copy(ArrayRef<Func *>(Ms)),
                         Mem.copy(ArrayRef<Type *>(Es)), {}, P->Implicit);
    N->Sp = P->Sp;
    N->Substituted = true;
    return N;
  }
  case Type::Kind::Named: {
    auto *N = cast<NamedType>(T);
    if (!N->Orig || N->TArgs.empty())
      return T;
    SmallVector<Type *, 4> Args;
    bool Changed = false;
    for (Type *X : N->TArgs) {
      Type *Y = Rec(X);
      Changed |= Y != X;
      Args.push_back(Y);
    }
    return Changed ? instantiateNamed(N->Orig, Args) : T;
  }
  }
  return T;
}

NamedType *Checker::instantiateNamed(NamedType *Orig, ArrayRef<Type *> TArgs) {
  return Ctx.instance(Orig->origin(), TArgs);
}

void Checker::expandNamed(NamedType *N) {
  std::lock_guard<std::recursive_mutex> G(Ctx.lock());
  if (N->Expanded || !N->Orig)
    return;
  // A recursive type asks for its own expansion while it is expanded: the
  // outer call finishes it. (Only the thread holding the lock sees this.)
  if (N->Expanding)
    return;
  NamedType *O = N->Orig;
  if (!O->Underlying && O->Obj && S.ObjMap.count(O->Obj))
    objDecl(O->Obj, nullptr);
  if (!O->Underlying)
    return; // still being declared: retry later
  N->Expanding = true;
  Type *U = invalid();
  if (O->TParams.size() == N->TArgs.size())
    U = under(subst(O->Underlying, O->TParams, N->TArgs));
  N->Underlying = U;
  N->Expanding = false;
  // Publishes Underlying to the other body threads.
  N->Expanded.store(true, std::memory_order_release);
}

Func *Checker::instantiatedMethod(NamedType *Inst, unsigned I) {
  std::lock_guard<std::recursive_mutex> G(Ctx.lock());
  NamedType *O = Inst->Orig;
  Func *M = (*O->Methods)[I];
  if (!Inst->InstMethods)
    Inst->InstMethods = Ctx.newMethodList();
  auto &IM = *Inst->InstMethods;
  while (IM.size() < O->Methods->size())
    IM.push_back(nullptr);
  if (IM[I])
    return IM[I];
  if (!M->Ty)
    objDecl(M, nullptr);
  Func *N;
  {
    std::lock_guard<std::mutex> A(Ctx.allocLock());
    N = Ctx.arena().make<Func>(*M);
  }
  N->Origin = M;
  if (M->Ty) {
    ArrayRef<TypeParamType *> From = M->RecvTParams;
    if (From.size() == Inst->TArgs.size())
      N->Ty = subst(M->Ty, From, Inst->TArgs);
  }
  IM[I] = N;
  return N;
}

SignatureType *Checker::instantiateFuncSig(SignatureType *Sig,
                                           ArrayRef<Type *> TArgs) {
  if (Sig->TParams.size() != TArgs.size())
    return Sig;
  llvm::DenseMap<const Type *, Type *> M;
  for (size_t I = 0; I < TArgs.size(); ++I)
    M[Sig->TParams[I]] = TArgs[I];
  auto *Pa = cast<TupleType>(substImpl(Sig->Params, M, 0));
  auto *Re = cast<TupleType>(substImpl(Sig->Results, M, 0));
  return Ctx.signature(Pa, Re, Sig->Variadic, Sig->Fails, Sig->MoveMask, {});
}

//===----------------------------------------------------------------------===//
// Verification
//===----------------------------------------------------------------------===//

int Checker::verify(SourceLoc Pos, ArrayRef<TypeParamType *> TParams,
                    ArrayRef<Type *> TArgs, std::string *Cause) {
  for (size_t I = 0; I < TParams.size() && I < TArgs.size(); ++I) {
    if (!TArgs[I] || !isValid(TArgs[I]))
      continue;
    if (!satisfies(Pos, TArgs[I], TParams[I], TParams, TArgs, Cause))
      return static_cast<int>(I);
  }
  return -1;
}

bool Checker::satisfies(SourceLoc Pos, Type *TArg, TypeParamType *TPar,
                        ArrayRef<TypeParamType *> TParams,
                        ArrayRef<Type *> TArgs, std::string *Cause) {
  Type *Bound = TPar->Bound;
  if (!Bound)
    return true;
  Bound = subst(Bound, TParams, TArgs);
  return implements(Pos, TArg, Bound, true, Cause);
}

//===----------------------------------------------------------------------===//
// Unification (types2 unify.go)
//===----------------------------------------------------------------------===//

namespace {

constexpr unsigned UAssign = 1; // top-level types may match inexactly
constexpr unsigned UExact = 2;  // types unify only if identical

class Unifier {
public:
  Unifier(Checker &C, ArrayRef<TypeParamType *> TPs) : C(C), TPs(TPs) {
    Val.assign(TPs.size(), nullptr);
    Parent.resize(TPs.size());
    for (size_t I = 0; I < TPs.size(); ++I)
      Parent[I] = I;
  }

  int indexOf(const Type *T) const {
    for (size_t I = 0; I < TPs.size(); ++I)
      if (TPs[I] == T)
        return static_cast<int>(I);
    return -1;
  }
  size_t find(size_t I) {
    while (Parent[I] != I)
      I = Parent[I] = Parent[Parent[I]];
    return I;
  }
  Type *at(size_t I) { return Val[find(I)]; }
  void set(size_t I, Type *T) { Val[find(I)] = T; }
  size_t unknowns() {
    size_t N = 0;
    for (size_t I = 0; I < TPs.size(); ++I)
      N += at(I) == nullptr;
    return N;
  }
  SmallVector<Type *, 4> inferred() {
    SmallVector<Type *, 4> Out;
    for (size_t I = 0; I < TPs.size(); ++I)
      Out.push_back(at(I));
    return Out;
  }

  bool unify(Type *X, Type *Y, unsigned Mode) { return nify(X, Y, Mode, 0); }

private:
  bool join(size_t X, size_t Y) {
    size_t RX = find(X), RY = find(Y);
    if (RX == RY)
      return true;
    if (Val[RX] && Val[RY])
      return false;
    Parent[RX] = RY;
    if (!Val[RY])
      Val[RY] = Val[RX];
    return true;
  }
  bool nify(Type *X, Type *Y, unsigned Mode, unsigned Depth);
  bool termsEqual(TypeSet *A, TypeSet *B);

  Checker &C;
  ArrayRef<TypeParamType *> TPs;
  SmallVector<Type *, 8> Val;
  SmallVector<size_t, 8> Parent;
  SmallVector<std::pair<const InterfaceType *, const InterfaceType *>, 4> Stack;
};

} // namespace

bool Unifier::termsEqual(TypeSet *A, TypeSet *B) {
  if (A->AllTerms != B->AllTerms)
    return false;
  if (A->AllTerms)
    return true;
  auto Sub = [&](TypeSet *P, TypeSet *Q) {
    for (const Term &T : P->Terms) {
      bool Found = false;
      for (const Term &U : Q->Terms)
        if (T.Tilde == U.Tilde && C.identical(T.T, U.T)) {
          Found = true;
          break;
        }
      if (!Found)
        return false;
    }
    return true;
  };
  return Sub(A, B) && Sub(B, A);
}

static InterfaceType *asInterfaceNotTP(Checker &C, Type *T) {
  if (!T || isa<TypeParamType>(T))
    return nullptr;
  return dyn_cast_or_null<InterfaceType>(C.under(T));
}

bool Unifier::nify(Type *X, Type *Y, unsigned Mode, unsigned Depth) {
  if (X == Y)
    return true;
  if (!X || !Y)
    return false;
  if (Depth > 50)
    return false;
  // Ensure that a defined type is in Y and a bound type parameter in X.
  if (isa<NamedType>(X) || indexOf(Y) >= 0)
    std::swap(X, Y);

  // Unification fails when matching a defined type against a type literal,
  // unless the match is inexact: then continue with the underlying type.
  if (auto *NY = dyn_cast<NamedType>(Y);
      (Mode & UExact) == 0 && NY && !isa<NamedType>(X) &&
      !isa<TypeParamType>(X) && !asInterfaceNotTP(C, X)) {
    Y = C.under(NY);
    if (X == Y)
      return true;
  }

  int PX = indexOf(X), PY = indexOf(Y);
  if (PX >= 0 && PY >= 0) {
    if (join(PX, PY))
      return true;
    return nify(at(PX), at(PY), Mode, Depth + 1);
  }
  if (PX >= 0) {
    Type *XT = at(PX);
    if (!XT) {
      if (C.isValid(Y))
        set(PX, Y);
      return true;
    }
    if (!nify(XT, Y, Mode, Depth + 1))
      return false;
    InterfaceType *XI = asInterfaceNotTP(C, XT);
    InterfaceType *YI = asInterfaceNotTP(C, Y);
    bool XN = isa<NamedType>(XT), YN = isa<NamedType>(Y);
    if (XI && YI) {
      if (XN && YN)
        return C.identical(XT, Y);
      if (C.typeSet(XI)->Methods.size() != C.typeSet(YI)->Methods.size())
        return false;
    } else if (XI || YI) {
      return false; // order dependence (go.dev/issue/60933)
    }
    if ((Mode & UExact) == 0) {
      if (XN) {
        // x is a defined type: nothing to do.
      } else if (YN) {
        set(PX, Y);
      } else if (auto *YC = dyn_cast_or_null<sema::ChanType>(C.under(Y));
                 YC && YC->D != sema::ChanType::Both) {
        set(PX, Y);
      }
    }
    return true;
  }

  // Interface inference (Go 1.21).
  if ((Mode & UExact) == 0) {
    InterfaceType *XI = asInterfaceNotTP(C, X);
    InterfaceType *YI = asInterfaceNotTP(C, Y);
    if (XI && YI) {
      TypeSet *XS = C.typeSet(XI), *YS = C.typeSet(YI);
      if (XS->Comparable != YS->Comparable)
        return false;
      if (!termsEqual(XS, YS))
        return false;
      for (auto &P : Stack)
        if (P.first == XI && P.second == YI)
          return true;
      Stack.push_back({XI, YI});
      ArrayRef<Func *> XM = XS->Methods, YM = YS->Methods;
      if (XM.size() > YM.size())
        std::swap(XM, YM);
      bool Ok = true;
      for (Func *Xm : XM) {
        Func *Ym = nullptr;
        for (Func *Cand : YM)
          if (C.sameId(Cand, Xm->Pkg, Xm->Name, false)) {
            Ym = Cand;
            break;
          }
        if (!Ym || !nify(Xm->Ty, Ym->Ty, UExact, Depth + 1)) {
          Ok = false;
          break;
        }
      }
      Stack.pop_back();
      return Ok;
    }
    if (YI) {
      XI = YI;
      Y = X;
    }
    if (XI) {
      for (Func *Xm : C.typeSet(XI)->Methods) {
        auto LR = C.lookupFieldOrMethod(Y, false, Xm->Pkg, Xm->Name);
        auto *Ym = dyn_cast_or_null<Func>(LR.Obj);
        if (Ym && !Ym->Ty)
          C.objDecl(Ym, nullptr);
        if (!Ym || !nify(Xm->Ty, Ym->Ty, UExact, Depth + 1))
          return false;
      }
      return true;
    }
  }

  // An unbound type parameter goes into X.
  if (isa<TypeParamType>(Y))
    std::swap(X, Y);

  unsigned EMode = Mode;
  if (Mode & UAssign)
    EMode |= UExact;

  switch (X->getKind()) {
  case Type::Kind::Basic:
    if (auto *B = dyn_cast<BasicType>(Y))
      return cast<BasicType>(X)->BK == B->BK;
    return false;
  case Type::Kind::Array: {
    auto *A = cast<ArrayType>(X);
    if (auto *B = dyn_cast<ArrayType>(Y))
      return (A->Len < 0 || B->Len < 0 || A->Len == B->Len) &&
             nify(A->Elem, B->Elem, EMode, Depth + 1);
    return false;
  }
  case Type::Kind::Slice:
    if (auto *B = dyn_cast<SliceType>(Y))
      return nify(cast<SliceType>(X)->Elem, B->Elem, EMode, Depth + 1);
    return false;
  case Type::Kind::Pointer:
    if (auto *B = dyn_cast<PointerType>(Y))
      return nify(cast<PointerType>(X)->Elem, B->Elem, EMode, Depth + 1);
    return false;
  case Type::Kind::Ref:
    if (auto *B = dyn_cast<RefType>(Y))
      return nify(cast<RefType>(X)->Elem, B->Elem, EMode, Depth + 1);
    return false;
  case Type::Kind::Option:
    if (auto *B = dyn_cast<OptionType>(Y))
      return nify(cast<OptionType>(X)->Elem, B->Elem, EMode, Depth + 1);
    return false;
  case Type::Kind::Struct: {
    auto *A = cast<StructType>(X);
    auto *B = dyn_cast<StructType>(Y);
    if (!B || A->Fields.size() != B->Fields.size())
      return false;
    for (size_t I = 0; I < A->Fields.size(); ++I) {
      Var *F = A->Fields[I], *G = B->Fields[I];
      if (F->Embedded != G->Embedded || F->Tag != G->Tag ||
          !C.sameId(F, G->Pkg, G->Name, false) ||
          !nify(F->Ty, G->Ty, EMode, Depth + 1))
        return false;
    }
    return true;
  }
  case Type::Kind::Tuple: {
    auto *A = cast<TupleType>(X);
    auto *B = dyn_cast<TupleType>(Y);
    if (!B || A->size() != B->size())
      return false;
    for (size_t I = 0; I < A->size(); ++I)
      if (!nify(A->Elems[I], B->Elems[I], Mode, Depth + 1))
        return false;
    return true;
  }
  case Type::Kind::Signature: {
    auto *A = cast<SignatureType>(X);
    auto *B = dyn_cast<SignatureType>(Y);
    return B && A->Variadic == B->Variadic &&
           nify(A->Params, B->Params, EMode, Depth + 1) &&
           nify(A->Results, B->Results, EMode, Depth + 1);
  }
  case Type::Kind::Interface: {
    auto *A = cast<InterfaceType>(X);
    auto *B = dyn_cast<InterfaceType>(Y);
    if (!B)
      return false;
    TypeSet *XS = C.typeSet(A), *YS = C.typeSet(B);
    if (XS->Comparable != YS->Comparable || !termsEqual(XS, YS))
      return false;
    if (XS->Methods.size() != YS->Methods.size())
      return false;
    for (auto &P : Stack)
      if (P.first == A && P.second == B)
        return true;
    Stack.push_back({A, B});
    bool Ok = true;
    for (size_t I = 0; I < XS->Methods.size(); ++I) {
      Func *F = XS->Methods[I], *G = YS->Methods[I];
      if (!C.sameId(F, G->Pkg, G->Name, false) ||
          !nify(F->Ty, G->Ty, UExact, Depth + 1)) {
        Ok = false;
        break;
      }
    }
    Stack.pop_back();
    return Ok;
  }
  case Type::Kind::Map: {
    auto *A = cast<MapType>(X);
    auto *B = dyn_cast<MapType>(Y);
    return B && nify(A->Key, B->Key, EMode, Depth + 1) &&
           nify(A->Elem, B->Elem, EMode, Depth + 1);
  }
  case Type::Kind::Chan: {
    auto *A = cast<sema::ChanType>(X);
    auto *B = dyn_cast<sema::ChanType>(Y);
    return B && ((Mode & UExact) == 0 || A->D == B->D) &&
           nify(A->Elem, B->Elem, EMode, Depth + 1);
  }
  case Type::Kind::Named: {
    auto *A = cast<NamedType>(X);
    auto *B = dyn_cast<NamedType>(Y);
    if (!B || A->TArgs.size() != B->TArgs.size())
      return false;
    for (size_t I = 0; I < A->TArgs.size(); ++I)
      if (!nify(A->TArgs[I], B->TArgs[I], Mode, Depth + 1))
        return false;
    return A->origin() == B->origin();
  }
  case Type::Kind::TypeParam: {
    // An unbound type parameter unifies through its core type
    // (go.dev/issue/50755).
    if (Type *CX = C.commonUnder(X, nullptr, nullptr))
      return nify(CX, Y, UAssign, Depth + 1);
    return false;
  }
  case Type::Kind::Union:
    return C.identical(X, Y);
  }
  return false;
}

static bool mentionsAny(Checker &C, Type *T, ArrayRef<TypeParamType *> TPs,
                        unsigned Depth = 0) {
  if (!T || Depth > 40)
    return false;
  switch (T->getKind()) {
  case Type::Kind::Basic:
    return false;
  case Type::Kind::TypeParam:
    for (TypeParamType *P : TPs)
      if (P == T)
        return true;
    return false;
  case Type::Kind::Pointer:
    return mentionsAny(C, cast<PointerType>(T)->Elem, TPs, Depth + 1);
  case Type::Kind::Slice:
    return mentionsAny(C, cast<SliceType>(T)->Elem, TPs, Depth + 1);
  case Type::Kind::Ref:
    return mentionsAny(C, cast<RefType>(T)->Elem, TPs, Depth + 1);
  case Type::Kind::Option:
    return mentionsAny(C, cast<OptionType>(T)->Elem, TPs, Depth + 1);
  case Type::Kind::Array:
    return mentionsAny(C, cast<ArrayType>(T)->Elem, TPs, Depth + 1);
  case Type::Kind::Map:
    return mentionsAny(C, cast<MapType>(T)->Key, TPs, Depth + 1) ||
           mentionsAny(C, cast<MapType>(T)->Elem, TPs, Depth + 1);
  case Type::Kind::Chan:
    return mentionsAny(C, cast<sema::ChanType>(T)->Elem, TPs, Depth + 1);
  case Type::Kind::Tuple:
    for (Type *E : cast<TupleType>(T)->Elems)
      if (mentionsAny(C, E, TPs, Depth + 1))
        return true;
    return false;
  case Type::Kind::Signature: {
    auto *S = cast<SignatureType>(T);
    return mentionsAny(C, S->Params, TPs, Depth + 1) ||
           mentionsAny(C, S->Results, TPs, Depth + 1);
  }
  case Type::Kind::Struct:
    for (Var *F : cast<StructType>(T)->Fields)
      if (mentionsAny(C, F->Ty, TPs, Depth + 1))
        return true;
    return false;
  case Type::Kind::Named:
    for (Type *E : cast<NamedType>(T)->TArgs)
      if (mentionsAny(C, E, TPs, Depth + 1))
        return true;
    return false;
  case Type::Kind::Interface: {
    auto *I = cast<InterfaceType>(T);
    for (Func *M : I->Methods)
      if (mentionsAny(C, M->Ty, TPs, Depth + 1))
        return true;
    for (Type *E : I->Embeddeds)
      if (mentionsAny(C, E, TPs, Depth + 1))
        return true;
    return false;
  }
  case Type::Kind::Union:
    for (const Term &Tm : cast<UnionType>(T)->Terms)
      if (mentionsAny(C, Tm.T, TPs, Depth + 1))
        return true;
    return false;
  }
  return false;
}

/// The single core term of a type parameter's constraint (types2: coreTerm).
/// Single is set if the type set has exactly one term.
static bool coreTerm(Checker &C, TypeParamType *P, Term &Out, bool &Single) {
  Single = false;
  auto *I = dyn_cast_or_null<InterfaceType>(C.under(P));
  if (!I)
    return false;
  TypeSet *TS = C.typeSet(I);
  if (TS->AllTerms || TS->Terms.empty())
    return false;
  if (TS->Terms.size() == 1) {
    Out = TS->Terms[0];
    Single = true;
    return true;
  }
  Type *Core = C.commonUnder(P, nullptr, nullptr);
  if (!Core)
    return false;
  bool Tilde = false;
  for (const Term &T : TS->Terms)
    Tilde |= T.Tilde;
  Out = {Tilde, Core};
  return true;
}

static std::string typeParamsString(ArrayRef<TypeParamType *> L) {
  size_t N = L.size();
  auto Name = [&](size_t I) { return std::string(L[I]->Obj->Name); };
  if (N == 0)
    return "";
  if (N == 1)
    return Name(0);
  if (N == 2)
    return Name(0) + " and " + Name(1);
  std::string S;
  for (size_t I = 0; I + 1 < N; ++I) {
    if (I)
      S += ", ";
    S += Name(I);
  }
  return S + ", and " + Name(N - 1);
}

/// Nils out inferred types that are part of a cycle through a type parameter
/// being inferred (types2: killCycles).
static void killCycles(ArrayRef<TypeParamType *> TPs,
                       llvm::MutableArrayRef<Type *> Inferred) {
  llvm::DenseSet<const Type *> Seen;
  auto Index = [&](const Type *T) {
    for (size_t I = 0; I < TPs.size(); ++I)
      if (TPs[I] == T)
        return static_cast<int>(I);
    return -1;
  };
  std::function<void(Type *)> Walk = [&](Type *T) {
    if (!T)
      return;
    if (Seen.count(T)) {
      if (isa<TypeParamType>(T))
        if (int I = Index(T); I >= 0)
          Inferred[I] = nullptr;
      return;
    }
    Seen.insert(T);
    switch (T->getKind()) {
    case Type::Kind::Basic:
      break;
    case Type::Kind::Array:
      Walk(cast<ArrayType>(T)->Elem);
      break;
    case Type::Kind::Slice:
      Walk(cast<SliceType>(T)->Elem);
      break;
    case Type::Kind::Pointer:
      Walk(cast<PointerType>(T)->Elem);
      break;
    case Type::Kind::Ref:
      Walk(cast<RefType>(T)->Elem);
      break;
    case Type::Kind::Option:
      Walk(cast<OptionType>(T)->Elem);
      break;
    case Type::Kind::Struct:
      for (Var *F : cast<StructType>(T)->Fields)
        Walk(F->Ty);
      break;
    case Type::Kind::Tuple:
      for (Type *E : cast<TupleType>(T)->Elems)
        Walk(E);
      break;
    case Type::Kind::Signature:
      Walk(cast<SignatureType>(T)->Params);
      Walk(cast<SignatureType>(T)->Results);
      break;
    case Type::Kind::Union:
      for (const Term &Tm : cast<UnionType>(T)->Terms)
        Walk(Tm.T);
      break;
    case Type::Kind::Interface:
      for (Func *M : cast<InterfaceType>(T)->Methods)
        Walk(M->Ty);
      for (Type *E : cast<InterfaceType>(T)->Embeddeds)
        Walk(E);
      break;
    case Type::Kind::Map:
      Walk(cast<MapType>(T)->Key);
      Walk(cast<MapType>(T)->Elem);
      break;
    case Type::Kind::Chan:
      Walk(cast<sema::ChanType>(T)->Elem);
      break;
    case Type::Kind::Named:
      for (Type *E : cast<NamedType>(T)->TArgs)
        Walk(E);
      break;
    case Type::Kind::TypeParam:
      if (int I = Index(T); I >= 0 && Inferred[I])
        Walk(Inferred[I]);
      break;
    }
    Seen.erase(T);
  };
  for (TypeParamType *P : TPs)
    Walk(P);
}

SmallVector<Type *, 4>
Checker::infer(SourceLoc Pos, ArrayRef<TypeParamType *> TParams,
               ArrayRef<Type *> TArgs0, TupleType *Params0,
               ArrayRef<Operand> Args, bool Reverse, InferError *Err) {
  auto Fail = [&](SourceLoc At, std::string Msg) {
    if (Err) {
      Err->Pos = At;
      Err->Msg = std::move(Msg);
    }
    return SmallVector<Type *, 4>();
  };
  size_t N = TParams.size();
  SmallVector<Type *, 4> TArgs(TArgs0.begin(), TArgs0.end());
  TArgs.resize(N, nullptr);
  bool All = TArgs0.size() == N;
  for (Type *T : TArgs0)
    All &= T != nullptr;
  if (All)
    return TArgs;
  for (const Operand &A : Args)
    if (A.M == Mode::Invalid)
      return SmallVector<Type *, 4>();

  // Substitute the type arguments we have in the parameter list: better
  // error messages (go.dev/issue/...).
  TupleType *Params = Params0;
  {
    llvm::DenseMap<const Type *, Type *> M;
    for (size_t I = 0; I < N; ++I)
      if (TArgs[I])
        M[TParams[I]] = TArgs[I];
    if (!M.empty() && Params0->size() > 0)
      Params = cast<TupleType>(substImpl(Params0, M, 0));
  }

  Unifier U(*this, TParams);
  for (size_t I = 0; I < N; ++I)
    if (TArgs[I])
      U.set(I, TArgs[I]);

  auto SubstInferred = [&](Type *T) {
    llvm::DenseMap<const Type *, Type *> M;
    auto Inf = U.inferred();
    for (size_t I = 0; I < N; ++I)
      if (Inf[I])
        M[TParams[I]] = Inf[I];
    return substImpl(T, M, 0);
  };
  auto Mismatch = [&](Type *TPar, Type *TArg, const Operand &A) {
    auto Inf = U.inferred();
    if (!Inf[0]) {
      bool AllFailed = true;
      for (Type *T : Inf)
        if (T)
          AllFailed = false;
      if (AllFailed)
        return Fail(A.pos(), "type " + str(TArg) + " of " + str(A.E) +
                                 " does not match " + str(TPar) +
                                 " (cannot infer " + typeParamsString(TParams) +
                                 ")");
    }
    Type *Inferred = SubstInferred(TPar);
    if (Inferred != TPar) {
      if (Reverse)
        return Fail(A.pos(),
                    "inferred type " + str(Inferred) + " for " + str(TPar) +
                        " does not match type " + str(TArg) + " of " +
                        (ReverseDesc.empty() ? str(A.E) : ReverseDesc));
      return Fail(A.pos(), "type " + str(TArg) + " of " + str(A.E) +
                               " does not match inferred type " +
                               str(Inferred) + " for " + str(TPar));
    }
    return Fail(A.pos(), "type " + str(TArg) + " of " + str(A.E) +
                             " does not match " + str(TPar));
  };

  // 1. Use the information from the function arguments.
  SmallVector<size_t, 4> Untyped;
  for (size_t I = 0; I < Args.size() && I < Params->size(); ++I) {
    const Operand &A = Args[I];
    Type *PT = Params->Elems[I];
    if (A.M == Mode::Invalid)
      continue;
    if (!mentionsAny(*this, PT, TParams) && !mentionsAny(*this, A.Ty, TParams))
      continue;
    if (isTyped(A.Ty)) {
      if (!U.unify(PT, A.Ty, UAssign))
        return Mismatch(PT, A.Ty, A);
    } else if (isa<TypeParamType>(PT) && !A.isNil()) {
      Untyped.push_back(I);
    }
  }

  // 2. Use the information from the type parameter constraints.
  for (;;) {
    size_t NN = U.unknowns();
    for (size_t I = 0; I < N; ++I) {
      TypeParamType *TP = TParams[I];
      Type *TX = U.at(I);
      Term Core;
      bool Single;
      if (coreTerm(*this, TP, Core, Single)) {
        if (TX) {
          if (!U.unify(TX, Core.T, 0))
            return Fail(Pos, str(TP) + " (type " + str(TX) +
                                 ") does not satisfy " + str(TP->Bound));
        } else if (Single && !Core.Tilde) {
          U.set(I, Core.T);
        }
      }
      // The type argument must implement the methods of the constraint,
      // possibly after unification of the method signatures.
      if (TX && !mentionsAny(*this, TX, TParams)) {
        auto *CI = dyn_cast_or_null<InterfaceType>(under(TP));
        if (CI)
          for (Func *M : typeSet(CI)->Methods) {
            auto LR = lookupFieldOrMethod(TX, false, M->Pkg, M->Name);
            auto *F = dyn_cast_or_null<Func>(LR.Obj);
            if (F && !F->Ty)
              objDecl(F, nullptr);
            std::string Cause;
            if (!F)
              Cause = "(missing method " + std::string(M->Name) + ")";
            else if (!U.unify(F->Ty, M->Ty, UExact))
              Cause = "(wrong type for method " + std::string(M->Name) + ")";
            if (!Cause.empty())
              return Fail(Pos, str(TP) + " (type " + str(TX) +
                                   ") does not satisfy " + str(TP->Bound) +
                                   " " + Cause);
          }
      }
    }
    if (U.unknowns() == NN)
      break;
  }

  // 3. Use the information from untyped constants.
  llvm::DenseMap<const Type *, Type *> MaxUntyped;
  SmallVector<TypeParamType *, 4> MaxOrder;
  for (size_t Index : Untyped) {
    auto *TP = cast<TypeParamType>(Params->Elems[Index]);
    int TI = U.indexOf(TP);
    if (TI < 0 || U.at(TI))
      continue;
    const Operand &A = Args[Index];
    Type *&Max = MaxUntyped[TP];
    if (!Max) {
      Max = A.Ty;
      MaxOrder.push_back(TP);
    } else {
      Type *M = maxType(Max, A.Ty);
      if (!M)
        return Fail(A.pos(), "mismatched types " + str(Max) + " and " +
                                 str(A.Ty) + " (cannot infer " + str(TP) + ")");
      Max = M;
    }
  }
  for (TypeParamType *TP : MaxOrder)
    U.set(U.indexOf(TP), defaultType(MaxUntyped[TP]));

  // Simplify: substitute the type parameters in the inferred types until
  // nothing changes anymore.
  SmallVector<Type *, 4> Inferred = U.inferred();
  killCycles(TParams, Inferred);
  SmallVector<size_t, 4> Dirty;
  for (size_t I = 0; I < N; ++I)
    if (Inferred[I] && !TArgs[I])
      Dirty.push_back(I);
  while (!Dirty.empty()) {
    llvm::DenseMap<const Type *, Type *> M;
    for (size_t I = 0; I < N; ++I)
      if (Inferred[I])
        M[TParams[I]] = Inferred[I];
    size_t K = 0;
    for (size_t Index : Dirty) {
      Type *T0 = Inferred[Index];
      Type *T1 = substImpl(T0, M, 0);
      if (T1 != T0) {
        Inferred[Index] = T1;
        Dirty[K++] = Index;
      }
    }
    Dirty.resize(K);
  }
  for (size_t I = 0; I < N; ++I)
    if (!Inferred[I] || mentionsAny(*this, Inferred[I], TParams)) {
      TypeName *O = TParams[I]->Obj;
      PresumedLoc PL = S.SM.getPresumedLoc(O->Pos);
      return Fail(Pos, "cannot infer " + std::string(O->Name) +
                           " (declared at " + std::string(PL.Filename) + ":" +
                           std::to_string(PL.Line) + ":" +
                           std::to_string(PL.Column) + ")");
    }
  return Inferred;
}
