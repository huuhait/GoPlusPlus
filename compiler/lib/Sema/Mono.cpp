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
// type (or inside a bigger unit: `F[U*M]` for a unit parameter, GEN-14). A
// cycle with a weight-1 edge makes the type arguments grow without end. types2
// finds it with a Bellman-Ford variant (quadratic in the worst case); here the
// strongly connected components decide it in linear time: a weight-1 edge
// inside one component lies on a cycle.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/DenseMap.h"

#include <algorithm>
#include <functional>

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

static void walkMonoSources(Type *Arg, bool Locals,
                            llvm::function_ref<void(Type *, bool)> Fn) {
  SmallVector<Type *, 16> Work{Arg};
  llvm::SmallPtrSet<const Type *, 16> Seen;
  while (!Work.empty()) {
    Type *X = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    switch (X->getKind()) {
    case Type::Kind::TypeParam:
      Fn(X, X == Arg);
      break;
    case Type::Kind::Basic:
      // GEN-14: a unit (`float64[U*M]`, the unit argument `U*M`) that
      // mentions a unit parameter. The parameter alone is the TypeParam
      // itself (TypeContext::unitArgType), so here it is always inside a
      // bigger unit: a growing edge, as `*T` is for a type (`F[U]` calling
      // `F[U*M]` makes F[M], F[M*M], ...).
      if (Locals)
        if (auto *B = cast<BasicType>(X); B->Unit && B->Unit->HasParam)
          for (const UnitDim &D : B->Unit->Dims)
            if (D.Param)
              Fn(const_cast<TypeParamType *>(D.Param), false);
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
      if (Type *Ext = cast<InterfaceType>(X)->Extends)
        Work.push_back(Ext);
      break;
    case Type::Kind::Named:
      // The arguments of an instance; a named type's own body is not part
      // of the argument (its instantiations are edges of their own).
      if (Locals)
        if (NamedType *O = cast<NamedType>(X)->origin();
            !O->ImplicitTParams.empty())
          Fn(O, X == Arg);
      for (Type *A : cast<NamedType>(X)->TArgs)
        Work.push_back(A);
      break;
    }
  }
}

void sema::forEachTypeParam(
    Type *Arg, llvm::function_ref<void(TypeParamType *, bool)> Fn) {
  walkMonoSources(Arg, false, [&](Type *X, bool Exact) {
    Fn(cast<TypeParamType>(X), Exact);
  });
}

void sema::forEachMonoSource(Type *Arg,
                             llvm::function_ref<void(Type *, bool)> Fn) {
  walkMonoSources(Arg, true, Fn);
}

void Checker::recordMono(SourceLoc Pos, NamedType *Orig,
                         ArrayRef<TypeParamType *> TParams,
                         ArrayRef<Type *> TArgs, ArrayRef<SourceLoc> ArgPos,
                         bool BindRecv) {
  if (NoMono)
    return;
  MonoInst M;
  M.Pos = Pos;
  if (!S.Opts.GoCompat) {
    M.SkipOwn = MonoInSig > 0;
    M.SrcRaw =
        !M.SkipOwn && MonoFn && MonoFn->Recv && !MonoFn->OwnTParams.empty();
    M.DstRaw = BindRecv;
  }
  M.Orig = Orig;
  M.TParams.assign(TParams.begin(), TParams.end());
  M.TArgs.assign(TArgs.begin(), TArgs.end());
  M.ArgPos.assign(ArgPos.begin(), ArgPos.end());
  // Only an argument that mentions a type parameter (or a type declared in a
  // generic function) can be part of a cycle.
  bool Any = false;
  for (Type *A : M.TArgs)
    forEachMonoSource(A, [&](Type *, bool) { Any = true; });
  if (Any)
    MonoInsts.push_back(std::move(M));
}

void Checker::monoBindRecv(SourceLoc Pos, const ast::Expr *Ex) {
  if (S.Opts.GoCompat || NoMono)
    return;
  while (Ex) {
    if (auto *P = dyn_cast<ast::ParenExpr>(Ex))
      Ex = P->X;
    else if (auto *Ix = dyn_cast<ast::IndexExpr>(Ex))
      Ex = Ix->X;
    else
      break;
  }
  auto *Sel = dyn_cast_or_null<ast::SelectorExpr>(Ex);
  auto It = Sel ? I.Selections.find(Sel) : I.Selections.end();
  if (It == I.Selections.end())
    return;
  auto *F = dyn_cast_or_null<Func>(It->second.Obj);
  if (!F || !F->RecvInst || !F->Origin || F->Origin->OwnTParams.empty() ||
      F->Origin->RecvTParams.size() != F->RecvInst->TArgs.size())
    return;
  recordMono(Pos, nullptr, F->Origin->RecvTParams, F->RecvInst->TArgs, {},
             /*BindRecv=*/true);
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
  llvm::DenseMap<const Type *, unsigned> Index;
  SmallVector<Type *, 16> Verts; // type parameters and types declared in
                                 // generic functions
  std::vector<Edge> Edges;
  // types2 localNamedVertex: a type declared inside a generic function is
  // implicitly parameterized by the type parameters around it (weight 1),
  // from the first time it is mentioned.
  std::function<unsigned(Type *, bool)> Vertex = [&](Type *P,
                                                     bool Raw) -> unsigned {
    if (auto *TP = dyn_cast<TypeParamType>(P); TP && !Raw)
      if (auto C = Canon.find(TP); C != Canon.end())
        P = C->second;
    auto [It, New] = Index.try_emplace(P, Verts.size());
    if (!New)
      return It->second;
    unsigned Id = It->second; // (try_emplace of the others may rehash)
    Verts.push_back(P);
    if (auto *N = dyn_cast<NamedType>(P))
      for (TypeParamType *Q : N->ImplicitTParams) {
        unsigned From = Vertex(Q, false);
        Edges.push_back({From, Id, 1u, N->Obj->Pos, Q, nullptr});
      }
    return Id;
  };
  for (const MonoInst &M : Insts)
    for (size_t K = 0; K < M.TParams.size() && K < M.TArgs.size(); ++K) {
      Type *Arg = M.TArgs[K];
      // GEN-14 (owner decision 2026-10-30): in a method's signature, an
      // argument made with the method's own type parameter (`Vec[U/V]` in
      // `Per[V unit]`, `List[Pair[T, U]]` in `Zip[U any]`) depends on what
      // each call gives: no edge. Only the receiver type's own parameters
      // can grow a cycle there. A body keeps the full rule (SrcRaw).
      if (M.SkipOwn && !MonoMethodOwn.empty()) {
        bool OwnArg = false;
        forEachMonoSource(Arg, [&](Type *X, bool) {
          if (auto *Q = dyn_cast<TypeParamType>(X))
            OwnArg |=
                MonoMethodOwn.count(Q->RenamedFrom ? Q->RenamedFrom : Q) > 0;
        });
        if (OwnArg)
          continue;
      }
      unsigned To = Vertex(M.TParams[K], M.DstRaw);
      forEachMonoSource(Arg, [&](Type *Q, bool Exact) {
        unsigned From = Vertex(Q, M.SrcRaw);
        Edges.push_back({From, To, Exact ? 0u : 1u,
                         K < M.ArgPos.size() ? M.ArgPos[K] : M.Pos, Arg,
                         M.Orig});
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
    SmallVector<const Edge *, 8> Cycle{Bad}; // forward: Bad, then To ... From
    for (unsigned V = Bad->From; V != Bad->To;) {
      const Edge &E = Edges[Pre[V]];
      Cycle.push_back(&E);
      V = E.From;
    }
    std::reverse(Cycle.begin() + 1, Cycle.end());
    for (unsigned V : Queue) // only what this search touched
      Reached[V] = false;
    // Reported at the first type parameter of the cycle in source order.
    auto ObjOf = [&](Type *V) -> TypeName * {
      if (auto *TP = dyn_cast<TypeParamType>(V))
        return TP->Obj;
      return cast<NamedType>(V)->Obj;
    };
    Type *P0 = Verts[Bad->To];
    for (const Edge *E : Cycle) {
      Type *P = Verts[E->To];
      if (ObjOf(P) &&
          (!ObjOf(P0) || ObjOf(P)->Pos.getRaw() < ObjOf(P0)->Pos.getRaw()))
        P0 = P;
    }
    Err Er = err(ObjOf(P0) ? ObjOf(P0)->Pos : Bad->Pos,
                 diag::err_instantiation_cycle);
    // As types2's reportInstanceLoop: the edge into the reported vertex
    // first, then backwards round the cycle.
    size_t Start = 0;
    for (size_t K = 0; K < Cycle.size(); ++K)
      if (Verts[Cycle[K]->To] == P0)
        Start = K;
    for (size_t K = 0; K < Cycle.size(); ++K) {
      const Edge *E = Cycle[(Start + Cycle.size() - K) % Cycle.size()];
      Type *P = Verts[E->To];
      Er.note(E->Pos, std::string(ObjOf(P) ? StringRef(ObjOf(P)->Name)
                                           : StringRef("?")) +
                          (isa<NamedType>(P) ? " implicitly parameterized by "
                                             : " instantiated as ") +
                          str(E->Arg));
    }
  }
  return true;
}

void Checker::noteImplicitTParams(TypeName *TN) {
  auto *N = dyn_cast_or_null<NamedType>(TN->Ty);
  if (!N || TN->IsAlias || NoMono)
    return;
  SmallVector<TypeParamType *, 4> Around;
  for (Scope *Sc = E.Sc; Sc && !Sc->isPackageScope(); Sc = Sc->Parent) {
    // Type parameters live in a type parameter list or (a receiver's) in a
    // function scope; the blocks of a body hold the other locals. The
    // function scope holds the body's top-level locals as well, so its type
    // parameters are collected once per scope (all are declared before the
    // body is checked): a function with many local types costs one scan.
    StringRef Kind = Sc->Comment ? Sc->Comment : "";
    if (Kind != "type parameters" && Kind != "function")
      continue;
    auto It = ScopeTParams.find(Sc);
    if (It == ScopeTParams.end()) {
      SmallVector<TypeParamType *, 4> Own;
      for (auto &KV : Sc->Elems)
        if (auto *T = dyn_cast<TypeName>(KV.second); T && !T->IsAlias)
          if (auto *TP = dyn_cast_or_null<TypeParamType>(T->Ty))
            Own.push_back(TP);
      It = ScopeTParams.try_emplace(Sc, std::move(Own)).first;
    }
    Around.append(It->second.begin(), It->second.end());
  }
  if (Around.empty())
    return;
  // The scopes hold their objects in no fixed order; the cycle report must
  // not depend on it.
  llvm::sort(Around, [](TypeParamType *A, TypeParamType *B) {
    return A->Obj->Pos.getRaw() < B->Obj->Pos.getRaw();
  });
  N->ImplicitTParams = Mem.copy(ArrayRef<TypeParamType *>(Around));
}
