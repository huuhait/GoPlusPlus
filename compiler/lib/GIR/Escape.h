//===- Escape.h - Which closures escape, which variables need a cell -----===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.07: the facts the lowering of function literals and method values needs,
// decided on the checker's AST before any GIR is made (gir.md section 15,
// "Closures and func values"):
//
//   - the captured variables of each literal, and its ordinal (1, 2, ... in
//     source order) among the literals directly inside its parent (its
//     symbol, sym::Context::closure, appendix A.5);
//   - the variable at the root of the receiver of a method value `x.M` of a
//     pointer method, which binds its address (`x`, `x.f`, `a[i]`, `s[i]`,
//     `a[i:j][k]`: through fields held by value, indexes and slicing, never
//     through a pointer; CLO-5);
//   - whether the literal or method value can outlive the call that made it
//     (it "escapes", CLO-3), which decides if its environment lives in the
//     frame or on the heap;
//   - which variables must live in a heap cell: those a literal or a method
//     value that escapes holds (CLO-2).
//
// A literal or method value does not escape when every use of its value is
// one of: the callee of a call, an argument of a call of a function of this
// package whose parameter the function does not keep (KeepTable), or the one
// initializer of a local variable that is only called, compared with nil or
// passed to such a parameter, and that is declared inside the scope of every
// variable it captures (CLO-1, CLO-3). Everything else escapes, and so does a
// literal held by a variable that an escaping one captures. Every walk is
// iterative (a stack of its own) and counts its steps in EscapeSteps
// (gir::escapeSteps).
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_LIB_GIR_ESCAPE_H
#define GOPP_LIB_GIR_ESCAPE_H

#include "gopp/AST/AST.h"
#include "gopp/Sema/Sema.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"

namespace gopp {
namespace gir {
namespace impl {

namespace S = gopp::sema;

/// "Does the function keep this parameter?" for the functions of one package:
/// a parameter is kept when the function stores it, returns it, captures it
/// in a closure, or passes it to something unknown (a method, a func value,
/// another package, a variadic tail) or to a parameter that is kept. The least
/// fixpoint, from "not kept".
class KeepTable {
public:
  void compute(llvm::ArrayRef<const ast::File *> Files, const S::Info &I);
  /// True when F's parameter K may outlive the call (also for a function not
  /// in the table).
  bool keeps(const S::Func *F, size_t K) const;

private:
  llvm::DenseMap<const S::Func *, unsigned> Base; // first slot of the function
  llvm::SmallVector<char, 64> Kept;
};

/// Which checker types can hold a func value (P04.07 review R1): a func type,
/// an Option of one, an interface (it can box one), a type parameter, and
/// every type that holds one of these in a field, an element, a key, or
/// behind a pointer or a view. The one predicate of the escape analysis for
/// "this variable may hold a closure or a method value": a variable is a
/// holder of a literal (or a method value) only if its type can hold one,
/// and every use of every such variable is followed. Iterative (a type may
/// refer to itself) and memoized: a question decides every type it reaches,
/// so the questions of a package cost work linear in its types; the steps
/// count in EscapeSteps (`GIRStress.*HolderTypes*`).
class FuncHolderTypes {
public:
  bool mayContainFuncValue(S::Type *T);

private:
  llvm::DenseMap<const S::Type *, bool> Memo;
};

/// A function literal, or a method value `x.M` (a closure over its receiver,
/// CLO-5).
struct LitFacts {
  /// The ordinal among the literals directly inside the parent literal (or
  /// the function), in source order, from 1 (appendix A.5). 0 for a method
  /// value, which is no literal and is named by its method (`TV`).
  unsigned Ordinal = 0;
  const ast::FuncLit *Parent = nullptr;
  /// A literal: the variables of enclosing functions it (or a literal inside
  /// it) uses, in order of first use. A method value of a pointer method:
  /// the variable whose storage holds its receiver (none when the receiver is
  /// reached through a pointer or a slice).
  llvm::SmallVector<const S::Var *, 4> Captures;
  bool Escapes = true;
  bool Method = false;
};

struct EntityFacts {
  /// Keyed by the FuncLit or the SelectorExpr of a method value.
  llvm::DenseMap<const ast::Expr *, LitFacts> Lits;
  /// Variables held by a literal or a method value that escapes: each lives
  /// in a heap cell.
  llvm::DenseSet<const S::Var *> Cells;
};

/// Analyzes the function literals and method values inside Roots (a function
/// body, or the initializer of a package variable). Body is the block whose
/// locals are the entity's own (a literal's body is another function), null for
/// an initializer; Outer are variables declared outside Roots that belong to
/// the entity (its parameters and results).
void analyzeEntity(llvm::ArrayRef<const ast::Node *> Roots,
                   const ast::BlockStmt *Body,
                   llvm::ArrayRef<const S::Var *> Outer, const KeepTable &Keeps,
                   FuncHolderTypes &Holders, const S::Info &I,
                   EntityFacts &Out);

} // namespace impl
} // namespace gir
} // namespace gopp

#endif // GOPP_LIB_GIR_ESCAPE_H
