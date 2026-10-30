//===- SemaSymbols.cpp - Symbol names of checked entities -----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What identifies a type in a symbol is decided in one place, build(), from
// the parts the checker's identity compares (Checker::identical in
// Predicates.cpp, StructType::Profile in Types.cpp). SemaSymbolsTest checks
// the two agree on a generated table of type pairs, with the checker as the
// oracle.
//
//===----------------------------------------------------------------------===//

#include "gopp/Mangle/SemaSymbols.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::sym;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace {

/// The kind of a predeclared type, or NumKinds if it has no symbol (untyped,
/// invalid).
Kind kindOf(BasicKind BK) {
  switch (BK) {
  case BasicKind::Bool:
    return Kind::Bool;
  case BasicKind::Int:
    return Kind::Int;
  case BasicKind::Int8:
    return Kind::Int8;
  case BasicKind::Int16:
    return Kind::Int16;
  case BasicKind::Int32:
    return Kind::Int32;
  case BasicKind::Int64:
    return Kind::Int64;
  case BasicKind::Uint:
    return Kind::Uint;
  case BasicKind::Uint8:
    return Kind::Uint8;
  case BasicKind::Uint16:
    return Kind::Uint16;
  case BasicKind::Uint32:
    return Kind::Uint32;
  case BasicKind::Uint64:
    return Kind::Uint64;
  case BasicKind::Uintptr:
    return Kind::Uintptr;
  case BasicKind::Float32:
    return Kind::Float32;
  case BasicKind::Float64:
    return Kind::Float64;
  case BasicKind::Complex64:
    return Kind::Complex64;
  case BasicKind::Complex128:
    return Kind::Complex128;
  case BasicKind::String:
    return Kind::String;
  case BasicKind::UnsafePointer:
    return Kind::UnsafePointer;
  default:
    return Kind::NumKinds;
  }
}

/// The package of an unexported name takes part in its identity.
llvm::StringRef pkgOfUnexported(llvm::StringRef Name, const Package *P) {
  return (!isExportedName(Name) && P) ? P->Path : llvm::StringRef();
}

bool isLocal(const TypeName *Obj) {
  return Obj->Pkg && Obj->Parent && Obj->Parent != Obj->Pkg->PkgScope;
}

} // namespace

const Node *SemaNames::unitImpl(const UnitCanon *U, bool TParams,
                                std::string &Reason) {
  if (U->HasParam && !TParams) {
    Reason = "a unit with a unit type parameter";
    return nullptr;
  }
  if (!U->Factor.isRat()) {
    Reason = "a unit factor that is not exact";
    return nullptr;
  }
  llvm::SmallString<32> Num, Den;
  U->Factor.ratNum().toString(Num, 10, true);
  U->Factor.ratDen().toString(Den, 10, false);
  llvm::SmallVector<const Node *, 4> Dims;
  for (const UnitDim &D : U->Dims) {
    if (D.Unit) {
      Dims.push_back(C.dim(D.Unit->Pkg ? D.Unit->Pkg->Path : llvm::StringRef(),
                           D.Unit->Name, D.Exp));
    } else if (D.Param && TParams) {
      Dims.push_back(C.paramDim(D.Param->Index, D.Exp));
    } else {
      Reason = "a unit with a unit type parameter";
      return nullptr;
    }
  }
  return C.unit(Num, Den, Dims);
}

void SemaNames::declareLocalType(const TypeName *Obj, const Node *Scope,
                                 uint64_t Ordinal) {
  LocalTypes[Obj] = {Scope, Ordinal};
}

const Node *SemaNames::unit(const UnitCanon *U) {
  std::string Reason;
  const Node *N = unitImpl(U, false, Reason);
  Why = std::move(Reason);
  return N;
}

bool SemaNames::children(const Type *T, bool TParams,
                         llvm::SmallVectorImpl<const Type *> &Out,
                         std::string &Reason) {
  switch (T->getKind()) {
  case Type::Kind::Basic:
    return true;
  case Type::Kind::Pointer:
    Out.push_back(cast<PointerType>(T)->Elem);
    return true;
  case Type::Kind::Slice:
    Out.push_back(cast<SliceType>(T)->Elem);
    return true;
  case Type::Kind::Array:
    Out.push_back(cast<ArrayType>(T)->Elem);
    return true;
  case Type::Kind::Ref:
    Out.push_back(cast<RefType>(T)->Elem);
    return true;
  case Type::Kind::Option:
    Out.push_back(cast<OptionType>(T)->Elem);
    return true;
  case Type::Kind::Chan:
    Out.push_back(cast<ChanType>(T)->Elem);
    return true;
  case Type::Kind::Map:
    Out.push_back(cast<MapType>(T)->Key);
    Out.push_back(cast<MapType>(T)->Elem);
    return true;
  case Type::Kind::Tuple:
    for (const Type *E : cast<TupleType>(T)->Elems)
      Out.push_back(E);
    return true;
  case Type::Kind::Signature: {
    auto *S = cast<SignatureType>(T);
    if (!S->TParams.empty()) {
      Reason = "a generic signature";
      return false;
    }
    Out.push_back(S->Params);
    Out.push_back(S->Results);
    return true;
  }
  case Type::Kind::Struct:
    for (const Var *F : cast<StructType>(T)->Fields)
      Out.push_back(F->Ty);
    return true;
  case Type::Kind::Interface: {
    auto *I = cast<InterfaceType>(T);
    if (I->Sp != InterfaceType::None || I->Implicit || I->Extends) {
      Reason = "an interface that is a constraint";
      return false;
    }
    // The method set: the declared methods and those of the embedded
    // interfaces, by name; never the checker's cached type set, which exists
    // or not depending on who asked first.
    llvm::SmallVector<const Func *, 4> Ms;
    llvm::SmallVector<const Type *, 4> Work{I};
    llvm::SmallPtrSet<const Type *, 4> Seen;
    while (!Work.empty()) {
      const Type *W = Work.pop_back_val();
      if (!Seen.insert(W).second)
        continue;
      if (auto *N = dyn_cast<NamedType>(W)) {
        if (!N->Underlying) {
          Reason = "an embedded interface that is not resolved";
          return false;
        }
        W = N->Underlying;
      }
      auto *WI = dyn_cast<InterfaceType>(W);
      if (!WI || WI->Sp != InterfaceType::None || WI->Extends) {
        Reason = "an interface that embeds a type set (a constraint)";
        return false;
      }
      for (const Func *M : WI->Methods) {
        // An operator element makes the interface a constraint (TYP-27);
        // its overloads would also share a name below.
        if (M->isOperator()) {
          Reason = "an interface with an operator element (a constraint)";
          return false;
        }
        Ms.push_back(M);
      }
      for (const Type *E : WI->Embeddeds)
        Work.push_back(E);
    }
    std::stable_sort(Ms.begin(), Ms.end(), [](const Func *X, const Func *Y) {
      return std::make_pair(pkgOfUnexported(X->Name, X->Pkg), X->Name) <
             std::make_pair(pkgOfUnexported(Y->Name, Y->Pkg), Y->Name);
    });
    Ms.erase(std::unique(Ms.begin(), Ms.end(),
                         [](const Func *X, const Func *Y) {
                           return X->Name == Y->Name &&
                                  pkgOfUnexported(X->Name, X->Pkg) ==
                                      pkgOfUnexported(Y->Name, Y->Pkg);
                         }),
             Ms.end());
    for (const Func *M : Ms)
      Out.push_back(M->Ty);
    Methods[T] = std::move(Ms);
    return true;
  }
  case Type::Kind::Union:
    Reason = "a union (a constraint)";
    return false;
  case Type::Kind::TypeParam:
    if (TParams)
      return true;
    Reason = "a type parameter";
    return false;
  case Type::Kind::Named: {
    auto *N = cast<NamedType>(T);
    if (!N->Obj) {
      Reason = "a type without a declaration";
      return false;
    }
    if (isLocal(N->Obj) || (N->Orig && isLocal(N->Orig->Obj))) {
      if (N->Orig || !LocalTypes.count(N->Obj)) {
        Reason = "a type declared inside a function";
        return false;
      }
    }
    if (!N->Orig && !N->TParams.empty()) {
      Reason = "a generic type without type arguments";
      return false;
    }
    for (const Type *A : N->TArgs)
      Out.push_back(A);
    return true;
  }
  }
  Reason = "a type that has no symbol";
  return false;
}

const Node *SemaNames::build(const Type *T, bool TParams, std::string &Reason) {
  auto Kid = [&](const Type *K) { return Memo[TParams].lookup(K); };
  switch (T->getKind()) {
  case Type::Kind::Basic: {
    // `byte` and `rune` are uint8 and int32 (one type, another spelling).
    auto *B = cast<BasicType>(T);
    if (B->BK == BasicKind::UnitArg) {
      if (!B->Unit) {
        Reason = "a unit argument without a unit";
        return nullptr;
      }
      return unitImpl(B->Unit, TParams, Reason);
    }
    Kind K = kindOf(B->BK);
    if (K == Kind::NumKinds) {
      Reason = "an untyped or invalid type";
      return nullptr;
    }
    if (B->Unit) {
      const Node *U = unitImpl(B->Unit, TParams, Reason);
      return U ? C.unitType(K, U) : nullptr;
    }
    return C.basic(K);
  }
  case Type::Kind::Pointer:
    return C.pointer(Kid(cast<PointerType>(T)->Elem));
  case Type::Kind::Slice:
    return C.slice(Kid(cast<SliceType>(T)->Elem));
  case Type::Kind::Array: {
    auto *A = cast<ArrayType>(T);
    if (A->Len < 0) {
      Reason = "an array of unknown length";
      return nullptr;
    }
    return C.array(Kid(A->Elem), static_cast<uint64_t>(A->Len));
  }
  case Type::Kind::Ref: {
    auto *R = cast<RefType>(T);
    return R->Borrowed ? C.borrowed(Kid(R->Elem)) : C.view(Kid(R->Elem));
  }
  case Type::Kind::Option:
    return C.option(Kid(cast<OptionType>(T)->Elem));
  case Type::Kind::Chan: {
    auto *Ch = cast<ChanType>(T);
    char D = Ch->D == ChanType::Send   ? 's'
             : Ch->D == ChanType::Recv ? 'r'
                                       : 'b';
    return C.chan(D, Kid(Ch->Elem));
  }
  case Type::Kind::Map: {
    auto *M = cast<MapType>(T);
    return C.map(Kid(M->Key), Kid(M->Elem));
  }
  case Type::Kind::Tuple: {
    llvm::SmallVector<const Node *, 4> Es;
    for (const Type *E : cast<TupleType>(T)->Elems)
      Es.push_back(Kid(E));
    return C.tuple(Es);
  }
  case Type::Kind::Signature: {
    // The `!` mark is not part of the identity (ERR-2): the results already
    // end with `error`.
    auto *S = cast<SignatureType>(T);
    return C.signature(Kid(S->Params), Kid(S->Results),
                       S->Variadic ? SigVariadic : 0, S->MoveMask);
  }
  case Type::Kind::Struct: {
    auto *S = cast<StructType>(T);
    llvm::SmallVector<const Node *, 8> Fs;
    for (const Var *F : S->Fields) {
      // Every mark the identity compares.
      uint64_t Fl = (F->Embedded ? FieldEmbedded : 0) |
                    (F->IsParent ? FieldParent : 0) |
                    (F->Required ? FieldRequired : 0) |
                    (F->MarkedRequired ? FieldMarkedRequired : 0) |
                    (F->RequiredSetOnly ? FieldSetOnly : 0) |
                    (F->MarkedOption ? FieldOption : 0);
      Fs.push_back(C.field(F->Name, pkgOfUnexported(F->Name, F->Pkg),
                           Kid(F->Ty), Fl, F->Tag));
    }
    return C.structOf(Fs, S->Strict);
  }
  case Type::Kind::Interface: {
    llvm::SmallVector<const Node *, 8> Ms;
    for (const Func *M : Methods[T])
      Ms.push_back(
          C.imethod(M->Name, pkgOfUnexported(M->Name, M->Pkg), Kid(M->Ty)));
    return C.interfaceOf(Ms);
  }
  case Type::Kind::Named: {
    auto *N = cast<NamedType>(T);
    const TypeName *O = N->origin()->Obj;
    if (auto L = LocalTypes.find(O); L != LocalTypes.end() && !N->Orig)
      return C.localNamed(L->second.first, L->second.second, O->Name);
    const Node *Base =
        O->Pkg ? C.named(pathOf(O->Pkg), O->Name) : C.universe(O->Name);
    if (N->TArgs.empty())
      return Base;
    llvm::SmallVector<const Node *, 4> Args;
    for (const Type *A : N->TArgs)
      Args.push_back(Kid(A));
    return C.instance(Base, Args);
  }
  case Type::Kind::TypeParam:
    if (TParams)
      return C.typeParam(cast<TypeParamType>(T)->Index);
    break;
  case Type::Kind::Union:
    break;
  }
  Reason = "a type that has no symbol";
  return nullptr;
}

const Node *SemaNames::convert(const Type *Root, bool TParams) {
  auto &M = Memo[TParams];
  auto &Bad = Reasons[TParams];
  llvm::SmallVector<const Type *, 64> Stack{Root};
  llvm::SmallVector<const Type *, 8> Kids;
  while (!Stack.empty()) {
    const Type *T = Stack.back();
    ++Counts.Visits;
    if (M.count(T)) {
      Stack.pop_back();
      continue;
    }
    Kids.clear();
    std::string Reason;
    ++Counts.Misses;
    if (!children(T, TParams, Kids, Reason)) {
      Stack.pop_back();
      M[T] = nullptr;
      Bad[T] = std::move(Reason);
      continue;
    }
    // A part without a node: T has none, for the same reason.
    const Type *BadKid = nullptr;
    for (const Type *K : Kids) {
      auto It = M.find(K);
      if (It != M.end() && !It->second) {
        BadKid = K;
        break;
      }
    }
    if (BadKid) {
      Stack.pop_back();
      M[T] = nullptr;
      std::string KidReason = Bad.lookup(BadKid);
      Bad[T] = std::move(KidReason);
      continue;
    }
    bool Ready = true;
    for (const Type *K : Kids)
      if (!M.count(K)) {
        Stack.push_back(K);
        Ready = false;
      }
    if (!Ready)
      continue;
    Stack.pop_back();
    const Node *N = build(T, TParams, Reason);
    M[T] = N;
    if (!N)
      Bad[T] = std::move(Reason);
  }
  if (const Node *N = M.lookup(Root))
    return N;
  return fail(Bad.lookup(Root));
}

const Node *SemaNames::type(const Type *T) {
  Why.clear();
  return convert(T, false);
}

const Node *SemaNames::opParams(const Func *F) {
  // The declaration: an instantiated method's origin. Its parameters name
  // the receiver's type parameters by index (an operator method has none of
  // its own, EXP-30), so the name does not depend on the instance.
  const Func *D = F->Origin ? F->Origin : F;
  auto *Sig = dyn_cast_or_null<SignatureType>(D->Ty);
  if (!Sig)
    return fail("a method that is not declared yet");
  return convert(Sig->Params, true);
}

const Node *SemaNames::funcImpl(const Func *F, bool Generic) {
  if (F->IsInterfaceMethod || F->IsBuiltinMethod)
    return fail("not a function with a body");
  // A blank function or method is never called, and a package (a type) may
  // have many: one name for all of them would collide.
  if (F->Name == "_")
    return fail("a blank function (never emitted)");
  auto *Sig = dyn_cast_or_null<SignatureType>(F->Ty);
  if (!Sig)
    return fail("a function that is not declared yet");
  if (!Generic && (!Sig->TParams.empty() || !F->OwnTParams.empty()))
    return fail("a generic function (name an instance)");
  if (!F->Recv) {
    if (!F->Pkg)
      return fail("a function without a package");
    // A package may have many `init` functions, none in its scope.
    if (F->Name == "init")
      return fail("an init function (name it with Context::initFunc and its "
                  "ordinal)");
    return C.func(pathOf(F->Pkg), F->Name);
  }
  const NamedType *R = F->RecvInst;
  if (!R) {
    const Type *RT = F->Recv->Ty;
    if (auto *P = dyn_cast_or_null<PointerType>(RT))
      RT = P->Elem;
    R = dyn_cast_or_null<NamedType>(RT);
    if (!R)
      return fail("a method of a type that is not a defined type");
    if (!R->Orig && !R->TParams.empty() && !Generic)
      return fail("a method of a generic type (name an instance)");
  }
  const Node *Recv = convert(R, false);
  if (!Recv)
    return nullptr;
  if (!F->isOperator())
    return C.method(Recv, F->Name);
  const Node *Params = opParams(F);
  return Params ? C.method(Recv, F->Name, Params, F->Right) : nullptr;
}

const Node *SemaNames::method(const NamedType *Recv, const Func *Decl) {
  Why.clear();
  if (Decl->Name == "_")
    return fail("a blank function (never emitted)");
  const Node *R = convert(Recv, false);
  if (!R)
    return nullptr;
  if (!Decl->isOperator())
    return C.method(R, Decl->Name);
  const Node *Params = opParams(Decl);
  return Params ? C.method(R, Decl->Name, Params, Decl->Right) : nullptr;
}

const Node *SemaNames::func(const Func *F) {
  Why.clear();
  return funcImpl(F, false);
}

const Node *SemaNames::funcInstance(const Func *F,
                                    llvm::ArrayRef<Type *> TArgs) {
  Why.clear();
  const Node *Base = funcImpl(F, true);
  if (!Base)
    return nullptr;
  // One argument per type parameter of its own: never a name of the generic
  // code itself.
  size_t Own = std::max(F->sig()->TParams.size(), F->OwnTParams.size());
  if (TArgs.size() != Own)
    return fail("an instance needs one type argument per type parameter");
  llvm::SmallVector<const Node *, 4> Args;
  for (const Type *A : TArgs) {
    const Node *N = convert(A, false);
    if (!N)
      return nullptr;
    Args.push_back(N);
  }
  if (Args.empty())
    return Base;
  return C.instance(Base, Args);
}

const Node *SemaNames::var(const Var *V) {
  Why.clear();
  if (!V->IsPackageLevel || !V->Pkg)
    return fail("not a package-level variable");
  // `var _ = f()` has no storage, and a package may have many.
  if (V->Name == "_")
    return fail("a blank variable (it has no storage)");
  return C.var(pathOf(V->Pkg), V->Name);
}
