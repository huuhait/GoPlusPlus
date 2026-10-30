//===- Print.cpp - The GIR text form --------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Print.h"

#include "gopp/Basic/WorkCount.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"

#include <cstring>

using namespace gopp;
using namespace gopp::gir;
using llvm::raw_ostream;

namespace {

/// Test-only work counter (M8).
uint64_t &printSteps() {
  thread_local uint64_t N = 0;
  return N;
}

/// The loops below count their elements: `counted(printSteps(), Range)`.

struct Printer {
  raw_ostream &OS;
  const Module &M;
  const PrintOptions &Opts;

  void place(const Place &P) { gir::print(OS, P); }

  void constant(const Constant &C) {
    switch (C.K) {
    case Constant::Int:
      OS << "const ";
      if (C.Ty->isSigned()) {
        unsigned B = C.Ty->bits();
        uint64_t V = C.Bits;
        int64_t S = B == 64 ? static_cast<int64_t>(V)
                            : static_cast<int64_t>(V << (64 - B)) >> (64 - B);
        OS << S;
      } else {
        OS << C.Bits;
      }
      OS << ": " << C.Ty->str();
      return;
    case Constant::Float: {
      double D;
      std::memcpy(&D, &C.Bits, sizeof D);
      OS << "const " << llvm::format("%.17g", D) << ": " << C.Ty->str();
      return;
    }
    case Constant::Bool:
      OS << "const " << (C.Bits ? "true" : "false");
      return;
    case Constant::Str:
      OS << "const str#" << C.Bits << " \"";
      llvm::printEscapedString(M.Strings[C.Bits], OS);
      OS << '"';
      return;
    case Constant::Func:
      OS << "const fn @" << M.Funcs[C.Bits]->Name;
      return;
    case Constant::Null:
      OS << "const null: " << C.Ty->str();
      return;
    case Constant::Zero:
      OS << "const zero: " << C.Ty->str();
      return;
    case Constant::Global:
      OS << "const global @" << M.Globals[C.Bits].Name;
      return;
    }
  }

  void operand(const Operand &O) {
    switch (O.K) {
    case Operand::Copy:
      OS << "copy ";
      place(O.P);
      return;
    case Operand::Move:
      OS << "move ";
      place(O.P);
      return;
    case Operand::Const:
      constant(O.C);
      return;
    }
  }

  void operands(llvm::ArrayRef<Operand> Ops) {
    bool First = true;
    for (const Operand &O : counted(printSteps(), Ops)) {
      if (!First)
        OS << ", ";
      First = false;
      operand(O);
    }
  }

  void rvalue(const Rvalue &R) {
    switch (R.K) {
    case Rvalue::Use:
      operand(R.A);
      return;
    case Rvalue::AddrOf:
      OS << (R.Mut ? "&mut " : "&");
      place(R.P);
      return;
    case Rvalue::Binary:
      OS << getBinOpName(R.Bin) << '(';
      operand(R.A);
      OS << ", ";
      operand(R.B);
      OS << ')';
      return;
    case Rvalue::Unary:
      OS << getUnOpName(R.Un) << '(';
      operand(R.A);
      OS << ')';
      return;
    case Rvalue::Cast:
      OS << getCastKindName(R.CastOp) << '(';
      operand(R.A);
      OS << ") as " << R.Ty->str();
      return;
    case Rvalue::Aggregate:
      OS << getAggKindName(R.Agg) << " " << R.Ty->str() << " {";
      operands(R.Ops);
      OS << '}';
      return;
    case Rvalue::Len:
      OS << "len(";
      place(R.P);
      OS << ')';
      return;
    }
  }

  void span(const Span &S) {
    if (!Opts.Spans || !S.isValid())
      return;
    // (A module that breaks V070 still prints.)
    OS << "  ; "
       << (S.File <= M.Files.size()
               ? llvm::sys::path::filename(M.Files[S.File - 1])
               : llvm::StringRef("?"))
       << ':' << S.Line << ':' << S.Col;
  }

  void unwind(const UnwindAction &U) {
    switch (U.K) {
    case UnwindAction::Continue:
      OS << "continue";
      return;
    case UnwindAction::Cleanup:
      OS << "cleanup bb" << U.Target;
      return;
    case UnwindAction::Terminate:
      OS << "terminate";
      return;
    case UnwindAction::Unreachable:
      OS << "unreachable";
      return;
    }
  }

  void statement(const Statement &S) {
    OS << "    ";
    switch (S.K) {
    case Statement::Assign:
      place(S.Dest);
      OS << " = ";
      rvalue(S.RV);
      break;
    case Statement::StorageLive:
      OS << "StorageLive(_" << S.L << ')';
      break;
    case Statement::StorageDead:
      OS << "StorageDead(_" << S.L << ')';
      break;
    case Statement::Nop:
      OS << "nop";
      break;
    }
    span(S.Loc);
    OS << '\n';
  }

  void terminator(const Terminator &T) {
    OS << "    ";
    switch (T.K) {
    case Terminator::Goto:
      OS << "goto -> bb" << T.Target;
      break;
    case Terminator::Switch:
      OS << "switch ";
      operand(T.Discr);
      OS << " [";
      for (const SwitchCase &C : counted(printSteps(), T.Cases))
        OS << C.Value << ": bb" << C.Target << ", ";
      OS << "otherwise: bb" << T.Otherwise << ']';
      break;
    case Terminator::Return:
      OS << "return";
      break;
    case Terminator::Unreachable:
      OS << "unreachable";
      break;
    case Terminator::Call:
      place(T.Dest);
      OS << " = call ";
      operand(T.Discr);
      OS << '(';
      operands(T.Args);
      OS << ") -> [";
      if (T.Target != NoId)
        OS << "return: bb" << T.Target << ", ";
      OS << "unwind: ";
      unwind(T.Unwind);
      OS << ']';
      break;
    case Terminator::Drop:
      OS << "drop(";
      place(T.Dest);
      OS << ") -> [return: bb" << T.Target << ", unwind: ";
      unwind(T.Unwind);
      OS << ']';
      break;
    case Terminator::Assert:
      OS << "assert(";
      operand(T.Discr);
      OS << " == " << (T.Expected ? "true" : "false") << ", \""
         << getAssertKindName(T.AK) << "\"";
      for (const Operand &A : counted(printSteps(), T.Args)) {
        OS << ", ";
        operand(A);
      }
      OS << ") -> [success: bb" << T.Target << ", unwind: ";
      unwind(T.Unwind);
      OS << ']';
      break;
    case Terminator::Resume:
      OS << "resume";
      break;
    }
    span(T.Loc);
    OS << '\n';
  }

  void function(const Function &F) {
    ++printSteps();
    OS << (F.isDeclaration() ? "declare " : "") << "fn " << F.Name << '(';
    for (unsigned I = 0; I < F.numParams(); ++I, ++printSteps()) {
      if (I)
        OS << ", ";
      OS << '_' << I + 1 << ": " << F.Sig->elems()[I]->str();
    }
    OS << ')' << (F.Sig->fails() ? "!" : "") << " -> " << F.Sig->ret()->str()
       << " [" << (F.CC == CallConv::C ? "c" : "go") << ", "
       << (F.Link == Linkage::External   ? "external"
           : F.Link == Linkage::Internal ? "internal"
                                         : "linkonce_odr")
       << (F.NoUnwind ? ", nounwind" : "") << (F.NoReturn ? ", noreturn" : "")
       << ']';
    if (F.isDeclaration()) {
      OS << '\n';
      return;
    }
    OS << " {\n";
    for (LocalId I = 0; I < F.Locals.size(); ++I) {
      const Local &L = F.Locals[I];
      ++printSteps();
      OS << "  let _" << I << ": " << L.Ty->str();
      if (!L.Name.empty())
        OS << "  ; " << L.Name;
      OS << '\n';
    }
    for (BlockId B = 0; B < F.Blocks.size(); ++B) {
      const BasicBlock &BB = F.Blocks[B];
      ++printSteps();
      OS << "  bb" << B
         << (BB.Catch     ? " (catch)"
             : BB.Cleanup ? " (cleanup)"
                          : "")
         << ":\n";
      for (const Statement &S : BB.Stmts) {
        ++printSteps();
        statement(S);
      }
      if (BB.Term.Set)
        terminator(BB.Term);
      else
        OS << "    <no terminator>\n";
    }
    OS << "}\n";
  }
};

} // namespace

uint64_t gir::printerSteps() { return printSteps(); }

void gir::print(raw_ostream &OS, const Place &P) {
  std::string S = "_" + std::to_string(P.Local);
  for (const PlaceElem &E : counted(printSteps(), P.Proj)) {
    switch (E.K) {
    case PlaceElem::Deref:
      S = "(*" + S + ")";
      break;
    case PlaceElem::Field:
      S += "." + std::to_string(E.Val);
      break;
    case PlaceElem::Index:
      S += "[_" + std::to_string(E.Val) + "]";
      break;
    case PlaceElem::ConstIndex:
      S += "[" + std::to_string(E.Val) + "]";
      break;
    case PlaceElem::Downcast:
      S = "(" + S + " as variant " + std::to_string(E.Val) + ")";
      break;
    }
  }
  OS << S;
}

void gir::print(raw_ostream &OS, const Module &M, const Function &F,
                const PrintOptions &Opts) {
  Printer{OS, M, Opts}.function(F);
}

void gir::print(raw_ostream &OS, const Module &M, const PrintOptions &Opts) {
  OS << "module \"" << M.Name << "\"\n";
  for (unsigned I = 0; I < M.Files.size(); ++I, ++printSteps())
    OS << "file " << I << " \"" << llvm::sys::path::filename(M.Files[I])
       << "\"\n";
  if (!M.Globals.empty())
    OS << '\n';
  for (const Global &G : counted(printSteps(), M.Globals)) {
    OS << (G.ReadOnly ? "constdata @" : "global @") << G.Name << ": "
       << G.Ty->str();
    if (G.Init.K != Constant::Zero) {
      OS << " = ";
      Printer P{OS, M, Opts};
      P.constant(G.Init);
    }
    OS << '\n';
  }
  for (const auto &F : M.Funcs) {
    OS << '\n';
    print(OS, M, *F, Opts);
  }
}
