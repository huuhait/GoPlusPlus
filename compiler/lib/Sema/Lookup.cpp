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

#include <algorithm>

#include "llvm/ADT/SmallPtrSet.h"

#include "llvm/ADT/StringExtras.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

bool Checker::sameId(const Object *O, Package *Pkg, StringRef Name, bool Fold) {
  // As in types2: when the case does not matter, the package does not either.
  // (The package of an exported struct field is not stored: uniqued types
  // carry no body-specific data, see TypeContext::unique.)
  if (Fold && O->Name.equals_insensitive(Name))
    return true;
  if (O->Name != Name)
    return false;
  // Exported names and names of foreign packages do not depend on the package,
  // nor do operator names (ADR-0024: importers get the operator methods).
  if (isExportedName(Name) || isOperatorSpelling(Name))
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

const Func *sema::declaredMethod(const Func *F) {
  while (F && F->Origin)
    F = F->Origin;
  return F;
}

Func *Checker::lookupMethod(NamedType *N, Package *Pkg, StringRef Name,
                            bool Fold, int *Idx, const Func *Want) {
  NamedType *O = N->origin();
  if (!O->Methods)
    return nullptr;
  const Func *WantDecl = declaredMethod(Want);
  for (unsigned I = 0; I < O->Methods->size(); ++I) {
    Func *M = (*O->Methods)[I];
    if (Want ? M == WantDecl : sameId(M, Pkg, Name, Fold)) {
      if (Idx)
        *Idx = static_cast<int>(I);
      return N->Orig ? instantiatedMethod(N, I) : M;
    }
  }
  return nullptr;
}

Checker::LookupResult Checker::lookupFieldOrMethod(Type *T, bool Addressable,
                                                   Package *Pkg, StringRef Name,
                                                   bool FoldCase,
                                                   const Func *Want) {
  LookupResult R;
  if (Name == "_")
    return R;
  // Methods cannot be associated with a named pointer type: look in the
  // pointer, and drop a method found there.
  if (auto *N = dyn_cast<NamedType>(T)) {
    if (auto *P = dyn_cast<PointerType>(under(N))) {
      R = lookupImpl(P, false, Pkg, Name, FoldCase, Want);
      if (R.Obj && isa<Func>(R.Obj))
        return LookupResult();
      return R;
    }
  }
  return lookupImpl(T, Addressable, Pkg, Name, FoldCase, Want);
}

namespace {
struct Embedded {
  Type *Ty;
  int Index; // a node of the path list in lookupImpl; -1 is the empty path
  bool Indirect;
  bool Multiples;
};
} // namespace

Checker::LookupResult Checker::lookupImpl(Type *T, bool Addressable,
                                          Package *Pkg, StringRef Name,
                                          bool FoldCase, const Func *Want) {
  LookupResult R;
  if (Name == "_")
    return R;
  bool IsPtr;
  Type *Ty = deref(T, IsPtr);
  // GEN-12: a type parameter bounded by `extends P` has the fields and
  // methods of P (also through a pointer).
  Type *Ext = nullptr;
  if (!S.Opts.GoCompat)
    if (auto *TP = dyn_cast<TypeParamType>(Ty))
      if (auto *BI = dyn_cast_or_null<InterfaceType>(under(TP)))
        Ext = typeSet(BI)->Extends;
  // *typ where typ is an interface (incl. a type parameter) has no methods.
  if (IsPtr && isa<InterfaceType>(under(Ty)) && !Ext)
    return R;

  // A type of an `extends` chain: its parents are found by the memo, one
  // step for each type of the chain however many lookups there are.
  if (!Ext && !S.Opts.GoCompat)
    if (auto *N = dyn_cast<NamedType>(Ty)) {
      LookupResult CR;
      if (!FoldCase) {
        if (chainLookup(N, IsPtr, Addressable, Pkg, Name, CR, Want))
          return CR;
      } else if (typesFinal() && !Ctx.memberNameSeenFold(Name) &&
                 parentOf(N->origin())) {
        return CR; // no member of any type has this name, up to case
      }
    }

  SmallVector<Embedded, 4> Current;
  Current.push_back({Ext ? Ext : Ty, -1, IsPtr, false});
  if (Ext && !IsPtr)
    Current.push_back({Ty, -1, false, false}); // the methods of the constraint
  llvm::SmallPtrSet<NamedType *, 8> Seen;

  // The paths to the embedded types share their prefixes: a node is a step
  // and the node before it. A path is made only for a result, so a chain of
  // a thousand parents costs a thousand nodes, not a thousand copies of a
  // growing vector.
  SmallVector<std::pair<int, int>, 16> Nodes;
  auto Step = [&Nodes](int Prev, int I) {
    Nodes.push_back({I, Prev});
    return static_cast<int>(Nodes.size()) - 1;
  };
  auto Path = [&Nodes](int Node, int Last) {
    SmallVector<int, 4> Out;
    Out.push_back(Last);
    for (; Node >= 0; Node = Nodes[Node].second)
      Out.push_back(Nodes[Node].first);
    std::reverse(Out.begin(), Out.end());
    return Out;
  };

  while (!Current.empty()) {
    SmallVector<Embedded, 4> Next;
    for (Embedded &E : Current) {
      Type *Tp = E.Ty;
      if (auto *N = dyn_cast<NamedType>(Tp)) {
        if (!Seen.insert(N).second)
          continue;
        int Idx = 0;
        if (Func *M = lookupMethod(N, Pkg, Name, FoldCase, &Idx, Want)) {
          SmallVector<int, 4> Index = Path(E.Index, Idx);
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
          if (!Want && sameId(F, Pkg, Name, FoldCase)) {
            SmallVector<int, 4> Index = Path(E.Index, static_cast<int>(I));
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
            Next.push_back({FT, Step(E.Index, static_cast<int>(I)),
                            E.Indirect || P, E.Multiples});
          }
        }
      } else if (auto *In = dyn_cast<InterfaceType>(U)) {
        int Idx = 0;
        if (Func *M = lookupMethodInSet(typeSet(In), Pkg, Name, FoldCase, &Idx,
                                        Want)) {
          SmallVector<int, 4> Index = Path(E.Index, Idx);
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
    GenericMethod,
    OpMissing
  } State = Ok;
  std::string OpCause;
  Func *M = nullptr; // method of T we try to implement
  Func *F = nullptr; // method of V found
  if (auto *VI = dyn_cast<InterfaceType>(under(V))) {
    TypeSet *VTS = typeSet(VI);
    for (Func *Mi : TTS->Methods) {
      M = Mi;
      // GEN-13: an operator element is met by operator and signature.
      if (M->isOperator()) {
        if (satisfiesOpElem(V, M, Equiv, &OpCause))
          continue;
        State = OpMissing;
        break;
      }
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
      if (M->isOperator()) {
        if (satisfiesOpElem(V, M, Equiv, &OpCause))
          continue;
        State = OpMissing;
        break;
      }
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
    case OpMissing:
      *Cause = OpCause;
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
  // GEN-12: `extends P`: the type set is P and every type that extends it.
  if (TTS->Extends && !extendsOrIs(V, TTS->Extends)) {
    if (Cause)
      *Cause = str(V) + " does not " + Verb + " " + str(T) + " (" + str(V) +
               " does not extend " + str(TTS->Extends) + ")";
    return false;
  }
  // Go++: Copy and Clone constraints (GEN-1, GEN-2).
  if (TTS->Copy && own() && (!isCopyType(V) || isTagged(V))) {
    // ADR-0023 decision 16: generic code copies a T through a *T freely, and
    // a *T of a type with a type table may point at a child.
    if (Cause)
      *Cause = str(V) + " does not " + Verb + " Copy" +
               (isTagged(V)
                    ? " (" + str(V) +
                          " has a type table, so a copy through a *" + str(V) +
                          " could cover only the parent part of a "
                          "child; ADR-0023)"
                    : "");
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

/// The chain name index (TypeContext::chainEntry) works on the ORIGINS of a
/// chain (the declarations: a generic chain is one chain, however many
/// instances it has, so a body of a generic method never makes the fresh
/// instances of its receiver's chain). Each origin has its depth, its
/// parent and a skew-binary jump pointer (as Checker::hierDepth), each with
/// its type arguments in terms of the origin's own parameters, so the
/// ancestor at any depth and its type arguments are O(log depth) away.
bool Checker::chainIndex(NamedType *O, TypeContext::ChainEntry &E) {
  if (Ctx.chainEntry(O, E))
    return true;
  if (!typesFinal())
    return false;
  // Up to the first origin entered (or the root), then down: each origin is
  // entered once. A ring (reported and cut by checkExtendsCycles) ends the
  // walk with no answer: Brent's method, so no set is filled.
  SmallVector<NamedType *, 16> Path;
  TypeContext::ChainEntry Top;
  NamedType *TopO = nullptr; // the first origin entered, if any
  NamedType *Mark = nullptr;
  unsigned Steps = 0, Limit = 1;
  for (NamedType *X = O; X;) {
    if (X == Mark || X->InstCycle || !declFinished(X))
      return false;
    if (Ctx.chainEntry(X, Top)) {
      TopO = X;
      break;
    }
    Path.push_back(X);
    if (++Steps == Limit) {
      Mark = X;
      Steps = 0;
      Limit *= 2;
    }
    NamedType *P = parentOf(X);
    X = P ? P->origin() : nullptr;
  }
  auto Args = [&](ArrayRef<Type *> As, NamedType *Of, ArrayRef<Type *> To) {
    SmallVector<Type *, 4> Out;
    for (Type *A : As)
      Out.push_back(subst(A, Of->TParams, To));
    return Ctx.copyTypes(Out);
  };
  TypeContext::ChainEntry Par = Top;
  NamedType *ParO = TopO;
  SmallVector<TypeContext::ChainMember, 8> Members;
  for (size_t K = Path.size(); K-- > 0;) {
    NamedType *X = Path[K];
    auto *St = dyn_cast_or_null<StructType>(under(X));
    TypeContext::ChainEntry Cur;
    if (ParO) {
      NamedType *PI = parentOf(X);
      Cur.Root = Par.Root;
      Cur.Depth = Par.Depth + 1;
      Cur.Plain = Par.Plain;
      Cur.Parent = ParO;
      Cur.ParentArgs = Ctx.copyTypes(PI->TArgs);
      // The jump of the parent's jump when the two jumps have the same
      // length, else the parent (Myers' skew-binary jump pointers).
      TypeContext::ChainEntry PJ, PJJ;
      Ctx.chainEntry(Par.Jump, PJ);
      Ctx.chainEntry(PJ.Jump, PJJ);
      if (Par.Depth - PJ.Depth == PJ.Depth - PJJ.Depth) {
        Cur.Jump = PJ.Jump;
        // PJ.Jump's arguments in terms of Par.Jump's, of ParO's, of X's.
        Cur.JumpArgs = Args(PJ.JumpArgs, Par.Jump,
                            Args(Par.JumpArgs, ParO, Cur.ParentArgs));
      } else {
        Cur.Jump = ParO;
        Cur.JumpArgs = Cur.ParentArgs;
      }
    } else {
      Cur.Root = X;
      Cur.Depth = 1;
      Cur.Plain = true;
      Cur.Jump = X;
      SmallVector<Type *, 4> Own(X->TParams.begin(), X->TParams.end());
      Cur.JumpArgs = Ctx.copyTypes(Own);
    }
    // Only the parent part may be embedded: the promoted members of any
    // other embedded field (a Go-mode parent) take the general walk.
    Cur.Plain = Cur.Plain && St;
    Members.clear();
    if (Cur.Plain) {
      if (X->Methods)
        for (unsigned I = 0; I < X->Methods->size(); ++I)
          Members.push_back({(*X->Methods)[I], static_cast<int>(I)});
      for (size_t I = 0; I < St->Fields.size(); ++I) {
        Var *F = St->Fields[I];
        if (F->Embedded && !F->IsParent)
          Cur.Plain = false;
        else
          Members.push_back({F, static_cast<int>(I)}); // the parent part too
      }
      if (!Cur.Plain)
        Members.clear();
    }
    Ctx.chainAdd(X, Cur, Members);
    Par = Cur;
    ParO = X;
  }
  return Ctx.chainEntry(O, E);
}

NamedType *Checker::chainAncestor(NamedType *O, TypeContext::ChainEntry E,
                                  uint32_t D,
                                  llvm::SmallVectorImpl<Type *> *Args) {
  // Args: the type arguments of the current origin X in terms of O's own
  // parameters; each step puts them in for X's parameters.
  if (Args)
    Args->assign(O->TParams.begin(), O->TParams.end());
  NamedType *X = O;
  while (E.Depth > D) {
    TypeContext::ChainEntry JE, Next;
    Ctx.chainEntry(E.Jump, JE);
    bool Jump = JE.Depth >= D;
    NamedType *To = Jump ? E.Jump : E.Parent;
    if (Args) {
      SmallVector<Type *, 4> Cur;
      for (Type *A : Jump ? E.JumpArgs : E.ParentArgs)
        Cur.push_back(subst(A, X->TParams, *Args));
      Args->swap(Cur);
    }
    if (Jump)
      Next = JE;
    else
      Ctx.chainEntry(To, Next);
    X = To;
    E = Next;
  }
  return X;
}

/// The lookup of Name in a type of an `extends` chain (single inheritance: the
/// parent part is the first field, and the only embedded one in .gopp). The
/// member is the first one up the chain; the path is that many zeros (the
/// parent field) and its index. The name index of the chain's origins gives
/// the depths at which its tree declares Name; the ancestor at the deepest of
/// them that has it is the owner (R19), and only the member found is put in
/// for N's type arguments. False if N is not such a type, or its types are
/// not final yet (the general walk then decides).
bool Checker::chainLookup(NamedType *N, bool IsPtr, bool Addressable,
                          Package *Pkg, StringRef Name, LookupResult &R,
                          const Func *Want) {
  if (!typesFinal())
    return false;
  NamedType *O = N->origin();
  // A name that no struct, interface or method list has: a miss at once.
  if (!Ctx.memberNameSeen(Name))
    return parentOf(O) != nullptr;
  if (!parentOf(O) || O->InstCycle ||
      (N->Orig && O->TParams.size() != N->TArgs.size()))
    return false;
  TypeContext::ChainEntry E;
  if (!chainIndex(O, E) || !E.Plain)
    return false;
  SmallVector<TypeContext::ChainMember, 2> Members;
  const Func *WantDecl = declaredMethod(Want);
  for (uint32_t D = Ctx.chainDepthAtMost(E.Root, Name, E.Depth); D;
       D = Ctx.chainDepthAtMost(E.Root, Name, D - 1)) {
    NamedType *A = chainAncestor(O, E, D, nullptr);
    Members.clear();
    Ctx.chainMembers(A, Name, Members);
    for (const TypeContext::ChainMember &M : Members) {
      if (Want ? M.Obj != WantDecl : !sameId(M.Obj, Pkg, Name, false))
        continue;
      Object *Mem = M.Obj;
      if (!A->TParams.empty()) {
        // The owner's instance: its arguments in terms of O's parameters,
        // then N's arguments put in.
        SmallVector<Type *, 4> Args;
        chainAncestor(O, E, D, &Args);
        if (N->Orig)
          for (Type *&T : Args)
            T = subst(T, O->TParams, N->TArgs);
        if (Args.size() != A->TParams.size())
          return false;
        NamedType *Owner = instantiateNamed(A, Args);
        if (isa<Var>(M.Obj)) {
          auto *St = dyn_cast_or_null<StructType>(under(Owner));
          if (!St || static_cast<size_t>(M.Idx) >= St->Fields.size())
            return false;
          Mem = St->Fields[M.Idx];
        } else {
          Mem = namedMethod(Owner, static_cast<unsigned>(M.Idx));
        }
      }
      R.Obj = Mem;
      R.Index.assign(1, M.Idx);
      R.ParentSteps = E.Depth - D;
      R.Indirect = IsPtr;
      if (auto *F = dyn_cast<Func>(R.Obj))
        if (F->HasPtrRecv && !R.Indirect && !Addressable) {
          R = LookupResult();
          R.Indirect = true; // pointer/addressable receiver required
        }
      return true;
    }
  }
  return true; // not found: R stays empty
}
