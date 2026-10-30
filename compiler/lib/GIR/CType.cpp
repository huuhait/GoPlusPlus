//===- CType.cpp - Which GIR types cross the C ABI ------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/CType.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::gir;

static CVerdict invalid(std::string Why) {
  CVerdict V;
  V.K = CVerdict::Invalid;
  V.Why = std::move(Why);
  return V;
}

/// Sizes saturate here, far above MaxCTypeSize, so nothing overflows.
constexpr uint64_t SizeCap = MaxCTypeSize * 4;
constexpr uint64_t FieldCap = MaxCTypeFields + 1;

static uint64_t satMul(uint64_t A, uint64_t B) {
  if (A != 0 && B > SizeCap / A)
    return SizeCap;
  return std::min(A * B, SizeCap);
}

static uint64_t satFields(uint64_t A, uint64_t B) {
  return std::min(A + B, FieldCap); // both are at most FieldCap
}

static uint64_t alignUp(uint64_t V, uint64_t A) { return (V + A - 1) / A * A; }

/// Tuple{f, f}: how a complex number is lowered (GIR has no complex type).
static bool isFloatPair(const Type *T) {
  return T->getKind() == Type::Kind::Tuple && T->elems().size() == 2 &&
         T->elems()[0]->isFloat() && T->elems()[0] == T->elems()[1];
}

CVerdict CTypeRules::check(const Type *T, CUse Use) {
  if (Use == CUse::Result && isFloatPair(T))
    // The same GIR type as a complex number (the front end tells them apart
    // before it asks: Lowering::declareFunction).
    return invalid("a C function has one result (return a struct), and "
                   "complex numbers have no C mapping yet (P07)");
  if (T->isUnit()) {
    if (Use == CUse::Result)
      return CVerdict();
    return invalid("the empty tuple is not a C type");
  }
  if (T->getKind() == Type::Kind::Array && Use != CUse::Field)
    return invalid(
        "an array is not passed by value in C (wrap it in a struct)");
  Info I = info(T, 0);
  if (I.TooDeep || I.Height > MaxCTypeDepth)
    return invalid("a C type nested more than " +
                   std::to_string(MaxCTypeDepth) + " levels deep");
  if (!I.V.ok())
    return I.V;
  if (I.Fields > MaxCTypeFields)
    return invalid("a C type of more than " + std::to_string(MaxCTypeFields) +
                   " fields when its structs are written out");
  if (Use != CUse::Field && I.L.Size > MaxCTypeSize)
    return invalid("type " + T->str() + " is larger than 1 GiB");
  return CVerdict();
}

CTypeRules::Layout CTypeRules::layout(const Type *T) {
  auto It = Memo.find(T);
  return It == Memo.end() || !It->second.V.ok() ? Layout() : It->second.L;
}

CTypeRules::Info CTypeRules::info(const Type *T, unsigned Depth) {
  if (Work)
    ++*Work;
  if (auto It = Memo.find(T); It != Memo.end())
    return It->second;
  Info R;
  if (Depth > MaxCTypeDepth) {
    // Deeper than any C type may be: the asked type is refused whatever is
    // below (check), so the walk stops here and nothing on its path is
    // memoized (the cut depends on where the walk started).
    R.TooDeep = true;
    return R;
  }
  switch (T->getKind()) {
  case Type::Kind::Bool:
    R.L = {1, 1};
    break;
  case Type::Kind::Int:
  case Type::Kind::Float:
    R.L = {T->bits() / 8, T->bits() / 8};
    break;
  case Type::Kind::Ptr:
    R.L = {8, 8};
    // A map is a pointer to the runtime's object (P04.05): it has no C
    // layout, as the front end says on the checker's types (E0442). A
    // pointer to a map variable is a plain pointer.
    if (T->elem()->getKind() == Type::Kind::Struct &&
        T->elem()->name() == "gopp.map")
      R.V = invalid("a map has no C layout");
    break;
  case Type::Kind::Unit:
    R.V = invalid("the empty tuple is not a C type");
    break;
  case Type::Kind::String:
    R.V = invalid("a string has no C layout (pass a *byte and a length)");
    break;
  case Type::Kind::Slice:
    R.V = invalid("a slice has no C layout (pass a *T and a length)");
    break;
  case Type::Kind::Tuple:
    // A complex number is lowered to Tuple{f, f}: as a parameter or a field
    // it can only be one, so it must not get the text about results.
    if (isFloatPair(T))
      R.V = invalid("complex numbers have no C mapping yet (P07)");
    else
      R.V = invalid("a C function has one result (return a struct)");
    break;
  case Type::Kind::Func:
    R.V.K = CVerdict::Unsupported;
    R.V.Why = "a func value passed to C (P04.18)";
    break;
  case Type::Kind::Array: {
    Info E = info(T->elem(), Depth + 1);
    if (E.TooDeep)
      return E;
    R.V = E.V;
    if (R.V.ok() && E.L.Size == 0)
      // Clang's x86-64 classification divides by the element size (and Clang
      // itself crashes on the C declaration): refused before it is asked.
      R.V = invalid("an array of zero-size elements, " + T->str() +
                    ", is not supported");
    R.L = {satMul(E.L.Size, T->length()), E.L.Align};
    R.Height = E.Height + 1;
    R.Fields = satFields(1, E.Fields);
    break;
  }
  case Type::Kind::Option:
    // Option[*T] is a plain pointer in GIR (Kind::Ptr), so only the by-value
    // {T, flag} reaches here. Its layout is Go++'s (D4.5), not a C ABI promise.
    R.V = invalid("Option[T] of a value has no C type until P07 defines how "
                  "Go++-only types map to C (Option[*T] is a C pointer)");
    break;
  case Type::Kind::Struct: {
    uint64_t Off = 0, A = 1;
    for (const Type *F : T->elems()) {
      Info E = info(F, Depth + 1);
      if (E.TooDeep)
        return E;
      if (R.V.ok())
        R.V = E.V; // the first field that is not a C type says why
      Off = std::min(alignUp(Off, E.L.Align) + E.L.Size, SizeCap);
      A = std::max(A, E.L.Align);
      R.Height = std::max(R.Height, E.Height + 1);
      R.Fields = satFields(R.Fields, E.Fields);
    }
    R.L = {alignUp(Off, A), A};
    break;
  }
  }
  Memo[T] = R;
  return R;
}
