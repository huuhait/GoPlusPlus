//===- MethodSets.cpp - Method sets for the lowering of interfaces --------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.06. Which method a call through an interface reaches is the checker's
// decision (Lookup.cpp: embedding, shadowing, pointer receivers, ambiguity).
// The lowering asks it through this class and never decides again; so too
// which types hold a view (P04.07: a closure that outlives the call may not
// hold one, CLO-3a), the checker's containsView. A Checker is made once, on
// the context the package was checked with; it reports to a consumer that
// drops everything (the package is already checked).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/DenseSet.h"

#include <algorithm>
#include <cassert>

using namespace gopp;
using namespace gopp::sema;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace {
class NullConsumer : public DiagnosticConsumer {
public:
  void handleDiagnostic(const Diagnostic &) override {}
};

class NullImporter : public Importer {
public:
  Package *import(llvm::StringRef, std::string &Error) override {
    Error = "no imports while lowering";
    return nullptr;
  }
};
} // namespace

struct MethodSets::Impl {
  Impl(const CheckedPackage &CP)
      : CP(CP), Diags(*CP.SM, Sink), S(*CP.Ctx, *CP.SM, Imp, CP.Opts),
        C(S, Scratch, Mem, Diags) {
    S.Pkg = CP.Pkg;
    C.TypesDeclared = true;
  }
  const CheckedPackage &CP;
  NullConsumer Sink;
  NullImporter Imp;
  DiagnosticsEngine Diags;
  PackageState S;
  Info Scratch;
  Arena Mem;
  Checker C;
  uint64_t Steps = 0; // the facade's own loops (steps())
};

MethodSets::MethodSets(const CheckedPackage &CP, bool ScanMethods) {
  // (Every CheckedPackage comes from checkPackage, which sets both.)
  assert(CP.Ctx && CP.SM && "a package that checkPackage made");
  if (CP.Ctx && CP.SM) {
    P = std::make_unique<Impl>(CP);
    P->C.ScanMethods = ScanMethods;
  }
}

uint64_t MethodSets::steps() const {
  return P ? P->C.LookupSteps + P->Steps : 0;
}

uint64_t MethodSets::viewSteps() const { return P ? P->C.ViewSteps : 0; }

bool MethodSets::holdsView(Type *T) {
  // (Without a checker the answer that can only refuse: yes.)
  return !P || (T && P->C.containsView(T));
}

bool MethodSets::holdsBorrow(const Var *V) {
  return V->isBorrow() || holdsView(V->Ty);
}

MethodSets::~MethodSets() = default;

llvm::ArrayRef<Func *> MethodSets::interfaceMethods(Type *Iface) {
  if (!P)
    return {};
  auto *In = dyn_cast_or_null<InterfaceType>(P->C.under(Iface));
  if (!In)
    return {};
  return P->C.typeSet(In)->Methods;
}

static bool usable(Checker &C, const Func *F) {
  if (F->isOperator())
    return false;
  if (C.own() && (F->MoveRecv || !F->OwnTParams.empty()))
    return false;
  return true;
}

MethodHit MethodSets::lookup(Type *Dyn, const Func *M) {
  MethodHit Hit;
  if (!P)
    return Hit;
  Checker &C = P->C;
  Checker::LookupResult R = C.lookupFieldOrMethod(Dyn, false, M->Pkg, M->Name);
  auto *F = R.Obj ? dyn_cast<Func>(R.Obj) : nullptr;
  if (!F || R.Index.empty())
    return Hit;
  C.objDecl(F, nullptr);
  if (!usable(C, F))
    return Hit;
  Hit.F = F;
  Hit.Path.assign(R.Index.begin(), R.Index.end() - 1);
  Hit.ParentSteps = R.ParentSteps;
  Hit.Indirect = R.Indirect;
  return Hit;
}

void MethodSets::methodSet(Type *Dyn, llvm::SmallVectorImpl<MethodHit> &Out) {
  if (!P)
    return;
  Checker &C = P->C;
  // The names that may be in the set: the methods of the type, and of
  // everything it embeds. The lookup then decides, per name, as `x.M` does.
  llvm::SmallVector<std::pair<Package *, llvm::StringRef>, 16> Names;
  llvm::DenseSet<Type *> Seen;
  llvm::SmallVector<Type *, 8> Work{Dyn};
  while (!Work.empty()) {
    Type *T = Work.pop_back_val();
    bool IsPtr;
    T = C.deref(T, IsPtr);
    if (!T || !Seen.insert(T).second)
      continue;
    if (auto *N = dyn_cast<NamedType>(T)) {
      for (unsigned K = 0, E = C.numMethods(N); K < E; ++K, ++P->Steps)
        if (Func *F = C.namedMethod(N, K))
          Names.push_back({F->Pkg, F->Name});
    }
    Type *U = C.under(T);
    if (auto *St = dyn_cast_or_null<StructType>(U)) {
      for (Var *F : St->Fields) {
        ++P->Steps;
        if (F->Embedded || F->IsParent)
          Work.push_back(F->Ty);
      }
    } else if (auto *In = dyn_cast_or_null<InterfaceType>(U)) {
      for (Func *F : C.typeSet(In)->Methods) {
        ++P->Steps;
        Names.push_back({F->Pkg, F->Name});
      }
    }
  }
  std::sort(Names.begin(), Names.end(), [](auto &A, auto &B) {
    if (A.second != B.second)
      return A.second < B.second;
    llvm::StringRef PA = A.first ? A.first->Path : llvm::StringRef();
    llvm::StringRef PB = B.first ? B.first->Path : llvm::StringRef();
    return PA < PB;
  });
  Names.erase(std::unique(Names.begin(), Names.end()), Names.end());
  for (auto &[Pkg, Name] : Names) {
    // The package matters for an unexported name only (sameId).
    Func Probe;
    Probe.Name = Name;
    Probe.Pkg = Pkg;
    MethodHit H = lookup(Dyn, &Probe);
    if (H.F)
      Out.push_back(std::move(H));
  }
}

bool MethodSets::comparable(Type *T) {
  if (!P)
    return false;
  return P->C.comparableType(T, /*Dynamic=*/true, nullptr, nullptr);
}
