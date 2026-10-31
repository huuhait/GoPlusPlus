//===- Unicode.cpp - UTF-8 and character classes --------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Unicode.h"

#include <algorithm>
#include <iterator>

#include "UnicodeTables.inc"

using namespace gopp;

namespace {
template <size_t N> bool inRanges(const uint32_t (&Table)[N][2], uint32_t CP) {
  const uint32_t (*End)[2] = std::end(Table);
  // First range whose end is >= CP.
  const uint32_t (*It)[2] = std::lower_bound(
      std::begin(Table), End, CP,
      [](const uint32_t (&R)[2], uint32_t V) { return R[1] < V; });
  return It != End && (*It)[0] <= CP;
}
} // namespace

bool unicode::isLetterNonASCII(uint32_t CP) {
  return inRanges(LetterRanges, CP);
}

bool unicode::isDigitNonASCII(uint32_t CP) { return inRanges(DigitRanges, CP); }

bool unicode::isPrint(uint32_t CP) {
  if (CP < 0x80)
    return CP >= 0x20 && CP < 0x7F;
  return inRanges(PrintRanges, CP);
}

std::string unicode::quote(std::string_view S) {
  static const char Hex[] = "0123456789abcdef";
  std::string R = "\"";
  auto Escape = [&](char Kind, uint32_t V, int Digits) {
    R += '\\';
    R += Kind;
    for (int Shift = 4 * (Digits - 1); Shift >= 0; Shift -= 4)
      R += Hex[(V >> Shift) & 15];
  };
  const char *P = S.data(), *End = P + S.size();
  while (P < End) {
    uint32_t CP;
    bool Invalid;
    unsigned Len = decodeUTF8(P, End, CP, Invalid);
    if (Invalid) {
      // A byte that is not part of valid UTF-8: \xNN, one byte at a time.
      Escape('x', static_cast<unsigned char>(*P), 2);
      ++P;
      continue;
    }
    switch (CP) {
    case '"':
      R += "\\\"";
      break;
    case '\\':
      R += "\\\\";
      break;
    case '\a':
      R += "\\a";
      break;
    case '\b':
      R += "\\b";
      break;
    case '\f':
      R += "\\f";
      break;
    case '\n':
      R += "\\n";
      break;
    case '\r':
      R += "\\r";
      break;
    case '\t':
      R += "\\t";
      break;
    case '\v':
      R += "\\v";
      break;
    default:
      if (isPrint(CP))
        R.append(P, Len);
      else if (CP < 0x20 || CP == 0x7F)
        Escape('x', CP, 2);
      else if (CP < 0x10000)
        Escape('u', CP, 4);
      else
        Escape('U', CP, 8);
    }
    P += Len;
  }
  return R + "\"";
}

unsigned unicode::decodeUTF8(const char *P, const char *End, uint32_t &CP,
                             bool &Invalid) {
  Invalid = false;
  auto B = [&](unsigned I) { return static_cast<unsigned char>(P[I]); };
  unsigned C = B(0);
  if (C < 0x80) {
    CP = C;
    return 1;
  }
  size_t Avail = End - P;
  unsigned Len;
  uint32_t Min, V;
  if (C >= 0xC2 && C <= 0xDF) {
    Len = 2, Min = 0x80, V = C & 0x1F;
  } else if (C >= 0xE0 && C <= 0xEF) {
    Len = 3, Min = 0x800, V = C & 0x0F;
  } else if (C >= 0xF0 && C <= 0xF4) {
    Len = 4, Min = 0x10000, V = C & 0x07;
  } else {
    CP = ReplacementChar, Invalid = true;
    return 1;
  }
  if (Avail < Len) {
    CP = ReplacementChar, Invalid = true;
    return 1;
  }
  for (unsigned I = 1; I < Len; ++I) {
    if ((B(I) & 0xC0) != 0x80) {
      CP = ReplacementChar, Invalid = true;
      return 1;
    }
    V = (V << 6) | (B(I) & 0x3F);
  }
  if (V < Min || V > MaxRune || (V >= 0xD800 && V < 0xE000)) {
    CP = ReplacementChar, Invalid = true;
    return 1;
  }
  CP = V;
  return Len;
}
