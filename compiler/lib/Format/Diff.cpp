//===- Diff.cpp - Unified diff for `gopp fmt -d` --------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Format/Format.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <vector>

using namespace gopp;
using llvm::StringRef;

namespace {

std::vector<StringRef> splitLines(StringRef S) {
  std::vector<StringRef> Lines;
  while (!S.empty()) {
    size_t N = S.find('\n');
    if (N == StringRef::npos) {
      Lines.push_back(S);
      break;
    }
    Lines.push_back(S.substr(0, N + 1)); // keeps the newline
    S = S.substr(N + 1);
  }
  return Lines;
}

enum class Op : char { Keep = ' ', Del = '-', Add = '+' };

/// Edit script of A -> B: common prefix and suffix are cut off, the middle is
/// the longest common subsequence (or a plain replacement if it is huge).
std::vector<std::pair<Op, StringRef>>
editScript(const std::vector<StringRef> &A, const std::vector<StringRef> &B) {
  size_t Pre = 0;
  while (Pre < A.size() && Pre < B.size() && A[Pre] == B[Pre])
    ++Pre;
  size_t Suf = 0;
  while (Suf < A.size() - Pre && Suf < B.size() - Pre &&
         A[A.size() - 1 - Suf] == B[B.size() - 1 - Suf])
    ++Suf;
  size_t N = A.size() - Pre - Suf, M = B.size() - Pre - Suf;

  std::vector<std::pair<Op, StringRef>> Script;
  for (size_t I = 0; I < Pre; ++I)
    Script.push_back({Op::Keep, A[I]});

  if (N * M > 16u * 1024 * 1024) {
    for (size_t I = 0; I < N; ++I)
      Script.push_back({Op::Del, A[Pre + I]});
    for (size_t J = 0; J < M; ++J)
      Script.push_back({Op::Add, B[Pre + J]});
  } else {
    // L[i][j] = length of the LCS of A[Pre+i..] and B[Pre+j..]
    std::vector<unsigned> L((N + 1) * (M + 1), 0);
    auto at = [&](size_t I, size_t J) -> unsigned & {
      return L[I * (M + 1) + J];
    };
    for (size_t I = N; I-- > 0;)
      for (size_t J = M; J-- > 0;)
        at(I, J) = A[Pre + I] == B[Pre + J]
                       ? at(I + 1, J + 1) + 1
                       : std::max(at(I + 1, J), at(I, J + 1));
    size_t I = 0, J = 0;
    while (I < N && J < M) {
      if (A[Pre + I] == B[Pre + J]) {
        Script.push_back({Op::Keep, A[Pre + I]});
        ++I;
        ++J;
      } else if (at(I + 1, J) >= at(I, J + 1)) {
        Script.push_back({Op::Del, A[Pre + I++]});
      } else {
        Script.push_back({Op::Add, B[Pre + J++]});
      }
    }
    for (; I < N; ++I)
      Script.push_back({Op::Del, A[Pre + I]});
    for (; J < M; ++J)
      Script.push_back({Op::Add, B[Pre + J]});
  }

  for (size_t I = A.size() - Suf; I < A.size(); ++I)
    Script.push_back({Op::Keep, A[I]});
  return Script;
}

} // namespace

std::string gopp::unifiedDiff(StringRef Name, StringRef Old, StringRef New) {
  if (Old == New)
    return "";
  std::vector<StringRef> A = splitLines(Old), B = splitLines(New);
  auto Script = editScript(A, B);

  constexpr size_t Context = 3;
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  OS << "--- " << Name << ".orig\n+++ " << Name << "\n";

  size_t I = 0;
  size_t ALine = 1, BLine = 1; // 1-based lines of Script[I]
  while (I < Script.size()) {
    // Find the next change.
    size_t Next = I;
    while (Next < Script.size() && Script[Next].first == Op::Keep)
      ++Next;
    if (Next == Script.size())
      break;
    size_t Start = Next >= Context + I ? Next - Context : I;
    // The hunk starts at Start; count the lines up to there.
    size_t SA = ALine, SB = BLine;
    for (size_t K = I; K < Start; ++K) {
      ++SA;
      ++SB;
    }
    // Extend the hunk while changes are within 2*Context of each other.
    size_t End = Next;
    for (;;) {
      size_t K = End;
      while (K < Script.size() && Script[K].first != Op::Keep)
        ++K;
      End = K; // first Keep after the changes
      size_t Run = 0;
      while (K < Script.size() && Script[K].first == Op::Keep) {
        ++K;
        ++Run;
      }
      if (K < Script.size() && Run <= 2 * Context) {
        End = K;
        continue;
      }
      End = std::min(End + Context, Script.size());
      // `End` may point into the trailing Keep lines only.
      break;
    }
    size_t CountA = 0, CountB = 0;
    for (size_t K = Start; K < End; ++K) {
      if (Script[K].first != Op::Add)
        ++CountA;
      if (Script[K].first != Op::Del)
        ++CountB;
    }
    OS << "@@ -" << (CountA ? SA : SA - 1) << ',' << CountA << " +"
       << (CountB ? SB : SB - 1) << ',' << CountB << " @@\n";
    for (size_t K = Start; K < End; ++K) {
      OS << (char)Script[K].first << Script[K].second;
      if (!Script[K].second.ends_with("\n"))
        OS << "\n\\ No newline at end of file\n";
    }
    for (size_t K = I; K < End; ++K) {
      if (Script[K].first != Op::Add)
        ++ALine;
      if (Script[K].first != Op::Del)
        ++BLine;
    }
    I = End;
  }
  return Out;
}
