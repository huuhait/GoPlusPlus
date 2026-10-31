//===- gopp/GIR/UnitRuntime.h - Exact unit factors at run time --*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// GEN-14, ADR-0025 Amendment (P04.24): generic code that is shared across unit
// arguments computes the factor of a unit at run time, from one descriptor per
// unit parameter, exactly as the checker computes it for plain code: an exact
// rational whose numerator and denominator have at most 4096 bits (UNI-2,
// Real::MaxRatBits). The functions are written in GIR (RuntimeUnits.cpp), from
// the module alone, so the lowering and a unit test use the same code.
//
// A *number* is an array of 64-bit limbs, the least significant first, and a
// length; the length 0 is the number 0, otherwise the top limb is not 0.
//
// A *fraction* in memory (the factor of a unit, or of a collapse site):
//
//   +0   nn   u64   limbs of the numerator, 1 to UnitMaxLimbs; 0 marks a
//                   factor out of range (past 4096 bits): no more fields
//   +8   nd   u64   limbs of the denominator, 1 to UnitMaxLimbs
//   +16  the numerator's nn limbs, then the denominator's nd limbs
//
// always reduced, both parts at least 1. In a frame slot a fraction wider than
// the slot's room is *spilled*: {~0, pointer to the fraction in a heap block}
// (unit_frac resolves it). A *descriptor* (a unit at run time):
//
//   +0   count  u64   the base units with an exponent; ~0 marks an exponent
//                     out of range (past 2^20): no more fields
//   +8   pairs  { id u64, exp i64 } x count, sorted by id
//   +8 + 16 * count   the factor, a fraction
//
// A *recipe* (a constant of the module) says how a unit of generic code is
// made from the descriptors of the function's unit parameters, `dicts` (an
// array of their pointers, by the index of the parameter):
//
//   { np, (dict index, exp) x np, the base descriptor }   the base times each
//                                                         parameter^exp
//
// and a *site group* the factor of an expression's collapse sites:
//
//   { the static factor (a fraction), ns, (kind, recipe res, recipe x,
//     recipe y) x ns }
//
// A site's units are made inside the helpers, in their own frames; a frame
// slot holds only a descriptor that is passed to a callee (unit_make), so a
// recursive generic costs the frame of plain code.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_UNITRUNTIME_H
#define GOPP_GIR_UNITRUNTIME_H

#include "gopp/GIR/GIR.h"

#include "llvm/ADT/STLFunctionalExtras.h"

#include <string>

namespace gopp {
namespace gir {

/// The most limbs of a numerator or a denominator: 4096 bits.
constexpr unsigned UnitMaxLimbs = 64;
/// The words of a fraction with room for the bound.
constexpr unsigned UnitFracWords = 2 + 2 * UnitMaxLimbs;
/// The limbs (nn + nd) of the fraction a frame slot holds in place; a wider
/// one is spilled.
constexpr unsigned UnitSmallLimbs = 4;
/// The count of a descriptor whose exponent left the range.
constexpr uint64_t UnitCountOut = ~uint64_t(0);

/// The functions, by what they do. Numbers are (pointer, length) pairs; a
/// `dst` never shares memory with an input unless the line says so.
struct UnitArith {
  // 64-bit helpers.
  FuncId Gcd64 = NoId;     // unit_gcd64(a, b) u64: Euclid (0 if both are 0)
  FuncId Mul128 = NoId;    // unit_mul128(a, b) (hi, lo)
  FuncId DivMod128 = NoId; // unit_divmod128(hi, lo, d) (q, rem), hi < d
  FuncId MulDiv = NoId;    // unit_muldiv(r, n, d) u64: r * n / d, r < d
  FuncId BitLen = NoId;    // unit_bitlen(x) u64: the bits of x >= 1
  FuncId Pow2 = NoId;      // unit_pow2(k) f64: 2^k, |k| <= 1074
  // Numbers.
  FuncId Bits = NoId;     // unit_bits(a, na) u64
  FuncId Cmp = NoId;      // unit_cmp(a, na, b, nb) i64: -1, 0 or 1
  FuncId Sub = NoId;      // unit_sub(a, na, b, nb) u64: a -= b in place, a >= b
  FuncId Shr = NoId;      // unit_shr(a, na, k) u64: a >>= k in place
  FuncId Shl = NoId;      // unit_shl(dst, a, na, k) u64
  FuncId Mul = NoId;      // unit_mul(dst, a, na, b, nb) u64
  FuncId Mod1 = NoId;     // unit_mod1(a, na, m) u64: a mod m, m > 0
  FuncId DivExact = NoId; // unit_divexact(dst, a, na, b, nb) u64: a / b, b | a
  FuncId Gcd = NoId;      // unit_gcd(dst, a, na, b, nb) u64: a, b > 0
  /// unit_divq(r, nr, y, ny, out) u64: q = r / y, r = r mod y in place when
  /// q < 2^64; out[0] = the new length of r, out[1] = 1 if q >= 2^64 (r is
  /// then not the remainder). y > 0.
  FuncId DivQ = NoId;
  // Fractions.
  /// unit_fracmul(dst, a, ainv, b, binv): dst = a^(+-1) * b^(+-1) (ainv, binv
  /// 1 for the inverse), reduced; marked if an input is or the result passes
  /// the bound. dst may be a or b.
  FuncId FracMul = NoId;
  /// unit_fracpow(dst, b, e): dst = b^e, e != 0 (i64); marked past the bound,
  /// also where only an intermediate power passes it (as the checker's
  /// realPow).
  FuncId FracPow = NoId;
  /// unit_ratio(f, p) f64: f rounded once to p significant bits (24 or 53;
  /// the range of float32 or float64, subnormals too, +inf past it), to
  /// nearest, ties to even: the value Real::toIEEE gives the same fraction.
  /// 0 for a marked fraction.
  FuncId Ratio = NoId;
  /// unit_intfactor(out, f, mode): the integer scaling by f as three words,
  /// out = {A, n, d, inverted}: trunc(x * f) = x * A + trunc(x * n / d)
  /// modulo 2^64 for every |x| < 2^64, with A = floor(f) mod 2^64 and n < d
  /// (n / d is the fraction part of f, or, when its denominator passes 64
  /// bits, its best lower approximation with a denominator below 2^64,
  /// which has the same floor for every such x). mode 0: f; 1: 1/f; 2: the
  /// one of f and 1/f that is at least 1 (inverted says which). A marked f
  /// gives d = 0.
  FuncId IntFactor = NoId;
  // Descriptors and sites.
  /// unit_frac(f) *u8: f, or the fraction it was spilled to.
  FuncId Frac = NoId;
  /// unit_make(dst, cap, small, recipe, dicts, spill): the descriptor of the
  /// recipe's unit in a frame slot with room for cap pairs and a fraction of
  /// small limbs (nn + nd); a wider one is spilled to a heap block linked into
  /// the chain at *spill. An exponent past +-2^20 or more than cap pairs
  /// marks the count, a factor past the bound marks the factor.
  FuncId Make = NoId;
  /// unit_spill_free(spill *u8): frees the chain whose head pointer spill
  /// points to and zeroes it (the drop glue of the function's owned spill
  /// local).
  FuncId SpillFree = NoId;
  /// unit_site_f(group, dicts, p) f64: the factor of a site group rounded once
  /// to p bits (unit_ratio); 0 marks a factor out of range of units or one
  /// that rounds to +inf or 0 in the float type (plain code's E0435).
  FuncId SiteF = NoId;
  /// unit_site_i(out, group, dicts, mode): the integer words of a site
  /// group's factor (unit_intfactor).
  FuncId SiteI = NoId;
};

/// Adds the functions to M, each named Name(<the name above>), with Link.
/// Alloc is `gopp.mem_resize(p *u8, size u64, zero bool) *u8` and Free
/// `gopp.mem_free(p *u8)` (the spill chain).
UnitArith addUnitArith(Module &M,
                       llvm::function_ref<std::string(llvm::StringRef)> Name,
                       Linkage Link, FuncId Alloc, FuncId Free);

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_UNITRUNTIME_H
