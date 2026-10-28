//===- Decl.cpp - Declarations --------------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's resolver.go, decl.go and validtype.go (BSD-style
// license, Copyright The Go Authors).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/SmallPtrSet.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

//===----------------------------------------------------------------------===//
// Collecting objects
//===----------------------------------------------------------------------===//

Package *Checker::importedPackage(StringRef Path, SourceLoc Pos) {
  auto It = S.ImportedByPath.find(Path);
  if (It != S.ImportedByPath.end())
    return It->second;
  std::string Error;
  Package *P = S.Imp.import(Path, Error);
  if (!P) {
    err(Pos, diag::err_import_unresolved, {std::string(Path), Error});
    S.ImportedByPath[Mem.copy(Path)] = nullptr;
    return nullptr;
  }
  S.ImportedByPath[Mem.copy(Path)] = P;
  return P;
}

void Checker::importPackage(const ImportSpec *Spec, FileState &F) {
  std::string Path;
  if (!unquoteString(Spec->Path->Text, Path) || Path.empty()) {
    err(Spec->Path, diag::err_load_bad_import_path,
        {std::string(Spec->Path->Text)});
    return;
  }
  Package *P = importedPackage(Path, Spec->Path->getBegin());
  if (!P) {
    // Declare a fake package so uses do not cascade.
    P = Mem.make<Package>();
    P->Path = Mem.copy(Path);
    StringRef Base = P->Path;
    size_t Slash = Base.find_last_of('/');
    if (Slash != StringRef::npos)
      Base = Base.substr(Slash + 1);
    P->Name = Base;
    P->Fake = true;
    P->PkgScope = Mem.newScope(nullptr, {}, "package");
  }
  StringRef N = P->Name;
  const ast::Ident *NameId = Spec->Name;
  if (NameId)
    N = name(NameId);
  if (N == "init") {
    err(NameId ? NameId->getBegin() : Spec->Path->getBegin(),
        diag::err_sema_decl,
        {"cannot import package as init - init must be a func"});
    return;
  }
  if (N == "_") {
    return; // blank import: only the side effects
  }
  PkgName *PN = Mem.make<PkgName>();
  PN->Name = Mem.copy(N);
  PN->Pkg = S.Pkg;
  PN->Imported = P;
  PN->Foreign = P->IsForeign;
  PN->Pos = NameId ? NameId->getBegin() : Spec->Path->getBegin();
  PN->ImportLoc = Spec->Path->getBegin();
  PN->Ty = typ(BasicKind::Invalid);
  PN->Used = P->Fake;
  if (NameId)
    recordDef(NameId, PN);
  else
    recordImplicit(Spec, PN);
  if (N == ".") {
    // A dot import declares the exported names of the package in the file
    // scope (types2); it is unused if none of them is used. The objects are
    // not changed: they may be imported into other files too.
    SmallVector<StringRef, 16> Names;
    for (auto &KV : P->PkgScope->Elems)
      if (isExportedName(KV.first) || P->IsForeign)
        Names.push_back(KV.first);
    llvm::sort(Names);
    for (StringRef Name : Names) {
      Object *O = P->PkgScope->lookupLocal(Name);
      if (Object *Alt = F.FileScope->lookupLocal(Name)) {
        err(Spec->Name, diag::err_redeclared, {std::string(Name)})
            .note(Alt->Pos, diag::note_other_declaration, {std::string(Name)});
        continue;
      }
      F.FileScope->Elems[Name] = O;
      S.DotImportMap[{F.FileScope, Name}] = PN;
    }
    F.Imports.push_back(PN);
    return;
  }
  // Declared in the file scope; visible everywhere in the file.
  Object *Old = F.FileScope->insert(PN);
  if (Old) {
    err(PN->Pos, diag::err_redeclared, {std::string(N)})
        .note(Old->Pos, diag::note_other_declaration, {std::string(N)});
    return;
  }
  PN->Parent = F.FileScope;
  F.Imports.push_back(PN);
}

/// `@Pinned` before a type declaration (OWN-R10).
static bool hasPinnedAttr(const GenDecl *GD) {
  for (const Attribute *A : GD->Attrs)
    if (!A->Pkg && A->Name && A->Name->Name.str() == "Pinned")
      return true;
  return false;
}

static StringRef recvBaseName(const ast::Expr *R, bool &Ptr,
                              const ast::Ident *&Id) {
  Ptr = false;
  Id = nullptr;
  for (;;) {
    if (auto *P = dyn_cast<ParenExpr>(R)) {
      R = P->X;
    } else if (auto *St = dyn_cast<StarExpr>(R)) {
      if (Ptr)
        return {};
      Ptr = true;
      R = St->X;
    } else if (auto *Ix = dyn_cast<IndexExpr>(R)) {
      R = Ix->X;
    } else {
      break;
    }
  }
  if (auto *I = dyn_cast<ast::Ident>(R)) {
    Id = I;
    return I->Name.str();
  }
  return {};
}

void Checker::collectObjects() {
  Scope *PkgScope = S.Pkg->PkgScope;
  uint32_t Order = 0;
  struct PendingMethod {
    Func *F;
    DeclInfo *D;
    StringRef Base;
  };
  SmallVector<PendingMethod, 16> PendingMethods;

  auto DeclarePkg = [&](const ast::Ident *Id, Object *O, DeclInfo *D) {
    O->Order = Order++;
    O->Pkg = S.Pkg;
    if (Id)
      recordDef(Id, O);
    if (D)
      S.ObjMap[O] = D;
    S.PkgObjects.push_back(O);
    StringRef N = O->Name;
    if (N == "_")
      return;
    if (N == "init") {
      if (!isa<Func>(O))
        err(O->Pos, diag::err_sema_decl,
            {"cannot declare init - must be func"});
      return; // init functions are not declared
    }
    if (N == "main" && S.Pkg->Name == "main" && !isa<Func>(O)) {
      err(O->Pos, diag::err_sema_decl, {"cannot declare main - must be func"});
      return;
    }
    if (Object *Old = PkgScope->insert(O)) {
      err(O->Pos, diag::err_redeclared, {std::string(N)})
          .note(Old->Pos, diag::note_other_declaration, {std::string(N)});
      return;
    }
    O->Parent = PkgScope;
  };

  for (FileState &F : S.Files) {
    const ast::File *File = F.AST;
    if (File->Name && File->Name->Name.str() == "_")
      err(File->Name, diag::err_sema_decl, {"invalid package name _"});
    F.FileScope = Mem.newScope(PkgScope, File->getRange(), "file");
    for (const ast::Decl *D : File->Decls) {
      if (auto *GD = dyn_cast<GenDecl>(D)) {
        const ConstValue *IotaPtr = nullptr;
        (void)IotaPtr;
        const ValueSpec *LastWithValues = nullptr;
        unsigned Iota = 0;
        for (const Spec *Sp : GD->Specs) {
          switch (GD->Keyword) {
          case tok::kw_import:
            importPackage(cast<ImportSpec>(Sp), F);
            break;
          case tok::kw_const: {
            auto *VS = cast<ValueSpec>(Sp);
            const ValueSpec *Src = VS;
            bool Inherited = false;
            if (VS->Type || !VS->Values.empty()) {
              LastWithValues = VS;
            } else if (LastWithValues) {
              Src = LastWithValues;
              Inherited = true;
            }
            for (size_t K = 0; K < VS->Names.size(); ++K) {
              const ast::Ident *Id = VS->Names[K];
              Const *C = newConst(name(Id), nullptr, Id->getBegin());
              DeclInfo *DI = S.newDecl();
              DI->FileScope = F.FileScope;
              DI->VType = Src->Type;
              DI->Init = K < Src->Values.size() ? Src->Values[K] : nullptr;
              DI->Inherited = Inherited;
              DI->IotaVal = Iota;
              DeclarePkg(Id, C, DI);
            }
            // Arity errors.
            size_t NN = VS->Names.size(), NV = Src->Values.size();
            if (NV > NN && !Inherited)
              err(Src->Values[NN], diag::err_extra_init_expr,
                  {str(Src->Values[NN])});
            else if (NV < NN && (NV > 0 || Inherited || !Src->Type))
              err(VS->Names[NV], diag::err_missing_init_expr,
                  {std::string(name(VS->Names[NV]))});
            ++Iota;
            break;
          }
          case tok::kw_var: {
            auto *VS = cast<ValueSpec>(Sp);
            SmallVector<Var *, 4> Vars;
            for (const ast::Ident *Id : VS->Names) {
              Var *V = newVar(name(Id), nullptr, Id->getBegin());
              V->IsPackageLevel = true;
              Vars.push_back(V);
            }
            ArrayRef<Var *> VarsRef = Mem.copy(ArrayRef<Var *>(Vars));
            bool N1 = VS->Values.size() == 1 && VS->Names.size() > 1;
            for (size_t K = 0; K < VS->Names.size(); ++K) {
              DeclInfo *DI = S.newDecl();
              DI->FileScope = F.FileScope;
              DI->VType = VS->Type;
              if (N1) {
                DI->Lhs = VarsRef;
                DI->Init = VS->Values[0];
              } else if (K < VS->Values.size()) {
                DI->Init = VS->Values[K];
              }
              DeclarePkg(VS->Names[K], Vars[K], DI);
            }
            size_t NN = VS->Names.size(), NV = VS->Values.size();
            if (!N1 && NV > 0) {
              if (NV > NN)
                err(VS->Values[NN], diag::err_extra_init_expr,
                    {str(VS->Values[NN])});
              else if (NV < NN)
                err(VS->Names[NV], diag::err_missing_init_expr,
                    {std::string(name(VS->Names[NV]))});
            }
            if (NV == 0 && !VS->Type)
              err(VS->Names[0], diag::err_sema_decl,
                  {"missing type or init expr"});
            break;
          }
          case tok::kw_type: {
            auto *TS = cast<TypeSpec>(Sp);
            TypeName *TN =
                newTypeName(name(TS->Name), nullptr, TS->Name->getBegin());
            TN->IsAlias = TS->AssignLoc.isValid();
            DeclInfo *DI = S.newDecl();
            DI->FileScope = F.FileScope;
            DI->TDecl = TS;
            DI->Pinned = hasPinnedAttr(GD);
            DeclarePkg(TS->Name, TN, DI);
            break;
          }
          default:
            break;
          }
        }
      } else if (auto *FD = dyn_cast<FuncDecl>(D)) {
        StringRef N = name(FD->Name);
        Func *Fn = newFunc(N, nullptr, FD->Name->getBegin());
        Fn->Decl = FD;
        DeclInfo *DI = S.newDecl();
        DI->FileScope = F.FileScope;
        DI->FDecl = FD;
        if (!FD->Recv) {
          if (N == "init" || (N == "main" && S.Pkg->Name == "main")) {
            if (FD->Type->TypeParams && FD->Type->TypeParams->numFields())
              err(FD->Type->TypeParams, diag::err_init_type_params,
                  {std::string(N)});
            if ((FD->Type->Params && FD->Type->Params->numFields()) ||
                (FD->Type->Results && FD->Type->Results->numFields()))
              err(FD->Name, diag::err_init_signature, {std::string(N)});
          }
          if (N == "init" && !FD->Body)
            err(FD->Name, diag::err_init_with_body);
          DeclarePkg(FD->Name, Fn, DI);
        } else {
          // A method: associated with its base type below.
          Fn->Order = Order++;
          Fn->Pkg = S.Pkg;
          recordDef(FD->Name, Fn);
          S.ObjMap[Fn] = DI;
          S.PkgObjects.push_back(Fn);
          bool Ptr;
          const ast::Ident *BaseId;
          StringRef Base =
              FD->Recv->List.empty()
                  ? StringRef()
                  : recvBaseName(FD->Recv->List[0]->Type, Ptr, BaseId);
          PendingMethods.push_back({Fn, DI, Base});
        }
      } else if (isa<MacroDecl>(D) || isa<ProcMacroDecl>(D) ||
                 isa<MacroCallDecl>(D)) {
        // Expanded before type checking (P09).
      }
    }
  }

  // File scope names must not collide with package scope names.
  for (FileState &F : S.Files) {
    for (auto &KV : F.FileScope->Elems) {
      if (Object *Alt = PkgScope->lookupLocal(KV.first)) {
        err(Alt->Pos, diag::err_sema_decl,
            {std::string(KV.first) +
             " already declared through import of package " +
             std::string(cast<PkgName>(KV.second)->Imported->Name)});
      }
    }
  }

  // Associate methods with their receiver base types. The base type is
  // found through alias declarations (types2 resolveBaseTypeName), and a
  // pointer on the way makes it a pointer method before its signature is
  // checked (method lookups may need that first).
  for (PendingMethod &PM : PendingMethods) {
    bool Ptr = false;
    TypeName *Base = nullptr;
    if (!PM.Base.empty())
      Base = resolveRecvBase(PM.F->Decl->Recv->List[0]->Type, Ptr);
    if (Base) {
      PM.F->HasPtrRecv = Ptr;
      S.Methods[Base].push_back({PM.F, PM.D});
    } else {
      S.OrphanMethods.push_back({PM.F, PM.D});
    }
  }
}

TypeName *Checker::resolveRecvBase(const ast::Expr *R, bool &Ptr) {
  const ast::Ident *Id;
  StringRef N = recvBaseName(R, Ptr, Id);
  llvm::SmallPtrSet<TypeName *, 4> Seen;
  while (!N.empty()) {
    auto *TN = dyn_cast_or_null<TypeName>(S.Pkg->PkgScope->lookupLocal(N));
    if (!TN || !Seen.insert(TN).second)
      return nullptr;
    DeclInfo *D = S.ObjMap.lookup(TN);
    if (!D || !D->TDecl)
      return nullptr;
    if (!D->TDecl->AssignLoc.isValid())
      return TN; // a defined type
    // An alias: continue with its right-hand side, a (pointer to a) name.
    const ast::Expr *T = D->TDecl->Type;
    while (auto *P = dyn_cast<ParenExpr>(T))
      T = P->X;
    if (auto *St = dyn_cast<StarExpr>(T)) {
      if (Ptr)
        return nullptr;
      Ptr = true;
      T = St->X;
      while (auto *P = dyn_cast<ParenExpr>(T))
        T = P->X;
    }
    auto *Next = dyn_cast<ast::Ident>(T);
    N = Next ? Next->Name.str() : StringRef();
  }
  return nullptr;
}

void Checker::packageObjects() {
  // Non-alias types first, then aliases, then everything else (types2).
  SmallVector<Object *, 32> Types, Aliases, Others;
  for (Object *O : S.PkgObjects) {
    if (auto *TN = dyn_cast<TypeName>(O)) {
      (TN->IsAlias ? Aliases : Types).push_back(O);
    } else if (!isa<Func>(O) || !cast<Func>(O)->Decl ||
               !cast<Func>(O)->Decl->Recv) {
      Others.push_back(O);
    }
  }
  for (Object *O : Types)
    objDecl(O, nullptr);
  for (Object *O : Aliases)
    objDecl(O, nullptr);
  for (Object *O : Others)
    objDecl(O, nullptr);
  // Methods: resolve all signatures (also those of invalid receivers).
  for (Object *O : S.PkgObjects)
    if (auto *F = dyn_cast<Func>(O))
      if (F->Decl && F->Decl->Recv)
        objDecl(F, nullptr);
  processDelayed();
}

//===----------------------------------------------------------------------===//
// Object declarations
//===----------------------------------------------------------------------===//

bool Checker::validCycle(Object *Start, bool &Nval, bool &Ndef) {
  size_t From = Start->Color - 2;
  Nval = Ndef = false;
  unsigned NV = 0, ND = 0;
  size_t Len = ObjPath.size() - From;
  for (size_t K = From; K < ObjPath.size(); ++K) {
    Object *O = ObjPath[K];
    if (isa<Const>(O) || isa<Var>(O)) {
      ++NV;
    } else if (auto *TN = dyn_cast<TypeName>(O)) {
      if (E.InTParamList && TN->Ty && isGeneric(TN->Ty))
        return false;
      if (!TN->IsAlias)
        ++ND;
    }
  }
  if (NV == Len)
    return true;
  if (NV == 0 && ND > 0)
    return true;
  return false;
}

void Checker::cycleError(ArrayRef<Object *> Cycle, int Start) {
  // Report at the object Start, by default the first one in source order.
  size_t First = 0;
  if (Start >= 0) {
    First = Start;
  } else {
    for (size_t K = 1; K < Cycle.size(); ++K)
      if (Cycle[K]->Pos.getRaw() < Cycle[First]->Pos.getRaw())
        First = K;
  }
  Object *O = Cycle[First];
  bool IsType = isa<TypeName>(O);
  if (Cycle.size() == 1) {
    if (IsType)
      err(O->Pos, diag::err_invalid_recursive_type_self,
          {std::string(O->Name)});
    else
      err(O->Pos, diag::err_invalid_cycle, {std::string(O->Name)});
    return;
  }
  Err E2 = IsType
               ? err(O->Pos, diag::err_invalid_recursive_type,
                     {std::string(O->Name)})
               : err(O->Pos, diag::err_invalid_cycle, {std::string(O->Name)});
  for (size_t K = 0; K < Cycle.size(); ++K) {
    Object *A = Cycle[(First + K) % Cycle.size()];
    Object *B = Cycle[(First + K + 1) % Cycle.size()];
    E2.note(A->Pos,
            std::string(A->Name) + " refers to " + std::string(B->Name));
  }
}

void Checker::objDecl(Object *O, NamedType *Def) {
  if (O->Color == 1)
    return;
  if (O->Color >= 2) {
    // A cycle: O is on the object path.
    bool Nval, Ndef;
    switch (O->getKind()) {
    case Object::Kind::Const:
    case Object::Kind::Var:
      if (!validCycle(O, Nval, Ndef)) {
        cycleError(ArrayRef<Object *>(ObjPath).slice(O->Color - 2));
        O->Ty = invalid();
      } else if (!O->Ty) {
        // Reported by the initialization cycle check.
        O->Ty = invalid();
      }
      break;
    case Object::Kind::TypeName:
      if (!validCycle(O, Nval, Ndef)) {
        cycleError(ArrayRef<Object *>(ObjPath).slice(O->Color - 2));
        if (!cast<TypeName>(O)->IsAlias || !O->Ty)
          O->Ty = invalid();
        else
          O->Ty = invalid();
      } else if (!O->Ty) {
        O->Ty = invalid();
      }
      break;
    case Object::Kind::Func:
      // types2 guards functions with a placeholder signature, so a function
      // that refers to itself in its own signature is not a cycle.
      break;
    default:
      break;
    }
    return;
  }
  auto It = S.ObjMap.find(O);
  if (It == S.ObjMap.end()) {
    // An object without declaration info (an interface method, a universe
    // object) needs no resolving. Body checkers share it, so only phase 1
    // marks it.
    if (!Bodies)
      O->Color = 1;
    return;
  }
  DeclInfo *D = It->second;
  ObjPath.push_back(O);
  O->Color = static_cast<uint8_t>(std::min<size_t>(ObjPath.size() + 1, 250));
  Env Saved = E;
  E = Env();
  E.Sc = D->FileScope;
  E.Pos = SourceLoc();
  E.Decl = nullptr;
  E.Cur = D;
  bool SavedTP = E.InTParamList;
  (void)SavedTP;
  switch (O->getKind()) {
  case Object::Kind::Const: {
    IotaStore.push_back(ConstValue::makeInt64(D->IotaVal));
    constDecl(cast<Const>(O), D->VType, D->Init, D->Inherited,
              &IotaStore.back());
    break;
  }
  case Object::Kind::Var:
    varDecl(cast<Var>(O), D->Lhs, D->VType, D->Init);
    break;
  case Object::Kind::TypeName:
    typeDecl(cast<TypeName>(O), D->TDecl, Def, D->Pinned);
    break;
  case Object::Kind::Func:
    funcDecl(cast<Func>(O), D);
    break;
  default:
    break;
  }
  E = Saved;
  ObjPath.pop_back();
  O->Color = 1;
}

void Checker::constDecl(Const *O, const ast::Expr *Typ, const ast::Expr *Init,
                        bool Inherited, const ConstValue *Iota) {
  const ConstValue *SavedIota = E.Iota;
  E.Iota = Iota;
  Type *T = nullptr;
  if (Typ) {
    T = typ(Typ);
    if (isValid(T)) {
      if (!isConstType(T)) {
        if (isValid(under(T)))
          err(Typ, diag::err_invalid_constant_type, {str(T)});
        T = invalid();
      }
    }
    O->Ty = T;
    if (!isValid(T)) {
      E.Iota = SavedIota;
      return; // types2: the initializer is not checked
    }
  }
  Operand X;
  if (Init) {
    if (Inherited) {
      // Errors in an inherited expression are reported at the name.
      SourceLoc Saved = E.Pos;
      (void)Saved;
    }
    expr(X, Init);
  }
  initConst(O, X);
  E.Iota = SavedIota;
}

void Checker::varDecl(Var *O, ArrayRef<Var *> Lhs, const ast::Expr *Typ,
                      const ast::Expr *Init) {
  if (Typ) {
    O->Ty = varType(Typ);
    // All variables of an n:1 declaration share the type.
    for (Var *V : Lhs)
      if (V != O && !V->Ty) {
        V->Ty = O->Ty;
      }
  }
  if (!Init) {
    if (!Typ)
      O->Ty = invalid();
    return;
  }
  if (Lhs.empty() || Lhs.size() == 1) {
    Operand X;
    ReverseDesc = std::string(O->Name);
    exprWithHint(X, Init, O->Ty);
    ReverseDesc.clear();
    initVar(O, X, "variable declaration");
    return;
  }
  // n:1 declaration `var a, b = f()`: resolve all variables at once.
  for (Var *V : Lhs) {
    if (V != O)
      V->Color = 1; // the first variable's declaration initializes all
  }
  initVars(Lhs, {const_cast<ast::Expr *>(Init)}, nullptr);
}

void Checker::typeDecl(TypeName *O, const TypeSpec *TD, NamedType *Def,
                       bool Pinned) {
  (void)Def;
  if (TD->AssignLoc.isValid()) {
    // Alias, possibly generic (Go 1.24).
    Scope *SavedSc = E.Sc;
    ArrayRef<TypeParamType *> TPs;
    if (TD->TypeParams && TD->TypeParams->numFields()) {
      Scope *Sc = openScope(TD->getRange(), "type parameters");
      E.Sc = Sc;
      bool SavedTP = E.InTParamList;
      E.InTParamList = true;
      auto Collected = collectTypeParams(
          TD->TypeParams, Sc, [&](ArrayRef<TypeParamType *> Ps) {
            cast<TypeName>(O)->AliasTParams = Mem.copy(Ps);
          });
      E.InTParamList = SavedTP;
      TPs = Mem.copy(ArrayRef<TypeParamType *>(Collected));
      cast<TypeName>(O)->AliasTParams = TPs;
    }
    O->Ty = invalid(); // while resolving
    Type *T = definedType(TD->Type, nullptr);
    E.Sc = SavedSc;
    O->Ty = T;
    return;
  }
  NamedType *N = Ctx.newNamed(O);
  N->Pinned = Pinned; // OWN-R10
  O->Ty = N;
  // Checked in declaration order, after the right hand side is known.
  later([this, N]() { validType(N); });
  Scope *Saved = E.Sc;
  if (TD->TypeParams && TD->TypeParams->numFields()) {
    Scope *Sc = openScope(TD->getRange(), "type parameters");
    E.Sc = Sc;
    bool SavedTP = E.InTParamList;
    E.InTParamList = true;
    auto TPs = collectTypeParams(
        TD->TypeParams, Sc,
        [&](ArrayRef<TypeParamType *> Ps) { N->TParams = Mem.copy(Ps); });
    E.InTParamList = SavedTP;
    N->TParams = Mem.copy(ArrayRef<TypeParamType *>(TPs));
  }
  N->Resolving = true;
  Type *Rhs = definedType(TD->Type, N);
  N->Resolving = false;
  E.Sc = Saved;
  if (isa<TypeParamType>(Rhs)) {
    err(TD->Type, diag::err_cannot_use_type_param_as_rhs);
    Rhs = invalid();
  }
  Type *U = Rhs;
  if (auto *RN = dyn_cast<NamedType>(Rhs)) {
    // `type G[T any] G[T]`, or `type A[T any] B[T]; type B[T any] A[T]`: an
    // instance whose chain of right-hand sides leads back to N has no
    // underlying type (types2: Named.under).
    if (RN->Orig && !ReportedCycle.count(N)) {
      SmallVector<Object *, 4> Chain{N->Obj};
      NamedType *P = RN;
      NamedType *Back = nullptr;
      while (P && Chain.size() < 1000) {
        NamedType *PO = P->origin();
        if (PO == N) {
          Back = PO;
          break;
        }
        Chain.push_back(PO->Obj);
        P = PO->DirectRhs ? dyn_cast<NamedType>(PO->DirectRhs) : nullptr;
      }
      if (Back) {
        for (Object *C : Chain)
          if (auto *CN = dyn_cast_or_null<NamedType>(C->Ty))
            ReportedCycle.insert(CN);
        cycleError(Chain);
        N->Underlying = invalid();
        N->FromRHS = invalid();
        collectMethods(O);
        return;
      }
    }
    if (RN->Orig && !RN->Expanded)
      expandNamed(RN);
    if (RN->Orig && !RN->Expanded) {
      // An instance of a generic type that is still being declared
      // (`type Bar[Q any] Foo[Q]` inside Foo's declaration): under() follows
      // DirectRhs until the instance can expand (types2: lazy Named.under).
      N->DirectRhs = RN;
      later([this, N, RN]() {
        if (!N->Underlying)
          N->Underlying = under(RN);
      });
    }
    if (!RN->Orig) {
      // `type A B; type B A`: a cycle of defined types without a type
      // literal in between (types2: Named.under).
      N->DirectRhs = RN;
      SmallVector<Object *, 4> Chain{N->Obj};
      NamedType *P = RN;
      while (P && P != N && Chain.size() < 1000) {
        Chain.push_back(P->Obj);
        P = P->DirectRhs;
      }
      if (P == N && !ReportedCycle.count(N)) {
        for (Object *C : Chain)
          if (auto *CN = dyn_cast_or_null<NamedType>(C->Ty))
            ReportedCycle.insert(CN);
        cycleError(Chain);
        N->DirectRhs = nullptr;
        RN->Underlying = RN->Underlying ? RN->Underlying : invalid();
      }
    }
    // Not resolved yet (RN is still being declared): under() follows
    // DirectRhs once RN is complete.
    U = RN->Underlying ? RN->Underlying : (N->DirectRhs ? nullptr : invalid());
    // Predeclared pointer kinds keep their kind through `type X Unique[T]`?
    // No: a defined type does not inherit the predeclared pointer semantics.
  } else if (auto *Ba = dyn_cast<BasicType>(Rhs)) {
    U = Ba;
  }
  N->Underlying = U;
  N->FromRHS = Rhs;
  collectMethods(O);
}

void Checker::collectMethods(TypeName *O) {
  auto *N = dyn_cast_or_null<NamedType>(O->Ty);
  auto It = S.Methods.find(O);
  if (!N)
    return;
  if (!N->Methods)
    N->Methods = Ctx.newMethodList();
  if (It == S.Methods.end())
    return;
  // Field names for the field/method collision check.
  llvm::StringSet<> FieldNames;
  if (auto *St = dyn_cast_or_null<StructType>(N->Underlying))
    for (Var *F : St->Fields)
      FieldNames.insert(F->Name);
  llvm::StringMap<Func *> Seen;
  for (MethodInfo &MI : It->second) {
    Func *M = MI.F;
    StringRef MN = M->Name;
    if (MN != "_") {
      auto Ins = Seen.try_emplace(MN, M);
      if (!Ins.second) {
        err(M->Pos, diag::err_duplicate_method,
            {std::string(O->Name), std::string(MN),
             std::to_string(S.SM.getLineNumber(Ins.first->second->Pos))});
        continue;
      }
      if (FieldNames.contains(MN)) {
        err(M->Pos, diag::err_field_and_method, {std::string(MN)});
        continue;
      }
    }
    N->Methods->push_back(M);
  }
}

/// `A[P any]` for a generic alias, as types2 prints it in messages.
std::string Checker::genericAliasString(TypeName *TN) {
  std::string R = std::string(TN->Name) + "[";
  ArrayRef<TypeParamType *> L = TN->AliasTParams;
  for (size_t I = 0; I < L.size(); ++I) {
    if (I)
      R += L[I]->Bound != L[I - 1]->Bound ? " " + str(L[I - 1]->Bound) + ", "
                                          : ", ";
    R += str(L[I]);
  }
  if (!L.empty())
    R += " " + str(L.back()->Bound);
  return R + "]";
}

/// For an alias chain ending in an instantiation of a generic type (possibly
/// behind pointers), the instantiation expression, else null.
const ast::Expr *Checker::instantiatedAliasTarget(TypeName *TN) {
  for (unsigned Depth = 0; TN && Depth < 100; ++Depth) {
    DeclInfo *D = S.ObjMap.lookup(TN);
    if (!D || !D->TDecl || !D->TDecl->AssignLoc.isValid())
      return nullptr;
    const ast::Expr *T = D->TDecl->Type;
    for (;;) {
      if (auto *P = dyn_cast<ParenExpr>(T))
        T = P->X;
      else if (auto *St = dyn_cast<StarExpr>(T))
        T = St->X;
      else
        break;
    }
    if (auto *Ix = dyn_cast<IndexExpr>(T)) {
      auto *Id = dyn_cast<ast::Ident>(Ix->X);
      auto *G = Id ? dyn_cast_or_null<TypeName>(
                         S.Pkg->PkgScope->lookupLocal(name(Id)))
                   : nullptr;
      if (!G)
        return nullptr;
      objDecl(G, nullptr);
      if (G->IsAlias ? !G->AliasTParams.empty() : (G->Ty && isGeneric(G->Ty)))
        return Ix;
      return nullptr;
    }
    auto *Id = dyn_cast<ast::Ident>(T);
    TN = Id ? dyn_cast_or_null<TypeName>(S.Pkg->PkgScope->lookupLocal(name(Id)))
            : nullptr;
  }
  return nullptr;
}

void Checker::collectRecv(Func *F, const FieldList *Recv, Scope *Sc,
                          DeclInfo *D, const FuncDecl *FD,
                          NamedType *&RecvNamed) {
  (void)D;
  RecvNamed = nullptr;
  if (Recv->List.empty() || Recv->numFields() == 0) {
    err(FD->Name, diag::err_receiver_missing);
    return;
  }
  if (Recv->numFields() > 1) {
    err(Recv->List.size() > 1 ? Recv->List[1]->getBegin() : Recv->getBegin(),
        diag::err_receiver_count);
  }
  const Field *RF = Recv->List[0];
  const ast::Expr *TE = RF->Type;
  bool Ptr = false;
  const ast::Expr *Cur = TE;
  while (auto *P = dyn_cast<ParenExpr>(Cur))
    Cur = P->X;
  if (auto *St = dyn_cast<StarExpr>(Cur)) {
    Ptr = true;
    Cur = St->X;
    while (auto *P = dyn_cast<ParenExpr>(Cur))
      Cur = P->X;
  }
  const IndexExpr *Ix = dyn_cast<IndexExpr>(Cur);
  const ast::Expr *BaseE = Ix ? Ix->X : Cur;
  while (auto *P = dyn_cast<ParenExpr>(BaseE))
    BaseE = P->X;

  Type *RecvT = nullptr;
  SmallVector<TypeParamType *, 4> RTPs;
  // Resolve the base type name in the package scope.
  auto *BaseId = dyn_cast<ast::Ident>(BaseE);
  TypeName *BaseTN = nullptr;
  if (BaseId) {
    Object *O = S.Pkg->PkgScope->lookupLocal(name(BaseId));
    if (!O)
      O = Ctx.universe()->lookupLocal(name(BaseId));
    BaseTN = dyn_cast_or_null<TypeName>(O);
    if (!O)
      err(BaseId, diag::err_undefined, {std::string(name(BaseId))});
    else if (!BaseTN)
      err(BaseId, diag::err_not_a_type, {std::string(name(BaseId))});
  } else if (auto *Sel = dyn_cast<SelectorExpr>(BaseE)) {
    Operand X;
    selector(X, Sel, true);
    if (X.M == Mode::TypeExpr)
      err(TE, diag::err_receiver_method_pkg, {str(TE)});
  } else {
    err(TE, diag::err_invalid_receiver, {str(TE)});
  }
  if (BaseTN) {
    recordUse(BaseId, BaseTN);
    objDecl(BaseTN, nullptr);
    Type *BT = BaseTN->Ty;
    // An alias receiver denotes its target: `type T0 = *T; func (T0) m()`
    // is a pointer method of T.
    if (BaseTN->IsAlias && BT && isValid(BT))
      if (auto *P = dyn_cast<PointerType>(BT); P && !Ptr && !Ix) {
        Ptr = true;
        BT = P->Elem;
      }
    auto *BN = dyn_cast_or_null<NamedType>(BT);
    // types2 collectRecv: no methods on generic aliases, nor on an alias of
    // an instantiated type (`type A = T[int]`). The receiver stays invalid.
    bool Rejected = false;
    if (BaseTN->IsAlias && BT && isValid(BT)) {
      if (!BaseTN->AliasTParams.empty()) {
        Rejected = true;
        if (!Ix) {
          err(TE, diag::err_generic_without_instantiation,
              {"type", str(TE), ""});
        } else {
          err(BaseE, diag::err_sema_decl,
              {"cannot define new methods on generic alias type " +
               genericAliasString(BaseTN)});
          // The receiver type parameters stay declared (bound any), so the
          // body does not report them as undefined.
          unsigned K = 0;
          for (const ast::Expr *A : Ix->Indices) {
            auto *Id = dyn_cast<ast::Ident>(A);
            if (!Id)
              continue;
            TypeName *TN = newTypeName(name(Id), nullptr, Id->getBegin());
            TN->Ty = Ctx.newTypeParam(TN, K++, Ctx.anyType());
            recordDef(Id, TN);
            if (name(Id) != "_")
              declare(Sc, Id, TN, Sc->Extent.getBegin());
          }
        }
      } else if (!Ix) {
        if (const ast::Expr *Inst = instantiatedAliasTarget(BaseTN)) {
          Rejected = true;
          err(BaseE, diag::err_sema_decl,
              {"cannot define new methods on instantiated type " + str(Inst)});
        }
      }
    }
    if (Rejected) {
      // RecvT stays invalid: no follow-on errors.
    } else if (!BN || BN->Obj->Pkg != S.Pkg) {
      // types2 validRecv: a basic type is a non-local type too.
      bool NonLocal =
          (BN && BN->Obj->Pkg != S.Pkg) || llvm::isa_and_nonnull<BasicType>(BT);
      if (BT && isValid(BT))
        err(BaseE,
            NonLocal ? diag::err_receiver_method_pkg
                     : diag::err_invalid_receiver,
            {str(BaseTN->IsAlias ? BaseTN->Ty : BT)});
    } else {
      Type *U = BN->Underlying;
      if (U && (isa<PointerType>(U) || isa<InterfaceType>(U))) {
        err(BaseE, diag::err_invalid_receiver,
            {str(BN) + " (pointer or interface type)"});
      } else if (BN->Ptr != PtrKind::None) {
        err(BaseE, diag::err_receiver_method_pkg, {str(BN)});
      } else {
        RecvNamed = BN;
        if (!BN->TParams.empty()) {
          if (!Ix) {
            err(TE, diag::err_generic_without_instantiation,
                {"type", str(TE), ""});
            RecvT = invalid();
          } else {
            if (Ix->Indices.size() != BN->TParams.size()) {
              err(Ix, diag::err_invalid_receiver_type_params,
                  {num(Ix->Indices.size()) + " type parameter" +
                       (Ix->Indices.size() == 1 ? "" : "s"),
                   num(BN->TParams.size())});
              RecvT = invalid();
            } else {
              unsigned K = 0;
              for (const ast::Expr *A : Ix->Indices) {
                auto *Id = dyn_cast<ast::Ident>(A);
                if (!Id) {
                  err(A, diag::err_sema_decl,
                      {"receiver type parameter " + str(A) +
                       " must be an identifier"});
                  RecvT = invalid();
                  break;
                }
                TypeName *TN = newTypeName(name(Id), nullptr, Id->getBegin());
                TypeParamType *TP = Ctx.newTypeParam(TN, K++, nullptr);
                TN->Ty = TP;
                recordDef(Id, TN);
                if (name(Id) != "_")
                  declare(Sc, Id, TN, Sc->Extent.getBegin());
                RTPs.push_back(TP);
              }
              if (!RecvT) {
                SmallVector<Type *, 4> Args(RTPs.begin(), RTPs.end());
                for (size_t J = 0; J < RTPs.size(); ++J)
                  RTPs[J]->Bound =
                      subst(BN->TParams[J]->Bound, BN->TParams, Args);
                RecvT = instantiateNamed(BN, Args);
                recordTypeAndValue(Ix, Mode::TypeExpr, RecvT, nullptr);
              }
            }
          }
        } else {
          if (Ix) {
            err(Ix, diag::err_not_a_generic_type, {std::string(BN->Obj->Name)});
            RecvT = invalid();
          } else {
            RecvT = BN;
          }
        }
        if (RecvT && Ix == nullptr)
          recordTypeAndValue(BaseE, Mode::TypeExpr, RecvT, nullptr);
      }
    }
  }
  F->RecvTParams = Mem.copy(ArrayRef<TypeParamType *>(RTPs));
  F->HasPtrRecv = Ptr;
  F->MoveRecv = RF->isMove();
  if (!RecvT)
    RecvT = invalid();
  Type *VarT =
      (Ptr && isValid(RecvT)) ? static_cast<Type *>(Ctx.pointer(RecvT)) : RecvT;
  recordTypeAndValue(TE, Mode::TypeExpr, VarT, nullptr);
  StringRef RName = RF->Names.empty() ? StringRef("") : name(RF->Names[0]);
  Var *RV =
      newParam(RName, VarT,
               RF->Names.empty() ? RF->getBegin() : RF->Names[0]->getBegin());
  RV->IsParam = true;
  if (RF->isMove())
    RV->Mode = Var::MoveParam;
  if (!RF->Names.empty()) {
    recordDef(RF->Names[0], RV);
    if (RName != "_" && !RName.empty())
      declare(Sc, RF->Names[0], RV, FD->Type->getEnd());
  } else {
    recordImplicit(RF, RV);
  }
  F->Recv = RV;
}

void Checker::funcDecl(Func *O, DeclInfo *D) {
  const FuncDecl *FD = D->FDecl;
  Scope *Sc = Mem.newScope(E.Sc, FD->getRange(), "function");
  Sc->IsFunc = true;
  Scope *Saved = E.Sc;
  E.Sc = Sc;
  NamedType *RecvNamed = nullptr;
  if (FD->Recv) {
    collectRecv(O, FD->Recv, Sc, D, FD, RecvNamed);
  }
  FuncInfo FI = funcSignature(FD->Type, Sc, O);
  E.Sc = Saved;
  O->Ty = FI.Sig;
  O->Params = FI.Params;
  O->Results = FI.Results;
  O->Fails = FI.Sig->Fails;
  O->OwnTParams = FI.Sig->TParams;
  if (own() && FD->Recv) {
    checkMoveRecvSig(O, FD);
    checkSpecialMethod(O, FD);
  }
  if (!FD->Body && FD->Type->TypeParams && FD->Type->TypeParams->numFields())
    err(FD, diag::err_sema_decl, {"generic function is missing function body"});
  if (FD->Body && S.Opts.CheckBodies) {
    BodyJob J;
    J.F = O;
    J.Decl = FD;
    J.D = D;
    J.Sig = FI.Sig;
    J.FileScope = D->FileScope;
    J.Sc = Sc;
    J.Params = FI.Params;
    J.Results = FI.Results;
    J.Recv = O->Recv;
    // The parameters belong to this body. Set here, in phase 1, so that body
    // checkers never write the shared parameter objects (P03.10).
    for (Var *V : J.Params)
      V->FuncOwner = FD->Body;
    for (Var *V : J.Results)
      V->FuncOwner = FD->Body;
    if (J.Recv)
      J.Recv->FuncOwner = FD->Body;
    S.Bodies.push_back(J);
  }
  (void)RecvNamed;
}

//===----------------------------------------------------------------------===//
// Local declarations
//===----------------------------------------------------------------------===//

void Checker::declStmt(const DeclStmt *DS) {
  auto *GD = dyn_cast<GenDecl>(DS->D);
  if (!GD)
    return;
  const ValueSpec *LastWithValues = nullptr;
  unsigned Iota = 0;
  for (const Spec *Sp : GD->Specs) {
    switch (GD->Keyword) {
    case tok::kw_const: {
      auto *VS = cast<ValueSpec>(Sp);
      const ValueSpec *Src = VS;
      bool Inherited = false;
      if (VS->Type || !VS->Values.empty())
        LastWithValues = VS;
      else if (LastWithValues) {
        Src = LastWithValues;
        Inherited = true;
      }
      IotaStore.push_back(ConstValue::makeInt64(Iota));
      const ConstValue *IotaV = &IotaStore.back();
      SmallVector<Const *, 4> Cs;
      for (size_t K = 0; K < VS->Names.size(); ++K) {
        const ast::Ident *Id = VS->Names[K];
        Const *C = newConst(name(Id), nullptr, Id->getBegin());
        constDecl(C, Src->Type,
                  K < Src->Values.size() ? Src->Values[K] : nullptr, Inherited,
                  IotaV);
        Cs.push_back(C);
      }
      size_t NN = VS->Names.size(), NV = Src->Values.size();
      if (NV > NN && !Inherited)
        err(Src->Values[NN], diag::err_extra_init_expr, {str(Src->Values[NN])});
      else if (NV < NN)
        err(VS->Names[NV], diag::err_missing_init_expr,
            {std::string(name(VS->Names[NV]))});
      for (size_t K = 0; K < Cs.size(); ++K)
        declare(E.Sc, VS->Names[K], Cs[K], VS->getEnd());
      ++Iota;
      break;
    }
    case tok::kw_var: {
      auto *VS = cast<ValueSpec>(Sp);
      SmallVector<Var *, 4> Vars;
      for (const ast::Ident *Id : VS->Names)
        Vars.push_back(newVar(name(Id), nullptr, Id->getBegin()));
      Type *T = VS->Type ? varType(VS->Type) : nullptr;
      if (T)
        for (Var *V : Vars)
          V->Ty = T;
      if (!VS->Values.empty()) {
        if (VS->Values.size() == VS->Names.size()) {
          for (size_t K = 0; K < Vars.size(); ++K) {
            Operand X;
            ReverseDesc = std::string(Vars[K]->Name);
            exprWithHint(X, VS->Values[K], T);
            ReverseDesc.clear();
            initVar(Vars[K], X, "variable declaration");
          }
        } else {
          initVars(Vars, VS->Values, nullptr);
        }
      } else if (!T) {
        for (Var *V : Vars)
          V->Ty = invalid();
      }
      for (size_t K = 0; K < Vars.size(); ++K) {
        recordDef(VS->Names[K], Vars[K]);
        declare(E.Sc, VS->Names[K], Vars[K], VS->getEnd());
        LocalVars.push_back(Vars[K]);
      }
      break;
    }
    case tok::kw_type: {
      auto *TS = cast<TypeSpec>(Sp);
      TypeName *TN = newTypeName(name(TS->Name), nullptr, TS->Name->getBegin());
      TN->IsAlias = TS->AssignLoc.isValid();
      recordDef(TS->Name, TN);
      declare(E.Sc, TS->Name, TN, TS->Name->getBegin());
      ObjPath.push_back(TN);
      TN->Color =
          static_cast<uint8_t>(std::min<size_t>(ObjPath.size() + 1, 250));
      typeDecl(TN, TS, nullptr, hasPinnedAttr(GD));
      ObjPath.pop_back();
      TN->Color = 1;
      break;
    }
    default:
      break;
    }
  }
}

//===----------------------------------------------------------------------===//
// Valid recursive types
//===----------------------------------------------------------------------===//

bool Checker::validTypeRec(Type *T, SmallVector<NamedType *, 8> &Nest,
                           SmallVector<NamedType *, 8> &Path) {
  // types2 validtype.go: Nest is the stack of types containing (or, for
  // interfaces, embedding) other types; Path is the full path, for errors.
  if (!T)
    return true;
  DepthGuard DG(RecDepth);
  if (RecDepth > MaxRecDepth)
    return true;
  switch (T->getKind()) {
  case Type::Kind::Array:
    return validTypeRec(cast<ArrayType>(T)->Elem, Nest, Path);
  case Type::Kind::Struct:
    for (Var *F : cast<StructType>(T)->Fields)
      if (!validTypeRec(F->Ty, Nest, Path))
        return false;
    return true;
  case Type::Kind::Union:
    for (const Term &Tm : cast<UnionType>(T)->Terms)
      if (!validTypeRec(Tm.T, Nest, Path))
        return false;
    return true;
  case Type::Kind::Interface:
    for (Type *Em : cast<InterfaceType>(T)->Embeddeds)
      if (!validTypeRec(Em, Nest, Path))
        return false;
    return true;
  case Type::Kind::Named: {
    auto *N = cast<NamedType>(T);
    NamedType *O = N->origin();
    if (!isValid(under(N)))
      return false;
    bool Plain = !N->Orig && N->TParams.empty();
    if (Plain && ValidTypes.count(N))
      return true;
    for (NamedType *Ne : Nest) {
      if (!identical(Ne, N))
        continue;
      // N occurs inside its own definition (after expansion): invalid, no
      // matter how it is instantiated.
      N->Underlying = invalid();
      O->Underlying = invalid();
      for (size_t Start = 0; Start < Path.size(); ++Start)
        if (identical(Path[Start], N)) {
          SmallVector<Object *, 4> Cycle;
          for (size_t J = Start; J < Path.size(); ++J)
            Cycle.push_back(Path[J]->Obj);
          cycleError(Cycle, 0);
          return false;
        }
      return false;
    }
    Nest.push_back(N);
    Path.push_back(N);
    bool Ok = validTypeRec(O->FromRHS, Nest, Path);
    Nest.pop_back();
    Path.pop_back();
    if (Ok && Plain)
      ValidTypes.insert(N);
    return Ok;
  }
  case Type::Kind::TypeParam: {
    // A type parameter stands for the type argument it was instantiated with.
    if (Nest.empty())
      return true;
    size_t D = Nest.size() - 1;
    NamedType *Inst = Nest[D];
    ArrayRef<TypeParamType *> TPs = Inst->origin()->TParams;
    for (size_t I = 0; I < TPs.size(); ++I)
      if (TPs[I] == T && I < Inst->TArgs.size()) {
        // The argument must be valid where Inst was instantiated.
        SmallVector<NamedType *, 8> Outer(Nest.begin(), Nest.begin() + D);
        return validTypeRec(Inst->TArgs[I], Outer, Path);
      }
    return true;
  }
  default:
    return true;
  }
}

void Checker::validType(NamedType *T) {
  SmallVector<NamedType *, 8> Nest, Path;
  validTypeRec(T, Nest, Path);
}
