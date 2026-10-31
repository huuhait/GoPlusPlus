//===- LowerUnits.cpp - Lowering of units of measure (ADR-0025, P04.24) ---===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Units are erased (UNI-9): a `float64[KM]` is a `float64`, the unit literal
// `100.0[KM]` is the number, and GIR types carry no unit. What is left of a
// unit at run time is the multiplication by a constant where the checker
// changed the scale of a value, and it is decided in this file only:
//
//   Info::UnitConvs  the exact factor by which the value of the keyed
//                    expression is multiplied: an operand of `+ - % <=>`, a
//                    comparison, `min`, `max`, a `case` value, the right side
//                    of `x op= y`, the operand of a conversion, the receiver
//                    of `x.In[U]()`, and a product or quotient whose
//                    dimension is empty (`d / m` is a plain number times the
//                    factor that is left).
//   Info::UnitSites  the same for unit-generic code (GEN-14): the factor that
//                    depends on whether a unit is empty at the instance. A
//                    function that takes unit descriptors (a body shared
//                    across unit arguments) makes it at run time from them,
//                    once at its entry (Body::Hoist); a function whose units
//                    are known (plain code, a method of an instance of a
//                    generic type) has it as a constant. A shared body takes
//                    no unit fact from the types of its instance.
//
// All the factors that apply to one expression are multiplied as exact
// rationals and the product is applied once: a float is multiplied by it
// rounded once to its type; an integer by an integer factor (wrapping), and
// by a factor that is no integer as a truncating `x * p / q` (an explicit
// conversion to a coarser unit, EXP-32; generic code, ADR-0025 amendment
// point 7) whose second product is exact (unit_scale_int). A constant is
// scaled in one place, Body::constOperand. Nothing here depends on the order
// in which expressions or instances are lowered.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace {
bool isRoleOwn(const S::UnitSite &S) { return S.K != S::UnitSite::Conv; }
} // namespace

bool Body::erasedUnit(const S::UnitCanon *U) const {
  return U && U->HasParam && CurInst &&
         llvm::any_of(U->Dims, [&](const S::UnitDim &D) {
           return D.Param && InstanceWalk::erasedParam(CurInst, D.Param);
         });
}

/// The unit that the canonical unit U (over the unit parameters of the generic
/// code) is at this instance; null if it cannot be made.
const S::UnitCanon *Body::unitAtInstance(const S::UnitCanon *U) {
  if (!U || !U->HasParam)
    return U;
  S::Type *Arg = inst(L.CP.Ctx->unitArgType(U));
  auto *B = llvm::dyn_cast_or_null<S::BasicType>(Arg);
  return B && B->BK == S::BasicKind::UnitArg ? B->Unit : nullptr;
}

/// c(U) of GEN-14 at this instance: the factor of U where U has an empty
/// dimension, 1 elsewhere. False if the unit cannot be made.
bool Body::unitCorrection(const S::UnitCanon *U, S::Real &C) {
  C = S::Real::fromInt(llvm::APInt(2, 1));
  if (!U)
    return true;
  const S::UnitCanon *At = unitAtInstance(U);
  if (!At)
    return false;
  if (At->dimensionless())
    C = At->Factor;
  return true;
}

/// The factor of one collapse site at this instance (UnitSite).
bool Body::unitSiteFactor(const S::UnitSite &St, S::Real &F) {
  S::Real R, X, Y;
  if (!unitCorrection(St.Res, R) || !unitCorrection(St.X, X) ||
      !unitCorrection(St.Y, Y))
    return false;
  switch (St.K) {
  case S::UnitSite::Mul:
    F = S::Real::quo(R, S::Real::mul(X, Y));
    return true;
  case S::UnitSite::Quo:
    F = S::Real::quo(S::Real::mul(R, Y), X);
    return true;
  case S::UnitSite::Conv:
    F = S::Real::quo(R, X);
    return true;
  case S::UnitSite::Const:
    F = R;
    return true;
  }
  return false;
}

namespace {
bool sameDimension(const S::UnitCanon *A, const S::UnitCanon *B) {
  if (A->Dims.size() != B->Dims.size())
    return false;
  for (size_t K = 0; K < A->Dims.size(); ++K)
    if (A->Dims[K].Unit != B->Dims[K].Unit ||
        A->Dims[K].Param != B->Dims[K].Param ||
        A->Dims[K].Exp != B->Dims[K].Exp)
      return false;
  return true;
}
} // namespace

//===----------------------------------------------------------------------===//
// Run-time units: the descriptors of an instance's unit parameters (GEN-14)
//===----------------------------------------------------------------------===//

namespace {
/// The unit that stands for a unit type argument of generic code: the unit of
/// a unit argument, or a unit parameter alone.
const S::UnitCanon *argCanon(S::TypeContext &Ctx, S::Type *A) {
  if (auto *B = dyn_cast_or_null<S::BasicType>(A))
    return B->BK == S::BasicKind::UnitArg ? B->Unit : nullptr;
  if (auto *P = dyn_cast_or_null<S::TypeParamType>(A)) {
    S::UnitDim D;
    D.Param = P;
    D.Exp = 1;
    return Ctx.unitCanon(D, S::Real::fromInt(llvm::APInt(2, 1)));
  }
  return nullptr;
}
} // namespace

uint32_t Lowering::unitDescGlobal(const S::UnitCanon *U) {
  auto [It, New] = UnitDescGlobals.try_emplace(U, 0);
  if (!New)
    return It->second;
  Type *U64 = Ctx.u64();
  // {num, den, count, (id, exp) x count}, sorted by id (Runtime.cpp).
  uint64_t Num = 1, Den = 1;
  SmallVector<std::pair<uint64_t, int64_t>, 4> Pairs;
  if (U) {
    llvm::APInt N = U->Factor.ratNum(), D = U->Factor.ratDen();
    if (N.getActiveBits() > 64 || D.getActiveBits() > 64) {
      // Past 64 bits: marked, as the run-time operations mark it (Runtime.cpp).
      Num = Den = 0;
    } else {
      Num = N.getZExtValue();
      Den = D.getZExtValue();
    }
    for (const S::UnitDim &Dm : U->Dims)
      if (Dm.Exp != 0)
        Pairs.push_back({unitBaseId(Dm.Unit), Dm.Exp});
    if (Opts.DenseUnitDescriptors && unitK())
      for (const S::Object *B : UnitBaseList)
        if (llvm::none_of(Pairs,
                          [&](auto &P) { return P.first == unitBaseId(B); }))
          Pairs.push_back({unitBaseId(B), 0});
    llvm::sort(Pairs);
  }
  LowerSteps += 3 + 2 * Pairs.size(); // test-only: the size is counted
  SmallVector<Type *, 8> Fields(3 + 2 * Pairs.size(), U64);
  Global G;
  G.Name = symbolOf(SymbolKind::Runtime, CP.Pkg,
                    "unit." + std::to_string(UnitDescGlobals.size()));
  G.Ty = Ctx.tuple(Fields);
  G.Link = Linkage::Internal;
  G.ReadOnly = true;
  G.Parts.push_back(constInt(U64, Num).C);
  G.Parts.push_back(constInt(U64, Den).C);
  G.Parts.push_back(constInt(U64, Pairs.size()).C);
  for (auto &[Id, Exp] : Pairs) {
    G.Parts.push_back(constInt(U64, Id).C);
    G.Parts.push_back(constInt(U64, uint64_t(Exp)).C);
  }
  It->second = M->addGlobal(std::move(G));
  return It->second;
}

std::string Body::unitText(const S::UnitCanon *U) {
  S::Type *T = L.CP.Ctx->unitArgType(U);
  if (auto *B = dyn_cast<S::BasicType>(T))
    return L.unitSpeller().text(
        B, L.CP.Pkg && L.CP.Pkg->Name == "main" ? L.CP.Pkg : nullptr);
  if (auto *P = dyn_cast<S::TypeParamType>(T))
    return P->Obj ? P->Obj->Name.str() : "unit";
  return "unit";
}

std::string Body::siteText(const S::UnitCanon *U, const ast::Node *N) {
  if (auto *E = dyn_cast_or_null<ast::Expr>(N))
    if (auto It = I.UnitDisplays.find(E); It != I.UnitDisplays.end())
      return S::UnitSpeller::display(
          It->second,
          L.CP.Pkg && L.CP.Pkg->Name == "main" ? L.CP.Pkg : nullptr);
  return unitText(U);
}

Body::Hoist::Hoist(Body &B) : B(B) {
  // Out of loops and branches: into the block that the body's entry jumps
  // through (Body::lower), once for the function (GEN-14).
  if (B.HoistBlock == NoId || B.HoistDepth++)
    return;
  Saved = B.FB.block();
  SavedScope = B.FB.scope();
  SavedSpan = B.FB.span();
  B.FB.setBlock(B.HoistBlock);
  B.FB.setScope(B.HoistScope);
  On = true;
}

Body::Hoist::~Hoist() {
  if (B.HoistBlock == NoId)
    return;
  if (--B.HoistDepth)
    return;
  // (A call ends its block: the block that goes on is the new end.)
  B.HoistBlock = B.FB.block();
  B.FB.setBlock(Saved);
  B.FB.setScope(SavedScope);
  B.FB.setSpan(SavedSpan);
}

/// An E0439 of the units that names its task (P04.24, TODO).
void Body::unitUnsupported(const ast::Node *N, const std::string &What) {
  unsupported(N, What + " (P04.24 follow-up)");
}

/// A pointer to the descriptor of the unit U (over the unit parameters of the
/// generic code), at this instance: a constant of the module where every
/// parameter has its unit here, else made in a local of the frame from the
/// instance's own descriptors (unit_mulpow), once at the entry of the
/// function. Null U is the empty unit with the factor 1.
Operand Body::unitDesc(const S::UnitCanon *U, const ast::Node *N) {
  Type *PU8 = ctx().ptr(ctx().u8());
  auto Const = [&](const S::UnitCanon *C) {
    return copyOf(FB.temp(
        FB.cast(CastKind::PtrToPtr, constGlobal(M, L.unitDescGlobal(C)), PU8)));
  };
  if (!U || !U->HasParam)
    return Const(U);
  if (auto It = DescMemo.find(U); It != DescMemo.end())
    return It->second;
  // (A unit of a type argument that generic code shared across unit
  // arguments passed on: the instance has no descriptor of it, and its types
  // have it erased. The walk refuses the request of such an instance first.)
  if (CurInst && !CurInst->FromCaller.empty())
    for (const S::UnitDim &D : U->Dims)
      if (D.Param && (CurInst->FromCaller.count(D.Param) ||
                      (D.Param->RenamedFrom &&
                       CurInst->FromCaller.count(D.Param->RenamedFrom)))) {
        unitUnsupported(N, "a unit descriptor of a unit that generic code "
                           "shared across unit arguments passed on");
        return Const(nullptr);
      }
  // The parameters this instance knows (a method of an instance of a
  // generic type), else only the descriptors of the function's own.
  const S::UnitCanon *C = S::substituteUnit(
      *L.CP.Ctx, U, [&](const S::TypeParamType *P) -> const S::UnitCanon * {
        if (!CurInst)
          return nullptr;
        auto It = CurInst->Map.find(P);
        if (It == CurInst->Map.end())
          return nullptr;
        auto *B = dyn_cast<S::BasicType>(It->second);
        return B && B->BK == S::BasicKind::UnitArg && B->Unit &&
                       !B->Unit->dimensionless()
                   ? B->Unit
                   : nullptr;
      });
  if (!C) {
    unitUnsupported(N, "a unit whose exponent is past 2^20 in generic code");
    return Const(nullptr);
  }
  SmallVector<S::UnitDim, 4> Rest;
  SmallVector<std::pair<const S::TypeParamType *, int32_t>, 2> Params;
  for (const S::UnitDim &D : C->Dims) {
    if (D.Param)
      Params.push_back({D.Param, D.Exp});
    else
      Rest.push_back(D);
  }
  const S::UnitCanon *Base = L.CP.Ctx->unitCanon(Rest, C->Factor);
  if (Params.empty())
    return Const(Base);
  Lowering::UnitRtFns *RT = L.unitRt(N->getBegin());
  if (!RT)
    return Const(nullptr);
  // A parameter alone, to the power 1: its own descriptor.
  if (Params.size() == 1 && Params[0].second == 1 && Rest.empty() &&
      C->Factor.isRat() && C->Factor.ratNum().isOne() &&
      C->Factor.ratDen().isOne()) {
    const S::TypeParamType *P = Params[0].first;
    auto D = DictLocals.find(P);
    if (D == DictLocals.end() && P->RenamedFrom)
      D = DictLocals.find(P->RenamedFrom);
    if (D != DictLocals.end())
      return DescMemo[U] = copyOf(FB.local(D->second));
  }
  Hoist H(*this);
  // One frame slot for each parameter's product (the helper does not write
  // over its inputs): room for the most base units a unit can have.
  const uint64_t Cap = L.unitK();
  const unsigned Words = 3 + 2 * Cap;
  LowerSteps += Words * Params.size(); // test-only: the size is counted
  Operand Cur = Const(Base);
  at(N);
  for (auto &[P, Exp] : Params) {
    auto D = DictLocals.find(P);
    if (D == DictLocals.end() && P->RenamedFrom)
      D = DictLocals.find(P->RenamedFrom);
    if (D == DictLocals.end()) {
      unitUnsupported(N, "a function literal that needs the unit "
                         "descriptors of its function");
      return Const(nullptr);
    }
    LocalId Slot =
        FB.newLocal(ctx().array(ctx().u64(), Words), Local::Var, "unit");
    Operand Dst = copyOf(FB.temp(
        FB.cast(CastKind::PtrToPtr,
                copyOf(FB.temp(FB.addrOf(FB.local(Slot), true))), PU8)));
    FB.call(constFunc(M, RT->MulPow),
            {Dst, constInt(ctx().u64(), Cap), Cur, copyOf(FB.local(D->second)),
             constInt(ctx().i32(), uint64_t(int64_t(Exp))),
             constStr(M, unitText(U))},
            FB.local(FB.newTemp(ctx().unit())), {UnwindAction::Continue, NoId});
    Cur = Dst;
  }
  return DescMemo[U] = Cur;
}

/// The descriptors a call of the instance GI passes after its arguments: the
/// unit arguments of the call (Key's Info::Instances entry, over this
/// instance's own parameters) for GI's dictionary parameters.
void Body::dictArgsFor(const GenericInst *GI, const ast::Expr *Key,
                       llvm::SmallVectorImpl<const S::UnitCanon *> &Out) {
  if (GI->DictParams.empty())
    return;
  auto It = I.Instances.find(Key);
  if (It == I.Instances.end()) {
    L.internalError(Key->getBegin(), "a call of a generic with unit "
                                     "parameters has no recorded unit "
                                     "arguments");
    return;
  }
  for (unsigned K : GI->DictArgIdx) {
    const S::UnitCanon *C = K < It->second.TArgs.size()
                                ? argCanon(*L.CP.Ctx, It->second.TArgs[K])
                                : nullptr;
    Out.push_back(C);
  }
}

/// The run-time factor of one collapse site, times Sp/Sq: (num, den), made
/// once at the entry of the function.
std::pair<Operand, Operand> Body::dynSite(const S::UnitSite &St, uint64_t Sp,
                                          uint64_t Sq, const ast::Node *N) {
  Lowering::UnitRtFns *RT = L.unitRt(N->getBegin());
  Type *U64 = ctx().u64();
  if (!RT)
    return {constInt(U64, 1), constInt(U64, 1)};
  auto Key = std::make_tuple(unsigned(St.K), St.Res, St.X, St.Y, Sp, Sq);
  if (auto It = SiteMemo.find(Key); It != SiteMemo.end())
    return It->second;
  Hoist H(*this);
  Operand Res = unitDesc(St.Res, N), X = unitDesc(St.X, N),
          Y = unitDesc(St.Y, N);
  unsigned Kind = 0;
  switch (St.K) {
  case S::UnitSite::Mul:
    Kind = 0;
    break;
  case S::UnitSite::Quo:
    Kind = 1;
    break;
  case S::UnitSite::Conv:
    Kind = 2;
    break;
  case S::UnitSite::Const:
    Kind = 3;
    break;
  }
  const S::UnitCanon *Shown = St.Res ? St.Res : (St.X ? St.X : St.Y);
  at(N);
  Type *Pair = ctx().tuple({U64, U64});
  LocalId R = FB.newTemp(Pair);
  FB.call(constFunc(M, RT->Site),
          {constInt(ctx().i32(), Kind), Res, X, Y, constInt(U64, Sp),
           constInt(U64, Sq), constStr(M, Shown ? siteText(Shown, N) : "unit")},
          FB.local(R), {UnwindAction::Continue, NoId});
  Place PR = FB.local(R);
  std::pair<Operand, Operand> Out{
      copyOf(FB.temp(FB.use(copyOf(FB.field(PR, 0))))),
      copyOf(FB.temp(FB.use(copyOf(FB.field(PR, 1)))))};
  return SiteMemo[Key] = Out;
}

/// EXP-33: the quotient of two integer values of one dimension and two scales
/// is the physical ratio, the coarser operand scaled to the finer unit first
/// (`5[KM] / 2[M]` is 5000 / 2). The checker does that for plain code; generic
/// code is checked once, so it records a collapse site and the instance does
/// the same here, with the units it has: the answer is then plain code's, for
/// every value (a quotient first and the factor after would give 6 / 4 * 1000
/// for 6 KM / 4 M, not 6000 / 4).
bool Body::physicalIntQuo(const ast::BinaryExpr *B, S::Real &Ratio,
                          bool &ScaleX) {
  // (Never from the instance's types: a body shared across unit arguments
  // sees them erased, `U*KM` as `KM`; it decides at run time, lowerBinary.)
  if (B->Op != tok::quo || !CurInst || dictMode() || !I.UnitSites.count(B) ||
      erasedUnitOf(B->X) || erasedUnitOf(B->Y))
    return false;
  auto *BX = dyn_cast_or_null<S::BasicType>(sTyOf(B->X));
  auto *BY = dyn_cast_or_null<S::BasicType>(sTyOf(B->Y));
  if (!BX || !BY || !BX->Unit || !BY->Unit || BX->Unit == BY->Unit ||
      !(BX->Info & S::IsInteger) || !(BY->Info & S::IsInteger) ||
      !sameDimension(BX->Unit, BY->Unit))
    return false;
  S::Real Q = S::Real::quo(BX->Unit->Factor, BY->Unit->Factor);
  ScaleX = S::Real::cmp(Q, S::Real::fromInt(llvm::APInt(2, 1))) > 0;
  Ratio = ScaleX ? Q : S::Real::quo(S::Real::fromInt(llvm::APInt(2, 1)), Q);
  return true;
}

Body::UnitScale Body::unitScaleOf(const ast::Expr *E, bool Tag, bool Own) {
  ++LowerSteps; // test-only: the stress tests count the work per expression
  UnitScale U;
  S::Real PQ;
  bool PX;
  // (A quotient the instance does as plain code does has no site factor.)
  const ast::BinaryExpr *PhysQuo = nullptr;
  if (Own)
    if (auto *BE = dyn_cast<ast::BinaryExpr>(E))
      if (physicalIntQuo(BE, PQ, PX))
        PhysQuo = BE;
  U.F = S::Real::fromInt(llvm::APInt(2, 1));
  if (auto It = I.UnitConvs.find(E); It != I.UnitConvs.end()) {
    // (An entry of a plain product is the expression's own factor; the others
    // are scale changes of the value as an operand.)
    if (It->second.Tag == Tag) {
      U.F = S::Real::mul(U.F, It->second.Factor);
      U.Trunc |= It->second.Truncate;
      U.Any = true;
    }
  }
  // An instance that takes unit descriptors decides its sites at run time
  // (GEN-14); one whose units are known (plain code, a method of an instance
  // of a generic type) has constants.
  const bool Dict = dictMode();
  SmallVector<const S::UnitSite *, 2> Dyn;
  if (auto It = I.UnitSites.find(E); It != I.UnitSites.end())
    for (const S::UnitSite &St : It->second) {
      ++LowerSteps;
      if (isRoleOwn(St) ? !Own : St.Tag != Tag)
        continue;
      if (PhysQuo && St.K == S::UnitSite::Quo)
        continue;
      if (Dict) {
        // (An integer quotient decides which operand to scale: lowerBinary.)
        if (St.K == S::UnitSite::Quo && Own && isIntQuo(E))
          continue;
        Dyn.push_back(&St);
        continue;
      }
      // (A unit this instance does not know: the instance's types have it
      // erased, and the factor would be that of the empty unit. The walk
      // refuses the request of such an instance first.)
      if (erasedUnit(St.Res) || erasedUnit(St.X) || erasedUnit(St.Y)) {
        unitUnsupported(E, "a unit operation on a unit that generic code "
                           "shared across unit arguments passed on");
        U.Bad = true;
        continue;
      }
      S::Real F;
      if (!unitSiteFactor(St, F)) {
        U.Bad = true;
        continue;
      }
      U.F = S::Real::mul(U.F, F);
      U.Any = true;
    }
  if (!Dyn.empty()) {
    // The static factor goes into the first site's call, and the sites'
    // fractions are multiplied as exact fractions at run time, once at the
    // entry of the function (GEN-14).
    uint64_t Sp = 1, Sq = 1;
    if (!U.F.isRat() || U.F.ratNum().getActiveBits() > 64 ||
        U.F.ratDen().getActiveBits() > 64) {
      unitUnsupported(E, "a unit conversion whose factor does not fit 64 bits "
                         "in generic code");
      U.Bad = true;
      return U;
    }
    Sp = U.F.ratNum().getZExtValue();
    Sq = U.F.ratDen().getZExtValue();
    Hoist H(*this);
    Type *U64 = ctx().u64();
    auto Plain = [&](Operand O) {
      return copyOf(FB.temp(FB.use(std::move(O))));
    };
    std::pair<Operand, Operand> NQ{constInt(U64, 1), constInt(U64, 1)};
    bool First = true;
    for (const S::UnitSite *St : Dyn) {
      auto SQ = dynSite(*St, Sp, Sq, E);
      Sp = Sq = 1;
      if (First) {
        NQ = SQ;
        First = false;
        continue;
      }
      Lowering::UnitRtFns *RT = L.unitRt(E->getBegin());
      if (!RT)
        break;
      LocalId R = FB.newTemp(ctx().tuple({U64, U64}));
      FB.call(constFunc(M, RT->RatMul),
              {NQ.first, NQ.second, SQ.first, SQ.second, constStr(M, "unit")},
              FB.local(R), {UnwindAction::Continue, NoId});
      NQ = {Plain(copyOf(FB.field(FB.local(R), 0))),
            Plain(copyOf(FB.field(FB.local(R), 1)))};
    }
    {
      const S::UnitSite &St0 = *Dyn.front();
      const S::UnitCanon *Shown = St0.Res ? St0.Res : (St0.X ? St0.X : St0.Y);
      U.DynWhat = Shown ? siteText(Shown, E) : "unit";
    }
    U.DynN = NQ.first;
    U.DynD = NQ.second;
    U.Dyn = true;
    U.Any = true;
    U.F = S::Real::fromInt(llvm::APInt(2, 1));
    return U;
  }
  if (U.Any && U.F.isRat() && U.F.ratNum().isOne() && U.F.ratDen().isOne())
    U.Any = false;
  return U;
}

/// Is E a division of two integer values?
bool Body::isIntQuo(const ast::Expr *E) {
  auto *B = dyn_cast<ast::BinaryExpr>(E);
  if (!B || B->Op != tok::quo)
    return false;
  S::Type *T = sTyOf(E);
  auto *BT = dyn_cast_or_null<S::BasicType>(T ? S::under(T) : nullptr);
  return BT && (BT->Info & S::IsInteger);
}

/// The constant V times the exact factor F (a scale change of the unit the
/// constant is in): one rounding when it becomes a number of its type, a
/// truncation toward zero for an integer type. The place every constant goes
/// through (Body::constOperand).
S::ConstValue Body::scaleConstant(const S::ConstValue &V, const S::Real &F,
                                  bool IntKind) {
  if (!V.isNumeric() || (F.isRat() && F.ratNum().isOne() && F.ratDen().isOne()))
    return V;
  if (V.kind() == S::ConstValue::Complex)
    return S::ConstValue::makeComplex(S::Real::mul(V.floatVal(), F),
                                      S::Real::mul(V.imagVal(), F));
  S::Real X = V.kind() == S::ConstValue::Int ? S::Real::fromInt(V.intVal())
                                             : V.floatVal();
  S::Real P = S::Real::mul(X, F);
  llvm::APInt N;
  if (P.toInt(N))
    return IntKind || V.kind() == S::ConstValue::Int
               ? S::ConstValue::makeInt(N)
               : S::ConstValue::makeFloat(P);
  if (IntKind && P.isRat()) {
    llvm::APInt Nu = P.ratNum(), De = P.ratDen();
    unsigned W = std::max(Nu.getBitWidth(), De.getBitWidth());
    return S::ConstValue::makeInt(Nu.sext(W).sdiv(De.sext(W)));
  }
  return S::ConstValue::makeFloat(P);
}

Operand Body::scaleFloat(Operand V, const S::Real &F) {
  Type *T = V.type();
  double D = F.toIEEE(T->bits() == 32 ? 24 : 53);
  Operand P =
      copyOf(FB.temp(FB.binary(BinOp::Mul, std::move(V), constFloat(T, D))));
  // The product is a value of its own: not contracted into the add or the
  // subtract that reads it (the conversion is rounded once, EXP-32).
  return copyOf(FB.temp(FB.cast(CastKind::FloatToFloat, std::move(P), T)));
}

Operand Body::scaleInt(Operand V, const S::Real &F, const ast::Node *N) {
  Type *T = V.type();
  llvm::APInt Whole;
  if (F.toInt(Whole)) {
    // An integer factor: a wrapping multiplication (a factor past the type
    // wraps with it, like every integer product).
    uint64_t Bits = Whole.isNegative() ? Whole.sextOrTrunc(64).getZExtValue()
                                       : Whole.zextOrTrunc(64).getZExtValue();
    return copyOf(
        FB.temp(FB.binary(BinOp::Mul, std::move(V), constInt(T, Bits))));
  }
  // x * p / q, truncated toward zero. Done in 64 bits, as x / q * p +
  // (x % q) * p / q: the parts are exact and nothing but the result's own
  // wrapping overflows.
  if (!F.isRat() || F.sign() <= 0) {
    unitUnsupported(N, "a unit conversion by a factor that is not an exact "
                       "ratio");
    return errorValue(ctx());
  }
  llvm::APInt P = F.ratNum(), Q = F.ratDen();
  if (P.getActiveBits() > 64 || Q.getActiveBits() > 64) {
    unitUnsupported(N, "an integer unit conversion by " + F.str() +
                           ", whose numerator or denominator is past 64 bits "
                           "(convert to float first)");
    return errorValue(ctx());
  }
  llvm::APInt Prod = P.zext(130) * Q.zext(130);
  if (Prod.getActiveBits() > 62) {
    // The parts of x / q * p + (x % q) * p / q could overflow 64 bits: the
    // exact helper (a 128-bit product), as generic code does.
    Operand Dyn =
        scaleIntExact(std::move(V), constInt(ctx().u64(), P.getZExtValue()),
                      constInt(ctx().u64(), Q.getZExtValue()), N, F.str());
    return Dyn;
  }
  uint64_t Pn = P.getZExtValue(), Qd = Q.getZExtValue();
  bool Signed = T->isSigned();
  Type *Wide = Signed ? ctx().i64() : ctx().u64();
  Operand X = V;
  if (T != Wide)
    X = copyOf(FB.temp(FB.cast(CastKind::IntToInt, std::move(V), Wide)));
  auto Bin = [&](BinOp Op, Operand A, Operand B) {
    return copyOf(FB.temp(FB.binary(Op, std::move(A), std::move(B))));
  };
  Operand R;
  if (Pn == 1) {
    R = Bin(BinOp::Div, X, constInt(Wide, Qd));
  } else {
    Operand A = Bin(BinOp::Div, X, constInt(Wide, Qd));
    Operand B = Bin(BinOp::Rem, X, constInt(Wide, Qd));
    Operand AP = Bin(BinOp::Mul, A, constInt(Wide, Pn));
    Operand BP = Bin(BinOp::Mul, B, constInt(Wide, Pn));
    Operand BQ = Bin(BinOp::Div, BP, constInt(Wide, Qd));
    R = Bin(BinOp::Add, AP, BQ);
  }
  if (T != Wide)
    R = copyOf(FB.temp(FB.cast(CastKind::IntToInt, std::move(R), T)));
  return R;
}

/// The integer V times N / D (u64 operands), truncating toward zero, exact
/// (unit_scale_int: the second part of the product in 128 bits).
Operand Body::scaleIntExact(Operand V, Operand N, Operand D,
                            const ast::Node *N0, const std::string &What) {
  Lowering::UnitRtFns *RT = L.unitRt(N0->getBegin());
  if (!RT)
    return errorValue(ctx());
  Type *T = V.type();
  Type *U64 = ctx().u64();
  bool Signed = T->isSigned();
  auto Cast = [&](CastKind K, Operand X, Type *To) {
    return copyOf(FB.temp(FB.cast(K, std::move(X), To)));
  };
  at(N0);
  // (A narrower signed integer is widened by its sign, then taken as bits.)
  Operand X = V;
  if (T != U64) {
    if (Signed)
      X = Cast(CastKind::IntToInt, std::move(X), ctx().i64());
    X = Cast(CastKind::IntToInt, std::move(X), U64);
  }
  LocalId R = FB.newTemp(U64);
  FB.call(constFunc(M, RT->ScaleInt),
          {X, constBool(ctx(), Signed), std::move(N), std::move(D),
           constStr(M, What)},
          FB.local(R), {UnwindAction::Continue, NoId});
  Operand Res = copyOf(FB.local(R));
  return T == U64 ? Res : Cast(CastKind::IntToInt, std::move(Res), T);
}

/// V * N / D with N and D run-time fractions (u64), as the factor of a site
/// at an instance that takes unit descriptors. A float is multiplied by
/// N / D rounded once to its type (unit_ratio: as the compiler rounds a
/// constant factor, so generic code and plain code agree to the bit); an
/// integer by unit_scale_int, truncating toward zero.
Operand Body::scaleDyn(Operand V, Operand N, Operand D, const ast::Node *N0,
                       const std::string &What) {
  Type *T = V.type();
  Type *F64 = ctx().floatTy(64);
  auto Cast = [&](CastKind K, Operand X, Type *To) {
    return copyOf(FB.temp(FB.cast(K, std::move(X), To)));
  };
  at(N0);
  if (T->isFloat() || (T->getKind() == Type::Kind::Tuple &&
                       T->numFields() == 2 && T->elems()[0]->isFloat())) {
    Lowering::UnitRtFns *RT = L.unitRt(N0->getBegin());
    if (!RT)
      return errorValue(ctx());
    Type *FT0 = T->isFloat() ? T : T->elems()[0];
    uint64_t P = FT0->bits() == 32 ? 24 : 53;
    // The factor as a float of the type, made once at the entry.
    Operand Q;
    {
      std::tuple<LocalId, LocalId, uint64_t> Key{NoId, NoId, P};
      bool Memo = N.K == Operand::Copy && D.K == Operand::Copy &&
                  N.P.Proj.empty() && D.P.Proj.empty();
      if (Memo)
        Key = {N.P.Local, D.P.Local, P};
      auto It = Memo ? RatioMemo.find(Key) : RatioMemo.end();
      if (It != RatioMemo.end()) {
        Q = It->second;
      } else {
        Hoist H(*this);
        LocalId R = FB.newTemp(F64);
        FB.call(constFunc(M, RT->Ratio),
                {N, D, constInt(ctx().u64(), P), constStr(M, What)},
                FB.local(R), {UnwindAction::Continue, NoId});
        Q = copyOf(FB.local(R));
        if (FT0 != F64)
          Q = Cast(CastKind::FloatToFloat, Q, FT0);
        if (Memo)
          RatioMemo[Key] = Q;
      }
    }
    // A factor out of range (0 / 0) is the panic of this site, one compare
    // where the factor is used (the work to make it was done at the entry).
    {
      FuncId Panic = L.ensurePanicUnit(N0->getBegin());
      if (Panic == NoId)
        return errorValue(ctx());
      Operand Marked =
          copyOf(FB.temp(FB.binary(BinOp::Eq, D, constInt(ctx().u64(), 0))));
      BlockId Bad = FB.newBlock(), Ok = FB.newBlock();
      FB.branch(Marked, Bad, Ok);
      FB.setBlock(Bad);
      FB.call(constFunc(M, Panic), {constStr(M, What)},
              FB.local(FB.newTemp(ctx().unit())),
              {UnwindAction::Continue, NoId});
      FB.unreachable();
      FB.setBlock(Ok);
    }
    auto One = [&](Operand X) {
      Operand Prod = copyOf(FB.temp(FB.binary(BinOp::Mul, std::move(X), Q)));
      // (Not contracted into the add that reads it, EXP-32.)
      return Cast(CastKind::FloatToFloat, std::move(Prod), FT0);
    };
    if (T->isFloat())
      return One(std::move(V));
    Place PV = placeOf(std::move(V));
    Operand Re = One(copyOf(FB.field(PV, 0)));
    Operand Im = One(copyOf(FB.field(PV, 1)));
    return copyOf(FB.temp(FB.aggregate(AggKind::Tuple, T, {Re, Im})));
  }
  if (!T->isInt()) {
    unitUnsupported(N0, "a unit conversion of this type");
    return errorValue(ctx());
  }
  return scaleIntExact(std::move(V), std::move(N), std::move(D), N0, What);
}

Operand Body::scaleValue(Operand V, const UnitScale &U, const ast::Node *N) {
  ++LowerSteps;
  Type *T = V.type();
  if (!T || !U.Any)
    return V;
  at(N);
  if (U.Dyn)
    return scaleDyn(std::move(V), U.DynN, U.DynD, N, U.DynWhat);
  if (T->isFloat())
    return scaleFloat(std::move(V), U.F);
  if (T->isInt())
    return scaleInt(std::move(V), U.F, N);
  if (T->getKind() == Type::Kind::Tuple && T->numFields() == 2 &&
      T->elems()[0]->isFloat()) {
    // A complex number: both parts.
    Place PV = placeOf(std::move(V));
    Operand Re = scaleFloat(copyOf(FB.field(PV, 0)), U.F);
    Operand Im = scaleFloat(copyOf(FB.field(PV, 1)), U.F);
    return copyOf(FB.temp(FB.aggregate(AggKind::Tuple, T, {Re, Im})));
  }
  unitUnsupported(N, "a unit conversion of this type");
  return errorValue(ctx());
}

Operand Body::lowerUnitIn(const ast::SelectorExpr *Sel) {
  // `x.In[U]()` is the receiver rescaled: the checker keyed the scale change
  // to the receiver expression (it is lowered, and scaled, as any operand).
  return lowerExpr(Sel->X);
}
