//===- Units.h - Unit arithmetic (private) ----------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The arithmetic on canonical units (ADR-0025, UNI-5): product, quotient,
// power, the ratio of two scales, and the text of a unit. Every result is a
// canonical, interned UnitCanon of the TypeContext, so identity is a pointer
// compare and no result depends on the thread or the order that made it.
//
// Nothing here rounds or wraps: a factor that stops being an exact rational
// (it needs more than Real::MaxRatBits bits) or an exponent past MaxExp is a
// status the caller reports, never a silent wrong unit.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_UNITS_H
#define GOPP_SEMA_UNITS_H

#include "gopp/Sema/Types.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <string>

namespace gopp {
namespace sema {

enum class UnitStatus : uint8_t {
  Ok,
  ExpRange,    // an exponent past MaxExp
  FactorRange, // the factor is no longer an exact rational (too many bits)
};

/// GEN-14, UNI-2: what the unit types of a generic declaration (its fields
/// and methods, and those of the generic types it holds, by substitution of
/// the declarations, never of instances) can make of its unit parameters:
/// for each parameter the largest |exponent| it has in them, and the largest
/// |exponent| of a base unit and number of bits of the factor that they
/// write themselves. Saturated: no bound (a growing cycle). An instance whose
/// unit arguments keep every bound in range needs nothing else.
struct UnitRangeSum {
  bool Saturated = false;
  llvm::ArrayRef<int64_t> MaxExp; // by the index of the type parameter
  int64_t MaxBaseExp = 0;         // of a base unit written in the units
  int64_t MaxFactorBits = 0;      // of the factor written in the units
};

namespace units {

/// The largest exponent magnitude of a base unit.
constexpr int64_t MaxExp = 1 << 20;

/// The rational 1.
Real realOne();
/// The unit with no dimension and factor 1.
const UnitCanon *one(TypeContext &Ctx);
/// A declared base unit, or a unit type parameter, as a unit.
const UnitCanon *base(TypeContext &Ctx, const Object *Unit,
                      const TypeParamType *Param);
/// Normalizes Dims (merge, drop zeros, sort) and interns.
const UnitCanon *make(TypeContext &Ctx, llvm::SmallVectorImpl<UnitDim> &Dims,
                      const Real &Factor, UnitStatus &St);
const UnitCanon *mul(TypeContext &Ctx, const UnitCanon *A, const UnitCanon *B,
                     UnitStatus &St);
const UnitCanon *div(TypeContext &Ctx, const UnitCanon *A, const UnitCanon *B,
                     UnitStatus &St);
const UnitCanon *pow(TypeContext &Ctx, const UnitCanon *A, int64_t N,
                     UnitStatus &St);
/// A unit with the dimension of A and the factor Factor * A's.
const UnitCanon *scaled(TypeContext &Ctx, const UnitCanon *A,
                        const Real &Factor, UnitStatus &St);
/// A with each unit parameter P replaced by Rep(P) (a unit, or null to keep
/// the parameter). Null with St set if the result is out of range.
const UnitCanon *
substitute(TypeContext &Ctx, const UnitCanon *A,
           llvm::function_ref<const UnitCanon *(const TypeParamType *)> Rep,
           UnitStatus &St);
/// The same dimension (the exponents of every base unit are equal).
bool sameDim(const UnitCanon *A, const UnitCanon *B);
/// Factor(A) / Factor(B): the multiplier that turns a number of A into the
/// same amount in B. Exact (both are rationals).
Real ratio(const UnitCanon *A, const UnitCanon *B);
/// Exact value of x^N for a rational x (N may be negative). False on a
/// factor out of range.
bool realPow(const Real &X, int64_t N, Real &Out);
bool isOne(const Real &R);
/// Num/Den as text: `5/18`, `1000`.
std::string realStr(const Real &R);

/// The text of a unit for a message when no display form names it: `M/S`,
/// `KG*M/S^2`, `(5/18) M/S`, `1000 M`. Names of the package Qual are written
/// bare, others as `pkg.Name`.
std::string canonStr(const UnitCanon *U, const Package *Qual);
/// The name of one dimension or display part (a unit or a unit parameter).
std::string dimName(const UnitDim &D, const Package *Qual);

/// The display form made of Parts (any order: equal names are merged, zero
/// exponents dropped, the rest sorted); null if nothing is left, or if an
/// exponent leaves the range (St says so).
const UnitDisplay *display(TypeContext &Ctx,
                           llvm::SmallVectorImpl<UnitDim> &Parts,
                           UnitStatus &St);
/// The form of A * B^Sign (Sign is 1 or -1, for `*` and `/`); null if
/// either is null or nothing is left.
const UnitDisplay *displayMul(TypeContext &Ctx, const UnitDisplay *A,
                              const UnitDisplay *B, int Sign);
/// The form written with the base units of U, when that names U exactly (its
/// factor is 1); else null.
const UnitDisplay *baseDisplay(TypeContext &Ctx, const UnitCanon *U);
/// The text of a display form (UNI-12): positive exponents first, joined by
/// `*`, then `/` and the negative ones (more than one in parentheses), `1/S`
/// with no positive part, `^N` for an exponent other than 1. Names of the
/// package Qual are written bare, others as `pkg.Name`.
std::string displayStr(const UnitDisplay *D, const Package *Qual);
/// The unit that the display form D names (the product of its parts); null
/// if a part is a unit with an error. Memoized on D.
const UnitCanon *displayCanon(TypeContext &Ctx, const UnitDisplay *D);

/// The display forms of one package while it is checked (UNI-5): for each
/// canonical form, the first in the fixed order (fewest names, then the
/// smaller text) among the forms that the declarations make (phase 1: the
/// units, the unit types written in declarations, the unit expressions of
/// package-level initializers) and the tables of the imports. Only phase 1
/// writes it; a set and a fixed order, so it never depends on the order of
/// the threads, the packages or the offers. Frozen into Package::UnitNames
/// before the bodies are checked. A body checker keeps the forms of its own
/// body in a Namer of its own on top of it (one thread, source order).
struct Namer {
  llvm::DenseMap<const UnitCanon *, const UnitDisplay *> Best;
  /// A body checker's forms (phase 2) lie on top of the package's: Parent is
  /// the package table, read only while the bodies run.
  const Namer *Parent = nullptr;
  /// Keeps D for C if it comes first in the fixed order.
  void offer(const UnitCanon *C, const UnitDisplay *D);
  /// The unit U, by its own name.
  void addUnit(TypeContext &Ctx, const UnitObj *U);
  /// The first form of C in the fixed order, here or in Parent.
  const UnitDisplay *find(const UnitCanon *C) const {
    auto It = Best.find(C);
    const UnitDisplay *Own = It == Best.end() ? nullptr : It->second;
    const UnitDisplay *Up = Parent ? Parent->find(C) : nullptr;
    if (!Own || !Up)
      return Own ? Own : Up;
    return UnitDisplay::before(Up, Own) ? Up : Own;
  }
  /// The table as Package::UnitNames (sorted by canonical form).
  llvm::ArrayRef<UnitNameEntry> freeze(Arena &Mem) const;
};
/// The display form that the package Qual prints for U: from N (Qual's own
/// check, still running) or Qual's table; null if neither names U.
const UnitDisplay *nameOf(const UnitCanon *U, const Package *Qual,
                          const Namer *N);
/// The text of U for a message of the package Qual: its display form if it
/// has one (nameOf), else the canonical text.
std::string str(const UnitCanon *U, const Package *Qual, const Namer *N);

} // namespace units
} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_UNITS_H
