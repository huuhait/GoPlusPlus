//===- Printer.cpp - The Go++ pretty printer ------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A port of go/printer (printer.go, nodes.go) that works on the Go++ AST:
//   Copyright 2009 The Go Authors. All rights reserved.
//   Use of this source code is governed by a BSD-style license (see NOTICE).
// The layout decisions (whitespace buffer, comment interspersing, expression
// list alignment, binary expression depth, one-line function bodies, sizes by
// trial printing) are the same, so that Go code comes out as gofmt prints it.
// The Go++ forms (ref types, `move` parameters, `!`, the range `&`, attributes,
// macros) are added where the grammar puts them.
//
// Differences from Go: no doc comment reformatting (go/doc/comment), no
// //go:build fixes, and no "incomplete" (filtered) nodes. Where Go reads
// ast.Field.Doc/Comment, this port looks the comment groups of the parser up by
// position (hasDoc, hasLineComment).
//
//===----------------------------------------------------------------------===//

#include "TabWriter.h"

#include "gopp/AST/ASTDumper.h"
#include "gopp/Format/Format.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cmath>

using namespace gopp;
using namespace gopp::ast;
using namespace gopp::format;
using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::StringRef;

namespace {

constexpr int MaxNewlines = 2;
constexpr int Infinity = 1 << 30;

enum class WS : char {
  Ignore = 0,
  Blank = ' ',
  VTab = '\v',
  Newline = '\n',
  Formfeed = '\f',
  Indent = '>',
  Unindent = '<',
};
constexpr WS ignore = WS::Ignore;
constexpr WS blank = WS::Blank;
constexpr WS vtab = WS::VTab;
constexpr WS newline = WS::Newline;
constexpr WS formfeed = WS::Formfeed;
constexpr WS indent = WS::Indent;
constexpr WS unindent = WS::Unindent;

enum class PMode : unsigned {
  NoExtraBlank = 1,     // disables extra blank after /*-style comment
  NoExtraLinebreak = 2, // disables extra line break after /*-style comment
};
constexpr PMode noExtraBlank = PMode::NoExtraBlank;
constexpr PMode noExtraLinebreak = PMode::NoExtraLinebreak;

/// A word printed like an identifier (the contextual keywords `ref`, `move`).
struct Word {
  StringRef Text;
};

enum ExprListMode : unsigned {
  commaTerm = 1, // list is optionally terminated by a comma
  noIndent = 2,  // no extra indentation in multi-line lists
};

enum class ParamMode { funcParam, funcTParam, typeTParam };

struct Position {
  uint32_t Offset = 0;
  int Line = 0;
  int Column = 0;
  bool isValid() const { return Line > 0; }
};

constexpr int LowestPrec = 0;
constexpr int UnaryPrec = 6;
constexpr int HighestPrec = 7;

int precedence(tok::Kind K) {
  switch (K) {
  case tok::lor:
    return 1;
  case tok::land:
    return 2;
  case tok::eql:
  case tok::neq:
  case tok::lss:
  case tok::leq:
  case tok::gtr:
  case tok::geq:
    return 3;
  case tok::add:
  case tok::sub:
  case tok::or_:
  case tok::xor_:
    return 4;
  case tok::mul:
  case tok::quo:
  case tok::rem:
  case tok::shl:
  case tok::shr:
  case tok::and_:
  case tok::and_not:
    return 5;
  default:
    return LowestPrec;
  }
}

StringRef spelling(tok::Kind K) {
  const char *S = tok::getSpelling(K);
  return S ? S : "";
}

bool isSpaceRune(uint32_t R) {
  switch (R) {
  case ' ':
  case '\t':
  case '\n':
  case '\v':
  case '\f':
  case '\r':
  case 0x85:
  case 0xA0:
  case 0x1680:
  case 0x2028:
  case 0x2029:
  case 0x202f:
  case 0x205f:
  case 0x3000:
    return true;
  default:
    return R >= 0x2000 && R <= 0x200a;
  }
}

/// strings.TrimRightFunc(s, unicode.IsSpace)
StringRef trimRight(StringRef S) {
  for (;;) {
    if (S.empty())
      return S;
    unsigned char C = S.back();
    if (C < 0x80) {
      if (!isSpaceRune(C))
        return S;
      S = S.drop_back();
      continue;
    }
    // decode the last UTF-8 sequence
    size_t I = S.size() - 1;
    while (I > 0 && (static_cast<unsigned char>(S[I]) & 0xC0) == 0x80 &&
           S.size() - I < 4)
      --I;
    unsigned char L = S[I];
    uint32_t R = 0;
    size_t Len = S.size() - I;
    if (Len == 2 && (L & 0xE0) == 0xC0)
      R = (L & 0x1F) << 6 | (S[I + 1] & 0x3F);
    else if (Len == 3 && (L & 0xF0) == 0xE0)
      R = (L & 0x0F) << 12 | (S[I + 1] & 0x3F) << 6 | (S[I + 2] & 0x3F);
    else
      return S;
    if (!isSpaceRune(R))
      return S;
    S = S.drop_back(Len);
  }
}

/// Go's stripCR: removes carriage returns from a comment or raw string (except
/// the one between `*` and `/` that would end a comment too early).
std::string stripCR(StringRef B, bool Comment) {
  std::string C;
  C.reserve(B.size());
  for (size_t J = 0; J < B.size(); ++J) {
    char Ch = B[J];
    if (Ch != '\r' || (Comment && C.size() > 2 && C.back() == '*' &&
                       J + 1 < B.size() && B[J + 1] == '/'))
      C.push_back(Ch);
  }
  return C;
}

int runeCount(StringRef S) {
  int N = 0;
  for (unsigned char C : S)
    if ((C & 0xC0) != 0x80)
      ++N;
  return N;
}

bool isBlankStr(StringRef S) {
  for (unsigned char C : S)
    if (C > ' ')
      return false;
  return true;
}

/// commonPrefix returns the common prefix of a and b.
StringRef commonPrefix(StringRef A, StringRef B) {
  size_t I = 0;
  while (I < A.size() && I < B.size() && A[I] == B[I] &&
         (static_cast<unsigned char>(A[I]) <= ' ' || A[I] == '*'))
    ++I;
  return A.substr(0, I);
}

/// The trimmer of go/printer: strips Escape bytes and trailing blanks and tabs,
/// converts formfeed to newline and vtab to tab.
std::string trim(StringRef Data) {
  enum { InSpace, InEscape, InText } State = InSpace;
  std::string Out, Space;
  size_t M = 0;
  for (size_t N = 0; N < Data.size(); ++N) {
    char B = Data[N];
    if (B == '\v')
      B = '\t';
    switch (State) {
    case InSpace:
      switch (B) {
      case '\t':
      case ' ':
        Space.push_back(B);
        break;
      case '\n':
      case '\f':
        Space.clear();
        Out.push_back('\n');
        break;
      case Escape:
        Out += Space;
        State = InEscape;
        M = N + 1;
        break;
      default:
        Out += Space;
        State = InText;
        M = N;
        break;
      }
      break;
    case InEscape:
      if (B == Escape) {
        Out.append(Data.data() + M, N - M);
        Space.clear();
        State = InSpace;
      }
      break;
    case InText:
      switch (B) {
      case '\t':
      case ' ':
        Out.append(Data.data() + M, N - M);
        Space.clear();
        State = InSpace;
        Space.push_back(B);
        break;
      case '\n':
      case '\f':
        Out.append(Data.data() + M, N - M);
        Space.clear();
        State = InSpace;
        Out.push_back('\n');
        break;
      case Escape:
        Out.append(Data.data() + M, N - M);
        State = InEscape;
        M = N + 1;
        break;
      default:
        break;
      }
      break;
    }
  }
  if (State != InSpace)
    Out.append(Data.data() + M, Data.size() - M);
  return Out;
}

/// A comment of the source: Text without carriage returns.
struct PComment {
  uint32_t Offset = 0;
  Position Pos;
  std::string Text;
};

struct PGroup {
  size_t First = 0, Count = 0;
};

struct Shared {
  const SourceManager *SM = nullptr;
  FileID File;
  StringRef Src;
  const ast::File *AST = nullptr;
  llvm::DenseMap<const Node *, int> NodeSizes;
};

class Printer {
public:
  Printer(Shared &Sh, bool Raw, bool WithComments)
      : Sh(Sh), SM(*Sh.SM), Raw(Raw) {
    pos = {0, 1, 1};
    out = {0, 1, 1};
    if (WithComments)
      loadComments();
    nextComment();
    print(PMode(0));
  }

  std::string finish();
  void printNode(const Node *N);

private:
  //===--------------------------------------------------------------------===//
  // Positions and comments
  //===--------------------------------------------------------------------===//

  Position posFor(SourceLoc L) const {
    Position P;
    if (!L.isValid())
      return P;
    PresumedLoc PL = SM.getPresumedLoc(L);
    P.Offset = SM.getFileOffset(L);
    P.Line = PL.Line;
    P.Column = PL.Column;
    return P;
  }

  int lineFor(SourceLoc L) {
    if (!L.isValid())
      return 0;
    if (L.getRaw() != CachedPos) {
      CachedPos = L.getRaw();
      CachedLine = SM.getLineNumber(L);
    }
    return CachedLine;
  }

  void loadComments() {
    const ast::File *F = Sh.AST;
    if (!F)
      return;
    Cmts.reserve(F->Comments.size());
    for (const Comment &C : F->Comments) {
      PComment PC;
      PC.Pos = posFor(C.Loc);
      PC.Offset = PC.Pos.Offset;
      PC.Text = stripCR(Sh.Src.substr(PC.Offset, C.Length), true);
      Cmts.push_back(std::move(PC));
    }
    for (const CommentGroup &G : F->CommentGroups)
      Groups.push_back({G.First, G.Count});
  }

  const PComment &firstOf(const PGroup &G) const { return Cmts[G.First]; }
  const PComment &lastOf(const PGroup &G) const {
    return Cmts[G.First + G.Count - 1];
  }

  /// Offset of the end of the comment (Go: Slash + len(Text)).
  static uint32_t endOffset(const PComment &C) {
    return C.Offset + C.Text.size();
  }

  bool commentsHaveNewline(const PGroup &G) {
    int Line = firstOf(G).Pos.Line;
    for (size_t I = 0; I < G.Count; ++I) {
      const PComment &C = Cmts[G.First + I];
      if (I > 0 && C.Pos.Line != Line)
        return true; // not all comments on the same line
      StringRef T = C.Text;
      if (T.size() >= 2 && (T[1] == '/' || T.contains('\n')))
        return true;
    }
    return false;
  }

  void nextComment() {
    while (cindex < Groups.size()) {
      const PGroup &G = Groups[cindex++];
      if (G.Count > 0) {
        comment = &G;
        commentOffset = firstOf(G).Offset;
        commentNewline = commentsHaveNewline(G);
        return;
      }
    }
    commentOffset = Infinity;
    comment = nullptr;
  }

  struct CommentInfo {
    size_t cindex;
    const PGroup *comment;
    int64_t commentOffset;
    bool commentNewline;
  };
  CommentInfo saveComment() const {
    return {cindex, comment, commentOffset, commentNewline};
  }
  void restoreComment(const CommentInfo &I) {
    cindex = I.cindex;
    comment = I.comment;
    commentOffset = I.commentOffset;
    commentNewline = I.commentNewline;
  }

  bool commentBefore(const Position &Next) const {
    return commentOffset < (int64_t)Next.Offset &&
           (!impliedSemi || !commentNewline);
  }

  int commentSizeBefore(const Position &Next) {
    CommentInfo Saved = saveComment();
    int Size = 0;
    while (commentBefore(Next)) {
      for (size_t I = 0; I < comment->Count; ++I)
        Size += Cmts[comment->First + I].Text.size();
      nextComment();
    }
    restoreComment(Saved);
    return Size;
  }

  /// The doc comment of the construct that starts at Begin.
  bool hasDoc(SourceLoc Begin) const {
    const ast::File *F = Sh.AST;
    if (!F)
      return false;
    ArrayRef<CommentGroup> Gs = F->CommentGroups;
    auto It = std::lower_bound(
        Gs.begin(), Gs.end(), Begin,
        [&](const CommentGroup &G, SourceLoc L) { return G.Next < L; });
    for (; It != Gs.end() && It->Next == Begin; ++It)
      if (It->Lead)
        return true;
    return false;
  }

  /// The line comment after a construct that ends at End (Go: Field.Comment,
  /// ValueSpec.Comment, ...): a comment group that starts on the line of the
  /// last token, behind at most one ';', and is followed by a later line.
  bool hasLineComment(SourceLoc End) const {
    const ast::File *F = Sh.AST;
    if (!F || !End.isValid())
      return false;
    uint32_t EndOff = SM.getFileOffset(End);
    ArrayRef<CommentGroup> Gs = F->CommentGroups;
    auto It = std::lower_bound(
        Gs.begin(), Gs.end(), EndOff, [&](const CommentGroup &G, uint32_t Off) {
          return SM.getFileOffset(F->Comments[G.First].Loc) < Off;
        });
    if (It == Gs.end() || !It->Line)
      return false;
    uint32_t CommentOff = SM.getFileOffset(F->Comments[It->First].Loc);
    StringRef Between = Sh.Src.substr(EndOff, CommentOff - EndOff);
    if (Between.contains('\n'))
      return false;
    Between = Between.ltrim(" \t");
    if (Between.starts_with(";"))
      Between = Between.drop_front().ltrim(" \t");
    return Between.empty();
  }

  //===--------------------------------------------------------------------===//
  // Low-level output
  //===--------------------------------------------------------------------===//

  void writeIndent() {
    // use "hard" htabs - indentation columns must not be discarded by the
    // tabwriter
    int N = indentLevel;
    output.append(N, '\t');
    pos.Offset += N;
    pos.Column += N;
    out.Column += N;
  }

  void writeByte(char Ch, int N) {
    if (endAlignment) {
      // Ignore any alignment control character; and at the end of the line,
      // break with a formfeed to indicate termination of existing columns.
      switch (Ch) {
      case '\t':
      case '\v':
        Ch = ' ';
        break;
      case '\n':
      case '\f':
        Ch = '\f';
        endAlignment = false;
        break;
      }
    }
    if (out.Column == 1)
      writeIndent();
    output.append(N, Ch);
    pos.Offset += N;
    if (Ch == '\n' || Ch == '\f') {
      pos.Line += N;
      out.Line += N;
      pos.Column = 1;
      out.Column = 1;
      return;
    }
    pos.Column += N;
    out.Column += N;
  }

  void writeString(const Position &P, StringRef S, bool IsLit) {
    if (out.Column == 1)
      writeIndent();
    if (P.isValid())
      pos = P; // update pos (if P is invalid, continue with existing pos)
    if (IsLit)
      output.push_back(Escape);
    output.append(S.data(), S.size());
    int NLines = 0;
    size_t LI = 0;
    for (size_t I = 0; I < S.size(); ++I) {
      if (S[I] == '\n') {
        // Only a newline moves the position: the scanner does not count a
        // formfeed inside a literal or comment as a line (go/printer does, and
        // loses its line count).
        ++NLines;
        LI = I;
      }
      if (S[I] == '\n' || S[I] == '\f') {
        // A line break inside a literal will break whatever column formatting
        // is in place; ignore any further alignment through the end of the
        // line.
        endAlignment = true;
      }
    }
    pos.Offset += S.size();
    if (NLines > 0) {
      pos.Line += NLines;
      out.Line += NLines;
      int C = (int)(S.size() - LI);
      pos.Column = C;
      out.Column = C;
    } else {
      pos.Column += S.size();
      out.Column += S.size();
    }
    if (IsLit)
      output.push_back(Escape);
    last = pos;
  }

  void writeCommentPrefix(const Position &P, const Position &Next,
                          const PComment *Prev, tok::Kind Tok) {
    if (output.empty())
      return; // the comment is the first item to be printed

    if (P.Line == last.Line && (!Prev || Prev->Text[1] != '/')) {
      // comment on the same line as last item: separate with at least one
      // separator
      bool HasSep = false;
      if (!Prev) {
        // first comment of a comment group
        size_t J = 0;
        for (size_t I = 0; I < wsbuf.size(); ++I) {
          WS Ch = wsbuf[I];
          if (Ch == blank) {
            wsbuf[I] = ignore; // ignore any blanks before a comment
            continue;
          }
          if (Ch == vtab) {
            // respect existing tabs - important for proper formatting of
            // commented structs
            HasSep = true;
            continue;
          }
          if (Ch == indent)
            continue; // apply pending indentation
          J = I;
          break;
        }
        writeWhitespace(J);
      }
      // make sure there is at least one separator
      if (!HasSep) {
        char Sep = '\t';
        if (P.Line == Next.Line) {
          // next item is on the same line as the comment (which must be a
          // /*-style comment): separate with a blank instead of a tab
          Sep = ' ';
        }
        writeByte(Sep, 1);
      }
    } else {
      // comment on a different line: separate with at least one line break
      bool DroppedLinebreak = false;
      size_t J = 0;
      for (size_t I = 0; I < wsbuf.size(); ++I) {
        WS Ch = wsbuf[I];
        bool Continue = false;
        switch (Ch) {
        case WS::Blank:
        case WS::VTab:
          // ignore any horizontal whitespace before line breaks
          wsbuf[I] = ignore;
          Continue = true;
          break;
        case WS::Indent:
          Continue = true; // apply pending indentation
          break;
        case WS::Unindent:
          // if this is not the last unindent, apply it as it is (likely)
          // belonging to the last construct (e.g., a multi-line expression
          // list) and is not part of closing a block
          if (I + 1 < wsbuf.size() && wsbuf[I + 1] == unindent) {
            Continue = true;
            break;
          }
          // if the next token is not a closing }, apply the unindent if it
          // appears that the comment is aligned with the token; otherwise
          // assume the unindent is part of a closing block and stop (this
          // scenario appears with comments before a case label where the
          // comments apply to the next case instead of the current one)
          if (Tok != tok::r_brace && P.Column == Next.Column) {
            Continue = true;
            break;
          }
          break;
        case WS::Newline:
        case WS::Formfeed:
          wsbuf[I] = ignore;
          DroppedLinebreak = Prev == nullptr; // only if first comment of group
          break;
        default:
          break;
        }
        if (Continue)
          continue;
        J = I;
        break;
      }
      writeWhitespace(J);

      // determine number of linebreaks before the comment
      int N = 0;
      if (P.isValid() && last.isValid()) {
        N = P.Line - last.Line;
        if (N < 0)
          N = 0;
      }

      // at the package scope level only (indent == 0), add an extra newline if
      // we dropped one before: this preserves a blank line before
      // documentation comments at the package scope level (issue 2570)
      if (indentLevel == 0 && DroppedLinebreak)
        ++N;

      // make sure there is at least one line break if the previous comment was
      // a line comment
      if (N == 0 && Prev && Prev->Text[1] == '/')
        N = 1;

      if (N > 0) {
        // use formfeeds to break columns before a comment; this is analogous to
        // using formfeeds to separate individual lines of /*-style comments
        writeByte('\f', nlimit(N));
      }
    }
  }

  static int nlimit(int N) { return std::min(N, MaxNewlines); }

  /// stripCommonPrefix removes a common prefix from /*-style comment lines
  /// (unless no comment line is indented, all but the first line have some
  /// form of space prefix).
  static void stripCommonPrefix(std::vector<std::string> &Lines) {
    if (Lines.size() <= 1)
      return; // at most one line - nothing to do

    std::string Prefix;
    bool PrefixSet = false;
    if (Lines.size() > 2) {
      for (size_t I = 1; I + 1 < Lines.size(); ++I) {
        if (isBlankStr(Lines[I])) {
          Lines[I].clear(); // replace blank lines with empty lines
        } else {
          if (!PrefixSet) {
            Prefix = Lines[I];
            PrefixSet = true;
          }
          Prefix = commonPrefix(Prefix, Lines[I]).str();
        }
      }
    }
    // If we don't have a prefix yet, consider the last line.
    if (!PrefixSet) {
      const std::string &L = Lines.back();
      Prefix = commonPrefix(L, L).str();
    }

    // Check for vertical "line of stars" and correct prefix accordingly.
    bool LineOfStars = false;
    size_t Star = Prefix.find('*');
    if (Star != std::string::npos) {
      // remove trailing blank from prefix so stars remain aligned
      Prefix = Prefix.substr(0, Star);
      if (!Prefix.empty() && Prefix.back() == ' ')
        Prefix.pop_back();
      LineOfStars = true;
    } else {
      // No line of stars present. Determine the white space on the first line
      // after the /* and before the beginning of the comment text, assume two
      // blanks instead of the /* unless the first character after the /* is a
      // tab. If the first comment line is empty but for the opening /*, assume
      // up to 3 blanks or a tab. This whitespace may be found as suffix in the
      // common prefix.
      const std::string &First = Lines[0];
      if (isBlankStr(StringRef(First).substr(2))) {
        // no comment text on the first line: reduce prefix by up to 3 blanks
        // or a tab if present - this keeps comment text indented relative to
        // the /* and */'s if it was indented in the first place
        size_t I = Prefix.size();
        for (int N = 0; N < 3 && I > 0 && Prefix[I - 1] == ' '; ++N)
          --I;
        if (I == Prefix.size() && I > 0 && Prefix[I - 1] == '\t')
          --I;
        Prefix = Prefix.substr(0, I);
      } else {
        // comment text on the first line
        std::string Suffix(First.size(), '\0');
        size_t N = 2; // start after opening /*
        while (N < First.size() && (unsigned char)First[N] <= ' ') {
          Suffix[N] = First[N];
          ++N;
        }
        std::string S;
        if (N > 2 && Suffix[2] == '\t') {
          // assume the '\t' compensates for the /*
          S = Suffix.substr(2, N - 2);
        } else {
          // otherwise assume two blanks
          Suffix[0] = ' ';
          Suffix[1] = ' ';
          S = Suffix.substr(0, N);
        }
        // Shorten the computed common prefix by the length of suffix, if it is
        // found as suffix of the prefix.
        if (StringRef(Prefix).ends_with(S))
          Prefix.resize(Prefix.size() - S.size());
      }
    }

    // Handle last line: If it only contains a closing */, align it with the
    // opening /*, otherwise align the text with the other lines.
    std::string &Last = Lines.back();
    std::string Closing = "*/";
    size_t Cl = Last.find(Closing); // closing always present
    StringRef Before = StringRef(Last).substr(0, Cl);
    if (isBlankStr(Before)) {
      // last line only contains closing */
      if (LineOfStars)
        Closing = " */"; // add blank to align final star
      Last = Prefix + Closing;
    } else {
      // last line contains more comment text - assume it is aligned like the
      // other lines and include in prefix computation
      Prefix = commonPrefix(Prefix, Last).str();
    }

    // Remove the common prefix from all but the first and empty lines.
    for (size_t I = 1; I < Lines.size(); ++I)
      if (!Lines[I].empty())
        Lines[I] = Lines[I].substr(Prefix.size());
  }

  void writeComment(const PComment &C) {
    StringRef Text = C.Text;
    Position P = C.Pos;

    const StringRef LinePrefix = "//line ";
    int SavedIndent = indentLevel;
    bool Restore = false;
    if (Text.starts_with(LinePrefix) && (!P.isValid() || P.Column == 1)) {
      // Possibly a //-style line directive. Suspend indentation temporarily to
      // keep line directive valid.
      Restore = true;
      indentLevel = 0;
    }

    // shortcut common case of //-style comments
    if (Text[1] == '/') {
      writeString(P, trimRight(Text), true);
      if (Restore)
        indentLevel = SavedIndent;
      return;
    }

    // for /*-style comments, print line by line and let the write function take
    // care of the proper indentation
    std::vector<std::string> Lines;
    {
      size_t B = 0;
      for (;;) {
        size_t E = Text.find('\n', B);
        if (E == StringRef::npos) {
          Lines.push_back(Text.substr(B).str());
          break;
        }
        Lines.push_back(Text.substr(B, E - B).str());
        B = E + 1;
      }
    }

    // The comment started in the first column but is going to be indented. For
    // an idempotent result, add indentation to all lines such that they look
    // like they were indented before - this will make sure the common prefix
    // computation is the same independent of how many times formatting is
    // applied (was issue 1835).
    if (P.isValid() && P.Column == 1 && indentLevel > 0)
      for (size_t I = 1; I < Lines.size(); ++I)
        Lines[I] = "   " + Lines[I];

    stripCommonPrefix(Lines);

    // write comment lines, separated by formfeed, without a line break after
    // the last line
    for (size_t I = 0; I < Lines.size(); ++I) {
      if (I > 0) {
        writeByte('\f', 1);
        P = pos;
      }
      if (!Lines[I].empty())
        writeString(P, trimRight(Lines[I]), true);
    }
    if (Restore)
      indentLevel = SavedIndent;
  }

  /// Writes a line break after a comment if indicated and processes any
  /// leftover indentation information. Returns {wroteNewline, droppedFF}.
  std::pair<bool, bool> writeCommentSuffix(bool NeedsLinebreak) {
    bool WroteNewline = false, DroppedFF = false;
    for (size_t I = 0; I < wsbuf.size(); ++I) {
      switch (wsbuf[I]) {
      case WS::Blank:
      case WS::VTab:
        wsbuf[I] = ignore; // ignore trailing whitespace
        break;
      case WS::Indent:
      case WS::Unindent:
        break; // don't lose indentation information
      case WS::Newline:
      case WS::Formfeed:
        // if we need a line break, keep exactly one but remember if we dropped
        // any formfeeds
        if (NeedsLinebreak) {
          NeedsLinebreak = false;
          WroteNewline = true;
        } else {
          if (wsbuf[I] == formfeed)
            DroppedFF = true;
          wsbuf[I] = ignore;
        }
        break;
      default:
        break;
      }
    }
    writeWhitespace(wsbuf.size());
    // make sure we have a line break
    if (NeedsLinebreak) {
      writeByte('\n', 1);
      WroteNewline = true;
    }
    return {WroteNewline, DroppedFF};
  }

  bool containsLinebreak() const {
    for (WS Ch : wsbuf)
      if (Ch == newline || Ch == formfeed)
        return true;
    return false;
  }

  /// Consumes all comments that appear before the next token Tok and prints
  /// them together with the buffered whitespace (see go/printer).
  std::pair<bool, bool> intersperseComments(const Position &Next,
                                            tok::Kind Tok) {
    const PComment *Last = nullptr;
    while (commentBefore(Next)) {
      const PGroup &G = *comment;
      for (size_t I = 0; I < G.Count; ++I) {
        const PComment &C = Cmts[G.First + I];
        writeCommentPrefix(C.Pos, Next, Last, Tok);
        writeComment(C);
        Last = &C;
      }
      nextComment();
    }

    if (Last) {
      // If the last comment is a /*-style comment and the next item follows on
      // the same line but is not a comma, and not a "closing" token immediately
      // following its corresponding "opening" token, add an extra separator
      // unless explicitly disabled. Use a blank as separator unless we have
      // pending linebreaks, they are not disabled, and we are outside a
      // composite literal, in which case we want a linebreak (issue 15137).
      bool NeedsLinebreak = false;
      if (!(mode & (unsigned)noExtraBlank) && Last->Text[1] == '*' &&
          Last->Pos.Line == Next.Line && Tok != tok::comma &&
          (Tok != tok::r_paren || prevOpen == tok::l_paren) &&
          (Tok != tok::r_brack || prevOpen == tok::l_brack)) {
        if (containsLinebreak() && !(mode & (unsigned)noExtraLinebreak) &&
            level == 0) {
          NeedsLinebreak = true;
        } else {
          writeByte(' ', 1);
        }
      }
      // Ensure that there is a line break after a //-style comment, before EOF,
      // and before a closing '}' unless explicitly disabled.
      if (Last->Text[1] == '/' || Tok == tok::eof ||
          (Tok == tok::r_brace && !(mode & (unsigned)noExtraLinebreak)))
        NeedsLinebreak = true;
      return writeCommentSuffix(NeedsLinebreak);
    }
    return {false, false};
  }

  void writeWhitespace(size_t N) {
    for (size_t I = 0; I < N; ++I) {
      WS Ch = wsbuf[I];
      switch (Ch) {
      case WS::Ignore:
        break;
      case WS::Indent:
        ++indentLevel;
        break;
      case WS::Unindent:
        --indentLevel;
        if (indentLevel < 0)
          indentLevel = 0;
        break;
      case WS::Newline:
      case WS::Formfeed:
        // A line break immediately followed by a "correcting" unindent is
        // swapped with the unindent - this permits proper label positioning. If
        // a comment is between the line break and the label, the unindent is
        // not part of the comment whitespace prefix and the comment will be
        // positioned correctly indented.
        if (I + 1 < N && wsbuf[I + 1] == unindent) {
          // Use a formfeed to terminate the current section. Otherwise, a long
          // label name on the next line leading to a wide column may increase
          // the indentation column of lines before the label; effectively
          // leading to wrong indentation.
          wsbuf[I] = unindent;
          wsbuf[I + 1] = formfeed;
          --I; // do it again (the loop increment undoes this)
          continue;
        }
        writeByte((char)Ch, 1);
        break;
      default:
        writeByte((char)Ch, 1);
        break;
      }
    }
    // shift remaining entries down
    wsbuf.erase(wsbuf.begin(), wsbuf.begin() + N);
  }

  //===--------------------------------------------------------------------===//
  // print
  //===--------------------------------------------------------------------===//

  static bool mayCombine(tok::Kind Prev, char Next) {
    switch (Prev) {
    case tok::int_lit:
      return Next == '.'; // 1.
    case tok::add:
      return Next == '+'; // ++
    case tok::sub:
      return Next == '-'; // --
    case tok::quo:
      return Next == '*'; // /*
    case tok::lss:
      return Next == '-' || Next == '<'; // <- or <<
    case tok::and_:
      return Next == '&' || Next == '^'; // && or &^
    default:
      return false;
    }
  }

  void setPos(SourceLoc L) {
    if (L.isValid())
      pos = posFor(L); // accurate position of next item
  }

  void beginArg() {
    // record previous opening token, if any
    switch (lastTok) {
    case tok::illegal:
      break; // ignore (white space)
    case tok::l_paren:
    case tok::l_brack:
      prevOpen = lastTok;
      break;
    default:
      // other tokens followed any opening token
      prevOpen = tok::illegal;
      break;
    }
  }

  void printOne(PMode M) {
    beginArg();
    mode ^= (unsigned)M;
  }

  void printOne(WS X) {
    beginArg();
    if (X == ignore)
      return; // don't add ignore's to the buffer; they may screw up
              // "correcting" unindents (see LabeledStmt)
    if (wsbuf.size() == 16) {
      // Whitespace sequences are very short so this should never happen.
      // Handle gracefully (but possibly with bad comment placement).
      writeWhitespace(wsbuf.size());
    }
    wsbuf.push_back(X);
    if (X == newline || X == formfeed) {
      // newlines affect the current state (impliedSemi) and not the state
      // after printing arg (impliedSemi) because comments can be interspersed
      // before the arg in this case
      impliedSemi = false;
    }
    lastTok = tok::illegal;
  }

  void printOne(const Ident *X) {
    beginArg();
    emit(X->Name.str(), false, true, tok::ident);
  }

  void printOne(Word W) {
    beginArg();
    emit(W.Text, false, true, tok::ident);
  }

  void printOne(const BasicLit *X) {
    beginArg();
    if (!Raw && isNumber(X->LitKind)) {
      std::string N = normalizedNumber(X->Text);
      emit(N, true, true, X->LitKind);
      return;
    }
    if (X->LitKind == tok::string_lit && X->Text.starts_with("`") &&
        X->Text.contains('\r')) {
      std::string S = stripCR(X->Text, false);
      emit(S, true, true, X->LitKind);
      return;
    }
    emit(X->Text, true, true, X->LitKind);
  }

  /// A string that is printed as is (e.g. an import path rewritten by the
  /// printer).
  void printLit(StringRef Text, tok::Kind Kind) {
    beginArg();
    emit(Text, true, true, Kind);
  }

  void printOne(tok::Kind K) {
    beginArg();
    StringRef S = spelling(K);
    if (mayCombine(lastTok, S[0])) {
      // the previous and the current token must be separated by a blank
      // otherwise they combine into a different incorrect token sequence
      wsbuf.resize(1);
      wsbuf[0] = blank;
    }
    bool Implied = false;
    // some keywords followed by a newline imply a semicolon
    switch (K) {
    case tok::kw_break:
    case tok::kw_continue:
    case tok::kw_fallthrough:
    case tok::kw_return:
    case tok::inc:
    case tok::dec:
    case tok::r_paren:
    case tok::r_brack:
    case tok::r_brace:
      Implied = true;
      break;
    default:
      break;
    }
    emit(S, false, Implied, K);
  }

  /// Message text for an incorrect AST (never reached for parsed trees).
  void printOne(const char *S) {
    beginArg();
    emit(S, true, true, tok::string_lit);
  }

  template <class... Args> void print(Args... As) { (printOne(As), ...); }

  static bool isNumber(tok::Kind K) {
    return K == tok::int_lit || K == tok::float_lit || K == tok::imag_lit;
  }

  void emit(StringRef Data, bool IsLit, bool ImpliedSemi, tok::Kind Tok) {
    lastTok = Tok;
    Position Next = pos; // estimated/accurate position of next item
    auto [WroteNewline, DroppedFF] = flush(Next, lastTok);

    // intersperse extra newlines if present in the source and if they don't
    // cause extra semicolons (don't do this in flush as it will cause extra
    // newlines at the end of a file)
    if (!impliedSemi) {
      int N = nlimit(Next.Line - pos.Line);
      // don't exceed maxNewlines if we already wrote one
      if (WroteNewline && N == MaxNewlines)
        N = MaxNewlines - 1;
      if (N > 0) {
        char Ch = '\n';
        if (DroppedFF)
          Ch = '\f'; // use formfeed since we dropped one before
        writeByte(Ch, N);
        ImpliedSemi = false;
      }
    }

    // the next token starts now - record its line number if requested
    if (linePtr) {
      *linePtr = out.Line;
      linePtr = nullptr;
    }

    writeString(Next, Data, IsLit);
    impliedSemi = ImpliedSemi;
  }

  /// Prints any pending comments and whitespace occurring textually before the
  /// position of the next token Tok.
  std::pair<bool, bool> flush(const Position &Next, tok::Kind Tok) {
    if (commentBefore(Next)) {
      // if there are comments before the next item, intersperse them
      return intersperseComments(Next, Tok);
    }
    // otherwise, write any leftover whitespace
    writeWhitespace(wsbuf.size());
    return {false, false};
  }

  //===--------------------------------------------------------------------===//
  // Common AST nodes
  //===--------------------------------------------------------------------===//

  /// Prints as many newlines as necessary (but at least Min newlines) to get to
  /// the current line. Ws is printed before the first line break. If NewSection
  /// is set, the first line break is printed as formfeed. Returns 0 if no line
  /// breaks were printed, returns 1 if there was exactly one newline printed,
  /// and returns a value > 1 if there was a formfeed or more than one newline
  /// printed.
  int linebreak(int Line, int Min, WS Ws, bool NewSection) {
    int N = std::max(nlimit(Line - pos.Line), Min);
    int NBreaks = 0;
    if (N > 0) {
      print(Ws);
      if (NewSection) {
        print(formfeed);
        --N;
        NBreaks = 2;
      }
      NBreaks += N;
      for (; N > 0; --N)
        print(newline);
    }
    return NBreaks;
  }

  /// If Indent is set, a multi-line identifier list is indented after the first
  /// linebreak encountered.
  void identList(ArrayRef<Ident *> List, bool Indent) {
    llvm::SmallVector<const Expr *, 8> XList(List.begin(), List.end());
    unsigned M = Indent ? 0 : (unsigned)noIndent;
    exprList(SourceLoc(), XList, 1, M, SourceLoc());
  }

  /// Prints a list of expressions. If the list spans multiple source lines, the
  /// original line breaks are respected between expressions.
  void exprList(SourceLoc Prev0, ArrayRef<const Expr *> List, int Depth,
                unsigned M, SourceLoc Next0) {
    if (List.empty())
      return;

    Position Prev = posFor(Prev0);
    Position Next = posFor(Next0);
    int Line = lineFor(List[0]->Begin);
    int EndLine = lineFor(List.back()->End);

    if (Prev.isValid() && Prev.Line == Line && Line == EndLine) {
      // all list entries on a single line
      for (size_t I = 0; I < List.size(); ++I) {
        if (I > 0) {
          // use position of expression following the comma as comma position
          // for correct comment placement
          setPos(List[I]->Begin);
          print(tok::comma, blank);
        }
        expr0(List[I], Depth);
      }
      return;
    }

    // list entries span multiple lines; use source code positions to guide line
    // breaks

    // Don't add extra indentation if noIndent is set; i.e., pretend that the
    // first line is already indented.
    WS Ws = ignore;
    if (!(M & noIndent))
      Ws = indent;

    // The first linebreak is always a formfeed since this section must not
    // depend on any previous formatting.
    int PrevBreak =
        -1; // index of last expression that was followed by a linebreak
    if (Prev.isValid() && Prev.Line < Line &&
        linebreak(Line, 0, Ws, true) > 0) {
      Ws = ignore;
      PrevBreak = 0;
    }

    // initialize expression/key size: a zero value indicates expr/key doesn't
    // fit on a single line
    int Size = 0;

    // We use the ratio between the geometric mean of the previous key sizes and
    // the current size to determine if there should be a break in the
    // alignment. To compute the geometric mean we accumulate the ln(size)
    // values (lnsum) and the number of sizes included (count).
    double LnSum = 0.0;
    int Count = 0;

    // print all list elements
    int PrevLine = Prev.Line;
    for (size_t I = 0; I < List.size(); ++I) {
      const Expr *X = List[I];
      Line = lineFor(X->Begin);

      // Determine if the next linebreak, if any, needs to use formfeed: in
      // general, use the entire node size to make the decision; for key:value
      // expressions, use the key size.
      bool UseFF = true;

      // Determine element size: All bets are off if we don't have position
      // information for the previous and next token (likely generated code -
      // simply ignore the size in this case by setting it to 0).
      int PrevSize = Size;
      constexpr int Big = 1000000; // larger than any source line
      Size = nodeSize(X, Big);
      const auto *Pair = dyn_cast<KeyValueExpr>(X);
      if (Size <= Big && Prev.isValid() && Next.isValid()) {
        // x fits on a single line
        if (Pair)
          Size = nodeSize(Pair->Key, Big); // size <= infinity
      } else {
        // size too large or we don't have good layout information
        Size = 0;
      }

      // If the previous line and the current line had single-line-expressions
      // and the key sizes are small or the ratio between the current key and
      // the geometric mean if the previous key sizes does not exceed a
      // threshold, align columns and do not use formfeed.
      if (PrevSize > 0 && Size > 0) {
        constexpr int SmallSize = 40;
        if (Count == 0 || (PrevSize <= SmallSize && Size <= SmallSize)) {
          UseFF = false;
        } else {
          constexpr double R = 2.5;                         // threshold
          double GeoMean = std::exp(LnSum / (double)Count); // count > 0
          double Ratio = (double)Size / GeoMean;
          UseFF = R * Ratio <= 1 || R <= Ratio;
        }
      }

      bool NeedsLinebreak = 0 < PrevLine && PrevLine < Line;
      if (I > 0) {
        // Use position of expression following the comma as comma position for
        // correct comment placement, but only if the expression is on the same
        // line.
        if (!NeedsLinebreak)
          setPos(X->Begin);
        print(tok::comma);
        bool NeedsBlank = true;
        if (NeedsLinebreak) {
          // Lines are broken using newlines so comments remain aligned unless
          // useFF is set or there are multiple expressions on the same line in
          // which case formfeed is used.
          int NBreaks = linebreak(Line, 0, Ws, UseFF || PrevBreak + 1 < (int)I);
          if (NBreaks > 0) {
            Ws = ignore;
            PrevBreak = (int)I;
            NeedsBlank = false; // we got a line break instead
          }
          // If there was a new section or more than one new line (which means
          // that the tabwriter will implicitly break the section), reset the
          // geomean variables since we are starting a new group of elements
          // with the next element.
          if (NBreaks > 1) {
            LnSum = 0;
            Count = 0;
          }
        }
        if (NeedsBlank)
          print(blank);
      }

      if (List.size() > 1 && Pair && Size > 0 && NeedsLinebreak) {
        // We have a key:value expression that fits onto one line and it's not
        // on the same line as the prior expression: Use a column for the key
        // such that consecutive entries can align if possible.
        expr(Pair->Key);
        setPos(Pair->Colon);
        print(tok::colon, vtab);
        expr(Pair->Value);
      } else {
        expr0(X, Depth);
      }

      if (Size > 0) {
        LnSum += std::log((double)Size);
        ++Count;
      }

      PrevLine = Line;
    }

    if ((M & commaTerm) && Next.isValid() && pos.Line < Next.Line) {
      // Print a terminating comma if the next token is on a new line.
      print(tok::comma);
      if (Ws == ignore && !(M & noIndent)) {
        // unindent if we indented
        print(unindent);
      }
      print(formfeed); // terminating comma needs a line break to look good
      return;
    }

    if (Ws == ignore && !(M & noIndent)) {
      // unindent if we indented
      print(unindent);
    }
  }

  void parameters(const FieldList *Fields, ParamMode PM) {
    tok::Kind OpenTok = tok::l_paren, CloseTok = tok::r_paren;
    if (PM != ParamMode::funcParam) {
      OpenTok = tok::l_brack;
      CloseTok = tok::r_brack;
    }
    setPos(Fields->Opening);
    print(OpenTok);
    if (!Fields->List.empty()) {
      int PrevLine = lineFor(Fields->Opening);
      WS Ws = indent;
      for (size_t I = 0; I < Fields->List.size(); ++I) {
        const Field *Par = Fields->List[I];
        // determine par begin and end line (may be different if there are
        // multiple parameter names for this par or the type is on a separate
        // line)
        int ParLineBeg = lineFor(Par->Begin);
        int ParLineEnd = lineFor(Par->End);
        // separating "," if needed
        bool NeedsLinebreak = 0 < PrevLine && PrevLine < ParLineBeg;
        if (I > 0) {
          // use position of parameter following the comma as comma position
          // for correct comma placement, but only if the next parameter is on
          // the same line
          if (!NeedsLinebreak)
            setPos(Par->Begin);
          print(tok::comma);
        }
        // separator if needed (linebreak or blank)
        if (NeedsLinebreak && linebreak(ParLineBeg, 0, Ws, true) > 0) {
          // break line if the opening "(" or previous parameter ended on a
          // different line
          Ws = ignore;
        } else if (I > 0) {
          print(blank);
        }
        if (Par->isMove()) {
          setPos(Par->MoveLoc);
          print(Word{"move"}, blank);
        }
        // parameter names
        if (!Par->Names.empty()) {
          // Very subtle: If we indented before (ws == ignore), identList won't
          // indent again. If we didn't (ws == indent), identList will indent if
          // the identList spans multiple lines, and it will outdent again at
          // the end (and still ws == indent). Thus, a subsequent indent by a
          // linebreak call after a type, or in the next multi-line identList
          // will do the right thing.
          identList(Par->Names, Ws == indent);
          print(blank);
        }
        if (Par->isExtends()) { // `T extends P` (GEN-12)
          setPos(Par->ExtendsLoc);
          print(Word{"extends"}, blank);
          expr(stripParensAlways(Par->Type));
          PrevLine = ParLineEnd;
          continue;
        }
        // parameter type
        const Expr *ParType = stripParensAlways(Par->Type);
        if (PM == ParamMode::typeTParam && I == 0 && Par->Names.size() == 1 &&
            !isConstraint(ParType)) {
          // A bad constraint like `+n` or `b*struct{}`: `[P +n]` would be an
          // array length (gofmt prints that and changes the program), but
          // `[P (+n),]` is the type parameter P (the parser takes the
          // parentheses for those of a call P(+n)).
          print(tok::l_paren);
          expr(ParType);
          print(tok::r_paren);
          PrevLine = ParLineEnd;
          continue;
        }
        expr(ParType);
        PrevLine = ParLineEnd;
      }

      // if the closing ")" is on a separate line from the last parameter, print
      // an additional "," and line break
      int Closing = lineFor(Fields->Closing);
      if (0 < PrevLine && PrevLine < Closing) {
        print(tok::comma);
        linebreak(Closing, 0, ignore, true);
      } else if (PM == ParamMode::typeTParam && Fields->numFields() == 1 &&
                 !Fields->List[0]->isExtends() &&
                 (combinesWithName(stripParensAlways(Fields->List[0]->Type)) ||
                  (!Fields->List[0]->Names.empty() &&
                   !isConstraint(stripParensAlways(Fields->List[0]->Type))))) {
        // A type parameter list [P T] where the name P and the type expression
        // T syntactically combine to another valid (value) expression requires
        // a trailing comma, as in [P *T,] (or an enclosing interface as in [P
        // interface(*T)]), so that the type parameter list is not parsed as an
        // array length [P*T].
        print(tok::comma);
      }

      // unindent if we indented
      if (Ws == ignore)
        print(unindent);
    }

    setPos(Fields->Closing);
    print(CloseTok);
  }

  /// Reports whether X can be a type constraint: terms (types, ~T) joined by
  /// `|`. The parser takes more (`b*struct{}`), the printer must not turn that
  /// into something that parses differently.
  static bool isConstraint(const Expr *X) {
    switch (X->getKind()) {
    case Node::Kind::BinaryExpr: {
      // Only the first term matters: the parser finds the name and the
      // constraint by looking at the left end (extractName).
      const auto *B = cast<BinaryExpr>(X);
      return B->Op == tok::or_ && isConstraint(B->X);
    }
    case Node::Kind::UnaryExpr:
      return cast<UnaryExpr>(X)->Op == tok::tilde;
    case Node::Kind::ParenExpr:
      return isConstraint(cast<ParenExpr>(X)->X);
    case Node::Kind::Ident:
    case Node::Kind::SelectorExpr:
    case Node::Kind::IndexExpr:
    case Node::Kind::StarExpr:
    case Node::Kind::ArrayType:
    case Node::Kind::SliceType:
    case Node::Kind::StructType:
    case Node::Kind::FuncType:
    case Node::Kind::InterfaceType:
    case Node::Kind::MapType:
    case Node::Kind::ChanType:
    case Node::Kind::MacroInvocation:
      return true;
    default:
      return false;
    }
  }

  /// Reports whether a name followed by the expression X syntactically combines
  /// to another valid (value) expression.
  static bool combinesWithName(const Expr *X) {
    if (const auto *S = dyn_cast<StarExpr>(X))
      return !isTypeElem(S->X);
    if (const auto *B = dyn_cast<BinaryExpr>(X))
      return combinesWithName(B->X) && !isTypeElem(B->Y);
    if (const auto *P = dyn_cast<ParenExpr>(X))
      return !isTypeElem(P->X);
    return false;
  }

  /// Reports whether X is a (possibly parenthesized) type element expression.
  static bool isTypeElem(const Expr *X) {
    switch (X->getKind()) {
    case Node::Kind::ArrayType:
    case Node::Kind::SliceType:
    case Node::Kind::StructType:
    case Node::Kind::FuncType:
    case Node::Kind::InterfaceType:
    case Node::Kind::MapType:
    case Node::Kind::ChanType:
      return true;
    case Node::Kind::UnaryExpr:
      return cast<UnaryExpr>(X)->Op == tok::tilde;
    case Node::Kind::BinaryExpr: {
      const auto *B = cast<BinaryExpr>(X);
      return isTypeElem(B->X) || isTypeElem(B->Y);
    }
    case Node::Kind::ParenExpr:
      return isTypeElem(cast<ParenExpr>(X)->X);
    default:
      return false;
    }
  }

  void signature(const FuncType *Sig) {
    if (Sig->TypeParams)
      parameters(Sig->TypeParams, ParamMode::funcTParam);
    if (Sig->Fails) {
      setPos(Sig->BangLoc);
      print(tok::not_);
    }
    if (Sig->Params)
      parameters(Sig->Params, ParamMode::funcParam);
    else
      print(tok::l_paren, tok::r_paren);
    const FieldList *Res = Sig->Results;
    unsigned N = Res ? Res->numFields() : 0;
    if (N > 0) {
      print(blank);
      if (N == 1 && Res->List[0]->Names.empty()) {
        // single anonymous res; no ()'s
        expr(stripParensAlways(Res->List[0]->Type));
        return;
      }
      parameters(Res, ParamMode::funcParam);
    }
  }

  static int identListSize(ArrayRef<Ident *> List, int MaxSize) {
    int Size = 0;
    for (size_t I = 0; I < List.size(); ++I) {
      if (I > 0)
        Size += 2; // ", "
      Size += runeCount(List[I]->Name.str());
      if (Size >= MaxSize)
        break;
    }
    return Size;
  }

  bool isOneLineFieldList(ArrayRef<Field *> List) {
    if (List.size() != 1)
      return false; // allow only one field
    const Field *F = List[0];
    if (F->Tag || !F->Attrs.empty() || hasLineComment(F->End))
      return false; // don't allow tags, attributes or comments
    // only name(s) and type
    constexpr int MaxSize =
        30; // adjust as appropriate, this is an approximate value
    int NamesSize = identListSize(F->Names, MaxSize);
    if (NamesSize > 0)
      NamesSize = 1; // blank between names and types
    int TypeSize = nodeSize(F->Type, MaxSize);
    return NamesSize + TypeSize <= MaxSize;
  }

  void attributes(ArrayRef<Attribute *> Attrs) {
    for (size_t I = 0; I < Attrs.size(); ++I) {
      if (I > 0)
        print(blank);
      attribute(Attrs[I]);
    }
  }

  void attribute(const Attribute *A) {
    macroRef(A->AtLoc, tok::at, A->Pkg, A->Name);
    if (A->HasArgs)
      rawParens(A->Lparen, A->Rparen);
  }

  /// `#` or `@`, then [Pkg "."] Name: every token at its source position, so
  /// that a comment between them stays where it was (`# // why` newline
  /// `Name(...)` is valid: no semicolon is inserted after `#`).
  void macroRef(SourceLoc MarkLoc, tok::Kind Mark, const Ident *Pkg,
                const Ident *Name) {
    setPos(MarkLoc);
    print(Mark);
    if (Pkg) {
      setPos(Pkg->Begin);
      print(Pkg, tok::period);
    }
    setPos(Name->Begin);
    print(Name);
  }

  /// A macro or attribute argument list is copied from the source, from '(' to
  /// ')'; the comments inside are part of the text. The '(' is a token of its
  /// own, so that the comments before it are printed before it.
  void rawParens(SourceLoc Lparen, SourceLoc Rparen) {
    uint32_t B = SM.getFileOffset(Lparen) + 1;
    uint32_t E = SM.getFileOffset(Rparen) + 1;
    setPos(Lparen);
    print(tok::l_paren);
    // The comments inside the parentheses are in the copied text.
    while (commentOffset < (int64_t)E)
      nextComment();
    beginArg();
    emit(Sh.Src.substr(B, E - B), true, true, tok::r_paren);
  }

  void fieldList(const FieldList *Fields, bool IsStruct,
                 bool BlankBefore = false) {
    SourceLoc Lbrace = Fields->Opening;
    ArrayRef<Field *> List = Fields->List;
    SourceLoc Rbrace = Fields->Closing;
    bool HasComments = commentBefore(posFor(Rbrace));
    bool SrcIsOneLine = Lbrace.isValid() && Rbrace.isValid() &&
                        lineFor(Lbrace) == lineFor(Rbrace);

    if (!HasComments && SrcIsOneLine) {
      // possibly a one-line struct/interface
      if (List.empty()) {
        // no blank between keyword and {} in this case
        if (BlankBefore)
          print(blank);
        setPos(Lbrace);
        print(tok::l_brace);
        setPos(Rbrace);
        print(tok::r_brace);
        return;
      }
      if (isOneLineFieldList(List)) {
        // small enough - print on one line (don't use identList and ignore
        // source line breaks)
        if (BlankBefore)
          print(blank);
        setPos(Lbrace);
        print(tok::l_brace, blank);
        const Field *F = List[0];
        if (IsStruct) {
          for (size_t I = 0; I < F->Names.size(); ++I) {
            if (I > 0) {
              // no comments so no need for comma position
              print(tok::comma, blank);
            }
            expr(F->Names[I]);
          }
          if (!F->Names.empty())
            print(blank);
          expr(F->Type);
        } else { // interface
          if (!F->Names.empty()) {
            const Ident *Name = F->Names[0]; // method name
            expr(Name);
            signature(cast<FuncType>(F->Type)); // don't print "func"
          } else {
            // embedded interface
            if (F->isExtends()) {
              setPos(F->ExtendsLoc);
              print(Word{"extends"}, blank);
            }
            expr(F->Type);
          }
        }
        print(blank);
        setPos(Rbrace);
        print(tok::r_brace);
        return;
      }
    }
    // hasComments || !srcIsOneLine

    print(blank);
    setPos(Lbrace);
    print(tok::l_brace, indent);
    if (HasComments || !List.empty())
      print(formfeed);

    if (IsStruct) {
      WS Sep = vtab;
      if (List.size() == 1)
        Sep = blank;
      int Line = 0;
      for (size_t I = 0; I < List.size(); ++I) {
        const Field *F = List[I];
        if (I > 0)
          linebreak(lineFor(F->Begin), 1, ignore, linesFrom(Line) > 0);
        int ExtraTabs = 0;
        recordLine(&Line);
        if (!F->Names.empty()) {
          // named fields
          identList(F->Names, false);
          print(Sep);
          expr(F->Type);
          ExtraTabs = 1;
        } else {
          // anonymous field
          expr(F->Type);
          ExtraTabs = 2;
        }
        if (F->Tag || !F->Attrs.empty()) {
          if (!F->Names.empty() && Sep == vtab)
            print(Sep);
          print(Sep);
          if (!F->Attrs.empty()) {
            attributes(F->Attrs);
            if (F->Tag)
              print(blank);
          }
          if (F->Tag)
            expr(F->Tag);
          ExtraTabs = 0;
        }
        if (hasLineComment(F->End)) {
          for (; ExtraTabs > 0; --ExtraTabs)
            print(Sep);
        }
      }
    } else { // interface
      int Line = 0;
      for (size_t I = 0; I < List.size(); ++I) {
        const Field *F = List[I];
        const Ident *Name = F->Names.empty() ? nullptr : F->Names[0];
        if (I > 0) {
          // don't do a line break (min == 0) if we are printing a list of
          // types
          linebreak(lineFor(F->Begin), 1, ignore, linesFrom(Line) > 0);
        }
        recordLine(&Line);
        if (Name) {
          // method
          expr(Name);
          signature(cast<FuncType>(F->Type)); // don't print "func"
        } else {
          // embedded interface
          if (F->isExtends()) {
            setPos(F->ExtendsLoc);
            print(Word{"extends"}, blank);
          }
          expr(F->Type);
        }
      }
    }
    print(unindent, formfeed);
    setPos(Rbrace);
    print(tok::r_brace);
  }

  void recordLine(int *LinePtr) { linePtr = LinePtr; }
  int linesFrom(int Line) const { return out.Line - Line; }

  //===--------------------------------------------------------------------===//
  // Expressions
  //===--------------------------------------------------------------------===//

  static void walkBinary(const BinaryExpr *E, bool &Has4, bool &Has5,
                         int &MaxProblem) {
    switch (precedence(E->Op)) {
    case 4:
      Has4 = true;
      break;
    case 5:
      Has5 = true;
      break;
    }

    if (const auto *L = dyn_cast<BinaryExpr>(E->X)) {
      // if l.Op.Precedence() < e.Op.Precedence(): parens will be inserted;
      // pretend this is an *ast.ParenExpr and do nothing.
      if (precedence(L->Op) >= precedence(E->Op)) {
        bool H4 = false, H5 = false;
        int MP = 0;
        walkBinary(L, H4, H5, MP);
        Has4 = Has4 || H4;
        Has5 = Has5 || H5;
        MaxProblem = std::max(MaxProblem, MP);
      }
    }

    if (const auto *R = dyn_cast<BinaryExpr>(E->Y)) {
      if (precedence(R->Op) > precedence(E->Op)) {
        bool H4 = false, H5 = false;
        int MP = 0;
        walkBinary(R, H4, H5, MP);
        Has4 = Has4 || H4;
        Has5 = Has5 || H5;
        MaxProblem = std::max(MaxProblem, MP);
      }
    } else if (isa<StarExpr>(E->Y)) {
      if (E->Op == tok::quo) // `*/`
        MaxProblem = 5;
    } else if (const auto *U = dyn_cast<UnaryExpr>(E->Y)) {
      std::string Ops = (spelling(E->Op) + spelling(U->Op)).str();
      if (Ops == "/*" || Ops == "&&" || Ops == "&^")
        MaxProblem = 5;
      else if (Ops == "++" || Ops == "--")
        MaxProblem = std::max(MaxProblem, 4);
    }
  }

  static int cutoff(const BinaryExpr *E, int Depth) {
    bool Has4 = false, Has5 = false;
    int MaxProblem = 0;
    walkBinary(E, Has4, Has5, MaxProblem);
    if (MaxProblem > 0)
      return MaxProblem + 1;
    if (Has4 && Has5) {
      if (Depth == 1)
        return 5;
      return 4;
    }
    if (Depth == 1)
      return 6;
    return 4;
  }

  static int diffPrec(const Expr *E, int Prec) {
    const auto *X = dyn_cast<BinaryExpr>(E);
    if (!X || Prec != precedence(X->Op))
      return 1;
    return 0;
  }

  static int reduceDepth(int Depth) {
    --Depth;
    if (Depth < 1)
      Depth = 1;
    return Depth;
  }

  /// Formats the binary expression: decide the cutoff and then format. Depth 1
  /// is Normal mode, depth > 1 is Compact mode.
  void binaryExpr(const BinaryExpr *X, int Prec1, int Cutoff, int Depth) {
    int Prec = precedence(X->Op);
    if (Prec < Prec1) {
      // parenthesis needed
      print(tok::l_paren);
      expr0(X, reduceDepth(Depth)); // parentheses undo one level of depth
      print(tok::r_paren);
      return;
    }

    bool PrintBlank = Prec < Cutoff;

    WS Ws = indent;
    expr1(X->X, Prec, Depth + diffPrec(X->X, Prec));
    if (PrintBlank)
      print(blank);
    int XLine = pos.Line; // before the operator (it may be on the next line!)
    int YLine = lineFor(X->Y->Begin);
    setPos(X->OpLoc);
    print(X->Op);
    if (XLine != YLine && XLine > 0 && YLine > 0) {
      // at least one line break, but respect an extra empty line in the source
      if (linebreak(YLine, 1, Ws, true) > 0) {
        Ws = ignore;
        PrintBlank = false; // no blank after line break
      }
    }
    if (PrintBlank)
      print(blank);
    expr1(X->Y, Prec + 1, Depth + 1);
    if (Ws == ignore)
      print(unindent);
  }

  static bool isBinary(const Expr *E) { return isa<BinaryExpr>(E); }

  void expr1(const Expr *E, int Prec1, int Depth) {
    setPos(E->Begin);

    switch (E->getKind()) {
    case Node::Kind::BadExpr:
      print("BadExpr");
      break;

    case Node::Kind::Ident:
      print(cast<Ident>(E));
      break;

    case Node::Kind::BinaryExpr: {
      const auto *X = cast<BinaryExpr>(E);
      if (Depth < 1)
        Depth = 1;
      binaryExpr(X, Prec1, cutoff(X, Depth), Depth);
      break;
    }

    case Node::Kind::KeyValueExpr: {
      const auto *X = cast<KeyValueExpr>(E);
      expr(X->Key);
      setPos(X->Colon);
      print(tok::colon, blank);
      expr(X->Value);
      break;
    }

    case Node::Kind::StarExpr: {
      const auto *X = cast<StarExpr>(E);
      if (UnaryPrec < Prec1) {
        // parenthesis needed
        print(tok::l_paren);
        print(tok::mul);
        expr(X->X);
        print(tok::r_paren);
      } else {
        // no parenthesis needed
        print(tok::mul);
        expr(X->X);
      }
      break;
    }

    case Node::Kind::UnaryExpr: {
      const auto *X = cast<UnaryExpr>(E);
      if (UnaryPrec < Prec1) {
        // parenthesis needed
        print(tok::l_paren);
        expr(X);
        print(tok::r_paren);
      } else {
        // no parenthesis needed
        print(X->Op);
        expr1(X->X, UnaryPrec, Depth);
      }
      break;
    }

    case Node::Kind::BasicLit:
      print(cast<BasicLit>(E));
      break;

    case Node::Kind::FuncLit: {
      const auto *X = cast<FuncLit>(E);
      setPos(X->Type->Begin);
      print(tok::kw_func);
      // See the comment in funcDecl about how the header size is computed.
      int StartCol = out.Column - 4; // len("func")
      signature(X->Type);
      funcBody(distanceFrom(X->Type->Begin, StartCol), blank, X->Body);
      break;
    }

    case Node::Kind::ParenExpr: {
      const auto *X = cast<ParenExpr>(E);
      if (isa<ParenExpr>(X->X)) {
        // don't print parentheses around an already parenthesized expression
        expr0(X->X, Depth);
      } else {
        print(tok::l_paren);
        expr0(X->X, reduceDepth(Depth)); // parentheses undo one level of depth
        setPos(X->Rparen);
        print(tok::r_paren);
      }
      break;
    }

    case Node::Kind::SelectorExpr:
      selectorExpr(cast<SelectorExpr>(E), Depth, false);
      break;

    case Node::Kind::TypeAssertExpr: {
      const auto *X = cast<TypeAssertExpr>(E);
      expr1(X->X, HighestPrec, Depth);
      // The AST has no positions for '.' and '(': find them (a comment may
      // stand in between).
      uint32_t DotOff = skipTrivia(SM.getFileOffset(X->X->End));
      uint32_t LparenOff = skipTrivia(DotOff + 1);
      if (Sh.Src.substr(DotOff, 1) == "." &&
          Sh.Src.substr(LparenOff, 1) == "(") {
        print(tok::period);
        setPos(X->X->End.getLocWithOffset(LparenOff -
                                          SM.getFileOffset(X->X->End)));
        print(tok::l_paren);
      } else {
        print(tok::period, tok::l_paren);
      }
      if (X->Type)
        expr(X->Type);
      else
        print(tok::kw_type);
      setPos(X->End.getLocWithOffset(-1));
      print(tok::r_paren);
      break;
    }

    case Node::Kind::IndexExpr: {
      const auto *X = cast<IndexExpr>(E);
      expr1(X->X, HighestPrec, 1);
      setPos(X->Lbrack);
      print(tok::l_brack);
      if (X->Indices.size() == 1) {
        expr0(X->Indices[0], Depth + 1);
      } else {
        llvm::SmallVector<const Expr *, 8> L(X->Indices.begin(),
                                             X->Indices.end());
        exprList(X->Lbrack, L, Depth + 1, commaTerm, X->Rbrack);
      }
      setPos(X->Rbrack);
      print(tok::r_brack);
      break;
    }

    case Node::Kind::SliceExpr: {
      const auto *X = cast<SliceExpr>(E);
      expr1(X->X, HighestPrec, 1);
      setPos(X->Lbrack);
      print(tok::l_brack);
      const Expr *Indices[3] = {X->Lo, X->Hi, X->Max};
      size_t NIdx = X->Max ? 3 : 2;
      // determine if we need extra blanks around ':'
      bool NeedsBlanks = false;
      if (Depth <= 1) {
        int IndexCount = 0;
        bool HasBinaries = false;
        for (size_t I = 0; I < NIdx; ++I) {
          if (Indices[I]) {
            ++IndexCount;
            if (isBinary(Indices[I]))
              HasBinaries = true;
          }
        }
        if (IndexCount > 1 && HasBinaries)
          NeedsBlanks = true;
      }
      for (size_t I = 0; I < NIdx; ++I) {
        if (I > 0) {
          if (Indices[I - 1] && NeedsBlanks)
            print(blank);
          print(tok::colon);
          if (Indices[I] && NeedsBlanks)
            print(blank);
        }
        if (Indices[I])
          expr0(Indices[I], Depth + 1);
      }
      setPos(X->Rbrack);
      print(tok::r_brack);
      break;
    }

    case Node::Kind::CallExpr: {
      const auto *X = cast<CallExpr>(E);
      if (X->Args.size() > 1)
        ++Depth;

      // Conversions to literal function types or <-chan types require
      // parentheses around the type.
      bool Paren = false;
      if (isa<FuncType>(X->Fun))
        Paren = true;
      else if (const auto *C = dyn_cast<ChanType>(X->Fun))
        Paren = C->Direction == ChanType::Recv;
      if (Paren)
        print(tok::l_paren);
      bool WasIndented = possibleSelectorExpr(X->Fun, HighestPrec, Depth);
      if (Paren)
        print(tok::r_paren);

      if (X->BangLoc.isValid()) {
        setPos(X->BangLoc);
        print(tok::not_);
      }
      setPos(X->Lparen);
      print(tok::l_paren);
      llvm::SmallVector<const Expr *, 8> Args(X->Args.begin(), X->Args.end());
      if (X->EllipsisLoc.isValid()) {
        exprList(X->Lparen, Args, Depth, 0, X->EllipsisLoc);
        setPos(X->EllipsisLoc);
        print(tok::ellipsis);
        if (X->Rparen.isValid() &&
            lineFor(X->EllipsisLoc) < lineFor(X->Rparen)) {
          print(tok::comma, formfeed);
        }
      } else {
        exprList(X->Lparen, Args, Depth, commaTerm, X->Rparen);
      }
      setPos(X->Rparen);
      print(tok::r_paren);
      if (WasIndented)
        print(unindent);
      break;
    }

    case Node::Kind::CompositeLit: {
      const auto *X = cast<CompositeLit>(E);
      // composite literal elements that are composite literals themselves may
      // have the type omitted
      if (X->Type)
        expr1(X->Type, HighestPrec, Depth);
      ++level;
      setPos(X->Lbrace);
      print(tok::l_brace);
      llvm::SmallVector<const Expr *, 8> Elts(X->Elts.begin(), X->Elts.end());
      exprList(X->Lbrace, Elts, 1, commaTerm, X->Rbrace);
      // do not insert extra line break following a /*-style comment before the
      // closing '}' as it might break the code if there is no trailing ','
      PMode M = noExtraLinebreak;
      unsigned MBits = (unsigned)M;
      // do not insert extra blank following a /*-style comment before the
      // closing '}' unless the literal is empty
      if (!X->Elts.empty())
        MBits |= (unsigned)noExtraBlank;
      // need the initial indent to print lone comments with the proper level of
      // indentation
      print(indent, unindent);
      togglePMode(MBits);
      setPos(X->Rbrace);
      print(tok::r_brace);
      togglePMode(MBits);
      --level;
      break;
    }

    case Node::Kind::Ellipsis: {
      const auto *X = cast<ast::Ellipsis>(E);
      print(tok::ellipsis);
      if (X->Elt)
        expr(X->Elt);
      break;
    }

    case Node::Kind::ArrayType: {
      const auto *X = cast<ArrayType>(E);
      print(tok::l_brack);
      if (X->Len)
        expr(X->Len);
      print(tok::r_brack);
      expr(X->Elt);
      break;
    }

    case Node::Kind::SliceType: {
      const auto *X = cast<SliceType>(E);
      print(tok::l_brack, tok::r_brack);
      expr(X->Elt);
      break;
    }

    case Node::Kind::StructType: {
      const auto *X = cast<StructType>(E);
      print(tok::kw_struct);
      fieldList(X->Fields, true);
      break;
    }

    case Node::Kind::FuncType: {
      const auto *X = cast<FuncType>(E);
      print(tok::kw_func);
      signature(X);
      break;
    }

    case Node::Kind::InterfaceType: {
      const auto *X = cast<InterfaceType>(E);
      print(tok::kw_interface);
      fieldList(X->Methods, false);
      break;
    }

    case Node::Kind::MapType: {
      const auto *X = cast<MapType>(E);
      print(tok::kw_map, tok::l_brack);
      expr(X->Key);
      print(tok::r_brack);
      expr(X->Value);
      break;
    }

    case Node::Kind::ChanType: {
      const auto *X = cast<ChanType>(E);
      switch (X->Direction) {
      case ChanType::Both:
        print(tok::kw_chan);
        break;
      case ChanType::Recv:
        print(tok::arrow, tok::kw_chan); // x.Arrow and x.Pos() are the same
        break;
      case ChanType::Send:
        print(tok::kw_chan);
        setPos(X->ArrowLoc);
        print(tok::arrow);
        break;
      }
      print(blank);
      expr(X->Value);
      break;
    }

    case Node::Kind::MacroInvocation: {
      const auto *X = cast<MacroInvocation>(E);
      macroRef(X->HashLoc, tok::hash, X->Pkg, X->Name);
      if (X->Lparen.isValid() && X->Rparen.isValid() && X->Lparen != X->Rparen)
        rawParens(X->Lparen, X->Rparen);
      break;
    }

    default:
      print("BadExpr");
      break;
    }
  }

  /// The offset of the first byte at or after Off that is neither white space
  /// nor part of a comment.
  uint32_t skipTrivia(uint32_t Off) const {
    StringRef S = Sh.Src;
    while (Off < S.size()) {
      char C = S[Off];
      if (C == ' ' || C == '\t' || C == '\n' || C == '\r') {
        ++Off;
      } else if (S.substr(Off).starts_with("//")) {
        while (Off < S.size() && S[Off] != '\n')
          ++Off;
      } else if (S.substr(Off).starts_with("/*")) {
        size_t E = S.find("*/", Off + 2);
        Off = E == StringRef::npos ? S.size() : (uint32_t)E + 2;
      } else {
        break;
      }
    }
    return Off;
  }

  void togglePMode(unsigned Bits) {
    if (Bits & (unsigned)noExtraBlank)
      print(noExtraBlank);
    if (Bits & (unsigned)noExtraLinebreak)
      print(noExtraLinebreak);
  }

  /// rewrites base prefixes and exponents of numbers to use lower-case letters
  /// (0X123 to 0x123 and 1.2E3 to 1.2e3), and removes leading 0's from integer
  /// imaginary literals (0765i to 765i). It leaves hexadecimal digits alone.
  static std::string normalizedNumber(StringRef Lit) {
    std::string X = Lit.str();
    if (X.size() < 2)
      return X; // only one digit (common case) - nothing to do
    StringRef P = StringRef(X).substr(0, 2);
    if (P == "0X") {
      X = "0x" + X.substr(2);
      size_t I = X.rfind('P'); // possibly a hexadecimal float
      if (I != std::string::npos)
        X[I] = 'p';
    } else if (P == "0x") {
      size_t I = X.rfind('P'); // possibly a hexadecimal float
      if (I != std::string::npos)
        X[I] = 'p';
    } else if (P == "0O") {
      X = "0o" + X.substr(2);
    } else if (P == "0B") {
      X = "0b" + X.substr(2);
    } else if (P != "0o" && P != "0b") {
      // 0-prefix octal, decimal int, or float (possibly with 'i' suffix)
      size_t I = X.rfind('E');
      if (I != std::string::npos) {
        X[I] = 'e';
      } else if (X.back() == 'i' &&
                 X.find_first_of(".e") == std::string::npos) {
        // remove leading 0's from integer (but not floating-point) imaginary
        // literals
        size_t F = X.find_first_not_of("0_");
        X = F == std::string::npos ? "" : X.substr(F);
        if (X == "i")
          X = "0i";
      }
    }
    return X;
  }

  bool possibleSelectorExpr(const Expr *E, int Prec1, int Depth) {
    if (const auto *X = dyn_cast<SelectorExpr>(E))
      return selectorExpr(X, Depth, true);
    expr1(E, Prec1, Depth);
    return false;
  }

  /// Handles a SelectorExpr and reports whether it spans multiple lines.
  bool selectorExpr(const SelectorExpr *X, int Depth, bool IsMethod) {
    expr1(X->X, HighestPrec, Depth);
    print(tok::period);
    int Line = lineFor(X->Sel->Begin);
    if (pos.isValid() && pos.Line < Line) {
      print(indent, newline);
      setPos(X->Sel->Begin);
      print(X->Sel);
      if (!IsMethod)
        print(unindent);
      return true;
    }
    setPos(X->Sel->Begin);
    print(X->Sel);
    return false;
  }

  void expr0(const Expr *X, int Depth) { expr1(X, LowestPrec, Depth); }
  void expr(const Expr *X) { expr1(X, LowestPrec, 1); }

  //===--------------------------------------------------------------------===//
  // Sizes
  //===--------------------------------------------------------------------===//

  /// The size of N printed in raw mode on a single line, or MaxSize + 1 if it
  /// does not fit (go/printer nodeSize).
  int nodeSize(const Node *N, int MaxSize) {
    auto It = Sh.NodeSizes.find(N);
    if (It != Sh.NodeSizes.end())
      return It->second;
    int Size = MaxSize + 1; // assume N does not fit
    Sh.NodeSizes[N] = Size;
    Printer P(Sh, /*Raw=*/true, /*WithComments=*/false);
    P.printNode(N);
    std::string T = trim(P.finish());
    if (!StringRef(T).contains('\n')) {
      int Runes = runeCount(T);
      if (Runes <= MaxSize) {
        Size = Runes;
        Sh.NodeSizes[N] = Size;
      }
    }
    return Size;
  }

  int bodySize(const BlockStmt *B, int MaxSize) {
    if (B->Lbrace.isValid() && B->Rbrace.isValid() &&
        lineFor(B->Lbrace) != lineFor(B->Rbrace))
      return MaxSize + 1; // opening and closing brace on different lines
    if (B->List.size() > 5)
      return MaxSize + 1; // too many statements
    int Size = commentSizeBefore(posFor(B->Rbrace));
    for (size_t I = 0; I < B->List.size(); ++I) {
      if (Size > MaxSize)
        break; // no need to continue
      if (I > 0)
        Size += 2; // space for a semicolon and blank
      Size += nodeSize(B->List[I], MaxSize);
    }
    return Size;
  }

  /// The distance from StartOutCol on the line of StartPos, or Infinity.
  int distanceFrom(SourceLoc StartPos, int StartOutCol) {
    if (StartPos.isValid() && pos.isValid() && lineFor(StartPos) == pos.Line)
      return out.Column - StartOutCol;
    return Infinity;
  }

  int numLines(const Node *N) {
    if (N->Begin.isValid() && N->End.isValid())
      return lineFor(N->End) - lineFor(N->Begin) + 1;
    return Infinity;
  }

  void funcBody(int HeaderSize, WS Sep, const BlockStmt *B) {
    if (!B)
      return;
    // save/restore composite literal nesting level
    int SavedLevel = level;
    level = 0;

    constexpr int MaxBody = 100;
    if (HeaderSize + bodySize(B, MaxBody) <= MaxBody) {
      print(Sep);
      setPos(B->Lbrace);
      print(tok::l_brace);
      if (!B->List.empty()) {
        print(blank);
        for (size_t I = 0; I < B->List.size(); ++I) {
          if (I > 0)
            print(tok::semi, blank);
          stmt(B->List[I], I + 1 == B->List.size());
        }
        print(blank);
      }
      print(noExtraLinebreak);
      setPos(B->Rbrace);
      print(tok::r_brace, noExtraLinebreak);
      level = SavedLevel;
      return;
    }

    if (Sep != ignore)
      print(blank); // always use blank
    block(B, 1);
    level = SavedLevel;
  }

  //===--------------------------------------------------------------------===//
  // Statements
  //===--------------------------------------------------------------------===//

  static bool isTypeName(const Expr *X) {
    if (isa<Ident>(X))
      return true;
    if (const auto *S = dyn_cast<SelectorExpr>(X))
      return isTypeName(S->X);
    return false;
  }

  /// Reports whether the unparenthesized composite literals below X start with
  /// a type name (parentheses protect those).
  static bool hasBareCompositeLit(const Node *N) {
    if (const auto *C = dyn_cast<CompositeLit>(N))
      return C->Type && isTypeName(C->Type);
    if (isa<ParenExpr>(N))
      return false;
    bool Found = false;
    forEachChild(N, [&](const char *, const Node *Child) {
      if (!Found && Child && hasBareCompositeLit(Child))
        Found = true;
    });
    return Found;
  }

  static const Expr *stripParens(const Expr *X) {
    if (const auto *P = dyn_cast<ParenExpr>(X)) {
      // parentheses must not be stripped if there are any unparenthesized
      // composite literals starting with a type name
      if (!hasBareCompositeLit(P->X))
        return stripParens(P->X);
    }
    return X;
  }

  static const Expr *stripParensAlways(const Expr *X) {
    if (const auto *P = dyn_cast<ParenExpr>(X))
      return stripParensAlways(P->X);
    return X;
  }

  /// `switch (x.(type)) {` is an expression switch with a bad tag; without
  /// the parentheses it would become a type switch, a different tree.
  static bool isParenTypeGuard(const Expr *X) {
    const auto *P = dyn_cast<ParenExpr>(X);
    if (!P)
      return false;
    const Expr *Inner = stripParensAlways(P);
    const auto *TA = dyn_cast<TypeAssertExpr>(Inner);
    return TA && !TA->Type;
  }

  void controlClause(bool IsForStmt, const Stmt *Init, const Expr *X,
                     const Stmt *Post, bool Strip = true) {
    print(blank);
    bool NeedsBlank = false;
    if (!Init && !Post) {
      // no semicolons required
      if (X) {
        expr(Strip ? stripParens(X) : X);
        NeedsBlank = true;
      }
    } else {
      // all semicolons required (they are not separators but terminators)
      if (Init)
        stmt(Init, false);
      print(tok::semi, blank);
      if (X) {
        expr(Strip ? stripParens(X) : X);
        NeedsBlank = true;
      }
      if (IsForStmt) {
        print(tok::semi, blank);
        NeedsBlank = false;
        if (Post) {
          stmt(Post, false);
          NeedsBlank = true;
        }
      }
    }
    if (NeedsBlank)
      print(blank);
  }

  /// Heuristic: reports whether there are more than one multi-line elements in
  /// the list, or if there is any element that is not starting on the same line
  /// as the previous one ends.
  bool indentList(ArrayRef<Expr *> List) {
    if (List.size() >= 2) {
      int B = lineFor(List[0]->Begin);
      int E = lineFor(List.back()->End);
      if (0 < B && B < E) {
        // list spans multiple lines
        int N = 0; // multi-line element count
        int Line = B;
        for (const Expr *X : List) {
          int XB = lineFor(X->Begin);
          int XE = lineFor(X->End);
          if (Line < XB) {
            // X is not starting on the same line as the previous one ended;
            // add it as a separate multi-line element
            return true;
          }
          if (XB < XE) {
            // X is a multi-line element
            ++N;
          }
          Line = XE;
        }
        return N > 1;
      }
    }
    return false;
  }

  static llvm::SmallVector<const Expr *, 8> asConst(ArrayRef<Expr *> L) {
    return llvm::SmallVector<const Expr *, 8>(L.begin(), L.end());
  }

  void stmtList(ArrayRef<Stmt *> List, int NIndent, bool NextIsRBrace) {
    if (NIndent > 0)
      print(indent);
    int Line = 0;
    size_t I = 0;
    for (const Stmt *S : List) {
      // ignore empty statements (was issue 3466)
      if (isa<EmptyStmt>(S))
        continue;
      // NIndent == 0 only for lists of switch/select case clauses; in those
      // cases each clause is a new section
      if (!output.empty()) {
        // only print line break if we are not at the beginning of the output
        // (i.e., we are not printing only a partial program)
        linebreak(lineFor(S->Begin), 1, ignore,
                  I == 0 || NIndent == 0 || linesFrom(Line) > 0);
      }
      recordLine(&Line);
      stmt(S, NextIsRBrace && I + 1 == List.size());
      // labeled statements put labels on a separate line, but here we only care
      // about the start line of the actual statement without label - correct
      // line for each label
      for (const Stmt *T = S;;) {
        const auto *LT = dyn_cast<LabeledStmt>(T);
        if (!LT)
          break;
        ++Line;
        T = LT->Body;
      }
      ++I;
    }
    if (NIndent > 0)
      print(unindent);
  }

  void block(const BlockStmt *B, int NIndent) {
    setPos(B->Lbrace);
    print(tok::l_brace);
    stmtList(B->List, NIndent, true);
    linebreak(lineFor(B->Rbrace), 1, ignore, true);
    setPos(B->Rbrace);
    print(tok::r_brace);
  }

  void stmt(const Stmt *S, bool NextIsRBrace) {
    setPos(S->Begin);

    switch (S->getKind()) {
    case Node::Kind::BadStmt:
      print("BadStmt");
      break;

    case Node::Kind::DeclStmt:
      decl(cast<DeclStmt>(S)->D);
      break;

    case Node::Kind::EmptyStmt:
      break; // nothing to do

    case Node::Kind::LabeledStmt: {
      const auto *L = cast<LabeledStmt>(S);
      // a "correcting" unindent immediately following a line break is applied
      // before the line break if there is no comment between (see
      // writeWhitespace)
      print(unindent);
      expr(L->Label);
      setPos(L->Colon);
      print(tok::colon, indent);
      if (const auto *E = dyn_cast<EmptyStmt>(L->Body)) {
        if (!NextIsRBrace) {
          print(newline);
          setPos(E->Begin);
          print(tok::semi);
          break;
        }
      } else {
        linebreak(lineFor(L->Body->Begin), 1, ignore, true);
      }
      stmt(L->Body, NextIsRBrace);
      break;
    }

    case Node::Kind::ExprStmt:
      expr0(cast<ExprStmt>(S)->X, 1);
      break;

    case Node::Kind::SendStmt: {
      const auto *X = cast<SendStmt>(S);
      expr0(X->Chan, 1);
      print(blank);
      setPos(X->ArrowLoc);
      print(tok::arrow, blank);
      expr0(X->Value, 1);
      break;
    }

    case Node::Kind::IncDecStmt: {
      const auto *X = cast<IncDecStmt>(S);
      expr0(X->X, 2);
      setPos(X->OpLoc);
      print(X->Op);
      break;
    }

    case Node::Kind::AssignStmt: {
      const auto *X = cast<AssignStmt>(S);
      int Depth = 1;
      if (X->Lhs.size() > 1 && X->Rhs.size() > 1)
        ++Depth;
      exprList(X->Begin, asConst(X->Lhs), Depth, 0, X->OpLoc);
      print(blank);
      setPos(X->OpLoc);
      print(X->Op, blank);
      exprList(X->OpLoc, asConst(X->Rhs), Depth, 0, SourceLoc());
      break;
    }

    case Node::Kind::GoStmt:
      print(tok::kw_go, blank);
      expr(cast<GoStmt>(S)->Call);
      break;

    case Node::Kind::DeferStmt:
      print(tok::kw_defer, blank);
      expr(cast<DeferStmt>(S)->Call);
      break;

    case Node::Kind::ReturnStmt: {
      const auto *X = cast<ReturnStmt>(S);
      print(tok::kw_return);
      if (!X->Results.empty()) {
        print(blank);
        // Use indentList heuristic to make corner cases look better: if there
        // are multiple multi-line results, indent them
        if (indentList(X->Results)) {
          print(indent);
          // Use NoPos so that a newline never goes before the results (see
          // issue #32854).
          exprList(SourceLoc(), asConst(X->Results), 1, noIndent, SourceLoc());
          print(unindent);
        } else {
          exprList(SourceLoc(), asConst(X->Results), 1, 0, SourceLoc());
        }
      }
      break;
    }

    case Node::Kind::BranchStmt: {
      const auto *X = cast<BranchStmt>(S);
      print(X->Keyword);
      if (X->Label) {
        print(blank);
        expr(X->Label);
      }
      break;
    }

    case Node::Kind::BlockStmt:
      block(cast<BlockStmt>(S), 1);
      break;

    case Node::Kind::IfStmt: {
      const auto *X = cast<IfStmt>(S);
      print(tok::kw_if);
      controlClause(false, X->Init, X->Cond, nullptr);
      block(X->Then, 1);
      if (X->Else) {
        print(blank, tok::kw_else, blank);
        stmt(X->Else, NextIsRBrace);
      }
      break;
    }

    case Node::Kind::CaseClause: {
      const auto *X = cast<CaseClause>(S);
      if (!X->IsDefault) {
        print(tok::kw_case, blank);
        exprList(X->Begin, asConst(X->List), 1, 0, X->Colon);
      } else {
        print(tok::kw_default);
      }
      setPos(X->Colon);
      print(tok::colon);
      stmtList(X->Body, 1, NextIsRBrace);
      break;
    }

    case Node::Kind::SwitchStmt: {
      const auto *X = cast<SwitchStmt>(S);
      print(tok::kw_switch);
      controlClause(false, X->Init, X->Tag, nullptr,
                    !(X->Tag && isParenTypeGuard(X->Tag)));
      block(X->Body, 0);
      break;
    }

    case Node::Kind::TypeSwitchStmt: {
      const auto *X = cast<TypeSwitchStmt>(S);
      print(tok::kw_switch);
      if (X->Init) {
        print(blank);
        stmt(X->Init, false);
        print(tok::semi);
      }
      print(blank);
      stmt(X->Assign, false);
      print(blank);
      block(X->Body, 0);
      break;
    }

    case Node::Kind::CommClause: {
      const auto *X = cast<CommClause>(S);
      if (X->Comm) {
        print(tok::kw_case, blank);
        stmt(X->Comm, false);
      } else {
        print(tok::kw_default);
      }
      setPos(X->Colon);
      print(tok::colon);
      stmtList(X->Body, 1, NextIsRBrace);
      break;
    }

    case Node::Kind::SelectStmt: {
      const auto *X = cast<SelectStmt>(S);
      print(tok::kw_select, blank);
      const BlockStmt *Body = X->Body;
      if (Body->List.empty() && !commentBefore(posFor(Body->Rbrace))) {
        // print empty select statement w/o comments on one line
        setPos(Body->Lbrace);
        print(tok::l_brace);
        setPos(Body->Rbrace);
        print(tok::r_brace);
      } else {
        block(Body, 0);
      }
      break;
    }

    case Node::Kind::ForStmt: {
      const auto *X = cast<ForStmt>(S);
      print(tok::kw_for);
      controlClause(true, X->Init, X->Cond, X->Post);
      block(X->Body, 1);
      break;
    }

    case Node::Kind::RangeStmt: {
      const auto *X = cast<RangeStmt>(S);
      print(tok::kw_for, blank);
      if (X->Key) {
        if (X->Alias == RangeStmt::AliasKind::Key) {
          setPos(X->AliasLoc);
          print(tok::and_);
        }
        expr(X->Key);
        if (X->Value) {
          // use position of value following the comma as comma position for
          // correct comment placement
          setPos(X->Value->Begin);
          print(tok::comma, blank);
          if (X->Alias == RangeStmt::AliasKind::Value) {
            setPos(X->AliasLoc);
            print(tok::and_);
          }
          expr(X->Value);
        }
        print(blank);
        setPos(X->OpLoc);
        print(X->Op, blank);
      }
      print(tok::kw_range, blank);
      expr(stripParens(X->X));
      print(blank);
      block(X->Body, 1);
      break;
    }

    default:
      print("BadStmt");
      break;
    }
  }

  //===--------------------------------------------------------------------===//
  // Declarations
  //===--------------------------------------------------------------------===//

  /// Reports for every spec of a run of const/var specs whether the type column
  /// must be kept.
  static std::vector<bool> keepTypeColumn(ArrayRef<Spec *> Specs) {
    std::vector<bool> M(Specs.size(), false);
    auto Populate = [&](size_t I, size_t J, bool KeepType) {
      if (KeepType)
        for (; I < J; ++I)
          M[I] = true;
    };

    long I0 = -1; // if I0 >= 0 we are in a run and I0 is the start of the run
    bool KeepType = false;
    for (size_t I = 0; I < Specs.size(); ++I) {
      const auto *T = cast<ValueSpec>(Specs[I]);
      if (!T->Values.empty()) {
        if (I0 < 0) {
          // start of a run of ValueSpecs with non-nil Values
          I0 = (long)I;
          KeepType = false;
        }
      } else if (I0 >= 0) {
        // end of a run
        Populate(I0, I, KeepType);
        I0 = -1;
      }
      if (T->Type)
        KeepType = true;
    }
    if (I0 >= 0)
      Populate(I0, Specs.size(), KeepType);
    return M;
  }

  void valueSpec(const ValueSpec *S, bool KeepType) {
    identList(S->Names, false); // always present
    int ExtraTabs = 3;
    if (S->Type || KeepType) {
      print(vtab);
      --ExtraTabs;
    }
    if (S->Type)
      expr(S->Type);
    if (!S->Values.empty()) {
      print(vtab, tok::assign, blank);
      exprList(SourceLoc(), asConst(S->Values), 1, 0, SourceLoc());
      --ExtraTabs;
    }
    if (hasLineComment(S->End)) {
      for (; ExtraTabs > 0; --ExtraTabs)
        print(vtab);
    }
  }

  /// A backquoted import path becomes a double-quoted one when that needs no
  /// escapes; anything else (also header paths) is kept as written.
  void importPath(const BasicLit *Lit) {
    StringRef T = Lit->Text;
    if (T.size() >= 2 && T.front() == '`' && T.back() == '`') {
      StringRef Body = T.drop_front().drop_back();
      bool Plain = true;
      for (unsigned char C : Body)
        if (C == '"' || C == '\\' || C < ' ' || C == 0x7f)
          Plain = false;
      if (Plain) {
        setPos(Lit->Begin);
        printLit(("\"" + Body + "\"").str(), tok::string_lit);
        return;
      }
    }
    expr(Lit);
  }

  void spec(const Spec *Sp, size_t N, bool DoIndent) {
    switch (Sp->getKind()) {
    case Node::Kind::ImportSpec: {
      const auto *S = cast<ImportSpec>(Sp);
      if (S->Name) {
        expr(S->Name);
        print(blank);
      }
      importPath(S->Path);
      break;
    }
    case Node::Kind::ValueSpec: {
      const auto *S = cast<ValueSpec>(Sp);
      identList(S->Names, DoIndent); // always present
      if (S->Type) {
        print(blank);
        expr(S->Type);
      }
      if (!S->Values.empty()) {
        print(blank, tok::assign, blank);
        exprList(SourceLoc(), asConst(S->Values), 1, 0, SourceLoc());
      }
      break;
    }
    case Node::Kind::TypeSpec: {
      const auto *S = cast<TypeSpec>(Sp);
      expr(S->Name);
      if (S->TypeParams)
        parameters(S->TypeParams, ParamMode::typeTParam);
      if (N == 1)
        print(blank);
      else
        print(vtab);
      if (S->AssignLoc.isValid())
        print(tok::assign, blank);
      if (S->Extends) { // `type C extends P { ... }` (TYP-16)
        setPos(S->ExtendsLoc);
        print(Word{"extends"}, blank);
        expr(S->Extends);
        fieldList(cast<StructType>(S->Type)->Fields, true, true);
        break;
      }
      expr(S->Type);
      break;
    }
    default:
      print("BadSpec");
      break;
    }
  }

  /// Attributes before a declaration: one line each if the source has them on
  /// separate lines.
  void declAttrs(ArrayRef<Attribute *> Attrs, SourceLoc Next) {
    for (size_t I = 0; I < Attrs.size(); ++I) {
      attribute(Attrs[I]);
      SourceLoc After = I + 1 < Attrs.size() ? Attrs[I + 1]->Begin : Next;
      if (lineFor(After) > lineFor(Attrs[I]->End))
        linebreak(lineFor(After), 1, ignore, false);
      else
        print(blank);
    }
  }

  void genDecl(const GenDecl *D) {
    setPos(D->Begin);
    declAttrs(D->Attrs, D->KeywordLoc);
    setPos(D->KeywordLoc);
    print(D->Keyword, blank);

    if (D->Lparen.isValid() || D->Specs.size() != 1) {
      // group of parenthesized declarations
      setPos(D->Lparen);
      print(tok::l_paren);
      size_t N = D->Specs.size();
      if (N > 0) {
        print(indent, formfeed);
        int Line = 0;
        if (N > 1 &&
            (D->Keyword == tok::kw_const || D->Keyword == tok::kw_var)) {
          // two or more grouped const/var declarations: determine if the type
          // column must be kept
          std::vector<bool> Keep = keepTypeColumn(D->Specs);
          for (size_t I = 0; I < N; ++I) {
            if (I > 0)
              linebreak(lineFor(D->Specs[I]->Begin), 1, ignore,
                        linesFrom(Line) > 0);
            recordLine(&Line);
            valueSpec(cast<ValueSpec>(D->Specs[I]), Keep[I]);
          }
        } else {
          for (size_t I = 0; I < N; ++I) {
            if (I > 0)
              linebreak(lineFor(D->Specs[I]->Begin), 1, ignore,
                        linesFrom(Line) > 0);
            recordLine(&Line);
            spec(D->Specs[I], N, false);
          }
        }
        print(unindent, formfeed);
      }
      setPos(D->Rparen);
      print(tok::r_paren);
    } else if (!D->Specs.empty()) {
      // single declaration
      spec(D->Specs[0], 1, true);
    }
  }

  void funcDecl(const FuncDecl *D) {
    setPos(D->Begin);
    declAttrs(D->Attrs, D->Type->FuncLoc);
    setPos(D->Type->FuncLoc);
    // We have to save startCol only after emitting FUNC; otherwise it can be on
    // a different line (all whitespace preceding the FUNC is emitted only when
    // the FUNC is emitted).
    print(tok::kw_func, blank);
    int StartCol = out.Column - 5; // len("func ")
    if (D->Recv) {
      parameters(D->Recv, ParamMode::funcParam); // method: print receiver
      print(blank);
    }
    expr(D->Name);
    signature(D->Type);
    funcBody(distanceFrom(D->Type->FuncLoc, StartCol), vtab, D->Body);
  }

  void macroParams(const MacroDecl *D) {
    setPos(D->Lparen);
    print(tok::l_paren);
    for (size_t I = 0; I < D->Params.size(); ++I) {
      const MacroParam *P = D->Params[I];
      if (I > 0) {
        setPos(P->Begin);
        print(tok::comma, blank);
      }
      expr(P->Name);
      print(blank);
      if (P->Variadic)
        print(tok::ellipsis);
      setPos(P->KindLoc);
      print(Word{getMacroKindName(P->ParamKind)});
    }
    setPos(D->Rparen);
    print(tok::r_paren);
  }

  void decl(const Decl *D) {
    switch (D->getKind()) {
    case Node::Kind::BadDecl:
      setPos(D->Begin);
      print("BadDecl");
      break;
    case Node::Kind::GenDecl:
      genDecl(cast<GenDecl>(D));
      break;
    case Node::Kind::FuncDecl:
      funcDecl(cast<FuncDecl>(D));
      break;
    case Node::Kind::MacroDecl: {
      // Like a function: a small body that is on one line stays there.
      const auto *M = cast<MacroDecl>(D);
      setPos(M->MacroLoc);
      print(tok::kw_macro, blank);
      int StartCol = out.Column - 6; // len("macro ")
      expr(M->Name);
      macroParams(M);
      funcBody(distanceFrom(M->MacroLoc, StartCol), vtab, M->Body);
      break;
    }
    case Node::Kind::ProcMacroDecl: {
      const auto *M = cast<ProcMacroDecl>(D);
      setPos(M->MacroLoc);
      print(tok::kw_macro, blank);
      setPos(M->FuncLoc);
      print(tok::kw_func, blank);
      int StartCol = out.Column - 5;
      expr(M->Name);
      signature(M->Type);
      funcBody(distanceFrom(M->FuncLoc, StartCol), vtab, M->Body);
      break;
    }
    case Node::Kind::MacroCallDecl:
      expr(cast<MacroCallDecl>(D)->Call);
      break;
    default:
      print("BadDecl");
      break;
    }
  }

  static tok::Kind declToken(const Decl *D) {
    switch (D->getKind()) {
    case Node::Kind::GenDecl:
      return cast<GenDecl>(D)->Keyword;
    case Node::Kind::FuncDecl:
      return tok::kw_func;
    case Node::Kind::MacroDecl:
    case Node::Kind::ProcMacroDecl:
      return tok::kw_macro;
    case Node::Kind::MacroCallDecl:
      return tok::hash;
    default:
      return tok::illegal;
    }
  }

  void declList(ArrayRef<Decl *> List) {
    tok::Kind Tok = tok::illegal;
    for (const Decl *D : List) {
      tok::Kind Prev = Tok;
      Tok = declToken(D);
      // If the declaration token changed (e.g., from CONST to TYPE) or the next
      // declaration has documentation associated with it, print an empty line
      // between top-level declarations.
      if (!output.empty()) {
        // only print line break if we are not at the beginning of the output
        // (i.e., we are not printing only a partial program)
        int Min = 1;
        if (Prev != Tok || hasDoc(D->Begin))
          Min = 2;
        // start a new section if the next declaration is a function that spans
        // multiple lines (see also issue #19544)
        linebreak(lineFor(D->Begin), Min, ignore,
                  Tok == tok::kw_func && numLines(D) > 1);
      }
      decl(D);
    }
  }

  void file(const ast::File *F) {
    setPos(F->PackageLoc);
    print(tok::kw_package, blank);
    expr(F->Name);
    declList(F->Decls);
    print(newline);
  }

  //===--------------------------------------------------------------------===//
  // Members
  //===--------------------------------------------------------------------===//

  Shared &Sh;
  const SourceManager &SM;
  bool Raw;

  std::string output;
  int indentLevel = 0;
  int level = 0;
  unsigned mode = 0;
  bool endAlignment = false;
  bool impliedSemi = false;
  tok::Kind lastTok = tok::illegal, prevOpen = tok::illegal;
  std::vector<WS> wsbuf;
  Position pos, out, last;
  int *linePtr = nullptr;

  std::vector<PComment> Cmts;
  std::vector<PGroup> Groups;
  size_t cindex = 0;
  const PGroup *comment = nullptr;
  int64_t commentOffset = Infinity;
  bool commentNewline = false;

  uint32_t CachedPos = ~0u;
  int CachedLine = 0;
};

//===----------------------------------------------------------------------===//
// Entry points of the class
//===----------------------------------------------------------------------===//

void Printer::printNode(const Node *N) {
  if (const auto *E = dyn_cast<Expr>(N)) {
    expr(E);
  } else if (const auto *S = dyn_cast<Stmt>(N)) {
    // A labeled statement will un-indent to position the label. Set indent to
    // 1 so we do not get indent "underflow".
    if (isa<LabeledStmt>(S))
      indentLevel = 1;
    stmt(S, false);
  } else if (const auto *D = dyn_cast<Decl>(N)) {
    decl(D);
  } else if (const auto *Sp = dyn_cast<Spec>(N)) {
    spec(Sp, 1, false);
  } else if (const auto *F = dyn_cast<ast::File>(N)) {
    file(F);
  }
}

std::string Printer::finish() {
  impliedSemi = false; // EOF acts like a newline
  Position End;
  End.Offset = Infinity;
  End.Line = Infinity;
  End.Column = 1;
  flush(End, tok::eof);
  return std::move(output);
}

} // namespace

std::string gopp::printFile(const SourceManager &SM, FileID F,
                            const ast::File *AST) {
  Shared Sh;
  Sh.SM = &SM;
  Sh.File = F;
  Sh.Src = SM.getBuffer(F);
  Sh.AST = AST;
  Printer P(Sh, /*Raw=*/false, /*WithComments=*/true);
  P.printNode(AST);
  std::string Raw = P.finish();

  std::string Aligned;
  TabWriter TW(Aligned, /*MinWidth=*/0, /*TabWidth=*/8, /*Padding=*/1, ' ',
               TabWriter::DiscardEmptyColumns | TabWriter::TabIndent);
  TW.write(Raw);
  TW.flush();
  return trim(Aligned);
}
