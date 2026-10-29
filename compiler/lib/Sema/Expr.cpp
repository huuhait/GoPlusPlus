//===- Expr.cpp - Expressions ---------------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's expr.go (BSD-style license, Copyright The Go Authors).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

static constexpr unsigned bit(Mode M) { return 1u << static_cast<unsigned>(M); }

bool Checker::isComparison(tok::Kind Op) {
  return Op == tok::eql || Op == tok::neq || Op == tok::lss || Op == tok::leq ||
         Op == tok::gtr || Op == tok::geq;
}
static bool isShiftOp(tok::Kind Op) { return Op == tok::shl || Op == tok::shr; }

//===----------------------------------------------------------------------===//
// Recording
//===----------------------------------------------------------------------===//

void Checker::recordTypeAndValue(const ast::Expr *Ex, Mode M, Type *T,
                                 const ConstValue *V) {
  if (M == Mode::Invalid)
    return;
  TypeAndValue &TV = I.Types[Ex];
  TV.Ty = T;
  TV.Val = V;
  TV.M = M;
}

void Checker::rememberUntyped(const ast::Expr *Ex, bool Lhs, Mode M, Type *T,
                              const ConstValue &V) {
  ExprInfo &Info = Untyped[Ex];
  Info.IsLhs = Lhs;
  Info.M = M;
  Info.Ty = T;
  Info.Val = V;
}

void Checker::recordUntyped() {
  for (auto &[Ex, Info] : Untyped) {
    const ConstValue *V =
        Info.M == Mode::Constant ? Mem.constant(Info.Val) : nullptr;
    recordTypeAndValue(Ex, Info.M, Info.Ty, V);
  }
  Untyped.clear();
}

void Checker::updateExprVal(const ast::Expr *Ex, const ConstValue &V) {
  auto It = Untyped.find(Ex);
  if (It != Untyped.end())
    It->second.Val = V;
}

void Checker::updateExprType(const ast::Expr *Ex, Type *T, bool Final) {
  if (!Ex)
    return;
  auto It = Untyped.find(Ex);
  if (It == Untyped.end())
    return;
  ExprInfo Old = It->second;
  switch (Ex->getKind()) {
  case Node::Kind::ParenExpr:
    updateExprType(cast<ParenExpr>(Ex)->X, T, Final);
    break;
  case Node::Kind::UnaryExpr:
    if (Old.M == Mode::Constant)
      break;
    updateExprType(cast<UnaryExpr>(Ex)->X, T, Final);
    break;
  case Node::Kind::BinaryExpr: {
    auto *B = cast<BinaryExpr>(Ex);
    if (Old.M == Mode::Constant)
      break;
    if (isComparison(B->Op)) {
    } else if (isShiftOp(B->Op)) {
      updateExprType(B->X, T, Final);
    } else {
      updateExprType(B->X, T, Final);
      updateExprType(B->Y, T, Final);
    }
    break;
  }
  case Node::Kind::CallExpr:
  case Node::Kind::Ident:
  case Node::Kind::BasicLit:
  case Node::Kind::SelectorExpr:
    break;
  default:
    return;
  }
  if (!Final && isUntyped(T)) {
    auto It2 = Untyped.find(Ex);
    if (It2 != Untyped.end())
      It2->second.Ty = under(T);
    return;
  }
  Untyped.erase(Ex);
  if (Old.IsLhs) {
    if (!allInteger(T)) {
      err(Ex, diag::err_shift_operand_invalid,
          {str(Ex) + " (type " + str(T) + ")"});
      return;
    }
  }
  if (Old.M == Mode::Constant) {
    Operand C;
    C.M = Old.M;
    C.E = Ex;
    C.Ty = Old.Ty;
    C.Val = Old.Val;
    convertUntyped(C, T);
    if (C.M == Mode::Invalid)
      return;
    Old.Val = C.Val;
  }
  recordTypeAndValue(Ex, Old.M, T,
                     Old.M == Mode::Constant ? Mem.constant(Old.Val) : nullptr);
}

void Checker::convertUntyped(Operand &X, Type *Target) {
  ConstValue NV;
  ReprCode Code;
  Type *NewType = implicitTypeAndValue(X, Target, &NV, &Code);
  if (!NewType) {
    Type *T = Target;
    if (!isTypeParam(Target))
      T = under(Target);
    invalidConversion(Code, X, T);
    X.M = Mode::Invalid;
    return;
  }
  if (X.M == Mode::Constant && !NV.isUnknown() && NewType != X.Ty) {
    X.Val = NV;
    updateExprVal(X.E, NV);
  } else if (X.M == Mode::Constant && Code == ReprCode::Ok &&
             isTyped(NewType) && !NV.isUnknown()) {
    X.Val = NV;
    updateExprVal(X.E, NV);
  }
  if (NewType != X.Ty) {
    X.Ty = NewType;
    updateExprType(X.E, NewType, false);
  }
}

//===----------------------------------------------------------------------===//
// Evaluation entry points
//===----------------------------------------------------------------------===//

ExprKind Checker::rawExpr(Operand &X, const ast::Expr *Ex, Type *Hint,
                          bool AllowGeneric) {
  ExprKind Kind = exprInternal(X, Ex, Hint);
  // Reverse type inference: a generic function used where a function type is
  // expected is instantiated from that type.
  if (!AllowGeneric && Hint && X.M != Mode::Invalid && X.M != Mode::NoValue)
    if (auto *Sg = dyn_cast_or_null<SignatureType>(X.Ty))
      if (!Sg->TParams.empty())
        if (auto *Tg = dyn_cast<SignatureType>(under(Hint)))
          if (Tg->TParams.empty())
            funcInst(X, Ex->getBegin(), nullptr, true, Hint);
  if (!AllowGeneric && X.M != Mode::Invalid && X.M != Mode::NoValue) {
    const char *What = nullptr;
    if (isGeneric(X.Ty))
      What = "type";
    else if (auto *Sg = dyn_cast<SignatureType>(X.Ty))
      if (!Sg->TParams.empty())
        What = "function";
    if (What) {
      err(Ex, diag::err_generic_without_instantiation, {What, str(Ex), ""});
      X.M = Mode::Invalid;
      X.Ty = invalid();
    }
  }
  // record
  switch (X.M) {
  case Mode::Invalid:
    break;
  default: {
    Type *T = X.Ty;
    if (X.M == Mode::NoValue)
      T = nullptr;
    if (T && isUntyped(T))
      rememberUntyped(X.E, false, X.M, cast<BasicType>(T), X.Val);
    else
      recordTypeAndValue(X.E, X.M, T,
                         X.M == Mode::Constant ? Mem.constant(X.Val) : nullptr);
  }
  }
  return Kind;
}

void Checker::exclude(Operand &X, unsigned ModeSet) {
  if (!(ModeSet & bit(X.M)))
    return;
  switch (X.M) {
  case Mode::NoValue:
    err(X, diag::err_no_value, {str(X.E)});
    break;
  case Mode::Builtin:
    err(X, diag::err_builtin_not_used_as_value, {str(X.E)});
    break;
  case Mode::TypeExpr:
    err(X, diag::err_not_an_expression, {str(X)});
    break;
  default:
    break;
  }
  X.M = Mode::Invalid;
}

void Checker::singleValue(Operand &X) {
  if (X.M == Mode::Value)
    if (auto *T = dyn_cast<TupleType>(X.Ty)) {
      (void)T;
      err(X, diag::err_multi_value_in_single, {str(X.E), str(X.Ty)});
      X.M = Mode::Invalid;
    }
}

void Checker::expr(Operand &X, const ast::Expr *Ex) {
  rawExpr(X, Ex, nullptr, false);
  exclude(X, bit(Mode::NoValue) | bit(Mode::Builtin) | bit(Mode::TypeExpr));
  singleValue(X);
}

void Checker::genericExpr(Operand &X, const ast::Expr *Ex) {
  rawExpr(X, Ex, nullptr, true);
  exclude(X, bit(Mode::NoValue) | bit(Mode::Builtin) | bit(Mode::TypeExpr));
  singleValue(X);
}

void Checker::exprWithHint(Operand &X, const ast::Expr *Ex, Type *Hint) {
  rawExpr(X, Ex, Hint, false);
  exclude(X, bit(Mode::NoValue) | bit(Mode::Builtin) | bit(Mode::TypeExpr));
  singleValue(X);
}

void Checker::exprOrType(Operand &X, const ast::Expr *Ex, bool AllowGeneric) {
  rawExpr(X, Ex, nullptr, AllowGeneric);
  exclude(X, bit(Mode::NoValue));
  singleValue(X);
}

void Checker::multiExpr(SmallVector<Operand, 4> &Out, const ast::Expr *Ex,
                        bool AllowCommaOk, bool *CommaOk) {
  Operand X;
  if (AllowCommaOk)
    markLookupOk(Ex); // `v, ok := m[k]` reads no value that may not exist
  rawExpr(X, Ex, nullptr, false);
  exclude(X, bit(Mode::NoValue) | bit(Mode::Builtin) | bit(Mode::TypeExpr));
  if (CommaOk)
    *CommaOk = false;
  if (X.M != Mode::Invalid)
    if (auto *T = dyn_cast<TupleType>(X.Ty)) {
      for (Type *E : T->Elems) {
        Operand O;
        O.M = Mode::Value;
        O.E = Ex;
        O.Ty = E;
        Out.push_back(O);
      }
      return;
    }
  Out.push_back(X);
  if (AllowCommaOk && (X.M == Mode::MapIndex || X.M == Mode::CommaOk ||
                       X.M == Mode::CommaErr)) {
    Operand X2;
    X2.M = Mode::Value;
    X2.E = Ex;
    X2.Ty = X.M == Mode::CommaErr
                ? Ctx.errorType()
                : static_cast<Type *>(typ(BasicKind::UntypedBool));
    Out.push_back(X2);
    if (CommaOk)
      *CommaOk = true;
  }
}

void Checker::use(const ast::Expr *Ex) {
  if (!Ex)
    return;
  Operand X;
  rawExpr(X, Ex, nullptr, true);
}

void Checker::useExprs(ArrayRef<const ast::Expr *> Es) {
  for (const ast::Expr *Ex : Es)
    use(Ex);
}

void Checker::useLHS(ArrayRef<const ast::Expr *> Es) {
  for (const ast::Expr *Ex : Es) {
    const ast::Expr *Inner = Ex;
    while (auto *P = dyn_cast_or_null<ParenExpr>(Inner))
      Inner = P->X;
    if (auto *Id = dyn_cast_or_null<ast::Ident>(Inner)) {
      if (name(Id) == "_")
        continue;
      Object *O = E.Sc->lookup(name(Id), E.Pos);
      if (auto *V = dyn_cast_or_null<Var>(O)) {
        bool Used = V->Used;
        use(Ex);
        // Not an unconditional store: V may be a package-level variable that
        // the other body threads read (P03.10).
        if (V->Used != Used)
          V->Used = Used;
        continue;
      }
    }
    use(Ex);
  }
}

//===----------------------------------------------------------------------===//
// Literals
//===----------------------------------------------------------------------===//

void Checker::basicLit(Operand &X, const BasicLit *Lit) {
  BasicKind K;
  switch (Lit->LitKind) {
  case tok::int_lit:
    K = BasicKind::UntypedInt;
    break;
  case tok::float_lit:
    K = BasicKind::UntypedFloat;
    break;
  case tok::imag_lit:
    K = BasicKind::UntypedComplex;
    break;
  case tok::char_lit:
    K = BasicKind::UntypedRune;
    break;
  default:
    K = BasicKind::UntypedString;
  }
  X.Val = ConstValue::fromLiteral(Lit->Text, Lit->LitKind);
  X.M = X.Val.isUnknown() ? Mode::Invalid : Mode::Constant;
  X.Ty = typ(K);
}

void Checker::overflow(Operand &X, SourceLoc OpLoc) {
  if (X.Val.isUnknown()) {
    err(OpLoc, diag::err_sema_expr, {"constant result is not representable"});
    return;
  }
  if (isTyped(X.Ty)) {
    representable(X, cast<BasicType>(under(X.Ty)));
    return;
  }
  if (X.Val.kind() == ConstValue::Int && X.Val.bitLen() > 512) {
    std::string Op;
    if (auto *B = dyn_cast_or_null<BinaryExpr>(X.E)) {
      switch (B->Op) {
      case tok::add:
        Op = "addition ";
        break;
      case tok::sub:
        Op = "subtraction ";
        break;
      case tok::xor_:
        Op = "bitwise XOR ";
        break;
      case tok::mul:
        Op = "multiplication ";
        break;
      case tok::shl:
        Op = "shift ";
        break;
      default:
        break;
      }
    } else if (auto *U = dyn_cast_or_null<UnaryExpr>(X.E)) {
      if (U->Op == tok::sub)
        Op = "negation ";
      else if (U->Op == tok::xor_)
        Op = "bitwise complement ";
    }
    err(OpLoc, diag::err_constant_expression_overflow, {Op});
    X.Val = ConstValue();
  }
}

//===----------------------------------------------------------------------===//
// Operators
//===----------------------------------------------------------------------===//

static bool opAllowed(Checker &C, tok::Kind Op, Type *T, bool Unary) {
  if (Unary) {
    switch (Op) {
    case tok::add:
    case tok::sub:
      return C.allNumeric(T);
    case tok::xor_:
      return C.allInteger(T);
    case tok::not_:
      return C.allBoolean(T);
    default:
      return false;
    }
  }
  switch (Op) {
  case tok::add:
    return C.allNumericOrString(T);
  case tok::sub:
  case tok::mul:
  case tok::quo:
    return C.allNumeric(T);
  case tok::rem:
  case tok::and_:
  case tok::or_:
  case tok::xor_:
  case tok::and_not:
    return C.allInteger(T);
  case tok::land:
  case tok::lor:
    return C.allBoolean(T);
  default:
    return false;
  }
}

void Checker::unary(Operand &X, const UnaryExpr *Ex) {
  // `&o` of an Option points at the Option itself (and o is never narrowed:
  // see narrowable).
  if (Ex->Op == tok::and_)
    markNoNarrow(Ex->X);
  expr(X, Ex->X);
  if (X.M == Mode::Invalid)
    return;
  tok::Kind Op = Ex->Op;
  switch (Op) {
  case tok::and_: {
    const ast::Expr *Inner = Ex->X;
    while (auto *P = dyn_cast<ParenExpr>(Inner))
      Inner = P->X;
    if (!isa<CompositeLit>(Inner) && X.M != Mode::Variable) {
      err(X, diag::err_address_of_unaddressable, {str(X)});
      X.M = Mode::Invalid;
      return;
    }
    if (own() && X.M == Mode::Variable) {
      // `&o` is handled by narrowable (o is never narrowed); `&o.In` points
      // into the payload of a checked Option.
      if (!isa<ast::Ident>(Inner))
        rejectPayloadAddress(Inner, "an `&` expression");
      noteReference(Inner);
      checkAddressOf(Ex, X);
    }
    X.M = Mode::Value;
    X.Ty = Ctx.pointer(X.Ty);
    return;
  }
  case tok::arrow: {
    Type *Elem = chanElem(X.pos(), X, true);
    if (!Elem) {
      X.M = Mode::Invalid;
      return;
    }
    if (!checkLookupZero(Ex, Elem)) {
      X.M = Mode::Invalid;
      return;
    }
    X.M = Mode::CommaOk;
    X.Ty = Elem;
    E.HasCallOrRecv = true;
    return;
  }
  case tok::tilde:
    if (!allInteger(X.Ty)) {
      err(Ex, diag::err_sema_expr,
          {"cannot use ~ outside of interface or type constraint"});
      X.M = Mode::Invalid;
      return;
    }
    err(Ex, diag::err_sema_expr,
        {"cannot use ~ outside of interface or type constraint (use ^ for "
         "bitwise complement)"});
    Op = tok::xor_;
    break;
  default:
    break;
  }
  if (!opAllowed(*this, Op, X.Ty, true)) {
    err(X, diag::err_operator_not_defined, {tok::getSpelling(Op), str(X)});
    X.M = Mode::Invalid;
    return;
  }
  if (X.M == Mode::Constant) {
    if (X.Val.isUnknown())
      return;
    unsigned Prec = 0;
    if (isUnsigned(X.Ty))
      Prec = sizeofType(X.Ty) * 8;
    X.Val = ConstValue::unaryOp(Op, X.Val, Prec);
    X.E = Ex;
    overflow(X, Ex->OpLoc);
    return;
  }
  X.M = Mode::Value;
}

bool Checker::mayConvert(Operand &X, Operand &Y) {
  if (isTyped(X.Ty) && isTyped(Y.Ty))
    return false;
  // `l == nil` for a never-nil type is reported by comparison().
  if (own() &&
      ((X.isNil() && neverNilKind(Y.Ty)) || (Y.isNil() && neverNilKind(X.Ty))))
    return false;
  if (isNonTypeParamInterface(X.Ty) || isNonTypeParamInterface(Y.Ty))
    return true;
  if (allBoolean(X.Ty) != allBoolean(Y.Ty))
    return false;
  if (allString(X.Ty) != allString(Y.Ty))
    return false;
  if (X.isNil())
    return hasNil(Y.Ty);
  if (Y.isNil())
    return hasNil(X.Ty);
  if (isPointer(X.Ty) || isPointer(Y.Ty))
    return false;
  return true;
}

void Checker::matchTypes(Operand &X, Operand &Y) {
  if (mayConvert(X, Y)) {
    convertUntyped(X, Y.Ty);
    if (X.M == Mode::Invalid)
      return;
    convertUntyped(Y, X.Ty);
    if (Y.M == Mode::Invalid) {
      X.M = Mode::Invalid;
      return;
    }
  }
}

void Checker::comparison(Operand &X, Operand &Y, tok::Kind Op) {
  if (!isValid(X.Ty) || !isValid(Y.Ty)) {
    X.M = Mode::Invalid;
    return;
  }
  Operand *ErrOp = &X;
  std::string Cause;
  bool Fail = false;
  // P03.19: `o == p` compares the Option itself where only a loop's
  // assumption narrows o and the Option compares with p as well
  // (unnarrowTentative).
  if (own() && (Op == tok::eql || Op == tok::neq) && !X.isNil() && !Y.isNil())
    for (Operand *A : {&X, &Y}) {
      Operand B = A == &X ? Y : X;
      if (Type *OT = tentOption(*A)) {
        Operand O = *A;
        O.Ty = OT;
        if ((assignableTo(O, B.Ty, nullptr) || assignableTo(B, OT, nullptr)) &&
            comparableType(OT, true, nullptr))
          unnarrowTentative(*A);
      }
    }
  // ADR-0018: `p == nil` on a pointer kind.
  if (own() && (Op == tok::eql || Op == tok::neq) && (X.isNil() || Y.isNil())) {
    Type *T = X.isNil() ? Y.Ty : X.Ty;
    if (isPointerKind(T)) {
      err(X.isNil() ? Y : X, diag::err_ptr_never_nil, {str(T)});
      X.M = Mode::Invalid;
      return;
    }
    if (const char *Why = neverNilKind(T)) {
      err(X.isNil() ? Y : X, diag::err_nil_never_nil, {str(T), Why});
      X.M = Mode::Invalid;
      return;
    }
  }
  bool Ok = assignableTo(X, Y.Ty, nullptr);
  if (!Ok)
    Ok = assignableTo(Y, X.Ty, nullptr);
  if (!Ok) {
    ErrOp = &Y;
    Cause = "mismatched types " + str(X.Ty) + " and " + str(Y.Ty);
    Fail = true;
  }
  auto IncomparableCause = [&](Type *T) {
    Type *U = under(T);
    if (isa<SliceType>(U) || isa<SignatureType>(U) || isa<MapType>(U)) {
      if (own() && isa<SignatureType>(U))
        return std::string(
            "a func is never nil and cannot be compared (TYP-3)");
      const char *K = isa<SliceType>(U) ? "slice"
                      : isa<MapType>(U) ? "map"
                                        : "func";
      return std::string(K) + " can only be compared to nil";
    }
    std::string C;
    comparableType(T, true, nullptr, &C);
    return C;
  };
  if (!Fail) {
    switch (Op) {
    case tok::eql:
    case tok::neq:
      if (X.isNil() || Y.isNil()) {
        Type *T = X.isNil() ? Y.Ty : X.Ty;
        if (!hasNil(T)) {
          ErrOp = &Y;
          Fail = true;
        }
      } else if (!comparableType(X.Ty, true, nullptr)) {
        ErrOp = &X;
        Cause = IncomparableCause(X.Ty);
        Fail = true;
      } else if (!comparableType(Y.Ty, true, nullptr)) {
        ErrOp = &Y;
        Cause = IncomparableCause(Y.Ty);
        Fail = true;
      }
      break;
    default:
      if (!allBasic(X.Ty, IsOrdered)) {
        ErrOp = &X;
        Fail = true;
      } else if (!allBasic(Y.Ty, IsOrdered)) {
        ErrOp = &Y;
        Fail = true;
      }
    }
  }
  if (Fail) {
    if (Cause.empty()) {
      if (isTypeParam(X.Ty) || isTypeParam(Y.Ty)) {
        if (!isTypeParam(X.Ty))
          ErrOp = &Y;
        Cause = "type parameter " + str(ErrOp->Ty) + " cannot use operator " +
                tok::getSpelling(Op);
      } else {
        Type *U = under(ErrOp->Ty);
        const char *What = "";
        switch (U->getKind()) {
        case Type::Kind::Array:
          What = "array";
          break;
        case Type::Kind::Slice:
          What = "slice";
          break;
        case Type::Kind::Struct:
          What = "struct";
          break;
        case Type::Kind::Pointer:
          What = "pointer";
          break;
        case Type::Kind::Signature:
          What = "func";
          break;
        case Type::Kind::Interface:
          What = "interface";
          break;
        case Type::Kind::Map:
          What = "map";
          break;
        case Type::Kind::Chan:
          What = "chan";
          break;
        default:
          break;
        }
        std::string W = *What && isa<NamedType>(ErrOp->Ty) ? What : "";
        if (W.empty() && *What)
          W = What;
        if (W.empty())
          W = str(ErrOp->Ty);
        Cause = std::string("operator ") + tok::getSpelling(Op) +
                " not defined on " + W;
      }
    }
    if (SwitchCaseCmp) {
      err(X, diag::err_sema_expr,
          {"invalid case " + str(X.E) + " in switch on " + str(Y.E) + " (" +
           Cause + ")"});
    } else {
      err(*ErrOp, diag::err_cannot_compare,
          {str(X.E) + " " + tok::getSpelling(Op) + " " + str(Y.E), Cause});
    }
    X.M = Mode::Invalid;
    return;
  }
  if (X.M == Mode::Constant && Y.M == Mode::Constant) {
    X.Val = ConstValue::makeBool(ConstValue::compare(X.Val, Op, Y.Val));
  } else {
    X.M = Mode::Value;
    updateExprType(X.E, defaultType(X.Ty), true);
    updateExprType(Y.E, defaultType(Y.Ty), true);
  }
  X.Ty = typ(BasicKind::UntypedBool);
}

void Checker::shift(Operand &X, Operand &Y, const ast::Expr *Ex, tok::Kind Op) {
  ConstValue XVal;
  if (X.M == Mode::Constant)
    XVal = X.Val.toInt();
  if (allInteger(X.Ty) || (isUntyped(X.Ty) && !XVal.isUnknown() &&
                           XVal.kind() == ConstValue::Int)) {
  } else {
    err(X, diag::err_shift_operand_invalid, {str(X)});
    X.M = Mode::Invalid;
    return;
  }
  ConstValue YVal;
  if (Y.M == Mode::Constant) {
    YVal = Y.Val.toInt();
    if (YVal.kind() == ConstValue::Int && YVal.sign() < 0) {
      err(Y, diag::err_shift_count_negative, {str(Y)});
      X.M = Mode::Invalid;
      return;
    }
    if (isUntyped(Y.Ty)) {
      representable(Y, typ(BasicKind::Uint));
      if (Y.M == Mode::Invalid) {
        X.M = Mode::Invalid;
        return;
      }
    }
  } else {
    if (allInteger(Y.Ty)) {
    } else if (isUntyped(Y.Ty)) {
      convertUntyped(Y, typ(BasicKind::Uint));
      if (Y.M == Mode::Invalid) {
        X.M = Mode::Invalid;
        return;
      }
    } else {
      err(Y, diag::err_sema_expr,
          {"invalid operation: shift count " + str(Y) + " must be integer"});
      X.M = Mode::Invalid;
      return;
    }
  }
  if (X.M == Mode::Constant) {
    if (Y.M == Mode::Constant) {
      if (X.Val.isUnknown() || Y.Val.isUnknown()) {
        X.Val = ConstValue();
        if (!isInteger(X.Ty))
          X.Ty = typ(BasicKind::UntypedInt);
        return;
      }
      uint64_t S;
      if (!YVal.fitsUint64(S) || S > 1074) {
        err(Y, diag::err_shift_too_large, {str(Y)});
        X.M = Mode::Invalid;
        return;
      }
      if (!isInteger(X.Ty))
        X.Ty = typ(BasicKind::UntypedInt);
      X.Val = ConstValue::shift(XVal, Op, static_cast<unsigned>(S));
      X.E = Ex;
      overflow(X, cast<BinaryExpr>(Ex)->OpLoc);
      return;
    }
    if (isUntyped(X.Ty)) {
      auto It = Untyped.find(X.E);
      if (It != Untyped.end())
        It->second.IsLhs = true;
      X.M = Mode::Value;
      return;
    }
  }
  if (!allInteger(X.Ty)) {
    err(X, diag::err_shift_operand_invalid, {str(X)});
    X.M = Mode::Invalid;
    return;
  }
  X.M = Mode::Value;
}

void Checker::binary(Operand &X, const ast::Expr *Ex, const ast::Expr *L,
                     const ast::Expr *R, tok::Kind Op, SourceLoc OpLoc) {
  Operand Y;
  // Flow typing: `o == nil` and `o != nil` look at the Option itself.
  if (own() && (Op == tok::eql || Op == tok::neq)) {
    if (isNilLit(R))
      markNoNarrow(L);
    else if (isNilLit(L))
      markNoNarrow(R);
  }
  expr(X, L);
  if (own() && (Op == tok::land || Op == tok::lor)) {
    // The right operand of `a && b` runs only if a is true, that of `a || b`
    // only if a is false: `o != nil && o.f` uses o as its value.
    FlowFacts F;
    flowFacts(L, F);
    FlowState Saved = Cur;
    size_t Logged = MoveLog.size();
    flowAdd(Op == tok::land ? F.True : F.False);
    expr(Y, R);
    Cur = std::move(Saved);
    // A move in the right operand may have run: o is nil (not known set).
    for (size_t K = Logged; K < MoveLog.size(); ++K)
      Cur.N.erase(MoveLog[K]);
  } else {
    expr(Y, R);
  }
  if (X.M == Mode::Invalid)
    return;
  if (Y.M == Mode::Invalid) {
    X.M = Mode::Invalid;
    X.E = Y.E;
    return;
  }
  if (isShiftOp(Op)) {
    shift(X, Y, Ex, Op);
    return;
  }
  matchTypes(X, Y);
  if (X.M == Mode::Invalid)
    return;
  if (isComparison(Op)) {
    comparison(X, Y, Op);
    return;
  }
  if (!identical(X.Ty, Y.Ty)) {
    if (isValid(X.Ty) && isValid(Y.Ty)) {
      if (Ex)
        err(X, diag::err_mismatched_types, {str(Ex), str(X.Ty), str(Y.Ty)});
      else
        err(X, diag::err_mismatched_types,
            {str(L) + " " + tok::getSpelling(Op) + "= " + str(R), str(X.Ty),
             str(Y.Ty)});
    }
    X.M = Mode::Invalid;
    return;
  }
  if (!opAllowed(*this, Op, X.Ty, false)) {
    err(X, diag::err_operator_not_defined, {tok::getSpelling(Op), str(X)});
    X.M = Mode::Invalid;
    return;
  }
  if (Op == tok::quo || Op == tok::rem) {
    if ((X.M == Mode::Constant || allInteger(X.Ty)) && Y.M == Mode::Constant &&
        Y.Val.sign() == 0 && !Y.Val.isUnknown()) {
      err(Y, diag::err_division_by_zero);
      X.M = Mode::Invalid;
      return;
    }
    if (X.M == Mode::Constant && Y.M == Mode::Constant && isComplex(X.Ty) &&
        !Y.Val.isUnknown()) {
      ConstValue Re = Y.Val.real(), Im = Y.Val.imag();
      ConstValue Re2 = ConstValue::binaryOp(Re, tok::mul, Re);
      ConstValue Im2 = ConstValue::binaryOp(Im, tok::mul, Im);
      if (Re2.sign() == 0 && Im2.sign() == 0) {
        err(Y, diag::err_division_by_zero);
        X.M = Mode::Invalid;
        return;
      }
    }
  }
  if (X.M == Mode::Constant && Y.M == Mode::Constant) {
    if (X.Val.isUnknown() || Y.Val.isUnknown()) {
      X.Val = ConstValue();
      return;
    }
    tok::Kind T = Op;
    if (Op == tok::quo && isInteger(X.Ty))
      T = tok::quo_assign;
    X.Val = ConstValue::binaryOp(X.Val, T, Y.Val);
    X.E = Ex;
    overflow(X, OpLoc);
    return;
  }
  X.M = Mode::Value;
  // Go++ (OWN-R3a, STM-8): `+` on strings borrows both operands: no check.
}

//===----------------------------------------------------------------------===//
// exprInternal
//===----------------------------------------------------------------------===//

void Checker::funcLit(Operand &X, const FuncLit *Lit) {
  FuncInfo FI = funcSignature(Lit->Type, nullptr, nullptr);
  if (!FI.Sig) {
    X.M = Mode::Invalid;
    return;
  }
  if (Lit->Body) {
    DeclInfo *D = E.Cur;
    const ConstValue *Iota = E.Iota;
    later([this, FI, Lit, D, Iota]() {
      funcBody(D, "<function literal>", FI.Sig, Lit->Body, Iota, FI.Sc,
               FI.Params, FI.Results, nullptr, Lit->Type);
    });
  }
  X.M = Mode::Value;
  X.Ty = FI.Sig;
}

ExprKind Checker::exprInternal(Operand &X, const ast::Expr *Ex, Type *Hint) {
  X.M = Mode::Invalid;
  X.Ty = invalid();
  X.E = Ex;
  using K = Node::Kind;
  auto Error = [&]() {
    X.M = Mode::Invalid;
    X.E = Ex;
    return ExprKind::Statement;
  };
  switch (Ex->getKind()) {
  case K::BadExpr:
    return Error();
  case K::Ident:
    ident(X, cast<ast::Ident>(Ex), nullptr, false);
    break;
  case K::Ellipsis:
    err(Ex, diag::err_invalid_use_of_dots);
    return Error();
  case K::BasicLit: {
    auto *L = cast<BasicLit>(Ex);
    if (L->Text.size() > 10000 &&
        (L->LitKind == tok::int_lit || L->LitKind == tok::float_lit ||
         L->LitKind == tok::imag_lit)) {
      err(Ex, diag::err_sema_expr,
          {"excessively long constant: " + L->Text.substr(0, 10).str() +
           "... (" + std::to_string(L->Text.size()) + " chars)"});
      return Error();
    }
    basicLit(X, L);
    if (X.M == Mode::Invalid) {
      if (L->LitKind == tok::int_lit || L->LitKind == tok::float_lit ||
          L->LitKind == tok::imag_lit)
        err(Ex, diag::err_sema_expr, {"malformed constant: " + L->Text.str()});
      return Error();
    }
    X.E = Ex;
    overflow(X, Ex->getBegin());
    break;
  }
  case K::FuncLit:
    funcLit(X, cast<FuncLit>(Ex));
    if (X.M == Mode::Invalid)
      return Error();
    break;
  case K::CompositeLit:
    compositeLit(X, cast<CompositeLit>(Ex), Hint);
    if (X.M == Mode::Invalid)
      return Error();
    break;
  case K::ParenExpr: {
    ExprKind PK = rawExpr(X, cast<ParenExpr>(Ex)->X, nullptr, false);
    X.E = Ex;
    return PK;
  }
  case K::SelectorExpr:
    selector(X, cast<SelectorExpr>(Ex), false);
    break;
  case K::IndexExpr:
    if (indexExpr(X, cast<IndexExpr>(Ex)))
      funcInst(X, Ex->getBegin(), cast<IndexExpr>(Ex), true, Hint);
    if (X.M == Mode::Invalid)
      return Error();
    break;
  case K::SliceExpr:
    sliceExpr(X, cast<SliceExpr>(Ex));
    if (X.M == Mode::Invalid)
      return Error();
    break;
  case K::TypeAssertExpr: {
    auto *TA = cast<TypeAssertExpr>(Ex);
    expr(X, TA->X);
    if (X.M == Mode::Invalid)
      return Error();
    if (!TA->Type) {
      err(Ex, diag::err_type_switch_outside);
      return Error();
    }
    if (isTypeParam(X.Ty)) {
      err(X, diag::err_sema_expr,
          {"invalid operation: cannot use type assertion on type parameter "
           "value " +
           str(X)});
      return Error();
    }
    if (!isa<InterfaceType>(under(X.Ty))) {
      err(X, diag::err_type_assertion_non_interface, {str(X)});
      return Error();
    }
    Type *T = varType(TA->Type);
    if (!isValid(T))
      return Error();
    typeAssertion(Ex, X, T, false);
    X.M = Mode::CommaOk;
    X.Ty = T;
    break;
  }
  case K::CallExpr:
    return callExpr(X, cast<CallExpr>(Ex));
  case K::StarExpr: {
    auto *St = cast<StarExpr>(Ex);
    exprOrType(X, St->X, false);
    switch (X.M) {
    case Mode::Invalid:
      return Error();
    case Mode::TypeExpr:
      validVarType(St->X, X.Ty);
      X.Ty = Ctx.pointer(X.Ty);
      break;
    default: {
      PtrKind PK;
      Type *PElem;
      if (hasPtrKind(X.Ty, &PK, &PElem)) {
        if (PK == PtrKind::Weak) {
          // ADR-0017: `*w` is not defined; Lock() asks if the target lives.
          err(X, diag::err_weak_no_indirect, {str(X.E)});
          return Error();
        }
        X.M = Mode::Variable;
        X.Ty = PElem;
        break;
      }
      if (own() && isa<OptionType>(under(X.Ty)) && X.E) {
        err(X, diag::err_option_maybe_nil, {str(X.E), nilCheckHint(X.E)});
        return Error();
      }
      Type *Base = nullptr;
      bool Ok = underIs(X.Ty, [&](Type *U) {
        auto *P = U ? dyn_cast<PointerType>(U) : nullptr;
        if (!P) {
          err(X, diag::err_deref_non_pointer, {str(X)});
          return false;
        }
        if (Base && !identical(P->Elem, Base)) {
          err(X, diag::err_sema_expr,
              {"invalid operation: pointers of " + str(X) +
               " must have identical base types"});
          return false;
        }
        Base = P->Elem;
        return true;
      });
      if (!Ok)
        return Error();
      X.M = Mode::Variable;
      X.Ty = Base;
    }
    }
    break;
  }
  case K::UnaryExpr: {
    auto *U = cast<UnaryExpr>(Ex);
    unary(X, U);
    if (X.M == Mode::Invalid)
      return Error();
    if (U->Op == tok::arrow) {
      X.E = Ex;
      return ExprKind::Statement;
    }
    break;
  }
  case K::BinaryExpr: {
    auto *B = cast<BinaryExpr>(Ex);
    binary(X, Ex, B->X, B->Y, B->Op, B->OpLoc);
    if (X.M == Mode::Invalid)
      return Error();
    break;
  }
  case K::KeyValueExpr:
    err(Ex, diag::err_sema_expr, {"no key:value expected"});
    return Error();
  case K::ArrayType:
  case K::SliceType:
  case K::StructType:
  case K::FuncType:
  case K::InterfaceType:
  case K::MapType:
  case K::ChanType:
  case K::RefType:
    X.M = Mode::TypeExpr;
    X.Ty = typ(Ex);
    break;
  case K::MacroInvocation:
    err(Ex, diag::err_sema_expr, {"macro invocations are expanded by P09"});
    return Error();
  default:
    return Error();
  }
  X.E = Ex;
  return ExprKind::Expression;
}

void Checker::typeAssertion(const ast::Expr *Ex, Operand &X, Type *T,
                            bool TypeSwitch) {
  std::string Cause;
  if (assertableTo(X.Ty, T, &Cause))
    return;
  if (TypeSwitch) {
    err(Ex, diag::err_sema_expr,
        {"impossible type switch case: " + str(Ex) + "\n\t" + str(X) +
         " cannot have dynamic type " + str(T) + " " + Cause});
    return;
  }
  err(Ex, diag::err_type_assertion_impossible,
      {str(Ex), str(T), str(X.Ty), Cause});
}
