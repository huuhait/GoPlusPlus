//===- DiagnosticsTest.cpp - Diagnostics engine and printers --------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Diagnostics.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

#include <set>
#include <string>

using namespace gopp;

namespace {

/// A source plus a text printer writing into a string.
struct TextFixture {
  explicit TextFixture(llvm::StringRef Text, unsigned Limit = 0)
      : OS(Out), Printer(OS, SM), Diags(SM, Printer, Limit) {
    llvm::Expected<FileID> F = SM.addMemoryBuffer("f.gopp", Text);
    if (!F)
      llvm::report_fatal_error("cannot add buffer");
    File = *F;
  }
  SourceLoc at(unsigned Off) { return SM.getLoc(File, Off); }
  SourceRange range(unsigned B, unsigned E) { return {at(B), at(E)}; }
  std::string str() {
    OS.flush();
    return Out;
  }
  SourceManager SM;
  std::string Out;
  llvm::raw_string_ostream OS;
  TextDiagnosticPrinter Printer;
  DiagnosticsEngine Diags;
  FileID File;
};

TEST(Diagnostics, CaretOnly) {
  TextFixture T("x := 1 +\n");
  T.Diags.report(T.at(8), Severity::Error, "expected operand");
  EXPECT_EQ(T.str(), "f.gopp:1:9: error: expected operand\n"
                     "    1 | x := 1 +\n"
                     "      |         ^\n");
}

TEST(Diagnostics, PrimaryRangeAndSecondaryRange) {
  TextFixture T("a := b + c\n");
  T.Diags.report(T.at(7), Severity::Error, "mismatched operands")
      .range(T.range(7, 8))
      .secondary(T.range(5, 6))
      .secondary(T.range(9, 10));
  EXPECT_EQ(T.str(), "f.gopp:1:8: error: mismatched operands\n"
                     "    1 | a := b + c\n"
                     "      |      - ^ -\n");
  // The primary range wins where ranges overlap; the caret wins over both.
  T.Out.clear();
  T.Diags.report(T.at(7), Severity::Error, "wide")
      .range(T.range(5, 10))
      .secondary(T.range(5, 6));
  EXPECT_EQ(T.str(), "f.gopp:1:8: error: wide\n"
                     "    1 | a := b + c\n"
                     "      |      ~~^~~\n");
}

TEST(Diagnostics, WarningAndNote) {
  TextFixture T("var x int\nvar x string\n");
  T.Diags.report(T.at(14), Severity::Error, "x redeclared")
      .note(T.at(4), "previous declaration of x", T.range(4, 9));
  EXPECT_EQ(T.str(), "f.gopp:2:5: error: x redeclared\n"
                     "    2 | var x string\n"
                     "      |     ^\n"
                     "f.gopp:1:5: note: previous declaration of x\n"
                     "    1 | var x int\n"
                     "      |     ^~~~~\n");
  T.Out.clear();
  T.Diags.report(T.at(0), Severity::Warning, "unused");
  EXPECT_EQ(T.str(), "f.gopp:1:1: warning: unused\n"
                     "    1 | var x int\n"
                     "      | ^\n");
  EXPECT_EQ(T.Diags.getNumErrors(), 1u);
  EXPECT_EQ(T.Diags.getNumWarnings(), 1u);
}

TEST(Diagnostics, FixItLine) {
  TextFixture T("s := \xE2\x80\x9Chi\xE2\x80\x9D\n");
  T.Diags.report(T.at(5), Severity::Error, "curly quotation mark")
      .range(T.range(5, 8))
      .fixit(T.range(5, 8), "\"");
  EXPECT_EQ(T.str(), "f.gopp:1:6: error: curly quotation mark\n"
                     "    1 | s := \xE2\x80\x9Chi\xE2\x80\x9D\n"
                     "      |      ^\n"
                     "      |      \"\n");
}

TEST(Diagnostics, TabsExpandToTabStop4) {
  TextFixture T("\tx := ;\n");
  T.Diags.report(T.at(6), Severity::Error, "expected operand");
  EXPECT_EQ(T.str(), "f.gopp:1:7: error: expected operand\n"
                     "    1 |     x := ;\n"
                     "      |          ^\n");
}

TEST(Diagnostics, WideAndInvalidCharacters) {
  // A CJK character is two cells wide; an invalid byte is spelled out.
  TextFixture T(std::string("\xE4\xB8\x96 x \xFF y\n"));
  T.Diags.report(T.at(4), Severity::Error, "at x");
  T.Diags.report(T.at(6), Severity::Error, "at ff");
  EXPECT_EQ(T.str(), "f.gopp:1:5: error: at x\n"
                     "    1 | \xE4\xB8\x96 x <FF> y\n"
                     "      |    ^\n"
                     "f.gopp:1:7: error: at ff\n"
                     "    1 | \xE4\xB8\x96 x <FF> y\n"
                     "      |      ^\n");
}

TEST(Diagnostics, CRLFLineIsPrintedWithoutCR) {
  TextFixture T("a b\r\nc\r\n");
  T.Diags.report(T.at(2), Severity::Error, "here");
  EXPECT_EQ(T.str(), "f.gopp:1:3: error: here\n"
                     "    1 | a b\n"
                     "      |   ^\n");
}

TEST(Diagnostics, NoLocationHasNoSnippet) {
  TextFixture T("x\n");
  T.Diags.report(SourceLoc(), Severity::Error, "cannot open file");
  EXPECT_EQ(T.str(), "error: cannot open file\n");
}

TEST(Diagnostics, GutterGrowsWithLineNumber) {
  std::string Text(99999, '\n');
  Text += "x\n";
  TextFixture T(Text);
  T.Diags.report(T.at(99999), Severity::Error, "late");
  EXPECT_EQ(T.str(), "f.gopp:100000:1: error: late\n"
                     "100000 | x\n"
                     "       | ^\n");
}

TEST(Diagnostics, MultiLineRangeIsClippedToTheLine) {
  TextFixture T("{\n  a\n}\n");
  T.Diags.report(T.at(0), Severity::Error, "unclosed").range(T.range(0, 7));
  EXPECT_EQ(T.str(), "f.gopp:1:1: error: unclosed\n"
                     "    1 | {\n"
                     "      | ^\n");
}

TEST(Diagnostics, ErrorLimit) {
  TextFixture T("a\n", /*Limit=*/2);
  for (int I = 0; I < 5; ++I)
    T.Diags.report(T.at(0), Severity::Error, "e" + std::to_string(I));
  EXPECT_TRUE(T.Diags.limitReached());
  EXPECT_EQ(T.Diags.getNumErrors(), 2u);
  EXPECT_EQ(T.str(), "f.gopp:1:1: error: e0\n"
                     "    1 | a\n"
                     "      | ^\n"
                     "f.gopp:1:1: error: e1\n"
                     "    1 | a\n"
                     "      | ^\n"
                     "error: too many errors\n");
}

TEST(Diagnostics, WarningsAreNotLimited) {
  TextFixture T("a\n", /*Limit=*/1);
  for (int I = 0; I < 5; ++I)
    T.Diags.report(T.at(0), Severity::Warning, "w");
  EXPECT_FALSE(T.Diags.limitReached());
  EXPECT_EQ(T.Diags.getNumWarnings(), 5u);
}

TEST(Diagnostics, DefaultLimitIsTen) {
  EXPECT_EQ(DiagnosticsEngine::DefaultErrorLimit, 10u);
  SourceManager SM;
  CollectingDiagnosticConsumer C;
  DiagnosticsEngine E(SM, C);
  for (int I = 0; I < 25; ++I)
    E.report(SourceLoc(), Severity::Error, "x");
  EXPECT_EQ(C.Diags.size(), 11u); // ten errors and "too many errors"
  EXPECT_EQ(C.Diags.back().ID, diag::err_too_many_errors);
  EXPECT_EQ(C.Diags.back().Message, "too many errors");
  EXPECT_EQ(E.getNumErrors(), 10u);
}

TEST(Diagnostics, UnlimitedWhenZero) {
  SourceManager SM;
  CollectingDiagnosticConsumer C;
  DiagnosticsEngine E(SM, C, 0);
  for (int I = 0; I < 100; ++I)
    E.report(SourceLoc(), Severity::Error, "x");
  EXPECT_EQ(E.getNumErrors(), 100u);
  EXPECT_FALSE(E.limitReached());
}

struct JSONFixture {
  JSONFixture() : OS(Out), Printer(OS, SM), Diags(SM, Printer, 0) {
    File = *SM.addMemoryBuffer("f.gopp", "var x int\nvar x string\n");
  }
  SourceLoc at(unsigned Off) { return SM.getLoc(File, Off); }
  SourceManager SM;
  std::string Out;
  llvm::raw_string_ostream OS;
  JSONDiagnosticPrinter Printer;
  DiagnosticsEngine Diags;
  FileID File;
};

TEST(Diagnostics, JSONGolden) {
  JSONFixture T;
  T.Diags.report(T.at(14), Severity::Error, "x redeclared")
      .range({T.at(14), T.at(15)})
      .secondary({T.at(4), T.at(5)})
      .note(T.at(4), "previous declaration")
      .fixit({T.at(14), T.at(15)}, "y");
  T.OS.flush();
  EXPECT_EQ(
      T.Out,
      "{\"severity\":\"error\",\"id\":\"custom\",\"message\":\"x "
      "redeclared\",\"file\":\"f.gopp\","
      "\"line\":2,\"column\":5,\"offset\":14,"
      "\"range\":{\"begin\":{\"line\":2,\"column\":5,\"offset\":14},"
      "\"end\":{\"line\":2,\"column\":6,\"offset\":15}},"
      "\"secondaryRanges\":[{\"begin\":{\"line\":1,\"column\":5,\"offset\":4},"
      "\"end\":{\"line\":1,\"column\":6,\"offset\":5}}],"
      "\"notes\":[{\"id\":\"custom\",\"message\":\"previous "
      "declaration\",\"file\":\"f.gopp\","
      "\"line\":1,\"column\":5,\"offset\":4}],"
      "\"fixits\":[{\"range\":{\"begin\":{\"line\":2,\"column\":5,\"offset\":"
      "14},"
      "\"end\":{\"line\":2,\"column\":6,\"offset\":15}},\"text\":\"y\"}]}\n");
}

TEST(Diagnostics, JSONLinesParseAndEscape) {
  JSONFixture T;
  T.Diags.report(T.at(0), Severity::Error,
                 "quote \" backslash \\ newline \n tab \t ctrl \x01");
  T.Diags.report(SourceLoc(), Severity::Warning, "no position");
  T.OS.flush();
  llvm::StringRef Rest = T.Out;
  unsigned N = 0;
  while (!Rest.empty()) {
    auto [Line, Tail] = Rest.split('\n');
    Rest = Tail;
    llvm::Expected<llvm::json::Value> V = llvm::json::parse(Line);
    ASSERT_TRUE(static_cast<bool>(V)) << Line.str();
    ++N;
    const llvm::json::Object *O = V->getAsObject();
    ASSERT_TRUE(O);
    if (N == 1) {
      EXPECT_EQ(*O->getString("message"),
                "quote \" backslash \\ newline \n tab \t ctrl \x01");
    } else {
      EXPECT_EQ(*O->getString("severity"), "warning");
      EXPECT_EQ(O->get("line"), nullptr);
    }
  }
  EXPECT_EQ(N, 2u);
}

TEST(Diagnostics, JSONWithInvalidUTF8InMessage) {
  JSONFixture T;
  // llvm::json replaces bytes that are not UTF-8; the line must stay valid.
  T.Diags.report(T.at(0), Severity::Error, std::string("bad \xff byte"));
  T.OS.flush();
  llvm::StringRef Line = llvm::StringRef(T.Out).trim();
  EXPECT_TRUE(static_cast<bool>(llvm::json::parse(Line)));
}

TEST(DiagnosticTable, NamesAreUniqueAndMatchTheSeverity) {
  std::set<std::string> Names;
  for (unsigned I = 0; I < diag::NUM_DIAGS; ++I) {
    auto ID = static_cast<diag::ID>(I);
    std::string Name = getDiagName(ID).str();
    EXPECT_TRUE(Names.insert(Name).second) << Name;
    EXPECT_FALSE(getDiagFormat(ID).empty()) << Name;
    Severity S = getDiagSeverity(ID);
    if (llvm::StringRef(Name).starts_with("err_"))
      EXPECT_EQ(S, Severity::Error) << Name;
    else if (llvm::StringRef(Name).starts_with("warn_"))
      EXPECT_EQ(S, Severity::Warning) << Name;
    else if (llvm::StringRef(Name).starts_with("note_"))
      EXPECT_EQ(S, Severity::Note) << Name;
    else
      EXPECT_EQ(Name, "custom");
  }
}

TEST(DiagnosticTable, FormatFillsPlaceholders) {
  EXPECT_EQ(formatDiagnostic("expected %0, found %1", {"';'", "'}'"}),
            "expected ';', found '}'");
  EXPECT_EQ(formatDiagnostic("%1 %0 %1", {"a", "b"}), "b a b");
  EXPECT_EQ(formatDiagnostic("100%% %0", {"x"}), "100% x");
  // A missing argument is empty; a lone '%' stays.
  EXPECT_EQ(formatDiagnostic("a %0 and %3", {"b"}), "a b and ");
  EXPECT_EQ(formatDiagnostic("50% off %", {}), "50% off %");
}

TEST(Diagnostics, TableDiagnosticWithArgumentsAndNote) {
  TextFixture T("#M(a, [b)\n");
  T.Diags.report(T.at(8), diag::err_macro_args_mismatched_expected)
          .note(T.at(6), diag::note_to_match_this, {"["})
      << ")" << "]";
  EXPECT_EQ(T.str(),
            "f.gopp:1:9: error: mismatched ')' in macro arguments, expected "
            "']'\n"
            "    1 | #M(a, [b)\n"
            "      |         ^\n"
            "f.gopp:1:7: note: to match this '['\n"
            "    1 | #M(a, [b)\n"
            "      |       ^\n");
}

TEST(Diagnostics, JSONNamesTheDiagnosticAndItsNotes) {
  JSONFixture T;
  T.Diags.report(T.at(14), diag::err_expected_found)
          .note(T.at(4), diag::note_to_match_this, {"("}, {T.at(4), T.at(5)})
      << "';'" << "x";
  T.OS.flush();
  llvm::Expected<llvm::json::Value> V =
      llvm::json::parse(llvm::StringRef(T.Out).trim());
  ASSERT_TRUE(static_cast<bool>(V)) << T.Out;
  const llvm::json::Object *O = V->getAsObject();
  ASSERT_TRUE(O);
  EXPECT_EQ(*O->getString("id"), "err_expected_found");
  EXPECT_EQ(*O->getString("message"), "expected ';', found x");
  const llvm::json::Array *Notes = O->getArray("notes");
  ASSERT_TRUE(Notes && Notes->size() == 1u);
  const llvm::json::Object *N = (*Notes)[0].getAsObject();
  ASSERT_TRUE(N);
  EXPECT_EQ(*N->getString("id"), "note_to_match_this");
  EXPECT_EQ(*N->getString("message"), "to match this '('");
  EXPECT_NE(N->getObject("range"), nullptr);
}

TEST(Diagnostics, MovedFromBuilderReportsNothing) {
  TextFixture T("x\n");
  {
    DiagnosticBuilder A = T.Diags.report(T.at(0), Severity::Error, "once");
    DiagnosticBuilder B(std::move(A));
  }
  EXPECT_EQ(T.Diags.getNumErrors(), 1u);
  EXPECT_EQ(T.str(), "f.gopp:1:1: error: once\n"
                     "    1 | x\n"
                     "      | ^\n");
}

TEST(Diagnostics, LocationFromAnotherFileUsesItsOwnName) {
  SourceManager SM;
  std::string Out;
  llvm::raw_string_ostream OS(Out);
  TextDiagnosticPrinter P(OS, SM);
  DiagnosticsEngine D(SM, P, 0);
  FileID A = *SM.addMemoryBuffer("a.gopp", "aaa\n");
  FileID B = *SM.addMemoryBuffer("b.gopp", "bbb\n");
  D.report(SM.getLoc(A, 1), Severity::Error, "in a")
      .note(SM.getLoc(B, 2), "see b");
  OS.flush();
  EXPECT_EQ(Out, "a.gopp:1:2: error: in a\n"
                 "    1 | aaa\n"
                 "      |  ^\n"
                 "b.gopp:1:3: note: see b\n"
                 "    1 | bbb\n"
                 "      |   ^\n");
}

} // namespace
