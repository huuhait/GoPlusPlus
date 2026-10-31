//===- Instances.cpp - The instances of generic code (P04.08, D4.3) -------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// See Instances.h for the design. Every walk here is iterative (the trees and
// the types nest as deep as the source does) and visits each node of each
// body once per instance, each type once in all.
//
//===----------------------------------------------------------------------===//

#include "Instances.h"

#include "gopp/AST/ASTDumper.h"
#include "gopp/GIR/Lower.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

InstanceWalk::InstanceWalk(const S::CheckedPackage &CP,
                           llvm::ArrayRef<const ast::File *> Files,
                           S::MethodSets &MS, Symbols &Syms,
                           DiagnosticsEngine &Diags, bool Quiet,
                           size_t MaxInstances, uint64_t MaxWalked)
    : CP(CP), I(CP.I), Files(Files), MS(MS), Syms(Syms), Diags(Diags),
      Quiet(Quiet), MaxInstances(MaxInstances), MaxWalked(MaxWalked) {}

//===----------------------------------------------------------------------===//
// Reports
//===----------------------------------------------------------------------===//

std::string InstanceWalk::typeText(S::Type *T) {
  if (!T)
    return "?";
  if (MS.extent(T).Size > MaxArgSize) {
    if (auto *N = dyn_cast<S::NamedType>(T))
      return N->Obj->Name.str() + "[...]";
    return "(a type of more than " + std::to_string(MaxArgSize) + " parts)";
  }
  return MS.typeText(T);
}

std::string InstanceWalk::nameOf(const GenericInst &In) {
  std::string Name;
  if (In.Method && In.Method->RecvInst) {
    Name = typeText(In.Method->RecvInst) + "." + In.Decl->Name.str();
  } else {
    Name = In.Decl->Name.str() + "[";
    for (size_t K = 0; K < In.TArgs.size(); ++K)
      Name += (K ? ", " : "") + typeText(In.TArgs[K]);
    Name += "]";
  }
  // A message names an instance in a line: a long one is cut (at a character
  // boundary) and ends in "...".
  constexpr size_t MaxName = 160;
  if (Name.size() > MaxName) {
    size_t Cut = MaxName;
    while (Cut > 0 && (static_cast<unsigned char>(Name[Cut]) & 0xC0) == 0x80)
      --Cut;
    Name.resize(Cut);
    Name += "...";
  }
  return Name;
}

void InstanceWalk::chainNotes(DiagnosticBuilder &B, const GenericInst *From) {
  // The instantiations that led here, the nearest first: a few, a long
  // chain says the same thing.
  constexpr unsigned Shown = 4;
  unsigned K = 0;
  for (; From && K < Shown; From = From->From, ++K)
    B.note(From->Site, "instantiated from " + nameOf(*From) + " here");
  if (From)
    B.note(From->Site, "... and " + std::to_string(From->Depth + 1) +
                           " more instantiations");
}

void InstanceWalk::notes(DiagnosticBuilder &B, const GenericInst &Cur,
                         SourceLoc At, const char *What) {
  if (At.isValid())
    B.note(At, std::string(What) + ", in the generic body of " + nameOf(Cur));
  else if (const ast::FuncDecl *FD = Cur.Decl->Decl)
    B.note(FD->getBegin(), "the generic body of " + nameOf(Cur) + " is here");
  chainNotes(B, Cur.From);
}

void InstanceWalk::tooLarge(GenericInst *Cur, SourceLoc Site,
                            const std::string &Name, const std::string &Why) {
  // One error: past a limit the walk makes no more instances (an input that
  // would expand without end stops here, and says so once).
  if (Stopped)
    return;
  Stopped = true;
  Failed = true;
  auto B = Diags.report(Site, diag::err_inst_too_large);
  B << Name << Why;
  chainNotes(B, Cur);
}

void InstanceWalk::unsupported(SourceLoc At, const std::string &What) {
  if (Quiet)
    return;
  // Said once per place: every instance of one body meets the same site.
  if (!Said.insert({At.getRaw(), What}).second)
    return;
  Failed = true;
  Diags.report(At, diag::err_gir_unsupported) << What;
}

void InstanceWalk::invalid(GenericInst *Cur, SourceLoc At, const Problem &P) {
  if (!Cur || Cur->Bad)
    return;
  Cur->Bad = true;
  Failed = true;
  // The instance is named at the first place that asked for it; the generic
  // code is where the invalid form is written.
  auto B =
      Diags.report(Cur->Site, P.K == Problem::Option ? diag::err_inst_option
                                                     : diag::err_inst_view);
  B << nameOf(*Cur) << typeText(P.Formed) << std::string(P.Why);
  notes(B, *Cur, At, "formed here");
}

bool InstanceWalk::isOurs(const S::Func *F) const {
  return !F->Pkg || F->Pkg == CP.Pkg;
}

//===----------------------------------------------------------------------===//
// Requests
//===----------------------------------------------------------------------===//

GenericInst *InstanceWalk::make(GenericInst *Cur, const S::Func *Decl,
                                const S::Func *M,
                                llvm::ArrayRef<S::Type *> TArgs,
                                std::string Symbol, SourceLoc Site) {
  auto In = std::make_unique<GenericInst>();
  In->Decl = Decl;
  In->Method = M;
  In->TArgs.assign(TArgs.begin(), TArgs.end());
  In->Site = Site;
  In->From = Cur;
  In->Depth = Cur ? Cur->Depth + 1 : 0;
  In->Symbol = std::move(Symbol);
  // Decl's type parameters: the receiver's, then its own.
  size_t K = 0;
  for (S::TypeParamType *P : Decl->RecvTParams)
    if (K < TArgs.size())
      In->Map[P] = TArgs[K++];
  if (!Decl->OwnTParams.empty()) {
    for (S::TypeParamType *P : Decl->OwnTParams)
      if (K < TArgs.size())
        In->Map[P] = TArgs[K++];
  } else if (auto *Sig = dyn_cast_or_null<S::SignatureType>(Decl->Ty)) {
    for (S::TypeParamType *P : Sig->TParams)
      if (K < TArgs.size())
        In->Map[P] = TArgs[K++];
  }
  GenericInst *Raw = In.get();
  BySymbol[Raw->Symbol] = Raw;
  Insts.push_back(std::move(In));
  return Raw;
}

/// The attribute `@Name` of FD, or null.
static const ast::Attribute *findAttrNamed(const ast::FuncDecl *FD,
                                           llvm::StringRef Name) {
  for (const ast::Attribute *A : FD->Attrs)
    if (!A->Pkg && A->Name && A->Name->Name.str() == Name)
      return A;
  return nullptr;
}

/// A unit used as a type argument (GEN-14, `Vec2[KM]`): units are P04.24's.
static bool isUnitArg(const S::Type *T) {
  auto *B = dyn_cast_or_null<S::BasicType>(T);
  return B && B->BK == S::BasicKind::UnitArg;
}

bool InstanceWalk::madeType(GenericInst *Cur, S::Type *Generic) {
  // A type parameter passed on as it is makes no type: it is a type argument
  // of Cur, written in the source or bounded where it was made.
  return Cur && Generic && !isa<S::TypeParamType>(Generic) &&
         MS.mentionsTypeParam(Generic);
}

bool InstanceWalk::madeArg(GenericInst *Cur, S::Type *Generic, size_t K) {
  if (!Cur || !Generic)
    return false; // plain code wrote it
  for (;;) {
    if (auto *P = dyn_cast<S::PointerType>(Generic))
      Generic = P->Elem;
    else if (auto *O = dyn_cast<S::OptionType>(Generic))
      Generic = O->Elem;
    else
      break;
  }
  // (A type parameter is a type argument of Cur: its own arguments too.)
  auto *N = dyn_cast<S::NamedType>(Generic);
  return N && K < N->TArgs.size() && madeType(Cur, N->TArgs[K]);
}

bool InstanceWalk::argsTooLarge(GenericInst *Cur,
                                llvm::ArrayRef<S::Type *> TArgs,
                                llvm::function_ref<bool(size_t)> Made,
                                SourceLoc Site, const std::string &Name) {
  for (size_t K = 0; K < TArgs.size(); ++K) {
    if (!Made(K))
      continue; // written in the source: as large as the source is
    S::Type *T = TArgs[K];
    S::MethodSets::Extent E = MS.extent(T);
    if (E.Height > MaxArgHeight) {
      tooLarge(Cur, Site, Name,
               "a type argument nests more than " +
                   std::to_string(MaxArgHeight) + " levels");
      return true;
    }
    if (E.Size > MaxArgSize) {
      tooLarge(Cur, Site, Name,
               "a type argument has more than " + std::to_string(MaxArgSize) +
                   " parts as a tree");
      return true;
    }
  }
  return false;
}

GenericInst *InstanceWalk::requestFunc(GenericInst *Cur, const S::Func *Decl,
                                       llvm::ArrayRef<S::Type *> TArgs,
                                       SourceLoc Site,
                                       llvm::ArrayRef<S::Type *> Generic) {
  ++Steps;
  if (Stopped)
    return nullptr;
  // The instances of code with a unit parameter are not followed: the units
  // are erased (D4.20), the checker does not follow them either (they can
  // double at every level), and the lowering of units is P04.24.
  for (S::Type *T : TArgs)
    if (isUnitArg(T)) {
      unsupported(Site, "a generic function with a unit parameter (P04.24)");
      return nullptr;
    }
  if (!isOurs(Decl)) {
    unsupported(Site, "an instance of the generic function " +
                          Decl->Name.str() + " of another package (P06.06)");
    return nullptr;
  }
  // (Before the symbol: the bounds keep every name of an instance made by
  // substitution short.)
  if (argsTooLarge(
          Cur, TArgs,
          [&](size_t K) {
            return K < Generic.size() && madeType(Cur, Generic[K]);
          },
          Site, Decl->Name.str()))
    return nullptr;
  std::string Sym = Syms.funcInstance(Decl, TArgs);
  if (Sym.empty()) {
    if (Quiet) {
      // A key that is good enough to tell the checker's types apart.
      Sym = "?" + llvm::utohexstr(reinterpret_cast<uintptr_t>(Decl));
      for (S::Type *T : TArgs)
        Sym += "," + llvm::utohexstr(reinterpret_cast<uintptr_t>(T));
    } else {
      unsupported(Site, "an instance of " + Decl->Name.str() + " (" +
                            Syms.why() + ")");
      return nullptr;
    }
  }
  if (ScanTable)
    for (const auto &I : Insts) {
      ++Steps;
      if (I->Symbol == Sym)
        return I.get();
    }
  if (auto It = BySymbol.find(Sym); It != BySymbol.end())
    return It->second;
  if (Insts.size() >= MaxInstances) {
    tooLarge(Cur, Site, Decl->Name.str(),
             "the package needs more than " + std::to_string(MaxInstances) +
                 " instances");
    return nullptr;
  }
  return make(Cur, Decl, nullptr, TArgs, std::move(Sym), Site);
}

GenericInst *InstanceWalk::requestMethod(GenericInst *Cur, const S::Func *M,
                                         SourceLoc Site, S::Type *GenericRecv) {
  ++Steps;
  if (auto It = ByMethod.find(M); It != ByMethod.end())
    return It->second;
  if (Stopped)
    return nullptr;
  if (M->RecvInst)
    for (S::Type *T : M->RecvInst->TArgs)
      if (isUnitArg(T)) {
        unsupported(Site, "a generic type with a unit parameter (P04.24)");
        return nullptr;
      }
  const S::Func *Decl = S::declaredMethod(M);
  if (!M->RecvInst || !Decl || !Decl->Decl)
    return nullptr;
  if (!isOurs(Decl)) {
    unsupported(Site, "a method of the generic type " + typeText(M->RecvInst) +
                          " of another package (P06.06)");
    return nullptr;
  }
  if (!Decl->OwnTParams.empty()) {
    unsupported(Decl->Decl->getBegin(), "a generic method (P04.28)");
    return nullptr;
  }
  if (argsTooLarge(
          Cur, M->RecvInst->TArgs,
          [&](size_t K) { return madeArg(Cur, GenericRecv, K); }, Site,
          M->RecvInst->Obj->Name.str()))
    return nullptr;
  std::string Sym = Syms.func(M);
  if (Sym.empty()) {
    if (Quiet) {
      Sym = "?" + llvm::utohexstr(reinterpret_cast<uintptr_t>(M));
    } else {
      unsupported(Site, "the method " + Decl->Name.str() + " of " +
                            typeText(M->RecvInst) + " (" + Syms.why() + ")");
      return nullptr;
    }
  }
  if (ScanTable)
    for (const auto &I : Insts) {
      ++Steps;
      if (I->Symbol == Sym) {
        I->Aliases.push_back(M);
        ByMethod[M] = I.get();
        return I.get();
      }
    }
  if (auto It = BySymbol.find(Sym); It != BySymbol.end()) {
    // The same instance spelled otherwise (`Box[byte]`, `Box[uint8]`).
    It->second->Aliases.push_back(M);
    ByMethod[M] = It->second;
    return It->second;
  }
  if (Insts.size() >= MaxInstances) {
    tooLarge(Cur, Site, M->RecvInst->Obj->Name.str(),
             "the package needs more than " + std::to_string(MaxInstances) +
                 " instances");
    return nullptr;
  }
  GenericInst *In =
      make(Cur, Decl, M, M->RecvInst->TArgs, std::move(Sym), Site);
  ByMethod[M] = In;
  return In;
}

const GenericInst *InstanceWalk::methodInst(const S::Func *F) const {
  auto It = ByMethod.find(F);
  return It == ByMethod.end() ? nullptr : It->second;
}

const GenericInst *InstanceWalk::funcRef(const GenericInst *Cur,
                                         const ast::Expr *Key) const {
  auto It = Refs.find({Cur, Key});
  return It == Refs.end() ? nullptr : It->second;
}

//===----------------------------------------------------------------------===//
// Types and selections at an instance
//===----------------------------------------------------------------------===//

S::Type *InstanceWalk::inst(GenericInst *Cur, S::Type *T) {
  if (!T)
    return T;
  ++Steps;
  if (!Cur || Cur->Map.empty()) {
    MS.expand(T);
    return T;
  }
  auto [It, New] = Cur->Types.try_emplace(T, T);
  if (!New)
    return It->second;
  S::Type *R = MS.subst(T, Cur->Map);
  Cur->Types[T] = R;
  return R;
}

const S::Selection *InstanceWalk::selection(GenericInst *Cur,
                                            const ast::SelectorExpr *E) {
  auto It = I.Selections.find(E);
  if (It == I.Selections.end())
    return nullptr;
  if (!Cur)
    return &It->second;
  const S::Selection &In = It->second;
  S::Type *Recv = inst(Cur, In.Recv);
  if (Recv == In.Recv)
    return &In;
  auto [CIt, New] = Cur->Sels.try_emplace(E, std::nullopt);
  if (New) {
    S::Selection Out;
    // An operator element: its signature at the instance picks the overload.
    S::Type *ElemSig = nullptr;
    if (auto *OF = llvm::dyn_cast_or_null<S::Func>(In.Obj);
        OF && OF->Op != S::OpKind::None)
      ElemSig = inst(Cur, OF->Ty);
    if (MS.resolveSelection(In, Recv, Out, ElemSig))
      CIt->second = Out;
  }
  return CIt->second ? &*CIt->second : nullptr;
}

//===----------------------------------------------------------------------===//
// The types met
//===----------------------------------------------------------------------===//

void InstanceWalk::meet(GenericInst *Cur, S::Type *Root, SourceLoc At) {
  if (!Root)
    return;
  auto Known = Taint.find(Root);
  if (Known != Taint.end()) {
    if (Known->second && Cur)
      invalid(Cur, At, *Known->second);
    return;
  }
  struct Frame {
    S::Type *T;
    llvm::SmallVector<S::Type *, 4> Kids;
    size_t Next = 0;
    const Problem *Bad = nullptr;
  };
  llvm::SmallVector<Frame, 16> Stack;
  llvm::DenseSet<const S::Type *> Active;
  auto Note = [&](Problem::Kind K, S::Type *Formed, const char *Why) {
    TaintProblems.push_back(std::make_unique<Problem>(Problem{K, Formed, Why}));
    return TaintProblems.back().get();
  };
  auto Push = [&](S::Type *T) {
    ++Steps;
    Frame F;
    F.T = T;
    switch (T->getKind()) {
    case S::Type::Kind::Pointer:
      F.Kids.push_back(cast<S::PointerType>(T)->Elem);
      break;
    case S::Type::Kind::Slice:
      F.Kids.push_back(cast<S::SliceType>(T)->Elem);
      break;
    case S::Type::Kind::Ref: {
      auto *R = cast<S::RefType>(T);
      F.Kids.push_back(R->Elem);
      // View[X] (ADR-0020): X is no pointer kind, no unsafe.Pointer, no view.
      if (!R->Borrowed)
        if (const char *Why = MS.badViewArg(R->Elem))
          F.Bad = Note(Problem::View, T, Why);
      break;
    }
    case S::Type::Kind::Option: {
      auto *O = cast<S::OptionType>(T);
      F.Kids.push_back(O->Elem);
      // Option[X] (ADR-0018 point 11): X is not nilable and no Option.
      if (const char *Why = MS.badOptionArg(O->Elem))
        F.Bad = Note(Problem::Option, T, Why);
      break;
    }
    case S::Type::Kind::Array:
      F.Kids.push_back(cast<S::ArrayType>(T)->Elem);
      break;
    case S::Type::Kind::Chan:
      F.Kids.push_back(cast<S::ChanType>(T)->Elem);
      break;
    case S::Type::Kind::Map:
      F.Kids.push_back(cast<S::MapType>(T)->Key);
      F.Kids.push_back(cast<S::MapType>(T)->Elem);
      break;
    case S::Type::Kind::Tuple:
      for (S::Type *E : cast<S::TupleType>(T)->Elems)
        F.Kids.push_back(E);
      break;
    case S::Type::Kind::Signature:
      F.Kids.push_back(cast<S::SignatureType>(T)->Params);
      F.Kids.push_back(cast<S::SignatureType>(T)->Results);
      break;
    case S::Type::Kind::Struct:
      for (S::Var *V : cast<S::StructType>(T)->Fields)
        F.Kids.push_back(V->Ty);
      break;
    case S::Type::Kind::Named: {
      auto *N = cast<S::NamedType>(T);
      for (S::Type *A : N->TArgs)
        F.Kids.push_back(A);
      if (N->Underlying)
        F.Kids.push_back(N->Underlying);
      break;
    }
    default:
      break; // basic types, interfaces and unions hold no instances to find
    }
    Active.insert(T);
    Stack.push_back(std::move(F));
  };
  Push(Root);
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    if (F.Bad || F.Next == F.Kids.size()) {
      const Problem *Bad = F.Bad;
      S::Type *T = F.T;
      Active.erase(T);
      Taint[T] = Bad;
      Stack.pop_back();
      if (Bad && !Stack.empty() && !Stack.back().Bad)
        Stack.back().Bad = Bad;
      continue;
    }
    S::Type *K = F.Kids[F.Next++];
    if (!K)
      continue;
    if (auto It = Taint.find(K); It != Taint.end()) {
      if (It->second && !F.Bad)
        F.Bad = It->second;
      continue;
    }
    if (Active.count(K))
      continue;
    Push(K);
  }
  if (const Problem *Bad = Taint[Root]; Bad && Cur)
    invalid(Cur, At, *Bad);
}

void InstanceWalk::described(GenericInst *Cur, S::Type *T, S::Type *Generic,
                             SourceLoc At) {
  if (!T || Stopped)
    return;
  // The results of a call: each of its values.
  llvm::SmallVector<S::Type *, 2> Ts, Gs;
  if (auto *Tup = dyn_cast<S::TupleType>(T)) {
    Ts.append(Tup->Elems.begin(), Tup->Elems.end());
    if (auto *GT = dyn_cast_or_null<S::TupleType>(Generic);
        GT && GT->Elems.size() == Tup->Elems.size())
      Gs.append(GT->Elems.begin(), GT->Elems.end());
  } else {
    Ts.push_back(T);
    Gs.push_back(Generic);
  }
  for (size_t J = 0; J < Ts.size(); ++J) {
    S::Type *D = Ts[J];
    S::Type *G = J < Gs.size() ? Gs[J] : nullptr;
    // An Option[X] becomes an interface value as its X (or as nil).
    if (auto *O = dyn_cast_or_null<S::OptionType>(D))
      D = O->Elem;
    S::Type *Base = D;
    if (auto *P = dyn_cast_or_null<S::PointerType>(D))
      Base = P->Elem;
    // Only an instance of a generic type has methods to make; an interface
    // has no descriptor of its own.
    auto *N = dyn_cast_or_null<S::NamedType>(Base);
    if (!N || !N->Orig || N->TArgs.empty() ||
        llvm::isa_and_nonnull<S::InterfaceType>(S::under(D)))
      continue;
    if (!Described.insert(D).second && !Redescribe)
      continue;
    // The method set the descriptor lists (Lowering::typeInfo): the
    // checker's, the same question the lowering asks.
    llvm::SmallVector<S::MethodHit, 8> Hits;
    MS.methodSet(D, Hits);
    for (const S::MethodHit &H : Hits) {
      ++Steps;
      // (A promoted method in an interface is refused by the lowering,
      // P04.22.)
      if (H.F && H.F->RecvInst && !H.ParentSteps && H.Path.empty())
        requestMethod(Cur, H.F, At, G);
    }
  }
}

//===----------------------------------------------------------------------===//
// The bodies
//===----------------------------------------------------------------------===//

void InstanceWalk::noteResultUse(const ast::Node *N, S::SignatureType *Sig) {
  auto CallOf = [](const ast::Expr *E) -> const ast::CallExpr * {
    while (auto *P = dyn_cast_or_null<ast::ParenExpr>(E))
      E = P->X;
    return dyn_cast_or_null<ast::CallExpr>(E);
  };
  auto IsBlank = [](const ast::Expr *E) {
    auto *Id = dyn_cast_or_null<ast::Ident>(E);
    return Id && Id->Name.str() == "_";
  };
  if (auto *RS = dyn_cast<ast::ReturnStmt>(N)) {
    // `return f()` into the results of the function (or the literal) it is
    // in: the caller checks them (ERR-3, OWN-P10): as many, or in a `!`
    // function the declared ones and the failure (the signature's results
    // end in it).
    if (!Sig || RS->Results.size() != 1)
      return;
    const ast::CallExpr *C = CallOf(RS->Results[0]);
    auto *Tup = C ? dyn_cast_or_null<S::TupleType>(I.typeOf(C)) : nullptr;
    if (Tup && Tup->Elems.size() == Sig->Results->size())
      ResultUses[C].Forward = true;
  } else if (auto *ES = dyn_cast<ast::ExprStmt>(N)) {
    if (const ast::CallExpr *C = CallOf(ES->X))
      ResultUses[C].Dropped = ~uint64_t(0);
  } else if (auto *AS = dyn_cast<ast::AssignStmt>(N)) {
    if (AS->Rhs.size() != 1 || AS->Lhs.size() < 2)
      return;
    if (const ast::CallExpr *C = CallOf(AS->Rhs[0])) {
      uint64_t Dropped = 0;
      for (size_t K = 0; K < AS->Lhs.size() && K < 64; ++K)
        if (IsBlank(AS->Lhs[K]))
          Dropped |= uint64_t(1) << K;
      ResultUses[C].Dropped = Dropped;
    }
  } else if (auto *VS = dyn_cast<ast::ValueSpec>(N)) {
    if (VS->Values.size() != 1 || VS->Names.size() < 2)
      return;
    if (const ast::CallExpr *C = CallOf(VS->Values[0])) {
      uint64_t Dropped = 0;
      for (size_t K = 0; K < VS->Names.size() && K < 64; ++K)
        if (IsBlank(VS->Names[K]))
          Dropped |= uint64_t(1) << K;
      ResultUses[C].Dropped = Dropped;
    }
  }
}

void InstanceWalk::checkedResult(GenericInst *Cur, const ast::CallExpr *C,
                                 S::SignatureType *Sig) {
  if (!Cur || Cur->Bad)
    return;
  // A last result written `error` is the checker's (Checker::guardKindOf);
  // one that is a type parameter is checked here, at the instance.
  auto *Tup = dyn_cast_or_null<S::TupleType>(I.typeOf(C));
  if (!Tup || Tup->Elems.size() < 2 ||
      !isa<S::TypeParamType>(Tup->Elems.back()))
    return;
  auto *R = dyn_cast_or_null<S::TupleType>(inst(Cur, Tup));
  if (!R || R->Elems.size() != Tup->Elems.size())
    return;
  // The error word: a last result identical to `error`, the one `f!()` tests
  // and the checker's guard is (Call.cpp, Checker::guardKindOf).
  S::Type *Err = CP.Ctx->errorType();
  if (!MS.identical(R->Elems.back(), Err))
    return;
  ResultUse U;
  if (auto It = ResultUses.find(C); It != ResultUses.end())
    U = It->second;
  // `return f()` hands the results on: the caller checks them in turn when
  // they end in error at the instance too, or in the failure of a `!`
  // function (Checker::checkForwarded).
  if (U.Forward && Sig &&
      (Sig->Fails ||
       (Sig->Results->size() &&
        MS.identical(inst(Cur, Sig->Results->Elems.back()), Err))))
    return;
  // A result with no zero value is left unset by a failure, so it is usable
  // only where the error is known nil (OWN-P10), which the generic body
  // cannot know. One dropped with `_` is never used.
  for (size_t K = 0; K + 1 < R->Elems.size(); ++K) {
    if (K < 64 && (U.Dropped >> K & 1))
      continue;
    if (MS.hasZeroValue(R->Elems[K]))
      continue;
    Cur->Bad = true;
    Failed = true;
    std::string Callee = "a function value";
    const ast::Expr *Fun = C->Fun;
    while (auto *P = dyn_cast<ast::ParenExpr>(Fun))
      Fun = P->X;
    if (auto *IX = dyn_cast<ast::IndexExpr>(Fun))
      Fun = IX->X;
    if (auto *Sel = dyn_cast<ast::SelectorExpr>(Fun))
      Callee = Sel->Sel->Name.str();
    else if (auto *Id = dyn_cast<ast::Ident>(Fun))
      Callee = Id->Name.str();
    auto B = Diags.report(Cur->Site, diag::err_inst_checked_result);
    B << nameOf(*Cur) << Callee + "(...)"
      << cast<S::TypeParamType>(Tup->Elems.back())->Obj->Name.str();
    notes(B, *Cur, C->getBegin(), "called here");
    return;
  }
}

void InstanceWalk::onSelection(GenericInst *Cur, const ast::SelectorExpr *E) {
  // A method called, taken as a value (`x.M`) or as an expression (`T.M`,
  // P04.07's thunks call it).
  const S::Selection *Sel = selection(Cur, E);
  if (!Sel ||
      (Sel->K != S::Selection::MethodVal && Sel->K != S::Selection::MethodExpr))
    return;
  auto *F = dyn_cast_or_null<S::Func>(Sel->Obj);
  if (!F || F->IsInterfaceMethod || F->IsBuiltinMethod)
    return;
  // A method with type parameters of its own is P04.28's: refused once, where
  // it is declared (a call of it is no second error; one never called draws
  // nothing).
  if (const S::Func *D = S::declaredMethod(F);
      D && !D->OwnTParams.empty() && D->Decl) {
    unsupported(D->Decl->getBegin(), "a generic method (P04.28)");
    return;
  }
  if (!F->RecvInst)
    return;
  // (The receiver as the generic code wrote it: which of the instance's type
  // arguments the instance made.)
  S::Type *GenericRecv = nullptr;
  if (auto It = I.Selections.find(E); It != I.Selections.end())
    GenericRecv = It->second.Recv;
  if (GenericInst *In = requestMethod(Cur, F, E->getBegin(), GenericRecv))
    Refs[{Cur, E}] = In;
}

void InstanceWalk::onInstance(GenericInst *Cur, const ast::Expr *E,
                              const S::Instance &In) {
  // The generic function or type the expression names.
  const ast::Ident *Id = dyn_cast<ast::Ident>(E);
  if (auto *Sel = dyn_cast<ast::SelectorExpr>(E))
    Id = Sel->Sel;
  auto U = Id ? I.Uses.find(Id) : I.Uses.end();
  if (U == I.Uses.end())
    return;
  if (auto *Fn = dyn_cast_or_null<S::Func>(U->second)) {
    auto *Sig = dyn_cast_or_null<S::SignatureType>(Fn->Ty);
    if (!Sig || Sig->TParams.empty())
      return;
    llvm::SmallVector<S::Type *, 4> TArgs;
    for (S::Type *A : In.TArgs)
      TArgs.push_back(inst(Cur, A));
    if (Cur && Cur->Bad)
      return;
    if (GenericInst *G = requestFunc(Cur, Fn, TArgs, E->getBegin(), In.TArgs))
      Refs[{Cur, E}] = G;
  }
}

void InstanceWalk::visitExpr(GenericInst *Cur, const ast::Expr *E) {
  ++Steps;
  if (const S::TypeAndValue *TV = I.lookup(E); TV && TV->Ty) {
    // A constant has no run time form of its own type (untyped).
    S::Type *T = inst(Cur, TV->Ty);
    // (The name of a generic function has a type with the callee's own
    // parameters: each call's instance is what is walked.)
    if (!MS.mentionsTypeParam(T))
      meet(Cur, T, E->getBegin());
  }
  // A name declared here (a variable, a parameter, a field): its type.
  if (auto *Id = dyn_cast<ast::Ident>(E))
    if (auto D = I.Defs.find(Id); D != I.Defs.end() && D->second)
      if (auto *V = dyn_cast<S::Var>(D->second); V && V->Ty) {
        S::Type *T = inst(Cur, V->Ty);
        if (!MS.mentionsTypeParam(T))
          meet(Cur, T, E->getBegin());
      }
  if (auto It = I.Instances.find(E); It != I.Instances.end())
    onInstance(Cur, E, It->second);
  if (auto *Sel = dyn_cast<ast::SelectorExpr>(E))
    onSelection(Cur, Sel);
  // A value that becomes an interface value, and a type assertion: the
  // descriptor of the dynamic type, with its methods.
  if (auto It = I.IfaceConversions.find(E); It != I.IfaceConversions.end())
    for (S::Type *T : It->second)
      described(Cur, inst(Cur, T), T, E->getBegin());
  if (auto *TA = dyn_cast<ast::TypeAssertExpr>(E); TA && TA->Type)
    described(Cur, inst(Cur, I.typeOf(TA->Type)), I.typeOf(TA->Type),
              TA->getBegin());
}

void InstanceWalk::visitOperator(GenericInst *Cur, const ast::Node *N) {
  auto Visit = [&](const S::OperatorUse &U) {
    for (const ast::CallExpr *C : {U.Call, U.SetCall})
      if (C)
        if (auto *Sel = dyn_cast<ast::SelectorExpr>(C->Fun)) {
          ++Steps;
          onSelection(Cur, Sel);
        }
  };
  if (auto It = I.Operators.find(N); It != I.Operators.end())
    Visit(It->second);
  // (The comparison of a case value with the tag.)
  if (auto *E = dyn_cast<ast::Expr>(N))
    if (auto It = I.CaseOperators.find(E); It != I.CaseOperators.end())
      Visit(It->second);
}

void InstanceWalk::scanTree(GenericInst *Cur, const ast::Node *Root,
                            const S::Func *Obj) {
  // Each node with the signature of the function, or the function literal,
  // whose body it is in (a `return` inside a literal hands its results to
  // the literal's caller).
  struct Item {
    const ast::Node *N;
    S::SignatureType *Sig;
  };
  llvm::SmallVector<Item, 64> Work{
      {Root, Obj ? dyn_cast_or_null<S::SignatureType>(Obj->Ty) : nullptr}};
  llvm::SmallVector<const ast::Node *, 8> Kids;
  while (!Work.empty() && !(Cur && Cur->Bad) && !Stopped) {
    auto [N, Sig] = Work.pop_back_val();
    if (!N)
      continue;
    // The code the instances hold is bounded too (and with it the GIR the
    // lowering makes of it): one error, then no more.
    if (++Walked > MaxWalked) {
      tooLarge(Cur, N->getBegin(), Cur ? Cur->Decl->Name.str() : "the package",
               "the instances of the package hold more than " +
                   std::to_string(MaxWalked) + " nodes of code");
      break;
    }
    // (A statement is met before the call it holds.)
    noteResultUse(N, Sig);
    // A type switch tests each case's type against the descriptor.
    if (auto *TS = dyn_cast<ast::TypeSwitchStmt>(N); TS && TS->Body)
      for (const ast::Stmt *St : TS->Body->List)
        if (auto *CC = dyn_cast<ast::CaseClause>(St))
          for (const ast::Expr *T : CC->List)
            described(Cur, inst(Cur, I.typeOf(T)), I.typeOf(T), T->getBegin());
    if (!I.Operators.empty() || !I.CaseOperators.empty())
      visitOperator(Cur, N);
    if (auto *E = dyn_cast<ast::Expr>(N)) {
      visitExpr(Cur, E);
      if (auto *C = dyn_cast<ast::CallExpr>(E))
        checkedResult(Cur, C, Sig);
    }
    S::SignatureType *KidSig = Sig;
    if (auto *Lit = dyn_cast<ast::FuncLit>(N))
      KidSig = dyn_cast_or_null<S::SignatureType>(I.typeOf(Lit));
    Kids.clear();
    ast::forEachChild(
        N, [&](const char *, const ast::Node *C) { Kids.push_back(C); });
    for (size_t K = Kids.size(); K-- > 0;)
      Work.push_back({Kids[K], KidSig});
  }
}

void InstanceWalk::scanFunc(GenericInst *Cur, const ast::FuncDecl *FD,
                            const S::Func *Obj) {
  // The signature first: its types are met at the instance, so an invalid
  // `Option[X]` in a parameter is found before the body is walked.
  SourceLoc At = FD->getBegin();
  if (Obj->Recv)
    if (S::Type *R = inst(Cur, Obj->Recv->Ty); !MS.mentionsTypeParam(R))
      meet(Cur, R, At);
  if (Obj->Ty)
    if (S::Type *Sg = inst(Cur, Obj->Ty); !MS.mentionsTypeParam(Sg))
      meet(Cur, Sg, At);
  if ((Cur && Cur->Bad) || Stopped)
    return;
  if (FD->Body)
    scanTree(Cur, FD->Body, Obj);
}

bool InstanceWalk::run() {
  // The checker records every instantiation a package writes (Info::Instances:
  // a generic function or type with its type arguments, explicit or
  // inferred). With none there is no instance to find: plain code pays
  // nothing for the walk.
  if (I.Instances.empty())
    return true;
  // The limits on instances and on the code they hold grow with the package:
  // a written program never needs more instances than it has bytes (a chain
  // of 200000 generic functions has 200000), nor walks more than a node or
  // so per byte of plain code, while one that expands without end (2^n
  // instances from n functions) passes them at once.
  if (CP.SM) {
    size_t Bytes = 0;
    for (const ast::File *File : Files)
      if (FileID F = CP.SM->getFileID(File->PackageLoc); F.isValid())
        Bytes += CP.SM->getBuffer(F).size();
    MaxInstances = std::max(MaxInstances, Bytes);
    MaxWalked = std::max<uint64_t>(MaxWalked, uint64_t(16) * Bytes);
  }
  // The roots: every function and method of the package that is not generic,
  // and the initializers of the package variables, in declaration order.
  for (const ast::File *File : Files)
    for (const ast::Decl *D : File->Decls) {
      if (Stopped)
        break;
      if (auto *FD = dyn_cast<ast::FuncDecl>(D)) {
        auto Def = I.Defs.find(FD->Name);
        auto *Obj = Def == I.Defs.end()
                        ? nullptr
                        : dyn_cast_or_null<S::Func>(Def->second);
        if (!Obj || !FD->Body)
          continue;
        auto *Sig = dyn_cast_or_null<S::SignatureType>(Obj->Ty);
        if (!Sig || !Sig->TParams.empty() || !Obj->RecvTParams.empty() ||
            !Obj->OwnTParams.empty())
          continue;
        scanFunc(nullptr, FD, Obj);
      } else if (auto *GD = dyn_cast<ast::GenDecl>(D)) {
        if (GD->Keyword == tok::kw_var && !GD->IsUnit)
          scanTree(nullptr, GD, nullptr);
      }
    }
  drain();
  return !Failed;
}

void InstanceWalk::drain() {
  // The instances, in the order they were found; the list grows as it is
  // walked.
  for (; Scanned < Insts.size() && !Stopped; ++Scanned) {
    GenericInst *In = Insts[Scanned].get();
    if (In->Bad || !In->Decl->Decl)
      continue;
    scanFunc(In, In->Decl->Decl, In->Decl);
  }
}

const GenericInst *InstanceWalk::needMethod(const S::Func *M, SourceLoc Site) {
  GenericInst *In = requestMethod(nullptr, M, Site);
  drain();
  return In;
}

//===----------------------------------------------------------------------===//
// Local types
//===----------------------------------------------------------------------===//

void impl::declareLocalTypes(const S::CheckedPackage &CP,
                             llvm::ArrayRef<const ast::File *> Files,
                             Symbols &Syms) {
  const S::Info &I = CP.I;
  // One pre-order walk of each body in source order (an explicit stack).
  // Each node is in a scope: the function, or a function literal named by
  // its place in the scope around it (sym::Context::closure, the ordinal of
  // Escape.cpp's analyzeEntity: 1, 2, ... among the literals directly
  // inside, so its types are named as its closure is). A type gets the next
  // ordinal of its scope, from 0.
  struct Scope {
    const sym::Node *Node;
    uint64_t Types = 0;
    unsigned Lits = 0;
  };
  llvm::SmallVector<Scope, 8> Scopes;
  struct Item {
    const ast::Node *N;
    unsigned Scope;
  };
  llvm::SmallVector<Item, 64> Work;
  llvm::SmallVector<const ast::Node *, 8> Kids;
  auto Walk = [&](const ast::Node *Root, const sym::Node *RootScope) {
    Scopes.assign({{RootScope}});
    Work.assign({{Root, 0}});
    while (!Work.empty()) {
      auto [N, Sc] = Work.pop_back_val();
      if (!N)
        continue;
      if (isa<ast::FuncLit>(N)) {
        Scopes.push_back(
            {Syms.Ctx.closure(Scopes[Sc].Node, ++Scopes[Sc].Lits)});
        Sc = Scopes.size() - 1;
      }
      if (auto *GD = dyn_cast<ast::GenDecl>(N);
          GD && GD->Keyword == tok::kw_type)
        for (const ast::Spec *Sp : GD->Specs) {
          auto *TS = dyn_cast<ast::TypeSpec>(Sp);
          if (!TS || TS->AssignLoc.isValid() ||
              (TS->TypeParams && TS->TypeParams->numFields()))
            continue; // an alias has no symbol; a generic one is P04.28
          auto Def = I.Defs.find(TS->Name);
          if (auto *TN = Def == I.Defs.end()
                             ? nullptr
                             : dyn_cast_or_null<S::TypeName>(Def->second))
            Syms.Names.declareLocalType(TN, Scopes[Sc].Node, Scopes[Sc].Types);
          ++Scopes[Sc].Types;
        }
      Kids.clear();
      ast::forEachChild(
          N, [&](const char *, const ast::Node *C) { Kids.push_back(C); });
      for (size_t K = Kids.size(); K-- > 0;)
        Work.push_back({Kids[K], Sc});
    }
  };
  // `init` functions are numbered in file order, as Lowering::declareFunction
  // numbers them (InitDecls).
  unsigned Inits = 0;
  for (const ast::File *File : Files)
    for (const ast::Decl *D : File->Decls) {
      auto *FD = dyn_cast<ast::FuncDecl>(D);
      if (!FD || !FD->Body)
        continue;
      const sym::Node *Scope = nullptr;
      if (!FD->Recv && FD->Name->Name.str() == "init") {
        if (findAttrNamed(FD, "Export"))
          continue;
        Scope = Syms.Ctx.initFunc(Syms.Names.pathOf(CP.Pkg), Inits++);
      } else {
        auto Def = I.Defs.find(FD->Name);
        auto *Obj = Def == I.Defs.end()
                        ? nullptr
                        : dyn_cast_or_null<S::Func>(Def->second);
        auto *Sig = Obj ? dyn_cast_or_null<S::SignatureType>(Obj->Ty) : nullptr;
        if (!Obj || !Obj->RecvTParams.empty() || !Obj->OwnTParams.empty() ||
            (Sig && !Sig->TParams.empty()))
          continue; // generic code: its types are refused (P04.28)
        Scope = Syms.funcNode(Obj);
      }
      if (Scope)
        Walk(FD->Body, Scope);
    }
  // The function literals of a package variable's initializer are named by
  // the variable (Body::lowerInit); a type in one, by that literal.
  for (const S::InitEntry &E : CP.InitOrder)
    if (!E.Lhs.empty() && E.Init)
      if (const sym::Node *Scope = Syms.Names.var(E.Lhs[0]))
        Walk(E.Init, Scope);
}

//===----------------------------------------------------------------------===//
// `gopp check`
//===----------------------------------------------------------------------===//

bool gir::checkInstances(const sema::CheckedPackage &CP,
                         llvm::ArrayRef<const ast::File *> Files,
                         DiagnosticsEngine &Diags, const LowerOptions &Opts) {
  // (No instantiation written, no instance: InstanceWalk::run.)
  if (CP.Opts.GoCompat || !CP.Ctx || !CP.SM || CP.I.Instances.empty())
    return true;
  sema::MethodSets MS(CP);
  Symbols Syms;
  declareLocalTypes(CP, Files, Syms);
  InstanceWalk Walk(CP, Files, MS, Syms, Diags, /*Quiet=*/true,
                    Opts.MaxInstances, Opts.MaxWalkedNodes);
  return Walk.run();
}
