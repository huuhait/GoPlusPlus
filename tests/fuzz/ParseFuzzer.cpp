//===- ParseFuzzer.cpp - libFuzzer target for the front end ---------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Lexer + parser + `gopp fmt` on arbitrary bytes; see FuzzCheck.h for the
// properties. A violation prints the reason and aborts, so libFuzzer saves the
// input as crash-*. Build and run: tests/fuzz/README.md.
//
//===----------------------------------------------------------------------===//

#include "FuzzCheck.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  // Bigger inputs only slow the fuzzer down; the parser is linear.
  if (Size > (1u << 16))
    return 0;
  llvm::StringRef Text(reinterpret_cast<const char *>(Data), Size);
  std::string Why = gopp::fuzz::checkText(Text);
  if (!Why.empty()) {
    std::fprintf(stderr, "fuzz check failed: %s\n", Why.c_str());
    std::abort();
  }
  return 0;
}
