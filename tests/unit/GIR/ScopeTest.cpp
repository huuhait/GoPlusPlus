//===- ScopeTest.cpp - Debug scopes of GIR (P04.12) -----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The debug scopes the code generator turns into DWARF (docs/design/gir.md
// section 14): both sides of the verifier rules V073 (the scope tree) and
// V074 (what names a scope), the bitstream round trip of scopes, source
// variables and struct field names, what the inliner does to scopes (a real
// inlined scope per call, the callee's blocks copied with their origin, the
// callee's variables with theirs), and that this is near-linear, counted as
// work (M8) with a control.
//
//===----------------------------------------------------------------------===//

#include "../Support/GIRTestSupport.h"

#include "gopp/GIR/Inline.h"
#include "gopp/GIR/Serialize.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::test;

namespace {

/// GoodModule plus a defined callee `h(a i64) -> i64` with a user parameter,
/// a block scope with a user local, and a trivial body.
struct Scoped : GoodModule {
  FuncId H;
  ScopeId HBlock = 0;
  Scoped() {
    H = declare(M, "h", C.func({I64}, I64));
    FuncBuilder B(M, M.func(H));
    Function &Fn = M.func(H);
    Fn.Locals[1].Name = "a";
    Fn.Locals[1].Source = true;
    Fn.Locals[1].ArgNo = 1;
    HBlock = B.newScope();
    B.setScope(HBlock);
    LocalId Z = B.newLocal(I64, Local::Var, "z");
    Fn.Locals[Z].Source = true;
    Fn.Locals[Z].Scope = HBlock;
    B.assign(B.local(Z), B.use(copyOf(B.local(1))));
    B.assign(B.local(0), B.use(copyOf(B.local(Z))));
    B.setScope(0);
    B.ret();
  }
  /// caller(x) = h(x) (a call that can be inlined).
  FuncId caller(const char *Name = "caller") {
    FuncId F = declare(M, Name, C.func({I64}, I64));
    FuncBuilder B(M, M.func(F));
    B.call(constFunc(M, H), {copyOf(B.local(1))}, B.local(0));
    B.ret();
    return F;
  }
};

//===----------------------------------------------------------------------===//
// V073, V074: both sides
//===----------------------------------------------------------------------===//

TEST(GIRScopes, GoodScopesAreValid) {
  Scoped S;
  EXPECT_GIR_VALID(S.M);
  // A block, a block in it, and an inlined scope with a copied block.
  Function &F = S.M.func(S.F);
  ScopeId A = F.addScope({DebugScope::Block, 0, {}, NoId, 0});
  ScopeId B = F.addScope({DebugScope::Block, A, {}, NoId, 0});
  ScopeId In = F.addScope({DebugScope::Inlined, B, {}, S.H, 0});
  ScopeId Cp = F.addScope({DebugScope::Block, In, {}, NoId, S.HBlock});
  F.Blocks[0].Stmts[0].Scope = Cp;
  F.Locals[3].Scope = B;
  Local L; // the copy of h's parameter a
  L.Ty = S.I64;
  L.Scope = Cp;
  L.Origin = 1;
  L.ArgNo = 1;
  F.addLocal(L);
  EXPECT_GIR_VALID(S.M);
}

TEST(GIRScopes, V073_ParentIsAnEarlierScope) {
  {
    Scoped S;
    Function &F = S.M.func(S.F);
    F.addScope({DebugScope::Block, 1, {}, NoId, 0}); // its own id
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S;
    Function &F = S.M.func(S.F);
    F.addScope({DebugScope::Block, 2, {}, NoId, 0}); // a later one
    F.addScope({DebugScope::Block, 0, {}, NoId, 0});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // good: a parent that comes first
    Function &F = S.M.func(S.F);
    F.addScope({DebugScope::Block, 0, {}, NoId, 0});
    F.addScope({DebugScope::Block, 1, {}, NoId, 0});
    EXPECT_GIR_VALID(S.M);
  }
  {
    Scoped S; // a position of a file the module does not have
    S.M.func(S.F).addScope({DebugScope::Block, 0, {7, 1, 1}, NoId, 0});
    EXPECT_GIR_RULE(S.M, "V070");
  }
}

TEST(GIRScopes, V073_AnInlineSiteNamesAnotherDefinedFunction) {
  {
    Scoped S; // itself
    S.M.func(S.F).addScope({DebugScope::Inlined, 0, {}, S.F, 0});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // a declaration has no body to be inlined
    S.M.func(S.F).addScope({DebugScope::Inlined, 0, {}, S.Ext, 0});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // no such function
    S.M.func(S.F).addScope({DebugScope::Inlined, 0, {}, 999, 0});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // a block names no callee
    S.M.func(S.F).addScope({DebugScope::Block, 0, {}, S.H, 0});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // an inline site has no origin
    S.M.func(S.F).addScope({DebugScope::Inlined, 0, {}, S.H, 1});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // good
    S.M.func(S.F).addScope({DebugScope::Inlined, 0, {}, S.H, 0});
    EXPECT_GIR_VALID(S.M);
  }
}

TEST(GIRScopes, V073_ACopiedBlockLiesInAnInlineSiteAndNamesACalleesBlock) {
  {
    Scoped S; // a copied block outside every inline site
    S.M.func(S.F).addScope({DebugScope::Block, 0, {}, NoId, 1});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // the function's own block inside an inline site
    Function &F = S.M.func(S.F);
    ScopeId In = F.addScope({DebugScope::Inlined, 0, {}, S.H, 0});
    F.addScope({DebugScope::Block, In, {}, NoId, 0});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // a copy of a block the callee does not have
    Function &F = S.M.func(S.F);
    ScopeId In = F.addScope({DebugScope::Inlined, 0, {}, S.H, 0});
    F.addScope({DebugScope::Block, In, {}, NoId, S.HBlock + 1});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // good: copied blocks below an inline site, at any depth
    Function &F = S.M.func(S.F);
    ScopeId In = F.addScope({DebugScope::Inlined, 0, {}, S.H, 0});
    ScopeId B1 = F.addScope({DebugScope::Block, In, {}, NoId, S.HBlock});
    F.addScope({DebugScope::Block, B1, {}, NoId, S.HBlock});
    EXPECT_GIR_VALID(S.M);
  }
}

TEST(GIRScopes, V073_ACopiedBlockNamesABlockOfTheCalleesOwn) {
  // The callee `caller` has h inlined: its scope 1 is an inline site and its
  // scope 2 a copy of h's block, neither a block of its own.
  Scoped S;
  FuncId Mid = S.caller();
  ASSERT_EQ(inlineTrivial(S.M).Inlined, 1u);
  ASSERT_EQ(S.M.func(Mid).Scopes.size(), 2u);
  for (ScopeId Origin : {1u, 2u}) {
    Scoped T;
    FuncId Mid2 = T.caller();
    inlineTrivial(T.M);
    Function &F = T.M.func(T.F);
    ScopeId In = F.addScope({DebugScope::Inlined, 0, {}, Mid2, 0});
    F.addScope({DebugScope::Block, In, {}, NoId, Origin});
    EXPECT_GIR_RULE(T.M, "V073") << Origin;
  }
}

TEST(GIRScopes, V073_ADeclarationHasNoScopes) {
  {
    Scoped S;
    S.M.func(S.Ext).addScope({DebugScope::Block, 0, {}, NoId, 0});
    EXPECT_GIR_RULE(S.M, "V073");
  }
  {
    Scoped S; // good: a declaration with none
    EXPECT_GIR_VALID(S.M);
  }
}

TEST(GIRScopes, V074_EveryUseNamesAScopeThatExists) {
  {
    Scoped S; // a statement
    S.M.func(S.F).Blocks[0].Stmts[0].Scope = 1;
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // a terminator
    S.M.func(S.F).Blocks[0].Term.Scope = 4;
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // a local
    S.M.func(S.F).Locals[1].Scope = 2;
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // good: the same, with the scopes made
    Function &F = S.M.func(S.F);
    F.addScope({DebugScope::Block, 0, {}, NoId, 0});
    F.addScope({DebugScope::Block, 1, {}, NoId, 0});
    F.Blocks[0].Stmts[0].Scope = 1;
    F.Blocks[0].Term.Scope = 2;
    F.Locals[1].Scope = 2;
    EXPECT_GIR_VALID(S.M);
  }
}

/// A local of f in a new inline site of h, a copy of h's local Origin.
LocalId copyIn(Scoped &S, LocalId Origin, uint32_t ArgNo) {
  Function &F = S.M.func(S.F);
  ScopeId In = F.addScope({DebugScope::Inlined, 0, {}, S.H, 0});
  Local L;
  L.Ty = S.I64;
  L.Scope = In;
  L.Origin = Origin;
  L.ArgNo = ArgNo;
  return F.addLocal(L);
}

TEST(GIRScopes, V074_ALocalIsACopyExactlyInAnInlineSite) {
  {
    Scoped S; // a copy from a callee in no inline site
    LocalId L = copyIn(S, 2, 0);
    S.M.func(S.F).Locals[L].Scope = 0;
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // the function's own local in an inline site
    LocalId L = copyIn(S, 2, 0);
    S.M.func(S.F).Locals[L].Origin = NoId;
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // a copy of a local the callee does not have
    copyIn(S, 9, 0);
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // good: copies of h's z and of its parameter a
    copyIn(S, 2, 0);
    copyIn(S, 1, 1);
    EXPECT_GIR_VALID(S.M);
  }
}

TEST(GIRScopes, V074_ACopyNamesALocalOfTheCalleesOwn) {
  // `caller` has h inlined: its locals after the parameter are copies of h's.
  Scoped S;
  FuncId Mid = S.caller();
  ASSERT_EQ(inlineTrivial(S.M).Inlined, 1u);
  const Function &M = S.M.func(Mid);
  LocalId Copy = NoId;
  for (LocalId L = 0; L < M.Locals.size(); ++L)
    if (M.Locals[L].Origin != NoId)
      Copy = L;
  ASSERT_NE(Copy, NoId);
  for (LocalId Origin : {Copy, LocalId(1)}) {
    Function &F = S.M.func(S.F);
    ScopeId In = F.addScope({DebugScope::Inlined, 0, {}, Mid, 0});
    Local L;
    L.Ty = M.Locals[Origin].Ty;
    L.Scope = In;
    L.Origin = Origin;
    L.ArgNo = Origin == 1 ? 1 : 0;
    F.addLocal(L);
    if (Origin == Copy)
      EXPECT_GIR_RULE(S.M, "V074"); // a copy of a copy
    else
      EXPECT_GIR_VALID(S.M); // a copy of caller's own parameter
    F.Locals.pop_back();
    F.Scopes.pop_back();
  }
}

TEST(GIRScopes, V074_AVariableMayStandForAParameter) {
  // A borrowed []T parameter's (ptr, len) pair is made into a slice that the
  // debugger shows as the parameter (P04.25): a source variable whose ArgNo
  // is the parameter's position, the parameter itself then no source one.
  auto Make = [](Scoped &S, bool ParamIsSource, uint32_t ArgNo, bool Source) {
    Function &F = S.M.func(S.F);
    F.Locals[1].Source = ParamIsSource;
    F.Locals[3].K = Local::Var;
    F.Locals[3].Source = Source;
    F.Locals[3].ArgNo = ArgNo;
  };
  {
    Scoped S; // good
    Make(S, false, 1, true);
    EXPECT_GIR_VALID(S.M);
  }
  {
    Scoped S; // the parameter is a source variable too: two of one name
    Make(S, true, 1, true);
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // a compiler variable does not stand for a parameter
    Make(S, false, 1, false);
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // no such parameter
    Make(S, false, 3, true);
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // two variables for one parameter
    Make(S, false, 1, true);
    Function &F = S.M.func(S.F);
    Local L = F.Locals[3];
    F.addLocal(L);
    EXPECT_GIR_RULE(S.M, "V074");
  }
}

TEST(GIRScopes, V074_ArgNoIsThePositionOfTheParameter) {
  {
    Scoped S; // f has two parameters: local 1 is the first
    S.M.func(S.F).Locals[1].ArgNo = 2;
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // no parameter at all
    S.M.func(S.F).Locals[3].ArgNo = 3;
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // a copy of h's parameter a is its parameter 1, not 2
    copyIn(S, 1, 2);
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // a copy of h's variable z is no parameter
    copyIn(S, 2, 1);
    EXPECT_GIR_RULE(S.M, "V074");
  }
  {
    Scoped S; // good: the second parameter, and a copy of a
    S.M.func(S.F).Locals[2].ArgNo = 2;
    copyIn(S, 1, 1);
    EXPECT_GIR_VALID(S.M);
  }
}

//===----------------------------------------------------------------------===//
// The bitstream
//===----------------------------------------------------------------------===//

TEST(GIRScopes, ScopesSourceLocalsAndFieldNamesRoundTrip) {
  Scoped S;
  Type *P = S.C.structTy("test.Pair");
  ASSERT_TRUE(S.C.setStructBody(P, {S.I64, S.Bool}));
  ASSERT_TRUE(S.C.setFieldNames(P, {"left", "ok"}));
  EXPECT_EQ(P->fieldName(0), "left");
  EXPECT_EQ(P->fieldName(1), "ok");
  FuncId Id = S.caller();
  S.M.func(Id).addLocal([&] { // the type is in the file once something has it
    Local L;
    L.Ty = P;
    return L;
  }());
  ASSERT_EQ(inlineTrivial(S.M).Inlined, 1u);
  Function &F = S.M.func(Id);
  ASSERT_EQ(F.Scopes.size(), 2u); // the inlined scope and the copied block
  EXPECT_EQ(errorsOf(S.M), "");

  std::string Bytes = writeModule(S.M);
  EXPECT_EQ(Bytes, writeModule(S.M));
  Context C2;
  auto R = readModule(Bytes, C2);
  ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
  EXPECT_EQ(writeModule(**R), Bytes);
  const Function *G = nullptr;
  for (const auto &Fn : (*R)->Funcs)
    if (Fn && Fn->Name == "caller")
      G = Fn.get();
  ASSERT_NE(G, nullptr);
  ASSERT_EQ(G->Scopes.size(), F.Scopes.size());
  for (size_t I = 0; I < F.Scopes.size(); ++I)
    EXPECT_TRUE(G->Scopes[I] == F.Scopes[I]) << I;
  ASSERT_EQ(G->Locals.size(), F.Locals.size());
  for (size_t I = 0; I < F.Locals.size(); ++I) {
    EXPECT_EQ(G->Locals[I].Source, F.Locals[I].Source) << I;
    EXPECT_EQ(G->Locals[I].Scope, F.Locals[I].Scope) << I;
    EXPECT_EQ(G->Locals[I].ArgNo, F.Locals[I].ArgNo) << I;
    EXPECT_EQ(G->Locals[I].Origin, F.Locals[I].Origin) << I;
  }
  EXPECT_EQ(G->Blocks[0].Stmts.size(), F.Blocks[0].Stmts.size());
  for (size_t I = 0; I < F.Blocks[0].Stmts.size(); ++I)
    EXPECT_EQ(G->Blocks[0].Stmts[I].Scope, F.Blocks[0].Stmts[I].Scope) << I;
  EXPECT_EQ(G->Blocks[0].Term.Scope, F.Blocks[0].Term.Scope);
  // The struct came back with its field names.
  Type *Q = C2.structTy("test.Pair");
  EXPECT_EQ(Q->fieldName(0), "left");
  EXPECT_EQ(Q->fieldName(1), "ok");
}

//===----------------------------------------------------------------------===//
// The inliner
//===----------------------------------------------------------------------===//

TEST(GIRScopes, InliningMakesARealInlinedScopeAndCopiesTheCalleesBlocks) {
  Scoped S;
  Span Site{S.M.addFile("c.gopp") + 1, 9, 4};
  FuncId Id = S.caller();
  Function &F = S.M.func(Id);
  F.Blocks[0].Term.Loc = Site;
  // The callee's own statements have positions in its file.
  Function &H = S.M.func(S.H);
  uint32_t HFile = S.M.addFile("h.gopp") + 1;
  for (Statement &St : H.Blocks[0].Stmts)
    St.Loc = {HFile, 20, 2};
  ASSERT_EQ(errorsOf(S.M), "");

  ASSERT_EQ(inlineTrivial(S.M).Inlined, 1u);
  EXPECT_GIR_VALID(S.M);
  ASSERT_EQ(F.Scopes.size(), 2u);
  // Scope 1: the callee's body, at the call site. Scope 2: its block.
  EXPECT_EQ(F.Scopes[0].K, DebugScope::Inlined);
  EXPECT_EQ(F.Scopes[0].Callee, S.H);
  EXPECT_EQ(F.Scopes[0].Parent, 0u);
  EXPECT_EQ(F.Scopes[0].Loc, Site);
  EXPECT_EQ(F.Scopes[1].K, DebugScope::Block);
  EXPECT_EQ(F.Scopes[1].Parent, 1u);
  EXPECT_EQ(F.Scopes[1].Origin, S.HBlock);
  // The callee's statements keep their own file and line (the old inliner
  // gave them the call site's), now in the callee's scopes.
  unsigned InCallee = 0;
  for (const BasicBlock &BB : F.Blocks)
    for (const Statement &St : BB.Stmts)
      if (St.Loc.File == HFile) {
        ++InCallee;
        EXPECT_NE(St.Scope, 0u);
        EXPECT_EQ(St.Loc.Line, 20u);
      }
  EXPECT_GE(InCallee, 2u);
  // The callee's variables are copies that name their origin.
  unsigned Copies = 0;
  for (const Local &L : F.Locals)
    if (L.Origin != NoId) {
      EXPECT_GE(L.Scope, 1u);
      Copies += L.Source;
    }
  EXPECT_EQ(Copies, 2u); // the variables a and z
  // The callee's own locals are untouched.
  for (const Local &L : H.Locals)
    EXPECT_EQ(L.Origin, NoId);
  EXPECT_TRUE(H.Scopes.size() == 1u);
}

TEST(GIRScopes, InliningMakesTheCalleesEntryTheCalls) {
  // v(a) = w, where w is the variable a borrowed parameter's pair is made
  // into (P04.25), set by the callee's entry (Statement::Prologue). Its copy
  // is the call's (position and scope) and no entry of the caller, so the
  // inlined frame starts with w set (debug/params-break.test). The other
  // side: the callee's other statement stays in the inlined scope with its
  // own line. The flag survives the bitstream.
  Scoped S;
  uint32_t VFile = S.M.addFile("v.gopp") + 1;
  Span Site{S.M.addFile("c.gopp") + 1, 9, 4};
  FuncId V = declare(S.M, "v", S.C.func({S.I64}, S.I64));
  {
    FuncBuilder B(S.M, S.M.func(V));
    Function &Fn = S.M.func(V);
    Fn.Loc = {VFile, 5, 1};
    Fn.Locals[1].Name = "w";
    LocalId W = B.newLocal(S.I64, Local::Var, "w");
    Fn.Locals[W].Source = true;
    Fn.Locals[W].ArgNo = 1;
    B.setSpan({VFile, 5, 1});
    B.assign(B.local(W), B.use(copyOf(B.local(1))));
    Fn.Blocks[0].Stmts.back().Prologue = true;
    B.setSpan({VFile, 6, 2});
    B.assign(B.local(0), B.use(copyOf(B.local(W))));
    B.ret();
  }
  FuncId Id = declare(S.M, "cv", S.C.func({S.I64}, S.I64));
  {
    FuncBuilder B(S.M, S.M.func(Id));
    B.setSpan(Site);
    B.call(constFunc(S.M, V), {copyOf(B.local(1))}, B.local(0));
    B.ret();
  }
  ASSERT_EQ(errorsOf(S.M), "");
  // The text shows the flag only when asked, as it shows the scopes.
  EXPECT_EQ(textOf(S.M).find("[prologue]"), std::string::npos);
  std::string With;
  llvm::raw_string_ostream OS(With);
  PrintOptions PO;
  PO.Scopes = true;
  print(OS, S.M, PO);
  EXPECT_NE(With.find("[prologue]"), std::string::npos);

  std::string Bytes = writeModule(S.M);
  Context C2;
  auto R = readModule(Bytes, C2);
  ASSERT_TRUE(static_cast<bool>(R)) << toString(R.takeError());
  EXPECT_EQ(writeModule(**R), Bytes);
  const Function *G = nullptr;
  for (const auto &Fn : (*R)->Funcs)
    if (Fn && Fn->Name == "v")
      G = Fn.get();
  ASSERT_NE(G, nullptr);
  ASSERT_EQ(G->Blocks[0].Stmts.size(), 2u);
  EXPECT_TRUE(G->Blocks[0].Stmts[0].Prologue);
  EXPECT_FALSE(G->Blocks[0].Stmts[1].Prologue);

  ASSERT_GE(inlineTrivial(S.M).Inlined, 1u);
  EXPECT_GIR_VALID(S.M);
  Function &F = S.M.func(Id);
  ASSERT_EQ(F.Scopes.size(), 1u);
  ASSERT_EQ(F.Scopes[0].K, DebugScope::Inlined);
  unsigned Entry = 0, Body = 0;
  for (const Statement &St : F.Blocks[0].Stmts) {
    EXPECT_FALSE(St.Prologue);
    if (St.K != Statement::Assign)
      continue;
    const Local &D = F.Locals[St.Dest.Local];
    if (D.Source && D.Name == "w") {
      ++Entry; // w = a: the call's position and scope
      EXPECT_EQ(St.Loc, Site);
      EXPECT_EQ(St.Scope, 0u);
    } else if (St.Loc.File == VFile) {
      ++Body; // the callee's line, in the callee's scope
      EXPECT_EQ(St.Loc.Line, 6u);
      EXPECT_EQ(St.Scope, 1u);
    }
  }
  EXPECT_EQ(Entry, 1u);
  EXPECT_EQ(Body, 1u);
}

TEST(GIRScopes, ACalleeWithoutScopesStillGetsItsInlinedScope) {
  // Both sides: a function of GoodModule has no user variables and no
  // scopes; inlining it is a scope with no blocks below.
  GoodModule G;
  FuncId Id = declare(G.M, "c", G.C.func({G.I64, G.Bool}, G.I64));
  {
    FuncBuilder B(G.M, G.fn(Id));
    B.call(constFunc(G.M, G.F), {copyOf(B.local(1)), copyOf(B.local(2))},
           B.local(0));
    B.ret();
  }
  // f has a branch, which is not trivial: it stays a call, with no scope.
  inlineTrivial(G.M);
  EXPECT_TRUE(G.fn(Id).Scopes.empty() ||
              G.fn(Id).Scopes[0].K == DebugScope::Inlined);
  EXPECT_GIR_VALID(G.M);
}

TEST(GIRScopes, TwoCallsMakeTwoScopesAndNestedCallsNest) {
  Scoped S;
  // outer(x) = h(x) + h(x): two calls; wrap(x) = outer(x) is not trivial
  // (it calls), so only h is inlined, twice.
  FuncId Id = declare(S.M, "twice", S.C.func({S.I64}, S.I64));
  {
    FuncBuilder B(S.M, S.M.func(Id));
    LocalId R1 = B.newTemp(S.I64), R2 = B.newTemp(S.I64);
    B.call(constFunc(S.M, S.H), {copyOf(B.local(1))}, B.local(R1));
    B.call(constFunc(S.M, S.H), {copyOf(B.local(1))}, B.local(R2));
    B.assign(B.local(0),
             B.binary(BinOp::Add, copyOf(B.local(R1)), copyOf(B.local(R2))));
    B.ret();
  }
  ASSERT_EQ(inlineTrivial(S.M).Inlined, 2u);
  EXPECT_GIR_VALID(S.M);
  Function &F = S.M.func(Id);
  ASSERT_EQ(F.Scopes.size(), 4u);
  EXPECT_EQ(F.Scopes[0].K, DebugScope::Inlined);
  EXPECT_EQ(F.Scopes[1].K, DebugScope::Block);
  EXPECT_EQ(F.Scopes[2].K, DebugScope::Inlined);
  EXPECT_EQ(F.Scopes[3].K, DebugScope::Block);
  // Each block is below its own inlined scope, with the same origin.
  EXPECT_EQ(F.Scopes[1].Parent, 1u);
  EXPECT_EQ(F.Scopes[3].Parent, 3u);
  EXPECT_EQ(F.Scopes[1].Origin, F.Scopes[3].Origin);
  // The text shows them (and only when asked).
  std::string Plain = textOf(S.M);
  EXPECT_EQ(Plain.find("scope 1"), std::string::npos);
  std::string With;
  llvm::raw_string_ostream OS(With);
  PrintOptions PO;
  PO.Scopes = true;
  print(OS, S.M, PO);
  EXPECT_NE(With.find("scope 1"), std::string::npos);
}

//===----------------------------------------------------------------------===//
// Work: near-linear (M8), with a control
//===----------------------------------------------------------------------===//

/// One caller with N calls of the callee that has a scope and variables.
void manyScopedCalls(Scoped &S, unsigned N) {
  FuncId F = declare(S.M, "many", S.C.func({S.I64}, S.I64));
  FuncBuilder B(S.M, S.M.func(F));
  LocalId R = B.newTemp(S.I64);
  for (unsigned I = 0; I < N; ++I)
    B.call(constFunc(S.M, S.H), {constInt(S.I64, I)}, B.local(R));
  B.assign(B.local(0), B.use(copyOf(B.local(R))));
  B.ret();
}

TEST(GIRScopes, InliningScopesIsLinear) {
  const unsigned N = 1500;
  Scoped A, B4;
  manyScopedCalls(A, N);
  manyScopedCalls(B4, 4 * N);
  uint64_t S0 = inlineSteps();
  ASSERT_EQ(inlineTrivial(A.M).Inlined, N);
  uint64_t S1 = inlineSteps();
  ASSERT_EQ(inlineTrivial(B4.M).Inlined, 4u * N);
  uint64_t S2 = inlineSteps();
  EXPECT_GIR_VALID(B4.M);
  // Two scopes per call, however many calls come before it.
  EXPECT_EQ(A.M.Funcs.back()->Scopes.size(), 2u * N);
  EXPECT_EQ(B4.M.Funcs.back()->Scopes.size(), 8u * N);
  uint64_t X1 = S1 - S0, X4 = S2 - S1;
  EXPECT_GT(X1, 0u);
  EXPECT_LE(X1, 40 * uint64_t(N));
  EXPECT_LE(X4, 40 * 4 * uint64_t(N));
  EXPECT_LE(2 * X4, 9 * X1) << "steps(N) = " << X1 << ", steps(4N) = " << X4;
}

TEST(GIRScopes, VerifyingScopesIsLinear) {
  // A chain of N nested scopes and N siblings: the verifier walks each scope
  // once (the count is the number of verifier steps).
  auto Steps = [](unsigned N) {
    Scoped S;
    Function &F = S.M.func(S.F);
    for (unsigned I = 0; I < N; ++I)
      F.addScope({DebugScope::Block, I, {}, NoId, 0}); // a chain
    for (unsigned I = 0; I < N; ++I)
      F.addScope({DebugScope::Block, 0, {}, NoId, 0}); // siblings
    uint64_t A = verifierSteps();
    EXPECT_TRUE(verify(S.M).empty());
    return verifierSteps() - A;
  };
  uint64_t S1 = Steps(3000), S4 = Steps(12000);
  EXPECT_GT(S1, 0u);
  EXPECT_LE(2 * S4, 9 * S1) << S1 << " " << S4;
}

TEST(GIRScopes, ControlAQuadraticCountFailsTheCheck) {
  auto Linear = [](uint64_t S1, uint64_t S4) { return 2 * S4 <= 9 * S1; };
  EXPECT_TRUE(Linear(5000, 20000));
  EXPECT_FALSE(Linear(1000ull * 1000, 16ull * 1000 * 1000));
}

} // namespace
