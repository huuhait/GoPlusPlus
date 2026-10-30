//===- ExprString.cpp - Short source text of expressions ------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The text of an expression as types2 shows it in messages (syntax.String's
// short form): literals keep their source text, bodies are elided to `{…}`.
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/Support/raw_ostream.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;

namespace {

class ExprPrinter {
public:
  explicit ExprPrinter(llvm::raw_string_ostream &OS) : OS(OS) {}

  void print(const ast::Expr *E, unsigned Depth = 0) {
    if (!E) {
      OS << "<nil>";
      return;
    }
    if (Depth > 40) {
      OS << "…";
      return;
    }
    ++Depth;
    using K = Node::Kind;
    switch (E->getKind()) {
    case K::BadExpr:
      OS << "BadExpr";
      break;
    case K::Ident:
      OS << cast<Ident>(E)->Name.str();
      break;
    case K::BasicLit:
      OS << cast<BasicLit>(E)->Text;
      break;
    case K::CompositeLit: {
      auto *C = cast<CompositeLit>(E);
      if (C->Type)
        print(C->Type, Depth);
      OS << (C->Elts.empty() ? "{}" : "{…}");
      break;
    }
    case K::KeyValueExpr: {
      auto *KV = cast<KeyValueExpr>(E);
      print(KV->Key, Depth);
      OS << ": ";
      print(KV->Value, Depth);
      break;
    }
    case K::FuncLit: {
      auto *F = cast<FuncLit>(E);
      print(F->Type, Depth);
      OS << " {…}";
      break;
    }
    case K::ParenExpr:
      OS << '(';
      print(cast<ParenExpr>(E)->X, Depth);
      OS << ')';
      break;
    case K::SelectorExpr: {
      auto *S = cast<SelectorExpr>(E);
      // The selector of an operator call the checker made (ADR-0024): no
      // source selector can name an operator.
      if (isOperatorSpelling(S->Sel->Name.str())) {
        OS << "the operator " << S->Sel->Name.str() << " of ";
        print(S->X, Depth);
        break;
      }
      print(S->X, Depth);
      OS << '.' << S->Sel->Name.str();
      break;
    }
    case K::IndexExpr: {
      auto *I = cast<IndexExpr>(E);
      print(I->X, Depth);
      OS << '[';
      list(I->Indices, Depth);
      OS << ']';
      break;
    }
    case K::SliceExpr: {
      auto *S = cast<SliceExpr>(E);
      print(S->X, Depth);
      OS << '[';
      if (S->Lo)
        print(S->Lo, Depth);
      OS << ':';
      if (S->Hi)
        print(S->Hi, Depth);
      if (S->Slice3) {
        OS << ':';
        if (S->Max)
          print(S->Max, Depth);
      }
      OS << ']';
      break;
    }
    case K::TypeAssertExpr: {
      auto *T = cast<TypeAssertExpr>(E);
      print(T->X, Depth);
      OS << ".(";
      if (T->Type)
        print(T->Type, Depth);
      else
        OS << "type";
      OS << ')';
      break;
    }
    case K::CallExpr: {
      auto *C = cast<CallExpr>(E);
      print(C->Fun, Depth);
      if (C->isMust())
        OS << '!';
      OS << '(';
      list(C->Args, Depth);
      if (C->EllipsisLoc.isValid())
        OS << "...";
      OS << ')';
      break;
    }
    case K::StarExpr:
      OS << '*';
      print(cast<StarExpr>(E)->X, Depth);
      break;
    case K::UnaryExpr: {
      auto *U = cast<UnaryExpr>(E);
      OS << tok::getSpelling(U->Op);
      print(U->X, Depth);
      break;
    }
    case K::BinaryExpr: {
      auto *B = cast<BinaryExpr>(E);
      print(B->X, Depth);
      OS << ' ' << tok::getSpelling(B->Op) << ' ';
      print(B->Y, Depth);
      break;
    }
    case K::MacroInvocation:
      OS << '#' << cast<MacroInvocation>(E)->Name->Name.str() << "(…)";
      break;
    case K::Ellipsis: {
      auto *El = cast<Ellipsis>(E);
      OS << "...";
      if (El->Elt)
        print(El->Elt, Depth);
      break;
    }
    case K::ArrayType: {
      auto *A = cast<ast::ArrayType>(E);
      OS << '[';
      if (A->Len)
        print(A->Len, Depth);
      OS << ']';
      print(A->Elt, Depth);
      break;
    }
    case K::SliceType:
      OS << "[]";
      print(cast<ast::SliceType>(E)->Elt, Depth);
      break;
    case K::StructType: {
      auto *S = cast<ast::StructType>(E);
      OS << "struct{";
      if (S->Fields && !S->Fields->List.empty())
        OS << "…";
      OS << '}';
      break;
    }
    case K::FuncType: {
      auto *F = cast<ast::FuncType>(E);
      if (F->FuncLoc.isValid())
        OS << "func";
      if (F->Fails)
        OS << '!';
      fields(F->Params, Depth);
      if (F->Results && !F->Results->List.empty()) {
        OS << ' ';
        if (F->Results->Opening.isValid() || F->Results->numFields() > 1)
          fields(F->Results, Depth);
        else
          print(F->Results->List[0]->Type, Depth);
      }
      break;
    }
    case K::InterfaceType: {
      auto *I = cast<ast::InterfaceType>(E);
      OS << "interface{";
      if (I->Methods && !I->Methods->List.empty())
        OS << "…";
      OS << '}';
      break;
    }
    case K::MapType: {
      auto *M = cast<ast::MapType>(E);
      OS << "map[";
      print(M->Key, Depth);
      OS << ']';
      print(M->Value, Depth);
      break;
    }
    case K::ChanType: {
      auto *C = cast<ast::ChanType>(E);
      if (C->Direction == ast::ChanType::Recv)
        OS << "<-chan ";
      else if (C->Direction == ast::ChanType::Send)
        OS << "chan<- ";
      else
        OS << "chan ";
      print(C->Value, Depth);
      break;
    }
    default:
      OS << "?";
    }
  }

private:
  template <class R> void list(const R &L, unsigned Depth) {
    bool First = true;
    for (const ast::Expr *E : L) {
      if (!First)
        OS << ", ";
      First = false;
      print(E, Depth);
    }
  }
  void fields(const FieldList *L, unsigned Depth) {
    OS << '(';
    if (L) {
      bool First = true;
      for (const Field *F : L->List) {
        if (!First)
          OS << ", ";
        First = false;
        for (size_t I = 0; I < F->Names.size(); ++I) {
          if (I)
            OS << ", ";
          OS << F->Names[I]->Name.str();
        }
        if (!F->Names.empty())
          OS << ' ';
        print(F->Type, Depth);
      }
    }
    OS << ')';
  }
  llvm::raw_string_ostream &OS;
};

} // namespace

std::string Checker::str(const ast::Expr *E) const {
  std::string S;
  llvm::raw_string_ostream OS(S);
  ExprPrinter P(OS);
  P.print(E);
  return OS.str();
}
