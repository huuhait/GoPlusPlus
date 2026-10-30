//===- Typexpr.cpp - Type expressions and identifiers ---------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's typexpr.go, struct.go, interface.go, signature.go and
// union.go (BSD-style license, Copyright The Go Authors).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::isa_and_nonnull;

//===----------------------------------------------------------------------===//
// Identifiers
//===----------------------------------------------------------------------===//

void Checker::ident(Operand &X, const ast::Ident *Id, NamedType *Def,
                    bool WantType) {
  X.M = Mode::Invalid;
  X.E = Id;
  X.Ty = invalid();
  // Is this name the start of a selector, or the operand of `&` (EXP-15)?
  const PartialMark PM = takePartialMark(Id);
  StringRef N = name(Id);
  Scope *Found = nullptr;
  Object *Obj = E.Sc->lookup(N, E.Pos, &Found);
  if (!Obj) {
    if (N == "_")
      err(Id, diag::err_cannot_use_blank_type);
    else if (N == "new" && own())
      err(Id, diag::err_undefined, {std::string(N)})
          .note(Id->getBegin(), diag::note_own_new_removed, {});
    else
      err(Id, diag::err_undefined, {std::string(N)});
    return;
  }
  if (N == "_") {
    err(Id, diag::err_cannot_use_blank_type);
    return;
  }
  recordUse(Id, Obj);
  // A name of a dot import uses that import.
  if (Found && !isa<PkgName>(Obj) && StringRef(Found->Comment) == "file") {
    auto It = S.DotImportMap.find({Found, N});
    if (It != S.DotImportMap.end())
      markUsed(It->second);
  }

  // A type is wanted but the name is not one: stop before the declaration is
  // checked (a cycle there would hide this more useful error; types2,
  // go.dev/issue/65344).
  if (WantType && !isa<TypeName>(Obj)) {
    err(Id, diag::err_not_a_type, {std::string(Obj->Name)});
    if (auto *V = dyn_cast<Var>(Obj); V && !V->IsPackageLevel)
      V->Used = true;
    return;
  }

  // Package-level objects are resolved lazily (D3.4).
  if (S.ObjMap.count(Obj) || Obj->Color >= 2) {
    objDecl(Obj, Def);
    if (isa<Const>(Obj) || isa<Var>(Obj) || isa<Func>(Obj))
      addDeclDep(Obj);
  }

  Type *T = Obj->Ty;
  switch (Obj->getKind()) {
  case Object::Kind::PkgName:
    err(Id, diag::err_package_without_selector, {std::string(N)});
    markUsed(cast<PkgName>(Obj));
    return;
  case Object::Kind::Const: {
    auto *C = cast<Const>(Obj);
    if (!T || !isValid(T))
      return;
    if (Obj->Parent == Ctx.universe() && N == "iota") {
      if (!E.Iota) {
        err(Id, diag::err_iota_outside_const);
        return;
      }
      X.Val = *E.Iota;
    } else if (C->Val) {
      X.Val = *C->Val;
    } else {
      // A constant whose declaration failed has a type but no value (types2
      // gives it the unknown value): uses are still checked.
      X.Val = ConstValue();
    }
    X.M = Mode::Constant;
    break;
  }
  case Object::Kind::TypeName:
    if (auto *TN = cast<TypeName>(Obj);
        TN->IsAlias && !TN->AliasTParams.empty() && !E.GenericOk) {
      err(Id, diag::err_sema_typexpr,
          {"cannot use generic type " + std::string(N) +
           " without instantiation"});
      return;
    }
    X.M = Mode::TypeExpr;
    break;
  case Object::Kind::Var: {
    auto *V = cast<Var>(Obj);
    if (!V->IsPackageLevel)
      V->Used = true;
    if (!T || !isValid(T))
      return;
    if (!S.Opts.GoCompat) {
      // TYP-20: `base` names the parent part of the receiver: only
      // `base.M(...)` and `base.f`, never a value (an upcast of the receiver).
      if (V->IsBase && (!PM.Ok || PM.Addr)) {
        err(Id, diag::err_base_value,
            {BaseMethodOf.empty() ? str(cast<PointerType>(T)->Elem)
                                  : BaseMethodOf});
        return;
      }
      // ADR-0023 decision 16: a value receiver of a tagged type may be the
      // parent part of a child.
      if (V->IsRecv && isTagged(T) &&
          !partialUse(Id, PartialKind::Receiver, T, nullptr, PM))
        return;
    }
    X.M = Mode::Variable;
    // Flow typing (ADR-0018): an Option known to be set has the type of its
    // value here. The side table records this narrowed type for the use.
    T = narrowedType(Id, V, T);
    guardedUse(Id, V);
    break;
  }
  case Object::Kind::Func:
    if (!T)
      return;
    X.M = Mode::Value;
    break;
  case Object::Kind::Builtin:
    X.M = Mode::Builtin;
    X.BId = cast<Builtin>(Obj)->Id;
    T = typ(BasicKind::Invalid);
    break;
  case Object::Kind::Nil:
    X.M = Mode::NilValue;
    T = typ(BasicKind::UntypedNil);
    break;
  default:
    return;
  }
  if (!T) {
    X.M = Mode::Invalid;
    return;
  }
  X.Ty = T;
}

//===----------------------------------------------------------------------===//
// Type expressions
//===----------------------------------------------------------------------===//

Type *Checker::typ(const ast::Expr *Ex) { return definedType(Ex, nullptr); }

Type *Checker::varType(const ast::Expr *Ex, bool ValueUse, const char *What) {
  Type *T = definedType(Ex, nullptr);
  validVarType(Ex, T);
  // TYP-19: every type expression that holds a value (a variable, a field,
  // an element, a parameter, a result, the type of make or of a type
  // assertion) comes through here, so this is the one place where an
  // abstract type by value is refused; the pointee of `*`, a type argument
  // (decided on the instance, noAbstractArgs) and a parent are no value.
  if (ValueUse)
    noAbstract(Ex, T, What ? What : "a value");
  return T;
}

void Checker::validVarType(const ast::Expr *Ex, Type *T) {
  if (!T || isTypeParam(T))
    return;
  later([this, Ex, T]() {
    Type *U = under(T);
    if (auto *It = dyn_cast_or_null<InterfaceType>(U)) {
      if (isTypeParam(T))
        return;
      TypeSet *TS = typeSet(It);
      if (TS->hasTerms()) {
        err(Ex, diag::err_interface_contains_type_constraints, {str(T)});
      } else if (TS->HasOps) {
        err(Ex, diag::err_cannot_use_constraint_outside,
            {str(T), "interface has operator elements (TYP-27)"});
      } else if (TS->Comparable || TS->Copy || TS->Clone) {
        if (TS->Comparable)
          err(Ex, diag::err_interface_is_comparable, {str(T)});
        else
          err(Ex, diag::err_cannot_use_constraint_outside,
              {str(T), "interface is (or embeds) a constraint"});
      }
    }
  });
}

Type *Checker::definedType(const ast::Expr *Ex, NamedType *Def) {
  Type *T = typInternal(Ex, Def);
  if (!T)
    T = invalid();
  if (isGeneric(T)) {
    err(Ex, diag::err_sema_typexpr,
        {"cannot use generic type " + str(T) + " without instantiation"});
    T = invalid();
  }
  recordTypeAndValue(Ex, Mode::TypeExpr, T, nullptr);
  return T;
}

Type *Checker::genericType(const ast::Expr *Ex, std::string *Cause,
                           TypeName **Alias) {
  Operand X;
  const ast::Ident *NameId = nullptr;
  bool SavedOk = E.GenericOk;
  E.GenericOk = true;
  if (auto *Id = dyn_cast<ast::Ident>(Ex)) {
    NameId = Id;
    ident(X, Id, nullptr, true);
  } else if (auto *Sel = dyn_cast<SelectorExpr>(Ex)) {
    NameId = Sel->Sel;
    selector(X, Sel, true);
  } else if (auto *P = dyn_cast<ParenExpr>(Ex)) {
    E.GenericOk = SavedOk;
    return genericType(P->X, Cause, Alias);
  } else {
    E.GenericOk = SavedOk;
    *Cause = str(Ex) + " is not a generic type";
    return invalid();
  }
  E.GenericOk = SavedOk;
  if (X.M == Mode::Invalid)
    return invalid();
  if (X.M != Mode::TypeExpr) {
    *Cause = str(Ex) + " is not a type";
    return invalid();
  }
  recordTypeAndValue(Ex, Mode::TypeExpr, X.Ty, nullptr);
  if (Alias && NameId) {
    auto It = I.Uses.find(NameId);
    if (It != I.Uses.end())
      if (auto *TN = dyn_cast<TypeName>(It->second);
          TN && TN->IsAlias && !TN->AliasTParams.empty()) {
        *Alias = TN;
        return X.Ty;
      }
  }
  if (!isValid(X.Ty))
    return invalid();
  if (!isGeneric(X.Ty)) {
    *Cause = str(X.Ty) + " is not a generic type";
    return invalid();
  }
  return X.Ty;
}

bool Checker::typeList(ArrayRef<ast::Expr *> Es, SmallVector<Type *, 4> &Out) {
  bool Ok = true;
  for (const ast::Expr *Ex : Es) {
    Type *T = varType(Ex, /*ValueUse=*/false);
    if (!isValid(T))
      Ok = false;
    Out.push_back(T);
  }
  return Ok;
}

Type *Checker::typInternal(const ast::Expr *Ex, NamedType *Def) {
  using K = ast::Node::Kind;
  DepthGuard DeclDepth(DeclNest); // see objDecl
  switch (Ex->getKind()) {
  case K::BadExpr:
    return invalid();
  case K::Ident: {
    Operand X;
    ident(X, cast<ast::Ident>(Ex), Def, true);
    switch (X.M) {
    case Mode::TypeExpr:
      return X.Ty;
    case Mode::Invalid:
      return invalid();
    case Mode::NoValue:
      err(Ex, diag::err_sema_typexpr, {str(Ex) + " used as type"});
      return invalid();
    default:
      err(Ex, diag::err_not_a_type, {str(Ex)});
      return invalid();
    }
  }
  case K::SelectorExpr: {
    Operand X;
    selector(X, cast<SelectorExpr>(Ex), true, Def);
    switch (X.M) {
    case Mode::TypeExpr:
      return X.Ty;
    case Mode::Invalid:
      return invalid();
    default:
      err(Ex, diag::err_not_a_type, {str(Ex)});
      return invalid();
    }
  }
  case K::IndexExpr:
    return instantiatedType(cast<IndexExpr>(Ex), Def);
  case K::ParenExpr:
    return definedType(cast<ParenExpr>(Ex)->X, Def);
  case K::ArrayType: {
    auto *AT = cast<ast::ArrayType>(Ex);
    if (isa<Ellipsis>(AT->Len)) {
      err(AT->Len, diag::err_sema_typexpr,
          {"invalid use of [...] array (outside a composite literal)"});
      varType(AT->Elt);
      return invalid();
    }
    int64_t N = arrayLength(AT->Len);
    Type *Elem = varType(AT->Elt, true, "an element type");
    if (N < 0)
      return invalid();
    return Ctx.array(Elem, N);
  }
  case K::SliceType:
    return Ctx.slice(
        varType(cast<ast::SliceType>(Ex)->Elt, true, "an element type"));
  case K::StructType:
    return structTypeExpr(cast<ast::StructType>(Ex));
  case K::StarExpr: {
    Type *El = varType(cast<StarExpr>(Ex)->X, /*ValueUse=*/false);
    return isValid(El) ? static_cast<Type *>(Ctx.pointer(El)) : El;
  }
  case K::FuncType: {
    FuncInfo FI = funcSignature(cast<ast::FuncType>(Ex), nullptr, nullptr);
    return FI.Sig ? static_cast<Type *>(FI.Sig) : invalid();
  }
  case K::InterfaceType:
    return interfaceTypeExpr(cast<ast::InterfaceType>(Ex), Def);
  case K::MapType: {
    auto *M = cast<ast::MapType>(Ex);
    Type *Key = varType(M->Key, true, "a map key type");
    Type *Val = varType(M->Value, true, "an element type");
    later([this, M, Key]() {
      if (!isValid(Key))
        return;
      if (!comparableType(Key, true, nullptr)) {
        std::string Cause;
        if (isTypeParam(Key))
          Cause = " (missing comparable constraint)";
        err(M->Key, diag::err_invalid_map_key, {str(Key), Cause});
      }
    });
    return Ctx.map(Key, Val);
  }
  case K::ChanType: {
    auto *C = cast<ast::ChanType>(Ex);
    Type *El = varType(C->Value, true, "an element type");
    return Ctx.chan(static_cast<sema::ChanType::Dir>(C->Direction), El);
  }
  case K::Ellipsis:
    err(Ex, diag::err_invalid_use_of_dots);
    return invalid();
  case K::MacroInvocation:
    err(Ex, diag::err_sema_typexpr, {"macro invocations are expanded by P09"});
    return invalid();
  default: {
    // types2: report, then evaluate the expression for its own errors.
    err(Ex, diag::err_not_a_type, {str(Ex)});
    use(Ex);
    return invalid();
  }
  }
}

int64_t Checker::arrayLength(const ast::Expr *Ex) {
  // A bare identifier may be an attempt at a generic type declaration with
  // a missing constraint (types2 arrayLength).
  if (auto *Id = dyn_cast<ast::Ident>(Ex)) {
    Object *O = E.Sc->lookup(name(Id), E.Pos);
    if (!O) {
      err(Id, diag::err_sema_typexpr,
          {"undefined array length " + std::string(name(Id)) +
           " or missing type constraint"});
      return -1;
    }
    if (!isa<Const>(O)) {
      err(Id, diag::err_sema_typexpr,
          {"invalid array length " + std::string(name(Id))});
      return -1;
    }
  }
  Operand X;
  expr(X, Ex);
  if (X.M != Mode::Constant) {
    if (X.M != Mode::Invalid)
      err(Ex, diag::err_array_length_not_constant, {str(X)});
    return -1;
  }
  if (isUntyped(X.Ty) || isInteger(X.Ty)) {
    ConstValue V = X.Val.toInt();
    int64_t N = 0;
    if (V.kind() == ConstValue::Kind::Int && V.fitsInt64(N)) {
      if (N >= 0 &&
          (isUntyped(X.Ty) ||
           representableConst(X.Val, cast<BasicType>(under(X.Ty)), nullptr)))
        return N;
      err(Ex, diag::err_array_length_negative, {str(X)});
      return -1;
    }
    if (V.kind() == ConstValue::Kind::Int) {
      err(Ex, diag::err_invalid_array_length, {str(X)});
      return -1;
    }
  }
  if (isInteger(X.Ty))
    err(Ex, diag::err_invalid_array_length, {str(X)});
  else
    err(Ex, diag::err_array_length_untyped, {str(X)});
  return -1;
}

//===----------------------------------------------------------------------===//
// Struct types
//===----------------------------------------------------------------------===//

/// The effective required bit of a struct field (TYP-15, TYP-15a): the ONE
/// place that decides it. It is stored in Var::Required when the struct type
/// is built from source; every reader (literal check, zero-value query,
/// never-nil stores, conversions, export data, vet) reads the stored bit, and
/// instances and export data keep it. A field is required when it is marked
/// `@Required`, or when its struct is `@Strict` and the field is not marked
/// `@Option`, not blank `_` and not of type `Option[T]`. Pointer, func and
/// method-interface fields are required in every struct already (no zero
/// value); in a `@Strict` struct they carry the bit as well.
static bool effectiveRequired(bool Strict, bool MarkedRequired,
                              bool MarkedOption, bool Blank,
                              const Type *FieldTy) {
  if (MarkedRequired)
    return true;
  return Strict && !MarkedOption && !Blank &&
         !isa_and_nonnull<OptionType>(FieldTy);
}

/// R widened over the blanks before it, so that a fix-it that removes an
/// attribute leaves `N int`, not `N int ` (TYP-15a).
static SourceRange withBlanksBefore(const SourceManager &SM, SourceRange R) {
  FileID F = SM.getFileID(R.getBegin());
  if (!F.isValid())
    return R;
  StringRef Buf = SM.getBuffer(F);
  uint32_t Off = SM.getFileOffset(R.getBegin()), K = Off;
  while (K > 0 && K <= Buf.size() && (Buf[K - 1] == ' ' || Buf[K - 1] == '\t'))
    --K;
  return SourceRange(R.getBegin().getLocWithOffset(-int64_t(Off - K)),
                     R.getEnd());
}

/// Does T hold a view (OWN-V5)? A view itself, or a slice, array, map,
/// channel, Option or defined non-struct type of one. A struct type declared
/// with a literal has its own fields (and its own error), so it is not looked
/// into. Any other struct is: an instance of a generic struct holds what its
/// type arguments make of its fields (`W[View[string]]` for `struct{ x T }`;
/// `W[int]` holds none), and `type T W[View[string]]` has no literal of its
/// own. The decision is made on the real, substituted fields, once for each
/// type: a walk that finds no view proves every type it expanded has none
/// (ViewFree), so any number of fields cost one expansion per type in all.
/// A `@View` struct may hold views, so it is looked into too (its own
/// fields were allowed to hold them at its declaration).
bool Checker::containsView(Type *T) {
  resolvePendingUnder();
  // Can the type argument A make a view part of the instance (is there a
  // view, or a named type that may hold one, in it)? A type parameter is no
  // view yet: the arguments of its instances are checked where they are
  // given. An instance with no such argument holds none (its own fields were
  // checked at its declaration), so it is not expanded: a chain of generic
  // types over a type parameter costs nothing.
  auto MayHold = [&](ArrayRef<Type *> Args) {
    SmallVector<Type *, 8> W(Args.begin(), Args.end());
    llvm::SmallPtrSet<const Type *, 8> Met;
    while (!W.empty()) {
      Type *X = W.pop_back_val();
      if (!X || !Met.insert(X).second)
        continue;
      switch (X->getKind()) {
      case Type::Kind::Ref:
        return true;
      case Type::Kind::Named: {
        auto *N = cast<NamedType>(X);
        if (N->Ptr != PtrKind::None)
          break;
        if (N->origin()->ViewAttr) // may hold views (OWN-V5)
          return true;
        if (N->Orig) {
          for (Type *A : N->TArgs)
            W.push_back(A);
        } else if (!isa_and_nonnull<StructType>(N->FromRHS) &&
                   !isa<BasicType>(under(N))) {
          return true;
        }
        break;
      }
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
  // The walk, with the way each type was reached: when a view is found, the
  // structs on the way hold it too (ViewHolders), so a chain of n generic
  // types asked about n times costs n steps in all, not n each.
  struct Item {
    Type *T;
    int Parent; // index in Trail of the struct it was reached in, or -1
  };
  SmallVector<Item, 8> Work{{T, -1}};
  SmallVector<const NamedType *, 8> Expanded;
  SmallVector<std::pair<const NamedType *, int>, 8> Trail; // N, its parent
  llvm::SmallPtrSet<const Type *, 8> Seen;
  auto Found = [&](int Parent, const NamedType *Self) {
    if (Self)
      ViewHolders.insert(Self);
    for (int I = Parent; I >= 0; I = Trail[I].second)
      ViewHolders.insert(Trail[I].first);
    return true;
  };
  while (!Work.empty()) {
    auto [X, Parent] = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    int Self = Parent;
    if (auto *N = dyn_cast<NamedType>(X)) {
      if (N->Ptr != PtrKind::None || onInstCycle(N) || ViewFree.count(N))
        continue;
      if (ViewHolders.count(N))
        return Found(Parent, nullptr);
      if (N->Orig && !N->Orig->ViewAttr && !MayHold(N->TArgs))
        continue;
      X = under(N);
      if (!X)
        continue;
      if (auto *St = dyn_cast<StructType>(X)) {
        // A `@View` struct holds the views its fields hold, so it is
        // looked into, an instance with its substituted fields (OWN-V5).
        const NamedType *O = N->origin();
        if (!O->ViewAttr && !N->Orig && isa_and_nonnull<StructType>(N->FromRHS))
          continue;
        Expanded.push_back(N);
        Trail.push_back({N, Parent});
        Self = static_cast<int>(Trail.size()) - 1;
        for (Var *F : St->Fields)
          Work.push_back({F->Ty, Self});
        continue;
      }
    }
    switch (X->getKind()) {
    case Type::Kind::Ref:
      return Found(Self, nullptr);
    case Type::Kind::Slice:
      Work.push_back({cast<SliceType>(X)->Elem, Self});
      break;
    case Type::Kind::Array:
      Work.push_back({cast<sema::ArrayType>(X)->Elem, Self});
      break;
    case Type::Kind::Option:
      Work.push_back({cast<OptionType>(X)->Elem, Self});
      break;
    case Type::Kind::Chan:
      Work.push_back({cast<sema::ChanType>(X)->Elem, Self});
      break;
    case Type::Kind::Map:
      Work.push_back({cast<sema::MapType>(X)->Key, Self});
      Work.push_back({cast<sema::MapType>(X)->Elem, Self});
      break;
    default:
      break;
    }
  }
  ViewFree.insert(Expanded.begin(), Expanded.end());
  return false;
}

Type *Checker::structTypeExpr(const ast::StructType *St) {
  // TYP-15a: only the struct type of a `@Strict` declaration is strict.
  const bool Strict = own() && St == StrictStructExpr;
  SmallVector<TypeContext::FieldSpec, 8> Fields;
  SmallVector<const ast::Ident *, 8> Idents;
  SmallVector<const ast::Expr *, 8> EmbExprs;
  llvm::StringSet<> Seen;
  // The `@Required` and `@Option` attributes of the field being added
  // (TYP-15, TYP-15a), if any.
  const Attribute *ReqAttr = nullptr;
  const Attribute *OptAttr = nullptr;
  // OWN-V5: a field that holds a view needs `@View` on the struct.
  const bool ViewOk = !own() || St == ViewStructExpr;
  std::string StructName = St == DeclStructExpr
                               ? "struct " + std::string(DeclStructName)
                               : std::string("the anonymous struct");
  const ast::Expr *CurTE = nullptr;
  Type *ParentTy = nullptr; // TYP-16: the type this declaration extends
  auto Add = [&](const ast::Ident *Id, StringRef N, Type *T, bool Emb,
                 StringRef Tag, SourceLoc Pos, const ast::Expr *EmbE,
                 bool IsParent = false) {
    if (N != "_" && !Seen.insert(N).second) {
      err(Pos, diag::err_duplicate_field, {std::string(N)});
      return;
    }
    TypeContext::FieldSpec FS;
    FS.Name = Mem.copy(N);
    FS.Pkg = S.Pkg;
    FS.Ty = T;
    FS.Embedded = Emb;
    FS.IsParent = IsParent;
    FS.Tag = Tag;
    FS.MarkedRequired = ReqAttr && own();
    FS.MarkedOption = OptAttr && own();
    // The parent part is never required as a whole: the required bits of its
    // own fields apply in the child (TYP-17).
    FS.Required = !IsParent && effectiveRequired(Strict, FS.MarkedRequired,
                                                 FS.MarkedOption, N == "_", T);
    Fields.push_back(FS);
    Idents.push_back(Id);
    EmbExprs.push_back(EmbE);
    if (ParentTy && !IsParent && N != "_")
      FieldPosOf[DeclStructExpr].push_back({Fields.back().Name, Pos});
  };
  // The attribute checks that need the field type, once per field
  // declaration (`A, B int @Option` is one). Name is its first name that is
  // not `_`, or `_`.
  auto CheckAttrs = [&](StringRef Name, Type *T) {
    if (!own())
      return;
    if (!ViewOk && isValid(T)) {
      const ast::Expr *TE = CurTE;
      std::string FName(Name), SName = StructName;
      later([this, TE, T, FName, SName]() {
        if (containsView(T))
          err(TE, diag::err_view_field_without_attr, {FName, SName, str(T)});
      });
    }
    if (OptAttr && !Strict)
      err(OptAttr->AtLoc, diag::err_option_not_strict);
    else if (OptAttr && ReqAttr)
      err(OptAttr->AtLoc, diag::err_option_and_required, {std::string(Name)});
    if (!isValid(T))
      return;
    const bool Blank = Name == "_";
    std::string FName(Name);
    if (ReqAttr) {
      // ADR-0018 point 7 and amendment point 3: a pointer, a func and an
      // interface with methods are never nil, so @Required says nothing.
      // In a `@Strict` struct the attribute is also redundant where
      // `@Strict` alone requires the field (TYP-15a); those errors first.
      bool Redundant =
          effectiveRequired(Strict, false, OptAttr != nullptr, Blank, T);
      SourceLoc ReqPos = ReqAttr->AtLoc;
      SourceRange ReqRange = withBlanksBefore(
          S.SM, SourceRange(ReqAttr->getBegin(), ReqAttr->getEnd()));
      later([this, ReqPos, ReqRange, FName, T, Redundant]() {
        if (isPointerKind(T))
          err(ReqPos, diag::err_required_pointer, {FName, str(T)});
        else if (const char *Why = neverNilKind(T))
          err(ReqPos, diag::err_required_never_nil, {FName, str(T), Why});
        else if (Redundant)
          err(ReqPos, diag::err_required_redundant, {FName})
              .fixit(ReqRange, "");
      });
    }
    if (OptAttr && Strict && !ReqAttr) {
      SourceLoc OptPos = OptAttr->AtLoc;
      if (isa<OptionType>(T))
        err(OptPos, diag::err_option_redundant,
            {FName, "a field of type " + str(T) +
                        " may be left out already (it is nil)"});
      else if (Blank)
        err(OptPos, diag::err_option_redundant,
            {FName, "the blank field _ is never required"});
      else if (!isa<TypeParamType>(T)) // judged per instance: checkOptionFields
        later([this, OptPos, FName, T]() {
          std::string Why;
          if (!hasZeroValue(T, &Why))
            err(OptPos, diag::err_option_no_zero, {FName, str(T), Why});
        });
    }
  };
  if (DeclExtendsExpr && St == DeclStructExpr)
    addParentField(DeclExtendsExpr, DeclStructName, [&](Type *PT, StringRef N) {
      ParentTy = PT;
      Add(nullptr, N, PT, true, StringRef(), DeclExtendsExpr->getBegin(),
          DeclExtendsExpr, /*IsParent=*/true);
    });
  if (St->Fields) {
    for (const Field *F : St->Fields->List) {
      std::string Tag;
      if (F->Tag) {
        std::string Out;
        if (unquoteString(F->Tag->Text, Out))
          Tag = Out;
        else
          err(F->Tag, diag::err_sema_typexpr, {"invalid struct tag"});
      }
      StringRef TagRef = Mem.copy(Tag);
      ReqAttr = nullptr;
      OptAttr = nullptr;
      CurTE = F->Type;
      for (const Attribute *A : F->Attrs) {
        if (A->Pkg || !A->Name)
          continue;
        StringRef AN = name(A->Name);
        if (AN == "Required") {
          if (own() && A->HasArgs)
            err(A->AtLoc, diag::err_required_args);
          ReqAttr = A;
        } else if (AN == "Option") {
          if (own() && A->HasArgs)
            err(A->AtLoc, diag::err_attr_no_args, {"@Option"});
          OptAttr = A;
        } else if (AN == "Strict" && own()) {
          // TYP-15a: @Strict is a type attribute only.
          err(A->AtLoc, diag::err_strict_placement, {"a field"});
        }
      }
      if (F->Names.empty()) {
        // Embedded field: T, *T, pkg.T or T[args]. (In a .gopp file the
        // embedding is the error; the field type gets no second one.)
        const ast::Expr *TE = F->Type;
        Type *T = varType(TE, /*ValueUse=*/S.Opts.GoCompat);
        const ast::Expr *Base = TE;
        if (auto *Sx = dyn_cast<StarExpr>(Base))
          Base = Sx->X;
        if (auto *Ix = dyn_cast<IndexExpr>(Base))
          Base = Ix->X;
        while (auto *P = dyn_cast<ParenExpr>(Base))
          Base = P->X;
        StringRef N;
        if (auto *Id = dyn_cast<ast::Ident>(Base))
          N = name(Id);
        else if (auto *Sel = dyn_cast<SelectorExpr>(Base))
          N = name(Sel->Sel);
        else {
          err(TE, diag::err_sema_typexpr,
              {"invalid embedded field type " + str(TE)});
          continue;
        }
        CheckAttrs(N, T);
        if (!S.Opts.GoCompat) {
          // TYP-25: no struct embedding in a .gopp file; the field is kept
          // as a named field, so nothing is promoted.
          err(TE, diag::err_struct_embedding, {std::string(N), str(TE)})
              .fixit(SourceRange(TE->getBegin(), TE->getBegin()),
                     std::string(N) + " ");
          Add(nullptr, N, T, false, TagRef, TE->getBegin(), TE);
          continue;
        }
        Add(nullptr, N, T, true, TagRef, TE->getBegin(), TE);
        if (isValid(T)) {
          later([this, TE, T]() {
            bool IsPtr = false;
            Type *Base = deref(T, IsPtr);
            Type *U = under(Base);
            if (!U || !isValid(U))
              return;
            if (isTypeParam(Base)) {
              err(TE, diag::err_embedded_field_type_param);
            } else if (isUnsafePointer(Base)) {
              err(TE, diag::err_embedded_field_unsafe_pointer);
            } else if (isa<PointerType>(U)) {
              err(TE, diag::err_embedded_field_pointer);
            } else if (IsPtr && isa<InterfaceType>(U)) {
              err(TE, diag::err_embedded_field_pointer_iface);
            }
          });
        }
      } else {
        Type *T = varType(F->Type, true, "the type of a struct field");
        StringRef Name = "_";
        for (const ast::Ident *Id : F->Names)
          if (name(Id) != "_") {
            Name = name(Id);
            break;
          }
        CheckAttrs(Name, T);
        for (const ast::Ident *Id : F->Names)
          Add(Id, name(Id), T, false, TagRef, Id->getBegin(), nullptr);
      }
    }
  }
  StructType *ST = Ctx.structOf(Fields, Strict);
  for (size_t K = 0; K < Idents.size() && K < ST->Fields.size(); ++K)
    if (Idents[K])
      recordDef(Idents[K], ST->Fields[K]);
  return ST;
}

//===----------------------------------------------------------------------===//
// Interfaces, unions and constraints
//===----------------------------------------------------------------------===//

static void flattenUnion(const ast::Expr *Ex,
                         SmallVector<const ast::Expr *, 4> &Out) {
  if (auto *B = dyn_cast<BinaryExpr>(Ex)) {
    if (B->Op == tok::or_) {
      flattenUnion(B->X, Out);
      flattenUnion(B->Y, Out);
      return;
    }
  }
  Out.push_back(Ex);
}

Type *Checker::unionTypeExpr(const ast::Expr *Ex) {
  // types2: parseUnion / parseTilde.
  constexpr size_t MaxTermCount = 100;
  SmallVector<const ast::Expr *, 4> Parts;
  flattenUnion(Ex, Parts);
  SmallVector<Term, 4> Terms;
  SmallVector<const ast::Expr *, 4> TermExprs;
  bool TooMany = false;
  for (const ast::Expr *P : Parts) {
    Term T;
    const ast::Expr *TE = P;
    if (auto *U = dyn_cast<UnaryExpr>(P); U && U->Op == tok::tilde) {
      T.Tilde = true;
      TE = U->X;
    }
    T.T = typ(TE);
    // Embedding stand-alone type parameters is not permitted
    // (go.dev/issue/47127).
    if (isTypeParam(T.T)) {
      if (T.Tilde)
        err(TE, diag::err_sema_typexpr,
            {"type in term ~" + str(TE) + " cannot be a type parameter"});
      else
        err(TE, diag::err_union_type_param);
      T.T = invalid();
    }
    if (Parts.size() == 1 && !T.Tilde)
      return T.T;
    if (Terms.size() >= MaxTermCount) {
      if (!TooMany)
        err(TE, diag::err_sema_typexpr,
            {"cannot handle more than 100 union terms (implementation "
             "limitation)"});
      TooMany = true;
    } else {
      Terms.push_back(T);
      TermExprs.push_back(TE);
    }
  }
  if (TooMany || Terms.empty())
    return invalid();
  SmallVector<Term, 4> Copy(Terms.begin(), Terms.end());
  later([this, Copy, TermExprs]() {
    auto TermStr = [&](const Term &T) {
      return (T.Tilde ? "~" : "") + typeString(T.T, nullptr);
    };
    for (size_t K = 0; K < Copy.size(); ++K) {
      const Term &T = Copy[K];
      if (!isValid(T.T))
        continue;
      const ast::Expr *TE = TermExprs[K];
      Type *U = under(T.T);
      auto *It = dyn_cast_or_null<InterfaceType>(U);
      if (T.Tilde) {
        if (It) {
          err(TE, diag::err_union_tilde_interface, {str(T.T)});
          continue;
        }
        if (U && !identical(U, T.T)) {
          err(TE, diag::err_union_tilde_underlying, {str(T.T), str(U)});
          continue;
        }
      }
      if (It) {
        TypeSet *TS = typeSet(It);
        if (!TS->Methods.empty())
          err(TE, diag::err_union_with_interface, {TermStr(T), TermStr(T)});
        else if (It->Sp == InterfaceType::Comparable)
          err(TE, diag::err_union_comparable);
        else if (TS->Comparable)
          err(TE, diag::err_sema_typexpr,
              {"cannot use " + TermStr(T) + " in union (" + TermStr(T) +
               " embeds comparable)"});
        continue; // interface terms are not subject to the no-overlap rule
      }
      for (size_t J = 0; J < K; ++J) {
        if (isa_and_nonnull<InterfaceType>(under(Copy[J].T)))
          continue;
        if (!termDisjoint(Copy[J], T)) {
          err(TE, diag::err_union_overlapping, {TermStr(T), TermStr(Copy[J])});
          break;
        }
      }
    }
  });
  return Ctx.newUnion(Mem.copy(ArrayRef<Term>(Terms)));
}

Type *Checker::interfaceTypeExpr(const ast::InterfaceType *It, NamedType *Def) {
  SmallVector<Func *, 8> Methods;
  SmallVector<Type *, 4> Embeds;
  SmallVector<SourceLoc, 4> EmbLocs;
  Type *Ext = nullptr; // GEN-12
  SourceLoc ExtLoc;
  if (It->Methods) {
    for (const Field *F : It->Methods->List) {
      if (!F->Names.empty()) {
        const ast::Ident *Id = F->Names[0];
        StringRef N = name(Id);
        auto *FT = dyn_cast_or_null<ast::FuncType>(F->Type);
        if (N == "_") {
          err(Id, diag::err_sema_typexpr,
              {"methods must have a unique non-blank name"});
          continue;
        }
        if (!FT) {
          err(F->Type, diag::err_sema_typexpr, {"invalid method signature"});
          continue;
        }
        Func *M = newFunc(N, nullptr, Id->getBegin());
        M->IsInterfaceMethod = true;
        // An operator element (TYP-27, GEN-13): an overload set of a
        // constraint shares the name, like the methods of a type.
        if (!S.Opts.GoCompat && isOperatorSpelling(N)) {
          M->Op = opKindOf(N, FT->Params ? FT->Params->numFields() : 0);
          Ctx.noteOperators();
        }
        FuncInfo FI = funcSignature(FT, nullptr, M);
        if (FT->TypeParams && FT->TypeParams->numFields())
          err(FT->TypeParams, diag::err_invalid_method_type_params);
        if (!FI.Sig)
          continue;
        M->Ty = FI.Sig;
        M->Params = FI.Params;
        M->Results = FI.Results;
        M->Fails = FI.Sig->Fails;
        recordDef(Id, M);
        Methods.push_back(M);
      } else if (F->isExtends()) {
        // GEN-12: `extends P`
        Type *PT = extendsConstraintType(F->Type);
        if (!PT)
          continue;
        if (Ext)
          err(F->ExtendsLoc, diag::err_extends_constraint,
              {"interface", "the constraint",
               "it extends more than one type; write one `extends`"});
        else {
          Ext = PT;
          ExtLoc = F->ExtendsLoc;
        }
      } else {
        Type *T = unionTypeExpr(F->Type);
        if (!T || !isValid(T))
          continue;
        Embeds.push_back(T);
        EmbLocs.push_back(F->Type->getBegin());
      }
    }
  }
  (void)Def;
  InterfaceType *Ity = Ctx.newInterface(Mem.copy(ArrayRef<Func *>(Methods)),
                                        Mem.copy(ArrayRef<Type *>(Embeds)),
                                        Mem.copy(ArrayRef<SourceLoc>(EmbLocs)));
  Ity->Extends = Ext;
  Ity->ExtendsLoc = ExtLoc;
  SourceLoc Pos = It->getBegin();
  later([this, Ity, Pos]() { computeTypeSet(Ity, Pos); });
  return Ity;
}

Type *Checker::boundTypeNoWrap(const ast::Expr *Ex) {
  bool IsUnion = false;
  if (auto *B = dyn_cast<BinaryExpr>(Ex); B && B->Op == tok::or_)
    IsUnion = true;
  if (auto *U = dyn_cast<UnaryExpr>(Ex); U && U->Op == tok::tilde)
    IsUnion = true;
  if (IsUnion)
    return unionTypeExpr(Ex);
  return typ(Ex);
}

Type *Checker::boundType(const ast::Expr *Ex, bool) {
  Type *T = boundTypeNoWrap(Ex);
  if (!T || !isValid(T))
    return T;
  bool NeedsWrap = true;
  if (!isa<UnionType>(T)) {
    Type *U = under(T);
    if (U && isa<InterfaceType>(U) && !isTypeParam(T))
      NeedsWrap = false;
    else if (isTypeParam(T)) {
      err(Ex, diag::err_type_param_bound_not_interface);
      return invalid();
    }
  }
  if (!NeedsWrap)
    return T;
  Type *Embeds[] = {T};
  SourceLoc Locs[] = {Ex->getBegin()};
  InterfaceType *I =
      Ctx.newInterface({}, Mem.copy(ArrayRef<Type *>(Embeds)),
                       Mem.copy(ArrayRef<SourceLoc>(Locs)), true);
  SourceLoc Pos = Ex->getBegin();
  later([this, I, Pos]() { computeTypeSet(I, Pos); });
  return I;
}

//===----------------------------------------------------------------------===//
// Instantiated types
//===----------------------------------------------------------------------===//

Type *Checker::instantiatedType(const IndexExpr *Ix, NamedType *Def) {
  (void)Def;
  if (Type *Foreign = foreignInstantiate(Ix))
    return Foreign;
  std::string Cause;
  TypeName *Alias = nullptr;
  Type *G = genericType(Ix->X, &Cause, &Alias);
  if (!isValid(G)) {
    // types2: the type arguments are not evaluated.
    if (!Cause.empty())
      err(Ix->X, diag::err_sema_typexpr,
          {"invalid operation: " + str(Ix) + " (" + Cause + ")"});
    return invalid();
  }
  if (Alias) {
    SmallVector<Type *, 4> AArgs;
    if (!typeList(Ix->Indices, AArgs))
      return invalid();
    size_t AGot = AArgs.size(), AWant = Alias->AliasTParams.size();
    if (AGot != AWant) {
      err(AGot < AWant ? Ix->Rbrack : Ix->Indices[AWant]->getBegin(),
          diag::err_sema_typexpr,
          {std::string(AGot < AWant ? "not enough" : "too many") +
           " type arguments for type " + std::string(Alias->Name) + ": have " +
           num(AGot) + ", want " + num(AWant)});
      return invalid();
    }
    SmallVector<SourceLoc, 4> ALocs;
    for (const ast::Expr *A : Ix->Indices)
      ALocs.push_back(A->getBegin());
    SmallVector<TypeParamType *, 4> ATPs(Alias->AliasTParams.begin(),
                                         Alias->AliasTParams.end());
    SourceLoc APos = Ix->getBegin();
    Type *Res = subst(G, Alias->AliasTParams, AArgs);
    // An instantiation of a generic alias is an edge of the cycle check like
    // any other (types2 mono.go: the alias's type parameters are vertices;
    // its right-hand side records the edges out of them).
    recordMono(APos, nullptr, ATPs, AArgs);
    later([this, APos, ATPs, AArgs, ALocs, Res]() {
      std::string Why;
      int Bad = verify(APos, ATPs, AArgs, &Why, /*ForType=*/true);
      if (Bad >= 0)
        err(static_cast<size_t>(Bad) < ALocs.size() ? ALocs[Bad] : APos,
            diag::err_sema_typexpr, {Why});
      else {
        checkOptionFields(APos, Res);
        noAbstractArgs(APos, Res, AArgs); // TYP-19
      }
    });
    return Res;
  }
  auto *N = cast<NamedType>(G);
  SmallVector<Type *, 4> TArgs;
  bool Ok = typeList(Ix->Indices, TArgs);
  if (!Ok)
    return invalid();
  size_t Got = TArgs.size();
  // The type parameters of a generic type that is still being declared are
  // available from the declaration (collected before the right hand side).
  size_t Want = N->TParams.size();
  if (Got != Want) {
    // types2: the instance still exists (with an invalid underlying type),
    // so `var x T[int]` has a type and follow-on errors are precise.
    const char *Which = Got < Want ? "not enough" : "too many";
    err(Got < Want ? Ix->Rbrack : Ix->Indices[Want]->getBegin(),
        diag::err_sema_typexpr,
        {std::string(Which) + " type arguments for type " +
         std::string(N->Obj->Name) + ": have " + num(Got) + ", want " +
         num(Want)});
    NamedType *Bad = instantiateNamed(N, TArgs);
    recordInstance(Ix, TArgs, Bad);
    return Bad;
  }
  if (N->IsView) {
    // View[X] (ADR-0020) is a type of its own too.
    Type *Arg = TArgs[0];
    if (!isValid(Arg))
      return invalid();
    if (const char *Why = badViewArg(Arg)) {
      err(Ix->Indices[0], diag::err_view_bad_arg, {str(Arg), Why});
      return invalid();
    }
    RefType *V = Ctx.ref(Arg);
    recordInstance(Ix, TArgs, V);
    return V;
  }
  if (N->IsOption) {
    // Option[T] (ADR-0018) is a type of its own, not an instance of a struct.
    Type *Arg = TArgs[0];
    OptionType *Opt = Ctx.option(Arg);
    recordInstance(Ix, TArgs, Opt);
    if (isValid(Arg)) {
      SourceLoc ArgPos = Ix->Indices[0]->getBegin();
      later([this, Arg, ArgPos, Opt]() {
        if (const char *Why = badOptionArg(Arg))
          err(ArgPos, diag::err_option_bad_arg, {str(Arg), Why});
        else
          noAbstractArgs(ArgPos, Opt, Arg); // TYP-19: Option holds it
      });
    }
    return Opt;
  }
  if (N->Ptr != PtrKind::None && own() && isValid(TArgs[0]) &&
      !isa<PointerType>(TArgs[0])) {
    // Unique[T] without the `*` (ADR-0018 point 2).
    const ast::Expr *A = Ix->Indices[0];
    Err Er = err(A, diag::err_ptr_arg_not_pointer,
                 {std::string(N->Obj->Name), str(TArgs[0])});
    Er.fixit(SourceRange(A->getBegin(), A->getBegin()), "*");
    return invalid();
  }
  NamedType *Inst = instantiateNamed(N, TArgs);
  recordInstance(Ix, TArgs, Inst);
  recordMono(Ix->getBegin(), N->origin(), N->TParams, TArgs);
  SmallVector<SourceLoc, 4> Locs;
  for (const ast::Expr *A : Ix->Indices)
    Locs.push_back(A->getBegin());
  SmallVector<TypeParamType *, 4> TPs(N->TParams.begin(), N->TParams.end());
  SourceLoc Pos = Ix->getBegin();
  later([this, Pos, TPs, TArgs, Locs, Inst]() {
    std::string Why;
    int Bad = verify(Pos, TPs, TArgs, &Why, /*ForType=*/true);
    if (Bad >= 0) {
      err(static_cast<size_t>(Bad) < Locs.size() ? Locs[Bad] : Pos,
          diag::err_sema_typexpr, {Why});
    } else {
      checkOptionArgs(Pos, Inst, TArgs);
      checkOptionFields(Pos, Inst);
      noAbstractArgs(Pos, Inst, TArgs); // TYP-19
    }
  });
  return Inst;
}

/// Why View[Arg] is not allowed (ADR-0020), or null. The allowed forms: every
/// type except the pointer kinds, unsafe.Pointer and a view.
const char *Checker::badViewArg(Type *Arg) {
  Type *U = under(Arg);
  if (!isValid(U))
    return nullptr;
  if (isa<RefType>(U))
    return "a View of a View is the same as one View, write View[T] once; "
           "allowed: any type except *T, Unique[*T], Shared[*T], Weak[*T], "
           "unsafe.Pointer and a View";
  if (isa<PointerType>(U) || hasPtrKind(Arg, nullptr, nullptr) ||
      isUnsafePointer(Arg))
    return "a View holds a value, not a pointer; allowed: any type except "
           "*T, Unique[*T], Shared[*T], Weak[*T], unsafe.Pointer and a View";
  return nullptr;
}

const char *Checker::badOptionArg(Type *Arg) {
  if (!own() || !Arg || isTypeParam(Arg) || !isValid(under(Arg)))
    return nullptr;
  if (isa<OptionType>(under(Arg)))
    return "an Option already has a nil";
  if (hasNil(Arg))
    return "it already has a nil";
  return nullptr;
}

namespace {
/// The first Option inside T (an instance) whose value type is one of Args
/// and cannot be an Option's value: Option[T] of the generic declaration
/// with T = error, say. The Options written with such a type are reported
/// where they are written.
struct BadOptionWalk {
  BadOptionWalk(Checker &C, ArrayRef<Type *> Args) : C(C), Args(Args) {}
  Checker &C;
  ArrayRef<Type *> Args;
  llvm::SmallPtrSet<const Type *, 16> Seen;
  unsigned Depth = 0;

  OptionType *walk(Type *T) {
    if (!T || Depth > Checker::MaxRecDepth || !Seen.insert(T).second)
      return nullptr;
    Checker::DepthGuard DG(Depth);
    switch (T->getKind()) {
    case Type::Kind::Option: {
      auto *O = cast<OptionType>(T);
      for (Type *A : Args)
        if (C.identical(A, O->Elem) && C.badOptionArg(O->Elem))
          return O;
      return walk(O->Elem);
    }
    case Type::Kind::Pointer:
      return walk(cast<PointerType>(T)->Elem);
    case Type::Kind::Slice:
      return walk(cast<SliceType>(T)->Elem);
    case Type::Kind::Ref:
      return walk(cast<RefType>(T)->Elem);
    case Type::Kind::Array:
      return walk(cast<sema::ArrayType>(T)->Elem);
    case Type::Kind::Chan:
      return walk(cast<sema::ChanType>(T)->Elem);
    case Type::Kind::Map: {
      auto *M = cast<sema::MapType>(T);
      if (OptionType *O = walk(M->Key))
        return O;
      return walk(M->Elem);
    }
    case Type::Kind::Struct:
      for (Var *F : cast<StructType>(T)->Fields)
        if (OptionType *O = walk(F->Ty))
          return O;
      return nullptr;
    case Type::Kind::Tuple:
      for (Type *E : cast<TupleType>(T)->Elems)
        if (OptionType *O = walk(E))
          return O;
      return nullptr;
    case Type::Kind::Signature: {
      auto *S = cast<SignatureType>(T);
      if (OptionType *O = walk(S->Params))
        return O;
      return walk(S->Results);
    }
    case Type::Kind::Interface:
      for (Func *M : cast<InterfaceType>(T)->Methods)
        if (OptionType *O = walk(M->Ty))
          return O;
      return nullptr;
    case Type::Kind::Named: {
      // Only an instance holds the arguments; under() expands it. An
      // instance on an instantiation cycle is a leaf (onInstCycle).
      auto *N = cast<NamedType>(T);
      if (!N->Orig || N->TArgs.empty() || Checker::onInstCycle(N))
        return nullptr;
      return walk(C.under(N));
    }
    default:
      return nullptr;
    }
  }
};
} // namespace

void Checker::checkOptionArgs(SourceLoc Pos, Type *Inst,
                              ArrayRef<Type *> TArgs) {
  // OWN-P9: in generic code an Option's value type is checked per instance.
  // Only arguments that cannot be an Option's value can make one invalid.
  if (!own() || llvm::none_of(TArgs, [&](Type *A) { return badOptionArg(A); }))
    return;
  BadOptionWalk W(*this, TArgs);
  if (OptionType *O = W.walk(Inst))
    err(Pos, diag::err_option_bad_arg,
        {str(O->Elem),
         std::string(badOptionArg(O->Elem)) + " (in " + str(Inst) + ")"});
}

/// Whether T mentions a type parameter, so that it is a type of generic code.
static bool mentionsTypeParam(const Type *T, unsigned Depth = 0) {
  if (!T || Depth > Checker::MaxRecDepth)
    return false;
  auto Rec = [&](const Type *X) { return mentionsTypeParam(X, Depth + 1); };
  switch (T->getKind()) {
  case Type::Kind::TypeParam:
    return true;
  case Type::Kind::Pointer:
    return Rec(cast<PointerType>(T)->Elem);
  case Type::Kind::Slice:
    return Rec(cast<SliceType>(T)->Elem);
  case Type::Kind::Ref:
    return Rec(cast<RefType>(T)->Elem);
  case Type::Kind::Option:
    return Rec(cast<OptionType>(T)->Elem);
  case Type::Kind::Array:
    return Rec(cast<sema::ArrayType>(T)->Elem);
  case Type::Kind::Chan:
    return Rec(cast<sema::ChanType>(T)->Elem);
  case Type::Kind::Map:
    return Rec(cast<sema::MapType>(T)->Key) ||
           Rec(cast<sema::MapType>(T)->Elem);
  case Type::Kind::Tuple:
    return llvm::any_of(cast<TupleType>(T)->Elems, Rec);
  case Type::Kind::Signature:
    return Rec(cast<SignatureType>(T)->Params) ||
           Rec(cast<SignatureType>(T)->Results);
  case Type::Kind::Struct:
    return llvm::any_of(cast<StructType>(T)->Fields,
                        [&](const Var *F) { return Rec(F->Ty); });
  case Type::Kind::Named:
    return llvm::any_of(cast<NamedType>(T)->TArgs, Rec);
  default:
    // A basic type; an interface value's zero value does not depend on
    // the types its methods mention.
    return false;
  }
}

void Checker::checkOptionFields(SourceLoc Pos, Type *Inst) {
  // TYP-15a per instance (GEN-5): `V T @Option` is accepted on the generic
  // declaration and judged here with the real T: Box[int] is fine,
  // Box[Payment] is an error. An argument of generic code is not judged
  // (that code is checked once; the literal and zero-value rules already
  // treat the field as required where its type may have no zero value).
  auto *N = dyn_cast_or_null<NamedType>(Inst);
  if (!own() || !N || !N->Orig)
    return;
  auto IsGenericOption = [](const Var *F) {
    return F->MarkedOption && isa<TypeParamType>(F->Ty);
  };
  // The declaration first, so that only these instances are expanded.
  auto *G = dyn_cast_or_null<StructType>(under(N->Orig));
  if (!G || !G->Strict || llvm::none_of(G->Fields, IsGenericOption))
    return;
  auto *IS = dyn_cast_or_null<StructType>(under(N));
  if (!IS || IS->Fields.size() != G->Fields.size())
    return;
  for (size_t K = 0; K < G->Fields.size(); ++K) {
    const Var *F = G->Fields[K];
    Type *Y = IS->Fields[K]->Ty;
    if (!IsGenericOption(F) || !isValid(Y) || mentionsTypeParam(Y))
      continue;
    std::string Why;
    if (!hasZeroValue(Y, &Why))
      err(Pos, diag::err_option_no_zero_instance,
          {str(N), std::string(F->Name), str(Y), Why, str(F->Ty)});
  }
}

//===----------------------------------------------------------------------===//
// Signatures
//===----------------------------------------------------------------------===//

SmallVector<TypeParamType *, 4> Checker::collectTypeParams(
    const FieldList *L, Scope *Sc,
    const std::function<void(ArrayRef<TypeParamType *>)> &Bind) {
  SmallVector<TypeParamType *, 4> TPs;
  if (!L)
    return TPs;
  struct Pending {
    TypeParamType *TP;
    const ast::Expr *Bound;
    SourceLoc ExtendsLoc; // `T extends P`: the bound is P (GEN-12)
  };
  SmallVector<Pending, 4> Pend;
  unsigned Index = 0;
  for (const Field *F : L->List) {
    for (const ast::Ident *Id : F->Names) {
      TypeName *TN = newTypeName(name(Id), nullptr, Id->getBegin());
      TypeParamType *TP = Ctx.newTypeParam(TN, Index++, nullptr);
      TN->Ty = TP;
      recordDef(Id, TN);
      declare(Sc, Id, TN, Sc->Extent.getBegin());
      TPs.push_back(TP);
      Pend.push_back({TP, F->Type, F->ExtendsLoc});
    }
  }
  if (Bind)
    Bind(TPs);
  // Bounds may refer to any of the parameters.
  Type *Last = nullptr;
  const ast::Expr *LastE = nullptr;
  bool Saved = E.InTParamList;
  E.InTParamList = true;
  for (Pending &P : Pend) {
    if (P.Bound != LastE) {
      Last = P.ExtendsLoc.isValid() ? extendsBound(P.Bound, P.ExtendsLoc)
                                    : boundType(P.Bound);
      LastE = P.Bound;
    }
    P.TP->Bound = Last && isValid(Last) ? Last : Ctx.anyType();
  }
  E.InTParamList = Saved;
  return TPs;
}

void Checker::collectParams(Scope *Sc, const FieldList *L, bool IsResult,
                            SmallVector<Var *, 8> &Vars,
                            SmallVector<Type *, 8> &Types, bool &Variadic,
                            uint64_t &MoveMask, SourceLoc ScopePos) {
  if (!L)
    return;
  unsigned Count = 0;
  for (size_t FI = 0; FI < L->List.size(); ++FI) {
    const Field *F = L->List[FI];
    Type *T;
    if (auto *Dots = dyn_cast<Ellipsis>(F->Type)) {
      if (IsResult || FI + 1 != L->List.size() || F->Names.size() > 1) {
        err(Dots, diag::err_dots_not_last);
        T = Dots->Elt ? varType(Dots->Elt, true, "the type of a parameter")
                      : invalid();
      } else if (!Dots->Elt) {
        err(Dots, diag::err_invalid_use_of_dots);
        T = invalid();
      } else {
        T = Ctx.slice(varType(Dots->Elt, true, "the type of a parameter"));
        Variadic = true;
      }
    } else {
      T = varType(F->Type, true,
                  IsResult ? "the type of a result"
                           : "the type of a parameter");
    }
    size_t N = F->Names.empty() ? 1 : F->Names.size();
    for (size_t K = 0; K < N; ++K) {
      const ast::Ident *Id = F->Names.empty() ? nullptr : F->Names[K];
      StringRef PName = Id ? name(Id) : StringRef("");
      Var *V = newParam(PName, T, Id ? Id->getBegin() : F->getBegin());
      V->Result = IsResult;
      if (F->isMove() && !IsResult) {
        V->Mode = Var::MoveParam;
        if (Count < 64)
          MoveMask |= uint64_t(1) << Count;
      }
      if (Id) {
        recordDef(Id, V);
        if (PName != "_" && !PName.empty())
          declare(Sc, Id, V, ScopePos);
      } else if (!Id) {
        recordImplicit(F, V);
      }
      Vars.push_back(V);
      Types.push_back(T);
      ++Count;
    }
  }
}

Checker::FuncInfo Checker::funcSignature(const ast::FuncType *FT, Scope *Sc,
                                         Func *F) {
  FuncInfo FI;
  if (!Sc)
    Sc = Mem.newScope(E.Sc, FT->getRange(), "function");
  Scope *Saved = E.Sc;
  SourceLoc SavedPos = E.Pos;
  E.Sc = Sc;
  E.Pos = SourceLoc();
  Sc->IsFunc = true;
  SmallVector<TypeParamType *, 4> TPs;
  if (FT->TypeParams && FT->TypeParams->numFields())
    TPs = collectTypeParams(FT->TypeParams, Sc);
  SmallVector<Var *, 8> Params, Results;
  SmallVector<Type *, 8> PT, RT;
  bool Variadic = false;
  uint64_t MoveMask = 0;
  SourceLoc ScopePos = FT->getEnd();
  collectParams(Sc, FT->Params, false, Params, PT, Variadic, MoveMask,
                ScopePos);
  bool Dummy = false;
  uint64_t NoMask = 0;
  collectParams(Sc, FT->Results, true, Results, RT, Dummy, NoMask, ScopePos);
  // `func f!() error` (ERR-15) is reported by the parser.
  bool Fails = FT->Fails;
  if (Fails)
    RT.push_back(Ctx.errorType());
  E.Sc = Saved;
  E.Pos = SavedPos;
  FI.Sc = Sc;
  FI.Params = Mem.copy(ArrayRef<Var *>(Params));
  FI.Results = Mem.copy(ArrayRef<Var *>(Results));
  FI.Sig = Ctx.signature(Ctx.tuple(PT), Ctx.tuple(RT), Variadic, Fails,
                         MoveMask, Mem.copy(ArrayRef<TypeParamType *>(TPs)));
  if (F) {
    F->Fails = Fails;
    F->OwnTParams = FI.Sig->TParams;
  }
  if (own())
    checkSigOwnership(FT, FI.Sig, FI.Params, FI.Results);
  return FI;
}
