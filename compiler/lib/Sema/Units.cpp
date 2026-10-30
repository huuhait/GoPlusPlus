//===- Units.cpp - Unit arithmetic ----------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Units.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::sema;
using llvm::ArrayRef;
using llvm::SmallVector;
using llvm::SmallVectorImpl;
using llvm::StringRef;

//===----------------------------------------------------------------------===//
// Dimension order and interning
//===----------------------------------------------------------------------===//

namespace gopp::sema::units {
Real realOne() { return Real::fromInt(llvm::APInt(64, 1, true)); }
std::string qualifiedText(ArrayRef<UnitDim> Parts);
} // namespace gopp::sema::units

namespace {

/// The sort key of a dimension: declared units by package path and name, unit
/// parameters after them by name and index. Names decide, never addresses,
/// except between two different objects with the same key (two unit
/// parameters of one name cannot meet in one unit).
std::string dimKey(const UnitDim &D) {
  if (D.Unit) {
    const Package *P = D.Unit->Pkg;
    std::string K = P ? P->Path.str() : std::string();
    K += '\0';
    K += D.Unit->Name.str();
    return K;
  }
  std::string K = "\x7f";
  K += D.Param->Obj ? D.Param->Obj->Name.str() : std::string();
  K += '\0';
  K += std::to_string(D.Param->Index);
  return K;
}

bool sameBase(const UnitDim &A, const UnitDim &B) {
  return A.Unit == B.Unit && A.Param == B.Param;
}

std::string internKey(ArrayRef<UnitDim> Dims, const Real &F) {
  std::string K;
  llvm::raw_string_ostream OS(K);
  for (const UnitDim &D : Dims)
    OS << (D.Unit ? 'u' : 'p')
       << (const void *)(D.Unit ? (const void *)D.Unit : (const void *)D.Param)
       << ':' << D.Exp << ';';
  OS << units::realStr(F);
  return K;
}

} // namespace

const UnitCanon *TypeContext::unitCanon(ArrayRef<UnitDim> Dims,
                                        const Real &Factor) {
  noteUnits();
  std::string Key = internKey(Dims, Factor);
  std::lock_guard<std::mutex> G(UnitMu);
  auto It = UnitCanons.find(Key);
  if (It != UnitCanons.end())
    return It->second;
  UnitStore.emplace_back();
  UnitCanon *C = &UnitStore.back();
  {
    std::lock_guard<std::mutex> G2(AllocMu);
    C->Dims = Objs.copy(Dims);
  }
  C->Factor = Factor;
  for (const UnitDim &D : Dims)
    if (D.Param)
      C->HasParam = true;
  UnitCanons.try_emplace(Key, C);
  return C;
}

const UnitDisplay *TypeContext::unitDisplay(ArrayRef<UnitDim> Parts) {
  if (Parts.empty())
    return nullptr;
  std::string Key = internKey(Parts, units::realOne());
  std::lock_guard<std::mutex> G(UnitMu);
  auto It = UnitDisplays.find(Key);
  if (It != UnitDisplays.end())
    return It->second;
  UnitDisplayStore.emplace_back();
  UnitDisplay *D = &UnitDisplayStore.back();
  std::string Text = units::qualifiedText(Parts);
  {
    std::lock_guard<std::mutex> G2(AllocMu);
    D->Parts = Objs.copy(Parts);
    D->Key = Objs.copy(StringRef(Text));
  }
  for (const UnitDim &P : Parts)
    D->Names += static_cast<uint64_t>(P.Exp < 0 ? -int64_t(P.Exp) : P.Exp);
  UnitDisplays.try_emplace(Key, D);
  return D;
}

const UnitDisplay *Package::unitName(const UnitCanon *C) const {
  auto It =
      std::lower_bound(UnitNames.begin(), UnitNames.end(), C,
                       [](const UnitNameEntry &E, const UnitCanon *X) {
                         return std::less<const UnitCanon *>()(E.Canon, X);
                       });
  return It != UnitNames.end() && It->Canon == C ? It->Disp : nullptr;
}

BasicType *TypeContext::unitType(BasicKind BK, const UnitCanon *U) {
  // An empty dimension is no unit (except as a unit argument).
  if (!U || (U->dimensionless() && BK != BasicKind::UnitArg))
    return typ(BK);
  std::lock_guard<std::mutex> G(UnitMu);
  auto Key = std::make_pair(static_cast<unsigned>(BK), U);
  auto It = UnitTypes.find(Key);
  if (It != UnitTypes.end())
    return It->second;
  UnitsSeen.store(true, std::memory_order_relaxed);
  BasicType *Base = typ(BK);
  BasicType *T;
  {
    std::lock_guard<std::mutex> G2(AllocMu);
    T = Objs.make<BasicType>(BK, Base->Info, Base->Name);
  }
  T->Unit = U;
  UnitTypes.try_emplace(Key, T);
  return T;
}

Type *TypeContext::unitArgType(const UnitCanon *U) {
  if (U->Dims.size() == 1 && U->Dims[0].Param && U->Dims[0].Exp == 1 &&
      U->Factor.isRat() && U->Factor.ratNum().isOne() &&
      U->Factor.ratDen().isOne())
    return const_cast<TypeParamType *>(U->Dims[0].Param);
  return unitType(BasicKind::UnitArg, U);
}

//===----------------------------------------------------------------------===//
// Arithmetic
//===----------------------------------------------------------------------===//

namespace gopp::sema::units {

bool isOne(const Real &R) {
  return R.isRat() && R.ratNum().isOne() && R.ratDen().isOne();
}

std::string realStr(const Real &R) {
  if (!R.isRat())
    return R.str();
  llvm::SmallString<64> N, D;
  R.ratNum().toString(N, 10, true);
  std::string S(N.begin(), N.end());
  if (!R.ratDen().isOne()) {
    R.ratDen().toString(D, 10, true);
    S += '/';
    S.append(D.begin(), D.end());
  }
  return S;
}

const UnitCanon *one(TypeContext &Ctx) { return Ctx.unitCanon({}, realOne()); }

const UnitCanon *base(TypeContext &Ctx, const Object *Unit,
                      const TypeParamType *Param) {
  UnitDim D;
  D.Unit = Unit;
  D.Param = Param;
  D.Exp = 1;
  return Ctx.unitCanon(ArrayRef<UnitDim>(&D, 1), realOne());
}

const UnitCanon *make(TypeContext &Ctx, SmallVectorImpl<UnitDim> &Dims,
                      const Real &Factor, UnitStatus &St) {
  St = UnitStatus::Ok;
  if (!Factor.isRat() || Factor.isZero()) {
    St = UnitStatus::FactorRange;
    return nullptr;
  }
  // Sort by key (computed once per entry), merge equal bases.
  SmallVector<std::pair<std::string, size_t>, 8> Keys;
  Keys.reserve(Dims.size());
  for (size_t I = 0; I < Dims.size(); ++I)
    Keys.push_back({dimKey(Dims[I]), I});
  std::stable_sort(Keys.begin(), Keys.end(), [&](const auto &A, const auto &B) {
    if (A.first != B.first)
      return A.first < B.first;
    const UnitDim &X = Dims[A.second], &Y = Dims[B.second];
    return std::less<const void *>()(
        X.Unit ? (const void *)X.Unit : (const void *)X.Param,
        Y.Unit ? (const void *)Y.Unit : (const void *)Y.Param);
  });
  SmallVector<UnitDim, 8> Out;
  SmallVector<int64_t, 8> Sum;
  for (auto &K : Keys) {
    const UnitDim &D = Dims[K.second];
    if (!Out.empty() && sameBase(Out.back(), D)) {
      Sum.back() += D.Exp;
    } else {
      Out.push_back(D);
      Sum.push_back(D.Exp);
    }
  }
  SmallVector<UnitDim, 8> Final;
  for (size_t I = 0; I < Out.size(); ++I) {
    if (Sum[I] == 0)
      continue;
    if (Sum[I] > MaxExp || Sum[I] < -MaxExp) {
      St = UnitStatus::ExpRange;
      return nullptr;
    }
    UnitDim D = Out[I];
    D.Exp = static_cast<int32_t>(Sum[I]);
    Final.push_back(D);
  }
  return Ctx.unitCanon(Final, Factor);
}

bool realPow(const Real &X, int64_t N, Real &Out) {
  bool Neg = N < 0;
  uint64_t E = Neg ? static_cast<uint64_t>(-N) : static_cast<uint64_t>(N);
  Real R = realOne(), B = X;
  while (E) {
    if (E & 1) {
      R = Real::mul(R, B);
      if (!R.isRat())
        return false;
    }
    E >>= 1;
    if (E) {
      B = Real::mul(B, B);
      if (!B.isRat())
        return false;
    }
  }
  if (Neg) {
    if (R.isZero())
      return false;
    R = Real::quo(realOne(), R);
    if (!R.isRat())
      return false;
  }
  Out = R;
  return true;
}

const UnitCanon *mul(TypeContext &Ctx, const UnitCanon *A, const UnitCanon *B,
                     UnitStatus &St) {
  SmallVector<UnitDim, 8> D(A->Dims.begin(), A->Dims.end());
  D.append(B->Dims.begin(), B->Dims.end());
  Real F = Real::mul(A->Factor, B->Factor);
  return make(Ctx, D, F, St);
}

const UnitCanon *div(TypeContext &Ctx, const UnitCanon *A, const UnitCanon *B,
                     UnitStatus &St) {
  SmallVector<UnitDim, 8> D(A->Dims.begin(), A->Dims.end());
  for (UnitDim X : B->Dims) {
    X.Exp = -X.Exp;
    D.push_back(X);
  }
  Real F = Real::quo(A->Factor, B->Factor);
  return make(Ctx, D, F, St);
}

const UnitCanon *pow(TypeContext &Ctx, const UnitCanon *A, int64_t N,
                     UnitStatus &St) {
  St = UnitStatus::Ok;
  if (N == 0)
    return one(Ctx);
  if (N > MaxExp || N < -MaxExp) {
    St = UnitStatus::ExpRange;
    return nullptr;
  }
  Real F;
  if (!realPow(A->Factor, N, F)) {
    St = UnitStatus::FactorRange;
    return nullptr;
  }
  SmallVector<UnitDim, 8> D;
  for (UnitDim X : A->Dims) {
    int64_t E = static_cast<int64_t>(X.Exp) * N;
    if (E > MaxExp || E < -MaxExp) {
      St = UnitStatus::ExpRange;
      return nullptr;
    }
    X.Exp = static_cast<int32_t>(E);
    D.push_back(X);
  }
  return make(Ctx, D, F, St);
}

const UnitCanon *scaled(TypeContext &Ctx, const UnitCanon *A,
                        const Real &Factor, UnitStatus &St) {
  SmallVector<UnitDim, 8> D(A->Dims.begin(), A->Dims.end());
  return make(Ctx, D, Real::mul(A->Factor, Factor), St);
}

bool sameDim(const UnitCanon *A, const UnitCanon *B) {
  if (A == B)
    return true;
  if (A->Dims.size() != B->Dims.size())
    return false;
  for (size_t I = 0; I < A->Dims.size(); ++I)
    if (!sameBase(A->Dims[I], B->Dims[I]) || A->Dims[I].Exp != B->Dims[I].Exp)
      return false;
  return true;
}

Real ratio(const UnitCanon *A, const UnitCanon *B) {
  return Real::quo(A->Factor, B->Factor);
}

//===----------------------------------------------------------------------===//
// Text
//===----------------------------------------------------------------------===//

std::string dimName(const UnitDim &D, const Package *Qual) {
  if (D.Param)
    return D.Param->Obj ? D.Param->Obj->Name.str() : std::string("?");
  std::string N = D.Unit->Name.str();
  if (D.Unit->Pkg && D.Unit->Pkg != Qual && !D.Unit->Pkg->Name.empty())
    return D.Unit->Pkg->Name.str() + "." + N;
  return N;
}

/// `KG*M/S^2` (UNI-12): the positive part, then the negative; a quotient with
/// more than one negative name has them in parentheses. Name gives the text
/// of one part.
static std::string
partsStr(ArrayRef<UnitDim> Dims,
         llvm::function_ref<std::string(const UnitDim &)> Name) {
  std::string Pos, Neg;
  unsigned NegCount = 0;
  auto Term = [&](const UnitDim &D, int64_t E) {
    std::string T = Name(D);
    if (E != 1)
      T += "^" + std::to_string(E);
    return T;
  };
  for (const UnitDim &D : Dims) {
    if (D.Exp > 0) {
      if (!Pos.empty())
        Pos += '*';
      Pos += Term(D, D.Exp);
    }
  }
  for (const UnitDim &D : Dims) {
    if (D.Exp < 0) {
      if (!Neg.empty())
        Neg += '*';
      Neg += Term(D, -int64_t(D.Exp));
      ++NegCount;
    }
  }
  if (Neg.empty())
    return Pos;
  if (NegCount > 1)
    Neg = "(" + Neg + ")";
  return (Pos.empty() ? std::string("1") : Pos) + "/" + Neg;
}

static std::string dimsStr(ArrayRef<UnitDim> Dims, const Package *Qual) {
  return partsStr(Dims, [&](const UnitDim &D) { return dimName(D, Qual); });
}

/// The text of the fixed order: every name qualified by its package path.
std::string qualifiedText(ArrayRef<UnitDim> Parts) {
  return partsStr(Parts, [](const UnitDim &D) {
    if (D.Param)
      return D.Param->Obj ? D.Param->Obj->Name.str() : std::string("?");
    const Package *P = D.Unit->Pkg;
    return (P ? P->Path.str() + "." : std::string()) + D.Unit->Name.str();
  });
}

std::string canonStr(const UnitCanon *U, const Package *Qual) {
  std::string S = dimsStr(U->Dims, Qual);
  if (isOne(U->Factor))
    return S.empty() ? "1" : S;
  std::string F = realStr(U->Factor);
  if (F.find('/') != std::string::npos)
    F = "(" + F + ")";
  return S.empty() ? F : F + " " + S;
}

const UnitDisplay *display(TypeContext &Ctx, SmallVectorImpl<UnitDim> &Parts,
                           UnitStatus &St) {
  St = UnitStatus::Ok;
  // The same order and merging as the dimensions of a canonical form.
  SmallVector<std::pair<std::string, size_t>, 8> Keys;
  Keys.reserve(Parts.size());
  for (size_t I = 0; I < Parts.size(); ++I)
    Keys.push_back({dimKey(Parts[I]), I});
  std::stable_sort(Keys.begin(), Keys.end(), [&](const auto &A, const auto &B) {
    if (A.first != B.first)
      return A.first < B.first;
    const UnitDim &X = Parts[A.second], &Y = Parts[B.second];
    return std::less<const void *>()(
        X.Unit ? (const void *)X.Unit : (const void *)X.Param,
        Y.Unit ? (const void *)Y.Unit : (const void *)Y.Param);
  });
  SmallVector<UnitDim, 8> Out;
  SmallVector<int64_t, 8> Sum;
  for (auto &K : Keys) {
    const UnitDim &D = Parts[K.second];
    if (!Out.empty() && sameBase(Out.back(), D)) {
      Sum.back() += D.Exp;
    } else {
      Out.push_back(D);
      Sum.push_back(D.Exp);
    }
  }
  SmallVector<UnitDim, 8> Final;
  for (size_t I = 0; I < Out.size(); ++I) {
    if (Sum[I] == 0)
      continue;
    if (Sum[I] > MaxExp || Sum[I] < -MaxExp) {
      St = UnitStatus::ExpRange;
      return nullptr;
    }
    UnitDim D = Out[I];
    D.Exp = static_cast<int32_t>(Sum[I]);
    Final.push_back(D);
  }
  return Ctx.unitDisplay(Final);
}

const UnitDisplay *displayMul(TypeContext &Ctx, const UnitDisplay *A,
                              const UnitDisplay *B, int Sign) {
  if (!A || !B)
    return nullptr;
  SmallVector<UnitDim, 8> P(A->Parts.begin(), A->Parts.end());
  for (UnitDim D : B->Parts) {
    D.Exp = Sign < 0 ? -D.Exp : D.Exp;
    P.push_back(D);
  }
  UnitStatus St;
  return display(Ctx, P, St);
}

const UnitDisplay *baseDisplay(TypeContext &Ctx, const UnitCanon *U) {
  if (!U || U->Dims.empty() || !isOne(U->Factor))
    return nullptr;
  return Ctx.unitDisplay(U->Dims);
}

std::string displayStr(const UnitDisplay *D, const Package *Qual) {
  return dimsStr(D->Parts, Qual);
}

const UnitCanon *displayCanon(TypeContext &Ctx, const UnitDisplay *D) {
  if (!D)
    return nullptr;
  if (const UnitCanon *C = D->Canon.load(std::memory_order_acquire))
    return C;
  // All the parts at once, normalized and interned once (one product per
  // part would rebuild the whole unit each time: cubic for a long product).
  SmallVector<UnitDim, 16> Dims;
  Real Factor = realOne();
  for (const UnitDim &P : D->Parts) {
    const UnitCanon *B;
    if (P.Param) {
      B = base(Ctx, nullptr, P.Param);
    } else {
      auto *UO = llvm::dyn_cast<UnitObj>(P.Unit);
      if (!UO || !UO->Canon)
        return nullptr;
      B = UO->Canon;
    }
    for (UnitDim X : B->Dims) {
      int64_t E = int64_t(X.Exp) * P.Exp;
      if (E > MaxExp || E < -MaxExp)
        return nullptr;
      X.Exp = static_cast<int32_t>(E);
      Dims.push_back(X);
    }
    Real F;
    if (!realPow(B->Factor, P.Exp, F))
      return nullptr;
    Factor = Real::mul(Factor, F);
    if (!Factor.isRat())
      return nullptr;
  }
  UnitStatus St;
  const UnitCanon *Acc = make(Ctx, Dims, Factor, St);
  if (!Acc)
    return nullptr;
  D->Canon.store(Acc, std::memory_order_release);
  return Acc;
}

void Namer::offer(const UnitCanon *C, const UnitDisplay *D) {
  if (!C || !D || C->dimensionless())
    return;
  auto [It, Ins] = Best.try_emplace(C, D);
  if (!Ins && It->second != D && UnitDisplay::before(D, It->second))
    It->second = D;
}

void Namer::addUnit(TypeContext &Ctx, const UnitObj *U) {
  if (!U->Canon)
    return;
  UnitDim P;
  P.Unit = U;
  P.Exp = 1;
  offer(U->Canon, Ctx.unitDisplay(ArrayRef<UnitDim>(&P, 1)));
}

ArrayRef<UnitNameEntry> Namer::freeze(Arena &Mem) const {
  SmallVector<UnitNameEntry, 16> Out;
  Out.reserve(Best.size());
  for (auto &KV : Best)
    Out.push_back({KV.first, KV.second});
  std::sort(Out.begin(), Out.end(),
            [](const UnitNameEntry &A, const UnitNameEntry &B) {
              return std::less<const UnitCanon *>()(A.Canon, B.Canon);
            });
  return Mem.copy(ArrayRef<UnitNameEntry>(Out));
}

const UnitDisplay *nameOf(const UnitCanon *U, const Package *Qual,
                          const Namer *N) {
  if (!U)
    return nullptr;
  if (N)
    return N->find(U);
  return Qual ? Qual->unitName(U) : nullptr;
}

std::string str(const UnitCanon *U, const Package *Qual, const Namer *N) {
  if (const UnitDisplay *D = nameOf(U, Qual, N))
    return displayStr(D, Qual);
  return canonStr(U, Qual);
}

const UnitCanon *
substitute(TypeContext &Ctx, const UnitCanon *A,
           llvm::function_ref<const UnitCanon *(const TypeParamType *)> Rep,
           UnitStatus &St) {
  St = UnitStatus::Ok;
  if (!A->HasParam)
    return A;
  llvm::SmallVector<UnitDim, 8> Keep;
  const UnitCanon *Acc = nullptr;
  bool Any = false;
  for (const UnitDim &D : A->Dims) {
    const UnitCanon *R = D.Param ? Rep(D.Param) : nullptr;
    if (!R) {
      Keep.push_back(D);
      continue;
    }
    Any = true;
    const UnitCanon *P = pow(Ctx, R, D.Exp, St);
    if (!P)
      return nullptr;
    Acc = Acc ? mul(Ctx, Acc, P, St) : P;
    if (!Acc)
      return nullptr;
  }
  if (!Any)
    return A;
  const UnitCanon *Rest = make(Ctx, Keep, A->Factor, St);
  if (!Rest)
    return nullptr;
  return mul(Ctx, Rest, Acc, St);
}

} // namespace gopp::sema::units
