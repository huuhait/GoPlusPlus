//===- Hierarchy.cpp - extends, method marks, abstract types ----*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Single inheritance (ADR-0023, TYP-16 to TYP-24, GEN-12).
//
// The model: `type C extends P { ... }` is a struct whose first field is the
// parent part (Var::IsParent, an embedded field named like P). So the lookup
// of an inherited field or method is the ordinary lookup through an embedded
// field (Lookup.cpp), the parent chain is the chain of first fields, and the
// layout, the zero-value, Clone, Send/Sync and drop rules reach the parent's
// fields as the fields of a field (the parent is the first field, so it is
// dropped last, after the child's own fields: TYP-24, P04.22).
//
// Every question is answered by a walk that never recurses (a chain of
// 200000 types is not a stack problem), and every answer that is a function
// of the type alone is kept on the type, stored only once the types are
// final, so no answer depends on which thread asks first:
//   - parentOf(N)          the parent, or null
//   - hierDepth(N)         the depth, with a skew-binary jump pointer, so
//                          extendsType ("is A an ancestor of C") takes
//                          O(log depth) steps (NamedType::HierDepth)
//   - hasTypeTable(N)      N or an ancestor has a @Virtual / @Abstract method
//                          ("tagged", TYP-22; NamedType::TableState)
//   - abstractState(N)     an @Abstract method is open on N's path, or N is
//                          marked @Abstract (NamedType::AbstractState)
// The declaration rules (name clashes, the marks and overrides, the open
// @Abstract methods) are checked by ONE depth-first walk of the `extends`
// forest of a batch of declarations (hierarchyBatch: the whole package once,
// and the local types of a body), with the names of the current path in a
// hash map and an undo log: each type is entered once and each name is
// looked up once, so the cost is linear in the number of declarations,
// however deep or wide the hierarchy is.
//
// The places that may hold only the parent part of a bigger value (the
// parent part `c.Payment`, `*p` and a value receiver of a type with a type
// table) are decided by one function, partialUse (EXP-15, ADR-0023 decision
// 16), next to the upcast rules.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include <set>

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::isa_and_nonnull;

//===----------------------------------------------------------------------===//
// Declarations: marks and the parent field
//===----------------------------------------------------------------------===//

static const char *markName(Func::Mark M) {
  switch (M) {
  case Func::Mark::Virtual:
    return "@Virtual";
  case Func::Mark::Abstract:
    return "@Abstract";
  case Func::Mark::Override:
    return "@Override";
  default:
    return "";
  }
}

static Func::Mark markOfAttr(StringRef N) {
  if (N == "Virtual")
    return Func::Mark::Virtual;
  if (N == "Abstract")
    return Func::Mark::Abstract;
  if (N == "Override")
    return Func::Mark::Override;
  return Func::Mark::None;
}

void Checker::readMethodMarks(const FuncDecl *FD, Func *Fn) {
  if (S.Opts.GoCompat)
    return;
  Func::Mark First = Func::Mark::None;
  bool Conflict = false;
  for (const Attribute *A : FD->Attrs) {
    if (A->Pkg || !A->Name)
      continue;
    Func::Mark M = markOfAttr(name(A->Name));
    if (M == Func::Mark::None)
      continue;
    if (!FD->Recv) {
      err(A->AtLoc, diag::err_method_mark_not_method,
          {markName(M), "the function " + std::string(name(FD->Name))});
      continue;
    }
    if (A->HasArgs)
      err(A->AtLoc, diag::err_attr_no_args, {markName(M)});
    if (First == Func::Mark::None)
      First = M;
    else if (!Conflict) {
      Conflict = true;
      err(A->AtLoc, diag::err_method_marks_conflict,
          {std::string(name(FD->Name))});
    }
  }
  Fn->MethodMark = First;
  if (First == Func::Mark::Abstract && FD->Body)
    err(FD->Name, diag::err_abstract_body, {std::string(name(FD->Name))});
}

void Checker::checkDeclMarks(const ast::Decl *D) {
  if (S.Opts.GoCompat)
    return;
  auto *GD = dyn_cast<GenDecl>(D);
  if (!GD)
    return;
  for (const Attribute *A : GD->Attrs) {
    if (A->Pkg || !A->Name)
      continue;
    StringRef AN = name(A->Name);
    Func::Mark M = markOfAttr(AN);
    if (M == Func::Mark::None)
      continue;
    const char *What = GD->Keyword == tok::kw_type    ? "a type declaration"
                       : GD->Keyword == tok::kw_var   ? "a variable declaration"
                       : GD->Keyword == tok::kw_const ? "a constant declaration"
                                                      : "an import";
    if (M != Func::Mark::Abstract || GD->Keyword != tok::kw_type) {
      err(A->AtLoc, diag::err_method_mark_not_method, {markName(M), What});
      continue;
    }
    if (A->HasArgs)
      err(A->AtLoc, diag::err_attr_no_args, {"@Abstract"});
    // TYP-19: a type attribute of an `extends` type.
    for (const Spec *Sp : GD->Specs) {
      auto *TS = cast<TypeSpec>(Sp);
      if (TS->AssignLoc.isValid())
        err(A->AtLoc, diag::err_abstract_attr_placement,
            {"the alias " + std::string(name(TS->Name))});
      else if (!TS->Extends)
        err(A->AtLoc, diag::err_abstract_attr_placement,
            {"type " + std::string(name(TS->Name)) +
             ", which does not extend a type"});
    }
  }
}

/// The name of the field a parent type expression gives (`Payment`,
/// `pkg.Base`, `Box[int]`), or empty if it is no type name.
static StringRef parentFieldName(const ast::Expr *E) {
  for (;;) {
    if (auto *P = dyn_cast<ParenExpr>(E))
      E = P->X;
    else if (auto *Ix = dyn_cast<IndexExpr>(E))
      E = Ix->X;
    else
      break;
  }
  if (auto *Id = dyn_cast<ast::Ident>(E))
    return Id->Name.str();
  if (auto *Sel = dyn_cast<SelectorExpr>(E))
    return Sel->Sel->Name.str();
  return {};
}

void Checker::addParentField(const ast::Expr *PE, StringRef DeclName,
                             llvm::function_ref<void(Type *, StringRef)> Add) {
  const ast::Expr *Bare = PE;
  while (auto *P = dyn_cast<ParenExpr>(Bare))
    Bare = P->X;
  std::string Decl(DeclName);
  const char *Kind = nullptr;
  if (isa<BadExpr>(Bare)) // the parser reported the missing parent
    return;
  if (isa<StarExpr>(Bare))
    Kind = "a pointer type cannot be a parent";
  else if (isa<ast::SliceType>(Bare) || isa<ast::ArrayType>(Bare) ||
           isa<ast::MapType>(Bare) || isa<ast::ChanType>(Bare) ||
           isa<ast::FuncType>(Bare))
    Kind = "the parent must be a struct type";
  else if (isa<ast::InterfaceType>(Bare))
    Kind = "an interface cannot be a parent";
  else if (isa<ast::StructType>(Bare))
    Kind = "the parent must be a named struct type";
  StringRef FieldName = parentFieldName(Bare);
  if (!Kind && FieldName.empty())
    Kind = "the parent must be a named struct type";
  if (Kind) {
    use(PE);
    err(PE, diag::err_extends_parent_kind, {"type " + Decl, str(PE), Kind});
    return;
  }
  // The parent part is not a standalone value: an abstract parent is fine.
  Type *PT = varType(PE, /*ValueUse=*/false);
  if (!isValid(PT))
    return;
  Add(PT, FieldName);
  // Whether it is a struct is known once the declaration of the parent is
  // done.
  later([this, PE, PT, Decl]() {
    Type *U = under(PT);
    if (!U || !isValid(U) || isa<StructType>(U))
      return;
    const char *Why = "the parent must be a struct type";
    if (isTypeParam(PT))
      Why = "a type parameter cannot be a parent";
    else if (isa<InterfaceType>(U))
      Why = "an interface cannot be a parent";
    else if (isa<PointerType>(U))
      Why = "a pointer type cannot be a parent";
    err(PE, diag::err_extends_parent_kind, {"type " + Decl, str(PT), Why});
  });
}

//===----------------------------------------------------------------------===//
// The hierarchy queries
//===----------------------------------------------------------------------===//

NamedType *Checker::parentOf(NamedType *N) {
  if (!N || N->Ptr != PtrKind::None || N->IsOption || N->IsView)
    return nullptr;
  auto *St = dyn_cast_or_null<StructType>(under(N));
  if (!St || St->Fields.empty() || !St->Fields[0]->IsParent)
    return nullptr;
  return dyn_cast<NamedType>(St->Fields[0]->Ty);
}

NamedType *Checker::ancestorInstanceOf(NamedType *From, NamedType *Orig) {
  // The chain index of the origins (shared, built once): Orig's depth, the
  // ancestor of From at that depth and its type arguments, O(log depth) (R24).
  NamedType *O = From->origin();
  TypeContext::ChainEntry E, EO;
  if (!From->InstCycle && !O->InstCycle &&
      (!From->Orig || O->TParams.size() == From->TArgs.size()) &&
      chainIndex(O, E) && chainIndex(Orig, EO)) {
    if (EO.Root != E.Root || EO.Depth > E.Depth)
      return nullptr;
    SmallVector<Type *, 4> Args;
    if (chainAncestor(O, E, EO.Depth, &Args) != Orig)
      return nullptr;
    if (Orig->TParams.empty())
      return Orig;
    if (From->Orig)
      for (Type *&T : Args)
        T = subst(T, O->TParams, From->TArgs);
    return Args.size() == Orig->TParams.size() ? instantiateNamed(Orig, Args)
                                               : nullptr;
  }
  // Before the types are final: up the chain, memoized per checker.
  SmallVector<NamedType *, 16> Path;
  llvm::SmallPtrSet<const NamedType *, 16> OnPath; // a ring is an error
  NamedType *Found = nullptr;
  for (NamedType *X = From; X && OnPath.insert(X).second; X = parentOf(X)) {
    if (X->Orig == Orig || X == Orig) {
      Found = X;
      break;
    }
    if (auto It = AncestorInst.find({X, Orig}); It != AncestorInst.end()) {
      Found = It->second;
      break;
    }
    Path.push_back(X);
  }
  for (NamedType *X : Path)
    AncestorInst[{X, Orig}] = Found;
  return Found;
}

NamedType *Checker::requiredAncestor(NamedType *N) {
  static const std::string Key = "\x02req";
  TypeContext::MemberMemo M;
  SmallVector<NamedType *, 16> Pending;
  NamedType *Found = nullptr;
  for (NamedType *X = N; X; X = parentOf(X)) {
    if (Ctx.memberMemoGet(X, Key, M)) {
      Found = M.Owner;
      break;
    }
    Pending.push_back(X);
    bool Has = false;
    if (auto *St = dyn_cast_or_null<StructType>(under(X)))
      for (Var *F : St->Fields)
        if (!F->IsParent && (F->Required || !hasZeroValue(F->Ty)))
          Has = true;
    if (Has) {
      Found = X;
      break;
    }
  }
  for (NamedType *X : Pending)
    Ctx.memberMemoPut(X, Key, {Found});
  return Found;
}

uint32_t Checker::hierDepth(NamedType *N) {
  if (!N)
    return 0;
  if (uint32_t D = N->HierDepth.load(std::memory_order_acquire))
    return D;
  // Before the types are final a parent may still change: no answer.
  if (!typesFinal())
    return 0;
  // Up to the first type whose depth is known (or a root). A ring (reported
  // and cut by checkExtendsCycles) ends the walk with no answer: Brent's
  // method, so no set is filled.
  SmallVector<NamedType *, 16> Path;
  NamedType *Cur = N;
  NamedType *Mark = nullptr;
  unsigned Steps = 0, Limit = 1;
  while (Cur && !Cur->HierDepth.load(std::memory_order_acquire)) {
    if (Cur == Mark || !declFinished(Cur))
      return 0;
    Path.push_back(Cur);
    if (++Steps == Limit) {
      Mark = Cur;
      Steps = 0;
      Limit *= 2;
    }
    Cur = parentOf(Cur);
  }
  // Down again: the depth is the parent's plus one, and the jump pointer is
  // the parent's jump of a jump when the two jumps have the same length
  // (Myers' skew-binary jump pointers), else the parent.
  for (size_t K = Path.size(); K-- > 0;) {
    NamedType *V = Path[K];
    NamedType *Par = K + 1 < Path.size() ? Path[K + 1] : Cur;
    uint32_t D = 1;
    NamedType *Jump = V;
    if (Par) {
      uint32_t DP = Par->HierDepth.load(std::memory_order_acquire);
      D = DP + 1;
      NamedType *PJ = Par->HierJump.load(std::memory_order_relaxed);
      NamedType *PJJ = PJ->HierJump.load(std::memory_order_relaxed);
      uint32_t DPJ = PJ->HierDepth.load(std::memory_order_relaxed),
               DPJJ = PJJ->HierDepth.load(std::memory_order_relaxed);
      Jump = DP - DPJ == DPJ - DPJJ ? PJJ : Par;
    }
    V->HierParent.store(Par, std::memory_order_relaxed);
    V->HierJump.store(Jump, std::memory_order_relaxed);
    V->HierDepth.store(D, std::memory_order_release);
  }
  return N->HierDepth.load(std::memory_order_acquire);
}

NamedType *Checker::levelAncestor(NamedType *X, uint32_t D) {
  while (X->HierDepth.load(std::memory_order_acquire) > D) {
    NamedType *J = X->HierJump.load(std::memory_order_relaxed);
    X = J->HierDepth.load(std::memory_order_acquire) >= D
            ? J
            : X->HierParent.load(std::memory_order_relaxed);
  }
  return X;
}

bool Checker::extendsType(NamedType *Child, NamedType *Anc) {
  if (!Child || !Anc || Child == Anc)
    return false;
  uint32_t DC = hierDepth(Child), DA = DC ? hierDepth(Anc) : 0;
  if (DC && DA) {
    if (DA >= DC)
      return false;
    NamedType *A = levelAncestor(Child, DA);
    return A == Anc || identical(A, Anc);
  }
  // Before the types are final (a declaration still being resolved): the
  // plain walk, which stores nothing.
  NamedType *Mark = nullptr;
  unsigned Steps = 0, Limit = 1;
  for (NamedType *P = parentOf(Child); P && P != Mark; P = parentOf(P)) {
    if (P == Anc || identical(P, Anc))
      return true;
    if (++Steps == Limit) {
      Mark = P;
      Steps = 0;
      Limit *= 2;
    }
  }
  return false;
}

Func::Mark Checker::markOf(const Func *F) {
  // The mark as the hierarchy sees it. A mark on a value receiver (an error,
  // TYP-18) replaces nothing and opens no table, and an @Abstract method with
  // a body (an error) is a replaceable method with that body: so a wrong mark
  // is reported once, at its method, and not again at every child. The
  // receiver kind is read from the declaration, so the answer is the same
  // before and after the receiver is resolved.
  const Func *O = F->Origin ? F->Origin : F;
  Func::Mark M = O->MethodMark;
  if (M == Func::Mark::None)
    return M;
  bool ValueRecv = O->Recv && !O->HasPtrRecv;
  if (O->Decl && O->Decl->Recv && !O->Decl->Recv->List.empty()) {
    const ast::Expr *RT = O->Decl->Recv->List[0]->Type;
    while (auto *P = dyn_cast_or_null<ParenExpr>(RT))
      RT = P->X;
    ValueRecv = !isa_and_nonnull<StarExpr>(RT);
  }
  if (ValueRecv)
    return Func::Mark::None;
  if (M == Func::Mark::Abstract && O->Decl && O->Decl->Body)
    return Func::Mark::Virtual;
  return M;
}

static bool ownsTableMethod(NamedType *N) {
  NamedType *O = N->origin();
  if (!O->Methods)
    return false;
  for (Func *M : *O->Methods) {
    Func::Mark Mk = Checker::markOf(M);
    if (Mk == Func::Mark::Virtual || Mk == Func::Mark::Abstract)
      return true;
  }
  return false;
}

bool Checker::hasTypeTable(NamedType *N) {
  if (!N)
    return false;
  // Whether a type has a table does not depend on its type arguments, and an
  // instance of a generic chain would make every query expand every level
  // (the type parameters of each level are different types): ask the origin.
  N = N->origin();
  if (uint8_t St = N->TableState.load(std::memory_order_acquire))
    return St == 2;
  // Up to the first type whose answer is known (or the root), then down:
  // each type is looked at once.
  SmallVector<NamedType *, 8> Chain;
  NamedType *Mark = nullptr;
  unsigned Steps = 0, Limit = 1;
  bool Cur = false;
  for (NamedType *X = N; X && X != Mark;
       X = parentOf(X) ? parentOf(X)->origin() : nullptr) {
    if (uint8_t St = X->TableState.load(std::memory_order_acquire)) {
      Cur = St == 2;
      break;
    }
    Chain.push_back(X);
    if (++Steps == Limit) {
      Mark = X;
      Steps = 0;
      Limit *= 2;
    }
  }
  bool Final = typesFinal();
  for (size_t I = Chain.size(); I-- > 0;) {
    NamedType *C = Chain[I];
    Final = Final && declFinished(C);
    Cur = Cur || ownsTableMethod(C);
    if (Final)
      C->TableState.store(Cur ? 2 : 1, std::memory_order_release);
  }
  return Cur;
}

bool Checker::isTagged(Type *T) {
  if (S.Opts.GoCompat || !T)
    return false;
  if (auto *N = dyn_cast<NamedType>(T))
    return N->Ptr == PtrKind::None && !N->IsOption && !N->IsView &&
           isa_and_nonnull<StructType>(under(N)) && hasTypeTable(N);
  // A type parameter `T extends P` with a tagged P: every type that extends
  // a tagged type is tagged too (GEN-12).
  if (auto *TP = dyn_cast<TypeParamType>(T))
    if (auto *I = dyn_cast_or_null<InterfaceType>(under(TP)))
      if (Type *Ext = typeSet(I)->Extends)
        return !isa<TypeParamType>(Ext) && isTagged(Ext);
  return false;
}

uint8_t Checker::abstractState(NamedType *N) {
  if (!N || S.Opts.GoCompat || N->Ptr != PtrKind::None || N->IsOption ||
      N->IsView)
    return 1;
  N = N->origin();
  if (uint8_t St = N->AbstractState.load(std::memory_order_acquire))
    return St;
  // A package-level type before the walk of the package: run it now.
  ensureHierarchy();
  if (uint8_t St = N->AbstractState.load(std::memory_order_acquire))
    return St;
  // A type without methods of its own (a local type) has the open methods of
  // its parent: up to the first type whose answer is known, then down. A
  // type with methods whose answer is not known yet (a declaration of
  // another package without one) gets a walk of its own.
  SmallVector<NamedType *, 8> Chain;
  NamedType *Mark = nullptr;
  unsigned Steps = 0, Limit = 1;
  uint8_t Base = 1; // the state of the first known ancestor (1: none)
  Func *Witness = nullptr;
  for (NamedType *X = N; X && X != Mark;) {
    if (uint8_t St = X->AbstractState.load(std::memory_order_acquire)) {
      Base = St;
      Witness = X->OpenWitness.load(std::memory_order_relaxed);
      break;
    }
    if (X->Methods && !X->Methods->empty()) {
      NamedType *Seeds[] = {X};
      hierarchyBatch(Seeds, /*Report=*/false);
      if (uint8_t St = X->AbstractState.load(std::memory_order_acquire)) {
        Base = St;
        Witness = X->OpenWitness.load(std::memory_order_relaxed);
      }
      break;
    }
    Chain.push_back(X);
    if (++Steps == Limit) {
      Mark = X;
      Steps = 0;
      Limit *= 2;
    }
    NamedType *P = parentOf(X);
    X = P ? P->origin() : nullptr;
  }
  bool Open = Base == 2;
  bool Final = typesFinal();
  uint8_t St = Open ? 2 : 1;
  for (size_t I = Chain.size(); I-- > 0;) {
    NamedType *C = Chain[I];
    St = Open ? 2 : (C->AbstractAttr ? 3 : 1);
    Final = Final && declFinished(C);
    if (Final) {
      if (Open)
        C->OpenWitness.store(Witness, std::memory_order_relaxed);
      C->AbstractState.store(St, std::memory_order_release);
    }
  }
  return Chain.empty() ? Base : St;
}

bool Checker::isAbstractType(Type *T) {
  auto *N = dyn_cast_or_null<NamedType>(T);
  return N && abstractState(N) >= 2;
}

std::string Checker::abstractWhy(NamedType *N) {
  NamedType *O = N->origin();
  if (abstractState(O) == 2) {
    // A state read from export data has no witness yet: the walk of its
    // chain stores it (the same one in every package).
    if (!O->OpenWitness.load(std::memory_order_relaxed)) {
      NamedType *Seeds[] = {O};
      hierarchyBatch(Seeds, /*Report=*/false);
    }
    if (Func *W = O->OpenWitness.load(std::memory_order_relaxed))
      return "its @Abstract method " + std::string(W->Name) +
             " is not implemented";
  }
  return "it is marked @Abstract";
}

void Checker::noAbstract(const ast::Expr *At, Type *T, const char *What) {
  // A generic body that makes or holds a value of its type parameter (`var z
  // T`, `T{}`, `make([]T, n)`, a by-value local) records a Make use of T,
  // decided on every type argument (R8, useWhy). The signature of a generic
  // is checked on its instance (noAbstractArgs), so only bodies note it.
  if (Bodies && T && isa<TypeParamType>(T))
    noteUse(T, {TypeParamType::Use::Make});
  if (S.Opts.GoCompat || !T || !isa<NamedType>(T))
    return;
  SourceLoc Loc = At ? At->getBegin() : SourceLoc();
  std::string W(What);
  later([this, Loc, T, W]() {
    if (!isAbstractType(T))
      return;
    err(Loc, diag::err_abstract_instance,
        {W, str(T), abstractWhy(cast<NamedType>(T))});
  });
}

/// Does T hold the type A by value, decided on the real, substituted types?
/// Pointers, the pointer kinds, views and interfaces hold no value of their
/// target; a func type holds its parameters and results (varType refuses an
/// abstract one there, R25); a non-generic named type was checked at its
/// declaration.
/// An instance whose arguments do not mention A is not expanded, so a chain
/// of generic types costs nothing here.
static bool holdsByValue(Checker &C, Type *T, Type *A) {
  auto Mentions = [&](ArrayRef<Type *> Args) {
    SmallVector<Type *, 8> W(Args.begin(), Args.end());
    llvm::SmallPtrSet<const Type *, 8> Met;
    while (!W.empty()) {
      Type *X = W.pop_back_val();
      if (!X || !Met.insert(X).second)
        continue;
      if (X == A || C.identical(X, A))
        return true;
      switch (X->getKind()) {
      case Type::Kind::Named:
        for (Type *Arg : cast<NamedType>(X)->TArgs)
          W.push_back(Arg);
        break;
      case Type::Kind::Slice:
        W.push_back(cast<SliceType>(X)->Elem);
        break;
      case Type::Kind::Array:
        W.push_back(cast<sema::ArrayType>(X)->Elem);
        break;
      case Type::Kind::Option:
        W.push_back(cast<OptionType>(X)->Elem);
        break;
      case Type::Kind::Chan:
        W.push_back(cast<sema::ChanType>(X)->Elem);
        break;
      case Type::Kind::Map:
        W.push_back(cast<sema::MapType>(X)->Key);
        W.push_back(cast<sema::MapType>(X)->Elem);
        break;
      default:
        break;
      }
    }
    return false;
  };
  SmallVector<Type *, 8> Work{T};
  llvm::SmallPtrSet<const Type *, 8> Seen;
  while (!Work.empty()) {
    Type *X = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    if (X == A || C.identical(X, A))
      return true;
    switch (X->getKind()) {
    case Type::Kind::Named: {
      auto *N = cast<NamedType>(X);
      if (N->Ptr != PtrKind::None || Checker::onInstCycle(N) || !N->Orig ||
          !Mentions(N->TArgs))
        break;
      if (Type *U = C.under(N))
        Work.push_back(U);
      break;
    }
    case Type::Kind::Struct:
      for (Var *F : cast<StructType>(X)->Fields)
        Work.push_back(F->Ty);
      break;
    case Type::Kind::Tuple:
      for (Type *E : cast<TupleType>(X)->Elems)
        Work.push_back(E);
      break;
    case Type::Kind::Signature:
      Work.push_back(cast<SignatureType>(X)->Params);
      Work.push_back(cast<SignatureType>(X)->Results);
      break;
    case Type::Kind::Slice:
      Work.push_back(cast<SliceType>(X)->Elem);
      break;
    case Type::Kind::Array:
      Work.push_back(cast<sema::ArrayType>(X)->Elem);
      break;
    case Type::Kind::Option:
      Work.push_back(cast<OptionType>(X)->Elem);
      break;
    case Type::Kind::Chan:
      Work.push_back(cast<sema::ChanType>(X)->Elem);
      break;
    case Type::Kind::Map:
      Work.push_back(cast<sema::MapType>(X)->Key);
      Work.push_back(cast<sema::MapType>(X)->Elem);
      break;
    default:
      break;
    }
  }
  return false;
}

void Checker::noAbstractArgs(SourceLoc At, Type *Holder,
                             ArrayRef<Type *> Args) {
  if (S.Opts.GoCompat || !Holder || Args.empty())
    return;
  noteHeldTParams(Holder, Args);
  for (Type *A : Args) {
    if (!isAbstractType(A))
      continue;
    // A signature holds its parameters and results by value; the func value
    // itself holds none.
    bool Held;
    if (auto *Sig = dyn_cast<SignatureType>(Holder))
      Held = holdsByValue(*this, Sig->Params, A) ||
             holdsByValue(*this, Sig->Results, A);
    else
      Held = holdsByValue(*this, Holder, A);
    if (!Held)
      continue;
    err(At, diag::err_abstract_instance,
        {"a type argument of " + str(Holder) + ", which holds it by value",
         str(A), abstractWhy(cast<NamedType>(A))});
    return;
  }
}

//===----------------------------------------------------------------------===//
// Cycles (TYP-22)
//===----------------------------------------------------------------------===//

void Checker::checkExtendsCycles(ArrayRef<NamedType *> Types) {
  if (S.Opts.GoCompat)
    return;
  // The functional graph origin -> origin of its parent, over the given
  // declarations (a parent of another package cannot lead back). Iterative,
  // each type once. A ring is reported once, from its type that comes first
  // in the source, and its types are made invalid, so the by-value cycle
  // sweep that follows does not report the same ring as a recursive type.
  llvm::DenseMap<NamedType *, NamedType *> Next;
  llvm::DenseMap<NamedType *, unsigned> Order;
  for (NamedType *N : Types) {
    N = N->origin();
    Order.try_emplace(N, Order.size());
    auto *St = dyn_cast_or_null<StructType>(N->Underlying);
    if (!St || St->Fields.empty() || !St->Fields[0]->IsParent)
      continue;
    if (auto *P = dyn_cast<NamedType>(St->Fields[0]->Ty))
      Next[N] = P->origin();
  }
  llvm::DenseMap<NamedType *, uint8_t> State; // 1 on the path, 2 done
  for (NamedType *N0 : Types) {
    NamedType *N = N0->origin();
    if (State.count(N))
      continue;
    SmallVector<NamedType *, 8> Path;
    NamedType *Cur = N;
    while (Cur && !State.count(Cur)) {
      State[Cur] = 1;
      Path.push_back(Cur);
      auto It = Next.find(Cur);
      Cur = It == Next.end() ? nullptr : It->second;
    }
    if (Cur && State[Cur] == 1) {
      // The ring is the path from Cur on; it is named from its type that
      // comes first in the source.
      size_t From = 0;
      while (Path[From] != Cur)
        ++From;
      size_t First = From;
      for (size_t K = From; K < Path.size(); ++K)
        if (Order.lookup(Path[K]) < Order.lookup(Path[First]))
          First = K;
      size_t Len = Path.size() - From;
      std::string Ring;
      for (size_t I = 0; I < Len; ++I) {
        NamedType *M = Path[From + (First - From + I) % Len];
        Ring += std::string(M->Obj->Name) + " extends ";
      }
      Ring += std::string(Path[First]->Obj->Name);
      err(Path[First]->Obj->Pos, diag::err_extends_cycle, {Ring});
      for (size_t K = From; K < Path.size(); ++K) {
        Path[K]->Underlying = invalid();
        Path[K]->FromRHS = invalid();
      }
    }
    for (NamedType *P : Path)
      State[P] = 2;
  }
}

//===----------------------------------------------------------------------===//
// The checks of the declared types (TYP-17, TYP-18, TYP-19): one walk
//===----------------------------------------------------------------------===//

/// The key under which a name is compared in a hierarchy: exported names
/// match in every package, unexported ones only in their own (sameId).
static std::string clashKey(StringRef Name, const Package *Pkg) {
  if (isExportedName(Name) || !Pkg)
    return Name.str();
  return Name.str() + '\0' + std::to_string(reinterpret_cast<uintptr_t>(Pkg));
}

void Checker::noteHierarchy(NamedType *N, const ast::StructType *Decl,
                            bool Local) {
  // The positions of the fields an `extends` struct declares (collected by
  // structTypeExpr), for the messages of the walk.
  if (Decl)
    if (auto It = FieldPosOf.find(Decl); It != FieldPosOf.end()) {
      FieldPos[N] = std::move(It->second);
      FieldPosOf.erase(It);
    }
  if (!Local)
    return;
  // The local types of a body are checked when the body is done.
  LocalBatch.push_back(N);
  if (LocalBatch.size() == 1)
    later([this]() {
      SmallVector<NamedType *, 8> Batch = std::move(LocalBatch);
      LocalBatch.clear();
      checkLocalHierarchy(Batch);
    });
}

bool Checker::extendsOrigin(NamedType *Child, NamedType *AncO) {
  // As extendsType, on origins: the ancestor at AncO's depth has AncO as its
  // origin (an instance's parent is an instance).
  uint32_t DC = hierDepth(Child), DA = DC ? hierDepth(AncO) : 0;
  if (!DC || !DA || DA > DC)
    return false;
  NamedType *A = Child;
  while (A->HierDepth.load(std::memory_order_acquire) > DA) {
    NamedType *J = A->HierJump.load(std::memory_order_relaxed);
    A = J->HierDepth.load(std::memory_order_acquire) >= DA
            ? J
            : A->HierParent.load(std::memory_order_relaxed);
  }
  return A->origin() == AncO;
}

void Checker::checkLocalHierarchy(ArrayRef<NamedType *> Batch) {
  // Local types have no methods, so their rules are the names of their
  // fields against the ancestors' (TYP-17) and the @Abstract methods they
  // inherit open (TYP-19, from the parent's stored answer). The names of
  // every ancestor are kept in an index (name -> the types that declare it),
  // filled once per type by a walk that stops at a type already in it, and
  // the nearest declaring ancestor is found with the jump pointers: so a
  // local type in each of many bodies costs its own names, not the depth of
  // its hierarchy each time (the walk of hierarchyBatch would).
  auto Name = [&](NamedType *T) {
    return T->Obj ? std::string(T->Obj->Name) : str(T);
  };
  for (NamedType *L : Batch) {
    L = L->origin();
    NamedType *P = parentOf(L);
    if (!P || !L->Obj)
      continue;
    // The names of L's ancestors not in the package's index (local ones,
    // and the chains only a local type reaches).
    indexNames(P, NameIndex, NameIndexed, &S.HierIndexed);
    std::string Self = Name(L);
    if (auto FP = FieldPos.find(L); FP != FieldPos.end()) {
      for (auto &[FName, Pos] : FP->second) {
        std::string K = clashKey(FName, S.Pkg);
        NamedType *Best = nullptr;
        Object *BestObj = nullptr;
        for (auto *Index : {&S.HierNames, &NameIndex}) {
          auto It = Index->find(K);
          if (It == Index->end())
            continue;
          for (auto &[Owner, Obj] : It->second)
            if (Owner != L && extendsOrigin(L, Owner) &&
                (!Best || hierDepth(Owner) > hierDepth(Best))) {
              Best = Owner;
              BestObj = Obj;
            }
        }
        if (Best)
          err(Pos, diag::err_extends_name_clash,
              {"field", std::string(FName), Self,
               isa<Func>(BestObj) ? "method" : "field", Name(Best)});
      }
      FieldPos.erase(FP);
    }
    // L's names join the index, for its local children.
    indexNames(L, NameIndex, NameIndexed, &S.HierIndexed);
    if (!L->AbstractAttr && abstractState(P) == 2) {
      // The open methods of the parent, for the message (an error only).
      SmallVector<NamedType *, 8> Chain;
      for (NamedType *X = P; X; X = parentOf(X))
        Chain.push_back(X->origin());
      llvm::StringMap<Func *> Open;
      std::set<std::pair<uint32_t, std::string>> Order;
      for (size_t I = Chain.size(); I-- > 0;)
        if (Chain[I]->Methods)
          for (Func *M : *Chain[I]->Methods) {
            std::string K = clashKey(M->Name, M->Pkg);
            if (auto It = Open.find(K); It != Open.end()) {
              Order.erase({It->second->Pos.getRaw(), K});
              Open.erase(It);
            }
            if (markOf(M) == Func::Mark::Abstract) {
              Open[K] = M;
              Order.insert({M->Pos.getRaw(), K});
            }
          }
      std::string Names;
      for (auto &OP : Order)
        Names += (Names.empty() ? "" : ", ") +
                 std::string(Open.lookup(OP.second)->Name);
      err(L->Obj->Pos, diag::err_abstract_unimplemented, {Self, Names, str(P)});
    }
  }
}

void Checker::ensureHierarchy() {
  // Phase 1 only: the walk of the package reports, so a body checker never
  // runs it (every package-level answer is stored by then).
  if (HierarchyDone || !TypesDeclared || Bodies || S.Opts.GoCompat)
    return;
  HierarchyDone = true;
  SmallVector<NamedType *, 32> Seeds;
  for (Object *O : S.PkgObjects)
    if (auto *TN = dyn_cast<TypeName>(O); TN && !TN->IsAlias)
      if (auto *N = dyn_cast_or_null<NamedType>(TN->Ty))
        Seeds.push_back(N);
  hierarchyBatch(Seeds, /*Report=*/true);
  // The names of these types and their ancestors, once, for the local types
  // of the bodies (read-only in phase 2).
  for (NamedType *Seed : Seeds)
    indexNames(Seed, S.HierNames, S.HierIndexed, nullptr);
}

void Checker::indexNames(
    NamedType *From,
    llvm::StringMap<SmallVector<std::pair<NamedType *, Object *>, 2>> &Index,
    llvm::DenseSet<NamedType *> &Indexed,
    const llvm::DenseSet<NamedType *> *Also) {
  // Up the chain to a type already indexed (here or in Also); each type's own
  // fields and methods once.
  for (NamedType *X = From; X;) {
    NamedType *O = X->origin();
    if (Indexed.count(O) || (Also && Also->count(O)))
      break;
    Indexed.insert(O);
    if (auto *St = dyn_cast_or_null<StructType>(O->Underlying))
      for (Var *F : St->Fields)
        if (F->Name != "_")
          Index[clashKey(F->Name, F->Pkg   ? F->Pkg
                                  : O->Obj ? O->Obj->Pkg
                                           : S.Pkg)]
              .push_back({O, F});
    if (O->Methods)
      for (Func *M : *O->Methods)
        if (M->Name != "_")
          Index[clashKey(M->Name, M->Pkg)].push_back({O, M});
    X = parentOf(O);
  }
}

/// The name an ancestor method is looked up by: the name for an ordinary
/// method; for an operator method (an overload set shares its name) the
/// operator and the parameter types, with @Right, so a child's extra overload
/// is its own method and an @Override replaces the method of the same
/// signature (EXP-30).
static std::string methodKey(Checker &C, Func *M) {
  if (!M->isOperator())
    return clashKey(M->Name, M->Pkg);
  C.objDecl(M, nullptr);
  std::string K = "operator ";
  K += M->Name;
  if (M->Right)
    K += " @Right";
  if (auto *Sig = dyn_cast_or_null<SignatureType>(M->Ty))
    for (Type *P : Sig->Params->Elems)
      K += " " + C.str(P);
  return K;
}

void Checker::hierarchyBatch(ArrayRef<NamedType *> Seeds, bool Report) {
  if (S.Opts.GoCompat || Seeds.empty())
    return;
  // The forest: the seeds and their ancestors (origins: names and marks do
  // not depend on type arguments), each once. A type outside the batch only
  // brings its names along.
  struct Node {
    NamedType *N;
    int Parent;
    bool Seed;
  };
  SmallVector<Node, 32> Nodes;
  llvm::DenseMap<NamedType *, int> Idx;
  for (NamedType *Seed : Seeds) {
    SmallVector<NamedType *, 8> Path;
    NamedType *O = Seed->origin();
    NamedType *Mark = nullptr;
    unsigned Steps = 0, Limit = 1;
    while (O && !Idx.count(O) && O != Mark) {
      Path.push_back(O);
      if (++Steps == Limit) {
        Mark = O;
        Steps = 0;
        Limit *= 2;
      }
      NamedType *P = parentOf(O);
      O = P ? P->origin() : nullptr;
    }
    auto PI = O ? Idx.find(O) : Idx.end();
    int ParentIdx = PI != Idx.end() ? PI->second : -1;
    for (size_t K = Path.size(); K-- > 0;) {
      if (Idx.count(Path[K]))
        continue; // a ring came round (it is reported elsewhere)
      Idx[Path[K]] = static_cast<int>(Nodes.size());
      Nodes.push_back({Path[K], ParentIdx, false});
      ParentIdx = static_cast<int>(Nodes.size()) - 1;
    }
  }
  for (NamedType *Seed : Seeds)
    if (auto It = Idx.find(Seed->origin()); It != Idx.end())
      Nodes[It->second].Seed = true;
  // Children as linked lists, in the order the types were met.
  SmallVector<int, 32> FirstChild(Nodes.size(), -1), NextSib(Nodes.size(), -1);
  for (size_t K = Nodes.size(); K-- > 0;)
    if (int P = Nodes[K].Parent; P >= 0) {
      NextSib[K] = FirstChild[P];
      FirstChild[P] = static_cast<int>(K);
    }

  // The names of the types on the current path (fields and methods) and the
  // @Abstract methods open there, each with an undo log. OpenOrder keeps the
  // open methods by position, so the first one is the witness for messages.
  struct VisEntry {
    Object *Obj = nullptr;
    NamedType *Owner = nullptr;
  };
  llvm::StringMap<VisEntry> Visible;
  struct VisUndo {
    std::string Key;
    VisEntry Old;
    bool Had;
  };
  SmallVector<VisUndo, 64> VisLog;
  llvm::StringMap<Func *> Open;
  using OpenPos = std::pair<uint32_t, std::string>;
  std::set<OpenPos> OpenOrder;
  struct OpenUndo {
    std::string Key;
    Func *Old;
    bool Had;
  };
  SmallVector<OpenUndo, 32> OpenLog;
  auto SetVis = [&](const std::string &Key, VisEntry E) {
    auto It = Visible.find(Key);
    if (It != Visible.end()) {
      VisLog.push_back({Key, It->second, true});
      It->second = E;
    } else {
      VisLog.push_back({Key, VisEntry(), false});
      Visible[Key] = E;
    }
  };
  auto SetOpen = [&](const std::string &Key, Func *F) {
    auto It = Open.find(Key);
    if (It != Open.end()) {
      OpenLog.push_back({Key, It->second, true});
      OpenOrder.erase({It->second->Pos.getRaw(), Key});
      if (F) {
        It->second = F;
        OpenOrder.insert({F->Pos.getRaw(), Key});
      } else {
        Open.erase(It);
      }
    } else if (F) {
      OpenLog.push_back({Key, nullptr, false});
      Open[Key] = F;
      OpenOrder.insert({F->Pos.getRaw(), Key});
    }
  };
  auto Name = [&](NamedType *T) {
    return T->Obj ? std::string(T->Obj->Name) : str(T);
  };

  // The ancestor operator method that M replaces (EXP-30): the nearest one
  // of the same operator and @Right whose parameters (the target for
  // @Convert) are identical to M's, both seen in the type parameters of O
  // (the instance signatures, as for a method override). The names of the
  // type parameters of the levels do not matter.
  auto OpAncestor = [&](NamedType *O, Func *M, Func *&Inst) -> VisEntry {
    Inst = nullptr;
    NamedType *PI = parentOf(O);
    if (!PI)
      return VisEntry();
    objDecl(M, nullptr);
    if (!M->Ty)
      return VisEntry();
    Type *MSig = M->Ty;
    if (!O->TParams.empty() && M->RecvTParams.size() == O->TParams.size()) {
      SmallVector<Type *, 4> Args(O->TParams.begin(), O->TParams.end());
      MSig = subst(M->Ty, M->RecvTParams, Args);
    }
    auto *MS = dyn_cast<SignatureType>(MSig);
    if (!MS)
      return VisEntry();
    SmallVector<Func *, 8> Cands;
    opCandidates(PI, M->Op, Cands);
    for (Func *F : Cands) {
      auto *FS = cast<SignatureType>(F->Ty);
      if (M->Op == OpKind::Convert
              ? !identical(MS->Results, FS->Results)
              : (F->Right != M->Right || !identical(MS->Params, FS->Params)))
        continue;
      Func *Decl = F->Origin ? F->Origin : F;
      Type *RT = Decl->Recv ? Decl->Recv->Ty : nullptr;
      if (auto *P = dyn_cast_or_null<PointerType>(RT))
        RT = P->Elem;
      auto *Owner = dyn_cast_or_null<NamedType>(RT);
      if (!Owner)
        continue;
      Inst = F;
      return {Decl, Owner->origin()};
    }
    return VisEntry();
  };

  struct Frame {
    int Node;
    int NextChild;
    size_t VisMark, OpenMark;
  };
  SmallVector<Frame, 32> Stack;
  auto Enter = [&](int NI) {
    NamedType *O = Nodes[NI].N;
    const bool HasParent = parentOf(O) != nullptr;
    const bool Rep = Report && Nodes[NI].Seed && O->Obj && O->Obj->Pkg == S.Pkg;
    Frame Fr{NI, FirstChild[NI], VisLog.size(), OpenLog.size()};
    std::string Self = Name(O);
    // Each declared name against the ancestors (Visible holds only theirs
    // until this type's names are added below).
    if (Rep)
      if (auto FP = FieldPos.find(O); FP != FieldPos.end())
        for (auto &[FName, Pos] : FP->second) {
          auto It = Visible.find(clashKey(FName, S.Pkg));
          if (It == Visible.end())
            continue;
          err(Pos, diag::err_extends_name_clash,
              {"field", std::string(FName), Self,
               isa<Func>(It->second.Obj) ? "method" : "field",
               Name(It->second.Owner)});
        }
    if (Rep && O->Methods)
      for (Func *M : *O->Methods) {
        if (M->Name == "_")
          continue;
        Func::Mark Mk = M->MethodMark;
        // The marks need a pointer receiver (TYP-18).
        if (Mk != Func::Mark::None && !M->HasPtrRecv && M->Decl &&
            M->Decl->Recv)
          for (const Attribute *A : M->Decl->Attrs)
            if (!A->Pkg && A->Name &&
                markOfAttr(name(A->Name)) != Func::Mark::None)
              err(A->AtLoc, diag::err_method_mark_value_recv,
                  {markName(Mk), std::string(M->Name)});
        VisEntry Found;
        Func *OpInst = nullptr;
        if (M->isOperator()) {
          Found = OpAncestor(O, M, OpInst);
        } else if (auto It = Visible.find(methodKey(*this, M));
                   It != Visible.end()) {
          Found = It->second;
        }
        if (!Found.Obj) {
          if (Mk == Func::Mark::Override)
            err(M->Pos, diag::err_override_nothing,
                {std::string(M->Name),
                 HasParent ? Self + " or any of its ancestors"
                           : Self + ", which extends no type,"});
          continue;
        }
        NamedType *AncT = Found.Owner;
        auto *Anc = dyn_cast<Func>(Found.Obj);
        if (!Anc) {
          err(M->Pos, diag::err_extends_name_clash,
              {"method", std::string(M->Name), Self, "field", Name(AncT)});
          continue;
        }
        Func::Mark AM = markOf(Anc);
        if (AM != Func::Mark::Virtual && AM != Func::Mark::Abstract &&
            AM != Func::Mark::Override) {
          err(M->Pos, diag::err_override_final,
              {std::string(M->Name), Name(AncT)});
          continue;
        }
        if (Mk != Func::Mark::Override) {
          err(M->Pos, diag::err_override_missing,
              {std::string(M->Name), Self, Name(AncT)});
          continue;
        }
        // The signature must match exactly: receiver kind, parameters,
        // results, `!`. A generic hierarchy compares in this type's own type
        // parameters: the method of the parent as written in the
        // declaration (`extends A[T]`), with the type arguments of each
        // level substituted by the lookup.
        objDecl(M, nullptr);
        objDecl(Anc, nullptr);
        Func *AncM = OpInst ? OpInst : Anc;
        if (!OpInst && (!O->TParams.empty() || !AncT->TParams.empty()))
          if (NamedType *PI = parentOf(O)) {
            LookupResult LR = lookupImpl(PI, true, M->Pkg, M->Name, false);
            if (auto *AF = dyn_cast_or_null<Func>(LR.Obj))
              AncM = AF;
          }
        if (!M->Ty || !AncM->Ty)
          continue;
        Type *MSig = M->Ty;
        if (!O->TParams.empty() && M->RecvTParams.size() == O->TParams.size()) {
          SmallVector<Type *, 4> Args(O->TParams.begin(), O->TParams.end());
          MSig = subst(M->Ty, M->RecvTParams, Args);
        }
        bool Same = M->HasPtrRecv == AncM->HasPtrRecv &&
                    M->MoveRecv == AncM->MoveRecv && identical(MSig, AncM->Ty);
        if (!Same) {
          std::string Have = funcString(M), Want = funcString(AncM);
          if (M->HasPtrRecv != AncM->HasPtrRecv) {
            Have += M->HasPtrRecv ? " with a pointer receiver"
                                  : " with a value receiver";
            Want += AncM->HasPtrRecv ? " with a pointer receiver"
                                     : " with a value receiver";
          }
          err(M->Pos, diag::err_override_signature,
              {std::string(M->Name), Self, Have, Want});
        }
      }
    // This type's names join the path.
    if (auto *St = dyn_cast_or_null<StructType>(O->Underlying))
      for (Var *F : St->Fields)
        if (F->Name != "_")
          SetVis(clashKey(F->Name, F->Pkg   ? F->Pkg
                                   : O->Obj ? O->Obj->Pkg
                                            : S.Pkg),
                 {F, O});
    if (O->Methods)
      for (Func *M : *O->Methods)
        if (M->Name != "_")
          SetVis(methodKey(*this, M), {M, O});
    // The open @Abstract methods: a method of the same name closes one (an
    // override, or an error reported above); an own @Abstract method opens
    // one. What is still open from the ancestors needs an @Abstract type.
    if (O->Methods)
      for (Func *M : *O->Methods)
        if (M->Name != "_") {
          std::string K;
          if (M->isOperator()) {
            Func *Inst = nullptr;
            if (VisEntry A = OpAncestor(O, M, Inst); A.Obj)
              K = methodKey(*this, cast<Func>(A.Obj));
          } else {
            K = methodKey(*this, M);
          }
          if (!K.empty() && Open.count(K))
            SetOpen(K, nullptr);
        }
    if (Rep && !O->AbstractAttr && !Open.empty()) {
      std::string Names;
      for (const OpenPos &OP : OpenOrder)
        Names += (Names.empty() ? "" : ", ") +
                 std::string(Open.lookup(OP.second)->Name);
      NamedType *P = parentOf(O);
      err(O->Obj->Pos, diag::err_abstract_unimplemented,
          {Self, Names, P ? str(P) : std::string("its parent")});
    }
    if (O->Methods)
      for (Func *M : *O->Methods)
        if (M->Name != "_" && markOf(M) == Func::Mark::Abstract)
          SetOpen(methodKey(*this, M), M);
    // The answers of this type (the same from every walk).
    if (typesFinal() && declFinished(O)) {
      Func *W = Open.empty() ? nullptr : Open.lookup(OpenOrder.begin()->second);
      if (W && !O->OpenWitness.load(std::memory_order_relaxed))
        O->OpenWitness.store(W, std::memory_order_relaxed);
      if (!O->AbstractState.load(std::memory_order_acquire))
        O->AbstractState.store(W ? 2 : (O->AbstractAttr ? 3 : 1),
                               std::memory_order_release);
    }
    Stack.push_back(Fr);
  };

  for (size_t R = 0; R < Nodes.size(); ++R) {
    if (Nodes[R].Parent >= 0)
      continue;
    Enter(static_cast<int>(R));
    while (!Stack.empty()) {
      Frame &Fr = Stack.back();
      if (Fr.NextChild >= 0) {
        int C = Fr.NextChild;
        Fr.NextChild = NextSib[C];
        Enter(C);
        continue;
      }
      // Leave: undo this type's entries.
      while (VisLog.size() > Fr.VisMark) {
        VisUndo &U = VisLog.back();
        if (U.Had)
          Visible[U.Key] = U.Old;
        else
          Visible.erase(U.Key);
        VisLog.pop_back();
      }
      while (OpenLog.size() > Fr.OpenMark) {
        OpenUndo &U = OpenLog.back();
        if (auto It = Open.find(U.Key); It != Open.end()) {
          OpenOrder.erase({It->second->Pos.getRaw(), U.Key});
          Open.erase(It);
        }
        if (U.Had) {
          Open[U.Key] = U.Old;
          OpenOrder.insert({U.Old->Pos.getRaw(), U.Key});
        }
        OpenLog.pop_back();
      }
      Stack.pop_back();
    }
  }
  for (NamedType *Seed : Seeds)
    FieldPos.erase(Seed->origin());
}

//===----------------------------------------------------------------------===//
// Upcasts, downcasts and the parent part (EXP-13 to EXP-15)
//===----------------------------------------------------------------------===//

/// The named struct type behind a pointer-like operand type: `*C` gives C.
static NamedType *namedTarget(Type *T) {
  if (auto *P = dyn_cast_or_null<PointerType>(T))
    return dyn_cast<NamedType>(P->Elem);
  return nullptr;
}

bool Checker::strictlyExtends(Type *Ch, Type *P) {
  if (!Ch || !P || Ch == P)
    return false;
  if (isa<TypeParamType>(Ch))
    return extendsOrIs(Ch, P);
  auto *C = dyn_cast<NamedType>(Ch);
  auto *PN = dyn_cast<NamedType>(P);
  return C && PN && extendsType(C, PN);
}

std::string Checker::untaggedCause(NamedType *P, Type *Child, Type *To) {
  std::string PS = str(P);
  return PS +
         " has no type table (no @Virtual or @Abstract method in its "
         "hierarchy), so a *" +
         PS +
         " points at a child only in the parent's own methods and what they "
         "return, and there is no upcast to " +
         str(To) + " (TYP-22, EXP-13); add a @Virtual method to " + PS +
         " to use children through *" + PS + ", or use *" + str(Child);
}

bool Checker::hierarchyUpcast(Type *V, Type *T, std::string *Cause,
                              bool &Handled) {
  Handled = false;
  if (S.Opts.GoCompat)
    return false;
  // Every upcast needs a type table in the TARGET parent (TYP-22, ADR-0023
  // decision 16): without one a parent pointer points at a child only in
  // the parent's own methods and what they return (the receiver is the
  // parent part, a valid value of the parent), so a clone through it copies
  // a whole value of the parent.
  auto Decide = [&](Type *Child, NamedType *P) {
    Handled = true;
    if (hasTypeTable(P))
      return true;
    if (Cause)
      *Cause = untaggedCause(P, Child, T);
    return false;
  };
  // The three pointer kinds: Unique[*C] to Unique[*P] and so on.
  PtrKind KV, KT;
  Type *EV, *ET;
  if (hasPtrKind(V, &KV, &EV) && hasPtrKind(T, &KT, &ET)) {
    auto *P = dyn_cast<NamedType>(ET);
    if (KV != KT || !P || !strictlyExtends(EV, P))
      return false;
    return Decide(EV, P);
  }
  // View[C] to View[P]: read-only.
  if (auto *RV = dyn_cast<RefType>(V))
    if (auto *RT = dyn_cast<RefType>(T)) {
      auto *P = dyn_cast<NamedType>(RT->Elem);
      if (!RV->Borrowed && !RT->Borrowed && P && strictlyExtends(RV->Elem, P))
        return Decide(RV->Elem, P);
    }
  // *C to *P: the same borrow.
  if (auto *PV = dyn_cast<PointerType>(V))
    if (auto *PT = dyn_cast<PointerType>(T)) {
      auto *P = dyn_cast<NamedType>(PT->Elem);
      if (P && strictlyExtends(PV->Elem, P))
        return Decide(PV->Elem, P);
    }
  return false;
}

NamedType *Checker::downcastParent(Type *From) {
  PtrKind K;
  Type *Elem;
  NamedType *P = nullptr;
  if (hasPtrKind(From, &K, &Elem)) {
    if (K == PtrKind::Weak)
      return nullptr;
    P = dyn_cast<NamedType>(Elem);
  } else {
    P = namedTarget(From);
  }
  return P && isa_and_nonnull<StructType>(under(P)) ? P : nullptr;
}

bool Checker::downcastTarget(NamedType *P, Type *T, const ast::Expr *At,
                             const std::string &What) {
  // A type assertion on a pointer to a struct is a downcast (EXP-15): the
  // target is a pointer to a strict descendant (`T extends P` too, GEN-12),
  // and the parent has a type table (the value has a dynamic type).
  auto *PT = dyn_cast<PointerType>(T);
  Type *CE = PT ? PT->Elem : nullptr;
  if (!CE || !strictlyExtends(CE, P)) {
    err(At, diag::err_downcast_impossible,
        {What, CE ? str(CE) : str(T), str(P)});
    return false;
  }
  if (!hasTypeTable(P)) {
    err(At, diag::err_downcast_no_table, {What, str(T), str(P)});
    return false;
  }
  return true;
}

bool Checker::downcastOk(Operand &X, Type *T, const ast::Expr *Ex) {
  NamedType *P = downcastParent(X.Ty);
  if (!P)
    return false;
  // The result of a downcast borrows from the owning pointer (like the
  // upcast, OWN-P6): a temporary is freed at the end of the statement.
  PtrKind K;
  if (own() && hasPtrKind(X.Ty, &K, nullptr) && K != PtrKind::Weak && X.E &&
      placeKind(X.E) == PlaceKind::Temp) {
    err(Ex, diag::err_sema_expr,
        {"cannot borrow a temporary " + str(X.Ty) + " for the downcast " +
         str(Ex) +
         ": the target would be freed at the end of the statement "
         "(OWN-P6); store it first"});
    X.M = Mode::Invalid;
    return true;
  }
  if (!downcastTarget(P, T, Ex, str(Ex)))
    X.M = Mode::Invalid;
  return true;
}

Checker::PartialMark Checker::takePartialMark(const ast::Expr *E) {
  PartialMark M;
  M.Ok = E && E == PartialOk;
  M.Addr = M.Ok && PartialOkAddr;
  PartialOk = nullptr;
  PartialOkAddr = false;
  return M;
}

void Checker::markPartialOk(const ast::Expr *E, bool Addr) {
  while (auto *P = dyn_cast_or_null<ParenExpr>(E))
    E = P->X;
  PartialOk = E;
  PartialOkAddr = Addr;
}

bool Checker::partialUse(const ast::Expr *E, PartialKind K, Type *PartTy,
                         Type *Whole, PartialMark M) {
  // The places that may hold only the parent part of a bigger value: the
  // parent part `c.Payment`, `*p` and a value receiver of a type with a type
  // table (its dynamic type may be a child). They are used only to reach a
  // field or call a method (the start of a selector), or under `&`; every
  // other use would copy, move or overwrite just that part (EXP-15, ADR-0023
  // decision 16). One rule, decided here for every such place.
  if (M.Ok && !M.Addr)
    return true;
  std::string Text = str(E);
  auto *PN = dyn_cast_or_null<NamedType>(PartTy);
  if (M.Addr) {
    // `&c.Payment` is an upcast: it needs a type table in the parent.
    if (K == PartialKind::ParentPart && PN && !hasTypeTable(PN)) {
      err(E, diag::err_upcast_untagged,
          {"&" + Text, str(PN), Whole ? str(Whole) : std::string("Child")});
      return false;
    }
    return true;
  }
  std::string P = str(PartTy);
  switch (K) {
  case PartialKind::None:
    return true;
  case PartialKind::ParentPart:
    err(E, diag::err_partial_value,
        {Text, "as a value",
         "it is the parent part of a " +
             (Whole ? str(Whole) : std::string("child")) +
             ", and a copy, a move or an assignment of it as a whole would "
             "cut off the child; use its fields and methods" +
             (PN && hasTypeTable(PN) ? ", or the pointer &" + Text
                                     : std::string())});
    break;
  case PartialKind::Deref:
    err(E, diag::err_partial_value,
        {Text, "as a value",
         P + " has a type table, so " + Text +
             " may be the parent part of a child, and a copy of it or an "
             "assignment through it would cover only that part; use the "
             "fields and methods through the pointer, or its Clone() for a "
             "full copy"});
    break;
  case PartialKind::Receiver:
    err(E, diag::err_partial_value,
        {Text, "as a value",
         P + " has a type table, so the value receiver " + Text +
             " may be the parent part of a child, and a copy of it would "
             "cover only that part; use its fields and methods, or a "
             "pointer receiver and its Clone()"});
    break;
  }
  return false;
}

Checker::PartialKind Checker::partialKindOf(const ast::Expr *E, Type *T) {
  while (auto *P = dyn_cast_or_null<ParenExpr>(E))
    E = P->X;
  if (!E || S.Opts.GoCompat)
    return PartialKind::None;
  if (isa<StarExpr>(E))
    return derefKind(T);
  if (auto *V = dyn_cast_or_null<Var>(identObj(E)))
    if (V->IsRecv && isTagged(V->Ty))
      return PartialKind::Receiver;
  if (const Var *F = selectedField(E); F && F->IsParent)
    return PartialKind::ParentPart;
  return PartialKind::None;
}

Type *Checker::cloneThroughTable(Type *T) {
  // `p.Clone()` on a *P or a View[P] of a tagged P (also *T with a tagged
  // `T extends P`) copies the whole dynamic value through the type table
  // into a new Unique[*P] (ADR-0023 decision 16, lowered by P04.22).
  if (S.Opts.GoCompat || !own())
    return nullptr;
  if (auto *P = dyn_cast_or_null<PointerType>(T))
    return isTagged(P->Elem) ? P->Elem : nullptr;
  if (auto *R = dyn_cast_or_null<RefType>(T))
    return !R->Borrowed && isTagged(R->Elem) ? R->Elem : nullptr;
  return nullptr;
}

Type *Checker::uniqueOf(Type *Elem) {
  Object *O = Ctx.universe()->lookupLocal("Unique");
  Type *El = Ctx.pointer(Elem);
  return instantiateNamed(cast<NamedType>(O->Ty), ArrayRef<Type *>(&El, 1));
}

bool Checker::rejectPartialClone(const ast::Expr *Recv, Type *T,
                                 const ast::Expr *At) {
  // `(*p).Clone()`, `a.Clone()` on a value receiver and `c.P.Clone()` of a
  // tagged type would copy only the parent part of a child (ADR-0023
  // decision 16); the parent part of an untagged parent is its own value
  // (like the receiver of an inherited method).
  PartialKind K = partialKindOf(Recv, T);
  if (K == PartialKind::None || (K == PartialKind::ParentPart && !isTagged(T)))
    return false;
  std::string R = str(Recv), P = str(T);
  std::string Why;
  switch (K) {
  case PartialKind::Deref:
    Why = P + " has a type table, so " + R +
          " may be the parent part of a child, and its Clone() would copy "
          "only that part; clone through the pointer for a full copy";
    break;
  case PartialKind::Receiver:
    Why = P + " has a type table, so the value receiver " + R +
          " may be the parent part of a child, and its Clone() would copy "
          "only that part; use a pointer receiver and its Clone()";
    break;
  default:
    Why = R +
          " is the parent part of a child, and its Clone() would copy "
          "only that part; clone the child, or clone through &" +
          R;
    break;
  }
  err(At, diag::err_partial_value,
      {str(At) + (isa<SelectorExpr>(At) ? "()" : ""), "here", Why});
  return true;
}

//===----------------------------------------------------------------------===//
// base (TYP-20)
//===----------------------------------------------------------------------===//

Var *Checker::makeBaseVar(Var *Recv, const void *Body) {
  if (S.Opts.GoCompat || !Recv || !Recv->Ty)
    return nullptr;
  bool IsPtr;
  auto *N = dyn_cast<NamedType>(deref(Recv->Ty, IsPtr));
  NamedType *P = N ? parentOf(N) : nullptr;
  if (!P)
    return nullptr;
  Var *V = newParam("base", Ctx.pointer(P), Recv->Pos);
  V->IsBase = true;
  V->Pkg = S.Pkg;
  V->FuncOwner = Body;
  return V;
}

std::string Checker::baseOwnerName(Var *Recv) {
  if (S.Opts.GoCompat || !Recv || !Recv->Ty)
    return {};
  bool IsPtr;
  auto *N = dyn_cast<NamedType>(deref(Recv->Ty, IsPtr));
  if (!N || !parentOf(N))
    return {};
  return N->origin()->Obj ? std::string(N->origin()->Obj->Name) : str(N);
}

//===----------------------------------------------------------------------===//
// The constraint `T extends P` (GEN-12)
//===----------------------------------------------------------------------===//

Type *Checker::extendsConstraintType(const ast::Expr *PE) {
  Type *PT = varType(PE, /*ValueUse=*/false);
  if (!isValid(PT))
    return nullptr;
  // P is a struct type (a type parameter, an interface or a pointer cannot
  // be extended); known once the declarations are done.
  later([this, PE, PT]() {
    Type *U = under(PT);
    if (!U || !isValid(U) || (isa<StructType>(U) && !isTypeParam(PT)))
      return;
    const char *Why = "a constraint `extends P` needs a struct type P";
    if (isTypeParam(PT)) // its underlying type is its constraint
      Why = "a type parameter cannot be extended";
    else if (isa<InterfaceType>(U))
      Why = "an interface cannot be extended";
    else if (isa<PointerType>(U))
      Why = "a pointer type cannot be extended";
    err(PE, diag::err_extends_parent_kind, {"a constraint", str(PT), Why});
  });
  return PT;
}

Type *Checker::extendsBound(const ast::Expr *PE, SourceLoc ExtendsLoc) {
  Type *PT = extendsConstraintType(PE);
  if (!PT)
    return nullptr;
  InterfaceType *I = Ctx.newInterface({}, {}, {}, /*Implicit=*/true);
  I->Extends = PT;
  I->ExtendsLoc = ExtendsLoc;
  SourceLoc Pos = PE->getBegin();
  later([this, I, Pos]() { computeTypeSet(I, Pos); });
  return I;
}

/// Does V (a defined struct type, or a type parameter with an `extends`
/// constraint) belong to the type set of `extends P`: V is P, or extends it,
/// directly or indirectly?
bool Checker::extendsOrIs(Type *V, Type *P) {
  // A type parameter bounded by `extends Q`: Q is P or extends it. Q is no
  // type parameter (extendsConstraintType), so this ends.
  if (auto *TP = dyn_cast_or_null<TypeParamType>(V)) {
    auto *I = dyn_cast_or_null<InterfaceType>(under(TP));
    TypeSet *TS = I ? typeSet(I) : nullptr;
    if (!TS || !TS->Extends || isa<TypeParamType>(TS->Extends))
      return false;
    V = TS->Extends;
  }
  auto *VN = dyn_cast_or_null<NamedType>(V);
  auto *PN = dyn_cast_or_null<NamedType>(P);
  if (!VN || !PN || VN->Ptr != PtrKind::None)
    return false;
  return VN == PN || identical(VN, PN) || extendsType(VN, PN);
}

//===----------------------------------------------------------------------===//
// The field table (TYP-10)
//===----------------------------------------------------------------------===//

void sema::fieldTable(Type *T, llvm::SmallVectorImpl<const Var *> &Out) {
  // Up to the root first (a cycle is an error elsewhere; stop on one).
  SmallVector<StructType *, 8> Chain;
  llvm::SmallPtrSet<const Type *, 8> Seen;
  for (Type *Cur = T; Cur && Seen.insert(Cur).second;) {
    auto *St = dyn_cast_or_null<StructType>(under(Cur));
    if (!St)
      break;
    Chain.push_back(St);
    Cur = !St->Fields.empty() && St->Fields[0]->IsParent ? St->Fields[0]->Ty
                                                         : nullptr;
  }
  for (auto It = Chain.rbegin(); It != Chain.rend(); ++It)
    for (const Var *F : (*It)->Fields)
      if (!F->IsParent)
        Out.push_back(F);
}

//===----------------------------------------------------------------------===//
// The uses of a type parameter, decided on the real type (GEN-12, EXP-15)
//===----------------------------------------------------------------------===//

using Use = TypeParamType::Use;

void Checker::noteUse(Type *T, Use U) {
  if (S.Opts.GoCompat || !own())
    return;
  auto *TP = dyn_cast_or_null<TypeParamType>(T);
  if (!TP)
    return;
  if (TP->RenamedFrom)
    TP = TP->RenamedFrom;
  // The uses of a parameter of another package came with its export data.
  if (TP->Obj && TP->Obj->Pkg && TP->Obj->Pkg != S.Pkg)
    return;
  U.Passed = NotingPassed;
  UseSink->push_back({TP, U});
}

/// The type parameters that a variable of type T may hold by value, each
/// once: Direct are the parts that varType sees as values (an element, a
/// field, a parameter, a result); Inner are those only inside an instance
/// (a type argument of a generic type or of Option), which holdsByValue
/// decides on the instance (TYP-19, noAbstractArgs).
static void heldTParams(Type *T, llvm::SmallVectorImpl<TypeParamType *> &Direct,
                        llvm::SmallVectorImpl<TypeParamType *> &Inner) {
  SmallVector<std::pair<Type *, bool>, 8> Work{{T, false}};
  llvm::SmallPtrSet<const Type *, 8> Seen;
  while (!Work.empty()) {
    auto [X, InInst] = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    auto Push = [&](Type *Y, bool Inst) {
      Work.push_back({Y, InInst || Inst});
    };
    switch (X->getKind()) {
    case Type::Kind::TypeParam:
      (InInst ? Inner : Direct).push_back(cast<TypeParamType>(X));
      break;
    case Type::Kind::Named:
      if (cast<NamedType>(X)->Ptr == PtrKind::None)
        for (Type *A : cast<NamedType>(X)->TArgs)
          Push(A, true);
      break;
    case Type::Kind::Option:
      Push(cast<OptionType>(X)->Elem, true);
      break;
    case Type::Kind::Struct:
      for (Var *F : cast<StructType>(X)->Fields)
        Push(F->Ty, false);
      break;
    case Type::Kind::Tuple:
      for (Type *E : cast<TupleType>(X)->Elems)
        Push(E, false);
      break;
    case Type::Kind::Signature:
      Push(cast<SignatureType>(X)->Params, false);
      Push(cast<SignatureType>(X)->Results, false);
      break;
    case Type::Kind::Slice:
      Push(cast<SliceType>(X)->Elem, false);
      break;
    case Type::Kind::Array:
      Push(cast<sema::ArrayType>(X)->Elem, false);
      break;
    case Type::Kind::Chan:
      Push(cast<sema::ChanType>(X)->Elem, false);
      break;
    case Type::Kind::Map:
      Push(cast<sema::MapType>(X)->Key, false);
      Push(cast<sema::MapType>(X)->Elem, false);
      break;
    default:
      break;
    }
  }
}

/// The parts of X that a value of it holds by value (holdsByValue's walk),
/// one step: an instance gives the arguments that its declaration holds, if
/// Held knows the declaration's HeldParams.
static void heldParts(Type *X, llvm::SmallVectorImpl<Type *> &Out,
                      llvm::function_ref<bool(NamedType *, uint64_t &)> Held) {
  auto BitOf = [](unsigned I) { return 1ull << (I < 63 ? I : 63); };
  switch (X->getKind()) {
  case Type::Kind::Named: {
    auto *N = cast<NamedType>(X);
    uint64_t M;
    if (N->Ptr != PtrKind::None || !N->Orig || Checker::onInstCycle(N) ||
        !Held(N->origin(), M))
      break;
    for (unsigned J = 0; J < N->TArgs.size(); ++J)
      if (M & BitOf(J))
        Out.push_back(N->TArgs[J]);
    break;
  }
  case Type::Kind::Struct:
    for (Var *F : cast<StructType>(X)->Fields)
      Out.push_back(F->Ty);
    break;
  case Type::Kind::Tuple:
    for (Type *E : cast<TupleType>(X)->Elems)
      Out.push_back(E);
    break;
  case Type::Kind::Signature:
    Out.push_back(cast<SignatureType>(X)->Params);
    Out.push_back(cast<SignatureType>(X)->Results);
    break;
  case Type::Kind::Slice:
    Out.push_back(cast<SliceType>(X)->Elem);
    break;
  case Type::Kind::Array:
    Out.push_back(cast<sema::ArrayType>(X)->Elem);
    break;
  case Type::Kind::Option:
    Out.push_back(cast<OptionType>(X)->Elem);
    break;
  case Type::Kind::Chan:
    Out.push_back(cast<sema::ChanType>(X)->Elem);
    break;
  case Type::Kind::Map:
    Out.push_back(cast<sema::MapType>(X)->Key);
    Out.push_back(cast<sema::MapType>(X)->Elem);
    break;
  default:
    break;
  }
}

uint64_t Checker::heldParams(NamedType *O) {
  if (O->HeldKnown.load(std::memory_order_acquire))
    return O->HeldParams.load(std::memory_order_relaxed);
  // Each declaration is walked once its declarations held by value are
  // known (an explicit stack: a chain of any depth). A cycle by value is an
  // invalid type (reported elsewhere): its parameters count as not held.
  llvm::DenseMap<NamedType *, uint64_t> Done;
  llvm::SmallPtrSet<NamedType *, 8> Open;
  SmallVector<NamedType *, 8> Stack{O};
  const bool Store = typesFinal();
  auto Known = [&](NamedType *D, uint64_t &M) {
    if (D->HeldKnown.load(std::memory_order_acquire)) {
      M = D->HeldParams.load(std::memory_order_relaxed);
      return true;
    }
    auto It = Done.find(D);
    if (It == Done.end())
      return false;
    M = It->second;
    return true;
  };
  while (!Stack.empty()) {
    NamedType *D = Stack.back();
    uint64_t M = 0;
    if (Known(D, M)) {
      Stack.pop_back();
      continue;
    }
    Open.insert(D);
    // Walk D's underlying type with its parameters open; an instance whose
    // declaration is not known yet is walked first.
    SmallVector<NamedType *, 4> Need;
    SmallVector<Type *, 8> Work{under(D)};
    llvm::SmallPtrSet<const Type *, 8> Seen;
    while (!Work.empty()) {
      Type *X = Work.pop_back_val();
      if (!X || !Seen.insert(X).second)
        continue;
      if (auto *P = dyn_cast<TypeParamType>(X)) {
        if (P->Index < D->TParams.size() && D->TParams[P->Index] == P)
          M |= 1ull << (P->Index < 63 ? P->Index : 63);
        continue;
      }
      heldParts(X, Work, [&](NamedType *G, uint64_t &GM) {
        if (Known(G, GM))
          return true;
        if (!Open.count(G))
          Need.push_back(G);
        return false;
      });
    }
    if (!Need.empty()) {
      Stack.append(Need.begin(), Need.end());
      continue;
    }
    Done[D] = M;
    Open.erase(D);
    if (Store && declFinished(D)) {
      D->HeldParams.store(M, std::memory_order_relaxed);
      D->HeldKnown.store(true, std::memory_order_release);
    }
    Stack.pop_back();
  }
  uint64_t M = 0;
  Known(O, M);
  return M;
}

bool Checker::holdsTParam(Type *T, TypeParamType *TP) {
  SmallVector<Type *, 8> Work{T};
  llvm::SmallPtrSet<const Type *, 8> Seen;
  while (!Work.empty()) {
    Type *X = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    if (X == TP)
      return true;
    heldParts(X, Work, [&](NamedType *G, uint64_t &M) {
      M = heldParams(G);
      return true;
    });
  }
  return false;
}

void Checker::noteHeldTParams(Type *Holder, ArrayRef<Type *> Args) {
  // The plain code refuses an instance that holds an abstract type by value
  // (M3: `Option[Shape]`, `Box[Shape]`); in generic code the instance
  // `Box[T]` holds T, so T has a Make use, decided on every type argument.
  if (S.Opts.GoCompat || !own() || !Bodies)
    return;
  SmallVector<TypeParamType *, 4> TPs;
  for (Type *A : Args)
    heldTParams(A, TPs, TPs);
  for (TypeParamType *TP : TPs) {
    bool Held;
    if (auto *Sig = dyn_cast<SignatureType>(Holder))
      Held = holdsTParam(Sig->Params, TP) || holdsTParam(Sig->Results, TP);
    else
      Held = holdsTParam(Holder, TP);
    if (Held)
      noteUse(TP, {Use::Make});
  }
}

bool Checker::mayBreakUses(Type *TArg) {
  if (!TArg)
    return false;
  // A struct type (tagged, abstract, or a child with inherited methods), a
  // pointer to one, or a type parameter the uses travel to.
  Type *X = pointeeOf(TArg, true);
  if (!X)
    X = TArg;
  if (isa<TypeParamType>(X) ||
      (isa<NamedType>(X) && isa_and_nonnull<StructType>(under(X))))
    return true;
  SmallVector<TypeParamType *, 4> TPs;
  heldTParams(TArg, TPs, TPs);
  return !TPs.empty();
}

Checker::PartialKind Checker::derefKind(Type *T) {
  return isTagged(T) ? PartialKind::Deref : PartialKind::None;
}

std::string Checker::useWhy(Type *TArg, TypeParamType *TPar, const Use &U) {
  auto Head = [&] {
    return str(TArg) + " does not satisfy the implicit requirement of " +
           str(TPar) + ": the generic code ";
  };
  std::string T = str(TPar), A = str(TArg);
  switch (U.K) {
  case Use::Copy:
  case Use::Clone: {
    // The plain code's `*p` with p a pointer to TArg (partialKindOf,
    // partialUse, rejectPartialClone).
    if (derefKind(TArg) == PartialKind::None) {
      noteUse(TArg, U);
      return {};
    }
    std::string What =
        U.Passed ? "passes " + T +
                       " on to a generic that copies, moves, assigns or "
                       "clones it through a pointer"
        : U.K == Use::Copy
            ? "copies, moves or assigns a value of " + T +
                  " through a pointer, a view or a receiver (`*p`)"
            : "clones a value of " + T +
                  " through a pointer or a view (`(*p).Clone()`)";
    return Head() + What + ", and a pointer to " + A +
           " may point to a child, so only its " + A +
           " part would be handled; use a type without a type table "
           "(GEN-12, EXP-15)";
  }
  case Use::Make: {
    // The plain code's `var z TArg` (noAbstract, noAbstractArgs).
    if (!isAbstractType(TArg)) {
      SmallVector<TypeParamType *, 4> Direct, Inner;
      heldTParams(TArg, Direct, Inner);
      for (TypeParamType *TP : Direct)
        noteUse(TP, U);
      for (TypeParamType *TP : Inner)
        if (holdsTParam(TArg, TP))
          noteUse(TP, U);
      return {};
    }
    std::string What =
        U.Passed ? "passes " + T +
                       " on to a generic that makes or holds a value of it"
                 : "makes or holds a value of " + T +
                       " (`var`, a literal, `make`, `Option[" + T + "]`)";
    return Head() + What + ", and " + A +
           " is abstract, so no value of it can exist; use a concrete child "
           "(GEN-12, TYP-19)";
  }
  case Use::Method: {
    // The plain code's `x.M`, `(*p).M` or `p.M` on TArg: the same lookup
    // (an owning pointer is looked through, as Checker::selector does) and
    // the same decision (receiverWhy).
    Type *Rcv = U.P == Use::Pointer ? Ctx.pointer(TArg) : TArg;
    Type *LookupT = Rcv;
    PtrKind PK;
    Type *PE;
    if (hasPtrKind(Rcv, &PK, &PE) && PK != PtrKind::Weak)
      LookupT = Ctx.pointer(PE);
    Package *P = TPar->Obj && TPar->Obj->Pkg ? TPar->Obj->Pkg : S.Pkg;
    LookupResult R = lookupFieldOrMethod(LookupT, true, P, U.Member, false);
    auto *F = dyn_cast_or_null<Func>(R.Obj);
    if (!F)
      return {}; // not a method of TArg: the constraint says why
    if (!F->Ty)
      objDecl(F, nullptr);
    const char *Text = U.P == Use::Deref   ? "(*p)"
                       : U.P == Use::Value ? "x"
                                           : "p";
    std::string Core = receiverWhy(
        Rcv, U.P == Use::Deref ? derefKind(TArg) : PartialKind::None,
        U.P == Use::Deref, F, ownerStep(Rcv, R), U.Taken,
        [&] { return std::string(Text); });
    if (Core.empty())
      return {};
    std::string M(U.Member), X = std::string(Text) + "." + M;
    std::string What =
        F->MoveRecv ? "calls the method " + M + " of " + T + " (`" + X + "()`)"
                    : "takes the method " + M + " of " + T + " as a value (`" +
                          X + "`, `go " + X + "()`, `defer " + X + "()`)";
    if (U.Passed)
      What = "passes " + T + " on to a generic that " +
             (F->MoveRecv ? "calls the method " + M + " of it"
                          : "takes the method " + M + " of it as a value");
    return Head() + What + ", and with " + T + " = " + A + ", " + Core +
           (F->MoveRecv
                ? ", and its `move` receiver would move out just that part"
                : ", and binding or deferring its value receiver copies it, "
                  "so the copy would cover only that part") +
           "; give " + M +
           " a pointer receiver or use a type without a type table, or "
           "call the method directly (GEN-12, EXP-15)";
  }
  }
  return {};
}

void Checker::noteIntrinsicUses() {
  if (!S.Pkg || S.Pkg->Path != "mem" || !S.Pkg->PkgScope)
    return;
  auto *F = dyn_cast_or_null<Func>(S.Pkg->PkgScope->lookupLocal("Replace"));
  auto *Sig = F ? dyn_cast_or_null<SignatureType>(F->Ty) : nullptr;
  if (Sig && Sig->TParams.size() == 1)
    noteUse(Sig->TParams[0], {Use::Copy});
}

void Checker::untaggedCheck(ArrayRef<UntaggedPending> Pend) {
  if (Pend.empty() && TPUses.empty())
    return;
  // The vertices are type parameters (a method's stands for its type's, as
  // in monoCheck). Deciding a use on a type argument that is, or points to,
  // a type parameter notes a use there (NotingPassed): the new uses are
  // decided again on that parameter's own instantiations, up to a fixpoint.
  // Every use is decided once per instantiation, so the cost is linear in
  // the instantiations times the uses of their parameters.
  llvm::DenseMap<const TypeParamType *, TypeParamType *> Canon;
  for (auto &[Method, Type] : MonoCanon)
    Canon[Method] = Type;
  auto CanonOf = [&](TypeParamType *P) {
    if (P->RenamedFrom)
      P = P->RenamedFrom;
    auto C = Canon.find(P);
    return C != Canon.end() ? C->second : P;
  };
  struct Node {
    TypeParamType *TP;
    std::set<Use> Set;
    std::vector<Use> List;      // Set in the order the uses came
    std::vector<unsigned> Pend; // the instantiations of TP
    bool Queued = false;
  };
  llvm::DenseMap<const TypeParamType *, unsigned> Index;
  std::vector<Node> Nodes;
  auto NodeOf = [&](TypeParamType *P) {
    P = CanonOf(P);
    auto [It, New] = Index.try_emplace(P, Nodes.size());
    if (New)
      Nodes.push_back(Node{P, {}, {}, {}});
    return It->second;
  };
  std::vector<unsigned> Work;
  auto Add = [&](TypeParamType *P, const Use &U) {
    unsigned V = NodeOf(P);
    if (!Nodes[V].Set.insert(U).second)
      return;
    Nodes[V].List.push_back(U);
    if (!Nodes[V].Queued) {
      Nodes[V].Queued = true;
      Work.push_back(V);
    }
  };
  for (const UseRec &R : TPUses)
    Add(R.TP, R.U);
  for (unsigned I = 0; I < Pend.size(); ++I)
    Nodes[NodeOf(Pend[I].TPar)].Pend.push_back(I);
  std::vector<size_t> Done(Pend.size(), 0);
  std::vector<UseRec> Passed;
  std::vector<UseRec> *SavedSink = UseSink;
  UseSink = &Passed;
  NotingPassed = true;
  while (!Work.empty()) {
    unsigned V = Work.back();
    Work.pop_back();
    Nodes[V].Queued = false;
    for (size_t K = 0; K < Nodes[V].Pend.size(); ++K) {
      unsigned I = Nodes[V].Pend[K];
      for (; Done[I] < Nodes[V].List.size(); ++Done[I]) {
        Use U = Nodes[V].List[Done[I]];
        (void)useWhy(Pend[I].TArg, Pend[I].TPar, U);
      }
      for (const UseRec &R : Passed)
        Add(R.TP, R.U);
      Passed.clear();
    }
  }
  NotingPassed = false;
  // The sets are final: published for this package's parameters (the
  // export data carries them), then each instantiation is decided on its
  // parameter's sorted uses, so the error is the same on every run.
  for (Node &N : Nodes)
    if (!N.TP->Obj || !N.TP->Obj->Pkg || N.TP->Obj->Pkg == S.Pkg) {
      std::vector<Use> Sorted(N.Set.begin(), N.Set.end());
      Ctx.setTParamUses(N.TP, Sorted);
    }
  for (const UntaggedPending &P : Pend) {
    for (const Use &U : Nodes[NodeOf(P.TPar)].Set) {
      std::string Why = useWhy(P.TArg, P.TPar, U);
      if (Why.empty())
        continue;
      err(P.Pos, P.ForType ? diag::err_sema_typexpr : diag::err_sema_call,
          {Why});
      break;
    }
  }
  Passed.clear();
  UseSink = SavedSink;
}

static const ast::Expr *unparenPartial(const ast::Expr *E) {
  while (auto *P = dyn_cast_or_null<ParenExpr>(E))
    E = P->X;
  return E;
}

Type *Checker::pointeeOf(Type *T, bool IncludeWeak) {
  if (!T)
    return nullptr;
  PtrKind K;
  Type *E;
  if (hasPtrKind(T, &K, &E))
    return K == PtrKind::Weak && !IncludeWeak ? nullptr : E;
  if (auto *P = dyn_cast<PointerType>(T))
    return P->Elem;
  if (auto *R = dyn_cast<RefType>(T); R && !R->Borrowed)
    return R->Elem;
  return nullptr;
}

void Checker::noteViewClone(Type *RcvTy) {
  if (auto *R = dyn_cast_or_null<RefType>(RcvTy); R && !R->Borrowed)
    noteUse(R->Elem, {Use::Clone});
}

Checker::OwnerStep Checker::ownerStep(Type *RcvTy, const LookupResult &R) {
  if (R.ParentSteps > 0)
    return {OwnerKind::Parent}; // a chain lookup: every step is a parent part
  if (R.Index.size() < 2)
    return {};
  Type *T = pointeeOf(RcvTy);
  if (!T) {
    bool IsPtr;
    T = deref(RcvTy, IsPtr);
  }
  for (size_t I = 0; I + 1 < R.Index.size(); ++I) {
    auto *St = dyn_cast_or_null<StructType>(under(T));
    if (!St || static_cast<size_t>(R.Index[I]) >= St->Fields.size())
      return {};
    Var *F = St->Fields[R.Index[I]];
    if (I + 2 == R.Index.size())
      return F->IsParent ? OwnerStep{OwnerKind::Parent}
                         : OwnerStep{OwnerKind::Field, F};
    bool IsPtr;
    T = deref(F->Ty, IsPtr);
  }
  return {};
}

std::string Checker::receiverWhy(Type *RcvTy, PartialKind K, bool Star, Func *F,
                                 OwnerStep Step, bool Taken,
                                 llvm::function_ref<std::string()> Text) {
  // Only a method value, a `go` / `defer` call of a value receiver and a
  // `move` receiver take the receiver itself; a call borrows it (PAR-1).
  if (F->IsBuiltinMethod || !(F->MoveRecv || (!F->HasPtrRecv && Taken)))
    return {};
  // Promoted through a Go embedded field: the receiver is reached through
  // that field, whatever place holds it (R27), and decided as the written-out
  // selector `w.Field.M` is: a value field is a whole value, a pointer, an
  // owning pointer or a view of a tagged type may see a child (R28).
  if (Step.K == OwnerKind::Field && Step.Via)
    return receiverWhy(
        Step.Via->Ty, PartialKind::None, false, F, OwnerStep{}, Taken,
        [&] { return Text() + "." + std::string(Step.Via->Name); });
  Type *Pt = pointeeOf(RcvTy);
  if (auto *TP = dyn_cast<TypeParamType>(Pt ? Pt : RcvTy)) {
    // A type parameter: what is partial for each of its types (a tagged
    // `T extends P`) is decided here, the rest on each type argument.
    if (Pt ? !isTagged(Pt) : K == PartialKind::None) {
      noteUse(TP, {Use::Method,
                   Pt     ? Use::Pointer
                   : Star ? Use::Deref
                          : Use::Value,
                   Taken, F->Name});
      return {};
    }
  } else if (Step.K == OwnerKind::Parent) {
    // A method found on a parent: the receiver is the parent part.
    bool IsPtr;
    Type *Whole = Pt ? Pt : deref(RcvTy, IsPtr);
    return "the method " + std::string(F->Name) +
           " is inherited, so its receiver is the parent part of " + str(Whole);
  }
  // Through a pointer, an owning pointer or a view. `base` points at the
  // parent part of the receiver (TYP-20), whatever its table.
  if (Pt && K == PartialKind::ParentPart)
    return Text() + " is the parent part of the receiver";
  if (Pt)
    return isTagged(Pt) ? str(Pt) + " has a type table, so the receiver *" +
                              Text() + " may be the parent part of a child"
                        : std::string();
  // A place of the value itself: `*p`, a value receiver, a parent part.
  switch (K) {
  case PartialKind::Deref:
    return str(RcvTy) + " has a type table, so " + Text() +
           " may be the parent part of a child";
  case PartialKind::Receiver:
    return str(RcvTy) + " has a type table, so the value receiver " + Text() +
           " may be the parent part of a child";
  case PartialKind::ParentPart:
    return Text() + " is the parent part of a child";
  case PartialKind::None:
    break;
  }
  return {};
}

bool Checker::receiverTakenOk(const SelectorExpr *Ex, Func *F, OwnerStep Step,
                              Type *RcvTy, bool Taken) {
  if (S.Opts.GoCompat || !own() || !RcvTy || !isValid(RcvTy))
    return true;
  const ast::Expr *R = Ex->X;
  PartialKind K = partialKindOf(R, RcvTy);
  if (auto *V = dyn_cast_or_null<Var>(identObj(R)); V && V->IsBase)
    K = PartialKind::ParentPart; // `base`: the parent part (TYP-20)
  std::string Core = receiverWhy(RcvTy, K, isa<StarExpr>(unparenPartial(R)), F,
                                 Step, Taken, [&] { return str(R); });
  if (Core.empty())
    return true;
  const bool Moves = F->MoveRecv;
  err(Ex, diag::err_partial_value,
      {str(Ex), Moves ? "here" : "as a method value or deferred call",
       Core +
           (Moves ? ", and a `move` receiver would move out just that part"
                  : ", and binding or deferring a value receiver copies "
                    "it, so the copy would cover only that part") +
           "; call the method directly (a call borrows the receiver) or give "
           "it a pointer receiver"});
  return false;
}
