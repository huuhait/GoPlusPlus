//===- InitOrder.cpp - Package initialization cycles ----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's initorder.go (BSD-style license, Copyright The Go
// Authors): builds the dependency graph of the package-level constants,
// variables and functions, drops the function nodes and reports every
// initialization cycle. The initialization order itself is not computed yet.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"

#include <algorithm>
#include <set>

using namespace gopp;
using namespace gopp::sema;
using llvm::dyn_cast;
using llvm::isa;

namespace {
struct InitNode {
  Object *Obj = nullptr;
  llvm::SmallPtrSet<InitNode *, 4> Succ, Pred;
  size_t NDeps = 0;
};
} // namespace

static SmallVector<Object *, 4> sortedDeps(PackageState &S, Object *O) {
  SmallVector<Object *, 4> Out;
  auto It = S.ObjMap.find(O);
  if (It == S.ObjMap.end())
    return Out;
  for (Object *D : It->second->Deps)
    Out.push_back(D);
  std::sort(Out.begin(), Out.end(),
            [](Object *A, Object *B) { return A->Order < B->Order; });
  return Out;
}

/// The strongly connected components of the declaration dependency graph
/// (Tarjan, with an explicit stack: a cycle of n objects is n deep). Comp[O]
/// is the component of O; Cyclic[Comp] says it holds a cycle (two objects, or
/// one that depends on itself).
namespace {
struct Components {
  llvm::DenseMap<Object *, unsigned> Comp;
  std::vector<bool> Cyclic;
};
} // namespace

static Components findComponents(PackageState &S, ArrayRef<Object *> Roots) {
  Components R;
  struct Frame {
    Object *O;
    SmallVector<Object *, 4> Deps;
    size_t Next = 0;
    unsigned Index = 0, Low = 0;
    bool SelfLoop = false;
  };
  llvm::DenseMap<Object *, unsigned> IndexOf;
  SmallVector<Object *, 32> Open; // Tarjan's stack
  llvm::DenseSet<Object *> OnOpen;
  std::vector<Frame> Stack;
  unsigned Counter = 0;
  auto Push = [&](Object *O) {
    Frame F;
    F.O = O;
    F.Deps = sortedDeps(S, O);
    F.Index = F.Low = IndexOf[O] = Counter++;
    Open.push_back(O);
    OnOpen.insert(O);
    Stack.push_back(std::move(F));
  };
  for (Object *Root : Roots) {
    if (IndexOf.count(Root))
      continue;
    Push(Root);
    while (!Stack.empty()) {
      Frame &F = Stack.back();
      if (F.Next < F.Deps.size()) {
        Object *D = F.Deps[F.Next++];
        if (D == F.O)
          F.SelfLoop = true;
        auto It = IndexOf.find(D);
        if (It == IndexOf.end())
          Push(D); // (invalidates F)
        else if (OnOpen.count(D))
          F.Low = std::min(F.Low, It->second);
        continue;
      }
      Frame Done = std::move(Stack.back());
      Stack.pop_back();
      if (!Stack.empty())
        Stack.back().Low = std::min(Stack.back().Low, Done.Low);
      if (Done.Low != Done.Index)
        continue;
      unsigned Id = R.Cyclic.size();
      size_t Size = 0;
      Object *M;
      do {
        M = Open.pop_back_val();
        OnOpen.erase(M);
        R.Comp[M] = Id;
        ++Size;
      } while (M != Done.O);
      R.Cyclic.push_back(Size > 1 || Done.SelfLoop);
    }
  }
  return R;
}

/// The path of dependencies From -> ... -> To, as the first one a depth-first
/// walk in declaration order finds, in the reverse order, To first (types2's
/// findPath; the walk has an explicit stack, and stays in the component of
/// From, where all the paths back to From are).
static bool findPath(PackageState &S, const Components &Cs, Object *From,
                     Object *To, SmallVector<Object *, 8> &Path) {
  unsigned Comp = Cs.Comp.lookup(From);
  struct Frame {
    Object *O;
    SmallVector<Object *, 4> Deps;
    size_t Next = 0;
  };
  llvm::DenseSet<Object *> Seen;
  std::vector<Frame> Stack;
  auto Push = [&](Object *O) {
    Seen.insert(O);
    Stack.push_back({O, sortedDeps(S, O), 0});
  };
  Push(From);
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    if (F.Next == F.Deps.size()) {
      Stack.pop_back();
      continue;
    }
    Object *D = F.Deps[F.Next++];
    if (D == To) {
      Path.push_back(D);
      for (size_t K = Stack.size(); K-- > 1;)
        Path.push_back(Stack[K].O);
      return true;
    }
    auto C = Cs.Comp.find(D);
    if (C == Cs.Comp.end() || C->second != Comp || Seen.count(D))
      continue;
    Push(D); // (invalidates F)
  }
  return false;
}

void Checker::checkInitOrder(std::vector<InitEntry> &Out) {
  llvm::DenseMap<Object *, InitNode *> Nodes;
  std::vector<std::unique_ptr<InitNode>> Store;
  SmallVector<InitNode *, 32> Order;
  for (Object *O : S.PkgObjects) {
    if (!S.ObjMap.count(O) || !(isa<Const>(O) || isa<Var>(O) || isa<Func>(O)))
      continue;
    Store.push_back(std::make_unique<InitNode>());
    Store.back()->Obj = O;
    Nodes[O] = Store.back().get();
    Order.push_back(Store.back().get());
  }
  for (InitNode *N : Order)
    for (Object *D : S.ObjMap[N->Obj]->Deps) {
      auto It = Nodes.find(D);
      if (It == Nodes.end())
        continue;
      N->Succ.insert(It->second);
      It->second->Pred.insert(N);
    }
  // Remove the function nodes: connect their predecessors to their successors.
  for (InitNode *N : Order) {
    if (!isa<Func>(N->Obj))
      continue;
    for (InitNode *P : N->Pred) {
      if (P == N)
        continue;
      for (InitNode *Sc : N->Succ) {
        if (Sc == N)
          continue;
        P->Succ.insert(Sc);
        Sc->Pred.insert(P);
      }
      P->Succ.erase(N);
    }
    for (InitNode *Sc : N->Succ)
      Sc->Pred.erase(N);
  }
  SmallVector<InitNode *, 32> Live;
  for (InitNode *N : Order)
    if (!isa<Func>(N->Obj)) {
      N->NDeps = N->Succ.size();
      Live.push_back(N);
    }
  // Only an object on a cycle has a path back to itself (so the walk below
  // runs for those, and a chain that ends in a cycle costs nothing per link).
  SmallVector<Object *, 32> Roots;
  for (InitNode *N : Live)
    Roots.push_back(N->Obj);
  Components Cs = findComponents(S, Roots);
  llvm::DenseSet<DeclInfo *> Emitted;
  using Key = std::pair<size_t, std::pair<uint32_t, InitNode *>>;
  std::set<Key> Queue;
  auto KeyOf = [](InitNode *N) { return Key{N->NDeps, {N->Obj->Order, N}}; };
  for (InitNode *N : Live)
    Queue.insert(KeyOf(N));
  while (!Queue.empty()) {
    InitNode *N = Queue.begin()->second.second;
    Queue.erase(Queue.begin());
    if (N->NDeps > 0) {
      SmallVector<Object *, 8> Cycle;
      auto C = Cs.Comp.find(N->Obj);
      if (C != Cs.Comp.end() && Cs.Cyclic[C->second] &&
          findPath(S, Cs, N->Obj, N->Obj, Cycle)) {
        // Cycle is the reverse of the path; types2 reports it as built.
        Object *O = Cycle[0];
        if (Cycle.size() == 1) {
          err(O->Pos, diag::err_init_cycle_self, {std::string(O->Name)});
        } else {
          Err E2 = err(O->Pos, diag::err_init_cycle, {std::string(O->Name)});
          for (size_t J = Cycle.size(); J-- > 0;) {
            Object *Next = Cycle[J];
            E2.note(O->Pos, std::string(O->Name) + " refers to " +
                                std::string(Next->Name));
            O = Next;
          }
        }
      }
    }
    // Record the initialization (types2's initorder.go emits it here): a
    // variable with an initializer, once for `var a, b = f()`.
    if (auto *V = dyn_cast<Var>(N->Obj)) {
      auto DI = S.ObjMap.find(V);
      DeclInfo *D = DI == S.ObjMap.end() ? nullptr : DI->second;
      if (D && D->Init && Emitted.insert(D).second) {
        InitEntry E;
        E.Init = D->Init;
        E.Lhs = D->Lhs.empty() ? Mem.copy(ArrayRef<Var *>(V)) : D->Lhs;
        Out.push_back(E);
      }
    }
    for (InitNode *P : N->Pred) {
      if (!Queue.erase(KeyOf(P)))
        continue;
      if (P->NDeps > 0)
        --P->NDeps;
      Queue.insert(KeyOf(P));
    }
  }
}
