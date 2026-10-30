//===- Symbols.h - Naming what the lowering emits (D4.13, P04.17) ---------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The one place that turns a checked entity into a symbol: it asks
// sym::SemaNames for the entity's node (from its content, never from a
// pointer or a creation order) and sym::mangle writes it. gir::symbolOf
// (Lower.h) is the same decision for the entities that need no checker
// object. Used by Lower.cpp and Runtime.cpp only.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_LIB_GIR_SYMBOLS_H
#define GOPP_LIB_GIR_SYMBOLS_H

#include "gopp/Mangle/SemaSymbols.h"
#include "gopp/Mangle/SymbolName.h"

namespace gopp {
namespace gir {

class Symbols {
public:
  Symbols() : Names(Ctx, /*MainPathIsMain=*/true) {}

  sym::Context Ctx;
  sym::SemaNames Names;

  /// The symbol of a package-level function. Empty (and why()) if the
  /// checker's object has no name.
  std::string func(const sema::Func *F);
  std::string var(const sema::Var *V);
  /// The ordinal-th `init` function of the package.
  std::string initFunc(const sema::Package *P, uint64_t Ordinal);
  /// The package's init entry point (variables, then the `init` functions).
  std::string pkgInit(const sema::Package *P);
  /// EqGlue or HashGlue of T.
  std::string glue(sym::Kind K, const sema::Type *T);
  /// The node of the function F, for the scope of a local type.
  const sym::Node *funcNode(const sema::Func *F) { return Names.func(F); }

  const std::string &why() const { return Names.why(); }
};

} // namespace gir
} // namespace gopp

#endif // GOPP_LIB_GIR_SYMBOLS_H
