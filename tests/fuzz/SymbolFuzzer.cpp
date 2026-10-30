//===- SymbolFuzzer.cpp - libFuzzer target for the symbol demangler -------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The demangler of docs/spec/appendix-a-symbol-names.md (P04.17) on arbitrary
// bytes, as a symbol and as a type. For every input: it returns (no crash, no
// hang, no sanitizer report); if it accepts, the text is exactly what the
// mangler writes for the tree it read, the readable form can be printed, and
// the same tree read again in the same Context is the same node; if it
// refuses, it says why. A violation prints the reason and aborts, so libFuzzer
// saves the input as crash-*. Build and run: tests/fuzz/README.md.
//
//===----------------------------------------------------------------------===//

#include "gopp/Mangle/SymbolName.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using namespace gopp::sym;

static void violation(const char *What, llvm::StringRef Text) {
  std::fprintf(stderr, "fuzz check failed: %s: %.*s\n", What,
               static_cast<int>(Text.size()), Text.data());
  std::abort();
}

static void check(llvm::StringRef Text, bool Symbol) {
  Context C;
  ParseError Err;
  const Node *N = Symbol ? parseSymbol(C, Text, Err) : parseType(C, Text, Err);
  if (!N) {
    if (Err.Message.empty() || Err.Pos > Text.size())
      violation("a refusal without a reason", Text);
    if (Symbol && demangle(Text).Ok)
      violation("demangle accepts what parseSymbol refuses", Text);
    return;
  }
  if ((Symbol ? mangle(N) : mangleType(N)) != Text)
    violation("accepted text is not canonical", Text);
  const Node *Again =
      Symbol ? parseSymbol(C, Text, Err) : parseType(C, Text, Err);
  if (Again != N)
    violation("reading the text twice gives two nodes", Text);
  Printed P = print(N, 1 << 16);
  if (!P.Ok && P.Text.size() < (1u << 16))
    violation("the printer stopped early", Text);
  if (Symbol) {
    DemangleResult D = demangle(Text, 1 << 16);
    if (D.Ok != P.Ok || (D.Ok && D.Text != P.Text))
      violation("demangle and print disagree", Text);
  }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  // A longer input only slows the fuzzer down; the demangler is linear.
  if (Size > (1u << 16))
    return 0;
  llvm::StringRef Text(reinterpret_cast<const char *>(Data), Size);
  check(Text, true);
  check(Text, false);
  // Most inputs fail at the prefix: also try them behind it.
  std::string Behind = "_GP" + Text.str();
  check(Behind, true);
  return 0;
}
