//===- WorkCount.h - Count the elements a loop visits ---------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Test-only work counters (manager decision M8). A near-linear test in the
// normal suite counts the work a walk does instead of reading a clock. The
// count is only worth anything if every loop that can grow with the input
// counts its elements, so a loop is written
//
//     for (const Operand &Op : counted(Steps, S.RV.Ops)) ...
//
// (or `++Steps` per iteration in an index loop): the walk's own loops are the
// counted places, not a few outer ones. Dereferencing the iterator counts one.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BASIC_WORKCOUNT_H
#define GOPP_BASIC_WORKCOUNT_H

#include <cstdint>
#include <iterator>
#include <utility>

namespace gopp {

template <class Range> class CountedRange {
public:
  CountedRange(uint64_t &N, Range &&R) : N(N), R(std::forward<Range>(R)) {}

  class iterator {
  public:
    using Inner = decltype(std::begin(std::declval<Range &>()));
    iterator(uint64_t *N, Inner I) : N(N), I(I) {}
    decltype(auto) operator*() const {
      ++*N;
      return *I;
    }
    iterator &operator++() {
      ++I;
      return *this;
    }
    bool operator!=(const iterator &O) const { return I != O.I; }

  private:
    uint64_t *N;
    Inner I;
  };

  iterator begin() { return {&N, std::begin(R)}; }
  iterator end() { return {&N, std::end(R)}; }

private:
  uint64_t &N;
  Range R; // a reference for an lvalue range, a copy for a temporary
};

/// Range-for over R, adding one to N per element visited.
template <class Range> CountedRange<Range> counted(uint64_t &N, Range &&R) {
  return CountedRange<Range>(N, std::forward<Range>(R));
}

} // namespace gopp

#endif // GOPP_BASIC_WORKCOUNT_H
