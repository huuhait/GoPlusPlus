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
    // GEN-14: a unit that mentions a unit parameter (`float64[U/S]`, or the
    // unit argument `U*S` itself) takes the unit argument in its place (out
    // of line: this frame recurses per level of a type, M4).
    return unitSubstBasic(T, M);
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
    return E2 == P->Elem ? T : Ctx.ref(E2, P->Borrowed);
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
      // `Val T @Required` stays "must be set" only in every instance. The
      // effective bit was decided on the generic declaration (TYP-15a: `V T`
      // of a @Strict struct stays required for T = Option[int]).
      // The mark only matters where it changes never-nil-ness (error, any,
      // map, chan): elsewhere it would split the identity of equal types.
      // A type that is not resolved yet keeps the mark (the safe side).
      bool SetOnly = F->Required &&
                     (F->RequiredSetOnly || isa<TypeParamType>(F->Ty)) &&
                     (isa<TypeParamType>(Y) || !isValid(under(Y)) ||
                      requiredMeansNeverNil(Y));
      Fs.push_back({F->Name, F->Pkg, Y, F->Embedded, F->Tag, F->Required,
                    SetOnly, F->MarkedRequired, F->MarkedOption, F->IsParent});
    }
    return Changed ? Ctx.structOf(Fs, P->Strict) : T;
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
        N->Op = Mth->Op;
        N->Right = Mth->Right;
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
    Type *Ext = P->Extends ? Rec(P->Extends) : nullptr;
    Changed |= Ext != P->Extends;
    if (!Changed)
      return T;
    InterfaceType *N =
        Ctx.newInterface(Mem.copy(ArrayRef<Func *>(Ms)),
                         Mem.copy(ArrayRef<Type *>(Es)), {}, P->Implicit);
    N->Sp = P->Sp;
    N->Extends = Ext;
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
  if (!O->Underlying && O->Obj && S.ObjMap.count(O->Obj)) {
    if (PendingDecl.count(O->Obj))
      declarePending(O->Obj); // a generic type deferred by objDecl (P03.23)
    else
      objDecl(O->Obj, nullptr);
  }
  // `type B[P any] A` with A plain: B has no Underlying of its own, it is
  // A's, once A is resolved (found by following DirectRhs, a chain of plain
  // types; a ring is cut by Seen).
  Type *OU = O->Underlying;
  if (!OU) {
    llvm::SmallPtrSet<const NamedType *, 8> Seen;
    for (NamedType *P = O; P && Seen.insert(P).second;) {
      if (P->Underlying) {
        OU = P->Underlying;
        break;
      }
      P = P->DirectRhs ? dyn_cast<NamedType>(P->DirectRhs) : nullptr;
    }
  }
  if (!OU)
    return; // still being declared: retry later
  N->Expanding = true;
  Type *U = invalid();
  if (O->TParams.size() == N->TArgs.size())
    U = under(subst(OU, O->TParams, N->TArgs));
  N->Underlying = U;
  N->Expanding = false;
  if (ExpandedInsts)
    ExpandedInsts->push_back(N);
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
  N->RecvInst = Inst;
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
                    ArrayRef<Type *> TArgs, std::string *Cause, bool ForType,
                    std::string *ViewWhy) {
  for (size_t I = 0; I < TParams.size() && I < TArgs.size(); ++I) {
    if (!TArgs[I] || !isValid(TArgs[I]))
      continue;
    if (!satisfies(Pos, TArgs[I], TParams[I], TParams, TArgs, Cause, ForType,
                   ViewWhy))
      return static_cast<int>(I);
  }
  return -1;
}

bool Checker::satisfies(SourceLoc Pos, Type *TArg, TypeParamType *TPar,
                        ArrayRef<TypeParamType *> TParams,
                        ArrayRef<Type *> TArgs, std::string *Cause,
                        bool ForType, std::string *ViewWhy) {
  Type *Bound = TPar->Bound;
  if (!Bound)
    return true;
  // GEN-14: a unit parameter takes a unit, every other parameter a type.
  {
    bool IsUnitArg = unitArgCanon(TArg) != nullptr;
    if (isUnitParam(TPar) || IsUnitArg) {
      if (isUnitParam(TPar) && IsUnitArg) {
        unitDictUses(Pos, TArg, TPar);
        return true;
      }
      if (Cause)
        *Cause = IsUnitArg ? "the unit " + str(TArg) +
                                 " is not a type: " + str(TPar) +
                                 " is not a unit parameter"
                           : str(TArg) + " is not a unit: " + str(TPar) +
                                 " is a unit parameter (GEN-14)";
      return false;
    }
  }
  Bound = subst(Bound, TParams, TArgs);
  if (!implements(Pos, TArg, Bound, true, Cause))
    return false;
  // GEN-1, GEN-2 (R24): a parameter that is Copy (Clone) only because of a
  // type set with a ~T term requires the same of every type argument (a
  // defined type over T may have a drop method). Here, the one check that
  // runs per instantiation: explicit, inferred, in another package (the
  // type set travels in the export data), and through another generic (an
  // outer parameter must be Copy, or carry the same requirement).
  if (!own())
    return true;
  // GEN-12, EXP-15, TYP-19: what the generic code does with the parameter
  // (TypeParamType::uses) is decided on the real type argument, with the
  // predicate of the plain code (useWhy). Same decision for every way to
  // instantiate (explicit, inferred, through another generic, in another
  // package). The uses of a generic of this package are final only after
  // all its bodies: such an instantiation waits (untaggedCheck). A generic
  // of another package is complete: its uses came with the export data.
  if (mayBreakUses(TArg)) {
    // (A parameter copied for one call's inference stands for its original.)
    TypeParamType *Root = TPar->RenamedFrom ? TPar->RenamedFrom : TPar;
    if (Root->Obj && Root->Obj->Pkg == S.Pkg) {
      UntaggedPend.push_back({Pos, TArg, Root, ForType});
    } else {
      // A use that TArg gives on to a type parameter of this package is
      // noted there.
      bool Saved = NotingPassed;
      NotingPassed = true;
      std::string Why;
      bool IsView = false;
      for (const TypeParamType::Use &U : Root->uses())
        if (Why = useWhy(TArg, Root, U); !Why.empty()) {
          IsView = U.K == TypeParamType::Use::ViewArg;
          break;
        }
      NotingPassed = Saved;
      if (!Why.empty()) {
        if (Cause)
          *Cause = Why;
        if (IsView && ViewWhy)
          *ViewWhy = Why;
        return false;
      }
    }
  }
  bool Implicit = false;
  if (paramIs(TPar, /*Clone=*/false, &Implicit)) {
    if (Implicit && !isCopyType(TArg)) {
      if (Cause)
        *Cause = str(TArg) + " does not satisfy Copy, which the type set of " +
                 str(TPar) + " (" + str(Bound) +
                 ") implies: " + notCopyWhy(TArg) + " (GEN-1)";
      return false;
    }
    return true;
  }
  if (paramIs(TPar, /*Clone=*/true, &Implicit) && Implicit) {
    std::string Why;
    if (!satisfiesClone(TArg, &Why)) {
      if (Cause)
        *Cause = str(TArg) + " does not satisfy Clone, which the type set of " +
                 str(TPar) + " (" + str(Bound) + ") implies" +
                 (Why.empty() ? "" : ": " + Why) + " (GEN-2)";
      return false;
    }
  }
  return true;
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
  bool unifyUnits(const UnitCanon *X, const UnitCanon *Y);

  Checker &C;
  ArrayRef<TypeParamType *> TPs;
  SmallVector<Type *, 8> Val;
  SmallVector<size_t, 8> Parent;
  SmallVector<std::pair<const InterfaceType *, const InterfaceType *>, 4> Stack;
};

} // namespace

/// GEN-14: the unit of a parameter type (X, may mention the unit parameters
/// being inferred) against the unit of an argument type (Y). The unit
/// parameters already known are replaced first; then at most one is left, as
/// a factor with exponent 1 or -1, and the answer is exact: U^e * R = Y gives
/// U = (Y / R)^e.
bool Unifier::unifyUnits(const UnitCanon *X, const UnitCanon *Y) {
  if (X == Y)
    return true;
  if (!X || !Y)
    return false;
  TypeContext &Ctx = C.Ctx;
  UnitStatus St;
  const UnitCanon *K = units::substitute(
      Ctx, X,
      [&](const TypeParamType *P) -> const UnitCanon * {
        int I = indexOf(P);
        if (I < 0)
          return nullptr;
        return C.unitArgCanon(at(I));
      },
      St);
  if (!K)
    return false;
  if (K == Y)
    return true;
  // The unbound unit parameters of K.
  const UnitDim *Free = nullptr;
  for (const UnitDim &D : K->Dims)
    if (D.Param && indexOf(D.Param) >= 0) {
      if (Free)
        return false; // two unknown units in one expression
      Free = &D;
    }
  if (!Free || (Free->Exp != 1 && Free->Exp != -1))
    return false;
  // R = K without the free dimension.
  llvm::SmallVector<UnitDim, 8> Rest;
  for (const UnitDim &D : K->Dims)
    if (&D != Free)
      Rest.push_back(D);
  const UnitCanon *R = units::make(Ctx, Rest, K->Factor, St);
  if (!R)
    return false;
  const UnitCanon *Q = units::div(Ctx, Y, R, St);
  if (!Q)
    return false;
  if (Free->Exp == -1) {
    Q = units::div(Ctx, units::one(Ctx), Q, St);
    if (!Q)
      return false;
  }
  if (Q->dimensionless())
    return false;
  int PI = indexOf(Free->Param);
  Type *Arg = Ctx.unitArgType(Q);
  if (Type *Cur = at(PI))
    return Cur == Arg;
  set(PI, Arg);
  return true;
}

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
      return cast<BasicType>(X)->BK == B->BK &&
             unifyUnits(cast<BasicType>(X)->Unit, B->Unit);
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
    if (!B || A->Fields.size() != B->Fields.size() || A->Strict != B->Strict)
      return false;
    for (size_t I = 0; I < A->Fields.size(); ++I) {
      Var *F = A->Fields[I], *G = B->Fields[I];
      // Identity includes the attributes (TYP-15a), like the tag.
      if (F->Embedded != G->Embedded || F->IsParent != G->IsParent ||
          F->Tag != G->Tag || F->Required != G->Required ||
          F->RequiredSetOnly != G->RequiredSetOnly ||
          F->MarkedRequired != G->MarkedRequired ||
          F->MarkedOption != G->MarkedOption ||
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
  case Type::Kind::Basic: {
    auto *B = cast<BasicType>(T);
    if (B->Unit && B->Unit->HasParam)
      for (const UnitDim &D : B->Unit->Dims)
        for (TypeParamType *P : TPs)
          if (D.Param && P == D.Param)
            return true;
    return false;
  }
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
    if (I->Extends && mentionsAny(C, I->Extends, TPs, Depth + 1))
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
    // GEN-14: a unit parameter with a power other than 1 or -1 is not solved.
    std::string PowerHint;
    if (auto *PB = dyn_cast<BasicType>(TPar);
        PB && PB->Unit && PB->Unit->HasParam)
      for (const UnitDim &D : PB->Unit->Dims)
        if (D.Param && (D.Exp > 1 || D.Exp < -1))
          PowerHint = "; the unit parameter " + str(D.Param) +
                      " has the power " + std::to_string(D.Exp) +
                      ", which is not inferred: give it explicitly, as in "
                      "f[KM](...)";
    if (!Inf[0]) {
      bool AllFailed = true;
      for (Type *T : Inf)
        if (T)
          AllFailed = false;
      if (AllFailed)
        return Fail(A.pos(), "type " + str(TArg) + " of " + str(A.E) +
                                 " does not match " + str(TPar) +
                                 " (cannot infer " + typeParamsString(TParams) +
                                 ")" + PowerHint);
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
  // GEN-14: an argument whose unit has two unknown unit parameters (`U/V`)
  // cannot give either until the other is known; it waits for the others,
  // so the order of the arguments does not matter.
  SmallVector<TypeParamType *, 4> UnitTPs;
  if (Ctx.unitsSeen())
    for (TypeParamType *TP : TParams)
      if (isUnitParam(TP))
        UnitTPs.push_back(TP);
  SmallVector<size_t, 4> Untyped, Deferred;
  // False if the argument I does not unify with its parameter.
  auto UnifyArg = [&](size_t I) {
    const Operand &A = Args[I];
    Type *PT = Params->Elems[I];
    // EXP-31: `2.0[M]` is untyped in its number part only; its unit takes
    // part in inference like the unit of a typed argument.
    if (!isTyped(A.Ty) && unitOf(A.Ty))
      if (auto *PB = dyn_cast<BasicType>(PT);
          PB && PB->Unit && (PB->Info & IsNumeric))
        return U.unify(PT, Ctx.unitType(PB->BK, unitOf(A.Ty)), UAssign);
    if (isTyped(A.Ty)) {
      // OWN-V3: an owned (or borrowed) value converts to a view implicitly
      // wherever a view is expected, so a View[X] parameter takes an
      // argument of type X.
      Type *P = PT;
      if (auto *R = dyn_cast<RefType>(PT);
          own() && R && !R->Borrowed && !isa<RefType>(under(A.Ty)))
        P = R->Elem;
      return U.unify(P, A.Ty, UAssign);
    }
    if (isa<TypeParamType>(PT) && !A.isNil())
      Untyped.push_back(I);
    return true;
  };
  for (size_t I = 0; I < Args.size() && I < Params->size(); ++I) {
    const Operand &A = Args[I];
    Type *PT = Params->Elems[I];
    if (A.M == Mode::Invalid)
      continue;
    if (!mentionsAny(*this, PT, TParams) && !mentionsAny(*this, A.Ty, TParams))
      continue;
    if (!UnifyArg(I)) {
      if (UnitTPs.empty() || !mentionsAny(*this, PT, UnitTPs))
        return Mismatch(PT, A.Ty, A);
      Deferred.push_back(I);
    }
  }
  for (bool Progress = !Deferred.empty(); Progress;) {
    Progress = false;
    for (size_t K = 0; K < Deferred.size();)
      if (UnifyArg(Deferred[K])) {
        Deferred.erase(Deferred.begin() + K);
        Progress = true;
      } else {
        ++K;
      }
  }
  if (!Deferred.empty())
    return Mismatch(Params->Elems[Deferred[0]], Args[Deferred[0]].Ty,
                    Args[Deferred[0]]);

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
      // GEN-12 (R3): `T extends Box[U]` with T known gives U, from the
      // ancestor of T that is an instance of Box (`IntBox extends Box[int]`).
      // When the bound has nothing to infer, the check at instantiation
      // reports a mismatch.
      if (TX && !S.Opts.GoCompat && !mentionsAny(*this, TX, TParams))
        if (auto *CI = dyn_cast_or_null<InterfaceType>(under(TP)))
          if (Type *Ext = typeSet(CI)->Extends;
              Ext && mentionsAny(*this, Ext, TParams))
            if (auto *EN = dyn_cast<NamedType>(Ext); EN && EN->Orig)
              if (auto *TN = dyn_cast<NamedType>(TX))
                if (NamedType *A = ancestorInstanceOf(TN, EN->Orig))
                  if (!U.unify(A, Ext, UExact))
                    return Fail(Pos, str(TP) + " (type " + str(TX) +
                                         ") does not satisfy " +
                                         str(TP->Bound));
      // The type argument must implement the methods of the constraint,
      // possibly after unification of the method signatures.
      if (TX && !mentionsAny(*this, TX, TParams)) {
        auto *CI = dyn_cast_or_null<InterfaceType>(under(TP));
        if (CI)
          for (Func *M : typeSet(CI)->Methods) {
            if (M->isOperator()) {
              // GEN-13: an operator element is met by operator and
              // signature, which the instantiation check decides
              // (satisfiesOpElem). The one method of that operator is
              // unified, so a type parameter only the element mentions is
              // inferred; an overload set guesses nothing.
              SmallVector<Func *, 4> Cs;
              opCandidates(TX, M->Op, Cs);
              if (Cs.size() == 1 && Cs[0]->Ty)
                U.unify(Cs[0]->Ty, M->Ty, UExact);
              continue;
            }
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
