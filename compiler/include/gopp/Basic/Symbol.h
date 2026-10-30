//===- gopp/Basic/Symbol.h - Interned identifiers ---------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A Symbol is an interned string: two symbols from the same SymbolTable are
// equal exactly when their text is equal, and comparing them is a pointer
// compare. The text lives in the table's allocator (an AST arena).
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BASIC_SYMBOL_H
#define GOPP_BASIC_SYMBOL_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"

namespace gopp {

class Symbol {
  llvm::StringRef Text;

public:
  Symbol() = default;
  explicit Symbol(llvm::StringRef Interned) : Text(Interned) {}
  llvm::StringRef str() const { return Text; }
  bool empty() const { return Text.empty(); }
  friend bool operator==(Symbol A, Symbol B) {
    return A.Text.data() == B.Text.data();
  }
  friend bool operator!=(Symbol A, Symbol B) { return !(A == B); }
};

/// The name of an operator method (ADR-0024, TYP-27): `+`, `==`, `[]`, `[]=`,
/// `()`, `<-`, `range` and so on. The parser makes it one Ident with this
/// text, and the checker keeps it as the method's name. No identifier can
/// have it: `range` is a keyword.
inline bool isOperatorSpelling(llvm::StringRef N) {
  if (N.empty())
    return false;
  unsigned char C = static_cast<unsigned char>(N[0]);
  if (C >= 0x80 || C == '_' || (C >= 'a' && C <= 'z') ||
      (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9'))
    return N == "range";
  return true;
}

class SymbolTable {
public:
  explicit SymbolTable(llvm::BumpPtrAllocator &Alloc) : Set(Alloc) {}
  Symbol get(llvm::StringRef S) {
    return Symbol(Set.insert(S).first->getKey());
  }
  size_t size() const { return Set.size(); }

private:
  llvm::StringSet<llvm::BumpPtrAllocator &> Set;
};

} // namespace gopp

#endif // GOPP_BASIC_SYMBOL_H
