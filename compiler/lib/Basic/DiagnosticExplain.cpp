//===- DiagnosticExplain.cpp - gopp explain -------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/DiagnosticExplain.h"

#include "llvm/ADT/StringMap.h"

#include <vector>

using namespace gopp;

namespace {
struct Entry {
  const char *Name;
  DiagExplanation E;
};

const Entry Entries[] = {
#define EXPLAIN(Name, Text, Example) {#Name, {Text, Example}},
#include "DiagnosticExplain.def"
};

/// Family codes: the checker's messages are built where the error is found,
/// so one code (format "%0") stands for many messages. `gopp explain` shows a
/// few typical ones instead of "%0". They must be split into specific codes
/// before the v0.1 stability promise (D3.11, see P03 STATE.md).
struct Family {
  const char *Name;
  std::vector<const char *> Typical;
};

const Family Families[] = {
    {"err_sema_assign",
     {"cannot use \"text\" (untyped string constant) as int value in variable "
      "declaration",
      "cannot assign to struct field m[k].f in map",
      "cannot use generic function f without instantiation in assignment"}},
    {"err_sema_builtin",
     {"invalid argument: cannot make int: type must be slice, map, or channel",
      "invalid operation: cannot close receive-only channel c",
      "invalid append: first argument must be a slice"}},
    {"err_sema_call",
     {"invalid operation: cannot call x (variable of type int): int is not a "
      "function",
      "illegal cycle in method declaration",
      "missing argument in conversion to int"}},
    {"err_sema_decl",
     {"cannot declare main - must be func",
      "cannot declare init - must be func", "missing type or init expression"}},
    {"err_sema_expr",
     {"invalid operation: shift count x (variable of type float64) must be "
      "integer",
      "constant result is not representable",
      "cannot use ~ outside of interface or type constraint"}},
    {"err_sema_index",
     {"invalid operation: more than one index",
      "invalid argument: index i (constant of type int) must not be negative",
      "invalid argument: index 5 out of bounds [0:3]"}},
    {"err_sema_predicate",
     {"invalid operation: cannot receive from send-only channel c (variable of "
      "type chan<- int)",
      "invalid operation: operator + not defined on x (variable of type bool)",
      "invalid operation: mismatched types int and string"}},
    {"err_sema_stmt",
     {"non-boolean condition in if statement", "invalid statement",
      "assignment operation requires single-valued expressions"}},
    {"err_sema_typeset",
     {"cannot handle more than 100 union terms (implementation limitation)"}},
    {"err_sema_typexpr",
     {"invalid array length n",
      "cannot use generic type T without "
      "instantiation",
      "x used as type"}},
};

/// The explanation of every diagnostic, by ID.
const std::vector<const DiagExplanation *> &table() {
  static const std::vector<const DiagExplanation *> T = [] {
    llvm::StringMap<const DiagExplanation *> ByName;
    for (const Entry &E : Entries)
      ByName[E.Name] = &E.E;
    std::vector<const DiagExplanation *> V(diag::NUM_DIAGS, nullptr);
    for (unsigned I = 0; I < diag::NUM_DIAGS; ++I) {
      auto It = ByName.find(getDiagName(static_cast<diag::ID>(I)));
      if (It != ByName.end())
        V[I] = It->second;
    }
    return V;
  }();
  return T;
}
} // namespace

bool gopp::isDiagFamily(diag::ID ID) {
  for (const Family &F : Families)
    if (F.Name == llvm::StringRef(getDiagName(ID)))
      return true;
  return false;
}

const DiagExplanation *gopp::getDiagExplanation(diag::ID ID) {
  return ID < diag::NUM_DIAGS ? table()[ID] : nullptr;
}

void gopp::printDiagExplanation(llvm::raw_ostream &OS, diag::ID ID) {
  OS << getDiagCodeString(ID) << " (" << getDiagName(ID)
     << "): " << getSeverityName(getDiagSeverity(ID)) << "\n\n";
  const Family *Fam = nullptr;
  for (const Family &F : Families)
    if (F.Name == llvm::StringRef(getDiagName(ID)))
      Fam = &F;
  if (Fam) {
    OS << "Message: varies; this code covers a family of messages, chosen "
          "where the error is found. Typical ones:\n\n";
    for (const char *M : Fam->Typical)
      OS << "    " << M << '\n';
    OS << '\n';
  } else {
    OS << "Message: " << getDiagFormat(ID) << "\n\n";
  }
  const DiagExplanation *E = getDiagExplanation(ID);
  if (!E) {
    OS << "There is no explanation for this code yet.\n";
    return;
  }
  OS << E->Text << "\n\nExample:\n\n";
  llvm::StringRef Ex = E->Example;
  while (!Ex.empty()) {
    auto [Line, Rest] = Ex.split('\n');
    if (Line.empty())
      OS << '\n';
    else
      OS << "    " << Line << '\n';
    Ex = Rest;
  }
}

void gopp::printDiagCodeList(llvm::raw_ostream &OS) {
  for (unsigned I = 0; I < diag::NUM_DIAGS; ++I) {
    auto ID = static_cast<diag::ID>(I);
    if (getDiagCode(ID) == 0)
      continue;
    OS << getDiagCodeString(ID) << ' ' << getDiagName(ID) << "  "
       << getDiagFormat(ID) << '\n';
  }
}
