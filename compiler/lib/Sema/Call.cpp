//===- Call.cpp - Selectors, calls and instantiation ----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's call.go (BSD-style license, Copyright The Go Authors),
// with the Go++ typing of `x.Clone()`, `IsNil()`, `move` parameters and `f!()`.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "gopp/Sema/Foreign.h"

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

void Checker::recordSelection(const SelectorExpr *Ex, const Selection &Sel) {
  I.Selections[Ex] = Sel;
}

//===----------------------------------------------------------------------===//
// Selectors
//===----------------------------------------------------------------------===//

Func *Checker::syntheticMethod(StringRef Name, ArrayRef<Type *> Results,
                               SourceLoc Pos) {
  Func *F = newFunc(Name, nullptr, Pos);
  F->Pkg = nullptr;
  F->Ty = Ctx.signature(Ctx.tuple({}), Ctx.tuple(Results), false, false, 0);
  F->IsBuiltinMethod = true;
  return F;
}

bool Checker::valueHasMember(Type *T, StringRef Sel) {
  PtrKind PK;
  Type *PElem;
  if (hasPtrKind(T, &PK, &PElem)) {
    if (PK == PtrKind::Weak)
      return Sel == "Lock";
    if (Sel == "Clone")
      return true;
    T = Ctx.pointer(PElem);
  }
  LookupResult LR = lookupFieldOrMethod(T, true, S.Pkg, Sel);
  return LR.Obj || LR.Ambiguous;
}

/// PAR-5: a `move` receiver takes ownership of the receiver, so a call or a
/// method value is legal only on an owned value (a local or a temporary), or
/// on move(x) / x.Clone().
void Checker::checkMoveRecvUse(const SelectorExpr *Sel, const char *What) {
  Operand R;
  R.M = Mode::Variable;
  R.E = Sel->X;
  R.Ty = I.typeOf(Sel->X);
  if (!R.Ty || !isValid(R.Ty))
    return;
  if (isa<PointerType>(under(R.Ty))) {
    // `p.Finish()` copies *p out of the pointer target.
    err(R, diag::err_own_r3a_needs_ownership,
        {What, "*" + str(Sel->X), "a value in another place"})
        .note(R.pos(), diag::note_own_fix_clone, {"(*" + str(Sel->X) + ")"});
  } else {
    needOwnership(R, What);
  }
}

Package *Checker::qualifierPackage(const ast::Expr *X) {
  if (auto *Id = dyn_cast<ast::Ident>(X)) {
    auto *PN = dyn_cast_or_null<PkgName>(E.Sc->lookup(name(Id), E.Pos));
    if (!PN)
      return nullptr;
    recordUse(Id, PN);
    markUsed(PN);
    return PN->Imported;
  }
  // `pkg.ns.Name`: a namespace of a foreign package is a PkgName again.
  auto *Sel = dyn_cast<SelectorExpr>(X);
  if (!Sel)
    return nullptr;
  Package *P = qualifierPackage(Sel->X);
  if (!P || !P->Foreign)
    return nullptr;
  auto *PN = dyn_cast_or_null<PkgName>(P->Foreign->lookup(name(Sel->Sel)).Obj);
  if (!PN)
    return nullptr;
  recordUse(Sel->Sel, PN);
  return PN->Imported;
}

void Checker::markUsed(PkgName *PN) {
  // A body checker only records the import in the set of its worker;
  // checkPackage sets the flags after the bodies, so bodies never write
  // shared objects (P03.10).
  if (UsedImports)
    UsedImports->insert(PN);
  else
    PN->Used = true;
}

void Checker::selector(Operand &X, const SelectorExpr *Ex, bool WantType,
                       NamedType *Def) {
  StringRef Sel = name(Ex->Sel);
  auto Error = [&]() {
    X.M = Mode::Invalid;
    X.E = Ex;
  };
  // Is this selector the start of a longer one, or the operand of `&`? (The
  // parent part `c.Payment` is allowed only there, EXP-15.)
  const PartialMark SelfMark = takePartialMark(Ex);
  // Package-qualified identifier (`pkg.Name`), or a name in a namespace of a
  // C++ header (`pkg.ns.Name`, ADR-0010 Q4).
  if (Package *P = qualifierPackage(Ex->X)) {
    Object *Exp = P->PkgScope ? P->PkgScope->lookupLocal(Sel) : nullptr;
    if (!Exp && P->Foreign) {
      ForeignLookup FL = P->Foreign->lookup(Sel);
      Exp = FL.Obj;
      if (!Exp && !FL.Ambiguous.empty()) {
        // Reported where the name is used (ADR-0010 Q4); the full path
        // always works.
        Err Er = err(Ex->Sel, diag::err_foreign_ambiguous,
                     {std::string(Sel), std::string(P->Name)});
        for (const ForeignCandidate &C : FL.Ambiguous)
          Er.note(Ex->Sel->getBegin(),
                  "candidate " + C.Spelling + " (" + C.Origin + ")");
        Er.note(Ex->Sel->getBegin(), "help: write the full path, e.g. " +
                                         FL.Ambiguous.front().Spelling);
        return Error();
      }
    }
    if (!Exp) {
      if (!P->Fake && Sel != "_") {
        // A better message when Sel matches an exported name ignoring case
        // (the first in name order, so the result does not depend on the
        // scope's hash order).
        Object *Have = nullptr;
        if (P->PkgScope && !P->IsForeign)
          for (auto &KV : P->PkgScope->Elems)
            if (KV.second->exported() && KV.first.equals_insensitive(Sel) &&
                (!Have || KV.first < Have->Name))
              Have = KV.second;
        err(Ex->Sel, diag::err_undefined,
            {str(Ex) +
             (Have ? " (but have " + std::string(Have->Name) + ")" : "")});
      }
      return Error();
    }
    if (!Exp->exported() && !P->IsForeign) {
      err(Ex->Sel, diag::err_unexported_name,
          {std::string(Sel), std::string(P->Name)});
    }
    recordUse(Ex->Sel, Exp);
    switch (Exp->getKind()) {
    case Object::Kind::Const:
      X.M = Mode::Constant;
      X.Ty = Exp->Ty;
      X.Val = cast<Const>(Exp)->Val ? *cast<Const>(Exp)->Val : ConstValue();
      break;
    case Object::Kind::TypeName:
      X.M = Mode::TypeExpr;
      X.Ty = Exp->Ty;
      break;
    case Object::Kind::Var:
      X.M = Mode::Variable;
      X.Ty = Exp->Ty;
      break;
    case Object::Kind::Func:
      X.M = Mode::Value;
      X.Ty = Exp->Ty;
      break;
    case Object::Kind::Builtin:
      X.M = Mode::Builtin;
      X.Ty = Exp->Ty;
      X.BId = cast<Builtin>(Exp)->Id;
      break;
    case Object::Kind::PkgName:
      // A namespace of a C++ header used on its own.
      err(Ex, diag::err_package_without_selector, {str(Ex)});
      return Error();
    default:
      return Error();
    }
    X.E = Ex;
    return;
  }

  // Flow typing: a receiver that is known to be set has the type of its value
  // here, so `l.Get(k)` after `l != nil` calls the method of T. Option's own
  // IsNil and Get stay reachable on it when T has no member of that name.
  if (own() && (Sel == "IsNil" || Sel == "Get")) {
    // `move(o).Get()` takes the Option o itself (ADR-0018 point 11).
    const ast::Expr *Rcv = unparen(Ex->X);
    if (auto *C = dyn_cast<CallExpr>(Rcv))
      if (auto *F = dyn_cast<ast::Ident>(unparen(C->Fun));
          F && name(F) == "move" && C->Args.size() == 1 &&
          llvm::isa_and_nonnull<Builtin>(lookup(name(F))))
        Rcv = unparen(C->Args[0]);
    if (auto *Id = dyn_cast<ast::Ident>(Rcv))
      if (auto *V = dyn_cast_or_null<Var>(lookup(name(Id))))
        if (auto *Op = V->Ty ? dyn_cast<OptionType>(under(V->Ty)) : nullptr)
          // (A narrowing that only a loop's assumption gives never picks T's
          // method over the Option's own, P03.19.)
          if (!isNarrowed(V) || Cur.TentN.count(V) ||
              !valueHasMember(Op->Elem, Sel))
            markNoNarrow(Id);
  }
  markPartialOk(Ex->X, /*Addr=*/false); // the start of this selector
  exprOrType(X, Ex->X, false);
  PartialOk = nullptr;
  switch (X.M) {
  case Mode::TypeExpr:
    // `type T T.x` (go.dev/issue/51509).
    if (Def && X.Ty == Def) {
      Object *Cycle[] = {Def->Obj};
      cycleError(Cycle, 0);
      return Error();
    }
    break;
  case Mode::Builtin:
    err(Ex, diag::err_sema_call,
        {"invalid use of " + str(X) + " in selector expression"});
    return Error();
  case Mode::Invalid:
    return Error();
  default:
    break;
  }
  // Only a package-qualified name can be a type (types2,
  // go.dev/issue/57522).
  if (WantType) {
    err(Ex->Sel, diag::err_not_a_type, {str(Ex)});
    return Error();
  }

  // Go++ (OWN-P5, OWN-R6, ADR-0017, ADR-0018): Option, the pointer kinds, and
  // Clone.
  Type *LookupT = X.Ty;
  bool Addressable = X.M == Mode::Variable;
  auto Synth = [&](Func *B) {
    Selection SelInfo;
    SelInfo.K = Selection::MethodVal;
    SelInfo.Recv = X.Ty;
    SelInfo.Obj = B;
    recordSelection(Ex, SelInfo);
    X.M = Mode::Value;
    X.Ty = B->Ty;
    X.E = Ex;
  };
  if (own() && X.M != Mode::TypeExpr && isViewType(X.Ty)) {
    // A View[T] (OWN-V1): the fields of T are read through it, and only
    // Clone is a method.
    Type *Elem = cast<RefType>(under(X.Ty))->Elem;
    if (Sel == "Clone") {
      // ADR-0023 decision 16: the view of a tagged type may see a child; its
      // clone is a Unique holding a full copy of the dynamic type.
      const bool Tagged = cloneThroughTable(X.Ty) != nullptr;
      noteViewClone(X.Ty); // a View[T] of a type parameter (R9)
      std::string Why;
      if (!satisfiesClone(Elem, &Why, /*ByValue=*/!Tagged)) {
        err(Ex->Sel, diag::err_own_clone_not_cloneable,
            {str(Ex->X), str(Elem), Why.empty() ? "" : ": " + Why});
        return Error();
      }
      if (viewCopyHasPointer(Elem)) {
        err(Ex->Sel, diag::err_view_copy_pointer, {str(Ex->X), str(Elem)});
        return Error();
      }
      Type *Res[] = {Tagged ? uniqueOf(Elem) : Elem};
      Synth(syntheticMethod("Clone", Res, Ex->Sel->getBegin()));
      return;
    }
    // An Option read through a view: IsNil and Get, which gives a view of
    // the value (ADR-0018 point 11, TYP-9).
    if (auto *Op = dyn_cast<OptionType>(under(Elem))) {
      if (Sel == "IsNil") {
        Type *Res[] = {typ(BasicKind::Bool)};
        Synth(syntheticMethod("IsNil", Res, Ex->Sel->getBegin()));
        return;
      }
      if (Sel == "Get") {
        Type *Res[] = {viewRead(Op->Elem), typ(BasicKind::Bool)};
        Synth(syntheticMethod("Get", Res, Ex->Sel->getBegin()));
        return;
      }
    }
    LookupResult VR = lookupFieldOrMethod(Elem, true, S.Pkg, Sel);
    if (auto *F = dyn_cast_or_null<Var>(VR.Obj)) {
      if (!F->Ty)
        objDecl(F, nullptr);
      Selection SelInfo;
      SelInfo.K = Selection::FieldVal;
      SelInfo.Recv = X.Ty;
      SelInfo.Obj = F;
      SelInfo.Index = Mem.copy(ArrayRef<int>(VR.Index));
      SelInfo.ParentSteps = VR.ParentSteps;
      SelInfo.Indirect = VR.Indirect;
      recordSelection(Ex, SelInfo);
      recordUse(Ex->Sel, F);
      X.M = Mode::Variable;
      X.Ty = viewRead(F->Ty);
      X.E = Ex;
      return;
    }
    if (VR.Obj) {
      err(Ex->Sel, diag::err_view_no_methods, {str(Ex), str(X.Ty)});
      return Error();
    }
    err(Ex->Sel, diag::err_undefined_field_or_method,
        {str(Ex->X), std::string(Sel),
         "type " + str(X.Ty) + " has no field or method " + std::string(Sel)});
    return Error();
  }
  if (own() && X.M != Mode::TypeExpr) {
    auto *Op = dyn_cast_or_null<OptionType>(under(X.Ty));
    // A defined type over an Option keeps its own methods.
    if (Op && isa<NamedType>(X.Ty) &&
        lookupFieldOrMethod(X.Ty, Addressable, S.Pkg, Sel).Obj)
      Op = nullptr;
    if (Op) {
      // An Option has exactly two methods, IsNil and Get (ADR-0018 point 3).
      if (Sel == "IsNil") {
        Type *Res[] = {typ(BasicKind::Bool)};
        Synth(syntheticMethod("IsNil", Res, Ex->Sel->getBegin()));
        return;
      }
      if (Sel == "Get") {
        Type *Res[] = {Op->Elem, typ(BasicKind::Bool)};
        Synth(syntheticMethod("Get", Res, Ex->Sel->getBegin()));
        return;
      }
      // The value's own fields and methods are reached only after a check.
      if (isValid(Op->Elem) && valueHasMember(Op->Elem, Sel)) {
        err(Ex->X, diag::err_option_maybe_nil,
            {str(Ex->X), nilCheckHint(Ex->X)});
        return Error();
      }
      err(Ex->Sel, diag::err_undefined_field_or_method,
          {str(Ex->X), std::string(Sel),
           "type " + str(X.Ty) + " has no field or method " + std::string(Sel) +
               " (an Option has IsNil and Get only)"});
      return Error();
    }
    PtrKind PK;
    Type *PElem;
    if (hasPtrKind(X.Ty, &PK, &PElem)) {
      if (PK == PtrKind::Weak) {
        // ADR-0017: only Lock is found on a Weak pointer.
        if (Sel == "Lock") {
          Object *SharedObj = Ctx.universe()->lookupLocal("Shared");
          Type *PtrT = Ctx.pointer(PElem);
          Type *SharedT = instantiateNamed(cast<NamedType>(SharedObj->Ty),
                                           ArrayRef<Type *>(&PtrT, 1));
          Type *Res[] = {Ctx.option(SharedT)};
          Synth(syntheticMethod("Lock", Res, Ex->Sel->getBegin()));
          return;
        }
        LookupResult Inner =
            lookupFieldOrMethod(Ctx.pointer(PElem), true, S.Pkg, Sel);
        if (Inner.Obj || Inner.Ambiguous) {
          Err Er = err(Ex->Sel, diag::err_weak_no_deref,
                       {str(Ex->X), std::string(Sel)});
          Er.note(Ex->Sel->getBegin(), "fix-it: if c := " + str(Ex->X) +
                                           ".Lock(); c != nil { c." +
                                           std::string(Sel) + " }");
          return Error();
        }
        err(Ex->Sel, diag::err_undefined_field_or_method,
            {str(Ex->X), std::string(Sel),
             "type " + str(X.Ty) + " has no field or method " +
                 std::string(Sel) + " (a Weak pointer has Lock only)"});
        return Error();
      }
      if (Sel == "Clone") {
        if (PK == PtrKind::Unique) {
          // ADR-0023 decision 16: for a tagged target, a full copy of the
          // dynamic type (an abstract target is fine: the child is copied).
          std::string Why;
          if (!satisfiesClone(PElem, &Why, /*ByValue=*/!isTagged(PElem))) {
            err(Ex->Sel, diag::err_own_clone_not_cloneable,
                {str(Ex->X), str(PElem), Why.empty() ? "" : ": " + Why});
            return Error();
          }
        }
        Type *Res[] = {X.Ty};
        Synth(syntheticMethod("Clone", Res, Ex->Sel->getBegin()));
        return;
      }
      LookupT = Ctx.pointer(PElem);
      Addressable = true;
    }
  }

  // A selector synthesized for an operator call names the method itself:
  // an overload set shares the name (ADR-0024).
  const Func *WantOp = nullptr;
  if (!OpSelect.empty())
    if (auto It = OpSelect.find(Ex); It != OpSelect.end())
      WantOp = It->second;
  LookupResult LR =
      lookupFieldOrMethod(LookupT, Addressable, S.Pkg, Sel, false, WantOp);
  Object *Obj = LR.Obj;
  if (Obj && !Obj->Ty)
    objDecl(Obj, nullptr);
  if (Obj && isa<Func>(Obj))
    addDeclDep(Obj);

  // Go++ (OWN-R6): `x.Clone()` exists for every cloneable type.
  if (own() && Sel == "Clone" && X.M != Mode::TypeExpr) {
    bool Declared = Obj && isa<Func>(Obj);
    // ADR-0023 decision 16: a clone never loses the child part. A place that
    // may hold only a parent part is not cloned as a value, and a tagged
    // type reached through a pointer or a view clones through its table into
    // a Unique (this replaces the ambiguity and the pointer copy for these
    // types; a Clone declared on the pointer type itself is still called).
    if (rejectPartialClone(Ex->X, X.Ty, Ex))
      return Error();
    // `(*p).Clone()` for a p pointing to a type parameter T would clone only
    // the T part of a child: T must be untagged (R1). (`p.Clone()` is the
    // clone of the pointer itself.)
    if (isa<StarExpr>(unparen(Ex->X)))
      noteUse(X.Ty, {TypeParamType::Use::Clone});
    if (Type *Through = cloneThroughTable(X.Ty);
        Through && !(Declared && cast<Func>(Obj)->HasPtrRecv)) {
      std::string Why;
      if (!satisfiesClone(Through, &Why, /*ByValue=*/false)) {
        err(Ex->Sel, diag::err_own_clone_not_cloneable,
            {str(Ex->X), str(Through), Why.empty() ? "" : ": " + Why});
        return Error();
      }
      Type *Res[] = {uniqueOf(Through)};
      Func *B = syntheticMethod("Clone", Res, Ex->Sel->getBegin());
      Selection SelInfo;
      SelInfo.K = Selection::MethodVal;
      SelInfo.Recv = X.Ty;
      SelInfo.Obj = B;
      recordSelection(Ex, SelInfo);
      X.M = Mode::Value;
      X.Ty = B->Ty;
      X.E = Ex;
      return;
    }
    if (Declared) {
      // A raw *T whose T has a value-receiver Clone: ambiguous (OWN-R6).
      if (isa<PointerType>(X.Ty) && !cast<Func>(Obj)->HasPtrRecv) {
        auto *Fn = cast<Func>(Obj);
        objDecl(Fn, nullptr);
        err(Ex->Sel, diag::err_own_clone_ambiguous,
            {str(Ex), str(X.Ty), str(cast<PointerType>(X.Ty)->Elem), "Clone"})
            .note(Ex->Sel->getBegin(), "write (*" + str(Ex->X) +
                                           ").Clone() to copy the T, or q := " +
                                           str(Ex->X) + " to copy the pointer");
        return Error();
      }
    } else if (!Obj && !LR.Ambiguous && !LR.Indirect) {
      std::string Why;
      if (!satisfiesClone(X.Ty, &Why)) {
        err(Ex->Sel, diag::err_own_clone_not_cloneable,
            {str(Ex->X), str(X.Ty), Why.empty() ? "" : ": " + Why});
        return Error();
      }
      // A clone of a borrowed slice made by slicing is an owned slice
      // (OWN-V2).
      Type *Res[] = {X.Ty};
      if (auto *Rf = dyn_cast<RefType>(X.Ty); Rf && Rf->Borrowed)
        Res[0] = Rf->Elem;
      Func *B = syntheticMethod("Clone", Res, Ex->Sel->getBegin());
      Selection SelInfo;
      SelInfo.K = Selection::MethodVal;
      SelInfo.Recv = X.Ty;
      SelInfo.Obj = B;
      recordSelection(Ex, SelInfo);
      X.M = Mode::Value;
      X.Ty = B->Ty;
      X.E = Ex;
      return;
    }
  }
  if (!Obj) {
    // An interface type still being declared has no methods yet: `T.m` in
    // its own method signatures is "undefined" (types2).
    bool Building = false;
    {
      bool IsPtr;
      if (auto *N = dyn_cast<NamedType>(deref(X.Ty, IsPtr)))
        Building = N->ResolvingInterface;
    }
    if (!isValid(under(X.Ty)) && !Building)
      return Error();
    if (LR.Ambiguous) {
      err(Ex->Sel, diag::err_ambiguous_selector,
          {str(Ex->X) + "." + std::string(Sel)});
      return Error();
    }
    if (LR.Indirect) {
      if (X.M == Mode::TypeExpr)
        err(Ex->Sel, diag::err_method_expr_ptr_recv,
            {str(X.Ty) + "." + std::string(Sel), str(X.Ty), std::string(Sel)});
      else
        err(Ex->Sel, diag::err_sema_call,
            {"cannot call pointer method " + std::string(Sel) + " on " +
             str(X.Ty)});
      return Error();
    }
    std::string Why;
    if (isInterfacePtr(X.Ty)) {
      Type *PE = cast<PointerType>(under(X.Ty))->Elem;
      bool TP = isTypeParam(PE);
      Why = "type " + str(X.Ty) + " is pointer to " +
            (TP ? "type parameter, not type parameter"
                : "interface, not interface");
      // GEN-12: `T extends P` sees the fields and methods of P only.
      if (TP && !S.Opts.GoCompat)
        if (auto *BI = dyn_cast_or_null<InterfaceType>(under(PE)))
          if (Type *Ext = typeSet(BI)->Extends)
            Why = "type " + str(PE) + " extends " + str(Ext) +
                  ", so it has only the fields and methods of " + str(Ext) +
                  " (GEN-12)";
    } else {
      LookupResult Alt =
          lookupFieldOrMethod(LookupT, Addressable, S.Pkg, Sel, true);
      Why = lookupError(X.Ty, Sel, Alt.Obj);
    }
    err(Ex->Sel, diag::err_undefined_field_or_method,
        {str(Ex->X), std::string(Sel), Why});
    return Error();
  }

  if (auto *M = dyn_cast<Func>(Obj)) {
    objDecl(M, nullptr);
    // OWN-R7: drop glue calls `drop`; code cannot (like `init`).
    if (own() && M->Recv && M->Name == "drop") {
      err(Ex->Sel, diag::err_own_drop_called, {str(Ex)});
      return Error();
    }
  }

  if (X.M == Mode::TypeExpr) {
    auto *M = dyn_cast<Func>(Obj);
    if (!M) {
      err(Ex->Sel, diag::err_undefined_field_or_method,
          {str(Ex->X), std::string(Sel),
           "type " + str(X.Ty) + " has no method " + std::string(Sel)});
      return Error();
    }
    Selection SelInfo;
    SelInfo.K = Selection::MethodExpr;
    SelInfo.Recv = X.Ty;
    SelInfo.Obj = M;
    SelInfo.Index = Mem.copy(ArrayRef<int>(LR.Index));
    SelInfo.ParentSteps = LR.ParentSteps;
    SelInfo.Indirect = LR.Indirect;
    recordSelection(Ex, SelInfo);
    // TYP-19: the receiver becomes the first parameter, a value of the type
    // (a pointer type, `(*T).M`, is no value of it).
    noAbstract(Ex->X, X.Ty, "the receiver parameter of a method expression");
    auto *Sig = dyn_cast_or_null<SignatureType>(M->Ty);
    if (!Sig) {
      err(Ex, diag::err_sema_call, {"illegal cycle in method declaration"});
      return Error();
    }
    SmallVector<Type *, 4> Params;
    Params.push_back(X.Ty);
    Params.append(Sig->Params->Elems.begin(), Sig->Params->Elems.end());
    uint64_t Mask = Sig->MoveMask << 1;
    X.M = Mode::Value;
    X.Ty = Ctx.signature(Ctx.tuple(Params), Sig->Results, Sig->Variadic,
                         Sig->Fails, Mask, Sig->TParams);
    SelInfo.Ty = X.Ty;
  } else {
    if (auto *V = dyn_cast<Var>(Obj)) {
      // EXP-15: the parent part as a whole is no value (partialUse).
      if (V->IsParent && !S.Opts.GoCompat) {
        bool IsPtr;
        if (!partialUse(Ex, PartialKind::ParentPart, V->Ty, deref(X.Ty, IsPtr),
                        SelfMark))
          return Error();
      }
      Selection SelInfo;
      SelInfo.K = Selection::FieldVal;
      SelInfo.Recv = X.Ty;
      SelInfo.Obj = V;
      SelInfo.Index = Mem.copy(ArrayRef<int>(LR.Index));
      SelInfo.ParentSteps = LR.ParentSteps;
      SelInfo.Indirect = LR.Indirect;
      recordSelection(Ex, SelInfo);
      if (X.M == Mode::Variable || LR.Indirect)
        X.M = Mode::Variable;
      else
        X.M = Mode::Value;
      X.Ty = V->Ty;
      recordUse(Ex->Sel, V);
    } else {
      auto *F = cast<Func>(Obj);
      Selection SelInfo;
      SelInfo.K = Selection::MethodVal;
      SelInfo.Recv = X.Ty;
      SelInfo.Obj = F;
      SelInfo.Index = Mem.copy(ArrayRef<int>(LR.Index));
      SelInfo.ParentSteps = LR.ParentSteps;
      SelInfo.Indirect = LR.Indirect;
      // TYP-20: `base.M` calls the parent's version of M, statically.
      if (auto *BV = dyn_cast_or_null<Var>(identObj(Ex->X)); BV && BV->IsBase) {
        SelInfo.Base = true;
        if (markOf(F) == Func::Mark::Abstract) {
          err(Ex->Sel, diag::err_base_abstract,
              {std::string(Sel), str(cast<PointerType>(BV->Ty)->Elem)});
          return Error();
        }
      }
      recordSelection(Ex, SelInfo);
      recordUse(Ex->Sel, F);
      if (own() && F->HasPtrRecv) {
        // RNG-6b: a pointer method writes the place it is called on, also
        // through the pointer that is ranged over (`p.Inc()` in `range p`).
        checkUserRangeWrite(Ex->X, "call the pointer method " +
                                       std::string(F->Name) + " on");
        if (!LR.Indirect)
          checkPtrRecvCall(Ex, F);
      }
      // A pointer-receiver method value, or a `go` / `defer` call, on a
      // checked Option binds &o and outlives the statement: `o = nil` could
      // then empty the payload under it. It is an implicit `&o`, so o is
      // not narrowed there (an immediate call `o.M()` is fine). A method of
      // Option[*T] has an indirect receiver and copies the pointer.
      if (own() && F->HasPtrRecv && !LR.Indirect &&
          (Ex != CalleeSel || CalleeSuspended)) {
        rejectPayloadAddress(Ex->X, "a method value or `go` / `defer` call "
                                    "with a pointer receiver");
        noteReference(Ex->X);
      }
      // ONE decision for every route that takes the receiver itself: a
      // method value, a `go` / `defer` call (the receiver is evaluated and
      // copied) and a `move` receiver (R6, R7, R18).
      if (own() && !receiverTakenOk(Ex, F, ownerStep(X.Ty, LR), X.Ty,
                                    Ex != CalleeSel || CalleeSuspended))
        return Error();
      // A method value (not the callee of a call) binds, so moves, the
      // receiver; a call is checked in arguments().
      if (own() && F->MoveRecv && Ex != CalleeSel)
        checkMoveRecvUse(Ex, "bind a `move` method value to");
      X.M = Mode::Value;
      X.Ty = F->Ty;
    }
  }
  X.E = Ex;
}

std::string Checker::lookupError(Type *T, StringRef Sel, Object *Alt,
                                 bool StructLit) {
  // A port of types2's lookupError (errsupport.go): the message depends on
  // whether Alt (the object found with case folding) is in this package and
  // how its spelling differs from Sel.
  enum { Missing, Misspelled, Unexported, Inaccessible } E = Missing;
  // types2's tail: the name without its first UTF-8 character; a name of one
  // character is returned whole (so `g` and `G` are no misspelling).
  auto Tail = [](StringRef S) {
    for (size_t I = 1; I < S.size(); ++I)
      if ((static_cast<unsigned char>(S[I]) & 0xC0) != 0x80)
        return S.substr(I);
    return S;
  };
  StringRef AltName = Alt ? StringRef(Alt->Name) : StringRef();
  if (Alt) {
    // The package of an exported struct field is not stored (see sameId):
    // the fields of a defined type belong to the package of that type.
    Package *AltPkg = Alt->Pkg;
    if (!AltPkg) {
      bool IsPtr;
      Type *Base = deref(T, IsPtr);
      AltPkg = isa<NamedType>(Base) && cast<NamedType>(Base)->Obj
                   ? cast<NamedType>(Base)->Obj->Pkg
                   : S.Pkg;
    }
    if (AltPkg == S.Pkg) {
      E = Misspelled;
    } else if (isExportedName(Sel)) {
      if (isExportedName(AltName))
        E = Misspelled;
      else if (Tail(Sel) == Tail(AltName))
        E = Unexported;
    } else if (isExportedName(AltName)) {
      if (Tail(Sel) == Tail(AltName))
        E = Misspelled;
    } else if (Sel == AltName) {
      E = Inaccessible;
    }
  }
  std::string TS = str(T), S = Sel.str(), A = AltName.str();
  if (StructLit) {
    std::string Base =
        "unknown field " + S + " in struct literal of type " + TS;
    switch (E) {
    case Missing:
      return Base;
    case Misspelled:
      return Base + ", but does have " + A;
    case Unexported:
      return Base + ", but does have unexported " + A;
    case Inaccessible:
      return "cannot refer to unexported field " + A +
             " in struct literal of type " + TS;
    }
  }
  const char *What = !Alt ? "object" : isa<Func>(Alt) ? "method" : "field";
  std::string Base = "type " + TS + " has no field or method " + S;
  switch (E) {
  case Missing:
    return Base;
  case Misspelled:
    return Base + ", but does have " + What + " " + A;
  case Unexported:
    return Base + ", but does have unexported " + What + " " + A;
  case Inaccessible:
    return "cannot refer to unexported " + std::string(What) + " " + A;
  }
  return Base;
}

//===----------------------------------------------------------------------===//
// Instantiation of generic functions
//===----------------------------------------------------------------------===//

std::string Checker::typesSummary(ArrayRef<Type *> List, bool Variadic,
                                  bool HasDots) {
  std::string Res = "(";
  for (size_t I = 0; I < List.size(); ++I) {
    Type *T = List[I];
    std::string S;
    if (!T || !isValid(T))
      S = "unknown type";
    else if (isUntyped(T)) {
      if (isNumeric(T))
        S = "number";
      else {
        S = cast<BasicType>(T)->Name;
        if (StringRef(S).starts_with("untyped "))
          S = S.substr(8);
      }
    } else {
      S = str(T);
    }
    if (I + 1 == List.size()) {
      if (Variadic) {
        if (auto *Sl = dyn_cast<SliceType>(T))
          S = "..." + str(Sl->Elem);
        else
          S = "..." + S;
      } else if (HasDots) {
        S += "...";
      }
    }
    if (I)
      Res += ", ";
    Res += S;
  }
  return Res + ")";
}

Type *Checker::instantiateSignature(SourceLoc Pos, SignatureType *Sig,
                                    ArrayRef<Type *> TArgs,
                                    ArrayRef<SourceLoc> ArgPos,
                                    const ast::Expr *Ex) {
  SignatureType *Inst = instantiateFuncSig(Sig, TArgs);
  if (Ex)
    recordInstance(Ex, TArgs, Inst);
  recordMono(Pos, nullptr, Sig->TParams, TArgs);
  SmallVector<Type *, 4> TA(TArgs.begin(), TArgs.end());
  SmallVector<SourceLoc, 4> AP(ArgPos.begin(), ArgPos.end());
  SmallVector<TypeParamType *, 4> TP(Sig->TParams.begin(), Sig->TParams.end());
  later([this, Pos, TP, TA, AP, Inst]() {
    std::string Cause;
    int Bad = verify(Pos, TP, TA, &Cause);
    if (Bad >= 0) {
      SourceLoc At = Pos;
      if (static_cast<size_t>(Bad) < AP.size() && AP[Bad].isValid())
        At = AP[Bad];
      err(At, diag::err_sema_call, {Cause});
    } else {
      checkOptionArgs(Pos, Inst, TA);
      noAbstractArgs(Pos, Inst, TA); // TYP-19, on the substituted signature
    }
  });
  return Inst;
}

void Checker::funcInst(Operand &X, SourceLoc Pos, const IndexExpr *Inst,
                       bool Infer, Type *Hint) {
  if (Inst)
    X.E = Inst;
  SmallVector<Type *, 4> TArgs;
  SmallVector<SourceLoc, 4> ArgPos;
  if (Inst) {
    if (!typeList(Inst->Indices, TArgs)) {
      X.M = Mode::Invalid;
      return;
    }
    for (const ast::Expr *Ex : Inst->Indices)
      ArgPos.push_back(Ex->getBegin());
  }
  auto *Sig = cast<SignatureType>(X.Ty);
  size_t Got = TArgs.size(), Want = Sig->TParams.size();
  if (Got > Want) {
    err(Inst->Indices[Got - 1], diag::err_got_n_type_args_func,
        {num(Got), str(X.E), num(Want)});
    X.M = Mode::Invalid;
    return;
  }
  if (Got < Want) {
    if (!Infer) {
      // The remaining type arguments are inferred by the caller.
      X.PartialTArgs = Mem.copy(ArrayRef<Type *>(TArgs));
      return;
    }
    // Reverse inference (types2 funcInst): an uninstantiated function used
    // where a function type is expected is treated like the call g(v) of the
    // synthetic generic function g(func_type_of_x) with v of the target type.
    SmallVector<Operand, 4> Args;
    SmallVector<TypeParamType *, 4> TPs;
    TupleType *Params = Ctx.tuple({});
    bool Reverse = false;
    auto *Target = Hint ? dyn_cast<SignatureType>(under(Hint)) : nullptr;
    if (Target && Target->TParams.empty()) {
      SignatureType *G = Ctx.signature(Sig->Params, Sig->Results, Sig->Variadic,
                                       Sig->Fails, Sig->MoveMask);
      Params = Ctx.tuple({G});
      Operand A;
      A.M = Mode::Value;
      A.E = X.E;
      A.Ty = Target;
      Args.push_back(A);
      Reverse = true;
    }
    TupleType *Params2 = nullptr;
    renameTParams(Sig->TParams, Params, TPs, &Params2);
    InferError IE;
    SmallVector<Type *, 4> Inferred =
        infer(Pos, TPs, TArgs, Params2, Args, Reverse, &IE);
    if (Inferred.empty()) {
      if (!IE.Msg.empty())
        err(IE.Pos, diag::err_sema_call, {IE.Msg});
      X.M = Mode::Invalid;
      return;
    }
    TArgs.assign(Inferred.begin(), Inferred.end());
  }
  X.Ty = instantiateSignature(X.pos(), Sig, TArgs, ArgPos, X.E);
  X.M = Mode::Value;
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

/// `h(f())` / `h(o.Get())` hands the values of a checked form on, with no way
/// to check the error or ok first (P03.18): the parameters are plain
/// variables of the callee.
void Checker::checkSpreadCall(const ast::Expr *Ex, ArrayRef<Operand> Ops) {
  if (own() && Ops.size() >= 2)
    checkForwarded(Ex, Ops, nullptr, "the arguments of the call");
}

void Checker::exprList(ArrayRef<ast::Expr *> Es, SmallVector<Operand, 4> &Out) {
  if (Es.size() == 1) {
    bool CommaOk;
    multiExpr(Out, Es[0], false, &CommaOk);
    checkSpreadCall(Es[0], Out);
    return;
  }
  for (const ast::Expr *Ex : Es) {
    Operand X;
    expr(X, Ex);
    Out.push_back(X);
  }
}

void Checker::genericExprList(ArrayRef<ast::Expr *> Es,
                              SmallVector<Operand, 4> &Out) {
  // A generic function may be partially instantiated (`f[int]`): its type
  // arguments are kept in the operand and the rest is inferred by the call.
  auto Eval = [&](Operand &X, const ast::Expr *Ex) {
    // An operand the operator code evaluated already goes through rawExpr,
    // which hands it back: no operand is checked twice (ADR-0024). Every
    // other evaluation path (exprList, multiExpr, exprOrType, use) reaches
    // rawExpr; this one and the callee of callExpr are the only direct
    // indexExpr calls, and an operator call has a selector as its callee.
    auto *IE = dyn_cast<IndexExpr>(Ex);
    if (IE && !PreEval.count(Ex)) {
      if (indexExpr(X, IE)) {
        funcInst(X, IE->getBegin(), IE, false, nullptr);
        if (X.M != Mode::Invalid)
          recordTypeAndValue(IE, X.M, X.Ty, nullptr);
      }
      if (X.M != Mode::Invalid)
        X.E = IE; // the element, not the container, is the argument
      return;
    }
    rawExpr(X, Ex, nullptr, true);
  };
  if (Es.size() == 1) {
    const ast::Expr *Ex = Es[0];
    Operand X;
    Eval(X, Ex);
    exclude(X, (1u << static_cast<unsigned>(Mode::NoValue)) |
                   (1u << static_cast<unsigned>(Mode::Builtin)) |
                   (1u << static_cast<unsigned>(Mode::TypeExpr)));
    if (X.M != Mode::Invalid)
      if (auto *T = dyn_cast<TupleType>(X.Ty)) {
        for (Type *Et : T->Elems) {
          Operand O;
          O.M = Mode::Value;
          O.E = Ex;
          O.Ty = Et;
          Out.push_back(O);
        }
        checkSpreadCall(Ex, Out);
        return;
      }
    Out.push_back(X);
    return;
  }
  for (const ast::Expr *Ex : Es) {
    Operand X;
    Eval(X, Ex);
    if (!isa<IndexExpr>(Ex))
      exclude(X, (1u << static_cast<unsigned>(Mode::NoValue)) |
                     (1u << static_cast<unsigned>(Mode::Builtin)) |
                     (1u << static_cast<unsigned>(Mode::TypeExpr)));
    Out.push_back(X);
  }
}

void Checker::renameTParams(ArrayRef<TypeParamType *> TPs, TupleType *Params,
                            SmallVector<TypeParamType *, 4> &NewTPs,
                            TupleType **NewParams) {
  llvm::DenseMap<const Type *, Type *> Map;
  for (TypeParamType *P : TPs) {
    TypeName *TN = newTypeName(P->Obj->Name, nullptr, P->Obj->Pos);
    TypeParamType *N = Ctx.newTypeParam(TN, P->Index, nullptr);
    N->RenamedFrom = P->RenamedFrom ? P->RenamedFrom : P;
    NewTPs.push_back(N);
    Map[P] = N;
  }
  for (size_t K = 0; K < TPs.size(); ++K)
    NewTPs[K]->Bound =
        TPs[K]->Bound ? substImpl(TPs[K]->Bound, Map, 0) : nullptr;
  if (NewParams)
    *NewParams = cast<TupleType>(substImpl(Params, Map, 0));
}

std::string Checker::tupleString(TupleType *T, bool Variadic) {
  return typesSummary(T->Elems, Variadic, false);
}

bool Checker::enclosingCanFail() const {
  if (!E.Sig)
    return false;
  if (E.Sig->Fails)
    return true;
  const auto &R = E.Sig->Results->Elems;
  return !R.empty() && R.back() == Ctx.errorType();
}

/// `pkg.Name[args]` where Name is a C++ template of a foreign package
/// (ADR-0010 Q3), or null.
Object *Checker::foreignTemplateOf(const IndexExpr *IE) {
  const ast::Expr *Fun = IE->X;
  while (auto *P = dyn_cast<ParenExpr>(Fun))
    Fun = P->X;
  auto *Sel = dyn_cast<SelectorExpr>(Fun);
  if (!Sel)
    return nullptr;
  Package *P = qualifierPackage(Sel->X);
  if (!P || !P->Foreign)
    return nullptr;
  Object *O = P->Foreign->lookup(name(Sel->Sel)).Obj;
  if (!O || !O->IsTemplate)
    return nullptr;
  recordUse(Sel->Sel, O);
  return O;
}

bool Checker::foreignTemplateArgs(ArrayRef<ast::Expr *> Idx,
                                  SmallVector<ForeignTemplateArg, 4> &Out) {
  bool Ok = true;
  for (const ast::Expr *A : Idx) {
    Operand Z;
    exprOrType(Z, A, false);
    if (Z.M == Mode::TypeExpr) {
      Out.push_back({Z.Ty, nullptr});
    } else if (Z.M == Mode::Constant) {
      Out.push_back({Z.Ty, Mem.constant(Z.Val)});
    } else {
      if (Z.M != Mode::Invalid)
        err(A, diag::err_foreign_template_arg, {str(A)});
      Ok = false;
    }
  }
  return Ok;
}

Type *Checker::foreignInstantiate(const IndexExpr *IE) {
  Object *T = foreignTemplateOf(IE);
  if (!T || !isa<TypeName>(T))
    return nullptr;
  SmallVector<ForeignTemplateArg, 4> Args;
  if (!foreignTemplateArgs(IE->Indices, Args))
    return invalid();
  ForeignInstance R = T->Pkg->Foreign->instantiate(T, Args);
  if (!R.Obj || !R.Obj->Ty) {
    err(IE, diag::err_foreign_instantiate, {str(IE), R.Error});
    return invalid();
  }
  return R.Obj->Ty;
}

/// The foreign function or method a call goes to, and the receiver
/// expression of a selector call.
Object *Checker::foreignCallee(const ast::Expr *Fun, const ast::Expr *&Recv) {
  auto *Sel = dyn_cast<SelectorExpr>(unparen(Fun));
  if (!Sel)
    return nullptr;
  Object *O = nullptr;
  auto It = I.Selections.find(Sel);
  if (It != I.Selections.end() && It->second.K == Selection::MethodVal) {
    O = It->second.Obj;
  } else {
    auto U = I.Uses.find(Sel->Sel);
    O = U == I.Uses.end() ? nullptr : U->second;
  }
  if (!O || !isa<Func>(O) || !O->Pkg || !O->Pkg->Foreign)
    return nullptr;
  Recv = Sel->X;
  return O;
}

ExprKind Checker::callExpr(Operand &X, const CallExpr *Call) {
  const IndexExpr *Inst = nullptr;
  Object *FTmpl = nullptr; // a C++ function template called as f[T](x)
  SmallVector<ForeignTemplateArg, 4> FTArgs;
  auto *CalleeIE = dyn_cast<IndexExpr>(Call->Fun);
  if (CalleeIE && (FTmpl = foreignTemplateOf(CalleeIE)) && isa<Func>(FTmpl)) {
    X.M = foreignTemplateArgs(CalleeIE->Indices, FTArgs) ? Mode::Value
                                                         : Mode::Invalid;
    X.Ty = FTmpl->Ty;
    X.E = CalleeIE;
    if (X.M == Mode::Value)
      recordTypeAndValue(CalleeIE, X.M, X.Ty, nullptr);
  } else if (auto *IE = CalleeIE) {
    FTmpl = nullptr;
    if (indexExpr(X, IE))
      Inst = IE;
    X.E = IE;
    // record
    if (X.M != Mode::Invalid && !Inst)
      recordTypeAndValue(IE, X.M, X.Ty, nullptr);
  } else {
    const ast::Expr *SavedCallee = CalleeSel;
    CalleeSel = Call->Fun;
    while (auto *Pa = dyn_cast<ParenExpr>(CalleeSel))
      CalleeSel = Pa->X;
    bool SavedSusp = CalleeSuspended;
    CalleeSuspended = Call == GoCall || Call == DeferCall;
    // ADR-0024: the callee may be the receiver of a `()` method; a partial
    // place is decided with the method (opReceiverOk), and a struct is no
    // function anyway.
    if (!S.Opts.GoCompat && Ctx.operatorsSeen())
      markPartialOk(Call->Fun, /*Addr=*/false);
    exprOrType(X, Call->Fun, true);
    CalleeSuspended = SavedSusp;
    CalleeSel = SavedCallee;
  }

  auto UseArgs = [&]() {
    for (const ast::Expr *A : Call->Args)
      use(A);
  };
  // `mem.Replace(&c.Owner, v)` may take the address of a never-nil field
  // (ADR-0016 point 8), when v is not nil.
  struct ReplaceRestore {
    Checker &C;
    const ast::Expr *Arg, *Nil;
    const Var *Field;
    std::string Place;
    ~ReplaceRestore() {
      C.ReplaceArg = Arg;
      C.ReplaceNil = Nil;
      C.ReplaceField = Field;
      C.ReplacePlace = std::move(Place);
    }
  } ReplaceGuard{*this, ReplaceArg, ReplaceNil, ReplaceField, ReplacePlace};
  ReplaceField = nullptr; // set by checkAddressOf for this call's `&place`
  if (own() && !Call->Args.empty() && isMemReplace(Call->Fun)) {
    ReplaceArg = unparen(Call->Args[0]);
    ReplaceNil = Call->Args.size() == 2 && isNilLit(Call->Args[1])
                     ? Call->Args[1]
                     : nullptr;
  }

  switch (X.M) {
  case Mode::Invalid:
    UseArgs();
    X.E = Call;
    return ExprKind::Statement;
  case Mode::TypeExpr: {
    if (isGeneric(X.Ty)) {
      err(Call->Fun, diag::err_generic_without_instantiation,
          {"type", str(Call->Fun), ""});
      X.M = Mode::Invalid;
      return ExprKind::Conversion;
    }
    Type *T = X.Ty;
    X.M = Mode::Invalid;
    switch (Call->Args.size()) {
    case 0:
      err(Call, diag::err_sema_call,
          {"missing argument in conversion to " + str(T)});
      break;
    case 1: {
      expr(X, Call->Args[0]);
      if (X.M != Mode::Invalid) {
        if (auto *It = dyn_cast<InterfaceType>(under(T));
            It && !isTypeParam(T)) {
          TypeSet *TS = typeSet(It);
          if (TS->hasTerms() || TS->Comparable) {
            err(Call, diag::err_sema_call,
                {"cannot use interface " + str(T) +
                 " in conversion (contains specific type constraints or is "
                 "comparable)"});
            break;
          }
        }
        if (Call->EllipsisLoc.isValid()) {
          err(Call->Args[0], diag::err_sema_call,
              {"invalid use of ... in conversion to " + str(T)});
          break;
        }
        // ADR-0024, EXP-25: `float64(b)` through an @Convert method.
        bool ViaMethod = false;
        if (!S.Opts.GoCompat && Ctx.operatorsSeen() &&
            opConvert(X, T, Call, ViaMethod) && ViaMethod)
          break;
        conversion(X, T);
      }
      break;
    }
    default:
      UseArgs();
      err(Call->Args.back(), diag::err_sema_call,
          {"too many arguments in conversion to " + str(T)});
    }
    if (Call->isMust() && X.M != Mode::Invalid) {
      err(Call, diag::err_bang_on_conversion, {str(Call->Fun)});
      X.M = Mode::Invalid;
    }
    X.E = Call;
    return ExprKind::Conversion;
  }
  case Mode::Builtin: {
    BuiltinId Id = X.BId;
    if (Call->isMust()) {
      err(Call, diag::err_bang_on_conversion, {str(Call->Fun)});
      UseArgs();
      X.M = Mode::Invalid;
      X.E = Call;
      return ExprKind::Statement;
    }
    if (!builtin(X, Call, Id))
      X.M = Mode::Invalid;
    X.E = Call;
    if (X.M != Mode::Invalid && X.M != Mode::Constant)
      E.HasCallOrRecv = true;
    switch (Id) {
    case BuiltinId::Move:
      // `move(m[k])` on its own removes the entry and drops the value
      // (ADR-0016 point 9); any other move must use its result.
      return X.M == Mode::CommaOk ? ExprKind::Statement : ExprKind::Expression;
    case BuiltinId::Append:
    case BuiltinId::Cap:
    case BuiltinId::Complex:
    case BuiltinId::Imag:
    case BuiltinId::Len:
    case BuiltinId::Make:
    case BuiltinId::Max:
    case BuiltinId::Min:
    case BuiltinId::New:
    case BuiltinId::Real:
    case BuiltinId::Clone:
    case BuiltinId::NewUnique:
    case BuiltinId::NewShared:
    case BuiltinId::NewWeak:
    case BuiltinId::Alignof:
    case BuiltinId::Offsetof:
    case BuiltinId::Sizeof:
    case BuiltinId::Add:
    case BuiltinId::Slice:
    case BuiltinId::SliceData:
    case BuiltinId::String:
    case BuiltinId::StringData:
      return ExprKind::Expression;
    default:
      return ExprKind::Statement;
    }
  }
  default:
    break;
  }

  // ADR-0024, EXP-23: `a(x)` on a type with `()` methods (also through a
  // pointer, like the method call `p.M()`).
  if (!S.Opts.GoCompat && Ctx.operatorsSeen() && X.M != Mode::Invalid &&
      X.M != Mode::TypeExpr && opCandidateType(X.Ty, /*ThroughPtr=*/true)) {
    bool Handled = false;
    if (opCallExpr(X, Call, Handled) && Handled) {
      X.E = Call;
      return ExprKind::Statement;
    }
  }
  // Ordinary function or method call.
  std::string CallWhy;
  auto *Sig = dyn_cast_or_null<SignatureType>(commonUnder(
      X.Ty,
      [&](Type *T, Type *U) -> std::string {
        if (U && !isa<SignatureType>(U))
          return str(T) + " is not a function";
        return "";
      },
      &CallWhy));
  if (!Sig) {
    // `f()` on an Option[func()] that is not known to be set (OWN-P10).
    if (own() && X.E)
      if (auto *Op = dyn_cast_or_null<OptionType>(under(X.Ty)))
        if (isa<SignatureType>(under(Op->Elem))) {
          err(X, diag::err_option_maybe_nil, {str(X.E), nilCheckHint(X.E)});
          UseArgs();
          X.M = Mode::Invalid;
          X.E = Call;
          return ExprKind::Statement;
        }
    err(X, diag::err_sema_call,
        {"invalid operation: cannot call " + str(X) + ": " + CallWhy});
    UseArgs();
    X.M = Mode::Invalid;
    X.E = Call;
    return ExprKind::Statement;
  }
  bool WasGeneric = !Sig->TParams.empty();
  SmallVector<Type *, 4> TArgs;
  SmallVector<SourceLoc, 4> ArgPos;
  if (Inst) {
    if (!typeList(Inst->Indices, TArgs)) {
      UseArgs();
      X.M = Mode::Invalid;
      X.E = Call;
      return ExprKind::Statement;
    }
    for (const ast::Expr *Ex : Inst->Indices)
      ArgPos.push_back(Ex->getBegin());
    size_t Got = TArgs.size(), Want = Sig->TParams.size();
    if (Got > Want) {
      err(Inst->Indices[Want], diag::err_got_n_type_args_func,
          {num(Got), str(Call->Fun), num(Want)});
      UseArgs();
      X.M = Mode::Invalid;
      X.E = Call;
      return ExprKind::Statement;
    }
    if (Got == Want && Want > 0) {
      Sig = cast<SignatureType>(
          instantiateSignature(Inst->getBegin(), Sig, TArgs, ArgPos, Inst));
      TArgs.clear();
      ArgPos.clear();
    }
  }

  SmallVector<Operand, 4> Args;
  genericExprList(Call->Args, Args);
  // `mem.Replace(&c.F, v)` of a never-nil error, any, map or channel field:
  // v must be known non-nil (TYP-15).
  if (ReplaceField && Args.size() == 2)
    checkNeverNilStore(Args[1], ReplaceField, ReplacePlace);

  // A call into a C/C++ header: the foreign scope resolves overloads and
  // template arguments and says what the callee may change (D3.7, ADR-0008).
  SmallVector<ForeignArg, 4> FArgs;
  ForeignArg FRecv;
  ForeignCall FC;
  const ast::Expr *RecvExpr = nullptr;
  Object *FCallee = FTmpl;
  if (!FCallee)
    FCallee = foreignCallee(Call->Fun, RecvExpr);
  if (FCallee && FCallee->Pkg && FCallee->Pkg->Foreign) {
    bool Method = !FTmpl && RecvExpr &&
                  I.Selections.count(cast<SelectorExpr>(unparen(Call->Fun)));
    for (const Operand &A : Args) {
      ForeignArg FA;
      FA.Ty = A.isNil() ? nullptr : A.Ty;
      FA.Pos = A.pos();
      FA.Constant = A.M == Mode::Constant;
      FA.Nil = A.isNil();
      FA.Val = FA.Constant ? Mem.constant(A.Val) : nullptr;
      FA.ReadOnly = isReadOnlyValue(A.E, A.Ty);
      FArgs.push_back(FA);
    }
    if (Method) {
      FRecv.Ty = I.typeOf(RecvExpr);
      FRecv.Pos = RecvExpr->getBegin();
      FRecv.ReadOnly = isReadOnlyValue(RecvExpr, FRecv.Ty);
    }
    FC = FCallee->Pkg->Foreign->checkCall(FCallee, FTArgs,
                                          Method ? &FRecv : nullptr, FArgs);
    if (!FC.Sig) {
      Err Er = err(Call, diag::err_foreign_call, {str(Call->Fun), FC.Error});
      for (const std::string &N : FC.Notes)
        Er.note(Call->getBegin(), N);
      X.M = Mode::Invalid;
      X.E = Call;
      return ExprKind::Statement;
    }
    Sig = FC.Sig;
  }

  SignatureType *Out = Sig;
  arguments(Call, Sig, TArgs, ArgPos, Args, Call->EllipsisLoc.isValid(), Out);
  Sig = Out;
  // A call is not a constant (len/cap of an array containing it, builtins.go).
  E.HasCallOrRecv = true;
  if (own() && FC.Sig) {
    for (size_t K = 0; K < FArgs.size() && K < 64; ++K)
      if (((FC.MutableParams >> K) & 1) && FArgs[K].ReadOnly)
        err(FArgs[K].Pos, diag::err_foreign_readonly_arg,
            {str(Args[K].E), str(Call->Fun), num(K + 1)});
    if (FC.MutableRecv && FRecv.ReadOnly)
      err(FRecv.Pos, diag::err_foreign_readonly_recv,
          {str(RecvExpr), str(Call->Fun)});
  }
  if (WasGeneric && Sig->TParams.empty())
    recordTypeAndValue(Call->Fun, Mode::Value, Sig, nullptr);

  // Result.
  TupleType *Res = Sig->Results;
  size_t NRes = Res->size();
  if (Call->isMust()) {
    // D3.6: f!(...) drops the trailing error.
    if (!Sig->Fails &&
        !(NRes > 0 && identical(Res->Elems.back(), Ctx.errorType()))) {
      err(Call, diag::err_bang_no_error_result, {str(Call->Fun)});
      X.M = Mode::Invalid;
      X.E = Call;
      return ExprKind::Statement;
    }
    SmallVector<Type *, 4> Kept(Res->Elems.begin(), Res->Elems.end() - 1);
    Res = Ctx.tuple(Kept);
    NRes = Res->size();
    // ERR-6, ERR-11a: the innermost function decides what a failure does.
    BangKind BK = BangKind::Panic;
    if (Call != GoCall && enclosingCanFail())
      BK = Call == DeferCall ? BangKind::DeferSet : BangKind::Return;
    I.Bangs[Call] = BK;
  }
  switch (NRes) {
  case 0:
    X.M = Mode::NoValue;
    X.Ty = Ctx.tuple({});
    break;
  case 1:
    X.M = Mode::Value;
    X.Ty = Res->Elems[0];
    break;
  default:
    X.M = Mode::Value;
    X.Ty = Res;
  }
  X.E = Call;
  if (X.M == Mode::Value && !Sig->TParams.empty())
    X.M = Mode::Invalid;
  return ExprKind::Statement;
}

void Checker::arguments(const CallExpr *Call, SignatureType *Sig,
                        ArrayRef<Type *> TArgs0, ArrayRef<SourceLoc> ArgPos,
                        SmallVector<Operand, 4> &Args, bool HasDots,
                        SignatureType *&Out) {
  Out = Sig;
  size_t NArgs = Args.size();
  size_t NPars = Sig->Params->size();
  SmallVector<Type *, 8> SigParams(Sig->Params->Elems.begin(),
                                   Sig->Params->Elems.end());
  SmallVector<bool, 8> ParamMove;
  for (size_t K = 0; K < NPars; ++K)
    ParamMove.push_back(Sig->isMove(K));
  bool Adjusted = false;
  for (Operand &A : Args)
    if (A.M == Mode::Invalid)
      ; // errors were reported already
  if (Sig->Variadic) {
    if (HasDots) {
      if (Call->Args.size() == 1 && NArgs > 1) {
        err(Call, diag::err_cannot_use_ellipsis_with_multi,
            {str(Call->Args[0])});
        return;
      }
    } else if (NArgs + 1 >= NPars) {
      Type *Last = SigParams.back();
      Type *Elem = cast<SliceType>(Last)->Elem;
      bool LastMove = ParamMove.back();
      SigParams.pop_back();
      ParamMove.pop_back();
      while (SigParams.size() < NArgs) {
        SigParams.push_back(Elem);
        ParamMove.push_back(LastMove);
      }
      Adjusted = true;
      NPars = NArgs;
    } else {
      --NPars;
    }
  } else if (HasDots) {
    err(Call, diag::err_cannot_use_ellipsis, {str(Call->Fun)});
    return;
  }
  if (NArgs != NPars) {
    const ast::Expr *At = Call;
    bool TooMany = NArgs > NPars;
    if (TooMany)
      At = Args[NPars].E;
    else if (NArgs > 0)
      At = Args[NArgs - 1].E;
    SmallVector<Type *, 4> Have;
    for (Operand &A : Args)
      Have.push_back(A.Ty);
    err(At,
        TooMany ? diag::err_too_many_arguments : diag::err_not_enough_arguments,
        {str(Call->Fun), typesSummary(Have, false, HasDots),
         typesSummary(Sig->Params->Elems, Sig->Variadic, false)});
    return;
  }

  // Type parameters of the callee, renamed against recursive calls.
  SmallVector<TypeParamType *, 4> TPs;
  SmallVector<Type *, 4> TArgs(TArgs0.begin(), TArgs0.end());
  size_t N = Sig->TParams.size();
  TupleType *ParamTuple = Ctx.tuple(SigParams);
  if (N > 0) {
    TupleType *NewParams = nullptr;
    renameTParams(Sig->TParams, ParamTuple, TPs, &NewParams);
    SigParams.assign(NewParams->Elems.begin(), NewParams->Elems.end());
    ParamTuple = NewParams;
    while (TArgs.size() < TPs.size())
      TArgs.push_back(nullptr);
  }
  // Generic function arguments get their own type parameters.
  SmallVector<size_t, 2> GenericArgs;
  for (size_t K = 0; K < Args.size(); ++K) {
    auto *ASig = dyn_cast_or_null<SignatureType>(Args[K].Ty);
    if (ASig && !ASig->TParams.empty() && Args[K].M != Mode::Invalid) {
      SmallVector<TypeParamType *, 4> ATPs;
      TupleType *AParams = nullptr;
      renameTParams(ASig->TParams, ASig->Params, ATPs, &AParams);
      llvm::DenseMap<const Type *, Type *> Map;
      for (size_t J = 0; J < ATPs.size(); ++J)
        Map[ASig->TParams[J]] = ATPs[J];
      auto *NewSig = cast<SignatureType>(substImpl(ASig, Map, 0));
      Args[K].Ty =
          Ctx.signature(NewSig->Params, NewSig->Results, NewSig->Variadic,
                        NewSig->Fails, NewSig->MoveMask, ATPs);
      size_t Base = TPs.size();
      TPs.append(ATPs.begin(), ATPs.end());
      while (TArgs.size() < TPs.size())
        TArgs.push_back(nullptr);
      for (size_t J = 0; J < Args[K].PartialTArgs.size() && J < ATPs.size();
           ++J)
        TArgs[Base + J] = Args[K].PartialTArgs[J];
      GenericArgs.push_back(K);
    }
  }
  // P03.19 (OWN-P10): an argument that only a loop's assumption narrows is
  // the Option itself where its parameter takes the Option. For a generic
  // parameter inference decides, in one step: first with every such argument
  // the Option at once (they may share a type parameter: `pair(o, q)`), and
  // only if that does not infer, with each alone. An argument whose
  // instantiated parameter accepts the Option (`Option[T]`, `Option[*T]`,
  // `any`, a bare type parameter) is the Option (unnarrowTentative).
  if (!TPs.empty()) {
    SmallVector<std::pair<size_t, Type *>, 2> Tent;
    for (size_t K = 0; K < Args.size() && K < SigParams.size(); ++K)
      if (Type *OT = tentOption(Args[K]))
        Tent.push_back({K, OT});
    // Infer with the arguments in Which as their Options; unnarrow those
    // whose parameter then takes the Option. False if it does not infer.
    auto Try = [&](ArrayRef<std::pair<size_t, Type *>> Which) {
      SmallVector<Operand, 4> Trial(Args.begin(), Args.end());
      for (auto [K, OT] : Which)
        Trial[K].Ty = OT;
      InferError TE;
      SmallVector<Type *, 4> R =
          infer(Call->Lparen, TPs, TArgs, ParamTuple, Trial, false, &TE);
      if (R.empty())
        return false;
      for (auto [K, OT] : Which) {
        Operand O = Args[K];
        O.Ty = OT;
        if (assignableTo(O, subst(SigParams[K], TPs, R), nullptr))
          unnarrowTentative(Args[K]);
      }
      return true;
    };
    if (!Tent.empty() && !Try(Tent) && Tent.size() > 1)
      for (const auto &One : Tent)
        Try(One);
  }
  if (!TPs.empty()) {
    InferError IE;
    SmallVector<Type *, 4> Inferred =
        infer(Call->Lparen, TPs, TArgs, ParamTuple, Args, false, &IE);
    if (Inferred.empty()) {
      if (!IE.Msg.empty())
        err(IE.Pos, diag::err_sema_call,
            {"in call to " + str(Call->Fun) + ", " + IE.Msg});
      return;
    }
    // Instantiate the generic function arguments first: a type parameter of
    // the callee inferred as such an argument's (generic) signature is its
    // instance (`f(g)` with f[P any](P), g[Q int](Q): P is func(int)).
    size_t J = N;
    for (size_t K : GenericArgs) {
      auto *ASig = cast<SignatureType>(Args[K].Ty);
      size_t End = J + ASig->TParams.size();
      ArrayRef<Type *> Part(Inferred.data() + J, End - J);
      Args[K].Ty =
          instantiateSignature(Call->getBegin(), ASig, Part, {}, Args[K].E);
      recordTypeAndValue(Args[K].E, Mode::Value, Args[K].Ty, nullptr);
      for (size_t P = 0; P < N; ++P)
        if (auto *IS = dyn_cast_or_null<SignatureType>(Inferred[P]);
            IS && !IS->TParams.empty() && IS->TParams == ASig->TParams)
          Inferred[P] = Args[K].Ty;
      J = End;
    }
    if (N > 0) {
      ArrayRef<Type *> First(Inferred.data(), N);
      Out = cast<SignatureType>(instantiateSignature(Call->getBegin(), Sig,
                                                     First, ArgPos, Call->Fun));
      llvm::DenseMap<const Type *, Type *> Map;
      for (size_t K = 0; K < N; ++K)
        Map[TPs[K]] = Inferred[K];
      for (Type *&P : SigParams)
        P = substImpl(P, Map, 0);
      (void)Adjusted;
    }
  }

  // PAR-5: a `move` receiver takes ownership of the receiver, so the call is
  // legal only on an owned value (a local), or on move(x) / x.Clone().
  const ast::Expr *Callee = Call->Fun;
  while (auto *Pa = dyn_cast<ParenExpr>(Callee))
    Callee = Pa->X;
  if (own())
    if (auto *Sel = dyn_cast<SelectorExpr>(Callee)) {
      auto It = I.Selections.find(Sel);
      if (It != I.Selections.end() && It->second.K == Selection::MethodVal)
        if (auto *MF = dyn_cast_or_null<Func>(It->second.Obj);
            MF && MF->MoveRecv && !isImplicitMove(Sel->X, Call)) {
          checkMoveRecvUse(Sel, "call a `move` method on");
        }
      // OWN-R10: a value receiver of a @Pinned type copies the receiver.
      if (It != I.Selections.end() && It->second.K == Selection::MethodVal)
        if (auto *MF = dyn_cast_or_null<Func>(It->second.Obj);
            MF && !MF->HasPtrRecv && !MF->MoveRecv && MF->Recv &&
            isPinned(MF->Recv->Ty)) {
          Type *RT = I.typeOf(Sel->X);
          bool ViaPtr = RT && isa<PointerType>(under(RT));
          if (ViaPtr || placeKind(Sel->X) != PlaceKind::Temp)
            err(Sel->X, diag::err_own_pinned_copy,
                {ViaPtr ? "*" + str(Sel->X) : str(Sel->X), str(MF->Recv->Ty)});
        }
    }
  // Check the arguments.
  std::string Context = "argument to " + str(Call->Fun);
  for (size_t K = 0; K < Args.size(); ++K) {
    Operand &A = Args[K];
    bool Mv = own() && ParamMove[K];
    bool Saved = RefToOwnedOk;
    // A borrowed string / []T parameter takes a ref at zero cost (PAR-8).
    RefToOwnedOk = own() && !Mv;
    assignment(A, SigParams[K], Context);
    RefToOwnedOk = Saved;
    if (Mv && A.M != Mode::Invalid && !isImplicitMove(A.E, Call))
      needOwnership(A, "pass as a `move` argument");
    // OWN-R10: a by-value parameter of a @Pinned type copies the argument.
    else if (own() && !Mv && A.M != Mode::Invalid && A.E && A.Ty &&
             isPinned(SigParams[K]) && placeKind(A.E) != PlaceKind::Temp)
      err(A, diag::err_own_pinned_copy, {str(A.E), str(A.Ty)});
  }
  // Spread: append-like `move ...T` of owning elements needs ownership too.
  if (own() && HasDots && Sig->Variadic && !Args.empty() && ParamMove.back() &&
      Args.back().M != Mode::Invalid)
    needOwnership(Args.back(), "spread into a `move` parameter");
}
