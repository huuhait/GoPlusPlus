//===- gopp/AST/ASTDumper.h - AST dump and traversal ------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_AST_ASTDUMPER_H
#define GOPP_AST_ASTDUMPER_H

#include "gopp/AST/AST.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/raw_ostream.h"

namespace gopp {
namespace ast {

/// Calls F(label, child) for every non-null child node, in source order. The
/// label is the field name ("Cond", "Args", ...); list elements share a label.
void forEachChild(const Node *N,
                  llvm::function_ref<void(const char *, const Node *)> F);

/// Prints the tree, one node per line:
///   Label: Kind <line:col, line:col> details
/// The range is half-open: the end is the position after the last byte.
/// The output is deterministic and is what the lit golden tests check.
void dump(llvm::raw_ostream &OS, const SourceManager &SM, const Node *Root);

} // namespace ast
} // namespace gopp

#endif // GOPP_AST_ASTDUMPER_H
