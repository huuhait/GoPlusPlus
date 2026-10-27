//===- TabWriter.h - Elastic tabstops ---------------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A port of Go's text/tabwriter (Copyright 2009 The Go Authors; BSD-style
// license, see NOTICE) with the options gofmt uses: DiscardEmptyColumns and
// TabIndent, no HTML filter. Text between two Escape bytes passes through
// unchanged and counts as its rune width.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_FORMAT_TABWRITER_H
#define GOPP_FORMAT_TABWRITER_H

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace gopp::format {

constexpr char Escape = '\xff';

class TabWriter {
public:
  enum Flags : unsigned { DiscardEmptyColumns = 1, TabIndent = 2 };

  TabWriter(std::string &Out, int MinWidth, int TabWidth, int Padding,
            char PadChar, unsigned Flags);

  /// Adds text; output is produced at line breaks that end a column block.
  void write(llvm::StringRef Text);
  /// Writes everything that is pending.
  void flush();

private:
  struct Cell {
    int Size = 0;      // bytes
    int Width = 0;     // runes
    bool HTab = false; // terminated by '\t'
  };

  void addLine();
  void reset();
  void writePadding(int TextW, int CellW, bool UseTabs);
  size_t writeLines(size_t Pos0, size_t Line0, size_t Line1);
  size_t format(size_t Pos0, size_t Line0, size_t Line1);
  void append(llvm::StringRef Text);
  void updateWidth();
  void endEscape();
  size_t terminateCell(bool HTab);
  void flushNoDefers();

  std::string &Out;
  int MinWidth, TabWidth, Padding;
  char PadChar;
  unsigned Flags;

  std::string Buf;
  size_t Pos = 0; // buffer position up to which Cell.Width has been computed
  Cell Cur;
  bool InEscape = false;
  std::vector<std::vector<Cell>> Lines;
  std::vector<int> Widths;
};

} // namespace gopp::format

#endif // GOPP_FORMAT_TABWRITER_H
