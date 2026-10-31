//===- gopp/GIR/MoveCheck.h - Use after move --------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.09 (OWN-R3, OWN-R4, gir.md section 7). The checker rejects the moves it
// can see in one expression; whether a value is still there at a later use
// depends on the paths between them (a loop, a branch that moves, a branch
// that does not), which only the control-flow graph knows. This pass runs on
// the GIR the builder made, before drop elaboration (the moves are still
// `Move` operands), and reports every read of a place that some path to it
// has moved from and not written since.
//
// The state at a point is the set of *move paths* that may be moved: a local
// followed by fields and constant indexes (`x`, `x.f`, `x[2]`). The paths
// that some `Move` names are a trie (one node per path); the nodes are
// numbered in preorder, so the paths below a node are a range of numbers, and
// "is anything under x moved" is a range question on the state. A state is a
// hash-consed persistent radix trie of path numbers (PathSets, in the .cpp),
// a join is the union (maybe-moved), and a local's paths are dropped from the
// state at its StorageLive and StorageDead, so the state holds the moved
// paths of the locals alive at that point and nothing older. A change or a
// join copies only the trie nodes where the sets differ (at most 6 levels of
// 64), so a join of a state with itself plus a path costs that path, in any
// order the paths were moved (review R9). Work: blocks times the paths moved
// or joined at a block, and a read costs the depth of its path plus the moved
// paths under it.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_MOVECHECK_H
#define GOPP_GIR_MOVECHECK_H

#include "gopp/GIR/GIR.h"

#include <cstdint>
#include <vector>

namespace gopp {
namespace gir {

struct MoveError {
  Span Use;      // the read after the move
  Span Moved;    // the move
  LocalId Local; // the variable
};

/// Every use after a move in F, in block order (one per use and variable).
/// Only named locals are reported (the builder's temporaries are not source).
/// Rescan (test-only): every read looks at every moved path, a quadratic
/// mode of the pass that the stress tests' control must catch. FullJoins
/// (test-only): every join walks both states whole (PathSets::FullJoins, the
/// cost of a list merge before P04.09 R9), another such mode.
std::vector<MoveError> checkMoves(const Function &F, bool Rescan = false,
                                  bool FullJoins = false);

/// Test-only: the units (blocks, statements, state changes) this thread's
/// checks have visited so far.
uint64_t moveCheckSteps();

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_MOVECHECK_H
