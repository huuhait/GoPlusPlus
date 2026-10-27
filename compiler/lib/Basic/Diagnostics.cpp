//===- Diagnostics.cpp - Diagnostics engine -------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/Unicode.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/Unicode.h"

#include <algorithm>
#include <cassert>
#include <iterator>

using namespace gopp;

llvm::StringRef gopp::getSeverityName(Severity S) {
  switch (S) {
  case Severity::Note:
    return "note";
  case Severity::Warning:
    return "warning";
  case Severity::Error:
    return "error";
  }
  return "error";
}

//===----------------------------------------------------------------------===//
// The diagnostic table
//===----------------------------------------------------------------------===//

namespace {
struct DiagInfo {
  const char *Name;
  Severity Sev;
  const char *Format;
};

const DiagInfo DiagTable[] = {
#define DIAG(Name, Sev, Text) {#Name, Severity::Sev, Text},
#include "gopp/Basic/DiagnosticIDs.def"
};
static_assert(std::size(DiagTable) == diag::NUM_DIAGS);

const DiagInfo &info(diag::ID ID) {
  assert(ID < diag::NUM_DIAGS && "bad diagnostic ID");
  return DiagTable[ID < diag::NUM_DIAGS ? ID : diag::custom];
}
} // namespace

llvm::StringRef gopp::getDiagName(diag::ID ID) { return info(ID).Name; }

Severity gopp::getDiagSeverity(diag::ID ID) { return info(ID).Sev; }

llvm::StringRef gopp::getDiagFormat(diag::ID ID) { return info(ID).Format; }

std::string gopp::formatDiagnostic(llvm::StringRef Format,
                                   llvm::ArrayRef<std::string> Args) {
  std::string Out;
  Out.reserve(Format.size());
  for (size_t I = 0; I < Format.size(); ++I) {
    char C = Format[I];
    char Next = I + 1 < Format.size() ? Format[I + 1] : '\0';
    if (C == '%' && Next == '%') {
      Out += '%';
      ++I;
    } else if (C == '%' && Next >= '0' && Next <= '9') {
      size_t Idx = Next - '0';
      if (Idx < Args.size())
        Out += Args[Idx];
      ++I;
    } else {
      Out += C;
    }
  }
  return Out;
}

//===----------------------------------------------------------------------===//
// Engine
//===----------------------------------------------------------------------===//

DiagnosticBuilder::DiagnosticBuilder(DiagnosticsEngine &E, SourceLoc Loc,
                                     diag::ID ID)
    : Engine(&E) {
  D.ID = ID;
  D.Sev = getDiagSeverity(ID);
  D.Loc = Loc;
  D.Primary = SourceRange(Loc);
}

DiagnosticBuilder::DiagnosticBuilder(DiagnosticsEngine &E, SourceLoc Loc,
                                     Severity S, const llvm::Twine &Msg)
    : DiagnosticBuilder(E, Loc, diag::custom) {
  D.Sev = S;
  Args.push_back(Msg.str());
}

DiagnosticBuilder::DiagnosticBuilder(DiagnosticBuilder &&Other) noexcept
    : Engine(Other.Engine), D(std::move(Other.D)), Args(std::move(Other.Args)) {
  Other.Engine = nullptr;
}

DiagnosticBuilder &
DiagnosticBuilder::note(SourceLoc Loc, diag::ID NoteID,
                        llvm::ArrayRef<llvm::StringRef> NoteArgs,
                        SourceRange R) {
  llvm::SmallVector<std::string, 2> Strs;
  for (llvm::StringRef A : NoteArgs)
    Strs.push_back(A.str());
  D.Notes.push_back(
      {NoteID, Loc, R, formatDiagnostic(getDiagFormat(NoteID), Strs)});
  return *this;
}

DiagnosticBuilder::~DiagnosticBuilder() {
  if (!Engine)
    return;
  D.Message = formatDiagnostic(getDiagFormat(D.ID), Args);
  Engine->emit(std::move(D));
}

void DiagnosticsEngine::emit(Diagnostic D) {
  if (D.Sev == Severity::Error) {
    if (ErrorLimit != 0 && NumErrors >= ErrorLimit) {
      if (!LimitReached) {
        LimitReached = true;
        Diagnostic Stop;
        Stop.ID = diag::err_too_many_errors;
        Stop.Sev = getDiagSeverity(Stop.ID);
        Stop.Message = getDiagFormat(Stop.ID).str();
        Consumer.handleDiagnostic(Stop);
      }
      return;
    }
    ++NumErrors;
  } else if (D.Sev == Severity::Warning) {
    ++NumWarnings;
  }
  Consumer.handleDiagnostic(D);
}

//===----------------------------------------------------------------------===//
// Text output
//===----------------------------------------------------------------------===//

namespace {

constexpr unsigned TabStop = 4;

/// One displayed line: the text with tabs expanded and unprintable bytes
/// spelled out, and the display column of every source byte.
struct DisplayLine {
  std::string Text;
  // Column (0-based, in display cells) where byte I starts; has size()+1
  // entries, the last one is the end of the line.
  std::vector<unsigned> ColOfByte;
  // Offset in Text where byte I's cell starts; size()+1 entries.
  std::vector<size_t> TextOfByte;
};

// A longer line is clipped to this many display columns around the caret.
constexpr unsigned MaxSnippetWidth = 120;

DisplayLine layoutLine(llvm::StringRef Line) {
  DisplayLine DL;
  unsigned Col = 0;
  const char *P = Line.data(), *End = P + Line.size();
  DL.ColOfByte.assign(Line.size() + 1, 0);
  DL.TextOfByte.assign(Line.size() + 1, 0);
  while (P != End) {
    size_t Idx = P - Line.data();
    uint32_t CP;
    bool Invalid;
    unsigned N = unicode::decodeUTF8(P, End, CP, Invalid);
    std::string Cell;
    unsigned Width;
    if (!Invalid && CP == '\t') {
      Width = TabStop - (Col % TabStop);
      Cell.assign(Width, ' ');
    } else if (Invalid) {
      char Buf[8];
      snprintf(Buf, sizeof Buf, "<%02X>", static_cast<unsigned char>(*P));
      Cell = Buf;
      Width = Cell.size();
    } else {
      int W = llvm::sys::unicode::columnWidthUTF8(llvm::StringRef(P, N));
      if (W < 0 || CP == '\r' || CP == 0x7F) {
        char Buf[16];
        snprintf(Buf, sizeof Buf, "<U+%04X>", CP);
        Cell = Buf;
        Width = Cell.size();
      } else {
        Cell.assign(P, N);
        Width = W;
      }
    }
    for (unsigned I = 0; I < N; ++I) {
      DL.ColOfByte[Idx + I] = Col;
      DL.TextOfByte[Idx + I] = DL.Text.size();
    }
    DL.Text += Cell;
    Col += Width;
    P += N;
  }
  DL.ColOfByte[Line.size()] = Col;
  DL.TextOfByte[Line.size()] = DL.Text.size();
  return DL;
}

unsigned numDigits(unsigned N) {
  unsigned D = 1;
  while (N >= 10) {
    N /= 10;
    ++D;
  }
  return D;
}

} // namespace

void TextDiagnosticPrinter::printSnippet(SourceLoc Loc, SourceRange Primary,
                                         llvm::ArrayRef<SourceRange> Secondary,
                                         llvm::ArrayRef<FixIt> FixIts) {
  FileID F = SM.getFileID(Loc);
  if (!F.isValid())
    return;
  llvm::StringRef Line = SM.getLineText(Loc);
  unsigned LineNo = SM.getLineNumber(Loc);
  uint32_t LineStart = SM.getFileOffset(Loc) - (SM.getColumnNumber(Loc) - 1);
  DisplayLine DL = layoutLine(Line);

  unsigned Gutter = std::max(5u, numDigits(LineNo));
  size_t CaretIdx = std::min<size_t>(SM.getColumnNumber(Loc) - 1, Line.size());

  // The window of source bytes [WB, WE) that is shown. A long line is cut to
  // about MaxSnippetWidth columns around the caret, with "..." at cut ends.
  size_t WB = 0, WE = Line.size();
  if (DL.ColOfByte[Line.size()] > MaxSnippetWidth) {
    unsigned CaretCol = DL.ColOfByte[CaretIdx];
    unsigned S =
        CaretCol > MaxSnippetWidth / 2 ? CaretCol - MaxSnippetWidth / 2 : 0;
    auto Begin = DL.ColOfByte.begin();
    WB = std::lower_bound(Begin, Begin + Line.size() + 1, S) - Begin;
    WE = std::min<size_t>(
        std::lower_bound(Begin, Begin + Line.size() + 1, S + MaxSnippetWidth) -
            Begin,
        Line.size());
    // never cut in the middle of a character
    while (WB > 0 && DL.ColOfByte[WB - 1] == DL.ColOfByte[WB])
      --WB;
    while (WE < Line.size() && DL.ColOfByte[WE - 1] == DL.ColOfByte[WE])
      ++WE;
  }
  const char *Dots = "...";
  size_t Pre = WB > 0 ? 3 : 0;
  unsigned BaseCol = DL.ColOfByte[WB];
  unsigned Width = DL.ColOfByte[WE] - BaseCol;
  std::string Marks(Pre + Width + 1, ' ');
  // Maps a range onto this line's display columns; false if not on the line.
  auto Mark = [&](SourceRange R, char C) {
    if (!R.isValid() || SM.getFileID(R.getBegin()) != F)
      return;
    uint32_t B = SM.getFileOffset(R.getBegin());
    uint32_t E = SM.getFileOffset(R.getEnd());
    if (E < B)
      return;
    if (B >= LineStart + Line.size() + 1 || E < LineStart)
      return;
    if (B < LineStart)
      B = LineStart;
    size_t BI = std::min<size_t>(B - LineStart, Line.size());
    size_t EI = std::min<size_t>(E - LineStart, Line.size());
    BI = std::clamp(BI, WB, WE);
    EI = std::clamp(EI, WB, WE);
    for (unsigned Col = DL.ColOfByte[BI]; Col < DL.ColOfByte[EI]; ++Col)
      Marks[Pre + Col - BaseCol] = C;
  };
  for (SourceRange R : Secondary)
    Mark(R, '-');
  Mark(Primary, '~');
  Marks[Pre + DL.ColOfByte[CaretIdx] - BaseCol] = '^';
  while (!Marks.empty() && Marks.back() == ' ')
    Marks.pop_back();

  OS << llvm::right_justify(std::to_string(LineNo), Gutter) << " | "
     << (WB > 0 ? Dots : "")
     << llvm::StringRef(DL.Text).slice(DL.TextOfByte[WB], DL.TextOfByte[WE])
     << (WE < Line.size() ? Dots : "") << '\n';
  OS << std::string(Gutter, ' ') << " | " << Marks << '\n';

  // Fix-its on this line: show the replacement under the caret line.
  for (const FixIt &FI : FixIts) {
    if (!FI.Range.isValid() || SM.getFileID(FI.Range.getBegin()) != F ||
        SM.getLineNumber(FI.Range.getBegin()) != LineNo)
      continue;
    uint32_t B = SM.getFileOffset(FI.Range.getBegin());
    size_t BI = std::min<size_t>(B - LineStart, Line.size());
    if (BI < WB || BI > WE)
      continue; // outside the clipped window
    OS << std::string(Gutter, ' ') << " | "
       << std::string(Pre + DL.ColOfByte[BI] - BaseCol, ' ')
       << (FI.Text.empty() ? "(remove)"
           : llvm::StringRef(FI.Text).trim().empty()
               ? "(space)"
               : llvm::StringRef(FI.Text).rtrim())
       << '\n';
  }
}

void TextDiagnosticPrinter::handleDiagnostic(const Diagnostic &D) {
  auto Header = [&](SourceLoc Loc, Severity S, llvm::StringRef Msg) {
    PresumedLoc P = SM.getPresumedLoc(Loc);
    if (P.isValid())
      OS << P.Filename << ':' << P.Line << ':' << P.Column << ": ";
    OS << getSeverityName(S) << ": " << Msg << '\n';
  };
  Header(D.Loc, D.Sev, D.Message);
  printSnippet(D.Loc, D.Primary, D.Secondary, D.FixIts);
  for (const DiagNote &N : D.Notes) {
    Header(N.Loc, Severity::Note, N.Message);
    if (N.Loc.isValid())
      printSnippet(N.Loc, N.Range, {}, {});
  }
}

//===----------------------------------------------------------------------===//
// JSON output
//===----------------------------------------------------------------------===//

namespace {

void jsonPos(llvm::json::OStream &J, const SourceManager &SM, SourceLoc Loc) {
  PresumedLoc P = SM.getPresumedLoc(Loc);
  J.object([&] {
    J.attribute("line", P.Line);
    J.attribute("column", P.Column);
    J.attribute("offset", P.isValid() ? SM.getFileOffset(Loc) : 0);
  });
}

void jsonRange(llvm::json::OStream &J, const SourceManager &SM, SourceRange R) {
  J.object([&] {
    J.attributeBegin("begin");
    jsonPos(J, SM, R.getBegin());
    J.attributeEnd();
    J.attributeBegin("end");
    jsonPos(J, SM, R.getEnd());
    J.attributeEnd();
  });
}

} // namespace

void JSONDiagnosticPrinter::handleDiagnostic(const Diagnostic &D) {
  // Compact (no indentation): one line per diagnostic.
  llvm::json::OStream J(OS);
  J.object([&] {
    J.attribute("severity", getSeverityName(D.Sev));
    J.attribute("id", getDiagName(D.ID));
    J.attribute("message", llvm::json::fixUTF8(D.Message));
    PresumedLoc P = SM.getPresumedLoc(D.Loc);
    if (P.isValid()) {
      J.attribute("file", llvm::json::fixUTF8(P.Filename));
      J.attribute("line", P.Line);
      J.attribute("column", P.Column);
      J.attribute("offset", SM.getFileOffset(D.Loc));
    }
    if (D.Primary.isValid() && P.isValid()) {
      J.attributeBegin("range");
      jsonRange(J, SM, D.Primary);
      J.attributeEnd();
    }
    if (!D.Secondary.empty()) {
      J.attributeArray("secondaryRanges", [&] {
        for (SourceRange R : D.Secondary)
          if (R.isValid())
            jsonRange(J, SM, R);
      });
    }
    if (!D.Notes.empty()) {
      J.attributeArray("notes", [&] {
        for (const DiagNote &N : D.Notes) {
          J.object([&] {
            J.attribute("id", getDiagName(N.ID));
            J.attribute("message", llvm::json::fixUTF8(N.Message));
            PresumedLoc NP = SM.getPresumedLoc(N.Loc);
            if (NP.isValid()) {
              J.attribute("file", llvm::json::fixUTF8(NP.Filename));
              J.attribute("line", NP.Line);
              J.attribute("column", NP.Column);
              J.attribute("offset", SM.getFileOffset(N.Loc));
              if (N.Range.isValid()) {
                J.attributeBegin("range");
                jsonRange(J, SM, N.Range);
                J.attributeEnd();
              }
            }
          });
        }
      });
    }
    if (!D.FixIts.empty()) {
      J.attributeArray("fixits", [&] {
        for (const FixIt &FI : D.FixIts) {
          J.object([&] {
            J.attributeBegin("range");
            jsonRange(J, SM, FI.Range);
            J.attributeEnd();
            J.attribute("text", llvm::json::fixUTF8(FI.Text));
          });
        }
      });
    }
  });
  OS << '\n';
}

//===----------------------------------------------------------------------===//
// Applying fix-its
//===----------------------------------------------------------------------===//

namespace {
struct Edit {
  uint32_t Begin, End;
  const std::string *Text;
};
} // namespace

std::string gopp::applyFixIts(const SourceManager &SM, FileID File,
                              llvm::ArrayRef<Diagnostic> Diags,
                              unsigned *NumApplied) {
  llvm::StringRef Src = SM.getBuffer(File);
  std::vector<Edit> Accepted; // in acceptance order
  unsigned Applied = 0;
  for (const Diagnostic &D : Diags) {
    std::vector<Edit> Mine;
    bool Ok = !D.FixIts.empty();
    for (const FixIt &FI : D.FixIts) {
      if (!FI.Range.isValid() || SM.getFileID(FI.Range.getBegin()) != File ||
          SM.getFileID(FI.Range.getEnd()) != File ||
          FI.Range.getEnd() < FI.Range.getBegin()) {
        Ok = false;
        break;
      }
      Mine.push_back({SM.getFileOffset(FI.Range.getBegin()),
                      SM.getFileOffset(FI.Range.getEnd()), &FI.Text});
    }
    // Two edits clash if their ranges intersect; two insertions at one point
    // do not (they apply in order).
    auto Clash = [](const Edit &A, const Edit &B) {
      if (A.Begin == A.End && B.Begin == B.End)
        return false;
      return A.Begin < B.End && B.Begin < A.End;
    };
    for (size_t I = 0; Ok && I < Mine.size(); ++I) {
      for (const Edit &E : Accepted)
        if (Clash(Mine[I], E))
          Ok = false;
      for (size_t J = 0; Ok && J < I; ++J)
        if (Clash(Mine[I], Mine[J]))
          Ok = false;
    }
    if (!Ok)
      continue;
    Accepted.insert(Accepted.end(), Mine.begin(), Mine.end());
    ++Applied;
  }
  std::stable_sort(Accepted.begin(), Accepted.end(),
                   [](const Edit &A, const Edit &B) {
                     if (A.Begin != B.Begin)
                       return A.Begin < B.Begin;
                     // an insertion goes before a replacement that starts at
                     // the same place
                     return (A.End == A.Begin) > (B.End == B.Begin);
                   });
  std::string Out;
  uint32_t Pos = 0;
  for (const Edit &E : Accepted) {
    Out.append(Src.data() + Pos, E.Begin - Pos);
    Out += *E.Text;
    Pos = E.End;
  }
  Out.append(Src.data() + Pos, Src.size() - Pos);
  if (NumApplied)
    *NumApplied = Applied;
  return Out;
}
