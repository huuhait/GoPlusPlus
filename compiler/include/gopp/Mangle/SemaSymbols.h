//===- gopp/Mangle/SemaSymbols.h - Checked entities -------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The bridge from the checker's types and objects (gopp/Sema/Types.h) to the
// symbol model (gopp/Mangle/SymbolName.h), which codegen (P04.02 and later)
// uses to name what it emits. The name is a function of the content of the
// type or object (package paths, names, type arguments, canonical units),
// never of a pointer, a creation order or a thread: two TypeContexts that
// check the same packages in any order, with any number of body threads, give
// the same names.
//
// Two types get one node exactly when the checker's identity says they are
// identical (Checker::identical): every part the identity compares is in the
// node (all the marks of a struct field, the flattened method set of an
// interface), and nothing it ignores is (the `!` of a fallible func type,
// ERR-2; the spellings `byte` and `rune`).
//
// Every walk is iterative (the types of the checker nest as deep as the
// source does). A type that cannot be part of a symbol gives null and a
// reason (why()): a type parameter (symbols name instances, not generic
// code), a type declared inside a function (the lowering knows its enclosing
// function, P04.04 and P04.07), a constraint interface, an untyped type.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_MANGLE_SEMASYMBOLS_H
#define GOPP_MANGLE_SEMASYMBOLS_H

#include "gopp/Mangle/SymbolName.h"
#include "gopp/Sema/Types.h"

#include "llvm/ADT/DenseMap.h"

#include <string>

namespace gopp {
namespace sym {

class SemaNames {
public:
  explicit SemaNames(Context &C) : C(C) {}

  /// The node of a type, or null (see why()).
  const Node *type(const sema::Type *T);
  /// A canonical unit as a Unit node (null for a unit with a type parameter).
  const Node *unit(const sema::UnitCanon *U);
  /// A package-level function, or a method (of a named type, or of an
  /// instance of a generic one). Null for generic code: a generic function
  /// or method is named per instance with funcInstance().
  const Node *func(const sema::Func *F);
  /// The method Decl (declared on the generic type or on a plain one) of the
  /// type Recv, which may be an instance of a generic type. An operator
  /// method is named by its declaration (its declared parameter types, with
  /// the receiver's type parameters by index, and `@Right`), so two overloads
  /// stay two names in every instance.
  const Node *method(const sema::NamedType *Recv, const sema::Func *Decl);
  /// The instance F[TArgs] of a generic function or method.
  const Node *funcInstance(const sema::Func *F,
                           llvm::ArrayRef<sema::Type *> TArgs);
  /// A package-level variable.
  const Node *var(const sema::Var *V);

  /// Why the last call that returned null did.
  const std::string &why() const { return Why; }

private:
  const Node *fail(std::string Reason) {
    Why = std::move(Reason);
    return nullptr;
  }
  const Node *funcImpl(const sema::Func *F, bool Generic);
  /// The parameter tuple of an operator method, as declared.
  const Node *opParams(const sema::Func *F);
  /// The node of T, or null with Why set. TParams: a type parameter is the
  /// TypeParam (a unit type parameter the ParamDim) of its index, for the
  /// declared parameters of an operator method; else it has no node.
  const Node *convert(const sema::Type *T, bool TParams);
  const Node *unitImpl(const sema::UnitCanon *U, bool TParams,
                       std::string &Reason);
  /// Children of T that must be converted first (false: T has no node).
  bool children(const sema::Type *T, bool TParams,
                llvm::SmallVectorImpl<const sema::Type *> &Out,
                std::string &Reason);
  const Node *build(const sema::Type *T, bool TParams, std::string &Reason);

  Context &C;
  std::string Why;
  /// Per mode (convert's TParams): the node of every type converted so far
  /// (null: it has none), and the reason of each null.
  llvm::DenseMap<const sema::Type *, const Node *> Memo[2];
  llvm::DenseMap<const sema::Type *, std::string> Reasons[2];
  /// The flattened, sorted method sets of interfaces under conversion.
  llvm::DenseMap<const sema::Type *, llvm::SmallVector<const sema::Func *, 4>>
      Methods;
};

} // namespace sym
} // namespace gopp

#endif // GOPP_MANGLE_SEMASYMBOLS_H
