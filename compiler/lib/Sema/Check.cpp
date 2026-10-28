//===- Check.cpp - Checker driver, recording and the public entry points --===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

//===----------------------------------------------------------------------===//
// Errors
//===----------------------------------------------------------------------===//

Err::Err(Checker &C, SourceLoc Loc, diag::ID ID, std::vector<std::string> Args)
    : C(C) {
  D.ID = ID;
  D.Sev = getDiagSeverity(ID);
  D.Loc = Loc;
  D.Message = formatDiagnostic(getDiagFormat(ID), Args);
}

Err::~Err() {
  if (Live)
    C.emit(std::move(D));
}

Err &Err::note(SourceLoc Loc, diag::ID ID, std::vector<std::string> Args) {
  D.Notes.push_back({ID, Loc, {}, formatDiagnostic(getDiagFormat(ID), Args)});
  return *this;
}

Err &Err::note(SourceLoc Loc, std::string Msg) {
  D.Notes.push_back({diag::custom, Loc, {}, std::move(Msg)});
  return *this;
}

Err &Err::fixit(SourceRange R, StringRef Text) {
  D.FixIts.push_back({R, Text.str()});
  return *this;
}

Err Checker::err(SourceLoc Loc, diag::ID ID, std::vector<std::string> Args) {
  return Err(*this, Loc, ID, std::move(Args));
}

/// An error that mentions an invalid operand or type is a follow-on error:
/// after a first error it adds nothing (types2).
static bool isFollowOn(const Diagnostic &D) {
  size_t A = D.Message.find("invalid operand");
  size_t B = D.Message.find("invalid type");
  return (A != std::string::npos && A > 0) || (B != std::string::npos && B > 0);
}

void Checker::emit(Diagnostic D) {
  if (D.Sev == Severity::Error) {
    if (FirstErr && isFollowOn(D))
      return;
    FirstErr = true;
    ++NumErrors;
  }
  Diags.emit(std::move(D));
}

//===----------------------------------------------------------------------===//
// Objects and scopes
//===----------------------------------------------------------------------===//

Checker::Checker(PackageState &S, Info &I, Arena &Mem, DiagnosticsEngine &Diags)
    : S(S), Ctx(S.Ctx), I(I), Mem(Mem), Diags(Diags) {}

Var *Checker::newVar(StringRef Name, Type *T, SourceLoc Pos) {
  Var *V = Mem.make<Var>();
  V->Name = Name;
  V->Pkg = S.Pkg;
  V->Ty = T;
  V->Pos = Pos;
  V->FuncOwner = E.FuncId;
  return V;
}

Var *Checker::newParam(StringRef Name, Type *T, SourceLoc Pos) {
  Var *V = newVar(Name, T, Pos);
  V->IsParam = true;
  V->Used = true;
  return V;
}

Const *Checker::newConst(StringRef Name, Type *T, SourceLoc Pos) {
  Const *C = Mem.make<Const>();
  C->Name = Name;
  C->Pkg = S.Pkg;
  C->Ty = T;
  C->Pos = Pos;
  return C;
}

TypeName *Checker::newTypeName(StringRef Name, Type *T, SourceLoc Pos) {
  TypeName *TN = Mem.make<TypeName>();
  TN->Name = Name;
  TN->Pkg = S.Pkg;
  TN->Ty = T;
  TN->Pos = Pos;
  return TN;
}

Func *Checker::newFunc(StringRef Name, Type *T, SourceLoc Pos) {
  Func *F = Mem.make<Func>();
  F->Name = Name;
  F->Pkg = S.Pkg;
  F->Ty = T;
  F->Pos = Pos;
  return F;
}

Scope *Checker::openScope(SourceRange Extent, const char *Comment) {
  Scope *Sc = Mem.newScope(E.Sc, Extent, Comment);
  E.Sc = Sc;
  return Sc;
}

void Checker::closeScope() { E.Sc = E.Sc->Parent; }

void Checker::declare(Scope *Sc, const ast::Ident *Id, Object *Obj,
                      SourceLoc ScopePos) {
  if (Obj->Name != "_") {
    // Redeclaring a loop variable in the loop body (RNG-8) is reported by the
    // parser (ParseOptions::CheckLanguageRules).
    if (Object *Alt = Sc->insert(Obj)) {
      err(Id ? Id->getBegin() : Obj->Pos, diag::err_redeclared,
          {std::string(Obj->Name)})
          .note(Alt->Pos, diag::note_other_declaration,
                {std::string(Obj->Name)});
      return;
    }
    Obj->ScopePos = ScopePos;
    Obj->Parent = Sc;
  }
  if (Id)
    recordDef(Id, Obj);
}

//===----------------------------------------------------------------------===//
// Recording
//===----------------------------------------------------------------------===//

void Checker::recordDef(const ast::Ident *Id, Object *O) {
  if (Id)
    I.Defs[Id] = O;
}

void Checker::recordUse(const ast::Ident *Id, Object *O) {
  if (Id)
    I.Uses[Id] = O;
}

void Checker::recordImplicit(const ast::Node *N, Object *O) {
  I.Implicits[N] = O;
}

void Checker::recordBuiltinType(const ast::Expr *F, Type *Sig) {
  while (F) {
    I.Types[F] = {Sig, nullptr, Mode::Builtin};
    auto *P = dyn_cast<ParenExpr>(F);
    if (!P)
      break;
    F = P->X;
  }
}

void Checker::recordInstance(const ast::Expr *Ex, ArrayRef<Type *> TArgs,
                             Type *T) {
  // The expression is the identifier (or selector) being instantiated.
  const ast::Expr *Key = Ex;
  if (auto *Ix = dyn_cast<IndexExpr>(Key))
    Key = Ix->X;
  while (auto *P = dyn_cast<ParenExpr>(Key))
    Key = P->X;
  Instance In;
  In.TArgs = Mem.copy(TArgs);
  In.Ty = T;
  I.Instances[Key] = In;
}

void Checker::processDelayed(size_t From) {
  for (size_t K = From; K < Delayed.size(); ++K) {
    auto F = std::move(Delayed[K]);
    F();
  }
  Delayed.resize(From);
}

void Checker::unusedImports() {
  if (!S.Opts.ReportUnused)
    return;
  for (FileState &F : S.Files)
    for (PkgName *PN : F.Imports)
      if (!PN->Used) {
        // types2: the name is shown when it is not the last path element.
        StringRef Elem = PN->Imported->Path;
        size_t Slash = Elem.find_last_of('/');
        if (Slash != StringRef::npos)
          Elem = Elem.substr(Slash + 1);
        std::string Msg = "\"" + std::string(PN->Imported->Path) + "\"";
        if (PN->Name != Elem && PN->Name != ".")
          Msg += " imported as " + std::string(PN->Name) + " and not used";
        else
          Msg += " imported and not used";
        err(PN->ImportLoc, diag::custom, {Msg});
      }
}

void Checker::checkFiles(ArrayRef<const ast::File *> Files) {
  for (const ast::File *F : Files) {
    FileState FS;
    FS.AST = F;
    S.Files.push_back(FS);
  }
  E = Env();
  E.Sc = S.Pkg->PkgScope;
  collectObjects();
  packageObjects();
  processDelayed();
  recordUntyped();
}

//===----------------------------------------------------------------------===//
// Info
//===----------------------------------------------------------------------===//

void Info::merge(Info &&O) {
  for (auto &KV : O.Types)
    Types[KV.first] = KV.second;
  for (auto &KV : O.Defs)
    Defs[KV.first] = KV.second;
  for (auto &KV : O.Uses)
    Uses[KV.first] = KV.second;
  for (auto &KV : O.Implicits)
    Implicits[KV.first] = KV.second;
  for (auto &KV : O.Selections)
    Selections[KV.first] = KV.second;
  for (auto &KV : O.Instances)
    Instances[KV.first] = KV.second;
}

//===----------------------------------------------------------------------===//
// checkPackage
//===----------------------------------------------------------------------===//

std::unique_ptr<CheckedPackage>
sema::checkPackage(TypeContext &Ctx, const SourceManager &SM, StringRef PkgPath,
                   ArrayRef<const ast::File *> Files, Importer &Imp,
                   DiagnosticsEngine &Diags, const CheckOptions &Opts) {
  auto CP = std::make_unique<CheckedPackage>();
  CP->Mem.push_back(std::make_unique<Arena>());
  Arena &Mem = *CP->Mem.back();
  Package *Pkg = Mem.make<Package>();
  Pkg->Path = Mem.copy(PkgPath);
  Pkg->Name = Files.empty() || !Files[0]->Name
                  ? StringRef("_")
                  : Mem.copy(Files[0]->Name->Name.str());
  Pkg->PkgScope = Mem.newScope(Ctx.universe(), {}, "package");
  CP->Pkg = Pkg;

  PackageState S(Ctx, SM, Imp, Opts);
  S.Pkg = Pkg;
  Checker C(S, CP->I, Mem, Diags);
  C.checkFiles(Files);

  // Phase 2: one checker per body. Their diagnostics are replayed in
  // declaration order, whatever order the bodies ran in.
  CP->NumBodies = S.Bodies.size();
  for (const BodyJob &J : S.Bodies) {
    CP->Mem.push_back(std::make_unique<Arena>());
    Arena &BMem = *CP->Mem.back();
    Info BI;
    CollectingDiagnosticConsumer Collect;
    DiagnosticsEngine BDiags(SM, Collect, 0);
    Checker BC(S, BI, BMem, BDiags);
    BC.Bodies = true;
    BC.checkBody(J);
    BC.recordUntyped();
    for (Diagnostic &D : Collect.Diags) {
      if (D.Sev == Severity::Error) {
        // Earlier bodies may have reported the first error.
        if (C.NumErrors > 0 && isFollowOn(D))
          continue;
        ++C.NumErrors;
      }
      Diags.emit(std::move(D));
    }
    CP->I.merge(std::move(BI));
  }
  C.FirstErr = C.NumErrors > 0;
  C.checkInitOrder();
  C.unusedImports();
  Pkg->Complete = true;
  return CP;
}

//===----------------------------------------------------------------------===//
// dumpTypes
//===----------------------------------------------------------------------===//

namespace {
struct DumpEntry {
  uint32_t Begin, End;
  int Order;
  std::string Text;
};
} // namespace

void sema::dumpTypes(llvm::raw_ostream &OS, const SourceManager &SM,
                     const CheckedPackage &P,
                     ArrayRef<const ast::File *> Files) {
  (void)Files;
  const Package *Pkg = P.Pkg;
  auto Loc = [&](SourceLoc L) {
    PresumedLoc PL = SM.getPresumedLoc(L);
    return std::to_string(PL.Line) + ":" + std::to_string(PL.Column);
  };
  auto Text = [&](const ast::Node *N) {
    FileID F = SM.getFileID(N->getBegin());
    if (!F.isValid() || !N->getEnd().isValid())
      return std::string();
    uint32_t B = SM.getFileOffset(N->getBegin());
    uint32_t En = SM.getFileOffset(N->getEnd());
    StringRef Buf = SM.getBuffer(F);
    if (En < B || En > Buf.size())
      return std::string();
    std::string S0 = Buf.substr(B, En - B).str();
    for (char &C : S0)
      if (C == '\n' || C == '\t')
        C = ' ';
    if (S0.size() > 40)
      S0 = S0.substr(0, 37) + "...";
    return S0;
  };
  SmallVector<DumpEntry, 64> Entries;
  for (auto &KV : P.I.Types) {
    const ast::Expr *Ex = KV.first;
    const TypeAndValue &TV = KV.second;
    if (!Ex->getBegin().isValid())
      continue;
    std::string L = Loc(Ex->getBegin()) + " expr `" + Text(Ex) + "` " +
                    getModeName(TV.M) + " " + typeString(TV.Ty, Pkg);
    if (TV.Val)
      L += " = " + TV.Val->str();
    Entries.push_back({Ex->getBegin().getRaw(), Ex->getEnd().getRaw(), 1, L});
  }
  for (auto &KV : P.I.Defs) {
    if (!KV.second || !KV.first->getBegin().isValid())
      continue;
    Object *O = KV.second;
    std::string L = Loc(KV.first->getBegin()) + " def " + std::string(O->Name) +
                    " " +
                    (O->Ty ? typeString(O->Ty, Pkg) : std::string("<nil>"));
    Entries.push_back(
        {KV.first->getBegin().getRaw(), KV.first->getEnd().getRaw(), 0, L});
  }
  std::stable_sort(Entries.begin(), Entries.end(),
                   [](const DumpEntry &A, const DumpEntry &B) {
                     if (A.Begin != B.Begin)
                       return A.Begin < B.Begin;
                     if (A.Order != B.Order)
                       return A.Order < B.Order;
                     if (A.End != B.End)
                       return A.End < B.End;
                     return A.Text < B.Text;
                   });
  for (DumpEntry &En : Entries)
    OS << En.Text << "\n";
}
