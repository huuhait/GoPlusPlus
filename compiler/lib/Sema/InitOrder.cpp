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

static bool findPath(PackageState &S, Object *From, Object *To,
                     llvm::DenseSet<Object *> &Seen,
                     SmallVector<Object *, 8> &Path) {
  if (!Seen.insert(From).second)
    return false;
  for (Object *D : sortedDeps(S, From)) {
    if (D == To) {
      Path.push_back(D);
      return true;
    }
    if (findPath(S, D, To, Seen, Path)) {
      Path.push_back(D);
      return true;
    }
  }
  return false;
}

void Checker::checkInitOrder() {
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
  using Key = std::pair<size_t, std::pair<uint32_t, InitNode *>>;
  std::set<Key> Queue;
  auto KeyOf = [](InitNode *N) { return Key{N->NDeps, {N->Obj->Order, N}}; };
  for (InitNode *N : Live)
    Queue.insert(KeyOf(N));
  while (!Queue.empty()) {
    InitNode *N = Queue.begin()->second.second;
    Queue.erase(Queue.begin());
    if (N->NDeps > 0) {
      llvm::DenseSet<Object *> Seen;
      SmallVector<Object *, 8> Cycle;
      if (findPath(S, N->Obj, N->Obj, Seen, Cycle)) {
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
    for (InitNode *P : N->Pred) {
      if (!Queue.erase(KeyOf(P)))
        continue;
      if (P->NDeps > 0)
        --P->NDeps;
      Queue.insert(KeyOf(P));
    }
  }
}
