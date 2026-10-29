//===- FormatTest.cpp - gopp fmt unit tests -------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Layout rules by example (gofmt's), the Go++ forms, the import rules, and the
// invariants: idempotence and no lost comment.
//
//===----------------------------------------------------------------------===//

#include "gopp/Format/Format.h"

#include "gopp/Parse/Parser.h"

#include "../../../compiler/lib/Format/TabWriter.h"

#include "gtest/gtest.h"

using namespace gopp;

namespace {

std::string fmt(const std::string &Src, FormatOptions Opts = {}) {
  std::string Out, Err;
  EXPECT_TRUE(formatText("t.gopp", Src, Out, &Err, Opts)) << Err << "\n" << Src;
  return Out;
}

/// Formats and checks idempotence.
std::string fmtStable(const std::string &Src) {
  std::string Once = fmt(Src);
  EXPECT_EQ(fmt(Once), Once) << "not idempotent for:\n" << Src;
  return Once;
}

//===----------------------------------------------------------------------===//
// text/tabwriter port
//===----------------------------------------------------------------------===//

std::string tab(const std::string &In, unsigned Flags = 0, char Pad = ' ',
                int MinWidth = 0) {
  std::string Out;
  format::TabWriter TW(Out, MinWidth, 8, 1, Pad, Flags);
  TW.write(In);
  TW.flush();
  return Out;
}

TEST(TabWriter, AlignsColumns) {
  EXPECT_EQ(tab("a\tb\tc\naaa\tbbb\tc\n"), "a   b   c\naaa bbb c\n");
}

TEST(TabWriter, FormfeedBreaksTheBlock) {
  EXPECT_EQ(tab("aa\tb\n\fxxxxx\ty\n"), "aa b\n\nxxxxx y\n");
  EXPECT_EQ(tab("aa\tb\fxxxxx\ty\n"), "aa b\nxxxxx y\n");
}

TEST(TabWriter, EscapedTextKeepsItsWidth) {
  std::string In = std::string("\xff") + "a\tb" + "\xff" + "\tc\nxx\tyy\n";
  // The escape bytes stay for the trimmer; the text between counts by runes.
  EXPECT_EQ(tab(In), std::string("\xff") + "a\tb" + "\xff" + " c\nxx  yy\n");
}

TEST(TabWriter, DiscardEmptyColumns) {
  EXPECT_EQ(tab("a\v\vb\naa\v\vbb\n", format::TabWriter::DiscardEmptyColumns),
            "a  b\naa bb\n");
}

//===----------------------------------------------------------------------===//
// Layout
//===----------------------------------------------------------------------===//

TEST(Format, SpacesAndIndentation) {
  EXPECT_EQ(fmtStable("package p\nfunc f( a int,b int)int{\nreturn a+b*2\n}\n"),
            "package p\n\nfunc f(a int, b int) int {\n\treturn a + b*2\n}\n");
}

TEST(Format, AlignsStructFieldsAndTrailingComments) {
  EXPECT_EQ(
      fmtStable("package p\ntype T struct{\na int // x\nbbb string // y\n}\n"),
      "package p\n\ntype T struct {\n\ta   int    // x\n\tbbb string // "
      "y\n}\n");
}

TEST(Format, KeepsOneLineFunctionBodies) {
  EXPECT_EQ(fmtStable("package p\nfunc f() int { return 1 }\n"),
            "package p\n\nfunc f() int { return 1 }\n");
}

TEST(Format, BlankLinesAreKeptUpToOne) {
  EXPECT_EQ(fmtStable("package p\nvar a = 1\n\n\n\nvar b = 2\n"),
            "package p\n\nvar a = 1\n\nvar b = 2\n");
}

TEST(Format, CompositeLiteralKeyAlignment) {
  EXPECT_EQ(
      fmtStable(
          "package p\nvar m = map[string]int{\n\"a\": 1,\n\"bbb\": 2,\n}\n"),
      "package p\n\nvar m = map[string]int{\n\t\"a\":   1,\n\t\"bbb\": "
      "2,\n}\n");
}

TEST(Format, BinaryExpressionSpacingByPrecedence) {
  EXPECT_EQ(fmtStable("package p\nvar x = a+b*c - d/(e+f)\n"),
            "package p\n\nvar x = a + b*c - d/(e+f)\n");
}

TEST(Format, NumbersAreNormalized) {
  EXPECT_EQ(fmtStable("package p\nvar x = 0XAB + 1E3 + 0B1\n"),
            "package p\n\nvar x = 0xAB + 1e3 + 0b1\n");
}

TEST(Format, CarriageReturnsAreRemoved) {
  EXPECT_EQ(fmt("package p\r\n\r\n// c\r\nvar x = 1\r\n"),
            "package p\n\n// c\nvar x = 1\n");
}

//===----------------------------------------------------------------------===//
// Go++ forms
//===----------------------------------------------------------------------===//

TEST(Format, RefMoveBangAndRangeAlias) {
  EXPECT_EQ(
      fmtStable("package p\nfunc f!(move a   int,s View[string])View[string]{\n"
                "for i,&v:=range xs{use(i,v)}\nx:=g!(1)\nreturn s}\n"),
      "package p\n\nfunc f!(move a int, s View[string]) View[string] {\n"
      "\tfor i, &v := range xs {\n\t\tuse(i, v)\n\t}\n\tx := g!(1)\n"
      "\treturn s\n}\n");
}

TEST(Format, RangeAliasOfTheOnlyVariable) {
  EXPECT_EQ(fmtStable("package p\nfunc f() {\nfor &d:=range seq{}\n}\n"),
            "package p\n\nfunc f() {\n\tfor &d := range seq {\n\t}\n}\n");
}

TEST(Format, AttributesOneLineEachWhenWrittenThatWay) {
  EXPECT_EQ(fmtStable("package p\n@A\n@B(x)\ntype T int\n@C @D\nfunc f() {}\n"),
            "package p\n\n@A\n@B(x)\ntype T int\n\n@C @D\nfunc f() {}\n");
}

TEST(Format, FieldAttributeGoesBeforeTheTag) {
  EXPECT_EQ(
      fmtStable("package p\ntype T struct{\na int @J(\"a\") `t`\nbb int\n}\n"),
      "package p\n\ntype T struct {\n\ta  int @J(\"a\") `t`\n\tbb int\n}\n");
}

TEST(Format, MacroArgumentsAreCopiedVerbatim) {
  EXPECT_EQ(fmtStable("package p\n#Gen( a,b )\nfunc f() { x := #M( 1,2 ) }\n"),
            "package p\n\n#Gen( a,b )\n\nfunc f() { x := #M( 1,2 ) }\n");
}

TEST(Format, MacroDeclarations) {
  EXPECT_EQ(fmtStable("package p\nmacro Sw(a ident,b ...expr){\nreturn a\n}\n"
                      "macro func Gen(x expr) expr{return x}\n"),
            "package p\n\nmacro Sw(a ident, b ...expr) {\n\treturn a\n}\n"
            "macro func Gen(x expr) expr { return x }\n");
}

TEST(Format, MacroDeclarationsAreFormattedLikeFunctions) {
  // A small body written on one line stays there, as for a function.
  EXPECT_EQ(fmtStable("package p\nmacro Sum(xs ...expr) { x := 1; _ = x }\n"),
            "package p\n\nmacro Sum(xs ...expr) { x := 1; _ = x }\n");
}

TEST(Format, CommentBetweenHashAndMacroNameIsKept) {
  // Found by fuzzing during the merge: a comment between `#` (or `@`) and the
  // name was dropped (a line comment) or moved behind the name.
  const char *Src = "package p\n\nfunc f() {\n\t# // why\n\tM(x)\n\t#P. /* c "
                    "*/ M(y)\n}\n\n@ /* a */ Attr /* b */ (1)\nfunc g() {}\n";
  EXPECT_EQ(fmtStable(Src), Src);
}

//===----------------------------------------------------------------------===//
// Imports
//===----------------------------------------------------------------------===//

TEST(Format, HeaderPaths) {
  EXPECT_TRUE(isHeaderImportPath("<vector>"));
  EXPECT_TRUE(isHeaderImportPath("\"a/b.h\""));
  EXPECT_TRUE(isHeaderImportPath("a/b.hpp"));
  EXPECT_TRUE(isHeaderImportPath("a/b.hh"));
  EXPECT_TRUE(isHeaderImportPath("a/b.hxx"));
  EXPECT_FALSE(isHeaderImportPath("\"a/h\""));
  EXPECT_FALSE(isHeaderImportPath("\"net/http\""));
  EXPECT_FALSE(isHeaderImportPath("\"a.hs\""));
}

TEST(Format, HeaderRuleIsReplaceable) {
  // A build tool that knows the gopp.mod decides what a header is (D2.8).
  FormatOptions Opts;
  Opts.IsHeader = [](llvm::StringRef P) { return P == "legacy"; };
  std::string Out, Err;
  ASSERT_TRUE(formatText("t.gopp",
                         "package p\n\nimport (\n\t\"z\"\n\t\"legacy\"\n)\n",
                         Out, &Err, Opts))
      << Err;
  EXPECT_EQ(Out, "package p\n\nimport (\n\t\"z\"\n\t\"legacy\"\n)\n");
  ASSERT_TRUE(formatText("t.gopp",
                         "package p\n\nimport (\n\t\"z\"\n\t\"a.h\"\n)\n", Out,
                         &Err, Opts))
      << Err;
  EXPECT_EQ(Out, "package p\n\nimport (\n\t\"a.h\"\n\t\"z\"\n)\n");
}

TEST(Format, ImportsAreSorted) {
  EXPECT_EQ(fmtStable("package p\nimport (\n\"os\"\n\"fmt\"\nb \"bytes\"\n)\n"),
            "package p\n\nimport (\n\tb \"bytes\"\n\t\"fmt\"\n\t\"os\"\n)\n");
}

TEST(Format, ImportGroupsAreSortedSeparately) {
  EXPECT_EQ(fmtStable("package p\nimport (\n\"b\"\n\"a\"\n\n\"d\"\n\"c\"\n)\n"),
            "package p\n\nimport (\n\t\"a\"\n\t\"b\"\n\n\t\"c\"\n\t\"d\"\n)\n");
}

TEST(Format, ImportsOnTheLineOfTheParenAreSortedInTheFirstCall) {
  // Found by the fuzzer: the first spec stood on the line of "(", so it was
  // not sorted until the second pass. fmt(fmt(x)) must equal fmt(x).
  EXPECT_EQ(fmtStable("package p\nimport ( \"b\"\n\t\"a\")\n"),
            "package p\n\nimport (\n\t\"a\"\n\t\"b\"\n)\n");
}

TEST(Format, TypeParameterWithAUnaryConstraintKeepsItsComma) {
  // Found by the fuzzer (gofmt has the same bug): without the comma,
  // `[P +n]` is an array length and `[P +n,]` a syntax error.
  EXPECT_EQ(fmtStable("package e\ntype _[P ((+n)),]e\n"),
            "package e\n\ntype _[P (+n),] e\n");
}

TEST(Format, TypeParameterWithABinaryConstraintKeepsItsParentheses) {
  EXPECT_EQ(fmtStable("package e\ntype T[P(b*struct{})]t\n"),
            "package e\n\ntype T[P (b * struct{}),] t\n");
}

TEST(Format, ValidConstraintsAreNotParenthesized) {
  EXPECT_EQ(
      fmtStable(
          "package e\ntype T[P any, Q ~int|string, R interface{ *P }]t\n"),
      "package e\n\ntype T[P any, Q ~int | string, R interface{ *P }] t\n");
}

TEST(Format, FormfeedInAStringDoesNotMoveTheLineCount) {
  // Found by the fuzzer: go/printer counts a formfeed in a literal as a line,
  // so the blank lines after it were lost and a second pass differed.
  EXPECT_EQ(fmtStable("package p\nvar a = \"\f\"\n\nvar b = 1\n"),
            "package p\n\nvar a = \"\f\"\n\nvar b = 1\n");
}

TEST(Format, HeaderImportOrderIsKept) {
  const char *Src = "package p\n\nimport (\n\t\"zeta\"\n\t\"<vector>\"\n"
                    "\t\"alpha\"\n\t\"x/b.h\"\n\t\"x/a.h\"\n)\n";
  EXPECT_EQ(fmtStable(Src), Src);
}

TEST(Format, OnlyTheGroupWithAHeaderIsLeftAlone) {
  EXPECT_EQ(
      fmtStable(
          "package p\nimport (\n\"z\"\n\"a.h\"\n\"b\"\n\n\"y\"\n\"x\"\n)\n"),
      "package p\n\nimport "
      "(\n\t\"z\"\n\t\"a.h\"\n\t\"b\"\n\n\t\"x\"\n\t\"y\"\n)\n");
}

TEST(Format, NoSortImportsOption) {
  FormatOptions O;
  O.SortImports = false;
  EXPECT_EQ(fmt("package p\nimport (\n\"b\"\n\"a\"\n)\n", O),
            "package p\n\nimport (\n\t\"b\"\n\t\"a\"\n)\n");
}

TEST(Format, DuplicateImportsCollapseWithoutLosingTheComment) {
  EXPECT_EQ(fmtStable("package p\nimport (\n\"a\"\n\"a\" // why\n\"b\"\n)\n"),
            "package p\n\nimport (\n\t\"a\" // why\n\t\"b\"\n)\n");
}

TEST(Format, DifferentLocalNamesAreNotDuplicates) {
  EXPECT_EQ(fmtStable("package p\nimport (\n\"a\"\nx \"a\"\n)\n"),
            "package p\n\nimport (\n\t\"a\"\n\tx \"a\"\n)\n");
}

//===----------------------------------------------------------------------===//
// Comments
//===----------------------------------------------------------------------===//

TEST(Format, CommentsStayWhereTheyAre) {
  const char *Src = "// Package doc.\npackage p // trailing\n\n"
                    "// F does it.\nfunc F() {\n\t// only a comment\n}\n\n"
                    "// end\n";
  EXPECT_EQ(fmtStable(Src), Src);
}

TEST(Format, BlockCommentInsideExpression) {
  // The same as gofmt: the comment moves before the comma.
  EXPECT_EQ(fmtStable("package p\nvar x = f(a, /* b */ b)\n"),
            "package p\n\nvar x = f(a /* b */, b)\n");
}

TEST(Format, CommentAfterLastStatement) {
  EXPECT_EQ(fmtStable("package p\nfunc f() {\nx := 1\n_ = x\n// last\n}\n"),
            "package p\n\nfunc f() {\n\tx := 1\n\t_ = x\n\t// last\n}\n");
}

TEST(Format, CommentsInsideMacroArgumentsAreKept) {
  EXPECT_EQ(fmtStable("package p\n#Gen(a, /* kept */ b)\n"),
            "package p\n\n#Gen(a, /* kept */ b)\n");
}

TEST(Format, FileThatIsOnlyAComment) {
  // No package clause is a syntax error; a comment-only package is fine.
  EXPECT_EQ(fmtStable("package p\n// nothing\n"), "package p\n\n// nothing\n");
}

//===----------------------------------------------------------------------===//
// Unified diff (gopp fmt -d)
//===----------------------------------------------------------------------===//

TEST(Diff, EqualTextHasNoDiff) {
  EXPECT_EQ(unifiedDiff("f", "a\nb\n", "a\nb\n"), "");
}

TEST(Diff, OneHunkWithContext) {
  EXPECT_EQ(
      unifiedDiff("f", "1\n2\n3\n4\n5\n6\n7\n", "1\n2\n3\nx\n5\n6\n7\n"),
      "--- f.orig\n+++ f\n@@ -1,7 +1,7 @@\n 1\n 2\n 3\n-4\n+x\n 5\n 6\n 7\n");
}

TEST(Diff, DistantChangesAreSeparateHunks) {
  std::string Old, New;
  for (int I = 1; I <= 20; ++I) {
    Old += std::to_string(I) + "\n";
    New += (I == 2 || I == 19 ? "x" : std::to_string(I)) + "\n";
  }
  std::string D = unifiedDiff("f", Old, New);
  EXPECT_NE(D.find("@@ -1,5 +1,5 @@"), std::string::npos) << D;
  EXPECT_NE(D.find("@@ -16,5 +16,5 @@"), std::string::npos) << D;
}

TEST(Diff, MissingNewlineAtEndIsMarked) {
  EXPECT_EQ(unifiedDiff("f", "a\nb", "a\nb\n"),
            "--- f.orig\n+++ f\n@@ -1,2 +1,2 @@\n a\n-b\n\\ No newline at end "
            "of file\n+b\n");
}

TEST(Diff, InsertionAndDeletion) {
  EXPECT_EQ(unifiedDiff("f", "", "a\n"),
            "--- f.orig\n+++ f\n@@ -0,0 +1,1 @@\n+a\n");
  EXPECT_EQ(unifiedDiff("f", "a\n", ""),
            "--- f.orig\n+++ f\n@@ -1,1 +0,0 @@\n-a\n");
}

//===----------------------------------------------------------------------===//
// Errors and invariants
//===----------------------------------------------------------------------===//

TEST(Format, SyntaxErrorsAreReportedAndNothingIsFormatted) {
  std::string Out, Err;
  EXPECT_FALSE(formatText("bad.gopp", "package p\nfunc f( {\n", Out, &Err));
  EXPECT_NE(Err.find("bad.gopp:2"), std::string::npos) << Err;
  EXPECT_TRUE(Out.empty());
}

TEST(Format, AnEngineAtItsLimitDoesNotHideErrors) {
  // The caller's engine has used up its error limit: the parse must still see
  // the syntax error (it would otherwise stop at once, and the empty tree would
  // print as an empty file).
  SourceManager SM;
  llvm::Expected<FileID> F =
      SM.addMemoryBuffer("bad.gopp", "package p\nfunc f( {\n");
  ASSERT_TRUE(bool(F));
  CollectingDiagnosticConsumer C;
  DiagnosticsEngine Diags(SM, C, 1);
  Diags.report(SourceLoc(), Severity::Error, "an earlier error");
  Diags.report(SourceLoc(), Severity::Error, "the limit is reached");
  ASSERT_TRUE(Diags.limitReached());
  std::string Out = "untouched";
  EXPECT_FALSE(formatFile(SM, *F, Diags, Out));
  EXPECT_EQ(Out, "untouched");
}

TEST(Format, ABadNodeWithoutAnErrorIsNeverPrinted) {
  // The parser reports an error for every Bad node, so no input reaches the
  // guard through formatFile. A tree with a BadDecl and no error must be
  // refused with err_fmt_unreported_syntax_error, not printed.
  SourceManager SM;
  llvm::Expected<FileID> F = SM.addMemoryBuffer("t.gopp", "package p\n");
  ASSERT_TRUE(bool(F));
  CollectingDiagnosticConsumer Parse;
  DiagnosticsEngine PD(SM, Parse, 0);
  ast::ASTContext Ctx;
  ast::File *AST = parseFile(Ctx, SM, *F, PD);
  ASSERT_FALSE(PD.hasErrors());
  std::string Good;
  CollectingDiagnosticConsumer C0;
  DiagnosticsEngine D0(SM, C0, 0);
  ASSERT_TRUE(formatParsedFile(SM, *F, AST, D0, Good));
  EXPECT_EQ(Good, "package p\n");

  ast::BadDecl *Bad = Ctx.create<ast::BadDecl>();
  Bad->Begin = Bad->End = SM.getStartLoc(*F);
  ast::Decl *Decls[] = {Bad};
  AST->Decls = Ctx.copy(llvm::ArrayRef<ast::Decl *>(Decls));
  CollectingDiagnosticConsumer C;
  DiagnosticsEngine D(SM, C, 0);
  std::string Out = "untouched";
  EXPECT_FALSE(formatParsedFile(SM, *F, AST, D, Out));
  EXPECT_EQ(Out, "untouched");
  ASSERT_EQ(C.Diags.size(), 1u);
  EXPECT_EQ(C.Diags[0].ID, diag::err_fmt_unreported_syntax_error);
}

TEST(Format, TheSecondPassOfAnOneLineBodyIsStable) {
  // go/printer alone is not idempotent here (a one-line body behind a header
  // that was split over lines); gopp fmt prints until the text is stable.
  const char *Src = "package p\n\nfunc f[A interface {\n\t*B\n}, B any, C "
                    "interface{ *B }](x B) A { panic(0) }\n";
  std::string Once = fmt(Src);
  EXPECT_EQ(fmt(Once), Once);
}

TEST(Format, PreservesEveryCommentOfTheSamples) {
  const char *Srcs[] = {
      "package p\n/* a */ var x /* b */ = /* c */ 1 // d\n",
      "package p\nfunc f(a /* a */, b /* b */ int) { /* in */ }\n",
      "package p\nvar _ = []int{ // c\n1, /* d */ 2,\n// e\n}\n",
      "package p\nfunc f() {\nswitch x {\n// c\ncase 1: // d\n}\n}\n",
  };
  for (const char *Src : Srcs) {
    std::string Out = fmtStable(Src);
    for (const char *C : {"/* a */", "/* b */", "/* c */", "// d", "/* in */",
                          "// c", "/* d */", "// e"}) {
      if (std::string(Src).find(C) != std::string::npos)
        EXPECT_NE(Out.find(C), std::string::npos) << C << " lost in:\n" << Out;
    }
  }
}

} // namespace
