//===- UnitCheck.cpp - Units of measure (ADR-0025) ------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The rules of UNI-1 to UNI-16 and EXP-31 to EXP-37, each decided in one
// place:
//
//   what a unit is                  resolveUnit / unitDecl (a unit table per
//                                   package; an iterative walk, UNI-3)
//   what a unit expression means    unitExpr (flat terms, accumulated and
//                                   interned once, UNI-4, UNI-5)
//   `T[U]`                          unitTypeIndex (UNI-6, UNI-7)
//   the unit of a type              unitOf (the one question every rule asks)
//   `+ - % <=> < == ...`            unitAdditive (EXP-34, EXP-35)
//   `* /`                           unitPre + unitPostMulDiv (EXP-33)
//   `<< >> & | ^ &^`                unitBinary (EXP-36)
//   `T(x)`                          unitConversion (EXP-32)
//   `x.In[U]()`                     unitInCall (EXP-32)
//   a plain constant for a unit     unitZeroFits (EXP-35, the zero exception)
//
// A unit type is a BasicType of the base kind with the unit set, so every
// rule for numbers (arithmetic, ranges, representability, the zero value)
// holds without a second copy; only the rules above look at the unit.
//
// Nothing is rounded or wrapped here: factors are exact rationals, a value
// that cannot be converted exactly is an error.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"
#include "Units.h"

#include "llvm/ADT/SmallPtrSet.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

//===----------------------------------------------------------------------===//
// The unit of a type
//===----------------------------------------------------------------------===//

const UnitCanon *Checker::unitOf(Type *T) {
  if (!T || !Ctx.unitsSeen())
    return nullptr;
  Type *U = isa<BasicType>(T) ? T : under(T);
  auto *B = dyn_cast_or_null<BasicType>(U);
  return B && B->BK != BasicKind::UnitArg ? B->Unit : nullptr;
}

bool Checker::isUnitBound(Type *Bound) {
  if (!Bound)
    return false;
  Type *U = isa<NamedType>(Bound) ? cast<NamedType>(Bound)->Underlying : Bound;
  auto *I = dyn_cast_or_null<InterfaceType>(U);
  if (!I)
    return false;
  if (I->Sp == InterfaceType::Unit)
    return true;
  // `[U interface{ unit }]` is the same constraint; `unit` combined with
  // other elements is reported once, where the interface is written.
  for (Type *E : I->Embeddeds)
    if (isUnitBound(E))
      return true;
  return false;
}

/// The unit a type argument of a unit parameter stands for (null if it is
/// not one).
const UnitCanon *Checker::unitArgCanon(Type *T) {
  if (auto *B = dyn_cast_or_null<BasicType>(T))
    return B->BK == BasicKind::UnitArg ? B->Unit : nullptr;
  if (isUnitParam(T))
    return units::base(Ctx, nullptr, cast<TypeParamType>(T));
  return nullptr;
}

bool Checker::isUnitParam(Type *T) {
  auto *P = dyn_cast_or_null<TypeParamType>(T);
  return P && isUnitBound(P->Bound);
}

/// The base kind of a numeric type (`float64` for `float64[KM]` and for
/// `type Distance float64[M]`).
BasicKind Checker::baseKind(Type *T) {
  auto *B = dyn_cast_or_null<BasicType>(isa<BasicType>(T) ? T : under(T));
  return B ? B->BK : BasicKind::Invalid;
}

/// The type of kind BK with unit U (plain if U is null).
Type *Checker::withUnit(BasicKind BK, const UnitCanon *U) {
  return Ctx.unitType(BK, U);
}

static std::string describeObject(const Object *O) {
  switch (O->getKind()) {
  case Object::Kind::PkgName:
    return "a package";
  case Object::Kind::Const:
    return "a constant";
  case Object::Kind::TypeName:
    return "a type";
  case Object::Kind::Var:
    return "a variable";
  case Object::Kind::Func:
    return "a function";
  case Object::Kind::Builtin:
    return "a built-in function";
  case Object::Kind::Nil:
    return "nil";
  case Object::Kind::Label:
    return "a label";
  case Object::Kind::Unit:
    return "a unit";
  }
  return "something else";
}

void Checker::unitStatusError(const ast::Node *At, UnitStatus St,
                              StringRef What) {
  if (St == UnitStatus::ExpRange)
    err(At, diag::err_unit_scale,
        {std::string(What) + ": an exponent is out of range (at most " +
         std::to_string(units::MaxExp) + ")"});
  else
    err(At, diag::err_unit_scale,
        {std::string(What) +
         ": the factor is no longer an exact rational (it needs more than " +
         std::to_string(Real::MaxRatBits) + " bits)"});
}

//===----------------------------------------------------------------------===//
// Names in unit expressions
//===----------------------------------------------------------------------===//

/// The object a unit name denotes (`KM`, `units.KM`), reporting what is
/// undefined or unexported; null after an error.
Object *Checker::unitLookup(const ast::Expr *N) {
  if (auto *Id = dyn_cast<ast::Ident>(N)) {
    StringRef Nm = name(Id);
    Scope *Found = nullptr;
    Object *Obj = E.Sc->lookup(Nm, E.Pos, &Found);
    if (!Obj) {
      err(Id, diag::err_undefined, {std::string(Nm)});
      return nullptr;
    }
    recordUse(Id, Obj);
    if (Found && !isa<PkgName>(Obj) && StringRef(Found->Comment) == "file") {
      auto It = S.DotImportMap.find({Found, Nm});
      if (It != S.DotImportMap.end())
        markUsed(It->second);
    }
    return Obj;
  }
  auto *Sel = cast<SelectorExpr>(N);
  Package *P = qualifierPackage(Sel->X);
  if (!P) {
    err(N, diag::err_unit_expr,
        {"expected a unit name or `package.Unit`, found " + str(N)});
    return nullptr;
  }
  StringRef Nm = name(Sel->Sel);
  Object *Obj = P->PkgScope ? P->PkgScope->lookupLocal(Nm) : nullptr;
  if (!Obj) {
    err(Sel->Sel, diag::err_undefined, {str(N)});
    return nullptr;
  }
  if (!Obj->exported()) {
    err(Sel->Sel, diag::err_unexported_name,
        {std::string(Nm), std::string(P->Name)});
    return nullptr;
  }
  recordUse(Sel->Sel, Obj);
  return Obj;
}

/// The unit a name denotes, as a display part (a declared unit or a unit
/// type parameter) with its canonical form; false after an error (reported)
/// and for a unit whose declaration had one (it reported itself).
bool Checker::unitNamePart(const ast::Expr *N, UnitDim &Part,
                           const UnitCanon *&Canon) {
  Object *Obj = unitLookup(N);
  if (!Obj)
    return false;
  if (auto *U = dyn_cast<UnitObj>(Obj)) {
    if (!U->Resolved)
      resolveUnit(U);
    if (U->Bad || !U->Canon)
      return false;
    Part = UnitDim();
    Part.Unit = U;
    Canon = U->Canon;
    return true;
  }
  if (auto *TN = dyn_cast<TypeName>(Obj)) {
    if (auto *TP = dyn_cast_or_null<TypeParamType>(TN->Ty)) {
      if (isUnitBound(TP->Bound)) {
        Part = UnitDim();
        Part.Param = TP;
        Canon = units::base(Ctx, nullptr, TP);
        return true;
      }
      err(N, diag::err_unit_not_unit,
          {str(N), "a type parameter without the constraint `unit` (write "
                   "`[" +
                       std::string(TN->Name) + " unit]`)"});
      return false;
    }
  }
  err(N, diag::err_unit_not_unit, {str(N), describeObject(Obj)});
  return false;
}

//===----------------------------------------------------------------------===//
// Unit expressions (UNI-4, UNI-5)
//===----------------------------------------------------------------------===//

/// The written exponent of a term: false if it is not a number of at most
/// MaxExp.
static bool termPower(const ast::UnitTerm &T, int64_t &N) {
  N = 1;
  if (!T.Power && !T.CaretLoc.isValid())
    return true;
  if (!T.Power)
    return false; // `S^` without a number (a syntax error already)
  ConstValue V = ConstValue::fromLiteral(T.Power->Text, tok::int_lit);
  int64_t I;
  if (V.isUnknown() || !V.fitsInt64(I) || I < 0 || I > units::MaxExp)
    return false;
  N = T.NegPower ? -I : I;
  return true;
}

/// A * B for exponents, false past MaxExp.
static bool mulExp(int64_t A, int64_t B, int64_t &Out) {
  if (__builtin_mul_overflow(A, B, &Out))
    return false;
  return Out <= units::MaxExp && Out >= -units::MaxExp;
}

const UnitCanon *Checker::unitExpr(const ast::Expr *Root,
                                   const UnitDisplay **DispOut) {
  if (DispOut)
    *DispOut = nullptr;
  // An explicit work list of (expression, the exponent it is raised to), so
  // that parentheses nested to the parser's limit and a product of any
  // length take no stack. Every name is looked up where it is met, in
  // source order (the errors come in that order); its exponents are summed,
  // and each distinct name is applied once at the end.
  struct Item {
    const ast::Expr *E;
    int64_t Mult;
  };
  SmallVector<Item, 8> Work{{Root, 1}};
  SmallVector<UnitDim, 8> Parts; // the names, with their summed exponents
  SmallVector<const UnitCanon *, 8> PartCanon;
  SmallVector<int64_t, 8> PartExp;
  llvm::DenseMap<const void *, unsigned> PartAt;
  bool Ok = true;
  auto AddName = [&](const ast::Expr *N, int64_t Mult) {
    UnitDim P;
    const UnitCanon *C = nullptr;
    if (!unitNamePart(N, P, C)) {
      Ok = false;
      return;
    }
    const void *Key = P.Unit ? (const void *)P.Unit : (const void *)P.Param;
    auto [It, New] = PartAt.try_emplace(Key, Parts.size());
    if (New) {
      Parts.push_back(P);
      PartCanon.push_back(C);
      PartExp.push_back(0);
    }
    int64_t &Sum = PartExp[It->second];
    Sum += Mult;
    if (Sum > units::MaxExp || Sum < -units::MaxExp) {
      unitStatusError(N, UnitStatus::ExpRange, "in " + str(Root));
      Ok = false;
    }
  };
  while (!Work.empty()) {
    Item It = Work.pop_back_val();
    const ast::Expr *X = It.E;
    switch (X->getKind()) {
    case ast::Node::Kind::Ident:
    case ast::Node::Kind::SelectorExpr:
      AddName(X, It.Mult);
      break;
    case ast::Node::Kind::BadExpr:
      Ok = false;
      break;
    case ast::Node::Kind::BasicLit: {
      auto *L = cast<BasicLit>(X);
      if (L->LitKind == tok::int_lit) {
        ConstValue V = ConstValue::fromLiteral(L->Text, tok::int_lit);
        int64_t I;
        if (!V.isUnknown() && V.fitsInt64(I) && I == 1)
          break; // `1`: no unit (`1/S`)
      }
      err(X, diag::err_unit_expr,
          {"the only number in a unit expression is 1 (as in `1/S`), not " +
           L->Text.str()});
      Ok = false;
      break;
    }
    case ast::Node::Kind::ParenExpr:
      Work.push_back({cast<ParenExpr>(X)->X, It.Mult});
      break;
    case ast::Node::Kind::UnitExpr: {
      auto *U = cast<ast::UnitExpr>(X);
      // Pushed last to first, so the first term is taken first.
      for (size_t K = U->Terms.size(); K-- > 0;) {
        const ast::UnitTerm &T = U->Terms[K];
        int64_t N, M;
        if (!termPower(T, N)) {
          err(T.Power ? static_cast<const ast::Node *>(T.Power)
                      : static_cast<const ast::Node *>(U),
              diag::err_unit_expr,
              {"the exponent must be a written integer of at most " +
               std::to_string(units::MaxExp) + " (no fractional powers)"});
          Ok = false;
          continue;
        }
        if (!mulExp(It.Mult, T.Div ? -N : N, M)) {
          unitStatusError(T.Base, UnitStatus::ExpRange, "in " + str(Root));
          Ok = false;
          continue;
        }
        Work.push_back({T.Base, M}); // (`X^0` still checks X)
      }
      break;
    }
    case ast::Node::Kind::BinaryExpr: {
      // The unit written in the brackets of an expression (`Avg[M/S](xs)`,
      // UNI-7): `*` and `/` only; there `^` is Go's xor.
      auto *B = cast<ast::BinaryExpr>(X);
      if (B->Op == tok::xor_) {
        err(B->OpLoc, diag::err_unit_expr,
            {"`^` is Go's xor in an expression, so it is not a power here; "
             "name the unit with a `unit` declaration, or let inference find "
             "it (UNI-7)"});
        Ok = false;
        Work.clear();
        break;
      }
      if (B->Op != tok::mul && B->Op != tok::quo) {
        err(B->OpLoc, diag::err_unit_expr,
            {std::string("the operator ") + tok::getSpelling(B->Op) +
             " is not a unit operator; units have * / and a power"});
        Ok = false;
        Work.clear();
        break;
      }
      Work.push_back({B->Y, B->Op == tok::quo ? -It.Mult : It.Mult});
      Work.push_back({B->X, It.Mult});
      break;
    }
    default:
      err(X, diag::err_unit_expr,
          {"expected a unit (a name, 1, or a product or quotient of units), "
           "found " +
           str(X)});
      Ok = false;
      break;
    }
  }
  if (!Ok)
    return nullptr;
  // The canonical form: each distinct name once, raised to its exponent.
  SmallVector<UnitDim, 8> Dims;
  Real Factor = units::realOne();
  for (size_t K = 0; K < Parts.size(); ++K) {
    int64_t E = PartExp[K];
    if (E == 0)
      continue;
    for (UnitDim D : PartCanon[K]->Dims) {
      int64_t Ex;
      if (!mulExp(D.Exp, E, Ex)) {
        unitStatusError(Root, UnitStatus::ExpRange, "in " + str(Root));
        return nullptr;
      }
      D.Exp = static_cast<int32_t>(Ex);
      Dims.push_back(D);
    }
    if (!units::isOne(PartCanon[K]->Factor)) {
      Real F;
      if (!units::realPow(PartCanon[K]->Factor, E, F) ||
          !(Factor = Real::mul(Factor, F)).isRat()) {
        unitStatusError(Root, UnitStatus::FactorRange, "in " + str(Root));
        return nullptr;
      }
    }
  }
  UnitStatus St;
  const UnitCanon *R = units::make(Ctx, Dims, Factor, St);
  if (!R) {
    unitStatusError(Root, St, "in " + str(Root));
    return nullptr;
  }
  for (size_t K = 0; K < Parts.size(); ++K)
    Parts[K].Exp = static_cast<int32_t>(PartExp[K]);
  const UnitDisplay *D = units::display(Ctx, Parts, St);
  if (D)
    unitNamer().offer(R, D); // a form as written (UNI-5)
  if (DispOut)
    *DispOut = D;
  return R;
}

/// A unit expression as a type argument of a `unit` parameter (GEN-14).
Type *Checker::unitArgType(const ast::Expr *X) {
  const UnitCanon *C = unitExpr(X);
  if (!C)
    return invalid();
  if (C->dimensionless()) {
    // A unit parameter stands for a unit, which has a dimension (UNI-6).
    err(X, diag::err_unit_generic,
        {str(X) + " has an empty dimension, so it is not a unit"});
    return invalid();
  }
  return Ctx.unitArgType(C);
}

//===----------------------------------------------------------------------===//
// Collapse sites of unit-generic code (GEN-14)
//===----------------------------------------------------------------------===//
//
// A unit of generic code may have an empty dimension at one instance and not
// at another (`U/V` with U = KM, V = M is the plain factor 1000). The code is
// checked once, so it computes as plain code does at run time: a value whose
// unit has an empty dimension at the instance is the plain number (EXP-33),
// every other value the number in its unit. The few operations whose value
// depends on that (a product or quotient, a scale change, a number given a
// unit) are recorded with their units over the parameters (Info::UnitSites),
// and their unit parameters need their real units at run time: a Use::UnitDict
// of each, passed on by the use fixpoint to the parameters of a caller that
// gives them (a boolean per parameter, so the check stays linear).

/// Is the correction of `x * y` or `x / y` 1 at every instance? So when an
/// operand is a plain number (the unit of the other is kept, or inverted),
/// when `x / x` cancels exactly, and when every unit is a power of one unit
/// parameter with no other part and the factor 1 (c(P^k) = c(P)^k).
static bool alwaysExact(const UnitCanon *Res, const UnitCanon *X,
                        const UnitCanon *Y, bool Quo) {
  if (!X || !Y || (Quo && X == Y))
    return true;
  const TypeParamType *P = nullptr;
  for (const UnitCanon *U : {Res, X, Y}) {
    if (!U)
      continue;
    if (!units::isOne(U->Factor) || U->Dims.size() > 1 ||
        (U->Dims.size() == 1 && !U->Dims[0].Param))
      return false;
    if (U->Dims.empty())
      continue;
    if (P && P != U->Dims[0].Param)
      return false;
    P = U->Dims[0].Param;
  }
  return true;
}

void Checker::recordUnitSite(const ast::Expr *Ex, UnitSite::Kind K,
                             const UnitCanon *Res, const UnitCanon *X,
                             const UnitCanon *Y, bool Tag, bool Replace) {
  if (!Ex || S.Opts.GoCompat)
    return;
  // Every unit that mentions a unit parameter may be empty at an instance:
  // the parameter itself may be (a caller passes `V/W`). The one predicate
  // for all the operations whose value depends on it.
  auto Generic = [](const UnitCanon *U) { return U && U->HasParam; };
  bool Need = false;
  switch (K) {
  case UnitSite::Const:
    Need = Generic(Res);
    break;
  case UnitSite::Conv:
    Need = X != Res && (Generic(X) || Generic(Res));
    break;
  case UnitSite::Mul:
  case UnitSite::Quo:
    Need = (Generic(Res) || Generic(X) || Generic(Y)) &&
           !alwaysExact(Res, X, Y, K == UnitSite::Quo);
    break;
  }
  if (!Need)
    return;
  bool Operand = K == UnitSite::Conv;
  auto &L = I.UnitSites[Ex];
  auto It = llvm::find_if(
      L, [&](const UnitSite &O) { return (O.K == UnitSite::Conv) == Operand; });
  UnitSite New{K, Tag, Res, X, Y};
  if (It == L.end())
    L.push_back(New);
  else if (Replace || It->K != UnitSite::Const)
    *It = New;
  for (const UnitCanon *U : {Res, X, Y})
    for (size_t D = 0; U && D < U->Dims.size(); ++D)
      if (U->Dims[D].Param)
        noteUse(const_cast<TypeParamType *>(U->Dims[D].Param),
                TypeParamType::Use(TypeParamType::Use::UnitDict));
}

std::string Checker::unitRangeWhy(Type *Generic, ArrayRef<TypeParamType *> TPs,
                                  ArrayRef<Type *> TAs, StringRef Name) {
  if (!Ctx.unitsSeen() || !Generic)
    return {};
  auto Rep = [&](const TypeParamType *P) -> const UnitCanon * {
    for (size_t K = 0; K < TPs.size() && K < TAs.size(); ++K)
      if (TPs[K] == P || TPs[K]->RenamedFrom == P)
        return unitArgCanon(TAs[K]);
    return nullptr;
  };
  // The unit types of the signature or the fields, as plain code would
  // write them for this instance (a walk over the type, each part once).
  SmallVector<Type *, 16> Work{Generic};
  llvm::SmallPtrSet<const Type *, 16> Seen;
  while (!Work.empty()) {
    Type *X = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    switch (X->getKind()) {
    case Type::Kind::Basic: {
      auto *B = cast<BasicType>(X);
      if (!B->Unit || !B->Unit->HasParam)
        break;
      // A unit that also mentions a parameter the instance does not give (a
      // generic method's own one) is decided where the method is called
      // (GEN-14, owner decision 2026-10-30).
      if (llvm::any_of(B->Unit->Dims, [&](const UnitDim &D) {
            return D.Param && !Rep(D.Param);
          }))
        break;
      UnitStatus St;
      if (units::substitute(Ctx, B->Unit, Rep, St) || St == UnitStatus::Ok)
        break;
      std::string Args;
      for (size_t K = 0; K < TPs.size() && K < TAs.size(); ++K)
        if (isUnitParam(TPs[K])) {
          if (!Args.empty())
            Args += ", ";
          Args += str(TPs[K]) + " = " + str(TAs[K]);
        }
      // E0435, as plain code's float64[KM^1000] is (UNI-2).
      return "in the instance of " + std::string(Name) + " with " + Args +
             ", " + str(X) +
             (St == UnitStatus::ExpRange
                  ? ": an exponent is out of range (at most " +
                        std::to_string(units::MaxExp) + ")"
                  : ": the factor is no longer an exact rational (it needs "
                    "more than " +
                        std::to_string(Real::MaxRatBits) + " bits)");
    }
    case Type::Kind::Pointer:
      Work.push_back(cast<PointerType>(X)->Elem);
      break;
    case Type::Kind::Slice:
      Work.push_back(cast<SliceType>(X)->Elem);
      break;
    case Type::Kind::Ref:
      Work.push_back(cast<RefType>(X)->Elem);
      break;
    case Type::Kind::Option:
      Work.push_back(cast<OptionType>(X)->Elem);
      break;
    case Type::Kind::Array:
      Work.push_back(cast<sema::ArrayType>(X)->Elem);
      break;
    case Type::Kind::Chan:
      Work.push_back(cast<sema::ChanType>(X)->Elem);
      break;
    case Type::Kind::Map:
      Work.push_back(cast<sema::MapType>(X)->Key);
      Work.push_back(cast<sema::MapType>(X)->Elem);
      break;
    case Type::Kind::Tuple:
      for (Type *T : cast<TupleType>(X)->Elems)
        Work.push_back(T);
      break;
    case Type::Kind::Signature:
      Work.push_back(cast<SignatureType>(X)->Params);
      Work.push_back(cast<SignatureType>(X)->Results);
      break;
    case Type::Kind::Struct:
      for (Var *F : cast<StructType>(X)->Fields)
        Work.push_back(F->Ty);
      break;
    case Type::Kind::Named:
      // An instance of another generic inside: its unit arguments.
      for (Type *T : cast<NamedType>(X)->TArgs)
        Work.push_back(T);
      break;
    default:
      break;
    }
  }
  return {};
}

bool Checker::unitInstanceRange(SourceLoc Pos, Type *Generic,
                                ArrayRef<TypeParamType *> TPs,
                                ArrayRef<Type *> TAs, StringRef Name) {
  std::string Why = unitRangeWhy(Generic, TPs, TAs, Name);
  if (Why.empty())
    return false;
  err(Pos, diag::err_unit_scale, {Why});
  return true;
}

void Checker::unitRangeScan() {
  if (!Ctx.unitsSeen() || S.Opts.GoCompat)
    return;
  // Every instance of a generic type that this package's code reaches, by
  // any route (written, through an alias, a function's result, a method's
  // receiver, inside another instance, from another package), is in the
  // type of an expression of the package: each is decided once, from its
  // generic type and its arguments alone, and reported at its first place
  // in the source (deterministic, whichever thread made the instance).
  struct Hit {
    uint32_t At;
    SourceLoc Pos;
    std::string Why;
  };
  llvm::DenseMap<const NamedType *, size_t> Bad;
  std::vector<Hit> Hits;
  llvm::DenseSet<const Type *> Clean; // walked: reaches no bad instance
  llvm::DenseMap<const Type *, const NamedType *> RootBad;
  // Does T reach an instance out of range? (Iterative; each type once.)
  auto Reach = [&](Type *Root) -> const NamedType * {
    SmallVector<Type *, 16> Work{Root};
    llvm::SmallPtrSet<const Type *, 16> Seen;
    while (!Work.empty()) {
      Type *X = Work.pop_back_val();
      if (!X || Clean.count(X) || !Seen.insert(X).second)
        continue;
      switch (X->getKind()) {
      case Type::Kind::Named: {
        auto *N = cast<NamedType>(X);
        if (!N->Orig || onInstCycle(N))
          break;
        for (Type *A : N->TArgs)
          Work.push_back(A);
        // An instance with no real unit argument, or whose arguments keep
        // the bounds of its declaration in range, needs nothing more (no
        // instance is expanded: R18).
        if (unitRangeSafe(N->Orig->origin(), N->TArgs))
          break;
        // Else exactly: the instance and the instances its fields and the
        // signatures of its methods hold (R20), followed as (declaration,
        // units) pairs, each once, without making instances (R18).
        auto [BadInst, Why] = unitRangeExact(N->Orig->origin(), N->TArgs);
        if (!BadInst)
          break;
        if (auto It = Bad.find(BadInst); It != Bad.end())
          return BadInst;
        Bad[BadInst] = Hits.size();
        Hits.push_back({~0u, SourceLoc(), Why});
        return BadInst;
      }
      case Type::Kind::Pointer:
        Work.push_back(cast<PointerType>(X)->Elem);
        break;
      case Type::Kind::Slice:
        Work.push_back(cast<SliceType>(X)->Elem);
        break;
      case Type::Kind::Ref:
        Work.push_back(cast<RefType>(X)->Elem);
        break;
      case Type::Kind::Option:
        Work.push_back(cast<OptionType>(X)->Elem);
        break;
      case Type::Kind::Array:
        Work.push_back(cast<sema::ArrayType>(X)->Elem);
        break;
      case Type::Kind::Chan:
        Work.push_back(cast<sema::ChanType>(X)->Elem);
        break;
      case Type::Kind::Map:
        Work.push_back(cast<sema::MapType>(X)->Key);
        Work.push_back(cast<sema::MapType>(X)->Elem);
        break;
      case Type::Kind::Tuple:
        for (Type *T : cast<TupleType>(X)->Elems)
          Work.push_back(T);
        break;
      case Type::Kind::Signature:
        Work.push_back(cast<SignatureType>(X)->Params);
        Work.push_back(cast<SignatureType>(X)->Results);
        break;
      case Type::Kind::Struct:
        for (Var *F : cast<StructType>(X)->Fields)
          Work.push_back(F->Ty);
        break;
      default:
        break;
      }
    }
    for (const Type *X : Seen)
      Clean.insert(X);
    return nullptr;
  };
  for (auto &[Ex, TV] : I.Types) {
    if (!TV.Ty || !Ex->getBegin().isValid() || Clean.count(TV.Ty))
      continue;
    auto RB = RootBad.find(TV.Ty);
    const NamedType *N = RB != RootBad.end() ? RB->second : Reach(TV.Ty);
    if (!N)
      continue;
    RootBad[TV.Ty] = N;
    Hit &H = Hits[Bad[N]];
    if (Ex->getBegin().getRaw() < H.At) {
      H.At = Ex->getBegin().getRaw();
      H.Pos = Ex->getBegin();
    }
  }
  std::sort(Hits.begin(), Hits.end(),
            [](const Hit &A, const Hit &B) { return A.At < B.At; });
  for (const Hit &H : Hits)
    if (H.Pos.isValid())
      err(H.Pos, diag::err_unit_scale, {H.Why});
}

namespace {
constexpr int64_t RangeCap = int64_t(1) << 40; // past any range: saturated
int64_t capAdd(int64_t A, int64_t B) { return std::min(A + B, RangeCap); }
int64_t capMul(int64_t A, int64_t B) {
  return A && B > RangeCap / A ? RangeCap : std::min(A * B, RangeCap);
}
/// The bits of a factor's numerator or denominator, 0 for the factor 1, so
/// bits(a * b) <= bits(a) + bits(b) and a factor of 1 adds nothing.
int64_t factorBits(const Real &F) {
  if (!F.isRat())
    return RangeCap;
  if (units::isOne(F))
    return 0;
  return std::max<int64_t>(F.ratNum().getActiveBits(),
                           F.ratDen().getActiveBits());
}
/// Visits every part of the type Root once (iterative): the elements, the
/// fields, the parameters and results of a signature, the arguments of an
/// instance.
void forEachTypePart(Type *Root, llvm::function_ref<void(Type *)> Visit) {
  SmallVector<Type *, 16> Work{Root};
  llvm::SmallPtrSet<const Type *, 16> Seen;
  while (!Work.empty()) {
    Type *X = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    Visit(X);
    switch (X->getKind()) {
    case Type::Kind::Named:
      if (cast<NamedType>(X)->Orig)
        for (Type *T : cast<NamedType>(X)->TArgs)
          Work.push_back(T);
      break;
    case Type::Kind::Pointer:
      Work.push_back(cast<PointerType>(X)->Elem);
      break;
    case Type::Kind::Slice:
      Work.push_back(cast<SliceType>(X)->Elem);
      break;
    case Type::Kind::Ref:
      Work.push_back(cast<RefType>(X)->Elem);
      break;
    case Type::Kind::Option:
      Work.push_back(cast<OptionType>(X)->Elem);
      break;
    case Type::Kind::Array:
      Work.push_back(cast<sema::ArrayType>(X)->Elem);
      break;
    case Type::Kind::Chan:
      Work.push_back(cast<sema::ChanType>(X)->Elem);
      break;
    case Type::Kind::Map:
      Work.push_back(cast<sema::MapType>(X)->Key);
      Work.push_back(cast<sema::MapType>(X)->Elem);
      break;
    case Type::Kind::Tuple:
      for (Type *T : cast<TupleType>(X)->Elems)
        Work.push_back(T);
      break;
    case Type::Kind::Signature:
      Work.push_back(cast<SignatureType>(X)->Params);
      Work.push_back(cast<SignatureType>(X)->Results);
      break;
    case Type::Kind::Struct:
      for (Var *F : cast<StructType>(X)->Fields)
        Work.push_back(F->Ty);
      break;
    default:
      break;
    }
  }
}

int indexIn(ArrayRef<TypeParamType *> TPs, const TypeParamType *P) {
  for (size_t K = 0; K < TPs.size(); ++K)
    if (TPs[K] == P || TPs[K]->RenamedFrom == P)
      return static_cast<int>(K);
  return -1;
}
} // namespace

const UnitRangeSum *Checker::unitRangeSum(NamedType *O) {
  if (const UnitRangeSum *S0 = O->URange.load(std::memory_order_acquire))
    return S0;
  struct Acc {
    bool Saturated = false;
    SmallVector<int64_t, 4> MaxExp;
    int64_t MaxBaseExp = 0, MaxFactorBits = 0;
  };
  // A generic type held by a declaration (in its fields or methods): the
  // instance as written there, over the parameters Ps of the declaration (or
  // of a method's receiver copies).
  struct Edge {
    NamedType *To;
    NamedType *Inst;
    ArrayRef<TypeParamType *> Ps;
  };
  struct Vertex {
    NamedType *D;
    unsigned Index = 0, Low = 0;
    bool OnStack = true, Complete = true;
    Acc Own;
    SmallVector<Edge, 4> Edges;
    size_t Next = 0;
  };
  // The declarations as a graph, walked by Tarjan's algorithm (iterative, as
  // the cycle checks of Mono.cpp and Decl.cpp): each strongly connected set
  // is summarized once, after the sets it reaches; inside one, a fixpoint
  // over its members that never re-opens a declaration, and that saturates
  // (no bound) if the exponents still grow around the cycle.
  std::vector<Vertex> Vs;
  llvm::DenseMap<NamedType *, unsigned> Idx;
  llvm::DenseMap<NamedType *, Acc> Done;
  std::vector<unsigned> Stack, Dfs;
  const bool Store = typesFinal();
  auto Open = [&](NamedType *D) {
    unsigned Id = static_cast<unsigned>(Vs.size());
    Idx[D] = Id;
    Vertex V;
    V.D = D;
    V.Index = V.Low = Id;
    V.Own.MaxExp.assign(D->TParams.size(), 0);
    // Its own units, over D's parameters or a method's receiver copies.
    auto Units = [&](Type *Root, ArrayRef<TypeParamType *> Ps) {
      forEachTypePart(Root, [&](Type *X) {
        auto *B = dyn_cast<BasicType>(X);
        if (!B || !B->Unit || !B->Unit->HasParam)
          return;
        int64_t Base = 0;
        for (const UnitDim &Dm : B->Unit->Dims) {
          int64_t E = Dm.Exp < 0 ? -int64_t(Dm.Exp) : Dm.Exp;
          if (!Dm.Param)
            Base = std::max(Base, E);
          else if (int K = indexIn(Ps, Dm.Param); K >= 0)
            V.Own.MaxExp[K] = std::max(V.Own.MaxExp[K], E);
          // (a generic method's own parameter: decided where it is given)
        }
        V.Own.MaxBaseExp = std::max(V.Own.MaxBaseExp, Base);
        V.Own.MaxFactorBits =
            std::max(V.Own.MaxFactorBits, factorBits(B->Unit->Factor));
      });
    };
    Units(D->Underlying, D->TParams);
    V.Complete = D->Underlying != nullptr;
    if (D->Methods)
      for (Func *M : *D->Methods) {
        if (!M || !M->Ty) {
          V.Complete = false; // not resolved yet: the answer is not stored
          continue;
        }
        Units(M->Ty, M->RecvTParams);
      }
    // The generic types it holds (the same list the exact walk follows).
    SmallVector<UnitHeld, 4> Held;
    unitHeldInstances(D, Held);
    for (const UnitHeld &H : Held)
      V.Edges.push_back({H.To, H.Inst, H.Ps});
    Vs.push_back(std::move(V));
    Stack.push_back(Id);
    Dfs.push_back(Id);
  };
  // The summary of a declaration that is finished (stored, or made by this
  // walk), or null.
  auto Finished = [&](NamedType *D, Acc &Tmp) -> const Acc * {
    if (auto It = Done.find(D); It != Done.end())
      return &It->second;
    if (const UnitRangeSum *S1 = D->URange.load(std::memory_order_acquire)) {
      Tmp.Saturated = S1->Saturated;
      Tmp.MaxExp.assign(S1->MaxExp.begin(), S1->MaxExp.end());
      Tmp.MaxBaseExp = S1->MaxBaseExp;
      Tmp.MaxFactorBits = S1->MaxFactorBits;
      return &Tmp;
    }
    return nullptr;
  };
  // A held instance E whose declaration has the bounds S2, into A: S2's
  // parameters replaced by E's arguments (units over E.Ps). True if A grew.
  auto Apply = [&](Acc &A, const Edge &E, const Acc &S2) {
    if (A.Saturated)
      return false;
    if (S2.Saturated) {
      A.Saturated = true;
      return true;
    }
    bool Grew = false;
    SmallVector<int64_t, 4> Contrib(A.MaxExp.size(), 0);
    int64_t Base = S2.MaxBaseExp, Bits = S2.MaxFactorBits;
    for (size_t K = 0; K < E.Inst->TArgs.size() && K < S2.MaxExp.size(); ++K) {
      int64_t E2 = S2.MaxExp[K];
      const UnitCanon *G = E2 ? unitArgCanon(E.Inst->TArgs[K]) : nullptr;
      if (!G)
        continue;
      int64_t GB = 0;
      for (const UnitDim &Dm : G->Dims) {
        int64_t X = Dm.Exp < 0 ? -int64_t(Dm.Exp) : Dm.Exp;
        if (!Dm.Param)
          GB = std::max(GB, X);
        else if (int J = indexIn(E.Ps, Dm.Param); J >= 0)
          Contrib[J] = capAdd(Contrib[J], capMul(E2, X));
      }
      Base = capAdd(Base, capMul(E2, GB));
      Bits = capAdd(Bits, capMul(E2, factorBits(G->Factor)));
    }
    auto Max = [&](int64_t &Into, int64_t V) {
      if (V > Into) {
        Into = V;
        Grew = true;
      }
    };
    for (size_t J = 0; J < Contrib.size(); ++J)
      Max(A.MaxExp[J], Contrib[J]);
    Max(A.MaxBaseExp, Base);
    Max(A.MaxFactorBits, Bits);
    return Grew;
  };
  auto Publish = [&](NamedType *D, const Acc &A) {
    std::lock_guard<std::mutex> L(Ctx.allocLock());
    auto *Sum = Ctx.arena().make<UnitRangeSum>();
    Sum->Saturated = A.Saturated;
    Sum->MaxExp = Ctx.arena().copy(ArrayRef<int64_t>(A.MaxExp));
    Sum->MaxBaseExp = A.MaxBaseExp;
    Sum->MaxFactorBits = A.MaxFactorBits;
    const UnitRangeSum *Old = nullptr;
    D->URange.compare_exchange_strong(Old, Sum, std::memory_order_acq_rel);
  };
  Open(O);
  while (!Dfs.empty()) {
    unsigned VI = Dfs.back();
    if (Vs[VI].Next < Vs[VI].Edges.size()) {
      NamedType *To = Vs[VI].Edges[Vs[VI].Next++].To;
      Acc Tmp;
      if (Finished(To, Tmp))
        continue;
      auto It = Idx.find(To);
      if (It == Idx.end()) {
        Open(To);
        continue;
      }
      if (Vs[It->second].OnStack)
        Vs[VI].Low = std::min(Vs[VI].Low, Vs[It->second].Index);
      continue;
    }
    Dfs.pop_back();
    if (!Dfs.empty())
      Vs[Dfs.back()].Low = std::min(Vs[Dfs.back()].Low, Vs[VI].Low);
    if (Vs[VI].Low != Vs[VI].Index)
      continue;
    // VI heads a strongly connected set: the rest of Stack.
    auto SIt = std::find(Stack.begin(), Stack.end(), VI);
    std::vector<unsigned> Members(SIt, Stack.end());
    Stack.erase(SIt, Stack.end());
    llvm::SmallPtrSet<NamedType *, 8> In;
    for (unsigned M : Members) {
      Vs[M].OnStack = false;
      In.insert(Vs[M].D);
    }
    // Each member: its own units and the finished sets it holds.
    size_t Width = 1;
    for (unsigned M : Members) {
      Acc A = Vs[M].Own;
      for (const Edge &E : Vs[M].Edges)
        if (!In.count(E.To)) {
          Acc Tmp;
          if (const Acc *S2 = Finished(E.To, Tmp))
            Apply(A, E, *S2);
        }
      Width = std::max(Width, A.MaxExp.size());
      Done[Vs[M].D] = std::move(A);
    }
    // The edges inside the set, to a fixpoint by a worklist: a member is
    // looked at again only when a member it holds grew. The work is linear in
    // the set: past a budget proportional to its edges and parameters the
    // bounds still grow around a cycle, and then there is no bound (exact
    // instances decide, never a false error).
    llvm::DenseMap<NamedType *, SmallVector<std::pair<unsigned, unsigned>, 2>>
        Preds; // held declaration -> (member, its edge)
    for (unsigned M : Members)
      for (unsigned K = 0; K < Vs[M].Edges.size(); ++K)
        if (In.count(Vs[M].Edges[K].To))
          Preds[Vs[M].Edges[K].To].push_back({M, K});
    std::vector<unsigned> Queue(Members.begin(), Members.end());
    llvm::DenseSet<unsigned> Queued(Members.begin(), Members.end());
    size_t InEdges = 0;
    for (auto &P : Preds)
      InEdges += P.second.size();
    const size_t Budget = 8 * (InEdges + Members.size()) * (Width + 1);
    size_t Updates = 0;
    bool Grows = false;
    while (!Queue.empty() && !Grows) {
      unsigned X = Queue.back();
      Queue.pop_back();
      Queued.erase(X);
      auto PIt = Preds.find(Vs[X].D);
      if (PIt == Preds.end())
        continue;
      Acc S2 = Done[Vs[X].D]; // (a copy: a member may hold itself)
      for (auto [M, K] : PIt->second) {
        if (!Apply(Done[Vs[M].D], Vs[M].Edges[K], S2))
          continue;
        if (++Updates > Budget) {
          Grows = true;
          break;
        }
        if (Queued.insert(M).second)
          Queue.push_back(M);
      }
    }
    for (unsigned M : Members) {
      Acc &A = Done[Vs[M].D];
      A.Saturated |= Grows;
      if (Store && Vs[M].Complete)
        Publish(Vs[M].D, A);
    }
  }
  if (const UnitRangeSum *S2 = O->URange.load(std::memory_order_acquire))
    return S2;
  // Not final yet (types still being declared): a summary for this question
  // only, not stored.
  const Acc &A = Done[O];
  auto *Sum = Mem.make<UnitRangeSum>();
  Sum->Saturated = A.Saturated;
  Sum->MaxExp = Mem.copy(ArrayRef<int64_t>(A.MaxExp));
  Sum->MaxBaseExp = A.MaxBaseExp;
  Sum->MaxFactorBits = A.MaxFactorBits;
  return Sum;
}

bool Checker::unitRangeSafe(NamedType *O, ArrayRef<Type *> TArgs) {
  // Only real units can leave the range (a parameter of generic code is
  // decided where that code is instantiated).
  bool AnyReal = false;
  for (Type *A : TArgs)
    if (const UnitCanon *U = unitArgCanon(A); U && !U->HasParam)
      AnyReal = true;
  if (!AnyReal)
    return true;
  const UnitRangeSum *S = unitRangeSum(O);
  if (S->Saturated)
    return false;
  // Every unit of the instance: |exponent of a base unit| <= the written one
  // plus each parameter's exponent times its argument's; factor bits likewise.
  SmallVector<std::pair<const Object *, int64_t>, 8> Base;
  int64_t Bits = S->MaxFactorBits;
  for (size_t K = 0; K < TArgs.size() && K < S->MaxExp.size(); ++K) {
    int64_t E = S->MaxExp[K];
    const UnitCanon *U = E ? unitArgCanon(TArgs[K]) : nullptr;
    if (!U)
      continue;
    if (U->HasParam)
      return false; // mixed: decided exactly
    Bits = capAdd(Bits, capMul(E, factorBits(U->Factor)));
    for (const UnitDim &Dm : U->Dims) {
      int64_t X = capMul(E, Dm.Exp < 0 ? -int64_t(Dm.Exp) : Dm.Exp);
      auto It =
          llvm::find_if(Base, [&](auto &P) { return P.first == Dm.Unit; });
      if (It == Base.end())
        Base.push_back({Dm.Unit, X});
      else
        It->second = capAdd(It->second, X);
    }
  }
  if (Bits > Real::MaxRatBits)
    return false;
  for (auto &P : Base)
    if (capAdd(P.second, S->MaxBaseExp) > units::MaxExp)
      return false;
  return true;
}

void Checker::unitHeldInstances(NamedType *D,
                                llvm::SmallVectorImpl<UnitHeld> &Out) {
  auto Walk = [&](Type *Root, ArrayRef<TypeParamType *> Ps) {
    forEachTypePart(Root, [&](Type *X) {
      auto *N = dyn_cast<NamedType>(X);
      if (!N || !N->Orig)
        return;
      NamedType *O2 = N->Orig->origin();
      if (O2->TParams.size() == N->TArgs.size() &&
          llvm::any_of(O2->TParams,
                       [&](TypeParamType *Q) { return isUnitParam(Q); }))
        Out.push_back({O2, N, Ps});
    });
  };
  Walk(D->Underlying, D->TParams);
  if (D->Methods)
    for (Func *M : *D->Methods)
      if (M && M->Ty)
        Walk(M->Ty, M->RecvTParams);
}

std::pair<NamedType *, std::string>
Checker::unitRangeExact(NamedType *O, ArrayRef<Type *> TArgs) {
  struct Item {
    NamedType *O;
    SmallVector<Type *, 4> Args;
  };
  std::vector<Item> Work{
      {O, SmallVector<Type *, 4>(TArgs.begin(), TArgs.end())}};
  std::set<std::vector<const void *>> Seen;
  llvm::DenseMap<NamedType *, SmallVector<UnitHeld, 4>> Held;
  bool First = true;
  while (!Work.empty()) {
    Item It = std::move(Work.back());
    Work.pop_back();
    std::vector<const void *> Key{It.O};
    Key.insert(Key.end(), It.Args.begin(), It.Args.end());
    if (!Seen.insert(std::move(Key)).second)
      continue;
    if (!First && unitRangeSafe(It.O, It.Args))
      continue;
    First = false;
    std::string Name(It.O->Obj ? It.O->Obj->Name : StringRef("it"));
    std::string Why =
        unitRangeWhy(It.O->Underlying, It.O->TParams, It.Args, Name);
    if (Why.empty() && It.O->Methods)
      for (Func *M : *It.O->Methods)
        if (M && M->Ty && M->RecvTParams.size() == It.Args.size() &&
            !(Why = unitRangeWhy(M->Ty, M->RecvTParams, It.Args, Name)).empty())
          break;
    if (!Why.empty())
      return {instantiateNamed(It.O, It.Args), Why};
    auto HIt = Held.find(It.O);
    if (HIt == Held.end()) {
      SmallVector<UnitHeld, 4> L;
      unitHeldInstances(It.O, L);
      HIt = Held.try_emplace(It.O, std::move(L)).first;
    }
    for (const UnitHeld &H : HIt->second) {
      if (H.To->InstCycle)
        continue; // an instantiation cycle (E0366): its instances never end
      // The held instance's arguments with the real units of It.
      auto Rep = [&](const TypeParamType *Q) -> const UnitCanon * {
        int K = indexIn(H.Ps, Q);
        return K >= 0 && static_cast<size_t>(K) < It.Args.size()
                   ? unitArgCanon(It.Args[K])
                   : nullptr;
      };
      Item Next{H.To, {}};
      bool Ok = true;
      for (Type *A : H.Inst->TArgs) {
        const UnitCanon *G = unitArgCanon(A);
        if (!G || !G->HasParam) {
          Next.Args.push_back(A);
          continue;
        }
        UnitStatus St;
        const UnitCanon *C = units::substitute(Ctx, G, Rep, St);
        if (!C || C->HasParam) {
          Ok = false; // out of range (reported above), or not a real unit
          break;
        }
        Next.Args.push_back(Ctx.unitArgType(C));
      }
      if (Ok)
        Work.push_back(std::move(Next));
    }
  }
  return {nullptr, {}};
}

void Checker::unitDictUses(SourceLoc Pos, Type *TArg, TypeParamType *TPar) {
  const UnitCanon *A = unitArgCanon(TArg);
  if (!A || !A->HasParam || !own() || S.Opts.GoCompat)
    return;
  // As satisfies does for the other uses: a generic of this package waits
  // for all its bodies (untaggedCheck), one of another package is complete.
  TypeParamType *Root = TPar->RenamedFrom ? TPar->RenamedFrom : TPar;
  if (Root->Obj && Root->Obj->Pkg == S.Pkg) {
    UntaggedPend.push_back({Pos, TArg, Root, false});
    return;
  }
  bool Saved = NotingPassed;
  NotingPassed = true;
  for (const TypeParamType::Use &U : Root->uses())
    if (U.K == TypeParamType::Use::UnitDict)
      (void)useWhy(TArg, Root, U);
  NotingPassed = Saved;
}

//===----------------------------------------------------------------------===//
// Unit declarations (UNI-1 to UNI-3)
//===----------------------------------------------------------------------===//

/// The units of the same package that the unit expression names and that are
/// not resolved yet, in source order. No errors: unitDecl reports them.
static void pendingDeps(Checker &C, const ast::Expr *Root, Scope *Sc,
                        llvm::SmallVectorImpl<UnitObj *> &Out) {
  SmallVector<const ast::Expr *, 8> Work{Root};
  while (!Work.empty()) {
    const ast::Expr *X = Work.pop_back_val();
    if (!X)
      continue;
    switch (X->getKind()) {
    case ast::Node::Kind::Ident: {
      Object *O = Sc->lookup(cast<ast::Ident>(X)->Name.str());
      if (auto *U = dyn_cast_or_null<UnitObj>(O))
        if (!U->Resolved && U->Pkg == C.S.Pkg &&
            std::find(Out.begin(), Out.end(), U) == Out.end())
          Out.push_back(U);
      break;
    }
    case ast::Node::Kind::ParenExpr:
      Work.push_back(cast<ParenExpr>(X)->X);
      break;
    case ast::Node::Kind::UnitExpr: {
      auto *U = cast<ast::UnitExpr>(X);
      for (size_t I = U->Terms.size(); I-- > 0;)
        Work.push_back(U->Terms[I].Base);
      break;
    }
    default:
      break;
    }
  }
}

void Checker::resolveUnit(UnitObj *Root) {
  if (Root->Resolved)
    return;
  auto It = S.ObjMap.find(Root);
  if (It == S.ObjMap.end()) {
    Root->Resolved = true; // an imported unit is resolved when it is loaded
    return;
  }
  struct Frame {
    UnitObj *U;
    DeclInfo *D;
    SmallVector<UnitObj *, 4> Deps;
    size_t Next = 0;
  };
  SmallVector<Frame, 16> Stack;
  auto Push = [&](UnitObj *U) {
    DeclInfo *D = S.ObjMap[U];
    Frame F;
    F.U = U;
    F.D = D;
    if (D->UDecl && D->UDecl->Unit)
      pendingDeps(*this, D->UDecl->Unit, D->FileScope, F.Deps);
    U->Color = 2; // on the walk
    UnitPath.push_back(U);
    Stack.push_back(std::move(F));
  };
  if (Root->Color >= 2) {
    // Asked again while it is being resolved (through a constant in its
    // factor): the unit depends on itself.
    reportUnitCycle(Root);
    return;
  }
  Push(Root);
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    if (F.Next < F.Deps.size()) {
      UnitObj *D = F.Deps[F.Next++];
      if (D->Resolved)
        continue;
      if (D->Color >= 2) {
        reportUnitCycle(D);
        continue;
      }
      if (!S.ObjMap.count(D))
        continue;
      Push(D);
      continue;
    }
    UnitObj *U = F.U;
    DeclInfo *DI = F.D;
    Env Saved = E;
    E = Env();
    E.Sc = DI->FileScope;
    E.Cur = DI;
    unitDecl(U, DI);
    E = Saved;
    U->Resolved = true;
    S.UNamer.addUnit(Ctx, U);
    U->Color = 1;
    UnitPath.pop_back();
    Stack.pop_back();
  }
}

/// A unit that is on the walk was reached again: the units from it to the top
/// of the walk are a cycle. All of them are bad (they report nothing more).
void Checker::reportUnitCycle(UnitObj *At) {
  size_t From = 0;
  for (size_t I = 0; I < UnitPath.size(); ++I)
    if (UnitPath[I] == At)
      From = I;
  std::string Text;
  SmallVector<UnitObj *, 8> Members(UnitPath.begin() + From, UnitPath.end());
  // Report at the unit that comes first in the source.
  size_t First = 0;
  for (size_t I = 1; I < Members.size(); ++I)
    if (Members[I]->Pos.getRaw() < Members[First]->Pos.getRaw())
      First = I;
  for (size_t I = 0; I <= Members.size(); ++I) {
    if (I)
      Text += " -> ";
    Text += std::string(Members[(First + I) % Members.size()]->Name);
  }
  for (UnitObj *M : Members)
    M->Bad = true;
  err(Members[First]->Pos, diag::err_unit_cycle, {Text});
}

bool Checker::unitFactorValue(UnitObj *U, const ast::Expr *Factor, Real &Out) {
  Operand X;
  bool SavedExact = E.ExactQuo;
  E.ExactQuo = true;
  expr(X, Factor);
  E.ExactQuo = SavedExact;
  if (X.M == Mode::Invalid)
    return false;
  auto Bad = [&](const std::string &Why) {
    err(Factor, diag::err_unit_factor, {std::string(U->Name), Why});
    return false;
  };
  if (X.M != Mode::Constant)
    return Bad(str(Factor) + " is not a constant");
  if (isTyped(X.Ty))
    return Bad(str(Factor) + " is a typed constant (" + str(X.Ty) +
               "); the factor must be an untyped numeric constant");
  if (!isUntypedNumeric(X.Ty) ||
      (isa<BasicLit>(Factor) &&
       cast<BasicLit>(Factor)->LitKind == tok::char_lit))
    return Bad(str(Factor) + " is not a number");
  if (unitOf(X.Ty))
    return Bad(str(Factor) + " has a unit; the factor is a plain number");
  if (isComplex(X.Ty))
    return Bad(str(Factor) + " is a complex constant");
  ConstValue V = X.Val.toFloat();
  if (V.kind() != ConstValue::Float)
    return Bad(str(Factor) + " is not a real number");
  if (V.sign() <= 0)
    return Bad(str(Factor) + " must be greater than zero");
  if (!V.floatVal().isRat()) {
    err(Factor, diag::err_unit_scale,
        {"the factor of " + std::string(U->Name) +
         " is not an exact rational (it needs more than " +
         std::to_string(Real::MaxRatBits) + " bits)"});
    return false;
  }
  Out = V.floatVal();
  return true;
}

void Checker::unitDecl(UnitObj *U, DeclInfo *D) {
  const ast::UnitSpec *Sp = D->UDecl;
  if (U->Bad) // on a cycle: the cycle was reported
    return;
  if (!Sp->Eq.isValid()) {
    U->IsBase = true;
    U->Canon = units::base(Ctx, U, nullptr);
    return;
  }
  Real F = Real::fromInt(llvm::APInt(64, 1, true));
  bool Ok = true;
  if (Sp->Factor)
    Ok = unitFactorValue(U, Sp->Factor, F);
  const UnitCanon *UC = Sp->Unit ? unitExpr(Sp->Unit) : nullptr;
  if (!Sp->Unit) {
    err(Sp->Name, diag::err_unit_dimension,
        {std::string(U->Name),
         "a factor alone has no dimension; declare a base unit (`unit RAD`) "
         "or write a unit after the factor (`unit KM = 1000 M`)"});
    Ok = false;
  }
  if (!UC || !Ok) {
    U->Bad = true;
    return;
  }
  if (UC->dimensionless()) {
    err(Sp->Unit, diag::err_unit_dimension,
        {std::string(U->Name),
         str(Sp->Unit) +
             " has an empty dimension; a dimensionless quantity such as an "
             "angle is made from a base unit (`unit RAD`)"});
    U->Bad = true;
    return;
  }
  UnitStatus St;
  const UnitCanon *R = units::scaled(Ctx, UC, F, St);
  if (!R) {
    unitStatusError(Sp->Name, St, "unit " + std::string(U->Name));
    U->Bad = true;
    return;
  }
  U->Canon = R;
}

/// The display forms of an imported package (its table, from its check or
/// its export data) are forms for this package too (phase 1, UNI-5): an
/// importer prints the names the package prints. The fixed order makes the
/// order of the imports irrelevant.
void Checker::addImportedUnits(Package *P) {
  if (!P || P->Fake || P->IsForeign || !P->PkgScope)
    return;
  for (auto &KV : P->PkgScope->Elems)
    if (auto *U = dyn_cast<UnitObj>(KV.second); U && U->exported())
      S.UNamer.addUnit(Ctx, U);
  for (const UnitNameEntry &En : P->UnitNames)
    S.UNamer.offer(En.Canon, En.Disp);
}

/// `@Symbol("km")` (UNI-13): one, with a text that has no space and none of
/// `* / ^ ( )`. Reads the attribute of the declaration into U.
void Checker::readUnitSymbol(const GenDecl *GD, UnitObj *U) {
  const Attribute *First = nullptr;
  for (const Attribute *A : GD->Attrs) {
    if (A->Pkg || !A->Name || name(A->Name) != "Symbol")
      continue;
    if (First) {
      err(A->AtLoc, diag::err_unit_symbol,
          {"a unit has one @Symbol (the first is at line " +
           std::to_string(S.SM.getLineNumber(First->AtLoc)) + ")"});
      continue;
    }
    First = A;
    if (GD->Lparen.isValid()) {
      err(A->AtLoc, diag::err_unit_symbol,
          {"@Symbol is not defined on a group of units in v1; write one "
           "`unit` declaration for the unit"});
      continue;
    }
    if (!A->HasArgs || A->Args.size() != 1 ||
        A->Args[0].Kind != tok::string_lit) {
      err(A->AtLoc, diag::err_unit_symbol,
          {"@Symbol takes one string: @Symbol(\"km\")"});
      continue;
    }
    const Token &T = A->Args[0];
    FileID FI = S.SM.getFileID(T.Loc);
    StringRef Buf = S.SM.getBuffer(FI);
    StringRef Raw = Buf.substr(S.SM.getFileOffset(T.Loc), T.Length);
    std::string Text;
    if (!unquoteString(Raw, Text)) {
      err(A->AtLoc, diag::err_unit_symbol, {"malformed string"});
      continue;
    }
    if (Text.empty()) {
      err(A->AtLoc, diag::err_unit_symbol, {"the text is empty"});
      continue;
    }
    bool Bad = false;
    for (unsigned char C : Text)
      if (C <= ' ' || C == 0x7f || C == '*' || C == '/' || C == '^' ||
          C == '(' || C == ')') {
        err(A->AtLoc, diag::err_unit_symbol,
            {"the text \"" + Text +
             "\" has a space, a control character or one of * / ^ ( ), which "
             "would make the printed unit ambiguous"});
        Bad = true;
        break;
      }
    if (!Bad)
      U->Symbol = Mem.copy(StringRef(Text));
  }
}

//===----------------------------------------------------------------------===//
// Unit types (UNI-6, UNI-7)
//===----------------------------------------------------------------------===//

/// `T[U]`: T names a predeclared numeric type (the type or an alias of it),
/// so the brackets hold one unit (UNI-7). Null if T is anything else (the
/// normal instantiation reads the brackets); the invalid type after an error.
Type *Checker::unitTypeIndex(const IndexExpr *Ix) {
  if (S.Opts.GoCompat)
    return nullptr;
  auto *Id = dyn_cast<ast::Ident>(Ix->X);
  if (!Id)
    return nullptr;
  Object *Obj = E.Sc->lookup(name(Id), E.Pos);
  auto *TN = dyn_cast_or_null<TypeName>(Obj);
  if (!TN)
    return nullptr;
  if (S.ObjMap.count(TN) || TN->Color >= 2)
    objDecl(TN, nullptr);
  // A generic alias (`type A[T any] = float64`, `type R[U unit] =
  // float64[U]`) is instantiated as any generic is: its brackets hold its
  // arguments, not a unit (R17).
  if (TN->IsAlias && !TN->AliasTParams.empty())
    return nullptr;
  auto *BT = dyn_cast_or_null<BasicType>(TN->Ty);
  if (auto *N = dyn_cast_or_null<NamedType>(TN->Ty);
      N && N->TParams.empty() && Ix->Indices.size() == 1 && !TN->IsAlias) {
    // `Distance[KM]` for a type of your own: only a numeric type name takes
    // a unit in brackets (UNI-7). If the brackets hold a unit, say what to do.
    const ast::Expr *A = Ix->Indices[0];
    bool IsUnit = isa<ast::UnitExpr>(A);
    if (auto *AI = dyn_cast<ast::Ident>(A))
      IsUnit = llvm::isa_and_nonnull<UnitObj>(E.Sc->lookup(name(AI), E.Pos));
    if (IsUnit) {
      err(Ix, diag::err_unit_type,
          {str(Ix), std::string(TN->Name) +
                        " is a type you declared: only a predeclared number "
                        "type takes a unit in brackets; declare a type over a "
                        "unit type (`type Distance float64[" +
                        str(A) +
                        "]`) or make the type generic over a unit "
                        "(`type " +
                        std::string(TN->Name) + "[U unit] ...`)"});
      return invalid();
    }
  }
  if (!BT || BT->BK == BasicKind::Invalid || BT->BK == BasicKind::UnitArg)
    return nullptr;
  Operand X;
  ident(X, Id, nullptr, true); // records the use of the type name
  auto Bad = [&](const std::string &Why) -> Type * {
    err(Ix, diag::err_unit_type, {str(Ix), Why});
    return invalid();
  };
  if (BT->Unit)
    return Bad("the type " + str(BT) + " has a unit already");
  if (!(BT->Info & IsNumeric) || (BT->Info & IsUntyped))
    return Bad("units apply to numeric types only, and " + str(BT) +
               " is not one");
  if (BT->BK == BasicKind::Uintptr)
    return Bad("uintptr cannot have a unit");
  if (Ix->Indices.size() != 1) {
    for (const ast::Expr *A : Ix->Indices)
      (void)A;
    return Bad("expected one unit in the brackets, found " +
               num(Ix->Indices.size()));
  }
  const UnitDisplay *D = nullptr;
  const UnitCanon *C = unitExpr(Ix->Indices[0], &D);
  if (!C)
    return invalid();
  if (C->dimensionless())
    return Bad("the unit " + str(Ix->Indices[0]) +
               " has an empty dimension, so it is not a unit");
  if (D)
    I.UnitDisplays[Ix] = D; // the form as written (a conversion reads it)
  return Ctx.unitType(BT->BK, C);
}

//===----------------------------------------------------------------------===//
// Constants (EXP-35)
//===----------------------------------------------------------------------===//

/// The untyped constant zero without a unit fits a type with any unit, so
/// `d == 0` and `var d float64[KM] = 0` stay legal. Nothing else plain does.
bool Checker::unitZeroFits(const Operand &X, const BasicType *Target) {
  auto *XB = dyn_cast_or_null<BasicType>(X.Ty);
  if (!XB || XB->Unit || !(XB->Info & IsUntyped) || X.M != Mode::Constant)
    return false;
  if (!(XB->Info & IsNumeric) || !(Target->Info & IsNumeric))
    return false;
  return X.Val.isNumeric() && X.Val.sign() == 0;
}

//===----------------------------------------------------------------------===//
// Unit literals (EXP-31)
//===----------------------------------------------------------------------===//

void Checker::unitLit(Operand &X, const ast::UnitLit *Lit) {
  X.M = Mode::Invalid;
  X.E = Lit;
  tok::Kind K = Lit->Lit->LitKind;
  if (K != tok::int_lit && K != tok::float_lit && K != tok::imag_lit) {
    err(Lit, diag::err_unit_expr,
        {"a rune or string literal cannot take a unit: " + str(Lit)});
    return;
  }
  if (Lit->Lit->Text.size() > 10000) {
    err(Lit, diag::err_sema_expr, {"excessively long constant"});
    return;
  }
  basicLit(X, Lit->Lit);
  if (X.M == Mode::Invalid) {
    err(Lit, diag::err_sema_expr,
        {"malformed constant: " + Lit->Lit->Text.str()});
    return;
  }
  X.E = Lit;
  const UnitDisplay *D = nullptr;
  const UnitCanon *U = unitExpr(Lit->Unit, &D);
  if (!U) {
    X.M = Mode::Invalid;
    return;
  }
  if (U->dimensionless()) {
    err(Lit->Unit, diag::err_unit_expr,
        {str(Lit->Unit) + " has an empty dimension, so it is not a unit"});
    X.M = Mode::Invalid;
    return;
  }
  X.Ty = Ctx.unitType(cast<BasicType>(X.Ty)->BK, U);
  if (D)
    I.UnitDisplays[Lit] = D;
  overflow(X, Lit->getBegin());
}

//===----------------------------------------------------------------------===//
// Scaling a value to another unit (EXP-32, EXP-34, EXP-35, EXP-37)
//===----------------------------------------------------------------------===//

static std::string ratioStr(const Real &R) { return units::realStr(R); }

LLVM_ATTRIBUTE_NOINLINE bool Checker::unitFactorFits(const ast::Node *At,
                                                     const Real &R, Type *Ty) {
  auto *B = dyn_cast_or_null<BasicType>(under(Ty));
  if (!B || !isTyped(Ty) || !(B->Info & IsNumeric) || !R.isRat())
    return true;
  bool Ok;
  if (B->Info & IsInteger) {
    llvm::APInt N = R.ratNum(), D = R.ratDen();
    Ok = representableConst(ConstValue::makeInt(N), B, nullptr) &&
         representableConst(ConstValue::makeInt(D), B, nullptr);
  } else {
    BasicKind FK = B->BK == BasicKind::Float32 || B->BK == BasicKind::Complex64
                       ? BasicKind::Float32
                       : BasicKind::Float64;
    ConstValue Rounded;
    Ok = representableConst(ConstValue::makeFloat(R), cast<BasicType>(typ(FK)),
                            &Rounded) &&
         Rounded.kind() == ConstValue::Float && !Rounded.floatVal().isZero();
  }
  if (!Ok)
    err(At, diag::err_unit_scale,
        {"the factor " + ratioStr(R) + " of the conversion is not a " +
         str(Ctx.unitType(B->BK, nullptr)) + " number"});
  return Ok;
}

void Checker::recordUnitConv(const ast::Expr *Ex, const Real &Factor,
                             bool Truncate, bool Tag) {
  if (!Ex || units::isOne(Factor))
    return;
  I.UnitConvs[Ex] = UnitConv{Factor, Truncate, Tag};
}

/// Converts O, a unit value, to the unit To of the same dimension, with the
/// type NewTy (which has unit To). The one place that decides a scale change:
/// the implicit conversions of EXP-34 and the explicit ones of EXP-32
/// (`T[U](x)` and `x.In[U]()` come here through the same path). A constant is
/// scaled exactly (EXP-37); an explicit conversion of an integer value to an
/// integer kind truncates toward zero (`int[KM](int[M](1999))` is 1); an
/// implicit one is exact or an error. A value of an integer kind needs an
/// integer ratio unless Explicit. The lowering finds the scale change of a
/// value under Key (O.E if null; Tag: see UnitConv). False after an error.
bool Checker::unitScale(Operand &O, const UnitCanon *To, Type *NewTy,
                        bool Explicit, const ast::Node *At,
                        const ast::Expr *Key, bool Tag) {
  const UnitCanon *From = unitOf(O.Ty);
  if (From == To) {
    O.Ty = NewTy;
    return true;
  }
  Real R = units::ratio(From, To);
  bool IntKind = isInteger(NewTy);
  if (O.M == Mode::Constant && !O.Val.isUnknown()) {
    ConstValue V = O.Val;
    auto Bad = [&]() {
      err(At, diag::err_unit_scale,
          {"the constant " + str(O.E) + " cannot be scaled by " + ratioStr(R) +
           " exactly"});
      O.M = Mode::Invalid;
      return false;
    };
    if (V.kind() == ConstValue::Complex) {
      Real Re = Real::mul(V.floatVal(), R), Im = Real::mul(V.imagVal(), R);
      if (!Re.isRat() || !Im.isRat())
        return Bad();
      V = ConstValue::makeComplex(Re, Im);
    } else if (V.kind() == ConstValue::Int || V.kind() == ConstValue::Float) {
      Real X = V.kind() == ConstValue::Int ? Real::fromInt(V.intVal())
                                           : V.floatVal();
      Real P = Real::mul(X, R);
      if (!P.isRat())
        return Bad();
      llvm::APInt I;
      if (P.toInt(I)) {
        V = V.kind() == ConstValue::Int || IntKind ? ConstValue::makeInt(I)
                                                   : ConstValue::makeFloat(P);
      } else if (IntKind && Explicit && V.kind() == ConstValue::Int) {
        // EXP-32: an explicit conversion of an integer truncates toward
        // zero, like Go's integer division.
        llvm::APInt N = P.ratNum(), D = P.ratDen();
        unsigned W = std::max(N.getBitWidth(), D.getBitWidth());
        V = ConstValue::makeInt(N.sext(W).sdiv(D.sext(W)));
      } else if (IntKind && isTyped(NewTy) && !Explicit) {
        err(At, diag::err_unit_inexact,
            {str(O.E), ratioStr(R),
             "convert to float first, or use a unit that divides evenly"});
        O.M = Mode::Invalid;
        return false;
      } else if (IntKind && !isTyped(NewTy)) {
        // An untyped integer constant that is no longer whole becomes an
        // untyped float constant with the unit (constants are exact).
        V = ConstValue::makeFloat(P);
        NewTy = Ctx.unitType(BasicKind::UntypedFloat, To);
      } else {
        V = ConstValue::makeFloat(P); // (a typed integer kind: not
                                      // representable, reported below)
      }
    }
    O.Val = V;
    O.Ty = NewTy;
    recordUnitSite(O.E, UnitSite::Const, To, nullptr); // GEN-14: folded
    if (isTyped(NewTy)) {
      auto *B = cast<BasicType>(under(NewTy));
      representable(O, B);
      if (O.M == Mode::Invalid)
        return false;
    }
    return true;
  }
  if (IntKind && !Explicit && !R.isInt()) {
    err(At, diag::err_unit_inexact,
        {str(O.E), ratioStr(R),
         "convert first (`" + str(NewTy) + "(x)` truncates) or use floats"});
    O.M = Mode::Invalid;
    return false;
  }
  if (!unitFactorFits(At, R, NewTy)) {
    O.M = Mode::Invalid;
    return false;
  }
  recordUnitConv(Key ? Key : O.E, R, IntKind && !R.isInt(), Tag);
  recordUnitSite(Key ? Key : O.E, UnitSite::Conv, To, From, nullptr,
                 Tag); // GEN-14
  O.Ty = NewTy;
  O.M = Mode::Value;
  return true;
}

//===----------------------------------------------------------------------===//
// Display forms of expressions (UNI-5)
//===----------------------------------------------------------------------===//

/// The display form of the unit U of the expression Ex as a message prints
/// it: the form Ex makes (a literal, a conversion, `.In`, an operator, a
/// variable initialized from one), when it names U, and the form of the
/// package table, whichever comes first in the fixed order; null if neither
/// exists (the canonical text is printed then).
const UnitDisplay *Checker::unitDisplayOf(const ast::Expr *Ex,
                                          const UnitCanon *U) {
  if (!U)
    return nullptr;
  const UnitDisplay *Own = Ex && Ex == CompoundE ? CompoundD : nullptr;
  for (const ast::Expr *X = Ex; X && !Own;) {
    if (auto It = I.UnitDisplays.find(X); It != I.UnitDisplays.end()) {
      Own = It->second;
      break;
    }
    if (auto *P = dyn_cast<ParenExpr>(X)) {
      X = P->X;
    } else if (auto *Un = dyn_cast<UnaryExpr>(X);
               Un && (Un->Op == tok::sub || Un->Op == tok::add)) {
      X = Un->X;
    } else if (auto *Id = dyn_cast<ast::Ident>(X)) {
      auto Use = I.Uses.find(Id);
      auto *V = Use == I.Uses.end() ? nullptr : dyn_cast<Var>(Use->second);
      if (V) {
        auto &Map = V->IsPackageLevel ? S.UnitVarDisplays : UnitVarDisplays;
        if (auto It = Map.find(V); It != Map.end())
          Own = It->second;
      }
      break;
    } else {
      break;
    }
  }
  if (Own && units::displayCanon(Ctx, Own) != U)
    Own = nullptr; // the expression was converted to another unit
  const UnitDisplay *Table = units::nameOf(U, S.Pkg, &unitNamer());
  if (!Own)
    return Table;
  if (!Table)
    return Own;
  return UnitDisplay::before(Table, Own) ? Table : Own;
}

/// The text of the unit of the operand O, by its display form.
std::string Checker::unitStrOf(const Operand &O) {
  const UnitCanon *U = unitOf(O.Ty);
  if (!U)
    return "";
  if (const UnitDisplay *D = unitDisplayOf(O.E, U))
    return units::displayStr(D, S.Pkg);
  return units::canonStr(U, S.Pkg);
}

/// The type of the operand O as a message prints it: an unnamed unit type
/// with the display form of O's expression (`float64[KM/H]` for `d / t`).
std::string Checker::unitTypeStrOf(const Operand &O) {
  auto *B = dyn_cast_or_null<BasicType>(O.Ty);
  if (!B || !B->Unit || B->BK == BasicKind::UnitArg)
    return str(O.Ty);
  return std::string(B->Name) + "[" + unitStrOf(O) + "]";
}

/// Records the form that the expression Ex makes (Info::UnitDisplays); in
/// phase 1 it is a form of the declarations as well (UNI-5).
void Checker::noteUnitDisplay(const ast::Expr *Ex, const UnitCanon *U,
                              const UnitDisplay *D) {
  if (!Ex || !U || !D || U->dimensionless())
    return;
  I.UnitDisplays[Ex] = D;
  unitNamer().offer(U, D);
}

/// A variable initialized from a unit expression keeps its form (`speed :=
/// d / t` prints as `KM/H`).
void Checker::noteUnitVar(Var *V, const Operand &X) {
  const UnitCanon *U = unitOf(V->Ty);
  if (!U || !X.E || isa<NamedType>(V->Ty))
    return;
  if (const UnitDisplay *D = unitDisplayOf(X.E, U)) {
    if (V->IsPackageLevel) {
      if (!Bodies)
        S.UnitVarDisplays[V] = D;
    } else {
      UnitVarDisplays[V] = D;
    }
  }
}

//===----------------------------------------------------------------------===//
// + - % and comparisons (EXP-34, EXP-35, EXP-36)
//===----------------------------------------------------------------------===//

static const char *unitVerb(tok::Kind Op, const char *&Prep) {
  Prep = "to";
  switch (Op) {
  case tok::add:
    return "add";
  case tok::sub:
    Prep = "from";
    return "subtract";
  case tok::rem:
    Prep = "and";
    return "take the remainder of";
  case tok::cmp3:
    Prep = "with";
    return "compare";
  default:
    Prep = "with";
    return "compare";
  }
}

/// The type of kind BK of T with the unit U: T's untyped/typed kind kept.
Type *Checker::unitRetype(Type *T, const UnitCanon *U) {
  return Ctx.unitType(baseKind(T), U);
}

/// Makes X and Y agree on one unit for + - % <=> and comparisons. False
/// after an error (X is invalid). With Compound (x op= y), Y is converted to
/// the unit of X and X is never changed. CaseKey: X is the tag of a switch
/// and Y the case value CaseKey (a scale change of the tag is kept under it).
bool Checker::unitMatch(Operand &X, Operand &Y, tok::Kind Op, bool Compound,
                        const ast::Expr *Ex, const ast::Expr *CaseKey) {
  const UnitCanon *UX = unitOf(X.Ty), *UY = unitOf(Y.Ty);
  auto Fail = [&]() {
    X.M = Mode::Invalid;
    return false;
  };
  const char *Prep;
  const char *Verb = unitVerb(Op, Prep);
  std::string What =
      Ex ? str(Ex) : str(X.E) + " " + tok::getSpelling(Op) + " " + str(Y.E);
  if (!UX || !UY) {
    // One side has no unit. The untyped constant zero takes the unit of the
    // other side (EXP-35); any other number is an error.
    Operand &Plain = UX ? Y : X;
    Operand &Unit = UX ? X : Y;
    const UnitCanon *U = UX ? UX : UY;
    auto *PB = dyn_cast_or_null<BasicType>(under(Plain.Ty));
    if (!PB || !(PB->Info & IsNumeric))
      return true; // another kind of operand: the number rules report it
    auto *UB =
        dyn_cast<BasicType>(isa<BasicType>(Unit.Ty) ? Unit.Ty : under(Unit.Ty));
    if (UB && unitZeroFits(Plain, UB)) {
      Plain.Ty = Ctx.unitType(cast<BasicType>(Plain.Ty)->BK, U);
      return true;
    }
    std::string US = unitStrOf(Unit);
    std::string Fix =
        Plain.M == Mode::Constant && isUntyped(Plain.Ty)
            ? "the number with its unit, as in " +
                  (Plain.E && isa<BasicLit>(Plain.E) ? str(Plain.E)
                                                     : std::string("1.0")) +
                  "[" + US + "] (only an untyped zero needs no unit)"
            : (Unit.Ty == Plain.Ty ? str(Plain.Ty)
                                   : str(unitRetype(Plain.Ty, U))) +
                  "(" + str(Plain.E) + ") to give the value a unit";
    err(Plain, diag::err_unit_plain, {What, Verb, Prep, US, Fix});
    return Fail();
  }
  if (UX == UY)
    return true;
  if (!units::sameDim(UX, UY)) {
    err(X, diag::err_unit_mismatch, {What, Verb, unitStrOf(X), unitStrOf(Y)});
    return Fail();
  }
  // Same dimension, another scale. A named type does not convert.
  bool NX = isa<NamedType>(X.Ty), NY = isa<NamedType>(Y.Ty);
  if (NX || NY) {
    if (isValid(X.Ty) && isValid(Y.Ty))
      err(X, diag::err_mismatched_types, {What, str(X.Ty), str(Y.Ty)});
    return Fail();
  }
  // Which side is converted: with Compound the right one, else the coarser.
  // The finer unit has the smaller factor.
  bool ConvY = Compound || Real::cmp(UY->Factor, UX->Factor) > 0;
  Operand &C = ConvY ? Y : X;
  const UnitCanon *To = ConvY ? UX : UY;
  bool Ok = unitScale(C, To, unitRetype(C.Ty, To), /*Explicit=*/false,
                      ConvY ? Y.E : X.E, !ConvY ? CaseKey : nullptr,
                      /*Tag=*/!ConvY && CaseKey);
  if (!Ok)
    return Fail();
  return true;
}

/// `min(a, b, ...)` and `max(...)` with units (EXP-34, UNI-11): every
/// argument goes to the finest unit among them (decided first, so each
/// argument is converted once, from its own unit), plain numbers other than
/// the untyped zero are errors. False after an error.
bool Checker::unitMinMax(llvm::SmallVectorImpl<Operand> &Args,
                         const CallExpr *Call) {
  size_t First = Args.size();
  for (size_t K = 0; K < Args.size(); ++K)
    if (unitOf(Args[K].Ty)) {
      First = K;
      break;
    }
  if (First == Args.size())
    return true;
  const UnitCanon *U0 = unitOf(Args[First].Ty), *Fine = U0;
  size_t FineAt = First;
  for (size_t K = 0; K < Args.size(); ++K)
    if (const UnitCanon *U = unitOf(Args[K].Ty);
        U && units::sameDim(U, U0) && Real::cmp(U->Factor, Fine->Factor) < 0) {
      Fine = U;
      FineAt = K;
    }
  // The errors as for `a < b`, against the first argument with a unit.
  for (size_t K = 0; K < Args.size(); ++K) {
    if (K == First)
      continue;
    Operand X = Args[First], Y = Args[K];
    const UnitCanon *UY = unitOf(Y.Ty);
    if (!UY || !units::sameDim(UY, U0) || isa<NamedType>(X.Ty) ||
        isa<NamedType>(Y.Ty)) {
      Operand &L = K < First ? Y : X, &R = K < First ? X : Y;
      if (!unitMatch(L, R, tok::lss, false, nullptr))
        return false;
      Args[K].Ty = K < First ? L.Ty : R.Ty; // the zero takes the unit
    }
  }
  const UnitDisplay *D = unitDisplayOf(Args[FineAt].E, Fine);
  for (size_t K = 0; K < Args.size(); ++K) {
    Operand &A = Args[K];
    const UnitCanon *U = unitOf(A.Ty);
    if (!U || U == Fine || isa<NamedType>(A.Ty))
      continue;
    if (!unitScale(A, Fine, unitRetype(A.Ty, Fine), /*Explicit=*/false, A.E))
      return false;
  }
  if (D)
    noteUnitDisplay(Call, Fine, D);
  return true;
}

//===----------------------------------------------------------------------===//
// The binary operators on unit values
//===----------------------------------------------------------------------===//

/// True if the operator was decided here (X holds the result, or is invalid).
bool Checker::unitBinary(Operand &X, Operand &Y, const ast::Expr *Ex,
                         const ast::Expr *L, const ast::Expr *R, tok::Kind Op,
                         SourceLoc OpLoc) {
  const UnitCanon *UX = unitOf(X.Ty), *UY = unitOf(Y.Ty);
  if (!UX && !UY)
    return false;
  std::string What =
      Ex ? str(Ex) : str(L) + " " + tok::getSpelling(Op) + "= " + str(R);
  switch (Op) {
  case tok::shl:
  case tok::shr:
    if (UY) {
      err(Y, diag::err_unit_op,
          {What, "a shift count has no unit; remove the unit first, as in "
                 "uint(n)"});
      X.M = Mode::Invalid;
      return true;
    }
    return false; // the unit of the left operand is kept (EXP-36)
  case tok::and_:
  case tok::or_:
  case tok::xor_:
  case tok::and_not:
    err(X, diag::err_unit_op,
        {What, std::string("the operator ") + tok::getSpelling(Op) +
                   " works on bit patterns, which have no unit; remove the "
                   "unit first, as in uint32(x)"});
    X.M = Mode::Invalid;
    return true;
  case tok::mul:
  case tok::quo:
    return unitMulDiv(X, Y, Ex, L, R, Op, OpLoc);
  case tok::add:
  case tok::sub:
  case tok::rem:
  case tok::eql:
  case tok::neq:
  case tok::lss:
  case tok::leq:
  case tok::gtr:
  case tok::geq:
  case tok::cmp3:
    if (!unitMatch(X, Y, Op, /*Compound=*/!Ex, Ex))
      return true;
    // The result of `+ - %` has the unit of the operands now, and the form
    // of the one that kept its unit.
    if (Ex && (Op == tok::add || Op == tok::sub || Op == tok::rem))
      if (const UnitCanon *U = unitOf(X.Ty)) {
        const UnitDisplay *D = unitDisplayOf(X.E, U);
        if (!D || D == units::nameOf(U, S.Pkg, &unitNamer()))
          if (const UnitDisplay *DY = unitDisplayOf(Y.E, U))
            D = DY;
        noteUnitDisplay(Ex, U, D);
      }
    return false; // the units agree now: the number rules do the rest
  default:
    return false;
  }
}

LLVM_ATTRIBUTE_NOINLINE Type *Checker::unitPartType(Type *From, Type *Res) {
  if (!Ctx.unitsSeen())
    return Res;
  const UnitCanon *U = unitOf(From);
  auto *RB = dyn_cast_or_null<BasicType>(Res);
  return U && RB ? Ctx.unitType(RB->BK, U) : Res;
}

LLVM_ATTRIBUTE_NOINLINE void Checker::unitTermCheck(const ast::Expr *Term,
                                                    Type *T) {
  if (!T)
    return;
  later([this, Term, T]() {
    if (!unitOf(T))
      return;
    err(Term, diag::err_unit_generic,
        {"the type set cannot name the unit type " + str(T) +
         ": a body checked once would take it for a plain number; write a "
         "function generic over a unit instead (`[U unit]` with "
         "`float64[U]`)"});
  });
}

/// EXP-33: the ratio of two integer values of one dimension is the plain
/// physical ratio: the coarser operand is scaled to the finer unit (the rule
/// of `+`, so `5[KM] / 2[M]` is 5000 / 2) and the numbers divide. A constant
/// and a variable agree.
LLVM_ATTRIBUTE_NOINLINE bool Checker::unitIntQuo(Operand &X, Operand &Y,
                                                 const ast::Expr *Ex) {
  return unitMatch(X, Y, tok::quo, /*Compound=*/!Ex, Ex);
}

/// `*` and `/` (EXP-33): the number rules run on the operands without their
/// units, then the unit of the result is put back.
bool Checker::unitMulDiv(Operand &X, Operand &Y, const ast::Expr *Ex,
                         const ast::Expr *L, const ast::Expr *R, tok::Kind Op,
                         SourceLoc OpLoc) {
  const UnitCanon *UX = unitOf(X.Ty), *UY = unitOf(Y.Ty);
  if (Op == tok::quo && UX && UY && UX != UY && units::sameDim(UX, UY) &&
      isInteger(X.Ty) && isInteger(Y.Ty)) {
    if (!unitIntQuo(X, Y, Ex))
      return true;
    UX = unitOf(X.Ty);
    UY = unitOf(Y.Ty);
  }
  Type *OrigX = X.Ty, *OrigY = Y.Ty;
  // The forms of the operands, before the number rules change the operands.
  const UnitDisplay *DX = UX ? unitDisplayOf(X.E, UX) : nullptr;
  const UnitDisplay *DY = UY ? unitDisplayOf(Y.E, UY) : nullptr;
  if (UX && !DX)
    DX = units::baseDisplay(Ctx, UX);
  if (UY && !DY)
    DY = units::baseDisplay(Ctx, UY);
  auto Strip = [&](Operand &O, const UnitCanon *U) {
    if (U)
      O.Ty = Ctx.unitType(baseKind(O.Ty), nullptr);
  };
  Strip(X, UX);
  Strip(Y, UY);
  // A plain numeric operand and a unit type that is an untyped constant:
  // the number rules see plain numbers.
  binaryRest(X, Y, Ex, L, R, Op, OpLoc);
  if (X.M == Mode::Invalid)
    return true;
  auto Desc = [&]() {
    return Ex ? str(Ex) : str(L) + " " + tok::getSpelling(Op) + "= " + str(R);
  };
  UnitStatus St = UnitStatus::Ok;
  const UnitCanon *Res;
  const UnitDisplay *ResD = nullptr;
  if (UX && UY) {
    Res = Op == tok::mul ? units::mul(Ctx, UX, UY, St)
                         : units::div(Ctx, UX, UY, St);
    ResD = units::displayMul(Ctx, DX, DY, Op == tok::mul ? 1 : -1);
  } else if (UX) {
    Res = UX;
    ResD = DX;
  } else {
    Res = Op == tok::mul ? UY : units::div(Ctx, units::one(Ctx), UY, St);
    if (DY && Op == tok::mul) {
      ResD = DY;
    } else if (DY) {
      SmallVector<UnitDim, 4> Inv(DY->Parts.begin(), DY->Parts.end());
      for (UnitDim &P : Inv)
        P.Exp = -P.Exp;
      ResD = Ctx.unitDisplay(Inv); // `1.0 / t` is `1/H`
    }
  }
  if (!Res) {
    unitStatusError(X.E ? static_cast<const ast::Node *>(X.E) : L, St,
                    "in " + (Ex ? str(Ex) : std::string("the expression")));
    X.M = Mode::Invalid;
    return true;
  }
  // GEN-14 (a result that is plain whatever the instance has its factor in
  // UnitConvs, EXP-33: the site sees a plain result).
  recordUnitSite(Ex, Op == tok::mul ? UnitSite::Mul : UnitSite::Quo,
                 Res->dimensionless() ? nullptr : Res, UX, UY);
  if (Res->dimensionless()) {
    // The ratio of two lengths is a plain number: scaled by what is left.
    if (!units::isOne(Res->Factor)) {
      Real F = Res->Factor;
      if (X.M == Mode::Constant && !X.Val.isUnknown()) {
        ConstValue V = X.Val;
        Real Num = V.kind() == ConstValue::Int ? Real::fromInt(V.intVal())
                                               : V.floatVal();
        if (V.kind() == ConstValue::Complex) {
          V = ConstValue::makeComplex(Real::mul(V.floatVal(), F),
                                      Real::mul(V.imagVal(), F));
        } else {
          Real P = Real::mul(Num, F);
          llvm::APInt I;
          if (V.kind() == ConstValue::Int && P.toInt(I))
            V = ConstValue::makeInt(I);
          else if (isInteger(X.Ty) && isTyped(X.Ty)) {
            err(X, diag::err_unit_inexact,
                {Desc(), ratioStr(F),
                 "the remaining factor must be an integer for an integer "
                 "kind; use floats"});
            X.M = Mode::Invalid;
            return true;
          } else
            V = ConstValue::makeFloat(P);
        }
        X.Val = V;
        if (isTyped(X.Ty)) {
          representable(X, cast<BasicType>(under(X.Ty)));
        } else if (V.kind() == ConstValue::Float && isInteger(X.Ty)) {
          X.Ty = typ(BasicKind::UntypedFloat);
        }
      } else if (isInteger(X.Ty) && !F.isInt()) {
        err(X, diag::err_unit_inexact,
            {Desc(), ratioStr(F),
             "the remaining factor must be an integer for an integer kind; "
             "use floats"});
        X.M = Mode::Invalid;
        return true;
      } else if (Ex) {
        if (!unitFactorFits(Ex, F, X.Ty)) {
          X.M = Mode::Invalid;
          return true;
        }
        recordUnitConv(Ex, F, false, false); // the value times F (EXP-33)
      }
    }
    return true;
  }
  // Put the unit back. A scalar keeps the (possibly named) type of the
  // operand that has the unit.
  Type *Plain = X.Ty;
  if (UX && !UY && Res == UX &&
      identical(Plain, Ctx.unitType(baseKind(OrigX), nullptr)) &&
      isTyped(OrigX))
    X.Ty = OrigX;
  else if (!UX && UY && Op == tok::mul && Res == UY &&
           identical(Plain, Ctx.unitType(baseKind(OrigY), nullptr)) &&
           isTyped(OrigY))
    X.Ty = OrigY;
  else
    X.Ty = unitRetype(Plain, Res);
  if (X.M != Mode::Constant)
    X.M = Mode::Value;
  if (Ex && !isa<NamedType>(X.Ty)) {
    noteUnitDisplay(Ex, Res, ResD);
  } else if (!Ex && ResD && !isa<NamedType>(X.Ty)) {
    // `x *= y`: the result prints in the form of the product (UNI-5).
    CompoundE = L;
    CompoundD = ResD;
    unitNamer().offer(Res, ResD);
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Conversions (EXP-32)
//===----------------------------------------------------------------------===//

/// `T(x)` where T or the type of x has a unit: give a unit, change it, or
/// remove it. True if decided here (X holds the result or is invalid).
/// The number part is converted by the ordinary rules on the types without
/// their units, so every numeric rule of Go holds in the same place; the
/// scale change is unitScale's, as for `x.In[U]()`.
bool Checker::unitConversion(Operand &X, Type *T) {
  const UnitCanon *TU = unitOf(T), *XU = unitOf(X.Ty);
  if (!TU && !XU)
    return false;
  auto *TB = dyn_cast_or_null<BasicType>(under(T));
  auto *XB = dyn_cast_or_null<BasicType>(under(X.Ty));
  if (!TB || !XB || !(TB->Info & IsNumeric) || !(XB->Info & IsNumeric)) {
    // A unit value to a string, an interface and so on: the ordinary rules
    // (and their errors) apply, they see the type with its unit.
    if (TU && XB && !(XB->Info & IsNumeric)) {
      err(X, diag::err_unit_conv,
          {str(X), str(T), "only a number can be given a unit"});
      X.M = Mode::Invalid;
      return true;
    }
    return false;
  }
  Type *PlainT = Ctx.unitType(TB->BK, nullptr);
  if (!TU) {
    // Remove the unit: the number in its own unit.
    X.Ty = Ctx.unitType(XB->BK, nullptr);
    conversion(X, isa<NamedType>(T) ? T : PlainT);
    return true;
  }
  if (!XU) {
    // Give a unit.
    conversion(X, PlainT);
    if (X.M == Mode::Invalid)
      return true;
    if (X.M != Mode::Constant)
      recordUnitSite(X.E, UnitSite::Conv, TU, nullptr); // GEN-14
    X.Ty = T;
    return true;
  }
  // Both have a unit: the same dimension, and the value is scaled.
  if (!units::sameDim(XU, TU)) {
    err(X, diag::err_unit_conv,
        {str(X), str(T),
         unitStrOf(X) + " and " + unitStr(TU) + " have different dimensions"});
    X.M = Mode::Invalid;
    return true;
  }
  if (XU != TU) {
    Type *Mid = Ctx.unitType(XB->BK, TU);
    if (!unitScale(X, TU, Mid, /*Explicit=*/true, X.E))
      return true;
  }
  X.Ty = Ctx.unitType(baseKind(X.Ty), nullptr);
  conversion(X, PlainT);
  if (X.M == Mode::Invalid)
    return true;
  X.Ty = T;
  return true;
}

/// After `T(x)` that gives a value a unit: the form of the written T is the
/// form of the result (`float64[KM/H](v)`).
void Checker::unitNoteConversion(Operand &X, const CallExpr *Call) {
  const UnitCanon *U = unitOf(X.Ty);
  if (!U || isa<NamedType>(X.Ty))
    return;
  const ast::Expr *F = Call->Fun;
  while (auto *P = dyn_cast<ParenExpr>(F))
    F = P->X;
  if (auto It = I.UnitDisplays.find(F); It != I.UnitDisplays.end())
    noteUnitDisplay(Call, U, It->second);
}

//===----------------------------------------------------------------------===//
// x.In[U]() (EXP-32)
//===----------------------------------------------------------------------===//

/// `x.In` where x has no member In: the built-in In of a unit value (the
/// receiver is kept for the call), or the error for a number without a unit.
/// False if this is no `.In` of a number (the member lookup reports it).
bool Checker::unitInSelector(Operand &X, const SelectorExpr *Ex) {
  if (unitOf(X.Ty)) {
    UnitRecv[Ex] = X;
    X.M = Mode::Builtin;
    X.BId = BuiltinId::UnitIn;
    X.E = Ex;
    return true;
  }
  if (X.Ty && isNumeric(X.Ty)) {
    // A literal takes the unit after it; any other value, as a conversion.
    std::string Fix = isa<BasicLit>(Ex->X)
                          ? str(Ex->X) + "[KM]"
                          : str(isUntyped(X.Ty) ? defaultType(X.Ty) : X.Ty) +
                                "[KM](" + str(Ex->X) + ")";
    err(Ex->Sel, diag::err_unit_in,
        {str(Ex->X) +
         " has no unit, so it has no In: In is a member of unit "
         "values only; give the value a unit first, as in " +
         Fix});
    X.M = Mode::Invalid;
    X.E = Ex;
    return true;
  }
  return false;
}

/// `x.In[U]`: reads U. X is the Builtin operand of `x.In`; it stays a
/// Builtin (only a call completes it) or becomes invalid.
void Checker::unitInIndex(Operand &X, const IndexExpr *Ix) {
  if (!isa<SelectorExpr>(Ix->X) || Ix->Indices.size() != 1) {
    err(Ix, diag::err_unit_in,
        {"`In` takes one unit: x.In[KM](), found " + str(Ix)});
    X.M = Mode::Invalid;
    return;
  }
  const UnitDisplay *D = nullptr;
  const UnitCanon *U = unitExpr(Ix->Indices[0], &D);
  if (!U) {
    X.M = Mode::Invalid;
    return;
  }
  if (U->dimensionless()) {
    err(Ix->Indices[0], diag::err_unit_in,
        {str(Ix->Indices[0]) + " has an empty dimension, so it is not a unit"});
    X.M = Mode::Invalid;
    return;
  }
  UnitInArg[Ix] = {U, D};
  X.M = Mode::Builtin;
  X.BId = BuiltinId::UnitIn;
  X.E = Ix;
}

void Checker::unitInCall(Operand &X, const CallExpr *Call) {
  const ast::Expr *F = Call->Fun;
  while (auto *P = dyn_cast<ParenExpr>(F))
    F = P->X;
  auto Fail = [&]() {
    for (const ast::Expr *A : Call->Args)
      use(A);
    X.M = Mode::Invalid;
  };
  auto *Ix = dyn_cast<IndexExpr>(F);
  auto *Sel = Ix ? dyn_cast<SelectorExpr>(Ix->X) : dyn_cast<SelectorExpr>(F);
  if (!Ix || !Sel) {
    err(Call, diag::err_unit_in,
        {"`" + str(F) + "` needs a unit: write " + str(F) +
         "[KM]() with the unit in brackets"});
    return Fail();
  }
  auto RI = UnitRecv.find(Sel);
  auto UI = UnitInArg.find(Ix);
  if (RI == UnitRecv.end() || UI == UnitInArg.end()) {
    X.M = Mode::Invalid; // an error was reported where it was found
    return;
  }
  if (!Call->Args.empty()) {
    err(Call->Args[0], diag::err_unit_in,
        {"`In[U]()` takes no arguments, found " + str(Call->Args[0])});
    return Fail();
  }
  if (Call->isMust()) {
    err(Call, diag::err_unit_in, {"`In[U]()` cannot fail: remove the `!`"});
    return Fail();
  }
  Operand R = RI->second;
  const UnitCanon *To = UI->second.first;
  const UnitCanon *From = unitOf(R.Ty);
  if (!units::sameDim(From, To)) {
    err(Call, diag::err_unit_in,
        {str(Call) + ": " + unitStrOf(R) + " and " + unitStr(To) +
         " have different dimensions"});
    return Fail();
  }
  // The same as the conversion T[U](x) for the base kind T of x (EXP-32):
  // one scale change (unitScale, explicit); an untyped constant keeps its
  // kind and stays a constant (EXP-37).
  Type *Target = Ctx.unitType(baseKind(R.Ty), To);
  if (isUntyped(R.Ty)) {
    if (!unitScale(R, To, Target, /*Explicit=*/true, Call, R.E))
      return Fail();
  } else {
    conversion(R, Target);
    if (R.M == Mode::Invalid)
      return Fail();
  }
  X = R;
  if (X.M != Mode::Constant)
    X.M = Mode::Value;
  X.E = Call;
  noteUnitDisplay(Call, To, UI->second.second);
}

//===----------------------------------------------------------------------===//
// Out of line on purpose (M4): the callers recurse once per nesting level of
// an expression, a statement or a type (exprInternal, unary, callExpr,
// selector, indexExpr, stmt, typInternal, typeList, interfaceTypeExpr,
// substImpl), and the strings of these messages must not enlarge their
// frames (an ASan frame holds every temporary of the function).
//===----------------------------------------------------------------------===//

LLVM_ATTRIBUTE_NOINLINE Type *Checker::unitParamAsType(const ast::Expr *Ex) {
  err(Ex, diag::err_unit_generic,
      {str(Ex) +
       " is a unit parameter, not a type: use it inside the brackets of a "
       "number type, as in `float64[" +
       str(Ex) + "]`"});
  return invalid();
}

LLVM_ATTRIBUTE_NOINLINE Type *Checker::unitAsType(const ast::Expr *Ex) {
  err(Ex, diag::err_unit_expr,
      {Ex->getKind() == ast::Node::Kind::UnitLit
           ? str(Ex) + " is a value, not a type"
           : "a unit expression is not a type: " + str(Ex) +
                 " can stand in the brackets of a number type, as in "
                 "`float64[" +
                 str(Ex) + "]`"});
  return invalid();
}

LLVM_ATTRIBUTE_NOINLINE void Checker::unitNotExpr(const ast::Expr *Ex) {
  err(Ex, diag::err_unit_is_not_expr, {str(Ex)});
}

LLVM_ATTRIBUTE_NOINLINE void Checker::unitExprAsValue(const ast::Expr *Ex) {
  // The parser reads `^` as a power in the brackets after `.In` (UNI-7);
  // here it was in a place that is no unit.
  err(Ex, diag::err_unit_expr,
      {str(Ex) + " is a unit expression, not a value; after `.In` a `^` in "
                 "the brackets is a power (UNI-7): if `In` is a field or a "
                 "method of your own, compute the index first, as in "
                 "j := i^2; x.In[j]"});
}

LLVM_ATTRIBUTE_NOINLINE bool Checker::unitBitNot(Operand &X,
                                                 const ast::UnaryExpr *Ex) {
  if (!unitOf(X.Ty))
    return false;
  err(X, diag::err_unit_op,
      {str(Ex), "bit patterns have no unit; remove the unit first, as in "
                "uint32(x)"});
  X.M = Mode::Invalid;
  return true;
}

LLVM_ATTRIBUTE_NOINLINE void Checker::unitParamCall(const CallExpr *Call) {
  err(Call->Fun, diag::err_unit_generic,
      {str(Call->Fun) +
       " is a unit parameter, not a type: it cannot be called as a "
       "conversion; write float64[" +
       str(Call->Fun) + "](x)"});
}

LLVM_ATTRIBUTE_NOINLINE bool Checker::unitIncDec(const ast::IncDecStmt *ID,
                                                 const Operand &X) {
  const UnitCanon *U = unitOf(X.Ty);
  if (!U)
    return false;
  const char *Op = ID->Op == tok::inc ? "++" : "--";
  err(ID->X, diag::err_unit_op,
      {str(ID->X) + Op,
       std::string("a step of 1 of what? write ") + str(ID->X) +
           (ID->Op == tok::inc ? " += " : " -= ") + "1" +
           (isInteger(X.Ty) ? "" : ".0") + "[" + unitStrOf(X) + "]"});
  return true;
}

/// `x.In[KM]` where x had an error: the brackets hold a unit, not an index
/// (EXP-32), so they are not checked again.
LLVM_ATTRIBUTE_NOINLINE bool Checker::unitInAfterError(const IndexExpr *Ex) {
  auto *InSel = dyn_cast<SelectorExpr>(Ex->X);
  return InSel && name(InSel->Sel) == "In";
}

LLVM_ATTRIBUTE_NOINLINE void Checker::unitCombinedConstraint(SourceLoc At) {
  err(At, diag::err_unit_generic,
      {"`unit` cannot be combined with other elements of a constraint: a "
       "unit parameter has no methods, operators or type set (write "
       "`[U unit]`)"});
}

bool Checker::unitLooksLike(const ast::Expr *Ex) {
  SmallVector<const ast::Expr *, 8> Work{Ex};
  bool Name = false;
  while (!Work.empty()) {
    const ast::Expr *X = Work.pop_back_val();
    if (isa<ast::UnitExpr>(X))
      return true;
    if (auto *P = dyn_cast<ParenExpr>(X)) {
      Work.push_back(P->X);
    } else if (auto *B = dyn_cast<ast::BinaryExpr>(X)) {
      if (B->Op != tok::mul && B->Op != tok::quo && B->Op != tok::xor_)
        return false;
      Work.push_back(B->X);
      if (B->Op != tok::xor_) // (`^` 2: the exponent is no name)
        Work.push_back(B->Y);
    } else if (auto *L = dyn_cast<BasicLit>(X)) {
      if (L->LitKind != tok::int_lit)
        return false;
    } else if (auto *Id = dyn_cast<ast::Ident>(X)) {
      if (!llvm::isa_and_nonnull<UnitObj>(E.Sc->lookup(name(Id), E.Pos)))
        return false;
      Name = true;
    } else if (auto *Sel = dyn_cast<SelectorExpr>(X)) {
      auto *PId = dyn_cast<ast::Ident>(Sel->X);
      auto *PN = PId ? dyn_cast_or_null<PkgName>(E.Sc->lookup(name(PId), E.Pos))
                     : nullptr;
      Package *P = PN ? PN->Imported : nullptr;
      if (!P || !P->PkgScope ||
          !llvm::isa_and_nonnull<UnitObj>(
              P->PkgScope->lookupLocal(name(Sel->Sel))))
        return false;
      Name = true;
    } else {
      return false;
    }
  }
  return Name;
}

LLVM_ATTRIBUTE_NOINLINE int Checker::unitArgAt(const ast::Expr *Ex,
                                               ArrayRef<TypeParamType *> Want,
                                               size_t I) {
  if (I >= Want.size()) {
    // An argument past the parameters that names a unit is read as a unit,
    // so the count error is the only one.
    auto *Id = dyn_cast<ast::Ident>(Ex);
    return Id && llvm::isa_and_nonnull<UnitObj>(E.Sc->lookup(name(Id), E.Pos))
               ? 1
               : 0;
  }
  if (isUnitParam(Want[I]))
    return 1;
  if (!unitLooksLike(Ex))
    return 0;
  // A unit for a type parameter: one error, not one per name (M5).
  err(Ex, diag::err_unit_generic,
      {str(Ex) + " is a unit, not a type: the type parameter " + str(Want[I]) +
       " takes a type (only a parameter `[U unit]` takes a "
       "unit)"});
  return 2;
}

LLVM_ATTRIBUTE_NOINLINE Type *
Checker::unitSubstBasic(Type *T,
                        const llvm::DenseMap<const Type *, Type *> &M) {
  // GEN-14: a unit that mentions a unit parameter (`float64[U/S]`, or the
  // unit argument `U*S` itself) takes the unit argument in its place.
  auto *B = cast<BasicType>(T);
  if (!B->Unit || !B->Unit->HasParam)
    return T;
  UnitStatus St;
  const UnitCanon *C = units::substitute(
      Ctx, B->Unit,
      [&](const TypeParamType *P) -> const UnitCanon * {
        auto It = M.find(P);
        if (It == M.end())
          return nullptr;
        if (auto *R = dyn_cast<BasicType>(It->second);
            R && R->BK == BasicKind::UnitArg)
          return R->Unit;
        if (auto *TP = dyn_cast<TypeParamType>(It->second))
          return units::base(Ctx, nullptr, TP);
        return nullptr;
      },
      St);
  if (!C) // out of range: the instantiation reports it (unitInstanceRange)
    return invalid();
  return B->BK == BasicKind::UnitArg
             ? Ctx.unitArgType(C)
             : static_cast<Type *>(Ctx.unitType(B->BK, C));
}

/// `'a'[M]`, `"s"[M]`: a rune or string literal indexed with a unit is a
/// unit literal of the wrong kind (EXP-31): one error.
LLVM_ATTRIBUTE_NOINLINE bool Checker::unitIndexLiteral(Operand &X,
                                                       const IndexExpr *Ex) {
  const ast::Expr *L0 = Ex->X;
  while (auto *P = dyn_cast<ParenExpr>(L0))
    L0 = P->X;
  auto *L = dyn_cast<BasicLit>(L0);
  if (!L || (L->LitKind != tok::char_lit && L->LitKind != tok::string_lit) ||
      Ex->Indices.size() != 1 || !unitLooksLike(Ex->Indices[0]))
    return false;
  err(Ex, diag::err_unit_expr,
      {"a rune or string literal cannot take a unit: " + str(Ex)});
  X.M = Mode::Invalid;
  X.E = Ex;
  return true;
}

/// A local variable declared with a unit type keeps the form written there.
void Checker::noteUnitVarType(Var *V, const ast::Expr *TypeExpr) {
  if (!unitOf(V->Ty) || isa<NamedType>(V->Ty) || V->IsPackageLevel)
    return;
  while (auto *P = dyn_cast<ParenExpr>(TypeExpr))
    TypeExpr = P->X;
  if (auto It = I.UnitDisplays.find(TypeExpr); It != I.UnitDisplays.end())
    UnitVarDisplays[V] = It->second;
}
