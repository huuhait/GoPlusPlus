//===- Check.cpp - Checker driver, recording and the public entry points --===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>

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
  if (ErrPos.isValid() && D.Sev == Severity::Error)
    D.Loc = ErrPos;
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
  // TYP-20: `base` is the parent part in a method of an `extends` type.
  if (!BaseMethodOf.empty() && Obj->Name == "base" && isa<Var>(Obj))
    err(Id ? Id->getBegin() : Obj->Pos, diag::err_base_reserved,
        {std::string(BaseMethodOf), cast<Var>(Obj)->IsParam  ? "parameter"
                                    : cast<Var>(Obj)->Result ? "result"
                                                             : "variable"});
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
        std::string Quoted = "\"" + std::string(PN->Imported->Path) + "\"";
        if (PN->Name != Elem && PN->Name != ".")
          err(PN->ImportLoc, diag::err_unused_import_as,
              {Quoted, std::string(PN->Name)});
        else
          err(PN->ImportLoc, diag::err_unused_import, {Quoted});
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
  // Ask every package-level type once, so the "has a zero value" cache that
  // serialization writes is filled the same way whatever the bodies ask later
  // (it is a function of the type alone, see ZeroQuery).
  for (auto &KV : S.Pkg->PkgScope->Elems)
    if (auto *TN = dyn_cast<TypeName>(KV.second))
      if (auto *N = llvm::dyn_cast_or_null<NamedType>(TN->Ty);
          N && !TN->IsAlias) {
        if (N->TParams.empty())
          hasZeroValue(N, nullptr);
        isAbstractType(N); // the abstract bit is exported (TYP-19, P03.11)
      }
  recordUntyped();
}

//===----------------------------------------------------------------------===//
// Info
//===----------------------------------------------------------------------===//

void Info::merge(ArrayRef<Info *> Others) {
  // Each map is reserved once for all entries: copying a big table into a
  // smaller one that grows on the way is several times slower (the source is
  // walked in hash order, so the entries pile up in the growing table).
  auto One = [&](auto Member) {
    auto &To = this->*Member;
    size_t N = To.size();
    for (const Info *O : Others)
      N += (O->*Member).size();
    To.reserve(N);
    for (const Info *O : Others)
      for (auto &KV : O->*Member)
        To[KV.first] = KV.second;
  };
  One(&Info::Types);
  One(&Info::Defs);
  One(&Info::Uses);
  One(&Info::Implicits);
  One(&Info::Selections);
  One(&Info::Instances);
  One(&Info::Bangs);
}

//===----------------------------------------------------------------------===//
// checkPackage
//===----------------------------------------------------------------------===//

namespace {
/// One thread of phase 2. The bodies it checks share its arena, its side
/// table and its set of used imports: a body records entries only for its own
/// AST nodes, so the tables of the workers never overlap and are merged after
/// the bodies. With one thread the worker writes into the package's table.
struct BodyWorker {
  std::unique_ptr<Arena> Mem = std::make_unique<Arena>();
  Info Table;
  Info *Out = &Table;
  llvm::SmallPtrSet<PkgName *, 8> Used;
};
} // namespace

/// The number of threads phase 2 uses for N bodies.
static unsigned bodyThreads(const CheckOptions &O, size_t N) {
  unsigned Threads = O.BodyThreads;
  if (!Threads)
    Threads = std::min(llvm::hardware_concurrency().compute_thread_count(),
                       CheckOptions::MaxAutoBodyThreads);
  // A thread must have enough work to pay for itself.
  size_t PerThread = std::max<size_t>(1, O.MinBodiesPerThread);
  return static_cast<unsigned>(std::max<size_t>(
      1, std::min<size_t>(Threads, (N + PerThread - 1) / PerThread)));
}

/// Checks every body; Diags[K] gets the diagnostics of S.Bodies[K]. Workers
/// take the next body from a shared counter: the order of the bodies in time
/// is free, their diagnostics go to their own slots.
static void runBodies(PackageState &S, const SourceManager &SM,
                      std::vector<BodyWorker> &Workers,
                      std::vector<std::vector<Diagnostic>> &Diags,
                      std::vector<std::vector<Checker::MonoInst>> &Monos,
                      std::vector<std::vector<Checker::UntaggedPending>> &Pend,
                      std::vector<std::vector<Checker::UseRec>> &Uses) {
  size_t N = S.Bodies.size();
  std::atomic<size_t> Next{0};
  auto Run = [&](BodyWorker &W) {
    for (size_t K = Next.fetch_add(1, std::memory_order_relaxed); K < N;
         K = Next.fetch_add(1, std::memory_order_relaxed)) {
      CollectingDiagnosticConsumer Collect;
      DiagnosticsEngine BDiags(SM, Collect, 0);
      Checker BC(S, *W.Out, *W.Mem, BDiags);
      BC.Bodies = true;
      BC.UsedImports = &W.Used;
      BC.checkBody(S.Bodies[K]);
      BC.recordUntyped();
      Diags[K] = std::move(Collect.Diags);
      Monos[K] = std::move(BC.MonoInsts);
      Pend[K] = std::move(BC.UntaggedPend);
      Uses[K] = std::move(BC.TPUses);
    }
  };
  if (Workers.size() == 1) {
    Run(Workers[0]);
    return;
  }
  if (llvm::ThreadPoolInterface *Pool = S.Opts.Pool) {
    llvm::ThreadPoolTaskGroup Group(*Pool);
    for (BodyWorker &W : Workers)
      Group.async([&Run, &W] { Run(W); });
    Group.wait();
    return;
  }
  llvm::DefaultThreadPool Pool(llvm::hardware_concurrency(Workers.size()));
  for (BodyWorker &W : Workers)
    Pool.async([&Run, &W] { Run(W); });
  Pool.wait();
}

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

  // Phase 2: one checker per body, on one or more threads. A body writes only
  // to its worker; the diagnostics are replayed in declaration order, so the
  // output does not depend on the order or the thread the bodies ran on.
  CP->NumBodies = S.Bodies.size();
  std::vector<BodyWorker> Workers(bodyThreads(Opts, S.Bodies.size()));
  if (Workers.size() == 1)
    Workers[0].Out = &CP->I;
  std::vector<std::vector<Diagnostic>> BodyDiags(S.Bodies.size());
  std::vector<std::vector<Checker::MonoInst>> BodyMonos(S.Bodies.size());
  std::vector<std::vector<Checker::UntaggedPending>> BodyPend(S.Bodies.size());
  std::vector<std::vector<Checker::UseRec>> BodyUses(S.Bodies.size());
  runBodies(S, SM, Workers, BodyDiags, BodyMonos, BodyPend, BodyUses);
  for (std::vector<Diagnostic> &DL : BodyDiags)
    for (Diagnostic &D : DL) {
      if (D.Sev == Severity::Error) {
        // Earlier bodies may have reported the first error.
        if (C.NumErrors > 0 && isFollowOn(D))
          continue;
        ++C.NumErrors;
      }
      Diags.emit(std::move(D));
    }
  SmallVector<Info *, 8> Tables;
  for (BodyWorker &W : Workers) {
    for (PkgName *PN : W.Used)
      PN->Used = true;
    if (W.Out != &CP->I)
      Tables.push_back(&W.Table);
    CP->Mem.push_back(std::move(W.Mem));
  }
  CP->I.merge(Tables);
  C.FirstErr = C.NumErrors > 0;
  // GEN-12, EXP-15: the bodies are all checked, so the uses of this
  // package's type parameters are known.
  for (std::vector<Checker::UntaggedPending> &PB : BodyPend)
    C.UntaggedPend.insert(C.UntaggedPend.end(), PB.begin(), PB.end());
  for (std::vector<Checker::UseRec> &UB : BodyUses)
    C.TPUses.insert(C.TPUses.end(), UB.begin(), UB.end());
  C.noteIntrinsicUses();
  C.untaggedCheck(C.UntaggedPend);
  C.checkInitOrder();
  C.unusedImports();
  // Instantiation cycles, of the declarations and the bodies (taken in
  // declaration order): reported, as by Go, only when the package has no
  // other error (types2 check.go runs monomorph last, if firstErr is nil).
  if (C.NumErrors == 0) {
    for (std::vector<Checker::MonoInst> &MB : BodyMonos)
      for (Checker::MonoInst &M : MB)
        C.MonoInsts.push_back(std::move(M));
    C.monoCheck(C.MonoInsts, /*Report=*/true);
  }
  Pkg->Complete = true;
  if (Opts.Vet)
    vetPackage(SM, *CP, Files, Diags);
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
  for (auto &KV : P.I.Bangs) {
    const char *Kind = KV.second == BangKind::Return  ? "returns the error"
                       : KV.second == BangKind::Panic ? "panics"
                                                      : "sets the error";
    const ast::CallExpr *C = KV.first;
    Entries.push_back({C->BangLoc.getRaw(), C->BangLoc.getRaw(), 2,
                       Loc(C->BangLoc) + " bang `" + Text(C) + "` " + Kind});
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
