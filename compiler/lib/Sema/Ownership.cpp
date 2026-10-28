//===- Ownership.cpp - Type-level ownership rules (P03.07) ----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The type-level half of the ownership rules (docs/spec/04-ownership.md,
// 05-parameters.md, 08-slices-and-range.md, 03-types.md TYP-9): categories,
// places and borrowed variables (OWN-R3a, OWN-R3b), Clone/Copy, pointer
// kinds, refs, drop, @Pinned and the visible range mutations of RNG-3.
// Flow-sensitive checks (use after move, OWN-R3c, lifetimes) are P04.
// Everything here runs only when own() is true (not in Go mode).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

static const ast::Expr *unparen(const ast::Expr *E) {
  while (auto *P = dyn_cast<ParenExpr>(E))
    E = P->X;
  return E;
}

//===----------------------------------------------------------------------===//
// Categories (OWN-C1..C5)
//===----------------------------------------------------------------------===//

bool Checker::hasPtrKind(Type *T, PtrKind *K, Type **Elem) {
  auto *N = dyn_cast_or_null<NamedType>(T);
  if (!N || N->Ptr == PtrKind::None || N->TArgs.size() != 1)
    return false;
  // Unique[*T], Shared[*T], Weak[*T] (ADR-0018): Elem is the target T. An
  // instance written without the `*` is an error at its declaration and has
  // no pointer kind.
  auto *P = dyn_cast<PointerType>(N->TArgs[0]);
  if (!P)
    return false;
  if (K)
    *K = N->Ptr;
  if (Elem)
    *Elem = P->Elem;
  return true;
}

bool Checker::isPinned(Type *T) {
  if (!T)
    return false;
  switch (T->getKind()) {
  case Type::Kind::Named: {
    auto *N = cast<NamedType>(T);
    if (N->origin()->Pinned)
      return true;
    // A struct or array type that holds a pinned value by value is pinned.
    if (N->Ptr != PtrKind::None)
      return false;
    Type *U = under(N);
    return (isa<StructType>(U) || isa<sema::ArrayType>(U)) && isPinned(U);
  }
  case Type::Kind::Array:
    return isPinned(cast<sema::ArrayType>(T)->Elem);
  case Type::Kind::Option:
    return isPinned(cast<OptionType>(T)->Elem);
  case Type::Kind::Struct:
    for (Var *F : cast<StructType>(T)->Fields)
      if (isPinned(F->Ty))
        return true;
    return false;
  default:
    return false;
  }
}

static bool hasDropMethod(NamedType *N) {
  NamedType *O = N->origin();
  if (O->Methods)
    for (Func *M : *O->Methods)
      if (M->Name == "drop")
        return true;
  return false;
}

static Category worse(Category A, Category B) {
  if (A == Category::Owning || B == Category::Owning)
    return Category::Owning;
  if (A == Category::SharedHandle || B == Category::SharedHandle)
    return Category::SharedHandle;
  return Category::Trivial;
}

static Category categoryRec(Checker &C, Type *T, unsigned Depth) {
  if (!T || Depth > 24)
    return Category::Trivial;
  switch (T->getKind()) {
  case Type::Kind::Basic: {
    auto *B = cast<BasicType>(T);
    return (B->Info & IsString) ? Category::Owning : Category::Trivial;
  }
  case Type::Kind::Pointer:
  case Type::Kind::Ref:
    return Category::Trivial;
  case Type::Kind::Option:
    // Ownership follows T (ADR-0018).
    return categoryRec(C, cast<OptionType>(T)->Elem, Depth + 1);
  case Type::Kind::Slice:
  case Type::Kind::Map:
    return Category::Owning;
  case Type::Kind::Chan:
  case Type::Kind::Signature:
  case Type::Kind::Interface:
    return Category::SharedHandle;
  case Type::Kind::Array:
    return categoryRec(C, cast<sema::ArrayType>(T)->Elem, Depth + 1);
  case Type::Kind::Struct: {
    Category R = Category::Trivial;
    for (Var *F : cast<StructType>(T)->Fields)
      R = worse(R, categoryRec(C, F->Ty, Depth + 1));
    return R;
  }
  case Type::Kind::Tuple:
  case Type::Kind::Union:
    return Category::Trivial;
  case Type::Kind::TypeParam: {
    auto *I = dyn_cast_or_null<InterfaceType>(C.under(T));
    if (I && C.typeSet(I)->Copy)
      return Category::Trivial;
    return Category::Owning; // OWN-C5
  }
  case Type::Kind::Named: {
    auto *N = cast<NamedType>(T);
    switch (N->Ptr) {
    case PtrKind::Unique:
      return Category::Owning;
    case PtrKind::Shared:
    case PtrKind::Weak:
      return Category::SharedHandle;
    case PtrKind::None:
      break;
    }
    // A `drop` method makes a type owning (OWN-C4).
    if (hasDropMethod(N))
      return Category::Owning;
    if (N->Obj && N->Obj->Pkg == nullptr && N->Obj->Name == "error")
      return Category::SharedHandle;
    return categoryRec(C, C.under(T), Depth + 1);
  }
  }
  return Category::Trivial;
}

Category Checker::category(Type *T) { return categoryRec(*this, T, 0); }

bool Checker::isCopyType(Type *T) {
  if (!T || !isValid(T))
    return true;
  if (auto *P = dyn_cast<TypeParamType>(T)) {
    auto *I = dyn_cast_or_null<InterfaceType>(under(P));
    return I && typeSet(I)->Copy;
  }
  return category(T) == Category::Trivial && !isPinned(T);
}

bool Checker::satisfiesClone(Type *T, std::string *Why) {
  std::function<bool(Type *, unsigned)> Rec = [&](Type *X, unsigned D) -> bool {
    if (!X || !isValid(X) || D > 20)
      return true;
    if (isPinned(X)) {
      if (Why)
        *Why = str(X) + " is @Pinned";
      return false;
    }
    if (auto *P = dyn_cast<TypeParamType>(X)) {
      auto *I = dyn_cast_or_null<InterfaceType>(under(P));
      if (I) {
        TypeSet *TS = typeSet(I);
        if (TS->Clone || TS->Copy)
          return true;
        for (Func *M : TS->Methods)
          if (M->Name == "Clone")
            return true;
      }
      if (Why)
        *Why = str(X) + " is not constrained by Clone";
      return false;
    }
    // A declared Clone method decides.
    if (isa<NamedType>(X) || isa<PointerType>(X)) {
      LookupResult LR = lookupFieldOrMethod(X, true, S.Pkg, "Clone");
      if (auto *M = dyn_cast_or_null<Func>(LR.Obj)) {
        if (!M->Ty)
          objDecl(M, nullptr);
        auto *Sg = dyn_cast_or_null<SignatureType>(M->Ty);
        bool Exact = Sg && Sg->Params->size() == 0 &&
                     Sg->Results->size() == 1 &&
                     identical(Sg->Results->Elems[0], X);
        if ((isa<PointerType>(X) || !M->HasPtrRecv) && Exact)
          return true;
        if (Why)
          *Why =
              "its Clone method does not have the signature Clone() " + str(X);
        return false;
      }
      if (auto *N = dyn_cast<NamedType>(X)) {
        PtrKind K;
        Type *El;
        if (hasPtrKind(X, &K, &El)) {
          if (K == PtrKind::Unique)
            return Rec(El, D + 1);
          return true;
        }
        if (hasDropMethod(N)) {
          if (Why)
            *Why = str(X) + " has a drop method but no Clone() " + str(X);
          return false;
        }
        if (N->Obj && N->Obj->Pkg == nullptr && N->Obj->Name == "error")
          return true;
      }
    }
    Type *U = under(X);
    switch (U->getKind()) {
    case Type::Kind::Basic:
    case Type::Kind::Pointer:
    case Type::Kind::Ref:
    case Type::Kind::Chan:
    case Type::Kind::Signature:
    case Type::Kind::Interface:
      return true;
    case Type::Kind::Option:
      return Rec(cast<OptionType>(U)->Elem, D + 1);
    case Type::Kind::Slice:
      return Rec(cast<SliceType>(U)->Elem, D + 1);
    case Type::Kind::Map:
      return Rec(cast<sema::MapType>(U)->Key, D + 1) &&
             Rec(cast<sema::MapType>(U)->Elem, D + 1);
    case Type::Kind::Array:
      return Rec(cast<sema::ArrayType>(U)->Elem, D + 1);
    case Type::Kind::Struct: {
      // An owning struct with raw pointer fields needs a hand-written Clone:
      // an automatic one would copy the pointers (OWN-R6).
      bool Owning = category(U) == Category::Owning;
      for (Var *F : cast<StructType>(U)->Fields) {
        if (Owning && isa<PointerType>(under(F->Ty))) {
          if (Why)
            *Why = "it mixes owning fields with the raw pointer field " +
                   std::string(F->Name) + ": write Clone() by hand";
          return false;
        }
        if (!Rec(F->Ty, D + 1)) {
          if (Why && Why->empty())
            *Why = "field " + std::string(F->Name) + " is not cloneable";
          return false;
        }
      }
      return true;
    }
    default:
      return true;
    }
  };
  return Rec(T, 0);
}

//===----------------------------------------------------------------------===//
// Zero values (D1.4 rule 9, TYP-15, ADR-0016, ADR-0018)
//===----------------------------------------------------------------------===//

namespace {
/// Does T have a zero value? One definition for every site that needs it:
/// pointer kinds (never nil), structs with a `@Required` field, C++ types
/// without a default constructor, and whatever contains such a type. The
/// answer is a function of the type alone: no position, no error and no
/// limit of the asking body is part of it. Each named type met on the way
/// caches its answer in NamedType::ZeroState, but only when nothing below it
/// was cut short (an invalid recursive type, or a nesting deeper than
/// MaxDepth), so the cache holds exact answers only: a cached answer and a
/// fresh one are the same, whichever body (thread) asks first.
struct ZeroQuery {
  ZeroQuery(Checker &C, bool IgnoreForeign)
      : C(C), IgnoreForeign(IgnoreForeign) {}
  Checker &C;
  /// Treat a foreign (C++) type as having a zero value: the "Go++ type with
  /// no zero value" of the move rule (ADR-0016 point 5). Such a query reads
  /// only the "has one" answers of the cache and writes none.
  bool IgnoreForeign;
  SmallVector<const NamedType *, 8> InProgress;
  unsigned Depth = 0;
  bool Cut = false;
  static constexpr unsigned MaxDepth = 1000;

  /// Depth at which a long chain is resolved iteratively / its reason is
  /// summarized (both are functions of the type and the query root only).
  static constexpr unsigned PrewarmDepth = 64;
  static constexpr unsigned SummaryDepth = 16;

  /// The named types a type body mentions directly (not through another
  /// named type's body).
  void directNamed(Type *T, llvm::SmallVectorImpl<NamedType *> &Out,
                   unsigned D) {
    if (!T || D > 200)
      return;
    if (auto *N = dyn_cast<NamedType>(T)) {
      Out.push_back(N);
      return;
    }
    if (auto *St = dyn_cast<StructType>(T)) {
      for (Var *F : St->Fields)
        directNamed(F->Ty, Out, D + 1);
    } else if (auto *A = dyn_cast<sema::ArrayType>(T)) {
      directNamed(A->Elem, Out, D + 1);
    }
  }

  /// Fill the cache for N and the named types below it, bottom up, with an
  /// explicit stack. Each answer is computed by a fresh shallow query whose
  /// inner named types are already cached, so it is exact.
  void prewarm(NamedType *Root) {
    struct Frame {
      NamedType *N;
      SmallVector<NamedType *, 4> Deps;
      size_t Next = 0;
    };
    SmallVector<Frame, 16> Stack;
    SmallVector<const NamedType *, 16> OnStack;
    auto Push = [&](NamedType *N) {
      if (N->origin()->Ptr != PtrKind::None || N->Obj == nullptr)
        return;
      if (N->Obj->Pkg && N->Obj->Pkg->Foreign)
        return;
      if (N->ZeroState.load(std::memory_order_acquire) != 0 ||
          llvm::is_contained(OnStack, N))
        return;
      Type *U = C.under(N);
      if (!C.isValid(U))
        return;
      Frame F;
      F.N = N;
      directNamed(U, F.Deps, 0);
      OnStack.push_back(N);
      Stack.push_back(std::move(F));
    };
    Push(Root);
    while (!Stack.empty()) {
      Frame &F = Stack.back();
      if (F.Next < F.Deps.size()) {
        NamedType *D = F.Deps[F.Next++];
        Push(D); // invalidates F
        continue;
      }
      NamedType *N = F.N;
      Stack.pop_back();
      OnStack.pop_back();
      ZeroQuery Q(C, false);
      Q.has(N, nullptr); // writes N->ZeroState when the answer is exact
    }
  }

  /// The reason for a long chain: the number of nested fields and the leaf
  /// cause, found by following the first field without a zero value.
  std::string deepWhy(Type *T) {
    unsigned Hops = 0;
    std::string Last = C.str(T);
    std::string Leaf;
    auto Lacks = [&](Type *X) {
      ZeroQuery Q(C, false);
      return !Q.has(X, nullptr);
    };
    for (unsigned Guard = 0; Guard < 1000000; ++Guard) {
      Type *U = T;
      if (isa<NamedType>(T))
        U = C.under(T);
      Type *Next = nullptr;
      if (auto *St = dyn_cast<StructType>(U)) {
        for (Var *F : St->Fields) {
          if (F->Required) {
            Leaf = "field " + std::string(F->Name) + " of " + Last +
                   " is @Required";
            break;
          }
          if (Lacks(F->Ty)) {
            Next = F->Ty;
            Leaf = "field " + std::string(F->Name) + " of " + Last;
            break;
          }
        }
      } else if (auto *A = dyn_cast<sema::ArrayType>(U)) {
        if (Lacks(A->Elem))
          Next = A->Elem;
      }
      if (!Next || !isa<NamedType>(Next)) {
        if (Next) {
          // A pointer or other leaf type: ask it directly.
          std::string Inner;
          ZeroQuery Q(C, false);
          Q.has(Next, &Inner);
          Leaf += " has none: " + Inner;
        }
        break;
      }
      auto *NN = cast<NamedType>(Next);
      if (NN->origin()->Ptr != PtrKind::None ||
          (NN->Obj && NN->Obj->Pkg && NN->Obj->Pkg->Foreign)) {
        std::string Inner;
        ZeroQuery Q(C, false);
        Q.has(Next, &Inner);
        Leaf += " has none: " + Inner;
        break;
      }
      T = Next;
      Last = C.str(T);
      ++Hops;
    }
    return "it contains " + std::to_string(Hops) +
           " more nested types down to " + Last + ": " + Leaf;
  }

  bool has(Type *T, std::string *Why) {
    if (!T || !C.isValid(T))
      return true;
    if (Depth >= MaxDepth) {
      // Never answer "has a zero value" from a cut (the unsafe side). Cut
      // keeps the answer out of the cache.
      Cut = true;
      if (Why)
        *Why = "the type is nested more than " + std::to_string(MaxDepth) +
               " levels deep, too deep to check";
      return false;
    }
    struct G {
      unsigned &D;
      explicit G(unsigned &D) : D(D) { ++D; }
      ~G() { --D; }
    } Guard(Depth);
    switch (T->getKind()) {
    case Type::Kind::Pointer:
      if (Why)
        *Why = "pointers are never nil";
      return false;
    case Type::Kind::Array: {
      auto *A = cast<sema::ArrayType>(T);
      if (A->Len == 0)
        return true;
      if (Why && Depth >= SummaryDepth) {
        // A long run of nested arrays: walk it in a loop and report the
        // innermost type (the text stays short and the stack small).
        unsigned Layers = 0;
        Type *Leaf = A;
        while (auto *AA = dyn_cast<sema::ArrayType>(Leaf)) {
          if (AA->Len == 0)
            break;
          Leaf = AA->Elem;
          ++Layers;
        }
        if (Depth + Layers >= MaxDepth) {
          Cut = true;
          *Why = "the type is nested more than " + std::to_string(MaxDepth) +
                 " levels deep, too deep to check";
          return false;
        }
        std::string Inner;
        if (has(Leaf, &Inner))
          return true;
        *Why = "it nests " + std::to_string(Layers) +
               " array levels, and the innermost type " + C.str(Leaf) +
               " has none: " + Inner;
        return false;
      }
      std::string Inner;
      if (has(A->Elem, Why ? &Inner : nullptr))
        return true;
      if (Why)
        *Why = "its element type " + C.str(A->Elem) + " has none: " + Inner;
      return false;
    }
    case Type::Kind::Struct: {
      for (Var *F : cast<StructType>(T)->Fields) {
        if (F->Required) {
          if (Why)
            *Why = "field " + std::string(F->Name) + " is @Required";
          return false;
        }
        std::string Inner;
        if (!has(F->Ty, Why ? &Inner : nullptr)) {
          if (Why)
            *Why = "field " + std::string(F->Name) + " of type " +
                   C.str(F->Ty) + " has none: " + Inner;
          return false;
        }
      }
      return true;
    }
    case Type::Kind::TypeParam: {
      // A generic body is checked once: T may be anything its constraint
      // allows. Only a constraint whose terms all have a zero value says so.
      auto *I = dyn_cast_or_null<InterfaceType>(C.under(T));
      if (I) {
        TypeSet *TS = C.typeSet(I);
        if (TS->hasTerms() && !TS->Terms.empty()) {
          bool All = true;
          for (const Term &Tm : TS->Terms)
            All &= has(Tm.T, nullptr);
          if (All)
            return true;
        }
      }
      if (Why)
        *Why = "its constraint allows types with none (GEN-5); constrain " +
               C.str(T) + ", or use Option[" + C.str(T) + "]";
      return false;
    }
    case Type::Kind::Named: {
      auto *N = cast<NamedType>(T);
      if (N->origin()->Ptr != PtrKind::None) {
        if (Why)
          *Why = "pointers are never nil";
        return false;
      }
      // A C++ type has a zero value exactly when it has a default
      // constructor (OWN-R9); the move rule ignores C++ types.
      if (N->Obj && N->Obj->Pkg && N->Obj->Pkg->Foreign) {
        if (IgnoreForeign || N->Obj->Pkg->Foreign->hasDefaultConstructor(N))
          return true;
        if (Why)
          *Why = "it has no default constructor";
        return false;
      }
      uint8_t St = N->ZeroState.load(std::memory_order_acquire);
      // A long chain of named types is resolved from the bottom up on the
      // heap, so no depth limit decides the answer (and the stack stays
      // small).
      if (St == 0 && Depth >= PrewarmDepth && !IgnoreForeign) {
        prewarm(N);
        St = N->ZeroState.load(std::memory_order_acquire);
      }
      if (St == 1 || (St == 2 && !Why && !IgnoreForeign))
        return St == 1;
      if (St == 2 && Why && !IgnoreForeign && Depth >= SummaryDepth) {
        *Why = deepWhy(N);
        return false;
      }
      if (llvm::is_contained(InProgress, N)) {
        Cut = true; // an invalid recursive type, reported elsewhere
        return true;
      }
      Type *U = C.under(N);
      if (!C.isValid(U)) {
        // An invalid type counts as having one (its error is reported
        // elsewhere); for a type still being declared (phase 1) the answer
        // is not known yet. Instances are expanded by under().
        if (!N->Orig && !N->Underlying)
          Cut = true;
        return true;
      }
      InProgress.push_back(N);
      bool SavedCut = Cut;
      Cut = false;
      bool R = has(U, Why);
      if (!Cut && !IgnoreForeign)
        N->ZeroState.store(R ? 1 : 2, std::memory_order_release);
      Cut = Cut || SavedCut;
      InProgress.pop_back();
      return R;
    }
    default:
      return true;
    }
  }
};
} // namespace

bool Checker::hasZeroValue(Type *T, std::string *Why) {
  if (!own() || !T || !isValid(T))
    return true;
  ZeroQuery Q(*this, false);
  return Q.has(T, Why);
}

bool Checker::lacksGoZeroValue(Type *T) {
  if (!own() || !T || !isValid(T))
    return false;
  ZeroQuery Q(*this, true);
  return !Q.has(T, nullptr);
}

bool Checker::isNeverNilField(const Var *F) {
  if (!own() || !F || !F->Required || !F->Ty)
    return false;
  // A type-parameter field only means "must be set" (ADR-0016 point 8).
  if (isa<TypeParamType>(F->Ty) || F->RequiredSetOnly)
    return false;
  Type *U = under(F->Ty);
  return isa<InterfaceType>(U) || isa<SignatureType>(U) ||
         isa<sema::MapType>(U) || isa<sema::ChanType>(U);
}

const Var *Checker::selectedField(const ast::Expr *Ex) {
  auto *Sel = dyn_cast<SelectorExpr>(unparen(Ex));
  if (!Sel)
    return nullptr;
  auto It = I.Selections.find(Sel);
  if (It == I.Selections.end() || It->second.K != Selection::FieldVal)
    return nullptr;
  return dyn_cast_or_null<Var>(It->second.Obj);
}

bool Checker::isMemReplace(const ast::Expr *Fun) {
  auto *Sel = dyn_cast<SelectorExpr>(unparen(Fun));
  if (!Sel || name(Sel->Sel) != "Replace")
    return false;
  auto *Id = dyn_cast<ast::Ident>(unparen(Sel->X));
  if (!Id)
    return false;
  auto *PN = dyn_cast_or_null<PkgName>(E.Sc->lookup(name(Id), E.Pos));
  return PN && PN->Imported && PN->Imported->Path == "mem";
}

/// The builtin an identifier names, by scope (before the call is checked).
static Builtin *builtinNamed(Checker &C, const ast::Expr *Fun) {
  auto *Id = dyn_cast<ast::Ident>(unparen(Fun));
  if (!Id)
    return nullptr;
  return dyn_cast_or_null<Builtin>(C.lookup(C.name(Id)));
}

void Checker::markLookupOk(const ast::Expr *Ex, bool MoveOnly) {
  if (!own() || !Ex)
    return;
  Ex = unparen(Ex);
  if (isa<IndexExpr>(Ex)) {
    if (!MoveOnly)
      ZeroLookupOk.insert(Ex);
    return;
  }
  if (auto *U = dyn_cast<UnaryExpr>(Ex)) {
    if (U->Op == tok::arrow && !MoveOnly)
      ZeroLookupOk.insert(Ex);
    return;
  }
  // `v, ok := clone(m[k])`, `v, ok := move(m[k])`, a bare `move(m[k])`.
  if (auto *Call = dyn_cast<CallExpr>(Ex))
    if (Call->Args.size() == 1)
      if (Builtin *B = builtinNamed(*this, Call->Fun))
        if (B->Id == BuiltinId::Move ||
            (B->Id == BuiltinId::Clone && !MoveOnly))
          markLookupOk(Call->Args[0], false);
}

bool Checker::checkLookupZero(const ast::Expr *Ex, Type *Elem) {
  if (!own() || !Elem || !isValid(Elem) || ZeroLookupOk.count(Ex) ||
      hasZeroValue(Elem))
    return true;
  std::string Why;
  hasZeroValue(Elem, &Why);
  err(Ex, diag::err_no_zero_lookup, {str(Ex), str(Elem), Why});
  return false;
}

void Checker::needZeroValue(SourceLoc Pos, Type *T, std::string What,
                            std::string Hint) {
  if (!own() || !T)
    return;
  auto Check = [this, Pos, T, What = std::move(What),
                Hint = std::move(Hint)]() {
    if (hasZeroValue(T))
      return;
    std::string Why;
    hasZeroValue(T, &Why);
    if (!Hint.empty())
      Why += (Why.empty() ? "" : "; ") + Hint;
    err(Pos, diag::err_no_zero_value, {What, str(T), Why});
  };
  // Package-level declarations are checked in phase 1, while types may still
  // be unresolved; the check waits for the end of the phase.
  if (Bodies)
    Check();
  else
    later(Check);
}

//===----------------------------------------------------------------------===//
// Places (OWN-R3a)
//===----------------------------------------------------------------------===//

Object *Checker::identObj(const ast::Expr *Ex) {
  Ex = unparen(Ex);
  auto *Id = dyn_cast<ast::Ident>(Ex);
  if (!Id)
    return nullptr;
  auto It = I.Uses.find(Id);
  if (It != I.Uses.end())
    return It->second;
  auto D = I.Defs.find(Id);
  return D == I.Defs.end() ? nullptr : D->second;
}

bool Checker::isBuiltinCall(const ast::Expr *Ex, BuiltinId Id) {
  auto *Call = dyn_cast<CallExpr>(unparen(Ex));
  if (!Call)
    return false;
  auto *B = dyn_cast_or_null<Builtin>(identObj(Call->Fun));
  return B && B->Id == Id;
}

/// EXP-2: a field or element reached BY VALUE from an owned temporary (a
/// struct field, an array element, an element of an owned slice or map, or
/// through a Unique[T] the temporary owns) is itself a temporary. Through a
/// raw pointer, a ref or a Shared[T] it is a place in someone else's storage.
bool Checker::isOwnedTempPart(const ast::Expr *Base) {
  Base = unparen(Base);
  Type *BT = I.typeOf(Base);
  if (!BT || !isValid(BT) || isa<RefType>(BT))
    return false;
  PtrKind K;
  Type *El;
  if (hasPtrKind(BT, &K, &El))
    return K == PtrKind::Unique && placeKind(Base) == PlaceKind::Temp;
  if (isa<PointerType>(under(BT)))
    return false;
  return placeKind(Base) == PlaceKind::Temp;
}

/// True if Ex is a temporary, or a part reached through a Shared[T]
/// temporary: the temporary handle may release the last strong reference at
/// the end of the statement, so a ref into it dangles (EXP-2, OWN-V6).
bool Checker::isTempOrSharedTempPart(const ast::Expr *Ex) {
  Ex = unparen(Ex);
  if (placeKind(Ex) == PlaceKind::Temp)
    return true;
  for (;;) {
    const ast::Expr *Base = nullptr;
    if (auto *Sel = dyn_cast<SelectorExpr>(Ex))
      Base = Sel->X;
    else if (auto *Ix = dyn_cast<IndexExpr>(Ex))
      Base = Ix->X;
    else if (auto *St = dyn_cast<StarExpr>(Ex))
      Base = St->X;
    if (!Base)
      return false;
    Base = unparen(Base);
    Type *BT = I.typeOf(Base);
    PtrKind K;
    Type *El;
    if (BT && isValid(BT) && hasPtrKind(BT, &K, &El) && K == PtrKind::Shared &&
        placeKind(Base) == PlaceKind::Temp)
      return true;
    Ex = Base;
  }
}

bool Checker::isPlaceExpr(const ast::Expr *Ex) {
  Ex = unparen(Ex);
  switch (Ex->getKind()) {
  case ast::Node::Kind::Ident:
    return llvm::isa_and_nonnull<Var>(identObj(Ex));
  case ast::Node::Kind::SelectorExpr: {
    auto *Sel = cast<SelectorExpr>(Ex);
    auto It = I.Selections.find(Sel);
    if (It != I.Selections.end())
      return It->second.K == Selection::FieldVal && !isOwnedTempPart(Sel->X);
    return llvm::isa_and_nonnull<Var>(identObj(Sel->Sel));
  }
  case ast::Node::Kind::IndexExpr: {
    const TypeAndValue *TV = I.lookup(cast<IndexExpr>(Ex)->X);
    return TV && TV->M != Mode::TypeExpr &&
           !isOwnedTempPart(cast<IndexExpr>(Ex)->X);
  }
  case ast::Node::Kind::StarExpr:
  case ast::Node::Kind::TypeAssertExpr:
    return true;
  default:
    return false;
  }
}

Var *Checker::rootVar(const ast::Expr *Ex, bool &Through, bool OwnedParts) {
  Through = false;
  for (;;) {
    Ex = unparen(Ex);
    switch (Ex->getKind()) {
    case ast::Node::Kind::Ident:
      return dyn_cast_or_null<Var>(identObj(Ex));
    case ast::Node::Kind::SelectorExpr: {
      auto *Sel = cast<SelectorExpr>(Ex);
      auto It = I.Selections.find(Sel);
      if (It == I.Selections.end() || It->second.K != Selection::FieldVal)
        return nullptr;
      if (It->second.Indirect)
        Through = true;
      Ex = Sel->X;
      continue;
    }
    case ast::Node::Kind::IndexExpr: {
      auto *Ix = cast<IndexExpr>(Ex);
      Type *T = I.typeOf(Ix->X);
      Type *U = T ? coreType(T) : nullptr;
      // Slice, map, string, pointer to array: through, unless OwnedParts and
      // the slice or map is itself owned by the root (not a ref).
      bool Owned = OwnedParts && U && !isa<RefType>(T) &&
                   (isa<SliceType>(U) || isa<sema::MapType>(U));
      if ((!U || !isa<sema::ArrayType>(U)) && !Owned)
        Through = true;
      Ex = Ix->X;
      continue;
    }
    case ast::Node::Kind::StarExpr:
      Through = true;
      Ex = cast<StarExpr>(Ex)->X;
      continue;
    default:
      return nullptr;
    }
  }
}

Var *Checker::borrowedRoot(const ast::Expr *Ex) {
  bool Through = false;
  Var *V = rootVar(Ex, Through);
  return V && !Through && V->Mode == Var::Borrowed ? V : nullptr;
}

Checker::PlaceKind Checker::placeKind(const ast::Expr *Ex) {
  Ex = unparen(Ex);
  if (!isPlaceExpr(Ex))
    return PlaceKind::Temp;
  if (auto *Id = dyn_cast<ast::Ident>(Ex)) {
    auto *V = dyn_cast_or_null<Var>(identObj(Id));
    if (!V)
      return PlaceKind::Temp;
    if (V->IsPackageLevel || V->Mode == Var::Alias)
      return PlaceKind::OtherPlace;
    if (V->Mode == Var::Borrowed)
      return PlaceKind::BorrowedVar;
    if (V->FuncOwner && V->FuncOwner != E.FuncId)
      return PlaceKind::OtherPlace; // captured (CLO-9)
    return PlaceKind::Local;
  }
  if (borrowedRoot(Ex))
    return PlaceKind::BorrowedVar;
  return PlaceKind::OtherPlace;
}

/// True if evaluating Ex twice gives the same place: identifiers, literals,
/// selectors, dereferences, operators and index expressions of such operands.
/// A call, a receive or a function literal may give a different result each
/// time.
static bool isStablePlaceExpr(const ast::Expr *Ex) {
  Ex = unparen(Ex);
  switch (Ex->getKind()) {
  case ast::Node::Kind::Ident:
  case ast::Node::Kind::BasicLit:
    return true;
  case ast::Node::Kind::SelectorExpr:
    return isStablePlaceExpr(cast<SelectorExpr>(Ex)->X);
  case ast::Node::Kind::StarExpr:
    return isStablePlaceExpr(cast<StarExpr>(Ex)->X);
  case ast::Node::Kind::TypeAssertExpr:
    return isStablePlaceExpr(cast<TypeAssertExpr>(Ex)->X);
  case ast::Node::Kind::UnaryExpr: {
    auto *U = cast<UnaryExpr>(Ex);
    return U->Op != tok::arrow && isStablePlaceExpr(U->X);
  }
  case ast::Node::Kind::BinaryExpr: {
    auto *B = cast<BinaryExpr>(Ex);
    return isStablePlaceExpr(B->X) && isStablePlaceExpr(B->Y);
  }
  case ast::Node::Kind::IndexExpr: {
    auto *Ix = cast<IndexExpr>(Ex);
    if (!isStablePlaceExpr(Ix->X))
      return false;
    for (const ast::Expr *I : Ix->Indices)
      if (!isStablePlaceExpr(I))
        return false;
    return true;
  }
  default:
    return false;
  }
}

bool Checker::samePlace(const ast::Expr *A, const ast::Expr *B) {
  A = unparen(A);
  B = unparen(B);
  return isStablePlaceExpr(A) && isStablePlaceExpr(B) && str(A) == str(B);
}

bool Checker::isImplicitMove(const ast::Expr *Arg, const ast::Expr *Call) {
  auto It = AppendTargets.find(unparen(Call));
  return It != AppendTargets.end() && samePlace(It->second, Arg);
}

/// A borrowed variable that is neither a parameter nor a range variable is
/// the binding of a type switch or type assertion (TYP-9).
static bool isBinding(const Var *V) {
  return V->Mode == Var::Borrowed && !V->IsParam && !V->LoopVar;
}

/// For a conversion `T(x)` whose result shares x's storage (same underlying
/// type, or a conversion to an interface), the operand x; else null. Such a
/// conversion is no new value: ownership is checked on x (OWN-R3a, TYP-6).
/// Conversions that copy (`string([]byte)`, ref to owned, numeric) are
/// temporaries.
const ast::Expr *Checker::sharedConversionArg(const ast::Expr *Ex) {
  auto *Call = dyn_cast<CallExpr>(unparen(Ex));
  if (!Call || Call->Args.size() != 1 || Call->isMust())
    return nullptr;
  const TypeAndValue *F = I.lookup(Call->Fun);
  if (!F || F->M != Mode::TypeExpr)
    return nullptr;
  Type *To = I.typeOf(Call);
  Type *From = I.typeOf(Call->Args[0]);
  if (!To || !From || !isValid(To) || !isValid(From) || isUntyped(From))
    return nullptr;
  return conversionShares(From, To) ? Call->Args[0] : nullptr;
}

/// True if converting a value of type From to type To may leave both values
/// pointing at the same buffer. Go's conversion rule ignores struct tags, so
/// does this. For a type parameter on either side the conversion shares
/// unless every pair of terms of the type sets copies.
bool Checker::conversionShares(Type *From, Type *To) {
  if (isNonTypeParamInterface(To))
    return true;
  SmallVector<Type *, 4> Fs, Ts;
  bool Unknown = false;
  auto Collect = [&](Type *T, SmallVector<Type *, 4> &Out) {
    underIs(T, [&](Type *U) {
      if (!U) {
        Unknown = true;
        return false;
      }
      Out.push_back(U);
      return true;
    });
  };
  Collect(From, Fs);
  Collect(To, Ts);
  if (Unknown)
    return true;
  for (Type *Ua : Fs)
    for (Type *Ub : Ts) {
      // Only an owning type can share a buffer: float64(x) of a number
      // shares nothing.
      if (isa<InterfaceType>(Ub) ||
          (identicalIgnoreTags(Ua, Ub) &&
           (category(Ub) != Category::Trivial || isPinned(From))))
        return true;
      // A slice converted to an array copies the elements bitwise.
      if (isa<SliceType>(Ua))
        if (auto *Arr = dyn_cast<sema::ArrayType>(Ub))
          if (category(Arr->Elem) != Category::Trivial)
            return true;
    }
  return false;
}

void Checker::needOwnership(const Operand &X, const char *What, Type *Dst) {
  if (NoOwnCheck || !own() || !X.E || X.M == Mode::Invalid || !X.Ty ||
      !isValid(X.Ty) || !isTyped(X.Ty))
    return;
  if (const ast::Expr *Arg = sharedConversionArg(X.E)) {
    Operand Inner = X;
    Inner.E = Arg;
    Inner.Ty = I.typeOf(Arg);
    needOwnership(Inner, What, Dst);
    return;
  }
  const ast::Expr *Ex = unparen(X.E);
  PlaceKind PK = placeKind(Ex);
  if (PK == PlaceKind::Temp)
    return;
  std::string S0 = str(Ex);
  if (isPinned(X.Ty)) {
    err(X, diag::err_own_pinned_copy, {S0, str(X.Ty)});
    return;
  }
  if (category(X.Ty) != Category::Owning || PK == PlaceKind::Local)
    return;
  // OWN-P6: a Unique[*T] / Shared[*T] held in a place converts to *T as a
  // borrow; only a destination that owns needs ownership.
  if (Dst && isValid(Dst)) {
    Type *DU = under(Dst);
    if (auto *DO = dyn_cast<OptionType>(DU))
      DU = under(DO->Elem); // Option[*T]: a borrow that may be nil
    if (isa<PointerType>(DU))
      return;
  }
  bool Through = false;
  Var *Root = rootVar(Ex, Through);
  if (Through)
    Root = nullptr;
  // TYP-9: the value of v.(T), or a binding of it, lives in a shared box.
  if (isa<TypeAssertExpr>(Ex) ||
      (Root && isBinding(Root) && !Root->OptionBinding)) {
    Err E1 = err(X, diag::err_own_box_needs_ownership, {What, S0});
    E1.note(X.pos(), diag::note_own_fix_clone, {S0});
    E1.fixit(SourceRange(Ex->getEnd(), Ex->getEnd()), ".Clone()");
    return;
  }
  if (isa<TypeParamType>(X.Ty)) {
    // GEN-4: returning a borrowed T; GEN-3: any other copy of a T.
    if (StringRef(What) == "return" && Root && Root->IsParam &&
        Root->Mode == Var::Borrowed) {
      err(X, diag::err_own_generic_return, {S0, str(X.Ty)})
          .note(X.pos(), diag::note_own_generic_return_fix, {S0});
      return;
    }
    err(X, diag::err_own_generic_copy, {str(X.Ty)})
        .note(X.pos(), "fix-it: " + S0 +
                           ".Clone() with a Clone constraint, or move(" + S0 +
                           ")");
    return;
  }
  std::string Desc = "a value in another place";
  bool Part = !isa<ast::Ident>(Ex);
  if (Root && Root->OptionBinding) {
    // ADR-0018 point 11: `v, ok := o.Get()` borrows an owning T.
    Desc = std::string(Part ? "part of " : "") + "the value that " +
           std::string(Root->Name) + " borrows from an Option (Get)";
  } else if (PK == PlaceKind::BorrowedVar && Root) {
    const char *K = Root->IsParam   ? "borrowed parameter"
                    : Root->LoopVar ? "borrowed range variable"
                                    : "borrowed variable";
    Desc = Part
               ? "part of the " + std::string(K) + " " + std::string(Root->Name)
               : "a " + std::string(K);
  } else if (auto *V = dyn_cast_or_null<Var>(identObj(Ex))) {
    if (V->IsPackageLevel)
      Desc = "a package variable";
    else if (V->Mode == Var::Alias)
      Desc = "an & range variable (an element of the collection)";
    else
      Desc = "a captured variable (CLO-9)";
  }
  Err E2 = err(X, diag::err_own_r3a_needs_ownership, {What, S0, Desc});
  std::string Why;
  if (satisfiesClone(X.Ty, &Why)) {
    E2.note(X.pos(), diag::note_own_fix_clone, {S0});
    E2.fixit(SourceRange(Ex->getEnd(), Ex->getEnd()), ".Clone()");
  }
  if (PK != PlaceKind::BorrowedVar)
    E2.note(X.pos(), diag::note_own_fix_move, {S0});
  else if (Root && Root->IsParam)
    E2.note(X.pos(), diag::note_own_fix_move_param, {});
  else if (Root && Root->LoopVar)
    E2.note(X.pos(), diag::note_own_fix_alias, {std::string(Root->Name)});
}

//===----------------------------------------------------------------------===//
// Borrowed variables (OWN-R3b) and range mutations (RNG-3)
//===----------------------------------------------------------------------===//

void Checker::checkWrite(const ast::Expr *Lhs, bool Repoint, bool Redeclare) {
  if (!own())
    return;
  const ast::Expr *U = unparen(Lhs);
  // A write to the key variable of an enclosing range: from now on `m[k]`
  // may name another entry (RNG-3).
  if (auto *KV = dyn_cast_or_null<Var>(identObj(U)))
    for (RangeFrame &F : RangeStack)
      if (F.Key == KV)
        F.KeyWritten = true;
  Var *V = borrowedRoot(U);
  if (!V)
    return;
  // PAR-2: re-pointing a borrowed string / []T variable at a ref.
  if (Repoint && isa<ast::Ident>(U))
    return;
  std::string S0 = str(U);
  // `v, ok2 := g.Get()` that reuses v assigns it: the fix is a new name.
  std::string NewName =
      "fix-it: use a new name instead of redeclaring " + S0 + " with :=";
  if (V->LoopVar) {
    err(Lhs, diag::err_own_range_var_write, {S0})
        .note(Lhs->getBegin(),
              Redeclare ? NewName
                        : "write through the element with the & "
                          "form: for _, &" +
                              std::string(V->Name) + " := range ...");
    return;
  }
  if (isBinding(V)) {
    // ADR-0018 point 11: a Get binding borrows from the Option, not a box.
    Err E1 = err(Lhs,
                 V->OptionBinding ? diag::err_own_get_binding_read_only
                                  : diag::err_own_binding_read_only,
                 {"assign to", S0});
    if (Redeclare)
      E1.note(Lhs->getBegin(), NewName);
    else
      E1.note(Lhs->getBegin(), diag::note_own_fix_binding_copy,
              {std::string(V->Name)});
    return;
  }
  Err E2 = err(Lhs, diag::err_own_borrowed_read_only, {"assign to", S0});
  if (Redeclare)
    E2.note(Lhs->getBegin(), NewName);
}

void Checker::checkRangeMutation(const ast::Expr *Lhs, const char *What) {
  if (!own() || RangeStack.empty())
    return;
  const ast::Expr *L = unparen(Lhs);
  std::string LS = str(L);
  for (const RangeFrame &F : RangeStack) {
    if (F.X.empty() || !F.XTy)
      continue;
    Type *U = coreType(F.XTy);
    if (!U)
      continue;
    if (auto *Ix = dyn_cast<IndexExpr>(L)) {
      // m[x] = ... inside `range m`: an insert unless x is the range key
      // variable, not written earlier in the body.
      if (!isa<sema::MapType>(U) || str(unparen(Ix->X)) != F.X)
        continue;
      if (F.Key && !F.KeyWritten && Ix->Indices.size() == 1 &&
          identObj(Ix->Indices[0]) == F.Key)
        continue;
      err(Lhs, diag::err_own_range_map_insert, {LS, F.X})
          .note(Lhs->getBegin(), diag::note_own_range_collect, {});
      return;
    }
    if (LS != F.X)
      continue;
    // Slices and maps can be reallocated; an owned string grows in place
    // (STM-8). A ref has a fixed length.
    bool Ok = isa<SliceType>(U) || isa<sema::MapType>(U) ||
              (U == typ(BasicKind::String) && !isa<RefType>(F.XTy));
    if (!Ok)
      continue;
    err(Lhs, diag::err_own_range_mutation, {What, LS})
        .note(Lhs->getBegin(), diag::note_own_range_collect, {});
    return;
  }
}

void Checker::checkAddressOf(const UnaryExpr *Ex, Operand &X) {
  if (!own())
    return;
  (void)X;
  const ast::Expr *Inner = unparen(Ex->X);
  if (borrowedRoot(Inner)) {
    err(Ex, diag::err_own_borrowed_address, {str(Inner)});
    return;
  }
  // ADR-0016 point 8: nil could be written through the pointer; only
  // mem.Replace(&c.F, v) may take it, and v must not be nil either.
  const Var *F = selectedField(Inner);
  if (!F || !isNeverNilField(F))
    return;
  if (unparen(Ex) != ReplaceArg)
    err(Ex, diag::err_required_addr, {str(Inner)});
  else if (ReplaceNil)
    err(ReplaceNil, diag::err_required_nil_store, {str(Inner)});
}

void Checker::checkPtrRecvCall(const SelectorExpr *Sel, Func *M) {
  if (!own() || !M->HasPtrRecv)
    return;
  Type *BT = I.typeOf(Sel->X);
  if (!BT || isa<PointerType>(under(BT)))
    return;
  if (borrowedRoot(Sel->X))
    err(Sel, diag::err_own_borrowed_ptr_call,
        {std::string(M->Name), str(Sel->X)});
}

//===----------------------------------------------------------------------===//
// Refs (OWN-V6, OWN-V7, EXP-11)
//===----------------------------------------------------------------------===//

/// The expression a ref value R points into: the base of a slice
/// expression, or R itself when an owned value converts to a ref at Want.
/// Null if R is made from a ref or a pointer (checked where that was made).
static const ast::Expr *refBase(Checker &C, const ast::Expr *R, Type *Want,
                                Type *SrcTy = nullptr) {
  R = unparen(R);
  if (auto *Sl = dyn_cast<SliceExpr>(R)) {
    Type *BT = C.I.typeOf(Sl->X);
    if (!BT || isa<RefType>(BT) || isa<PointerType>(C.under(BT)))
      return nullptr;
    return unparen(Sl->X);
  }
  Type *T = C.I.typeOf(R);
  // A multi-value call: the element's type is the operand's, not the tuple.
  if (T && isa<TupleType>(T))
    T = SrcTy;
  if (Want && isa<RefType>(Want) && T && !isa<RefType>(T))
    return R; // an owned value converted to a ref
  return nullptr;
}

void Checker::checkReturnRef(const ast::Expr *R, Type *Want) {
  if (!own() || !Want)
    return;
  const ast::Expr *U = unparen(R);
  if (auto *Un = dyn_cast<UnaryExpr>(U); Un && Un->Op == tok::and_) {
    bool Through = false;
    Var *V = rootVar(Un->X, Through);
    if (V && !Through && !V->IsPackageLevel && V->FuncOwner == E.FuncId &&
        V->Mode != Var::Borrowed && V->Mode != Var::Alias)
      err(R, diag::err_own_return_addr_local, {std::string(V->Name)});
    return;
  }
  if (!isa<RefType>(Want))
    return;
  const ast::Expr *Base = refBase(*this, U, Want);
  if (!Base)
    return;
  if (isTempOrSharedTempPart(Base)) {
    // A string constant has static storage; anything else is a temporary.
    const TypeAndValue *TV = I.lookup(Base);
    if (!TV || TV->M != Mode::Constant)
      err(R, diag::err_own_return_ref_temp);
    return;
  }
  bool Through = false;
  Var *V = rootVar(Base, Through, true);
  if (!V || Through || V->IsPackageLevel || V->FuncOwner != E.FuncId)
    return;
  if (V->Mode == Var::Borrowed || V->Mode == Var::Alias || isa<RefType>(V->Ty))
    return;
  err(R, diag::err_own_return_ref_local, {std::string(V->Name)});
}

void Checker::checkRefStore(const ast::Expr *X, Type *Dst, Type *SrcTy) {
  if (!own() || !Dst || !isa<RefType>(Dst))
    return;
  const ast::Expr *Base = refBase(*this, X, Dst, SrcTy);
  if (!Base || !isTempOrSharedTempPart(Base))
    return;
  const TypeAndValue *TV = I.lookup(Base);
  if (TV && TV->M == Mode::Constant)
    return;
  err(X, diag::err_own_ref_into_temp, {str(X), "a longer-lived place"});
}

bool Checker::checkPointerField(Operand &X, Type *FieldTy) {
  if (!own() || X.M == Mode::Invalid || !X.E || !FieldTy)
    return false;
  // An Option[*T] field is a *T field that may be empty (ADR-0018).
  auto *Opt = dyn_cast<OptionType>(under(FieldTy));
  auto *P = dyn_cast<PointerType>(Opt ? under(Opt->Elem) : under(FieldTy));
  PtrKind K;
  Type *El;
  if (!P || !hasPtrKind(X.Ty, &K, &El) || K == PtrKind::Weak ||
      !identical(El, P->Elem) || placeKind(X.E) != PlaceKind::Temp)
    return false;
  err(X, diag::err_own_pointer_field,
      {str(FieldTy), Opt ? "Option[" + str(X.Ty) + "]" : str(X.Ty)});
  X.M = Mode::Invalid;
  return true;
}

//===----------------------------------------------------------------------===//
// Signatures (PAR-*), Clone and drop methods (OWN-R6, OWN-R7)
//===----------------------------------------------------------------------===//

void Checker::checkSigOwnership(const ast::FuncType *FT, SignatureType *Sig,
                                ArrayRef<Var *> Params,
                                ArrayRef<Var *> Results) {
  (void)FT;
  (void)Results;
  SmallVector<Var *, 8> Ps(Params.begin(), Params.end());
  later([this, Sig, Ps]() {
    // PAR-1: owning parameters borrow.
    for (Var *V : Ps) {
      if (!V->Ty || !isValid(V->Ty))
        continue;
      if (V->Mode == Var::MoveParam)
        continue;
      if (category(V->Ty) == Category::Owning)
        V->Mode = Var::Borrowed;
    }
    // A variadic parameter of an owning type must be `move ...T` (PAR-5).
    if (Sig->Variadic && !Ps.empty()) {
      Var *Last = Ps.back();
      if (auto *Sl = dyn_cast_or_null<SliceType>(Last->Ty)) {
        Type *El = Sl->Elem;
        if (Last->Mode != Var::MoveParam && !isNonTypeParamInterface(El)) {
          if (isa<TypeParamType>(El)) {
            if (!isCopyType(El))
              err(Last->Pos, diag::err_own_variadic_generic, {str(El)});
          } else if (category(El) == Category::Owning) {
            err(Last->Pos, diag::err_own_variadic_owning, {str(El)});
          }
        }
      }
    }
  });
}

void Checker::checkMoveRecvSig(Func *F, const FuncDecl *FD) {
  (void)FD;
  if (F->Recv && F->MoveRecv) {
    // A `move` receiver is an owned local.
    F->Recv->Mode = Var::MoveParam;
    return;
  }
  Var *R = F->Recv;
  if (!R)
    return;
  later([this, R, F]() {
    if (!F->HasPtrRecv && R->Ty && isValid(R->Ty) &&
        category(R->Ty) == Category::Owning)
      R->Mode = Var::Borrowed;
  });
}

void Checker::checkSpecialMethod(Func *F, const FuncDecl *FD) {
  auto *Sg = dyn_cast_or_null<SignatureType>(F->Ty);
  if (!Sg)
    return;
  if (F->Name == "drop") {
    // OWN-R7: func (x *T) drop().
    if (!F->HasPtrRecv || Sg->Params->size() != 0 || Sg->Results->size() != 0 ||
        !Sg->TParams.empty())
      err(FD->Name, diag::err_own_drop_signature);
    return;
  }
  // OWN-R6: `func (h *T) Clone() T` must have a value receiver.
  if (F->Name != "Clone" || !F->HasPtrRecv || Sg->Params->size() != 0 ||
      Sg->Results->size() != 1)
    return;
  auto *P = dyn_cast_or_null<PointerType>(F->Recv ? F->Recv->Ty : nullptr);
  if (P && identical(Sg->Results->Elems[0], P->Elem))
    err(FD->Name, diag::err_own_clone_ptr_receiver)
        .note(FD->Name->getBegin(), "use a value receiver");
}

bool Checker::isReadOnlyValue(const ast::Expr *Ex, Type *Ty) {
  if (!own())
    return false;
  if (Ty && isa<RefType>(under(Ty)))
    return true;
  return Ex && borrowedRoot(Ex) != nullptr;
}
