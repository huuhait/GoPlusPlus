//===- gopp/GIR/Verify.h - The GIR verifier ---------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01. Checks what the builder and every later pass promise: the rules
// V001 to V070 of docs/design/gir.md section 9 (ids in range, every place,
// operand and assignment typed right, calls against the callee's signature,
// the CFG shape, and the cleanup and unwind rules of D4.6). Each rule is
// decided in exactly one function of Verify.cpp, under its id.
//
// The verifier runs at the end of every build of GIR (Lower.h), after every
// read of the binary form (Serialize.h) and at the start of the LLVM lowering
// (CodeGen.h), so each test that makes GIR checks it. It trusts nothing it is
// given: every type is recomputed. Every walk is iterative and its time is
// linear in the size of the module.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_VERIFY_H
#define GOPP_GIR_VERIFY_H

#include "gopp/GIR/GIR.h"

#include <string>
#include <vector>

namespace gopp {
namespace gir {

/// One broken rule.
struct VerifyError {
  std::string Rule;   // "V027"; see docs/design/gir.md section 9
  std::string Func;   // the function's name; empty for a module-level error
  int64_t Block = -1; // -1: not in a block
  int64_t Index = -1; // the statement; the terminator is Stmts.size()
  std::string Message;
  /// "V027: fn main.f: bb3 #2: duplicate switch case 1".
  std::string str() const;
};

/// The broken rules of M, at most MaxErrors; empty if M is valid.
std::vector<VerifyError> verify(const Module &M, unsigned MaxErrors = 20);

/// verify, printing one "gir verifier: ..." line per error to OS; true if M
/// is valid.
bool verify(const Module &M, llvm::raw_ostream &OS);

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_VERIFY_H
