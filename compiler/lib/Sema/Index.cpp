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

#include <set>

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

bool Checker::isValidIndex(Operand &X, const char *What, bool AllowNegative) {
  if (X.M == Mode::Invalid)
    return false;
  convertUntyped(X, typ(BasicKind::Int));
  if (X.M == Mode::Invalid)
    return false;
  if (!allInteger(X.Ty)) {
    err(X, diag::custom,
        {std::string("invalid argument: ") + What + " " + str(X) +
         " must be integer"});
    return false;
  }
  if (X.M == Mode::Constant) {
    if (!AllowNegative && X.Val.sign() < 0) {
      err(X, diag::custom,
          {std::string("invalid argument: ") + What + " " + str(X) +
           " must not be negative"});
      return false;
    }
    ConstValue R;
    if (!representableConst(X.Val, typ(BasicKind::Int), &R)) {
      err(X, diag::custom,
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
  exprOrType(X, Ex->X, true);
  switch (X.M) {
  case Mode::Invalid:
    for (const ast::Expr *I : Ex->Indices)
      use(I);
    return false;
  case Mode::TypeExpr:
    X.M = Mode::Invalid;
    X.Ty = varType(Ex);
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
  if (X.M != Mode::Invalid && X.Ty && isGeneric(X.Ty)) {
    err(Ex->X, diag::err_generic_without_instantiation,
        {"type", str(Ex->X), ""});
    X.M = Mode::Invalid;
    return false;
  }

  bool Valid = false;
  int64_t Length = -1;
  Type *U = under(X.Ty);
  // Go++ (OWN-V1): a ref string / ref []T indexes like the string / slice.
  if (auto *Rf = dyn_cast<RefType>(U))
    U = under(Rf->Elem);
  auto SingleIndex = [&]() -> const ast::Expr * {
    if (Ex->Indices.empty()) {
      err(Ex, diag::custom, {"missing index for " + str(X.E)});
      return nullptr;
    }
    if (Ex->Indices.size() > 1)
      err(Ex->Indices[1], diag::custom,
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
    bool Saved = RefToOwnedOk;
    RefToOwnedOk = own();
    assignment(Key, M->Key, "map index");
    RefToOwnedOk = Saved;
    X.M = Mode::MapIndex;
    X.Ty = M->Elem;
    X.E = Ex;
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
    X.Ty = A->Elem;
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
    X.Ty = cast<SliceType>(U)->Elem;
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
  expr(X, Ex->X);
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
      err(X, diag::custom,
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
    if (own())
      X.Ty = Ctx.ref(X.Ty);
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
      X.Ty = Ctx.ref(cast<SliceType>(U));
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
    err(Ex, diag::custom, {"2nd and 3rd index required in 3-index slice"});
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
                             int64_t Length) {
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
      Typ = Ctx.array(varType(AT->Elt), -1);
      OpenArray = true;
      Base = Typ;
    } else {
      Typ = typ(Ex->Type);
      if (auto *NT = dyn_cast<NamedType>(Typ);
          NT && !NT->Underlying && !NT->Orig && NT->Obj &&
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
    if (Ex->Elts.empty())
      return Done();
    ArrayRef<Var *> Fields = St->Fields;
    if (isa<KeyValueExpr>(Ex->Elts[0])) {
      std::vector<bool> Visited(Fields.size());
      for (const ast::Expr *El : Ex->Elts) {
        auto *KV = dyn_cast<KeyValueExpr>(El);
        if (!KV) {
          err(El, diag::err_literal_mixture);
          continue;
        }
        auto *Key = dyn_cast<ast::Ident>(KV->Key);
        Operand V;
        expr(V, KV->Value);
        if (!Key) {
          err(KV, diag::err_literal_invalid_field_name, {str(KV->Key)});
          continue;
        }
        int Idx = -1, Alt = -1;
        for (size_t I = 0; I < Fields.size(); ++I) {
          if (sameId(Fields[I], S.Pkg, name(Key), false))
            Idx = static_cast<int>(I);
          if (sameId(Fields[I], S.Pkg, name(Key), true))
            Alt = static_cast<int>(I);
        }
        if (Idx < 0) {
          std::string Msg = "unknown field " + std::string(name(Key)) +
                            " in struct literal of type " + str(Base);
          if (Alt >= 0)
            Msg += ", but does have " + std::string(Fields[Alt]->Name);
          err(KV->Key, diag::custom, {Msg});
          continue;
        }
        Var *Fld = Fields[Idx];
        recordUse(Key, Fld);
        if (!checkPointerField(V, Fld->Ty)) {
          assignment(V, Fld->Ty, "struct literal");
          storeLiteralElem(V, Fld->Ty);
        }
        if (Visited[Idx]) {
          err(KV, diag::err_literal_duplicate_field, {std::string(name(Key))});
          continue;
        }
        Visited[Idx] = true;
      }
    } else {
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
    int64_t N = indexedElts(Ex->Elts, A->Elem, A->Len);
    if (A->Len < 0) {
      Typ = Ctx.array(A->Elem, N);
      if (OpenArr && Ex->Type)
        recordTypeAndValue(Ex->Type, Mode::TypeExpr, Typ, nullptr);
    }
  } else if (CoreT && isa<SliceType>(CoreT)) {
    indexedElts(Ex->Elts, cast<SliceType>(CoreT)->Elem, -1);
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
          if (V.kind() == K.Val.kind() &&
              ConstValue::compare(V, tok::eql, K.Val) &&
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
      err(Ex, diag::custom,
          {std::string("invalid composite literal") +
           (IsElem ? " element" : "") + " type " + str(Typ) +
           (CoreT ? "" : " (no common underlying type)")});
      X.M = Mode::Invalid;
      return;
    }
  }
  Done();
}
