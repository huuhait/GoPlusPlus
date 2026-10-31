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
#include "RtEmit.h"
#include "gopp/GIR/UnitRuntime.h"

#include "llvm/ADT/SmallString.h"

#include <array>

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace {
bool isRoleOwn(const S::UnitSite &S) { return S.K != S::UnitSite::Conv; }

/// The integer scaling by p/q as unit_intfactor makes it at run time (the
/// same words, so plain and generic code truncate alike): trunc(x * p / q) =
/// x * A + trunc(x * N / D) modulo 2^64 for every |x| < 2^64. A = floor(p/q)
/// mod 2^64; N / D = (p mod q) / q when q has at most 64 bits, else its best
/// lower approximation with a denominator of at most 2^64 - 1 (no fraction
/// with such a denominator lies between the two, so the floor of x times
/// either is the same).
struct IntFactorWords {
  uint64_t A, N, D;
};
IntFactorWords intFactorOf(const llvm::APInt &P0, const llvm::APInt &Q0) {
  unsigned W = std::max(P0.getBitWidth(), Q0.getBitWidth()) + 2;
  llvm::APInt P = P0.zext(W), Q = Q0.zext(W), Aq, B;
  llvm::APInt::udivrem(P, Q, Aq, B);
  uint64_t A = Aq.zextOrTrunc(64).getZExtValue();
  if (Q.getActiveBits() <= 64)
    return {A, B.getZExtValue(), Q.getZExtValue()};
  if (B.isZero())
    return {A, 0, 1};
  // The Stern-Brocot walk with jumps from L = 0/1, U = 1/1 (Euclid on
  // R = B ld - ln q and S = un q - B ud), as unit_intfactor.
  const uint64_t Top = ~uint64_t(0);
  llvm::APInt R = B, S = Q - B, T, Rm;
  uint64_t Ln = 0, Ld = 1, Un = 1, Ud = 1;
  for (;;) {
    uint64_t CapL = (Top - Ld) / Ud;
    llvm::APInt::udivrem(R, S, T, Rm);
    if (T.getActiveBits() > 64 || T.getZExtValue() > CapL) {
      Ln += CapL * Un;
      Ld += CapL * Ud;
      break;
    }
    Ln += T.getZExtValue() * Un;
    Ld += T.getZExtValue() * Ud;
    R = Rm;
    if (R.isZero())
      break;
    uint64_t CapU = (Top - Ud) / Ld;
    llvm::APInt::udivrem(S, R, T, Rm);
    if (T.getActiveBits() > 64 || T.getZExtValue() > CapU)
      break;
    Un += T.getZExtValue() * Ln;
    Ud += T.getZExtValue() * Ld;
    S = Rm;
    if (S.isZero())
      break;
  }
  return {A, Ln, Ld};
}
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

namespace {
/// The limbs of the non-negative A, the least significant first, no top zero.
void appendLimbs(const llvm::APInt &A, llvm::SmallVectorImpl<uint64_t> &Out) {
  unsigned N = (A.getActiveBits() + 63) / 64;
  for (unsigned I = 0; I < N; ++I)
    Out.push_back(A.extractBitsAsZExtValue(
        std::min(64u, A.getBitWidth() - 64 * I), 64 * I));
}

/// The words of the fraction F (gopp/GIR/UnitRuntime.h): {nn, nd, limbs};
/// {0, 0} if F is not an exact positive rational within the bound.
void fracWords(const S::Real &F, llvm::SmallVectorImpl<uint64_t> &Out) {
  if (!F.isRat() || F.sign() <= 0 ||
      F.ratNum().getActiveBits() > 64 * UnitMaxLimbs ||
      F.ratDen().getActiveBits() > 64 * UnitMaxLimbs) {
    Out.append({0, 0});
    return;
  }
  SmallVector<uint64_t, 8> N, D;
  appendLimbs(F.ratNum(), N);
  appendLimbs(F.ratDen(), D);
  Out.push_back(N.size());
  Out.push_back(D.size());
  Out.append(N.begin(), N.end());
  Out.append(D.begin(), D.end());
}
} // namespace

uint32_t Lowering::unitWordsGlobal(ArrayRef<uint64_t> Words, const char *Kind,
                                   size_t Number) {
  Type *U64 = Ctx.u64();
  SmallVector<Type *, 8> Fields(Words.size(), U64);
  Global G;
  G.Name = symbolOf(SymbolKind::Runtime, CP.Pkg,
                    std::string(Kind) + "." + std::to_string(Number));
  G.Ty = Ctx.tuple(Fields);
  G.Link = Linkage::Internal;
  G.ReadOnly = true;
  for (uint64_t W : Words)
    G.Parts.push_back(constInt(U64, W).C);
  return M->addGlobal(std::move(G));
}

void Lowering::unitDescWords(const S::UnitCanon *U,
                             llvm::SmallVectorImpl<uint64_t> &Words) {
  // {count, (id, exp) x count sorted by id, the factor} (Runtime.cpp).
  SmallVector<std::pair<uint64_t, int64_t>, 4> Pairs;
  S::Real F = S::Real::fromInt(llvm::APInt(2, 1));
  if (U) {
    F = U->Factor;
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
  size_t Start = Words.size();
  Words.push_back(Pairs.size());
  for (auto &[Id, Exp] : Pairs) {
    Words.push_back(Id);
    Words.push_back(uint64_t(Exp));
  }
  fracWords(F, Words);
  LowerSteps += Words.size() - Start; // test-only: the size is counted
}

uint32_t Lowering::unitDescGlobal(const S::UnitCanon *U) {
  auto [It, New] = UnitDescGlobals.try_emplace(U, 0);
  if (!New)
    return It->second;
  SmallVector<uint64_t, 16> Words;
  unitDescWords(U, Words);
  It->second = unitWordsGlobal(Words, "unit", UnitDescGlobals.size());
  return It->second;
}

uint32_t Lowering::unitRecipeGlobal(ArrayRef<uint64_t> Words) {
  std::vector<uint64_t> Key(Words.begin(), Words.end());
  auto [It, New] = UnitRecipeGlobals.try_emplace(std::move(Key), 0);
  if (!New)
    return It->second;
  It->second = unitWordsGlobal(Words, "unitr", UnitRecipeGlobals.size());
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

/// The recipe of the unit U (over the unit parameters of the generic code) at
/// this instance (gopp/GIR/UnitRuntime.h): the parameters it knows (a method
/// of an instance of a generic type) substituted, each other parameter by the
/// index of its descriptor. False after a report.
bool Body::unitRecipe(const S::UnitCanon *U, const ast::Node *N,
                      llvm::SmallVectorImpl<uint64_t> &Words) {
  if (!U || !U->HasParam) {
    Words.push_back(0);
    L.unitDescWords(U, Words);
    return true;
  }
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
        return false;
      }
  auto Known = [&](const S::TypeParamType *P) -> const S::UnitCanon * {
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
  };
  const S::UnitCanon *C = S::substituteUnit(*L.CP.Ctx, U, Known);
  if (!C) {
    // (The known units leave the range of units: the recipe is the unit out
    // of range, so a site that needs it panics, as for one made at run time.)
    Words.push_back(0);
    Words.push_back(UnitCountOut);
    return true;
  }
  SmallVector<S::UnitDim, 4> Rest;
  SmallVector<std::pair<uint64_t, int64_t>, 2> Params;
  for (const S::UnitDim &D : C->Dims) {
    if (!D.Param) {
      Rest.push_back(D);
      continue;
    }
    auto It = DictIndex.find(D.Param);
    if (It == DictIndex.end() && D.Param->RenamedFrom)
      It = DictIndex.find(D.Param->RenamedFrom);
    if (It == DictIndex.end()) {
      unitUnsupported(N, "a function literal that needs the unit "
                         "descriptors of its function");
      return false;
    }
    Params.push_back({It->second, D.Exp});
  }
  Words.push_back(Params.size());
  for (auto &[Idx, Exp] : Params) {
    Words.push_back(Idx);
    Words.push_back(uint64_t(Exp));
  }
  L.unitDescWords(L.CP.Ctx->unitCanon(Rest, C->Factor), Words);
  return true;
}

/// The array of the instance's descriptor pointers, by parameter index, made
/// once at the entry of the function.
Operand Body::unitDicts() {
  Type *PU8 = ctx().ptr(ctx().u8());
  Hoist H(*this);
  auto Addr = [&] {
    return copyOf(FB.temp(
        FB.cast(CastKind::PtrToPtr,
                copyOf(FB.temp(FB.addrOf(FB.local(DictsSlot), true))), PU8)));
  };
  const unsigned N = CurInst ? CurInst->DictParams.size() : 0;
  const bool New = DictsFn != &FB.function();
  if (New) {
    DictsFn = &FB.function();
    DictsSlot = FB.newLocal(ctx().array(ctx().u64(), std::max(N, 1u)),
                            Local::Var, "unitd");
  }
  // (Filled once where it is made at the entry; else at each use, which the
  // first use need not dominate.)
  if (New || HoistBlock == NoId) {
    Operand A = Addr();
    Rt E(M, FB);
    for (unsigned K = 0; K < N; ++K)
      if (auto It = DictLocals.find(CurInst->DictParams[K]);
          It != DictLocals.end())
        E.store(E.padd(Val{&E, A}, E.u(8 * K)), PU8,
                Val{&E, copyOf(FB.local(It->second))});
  }
  return Addr();
}

/// A pointer to the descriptor of the unit U (over the unit parameters of the
/// generic code), at this instance, to pass to a callee: a constant of the
/// module where every parameter has its unit here, a parameter's own
/// descriptor, else made by unit_make in a small slot of the frame (a wide
/// factor spills to the function's owned spill chain), once at the entry of
/// the function. Null U is the empty unit with the factor 1.
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
  SmallVector<uint64_t, 16> Words;
  if (!unitRecipe(U, N, Words))
    return Const(nullptr);
  // Every parameter known here (a method of an instance of a generic type, a
  // function literal in one): the recipe is its base, a constant descriptor.
  // (A cast made at each use, as Const: this body may have no entry block to
  // hoist into, and a temp of one branch is not defined in another.)
  if (Words[0] == 0)
    return copyOf(FB.temp(FB.cast(
        CastKind::PtrToPtr,
        constGlobal(M,
                    L.unitRecipeGlobal(ArrayRef<uint64_t>(Words).drop_front())),
        PU8)));
  // A parameter alone, to the power 1 (no base unit, the factor 1): its own
  // descriptor. {np 1, index, exp 1, count 0, nn 1, nd 1, 1, 1}
  if (Words.size() == 8 && Words[0] == 1 && Words[2] == 1 && Words[3] == 0 &&
      Words[4] == 1 && Words[5] == 1 && Words[6] == 1 && Words[7] == 1 &&
      CurInst && Words[1] < CurInst->DictParams.size())
    if (auto D = DictLocals.find(CurInst->DictParams[Words[1]]);
        D != DictLocals.end())
      return DescMemo[U] = copyOf(FB.local(D->second));
  Lowering::UnitRtFns *RT = L.unitRt(N->getBegin());
  if (!RT)
    return Const(nullptr);
  // (A run-time parameter needs the descriptors of this body: a body with no
  // spill chain has none, and the empty unit would be a wrong value.)
  if (SpillLocal == LocalId(NoId) || DictLocals.empty()) {
    unitUnsupported(N, "a function literal that needs the unit "
                       "descriptors of its function");
    return Const(nullptr);
  }
  Hoist H(*this);
  // A slot with room for the most base units a unit can have and a fraction
  // of UnitSmallLimbs limbs; a wider one goes to the spill chain.
  const uint64_t Cap = L.unitK();
  const unsigned Slot = 1 + 2 * Cap + 2 + UnitSmallLimbs;
  LowerSteps += Slot; // test-only: the size is counted
  Operand Dicts = unitDicts();
  Operand Recipe = copyOf(FB.temp(FB.cast(
      CastKind::PtrToPtr, constGlobal(M, L.unitRecipeGlobal(Words)), PU8)));
  at(N);
  LocalId S = FB.newLocal(ctx().array(ctx().u64(), Slot), Local::Var, "unit");
  Operand Dst = copyOf(FB.temp(FB.cast(
      CastKind::PtrToPtr, copyOf(FB.temp(FB.addrOf(FB.local(S), true))), PU8)));
  Operand Spill = copyOf(FB.temp(
      FB.cast(CastKind::PtrToPtr,
              copyOf(FB.temp(FB.addrOf(FB.local(SpillLocal), true))), PU8)));
  SpillUsed = true;
  FB.call(constFunc(M, RT->Make),
          {Dst, constInt(ctx().u64(), Cap),
           constInt(ctx().u64(), UnitSmallLimbs), Recipe, Dicts, Spill},
          FB.local(FB.newTemp(ctx().unit())), {UnwindAction::Continue, NoId});
  // (Memoized only where it was made at the entry: it dominates every use.)
  if (HoistBlock != NoId)
    DescMemo[U] = Dst;
  return Dst;
}

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

/// The site group of U (its run-time sites and static factor, UnitRuntime.h)
/// as a constant of the module; NoId after a report.
uint32_t Body::unitGroup(const UnitScale &U, const ast::Node *N) {
  SmallVector<uint64_t, 32> Words;
  fracWords(U.DynStatic, Words);
  Words.push_back(U.DynSites.size());
  for (const S::UnitSite *St : U.DynSites) {
    unsigned Kind = 0;
    switch (St->K) {
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
    Words.push_back(Kind);
    if (!unitRecipe(St->Res, N, Words) || !unitRecipe(St->X, N, Words) ||
        !unitRecipe(St->Y, N, Words))
      return NoId;
  }
  return L.unitRecipeGlobal(Words);
}

/// The float factor of U for a float of type FT, made once at the entry
/// (unit_site_f: rounded as the compiler rounds a constant factor): in FT,
/// and as the f64 that is 0 for a factor out of range.
std::pair<Operand, Operand> Body::dynFloat(const UnitScale &U, Type *FT,
                                           const ast::Node *N) {
  uint64_t P = FT->bits() == 32 ? 24 : 53;
  Lowering::UnitRtFns *RT = L.unitRt(N->getBegin());
  uint32_t G = unitGroup(U, N);
  if (!RT || G == NoId)
    return {errorValue(ctx()), constFloat(ctx().floatTy(64), 1.0)};
  auto Key = std::make_pair(G, P);
  if (auto It = DynFloatMemo.find(Key); It != DynFloatMemo.end())
    return It->second;
  Hoist H(*this);
  Type *PU8 = ctx().ptr(ctx().u8()), *F64 = ctx().floatTy(64);
  Operand Dicts = unitDicts();
  Operand Grp =
      copyOf(FB.temp(FB.cast(CastKind::PtrToPtr, constGlobal(M, G), PU8)));
  at(N);
  LocalId R = FB.newTemp(F64);
  FB.call(constFunc(M, RT->SiteF), {Grp, Dicts, constInt(ctx().u64(), P)},
          FB.local(R), {UnwindAction::Continue, NoId});
  Operand Q64 = copyOf(FB.local(R)), Q = Q64;
  if (FT != F64)
    Q = copyOf(FB.temp(FB.cast(CastKind::FloatToFloat, Q, FT)));
  if (HoistBlock != NoId)
    DynFloatMemo[Key] = {Q, Q64};
  return {Q, Q64};
}

/// The integer factor of U (unit_site_i with Mode): {A, n, d, inverted}, made
/// once at the entry.
std::array<Operand, 4> Body::dynInt(const UnitScale &U, unsigned Mode,
                                    const ast::Node *N) {
  Type *U64 = ctx().u64();
  Lowering::UnitRtFns *RT = L.unitRt(N->getBegin());
  uint32_t G = unitGroup(U, N);
  if (!RT || G == NoId)
    return {constInt(U64, 0), constInt(U64, 0), constInt(U64, 1),
            constInt(U64, 0)};
  auto Key = std::make_pair(G, uint64_t(100 + Mode));
  if (auto It = DynIntMemo.find(Key); It != DynIntMemo.end())
    return It->second;
  Hoist H(*this);
  Type *PU8 = ctx().ptr(ctx().u8());
  Operand Dicts = unitDicts();
  Operand Grp =
      copyOf(FB.temp(FB.cast(CastKind::PtrToPtr, constGlobal(M, G), PU8)));
  at(N);
  // (One slot for the words of every site of the function: they are read
  // into temporaries at once.)
  if (OutFn != &FB.function()) {
    OutFn = &FB.function();
    OutSlot = FB.newLocal(ctx().array(U64, 4), Local::Var, "unitk");
  }
  Operand OutP = copyOf(FB.temp(
      FB.cast(CastKind::PtrToPtr,
              copyOf(FB.temp(FB.addrOf(FB.local(OutSlot), true))), PU8)));
  FB.call(constFunc(M, RT->SiteI), {OutP, Grp, Dicts, constInt(U64, Mode)},
          FB.local(FB.newTemp(ctx().unit())), {UnwindAction::Continue, NoId});
  Rt E(M, FB);
  std::array<Operand, 4> W;
  for (unsigned K = 0; K < 4; ++K)
    W[K] = E.load(E.padd(Val{&E, OutP}, E.u(8 * K)), U64).Op;
  if (HoistBlock != NoId)
    DynIntMemo[Key] = W;
  return W;
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
    // The static factor and the sites' fractions are multiplied as exact
    // fractions at run time, once at the entry of the function (GEN-14),
    // where the value's type turns the product into its factor (scaleDyn).
    // (A static factor past the bound is a marked fraction: the site panics
    // with "unit out of range" where it is used.)
    const S::UnitSite &St0 = *Dyn.front();
    const S::UnitCanon *Shown = St0.Res ? St0.Res : (St0.X ? St0.X : St0.Y);
    U.DynWhat = Shown ? siteText(Shown, E) : "unit";
    U.DynSites.assign(Dyn.begin(), Dyn.end());
    U.DynStatic = U.F;
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
  llvm::APInt Prod = P.zext(P.getBitWidth() + Q.getBitWidth()) *
                     Q.zext(P.getBitWidth() + Q.getBitWidth());
  if (P.getActiveBits() > 64 || Q.getActiveBits() > 64 ||
      Prod.getActiveBits() > 62) {
    // The parts of x / q * p + (x % q) * p / q could overflow 64 bits, or p
    // or q has more: the exact helper, with the three words generic code
    // computes at run time (unit_intfactor), so both truncate alike.
    IntFactorWords W = intFactorOf(P, Q);
    Type *U64 = ctx().u64();
    return scaleIntExact(std::move(V), constInt(U64, W.A), constInt(U64, W.N),
                         constInt(U64, W.D), N, F.str());
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

/// The integer V times A + N / D (u64 operands, N < D, unit_intfactor's
/// words), truncating toward zero and wrapping, exact (unit_scale_int: the
/// second part of the product in 128 bits); D 0 panics with What.
Operand Body::scaleIntExact(Operand V, Operand A, Operand N, Operand D,
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
          {X, constBool(ctx(), Signed), std::move(A), std::move(N),
           std::move(D), constStr(M, What)},
          FB.local(R), {UnwindAction::Continue, NoId});
  Operand Res = copyOf(FB.local(R));
  return T == U64 ? Res : Cast(CastKind::IntToInt, std::move(Res), T);
}

/// V times the run-time factor of U, at an instance that takes unit
/// descriptors. A float is multiplied by the factor rounded once to its type
/// (unit_ratio: as the compiler rounds a constant factor, so generic code and
/// plain code agree to the bit); an integer by unit_scale_int, truncating
/// toward zero (unit_intfactor's words, as plain code's).
Operand Body::scaleDyn(Operand V, const UnitScale &U, const ast::Node *N0) {
  Type *T = V.type();
  auto Cast = [&](CastKind K, Operand X, Type *To) {
    return copyOf(FB.temp(FB.cast(K, std::move(X), To)));
  };
  at(N0);
  if (T->isFloat() || (T->getKind() == Type::Kind::Tuple &&
                       T->numFields() == 2 && T->elems()[0]->isFloat())) {
    Type *FT0 = T->isFloat() ? T : T->elems()[0];
    auto [Q, Q64] = dynFloat(U, FT0, N0);
    at(N0);
    // A factor out of range (of units, or of the float type: 0) is the panic
    // of this site, one compare where the factor is used (the work to make it
    // was done at the entry).
    {
      FuncId Panic = L.ensurePanicUnit(N0->getBegin());
      if (Panic == NoId)
        return errorValue(ctx());
      Operand Marked = copyOf(FB.temp(
          FB.binary(BinOp::Eq, Q64, constFloat(ctx().floatTy(64), 0.0))));
      BlockId Bad = FB.newBlock(), Ok = FB.newBlock();
      FB.branch(Marked, Bad, Ok);
      FB.setBlock(Bad);
      FB.call(constFunc(M, Panic), {constStr(M, U.DynWhat)},
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
  std::array<Operand, 4> W = dynInt(U, 0, N0);
  return scaleIntExact(std::move(V), W[0], W[1], W[2], N0, U.DynWhat);
}

Operand Body::scaleValue(Operand V, const UnitScale &U, const ast::Node *N) {
  ++LowerSteps;
  Type *T = V.type();
  if (!T || !U.Any)
    return V;
  at(N);
  if (U.Dyn)
    return scaleDyn(std::move(V), U, N);
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
