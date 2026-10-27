//===- gopp/Basic/Unicode.h - UTF-8 and character classes -------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Strict UTF-8 decoding with Go's rules (no overlong forms, no surrogates,
// nothing above U+10FFFF) and the character classes Go uses for identifiers.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BASIC_UNICODE_H
#define GOPP_BASIC_UNICODE_H

#include <cstdint>

namespace gopp {
namespace unicode {

constexpr uint32_t ReplacementChar = 0xFFFD;
constexpr uint32_t MaxRune = 0x10FFFF;
constexpr uint32_t BOM = 0xFEFF;

/// Decodes one code point from [P, End). P < End is required. Returns the
/// number of bytes used (1..4). For an invalid or truncated sequence it
/// returns 1 and sets CP to ReplacementChar; Invalid is then true (a valid
/// U+FFFD sets Invalid to false).
unsigned decodeUTF8(const char *P, const char *End, uint32_t &CP,
                    bool &Invalid);

/// Go's unicode.IsLetter (category L) for CP >= 0x80; ASCII is handled by
/// the callers.
bool isLetterNonASCII(uint32_t CP);
/// Go's unicode.IsDigit (category Nd) for CP >= 0x80.
bool isDigitNonASCII(uint32_t CP);

inline bool isLetter(uint32_t CP) {
  if (CP < 0x80)
    return (CP | 0x20) - 'a' < 26u || CP == '_';
  return isLetterNonASCII(CP);
}
inline bool isDigit(uint32_t CP) {
  if (CP < 0x80)
    return CP - '0' < 10u;
  return isDigitNonASCII(CP);
}

} // namespace unicode
} // namespace gopp

#endif // GOPP_BASIC_UNICODE_H
