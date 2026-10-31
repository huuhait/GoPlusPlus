//===- gopp/GIR/DropElab.h - Drop elaboration -------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.09 (gir.md section 7). The builder emits no drops: it marks which
// locals own their value (`Local::Owned`, with the drop glue to call), which
// stores replace a live owner (`Statement::DropOld`) and which reads take a
// value out of a place (`Move` operands). This pass is the ONE place that
// knows which owners are live at each point of a function and in what order
// they die, and it turns those marks into `Drop` terminators:
//
//   StorageDead(L) of an owned local        Drop(L) before it
//   Assign with DropOld                     Drop(Dest) before it
//   Return                                  Drop of each owned parameter
//   a Move operand                          the operand becomes a Copy and the
//                                           place is zeroed after the
//                                           statement (before the call, for a
//                                           call's argument)
//   the function's entry                    every owned local that is not a
//                                           parameter is set to zero bytes
//
// An owned local always holds a valid value or all-zero bytes, and a drop
// glue (or a Move) leaves all-zero bytes, so a drop is always right and never
// needs a flag: the glue of a zero value does nothing (OWN-R5). No dataflow
// over the values is needed for that; the dataflow here is over *which owners
// exist*, which the builder's scopes define.
//
// The seam for P04.26 (ADR-0028's cleanup pads). OwnerStates computes, for the
// start of every block, the set of owned locals between their StorageLive and
// StorageDead (a hash-consed persistent list, so two points with the same
// owners have the same StateId). The pass walks every block with the state of
// each point in hand, and every terminator that can unwind passes through
// `Elaborator::unwindFor(State)`. Today that returns the terminator's own
// action (no cleanup: a panic does not drop). P04.26 builds the pad of a
// state there: `OwnerStates::members(State)` lists the owners in drop order
// (last made first), a pad is one call to the glue of each, and a function
// that has `defer` makes ADR-0028's single state-indexed pad from the same
// StateIds. Nothing else in the pass changes.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_DROPELAB_H
#define GOPP_GIR_DROPELAB_H

#include "gopp/GIR/GIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gopp {
namespace gir {

using OwnerState = uint32_t;

/// The sets of live owners of one function: persistent lists of local ids in
/// increasing order, hash-consed (State 0 is the empty set). Adding or
/// removing an owner that is the newest costs O(1); anywhere else it costs
/// the owners made after it (a statement's temporary dies below the variable
/// the statement declared).
class OwnerStates {
public:
  static constexpr OwnerState Empty = 0;
  OwnerStates() { Nodes.push_back({Empty, NoId, 0}); }
  OwnerState add(OwnerState S, LocalId L);
  OwnerState remove(OwnerState S, LocalId L);
  /// The union. When one is the other with owners added on top (a branch
  /// that moved or made more, the common join), it is the longer one, found
  /// in O(owners added) (P04.09 R9); else the two lists are merged.
  OwnerState unite(OwnerState A, OwnerState B);
  /// Test-only: every union merges the two lists (the cost before R9), the
  /// quadratic mode of the stress tests' control.
  bool FullJoins = false;
  bool contains(OwnerState S, LocalId L) const;
  /// The owners of S, the one made last first: the order they are dropped in.
  void members(OwnerState S, llvm::SmallVectorImpl<LocalId> &Out) const;
  size_t size(OwnerState S) const { return Nodes[S].Depth; }
  size_t numStates() const { return Nodes.size(); }
  /// Is any owner of S in [Lo, Hi)? Costs the owners of S from Hi down.
  bool anyInRange(OwnerState S, LocalId Lo, LocalId Hi) const;
  /// The owners of S in [Lo, Hi), the biggest first.
  void rangeMembers(OwnerState S, LocalId Lo, LocalId Hi,
                    llvm::SmallVectorImpl<LocalId> &Out) const;

private:
  struct Node {
    OwnerState Parent;
    LocalId L;
    uint32_t Depth;
  };
  std::vector<Node> Nodes;
  llvm::DenseMap<uint64_t, OwnerState> Index;
  OwnerState cons(OwnerState Parent, LocalId L);
};

/// The live owners at the start of each block of F (the dataflow described
/// above; the join of two paths is the union, an owner that is not live on one
/// of them is zero there, so dropping it is harmless). `Out[B]` is the state
/// at B's entry; a block no edge reaches has Empty.
std::vector<OwnerState> computeOwnerStates(const Function &F,
                                           OwnerStates &States);

/// Elaborates the drops of F (the table above). Returns false with Error set
/// on an internal inconsistency: an owner still live at a `Return`, or an
/// owned local that dies without being live.
/// Rescan (test-only): the owner states are computed again for every block,
/// a quadratic mode of the pass that the stress tests' control must catch.
bool elaborateDrops(Module &M, Function &F, std::string *Error = nullptr,
                    bool Rescan = false);
/// Every function of M with a body; stops at the first error.
bool elaborateDrops(Module &M, std::string *Error = nullptr);

/// Test-only: the units (blocks, statements, owner-state changes) this
/// thread's elaborations have visited so far.
uint64_t dropElabSteps();

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_DROPELAB_H
