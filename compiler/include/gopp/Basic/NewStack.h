//===- gopp/Basic/NewStack.h - Deep recursion on stacks of their own
//-------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A walk over a chain of named types (`type T1 [1]T0; type T2 [1]T1; ...`)
// recurses once per link, and a chain can be as long as the source is (P04.04,
// review R6). Rather than a copy of each walk with an explicit stack, every
// such walk is a DeepWalk and, when the stack of the thread has less than a
// reserve left (the worst stack one step can use), runs the rest on a thread
// with a stack of its own while this thread waits. The order of the work, and
// so the answer, is that of plain recursion; the depth that fits is bounded by
// memory, not by one stack. Hopping by stack left (not by a count of levels)
// keeps the threads few for cheap links and gives a deep single step (an
// initializer at the parser limit) the whole reserve.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BASIC_NEWSTACK_H
#define GOPP_BASIC_NEWSTACK_H

#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>

namespace gopp {

/// Runs Fn on a thread with a stack of Bytes bytes (only the pages that are
/// touched use memory) and waits for it. False if no thread could be made (an
/// address-space or thread limit): Fn has not run, and the caller decides. At
/// depth 0, where nothing is deep yet, running Fn here is fine
/// (runOnNewStackOrHere); at a hop point inside a deep walk it is not (the
/// walk is deep because it would overflow the stack it is on), so the walk
/// stops with err_no_stack.
bool runOnNewStack(llvm::function_ref<void()> Fn, size_t Bytes);

/// runOnNewStack, and Fn right here when no thread could be made.
void runOnNewStackOrHere(llvm::function_ref<void()> Fn, size_t Bytes);

/// Can a thread be made now? True unless a test has asked for a failure
/// (failNewStacksAfter); every thread of this library and the checker's hops
/// ask first.
bool newStackAllowed();

/// Test hook: the next N threads may be made, then every request fails (a
/// negative N: never fail again). Returns the previous setting.
long failNewStacksAfter(long N);

/// Has this thread less than Reserve bytes of stack left? (The stack
/// pointer against the base of the thread's stack, found once per thread; a
/// thread whose stack is unknown never asks to hop.)
bool stackLow(size_t Reserve);

/// Threads made by runOnNewStack so far (for tests and measurements).
unsigned long newStackThreads();
void noteNewStackThread();

/// Test hook: with Levels != 0 a walk hops every Levels levels (and not by
/// stack left), so a smoke test of a few hundred levels hops many times.
/// Returns the previous setting (0: by stack left).
unsigned setNewStackSegment(unsigned Levels);
unsigned newStackSegment();

/// Sets the hook for a scope (a test).
struct ScopedNewStackSegment {
  unsigned Old;
  explicit ScopedNewStackSegment(unsigned Levels)
      : Old(setNewStackSegment(Levels)) {}
  ~ScopedNewStackSegment() { setNewStackSegment(Old); }
};

/// Should the walk at this depth (on this thread) hop now? The hook, else
/// the stack left against Reserve.
inline bool shouldHop(unsigned Depth, size_t Reserve) {
  if (unsigned H = newStackSegment())
    return Depth != 0 && Depth % H == 0;
  return stackLow(Reserve);
}

/// The depth of one recursion (one object per walk, on one thread at a time).
class DeepWalk {
public:
  /// A level of the walk: runs Fn, on a new stack when this one has less
  /// than Reserve left. False if the new stack could not be had; Fn has not
  /// run and failedAt() is the depth.
  template <class F> bool level(F &&Fn) {
    ++Depth;
    bool Ok = true;
    if (shouldHop(Depth, Reserve)) {
      Ok = runOnNewStack(Fn, SegmentBytes);
      if (!Ok && !Failed)
        Failed = Depth;
    } else {
      Fn();
    }
    --Depth;
    return Ok;
  }
  unsigned failedAt() const { return Failed; }

  /// The worst stack one step of these walks (lowerType, memTy, Type::print:
  /// one level plus what it calls) can use is 1.5 KB under ASan (97 KB for 64
  /// levels of the worst chain, measured); the reserve is 256 KB, 170 times
  /// that, and a segment is 2 MB (virtual: only touched pages cost).
  static constexpr size_t Reserve = size_t(256) << 10;
  static constexpr size_t SegmentBytes = size_t(2) << 20;

private:
  unsigned Depth = 0;
  unsigned Failed = 0;
};

} // namespace gopp

#endif // GOPP_BASIC_NEWSTACK_H
