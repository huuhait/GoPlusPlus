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
  bool Exact = true;
  return isPinnedRec(canonicalParams(T), Exact);
}

/// isPinned and satisfiesClone look at a type parameter only through its
/// constraint (its type set's Clone, Copy and method names; a type parameter
/// is never pinned), and compare types only by identity. So the answer for
/// G[T] is the answer for G[S], where S is a stand-in with T's constraint,
/// as long as distinct parameters get distinct stand-ins. The stand-ins are
/// shared, so a chain of generic types used by many generic functions, each
/// with its own T, is walked once instead of once per function. Not done
/// when a constraint mentions a type parameter (`[T Cloner[T]]`: the
/// constraint would still name the old T), when T is nested deeper than
/// substitution goes, or when a type parameter of T is inside an interface
/// or a union: those types are not uniqued, so the substituted type would
/// be a new one (and walked anew) on every query.
Type *Checker::canonicalParams(Type *T) {
  if (!T)
    return T;
  SmallVector<TypeParamType *, 4> Params; // in order of first appearance
  llvm::SmallPtrSet<const Type *, 16> Seen;
  // Collects the type parameters T mentions; false if too deep to follow.
  // With Record, a type parameter inside an interface or a union makes it
  // give up (nullopt) as well.
  auto Collect = [&](Type *Root, bool Record) {
    struct Item {
      Type *X;
      unsigned D;
      bool InIface; // below an interface or a union
    };
    SmallVector<Item, 16> Work{{Root, 0, false}};
    llvm::SmallPtrSet<const Type *, 16> SeenInIface;
    bool Found = false;
    while (!Work.empty()) {
      auto [X, D, InIface] = Work.pop_back_val();
      if (!X || !(InIface ? SeenInIface : Seen).insert(X).second)
        continue;
      if (D > 60)
        return std::optional<bool>();
      bool Below = InIface || isa<InterfaceType>(X) || isa<UnionType>(X);
      auto Push = [&](Type *Y) { Work.push_back({Y, D + 1, Below}); };
      switch (X->getKind()) {
      case Type::Kind::TypeParam:
        if (Record && InIface)
          return std::optional<bool>();
        Found = true;
        if (Record)
          Params.push_back(cast<TypeParamType>(X));
        break;
      case Type::Kind::Pointer:
        Push(cast<PointerType>(X)->Elem);
        break;
      case Type::Kind::Slice:
        Push(cast<SliceType>(X)->Elem);
        break;
      case Type::Kind::Ref:
        Push(cast<RefType>(X)->Elem);
        break;
      case Type::Kind::Option:
        Push(cast<OptionType>(X)->Elem);
        break;
      case Type::Kind::Array:
        Push(cast<sema::ArrayType>(X)->Elem);
        break;
      case Type::Kind::Chan:
        Push(cast<sema::ChanType>(X)->Elem);
        break;
      case Type::Kind::Map:
        Push(cast<sema::MapType>(X)->Key);
        Push(cast<sema::MapType>(X)->Elem);
        break;
      case Type::Kind::Tuple:
        for (Type *E : cast<TupleType>(X)->Elems)
          Push(E);
        break;
      case Type::Kind::Signature:
        Push(cast<SignatureType>(X)->Params);
        Push(cast<SignatureType>(X)->Results);
        break;
      case Type::Kind::Struct:
        for (Var *F : cast<StructType>(X)->Fields)
          Push(F->Ty);
        break;
      case Type::Kind::Union:
        for (const Term &Tm : cast<UnionType>(X)->Terms)
          Push(Tm.T);
        break;
      case Type::Kind::Interface:
        for (Func *M : cast<InterfaceType>(X)->Methods)
          Push(M->Ty);
        for (Type *E : cast<InterfaceType>(X)->Embeddeds)
          Push(E);
        break;
      case Type::Kind::Named:
        for (Type *A : cast<NamedType>(X)->TArgs)
          Push(A);
        break;
      default:
        break;
      }
    }
    return std::optional<bool>(Found);
  };
  std::optional<bool> Any = Collect(T, true);
  if (!Any || !*Any)
    return T;
  // The work list visits in reverse; number the parameters as they first
  // appear in a fixed order (any fixed order will do).
  SmallVector<Type *, 4> To;
  for (unsigned I = 0; I < Params.size(); ++I) {
    TypeParamType *P = Params[I];
    Seen.clear();
    std::optional<bool> InBound = Collect(P->Bound, false);
    if (!InBound || *InBound)
      return T;
    To.push_back(Ctx.canonicalParam(P->Bound, I));
  }
  SmallVector<TypeParamType *, 4> From(Params.begin(), Params.end());
  return subst(T, From, To);
}

/// Exact is cleared when an answer rests on a type that is not declared yet
/// (an invalid underlying type): such an answer is not cached. A named type
/// caches the rest in NamedType::PinnedState, so a chain of n nested named
/// types costs n steps in all, not n per query.
bool Checker::isPinnedRec(Type *T, bool &Exact) {
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
    uint8_t St = N->PinnedState.load(std::memory_order_acquire);
    if (St != 0)
      return St == 2;
    Type *U = under(N);
    if (!isValid(U)) {
      Exact = false;
      return false;
    }
    if (!isa<StructType>(U) && !isa<sema::ArrayType>(U)) {
      N->PinnedState.store(1, std::memory_order_release);
      return false;
    }
    bool Own = true;
    bool R = isPinnedRec(U, Own);
    if (Own)
      N->PinnedState.store(R ? 2 : 1, std::memory_order_release);
    Exact = Exact && Own;
    return R;
  }
  case Type::Kind::Array:
    return isPinnedRec(cast<sema::ArrayType>(T)->Elem, Exact);
  case Type::Kind::Option:
    return isPinnedRec(cast<OptionType>(T)->Elem, Exact);
  case Type::Kind::Struct:
    for (Var *F : cast<StructType>(T)->Fields)
      if (isPinnedRec(F->Ty, Exact))
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

/// More generic instances of one generic type than this in one clone walk
/// means an instance graph that never ends (`type T[P any] struct{ s
/// []T[Option[P]] }` reaches T[int], T[Option[int]], ...: Go reports an
/// instantiation cycle; this checker does not yet). The walk stops with the
/// safe answer and stores nothing. Finite code does not come near it.
static constexpr unsigned MaxInstancesPerWalk = 100000;

/// The category of T (OWN-C1..C5): the worst category of what T holds by
/// value (struct fields, array and Option elements, the underlying type of a
/// named type); slices, maps, strings, pointer kinds, drop types and the rest
/// decide by themselves. The walk is iterative with no depth limit: a cut
/// would answer Trivial for a deeply nested owning type, which then is
/// copied bitwise and dropped twice.
///
/// An instance G[A] is answered from its generic declaration, never expanded:
/// G's underlying type is walked once with its type parameters left open,
/// which gives the category of the rest and the parameters held by value
/// (NamedType::CatGeneric / CatParams); the instance joins that with the
/// categories of those arguments. So a chain of n generic types instantiated
/// over fresh type parameters costs n steps in all, and a generic type whose
/// instances grow without end (`T[P]` holding `Option[T[[]P]]`) is finite.
///
/// Every answer is exact and a function of the type alone. Named types
/// store theirs (CategoryState, and CatGeneric for a declaration) once the
/// types are final (typesFinal), but only when nothing on the way was an
/// unfinished declaration or a cycle by value (an invalid recursive type).
Category Checker::category(Type *T) {
  // A category and the open type parameters of the frame's generic
  // declaration (Gen) that it rests on.
  struct Val {
    Category C = Category::Trivial;
    uint64_t M = 0;
  };
  auto Join = [](Val A, Val B) { return Val{worse(A.C, B.C), A.M | B.M}; };
  auto BitOf = [](unsigned I) { return 1ull << (I < 63 ? I : 63); };
  struct Frame {
    Type *X;                      // the type; for a declaration's walk, Gen
    NamedType *Gen = nullptr;     // whose type parameters are open (or none)
    bool Decl = false;            // the walk of Gen's own underlying type
    NamedType *WaitFor = nullptr; // an instance waiting for its declaration
    SmallVector<Type *, 4> Kids;
    size_t Next = 0;
    Val Acc;
  };
  bool Exact = true;
  auto Store = [&] { return Exact && typesFinal(); };
  SmallVector<Frame, 8> Stack;
  struct Mark {
    bool Open = true;
    Val V;
  };
  llvm::DenseMap<std::pair<Type *, NamedType *>, Mark> Seen;
  llvm::DenseMap<NamedType *, Val> GenSeen; // declarations done this walk
  llvm::SmallPtrSet<NamedType *, 4> GenOpen;
  auto GenKnown = [&](NamedType *O, Val &V) {
    if (uint8_t St = O->CatGeneric.load(std::memory_order_acquire)) {
      V = {static_cast<Category>(St - 1),
           O->CatParams.load(std::memory_order_relaxed)};
      return true;
    }
    auto It = GenSeen.find(O);
    if (It == GenSeen.end())
      return false;
    V = It->second;
    return true;
  };
  // The parts of instance N, from its declaration's answer D.
  auto FromDecl = [&](Frame &F, NamedType *N, Val D) {
    F.Acc = Join(F.Acc, Val{D.C, 0});
    for (unsigned J = 0; J < N->TArgs.size(); ++J)
      if (D.M & BitOf(J))
        F.Kids.push_back(N->TArgs[J]);
  };
  // What X decides by itself in the walk of Gen, or none: then F (X's frame)
  // has its parts. Pushes the declaration's walk when an instance needs it.
  auto Local = [&](Type *X, NamedType *Gen, Frame &F) -> std::optional<Val> {
    switch (X->getKind()) {
    case Type::Kind::Basic:
      return Val{(cast<BasicType>(X)->Info & IsString) ? Category::Owning
                                                       : Category::Trivial};
    case Type::Kind::Pointer:
    case Type::Kind::Ref:
    case Type::Kind::Tuple:
    case Type::Kind::Union:
      return Val{Category::Trivial};
    case Type::Kind::Slice:
    case Type::Kind::Map:
      return Val{Category::Owning};
    case Type::Kind::Chan:
    case Type::Kind::Signature:
    case Type::Kind::Interface:
      return Val{Category::SharedHandle};
    case Type::Kind::Option: // ownership follows T (ADR-0018)
      F.Kids.push_back(cast<OptionType>(X)->Elem);
      return std::nullopt;
    case Type::Kind::Array:
      F.Kids.push_back(cast<sema::ArrayType>(X)->Elem);
      return std::nullopt;
    case Type::Kind::Struct:
      for (Var *Fd : cast<StructType>(X)->Fields)
        F.Kids.push_back(Fd->Ty);
      return std::nullopt;
    case Type::Kind::TypeParam: {
      auto *P = cast<TypeParamType>(X);
      if (Gen && P->Index < Gen->TParams.size() && Gen->TParams[P->Index] == P)
        return Val{Category::Trivial, BitOf(P->Index)}; // left open
      auto *I = dyn_cast_or_null<InterfaceType>(under(X));
      if (I && typeSet(I)->Copy)
        return Val{Category::Trivial};
      return Val{Category::Owning}; // OWN-C5
    }
    case Type::Kind::Named: {
      auto *N = cast<NamedType>(X);
      switch (N->Ptr) {
      case PtrKind::Unique:
        return Val{Category::Owning};
      case PtrKind::Shared:
      case PtrKind::Weak:
        return Val{Category::SharedHandle};
      case PtrKind::None:
        break;
      }
      bool Inst = N->Orig && !N->Orig->TParams.empty() &&
                  N->TArgs.size() == N->Orig->TParams.size();
      // A stored answer is about the type with no parameter left open; an
      // instance inside a declaration's walk may mention them.
      if (!Gen || !Inst)
        if (uint8_t St = N->CategoryState.load(std::memory_order_acquire))
          return Val{static_cast<Category>(St - 1)};
      // A `drop` method makes a type owning (OWN-C4).
      if (hasDropMethod(N))
        return Val{Category::Owning};
      if (N->Obj && N->Obj->Pkg == nullptr && N->Obj->Name == "error")
        return Val{Category::SharedHandle};
      if (!declFinished(N))
        Exact = false;
      if (!Inst) {
        F.Kids.push_back(under(N));
        return std::nullopt;
      }
      NamedType *O = N->Orig;
      Val D;
      if (GenKnown(O, D)) {
        FromDecl(F, N, D);
        return std::nullopt;
      }
      if (GenOpen.count(O)) {
        Exact = false; // G inside G by value: an invalid recursive type
        return Val{};
      }
      F.WaitFor = O; // FromDecl once O's walk is done
      return std::nullopt;
    }
    }
    return Val{};
  };
  // Starts the frame of X in the walk of Gen, or gives X's value at once.
  auto Start = [&](Type *X, NamedType *Gen) -> std::optional<Val> {
    Frame F;
    F.X = X;
    F.Gen = Gen;
    if (std::optional<Val> V = Local(X, Gen, F))
      return V;
    Seen[{X, Gen}] = Mark();
    NamedType *O = F.WaitFor;
    Stack.push_back(std::move(F));
    if (O) {
      // The declaration's own walk, with its type parameters left open.
      Frame G;
      G.X = O;
      G.Gen = O;
      G.Decl = true;
      G.Kids.push_back(under(O));
      if (!declFinished(O))
        Exact = false;
      GenOpen.insert(O);
      Stack.push_back(std::move(G));
    }
    return std::nullopt;
  };
  if (!T)
    return Category::Trivial;
  if (std::optional<Val> V = Start(T, nullptr))
    return V->C;
  while (true) {
    Frame &F = Stack.back();
    if (F.WaitFor) {
      Val D;
      bool Known = GenKnown(F.WaitFor, D);
      assert(Known && "the declaration's walk ends before its instance");
      (void)Known;
      NamedType *N = cast<NamedType>(F.X);
      F.WaitFor = nullptr;
      FromDecl(F, N, D);
      continue;
    }
    // Owning is the worst: the rest of the parts cannot change it.
    if (F.Acc.C != Category::Owning && F.Next < F.Kids.size()) {
      Type *Y = F.Kids[F.Next++];
      if (!Y)
        continue;
      NamedType *Gen = F.Gen;
      auto It = Seen.find({Y, Gen});
      if (It != Seen.end()) {
        if (It->second.Open)
          Exact = false; // a cycle by value: an invalid recursive type
        else
          F.Acc = Join(F.Acc, It->second.V);
        continue;
      }
      if (std::optional<Val> V = Start(Y, Gen)) // may invalidate F
        Stack.back().Acc = Join(Stack.back().Acc, *V);
      continue;
    }
    Frame Done = std::move(F);
    Stack.pop_back();
    Val R = Done.Acc;
    if (R.C == Category::Owning)
      R.M = 0; // the parameters cannot change it
    if (Done.Decl) {
      NamedType *O = cast<NamedType>(Done.X);
      GenOpen.erase(O);
      GenSeen[O] = R;
      if (Store()) {
        O->CatParams.store(R.M, std::memory_order_relaxed);
        O->CatGeneric.store(static_cast<uint8_t>(R.C) + 1,
                            std::memory_order_release);
      }
      continue; // the instance below reads it (FromDecl)
    }
    Seen[{Done.X, Done.Gen}] = Mark{false, R};
    if (auto *N = dyn_cast<NamedType>(Done.X); N && !R.M && Store())
      N->CategoryState.store(static_cast<uint8_t>(R.C) + 1,
                             std::memory_order_release);
    if (Stack.empty())
      return R.C;
    Stack.back().Acc = Join(Stack.back().Acc, R);
  }
}

bool Checker::isCopyType(Type *T) {
  if (!T || !isValid(T))
    return true;
  if (auto *P = dyn_cast<TypeParamType>(T)) {
    auto *I = dyn_cast_or_null<InterfaceType>(under(P));
    return I && typeSet(I)->Copy;
  }
  return category(T) == Category::Trivial && !isPinned(T);
}

/// Is T cloneable (OWN-R6, GEN-2)? The ONE place that decides it, for
/// `clone(x)`, `x.Clone()` and the `Clone` constraint. When it is not, Why
/// (if given) names the first thing that blocks it, with the path of field
/// names down to it: `field mu (Mu) has a drop method but no Clone()`,
/// `field A.mu (Mu) has ...` (owner decision 2026-10-28). The elements of
/// slices, arrays, maps, Options and Unique pointers are looked through
/// without adding to the path, so `[]Server` names the same field as `Server`
/// does.
///
/// The types form a graph: a type has an edge to each type its answer rests
/// on (a field, an element, the target of a Unique pointer), and some types
/// block by themselves (a drop method without Clone(), @Pinned, ...). A type
/// is cloneable if and only if no type that blocks can be reached from it, so
/// a type on a cycle (`Kids []Tree` in Tree) is cloneable unless something
/// reachable from the cycle blocks. The walk is an iterative depth-first
/// search (no recursion and no depth limit, so neither the stack nor a cut
/// decides anything) that takes the edges in field order and stops at the
/// first type that blocks: that type and the field path to it are the reason.
/// On the way it finds the strongly connected components (Tarjan): a finished
/// component reaches no blocker, so its types are cloneable; when the walk
/// stops at a blocker, every type still on Tarjan's stack reaches it, so
/// those are not. Both answers are exact, a function of the type alone, and
/// once the types are final (typesFinal: every package-level type and its
/// methods are declared) each named type stores its own in
/// NamedType::CloneState (every thread stores the same value). A walk that
/// wants the reason goes into a stored "no" to find it, and skips a stored
/// "yes" (nothing below it blocks), so the reason does not depend on what is
/// stored either. Nothing is stored before that, or once the walk met a type
/// whose declaration is not finished (a local type being declared) or a
/// runaway instance graph (MaxInstancesPerWalk: answered "no", the safe
/// side); each query still visits each type once.
bool Checker::satisfiesClone(Type *T, std::string *Why) {
  if (!T || !isValid(T))
    return true;
  // The answer is asked about T with shared stand-ins for its type
  // parameters (canonicalParams); the reason, wanted only when it is "no",
  // is found on T itself, so it names T's own types.
  Type *K = canonicalParams(T);
  if (K != T)
    return cloneWalk(K, nullptr) || (Why && cloneWalk(T, Why));
  return cloneWalk(T, Why);
}

bool Checker::cloneWalk(Type *T, std::string *Why) {
  // 3 ("capped") is read for the root only: see Capped below.
  auto Stored = [&](Type *X) -> uint8_t {
    auto *N = dyn_cast<NamedType>(X);
    uint8_t St = N ? N->CloneState.load(std::memory_order_acquire) : 0;
    return St == 3 ? 0 : St;
  };
  // The reason of a walk that reached MaxInstancesPerWalk: about the root,
  // so a stored "capped" answer gives the same text as a fresh walk.
  auto CapReason = [&] {
    return str(T) +
           " reaches instances of a generic type with ever larger "
           "type arguments (more than " +
           num(MaxInstancesPerWalk) + " of one), too many to check";
  };
  if (auto *N = dyn_cast<NamedType>(T);
      N && N->CloneState.load(std::memory_order_acquire) == 3) {
    if (Why)
      *Why = CapReason();
    return false;
  }
  if (uint8_t St = Stored(T); St == 1 || (St == 2 && !Why))
    return St == 1;

  struct Edge {
    Type *Ty;
    StringRef Field; // empty for an element (no name on the path)
  };
  struct Frame {
    Type *X;
    unsigned Index, Low; // Tarjan's
    SmallVector<Edge, 2> Edges;
    size_t Next = 0;
    /// Set when X blocks by itself (once its Edges are found cloneable).
    Type *Leaf = nullptr;
    std::string Pred; // what is wrong with Leaf, as a predicate (with Why)
  };
  std::vector<Frame> Stack;              // the depth-first path
  SmallVector<Type *, 16> Open;          // Tarjan's stack
  llvm::DenseMap<Type *, unsigned> Seen; // Index of each type met
  std::vector<bool> Done;                // by Index: known cloneable
  // Cleared once an answer may rest on an unfinished declaration or a cut:
  // from then on nothing is stored (what finished before is still exact).
  bool Exact = true;
  // The walk reached MaxInstancesPerWalk: the root is not cloneable (the
  // safe side). Whether a walk from the root gets there is a function of
  // the root alone (nothing in a runaway instance graph is ever stored as
  // cloneable), so that answer is stored for the root, as 3, unless a
  // declaration on the way was unfinished; never for the types in between.
  bool Capped = false, Unfinished = false;
  llvm::DenseMap<const NamedType *, unsigned> Instances;
  auto CanStore = [&] { return Exact && typesFinal(); };

  // Starts X: its own rule either decides it or lists the edges it rests on.
  auto Visit = [&](Type *X) {
    unsigned Idx = Done.size();
    Seen[X] = Idx;
    Done.push_back(false);
    Open.push_back(X);
    Frame &F = Stack.emplace_back();
    F.X = X;
    F.Index = F.Low = Idx;
    auto Block = [&](Type *Leaf, std::string Pred) {
      F.Leaf = Leaf;
      F.Pred = std::move(Pred);
    };
    auto Rests = [&](Type *Y, StringRef Field = {}) {
      if (Y && isValid(Y))
        F.Edges.push_back({Y, Field});
    };
    if (auto *N = dyn_cast<NamedType>(X)) {
      if (!declFinished(N))
        Exact = false, Unfinished = true;
      if (N->Orig && ++Instances[N->Orig] > MaxInstancesPerWalk) {
        Exact = false;
        Capped = true;
        return Block(N->Orig, "");
      }
    }
    if (isPinned(X))
      return Block(X, "is @Pinned");
    if (auto *P = dyn_cast<TypeParamType>(X)) {
      auto *I = dyn_cast_or_null<InterfaceType>(under(P));
      if (I) {
        TypeSet *TS = typeSet(I);
        if (TS->Clone || TS->Copy)
          return;
        // GEN-2: a method of the constraint counts only as `Clone() T`, T
        // being this very parameter (`Clone() int` clones nothing).
        for (Func *M : TS->Methods)
          if (M->Name == "Clone") {
            auto *Sg = dyn_cast_or_null<SignatureType>(M->Ty);
            if (Sg && Sg->Params->size() == 0 && Sg->Results->size() == 1 &&
                identical(Sg->Results->Elems[0], P))
              return;
          }
      }
      return Block(X, "is not constrained by Clone");
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
          return;
        // A raw pointer to a type with Clone() gets that method too; an
        // automatic Clone would copy the pointer.
        auto *Ptr = dyn_cast<PointerType>(X);
        if (Why && Ptr && Sg && Sg->Results->size() == 1 &&
            identical(Sg->Results->Elems[0], Ptr->Elem))
          return Block(X, "is a raw pointer to a type with Clone(): an "
                          "automatic Clone() would copy the pointer, write "
                          "Clone() by hand");
        return Block(X, Why ? "has a Clone method that does not have the "
                              "signature Clone() " +
                                  str(X)
                            : "");
      }
      if (auto *N = dyn_cast<NamedType>(X)) {
        PtrKind K;
        Type *El;
        if (hasPtrKind(X, &K, &El)) {
          if (K == PtrKind::Unique)
            Rests(El);
          return;
        }
        if (hasDropMethod(N))
          return Block(X, "has a drop method but no Clone()");
        if (N->Obj && N->Obj->Pkg == nullptr && N->Obj->Name == "error")
          return;
      }
    }
    Type *U = under(X);
    switch (U->getKind()) {
    case Type::Kind::Option:
      return Rests(cast<OptionType>(U)->Elem);
    case Type::Kind::Slice:
      return Rests(cast<SliceType>(U)->Elem);
    case Type::Kind::Map:
      Rests(cast<sema::MapType>(U)->Key);
      return Rests(cast<sema::MapType>(U)->Elem);
    case Type::Kind::Array:
      return Rests(cast<sema::ArrayType>(U)->Elem);
    case Type::Kind::Struct: {
      // An owning struct with raw pointer fields needs a hand-written Clone:
      // an automatic one would copy the pointers (OWN-R6). The fields before
      // that one come first, as the reason takes the first field that blocks.
      bool Owning = category(U) == Category::Owning;
      for (Var *Fd : cast<StructType>(U)->Fields) {
        if (Owning && isa<PointerType>(under(Fd->Ty)))
          return Block(X, "mixes owning fields with the raw pointer field " +
                              std::string(Fd->Name) +
                              ": write Clone() by hand");
        Rests(Fd->Ty, Fd->Name);
      }
      return;
    }
    default: // basic types, pointers, refs, channels, funcs, interfaces
      return;
    }
  };
  // The walk stopped at a blocker: every type still open reaches it.
  auto Blocked = [&] {
    if (Capped) {
      if (auto *N = dyn_cast<NamedType>(T); N && !Unfinished && typesFinal())
        N->CloneState.store(3, std::memory_order_release);
      if (Why)
        *Why = CapReason();
      return false;
    }
    if (CanStore())
      for (Type *Y : Open)
        if (auto *N = dyn_cast<NamedType>(Y))
          N->CloneState.store(2, std::memory_order_release);
    return false;
  };

  Visit(T);
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    if (F.Next < F.Edges.size()) {
      Type *Y = F.Edges[F.Next++].Ty;
      auto It = Seen.find(Y);
      if (It != Seen.end()) {
        if (!Done[It->second]) // open: on a cycle through the path
          F.Low = std::min(F.Low, It->second);
        continue;
      }
      uint8_t St = Stored(Y);
      if (St == 1)
        continue;
      if (St == 2 && !Why)
        return Blocked();
      Visit(Y); // invalidates F
      continue;
    }
    if (F.Leaf) {
      if (Why && !Capped) {
        // The field names on the path, outermost first. A very long path is
        // shortened in the middle; the blocking type is always named.
        constexpr size_t ReasonDepth = 200;
        SmallVector<StringRef, 8> Path;
        for (size_t I = 0; I + 1 < Stack.size(); ++I)
          if (StringRef Fd = Stack[I].Edges[Stack[I].Next - 1].Field;
              !Fd.empty())
            Path.push_back(Fd);
        std::string Subject = str(F.Leaf);
        if (!Path.empty()) {
          std::string P;
          for (size_t I = 0; I < Path.size() && I < ReasonDepth; ++I)
            P += (I ? "." : "") + Path[I].str();
          if (Path.size() > ReasonDepth + 1)
            P += ".(" + num(Path.size() - ReasonDepth - 1) + " more fields)";
          if (Path.size() > ReasonDepth)
            P += "." + Path.back().str();
          Subject = "field " + P + " (" + Subject + ")";
        }
        *Why = Subject + " " + F.Pred;
      }
      return Blocked();
    }
    // Every edge of F is cloneable. If F roots a component, nothing in it
    // reaches a blocker.
    Type *X = F.X;
    unsigned Idx = F.Index, Low = F.Low;
    Stack.pop_back();
    if (Low == Idx) {
      Type *Y;
      do {
        Y = Open.pop_back_val();
        Done[Seen[Y]] = true;
        if (auto *N = dyn_cast<NamedType>(Y); N && CanStore())
          N->CloneState.store(1, std::memory_order_release);
      } while (Y != X);
    }
    if (!Stack.empty())
      Stack.back().Low = std::min(Stack.back().Low, Low);
  }
  return true;
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
  /// Set by has() when it answers false: the answer rests only on type
  /// parameters (an instance may still have a zero value), no type that is
  /// certain to have none. Read right after a false answer.
  bool Poly = false;
  /// The type parameters (by index, bit 63 for index >= 63) that a false
  /// answer of the query rested on. Read for a generic declaration only (its
  /// own parameters), see NamedType::PolyMask.
  uint64_t PolyMask = 0;
  static uint64_t bitOf(unsigned I) { return 1ull << (I < 63 ? I : 63); }
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
            Leaf = "field " + std::string(F->Name) + " of " + Last + " is " +
                   Checker::requiredWhy(F);
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
    Poly = false;
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
    case Type::Kind::Signature:
      if (Why)
        *Why = "funcs are never nil";
      return false;
    case Type::Kind::Interface:
      // An interface with methods other than `error` is never nil (ADR-0018
      // point 3); the empty interface and `error` keep nil.
      if (C.neverNilKind(T)) {
        if (Why)
          *Why = "interfaces with methods are never nil";
        return false;
      }
      return true;
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
      // The first field without a zero value gives the reason. A field that
      // lacks one only through a type parameter does not end the walk: a
      // later field may lack one for certain.
      bool PolyFail = false;
      std::string FirstWhy;
      for (Var *F : cast<StructType>(T)->Fields) {
        if (F->Required) {
          if (Why && !PolyFail)
            *Why = "field " + std::string(F->Name) + " is " +
                   Checker::requiredWhy(F);
          else if (Why)
            *Why = FirstWhy;
          Poly = false;
          return false;
        }
        std::string Inner;
        if (!has(F->Ty, Why ? &Inner : nullptr)) {
          bool FieldPoly = Poly;
          if (Why && !PolyFail)
            FirstWhy = "field " + std::string(F->Name) + " of type " +
                       C.str(F->Ty) + " has none: " + Inner;
          if (!FieldPoly) {
            if (Why)
              *Why = FirstWhy;
            Poly = false;
            return false;
          }
          PolyFail = true;
        }
      }
      Poly = PolyFail;
      if (PolyFail && Why)
        *Why = FirstWhy;
      return !PolyFail;
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
      Poly = true;
      PolyMask |= bitOf(cast<TypeParamType>(T)->Index);
      return false;
    }
    case Type::Kind::Named: {
      auto *N = cast<NamedType>(T);
      if (N->origin()->Ptr != PtrKind::None) {
        if (Why)
          *Why = "pointers are never nil";
        return false;
      }
      if (isa<InterfaceType>(C.under(N))) {
        if (C.neverNilKind(N)) {
          if (Why)
            *Why = "interfaces with methods are never nil";
          return false;
        }
        return true;
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
      // An instance of a generic type is answered from the declaration: its
      // field types mention the type parameters, and the answer for the
      // arguments follows from the parameters it rests on (PolyMask). So a
      // chain of n instances over fresh type parameters costs n steps in all,
      // not n per instance. The answer is exact and a function of the
      // instance alone.
      if (!IgnoreForeign && N->Orig && !N->Orig->TParams.empty() &&
          N->TArgs.size() == N->Orig->TParams.size() &&
          !llvm::is_contained(InProgress, N->Orig)) {
        NamedType *O = N->Orig;
        uint8_t OSt = O->ZeroState.load(std::memory_order_acquire);
        if (OSt == 0) {
          ZeroQuery Q(C, false);
          Q.has(O, nullptr); // writes O->ZeroState when the answer is exact
          OSt = O->ZeroState.load(std::memory_order_acquire);
        }
        if (OSt != 0) {
          bool Answer = true, Pol = false, ArgCut = false;
          uint64_t Used = 0;
          if (OSt == 2)
            Answer = false;
          else if (OSt == 3) {
            uint64_t M = O->PolyMask.load(std::memory_order_relaxed);
            for (unsigned J = 0; J < N->TArgs.size() && Answer; ++J) {
              if (!(M & bitOf(J)) && !(J >= 63 && (M & bitOf(63))))
                continue;
              ZeroQuery Q(C, false);
              bool QHas = Q.has(N->TArgs[J], nullptr);
              ArgCut |= Q.Cut;
              if (QHas)
                continue;
              Used |= Q.PolyMask;
              if (Q.Poly)
                Pol = true;
              else
                Answer = false; // an argument that surely has none
            }
            if (Answer && Pol)
              Answer = false;
            else if (!Answer)
              Pol = false;
            if (!Answer && Pol)
              PolyMask |= Used;
          }
          Poly = !Answer && Pol;
          // An argument query that was cut short is not exact: not stored,
          // and the enclosing answers are not stored either.
          if (ArgCut)
            Cut = true;
          else
            N->ZeroState.store(Answer ? 1
                               : Poly ? 3
                                      : 2,
                               std::memory_order_release);
          if (Answer || !Why)
            return Answer;
        }
      }
      uint8_t St = N->ZeroState.load(std::memory_order_acquire);
      // A long chain of named types is resolved from the bottom up on the
      // heap, so no depth limit decides the answer (and the stack stays
      // small).
      if (St == 0 && Depth >= PrewarmDepth && !IgnoreForeign) {
        prewarm(N);
        St = N->ZeroState.load(std::memory_order_acquire);
      }
      if (St == 1 || (St >= 2 && !Why && !IgnoreForeign)) {
        Poly = St == 3;
        return St == 1;
      }
      if (St >= 2 && Why && !IgnoreForeign && Depth >= SummaryDepth) {
        // A chain of instances is named by its generic declaration: walking
        // it would instantiate every level again for every error.
        if (N->Orig)
          *Why = "it nests more than " + std::to_string(SummaryDepth) +
                 " levels of " + std::string(N->Orig->Obj->Name) +
                 " instances, and " +
                 (St == 3 ? "a type parameter allows types with none (GEN-5)"
                          : "one of them has no zero value");
        else
          *Why = deepWhy(N);
        Poly = St == 3;
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
      uint64_t SavedMask = PolyMask;
      PolyMask = 0;
      bool R = has(U, Why);
      bool RPoly = !R && Poly;
      if (!Cut && !IgnoreForeign) {
        if (RPoly && N->isGeneric())
          N->PolyMask.store(PolyMask, std::memory_order_relaxed);
        N->ZeroState.store(R ? 1 : RPoly ? 3 : 2, std::memory_order_release);
      }
      PolyMask |= SavedMask;
      Poly = RPoly;
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

bool Checker::lacksZeroValueForSure(Type *T) {
  if (!own() || !T || !isValid(T))
    return false;
  ZeroQuery Q(*this, false);
  return !Q.has(T, nullptr) && !Q.Poly;
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
  return requiredMeansNeverNil(F->Ty);
}

bool Checker::requiredMeansNeverNil(Type *Ty) {
  Type *U = under(Ty);
  // `error`, `any` and the other empty interfaces, maps and channels; a func
  // and an interface with methods are never nil without the attribute.
  return (isa<InterfaceType>(U) && !neverNilKind(Ty)) ||
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
  Fun = unparen(Fun);
  if (auto *Inst = dyn_cast<IndexExpr>(Fun)) // mem.Replace[T](...)
    Fun = unparen(Inst->X);
  auto *Sel = dyn_cast<SelectorExpr>(Fun);
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

//===----------------------------------------------------------------------===//
// Writes into maps and slices (P03.19, ADR-0018 amendment point 5, TYP-9)
//
// Two checks, each decided in one place (boxPlace / rejectBoxWrite for the
// box of an interface value, rejectTempMapWrite for a temporary map). Every
// way the language can reach a write calls them:
//  - `p[k] = v`, `p[k] op= v`, `p[k]++`, `p[i].f = v` (lhsVar, so also range
//    clauses, select receives and tuple assignments);
//  - the builtins that write their argument: delete, clear, copy, append (the
//    spare capacity of a slice that a conversion shares with the box), move
//    (leaves zero bytes);
//  - making a pointer into the storage: `&p[i]`, a pointer-receiver call or
//    method value on a part, an `&` range variable, a slice to array pointer
//    conversion `(*[N]T)(p)`, `unsafe.SliceData(p)`. Making it is the error,
//    so no pointer into the box is ever held anywhere.
// A temporary map is a place that only lives until the end of the statement
// (placeKind Temp: a call result, a literal, a receive, a field or element of
// one by value; a `Unique[*T]` result owns its target, a `*T` or `Shared[*T]`
// target is a place).
//
// The box: only DIRECT writes are checked here. The target is rooted, in the
// syntax, at a type switch or assertion binding (TYP-9, isBinding), an
// Option Get binding of a part of the box (BoxGetBindings), or an assertion,
// and reached from it through fields and array elements by value, slice and
// map elements, slice expressions and conversions that share storage, with
// no variable or call in between (boxPlace). A write through a variable or a
// call result that stands for a part of the box (a ref `r := v[1:]`, a range
// variable over a part, `head(v)[0] = 1`, a value a helper returns or keeps)
// is not seen: P03.21 makes these bindings views (ADR-0020), read-only all
// the way down by their type, which closes every indirect write and escape
// in one place. Also not covered: a call that hands a part of the box on as
// a borrowed argument or receiver (`fill(m)`, `sort.Ints(s)`), as for any
// borrowed map parameter (PAR-10); P04.09 adds a "writes its parameter" bit.
//===----------------------------------------------------------------------===//

bool Checker::boxBinding(const Var *V) {
  return (isBinding(V) && !V->OptionBinding) || BoxGetBindings.count(V);
}

Checker::BoxKind Checker::boxPlace(const ast::Expr *Ex) {
  Ex = unparen(Ex);
  if (const ast::Expr *Arg = sharedConversionArg(Ex))
    return boxPlace(Arg);
  switch (Ex->getKind()) {
  case ast::Node::Kind::Ident: {
    auto *V = dyn_cast_or_null<Var>(identObj(Ex));
    return V && boxBinding(V) ? BoxKind::Direct : BoxKind::None;
  }
  case ast::Node::Kind::TypeAssertExpr:
    return BoxKind::Direct;
  case ast::Node::Kind::SelectorExpr: {
    // A field by value; behind a pointer it is no part of the box.
    auto *Sel = cast<SelectorExpr>(Ex);
    auto It = I.Selections.find(Sel);
    if (It == I.Selections.end() || It->second.K != Selection::FieldVal ||
        It->second.Indirect)
      return BoxKind::None;
    return boxPlace(Sel->X);
  }
  case ast::Node::Kind::IndexExpr: {
    auto *Ix = cast<IndexExpr>(Ex);
    Type *T = I.typeOf(Ix->X);
    Type *U = T ? coreType(T) : nullptr;
    if (U && isa<RefType>(U))
      U = under(cast<RefType>(U)->Elem);
    if (U && isa<PointerType>(U))
      return BoxKind::None;
    BoxKind K = boxPlace(Ix->X);
    if (K == BoxKind::None)
      return K;
    // An array element is part of the array; an element of a slice or map
    // (or of a type we cannot see) is inside that container.
    return U && isa<sema::ArrayType>(U) ? K : BoxKind::Via;
  }
  case ast::Node::Kind::SliceExpr: {
    auto *Sl = cast<SliceExpr>(Ex);
    Type *T = I.typeOf(Sl->X);
    if (T && isa<PointerType>(under(T)))
      return BoxKind::None;
    return boxPlace(Sl->X);
  }
  default:
    return BoxKind::None;
  }
}

void Checker::noteBoxGet(const Var *V, const ast::Expr *Opt) {
  if (own() && boxPlace(Opt) != BoxKind::None)
    BoxGetBindings.insert(V);
}

/// The expression at the bottom of a place chain (the variable or assertion
/// the box part is reached from), for the hint.
static const ast::Expr *placeRoot(Checker &C, const ast::Expr *Ex) {
  for (;;) {
    Ex = unparen(Ex);
    if (const ast::Expr *Arg = C.sharedConversionArg(Ex)) {
      Ex = Arg;
      continue;
    }
    if (auto *Sel = dyn_cast<SelectorExpr>(Ex))
      Ex = Sel->X;
    else if (auto *Ix = dyn_cast<IndexExpr>(Ex))
      Ex = Ix->X;
    else if (auto *Sl = dyn_cast<SliceExpr>(Ex))
      Ex = Sl->X;
    else
      return Ex;
  }
}

bool Checker::rejectBoxWrite(const ast::Expr *At, const ast::Expr *Place,
                             bool AnyDepth, const std::string &What) {
  if (!own())
    return false;
  BoxKind K = boxPlace(Place);
  if (K == BoxKind::None || (!AnyDepth && K != BoxKind::Via))
    return false;
  std::string Root = str(placeRoot(*this, Place));
  // What kind of container the write goes into: the nearest map or slice.
  std::string Kind = "a map or slice";
  for (const ast::Expr *Ex = unparen(Place); Ex;) {
    const ast::Expr *Base = nullptr;
    if (auto *Ix = dyn_cast<IndexExpr>(Ex))
      Base = Ix->X;
    else if (AnyDepth && Ex == unparen(Place))
      Base = Ex;
    if (Base) {
      Type *T = I.typeOf(Base);
      Type *U = T ? coreType(T) : nullptr;
      if (U && isa<RefType>(U))
        U = under(cast<RefType>(U)->Elem);
      if (U && isa<sema::MapType>(U)) {
        Kind = "a map";
        break;
      }
      if (U && isa<SliceType>(U)) {
        Kind = "a slice";
        break;
      }
    }
    if (auto *Sel = dyn_cast<SelectorExpr>(Ex))
      Ex = unparen(Sel->X);
    else if (auto *Ix = dyn_cast<IndexExpr>(Ex))
      Ex = unparen(Ix->X);
    else if (auto *Sl = dyn_cast<SliceExpr>(Ex))
      Ex = unparen(Sl->X);
    else
      break;
  }
  const char *Copy = Root == "c" ? "d" : "c";
  err(At, diag::err_box_write,
      {What, Kind,
       "clone it first (`" + std::string(Copy) + " := " + Root +
           ".Clone()`, then write " + Copy +
           "), or put a pointer in the interface"});
  return true;
}

bool Checker::rejectTempMapWrite(const ast::Expr *At, const ast::Expr *MapEx) {
  if (!own())
    return false;
  const ast::Expr *M = unparen(MapEx);
  Type *T = I.typeOf(M);
  Type *U = T ? coreType(T) : nullptr;
  if (!U || !isa<sema::MapType>(U) || isa<RefType>(T))
    return false;
  // A conversion that shares storage is no new value: the write lands in its
  // operand (`N(m)["a"] = 1`), so that decides (as in boxPlace).
  const ast::Expr *Held = M;
  while (const ast::Expr *Arg = sharedConversionArg(Held))
    Held = unparen(Arg);
  if (boxPlace(Held) != BoxKind::None || placeKind(Held) != PlaceKind::Temp)
    return false;
  err(At, diag::err_map_temp_write, {str(At), str(M)});
  return true;
}

void Checker::checkContainerStore(const ast::Expr *Lhs) {
  if (!own())
    return;
  const ast::Expr *L = unparen(Lhs);
  if (rejectBoxWrite(Lhs, L, false, str(L)))
    return;
  if (auto *Ix = dyn_cast<IndexExpr>(L))
    rejectTempMapWrite(Lhs, Ix->X);
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
  // P03.19: a pointer into a map or slice of an interface box could write
  // into it (a binding itself and its by-value parts are the case above).
  if (rejectBoxWrite(Ex, Inner, true, "through " + str(Ex)))
    return;
  // ADR-0016 point 8: nil could be written through the pointer; only
  // mem.Replace(&c.F, v) may take it, and v must not be nil either.
  const Var *F = selectedField(Inner);
  if (!F || !isNeverNilField(F))
    return;
  if (unparen(Ex) != ReplaceArg)
    err(Ex, diag::err_required_addr, {str(Inner), requiredWhy(F)});
  else if (ReplaceNil)
    err(ReplaceNil, diag::err_required_nil_store, {str(Inner), requiredWhy(F)});
  else {
    // The new value must be known non-nil too: callExpr checks it once the
    // arguments are (TYP-15).
    ReplaceField = F;
    ReplacePlace = str(Inner);
  }
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
  else
    rejectBoxWrite(Sel, Sel->X, true,
                   "through the pointer receiver of " + str(Sel));
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
