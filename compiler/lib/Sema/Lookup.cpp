//===- Lookup.cpp - Field and method lookup, interface satisfaction -------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's lookup.go (BSD-style license, Copyright The Go
// Authors): LookupFieldOrMethod, missingMethod, implements.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/SmallPtrSet.h"

#include "llvm/ADT/StringExtras.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

bool Checker::sameId(const Object *O, Package *Pkg, StringRef Name, bool Fold) {
  // As in types2: when the case does not matter, the package does not either.
  // (The package of an exported struct field is not stored: uniqued types
  // carry no body-specific data, see TypeContext::unique.)
  if (Fold && O->Name.equals_insensitive(Name))
    return true;
  if (O->Name != Name)
    return false;
  // Exported names and names of foreign packages do not depend on the package.
  if (isExportedName(Name))
    return true;
  if ((O->Pkg && O->Pkg->IsForeign) || (Pkg && Pkg->IsForeign))
    return true;
  return O->Pkg == Pkg;
}

unsigned Checker::numMethods(NamedType *N) {
  NamedType *O = N->origin();
  return O->Methods ? O->Methods->size() : 0;
}

Func *Checker::namedMethod(NamedType *N, unsigned I) {
  if (!N->Orig)
    return (*N->Methods)[I];
  return instantiatedMethod(N, I);
}

Func *Checker::lookupMethod(NamedType *N, Package *Pkg, StringRef Name,
                            bool Fold, int *Idx) {
  NamedType *O = N->origin();
  if (!O->Methods)
    return nullptr;
  for (unsigned I = 0; I < O->Methods->size(); ++I) {
    Func *M = (*O->Methods)[I];
    if (sameId(M, Pkg, Name, Fold)) {
      if (Idx)
        *Idx = static_cast<int>(I);
      return N->Orig ? instantiatedMethod(N, I) : M;
    }
  }
  return nullptr;
}

Checker::LookupResult Checker::lookupFieldOrMethod(Type *T, bool Addressable,
                                                   Package *Pkg, StringRef Name,
                                                   bool FoldCase) {
  LookupResult R;
  if (Name == "_")
    return R;
  // Methods cannot be associated with a named pointer type: look in the
  // pointer, and drop a method found there.
  if (auto *N = dyn_cast<NamedType>(T)) {
    if (auto *P = dyn_cast<PointerType>(under(N))) {
      R = lookupImpl(P, false, Pkg, Name, FoldCase);
      if (R.Obj && isa<Func>(R.Obj))
        return LookupResult();
      return R;
    }
  }
  return lookupImpl(T, Addressable, Pkg, Name, FoldCase);
}

namespace {
struct Embedded {
  Type *Ty;
  SmallVector<int, 4> Index;
  bool Indirect;
  bool Multiples;
};
} // namespace

Checker::LookupResult Checker::lookupImpl(Type *T, bool Addressable,
                                          Package *Pkg, StringRef Name,
                                          bool FoldCase) {
  LookupResult R;
  if (Name == "_")
    return R;
  bool IsPtr;
  Type *Ty = deref(T, IsPtr);
  // *typ where typ is an interface (incl. a type parameter) has no methods.
  if (IsPtr && isa<InterfaceType>(under(Ty)))
    return R;

  SmallVector<Embedded, 4> Current;
  Current.push_back({Ty, {}, IsPtr, false});
  SmallVector<NamedType *, 8> Seen;

  auto Concat = [](const SmallVector<int, 4> &A, int I) {
    SmallVector<int, 4> Out(A.begin(), A.end());
    Out.push_back(I);
    return Out;
  };

  while (!Current.empty()) {
    SmallVector<Embedded, 4> Next;
    for (Embedded &E : Current) {
      Type *Tp = E.Ty;
      if (auto *N = dyn_cast<NamedType>(Tp)) {
        bool Dup = false;
        for (NamedType *S : Seen)
          if (S == N) {
            Dup = true;
            break;
          }
        if (Dup)
          continue;
        Seen.push_back(N);
        int Idx = 0;
        if (Func *M = lookupMethod(N, Pkg, Name, FoldCase, &Idx)) {
          SmallVector<int, 4> Index = Concat(E.Index, Idx);
          if (R.Obj || E.Multiples) {
            LookupResult Coll;
            Coll.Index = Index;
            Coll.Ambiguous = true;
            return Coll;
          }
          R.Obj = M;
          R.Index = Index;
          R.Indirect = E.Indirect;
          continue;
        }
      }
      Type *U = under(Tp);
      if (auto *St = dyn_cast<StructType>(U)) {
        for (size_t I = 0; I < St->Fields.size(); ++I) {
          Var *F = St->Fields[I];
          if (sameId(F, Pkg, Name, FoldCase)) {
            SmallVector<int, 4> Index = Concat(E.Index, static_cast<int>(I));
            if (R.Obj || E.Multiples) {
              LookupResult Coll;
              Coll.Index = Index;
              Coll.Ambiguous = true;
              return Coll;
            }
            R.Obj = F;
            R.Index = Index;
            R.Indirect = E.Indirect;
            continue;
          }
          if (!R.Obj && F->Embedded) {
            bool P;
            Type *FT = deref(F->Ty, P);
            Next.push_back({FT, Concat(E.Index, static_cast<int>(I)),
                            E.Indirect || P, E.Multiples});
          }
        }
      } else if (auto *In = dyn_cast<InterfaceType>(U)) {
        int Idx = 0;
        if (Func *M =
                lookupMethodInSet(typeSet(In), Pkg, Name, FoldCase, &Idx)) {
          SmallVector<int, 4> Index = Concat(E.Index, Idx);
          if (R.Obj || E.Multiples) {
            LookupResult Coll;
            Coll.Index = Index;
            Coll.Ambiguous = true;
            return Coll;
          }
          R.Obj = M;
          R.Index = Index;
          R.Indirect = E.Indirect;
        }
      }
    }
    if (R.Obj) {
      if (auto *F = dyn_cast<Func>(R.Obj)) {
        if (F->HasPtrRecv && !R.Indirect && !Addressable) {
          LookupResult PR;
          PR.Indirect = true; // pointer/addressable receiver required
          return PR;
        }
      }
      return R;
    }
    // Consolidate multiples: types that appear more than once at this depth.
    SmallVector<Embedded, 4> Cons;
    for (Embedded &E : Next) {
      bool Found = false;
      for (Embedded &C : Cons)
        if (identical(C.Ty, E.Ty)) {
          C.Multiples = true;
          Found = true;
          break;
        }
      if (!Found)
        Cons.push_back(std::move(E));
    }
    Current = std::move(Cons);
  }
  return R;
}

//===----------------------------------------------------------------------===//
// Interface satisfaction
//===----------------------------------------------------------------------===//

std::string Checker::funcString(const Func *F, bool) {
  std::string Name(F->Name);
  std::string Sig = typeString(F->Ty, S.Pkg);
  if (StringRef(Sig).starts_with("func"))
    Sig = Sig.substr(4);
  return Name + Sig;
}

Func *Checker::missingMethod(Type *V, Type *T, bool Static,
                             const std::function<bool(Type *, Type *)> &Equiv,
                             std::string *Cause, bool *WrongTypeOut) {
  auto *TI = dyn_cast<InterfaceType>(under(T));
  if (!TI)
    return nullptr;
  TypeSet *TTS = typeSet(TI);
  if (TTS->Methods.empty())
    return nullptr;
  enum {
    Ok,
    NotFound,
    WrongName,
    Unexported,
    WrongSig,
    AmbigSel,
    PtrRecv,
    FieldSt,
    MoveRecv,
    GenericMethod
  } State = Ok;
  Func *M = nullptr; // method of T we try to implement
  Func *F = nullptr; // method of V found
  if (auto *VI = dyn_cast<InterfaceType>(under(V))) {
    TypeSet *VTS = typeSet(VI);
    for (Func *Mi : TTS->Methods) {
      M = Mi;
      F = lookupMethodInSet(VTS, M->Pkg, M->Name, false);
      if (!F) {
        if (!Static)
          continue;
        State = NotFound;
        break;
      }
      if (!Equiv(F->Ty, M->Ty)) {
        State = WrongSig;
        break;
      }
    }
  } else {
    for (Func *Mi : TTS->Methods) {
      M = Mi;
      LookupResult LR = lookupImpl(V, false, M->Pkg, M->Name, false);
      if (!LR.Obj) {
        if (LR.Ambiguous)
          State = AmbigSel;
        else if (LR.Indirect)
          State = PtrRecv;
        else {
          State = NotFound;
          LookupResult Fold = lookupImpl(V, false, M->Pkg, M->Name, true);
          F = Fold.Obj ? dyn_cast<Func>(Fold.Obj) : nullptr;
          if (F) {
            State = WrongName;
            if (F->Name == M->Name)
              State = Unexported;
          }
        }
        break;
      }
      F = dyn_cast<Func>(LR.Obj);
      if (!F) {
        State = FieldSt;
        break;
      }
      objDecl(F, nullptr);
      if (own() && !F->OwnTParams.empty()) { // GEN-7
        State = GenericMethod;
        break;
      }
      if (!Equiv(F->Ty, M->Ty)) {
        State = WrongSig;
        break;
      }
      if (own() && F->MoveRecv) { // PAR-5: move receivers are in no method set
        State = MoveRecv;
        break;
      }
    }
  }
  if (State == Ok)
    return nullptr;
  if (WrongTypeOut)
    *WrongTypeOut = State == WrongSig;
  if (Cause) {
    std::string Mn(M->Name);
    switch (State) {
    case NotFound:
      *Cause = "(missing method " + Mn + ")";
      break;
    case WrongName:
      *Cause = "(missing method " + Mn + ")\n\t\thave " + funcString(F) +
               "\n\t\twant " + funcString(M);
      break;
    case Unexported:
      *Cause = "(unexported method " + Mn + ")";
      break;
    case WrongSig: {
      *Cause = "(wrong type for method " + Mn + ")\n\t\thave " + funcString(F) +
               "\n\t\twant " + funcString(M);
      break;
    }
    case AmbigSel:
      *Cause = "(ambiguous selector " + str(V) + "." + Mn + ")";
      break;
    case PtrRecv:
      *Cause = "(method " + Mn + " has pointer receiver)";
      break;
    case FieldSt:
      *Cause = "(" + str(V) + "." + Mn + " is a field, not a method)";
      break;
    case MoveRecv:
      *Cause = "(method " + Mn + " has a move receiver, PAR-5)";
      break;
    case GenericMethod:
      *Cause = "(method " + Mn +
               " is generic: a generic method never satisfies an interface "
               "method, GEN-7)";
      break;
    case Ok:
      break;
    }
  }
  return M;
}

/// types2 hasInvalidEmbeddedFields: a struct (or pointer to one) with an
/// embedded field of invalid type, directly or through embedded structs.
static bool hasInvalidEmbeddedFields(Checker &C, Type *T,
                                     llvm::SmallPtrSetImpl<Type *> &Seen) {
  auto *St = dyn_cast<StructType>(C.under(C.derefStructPtr(T)));
  if (!St || !Seen.insert(St).second)
    return false;
  for (Var *F : St->Fields)
    if (F->Embedded &&
        (!C.isValid(F->Ty) || hasInvalidEmbeddedFields(C, F->Ty, Seen)))
      return true;
  return false;
}

bool Checker::hasAllMethods(Type *V, Type *T, bool Static, std::string *Cause) {
  // An invalid V, or a struct with an invalid embedded field (which may have
  // the method), implements T as far as we know: no follow-on errors.
  if (!isValid(V))
    return true;
  if (missingMethod(
          V, T, Static, [this](Type *A, Type *B) { return identical(A, B); },
          Cause) == nullptr)
    return true;
  llvm::SmallPtrSet<Type *, 4> Seen;
  return hasInvalidEmbeddedFields(*this, V, Seen);
}

bool Checker::mentions(Type *T, Type *Want) {
  // types2: syntactic, through the embedded elements and union terms only.
  if (auto *I = dyn_cast<InterfaceType>(T)) {
    for (Type *Em : I->Embeddeds)
      if (mentions(Em, Want))
        return true;
    return false;
  }
  if (auto *U = dyn_cast<UnionType>(T)) {
    for (const Term &Tm : U->Terms)
      if (mentions(Tm.T, Want))
        return true;
    return false;
  }
  return identical(T, Want);
}

bool Checker::implements(SourceLoc, Type *V, Type *T, bool Constraint,
                         std::string *Cause) {
  Type *Vu = under(V);
  Type *Tu = under(T);
  if (!isValid(Vu) || !isValid(Tu))
    return true;
  if (auto *P = dyn_cast<PointerType>(Vu))
    if (!isValid(under(P->Elem)))
      return true;
  const char *Verb = Constraint ? "satisfy" : "implement";
  auto *TI = dyn_cast<InterfaceType>(Tu);
  if (!TI) {
    if (Cause)
      *Cause = str(T) + " is not an interface";
    return false;
  }
  TypeSet *TTS = typeSet(TI);
  if (TTS->isAll())
    return true;
  auto *VI = dyn_cast<InterfaceType>(Vu);
  if (VI && typeSet(VI)->isEmpty())
    return true;
  if (TTS->isEmpty()) {
    if (Cause)
      *Cause =
          "cannot " + std::string(Verb) + " " + str(T) + " (empty type set)";
    return false;
  }
  std::string MCause;
  if (!hasAllMethods(V, T, true, &MCause)) {
    if (Cause)
      *Cause = str(V) + " does not " + Verb + " " + str(T) + " " + MCause;
    return false;
  }
  // Go++: Copy and Clone constraints (GEN-1, GEN-2).
  if (TTS->Copy && own() && !isCopyType(V)) {
    if (Cause)
      *Cause = str(V) + " does not " + Verb + " Copy";
    return false;
  }
  if (TTS->Clone && own()) {
    std::string Why;
    if (!satisfiesClone(V, &Why)) {
      if (Cause)
        *Cause = str(V) + " does not " + Verb + " Clone (" + Why + ")";
      return false;
    }
  }
  auto CheckComparability = [&]() {
    if (!TTS->Comparable)
      return true;
    if (comparableType(V, false, nullptr))
      return true;
    if (Constraint && comparableType(V, true, nullptr))
      return true;
    if (Cause)
      *Cause = str(V) + " does not " + Verb + " comparable";
    return false;
  };
  if (TTS->AllTerms)
    return CheckComparability();
  if (VI) {
    if (!typeSetSubsetOf(typeSet(VI), TTS)) {
      if (Cause)
        *Cause = str(V) + " does not " + Verb + " " + str(T);
      return false;
    }
    return CheckComparability();
  }
  // V's type must be included in the term list of T.
  Type *Alt = nullptr;
  bool Included = false;
  for (const Term &Tm : TTS->Terms) {
    if (termIncludes(Tm, V)) {
      Included = true;
      break;
    }
    if (!Alt && !Tm.Tilde && identical(Tm.T, under(Tm.T))) {
      Term TT = Tm;
      TT.Tilde = true;
      if (termIncludes(TT, V))
        Alt = Tm.T;
    }
  }
  if (!Included) {
    if (Cause) {
      std::string Detail;
      if (Alt)
        Detail = "possibly missing ~ for " + str(Alt) + " in " + str(T);
      else if (mentions(Tu, V))
        Detail = str(T) + " mentions " + str(V) + ", but " + str(V) +
                 " is not in the type set of " + str(T);
      else
        Detail = str(V) + " missing in " + termsString(TTS->Terms);
      *Cause =
          str(V) + " does not " + Verb + " " + str(T) + " (" + Detail + ")";
    }
    return false;
  }
  return CheckComparability();
}

bool Checker::assertableTo(Type *V, Type *T, std::string *Cause) {
  // No static check is possible if T is an interface.
  if (isInterface(T))
    return true;
  return hasAllMethods(T, V, false, Cause);
}
