//===- gopp/GIR/CType.h - Which GIR types cross the C ABI -------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.11 (D4.4, docs/design/c-abi.md). The one place that decides whether a
// GIR type can be a parameter, a result or a field of a function with the C
// convention, so that the front end's error (E0442 / E0439,
// Lowering::declareFunction), the verifier (V067), the Go++ -> Clang type
// mapping (Import/CABI.cpp) and the tests agree.
//
//   - bool, integers, floats, pointers, `Option[*T]` (a pointer);
//   - a struct of C types;
//   - inside a struct, an array of a C type; never an array by value as a
//     parameter or result (C cannot pass one);
//   - the empty tuple as a result (void).
//
// Everything else is refused with a reason: strings, slices, tuples (several
// results), complex numbers and `Option[T]` by value (C has no such type; P07
// decides how Go++-only types map to C), an array of zero-size elements
// (Clang's x86-64 classification divides by the element size). A func value
// is a C type in one place only (P04.18): a parameter, where it is a C
// function pointer (a callback: a Func type of the C convention) whose own
// parameters and result are C types; as a result or inside a struct it needs
// P04.07's closures. Three limits keep
// every walk of a C type small, ours and Clang's (its classification recurses
// through the fields of every nested struct, without a memo):
//
//   MaxCTypeDepth   levels of structs and arrays;
//   MaxCTypeFields  fields when the structs are written out (a struct counts
//                   itself and its fields, an array its element once): a
//                   DAG of structs that doubles at each level is refused
//                   after a walk of its distinct types;
//   MaxCTypeSize    the size of a parameter or result.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_CTYPE_H
#define GOPP_GIR_CTYPE_H

#include "gopp/GIR/Type.h"

#include "llvm/ADT/DenseMap.h"

#include <cstdint>
#include <string>

namespace gopp {
namespace gir {

constexpr unsigned MaxCTypeDepth = 64;
constexpr uint64_t MaxCTypeFields = 4096;
constexpr uint64_t MaxCTypeSize = uint64_t(1) << 30;

/// Where a type is used.
enum class CUse { Param, Result, Field };

struct CVerdict {
  enum Kind : uint8_t {
    Ok,
    Invalid,    // never valid in C (E0442); Why says what
    Unsupported // valid later (E0439); Why names the task
  } K = Ok;
  std::string Why;
  bool ok() const { return K == Ok; }
};

/// Decides C-ness with a memo, so a type shared by many signatures or fields
/// is walked once. The memo holds only facts of a type itself (its verdict as
/// a field, layout, height, written-out field count), never where it was
/// reached, so no answer depends on the order the types are asked in. The
/// walk recurses at most MaxCTypeDepth + 1 levels below the asked type.
class CTypeRules {
public:
  /// The verdict on T used as Use. When several things are wrong, the depth
  /// is reported first (a walk that went too deep has not seen the rest),
  /// then the first part that is not a C type, then the field count, then
  /// the size.
  CVerdict check(const Type *T, CUse Use);

  /// Size and alignment in bytes of a type check() has walked (C layout,
  /// D4.5). Saturates above MaxCTypeSize; {0, 1} for a type that is not a C
  /// type.
  struct Layout {
    uint64_t Size = 0, Align = 1;
  };
  Layout layout(const Type *T);

  /// Test-only work counter (M8): one per type visited, through a field or
  /// an element, memo hits included.
  uint64_t *Work = nullptr;

private:
  struct Info {
    CVerdict V; // the type's own verdict as a field (no limits)
    Layout L;
    unsigned Height = 0;  // levels of structs and arrays (scalar: 0)
    uint64_t Fields = 1;  // written out, saturated above MaxCTypeFields
    bool TooDeep = false; // the walk was cut: never memoized
  };
  Info info(const Type *T, unsigned Depth);
  /// A callback: a parameter of Func type (P04.18), of the C convention. Its
  /// parameters and result are checked as a C function's; one callback is as
  /// deep as a signature.
  CVerdict checkCallback(const Type *T);
  llvm::DenseMap<const Type *, Info> Memo;
};

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_CTYPE_H
