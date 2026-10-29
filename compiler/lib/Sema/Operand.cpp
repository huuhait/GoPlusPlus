//===- Operand.cpp - Operands, assignability, conversion, representability ===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's operand.go, conversions.go and const.go (BSD-style
// license, Copyright The Go Authors), with the Go++ additions of TYP-2 and
// EXP-9.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include <cmath>

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

const char *sema::getModeName(Mode M) {
  switch (M) {
  case Mode::Invalid:
    return "invalid operand";
  case Mode::NoValue:
    return "no value";
  case Mode::Builtin:
    return "built-in";
  case Mode::TypeExpr:
    return "type";
  case Mode::Constant:
    return "constant";
  case Mode::Variable:
    return "variable";
  case Mode::MapIndex:
    return "map index expression";
  case Mode::Value:
    return "value";
  case Mode::NilValue:
    return "nil";
  case Mode::CommaOk:
    return "comma, ok expression";
  case Mode::CommaErr:
    return "comma, error expression";
  }
  return "";
}

static const char *compositeKind(Checker &C, Type *T) {
  switch (C.under(T)->getKind()) {
  case Type::Kind::Array:
    return "array";
  case Type::Kind::Slice:
    return "slice";
  case Type::Kind::Struct:
    return "struct";
  case Type::Kind::Pointer:
    return "pointer";
  case Type::Kind::Signature:
    return "func";
  case Type::Kind::Interface:
    return "interface";
  case Type::Kind::Map:
    return "map";
  case Type::Kind::Chan:
    return "chan";
  case Type::Kind::Tuple:
    return "tuple";
  case Type::Kind::Union:
    return "union";
  default:
    return "";
  }
}

std::string Checker::operandString(const Operand &X) {
  if (X.M == Mode::NilValue) {
    if (!X.Ty || !isValid(X.Ty))
      return "nil (with invalid type)";
    if (X.Ty == typ(BasicKind::UntypedNil))
      return "nil";
    return "nil (of type " + str(X.Ty) + ")";
  }
  std::string Expr;
  if (X.E) {
    Expr = str(X.E);
  } else {
    switch (X.M) {
    case Mode::Builtin:
      Expr = "builtin";
      break;
    case Mode::TypeExpr:
      Expr = str(X.Ty);
      break;
    case Mode::Constant:
      Expr = X.Val.str();
      break;
    default:
      break;
    }
  }
  std::string Buf;
  if (!Expr.empty())
    Buf = Expr + " (";
  bool HasType = false;
  switch (X.M) {
  case Mode::Invalid:
  case Mode::NoValue:
  case Mode::Builtin:
  case Mode::TypeExpr:
    break;
  default:
    if (X.Ty) {
      if (!isTyped(X.Ty)) {
        Buf += cast<BasicType>(X.Ty)->Name;
        Buf += ' ';
        break;
      }
      HasType = true;
    }
  }
  Buf += getModeName(X.M);
  if (X.M == Mode::Constant) {
    std::string V = X.Val.str();
    if (V != Expr)
      Buf += " " + V;
  }
  if (HasType) {
    if (isValid(X.Ty)) {
      std::string Desc;
      if (isGeneric(X.Ty))
        Desc = "generic ";
      auto *TP = dyn_cast<TypeParamType>(X.Ty);
      if (!TP && isa<NamedType>(X.Ty)) {
        const char *What = compositeKind(*this, X.Ty);
        if (*What)
          Desc += std::string(What) + " ";
        else if (auto *B = dyn_cast<BasicType>(under(X.Ty)))
          Desc += std::string(B->Name) + " "; // a defined basic type
      }
      Buf += " of " + Desc + "type " + str(X.Ty);
      if (TP) {
        Buf += " constrained by " + str(TP->Bound);
        TypeSet *TS = typeSetOf(TP);
        if (TS && TS->isEmpty())
          Buf += " with empty type set";
      }
    } else {
      Buf += " with invalid type";
    }
  }
  if (!Expr.empty())
    Buf += ")";
  return Buf;
}

std::string Checker::str(const Operand &X) const {
  return const_cast<Checker *>(this)->operandString(X);
}

void Checker::setConst(Operand &X, ConstValue V, Type *T) {
  X.M = Mode::Constant;
  X.Ty = T;
  X.Val = std::move(V);
}

//===----------------------------------------------------------------------===//
// Representability
//===----------------------------------------------------------------------===//

static bool fitsFloat32(const ConstValue &V) {
  float F = V.float32();
  return !std::isinf(F);
}
static bool fitsFloat64(const ConstValue &V) {
  double D = V.float64();
  return !std::isinf(D);
}

bool Checker::representableConst(const ConstValue &X, BasicType *T,
                                 ConstValue *Rounded) {
  if (X.isUnknown())
    return true;
  if (T->Info & IsInteger) {
    ConstValue V = X.toInt();
    if (V.kind() != ConstValue::Int)
      return false;
    if (Rounded)
      *Rounded = V;
    int64_t I64;
    if (V.fitsInt64(I64)) {
      switch (T->BK) {
      case BasicKind::Int:
      case BasicKind::Int64:
        return true;
      case BasicKind::Int8:
        return I64 >= -128 && I64 <= 127;
      case BasicKind::Int16:
        return I64 >= -32768 && I64 <= 32767;
      case BasicKind::Int32:
        return I64 >= INT32_MIN && I64 <= INT32_MAX;
      case BasicKind::Uint:
      case BasicKind::Uintptr:
      case BasicKind::Uint64:
        return I64 >= 0;
      case BasicKind::Uint8:
        return I64 >= 0 && I64 <= 255;
      case BasicKind::Uint16:
        return I64 >= 0 && I64 <= 65535;
      case BasicKind::Uint32:
        return I64 >= 0 && I64 <= UINT32_MAX;
      case BasicKind::UntypedInt:
      case BasicKind::UntypedRune:
        return true;
      default:
        return false;
      }
    }
    unsigned N = V.bitLen();
    switch (T->BK) {
    case BasicKind::Uint:
    case BasicKind::Uintptr:
    case BasicKind::Uint64:
      return V.sign() >= 0 && N <= 64;
    case BasicKind::UntypedInt:
    case BasicKind::UntypedRune:
      return true;
    default:
      return false;
    }
  }
  if (T->Info & IsFloat) {
    ConstValue V = X.toFloat();
    if (V.kind() != ConstValue::Float)
      return false;
    switch (T->BK) {
    case BasicKind::Float32:
      if (!fitsFloat32(V))
        return false;
      if (Rounded)
        *Rounded = V.roundFloat32();
      return true;
    case BasicKind::Float64:
      if (!fitsFloat64(V))
        return false;
      if (Rounded)
        *Rounded = V.roundFloat64();
      return true;
    case BasicKind::UntypedFloat:
      if (Rounded)
        *Rounded = V;
      return !V.overflows();
    default:
      return false;
    }
  }
  if (T->Info & IsComplex) {
    ConstValue V = X.toComplex();
    if (V.kind() != ConstValue::Complex)
      return false;
    switch (T->BK) {
    case BasicKind::Complex64: {
      ConstValue Re = V.real(), Im = V.imag();
      if (!fitsFloat32(Re.toFloat()) || !fitsFloat32(Im.toFloat()))
        return false;
      if (Rounded)
        *Rounded = V.roundFloat32();
      return true;
    }
    case BasicKind::Complex128: {
      ConstValue Re = V.real(), Im = V.imag();
      if (!fitsFloat64(Re.toFloat()) || !fitsFloat64(Im.toFloat()))
        return false;
      if (Rounded)
        *Rounded = V.roundFloat64();
      return true;
    }
    case BasicKind::UntypedComplex:
      if (Rounded)
        *Rounded = V;
      return true;
    default:
      return false;
    }
  }
  if (T->Info & IsString)
    return X.kind() == ConstValue::String;
  if (T->Info & IsBoolean)
    return X.kind() == ConstValue::Bool;
  return false;
}

Checker::ReprCode Checker::representation(Operand &X, BasicType *T,
                                          ConstValue &Out) {
  Out = X.Val;
  if (!representableConst(X.Val, T, &Out)) {
    if (isNumeric(X.Ty) && (T->Info & IsNumeric)) {
      if (!isInteger(X.Ty) && (T->Info & IsInteger))
        return ReprCode::Truncated;
      return ReprCode::Overflow;
    }
    return ReprCode::Invalid;
  }
  return ReprCode::Ok;
}

void Checker::invalidConversion(ReprCode C, Operand &X, Type *Target) {
  switch (C) {
  case ReprCode::Truncated:
    err(X, diag::err_value_truncated, {str(X), str(Target)});
    break;
  case ReprCode::Overflow:
    err(X, diag::err_value_overflows, {str(X), str(Target)});
    break;
  default:
    err(X, diag::err_cannot_convert, {str(X), str(Target)});
  }
}

void Checker::representable(Operand &X, BasicType *T) {
  ConstValue V;
  ReprCode C = representation(X, T, V);
  if (C != ReprCode::Ok) {
    invalidConversion(C, X, T);
    X.M = Mode::Invalid;
    return;
  }
  X.Val = std::move(V);
}

Type *Checker::implicitTypeAndValue(Operand &X, Type *Target,
                                    ConstValue *NewVal, ReprCode *Code) {
  if (Code)
    *Code = ReprCode::Ok;
  if (X.M == Mode::Invalid || isTyped(X.Ty) || !isValid(Target))
    return X.Ty;
  auto Fail = [&](ReprCode C = ReprCode::Invalid) -> Type * {
    if (Code)
      *Code = C;
    return nullptr;
  };
  if (isUntyped(Target)) {
    if (Type *M = maxType(X.Ty, Target))
      return M;
    return Fail();
  }
  if (X.isNil()) {
    if (hasNil(Target))
      return Target;
    return Fail();
  }
  Type *U = under(Target);
  if (auto *Op = dyn_cast<OptionType>(U)) {
    // `var n Option[int] = 5`: the constant takes the type of T; the
    // assignment then converts T to Option[T] (ADR-0018 point 3).
    if (!own())
      return Fail();
    return implicitTypeAndValue(X, Op->Elem, NewVal, Code);
  }
  if (auto *Rf = dyn_cast<RefType>(U)) {
    // A string constant is a ref string; a nil/untyped slice cannot be.
    if (isString(Rf->Elem) && X.Ty == typ(BasicKind::UntypedString))
      return Target;
    return Fail();
  }
  if (auto *B = dyn_cast<BasicType>(U)) {
    if (X.M == Mode::Constant) {
      ConstValue V;
      ReprCode C = representation(X, B, V);
      if (C != ReprCode::Ok)
        return Fail(C);
      if (NewVal)
        *NewVal = V;
      return Target;
    }
    switch (cast<BasicType>(X.Ty)->BK) {
    case BasicKind::UntypedBool:
      if (!isBoolean(Target))
        return Fail();
      break;
    case BasicKind::UntypedInt:
    case BasicKind::UntypedRune:
    case BasicKind::UntypedFloat:
    case BasicKind::UntypedComplex:
      if (!isNumeric(Target))
        return Fail();
      break;
    case BasicKind::UntypedString:
      if (!isString(Target))
        return Fail();
      break;
    default:
      return Fail();
    }
    return Target;
  }
  if (isa<InterfaceType>(U)) {
    if (isTypeParam(Target)) {
      bool Ok = underIs(Target, [&](Type *Uu) {
        if (!Uu)
          return false;
        Type *T2 = implicitTypeAndValue(X, Uu, nullptr, nullptr);
        return T2 != nullptr;
      });
      if (!Ok)
        return Fail();
      return Target;
    }
    if (!typeSet(cast<InterfaceType>(U))->isAll())
      return Fail();
    return defaultType(X.Ty);
  }
  return Fail();
}

//===----------------------------------------------------------------------===//
// Assignability
//===----------------------------------------------------------------------===//

bool Checker::assignableTo(Operand &X, Type *T, std::string *Cause) {
  if (X.M == Mode::Invalid || !isValid(T))
    return true;
  Type *V = X.Ty;
  if (identical(V, T))
    return true;
  Type *Vu = under(V), *Tu = under(T);
  auto *Vp = dyn_cast<TypeParamType>(V);
  auto *Tp = dyn_cast<TypeParamType>(T);

  if (isUntyped(Vu)) {
    if (Tp) {
      return typeParamIs(Tp, [&](const Term *Tm) {
        if (!Tm)
          return false;
        return implicitTypeAndValue(X, Tm->T, nullptr, nullptr) != nullptr;
      });
    }
    return implicitTypeAndValue(X, T, nullptr, nullptr) != nullptr;
  }

  if (identical(Vu, Tu) && (!hasName(V) || !hasName(T)) && !Vp && !Tp)
    return true;

  // Go++ (ADR-0018): T converts to Option[T] (also a Unique/Shared place to
  // Option[*T], a borrow); an Option[Unique[*T]] place borrows as Option[*T];
  // an Option converts to an interface through its T (nil stays nil).
  // A type parameter T converts to Option[T] (a generic "first or nothing");
  // whether Option[T] is valid for T is checked at the instantiation.
  if (own() && Vp && !Tp)
    if (auto *To = dyn_cast<OptionType>(Tu))
      if (identical(V, To->Elem))
        return true;
  if (own() && !Vp && !Tp) {
    if (auto *To = dyn_cast<OptionType>(Tu)) {
      if (auto *From = dyn_cast<OptionType>(Vu)) {
        PtrKind K;
        Type *Elem;
        if (hasPtrKind(From->Elem, &K, &Elem) && K != PtrKind::Weak &&
            isa<PointerType>(To->Elem) &&
            identical(Elem, cast<PointerType>(To->Elem)->Elem)) {
          if (X.E && placeKind(X.E) == PlaceKind::Temp) {
            if (Cause)
              *Cause = "cannot borrow a temporary " + str(V) +
                       ": the target would be freed immediately (OWN-P6)";
            return false;
          }
          return true;
        }
      } else if (assignableTo(X, To->Elem, Cause)) {
        return true;
      }
      // An Option[T] converts to an Option[I] for an interface I with
      // methods that T implements: nil stays nil, a set value boxes (OWN-P9).
      if (auto *From = dyn_cast<OptionType>(Vu))
        if (neverNilKind(To->Elem) && isa<InterfaceType>(under(To->Elem)) &&
            implements(X.pos(), From->Elem, To->Elem, false, nullptr))
          return true;
    }
    if (auto *From = dyn_cast<OptionType>(Vu))
      if (isa<InterfaceType>(Tu)) {
        // A nil Option becomes a nil interface, which only `error` and the
        // empty interface have; an interface with methods is never nil, so
        // the target there is an Option[I].
        if (neverNilKind(T)) {
          if (Cause)
            *Cause = "an Option may be nil, and " + str(T) +
                     " is never nil: convert it to Option[" + str(T) + "]";
          return false;
        }
        if (implements(X.pos(), From->Elem, T, false, Cause))
          return true;
        return false;
      }
  }

  if ((isa<InterfaceType>(Tu) && !Tp) || isInterfacePtr(Tu)) {
    if (implements(X.pos(), V, T, false, Cause))
      return true;
    if (!Vp)
      return false;
    if (Cause)
      Cause->clear();
  }

  if (auto *Vi = dyn_cast<InterfaceType>(Vu)) {
    (void)Vi;
    if (!Vp && implements(X.pos(), T, V, false, nullptr)) {
      if (Cause)
        *Cause = "need type assertion";
      return false;
    }
  }

  if (auto *Vc = dyn_cast<ChanType>(Vu))
    if (Vc->D == ChanType::Both)
      if (auto *Tc = dyn_cast<ChanType>(Tu))
        if (identical(Vc->Elem, Tc->Elem))
          return !hasName(V) || !hasName(T);

  // Go++ (TYP-2): owned string / []T to ref string / ref []T.
  if (own())
    if (auto *Tr = dyn_cast<RefType>(Tu))
      if (!Tp && identical(Vu, Tr->Elem))
        return true;
  // Go++ (PAR-8, OWN-V4): a borrowed string / []T parameter, and a map key
  // used for a lookup, take a ref at zero cost.
  if (own() && RefToOwnedOk)
    if (auto *Vr = dyn_cast<RefType>(Vu))
      if (!Tp && identical(Vr->Elem, T))
        return true;
  // Go++ (OWN-P6, TYP-2): Unique[*T] / Shared[*T] to *T, borrowed from a
  // place.
  if (own())
    if (auto *Tptr = dyn_cast<PointerType>(Tu)) {
      PtrKind K;
      Type *Elem;
      if (hasPtrKind(V, &K, &Elem) && K != PtrKind::Weak &&
          identical(Elem, Tptr->Elem)) {
        if (X.E && placeKind(X.E) == PlaceKind::Temp) {
          if (Cause)
            *Cause = "cannot borrow a temporary " + str(V) +
                     ": the target would be freed immediately (OWN-P6)";
          return false;
        }
        return true;
      }
    }
  if (own() && !Tp && !Vp && X.E)
    if (auto *From = dyn_cast<OptionType>(Vu))
      if (Cause && !isa<InterfaceType>(Tu) &&
          (identical(From->Elem, T) ||
           (isa<PointerType>(Tu) &&
            hasPtrKind(From->Elem, nullptr, nullptr)))) {
        *Cause = str(X.E) + " may be nil; " + nilCheckHint(X.E);
        return false;
      }
  if (own() && isa<RefType>(Vu) && !isa<RefType>(Tu) && !Tp && !Vp &&
      identical(cast<RefType>(Vu)->Elem, Tu) && Cause)
    *Cause = "a ref is not an owning value: write " + str(T) + "(" + str(X.E) +
             ") to copy (TYP-2)";

  if (!Vp && !Tp)
    return false;

  auto AddCause = [&](const std::string &Msg) {
    if (Cause)
      *Cause = Cause->empty() ? Msg : Msg + "\n\t" + *Cause;
  };
  if (!hasName(V) && Tp) {
    bool Ok = false;
    typeParamIs(Tp, [&](const Term *Tm) {
      if (!Tm)
        return false;
      Ok = assignableTo(X, Tm->T, Cause);
      if (!Ok) {
        AddCause("cannot assign " + str(X.Ty) + " to " + str(Tm->T) + " (in " +
                 str(Tp) + ")");
        return false;
      }
      return true;
    });
    return Ok;
  }
  if (Vp && !hasName(T)) {
    Operand Y = X;
    bool Ok = false;
    typeParamIs(Vp, [&](const Term *Tm) {
      if (!Tm)
        return false;
      Y.Ty = Tm->T;
      Ok = assignableTo(Y, T, Cause);
      if (!Ok) {
        AddCause("cannot assign " + str(Tm->T) + " (in " + str(Vp) + ") to " +
                 str(T));
        return false;
      }
      return true;
    });
    return Ok;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Conversion
//===----------------------------------------------------------------------===//

bool Checker::convertibleTo(Operand &X, Type *T, std::string *Cause) {
  if (assignableTo(X, T, Cause))
    return true;
  Type *V = X.Ty;
  Type *Vu = under(V), *Tu = under(T);
  auto *Vp = dyn_cast<TypeParamType>(V);
  auto *Tp = dyn_cast<TypeParamType>(T);

  if (identicalIgnoreTags(Vu, Tu) && !Vp && !Tp)
    return true;
  if (auto *VP = dyn_cast<PointerType>(V))
    if (auto *TP = dyn_cast<PointerType>(T))
      if (identicalIgnoreTags(under(VP->Elem), under(TP->Elem)) &&
          !isTypeParam(VP->Elem) && !isTypeParam(TP->Elem))
        return true;
  if (isIntegerOrFloat(Vu) && isIntegerOrFloat(Tu))
    return true;
  if (isComplex(Vu) && isComplex(Tu))
    return true;
  if ((isInteger(Vu) || isBytesOrRunes(Vu)) && isString(Tu))
    return true;
  if (isString(Vu) && isBytesOrRunes(Tu))
    return true;
  if ((isPointer(Vu) || isUintptr(Vu)) && isUnsafePointer(Tu))
    return true;
  if (isUnsafePointer(Vu) && (isPointer(Tu) || isUintptr(Tu)))
    return true;
  if (auto *S = dyn_cast<SliceType>(Vu)) {
    if (auto *A = dyn_cast<ArrayType>(Tu))
      if (identical(S->Elem, A->Elem))
        return true;
    if (auto *P = dyn_cast<PointerType>(Tu))
      if (auto *A = dyn_cast<ArrayType>(under(P->Elem)))
        if (identical(S->Elem, A->Elem))
          return true;
  }
  // Go++ (EXP-9): string(v) / []T(v) copy a ref into a new owned value.
  if (own())
    if (auto *Vr = dyn_cast<RefType>(Vu)) {
      Type *E = Vr->Elem;
      if (identicalIgnoreTags(under(E), Tu) && !Tp) {
        if (auto *Sl = dyn_cast<SliceType>(Tu)) {
          std::string Why;
          if (!satisfiesClone(Sl->Elem, &Why)) {
            if (Cause)
              *Cause = "element type " + str(Sl->Elem) +
                       " does not satisfy Clone (OWN-V3)";
            return false;
          }
        }
        return true;
      }
      if (isString(E) && isBytesOrRunes(Tu) && !Tp)
        return true;
      if (isBytesOrRunes(E) && isString(Tu))
        return true;
    }

  if (!Vp && !Tp)
    return false;

  if (Vp && Tp) {
    Operand Y = X;
    return typeParamIs(Vp, [&](const Term *VT) {
      if (!VT)
        return false;
      Y.Ty = VT->T;
      return typeParamIs(Tp, [&](const Term *TT) {
        if (!TT)
          return false;
        if (!convertibleTo(Y, TT->T, Cause)) {
          if (Cause)
            *Cause = "cannot convert " + str(VT->T) + " (in " + str(Vp) +
                     ") to type " + str(TT->T) + " (in " + str(Tp) + ")";
          return false;
        }
        return true;
      });
    });
  }
  if (Vp) {
    Operand Y = X;
    return typeParamIs(Vp, [&](const Term *VT) {
      if (!VT)
        return false;
      Y.Ty = VT->T;
      if (!convertibleTo(Y, T, Cause)) {
        if (Cause)
          *Cause = "cannot convert " + str(VT->T) + " (in " + str(Vp) +
                   ") to type " + str(T);
        return false;
      }
      return true;
    });
  }
  return typeParamIs(Tp, [&](const Term *TT) {
    if (!TT)
      return false;
    if (!convertibleTo(X, TT->T, Cause)) {
      if (Cause)
        *Cause = "cannot convert " + str(V) + " to type " + str(TT->T) +
                 " (in " + str(Tp) + ")";
      return false;
    }
    return true;
  });
}

bool Checker::constConvertibleTo(Operand &X, Type *T, ConstValue *Val) {
  auto *B = dyn_cast<BasicType>(under(T));
  if (!B)
    return false;
  if (representableConst(X.Val, B, Val))
    return true;
  if (isInteger(X.Ty) && (B->Info & IsString)) {
    uint32_t Cp = 0xFFFD;
    uint64_t U;
    if (X.Val.fitsUint64(U) && U <= 0x10FFFF)
      Cp = static_cast<uint32_t>(U);
    if (Val) {
      std::string S;
      appendUTF8(S, Cp);
      *Val = ConstValue::makeString(S);
    }
    return true;
  }
  return false;
}

void Checker::conversion(Operand &X, Type *T) {
  // An interface type still being declared has no methods yet: anything
  // converts to it (types2: an incomplete interface).
  if (auto *N = dyn_cast<NamedType>(T); N && N->ResolvingInterface) {
    X.M = Mode::Value;
    X.Ty = T;
    return;
  }
  bool ConstArg = X.M == Mode::Constant;
  bool Ok = false;
  std::string Cause;
  // P03.19: `T(o)` converts the Option itself where only a loop's assumption
  // narrows o and the Option converts to T as well (unnarrowTentative).
  if (Type *OT = ConstArg ? nullptr : tentOption(X)) {
    Operand O = X;
    O.Ty = OT;
    if (convertibleTo(O, T, nullptr))
      unnarrowTentative(X);
  }
  if (ConstArg && isConstType(T)) {
    ConstValue NV;
    Ok = constConvertibleTo(X, T, &NV);
    if (Ok)
      X.Val = NV;
    if (!Ok && isInteger(X.Ty) && isInteger(T)) {
      err(X, diag::err_constant_overflows, {X.Val.str(), str(T)});
      X.M = Mode::Invalid;
      return;
    }
    if (Ok)
      X.M = Mode::Constant;
  } else if (ConstArg && isTypeParam(T)) {
    Ok = underIs(T, [&](Type *U) {
      if (!U) {
        Cause = str(T) + " does not contain specific types";
        return false;
      }
      if (isString(X.Ty) && isBytesOrRunes(U))
        return true;
      if (!constConvertibleTo(X, U, nullptr)) {
        if (isInteger(X.Ty) && isInteger(U))
          Cause = "constant " + X.Val.str() + " overflows " + str(U) + " (in " +
                  str(T) + ")";
        else
          Cause = "cannot convert " + str(X) + " to type " + str(U) + " (in " +
                  str(T) + ")";
        return false;
      }
      return true;
    });
    X.M = Mode::Value;
  } else if (convertibleTo(X, T, &Cause)) {
    Ok = true;
    X.M = Mode::Value;
  }
  if (!Ok) {
    if (!Cause.empty())
      err(X, diag::err_cannot_convert_cause, {str(X), str(T), Cause});
    else
      err(X, diag::err_cannot_convert, {str(X), str(T)});
    X.M = Mode::Invalid;
    return;
  }
  if (checkNilableToNeverNil(X, T))
    return;
  // P03.19: `(*[N]T)(s)` points into s's storage, so it is a pointer into
  // the box when s is a part of one (as `&s[0]` is): making it is the error.
  if (own() && X.E && !ConstArg)
    if (auto *P = llvm::dyn_cast_or_null<PointerType>(coreType(T));
        P && isa<sema::ArrayType>(under(P->Elem)) &&
        llvm::isa_and_nonnull<SliceType>(coreType(X.Ty)))
      rejectBoxWrite(X.E, X.E, true,
                     "through (" + str(T) + ")(" + str(X.E) + ")");
  if (isUntyped(X.Ty)) {
    Type *Final = T;
    if (isNonTypeParamInterface(T) || (ConstArg && !isConstType(T)))
      Final = defaultType(X.Ty);
    else if (X.M == Mode::Constant && isInteger(X.Ty) && allString(T))
      Final = X.Ty;
    updateExprType(X.E, Final, true);
  }
  X.Ty = T;
}
