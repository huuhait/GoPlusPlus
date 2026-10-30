//===- gopp/GIR/Glue.h - Equality and hash glue -----------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.04. Functions the compiler writes in GIR for a type, from the GIR type
// alone (so a hand-built module, a unit test and the lowering all use the
// same code):
//
//   eq(a *T, b *T) bool          Go's == on T
//   hash(p *T, seed u64) u64     a hash that agrees with eq: equal values hash
//                                equal
//
// The rules, one per type:
//   integer, bool, pointer   the value;
//   float                    ==: IEEE (NaN != NaN, -0 == +0); hash: +0 and -0
//                            are the same, every NaN is one fixed value;
//   struct, tuple (a complex number is a tuple of two floats), array
//                            field by field, element by element (a loop for
//                            an array of more than 4);
//   Option[T]                ==: both flags clear, or both set and the
//                            payloads equal. The flag byte's padding and,
//                            when the flag is clear, the payload do not take
//                            part in == or in the hash (ADR-0018 amendment
//                            point 1; P05 map keys use the hash). An Option of
//                            a pointer kind is the pointer (no Option type).
// A type with a string, a slice or a function pointer has no glue yet
// (P04.05, P04.06, P04.07): the builders return NoId.
//
// P04.06 (interfaces) and P05 (maps) call these; they are the one place that
// decides how a type is compared and hashed.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_GLUE_H
#define GOPP_GIR_GLUE_H

#include "gopp/GIR/GIR.h"

namespace gopp {
namespace gir {

/// Can eq and hash glue be made for T?
bool hasGlue(const Type *T);

/// Adds the glue function named Symbol to M (LinkOnceODR, so a second module
/// that makes the same glue folds with it), or returns the one that has the
/// name. NoId if T has no glue.
FuncId addEqGlue(Module &M, Type *T, llvm::StringRef Symbol);
FuncId addHashGlue(Module &M, Type *T, llvm::StringRef Symbol);

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_GLUE_H
