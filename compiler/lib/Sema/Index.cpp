//===- Index.cpp - Index, slice and composite literal expressions ---------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's index.go and literals.go (BSD-style license,
// Copyright The Go Authors).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include <map>
#include <set>

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

bool Checker::isValidIndex(Operand &X, const char *What, bool AllowNegative) {
  if (X.M == Mode::Invalid)
    return false;
  convertUntyped(X, typ(BasicKind::Int));
  if (X.M == Mode::Invalid)
    return false;
  if (!allInteger(X.Ty)) {
    err(X, diag::err_sema_index,
        {std::string("invalid argument: ") + What + " " + str(X) +
         " must be integer"});
    return false;
  }
  if (X.M == Mode::Constant) {
    if (!AllowNegative && X.Val.sign() < 0) {
      err(X, diag::err_sema_index,
          {std::string("invalid argument: ") + What + " " + str(X) +
           " must not be negative"});
      return false;
    }
    ConstValue R;
    if (!representableConst(X.Val, typ(BasicKind::Int), &R)) {
      err(X, diag::err_sema_index,
          {std::string("invalid argument: ") + What + " " + str(X) +
           " overflows int"});
      return false;
    }
    X.Val = R;
  }
  return true;
}

/// Checks an index expression; returns the constant value or -1. Sets Valid.
int64_t Checker::index(const ast::Expr *Idx, int64_t Max) {
  Operand X;
  expr(X, Idx);
  if (!isValidIndex(X, "index", false))
    return -2; // invalid
  if (X.M != Mode::Constant)
    return -1;
  if (X.Val.isUnknown())
    return -2;
  int64_t V;
  if (!X.Val.fitsInt64(V))
    return -2;
  if (Max >= 0 && V >= Max) {
    err(X, diag::err_index_out_of_bounds, {X.Val.str(), num(Max)});
    return -2;
  }
  return V;
}

bool Checker::indexExpr(Operand &X, const IndexExpr *Ex) {
  // `pkg.vec[int32]`: a C++ template of a foreign package (ADR-0010 Q3).
  if (Object *T = foreignTemplateOf(Ex)) {
    X.E = Ex;
    X.M = Mode::Invalid;
    X.Ty = invalid();
    if (isa<TypeName>(T)) {
      if (Type *Inst = foreignInstantiate(Ex); Inst && isValid(Inst)) {
        X.M = Mode::TypeExpr;
        X.Ty = Inst;
      }
    } else {
      // In v0 a function template instance is only used in a call.
      err(Ex, diag::err_foreign_template_value, {str(Ex)});
    }
    return false;
  }
  // `A[int](x)`, `R[KM](v)`: a generic alias instantiated in an expression
  // is a type, as it is in a type (its name alone is no operand).
  // (Go mode is parked: it keeps master's answer.)
  if (auto *Id = dyn_cast<ast::Ident>(Ex->X); Id && !S.Opts.GoCompat)
    if (auto *TN = dyn_cast_or_null<TypeName>(E.Sc->lookup(name(Id), E.Pos));
        TN && TN->IsAlias && !TN->AliasTParams.empty()) {
      X.E = Ex;
      X.Ty = varType(Ex, /*ValueUse=*/false);
      X.M = isValid(X.Ty) ? Mode::TypeExpr : Mode::Invalid;
      return false;
    }
  chainOperand(X, Ex); // (chainSetup: the ADR-0024 mark)
  // ADR-0025: `x.In[KM]` with no unit value (the brackets hold a unit, not an
  // index, EXP-32), and `'a'[M]` (a unit on a rune or string literal).
  if (!S.Opts.GoCompat && Ctx.unitsSeen() &&
      ((X.M == Mode::Invalid && unitInAfterError(Ex)) ||
       (X.M == Mode::Constant && unitIndexLiteral(X, Ex))))
    return false;
  switch (X.M) {
  case Mode::Invalid:
    for (const ast::Expr *I : Ex->Indices)
      use(I);
    return false;
  case Mode::Builtin:
    if (X.BId == BuiltinId::UnitIn) {
      // `x.In[U]`: the unit is read with the unit grammar (UNI-7).
      unitInIndex(X, Ex);
      return false;
    }
    break;
  case Mode::TypeExpr:
    X.M = Mode::Invalid;
    X.Ty = varType(Ex, /*ValueUse=*/false); // a conversion decides (TYP-19)
    if (isValid(X.Ty))
      X.M = Mode::TypeExpr;
    return false;
  case Mode::Value:
    if (auto *Sig = dyn_cast<SignatureType>(under(X.Ty)))
      if (!Sig->TParams.empty())
        return true;
    break;
  default:
    break;
  }
  // ADR-0024, EXP-22: `a[i]`, `a[i, j]` on a type with `[]` methods (also
  // through a pointer, like the method call `p.M()`).
  if (X.M != Mode::Invalid && X.M != Mode::TypeExpr && !S.Opts.GoCompat &&
      Ctx.operatorsSeen() && opCandidateType(X.Ty, /*ThroughPtr=*/true) &&
      !isGeneric(X.Ty) && opIndex(X, Ex))
    return false;
  if (X.M != Mode::Invalid && X.Ty && isGeneric(X.Ty)) {
    err(Ex->X, diag::err_generic_without_instantiation,
        {"type", str(Ex->X), ""});
    X.M = Mode::Invalid;
    return false;
  }

  bool Valid = false;
  int64_t Length = -1;
  Type *U = under(X.Ty);
  // Go++ (OWN-V1): a View[string] / View[[]T] indexes like the string / slice;
  // what is read through a view is copied out or is a view again (viewRead).
  const bool BaseView = own() && isViewType(X.Ty);
  if (auto *Rf = dyn_cast<RefType>(U))
    U = under(Rf->Elem);
  auto SingleIndex = [&]() -> const ast::Expr * {
    if (Ex->Indices.empty()) {
      err(Ex, diag::err_sema_index, {"missing index for " + str(X.E)});
      return nullptr;
    }
    if (Ex->Indices.size() > 1)
      err(Ex->Indices[1], diag::err_sema_index,
          {"invalid operation: more than one index"});
    return Ex->Indices[0];
  };
  auto MapKey = [&](MapType *M) {
    const ast::Expr *Idx = SingleIndex();
    if (!Idx) {
      X.M = Mode::Invalid;
      return;
    }
    Operand Key;
    expr(Key, Idx);
    // OWN-V4: a lookup borrows the key, so a ref key costs nothing.
    bool Saved = RefToOwnedOk, SavedKey = ViewKeyLookup;
    RefToOwnedOk = ViewKeyLookup = own();
    assignment(Key, M->Key, "map index");
    RefToOwnedOk = Saved;
    ViewKeyLookup = SavedKey;
    X.M = Mode::MapIndex;
    X.Ty = BaseView ? viewRead(M->Elem) : M->Elem;
    X.E = Ex;
    // ADR-0016 point 9: a missing key would give zero bytes.
    if (!checkLookupZero(Ex, M->Elem))
      X.M = Mode::Invalid;
  };
  switch (U->getKind()) {
  case Type::Kind::Basic:
    if (isString(U)) {
      Valid = true;
      if (X.M == Mode::Constant)
        Length = static_cast<int64_t>(X.Val.stringVal().size());
      X.M = Mode::Value;
      X.Ty = Ctx.byteType();
    }
    break;
  case Type::Kind::Array: {
    auto *A = cast<ArrayType>(U);
    Valid = true;
    Length = A->Len;
    if (X.M != Mode::Variable)
      X.M = Mode::Value;
    X.Ty = BaseView ? viewRead(A->Elem) : A->Elem;
    break;
  }
  case Type::Kind::Pointer:
    if (auto *A = dyn_cast<ArrayType>(under(cast<PointerType>(U)->Elem))) {
      Valid = true;
      Length = A->Len;
      X.M = Mode::Variable;
      X.Ty = A->Elem;
    }
    break;
  case Type::Kind::Slice:
    Valid = true;
    X.M = Mode::Variable;
    X.Ty = BaseView ? viewRead(cast<SliceType>(U)->Elem)
                    : cast<SliceType>(U)->Elem;
    break;
  case Type::Kind::Map:
    MapKey(cast<MapType>(U));
    return false;
  case Type::Kind::Interface: {
    if (!isTypeParam(X.Ty))
      break;
    Type *Key = nullptr, *Elem = nullptr;
    Mode M = Mode::Variable;
    bool Ok = underIs(X.Ty, [&](Type *Uu) {
      int64_t L = -1;
      Type *K = nullptr, *El = nullptr;
      if (!Uu)
        return false;
      switch (Uu->getKind()) {
      case Type::Kind::Basic:
        if (isString(Uu)) {
          El = Ctx.byteType();
          M = Mode::Value;
        }
        break;
      case Type::Kind::Array:
        L = cast<ArrayType>(Uu)->Len;
        El = cast<ArrayType>(Uu)->Elem;
        if (X.M != Mode::Variable)
          M = Mode::Value;
        break;
      case Type::Kind::Pointer:
        if (auto *A = dyn_cast<ArrayType>(under(cast<PointerType>(Uu)->Elem))) {
          L = A->Len;
          El = A->Elem;
        }
        break;
      case Type::Kind::Slice:
        El = cast<SliceType>(Uu)->Elem;
        break;
      case Type::Kind::Map:
        K = cast<MapType>(Uu)->Key;
        El = cast<MapType>(Uu)->Elem;
        break;
      default:
        break;
      }
      if (!El)
        return false;
      if (!Elem) {
        Length = L;
        Key = K;
        Elem = El;
        return true;
      }
      if ((Key == nullptr) != (K == nullptr) || (Key && !identical(Key, K)))
        return false;
      if (!identical(Elem, El))
        return false;
      if (L >= 0 && L < Length)
        Length = L;
      return true;
    });
    if (Ok) {
      if (Key) {
        const ast::Expr *Idx = SingleIndex();
        if (!Idx) {
          X.M = Mode::Invalid;
          return false;
        }
        Operand K;
        expr(K, Idx);
        assignment(K, Key, "map index");
        X.M = Mode::MapIndex;
        X.Ty = Elem;
        X.E = Ex;
        if (!checkLookupZero(Ex, Elem))
          X.M = Mode::Invalid;
        return false;
      }
      Valid = true;
      X.M = M;
      X.Ty = Elem;
    }
    break;
  }
  default:
    break;
  }
  if (!Valid) {
    err(Ex, diag::err_cannot_index, {str(X)});
    for (const ast::Expr *I : Ex->Indices)
      use(I);
    X.M = Mode::Invalid;
    return false;
  }
  const ast::Expr *Idx = SingleIndex();
  if (!Idx) {
    X.M = Mode::Invalid;
    return false;
  }
  if (!X.Ty)
    X.Ty = invalid();
  index(Idx, Length);
  return false;
}

void Checker::sliceExpr(Operand &X, const SliceExpr *Ex) {
  chainOperand(X, Ex);
  if (X.M == Mode::Invalid) {
    use(Ex->Lo);
    use(Ex->Hi);
    use(Ex->Max);
    return;
  }
  bool Valid = false;
  int64_t Length = -1;
  Type *Orig = X.Ty;
  bool OrigIsRef = isa<RefType>(Orig);
  // The operand may not be written: a View, or a part of one (OWN-V2).
  bool InView = false;
  if (own() && !isViewType(Orig)) {
    InView = viewPlace(Ex->X);
    SliceInView[Ex] = InView; // (viewPlace of a slice chain asks it)
  }
  const bool ReadOnlySrc = own() && (isViewType(Orig) || InView);
  Type *U = nullptr;
  if (auto *TP = dyn_cast<TypeParamType>(X.Ty)) {
    // types2: all types of the type set slice alike; a string counts as a
    // []byte.
    Type *CT = nullptr, *CU = nullptr;
    bool HasString = false;
    std::string Why;
    typeParamIs(TP, [&](const Term *Tm) {
      if (!Tm) {
        Why = "no specific type in " + str(X.Ty);
        return false;
      }
      Type *Tu = under(Tm->T);
      if (isString(Tu)) {
        Tu = Ctx.slice(Ctx.byteType());
        HasString = true;
      }
      if (!CU) {
        CT = Tm->T;
        CU = Tu;
        return true;
      }
      if (!identical(CU, Tu)) {
        Why =
            str(CT) + " and " + str(Tm->T) + " have different underlying types";
        return false;
      }
      return true;
    });
    if (!Why.empty() || !CU) {
      err(X, diag::err_sema_index,
          {"cannot slice " + str(X) + ": " +
           (Why.empty() ? "no specific type in " + str(X.Ty) : Why)});
      X.M = Mode::Invalid;
      return;
    }
    U = HasString ? typ(BasicKind::String) : CU;
  } else {
    U = under(X.Ty);
    // Go++ (SLC-8, OWN-V1): a ref string / ref []T slices like its target and
    // the result is a ref again.
    if (auto *Rf = dyn_cast<RefType>(U)) {
      U = under(Rf->Elem);
      OrigIsRef = true;
    }
  }
  switch (U->getKind()) {
  case Type::Kind::Basic:
    if (isString(U)) {
      if (Ex->Slice3) {
        err(Ex->Max ? Ex->Max->getBegin() : Ex->getBegin(),
            diag::err_slice_3_string);
        X.M = Mode::Invalid;
        return;
      }
      Valid = true;
      if (X.M == Mode::Constant)
        Length = static_cast<int64_t>(X.Val.stringVal().size());
      if (isUntyped(X.Ty))
        X.Ty = typ(BasicKind::String);
      // Go++ (SLC-8): slicing makes a ref.
      if (own() && (X.Ty == typ(BasicKind::String) || OrigIsRef))
        X.Ty = Ctx.ref(typ(BasicKind::String));
    }
    break;
  case Type::Kind::Array: {
    auto *A = cast<ArrayType>(U);
    Valid = true;
    Length = A->Len;
    if (X.M != Mode::Variable) {
      err(X, diag::err_cannot_slice_unaddressable, {str(X)});
      X.M = Mode::Invalid;
      return;
    }
    X.Ty = Ctx.slice(A->Elem);
    if (own()) {
      X.Ty = Ctx.ref(X.Ty, !ReadOnlySrc);
      // The ref points into the array itself (OWN-P10).
      rejectPayloadAddress(Ex->X, "slicing an array held by value");
      noteReference(Ex->X);
    }
    break;
  }
  case Type::Kind::Pointer:
    if (auto *A = dyn_cast<ArrayType>(under(cast<PointerType>(U)->Elem))) {
      Valid = true;
      Length = A->Len;
      X.Ty = Ctx.slice(A->Elem);
    }
    break;
  case Type::Kind::Slice:
    Valid = true;
    if (own() && (isa<SliceType>(X.Ty) || OrigIsRef))
      X.Ty = Ctx.ref(cast<SliceType>(U), !ReadOnlySrc);
    break;
  default:
    break;
  }
  if (!Valid) {
    err(X, diag::err_cannot_slice, {str(X)});
    X.M = Mode::Invalid;
    return;
  }
  X.M = Mode::Value;
  const ast::Expr *Idx[3] = {Ex->Lo, Ex->Hi, Ex->Max};
  if (Ex->Slice3 && (!Ex->Hi || !Ex->Max)) {
    err(Ex, diag::err_sema_index,
        {"2nd and 3rd index required in 3-index slice"});
    X.M = Mode::Invalid;
    return;
  }
  int64_t Ind[3];
  for (int I = 0; I < 3; ++I) {
    int64_t V = -1;
    if (Idx[I]) {
      int64_t Max = Length >= 0 ? Length + 1 : -1;
      int64_t R = index(Idx[I], Max);
      if (R >= 0)
        V = R;
    } else if (I == 0) {
      V = 0;
    } else if (Length >= 0) {
      V = Length;
    }
    Ind[I] = V;
  }
  for (int I = 0; I < 2; ++I) {
    if (Ind[I] > 0) {
      for (int J = I + 1; J < 3; ++J) {
        if (Ind[J] >= 0 && Ind[J] < Ind[I]) {
          err(Idx[J], diag::err_slice_indices_inverted,
              {std::to_string(Ind[J]), std::to_string(Ind[I])});
          return;
        }
      }
    }
  }
}

int64_t Checker::indexedElts(ArrayRef<ast::Expr *> Elts, Type *Elem,
                             int64_t Length, int64_t *NumSet) {
  std::set<int64_t> Visited;
  int64_t Index = 0, Max = 0;
  for (const ast::Expr *Ex : Elts) {
    bool ValidIndex = false;
    const ast::Expr *Eval = Ex;
    if (auto *KV = dyn_cast<KeyValueExpr>(Ex)) {
      int64_t R = index(KV->Key, Length);
      if (R != -2) {
        if (R >= 0) {
          Index = R;
          ValidIndex = true;
        } else {
          err(Ex, diag::err_literal_index_not_constant, {str(KV->Key)});
        }
      }
      Eval = KV->Value;
    } else if (Length >= 0 && Index >= Length) {
      err(Ex, diag::err_literal_array_too_long, {num(Index), num(Length)});
    } else {
      ValidIndex = true;
    }
    if (ValidIndex) {
      if (Visited.count(Index))
        err(Ex, diag::err_literal_duplicate_index, {num(Index)});
      Visited.insert(Index);
    }
    ++Index;
    if (Index > Max)
      Max = Index;
    Operand X;
    exprWithHint(X, Eval, Elem);
    assignment(X, Elem, "array or slice literal");
    storeLiteralElem(X, Elem);
  }
  if (NumSet)
    *NumSet = static_cast<int64_t>(Visited.size());
  return Max;
}

void Checker::storeLiteralElem(Operand &X, Type *T) {
  if (!own() || X.M == Mode::Invalid)
    return;
  // A composite literal owns its elements (OWN-R3a, EXP-10); a ref element
  // must not point into a temporary (OWN-V6).
  if (isa<RefType>(T))
    checkRefStore(X.E, T, X.Ty);
  else
    needOwnership(X, "store", T);
}

void Checker::compositeLit(Operand &X, const CompositeLit *Ex, Type *Hint) {
  Type *Typ = nullptr, *Base = nullptr;
  bool IsElem = false;
  bool OpenArray = false;
  if (Ex->Type) {
    auto *AT = dyn_cast<ast::ArrayType>(Ex->Type);
    if (AT && AT->Len && isa<Ellipsis>(AT->Len)) {
      Typ = Ctx.array(varType(AT->Elt, true, "an element type"), -1);
      OpenArray = true;
      Base = Typ;
    } else {
      Typ = typ(Ex->Type);
      if (auto *NT = dyn_cast<NamedType>(Typ);
          NT && !NT->Orig && !NT->Underlying && NT->Obj &&
          NT->Obj->Color >= 2 &&
          static_cast<size_t>(NT->Obj->Color - 2) < ObjPath.size()) {
        // `type T [len(T{})]int`: the literal needs the type being declared.
        if (!ReportedCycle.count(NT)) {
          ReportedCycle.insert(NT);
          cycleError(ArrayRef<Object *>(ObjPath).slice(NT->Obj->Color - 2));
        }
        Typ = invalid();
      }
      Base = Typ;
    }
  } else if (Hint) {
    Typ = Hint;
    Base = Typ;
    Type *Core = coreType(Base);
    if (Core)
      if (auto *P = dyn_cast<PointerType>(Core))
        Base = P->Elem;
    IsElem = true;
  } else {
    err(Ex, diag::err_literal_missing_type);
    Typ = invalid();
    Base = Typ;
  }
  Type *CoreT = coreType(Base);
  ArrayType *OpenArr = nullptr;
  if (OpenArray) {
    // [...]T: the length is set below; keep a mutable array by re-making it.
    OpenArr = cast<ArrayType>(Typ);
  }
  auto Done = [&]() {
    X.M = Mode::Value;
    X.Ty = Typ;
  };
  if (CoreT && isa<StructType>(CoreT)) {
    auto *St = cast<StructType>(CoreT);
    ArrayRef<Var *> Fields = St->Fields;
    // TYP-19: a standalone value of an abstract type; the parent part of a
    // child's literal (`Card{Payment: Payment{...}}`) is not one.
    if (Ex != ParentPartLit)
      noAbstract(Ex, Typ, "a composite literal");
    const bool Extends =
        !S.Opts.GoCompat && !Fields.empty() && Fields[0]->IsParent;
    // The fields set by name, directly or through the parent chain (TYP-17).
    // A field of the ancestor Depth levels up covers the Depth parent fields
    // on the way: only the numbers are kept (MaxDepth the deepest field set,
    // ParentSet the parent fields set as a whole by their level), so a
    // literal costs no walk over the chain.
    llvm::DenseSet<const Var *> SetVars;
    unsigned MaxDepth = 0;
    std::map<unsigned, Var *> ParentSet;
    constexpr unsigned NoParentSet = ~0u;
    // TYP-15: every field without a zero value (a `@Required` field, a
    // pointer, a nested type with none) must be set, by name or by position.
    // A parent part that is not set as a whole is looked into (TYP-17).
    auto CheckRequired = [&](std::vector<bool> Set,
                             llvm::DenseSet<const Var *> Keys,
                             unsigned FirstParentSet) {
      if (!own() || !isValid(Typ))
        return;
      SourceLoc At = Ex->Rbrace;
      NamedType *NT = dyn_cast<NamedType>(Typ);
      auto Run = [this, St, Set, Keys, FirstParentSet, At, Base, NT,
                  Extends]() {
        std::string Names;
        unsigned N = 0;
        auto Add = [&](Var *F) {
          Names += (N++ ? ", " : "") + std::string(F->Name);
        };
        // Once the types are final the unset parents are walked by the
        // ancestors that have a field to set (a memo per type), not by all.
        const uint32_t DN = Extends && NT && typesFinal() ? hierDepth(NT) : 0;
        // Work list of structs (the literal's, then its unset parents').
        SmallVector<std::pair<StructType *, bool>, 4> Work{{St, true}};
        while (!Work.empty()) {
          auto [Cur, Top] = Work.pop_back_val();
          for (size_t I = 0; I < Cur->Fields.size(); ++I) {
            Var *F = Cur->Fields[I];
            if (Top ? (I < Set.size() && Set[I]) : Keys.count(F))
              continue;
            if (F->IsParent) {
              if (!DN)
                if (auto *PS = dyn_cast_or_null<StructType>(under(F->Ty)))
                  Work.push_back({PS, false});
              continue;
            }
            if (!F->Required && hasZeroValue(F->Ty))
              continue;
            Add(F);
          }
        }
        if (DN) {
          for (NamedType *Y = requiredAncestor(parentOf(NT)); Y;
               Y = requiredAncestor(parentOf(Y))) {
            uint32_t DY = hierDepth(Y);
            if (!DY || DY > DN || DN - DY > FirstParentSet)
              break;
            for (Var *F : cast<StructType>(under(Y))->Fields)
              if (!F->IsParent && !Keys.count(F) &&
                  (F->Required || !hasZeroValue(F->Ty)))
                Add(F);
          }
        }
        if (N)
          err(At, diag::err_required_missing,
              {std::string(N == 1 ? "field " : "fields ") + Names, str(Base)});
      };
      if (Bodies)
        Run();
      else
        later(Run);
    };
    if (Ex->Elts.empty()) {
      CheckRequired(std::vector<bool>(Fields.size()), {}, NoParentSet);
      return Done();
    }
    if (isa<KeyValueExpr>(Ex->Elts[0])) {
      std::vector<bool> Visited(Fields.size());
      struct Finish {
        std::function<void()> F;
        ~Finish() { F(); }
      } Fin{[&]() {
        CheckRequired(Visited, SetVars,
                      ParentSet.empty() ? NoParentSet
                                        : ParentSet.begin()->first);
      }};
      for (const ast::Expr *El : Ex->Elts) {
        auto *KV = dyn_cast<KeyValueExpr>(El);
        if (!KV) {
          err(El, diag::err_literal_mixture);
          continue;
        }
        auto *Key = dyn_cast<ast::Ident>(KV->Key);
        // The field the key names: a direct field, or (TYP-17) one of an
        // ancestor, with the parent fields on the way.
        Var *Fld = nullptr, *AltFld = nullptr;
        unsigned FldDepth = 0;
        int Idx = -1;
        if (Key) {
          for (size_t I = 0; I < Fields.size(); ++I) {
            if (sameId(Fields[I], S.Pkg, name(Key), false)) {
              Fld = Fields[I];
              Idx = static_cast<int>(I);
            }
            if (!AltFld && sameId(Fields[I], S.Pkg, name(Key), true))
              AltFld = Fields[I];
          }
          // Not a field of the literal's own struct: an ancestor's, found by
          // the memoized chain lookup, or (what it cannot answer) a walk.
          if (!Fld && Extends) {
            LookupResult CR;
            auto *NT = dyn_cast<NamedType>(Typ);
            if (NT && chainLookup(NT, false, true, S.Pkg, name(Key), CR) &&
                CR.Obj && isa<Var>(CR.Obj)) {
              Fld = cast<Var>(CR.Obj);
              FldDepth = CR.ParentSteps;
            } else {
              StructType *Cur = St;
              for (unsigned Depth = 0; Cur && !Fld; ++Depth) {
                for (size_t I = 0; I < Cur->Fields.size(); ++I) {
                  if (sameId(Cur->Fields[I], S.Pkg, name(Key), false)) {
                    Fld = Cur->Fields[I];
                    FldDepth = Depth;
                  }
                  if (!AltFld && sameId(Cur->Fields[I], S.Pkg, name(Key), true))
                    AltFld = Cur->Fields[I];
                }
                if (Fld || Cur->Fields.empty() || !Cur->Fields[0]->IsParent)
                  break;
                Cur = dyn_cast_or_null<StructType>(under(Cur->Fields[0]->Ty));
              }
            }
          }
        }
        // The parent part is no standalone value (TYP-19).
        const CompositeLit *SavedPP = ParentPartLit;
        ParentPartLit = nullptr;
        if (Fld && Fld->IsParent)
          if (auto *PL = dyn_cast<CompositeLit>(unparen(KV->Value)))
            ParentPartLit = PL;
        Operand V;
        expr(V, KV->Value);
        ParentPartLit = SavedPP;
        if (!Key) {
          err(KV, diag::err_literal_invalid_field_name, {str(KV->Key)});
          continue;
        }
        if (!Fld) {
          std::string Msg = lookupError(Base, name(Key), AltFld, true);
          err(KV->Key, diag::err_sema_index, {Msg});
          continue;
        }
        recordUse(Key, Fld);
        if (V.isNil() && isNeverNilField(Fld))
          err(V, diag::err_required_nil_store,
              {str(Base) + "." + std::string(Fld->Name), requiredWhy(Fld)});
        else
          checkNeverNilStore(V, Fld, str(Base) + "." + std::string(Fld->Name));
        if (!checkPointerField(V, Fld->Ty)) {
          assignment(V, Fld->Ty, "struct literal");
          storeLiteralElem(V, Fld->Ty);
        }
        if (Idx >= 0 && Visited[Idx]) {
          err(KV, diag::err_literal_duplicate_field, {std::string(name(Key))});
          continue;
        }
        if (Idx < 0 && SetVars.count(Fld)) {
          err(KV, diag::err_literal_duplicate_field, {std::string(name(Key))});
          continue;
        }
        // The same field both directly and through its parent is an error.
        const Var *Twice = nullptr;
        if (Extends) {
          // A field of an ancestor set before covers this parent field; a
          // parent field set as a whole covers this one (the deepest wins).
          if (Fld->IsParent && MaxDepth > FldDepth)
            Twice = Fld;
          auto It = ParentSet.lower_bound(FldDepth);
          if (It != ParentSet.begin())
            Twice = std::prev(It)->second;
          if (Twice)
            err(KV, diag::err_extends_literal_twice,
                {str(Base), std::string(Twice->Name)});
        }
        if (Idx >= 0)
          Visited[Idx] = true;
        SetVars.insert(Fld);
        MaxDepth = std::max(MaxDepth, FldDepth);
        if (Fld->IsParent)
          ParentSet.emplace(FldDepth, Fld);
      }
    } else {
      if (Extends) {
        for (const ast::Expr *El : Ex->Elts)
          use(El);
        err(Ex->Elts[0], diag::err_extends_literal_positional, {str(Base)});
        return Done();
      }
      for (size_t I = 0; I < Ex->Elts.size(); ++I) {
        const ast::Expr *El = Ex->Elts[I];
        if (isa<KeyValueExpr>(El)) {
          err(El, diag::err_literal_mixture);
          continue;
        }
        Operand V;
        expr(V, El);
        if (I >= Fields.size()) {
          err(V, diag::err_literal_too_many, {str(Base)});
          break;
        }
        Var *Fld = Fields[I];
        if (!Fld->exported() && Fld->Pkg != S.Pkg) {
          err(V, diag::err_literal_unexported_field,
              {std::string(Fld->Name), str(Base)});
          continue;
        }
        if (V.isNil() && isNeverNilField(Fld))
          err(V, diag::err_required_nil_store,
              {str(Base) + "." + std::string(Fld->Name), requiredWhy(Fld)});
        else
          checkNeverNilStore(V, Fld, str(Base) + "." + std::string(Fld->Name));
        if (!checkPointerField(V, Fld->Ty)) {
          assignment(V, Fld->Ty, "struct literal");
          storeLiteralElem(V, Fld->Ty);
        }
      }
      if (Ex->Elts.size() < Fields.size())
        err(Ex->Rbrace, diag::err_literal_too_few, {str(Base)});
    }
  } else if (CoreT && isa<ArrayType>(CoreT)) {
    auto *A = cast<ArrayType>(CoreT);
    int64_t NumSet = 0;
    int64_t N = indexedElts(Ex->Elts, A->Elem, A->Len, &NumSet);
    // A literal that leaves elements out makes zero values for them.
    if (own() && NumSet < (A->Len < 0 ? N : A->Len))
      needZeroValue(Ex->Rbrace, A->Elem, "array literal with missing elements");
    if (A->Len < 0) {
      Typ = Ctx.array(A->Elem, N);
      if (OpenArr && Ex->Type)
        recordTypeAndValue(Ex->Type, Mode::TypeExpr, Typ, nullptr);
    }
  } else if (CoreT && isa<SliceType>(CoreT)) {
    int64_t NumSet = 0;
    int64_t N =
        indexedElts(Ex->Elts, cast<SliceType>(CoreT)->Elem, -1, &NumSet);
    if (own() && NumSet < N)
      needZeroValue(Ex->Rbrace, cast<SliceType>(CoreT)->Elem,
                    "slice literal with gaps");
  } else if (CoreT && isa<MapType>(CoreT)) {
    auto *M = cast<MapType>(CoreT);
    bool KeyIsInterface = isNonTypeParamInterface(M->Key);
    SmallVector<std::pair<ConstValue, Type *>, 8> Visited;
    for (const ast::Expr *El : Ex->Elts) {
      auto *KV = dyn_cast<KeyValueExpr>(El);
      if (!KV) {
        err(El, diag::err_literal_missing_key);
        continue;
      }
      Operand K;
      exprWithHint(K, KV->Key, M->Key);
      assignment(K, M->Key, "map literal");
      if (K.M == Mode::Invalid)
        continue;
      storeLiteralElem(K, M->Key);
      if (K.M == Mode::Constant) {
        bool Dup = false;
        for (auto &[V, T] : Visited)
          // compare matches the kinds, so 1, 1.0 and 1+0i are the same key
          // (types2's keyVal), as when the key is a type parameter.
          if (ConstValue::compare(V, tok::eql, K.Val) &&
              (!KeyIsInterface || identical(T, K.Ty))) {
            Dup = true;
            break;
          }
        Visited.push_back({K.Val, K.Ty});
        if (Dup) {
          err(K, diag::err_literal_duplicate_key, {K.Val.str()});
          continue;
        }
      }
      Operand V;
      exprWithHint(V, KV->Value, M->Elem);
      assignment(V, M->Elem, "map literal");
      storeLiteralElem(V, M->Elem);
    }
  } else {
    for (const ast::Expr *El : Ex->Elts) {
      if (auto *KV = dyn_cast<KeyValueExpr>(El))
        El = KV->Value;
      use(El);
    }
    if (!CoreT || isValid(CoreT)) {
      err(Ex, diag::err_sema_index,
          {std::string("invalid composite literal") +
           (IsElem ? " element" : "") + " type " + str(Typ) +
           (CoreT ? "" : " (no common underlying type)")});
      X.M = Mode::Invalid;
      return;
    }
  }
  Done();
}
