//===- Format.cpp - gopp fmt driver ---------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Parse, sort the imports, print. The printer is in Printer.cpp.
//
// Import sorting (go/ast SortImports) is done on the text before printing: the
// specs of a parenthesized import declaration that stand on successive lines
// form a run, each run is sorted by (path, name, comment) and exact duplicates
// are removed. A run with a header import is left alone: C/C++ header order can
// matter. The sorted text is parsed again, so the printer always works on a
// consistent tree.
//
//===----------------------------------------------------------------------===//

#include "gopp/Format/Format.h"

#include "gopp/AST/ASTDumper.h"
#include "gopp/Parse/Parser.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <optional>

using namespace gopp;
using namespace gopp::ast;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::StringRef;

bool gopp::isHeaderImportPath(StringRef Path) {
  Path = Path.trim();
  if (Path.size() >= 2 && (Path.front() == '"' || Path.front() == '`') &&
      Path.back() == Path.front())
    Path = Path.drop_front().drop_back();
  if (Path.starts_with("<"))
    return true;
  return Path.ends_with(".h") || Path.ends_with(".hh") ||
         Path.ends_with(".hpp") || Path.ends_with(".hxx");
}

namespace {

struct SpecLine {
  std::string Text;    // the line without indentation and trailing blanks
  StringRef Path;      // the path between the quotes
  StringRef Name;      // the local name or ""
  std::string Comment; // the text behind the spec
  bool Header = false;
  bool Moved = false; // comments lead the spec or sit inside it
};

/// The text behind a spec on its line: nothing, or one comment, optionally
/// behind a `;`. Returns false if anything else follows.
bool trailingComment(StringRef Rest, std::string &Comment) {
  Rest = Rest.ltrim(" \t");
  if (Rest.starts_with(";"))
    Rest = Rest.drop_front().ltrim(" \t");
  Rest = Rest.rtrim(" \t\r");
  if (Rest.empty()) {
    Comment.clear();
    return true;
  }
  // Block comments, then maybe a line comment.
  StringRef R = Rest;
  while (R.starts_with("/*")) {
    size_t E = R.find("*/");
    if (E == StringRef::npos)
      return false;
    R = R.substr(E + 2).ltrim(" \t");
  }
  if (R.empty() || R.starts_with("//")) {
    Comment = Rest.str();
    return true;
  }
  return false;
}

std::optional<std::string> sortImports(const SourceManager &SM,
                                       const ast::File *AST, StringRef Text,
                                       const FormatOptions &Opts) {
  // The lines of the text.
  std::vector<StringRef> Lines;
  {
    StringRef Rest = Text;
    while (!Rest.empty()) {
      auto [L, R] = Rest.split('\n');
      Lines.push_back(L);
      Rest = R;
    }
  }
  std::vector<std::string> Out(Lines.begin(), Lines.end());
  std::vector<bool> Drop(Lines.size(), false);
  bool Changed = false;

  auto lineOf = [&](SourceLoc L) { return (int)SM.getLineNumber(L) - 1; };

  for (const Decl *D : AST->Decls) {
    const auto *G = dyn_cast<GenDecl>(D);
    if (!G || G->Keyword != tok::kw_import || !G->Lparen.isValid())
      continue;

    // The runs of specs on successive lines.
    size_t I = 0;
    while (I < G->Specs.size()) {
      size_t J = I;
      std::vector<SpecLine> Run;
      bool Ok = true;
      while (J < G->Specs.size()) {
        const auto *S = cast<ImportSpec>(G->Specs[J]);
        int L0 = lineOf(S->Begin), L1 = lineOf(S->End);
        if (J > I) {
          int PrevEnd = lineOf(G->Specs[J - 1]->End);
          if (L0 != PrevEnd + 1)
            break;
        }
        // The spec must be alone on its line.
        SpecLine SL;
        if (L0 != L1 || L0 < 0 || (size_t)L0 >= Lines.size()) {
          Ok = false;
        } else {
          StringRef Line = Lines[L0];
          uint32_t LineStart =
              SM.getFileOffset(S->Begin) - (SM.getColumnNumber(S->Begin) - 1);
          StringRef Before =
              Text.substr(LineStart, SM.getFileOffset(S->Begin) - LineStart);
          // Block comments may lead the spec; they move with it.
          StringRef Lead = Before.trim(" \t");
          StringRef LeadRest = Lead;
          while (LeadRest.starts_with("/*")) {
            size_t E = LeadRest.find("*/");
            if (E == StringRef::npos)
              break;
            LeadRest = LeadRest.substr(E + 2).ltrim(" \t");
          }
          if (!LeadRest.empty()) {
            Ok = false;
          } else {
            uint32_t EndOff = SM.getFileOffset(S->End);
            StringRef After = Text.substr(EndOff).take_until(
                [](char C) { return C == '\n'; });
            if (!trailingComment(After, SL.Comment))
              Ok = false;
            SL.Text = Line.trim(" \t\r").str();
            SL.Moved = !Lead.empty();
            // Comments inside the spec move behind it (go/ast SortImports).
            std::string Inner;
            for (const Comment &C : AST->Comments) {
              if (C.Loc < S->Begin || !(C.Loc < S->End))
                continue;
              Inner +=
                  " " + Text.substr(SM.getFileOffset(C.Loc), C.Length).str();
            }
            if (!Inner.empty()) {
              SL.Moved = true;
              StringRef P = S->Path->Text;
              std::string Spec = Lead.empty() ? "" : Lead.str() + " ";
              if (S->Name)
                Spec += S->Name->Name.str().str() + " ";
              SL.Text = Spec + P.str() + Inner;
              if (!SL.Comment.empty())
                SL.Text += " " + SL.Comment;
            }
            if (S->Name)
              SL.Name = S->Name->Name.str();
          }
        }
        StringRef PathText = S->Path->Text;
        if (PathText.size() >= 2 &&
            (PathText.front() == '"' || PathText.front() == '`'))
          PathText = PathText.drop_front().drop_back();
        SL.Header = Opts.IsHeader ? Opts.IsHeader(PathText)
                                  : isHeaderImportPath(PathText);
        SL.Path = PathText;
        Run.push_back(std::move(SL));
        ++J;
      }

      bool AnyHeader = false;
      for (const SpecLine &SL : Run)
        AnyHeader |= SL.Header;
      if (Ok && !AnyHeader && Run.size() > 1) {
        int First = lineOf(G->Specs[I]->Begin);
        std::vector<size_t> Order(Run.size());
        for (size_t K = 0; K < Order.size(); ++K)
          Order[K] = K;
        std::stable_sort(Order.begin(), Order.end(), [&](size_t A, size_t B) {
          const SpecLine &X = Run[A], &Y = Run[B];
          if (X.Path != Y.Path)
            return X.Path < Y.Path;
          if (X.Name != Y.Name)
            return X.Name < Y.Name;
          return X.Comment < Y.Comment;
        });
        // Remove exact duplicates; the comment is kept.
        std::vector<size_t> Kept;
        for (size_t K : Order) {
          if (!Kept.empty()) {
            SpecLine &P = Run[Kept.back()];
            const SpecLine &C = Run[K];
            if (P.Path == C.Path && P.Name == C.Name) {
              bool CPlain = C.Comment.empty() && !C.Moved;
              bool PPlain = P.Comment.empty() && !P.Moved;
              if (CPlain)
                continue; // nothing is lost
              if (PPlain) {
                P = C; // the duplicate carries the comments
                continue;
              }
              if (P.Comment.empty() && !C.Moved) {
                // Leading comments of P, trailing comment of C: one line.
                P.Text += " " + C.Comment;
                P.Comment = C.Comment;
                continue;
              }
            }
          }
          Kept.push_back(K);
        }
        for (size_t K = 0; K < Run.size(); ++K) {
          if (K < Kept.size()) {
            if (Out[First + K] != Run[Kept[K]].Text)
              Changed = true;
            Out[First + K] = Run[Kept[K]].Text;
          } else {
            Drop[First + K] = true;
            Changed = true;
          }
        }
      }
      I = J;
    }
  }

  if (!Changed)
    return std::nullopt;
  std::string Result;
  for (size_t L = 0; L < Out.size(); ++L) {
    if (Drop[L])
      continue;
    Result += Out[L];
    if (L + 1 < Out.size() || Text.ends_with("\n"))
      Result += '\n';
  }
  return Result;
}

} // namespace

/// Sorts the imports (if asked) and prints the file. AST must be the tree of F
/// in SM and free of errors.
static std::string printSorted(SourceManager &SM, FileID F, ast::File *AST,
                               const ParseOptions &PO,
                               const FormatOptions &Opts) {
  if (Opts.SortImports && !AST->Imports.empty()) {
    StringRef Text = SM.getBuffer(F);
    if (std::optional<std::string> Sorted = sortImports(SM, AST, Text, Opts)) {
      llvm::Expected<FileID> F2 = SM.addMemoryBuffer("<sorted>", *Sorted);
      if (F2) {
        ast::ASTContext Ctx2;
        CollectingDiagnosticConsumer Quiet;
        DiagnosticsEngine QuietDiags(SM, Quiet, 1);
        ast::File *AST2 = parseFile(Ctx2, SM, *F2, QuietDiags, PO);
        if (!QuietDiags.hasErrors())
          return printFile(SM, *F2, AST2);
      } else {
        llvm::consumeError(F2.takeError());
      }
      // Sorting failed: print the original.
    }
  }
  return printFile(SM, F, AST);
}

/// Formats Text again. Returns false if it does not parse (never for our own
/// output).
static bool reformat(StringRef Text, const ParseOptions &PO,
                     const FormatOptions &Opts, std::string &Out) {
  SourceManager SM;
  llvm::Expected<FileID> F = SM.addMemoryBuffer("<fmt>", Text);
  if (!F) {
    llvm::consumeError(F.takeError());
    return false;
  }
  CollectingDiagnosticConsumer Quiet;
  DiagnosticsEngine Diags(SM, Quiet, 1);
  ast::ASTContext Ctx;
  ast::File *AST = parseFile(Ctx, SM, *F, Diags, PO);
  if (Diags.hasErrors())
    return false;
  Out = printSorted(SM, *F, AST, PO, Opts);
  return true;
}

/// go/printer is not idempotent in rare cases (a one-line function body whose
/// header was split over lines: the second pass breaks the body), and sorting
/// the imports needs each spec on a line of its own, which only the printed
/// text has. `gopp fmt` promises idempotence, so it formats until the text is
/// stable.
static void settle(const ParseOptions &PO, const FormatOptions &Opts,
                   std::string &Out) {
  constexpr int MaxPasses = 4;
  for (int I = 0; I < MaxPasses; ++I) {
    std::string Next;
    if (!reformat(Out, PO, Opts, Next) || Next == Out)
      return;
    Out = std::move(Next);
  }
}

/// True if the tree has a Bad node. The parser reports an error for each, so
/// this is a guard: whatever happens to the diagnostics, `gopp fmt` must never
/// print "BadExpr" into a file.
static bool hasBadNode(const ast::Node *Root) {
  llvm::SmallVector<const ast::Node *, 64> Work{Root};
  while (!Work.empty()) {
    const ast::Node *N = Work.pop_back_val();
    if (isa<ast::BadExpr, ast::BadStmt, ast::BadDecl>(N))
      return true;
    ast::forEachChild(
        N, [&](const char *, const ast::Node *C) { Work.push_back(C); });
  }
  return false;
}

bool gopp::formatFile(SourceManager &SM, FileID F, DiagnosticsEngine &Diags,
                      std::string &Out, const FormatOptions &Opts) {
  // The Go++ rules that need only syntax (`&T{}`, ...) do not stop a file
  // from being formatted, as type errors do not stop gofmt.
  ParseOptions PO;
  PO.CheckLanguageRules = false;
  ast::ASTContext Ctx;
  // A private engine without a limit: the parse must see every error even if
  // the caller's engine has reached its limit (it would stop the parser and
  // leave an empty tree, which prints as an empty file).
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Local(SM, Collect, 0);
  ast::File *AST = parseFile(Ctx, SM, F, Local, PO);
  for (Diagnostic &D : Collect.Diags)
    Diags.emit(std::move(D));
  if (Local.hasErrors())
    return false;
  return formatParsedFile(SM, F, AST, Diags, Out, Opts);
}

bool gopp::formatParsedFile(SourceManager &SM, FileID F, ast::File *AST,
                            DiagnosticsEngine &Diags, std::string &Out,
                            const FormatOptions &Opts) {
  if (hasBadNode(AST)) {
    Diags.report(SM.getStartLoc(F), diag::err_fmt_unreported_syntax_error);
    return false;
  }
  ParseOptions PO;
  PO.CheckLanguageRules = false;
  Out = printSorted(SM, F, AST, PO, Opts);
  settle(PO, Opts, Out);
  return true;
}

bool gopp::formatText(StringRef Name, StringRef Text, std::string &Out,
                      std::string *Errors, const FormatOptions &Opts) {
  SourceManager SM;
  llvm::Expected<FileID> F = SM.addMemoryBuffer(Name, Text);
  if (!F) {
    if (Errors)
      *Errors = llvm::toString(F.takeError());
    else
      llvm::consumeError(F.takeError());
    return false;
  }
  std::string Buf;
  llvm::raw_string_ostream OS(Buf);
  TextDiagnosticPrinter Printer(OS, SM);
  DiagnosticsEngine Diags(SM, Printer, 0);
  bool Ok = formatFile(SM, *F, Diags, Out, Opts);
  Diags.finish();
  if (Errors)
    *Errors = std::move(Buf);
  return Ok;
}
