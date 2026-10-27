//===- FuzzCheck.h - The properties the parser fuzzer checks ----*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// For any input text:
//   1. the lexer and parser return (no crash, no hang, no sanitizer report),
//      with the Go++ language rules on and off;
//   2. the tree is sound: every node range lies inside the file and is not
//      reversed, and there is a Bad node only if an error was reported;
//   3. the diagnostics can be rendered (carets, fix-its), and the text with
//      their fix-its applied parses without a crash;
//   4. the text cut into several files parses as one package on a thread
//      pool (parseFiles), with the diagnostics byte-equal to a serial run
//      (error limits 0 and 3) and the Bad-node check per file;
//   5. if the text parses without errors:
//      a. `gopp fmt` succeeds;
//      b. the output parses without errors, to the same syntax tree
//         (positions ignored; like gofmt, empty statements, redundant
//         parentheses, the spelling of numbers and the empty result list are
//         not part of the tree), with imports left unsorted;
//      c. formatting the output again gives the same text (idempotence);
//      d. every comment of the input is in the output (up to white space),
//         also with imports sorted (a duplicate import is dropped only when
//         it has no comment).
// The fuzz target (ParseFuzzer.cpp) runs one variant of 1-4 per input (two
// bits of its size pick it, so plain source files work as seeds) and always
// 5; the unit test that replays the seed corpus and the regressions
// (FuzzCorpusTest.cpp) runs all of them.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_TESTS_FUZZ_FUZZCHECK_H
#define GOPP_TESTS_FUZZ_FUZZCHECK_H

#include "gopp/AST/ASTDumper.h"
#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/SourceManager.h"
#include "gopp/Format/Format.h"
#include "gopp/Parse/Parser.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace gopp::fuzz {

struct Parsed {
  bool Ok = false;
  std::string Tree;                  // canonical dump
  std::vector<std::string> Comments; // normalized comment texts
};

inline std::vector<llvm::StringRef> splitLines(llvm::StringRef S) {
  std::vector<llvm::StringRef> Lines;
  while (!S.empty()) {
    auto [L, R] = S.split('\n');
    if (!L.trim().empty())
      Lines.push_back(L);
    S = R;
  }
  return Lines;
}

inline bool isNumberKind(llvm::StringRef S) {
  return S.contains(" int_lit ") || S.contains(" float_lit ") ||
         S.contains(" imag_lit ");
}

/// The dump of the tree without positions, and without what gofmt does not
/// keep (see the file comment). Roles are the "Name:" before the node kind.
inline std::string canonicalTree(llvm::StringRef Dump) {
  struct Dropped {
    size_t Indent;
    std::string Role;
  };
  std::vector<Dropped> DroppedParens;
  std::vector<llvm::StringRef> Lines = splitLines(Dump);
  std::string Out;
  for (size_t N = 0; N < Lines.size(); ++N) {
    // Drop the position "<l:c, l:c>" (the first " <" of a line is it: text of
    // literals comes later).
    std::string LineText = Lines[N].str();
    // The scanner drops carriage returns of raw strings, the printer too.
    LineText.erase(std::remove(LineText.begin(), LineText.end(), '\r'),
                   LineText.end());
    size_t Lt = LineText.find(" <");
    if (Lt != std::string::npos) {
      size_t Gt = LineText.find('>', Lt);
      if (Gt != std::string::npos)
        LineText.erase(Lt + 1, Gt - Lt); // keeps one blank
    }
    llvm::StringRef Line = LineText;
    size_t Indent = Line.size() - Line.ltrim(' ').size();
    llvm::StringRef Body = Line.ltrim(' ');
    while (!DroppedParens.empty() && Indent <= DroppedParens.back().Indent)
      DroppedParens.pop_back();
    auto [Role, Rest] = Body.split(": ");
    std::string RestText = Rest.str();
    llvm::StringRef R = RestText;
    if (R.starts_with("EmptyStmt"))
      continue;
    if (Role == "Results" && R.starts_with("FieldList")) {
      size_t NextIndent = 0;
      if (N + 1 < Lines.size())
        NextIndent = Lines[N + 1].size() - Lines[N + 1].ltrim(' ').size();
      if (N + 1 >= Lines.size() || NextIndent <= Indent)
        continue; // an empty result list `()`
    }
    std::string RoleText = Role.str();
    if (R.starts_with("ParenExpr")) {
      bool Direct =
          !DroppedParens.empty() && Indent == DroppedParens.back().Indent + 2;
      DroppedParens.push_back(
          {Indent, Direct ? DroppedParens.back().Role : RoleText});
      continue;
    }
    if (!DroppedParens.empty() && Indent == DroppedParens.back().Indent + 2)
      RoleText = DroppedParens.back().Role;
    if (R.starts_with("BasicLit") && isNumberKind(R)) {
      // 0X1 -> 0x1, 1E3 -> 1e3 and 0765i -> 765i
      std::string L;
      for (char C : RestText)
        L.push_back((char)std::tolower((unsigned char)C));
      RestText = L;
      size_t Sp = RestText.find("imag_lit ");
      if (Sp != std::string::npos) {
        size_t V = Sp + 9;
        std::string Val = RestText.substr(V);
        bool Plain = Val.find_first_of(".e") == std::string::npos &&
                     !(Val.size() > 1 && Val[0] == '0' &&
                       (Val[1] == 'x' || Val[1] == 'b' || Val[1] == 'o'));
        if (Plain) {
          size_t F = Val.find_first_not_of("0_");
          Val = F == std::string::npos ? "i" : Val.substr(F);
          if (Val == "i")
            Val = "0i";
          RestText = RestText.substr(0, V) + Val;
        }
      }
    }
    // A backquoted import path becomes a double-quoted one.
    R = RestText; // RestText may have been replaced above
    if (Role == "Path" && R.starts_with("BasicLit") &&
        R.contains(" string_lit `") && R.ends_with("`")) {
      size_t B = RestText.find('`');
      llvm::StringRef Body2 = llvm::StringRef(RestText).substr(B + 1);
      if (!Body2.empty())
        Body2 = Body2.drop_back();
      if (!Body2.empty() &&
          Body2.find_first_of("\"\\") == llvm::StringRef::npos) {
        RestText = RestText.substr(0, B) + "\"" + Body2.str() + "\"";
      }
    }
    Out.append(Indent - 2 * DroppedParens.size(), ' ');
    Out += RoleText + ": " + RestText + "\n";
  }
  return Out;
}

inline std::string collapseSpace(llvm::StringRef S) {
  std::string Out;
  bool InSpace = true;
  for (char C : S) {
    if (C == '\r')
      continue; // the scanner drops carriage returns inside comments
    // gofmt also drops control characters at the start of a comment line
    if ((unsigned char)C <= ' ') {
      if (!InSpace)
        Out.push_back(' ');
      InSpace = true;
    } else {
      Out.push_back(C);
      InSpace = false;
    }
  }
  while (!Out.empty() && Out.back() == ' ')
    Out.pop_back();
  return Out;
}

/// Parses Text with the options of `gopp fmt`.
inline Parsed parseText(llvm::StringRef Text) {
  Parsed P;
  SourceManager SM;
  llvm::Expected<FileID> F = SM.addMemoryBuffer("fuzz.gopp", Text);
  if (!F) {
    llvm::consumeError(F.takeError());
    return P;
  }
  CollectingDiagnosticConsumer Consumer;
  DiagnosticsEngine Diags(SM, Consumer, 1);
  ast::ASTContext Ctx;
  ParseOptions Opts;
  Opts.CheckLanguageRules = false;
  ast::File *AST = parseFile(Ctx, SM, *F, Diags, Opts);
  P.Ok = !Diags.hasErrors();
  if (!P.Ok)
    return P;
  std::string Dump;
  llvm::raw_string_ostream OS(Dump);
  ast::dump(OS, SM, AST);
  P.Tree = canonicalTree(Dump);
  llvm::StringRef Src = SM.getBuffer(*F);
  for (const ast::Comment &C : AST->Comments) {
    llvm::StringRef T = Src.substr(SM.getFileOffset(C.Loc), C.Length);
    P.Comments.push_back(collapseSpace(T));
  }
  return P;
}

/// Property 2: every node range lies inside the file and is not reversed; a
/// Bad node means an error was reported. Returns the reason, or "".
inline std::string checkTree(const SourceManager &SM, FileID F,
                             const ast::File *AST, bool HadErrors) {
  SourceLoc Begin = SM.getStartLoc(F), End = SM.getEndLoc(F);
  llvm::SmallVector<const ast::Node *, 64> Work{AST};
  while (!Work.empty()) {
    const ast::Node *N = Work.pop_back_val();
    SourceLoc B = N->getBegin(), E = N->getEnd();
    if ((B.isValid() && (B < Begin || End < B)) ||
        (E.isValid() && (E < Begin || End < E)))
      return std::string("a ") + N->getKindName() + " lies outside the file";
    if (B.isValid() && E.isValid() && E < B)
      return std::string("a ") + N->getKindName() + " ends before it starts";
    if (!HadErrors && llvm::isa<ast::BadExpr, ast::BadStmt, ast::BadDecl>(N))
      return std::string("a ") + N->getKindName() + " without an error";
    ast::forEachChild(
        N, [&](const char *, const ast::Node *C) { Work.push_back(C); });
  }
  return "";
}

/// Properties 1-3: parse with the language rules on or off, check the tree,
/// dump it, render every diagnostic, and parse the text with the fix-its
/// applied (that must not crash either).
inline std::string checkDiagnostics(llvm::StringRef Text, bool Rules) {
  SourceManager SM;
  llvm::Expected<FileID> F = SM.addMemoryBuffer("fuzz.gopp", Text);
  if (!F) {
    llvm::consumeError(F.takeError());
    return "";
  }
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, /*ErrorLimit=*/0);
  ast::ASTContext Ctx;
  ParseOptions PO;
  PO.CheckLanguageRules = Rules;
  ast::File *AST = parseFile(Ctx, SM, *F, Diags, PO);
  std::string Why = checkTree(SM, *F, AST, Diags.hasErrors());
  if (!Why.empty())
    return Why;
  llvm::raw_null_ostream Null;
  ast::dump(Null, SM, AST);
  TextDiagnosticPrinter Text1(Null, SM);
  JSONDiagnosticPrinter Json(Null, SM);
  for (const Diagnostic &D : Collect.Diags) {
    Text1.handleDiagnostic(D);
    Json.handleDiagnostic(D);
  }
  if (!Diags.hasErrors())
    return "";
  std::string Fixed = applyFixIts(SM, *F, Collect.Diags);
  SourceManager SM2;
  llvm::Expected<FileID> F2 = SM2.addMemoryBuffer("fixed.gopp", Fixed);
  if (!F2) {
    llvm::consumeError(F2.takeError());
    return "";
  }
  CollectingDiagnosticConsumer Collect2;
  DiagnosticsEngine Diags2(SM2, Collect2, 10);
  ast::ASTContext Ctx2;
  ast::File *AST2 = parseFile(Ctx2, SM2, *F2, Diags2, PO);
  return checkTree(SM2, *F2, AST2, Diags2.hasErrors() || Diags2.limitReached());
}

/// Property 4: the text cut at NUL bytes (or in halves if it has none) is a
/// package of several files, parsed on two threads.
inline std::string checkPackage(llvm::StringRef Text) {
  llvm::SmallVector<llvm::StringRef, 4> Pieces;
  Text.split(Pieces, '\0');
  if (Pieces.size() < 2) {
    Pieces.clear();
    Pieces.push_back(Text.substr(0, Text.size() / 2));
    Pieces.push_back(Text.substr(Text.size() / 2));
  }
  SourceManager SM;
  std::vector<FileID> Files;
  for (llvm::StringRef P : Pieces) {
    llvm::Expected<FileID> F = SM.addMemoryBuffer("piece.gopp", P);
    if (!F) {
      llvm::consumeError(F.takeError());
      return "";
    }
    Files.push_back(*F);
  }
  // Which files have errors, from a parse of each file alone.
  std::vector<bool> HadErrors;
  for (FileID F : Files) {
    CollectingDiagnosticConsumer C;
    DiagnosticsEngine D(SM, C, /*ErrorLimit=*/0);
    ast::ASTContext Ctx;
    parseFile(Ctx, SM, F, D);
    HadErrors.push_back(D.hasErrors());
  }
  // The diagnostics of the parallel run are those of a serial run, with no
  // error limit and with a small one.
  for (unsigned Limit : {0u, 3u}) {
    std::string Rendered[2];
    for (unsigned Threads : {2u, 1u}) {
      CollectingDiagnosticConsumer Collect;
      DiagnosticsEngine Diags(SM, Collect, Limit);
      std::vector<ParsedFile> Parsed =
          parseFiles(SM, Files, Diags, {}, Threads);
      llvm::raw_string_ostream OS(Rendered[Threads == 1]);
      TextDiagnosticPrinter Printer(OS, SM);
      for (const Diagnostic &D : Collect.Diags)
        Printer.handleDiagnostic(D);
      OS.flush();
      if (Threads != 2)
        continue;
      // Past the limit a parallel run still builds every tree.
      for (size_t I = 0; I < Files.size(); ++I) {
        std::string Why = checkTree(SM, Files[I], Parsed[I].AST, HadErrors[I]);
        if (!Why.empty())
          return Why;
      }
    }
    if (Rendered[0] != Rendered[1])
      return "the parallel parse reports other diagnostics than the serial "
             "one\n--- parallel\n" +
             Rendered[0] + "--- serial\n" + Rendered[1];
  }
  return "";
}

/// Property 5: the formatter. Returns the empty string if it holds, else the
/// reason.
inline std::string checkFormat(llvm::StringRef Text) {
  Parsed In = parseText(Text);
  if (!In.Ok)
    return ""; // property 1 only: it returned
  FormatOptions Unsorted;
  Unsorted.SortImports = false;
  std::string Out, Err;
  if (!formatText("fuzz.gopp", Text, Out, &Err, Unsorted))
    return "fmt failed on valid input:\n" + Err;
  Parsed Re = parseText(Out);
  if (!Re.Ok)
    return "the output does not parse:\n" + Out;
  if (Re.Tree != In.Tree)
    return "the syntax tree changed\n--- input tree\n" + In.Tree +
           "--- output tree\n" + Re.Tree + "--- output\n" + Out;
  if (Re.Comments != In.Comments)
    return "the comments changed\n--- output\n" + Out;
  // Idempotence, with the default options (imports sorted).
  std::string Once, Twice;
  if (!formatText("fuzz.gopp", Text, Once, &Err))
    return "fmt failed on valid input:\n" + Err;
  if (!formatText("fuzz.gopp", Once, Twice, &Err))
    return "fmt failed on its own output:\n" + Err + "\n" + Once;
  if (Once != Twice)
    return "fmt is not idempotent\n--- first\n" + Once + "--- second\n" + Twice;
  // Sorting and removing duplicate imports moves comments but loses none.
  Parsed Sorted = parseText(Once);
  if (!Sorted.Ok)
    return "the output with sorted imports does not parse:\n" + Once;
  std::vector<std::string> A = In.Comments, B = Sorted.Comments;
  std::sort(A.begin(), A.end());
  std::sort(B.begin(), B.end());
  if (A != B)
    return "the comments changed with sorted imports\n--- output\n" + Once;
  return "";
}

/// Variant (0-3) of properties 1-4: bit 0 turns the language rules off, bit 1
/// parses the text as a package of several files instead.
inline std::string checkVariant(llvm::StringRef Text, unsigned Variant) {
  if (Variant & 2)
    return checkPackage(Text);
  return checkDiagnostics(Text, !(Variant & 1));
}

/// All checks for one fuzz input: the variant its size picks, then the
/// formatter. Returns the empty string if they hold, else the reason.
inline std::string checkText(llvm::StringRef Text) {
  std::string Why = checkVariant(Text, Text.size() & 3);
  if (!Why.empty())
    return Why;
  return checkFormat(Text);
}

/// Every check, every variant (the corpus replay).
inline std::string checkAll(llvm::StringRef Text) {
  for (unsigned V = 0; V < 4; ++V) {
    std::string Why = checkVariant(Text, V);
    if (!Why.empty())
      return Why;
  }
  return checkFormat(Text);
}

} // namespace gopp::fuzz

#endif // GOPP_TESTS_FUZZ_FUZZCHECK_H
