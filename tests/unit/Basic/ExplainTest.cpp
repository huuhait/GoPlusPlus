//===- ExplainTest.cpp - gopp explain: every code has a checked example ---===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P03.14. `gopp explain E0042` prints an explanation and an example. This test
// keeps both honest:
//   - every row of DiagnosticIDs.def except `custom` has an entry in
//     DiagnosticExplain.def, and no entry names a row that does not exist;
//   - the example of an entry makes the compiler (lexer, parser, checker and
//     vet) report that very diagnostic. The few rows an example cannot reach
//     are listed in Unchecked with the reason.
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/DiagnosticExplain.h"
#include "gopp/Sema/Loader.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <set>

using namespace gopp;

namespace {

/// Rows whose example is not compiled here, and why.
const std::map<std::string, const char *> Unchecked = {
    {"warn_vet_ref_field", "retired: err_view_field_without_attr (a check "
                           "error) replaced the lint (OWN-V5)"},
    {"err_own_case_ref_unreachable",
     "retired: a type switch case on a view is an ordinary case (OWN-V1)"},
    {"err_ref_bad_elem", "retired: `ref` is an ordinary name; a bad View "
                         "argument is err_view_bad_arg (ADR-0020)"},
    {"err_too_many_errors", "needs -error-limit=N; DiagnosticsTest.ErrorLimit"},
    {"err_lex_illegal_nul", "the example is a description (a NUL byte)"},
    {"err_lex_utf16", "the example is a description (UTF-16 bytes)"},
    {"err_lex_illegal_utf8", "the example is a description (a bad byte)"},
    {"err_lex_illegal_bom", "the example is a description (a BOM)"},
    {"err_nesting_too_deep", "the example is a description (a deep file)"},
    {"err_expression_too_deeply_nested", "the example is a description; "
                                         "tests/parse/deep-tree.test"},
    {"err_fmt_unreported_syntax_error", "a guard that never fires"},
    {"err_load_no_such_package", "the cause text of err_import_unresolved"},
    {"err_load_cannot_read", "needs a file the operating system refuses"},
    {"err_foreign_unsupported", "the cause text of err_import_unresolved"},
    {"err_foreign_ambiguous", "needs a C++ header: ForeignTest.cpp"},
    {"err_foreign_call", "needs a C++ header: ForeignTest.cpp"},
    {"err_foreign_readonly_arg", "needs a C++ header: ForeignTest.cpp"},
    {"err_foreign_readonly_recv", "needs a C++ header: ForeignTest.cpp"},
    {"err_foreign_template_value", "needs a C++ header: ForeignTest.cpp"},
    {"err_foreign_template_arg", "needs a C++ header: ForeignTest.cpp"},
    {"err_foreign_instantiate", "needs a C++ header: ForeignTest.cpp"},
    {"err_sema_typeset", "needs a union of more than 100 terms"},
    {"err_cannot_use_blank", "not reported: err_cannot_use_blank_type is"},
    {"err_builtin_invalid_arg", "not reported: err_sema_builtin is"},
    {"err_got_n_type_args", "not reported: err_got_n_type_args_func is"},
    {"err_literal_missing_type", "only the parser could make such a tree"},
    {"err_go_defer_needs_call", "the parser reports err_call_expected first"},
    {"err_invalid_use_of_dots", "the parser reports it first"},
    {"err_dots_not_last", "the parser reports err_dots_not_final first"},
    {"err_own_isnil_ambiguous", "retired by ADR-0018: pointers have no IsNil, "
                                "so there is no clash; the code stays "
                                "reserved"},
    {"err_gir_unsupported", "reported by the code generator, not the checker; "
                            "tests/gir/unsupported.test"},
    {"err_gir_verify",
     "an internal error; it cannot be triggered by a program"},
    {"err_codegen", "reported after GIR is built; tests/codegen"},
    {"err_gir_extern_bad", "reported by the code generator, not the "
                           "checker; tests/gir/extern-bad.test"},
    {"err_gir_export_bad", "reported by the code generator, not the "
                           "checker; tests/gir/export-bad.test"},
    {"err_gir_no_main", "reported by the code generator, not the checker; "
                        "tests/gir/no-main.test"},
    {"err_no_stack", "needs no thread to be had: DeepChain."
                     "NoStackForADeepChain, tests/sema/"
                     "no-stack.long.test"},
    {"err_invalid_method_type_params", "the parser reports "
                                       "err_interface_method_type_params and "
                                       "drops the list first"},
};

struct Outcome {
  std::set<std::string> Names;
  std::string Text;
};

Outcome compile(const std::string &Source) {
  llvm::SmallString<128> Dir;
  EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-explain", Dir));
  // An example of several files: sections start with a line `//--- path`.
  {
    std::string Path = "main.gopp", Body;
    auto Flush = [&] {
      if (Path.empty())
        return;
      llvm::SmallString<128> Full(std::string(Dir) + "/" + Path);
      llvm::sys::fs::create_directories(llvm::sys::path::parent_path(Full));
      std::ofstream Out(std::string(Full), std::ios::binary);
      Out << Body;
    };
    llvm::StringRef Rest = Source;
    while (!Rest.empty()) {
      auto [Line, Next] = Rest.split('\n');
      if (Line.starts_with("//--- ")) {
        Flush();
        Path = Line.drop_front(6).str();
        Body.clear();
      } else {
        Body += Line.str();
        if (!Next.empty() || Rest.ends_with("\n"))
          Body += '\n';
      }
      Rest = Next;
    }
    Flush();
  }
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  // A first line `// gopp check -go` says the example is for Go mode.
  bool GoMode = llvm::StringRef(Source).starts_with("// gopp check -go\n");
  sema::LoadOptions LO;
  LO.Root = std::string(Dir);
  LO.LanguageRules = !GoMode;
  sema::CheckOptions CO;
  CO.Vet = !GoMode;
  CO.GoCompat = GoMode;
  CO.Ownership = !GoMode;
  sema::TypeContext Ctx(GoMode);
  sema::Loader L(SM, Diags, Ctx, LO, CO);
  L.loadDir(std::string(Dir), "");
  Outcome R;
  for (const Diagnostic &D : Collect.Diags) {
    R.Names.insert(getDiagName(D.ID).str());
    R.Text += "  " + getDiagName(D.ID).str() + ": " + D.Message + "\n";
    for (const DiagNote &N : D.Notes)
      R.Names.insert(getDiagName(N.ID).str());
  }
  llvm::sys::fs::remove_directories(Dir);
  return R;
}

TEST(Explain, EveryDiagnosticHasAnExplanationAndAnExample) {
  EXPECT_EQ(getDiagExplanation(diag::custom), nullptr);
  for (unsigned I = 0; I < diag::NUM_DIAGS; ++I) {
    auto ID = static_cast<diag::ID>(I);
    if (ID == diag::custom)
      continue;
    const DiagExplanation *E = getDiagExplanation(ID);
    std::string Name = getDiagName(ID).str();
    ASSERT_NE(E, nullptr) << Name << " (" << getDiagCodeString(ID) << ")";
    EXPECT_GT(std::string(E->Text).size(), 20u) << Name;
    EXPECT_GT(std::string(E->Example).size(), 5u) << Name;
  }
}

TEST(Explain, ThePageNamesTheCodeTheMessageAndTheExample) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  printDiagExplanation(OS, diag::err_undefined);
  OS.flush();
  EXPECT_NE(S.find("E0111 (err_undefined): error"), std::string::npos) << S;
  EXPECT_NE(S.find("Message: undefined: %0"), std::string::npos) << S;
  EXPECT_NE(S.find("\nExample:\n\n    package"), std::string::npos) << S;

  std::string L;
  llvm::raw_string_ostream LS(L);
  printDiagCodeList(LS);
  LS.flush();
  EXPECT_NE(L.find("E0001 err_too_many_errors  too many errors\n"),
            std::string::npos);
  EXPECT_EQ(L.find("custom"), std::string::npos);
}

TEST(Explain, AFamilyCodePrintsTypicalMessagesNotPercentZero) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  printDiagExplanation(OS, diag::err_sema_assign);
  OS.flush();
  EXPECT_EQ(S.find("Message: %0"), std::string::npos) << S;
  EXPECT_NE(S.find("Message: varies; this code covers a family"),
            std::string::npos)
      << S;
  EXPECT_NE(
      S.find("\n    cannot use \"text\" (untyped string constant) as int"),
      std::string::npos)
      << S;
  // Both sides: a code with a message of its own keeps it. And every code
  // whose whole message is "%0" is a family (so none prints "Message: %0").
  EXPECT_FALSE(isDiagFamily(diag::err_undefined));
  for (unsigned I = 0; I < diag::NUM_DIAGS; ++I) {
    auto ID = static_cast<diag::ID>(I);
    if (getDiagCode(ID) != 0 && llvm::StringRef(getDiagFormat(ID)) == "%0")
      EXPECT_TRUE(isDiagFamily(ID)) << getDiagName(ID).str();
  }
}

TEST(Explain, EveryExampleMakesTheCompilerReportItsDiagnostic) {
  unsigned Checked = 0;
  for (unsigned I = 0; I < diag::NUM_DIAGS; ++I) {
    auto ID = static_cast<diag::ID>(I);
    if (ID == diag::custom)
      continue;
    std::string Name = getDiagName(ID).str();
    if (Unchecked.count(Name))
      continue;
    const DiagExplanation *E = getDiagExplanation(ID);
    ASSERT_NE(E, nullptr) << Name;
    Outcome R = compile(E->Example);
    EXPECT_TRUE(R.Names.count(Name))
        << getDiagCodeString(ID) << " " << Name
        << ": the example does not report it. It reports:\n"
        << (R.Text.empty() ? "  nothing\n" : R.Text) << "Example:\n"
        << E->Example;
    ++Checked;
  }
  // Every row but `custom` was either compiled or listed above.
  EXPECT_EQ(Checked + Unchecked.size() + 1,
            static_cast<size_t>(diag::NUM_DIAGS));
  EXPECT_GT(Checked, 280u);
}

TEST(Explain, UncheckedRowsExist) {
  for (const auto &[Name, Why] : Unchecked) {
    bool Found = false;
    for (unsigned I = 0; I < diag::NUM_DIAGS; ++I)
      Found |= getDiagName(static_cast<diag::ID>(I)) == Name;
    EXPECT_TRUE(Found) << Name << " (" << Why << ")";
  }
}

} // namespace
