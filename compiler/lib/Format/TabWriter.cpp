//===- TabWriter.cpp - Elastic tabstops -----------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Port of text/tabwriter/tabwriter.go (Copyright 2009 The Go Authors. All
// rights reserved. Use of this source code is governed by a BSD-style license,
// see NOTICE).
//
//===----------------------------------------------------------------------===//

#include "TabWriter.h"

using namespace gopp::format;

namespace {
/// Number of UTF-8 code points (a byte that is not a continuation byte starts
/// one; invalid bytes count as one each, like utf8.RuneCount for valid input).
int runeCount(llvm::StringRef S) {
  int N = 0;
  for (unsigned char C : S)
    if ((C & 0xC0) != 0x80)
      ++N;
  return N;
}
} // namespace

TabWriter::TabWriter(std::string &Out, int MinWidth, int TabWidth, int Padding,
                     char PadChar, unsigned Flags)
    : Out(Out), MinWidth(MinWidth), TabWidth(TabWidth), Padding(Padding),
      PadChar(PadChar), Flags(Flags) {
  reset();
}

void TabWriter::addLine() { Lines.emplace_back(); }

void TabWriter::reset() {
  Buf.clear();
  Pos = 0;
  Cur = Cell();
  InEscape = false;
  Lines.clear();
  Widths.clear();
  addLine();
}

void TabWriter::writePadding(int TextW, int CellW, bool UseTabs) {
  if (PadChar == '\t' || UseTabs) {
    if (TabWidth == 0)
      return; // tabs have no width - can't do any padding
    CellW = (CellW + TabWidth - 1) / TabWidth * TabWidth;
    int N = CellW - TextW; // amount of padding
    Out.append((N + TabWidth - 1) / TabWidth, '\t');
    return;
  }
  Out.append(CellW - TextW, PadChar);
}

size_t TabWriter::writeLines(size_t Pos0, size_t Line0, size_t Line1) {
  size_t P = Pos0;
  for (size_t I = Line0; I < Line1; ++I) {
    const std::vector<Cell> &Line = Lines[I];
    bool UseTabs = Flags & TabIndent;
    for (size_t J = 0; J < Line.size(); ++J) {
      const Cell &C = Line[J];
      if (C.Size == 0) {
        // empty cell
        if (J < Widths.size())
          writePadding(C.Width, Widths[J], UseTabs);
      } else {
        // non-empty cell
        UseTabs = false;
        Out.append(Buf, P, C.Size);
        P += C.Size;
        if (J < Widths.size())
          writePadding(C.Width, Widths[J], false);
      }
    }
    if (I + 1 == Lines.size()) {
      // last buffered line - we don't have a newline, so we just write any
      // outstanding buffered text
      Out.append(Buf, P, Cur.Size);
      P += Cur.Size;
    } else {
      Out.push_back('\n');
    }
  }
  return P;
}

size_t TabWriter::format(size_t Pos0, size_t Line0, size_t Line1) {
  size_t P = Pos0;
  size_t Column = Widths.size();
  for (size_t This = Line0; This < Line1; ++This) {
    const std::vector<Cell> *Line = &Lines[This];
    if (Column + 1 >= Line->size())
      continue;
    // cell exists in this column => this line has more cells than the previous
    // line (the last cell per line is ignored because cells are tab-terminated;
    // the last cell per line describes the text before the newline/formfeed and
    // does not belong to a column)

    // print unprinted lines until beginning of block
    P = writeLines(P, Line0, This);
    Line0 = This;

    // column block begin
    int Width = MinWidth; // minimal column width
    bool Discardable =
        true; // true if all cells in this column are empty and "soft"
    for (; This < Line1; ++This) {
      Line = &Lines[This];
      if (Column + 1 >= Line->size())
        break;
      // cell exists in this column
      const Cell &C = (*Line)[Column];
      // update width
      int W = C.Width + Padding;
      if (W > Width)
        Width = W;
      // update discardable
      if (C.Width > 0 || C.HTab)
        Discardable = false;
    }
    // column block end

    // discard empty columns if necessary
    if (Discardable && (Flags & DiscardEmptyColumns))
      Width = 0;

    // format and print all columns to the right of this column
    // (we know the widths of this column and all columns to the left)
    Widths.push_back(Width);
    P = format(P, Line0, This);
    Widths.pop_back();
    Line0 = This;
  }
  // print unprinted lines until end
  return writeLines(P, Line0, Line1);
}

void TabWriter::append(llvm::StringRef Text) {
  Buf.append(Text.data(), Text.size());
  Cur.Size += static_cast<int>(Text.size());
}

void TabWriter::updateWidth() {
  Cur.Width += runeCount(llvm::StringRef(Buf).substr(Pos));
  Pos = Buf.size();
}

void TabWriter::endEscape() {
  // Escape: the text was counted as if it had the Escape bytes in it
  updateWidth();
  Cur.Width -= 2; // don't count the Escape bytes
  Pos = Buf.size();
  InEscape = false;
}

size_t TabWriter::terminateCell(bool HTab) {
  Cur.HTab = HTab;
  Lines.back().push_back(Cur);
  Cur = Cell();
  return Lines.back().size();
}

void TabWriter::flushNoDefers() {
  // add current cell if not empty
  if (Cur.Size > 0) {
    if (InEscape)
      endEscape(); // terminate escape sequence
    terminateCell(false);
  }
  // format contents of buffer
  format(0, 0, Lines.size());
  reset();
}

void TabWriter::flush() { flushNoDefers(); }

void TabWriter::write(llvm::StringRef Text) {
  size_t N = 0; // buffer value up to which text was already appended
  for (size_t I = 0; I < Text.size(); ++I) {
    char Ch = Text[I];
    if (!InEscape) {
      switch (Ch) {
      case '\t':
      case '\v':
      case '\n':
      case '\f': {
        // end of cell
        append(Text.substr(N, I - N));
        updateWidth();
        N = I + 1; // Ch consumed
        size_t NCells = terminateCell(Ch == '\t');
        if (Ch == '\n' || Ch == '\f') {
          // terminate line
          addLine();
          if (Ch == '\f' || NCells == 1) {
            // A '\f' always forces a flush. Otherwise, if the previous line has
            // only one cell which does not have an impact on the formatting of
            // the following lines (the last cell per line is ignored by
            // format()), thus we can flush the Writer contents.
            flushNoDefers();
          }
        }
        break;
      }
      case Escape:
        // start of escaped sequence
        append(Text.substr(N, I - N));
        updateWidth();
        N = I;
        InEscape = true;
        break;
      default:
        break;
      }
    } else if (Ch == Escape) {
      // end of escaped sequence
      append(Text.substr(N, I + 1 - N));
      N = I + 1; // Ch consumed
      endEscape();
    }
  }
  // append leftover text
  append(Text.substr(N));
}
