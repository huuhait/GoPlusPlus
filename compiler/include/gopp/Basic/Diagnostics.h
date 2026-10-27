//===- gopp/Basic/Diagnostics.h - Diagnostics engine ------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// D2.5. A diagnostic has an ID, a severity, a primary location and range,
// secondary ranges, notes and fix-its. Every diagnostic the compiler reports is
// an entry of DiagnosticIDs.def (a stable name, a severity and a message with
// %0..%9 placeholders); the arguments are given with `<<`, as in Clang:
//
//   Diags.report(Loc, diag::err_expected_found) << "';'" << Found;
//
// The engine counts errors, applies the error limit (default 10 per package,
// as in Go) and hands the diagnostics to a consumer:
//   - TextDiagnosticPrinter: Clang-style text with source line and carets;
//   - JSONDiagnosticPrinter: one JSON object per line (-diag-format=json);
//   - CollectingDiagnosticConsumer: stores them (tests, LSP).
// One engine serves one package: the error count is per engine.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_BASIC_DIAGNOSTICS_H
#define GOPP_BASIC_DIAGNOSTICS_H

#include "gopp/Basic/SourceManager.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace gopp {

enum class Severity { Note, Warning, Error };

llvm::StringRef getSeverityName(Severity S);

namespace diag {
enum ID : unsigned {
#define DIAG(Name, Sev, Text) Name,
#include "gopp/Basic/DiagnosticIDs.def"
  NUM_DIAGS
};
} // namespace diag

/// The stable name of a diagnostic ("err_expected"); JSON output shows it.
llvm::StringRef getDiagName(diag::ID ID);
/// The severity the table gives the diagnostic.
Severity getDiagSeverity(diag::ID ID);
/// The message text of the table, with its %0..%9 placeholders.
llvm::StringRef getDiagFormat(diag::ID ID);
/// Replaces %0..%9 in Format with Args (a missing argument becomes empty) and
/// %% with %.
std::string formatDiagnostic(llvm::StringRef Format,
                             llvm::ArrayRef<std::string> Args);

/// Replace Range by Text. An insertion has an empty range.
struct FixIt {
  SourceRange Range;
  std::string Text;
};

struct Diagnostic;

/// Applies the fix-its of Diags that lie in File to the file's text and
/// returns the new text (`gopp parse -fixit`). The fix-its of one diagnostic
/// are applied together or not at all; a diagnostic whose fix-its overlap an
/// earlier one is skipped. NumApplied, if given, gets the number of
/// diagnostics applied.
std::string applyFixIts(const SourceManager &SM, FileID File,
                        llvm::ArrayRef<Diagnostic> Diags,
                        unsigned *NumApplied = nullptr);

struct DiagNote {
  diag::ID ID = diag::custom;
  SourceLoc Loc;
  SourceRange Range;
  std::string Message;
};

struct Diagnostic {
  diag::ID ID = diag::custom;
  Severity Sev = Severity::Error;
  /// Where the caret goes. Invalid for diagnostics without a position.
  SourceLoc Loc;
  /// The primary range (underlined with '~'); may be empty or invalid.
  SourceRange Primary;
  /// Related ranges (underlined with '-').
  llvm::SmallVector<SourceRange, 2> Secondary;
  std::string Message;
  std::vector<DiagNote> Notes;
  std::vector<FixIt> FixIts;
};

class DiagnosticConsumer {
public:
  virtual ~DiagnosticConsumer() = default;
  virtual void handleDiagnostic(const Diagnostic &D) = 0;
  /// Called once when the run ends (e.g. to close a JSON array).
  virtual void finish() {}
};

class CollectingDiagnosticConsumer : public DiagnosticConsumer {
public:
  void handleDiagnostic(const Diagnostic &D) override { Diags.push_back(D); }
  std::vector<Diagnostic> Diags;
};

/// Clang-style text:
///   file:3:5: error: message
///       3 | x := 1 +
///         |          ^
class TextDiagnosticPrinter : public DiagnosticConsumer {
public:
  TextDiagnosticPrinter(llvm::raw_ostream &OS, const SourceManager &SM)
      : OS(OS), SM(SM) {}
  void handleDiagnostic(const Diagnostic &D) override;

private:
  void printSnippet(SourceLoc Loc, SourceRange Primary,
                    llvm::ArrayRef<SourceRange> Secondary,
                    llvm::ArrayRef<FixIt> FixIts);
  llvm::raw_ostream &OS;
  const SourceManager &SM;
};

/// One compact JSON object per diagnostic, one per line.
class JSONDiagnosticPrinter : public DiagnosticConsumer {
public:
  JSONDiagnosticPrinter(llvm::raw_ostream &OS, const SourceManager &SM)
      : OS(OS), SM(SM) {}
  void handleDiagnostic(const Diagnostic &D) override;

private:
  llvm::raw_ostream &OS;
  const SourceManager &SM;
};

class DiagnosticsEngine;

/// Fills a Diagnostic and emits it from the destructor.
class DiagnosticBuilder {
public:
  DiagnosticBuilder(DiagnosticsEngine &E, SourceLoc Loc, diag::ID ID);
  /// A custom diagnostic (diag::custom) with severity S and message Msg.
  DiagnosticBuilder(DiagnosticsEngine &E, SourceLoc Loc, Severity S,
                    const llvm::Twine &Msg);
  /// Takes over the diagnostic: the moved-from builder emits nothing.
  DiagnosticBuilder(DiagnosticBuilder &&Other) noexcept;
  DiagnosticBuilder(const DiagnosticBuilder &) = delete;
  DiagnosticBuilder &operator=(const DiagnosticBuilder &) = delete;
  DiagnosticBuilder &operator=(DiagnosticBuilder &&) = delete;
  ~DiagnosticBuilder();

  /// Fills the next %N placeholder of the message.
  DiagnosticBuilder &operator<<(llvm::StringRef Arg) {
    Args.push_back(Arg.str());
    return *this;
  }
  DiagnosticBuilder &operator<<(unsigned N) {
    Args.push_back(std::to_string(N));
    return *this;
  }
  /// A char would print as a number: pass a string.
  DiagnosticBuilder &operator<<(char) = delete;

  DiagnosticBuilder &range(SourceRange R) {
    D.Primary = R;
    return *this;
  }
  DiagnosticBuilder &secondary(SourceRange R) {
    D.Secondary.push_back(R);
    return *this;
  }
  /// A note from the table; NoteArgs fill its placeholders.
  DiagnosticBuilder &note(SourceLoc Loc, diag::ID NoteID,
                          llvm::ArrayRef<llvm::StringRef> NoteArgs = {},
                          SourceRange R = {});
  /// A custom note with a ready message.
  DiagnosticBuilder &note(SourceLoc Loc, const llvm::Twine &Msg,
                          SourceRange R = {}) {
    D.Notes.push_back({diag::custom, Loc, R, Msg.str()});
    return *this;
  }
  DiagnosticBuilder &fixit(SourceRange R, llvm::StringRef Text) {
    D.FixIts.push_back({R, Text.str()});
    return *this;
  }
  DiagnosticBuilder &insert(SourceLoc At, llvm::StringRef Text) {
    return fixit(SourceRange(At, At), Text);
  }

private:
  DiagnosticsEngine *Engine; // null once moved from
  Diagnostic D;
  llvm::SmallVector<std::string, 2> Args;
};

class DiagnosticsEngine {
public:
  static constexpr unsigned DefaultErrorLimit = 10;

  /// ErrorLimit 0 means no limit.
  DiagnosticsEngine(const SourceManager &SM, DiagnosticConsumer &Consumer,
                    unsigned ErrorLimit = DefaultErrorLimit)
      : SM(SM), Consumer(Consumer), ErrorLimit(ErrorLimit) {}

  const SourceManager &getSourceManager() const { return SM; }
  unsigned getErrorLimit() const { return ErrorLimit; }

  /// Reports diagnostic ID at Loc; `<<` on the result fills its arguments.
  DiagnosticBuilder report(SourceLoc Loc, diag::ID ID) {
    return DiagnosticBuilder(*this, Loc, ID);
  }
  /// A custom diagnostic with a ready message, for tests and tools. Compiler
  /// code adds an entry to DiagnosticIDs.def instead.
  DiagnosticBuilder report(SourceLoc Loc, Severity S, const llvm::Twine &Msg) {
    return DiagnosticBuilder(*this, Loc, S, Msg);
  }

  /// Applies the limit and forwards to the consumer. After ErrorLimit errors,
  /// one "too many errors" error follows and later errors are dropped.
  void emit(Diagnostic D);

  unsigned getNumErrors() const { return NumErrors; }
  unsigned getNumWarnings() const { return NumWarnings; }
  bool hasErrors() const { return NumErrors != 0; }
  /// True once an error was dropped. Producers may stop early.
  bool limitReached() const { return LimitReached; }
  void finish() { Consumer.finish(); }

private:
  const SourceManager &SM;
  DiagnosticConsumer &Consumer;
  unsigned ErrorLimit;
  unsigned NumErrors = 0;
  unsigned NumWarnings = 0;
  bool LimitReached = false;
};

} // namespace gopp

#endif // GOPP_BASIC_DIAGNOSTICS_H
