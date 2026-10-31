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
  /// subst/expand/mentionsTypeParam memos: types are walked once each.
  llvm::DenseMap<const Type *, bool> Mentions;
  llvm::DenseSet<const Type *> Expanded;
  llvm::DenseMap<const Type *, MethodSets::Extent> Extents;
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

bool MethodSets::isOwning(Type *T) {
  // (Without a checker the answer that can only refuse: yes.)
  return !P || (T && P->C.category(T) == Category::Owning);
}

Var::Borrow MethodSets::modeAt(const Var *V, Type *Ty) {
  if (!P || !Ty || V->Mode == Var::MoveParam || V->Mode == Var::Alias)
    return V->Mode;
  // PAR-1 and the receiver rule on the real type (a pointer receiver's type
  // is never owning).
  if (V->IsParam || V->IsRecv)
    return isOwning(Ty) ? Var::Borrowed : Var::Owned;
  if (V->Mode != Var::Borrowed)
    return V->Mode;
  // TYP-9: the binding is the generic code's view of the case type; plain
  // code binds a copy of a case type that is a plain copy.
  if (V->BoxBinding) {
    auto *R = dyn_cast<RefType>(Ty);
    return P->C.bindsBox(R ? R->Elem : Ty) ? Var::Borrowed : Var::Owned;
  }
  // RNG-1 (a range key or value) and ADR-0018 point 11 (`v, ok := o.Get()`).
  if (V->LoopVar || V->OptionBinding)
    return isOwning(Ty) ? Var::Borrowed : Var::Owned;
  return V->Mode;
}

bool MethodSets::holdsBorrowAt(const Var *V, Type *Ty) {
  Var::Borrow M = modeAt(V, Ty);
  if (M == Var::Borrowed || M == Var::Alias)
    return true;
  // (A copied-out box binding holds a plain copy: no view, no pointer.)
  if (V->BoxBinding)
    return false;
  return llvm::isa_and_nonnull<RefType>(Ty) || holdsView(Ty);
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

//===----------------------------------------------------------------------===//
// Generics (P04.08)
//===----------------------------------------------------------------------===//

namespace {
/// The types T is made of, one level (a named type's underlying type is not
/// one: its instances are reached through the arguments; Direct is set when T
/// itself is generic code: a type parameter, a unit with a parameter, a
/// generic type without arguments, a type declared in generic code).
void partsOf(const Type *T, llvm::SmallVectorImpl<Type *> &Out, bool &Direct) {
  Direct = false;
  switch (T->getKind()) {
  case Type::Kind::TypeParam:
    Direct = true;
    return;
  case Type::Kind::Basic: {
    auto *B = llvm::cast<BasicType>(T);
    Direct = B->Unit && B->Unit->HasParam;
    return;
  }
  case Type::Kind::Pointer:
    Out.push_back(llvm::cast<PointerType>(T)->Elem);
    return;
  case Type::Kind::Slice:
    Out.push_back(llvm::cast<SliceType>(T)->Elem);
    return;
  case Type::Kind::Ref:
    Out.push_back(llvm::cast<RefType>(T)->Elem);
    return;
  case Type::Kind::Option:
    Out.push_back(llvm::cast<OptionType>(T)->Elem);
    return;
  case Type::Kind::Array:
    Out.push_back(llvm::cast<ArrayType>(T)->Elem);
    return;
  case Type::Kind::Chan:
    Out.push_back(llvm::cast<ChanType>(T)->Elem);
    return;
  case Type::Kind::Map:
    Out.push_back(llvm::cast<MapType>(T)->Key);
    Out.push_back(llvm::cast<MapType>(T)->Elem);
    return;
  case Type::Kind::Tuple:
    for (Type *E : llvm::cast<TupleType>(T)->Elems)
      Out.push_back(E);
    return;
  case Type::Kind::Signature: {
    auto *S = llvm::cast<SignatureType>(T);
    Out.push_back(S->Params);
    Out.push_back(S->Results);
    return;
  }
  case Type::Kind::Struct:
    for (Var *F : llvm::cast<StructType>(T)->Fields)
      Out.push_back(F->Ty);
    return;
  case Type::Kind::Union:
    for (const Term &Tm : llvm::cast<UnionType>(T)->Terms)
      Out.push_back(Tm.T);
    return;
  case Type::Kind::Interface: {
    auto *I = llvm::cast<InterfaceType>(T);
    for (Func *M : I->Methods)
      Out.push_back(M->Ty);
    for (Type *E : I->Embeddeds)
      Out.push_back(E);
    if (I->Extends)
      Out.push_back(I->Extends);
    return;
  }
  case Type::Kind::Named: {
    auto *N = llvm::cast<NamedType>(T);
    Direct = !N->ImplicitTParams.empty() || (!N->Orig && !N->TParams.empty());
    for (Type *A : N->TArgs)
      Out.push_back(A);
    return;
  }
  }
}
} // namespace

bool MethodSets::mentionsTypeParam(Type *Root) {
  if (!P || !Root)
    return false;
  auto &Memo = P->Mentions;
  if (auto It = Memo.find(Root); It != Memo.end())
    return It->second;
  struct Frame {
    Type *T;
    llvm::SmallVector<Type *, 4> Kids;
    size_t Next = 0;
    bool Any = false;
  };
  llvm::SmallVector<Frame, 16> Stack;
  llvm::DenseSet<const Type *> Active;
  auto Push = [&](Type *T) {
    Frame F;
    F.T = T;
    partsOf(T, F.Kids, F.Any);
    Active.insert(T);
    Stack.push_back(std::move(F));
  };
  Push(Root);
  while (!Stack.empty()) {
    ++P->Steps;
    Frame &F = Stack.back();
    if (F.Any || F.Next == F.Kids.size()) {
      bool R = F.Any;
      Type *T = F.T;
      Active.erase(T);
      Memo[T] = R;
      Stack.pop_back();
      if (!Stack.empty() && R)
        Stack.back().Any = true;
      continue;
    }
    Type *K = F.Kids[F.Next++];
    if (!K)
      continue;
    if (auto It = Memo.find(K); It != Memo.end()) {
      if (It->second)
        F.Any = true;
      continue;
    }
    if (Active.count(K))
      continue;
    Push(K);
  }
  return Memo[Root];
}

void MethodSets::expand(Type *Root) {
  if (!P || !Root)
    return;
  Checker &C = P->C;
  llvm::SmallVector<Type *, 16> Work{Root};
  while (!Work.empty()) {
    Type *T = Work.pop_back_val();
    if (!T || !P->Expanded.insert(T).second)
      continue;
    ++P->Steps;
    llvm::SmallVector<Type *, 4> Kids;
    bool Direct;
    partsOf(T, Kids, Direct);
    for (Type *K : Kids)
      Work.push_back(K);
    if (auto *N = dyn_cast<NamedType>(T)) {
      if (N->Orig)
        C.expandNamed(N);
      // A defined type's fields hold instances too (never asked of the
      // checker if no body used them).
      if (N->Underlying)
        Work.push_back(N->Underlying);
      else if (N->DirectRhs)
        Work.push_back(N->DirectRhs);
    }
  }
}

Type *MethodSets::subst(Type *T, const TypeMap &Map) {
  if (!P || !T || Map.empty()) {
    expand(T);
    return T;
  }
  Type *R = P->C.substImpl(T, Map, 0);
  expand(R);
  return R;
}

bool MethodSets::resolveSelection(const Selection &In, Type *Recv,
                                  Selection &Out, Type *ElemSig) {
  Out = In;
  Out.Recv = Recv;
  if (!P)
    return false;
  Checker &C = P->C;
  auto *F = dyn_cast_or_null<Func>(In.Obj);
  // A built-in method (Option.Get, Clone, ...) has no declaration to find.
  if (F && F->IsBuiltinMethod)
    return true;
  if (In.K == Selection::MethodExpr)
    return true; // (a method expression is a func value: P04.07's)
  if (!In.Obj)
    return false;
  // A field read through a View is looked up in the element.
  Type *LookupT = Recv;
  if (auto *R = dyn_cast<RefType>(C.under(Recv)); R && !R->Borrowed)
    LookupT = R->Elem;
  Func *Pick = nullptr;
  if (F && F->Op != OpKind::None && ElemSig) {
    Pick = C.opAtInstance(LookupT, F->Op, ElemSig);
    if (!Pick)
      return false; // the built-in operator
  }
  Checker::LookupResult R =
      C.lookupFieldOrMethod(LookupT, /*Addressable=*/true,
                            In.Obj->Pkg ? In.Obj->Pkg : P->S.Pkg, In.Obj->Name);
  if (!R.Obj || R.Index.empty() || R.Ambiguous)
    return false;
  if (auto *RF = dyn_cast<Func>(R.Obj)) {
    if (In.K != Selection::MethodVal)
      return false;
    if (Pick) {
      RF = Pick;
      R.Obj = Pick;
    }
    C.objDecl(RF, nullptr);
    Out.Ty = RF->Ty;
  } else if (auto *V = dyn_cast<Var>(R.Obj)) {
    if (In.K != Selection::FieldVal)
      return false;
    if (!V->Ty)
      C.objDecl(V, nullptr);
    Out.Ty = V->Ty;
  } else {
    return false;
  }
  Out.Obj = R.Obj;
  Out.Index = P->Mem.copy(llvm::ArrayRef<int>(R.Index));
  Out.ParentSteps = R.ParentSteps;
  Out.Indirect = R.Indirect;
  return true;
}

const char *MethodSets::badOptionArg(Type *Arg) {
  return P ? P->C.badOptionArg(Arg) : nullptr;
}

const char *MethodSets::badViewArg(Type *Arg) {
  return P ? P->C.badViewArg(Arg) : nullptr;
}

std::string MethodSets::typeText(Type *T) { return P ? P->C.str(T) : ""; }

bool MethodSets::identical(Type *A, Type *B) {
  return P ? P->C.identical(A, B) : A == B;
}

MethodSets::Extent MethodSets::extent(Type *Root) {
  if (!P || !Root)
    return {};
  auto &Memo = P->Extents;
  if (auto It = Memo.find(Root); It != Memo.end())
    return It->second;
  // Post-order over the DAG; a frame is a type and the next of its parts.
  struct Frame {
    Type *T;
    llvm::SmallVector<Type *, 4> Kids;
    size_t Next = 0;
    Extent Acc;
  };
  llvm::SmallVector<Frame, 16> Stack;
  auto Push = [&](Type *T) {
    Frame F;
    F.T = T;
    bool Direct;
    partsOf(T, F.Kids, Direct);
    F.Acc.Height = 1;
    F.Acc.Size = 1;
    Stack.push_back(std::move(F));
  };
  auto Add = [](Extent &To, const Extent &Kid) {
    To.Height = std::max(To.Height, Kid.Height + 1);
    To.Size = std::min(SizeCap, To.Size + Kid.Size);
  };
  Push(Root);
  while (!Stack.empty()) {
    ++P->Steps;
    Frame &F = Stack.back();
    if (F.Next < F.Kids.size()) {
      Type *K = F.Kids[F.Next++];
      if (!K)
        continue;
      if (auto It = Memo.find(K); It != Memo.end()) {
        Add(F.Acc, It->second);
        continue;
      }
      Push(K); // (invalidates F)
      continue;
    }
    Extent E = F.Acc;
    Type *T = F.T;
    Stack.pop_back();
    Memo[T] = E;
    if (!Stack.empty())
      Add(Stack.back().Acc, E);
  }
  return Memo[Root];
}

bool MethodSets::hasZeroValue(Type *T) {
  return P ? P->C.hasZeroValue(T) : true;
}
