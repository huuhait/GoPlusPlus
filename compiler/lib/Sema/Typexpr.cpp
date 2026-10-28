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
      It->second->Used = true;
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
    cast<PkgName>(Obj)->Used = true;
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
      return;
    }
    X.M = Mode::Constant;
    break;
  }
  case Object::Kind::TypeName:
    if (auto *TN = cast<TypeName>(Obj);
        TN->IsAlias && !TN->AliasTParams.empty() && !E.GenericOk) {
      err(Id, diag::custom,
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
    X.M = Mode::Variable;
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
  (void)WantType;
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

Type *Checker::varType(const ast::Expr *Ex) {
  Type *T = definedType(Ex, nullptr);
  validVarType(Ex, T);
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
    err(Ex, diag::custom,
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
    Type *T = varType(Ex);
    if (!isValid(T))
      Ok = false;
    Out.push_back(T);
  }
  return Ok;
}

Type *Checker::typInternal(const ast::Expr *Ex, NamedType *Def) {
  using K = ast::Node::Kind;
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
      err(Ex, diag::custom, {str(Ex) + " used as type"});
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
      err(AT->Len, diag::custom,
          {"invalid use of [...] array (outside a composite literal)"});
      varType(AT->Elt);
      return invalid();
    }
    int64_t N = arrayLength(AT->Len);
    Type *Elem = varType(AT->Elt);
    if (N < 0)
      return invalid();
    return Ctx.array(Elem, N);
  }
  case K::SliceType:
    return Ctx.slice(varType(cast<ast::SliceType>(Ex)->Elt));
  case K::RefType: {
    auto *R = cast<ast::RefType>(Ex);
    Type *El = varType(R->Elem);
    if (!isValid(El))
      return invalid();
    Type *U = under(El);
    if (!(El == typ(BasicKind::String) || isa<SliceType>(U))) {
      err(R, diag::custom,
          {"invalid use of ref: " + str(El) + " is not a string or slice"});
      return invalid();
    }
    return Ctx.ref(El);
  }
  case K::StructType:
    return structTypeExpr(cast<ast::StructType>(Ex));
  case K::StarExpr: {
    Type *El = varType(cast<StarExpr>(Ex)->X);
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
    Type *Key = varType(M->Key);
    Type *Val = varType(M->Value);
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
    Type *El = varType(C->Value);
    return Ctx.chan(static_cast<sema::ChanType::Dir>(C->Direction), El);
  }
  case K::Ellipsis:
    err(Ex, diag::err_invalid_use_of_dots);
    return invalid();
  case K::MacroInvocation:
    err(Ex, diag::custom, {"macro invocations are expanded by P09"});
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
      err(Id, diag::custom,
          {"undefined array length " + std::string(name(Id)) +
           " or missing type constraint"});
      return -1;
    }
    if (!isa<Const>(O)) {
      err(Id, diag::custom, {"invalid array length " + std::string(name(Id))});
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

Type *Checker::structTypeExpr(const ast::StructType *St) {
  SmallVector<TypeContext::FieldSpec, 8> Fields;
  SmallVector<const ast::Ident *, 8> Idents;
  SmallVector<const ast::Expr *, 8> EmbExprs;
  llvm::StringSet<> Seen;
  auto Add = [&](const ast::Ident *Id, StringRef N, Type *T, bool Emb,
                 StringRef Tag, SourceLoc Pos, const ast::Expr *EmbE) {
    if (N != "_" && !Seen.insert(N).second) {
      err(Pos, diag::err_duplicate_field, {std::string(N)});
      return;
    }
    TypeContext::FieldSpec FS;
    FS.Name = Mem.copy(N);
    FS.Pkg = S.Pkg;
    FS.Ty = T;
    FS.Embedded = Emb;
    FS.Tag = Tag;
    FS.Pos = Pos;
    Fields.push_back(FS);
    Idents.push_back(Id);
    EmbExprs.push_back(EmbE);
  };
  if (St->Fields) {
    for (const Field *F : St->Fields->List) {
      std::string Tag;
      if (F->Tag) {
        std::string Out;
        if (unquoteString(F->Tag->Text, Out))
          Tag = Out;
        else
          err(F->Tag, diag::custom, {"invalid struct tag"});
      }
      StringRef TagRef = Mem.copy(Tag);
      if (F->Names.empty()) {
        // Embedded field: T, *T, pkg.T or T[args].
        const ast::Expr *TE = F->Type;
        Type *T = varType(TE);
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
          err(TE, diag::custom, {"invalid embedded field type " + str(TE)});
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
        Type *T = varType(F->Type);
        for (const ast::Ident *Id : F->Names)
          Add(Id, name(Id), T, false, TagRef, Id->getBegin(), nullptr);
      }
    }
  }
  StructType *ST = Ctx.structOf(Fields);
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
        err(TE, diag::custom,
            {"type in term ~" + str(TE) + " cannot be a type parameter"});
      else
        err(TE, diag::err_union_type_param);
      T.T = invalid();
    }
    if (Parts.size() == 1 && !T.Tilde)
      return T.T;
    if (Terms.size() >= MaxTermCount) {
      if (!TooMany)
        err(TE, diag::custom,
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
          err(TE, diag::custom,
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
  if (It->Methods) {
    for (const Field *F : It->Methods->List) {
      if (!F->Names.empty()) {
        const ast::Ident *Id = F->Names[0];
        StringRef N = name(Id);
        auto *FT = dyn_cast_or_null<ast::FuncType>(F->Type);
        if (N == "_") {
          err(Id, diag::custom, {"methods must have a unique non-blank name"});
          continue;
        }
        if (!FT) {
          err(F->Type, diag::custom, {"invalid method signature"});
          continue;
        }
        Func *M = newFunc(N, nullptr, Id->getBegin());
        M->IsInterfaceMethod = true;
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
  std::string Cause;
  TypeName *Alias = nullptr;
  Type *G = genericType(Ix->X, &Cause, &Alias);
  if (!isValid(G)) {
    // types2: the type arguments are not evaluated.
    if (!Cause.empty())
      err(Ix->X, diag::custom,
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
          diag::custom,
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
    later([this, APos, ATPs, AArgs, ALocs]() {
      std::string Why;
      int Bad = verify(APos, ATPs, AArgs, &Why);
      if (Bad >= 0)
        err(static_cast<size_t>(Bad) < ALocs.size() ? ALocs[Bad] : APos,
            diag::custom, {Why});
    });
    return subst(G, Alias->AliasTParams, AArgs);
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
    err(Got < Want ? Ix->Rbrack : Ix->Indices[Want]->getBegin(), diag::custom,
        {std::string(Which) + " type arguments for type " +
         std::string(N->Obj->Name) + ": have " + num(Got) + ", want " +
         num(Want)});
    NamedType *Bad = instantiateNamed(N, TArgs);
    recordInstance(Ix, TArgs, Bad);
    return Bad;
  }
  NamedType *Inst = instantiateNamed(N, TArgs);
  recordInstance(Ix, TArgs, Inst);
  SmallVector<SourceLoc, 4> Locs;
  for (const ast::Expr *A : Ix->Indices)
    Locs.push_back(A->getBegin());
  SmallVector<TypeParamType *, 4> TPs(N->TParams.begin(), N->TParams.end());
  SourceLoc Pos = Ix->getBegin();
  later([this, Pos, TPs, TArgs, Locs]() {
    std::string Why;
    int Bad = verify(Pos, TPs, TArgs, &Why);
    if (Bad >= 0)
      err(static_cast<size_t>(Bad) < Locs.size() ? Locs[Bad] : Pos,
          diag::custom, {Why});
  });
  return Inst;
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
      Pend.push_back({TP, F->Type});
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
      Last = boundType(P.Bound);
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
        T = Dots->Elt ? varType(Dots->Elt) : invalid();
      } else if (!Dots->Elt) {
        err(Dots, diag::err_invalid_use_of_dots);
        T = invalid();
      } else {
        T = Ctx.slice(varType(Dots->Elt));
        Variadic = true;
      }
    } else {
      T = varType(F->Type);
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
