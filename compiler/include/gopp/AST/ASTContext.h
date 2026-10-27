//===- gopp/AST/ASTContext.h - AST arena ------------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One ASTContext per parsed file (D2.2): a bump allocator plus the table of
// interned identifiers. Nothing is freed individually; destroying the context
// frees the whole tree. Nodes must be trivially destructible. Literal text and
// token trees point into the SourceManager's buffers, so the SourceManager
// must outlive the context.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_AST_ASTCONTEXT_H
#define GOPP_AST_ASTCONTEXT_H

#include "gopp/AST/AST.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Allocator.h"

#include <new>
#include <type_traits>

namespace gopp {
namespace ast {

class ASTContext {
public:
  ASTContext() : Symbols(Alloc) {}
  ASTContext(const ASTContext &) = delete;
  ASTContext &operator=(const ASTContext &) = delete;

  /// Allocates a default-constructed node; the range is set by the caller.
  template <class T> T *create() {
    static_assert(std::is_trivially_destructible_v<T>,
                  "AST nodes live in an arena and are never destroyed");
    void *Mem = Alloc.Allocate(sizeof(T), alignof(T));
    return new (Mem) T();
  }

  /// Copies the elements into the arena.
  template <class T> llvm::ArrayRef<T> copy(llvm::ArrayRef<T> Elts) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (Elts.empty())
      return {};
    T *Mem = Alloc.Allocate<T>(Elts.size());
    std::uninitialized_copy(Elts.begin(), Elts.end(), Mem);
    return llvm::ArrayRef<T>(Mem, Elts.size());
  }
  template <class T, unsigned N>
  llvm::ArrayRef<T> copy(const llvm::SmallVector<T, N> &V) {
    return copy(llvm::ArrayRef<T>(V));
  }

  Symbol intern(llvm::StringRef S) { return Symbols.get(S); }

  size_t getTotalMemory() const { return Alloc.getTotalMemory(); }
  llvm::BumpPtrAllocator &getAllocator() { return Alloc; }

private:
  llvm::BumpPtrAllocator Alloc;
  SymbolTable Symbols;
};

} // namespace ast
} // namespace gopp

#endif // GOPP_AST_ASTCONTEXT_H
