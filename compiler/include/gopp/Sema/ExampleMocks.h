//===- gopp/Sema/ExampleMocks.h - The spec examples' headers ----*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// TEST ONLY. The C and C++ headers that docs/spec/examples import (06-zcrc:
// zlib.h, 07-vecsort: <vector> and <algorithm>), declared by hand in
// MockForeignScopes with exactly the names the examples use. The real
// importers (P07 for C, P08 for C++) replace this by filling the same
// ForeignScope interface from Clang; nothing in the checker depends on these
// mocks. The hidden `gopp check --test-stubs` installs them (for
// tests/sema/spec-examples-stubs.test); a normal `gopp check` never does.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_EXAMPLEMOCKS_H
#define GOPP_SEMA_EXAMPLEMOCKS_H

#include "gopp/Sema/Foreign.h"

#include <map>

namespace gopp {
namespace sema {

class ExampleHeaderMocks {
public:
  /// Adds the mock scopes to Prov. The object must outlive the check: it owns
  /// the declarations.
  ExampleHeaderMocks(TypeContext &Ctx, MockForeignProvider &Prov);

private:
  void zlib();
  void vector();
  void algorithm();
  NamedType *vectorOf(Type *Elem);

  TypeContext &Ctx;
  MockForeignProvider &Prov;
  Arena Mem;
  Package *VecPkg = nullptr, *AlgPkg = nullptr;
  MockForeignScope *Vec = nullptr, *Alg = nullptr;
  std::map<Type *, NamedType *> Instances;
  std::map<Type *, NamedType *> Iterators;
};

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_EXAMPLEMOCKS_H
