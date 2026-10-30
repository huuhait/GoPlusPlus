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
    addImportedUnits(P);
    return;
  }
  addImportedUnits(P);
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

static const ast::Expr *unparen(const ast::Expr *E) {
  while (auto *P = dyn_cast<ParenExpr>(E))
    E = P->X;
  return E;
}

/// A built-in attribute `@Name` before a declaration.
static bool hasDeclAttr(const GenDecl *GD, StringRef Name) {
  for (const Attribute *A : GD->Attrs)
    if (!A->Pkg && A->Name && A->Name->Name.str() == Name)
      return true;
  return false;
}

/// `@Ref` was renamed `@View` (ADR-0020, MAC-8): it is an unknown attribute
/// now. Reported once per declaration group.
void Checker::rejectRefAttr(const GenDecl *GD) {
  if (!own())
    return;
  for (const Attribute *A : GD->Attrs)
    if (!A->Pkg && A->Name && A->Name->Name.str() == "Ref" &&
        RefAttrReported.insert(A).second)
      err(A->Name, diag::err_unknown_attribute,
          {"Ref", "it was renamed @View (ADR-0020); mark a struct that holds "
                  "a view @View (OWN-V5)"});
}

/// `@Pinned` before a type declaration (OWN-R10).
static bool hasPinnedAttr(const GenDecl *GD) {
  return hasDeclAttr(GD, "Pinned");
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
      checkDeclMarks(D);
      // @Symbol is for a `unit` declaration only (UNI-13, MAC-8).
      if (!S.Opts.GoCompat) {
        auto *GDs = dyn_cast<GenDecl>(D);
        auto *FDs = dyn_cast<FuncDecl>(D);
        ArrayRef<Attribute *> As = GDs   ? GDs->Attrs
                                   : FDs ? FDs->Attrs
                                         : ArrayRef<Attribute *>();
        if (!(GDs && GDs->IsUnit))
          for (const Attribute *A : As)
            if (!A->Pkg && A->Name && name(A->Name) == "Symbol")
              err(A->AtLoc, diag::err_unit_symbol,
                  {"@Symbol is only for a `unit` declaration, not for " +
                   std::string(
                       GDs ? (GDs->Keyword == tok::kw_type
                                  ? "a type declaration"
                              : GDs->Keyword == tok::kw_var
                                  ? "a variable declaration"
                              : GDs->Keyword == tok::kw_const
                                  ? "a constant declaration"
                                  : "an import")
                           : (FDs && FDs->Recv ? "a method" : "a function"))});
      }
      // @Required and @Option are field attributes only, @Strict is for a
      // struct type declaration (MAC-8, TYP-15a).
      if (own()) {
        ArrayRef<Attribute *> Attrs;
        const char *What = "";
        if (auto *GD0 = dyn_cast<GenDecl>(D)) {
          Attrs = GD0->Attrs;
          What = GD0->IsUnit                     ? "a unit declaration"
                 : GD0->Keyword == tok::kw_type  ? "a type declaration"
                 : GD0->Keyword == tok::kw_var   ? "a variable declaration"
                 : GD0->Keyword == tok::kw_const ? "a constant declaration"
                                                 : "an import";
        } else if (auto *FD0 = dyn_cast<FuncDecl>(D)) {
          Attrs = FD0->Attrs;
          What = FD0->Recv ? "a method" : "a function";
        }
        for (const Attribute *A : Attrs) {
          if (A->Pkg || !A->Name)
            continue;
          StringRef AN = name(A->Name);
          if (AN == "Required")
            err(A->AtLoc, diag::err_required_placement, {What});
          else if (AN == "Option")
            err(A->AtLoc, diag::err_option_placement, {What});
          else if (AN == "Strict") {
            // TYP-15a: @Strict stands before a struct type declaration; each
            // type of a group must be a (non-alias) struct type literal, so
            // a defined type of a named struct is not one.
            auto *GD0 = dyn_cast<GenDecl>(D);
            if (!GD0 || GD0->Keyword != tok::kw_type) {
              err(A->AtLoc, diag::err_strict_placement, {What});
              continue;
            }
            if (A->HasArgs)
              err(A->AtLoc, diag::err_attr_no_args, {"@Strict"});
            for (const Spec *Sp : GD0->Specs) {
              auto *TS = cast<TypeSpec>(Sp);
              if (TS->AssignLoc.isValid())
                err(A->AtLoc, diag::err_strict_placement,
                    {"the alias " + std::string(name(TS->Name))});
              else if (!isa<ast::StructType>(unparen(TS->Type)))
                err(A->AtLoc, diag::err_strict_placement,
                    {"type " + std::string(name(TS->Name)) +
                     ", which is not a struct type"});
            }
          }
        }
      }
      if (auto *GD = dyn_cast<GenDecl>(D); GD && GD->IsUnit) {
        // UNI-1: a unit lives in the package block like a type or a function.
        for (const Spec *Sp : GD->Specs) {
          auto *US = cast<ast::UnitSpec>(Sp);
          auto *U = Mem.make<UnitObj>();
          U->Name = name(US->Name);
          U->Pos = US->Name->getBegin();
          DeclInfo *DI = S.newDecl();
          DI->FileScope = F.FileScope;
          DI->UDecl = US;
          if (Sp == GD->Specs.front()) // the attribute is read once
            readUnitSymbol(GD, U);
          DeclarePkg(US->Name, U, DI);
        }
      } else if (auto *GD = dyn_cast<GenDecl>(D)) {
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
            constArity(VS, Src, Inherited);
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
            // `var a, b = f()`: one declaration info for all names, so the
            // dependencies of f belong to every one of them (types2).
            DeclInfo *Shared = nullptr;
            for (size_t K = 0; K < VS->Names.size(); ++K) {
              DeclInfo *DI = N1 && Shared ? Shared : S.newDecl();
              if (N1 && !Shared)
                Shared = DI;
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
            DI->Strict = hasDeclAttr(GD, "Strict");
            DI->ViewOk = hasDeclAttr(GD, "View");
            DI->Abstract = hasDeclAttr(GD, "Abstract");
            rejectRefAttr(GD);
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
        readMethodMarks(FD, Fn);
        readOperatorMarks(FD, Fn);
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
  std::vector<NamedType *> Insts;
  ExpandedInsts = &Insts;
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
  // UNI-3: the units first, so that a type or a constant that names one sees
  // it resolved. A unit depends on units (and on constants of its factor)
  // only; the walk is iterative.
  for (Object *O : S.PkgObjects)
    if (auto *U = dyn_cast<UnitObj>(O))
      resolveUnit(U);
  for (Object *O : Types)
    objDecl(O, nullptr);
  for (Object *O : Aliases)
    objDecl(O, nullptr);
  // Every package-level type and its method list are final now; a local type
  // declared later (in a function literal) is final once declFinished says
  // so.
  TypesDeclared = true;
  // TYP-22: the `extends` rings, before anything asks for a parent, and
  // before the by-value sweep below would call them recursive types.
  {
    SmallVector<NamedType *, 32> Ts;
    for (Object *O : Types)
      if (auto *N = dyn_cast_or_null<NamedType>(O->Ty))
        Ts.push_back(N);
    checkExtendsCycles(Ts);
  }
  for (Object *O : Others)
    objDecl(O, nullptr);
  // Methods: resolve all signatures (also those of invalid receivers).
  for (Object *O : S.PkgObjects)
    if (auto *F = dyn_cast<Func>(O))
      if (F->Decl && F->Decl->Recv)
        objDecl(F, nullptr);
  // ADR-0024: the overload sets of the package, once every signature is known.
  checkOperatorSets();
  // Every package-level type is declared: check for by-value cycles (Go
  // reports the recursion and no instantiation cycle then), and mark the
  // generic types on an instantiation cycle before the delayed checks expand
  // instances, so no walk follows an instance graph that never ends
  // (onInstCycle; nothing is reported or made invalid here).
  for (size_t K = 0; K < WalkQueue.size(); ++K)
    validType(WalkQueue[K]);
  flushDeclRings();
  monoCheck(MonoInsts, /*Report=*/false);
  ExpandedInsts = nullptr;
  if (ByValueCycle) {
    // The instances expanded so far too: the declarations of a cycle are
    // invalid by now and no longer lead to the instances that kept a copy
    // of their fields.
    WalkQueue.insert(WalkQueue.end(), CycleSeeds.begin(), CycleSeeds.end());
    WalkQueue.insert(WalkQueue.end(), Insts.begin(), Insts.end());
    invalidateByValueCycles(WalkQueue);
  }
  WalkQueue.clear();
  CycleSeeds.clear();
  // TYP-17 to TYP-19: the walk of the package's hierarchy (once; an earlier
  // question about an abstract type may have run it already).
  ensureHierarchy();
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

void Checker::cycleError(ArrayRef<Object *> Cycle, int Start,
                         ArrayRef<SourceLoc> At) {
  // At (if given): where each member is written, for an instance its
  // instantiation, as types2 names it (Go's instance objects have that
  // position).
  auto PosOf = [&](size_t K) {
    return K < At.size() && At[K].isValid() ? At[K] : Cycle[K]->Pos;
  };
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
  if (IsType)
    ByValueCycle = true; // invalidateByValueCycles looks for what is left
  if (Cycle.size() == 1) {
    if (IsType)
      err(PosOf(First), diag::err_invalid_recursive_type_self,
          {std::string(O->Name)});
    else
      err(PosOf(First), diag::err_invalid_cycle, {std::string(O->Name)});
    return;
  }
  Err E2 = IsType ? err(PosOf(First), diag::err_invalid_recursive_type,
                        {std::string(O->Name)})
                  : err(PosOf(First), diag::err_invalid_cycle,
                        {std::string(O->Name)});
  for (size_t K = 0; K < Cycle.size(); ++K) {
    Object *A = Cycle[(First + K) % Cycle.size()];
    Object *B = Cycle[(First + K + 1) % Cycle.size()];
    E2.note(PosOf((First + K) % Cycle.size()),
            std::string(A->Name) + " refers to " + std::string(B->Name));
  }
}

void Checker::objDecl(Object *O, NamedType *Def) {
  if (O->Color == 1) {
    // A type whose declaration was deferred (see below) is declared when
    // declarePending asks for it; any other reference sees it pending, like
    // a type being resolved, so a chain stays off the stack however often
    // each type names the next.
    auto *TN = dyn_cast<TypeName>(O);
    if (!TN || TN != ForcePending || !PendingDecl.count(TN))
      return;
    ForcePending = nullptr;
    O->Color = 0;
  }
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
  // Resolving a type's right-hand side recurses once per level of type
  // expression and once per declaration reached from it, so a ring or chain
  // of named types that refer to each other would use the thread's stack
  // (P03.23). Past a fixed depth (a count, so the answer never depends on the
  // thread or its stack) a plain named type only gets its NamedType now: its
  // right-hand side is resolved from a work list when the declaration that
  // started the chain is done, or at once when something asks for its
  // underlying type (Checker::under). Whoever refers to it in between sees
  // the same thing as a reference to a type that is being resolved. A generic
  // type gets its type parameters now (an instantiation needs them); only its
  // right-hand side waits.
  //
  // The same for a defined type named in the right-hand side of an alias
  // (InAliasRhs): an alias has no type until its right-hand side is done, so
  // a defined type declared from there that names the alias again would see
  // nothing (`type N0 [1]N5[int]; type N1 N5[int]; type N5[P any] = N1`: N1
  // is N1 itself, which Go reports in any order). Declared after the alias,
  // it sees what the alias stands for, as it does when it comes first.
  if (DeclNest >= MaxDeclNest || InAliasRhs)
    if (auto *TN = dyn_cast<TypeName>(O);
        TN && !TN->IsAlias && D->TDecl && !PendingDecl.count(TN)) {
      NamedType *N = Ctx.newNamed(TN);
      N->Pinned = D->Pinned; // OWN-R10
      TN->Ty = N;
      PendingDecl[TN] = N;
      DeferredDecls.push_back(TN);
      if (D->TDecl->TypeParams && D->TDecl->TypeParams->numFields()) {
        // As typeDecl does it, with TN on the object path (a bound that
        // names TN is a valid cycle, not a new declaration).
        ObjPath.push_back(TN);
        TN->Color =
            static_cast<uint8_t>(std::min<size_t>(ObjPath.size() + 1, 250));
        Env SavedEnv = E;
        E = Env();
        E.Sc = D->FileScope;
        E.Cur = D;
        Scope *Sc = openScope(D->TDecl->getRange(), "type parameters");
        E.Sc = Sc;
        E.InTParamList = true;
        auto TPs = collectTypeParams(
            D->TDecl->TypeParams, Sc,
            [&](ArrayRef<TypeParamType *> Ps) { N->TParams = Mem.copy(Ps); });
        N->TParams = Mem.copy(ArrayRef<TypeParamType *>(TPs));
        PendingTParamScope[TN] = Sc;
        E = SavedEnv;
        ObjPath.pop_back();
      }
      TN->Color = 1;
      return;
    }
  // A package-level variable or constant whose initializer names one declared
  // later is declared from inside objDecl of the first, so a chain of n such
  // links (or a cycle of n) recurses n times, and the thread's stack is used
  // up at some n (P04.04). Whatever the links are made of, when less than
  // DeclReserveBytes of the stack is left (the worst one declaration can use:
  // an initializer nested to the parser's limit, R13) the rest of the
  // declaration runs on a stack of its own and this thread waits
  // (runOnNewStack; if the thread cannot be made, err_no_stack): the order of
  // the work and of its diagnostics is that of plain recursion, and the depth
  // that fits is bounded by memory, not by one stack. SegmentDepth: the level a
  // segment began at (its body asks again for the same level and must not start
  // another).
  if (SegmentDepth != DeclNest && shouldHop(DeclNest, DeclReserveBytes)) {
    unsigned Saved = SegmentDepth;
    SegmentDepth = DeclNest;
    bool Ran = runOnNewStack([&] { objDecl(O, Def); }, DeclSegmentBytes);
    SegmentDepth = Saved;
    if (!Ran) {
      // No stack for the rest of a chain this deep: stop here, with O an
      // invalid object that is declared (it is not tried again), and say so
      // once. Nothing was changed before the hop.
      if (!NoStack) {
        NoStack = true;
        err(O->Pos, diag::err_no_stack, {std::to_string(DeclNest)});
      }
      if (!O->Ty)
        O->Ty = invalid();
      O->Color = 1;
    }
    return;
  }
  ++DeclNest;
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
    typeDecl(cast<TypeName>(O), D->TDecl, Def, D->Pinned, D->Strict, D->ViewOk);
    break;
  case Object::Kind::Func:
    // Names used in a signature are no initialization dependencies of the
    // function, only those in its body are (types2: objDecl sets no decl
    // for a func).
    E.Cur = nullptr;
    funcDecl(cast<Func>(O), D);
    break;
  default:
    break;
  }
  E = Saved;
  ObjPath.pop_back();
  --DeclNest;
  O->Color = 1;
  if (ObjPath.empty() && !DrainingDecls)
    drainDeferredDecls();
}

/// Declares the types objDecl deferred, first come first served, until none
/// is left (a declaration may defer more).
void Checker::drainDeferredDecls() {
  DrainingDecls = true;
  for (size_t K = 0; K < DeferredDecls.size(); ++K)
    declarePending(DeferredDecls[K]);
  DeferredDecls.clear();
  DrainingDecls = false;
}

void Checker::declarePending(TypeName *TN) {
  if (!PendingDecl.count(TN))
    return;
  TypeName *Saved = ForcePending;
  ForcePending = TN;
  objDecl(TN, nullptr);
  ForcePending = Saved;
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
  // Errors in an inherited expression are reported at the name (types2's
  // errpos).
  SourceLoc SavedPos = ErrPos;
  if (Init && Inherited)
    ErrPos = O->Pos;
  if (Init)
    expr(X, Init);
  initConst(O, X);
  ErrPos = SavedPos;
  E.Iota = SavedIota;
}

void Checker::varDecl(Var *O, ArrayRef<Var *> Lhs, const ast::Expr *Typ,
                      const ast::Expr *Init) {
  if (Typ) {
    O->Ty = varType(Typ, true, "the type of a variable");
    // All variables of an n:1 declaration share the type.
    for (Var *V : Lhs)
      if (V != O && !V->Ty) {
        V->Ty = O->Ty;
      }
  }
  if (!Init) {
    if (!Typ)
      O->Ty = invalid();
    else
      needZeroValue(O->Pos, O->Ty,
                    "cannot declare " + std::string(O->Name) +
                        " without a value");
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
                       bool Pinned, bool Strict, bool ViewOk) {
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
    ++InAliasRhs;
    Type *T = definedType(TD->Type, nullptr);
    --InAliasRhs;
    E.Sc = SavedSc;
    O->Ty = T;
    return;
  }
  NamedType *N;
  if (auto PIt = PendingDecl.find(O); PIt != PendingDecl.end()) {
    N = PIt->second; // deferred by objDecl: the NamedType exists already
    PendingDecl.erase(PIt);
  } else {
    N = Ctx.newNamed(O);
    N->Pinned = Pinned; // OWN-R10
    O->Ty = N;
  }
  // Checked in declaration order, after the right hand side is known. A
  // package-level type waits until every package-level type is declared
  // (packageObjects): the walk of one may reach an instance of another that
  // is not expanded yet.
  if (S.ObjMap.count(O))
    WalkQueue.push_back(N);
  else
    later([this, N]() {
      // A local type is walked once it is declared, after every type it can
      // reach: what its walk found silently is cut at once (packageObjects
      // does it for the package-level types when they are all walked).
      size_t From = CycleSeeds.size();
      validType(N);
      flushDeclRings();
      if (CycleSeeds.size() > From) {
        std::vector<NamedType *> Seeds(CycleSeeds.begin() + From,
                                       CycleSeeds.end());
        CycleSeeds.resize(From);
        invalidateByValueCycles(Seeds);
      }
    });
  Scope *Saved = E.Sc;
  if (auto SIt = PendingTParamScope.find(O); SIt != PendingTParamScope.end()) {
    E.Sc = SIt->second; // the type parameters were declared by objDecl
    PendingTParamScope.erase(SIt);
  } else if (TD->TypeParams && TD->TypeParams->numFields()) {
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
  if (auto OIt = S.ObjMap.find(O); OIt != S.ObjMap.end())
    N->AbstractAttr = OIt->second->Abstract; // TYP-19
  N->Resolving = true;
  N->ResolvingInterface = isa<ast::InterfaceType>(TD->Type);
  // TYP-15a: `@Strict` makes the fields of this very struct type required.
  const ast::StructType *SavedStrict = StrictStructExpr;
  StrictStructExpr = nullptr;
  if (Strict && own())
    if (auto *St = dyn_cast<ast::StructType>(unparen(TD->Type)))
      StrictStructExpr = St;
  const ast::StructType *SavedView = ViewStructExpr;
  ViewStructExpr = nullptr;
  if (ViewOk)
    if (auto *St = dyn_cast<ast::StructType>(unparen(TD->Type))) {
      ViewStructExpr = St;
      N->ViewAttr = true;
    }
  const ast::StructType *SavedDecl = DeclStructExpr;
  const ast::Expr *SavedExtends = DeclExtendsExpr;
  StringRef SavedDeclName = DeclStructName;
  DeclStructExpr = dyn_cast<ast::StructType>(unparen(TD->Type));
  DeclExtendsExpr = TD->Extends; // TYP-16
  DeclStructName = O->Name;
  Type *Rhs = definedType(TD->Type, N);
  DeclStructExpr = SavedDecl;
  DeclExtendsExpr = SavedExtends;
  DeclStructName = SavedDeclName;
  ViewStructExpr = SavedView;
  StrictStructExpr = SavedStrict;
  N->Resolving = false;
  N->ResolvingInterface = false;
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
      SmallVector<std::pair<NamedType *, NamedType *>, 4> Links{{N, RN}};
      NamedType *P = RN;
      NamedType *Back = nullptr;
      // Every link once, however long the chain (a length cap would call a
      // long ring valid).
      llvm::SmallPtrSet<const NamedType *, 16> Linked;
      while (P && Linked.insert(P->origin()).second) {
        NamedType *PO = P->origin();
        if (PO == N) {
          Back = PO;
          break;
        }
        Chain.push_back(PO->Obj);
        P = PO->DirectRhs ? dyn_cast<NamedType>(PO->DirectRhs) : nullptr;
        Links.push_back({PO, P});
      }
      if (Back) {
        for (Object *C : Chain)
          if (auto *CN = dyn_cast_or_null<NamedType>(C->Ty))
            ReportedCycle.insert(CN);
        noteDeclRing(Links);
        N->Underlying = invalid();
        N->FromRHS = invalid();
        collectMethods(O);
        return;
      }
    }
    if (RN->Orig && !RN->Expanded) {
      // Expanding RN may declare a type deferred from an alias's right-hand
      // side (InAliasRhs) whose own right-hand side leads back to N: N's is
      // known by now, so that type's ring check above finds the ring through
      // N (`type N0[P any] A[P]; type N1[P any] N0[int]; type A[P any] =
      // N1[P]`), as it does when N1 is declared first.
      NamedType *SavedRhs = N->DirectRhs;
      N->DirectRhs = RN;
      expandNamed(RN);
      N->DirectRhs = SavedRhs;
    }
    if (RN->Orig && !RN->Expanded) {
      // An instance of a generic type that is still being declared
      // (`type Bar[Q any] Foo[Q]` inside Foo's declaration): under() follows
      // DirectRhs until the instance can expand (types2: lazy Named.under).
      N->DirectRhs = RN;
      PendingUnder.push_back({N, RN});
    }
    if (!RN->Orig) {
      // `type A B; type B A`: a cycle of defined types without a type
      // literal in between (types2: Named.under).
      N->DirectRhs = RN;
      SmallVector<Object *, 4> Chain{N->Obj};
      SmallVector<std::pair<NamedType *, NamedType *>, 4> Links{{N, RN}};
      NamedType *P = RN;
      llvm::SmallPtrSet<const NamedType *, 16> Linked;
      // A link may be an instance of a generic type (`type N2 N1[int]` with
      // `type N1[P any] N2`): it leads on through its origin.
      while (P && P->origin() != N && Linked.insert(P->origin()).second) {
        Chain.push_back(P->origin()->Obj);
        NamedType *PO = P->origin();
        P = PO->DirectRhs ? dyn_cast<NamedType>(PO->DirectRhs) : nullptr;
        Links.push_back({PO, P});
      }
      if (P && P->origin() == N && !ReportedCycle.count(N)) {
        for (Object *C : Chain)
          if (auto *CN = dyn_cast_or_null<NamedType>(C->Ty))
            ReportedCycle.insert(CN);
        noteDeclRing(Links);
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
  if (!S.Opts.GoCompat) {
    // TYP-17 to TYP-19: the names, marks and open @Abstract methods are
    // checked by the walk of the hierarchy (Hierarchy.cpp): for the package
    // once its types are declared, for the local types of a body when the
    // body is done. A local type can only extend itself in a ring; it is cut
    // here, before its by-value walk would call it a recursive type.
    const bool Local = !S.ObjMap.count(O);
    bool Extends = false;
    if (auto *St = dyn_cast_or_null<StructType>(N->Underlying))
      Extends = !St->Fields.empty() && St->Fields[0]->IsParent;
    if (Local && Extends) {
      NamedType *Self[] = {N};
      checkExtendsCycles(Self);
    }
    noteHierarchy(N, dyn_cast<ast::StructType>(unparen(TD->Type)),
                  Local && Extends && isValid(N->Underlying));
    // TYP-16: `type X Y` copies the declaration of Y, hidden parent part and
    // all, so X would silently be a child (or a second root with the type
    // table of Y); a child is made only by `type X extends Y {}`.
    if (!TD->Extends && !TD->AssignLoc.isValid())
      if (auto *RN = dyn_cast_or_null<NamedType>(N->FromRHS))
        later([this, N, RN, TD]() {
          auto *St = dyn_cast_or_null<StructType>(RN->Underlying);
          if (!St)
            return;
          std::string Why;
          if (!St->Fields.empty() && St->Fields[0]->IsParent)
            Why = str(RN) + " extends " + str(St->Fields[0]->Ty);
          else if (isAbstractType(RN))
            Why = str(RN) + " is abstract";
          else if (isTagged(RN))
            Why = str(RN) + " has a type table";
          if (!Why.empty())
            err(TD->Type, diag::err_defined_from_hierarchy,
                {std::string(N->Obj->Name), str(RN), Why});
        });
  }
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
    // The operator methods of one name form an overload set (EXP-17): their
    // duplicates are found by the parameter types once every signature is
    // known (checkOperatorSets), and no field can have their names.
    if (MN != "_" && !M->isOperator()) {
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
    Ctx.noteMemberName(M->Name);
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
      // Through a defined type that names another (`type N2[P any] N3`):
      // its own Underlying waits for N3's when N3 was declared after it.
      Type *U = under(BN);
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
                // A method's type parameter is its type's (instantiation
                // cycles, Mono.cpp).
                for (size_t J = 0; J < RTPs.size(); ++J)
                  MonoCanon.push_back({RTPs[J], BN->TParams[J]});
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
  // TYP-19: a value receiver is a value of the type (the method expression
  // `T.M` takes it as a parameter too), so an abstract type has none.
  if (!Ptr && RecvNamed)
    noAbstract(TE, RecvT, "a value receiver");
  Type *VarT =
      (Ptr && isValid(RecvT)) ? static_cast<Type *>(Ctx.pointer(RecvT)) : RecvT;
  recordTypeAndValue(TE, Mode::TypeExpr, VarT, nullptr);
  StringRef RName = RF->Names.empty() ? StringRef("") : name(RF->Names[0]);
  Var *RV =
      newParam(RName, VarT,
               RF->Names.empty() ? RF->getBegin() : RF->Names[0]->getBegin());
  RV->IsParam = true;
  RV->IsRecv = true;
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
  // A use of the function in its own signature sees an empty signature
  // (types2: guard against cycles).
  // (Not for a generic function: its type parameters are collected later.)
  if (!(FD->Type->TypeParams && FD->Type->TypeParams->numFields()))
    O->Ty = Ctx.signature(Ctx.tuple({}), Ctx.tuple({}), false, false, 0, {});
  NamedType *RecvNamed = nullptr;
  if (FD->Recv) {
    collectRecv(O, FD->Recv, Sc, D, FD, RecvNamed);
  }
  // The signature of a method does not instantiate anything for the cycle
  // check in Go (types2: `func (List[P]) m() List[List[P]]` is fine; only a
  // body is instantiated per type argument, go.dev/issue/48703). Outside Go
  // mode it does (GEN-14, owner decision 2026-10-30): `func (G[U, V]) Next()
  // Option[*G[U, U*V]]` and `func (U[T]) m() *U[*T]` would make instances
  // without end, as a field or a body does, so they are the instantiation
  // cycle too.
  const bool SigNoMono = FD->Recv && S.Opts.GoCompat;
  if (SigNoMono)
    ++NoMono;
  else if (FD->Recv)
    ++MonoInSig;
  FuncInfo FI = funcSignature(FD->Type, Sc, O);
  if (SigNoMono)
    --NoMono;
  else if (FD->Recv)
    --MonoInSig;
  E.Sc = Saved;
  // TYP-20: a method of an `extends` type has the hidden `base`, in a scope
  // of its own between the function scope and its parent: the signature
  // does not see it, and `base := 1` in the body declares a new local (the
  // reserved-name error) instead of finding it. Made here, in phase 1, so a
  // body checker never writes the shared scopes.
  if (FD->Body && O->Recv && S.Opts.CheckBodies)
    if (Var *BV = makeBaseVar(O->Recv, FD->Body)) {
      Scope *BaseSc = Mem.newScope(Sc->Parent, Sc->Extent, "base");
      BaseSc->Elems[BV->Name] = BV;
      BV->Parent = BaseSc;
      Sc->Parent = BaseSc;
    }
  O->Ty = FI.Sig;
  O->Params = FI.Params;
  O->Results = FI.Results;
  O->Fails = FI.Sig->Fails;
  O->OwnTParams = FI.Sig->TParams;
  if (FD->Recv && !S.Opts.GoCompat)
    MonoMethodOwn.insert(O->OwnTParams.begin(), O->OwnTParams.end());
  if (own() && FD->Recv) {
    checkMoveRecvSig(O, FD);
    checkSpecialMethod(O, FD);
  }
  if (FD->Recv && !S.Opts.GoCompat)
    checkOperatorShape(O, FD, RecvNamed);
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

/// types2's arity for a constant declaration: every name needs an init
/// expression.
void Checker::constArity(const ValueSpec *VS, const ValueSpec *Src,
                         bool Inherited) {
  size_t NN = VS->Names.size(), NV = Src->Values.size();
  if (NN < NV) {
    if (Inherited)
      err(VS->Names[0], diag::err_extra_init_expr,
          {"at line " +
           std::to_string(S.SM.getLineNumber(Src->Values[NN]->getBegin()))});
    else
      err(Src->Values[NN], diag::err_extra_init_expr, {str(Src->Values[NN])});
  } else if (NN > NV) {
    err(VS->Names[NV], diag::err_missing_init_expr,
        {std::string(name(VS->Names[NV]))});
  }
}

void Checker::declStmt(const DeclStmt *DS) {
  auto *GD = dyn_cast<GenDecl>(DS->D);
  if (!GD)
    return;
  checkDeclMarks(GD); // TYP-18, TYP-19, as for a package-level declaration
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
      size_t DelayedTop = Delayed.size();
      SmallVector<Const *, 4> Cs;
      for (size_t K = 0; K < VS->Names.size(); ++K) {
        const ast::Ident *Id = VS->Names[K];
        Const *C = newConst(name(Id), nullptr, Id->getBegin());
        constDecl(C, Src->Type,
                  K < Src->Values.size() ? Src->Values[K] : nullptr, Inherited,
                  IotaV);
        Cs.push_back(C);
      }
      constArity(VS, Src, Inherited);
      // Function literals in the initializers must not see the new names.
      processDelayed(DelayedTop);
      for (size_t K = 0; K < Cs.size(); ++K)
        declare(E.Sc, VS->Names[K], Cs[K], VS->getEnd());
      ++Iota;
      break;
    }
    case tok::kw_var: {
      auto *VS = cast<ValueSpec>(Sp);
      size_t DelayedTop = Delayed.size();
      SmallVector<Var *, 4> Vars;
      for (const ast::Ident *Id : VS->Names)
        Vars.push_back(newVar(name(Id), nullptr, Id->getBegin()));
      Type *T = VS->Type ? varType(VS->Type, true, "the type of a variable")
                         : nullptr;
      if (T)
        for (Var *V : Vars)
          V->Ty = T;
      if (T && Ctx.unitsSeen() && !S.Opts.GoCompat)
        for (Var *V : Vars)
          noteUnitVarType(V, VS->Type); // its unit as written (UNI-5)
      if (!VS->Values.empty()) {
        if (VS->Values.size() == 1 && VS->Names.size() > 1) {
          // One multi-valued expression.
          initVars(Vars, VS->Values, nullptr);
        } else {
          // n:n, or a count mismatch: each variable takes the value in its
          // position, the others have none (types2: arity reports the
          // mismatch, no assignment-mismatch error on top of it).
          for (size_t K = 0; K < Vars.size(); ++K) {
            if (K >= VS->Values.size()) {
              if (!T)
                Vars[K]->Ty = invalid();
              continue;
            }
            Operand X;
            ReverseDesc = std::string(Vars[K]->Name);
            exprWithHint(X, VS->Values[K], T);
            ReverseDesc.clear();
            initVar(Vars[K], X, "variable declaration");
          }
          size_t NN = VS->Names.size(), NV = VS->Values.size();
          if (NV > NN)
            err(VS->Values[NN], diag::err_extra_init_expr,
                {str(VS->Values[NN])});
          else if (NV < NN)
            err(VS->Names[NV], diag::err_missing_init_expr,
                {std::string(name(VS->Names[NV]))});
        }
      } else if (!T) {
        for (Var *V : Vars)
          V->Ty = invalid();
      } else {
        // TYP-15, OWN-R9: `var u User` creates a zero value.
        for (size_t K = 0; K < Vars.size(); ++K)
          needZeroValue(VS->Names[K]->getBegin(), T,
                        "cannot declare " + std::string(Vars[K]->Name) +
                            " without a value");
      }
      // Function literals in the values are checked before the variables are
      // declared: they cannot see them.
      processDelayed(DelayedTop);
      for (size_t K = 0; K < Vars.size(); ++K) {
        recordDef(VS->Names[K], Vars[K]);
        declare(E.Sc, VS->Names[K], Vars[K], VS->getEnd());
        LocalVars.push_back(Vars[K]);
        // CON-6a: a channel declared with no value, or with nil, is nil.
        if (own() && Vars[K]->Ty && isValid(Vars[K]->Ty) &&
            isa<sema::ChanType>(under(Vars[K]->Ty)) &&
            (VS->Values.empty() || (VS->Values.size() == VS->Names.size() &&
                                    isNilValue(VS->Values[K]))))
          PendingNil.push_back(Vars[K]);
      }
      // A later spec of the same group sees this one's checked results as
      // pending, as after a separate `var` statement (OWN-P10).
      flowFlushKills();
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
      rejectRefAttr(GD);
      typeDecl(TN, TS, nullptr, hasPinnedAttr(GD), false,
               hasDeclAttr(GD, "View"));
      if (auto *LN = dyn_cast_or_null<NamedType>(TN->Ty); LN && !TN->IsAlias)
        LN->AbstractAttr = !S.Opts.GoCompat && TS->Extends &&
                           hasDeclAttr(GD, "Abstract"); // TYP-19
      noteImplicitTParams(TN);
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

/// types2 validtype.go with an explicit stack, so a chain or ring of any
/// length is checked exactly (no depth limit that would call a cycle valid,
/// P03.23). Nest is the stack of types containing (or, for interfaces,
/// embedding) other types; Path is the full path, for errors. NestByOrigin
/// indexes Nest by generic origin, so "does N occur in its own definition"
/// looks at the few entries of N's own origin instead of the whole stack.
///
/// Go stops at the first problem (a cycle, reported, or a type already
/// known to be invalid) and reports nothing more. This walk reports the same
/// thing, but then goes on silently, so every by-value cycle is found and
/// noted (CycleSeeds, ByValueCycle) and none is left without an invalid type
/// behind an earlier error (`type I struct{ i I }` reported, then `E` and `A`
/// holding each other next to an I field): invalidateByValueCycles cuts them
/// all once every walk is done, and no later walk (zero values, categories,
/// clones) goes round one forever. A cycle found silently changes nothing
/// else: its types stay as they are until then, so the walk of another type
/// that meets it first reports it as Go does, whatever the declaration order
/// (`type A struct{ a A; b B[int] }` reports A, then B's own walk reports B).
///
/// A finished walk of a type is remembered (ValidKeys, FailedKeys), by key:
/// the type, and for an instance the type arguments as they are where the
/// walk sees them. A type parameter in an argument stands for what the
/// instance it belongs to was given (the walk resolves it down the nest, as
/// types2 does), so two instances written differently, in different places
/// (`G[P, int]` inside G1 and `G[Q, int]` inside G2), have the same key when
/// the arguments resolve to the same types (what the walk never looks into,
/// a pointer, a slice, a basic type or a free type parameter, counts as one
/// type). The walk of a type is a function of its key alone: what it reaches
/// below it does not depend on anything above, and if it reached something
/// above, that is a cycle through the type, found below it as well. So a
/// finished walk is valid or failed for every place it is asked in, and each
/// key is walked once: n types in a chain or ring, and the shapes that made
/// Go drop its own cache (go.dev/issue/65711), cost n steps in all. An
/// argument is turned into a number without building the resolved type
/// (ArgId), so an argument that grows with the depth costs its written size
/// only. (An argument that grows through a named type, `G[W[P]]` inside
/// `G[P]`, has a new key at each depth: polynomial, not exponential.)
///
/// What a walk reports is what Go's walk reports, whatever was walked before
/// (review R12): a valid key has no problem below it in any context, and a
/// walk that has failed reports nothing more, so both skip freely. A walk
/// that has not failed yet skips a failed key only if no by-value cycle was
/// met below it (the stored flag): then its first problem is an invalid
/// type, silent in any context. With a cycle below, the problem Go meets
/// first may be another one (the key may reach a type on the walk's own
/// path before its own first problem, R12b, R12c), so the key is walked
/// again and its entry stored anew: once the cycle is reported its types
/// are invalid, the new walk meets that first, and the flag is clear from
/// then on, so each cycle costs one more walk of the keys that reach it.
bool Checker::validTypeWalk(Type *Root) {
  // Every right-hand side must be known before a type is walked: an instance
  // whose generic type is still waiting for its underlying type would look
  // invalid (and be cached so), whatever order the declarations came in.
  resolvePendingUnder();
  struct NestEntry {
    NamedType *N;
    SmallVector<unsigned, 4> Args; // the ids of N's type arguments (ArgId)
    unsigned Env;                  // the id of Args
  };
  SmallVector<NestEntry, 16> Nest;
  SmallVector<NamedType *, 16> Path;
  llvm::DenseMap<const NamedType *, SmallVector<NamedType *, 2>> NestByOrigin;
  auto NestPush = [&](NestEntry E) {
    NestByOrigin[E.N->origin()].push_back(E.N);
    Nest.push_back(std::move(E));
  };
  auto NestPop = [&]() {
    NestEntry E = Nest.pop_back_val();
    NestByOrigin[E.N->origin()].pop_back();
    return E;
  };
  enum class FK : uint8_t { Composite, Named, Param };
  struct Frame {
    Type *T;
    NamedType *N; // Named: the type; Param: the instance it was seen inside
    Type *Child;  // Named / Param: the one child
    size_t Next;
    FK Kind;
    unsigned Key;            // Named: the key of the walk
    unsigned ProblemsBefore; // Problems when the frame started
    NestEntry Saved;         // Param: the instance taken off the nest
    bool CycleBelow = false; // a by-value cycle was met below this frame
  };
  SmallVector<Frame, 32> St;
  // Problems met so far (a frame whose subtree met one is not valid), and
  // whether the walk has failed (then nothing more is reported).
  unsigned Problems = 0;
  bool Failed = false;
  auto Problem = [&]() {
    ++Problems;
    Failed = true;
  };

  // Ids: equal ids, equal types (where the walk sees them).
  auto Intern = [&](ArrayRef<uint64_t> K) -> unsigned {
    auto [It, New] = WalkIds.try_emplace(
        std::vector<uint64_t>(K.begin(), K.end()), WalkIds.size() + 1);
    return It->second;
  };
  enum : uint64_t {
    TagLeaf = 1,
    TagInert,
    TagComp,
    TagInst,
    TagOpaque,
    TagEnv,
    TagKey
  };
  // Does the type mention a type parameter? (Cached: types are uniqued.)
  auto MentionsParam = [&](Type *Root) {
    SmallVector<Type *, 8> W{Root};
    llvm::SmallPtrSet<const Type *, 8> Met;
    while (!W.empty()) {
      Type *X = W.pop_back_val();
      if (!X || !Met.insert(X).second)
        continue;
      switch (X->getKind()) {
      case Type::Kind::TypeParam:
        return true;
      case Type::Kind::Pointer:
        W.push_back(cast<PointerType>(X)->Elem);
        break;
      case Type::Kind::Slice:
        W.push_back(cast<SliceType>(X)->Elem);
        break;
      case Type::Kind::Ref:
        W.push_back(cast<RefType>(X)->Elem);
        break;
      case Type::Kind::Option:
        W.push_back(cast<OptionType>(X)->Elem);
        break;
      case Type::Kind::Array:
        W.push_back(cast<sema::ArrayType>(X)->Elem);
        break;
      case Type::Kind::Chan:
        W.push_back(cast<sema::ChanType>(X)->Elem);
        break;
      case Type::Kind::Map:
        W.push_back(cast<sema::MapType>(X)->Key);
        W.push_back(cast<sema::MapType>(X)->Elem);
        break;
      case Type::Kind::Tuple:
        for (Type *E : cast<TupleType>(X)->Elems)
          W.push_back(E);
        break;
      case Type::Kind::Signature:
        W.push_back(cast<SignatureType>(X)->Params);
        W.push_back(cast<SignatureType>(X)->Results);
        break;
      case Type::Kind::Struct:
        for (Var *F : cast<StructType>(X)->Fields)
          W.push_back(F->Ty);
        break;
      case Type::Kind::Union:
        for (const Term &Tm : cast<UnionType>(X)->Terms)
          W.push_back(Tm.T);
        break;
      case Type::Kind::Interface:
        for (Func *M : cast<InterfaceType>(X)->Methods)
          W.push_back(M->Ty);
        for (Type *E : cast<InterfaceType>(X)->Embeddeds)
          W.push_back(E);
        break;
      case Type::Kind::Named:
        for (Type *A : cast<NamedType>(X)->TArgs)
          W.push_back(A);
        break;
      default:
        break;
      }
    }
    return false;
  };
  // The id of the type argument A as written where the walk now is: a type
  // parameter of the enclosing instance is the id of the argument that
  // instance was given. What the walk does with an argument depends only on
  // the types it holds by value (arrays, Options, structs, named types):
  // pointers, slices, maps, channels, funcs and the basic types are never
  // walked into, and a type parameter nothing gives a type to stands for
  // nothing. All of those have one id, Inert, so `G[P, int]`, `G[int, Q]`,
  // `G[[]P, *Q]` and `G[[1]P, string]` are the same walk. Iterative, each
  // type once.
  const unsigned Inert = Intern({TagInert});
  auto ArgId = [&](Type *Root) -> unsigned {
    const NestEntry *Inst = Nest.empty() ? nullptr : &Nest.back();
    llvm::DenseMap<const Type *, unsigned> Ids;
    SmallVector<std::pair<Type *, bool>, 16> W{{Root, false}};
    // The parts of X an id is made of, and what kind of id it is.
    auto Parts = [&](Type *X, llvm::SmallVectorImpl<Type *> &Out,
                     llvm::SmallVectorImpl<uint64_t> &Head) {
      Out.clear();
      switch (X->getKind()) {
      case Type::Kind::Array:
        Out.push_back(cast<sema::ArrayType>(X)->Elem);
        Head.assign(
            {TagComp, 1, static_cast<uint64_t>(cast<sema::ArrayType>(X)->Len)});
        return true;
      case Type::Kind::Option:
        Out.push_back(cast<OptionType>(X)->Elem);
        Head.assign({TagComp, 2, 0});
        return true;
      case Type::Kind::Named: {
        auto *N = cast<NamedType>(X);
        if (!N->Orig)
          return false;
        Head.assign({TagInst, reinterpret_cast<uint64_t>(N->Orig)});
        Out.assign(N->TArgs.begin(), N->TArgs.end());
        return true;
      }
      default:
        return false;
      }
    };
    while (!W.empty()) {
      auto [X, Ready] = W.pop_back_val();
      if (!X || Ids.count(X))
        continue;
      SmallVector<Type *, 4> Ks;
      SmallVector<uint64_t, 8> Key;
      switch (X->getKind()) {
      case Type::Kind::TypeParam: {
        unsigned Id = Inert;
        if (Inst) {
          ArrayRef<TypeParamType *> TPs = Inst->N->origin()->TParams;
          for (size_t I = 0; I < TPs.size(); ++I)
            if (TPs[I] == X && I < Inst->Args.size()) {
              Id = Inst->Args[I];
              break;
            }
        }
        Ids[X] = Id;
        continue;
      }
      case Type::Kind::Array:
      case Type::Kind::Option:
      case Type::Kind::Named:
        break;
      case Type::Kind::Struct:
      case Type::Kind::Union:
      case Type::Kind::Interface:
        // Walked into: itself, and where it is seen when it mentions a type
        // parameter.
        Ids[X] = MentionsParam(X)
                     ? Intern({TagOpaque, reinterpret_cast<uint64_t>(X),
                               Inst ? Inst->Env : 0})
                     : Intern({TagLeaf, reinterpret_cast<uint64_t>(X)});
        continue;
      default:
        Ids[X] = Inert;
        continue;
      }
      if (!Parts(X, Ks, Key)) {
        Ids[X] = Intern({TagLeaf, reinterpret_cast<uint64_t>(X)}); // plain
        continue;
      }
      if (!Ready) {
        W.push_back({X, true});
        for (Type *K : Ks)
          W.push_back({K, false});
        continue;
      }
      bool AllInert = X->getKind() != Type::Kind::Named;
      for (Type *K : Ks) {
        unsigned Id = Ids.lookup(K);
        AllInert = AllInert && Id == Inert;
        Key.push_back(Id);
      }
      Ids[X] = AllInert ? Inert : Intern(Key);
    }
    return Ids.lookup(Root);
  };

  // 0: a frame was pushed, 1: done (a problem, if any, is counted).
  auto Enter = [&](Type *T) -> int {
    if (!T)
      return 1;
    switch (T->getKind()) {
    case Type::Kind::Array:
    case Type::Kind::Option:
    case Type::Kind::Struct:
    case Type::Kind::Union:
    case Type::Kind::Interface:
      // An Option holds its value (plus a flag): `next Option[L]` in L is a
      // cycle, `next Option[*L]` is not (ADR-0018).
      St.push_back({T, nullptr, nullptr, 0, FK::Composite, 0, Problems, {}});
      return 0;
    case Type::Kind::Named: {
      auto *N = cast<NamedType>(T);
      NamedType *O = N->origin();
      // A ring of right-hand sides found at its declaration is reported by
      // the first walk that meets it, as Go's walk does (R22).
      if (!Failed && !PendingRings.empty())
        reportDeclRing(N);
      // Don't report a 2nd error for a type known to be invalid.
      if (!isValid(under(N))) {
        Problem();
        return 1;
      }
      // The key: the type, and for an instance (or a generic type walked for
      // itself) its type arguments.
      NestEntry Entry{N, {}, 0};
      SmallVector<uint64_t, 8> KeyOps{TagKey, reinterpret_cast<uint64_t>(O)};
      if (N->Orig || !N->TParams.empty()) {
        if (N->Orig)
          for (Type *A : N->TArgs)
            Entry.Args.push_back(ArgId(A));
        else
          Entry.Args.assign(N->TParams.size(), Inert);
        for (unsigned A : Entry.Args)
          KeyOps.push_back(A);
        SmallVector<uint64_t, 8> EnvOps{TagEnv};
        EnvOps.append(Entry.Args.begin(), Entry.Args.end());
        Entry.Env = Intern(EnvOps);
      }
      unsigned Key = Intern(KeyOps);
      if (ValidKeys.count(Key))
        return 1;
      if (auto FIt = FailedKeys.find(Key);
          FIt != FailedKeys.end() && (Failed || !FIt->second)) {
        if (FIt->second && !St.empty())
          St.back().CycleBelow = true;
        Problem();
        return 1;
      }
      auto It = NestByOrigin.find(O);
      if (It != NestByOrigin.end())
        for (NamedType *Ne : It->second) {
          if (!identical(Ne, N))
            continue;
          // N occurs inside its own definition (after expansion): invalid,
          // no matter how it is instantiated. Found silently, it is left to
          // invalidateByValueCycles (see above).
          if (!Failed) {
            N->Underlying = invalid();
            O->Underlying = invalid();
          }
          ByValueCycle = true;
          if (!St.empty())
            St.back().CycleBelow = true;
          for (size_t Start = 0; Start < Path.size(); ++Start)
            if (identical(Path[Start], N)) {
              SmallVector<Object *, 4> Cycle;
              for (size_t J = Start; J < Path.size(); ++J) {
                Cycle.push_back(Path[J]->Obj);
                CycleSeeds.push_back(Path[J]); // see invalidateByValueCycles
              }
              if (!Failed)
                cycleError(Cycle, 0);
              break;
            }
          Problem();
          return 1;
        }
      NestPush(std::move(Entry));
      Path.push_back(N);
      St.push_back({T, N, O->FromRHS, 0, FK::Named, Key, Problems, {}});
      return 0;
    }
    case Type::Kind::TypeParam: {
      // A type parameter stands for the type argument it was instantiated
      // with.
      if (Nest.empty())
        return 1;
      NamedType *Inst = Nest.back().N;
      ArrayRef<TypeParamType *> TPs = Inst->origin()->TParams;
      for (size_t I = 0; I < TPs.size(); ++I)
        if (TPs[I] == T && I < Inst->TArgs.size()) {
          // The argument must be valid where Inst was instantiated.
          Frame F{T, Inst, Inst->TArgs[I], 0, FK::Param, 0, Problems, {}};
          F.Saved = NestPop();
          St.push_back(std::move(F));
          return 0;
        }
      return 1;
    }
    default:
      return 1;
    }
  };
  // The Next-th child of a frame, or false when there is none left.
  auto NextChild = [&](Frame &F, Type *&Out) -> bool {
    switch (F.Kind) {
    case FK::Named:
    case FK::Param:
      if (F.Next++)
        return false;
      Out = F.Child;
      return true;
    case FK::Composite:
      break;
    }
    size_t K = F.Next++;
    switch (F.T->getKind()) {
    case Type::Kind::Array:
    case Type::Kind::Option:
      if (K)
        return false;
      Out = F.T->getKind() == Type::Kind::Array
                ? cast<sema::ArrayType>(F.T)->Elem
                : cast<OptionType>(F.T)->Elem;
      return true;
    case Type::Kind::Struct: {
      auto &Fs = cast<StructType>(F.T)->Fields;
      if (K >= Fs.size())
        return false;
      Out = Fs[K]->Ty;
      return true;
    }
    case Type::Kind::Union: {
      auto &Ts = cast<UnionType>(F.T)->Terms;
      if (K >= Ts.size())
        return false;
      Out = Ts[K].T;
      return true;
    }
    case Type::Kind::Interface: {
      auto &Es = cast<InterfaceType>(F.T)->Embeddeds;
      if (K >= Es.size())
        return false;
      Out = Es[K];
      return true;
    }
    default:
      return false;
    }
  };
  if (Enter(Root) != 0)
    return !Failed;
  while (!St.empty()) {
    Type *Child = nullptr;
    if (!NextChild(St.back(), Child)) {
      Frame Done = std::move(St.back());
      St.pop_back();
      if (Done.CycleBelow && !St.empty())
        St.back().CycleBelow = true;
      if (Done.Kind == FK::Named) {
        NestPop();
        Path.pop_back();
        if (Done.ProblemsBefore == Problems)
          ValidKeys.insert(Done.Key);
        else
          FailedKeys[Done.Key] = Done.CycleBelow;
      } else if (Done.Kind == FK::Param) {
        NestPush(std::move(Done.Saved));
      }
      continue;
    }
    Enter(Child);
  }
  return !Failed;
}

void Checker::validType(NamedType *T) { validTypeWalk(T); }

/// A ring of right-hand sides (`type A B; type B A`, or through instances,
/// `type N0[P any] N1; type N1 N0[int]`) is found when its last type is
/// declared, which depends on the declaration order. Go reports it where a
/// walk of the types first meets it (types2 validtype.go, or Named.under):
/// the cycle starts at the first type that comes round again. So it is only
/// noted here, with the right-hand side each type leads to; the types are
/// invalid at once (nothing follows the ring), and the walk reports it
/// (reportDeclRing), so the name does not depend on the order (R22).
void Checker::noteDeclRing(
    ArrayRef<std::pair<NamedType *, NamedType *>> Links) {
  unsigned Id = DeclRings.size();
  DeclRings.emplace_back();
  for (auto [From, To] : Links) {
    RingNext[From] = To;
    RingOf[From] = Id;
    DeclRings.back().push_back(From);
  }
  PendingRings.insert(Id);
  ByValueCycle = true;
}

/// Reports the ring the walk meets at N (if N's type is on one not reported
/// yet): from N along the right-hand sides to the first type that comes
/// round again, as Go's walk names it.
void Checker::reportDeclRing(NamedType *N) {
  auto It = RingOf.find(N->origin());
  if (It == RingOf.end() || !PendingRings.count(It->second))
    return;
  PendingRings.erase(It->second);
  // Each type with where it is written: an instance at the right-hand side
  // that names it (an origin at its own declaration).
  SmallVector<NamedType *, 8> Seq{N};
  SmallVector<SourceLoc, 8> Where{SourceLoc()};
  for (NamedType *Prev = N, *Cur = RingNext.lookup(N->origin()); Cur;
       Prev = Cur, Cur = RingNext.lookup(Cur->origin())) {
    auto Again = llvm::find_if(
        Seq, [&](NamedType *X) { return X == Cur || identical(X, Cur); });
    if (Again != Seq.end()) {
      SmallVector<Object *, 8> Cycle;
      SmallVector<SourceLoc, 8> At;
      for (auto J = Again; J != Seq.end(); ++J) {
        Cycle.push_back((*J)->Obj);
        At.push_back(Where[J - Seq.begin()]);
      }
      cycleError(Cycle, 0, At);
      return;
    }
    if (Seq.size() > RingNext.size()) // not a ring after all: cannot happen
      break;
    SourceLoc W;
    if (Cur->Orig)
      if (DeclInfo *D = S.ObjMap.lookup(Prev->origin()->Obj);
          D && D->TDecl && D->TDecl->Type)
        W = D->TDecl->Type->getBegin();
    Seq.push_back(Cur);
    Where.push_back(W);
  }
  SmallVector<Object *, 8> Cycle;
  for (NamedType *X : DeclRings[It->second])
    Cycle.push_back(X->Obj);
  cycleError(Cycle);
}

/// The rings no walk met (a type only reached in a body, say): reported in
/// the order they were found, each at its first type in source order.
void Checker::flushDeclRings() {
  for (unsigned Id = 0; Id < DeclRings.size(); ++Id) {
    if (!PendingRings.count(Id))
      continue;
    PendingRings.erase(Id);
    SmallVector<Object *, 8> Cycle;
    for (NamedType *X : DeclRings[Id])
      Cycle.push_back(X->Obj);
    cycleError(Cycle);
  }
}

/// Gives the types typeDecl left waiting (`type Bar[Q any] Foo[Q]` inside
/// Foo's declaration) their underlying types, in declaration order, once the
/// declarations they wait for are done.
void Checker::resolvePendingUnder() {
  for (size_t K = 0; K < PendingUnder.size(); ++K) {
    auto [N, RN] = PendingUnder[K];
    if (!N->Underlying)
      N->Underlying = under(RN);
  }
  PendingUnder.clear();
}

/// After a by-value cycle was reported: makes every type on a by-value cycle
/// of the types reachable from Roots invalid. The walk (validTypeWalk) finds
/// each cycle but marks only the types it passes through, in the shape of the
/// declarations; an instance expanded before keeps its own copy of the
/// fields, so a cycle can survive among instances (`type N0[P any]
/// N1[...]` with `N1` holding `N0[int]`), and then anything that follows the
/// fields of a type (zero values, clones, categories) would never end. Here
/// the cycles of the real graph are found (strongly connected components,
/// iteratively, each type once) and no cycle is left without an invalid type.
/// The graph has an edge from each instance to its generic declaration as
/// well: queries answer an instance from the declaration, so a cycle through
/// a declaration (it holds, deep down, an instance of itself) must be cut
/// too.
void Checker::invalidateByValueCycles(ArrayRef<NamedType *> Roots) {
  // The named types reachable from Roots through anything (fields of any
  // kind, type arguments), in discovery order. Types made invalid are not
  // opened, nor an instance of a generic type on an instantiation cycle
  // (onInstCycle) that is not expanded yet: its instances grow without end,
  // so the walk expands none of them (one expanded already holds a copy of
  // its fields, which may close a cycle, so it is opened). Every other
  // instance graph is finite (types2 mono.go), so the walk ends.
  std::vector<NamedType *> Nodes;
  llvm::DenseMap<const NamedType *, unsigned> Index;
  llvm::SmallPtrSet<const Type *, 32> Seen;
  SmallVector<Type *, 32> Work;
  // The named types held by value by T's underlying type: through arrays,
  // options and struct fields, not through a named type.
  auto ByValueKids = [&](NamedType *N,
                         llvm::SmallVectorImpl<NamedType *> &Out) {
    if (N->Ptr != PtrKind::None || (onInstCycle(N) && !N->Expanded) ||
        (N->Obj && N->Obj->Pkg && N->Obj->Pkg->Foreign))
      return;
    if (N->Orig && !N->TArgs.empty() && !N->Expanded &&
        llvm::all_of(N->TArgs, [](Type *A) { return isa<TypeParamType>(A); }))
      return; // not opened above: see the reachability loop
    SmallVector<Type *, 16> W{under(N)};
    llvm::SmallPtrSet<const Type *, 16> Local;
    while (!W.empty()) {
      Type *X = W.pop_back_val();
      if (!X || !Local.insert(X).second)
        continue;
      switch (X->getKind()) {
      case Type::Kind::Named:
        Out.push_back(cast<NamedType>(X));
        break;
      case Type::Kind::Array:
        W.push_back(cast<sema::ArrayType>(X)->Elem);
        break;
      case Type::Kind::Option:
        W.push_back(cast<OptionType>(X)->Elem);
        break;
      case Type::Kind::Struct:
        for (Var *F : cast<StructType>(X)->Fields)
          W.push_back(F->Ty);
        break;
      default:
        break;
      }
    }
  };
  for (NamedType *R : Roots)
    Work.push_back(R);
  while (!Work.empty()) {
    Type *X = Work.pop_back_val();
    if (!X || !Seen.insert(X).second)
      continue;
    switch (X->getKind()) {
    case Type::Kind::Named: {
      auto *N = cast<NamedType>(X);
      Index.try_emplace(N, Nodes.size());
      Nodes.push_back(N);
      for (Type *A : N->TArgs)
        Work.push_back(A);
      if (N->Orig)
        Work.push_back(N->Orig);
      // An instance whose arguments are all bare type parameters (`G1[T]`
      // in the declaration of `G0[T]`) holds nothing a parameter could
      // change: a parameter is a leaf. Unless it was expanded already (its
      // copy of the fields may be stale), its edge to its generic
      // declaration says all there is, and not opening it keeps a ring of n
      // generic types from making n^2 such instances.
      bool Opaque = N->Orig && !N->TArgs.empty() && !N->Expanded;
      for (Type *A : N->TArgs)
        Opaque = Opaque && isa<TypeParamType>(A);
      if (!Opaque && !(onInstCycle(N) && !N->Expanded) && isValid(under(N)))
        Work.push_back(under(N));
      break;
    }
    case Type::Kind::Pointer:
      Work.push_back(cast<PointerType>(X)->Elem);
      break;
    case Type::Kind::Slice:
      Work.push_back(cast<SliceType>(X)->Elem);
      break;
    case Type::Kind::Ref:
      Work.push_back(cast<RefType>(X)->Elem);
      break;
    case Type::Kind::Option:
      Work.push_back(cast<OptionType>(X)->Elem);
      break;
    case Type::Kind::Array:
      Work.push_back(cast<sema::ArrayType>(X)->Elem);
      break;
    case Type::Kind::Chan:
      Work.push_back(cast<sema::ChanType>(X)->Elem);
      break;
    case Type::Kind::Map:
      Work.push_back(cast<sema::MapType>(X)->Key);
      Work.push_back(cast<sema::MapType>(X)->Elem);
      break;
    case Type::Kind::Struct:
      for (Var *F : cast<StructType>(X)->Fields)
        Work.push_back(F->Ty);
      break;
    default:
      break;
    }
  }
  // Tarjan over the by-value edges.
  size_t NN = Nodes.size();
  std::vector<std::vector<unsigned>> Adj(NN);
  for (size_t V = 0; V < NN; ++V) {
    SmallVector<NamedType *, 8> Kids;
    ByValueKids(Nodes[V], Kids);
    // An instance rests on its generic declaration too (zero values and
    // categories of an instance are answered from it).
    if (Nodes[V]->Orig)
      Kids.push_back(Nodes[V]->Orig);
    for (NamedType *K : Kids)
      if (auto It = Index.find(K); It != Index.end())
        Adj[V].push_back(It->second);
  }
  constexpr unsigned None = ~0u;
  std::vector<unsigned> Idx(NN, None), Low(NN, 0), Next(NN, 0), Stk, Path;
  std::vector<bool> OnStk(NN, false);
  unsigned Counter = 0;
  std::vector<unsigned> Bad;
  for (unsigned R = 0; R < NN; ++R) {
    if (Idx[R] != None)
      continue;
    Idx[R] = Low[R] = Counter++;
    Stk.push_back(R);
    OnStk[R] = true;
    Path.push_back(R);
    while (!Path.empty()) {
      unsigned V = Path.back();
      if (Next[V] < Adj[V].size()) {
        unsigned To = Adj[V][Next[V]++];
        if (Idx[To] == None) {
          Idx[To] = Low[To] = Counter++;
          Stk.push_back(To);
          OnStk[To] = true;
          Path.push_back(To);
        } else if (OnStk[To]) {
          Low[V] = std::min(Low[V], Idx[To]);
        }
        continue;
      }
      Path.pop_back();
      if (!Path.empty())
        Low[Path.back()] = std::min(Low[Path.back()], Low[V]);
      if (Low[V] != Idx[V])
        continue;
      size_t Start = Stk.size();
      unsigned W;
      do {
        W = Stk[--Start];
        OnStk[W] = false;
      } while (W != V);
      bool Loop = Stk.size() - Start > 1 || llvm::is_contained(Adj[V], V);
      if (Loop)
        Bad.insert(Bad.end(), Stk.begin() + Start, Stk.end());
      Stk.resize(Start);
    }
  }
  for (unsigned V : Bad) {
    NamedType *N = Nodes[V];
    N->Underlying = invalid();
    if (N->Orig)
      N->Expanded.store(true, std::memory_order_release);
  }
}
