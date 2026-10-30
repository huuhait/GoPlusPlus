//===- TypeSet.cpp - Type sets of interfaces and constraints --------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's typeset.go, typeterm.go and termlist.go (BSD-style
// license, Copyright The Go Authors).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include <algorithm>
#include <tuple>

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

bool Checker::termIncludes(const Term &T, Type *V) {
  Type *U = V;
  if (T.Tilde)
    U = under(V);
  return identical(T.T, U);
}

bool Checker::termDisjoint(const Term &X, const Term &Y) {
  Type *UX = X.T;
  if (Y.Tilde)
    UX = under(UX);
  Type *UY = Y.T;
  if (X.Tilde)
    UY = under(UY);
  return !identical(UX, UY);
}

bool Checker::termSubsetOf(const Term &X, const Term &Y) {
  if (termDisjoint(X, Y))
    return false;
  return !X.Tilde || Y.Tilde;
}

static bool intersectTerm(Checker &C, const Term &X, const Term &Y, Term &Out) {
  if (C.termDisjoint(X, Y))
    return false;
  Out = (!X.Tilde || Y.Tilde) ? X : Y;
  return true;
}

static void normalize(Checker &C, TermList &L) {
  SmallVector<Term, 4> Out;
  for (size_t I = 0; I < L.Terms.size(); ++I) {
    bool Redundant = false;
    for (size_t J = 0; J < L.Terms.size() && !Redundant; ++J) {
      if (I == J)
        continue;
      if (C.termSubsetOf(L.Terms[I], L.Terms[J])) {
        // Keep the first of two equal terms.
        bool Equal = C.termSubsetOf(L.Terms[J], L.Terms[I]);
        Redundant = !Equal || J < I;
      }
    }
    if (!Redundant)
      Out.push_back(L.Terms[I]);
  }
  L.Terms = std::move(Out);
}

TermList Checker::unionTerms(const TermList &A, const TermList &B) {
  TermList R;
  if (A.All || B.All)
    return R;
  R.All = false;
  R.Terms = A.Terms;
  R.Terms.append(B.Terms.begin(), B.Terms.end());
  normalize(*this, R);
  return R;
}

TermList Checker::intersectTerms(const TermList &A, const TermList &B) {
  if (A.All)
    return B;
  if (B.All)
    return A;
  TermList R;
  R.All = false;
  for (const Term &X : A.Terms)
    for (const Term &Y : B.Terms) {
      Term Z;
      if (intersectTerm(*this, X, Y, Z))
        R.Terms.push_back(Z);
    }
  normalize(*this, R);
  return R;
}

bool Checker::termListIncludes(const TermList &L, Type *V) {
  if (L.All)
    return true;
  for (const Term &T : L.Terms)
    if (termIncludes(T, V))
      return true;
  return false;
}

bool Checker::typeSetSubsetOf(TypeSet *A, TypeSet *B) {
  if (B->AllTerms)
    return true;
  if (A->AllTerms)
    return false;
  for (const Term &X : A->Terms) {
    bool Found = false;
    for (const Term &Y : B->Terms)
      if (termSubsetOf(X, Y)) {
        Found = true;
        break;
      }
    if (!Found)
      return false;
  }
  return true;
}

std::string Checker::termsString(ArrayRef<Term> Ts) {
  std::string S;
  for (size_t I = 0; I < Ts.size(); ++I) {
    if (I)
      S += " | ";
    if (Ts[I].Tilde)
      S += "~";
    // types2 prints a term list with every type qualified.
    S += typeString(Ts[I].T, nullptr);
  }
  return S.empty() ? "∅" : S;
}

bool Checker::typeSetIsComparable(TypeSet *TS) {
  if (TS->AllTerms)
    return TS->Comparable;
  if (TS->Terms.empty())
    return false;
  for (const Term &T : TS->Terms)
    if (!comparableType(T.T, false, nullptr))
      return false;
  return true;
}

Func *Checker::lookupMethodInSet(TypeSet *TS, Package *P, StringRef N,
                                 bool Fold, int *Idx, const Func *Want) {
  for (size_t I = 0; I < TS->Methods.size(); ++I) {
    Func *M = TS->Methods[I];
    if (Want ? M == Want : sameId(M, P, N, Fold)) {
      if (Idx)
        *Idx = static_cast<int>(I);
      return M;
    }
  }
  return nullptr;
}

TypeSet *Checker::typeSet(InterfaceType *I) {
  if (TypeSet *TS = I->Set.load(std::memory_order_acquire))
    return TS;
  return computeTypeSet(I, SourceLoc());
}

TypeSet *Checker::typeSetOf(Type *T) {
  Type *U = T;
  if (auto *P = dyn_cast<TypeParamType>(T)) {
    U = P->Bound ? P->Bound : Ctx.anyType();
    U = under(U);
  } else {
    U = under(T);
  }
  if (auto *I = dyn_cast<InterfaceType>(U))
    return typeSet(I);
  return nullptr;
}

TypeSet *Checker::computeTypeSet(InterfaceType *I, SourceLoc) {
  std::lock_guard<std::recursive_mutex> G(Ctx.lock());
  if (TypeSet *Done = I->Set.load(std::memory_order_acquire))
    return Done;
  TypeSet *TS;
  {
    std::lock_guard<std::mutex> A(Ctx.allocLock());
    TS = Ctx.arena().make<TypeSet>();
  }
  if (I->Computing) {
    // A recursive interface asks for its own set: it gets the universe for
    // now. Not published: the other threads wait for the real one.
    if (!I->Pending)
      I->Pending = TS;
    return I->Pending;
  }
  // What is published here is seen by every body, so it must not depend on
  // who asked first. The cut-off is the nesting of this computation only (not
  // the caller's RecDepth), and a set that was cut, or that was built from a
  // cut one, is returned but never published: the next request computes it
  // again (that only happens for an invalid, endlessly expanding program,
  // which the declaration check has reported already).
  struct Level {
    Checker &C;
    unsigned SavedRec;
    explicit Level(Checker &C) : C(C), SavedRec(C.RecDepth) {
      ++C.TypeSetDepth;
      C.RecDepth = 0;
    }
    ~Level() {
      --C.TypeSetDepth;
      C.RecDepth = SavedRec;
      if (C.TypeSetDepth == 0)
        C.TypeSetCut = false;
    }
  } Lvl(*this);
  if (TypeSetDepth > MaxTypeSetDepth) {
    TypeSetCut = true;
    return TS;
  }
  I->Computing = true;
  SmallVector<Func *, 8> Methods;
  auto AddMethod = [&](Func *M, SourceLoc At, bool Explicit) {
    for (Func *Old : Methods)
      // Operator elements of one name are an overload set (TYP-27): only the
      // same form with the same signature is the same element.
      if (sameId(Old, M->Pkg, M->Name, false) &&
          (!M->isOperator() || (Old->Op == M->Op && Old->Right == M->Right &&
                                identical(Old->Ty, M->Ty)))) {
        if (Old == M)
          return;
        if (!Explicit && identical(Old->Ty, M->Ty))
          return;
        if (!I->Substituted && (At.isValid() || M->Pos.isValid()))
          err(At.isValid() ? At : M->Pos, diag::err_duplicate_method_iface,
              {std::string(M->Name)});
        return;
      }
    Methods.push_back(M);
  };
  for (Func *M : I->Methods)
    AddMethod(M, M->Pos, true);

  TermList Terms;
  Type *Extends = I->Extends; // GEN-12
  bool Comparable = false, Copy = false, Clone = false;
  switch (I->Sp) {
  case InterfaceType::Comparable:
    Comparable = true;
    break;
  case InterfaceType::Copy:
    Copy = true;
    break;
  case InterfaceType::Clone:
    Clone = true;
    break;
  default:
    break;
  }
  for (size_t K = 0; K < I->Embeddeds.size(); ++K) {
    Type *E = I->Embeddeds[K];
    SourceLoc At = K < I->EmbedLocs.size() ? I->EmbedLocs[K] : SourceLoc();
    Type *U = under(E);
    TermList Cur;
    if (auto *EI = dyn_cast<InterfaceType>(U)) {
      TypeSet *ES = typeSet(EI);
      for (Func *M : ES->Methods)
        AddMethod(M, At, false);
      Comparable |= ES->Comparable;
      Copy |= ES->Copy;
      Clone |= ES->Clone;
      if (ES->Extends) {
        if (Extends && Extends != ES->Extends && !I->Substituted &&
            At.isValid())
          err(At, diag::err_extends_constraint,
              {str(I), "the constraint",
               "it extends both " + str(Extends) + " and " + str(ES->Extends)});
        Extends = Extends ? Extends : ES->Extends;
      }
      Cur.All = ES->AllTerms;
      if (!ES->AllTerms)
        Cur.Terms.assign(ES->Terms.begin(), ES->Terms.end());
    } else if (auto *UT = dyn_cast<UnionType>(U)) {
      TermList Acc;
      Acc.All = false;
      for (const Term &T : UT->Terms) {
        TermList One;
        Type *TU = under(T.T);
        if (auto *TI = dyn_cast<InterfaceType>(TU)) {
          TypeSet *TTS = typeSet(TI);
          One.All = TTS->AllTerms;
          if (!TTS->AllTerms)
            One.Terms.assign(TTS->Terms.begin(), TTS->Terms.end());
        } else if (isValid(TU)) {
          // ~T with T not an underlying type is the empty set (types2).
          One.All = false;
          if (!T.Tilde || identical(T.T, TU))
            One.Terms.push_back(T);
        } else {
          continue;
        }
        Acc = unionTerms(Acc, One);
        if (Acc.All)
          break;
      }
      // types2 computeUnionTypeSet: the normalized union is limited too.
      if (!Acc.All && Acc.Terms.size() > 100) {
        if (At.isValid() && !I->Substituted)
          err(At, diag::err_sema_typeset,
              {"cannot handle more than 100 union terms (implementation "
               "limitation)"});
        Acc.All = true;
        Acc.Terms.clear();
      }
      Cur = Acc;
    } else if (!isValid(U)) {
      continue;
    } else {
      Cur.All = false;
      Cur.Terms.push_back(Term{false, E});
    }
    Terms = intersectTerms(Terms, Cur);
  }
  // types2 intersectTermLists: with comparable and specific terms, only the
  // strictly comparable terms remain, and the bit is dropped.
  if (Comparable && !Terms.All) {
    llvm::erase_if(Terms.Terms, [&](const Term &T) {
      return !comparableType(T.T, false, nullptr);
    });
    Comparable = false;
  }
  // GEN-12: `extends P` and specific types (`~int`) have no type in common.
  if (Extends && !Terms.All) {
    if (!I->Substituted && I->ExtendsLoc.isValid())
      err(I->ExtendsLoc, diag::err_extends_constraint,
          {str(I), "the constraint",
           "extends " + str(Extends) +
               " cannot be combined with the type "
               "terms of a type set (empty type set)"});
    Terms.All = false;
    Terms.Terms.clear();
  }
  // The order must not depend on addresses or on who asks first: by name,
  // then package path, then signature (the same-named operator elements of
  // different packages and the overloads of one operator). Elements equal in
  // all three keep their order.
  {
    auto Key = [&](const Func *F) {
      return std::make_tuple(F->Name, F->Pkg ? F->Pkg->Path : StringRef(),
                             F->Ty ? str(F->Ty) : std::string(), F->Right);
    };
    std::stable_sort(
        Methods.begin(), Methods.end(),
        [&](const Func *A, const Func *B) { return Key(A) < Key(B); });
  }
  const bool HasOps =
      llvm::any_of(Methods, [](const Func *M) { return M->isOperator(); });
  std::unique_lock<std::mutex> A(Ctx.allocLock());
  TS->Methods = Ctx.arena().copy(ArrayRef<Func *>(Methods));
  TS->HasOps = HasOps;
  TS->AllTerms = Terms.All;
  if (!Terms.All)
    TS->Terms = Ctx.arena().copy(ArrayRef<Term>(Terms.Terms));
  TS->Comparable = Comparable;
  TS->Copy = Copy;
  TS->Clone = Clone;
  TS->Extends = Extends;
  I->Computing = false;
  if (TypeSetCut)
    return TS;
  I->Set.store(TS, std::memory_order_release);
  return TS;
}
