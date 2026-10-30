//===- gopp/GIR/Inline.h - Trivial inlining on GIR --------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.13, D4.2 step 6, D4.8. The one light GIR pass the `opt 0` pipeline runs
// (docs/design/pipelines.md): it inlines a direct call of a *trivial*
// function, so a small accessor or an operator method costs no call even
// where LLVM runs no inliner.
//
// A function is trivial when it is a leaf with one block: it is defined here,
// has no call, no assertion (so no bounds, nil, division or overflow check),
// no drop and no cleanup, takes no address (`AddrOf`), has no owned
// temporary (`Local::Owned`, a drop of P04.09), and has at most
// MaxStatements statements. Such a function cannot panic and cannot reach any
// other function, so inlining it changes nothing a program can observe: the
// order of evaluation, the panics and the output are those of the call. In
// particular it never makes a float product and a sum meet that were not
// fused before (FMA contraction depends on the GIR a value is *written* in,
// docs/design/pipelines.md section 4): the callee's result is copied into the
// call's destination, and a copy is not a product.
//
// The call's block ends in `goto` the call's return target, after the
// parameter copies, the callee's statements and the result copy. A call whose
// unwind action is `Cleanup` is left alone (its cleanup block would lose its
// only edge; dead-block removal comes with the other GIR passes).
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_INLINE_H
#define GOPP_GIR_INLINE_H

#include "gopp/GIR/GIR.h"

#include <cstdint>

namespace gopp {
namespace gir {

struct InlineOptions {
  /// The most statements a trivial function may have.
  unsigned MaxStatements = 12;
};

struct InlineStats {
  /// Functions that are trivial, calls inlined.
  uint64_t Trivial = 0, Inlined = 0;
};

/// Inlines the calls of trivial functions in every function of M. The module
/// stays valid (the verifier rules hold) and a second run finds nothing more
/// to do except calls that the first one made possible: there are none, a
/// trivial function has no call.
InlineStats inlineTrivial(Module &M, const InlineOptions &Opts = {});

/// Test-only (M8): the units (functions, blocks, statements copied) this
/// thread's inlineTrivial calls have visited so far.
uint64_t inlineSteps();

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_INLINE_H
