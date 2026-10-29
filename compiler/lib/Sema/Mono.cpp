//===- Mono.cpp - Instantiation cycles (types2 mono.go)
//--------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `type T[P any] struct{ s []T[[]P] }` instantiates T over ever larger types
// (T[int], T[[]int], T[[][]int], ...): Go rejects it as an "instantiation
// cycle" (P03.23). The vertices are type parameters. Each instantiation of a
// generic entity with type arguments that mention a type parameter Q of the
// enclosing declaration gives an edge from Q to the instantiated parameter:
// weight 0 when the argument is exactly Q, weight 1 when Q is inside a bigger
// type. A cycle with a weight-1 edge makes the type arguments grow without
// end. types2 finds it with a Bellman-Ford variant (quadratic in the worst
// case); here the strongly connected components decide it in linear time: a
// weight-1 edge inside one component lies on a cycle.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/DenseMap.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

void sema::forEachTypeParam(
    Type *Arg, llvm::function_ref<void(TypeParamType *, bool)> Fn) {
  SmallVector<Type *, 16> Work{Arg};
  llvm::SmallPtrSet<const Type *, 16> Seen;
  while (!Work.empty()) {
    Type *X = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    switch (X->getKind()) {
    case Type::Kind::TypeParam:
      Fn(cast<TypeParamType>(X), X == Arg);
      break;
    case Type::Kind::Pointer:
      Work.push_back(cast<PointerType>(X)->Elem);
      break;
    case Type::Kind::Slice:
      Work.push_back(cast<SliceType>(X)->Elem);
      break;
    case Type::Kind::Ref:
      Work.push_back(cast<RefType>(X)->Elem);
      break;
    case Type::Kind::Option:
      Work.push_back(cast<OptionType>(X)->Elem);
      break;
    case Type::Kind::Array:
      Work.push_back(cast<sema::ArrayType>(X)->Elem);
      break;
    case Type::Kind::Chan:
      Work.push_back(cast<sema::ChanType>(X)->Elem);
      break;
    case Type::Kind::Map:
      Work.push_back(cast<sema::MapType>(X)->Key);
      Work.push_back(cast<sema::MapType>(X)->Elem);
      break;
    case Type::Kind::Tuple:
      for (Type *E : cast<TupleType>(X)->Elems)
        Work.push_back(E);
      break;
    case Type::Kind::Signature:
      Work.push_back(cast<SignatureType>(X)->Params);
      Work.push_back(cast<SignatureType>(X)->Results);
      break;
    case Type::Kind::Struct:
      for (Var *F : cast<StructType>(X)->Fields)
        Work.push_back(F->Ty);
      break;
    case Type::Kind::Union:
      for (const Term &Tm : cast<UnionType>(X)->Terms)
        Work.push_back(Tm.T);
      break;
    case Type::Kind::Interface:
      for (Func *M : cast<InterfaceType>(X)->Methods)
        Work.push_back(M->Ty);
      for (Type *E : cast<InterfaceType>(X)->Embeddeds)
        Work.push_back(E);
      break;
    case Type::Kind::Named:
      // The arguments of an instance; a named type's own body is not part
      // of the argument (its instantiations are edges of their own).
      for (Type *A : cast<NamedType>(X)->TArgs)
        Work.push_back(A);
      break;
    default:
      break;
    }
  }
}

void Checker::recordMono(SourceLoc Pos, NamedType *Orig,
                         ArrayRef<TypeParamType *> TParams,
                         ArrayRef<Type *> TArgs) {
  if (NoMono)
    return;
  MonoInst M;
  M.Pos = Pos;
  M.Orig = Orig;
  M.TParams.assign(TParams.begin(), TParams.end());
  M.TArgs.assign(TArgs.begin(), TArgs.end());
  // Only an argument that mentions a type parameter can be part of a cycle.
  bool Any = false;
  for (Type *A : M.TArgs)
    forEachTypeParam(A, [&](TypeParamType *, bool) { Any = true; });
  if (Any)
    MonoInsts.push_back(std::move(M));
}

bool Checker::monoCheck(ArrayRef<MonoInst> Insts, bool Report) {
  struct Edge {
    unsigned From, To; // From is mentioned in the argument for To
    unsigned W;
    SourceLoc Pos;
    Type *Arg;
    NamedType *Orig;
  };
  llvm::DenseMap<const TypeParamType *, TypeParamType *> Canon;
  for (auto &[Method, Type] : MonoCanon)
    Canon[Method] = Type;
  llvm::DenseMap<const TypeParamType *, unsigned> Index;
  SmallVector<TypeParamType *, 16> Verts;
  auto Vertex = [&](TypeParamType *P) {
    if (auto C = Canon.find(P); C != Canon.end())
      P = C->second;
    auto [It, New] = Index.try_emplace(P, Verts.size());
    if (New)
      Verts.push_back(P);
    return It->second;
  };
  std::vector<Edge> Edges;
  for (const MonoInst &M : Insts)
    for (size_t K = 0; K < M.TParams.size() && K < M.TArgs.size(); ++K) {
      unsigned To = Vertex(M.TParams[K]);
      Type *Arg = M.TArgs[K];
      forEachTypeParam(Arg, [&](TypeParamType *Q, bool Exact) {
        Edges.push_back({Vertex(Q), To, Exact ? 0u : 1u, M.Pos, Arg, M.Orig});
      });
    }
  if (Edges.empty())
    return false;
  size_t NV = Verts.size();
  // Adjacency (CSR) in edge order.
  std::vector<unsigned> Start(NV + 1, 0), Adj(Edges.size());
  for (const Edge &E : Edges)
    ++Start[E.From + 1];
  for (size_t V = 0; V < NV; ++V)
    Start[V + 1] += Start[V];
  {
    std::vector<unsigned> Fill(Start.begin(), Start.end() - 1);
    for (unsigned I = 0; I < Edges.size(); ++I)
      Adj[Fill[Edges[I].From]++] = I;
  }
  // Tarjan's components, iteratively.
  constexpr unsigned None = ~0u;
  std::vector<unsigned> Idx(NV, None), Low(NV, 0), Comp(NV, None), Next(NV, 0);
  std::vector<unsigned> Stk, Path;
  std::vector<bool> OnStk(NV, false);
  unsigned Counter = 0, NComp = 0;
  for (unsigned R = 0; R < NV; ++R) {
    if (Idx[R] != None)
      continue;
    Path.push_back(R);
    Idx[R] = Low[R] = Counter++;
    Stk.push_back(R);
    OnStk[R] = true;
    while (!Path.empty()) {
      unsigned V = Path.back();
      if (Start[V] + Next[V] < Start[V + 1]) {
        unsigned To = Edges[Adj[Start[V] + Next[V]++]].To;
        if (Idx[To] == None) {
          Idx[To] = Low[To] = Counter++;
          Stk.push_back(To);
          OnStk[To] = true;
          Path.push_back(To);
        } else if (OnStk[To]) {
          Low[V] = std::min(Low[V], Idx[To]);
        }
        continue;
      }
      Path.pop_back();
      if (!Path.empty())
        Low[Path.back()] = std::min(Low[Path.back()], Low[V]);
      if (Low[V] == Idx[V]) {
        unsigned W;
        do {
          W = Stk.back();
          Stk.pop_back();
          OnStk[W] = false;
          Comp[W] = NComp;
        } while (W != V);
        ++NComp;
      }
    }
  }
  // A component with a weight-1 edge inside it is an instantiation cycle.
  // One error for each, at its first such edge (in recording order, so the
  // text does not depend on anything else); every generic type on such a
  // component is marked (InstCycle), so no walk follows its instances
  // without end (onInstCycle). It stays valid: every other error is still
  // found as Go finds it.
  std::vector<bool> BadComp(NComp, false);
  std::vector<const Edge *> Firsts;
  for (const Edge &E : Edges)
    if (E.W == 1 && Comp[E.From] == Comp[E.To] && !BadComp[Comp[E.To]]) {
      BadComp[Comp[E.To]] = true;
      Firsts.push_back(&E);
    }
  if (Firsts.empty())
    return false;
  // A type that holds an instance of itself by value is an invalid recursive
  // type first (E0140, as in Go); asked in the early pass (the final pass
  // runs only when there is no error, so no E0140 either).
  std::vector<bool> ByValue(NComp, false);
  if (!Report)
    for (const Edge &E : Edges)
      if (E.Orig && Comp[E.From] == Comp[E.To] && BadComp[Comp[E.To]] &&
          !ByValue[Comp[E.To]] && !validTypeWalk(E.Orig))
        ByValue[Comp[E.To]] = true;
  for (const Edge &E : Edges)
    if (E.Orig && Comp[E.From] == Comp[E.To] && BadComp[Comp[E.To]])
      E.Orig->InstCycle = true;
  // Go reports instantiation cycles only for a package without other errors
  // (types2 check.go: `if check.firstErr == nil { check.monomorph() }`); the
  // early pass only marks the types, so nothing walks an instance graph that
  // never ends, and reports nothing.
  if (!Report)
    return true;
  std::vector<unsigned> Pre(NV, None);
  std::vector<unsigned> Queue;
  std::vector<bool> Reached(NV, false);
  for (const Edge *Bad : Firsts) {
    if (ByValue[Comp[Bad->To]])
      continue;
    // The cycle: Bad, then the shortest way back from Bad->To to Bad->From
    // inside the component (breadth first, edges in order).
    Queue.assign(1, Bad->To);
    Reached[Bad->To] = true;
    for (size_t Q = 0; Q < Queue.size() && !Reached[Bad->From]; ++Q) {
      unsigned V = Queue[Q];
      for (unsigned A = Start[V]; A < Start[V + 1]; ++A) {
        const Edge &E = Edges[Adj[A]];
        if (Reached[E.To] || Comp[E.To] != Comp[Bad->To])
          continue;
        Reached[E.To] = true;
        Pre[E.To] = Adj[A];
        Queue.push_back(E.To);
      }
    }
    SmallVector<const Edge *, 8> Cycle{Bad};
    for (unsigned V = Bad->From; V != Bad->To;) {
      const Edge &E = Edges[Pre[V]];
      Cycle.push_back(&E);
      V = E.From;
    }
    for (unsigned V : Queue) // only what this search touched
      Reached[V] = false;
    // Reported at the first type parameter of the cycle in source order.
    TypeParamType *P0 = Verts[Bad->To];
    for (const Edge *E : Cycle) {
      TypeParamType *P = Verts[E->To];
      if (P->Obj && (!P0->Obj || P->Obj->Pos.getRaw() < P0->Obj->Pos.getRaw()))
        P0 = P;
    }
    Err Er =
        err(P0->Obj ? P0->Obj->Pos : Bad->Pos, diag::err_instantiation_cycle);
    for (const Edge *E : Cycle) {
      TypeParamType *P = Verts[E->To];
      Er.note(E->Pos, std::string(P->Obj ? StringRef(P->Obj->Name) : "?") +
                          " instantiated as " + str(E->Arg));
    }
  }
  return true;
}
