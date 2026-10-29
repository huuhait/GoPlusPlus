//===- ASTDumper.cpp - AST dump and traversal -----------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/AST/ASTDumper.h"

#include "llvm/ADT/StringExtras.h"

using namespace gopp;
using namespace gopp::ast;
using llvm::ArrayRef;
using llvm::cast;

const char *Node::getKindName() const {
  switch (K) {
#define EXPR(X)                                                                \
  case Kind::X:                                                                \
    return #X;
#define STMT(X) EXPR(X)
#define DECL(X) EXPR(X)
#define SPEC(X) EXPR(X)
#define MISC(X) EXPR(X)
#include "gopp/AST/NodeKinds.def"
  case Kind::NumKinds:
    break;
  }
  return "<invalid>";
}

const char *ast::getMacroKindName(MacroKind K) {
  switch (K) {
  case MacroKind::Expr:
    return "expr";
  case MacroKind::Stmt:
    return "stmt";
  case MacroKind::Block:
    return "block";
  case MacroKind::Type:
    return "type";
  case MacroKind::Ident:
    return "ident";
  case MacroKind::Lit:
    return "lit";
  case MacroKind::Decl:
    return "decl";
  case MacroKind::Tokens:
    return "tokens";
  case MacroKind::Invalid:
    break;
  }
  return "<invalid>";
}

//===----------------------------------------------------------------------===//
// Children
//===----------------------------------------------------------------------===//

namespace {
using Visit = llvm::function_ref<void(const char *, const Node *)>;

template <class T, class Fn>
void list(Fn &F, const char *Label, ArrayRef<T *> L) {
  for (const T *E : L)
    F(Label, E);
}
} // namespace

void ast::forEachChild(const Node *N, Visit Callback) {
  auto F = [&](const char *Label, const Node *C) {
    if (C)
      Callback(Label, C);
  };
  switch (N->getKind()) {
  case Node::Kind::BadExpr:
  case Node::Kind::Ident:
  case Node::Kind::BasicLit:
  case Node::Kind::MacroInvocation:
  case Node::Kind::BadStmt:
  case Node::Kind::EmptyStmt:
  case Node::Kind::BadDecl:
  case Node::Kind::MacroParam:
  case Node::Kind::NumKinds:
    break;
  case Node::Kind::CompositeLit: {
    auto *E = cast<CompositeLit>(N);
    F("Type", E->Type);
    list(F, "Elts", E->Elts);
    break;
  }
  case Node::Kind::KeyValueExpr: {
    auto *E = cast<KeyValueExpr>(N);
    F("Key", E->Key);
    F("Value", E->Value);
    break;
  }
  case Node::Kind::FuncLit: {
    auto *E = cast<FuncLit>(N);
    F("Type", E->Type);
    F("Body", E->Body);
    break;
  }
  case Node::Kind::ParenExpr:
    F("X", cast<ParenExpr>(N)->X);
    break;
  case Node::Kind::SelectorExpr: {
    auto *E = cast<SelectorExpr>(N);
    F("X", E->X);
    F("Sel", E->Sel);
    break;
  }
  case Node::Kind::IndexExpr: {
    auto *E = cast<IndexExpr>(N);
    F("X", E->X);
    list(F, "Index", E->Indices);
    break;
  }
  case Node::Kind::SliceExpr: {
    auto *E = cast<SliceExpr>(N);
    F("X", E->X);
    F("Lo", E->Lo);
    F("Hi", E->Hi);
    F("Max", E->Max);
    break;
  }
  case Node::Kind::TypeAssertExpr: {
    auto *E = cast<TypeAssertExpr>(N);
    F("X", E->X);
    F("Type", E->Type);
    break;
  }
  case Node::Kind::CallExpr: {
    auto *E = cast<CallExpr>(N);
    F("Fun", E->Fun);
    list(F, "Args", E->Args);
    break;
  }
  case Node::Kind::StarExpr:
    F("X", cast<StarExpr>(N)->X);
    break;
  case Node::Kind::UnaryExpr:
    F("X", cast<UnaryExpr>(N)->X);
    break;
  case Node::Kind::BinaryExpr: {
    auto *E = cast<BinaryExpr>(N);
    F("X", E->X);
    F("Y", E->Y);
    break;
  }
  case Node::Kind::Ellipsis:
    F("Elt", cast<Ellipsis>(N)->Elt);
    break;
  case Node::Kind::ArrayType: {
    auto *E = cast<ArrayType>(N);
    F("Len", E->Len);
    F("Elt", E->Elt);
    break;
  }
  case Node::Kind::SliceType:
    F("Elt", cast<SliceType>(N)->Elt);
    break;
  case Node::Kind::StructType:
    F("Fields", cast<StructType>(N)->Fields);
    break;
  case Node::Kind::FuncType: {
    auto *E = cast<FuncType>(N);
    F("TypeParams", E->TypeParams);
    F("Params", E->Params);
    F("Results", E->Results);
    break;
  }
  case Node::Kind::InterfaceType:
    F("Methods", cast<InterfaceType>(N)->Methods);
    break;
  case Node::Kind::MapType: {
    auto *E = cast<MapType>(N);
    F("Key", E->Key);
    F("Value", E->Value);
    break;
  }
  case Node::Kind::ChanType:
    F("Value", cast<ChanType>(N)->Value);
    break;
  case Node::Kind::Attribute: {
    auto *A = cast<Attribute>(N);
    F("Pkg", A->Pkg);
    F("Name", A->Name);
    break;
  }
  case Node::Kind::Field: {
    auto *E = cast<Field>(N);
    list(F, "Names", E->Names);
    F("Type", E->Type);
    list(F, "Attrs", E->Attrs);
    F("Tag", E->Tag);
    break;
  }
  case Node::Kind::FieldList:
    list(F, "Field", cast<FieldList>(N)->List);
    break;
  case Node::Kind::DeclStmt:
    F("Decl", cast<DeclStmt>(N)->D);
    break;
  case Node::Kind::LabeledStmt: {
    auto *S = cast<LabeledStmt>(N);
    F("Label", S->Label);
    F("Stmt", S->Body);
    break;
  }
  case Node::Kind::ExprStmt:
    F("X", cast<ExprStmt>(N)->X);
    break;
  case Node::Kind::SendStmt: {
    auto *S = cast<SendStmt>(N);
    F("Chan", S->Chan);
    F("Value", S->Value);
    break;
  }
  case Node::Kind::IncDecStmt:
    F("X", cast<IncDecStmt>(N)->X);
    break;
  case Node::Kind::AssignStmt: {
    auto *S = cast<AssignStmt>(N);
    list(F, "Lhs", S->Lhs);
    list(F, "Rhs", S->Rhs);
    break;
  }
  case Node::Kind::GoStmt:
    F("Call", cast<GoStmt>(N)->Call);
    break;
  case Node::Kind::DeferStmt:
    F("Call", cast<DeferStmt>(N)->Call);
    break;
  case Node::Kind::ReturnStmt:
    list(F, "Result", cast<ReturnStmt>(N)->Results);
    break;
  case Node::Kind::BranchStmt:
    F("Label", cast<BranchStmt>(N)->Label);
    break;
  case Node::Kind::BlockStmt:
    list(F, "Stmt", cast<BlockStmt>(N)->List);
    break;
  case Node::Kind::IfStmt: {
    auto *S = cast<IfStmt>(N);
    F("Init", S->Init);
    F("Cond", S->Cond);
    F("Then", S->Then);
    F("Else", S->Else);
    break;
  }
  case Node::Kind::CaseClause: {
    auto *S = cast<CaseClause>(N);
    list(F, "Case", S->List);
    list(F, "Body", S->Body);
    break;
  }
  case Node::Kind::SwitchStmt: {
    auto *S = cast<SwitchStmt>(N);
    F("Init", S->Init);
    F("Tag", S->Tag);
    F("Body", S->Body);
    break;
  }
  case Node::Kind::TypeSwitchStmt: {
    auto *S = cast<TypeSwitchStmt>(N);
    F("Init", S->Init);
    F("Assign", S->Assign);
    F("Body", S->Body);
    break;
  }
  case Node::Kind::CommClause: {
    auto *S = cast<CommClause>(N);
    F("Comm", S->Comm);
    list(F, "Body", S->Body);
    break;
  }
  case Node::Kind::SelectStmt:
    F("Body", cast<SelectStmt>(N)->Body);
    break;
  case Node::Kind::ForStmt: {
    auto *S = cast<ForStmt>(N);
    F("Init", S->Init);
    F("Cond", S->Cond);
    F("Post", S->Post);
    F("Body", S->Body);
    break;
  }
  case Node::Kind::RangeStmt: {
    auto *S = cast<RangeStmt>(N);
    F("Key", S->Key);
    F("Value", S->Value);
    F("X", S->X);
    F("Body", S->Body);
    break;
  }
  case Node::Kind::GenDecl: {
    auto *D = cast<GenDecl>(N);
    list(F, "Attr", D->Attrs);
    list(F, "Spec", D->Specs);
    break;
  }
  case Node::Kind::FuncDecl: {
    auto *D = cast<FuncDecl>(N);
    list(F, "Attr", D->Attrs);
    F("Recv", D->Recv);
    F("Name", D->Name);
    F("Type", D->Type);
    F("Body", D->Body);
    break;
  }
  case Node::Kind::MacroDecl: {
    auto *D = cast<MacroDecl>(N);
    F("Name", D->Name);
    list(F, "Param", D->Params);
    F("Body", D->Body);
    break;
  }
  case Node::Kind::ProcMacroDecl: {
    auto *D = cast<ProcMacroDecl>(N);
    F("Name", D->Name);
    F("Type", D->Type);
    F("Body", D->Body);
    break;
  }
  case Node::Kind::MacroCallDecl:
    F("Call", cast<MacroCallDecl>(N)->Call);
    break;
  case Node::Kind::ImportSpec: {
    auto *S = cast<ImportSpec>(N);
    F("Name", S->Name);
    F("Path", S->Path);
    break;
  }
  case Node::Kind::ValueSpec: {
    auto *S = cast<ValueSpec>(N);
    list(F, "Name", S->Names);
    F("Type", S->Type);
    list(F, "Value", S->Values);
    break;
  }
  case Node::Kind::TypeSpec: {
    auto *S = cast<TypeSpec>(N);
    F("Name", S->Name);
    F("TypeParams", S->TypeParams);
    F("Type", S->Type);
    break;
  }
  case Node::Kind::File: {
    auto *Fi = cast<File>(N);
    F("Name", Fi->Name);
    list(F, "Decl", Fi->Decls);
    break;
  }
  }
}

//===----------------------------------------------------------------------===//
// Dump
//===----------------------------------------------------------------------===//

namespace {

class Dumper {
public:
  Dumper(llvm::raw_ostream &OS, const SourceManager &SM) : OS(OS), SM(SM) {}

  void dump(const Node *N, const char *Label, unsigned Depth) {
    for (unsigned I = 0; I < Depth; ++I)
      OS << "  ";
    if (Label && *Label)
      OS << Label << ": ";
    OS << N->getKindName() << ' ';
    printRange(N->getBegin(), N->getEnd());
    details(N);
    OS << '\n';
    forEachChild(N,
                 [&](const char *L, const Node *C) { dump(C, L, Depth + 1); });
  }

private:
  void printLoc(SourceLoc L) {
    PresumedLoc P = SM.getPresumedLoc(L);
    if (P.isValid())
      OS << P.Line << ':' << P.Column;
    else
      OS << "?:?";
  }
  void printRange(SourceLoc B, SourceLoc E) {
    OS << '<';
    printLoc(B);
    OS << ", ";
    printLoc(E);
    OS << '>';
  }
  void tokenText(const Token &T) {
    if (T.isAutoSemi()) {
      OS << "<nl>";
      return;
    }
    FileID F = SM.getFileID(T.Loc);
    OS << SM.getBuffer(F).substr(SM.getFileOffset(T.Loc), T.Length);
  }
  void tokens(const char *Name, llvm::ArrayRef<Token> Toks) {
    OS << ' ' << Name << "=[";
    llvm::ListSeparator LS(" ");
    for (const Token &T : Toks) {
      OS << LS;
      tokenText(T);
    }
    OS << ']';
  }
  void flag(bool B, const char *Name) {
    if (B)
      OS << ' ' << Name;
  }
  void op(tok::Kind K) {
    const char *S = tok::getSpelling(K);
    OS << " op='" << (S ? S : tok::getName(K)) << '\'';
  }

  void details(const Node *N) {
    switch (N->getKind()) {
    case Node::Kind::Ident:
      OS << " '" << cast<Ident>(N)->Name.str() << '\'';
      break;
    case Node::Kind::BasicLit: {
      auto *L = cast<BasicLit>(N);
      OS << ' ' << tok::getName(L->LitKind) << ' ' << L->Text;
      break;
    }
    case Node::Kind::SliceExpr:
      flag(cast<SliceExpr>(N)->Slice3, "slice3");
      break;
    case Node::Kind::CallExpr: {
      auto *C = cast<CallExpr>(N);
      flag(C->isMust(), "must");
      flag(C->EllipsisLoc.isValid(), "ellipsis");
      break;
    }
    case Node::Kind::UnaryExpr:
      op(cast<UnaryExpr>(N)->Op);
      break;
    case Node::Kind::BinaryExpr:
      op(cast<BinaryExpr>(N)->Op);
      break;
    case Node::Kind::MacroInvocation: {
      auto *M = cast<MacroInvocation>(N);
      OS << " name=" << (M->Pkg ? M->Pkg->Name.str() + "." : std::string())
         << M->Name->Name.str();
      tokens("args", M->Args);
      break;
    }
    case Node::Kind::FuncType:
      flag(cast<FuncType>(N)->Fails, "fails");
      break;
    case Node::Kind::ChanType: {
      auto *C = cast<ChanType>(N);
      OS << (C->Direction == ChanType::Send   ? " send"
             : C->Direction == ChanType::Recv ? " recv"
                                              : " both");
      break;
    }
    case Node::Kind::Attribute: {
      auto *A = cast<Attribute>(N);
      OS << " name=" << (A->Pkg ? A->Pkg->Name.str() + "." : std::string())
         << A->Name->Name.str();
      flag(A->HasArgs, "parens");
      if (A->HasArgs)
        tokens("args", A->Args);
      break;
    }
    case Node::Kind::Field:
      flag(cast<Field>(N)->isMove(), "move");
      break;
    case Node::Kind::MacroParam: {
      auto *P = cast<MacroParam>(N);
      OS << " '" << P->Name->Name.str()
         << "' kind=" << getMacroKindName(P->ParamKind);
      flag(P->Variadic, "variadic");
      break;
    }
    case Node::Kind::EmptyStmt:
      flag(cast<EmptyStmt>(N)->Implicit, "implicit");
      break;
    case Node::Kind::IncDecStmt:
      op(cast<IncDecStmt>(N)->Op);
      break;
    case Node::Kind::AssignStmt:
      op(cast<AssignStmt>(N)->Op);
      break;
    case Node::Kind::BranchStmt:
      OS << ' ' << tok::getSpelling(cast<BranchStmt>(N)->Keyword);
      break;
    case Node::Kind::CaseClause:
      flag(cast<CaseClause>(N)->IsDefault, "default");
      break;
    case Node::Kind::RangeStmt: {
      auto *R = cast<RangeStmt>(N);
      if (R->Op != tok::eof)
        op(R->Op);
      if (R->Alias == RangeStmt::AliasKind::Key)
        OS << " alias=key";
      else if (R->Alias == RangeStmt::AliasKind::Value)
        OS << " alias=value";
      break;
    }
    case Node::Kind::GenDecl: {
      auto *D = cast<GenDecl>(N);
      OS << ' ' << tok::getSpelling(D->Keyword);
      flag(D->Lparen.isValid(), "grouped");
      break;
    }
    case Node::Kind::FuncDecl:
      flag(cast<FuncDecl>(N)->Type && cast<FuncDecl>(N)->Type->Fails, "fails");
      break;
    case Node::Kind::ProcMacroDecl: {
      auto *D = cast<ProcMacroDecl>(N);
      flag(D->Type && D->Type->Fails, "fails");
      break;
    }
    case Node::Kind::TypeSpec:
      flag(cast<TypeSpec>(N)->AssignLoc.isValid(), "alias");
      break;
    default:
      break;
    }
  }

  llvm::raw_ostream &OS;
  const SourceManager &SM;
};

} // namespace

void ast::dump(llvm::raw_ostream &OS, const SourceManager &SM,
               const Node *Root) {
  if (Root)
    Dumper(OS, SM).dump(Root, "", 0);
}
