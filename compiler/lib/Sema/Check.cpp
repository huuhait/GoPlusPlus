//===- Check.cpp - Checker driver, recording and the public entry points --===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Checker.h"
#include "Units.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>

#include <pthread.h>

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
    : S(S), Ctx(S.Ctx), I(I), Mem(Mem), Diags(Diags) {
  BodyUnitNames.Parent = &S.UNamer;
}

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
  One(&Info::Operators);
  One(&Info::UnitConvs);
  One(&Info::UnitSites);
  One(&Info::UnitDisplays);
  for (const Info *O : Others)
    Reslices.insert(O->Reslices.begin(), O->Reslices.end());
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

/// The checker recurses once per level of a nested expression or statement.
/// The parser lets a declaration be 1024 levels tall, and a level costs up to
/// 16.3 KB of stack in a sanitizer build (measured over every expression
/// kind at the parser's limit, the smallest stack that passes: nested
/// built-ins `unsafe.Sizeof(…)`, `clone(…)`, `move(…)` need 16191 KB at 996
/// levels; calls, inference, method calls and `()` methods 13.2 KB,
/// composite literals 14.3 KB, conversions 12.0 KB, closures 9.3 KB, `[]`
/// methods 8.5 KB, a postfix chain 0.1 KB, as it is a loop; R13), so every
/// piece of checking runs on a thread with a stack of 16.3 KB x 1024 x 2 = 32.5
/// MiB, rounded up: phase 1, each worker of phase 2 (also on a lent pool) and
/// the checks after the bodies (P03.26). The answer never depends on the thread
/// that runs the work. Only the pages that are touched use memory. A thread
/// that cannot be made (address space limit) is never an abort: the work runs
/// on fewer threads, or on the calling thread.
static constexpr size_t CheckStackBytes = 40u << 20;

/// Is this thread a checker thread (a CheckerStack, or a phase-2 worker)? A
/// job that needs a checker stack while it runs on one runs right there: a
/// nested import chain does not make a thread per level (P03.26, R4).
static thread_local bool OnCheckerThread = false;

namespace {
/// A thread with a checker stack. Creating it can fail; then Ok is false and
/// the caller does the work itself.
class BigThread {
public:
  BigThread() = default;
  BigThread(const BigThread &) = delete;
  bool start(std::function<void()> Fn, size_t Bytes = CheckStackBytes) {
    Body = std::move(Fn);
    pthread_attr_t Attr;
    if (pthread_attr_init(&Attr) != 0)
      return false;
    bool Ok = pthread_attr_setstacksize(&Attr, Bytes) == 0 &&
              pthread_create(&Id, &Attr, &BigThread::entry, this) == 0;
    pthread_attr_destroy(&Attr);
    Started = Ok;
    return Ok;
  }
  void join() {
    if (Started)
      pthread_join(Id, nullptr);
    Started = false;
  }
  ~BigThread() { join(); }

private:
  static void *entry(void *Self) {
    OnCheckerThread = true;
    static_cast<BigThread *>(Self)->Body();
    return nullptr;
  }
  pthread_t Id{};
  bool Started = false;
  std::function<void()> Body;
};

/// A thread with a checker stack that runs the jobs of the one thread that
/// owns it, one at a time, while the owner waits. Each calling thread keeps
/// its own for its lifetime: a new thread per package would start cold (its
/// stack pages and its allocator) at every call. Without the thread (it could
/// not be made) a job runs on the caller.
class CheckerStack {
public:
  CheckerStack() {
    Ok = T.start([this] { loop(); });
  }
  void run(llvm::function_ref<void()> Fn) {
    if (!Ok) {
      Fn();
      return;
    }
    std::unique_lock<std::mutex> L(M);
    Job = Fn;
    Pending = true;
    CV.notify_all();
    CV.wait(L, [this] { return !Pending; });
  }
  ~CheckerStack() {
    if (!Ok)
      return;
    {
      std::lock_guard<std::mutex> L(M);
      Quit = true;
    }
    CV.notify_all();
    T.join();
  }

private:
  void loop() {
    std::unique_lock<std::mutex> L(M);
    for (;;) {
      CV.wait(L, [this] { return Pending || Quit; });
      if (Quit)
        return;
      L.unlock();
      Job();
      L.lock();
      Pending = false;
      CV.notify_all();
    }
  }
  std::mutex M;
  std::condition_variable CV;
  llvm::function_ref<void()> Job;
  bool Pending = false, Quit = false, Ok = false;
  BigThread T; // (the last member)
};
} // namespace

/// Runs Fn on a checker stack and waits for it, or right here when this
/// thread is one already (Here, or OnCheckerThread). One CheckerStack per
/// calling thread, which is no checker thread.
static void onCheckerStack(llvm::function_ref<void()> Fn, bool Here = false) {
  if (Here || OnCheckerThread) {
    Fn();
    return;
  }
  static thread_local CheckerStack Stack;
  Stack.run(Fn);
}

/// Runs Fn on a thread with a stack of Bytes bytes (a checker thread) and
/// waits for it: the work and its order are those of a plain call. False,
/// with Fn not run, if the thread cannot be made (address space, thread
/// limit, or a test asked for that): the caller is deep in a walk and stops.
bool sema::runOnNewStack(llvm::function_ref<void()> Fn, size_t Bytes) {
  if (!newStackAllowed())
    return false;
  BigThread T;
  if (!T.start([&Fn] { Fn(); }, Bytes))
    return false;
  noteNewStackThread();
  T.join();
  return true;
}

/// Checks every body; Diags[K] gets the diagnostics of S.Bodies[K]. Workers
/// take the next body from a shared counter: the order of the bodies in time
/// is free, their diagnostics go to their own slots. Each worker runs on a
/// checker stack (OnStack: the calling thread is one); the wait is on the
/// calling thread, so a caller that is a worker of the lent pool runs tasks
/// of the group while it waits.
static void runBodies(PackageState &S, const SourceManager &SM,
                      std::vector<BodyWorker> &Workers,
                      std::vector<std::vector<Diagnostic>> &Diags,
                      std::vector<std::vector<Checker::MonoInst>> &Monos,
                      std::vector<std::vector<Checker::UntaggedPending>> &Pend,
                      std::vector<std::vector<Checker::UseRec>> &Uses,
                      bool OnStack) {
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
    onCheckerStack([&] { Run(Workers[0]); }, OnStack);
    return;
  }
  if (S.Opts.Pool && OnStack) {
    // A checker thread (a nested import) must not wait for the pool: every
    // worker of the pool may be waiting for a checker thread like this one
    // (R5). Its bodies run right here; the answer does not depend on the
    // thread that ran them.
    for (BodyWorker &W : Workers)
      Run(W);
    return;
  }
  if (llvm::ThreadPoolInterface *Pool = S.Opts.Pool) {
    llvm::ThreadPoolTaskGroup Group(*Pool);
    for (BodyWorker &W : Workers)
      Group.async([&Run, &W] { onCheckerStack([&] { Run(W); }); });
    Group.wait();
    return;
  }
  // Threads of their own with a checker stack. One that cannot be made
  // (address space) is left out: the bodies are taken from a shared counter,
  // so the others, and the calling thread when it has a checker stack or
  // when none could be made, do the work.
  std::vector<std::unique_ptr<BigThread>> Threads;
  Threads.reserve(Workers.size());
  size_t Made = 0;
  for (BodyWorker &W : Workers) {
    auto T = std::make_unique<BigThread>();
    if (T->start([&Run, &W] { Run(W); })) {
      Threads.push_back(std::move(T));
      ++Made;
    } else {
      break;
    }
  }
  // The workers without a thread: this thread runs them one after the other
  // (it takes whatever the others have not taken).
  for (size_t K = Made; K < Workers.size(); ++K)
    Run(Workers[K]);
  for (auto &T : Threads)
    T->join();
}

/// checkPackage; OnStack: this thread has a checker stack, so phase 1, a
/// single worker of phase 2 and the checks after the bodies run right here.
static std::unique_ptr<CheckedPackage>
checkPackageHere(TypeContext &Ctx, const SourceManager &SM, StringRef PkgPath,
                 ArrayRef<const ast::File *> Files, Importer &Imp,
                 DiagnosticsEngine &Diags, const CheckOptions &Opts,
                 bool OnStack) {
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
  CP->Ctx = &Ctx;
  CP->SM = &SM;
  CP->Opts = Opts;

  PackageState S(Ctx, SM, Imp, Opts);
  S.Pkg = Pkg;
  Checker C(S, CP->I, Mem, Diags);
  onCheckerStack([&] { C.checkFiles(Files); }, OnStack);
  // ADR-0025, UNI-5: the display forms of the units are decided by phase 1
  // (the declarations and the imports), before any body.
  if (!S.UNamer.Best.empty())
    Pkg->UnitNames = S.UNamer.freeze(Mem);

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
  runBodies(S, SM, Workers, BodyDiags, BodyMonos, BodyPend, BodyUses, OnStack);
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
  onCheckerStack(
      [&] {
        // GEN-12, EXP-15: the bodies are all checked, so the uses of this
        // package's type parameters are known.
        for (std::vector<Checker::UntaggedPending> &PB : BodyPend)
          C.UntaggedPend.insert(C.UntaggedPend.end(), PB.begin(), PB.end());
        for (std::vector<Checker::UseRec> &UB : BodyUses)
          C.TPUses.insert(C.TPUses.end(), UB.begin(), UB.end());
        C.noteIntrinsicUses();
        C.untaggedCheck(C.UntaggedPend);
        C.unitRangeScan(); // GEN-14, UNI-2: every instance, any route
        C.checkInitOrder(CP->InitOrder);
        C.unusedImports();
        // Instantiation cycles, of the declarations and the bodies (taken in
        // declaration order): reported, as by Go, only when the package has no
        // other error (types2 check.go runs monomorph last, if firstErr is
        // nil).
        if (C.NumErrors == 0) {
          for (std::vector<Checker::MonoInst> &MB : BodyMonos)
            for (Checker::MonoInst &M : MB)
              C.MonoInsts.push_back(std::move(M));
          C.monoCheck(C.MonoInsts, /*Report=*/true);
        }
        Pkg->Complete = true;
        if (Opts.Vet)
          vetPackage(SM, *CP, Files, Diags);
      },
      OnStack);
  return CP;
}

std::unique_ptr<CheckedPackage>
sema::checkPackage(TypeContext &Ctx, const SourceManager &SM, StringRef PkgPath,
                   ArrayRef<const ast::File *> Files, Importer &Imp,
                   DiagnosticsEngine &Diags, const CheckOptions &Opts) {
  // The whole check runs on one checker stack. With a lent pool the calling
  // thread must wait for the pool's tasks itself (runBodies), so then each
  // piece gets a checker stack of its own. A call from a checker thread (an
  // import of a package being checked) runs right there.
  if (OnCheckerThread)
    return checkPackageHere(Ctx, SM, PkgPath, Files, Imp, Diags, Opts, true);
  if (Opts.Pool)
    return checkPackageHere(Ctx, SM, PkgPath, Files, Imp, Diags, Opts, false);
  std::unique_ptr<CheckedPackage> CP;
  onCheckerStack([&] {
    CP = checkPackageHere(Ctx, SM, PkgPath, Files, Imp, Diags, Opts, true);
  });
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
  auto Text = [&](const ast::Node *N, bool Full = false) {
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
    if (S0.size() > 40 && !Full)
      S0 = S0.substr(0, 37) + "...";
    return S0;
  };
  // The calls the checker made for operator expressions (ADR-0024) are not
  // source expressions: they are shown as the operator lines below.
  llvm::DenseSet<const ast::Node *> Made;
  for (auto &KV : P.I.Operators)
    for (const ast::CallExpr *C : {KV.second.Call, KV.second.SetCall})
      if (C) {
        Made.insert(C);
        Made.insert(C->Fun);
        for (const ast::Expr *A : C->Args)
          if (auto *Id = dyn_cast<ast::Ident>(A);
              Id && Id->getBegin() == C->getBegin() && Id->Name.str() == "_")
            Made.insert(Id); // the value of `a[i] op= v` (synthExpr)
      }
  SmallVector<DumpEntry, 64> Entries;
  for (auto &KV : P.I.Types) {
    const ast::Expr *Ex = KV.first;
    const TypeAndValue &TV = KV.second;
    if (!Ex->getBegin().isValid() || Made.count(Ex))
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
    // GEN-14: the unit parameters of a generic whose code needs their units
    // at run time (Use::UnitDict, P04.24 passes them).
    ArrayRef<TypeParamType *> TPs;
    if (auto *F = dyn_cast<Func>(O); F && F->Ty)
      TPs = cast<SignatureType>(F->Ty)->TParams;
    else if (auto *TN = dyn_cast<TypeName>(O); TN && !TN->IsAlias)
      if (auto *N = llvm::dyn_cast_or_null<NamedType>(TN->Ty))
        TPs = N->TParams;
    std::string Dict;
    for (TypeParamType *TP : TPs)
      if (llvm::any_of(TP->uses(), [](const TypeParamType::Use &U) {
            return U.K == TypeParamType::Use::UnitDict;
          }))
        Dict += " " + std::string(TP->Obj ? TP->Obj->Name : StringRef("?"));
    if (!Dict.empty())
      Entries.push_back({KV.first->getBegin().getRaw(),
                         KV.first->getEnd().getRaw(), 6,
                         Loc(KV.first->getBegin()) + " unit-dict " +
                             std::string(O->Name) + ":" + Dict});
  }
  for (auto &KV : P.I.Bangs) {
    const char *Kind = KV.second == BangKind::Return  ? "returns the error"
                       : KV.second == BangKind::Panic ? "panics"
                                                      : "sets the error";
    const ast::CallExpr *C = KV.first;
    Entries.push_back({C->BangLoc.getRaw(), C->BangLoc.getRaw(), 2,
                       Loc(C->BangLoc) + " bang `" + Text(C) + "` " + Kind});
  }
  // P03.24: how each operator use was resolved, as the lowering reads it.
  static const char *const KindNames[] = {
      "none",      "add",       "sub",       "mul",       "quo",
      "rem",       "and",       "or",        "xor",       "shl",
      "shr",       "andnot",    "neg",       "pos",       "compl",
      "not",       "eq",        "cmp",       "land",      "lor",
      "index",     "indexset",  "call",      "send",      "recv",
      "inc",       "dec",       "deref",     "range",     "addassign",
      "subassign", "mulassign", "quoassign", "remassign", "andassign",
      "orassign",  "xorassign", "shlassign", "shrassign", "andnotassign",
      "convert",   "bad"};
  static_assert(std::size(KindNames) == size_t(OpKind::Bad) + 1);
  auto Method = [&](const Func *M) {
    return std::string(M->Name) + typeString(M->Ty, Pkg).substr(4);
  };
  for (auto &KV : P.I.Operators) {
    const ast::Node *N = KV.first;
    const OperatorUse &U = KV.second;
    if (!N->getBegin().isValid())
      continue;
    std::string L = Loc(N->getBegin()) + " operator `" + Text(N) + "` " +
                    KindNames[size_t(U.Op)];
    if (U.Method)
      L += " " + Method(U.Method);
    if (U.Index && U.Index != U.Method)
      L += " read " + Method(U.Index);
    if (U.IndexSet && U.IndexSet != U.Method)
      L += " write " + Method(U.IndexSet);
    for (auto [On, Flag] : {std::pair{U.Swapped, "swapped"},
                            {U.Negate, "negated"},
                            {U.Lazy, "lazy"},
                            {U.InPlace, "inplace"},
                            {U.Builtin, "builtin"},
                            {U.ViaConstraint, "constraint"},
                            {U.Inherited, "inherited"}})
      if (On)
        L += std::string(" ") + Flag;
    if (U.Sign != tok::illegal)
      L += std::string(" sign ") + tok::getSpelling(U.Sign);
    Entries.push_back({N->getBegin().getRaw(), N->getEnd().getRaw(), 3, L});
  }
  // P03.25: the scale changes and the display forms of the unit checker, as
  // the lowering reads them (P04.24).
  for (auto &KV : P.I.UnitConvs) {
    const ast::Expr *N = KV.first;
    if (!N->getBegin().isValid())
      continue;
    // (the whole text: the lowering records of nested nodes that start at
    // one column must stay apart)
    std::string L = Loc(N->getBegin()) + " unit-conv `" + Text(N, true) +
                    "` *" + units::realStr(KV.second.Factor);
    if (KV.second.Truncate)
      L += " truncates";
    if (KV.second.Tag)
      L += " tag";
    Entries.push_back({N->getBegin().getRaw(), N->getEnd().getRaw(), 4, L});
  }
  for (auto &KV : P.I.UnitSites) {
    const ast::Expr *N = KV.first;
    if (!N->getBegin().isValid())
      continue;
    auto U = [&](const UnitCanon *C) {
      return C ? units::canonStr(C, Pkg) : std::string("plain");
    };
    for (const UnitSite &St : KV.second) {
      std::string L =
          Loc(N->getBegin()) + " unit-site `" + Text(N, true) + "` ";
      if (St.K == UnitSite::Conv)
        L += U(St.Res) + " from " + U(St.X) + (St.Tag ? " tag" : "");
      else if (St.K == UnitSite::Const)
        L += U(St.Res) + " const";
      else
        L += U(St.Res) + " = " + U(St.X) +
             (St.K == UnitSite::Mul ? " * " : " / ") + U(St.Y);
      Entries.push_back({N->getBegin().getRaw(), N->getEnd().getRaw(), 4, L});
    }
  }
  for (auto &KV : P.I.UnitDisplays) {
    const ast::Expr *N = KV.first;
    if (!N->getBegin().isValid())
      continue;
    Entries.push_back({N->getBegin().getRaw(), N->getEnd().getRaw(), 5,
                       Loc(N->getBegin()) + " unit-form `" + Text(N) + "` " +
                           units::displayStr(KV.second, Pkg)});
  }
  // SLC-6: the re-slices that shrink a slice in place, as the lowering reads
  // them (P04.05).
  for (const ast::SliceExpr *N : P.I.Reslices)
    Entries.push_back({N->getBegin().getRaw(), N->getEnd().getRaw(), 7,
                       Loc(N->getBegin()) + " reslice `" + Text(N) + "`"});
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
