//===- VerifierTest.cpp - Both sides of every GIR verifier rule -----------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01. For each rule of docs/design/gir.md section 9: the module that
// breaks it is rejected under that rule's id, and the nearby module that
// keeps it passes (a rule that rejects good GIR is a new bug).
//
//===----------------------------------------------------------------------===//

#include "../Support/GIRTestSupport.h"

#include <functional>

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::test;

namespace {

Rvalue rv(Rvalue::Kind K, Type *Ty) {
  Rvalue R;
  R.K = K;
  R.Ty = Ty;
  return R;
}
Rvalue useOf(Operand A, Type *Ty) {
  Rvalue R = rv(Rvalue::Use, Ty);
  R.A = std::move(A);
  return R;
}
Rvalue bin(BinOp Op, Operand A, Operand B, Type *Ty) {
  Rvalue R = rv(Rvalue::Binary, Ty);
  R.Bin = Op;
  R.A = std::move(A);
  R.B = std::move(B);
  return R;
}
Rvalue un(UnOp Op, Operand A, Type *Ty) {
  Rvalue R = rv(Rvalue::Unary, Ty);
  R.Un = Op;
  R.A = std::move(A);
  return R;
}
Rvalue castTo(CastKind K, Operand A, Type *Ty) {
  Rvalue R = rv(Rvalue::Cast, Ty);
  R.CastOp = K;
  R.A = std::move(A);
  return R;
}
Rvalue agg(AggKind K, Type *Ty, std::vector<Operand> Ops) {
  Rvalue R = rv(Rvalue::Aggregate, Ty);
  R.Agg = K;
  R.Ops.assign(Ops.begin(), Ops.end());
  return R;
}
Rvalue addr(Place P, Type *Ty) {
  Rvalue R = rv(Rvalue::AddrOf, Ty);
  R.P = std::move(P);
  return R;
}
Rvalue lenOf(Place P, Type *Ty) {
  Rvalue R = rv(Rvalue::Len, Ty);
  R.P = std::move(P);
  return R;
}
Operand nullOf(Type *Ptr) {
  Operand O;
  O.C.K = Constant::Null;
  O.C.Ty = Ptr;
  return O;
}
Operand zeroOf(Type *Ty) {
  Operand O;
  O.C.K = Constant::Zero;
  O.C.Ty = Ty;
  return O;
}

/// A call terminator: Dest = Callee(Args) -> Target, unwinding by U.
Terminator callOf(Operand Callee, std::vector<Operand> Args, Place Dest,
                  BlockId Target, UnwindAction U = {}) {
  Terminator T = term(Terminator::Call, Target);
  T.Discr = std::move(Callee);
  T.Args.assign(Args.begin(), Args.end());
  T.Dest = std::move(Dest);
  T.Unwind = U;
  return T;
}

/// GoodModule plus p(), a function with one local of each shape and a block
/// that returns: the places, constants and rvalues of a test go there.
struct PlaceFixture : GoodModule {
  FuncId P;
  Type *Pair, *Arr, *PI64, *Str;
  LocalId LS, LA, LP, LI, LB, LStr;
  PlaceFixture() {
    Pair = C.structTy("test.Pair");
    C.setStructBody(Pair, {I64, Bool});
    Arr = C.array(I64, 3);
    PI64 = C.ptr(I64);
    Str = C.string();
    P = declare(M, "p", C.func({}, Unit));
    FuncBuilder B(M, fn(P));
    LS = B.newLocal(Pair, Local::Var, "s");
    LA = B.newLocal(Arr, Local::Var, "a");
    LP = B.newLocal(PI64, Local::Var, "p");
    LI = B.newLocal(U64, Local::Var, "i");
    LB = B.newLocal(Bool, Local::Var, "b");
    LStr = B.newLocal(Str, Local::Var, "str");
    B.ret();
  }
  LocalId local(Type *T) {
    Local L;
    L.Ty = T;
    return fn(P).addLocal(L);
  }
  /// p: Dest = use V.
  void store(Place Dest, Operand V) {
    Type *T = Dest.Ty;
    append(P, 0, std::move(Dest), useOf(std::move(V), T));
  }
  /// p: a new local of type Dest = R.
  void set(Type *Dest, Rvalue R) {
    append(P, 0, localPlace(local(Dest), Dest), std::move(R));
  }
};

//===----------------------------------------------------------------------===//
// The good modules
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, GoodModuleIsValid) {
  GoodModule G;
  EXPECT_GIR_VALID(G.M);
}

TEST(GIRVerifier, EveryKindOfStatementAndTerminatorIsValid) {
  PlaceFixture X;
  Function &Fn = X.fn(X.P);
  X.set(X.Bool, bin(BinOp::Eq, copyOf(localPlace(X.LP, X.PI64)), nullOf(X.PI64),
                    X.Bool));
  X.set(X.I64, bin(BinOp::Shl, constInt(X.I64, 1), constInt(X.U8, 3), X.I64));
  X.set(X.I32, castTo(CastKind::IntToInt, constInt(X.I64, 5), X.I32));
  X.set(X.F64, castTo(CastKind::IntToFloat, constInt(X.I64, 5), X.F64));
  X.set(X.I64, castTo(CastKind::FloatToInt, constFloat(X.F64, 2.5), X.I64));
  X.set(X.U64, castTo(CastKind::PtrToInt, nullOf(X.PI64), X.U64));
  X.set(X.PI64, castTo(CastKind::IntToPtr, constInt(X.U64, 8), X.PI64));
  X.set(X.C.ptr(X.U8),
        castTo(CastKind::PtrToPtr, nullOf(X.PI64), X.C.ptr(X.U8)));
  X.append(
      X.P, 0, localPlace(X.LS, X.Pair),
      agg(AggKind::Struct, X.Pair, {constInt(X.I64, 1), constBool(X.C, true)}));
  X.set(X.C.tuple({X.I64, X.Bool}),
        agg(AggKind::Tuple, X.C.tuple({X.I64, X.Bool}),
            {constInt(X.I64, 1), constBool(X.C, false)}));
  X.append(X.P, 0, localPlace(X.LA, X.Arr),
           agg(AggKind::Array, X.Arr,
               {constInt(X.I64, 1), constInt(X.I64, 2), constInt(X.I64, 3)}));
  X.append(
      X.P, 0, localPlace(X.LStr, X.Str),
      agg(AggKind::String, X.Str,
          {nullOf(X.C.ptr(X.U8)), constInt(X.U64, 0), constInt(X.U64, 0)}));
  X.store(localPlace(X.LI, X.U64), constInt(X.U64, 2));
  X.store(placeOf(X.LA, X.I64, {elem(PlaceElem::Index, X.LI, X.I64)}),
          constInt(X.I64, 9));
  X.store(placeOf(X.LS, X.I64, {elem(PlaceElem::Field, 0, X.I64)}),
          constInt(X.I64, 5));
  Rvalue A = addr(placeOf(X.LA, X.I64, {elem(PlaceElem::ConstIndex, 0, X.I64)}),
                  X.PI64);
  A.Mut = true;
  X.append(X.P, 0, localPlace(X.LP, X.PI64), A);
  X.store(placeOf(X.LP, X.I64, {elem(PlaceElem::Deref, 0, X.I64)}),
          constInt(X.I64, 4));
  X.set(X.U64, lenOf(localPlace(X.LStr, X.Str), X.U64));
  X.set(X.U64, lenOf(localPlace(X.LA, X.Arr), X.U64));
  X.set(X.F64, un(UnOp::Neg, constFloat(X.F64, 1.5), X.F64));
  X.set(X.Bool, un(UnOp::Not, copyOf(localPlace(X.LB, X.Bool)), X.Bool));
  X.set(X.I64, un(UnOp::Compl, constInt(X.I64, 1), X.I64));
  X.set(X.Pair, useOf(zeroOf(X.Pair), X.Pair));
  X.set(X.F64,
        bin(BinOp::Div, constFloat(X.F64, 1), constFloat(X.F64, 2), X.F64));
  X.set(X.Bool,
        bin(BinOp::Lt, constFloat(X.F64, 1), constFloat(X.F64, 2), X.Bool));
  X.set(X.I64,
        bin(BinOp::AndNot, constInt(X.I64, 6), constInt(X.I64, 2), X.I64));
  Statement Live, Dead, Nop;
  Live.K = Statement::StorageLive;
  Live.L = X.LI;
  Dead.K = Statement::StorageDead;
  Dead.L = X.LI;
  Nop.K = Statement::Nop;
  Fn.Blocks[0].Stmts.insert(Fn.Blocks[0].Stmts.begin(), Live);
  Fn.Blocks[0].Stmts.push_back(Dead);
  Fn.Blocks[0].Stmts.push_back(Nop);
  // assert -> drop -> switch -> return.
  BlockId B1 = Fn.addBlock(), B2 = Fn.addBlock(), B3 = Fn.addBlock();
  Terminator As = term(Terminator::Assert, B1);
  As.Discr = copyOf(localPlace(X.LB, X.Bool));
  As.AK = AssertKind::Overflow;
  Fn.Blocks[0].Term = As;
  Terminator D = term(Terminator::Drop, B2);
  D.Dest = localPlace(X.LStr, X.Str);
  Fn.Blocks[B1].Term = D;
  Terminator Sw = term(Terminator::Switch);
  Sw.Discr = copyOf(localPlace(X.LI, X.U64));
  Sw.Cases = {{1, B3}, {2, B3}};
  Sw.Otherwise = B3;
  Fn.Blocks[B2].Term = Sw;
  Fn.Blocks[B3].Term = term(Terminator::Return);
  EXPECT_GIR_VALID(X.M);
}

TEST(GIRVerifier, CleanupChainIsValid) {
  // A call unwinds into bb1 (cleanup): a drop and a call there terminate on
  // a panic, then bb3 resumes.
  GoodModule G;
  FuncId F = declare(G.M, "c", G.C.func({}, G.Unit));
  Function &Fn = G.fn(F);
  {
    FuncBuilder B(G.M, Fn);
    B.newLocal(G.I64, Local::Temp);
    B.newLocal(G.C.string(), Local::Var, "s");
    B.ret();
  }
  BlockId C1 = Fn.addBlock(true), C2 = Fn.addBlock(true),
          C3 = Fn.addBlock(true), Ok = Fn.addBlock();
  Fn.Blocks[0].Term =
      callOf(constFunc(G.M, G.Ext), {constInt(G.I64, 1)}, localPlace(1, G.I64),
             Ok, {UnwindAction::Cleanup, C1});
  Terminator D = term(Terminator::Drop, C2);
  D.Dest = localPlace(2, G.C.string());
  D.Unwind = {UnwindAction::Terminate, NoId};
  Fn.Blocks[C1].Term = D;
  Fn.Blocks[C2].Term =
      callOf(constFunc(G.M, G.Ext), {constInt(G.I64, 2)}, localPlace(1, G.I64),
             C3, {UnwindAction::Terminate, NoId});
  Fn.Blocks[C3].Term = term(Terminator::Resume);
  Fn.Blocks[Ok].Term = term(Terminator::Return);
  EXPECT_GIR_VALID(G.M);
}

TEST(GIRVerifier, NoReturnCallWithoutTargetIsValid) {
  GoodModule G;
  FuncId Die = declare(G.M, "die", G.C.func({}, G.Unit));
  G.fn(Die).NoReturn = true;
  FuncId F = declare(G.M, "h", G.C.func({}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  LocalId U = B.newTemp(G.Unit);
  G.fn(F).Blocks[0].Term =
      callOf(constFunc(G.M, Die), {}, localPlace(U, G.Unit), NoId);
  EXPECT_GIR_VALID(G.M);
}

//===----------------------------------------------------------------------===//
// The module: V001-V003
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, V001_FunctionNames) {
  {
    GoodModule G;
    G.fn(G.G).Name = "f";
    EXPECT_GIR_RULE(G.M, "V001");
  }
  {
    GoodModule G;
    G.fn(G.Ext).Name = "";
    EXPECT_GIR_RULE(G.M, "V001");
  }
  {
    GoodModule G;
    G.M.Funcs.push_back(nullptr);
    EXPECT_GIR_RULE(G.M, "V001");
  }
}

TEST(GIRVerifier, V002_TypeShapes) {
  {
    PlaceFixture X; // a struct the module uses has no body
    X.local(X.C.structTy("test.Opaque"));
    EXPECT_GIR_RULE(X.M, "V002");
  }
  {
    PlaceFixture X; // a pointer type without an element
    X.local(X.C.ptr(nullptr));
    EXPECT_GIR_RULE(X.M, "V002");
  }
  {
    PlaceFixture X; // good: the same struct with its body
    Type *S = X.C.structTy("test.Opaque");
    X.C.setStructBody(S, {X.I64});
    X.local(S);
    EXPECT_GIR_VALID(X.M);
  }
}

TEST(GIRVerifier, V002_V003_SelfNamingTypes) {
  // A self-naming type (`type S []S`, `type O Option[*O]`, P04.05) needs its
  // body like a struct, and may not hold itself by value.
  {
    PlaceFixture X; // no body set
    X.local(X.C.recTy("test.Shell"));
    EXPECT_GIR_RULE(X.M, "V002");
  }
  {
    PlaceFixture X; // good: type S []S
    Type *S = X.C.recTy("test.S");
    ASSERT_TRUE(X.C.setRecBody(S, X.C.slice(S)));
    X.local(S);
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // good: type O Option[*O] (a pointer: one word)
    Type *O = X.C.recTy("test.O");
    ASSERT_TRUE(X.C.setRecBody(O, X.C.ptr(O)));
    X.local(O);
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // good: type A [2]Option[*A]
    Type *A = X.C.recTy("test.A");
    ASSERT_TRUE(X.C.setRecBody(A, X.C.array(X.C.option(X.C.ptr(A)), 2)));
    X.local(A);
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // bad: type A [2]A
    Type *A = X.C.recTy("test.A");
    ASSERT_TRUE(X.C.setRecBody(A, X.C.array(A, 2)));
    X.local(A);
    EXPECT_GIR_RULE(X.M, "V003");
  }
  {
    PlaceFixture X; // bad: type A Option[A] (the payload is A itself)
    Type *A = X.C.recTy("test.A");
    ASSERT_TRUE(X.C.setRecBody(A, X.C.option(A)));
    X.local(A);
    EXPECT_GIR_RULE(X.M, "V003");
  }
  {
    PlaceFixture X; // bad, through a struct: type A [1]B, B { A }
    Type *A = X.C.recTy("test.A"), *B = X.C.structTy("test.B");
    X.C.setStructBody(B, {A});
    ASSERT_TRUE(X.C.setRecBody(A, X.C.array(B, 1)));
    X.local(A);
    EXPECT_GIR_RULE(X.M, "V003");
  }
  {
    PlaceFixture X; // good, through a struct and a slice: B { []A }
    Type *A = X.C.recTy("test.A"), *B = X.C.structTy("test.B");
    X.C.setStructBody(B, {X.C.slice(A)});
    ASSERT_TRUE(X.C.setRecBody(A, X.C.array(B, 1)));
    X.local(A);
    EXPECT_GIR_VALID(X.M);
  }
}

TEST(GIRVerifier, V003_StructContainsItself) {
  {
    PlaceFixture X;
    Type *S = X.C.structTy("test.Node");
    X.C.setStructBody(S, {X.I64, S});
    X.local(S);
    EXPECT_GIR_RULE(X.M, "V003");
  }
  {
    // Through an array and a second struct.
    PlaceFixture X;
    Type *A = X.C.structTy("test.A"), *B = X.C.structTy("test.B");
    X.C.setStructBody(A, {X.C.array(B, 2)});
    X.C.setStructBody(B, {A});
    X.local(A);
    EXPECT_GIR_RULE(X.M, "V003");
  }
  {
    // Through a tuple.
    PlaceFixture X;
    Type *S = X.C.structTy("test.T");
    X.C.setStructBody(S, {X.C.tuple({X.I64, S})});
    X.local(X.C.ptr(S)); // reached through a pointer, still checked
    EXPECT_GIR_RULE(X.M, "V003");
  }
  {
    // Good: a pointer to itself is a list node; a slice of itself too.
    PlaceFixture X;
    Type *S = X.C.structTy("test.Node");
    X.C.setStructBody(S, {X.I64, X.C.ptr(S), X.C.slice(S)});
    X.local(S);
    EXPECT_GIR_VALID(X.M);
  }
}

//===----------------------------------------------------------------------===//
// A function: V010-V013
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, V010_Signature) {
  GoodModule G;
  G.fn(G.F).Sig = G.I64;
  EXPECT_GIR_RULE(G.M, "V010");
}

TEST(GIRVerifier, V011_Locals) {
  {
    GoodModule G; // the return place has the wrong type
    G.fn(G.F).Locals[0].Ty = G.Bool;
    EXPECT_GIR_RULE(G.M, "V011");
  }
  {
    GoodModule G; // a parameter has the wrong type
    G.fn(G.F).Locals[1].Ty = G.Bool;
    EXPECT_GIR_RULE(G.M, "V011");
  }
  {
    GoodModule G; // a parameter that is a variable
    G.fn(G.F).Locals[1].K = Local::Var;
    EXPECT_GIR_RULE(G.M, "V011");
  }
  {
    GoodModule G; // a second return place
    G.fn(G.F).Locals[3].K = Local::Return;
    EXPECT_GIR_RULE(G.M, "V011");
  }
  {
    GoodModule G; // fewer locals than 1 + parameters
    G.fn(G.F).Locals.resize(2);
    EXPECT_GIR_RULE(G.M, "V011");
  }
  {
    GoodModule G; // a local without a type
    G.fn(G.F).Locals[3].Ty = nullptr;
    EXPECT_GIR_RULE(G.M, "V011");
  }
}

TEST(GIRVerifier, V012_DeclarationHasLocals) {
  GoodModule G;
  Local L;
  L.K = Local::Return;
  L.Ty = G.I64;
  G.fn(G.Ext).Locals.push_back(L);
  EXPECT_GIR_RULE(G.M, "V012");
}

TEST(GIRVerifier, V013_Entry) {
  {
    GoodModule G; // a block jumps back to the entry
    G.termOf(G.F, 1) = term(Terminator::Goto, 0);
    EXPECT_GIR_RULE(G.M, "V013");
  }
  {
    GoodModule G;
    G.fn(G.F).Blocks[0].Cleanup = true;
    EXPECT_GIR_RULE(G.M, "V013");
  }
  {
    // Good: a loop that does not go through the entry.
    GoodModule G;
    FuncId F = declare(G.M, "loop", G.C.func({}, G.Unit));
    Function &Fn = G.fn(F);
    FuncBuilder B(G.M, Fn);
    BlockId L = B.newBlock();
    B.gotoBlock(L);
    B.setBlock(L);
    B.gotoBlock(L);
    EXPECT_GIR_VALID(G.M);
  }
}

//===----------------------------------------------------------------------===//
// The CFG: V020-V028
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, V014_OwnedIsATemporary) {
  // P04.05: a value the runtime allocates is held by a temporary marked
  // Owned, for drop elaboration (P04.09) to find; nothing else is marked.
  {
    PlaceFixture X; // good: an owned temporary
    LocalId T = X.local(X.Str);
    X.fn(X.P).Locals[T].K = Local::Temp;
    X.fn(X.P).Locals[T].Owned = true;
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // bad: an owned variable
    X.fn(X.P).Locals[X.LStr].Owned = true;
    EXPECT_GIR_RULE(X.M, "V014");
  }
  {
    PlaceFixture X; // and an unowned variable is fine
    EXPECT_GIR_VALID(X.M);
  }
}

TEST(GIRVerifier, V011_SourceIsAParameterOrAVariable) {
  {
    PlaceFixture X; // good: a source variable and a source parameter
    X.fn(X.P).Locals[X.LStr].Source = true;
    X.fn(X.P).Locals[1].Source = true;
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // bad: the result and a temporary
    X.fn(X.P).Locals[0].Source = true;
    EXPECT_GIR_RULE(X.M, "V011");
  }
  {
    PlaceFixture X;
    LocalId T = X.local(X.Str);
    X.fn(X.P).Locals[T].K = Local::Temp;
    X.fn(X.P).Locals[T].Source = true;
    EXPECT_GIR_RULE(X.M, "V011");
  }
}

TEST(GIRVerifier, V020_MissingTerminator) {
  GoodModule G;
  G.termOf(G.F, 1).Set = false;
  EXPECT_GIR_RULE(G.M, "V020");
}

TEST(GIRVerifier, V021_EdgeOutOfRange) {
  {
    GoodModule G;
    G.termOf(G.F, 1) = term(Terminator::Goto, 99);
    EXPECT_GIR_RULE(G.M, "V021");
  }
  {
    GoodModule G;
    G.termOf(G.G, 0).Unwind.Target = 99;
    EXPECT_GIR_RULE(G.M, "V021");
  }
}

TEST(GIRVerifier, V022_Unreachable) {
  GoodModule G;
  BlockId B = G.fn(G.F).addBlock();
  G.termOf(G.F, B) = term(Terminator::Return);
  EXPECT_GIR_RULE(G.M, "V022");
}

TEST(GIRVerifier, V023_NormalEdgeCrossesTheCleanupBoundary) {
  {
    GoodModule G; // normal code jumps into cleanup
    G.termOf(G.G, 2) = term(Terminator::Goto, 1);
    EXPECT_GIR_RULE(G.M, "V023");
  }
  {
    GoodModule G; // cleanup jumps back into normal code
    G.termOf(G.G, 1) = term(Terminator::Goto, 2);
    EXPECT_GIR_RULE(G.M, "V023");
  }
}

TEST(GIRVerifier, V024_UnwindTargets) {
  {
    GoodModule G; // the unwind edge goes to a normal block
    G.termOf(G.G, 0).Unwind.Target = 2;
    EXPECT_GIR_RULE(G.M, "V024");
  }
  {
    GoodModule G; // `continue` names a block
    G.termOf(G.G, 0).Unwind = {UnwindAction::Continue, 1};
    EXPECT_GIR_RULE(G.M, "V024");
  }
}

TEST(GIRVerifier, V025_CleanupShape) {
  {
    GoodModule G; // a cleanup block returns
    G.termOf(G.G, 1) = term(Terminator::Return);
    EXPECT_GIR_RULE(G.M, "V025");
  }
  {
    GoodModule G; // resume outside cleanup
    G.termOf(G.G, 2) = term(Terminator::Resume);
    EXPECT_GIR_RULE(G.M, "V025");
  }
  for (UnwindAction::Kind K : {UnwindAction::Continue, UnwindAction::Cleanup,
                               UnwindAction::Terminate}) {
    // A call in the cleanup block: it may only terminate on a panic.
    GoodModule G;
    Function &Fn = G.fn(G.G);
    BlockId Next = Fn.addBlock(true), Pad = Fn.addBlock(true);
    Fn.Blocks[Next].Term = term(Terminator::Resume);
    Fn.Blocks[Pad].Term = term(Terminator::Resume);
    Fn.Blocks[1].Term = callOf(constFunc(G.M, G.Ext), {constInt(G.I64, 1)},
                               localPlace(2, G.I64), Next,
                               {K, K == UnwindAction::Cleanup ? Pad : NoId});
    if (K == UnwindAction::Terminate) {
      Fn.Blocks.pop_back(); // the pad is not needed
      EXPECT_GIR_VALID(G.M);
    } else {
      EXPECT_GIR_RULE(G.M, "V025");
    }
  }
}

// A function `h`: bb0 calls ext with a landing block Pad (a catch block or a
// plain cleanup block), bb1 is the normal return. Shape(B, Pad, Ret) fills the
// landing block. This is what the P04.07 `defer` handler needs (D4.6,
// ADR-0028).
template <typename ShapeFn>
static void landing(GoodModule &G, bool Catch, ShapeFn Shape) {
  FuncId H = declare(G.M, "h", G.C.func({}, G.Unit));
  FuncBuilder B(G.M, G.fn(H));
  BlockId Pad = Catch ? B.newCatchBlock() : B.newBlock(true);
  LocalId R = B.newTemp(G.I64);
  BlockId Normal = B.call(constFunc(G.M, G.Ext), {constInt(G.I64, 1)},
                          B.local(R), {UnwindAction::Cleanup, Pad});
  B.ret(); // the normal path: the block after the call
  B.setBlock(Pad);
  Shape(B, G, Normal, R);
}

TEST(GIRVerifier, V025_CatchBlockMayReturnBranchAndUnwindOnward) {
  using Shape =
      std::function<void(FuncBuilder &, GoodModule &, BlockId, LocalId)>;
  // Each shape is what a `defer` handler does after `recover`.
  const std::pair<const char *, Shape> Shapes[] = {
      {"return",
       [](FuncBuilder &B, GoodModule &, BlockId, LocalId) {
         B.ret(); // recovered: the function returns normally
       }},
      {"goto-normal",
       [](FuncBuilder &B, GoodModule &, BlockId Normal, LocalId) {
         B.gotoBlock(Normal); // recovered: join the normal return path
       }},
      {"unwind-continue",
       [](FuncBuilder &B, GoodModule &G, BlockId, LocalId R) {
         // a deferred call that panics again unwinds onward
         B.call(constFunc(G.M, G.Ext), {constInt(G.I64, 2)}, B.local(R),
                {UnwindAction::Continue, NoId});
         B.resume();
       }},
      {"unwind-cleanup",
       [](FuncBuilder &B, GoodModule &G, BlockId, LocalId R) {
         // ... or into a drop pad of the function's locals
         BlockId Drops = B.newBlock(/*Cleanup=*/true);
         B.call(constFunc(G.M, G.Ext), {constInt(G.I64, 2)}, B.local(R),
                {UnwindAction::Cleanup, Drops});
         B.resume();
         B.setBlock(Drops);
         B.resume();
       }},
      {"branch-to-drop-pad",
       [](FuncBuilder &B, GoodModule &, BlockId, LocalId) {
         // not recovered: run the drops, then continue unwinding
         BlockId Drops = B.newBlock(/*Cleanup=*/true);
         B.gotoBlock(Drops);
         B.setBlock(Drops);
         B.resume();
       }},
  };
  for (const auto &[Name, Shape] : Shapes) {
    {
      GoodModule G; // accepted in a catch block
      landing(G, /*Catch=*/true, Shape);
      EXPECT_GIR_VALID(G.M) << Name;
    }
    if (std::string(Name) == "branch-to-drop-pad")
      continue; // valid in a plain pad too
    {
      GoodModule G; // the same shape in a plain drop pad is rejected
      landing(G, /*Catch=*/false, Shape);
      bool V023 = std::string(Name) == "goto-normal";
      EXPECT_TRUE(hasRule(G.M, V023 ? "V023" : "V025")) << Name << ":\n"
                                                        << errorsOf(G.M);
    }
  }
}

TEST(GIRVerifier, V023_OnlyCatchBlocksAreEnteredByNormalEdges) {
  auto Ret = [](FuncBuilder &B, GoodModule &, BlockId, LocalId) { B.ret(); };
  // h: bb0 calls ext [return: bb2, unwind: catch bb1]; bb1 returns; bb2
  // returns.
  {
    GoodModule G; // normal code branches into a catch block: rejected
    landing(G, /*Catch=*/true, Ret);
    G.fn(G.M.lookupFunction("h")).Blocks[2].Term = term(Terminator::Goto, 1);
    EXPECT_GIR_RULE(G.M, "V023");
  }
  {
    GoodModule G; // a plain cleanup block branches into a catch block: rejected
    landing(G, /*Catch=*/true, Ret);
    Function &H = G.fn(G.M.lookupFunction("h"));
    BlockId Plain = H.addBlock(/*Cleanup=*/true);
    H.Blocks[Plain].Term = term(Terminator::Goto, 1);
    H.Blocks[0].Term.Unwind.Target = Plain;
    EXPECT_GIR_RULE(G.M, "V023");
  }
  {
    GoodModule G; // good: the same, reached by the unwind edge
    landing(G, /*Catch=*/true, Ret);
    EXPECT_GIR_VALID(G.M);
  }
  {
    GoodModule G; // a catch block is also a cleanup block
    landing(G, /*Catch=*/true, Ret);
    G.fn(G.M.lookupFunction("h")).Blocks[1].Cleanup = false;
    EXPECT_GIR_RULE(G.M, "V024");
  }
}

TEST(GIRVerifier, V026_NoUnwind) {
  {
    GoodModule G; // a nounwind function with a resume
    G.fn(G.G).NoUnwind = true;
    EXPECT_GIR_RULE(G.M, "V026");
  }
  {
    GoodModule G; // a nounwind function lets a panic through
    FuncId H = declare(G.M, "h", G.C.func({}, G.Unit));
    FuncBuilder B(G.M, G.fn(H));
    B.call(constFunc(G.M, G.Ext), {constInt(G.I64, 1)},
           B.local(B.newTemp(G.I64)), {UnwindAction::Continue, NoId});
    B.ret();
    EXPECT_GIR_VALID(G.M);
    G.fn(H).NoUnwind = true;
    EXPECT_GIR_RULE(G.M, "V026");
  }
  {
    GoodModule G; // an unwind edge on a call of a nounwind callee
    G.fn(G.Ext).NoUnwind = true;
    EXPECT_GIR_RULE(G.M, "V026");
  }
  for (bool CalleeNoUnwind : {false, true}) {
    // `unreachable` on a call (lowered `nounwind`): only for a callee that
    // cannot unwind.
    GoodModule G;
    FuncId W = declare(G.M, "w", G.C.func({}, G.Unit));
    G.fn(W).NoUnwind = CalleeNoUnwind;
    FuncId H = declare(G.M, "h", G.C.func({}, G.Unit));
    FuncBuilder B(G.M, G.fn(H));
    B.call(constFunc(G.M, W), {}, B.local(B.newTemp(G.Unit)),
           {UnwindAction::Unreachable, NoId});
    B.ret();
    if (CalleeNoUnwind)
      EXPECT_GIR_VALID(G.M);
    else
      EXPECT_GIR_RULE(G.M, "V026");
  }
}

TEST(GIRVerifier, V027_SwitchCases) {
  auto Make = [](GoodModule &G, Type *Ty,
                 std::vector<SwitchCase> Cases) -> Function & {
    FuncId F = declare(G.M, "sw" + std::to_string(G.M.Funcs.size()),
                       G.C.func({Ty}, G.Unit));
    Function &Fn = G.fn(F);
    FuncBuilder B(G.M, Fn);
    BlockId Out = B.newBlock();
    for (SwitchCase &C : Cases)
      C.Target = Out;
    B.switchInt(copyOf(B.local(1)), Cases, Out);
    B.setBlock(Out);
    B.ret();
    return Fn;
  };
  {
    GoodModule G;
    Make(G, G.U8, {{1, NoId}, {1, NoId}}); // duplicate
    EXPECT_GIR_RULE(G.M, "V027");
  }
  {
    GoodModule G;
    Make(G, G.U8, {{300, NoId}}); // does not fit u8
    EXPECT_GIR_RULE(G.M, "V027");
  }
  {
    GoodModule G;
    Make(G, G.Bool, {{2, NoId}}); // not a bool
    EXPECT_GIR_RULE(G.M, "V027");
  }
  {
    GoodModule G;
    Make(G, G.U8, {{0, NoId}, {255, NoId}});
    Make(G, G.I64, {{uint64_t(-1), NoId}});
    EXPECT_GIR_VALID(G.M);
  }
}

TEST(GIRVerifier, V028_Targets) {
  {
    GoodModule G; // ext returns: its call needs a return target
    G.termOf(G.G, 0).Target = NoId;
    EXPECT_GIR_RULE(G.M, "V028");
  }
  {
    PlaceFixture X;
    Terminator D = term(Terminator::Drop, NoId);
    D.Dest = localPlace(X.LStr, X.Str);
    X.fn(X.P).Blocks[0].Term = D;
    EXPECT_GIR_RULE(X.M, "V028");
  }
  {
    PlaceFixture X;
    Terminator A = term(Terminator::Assert, NoId);
    A.Discr = constBool(X.C, true);
    X.fn(X.P).Blocks[0].Term = A;
    EXPECT_GIR_RULE(X.M, "V028");
  }
}

//===----------------------------------------------------------------------===//
// Places: V030-V036
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, GoodPlaces) {
  PlaceFixture X;
  X.store(placeOf(X.LS, X.I64, {elem(PlaceElem::Field, 0, X.I64)}),
          constInt(X.I64, 1));
  X.store(placeOf(X.LS, X.Bool, {elem(PlaceElem::Field, 1, X.Bool)}),
          constBool(X.C, true));
  X.store(placeOf(X.LA, X.I64, {elem(PlaceElem::Index, X.LI, X.I64)}),
          constInt(X.I64, 1));
  X.store(placeOf(X.LA, X.I64, {elem(PlaceElem::ConstIndex, 2, X.I64)}),
          constInt(X.I64, 1));
  X.store(placeOf(X.LP, X.I64, {elem(PlaceElem::Deref, 0, X.I64)}),
          constInt(X.I64, 1));
  X.store(placeOf(X.LStr, X.U64, {elem(PlaceElem::Field, 1, X.U64)}),
          constInt(X.U64, 1));
  EXPECT_GIR_VALID(X.M);
}

TEST(GIRVerifier, V030_NoSuchLocal) {
  PlaceFixture X;
  X.store(placeOf(99, X.Bool), constBool(X.C, true));
  EXPECT_GIR_RULE(X.M, "V030");
}

TEST(GIRVerifier, V031_PlaceType) {
  PlaceFixture X;
  X.store(placeOf(X.LB, X.I64), constInt(X.I64, 0)); // _b is a bool
  EXPECT_GIR_RULE(X.M, "V031");
}

TEST(GIRVerifier, V032_BadProjection) {
  struct Case {
    const char *Name;
    LocalId L;
    PlaceElem E;
  };
  PlaceFixture X0;
  Case Cases[] = {
      {"no field 5", X0.LS, elem(PlaceElem::Field, 5, X0.I64)},
      {"field of a bool", X0.LB, elem(PlaceElem::Field, 0, X0.I64)},
      {"index into a struct", X0.LS, elem(PlaceElem::Index, X0.LI, X0.I64)},
      {"constant index past the end", X0.LA,
       elem(PlaceElem::ConstIndex, 3, X0.I64)},
      {"downcast", X0.LS, elem(PlaceElem::Downcast, 0, X0.I64)},
  };
  for (const Case &Cs : Cases) {
    PlaceFixture X;
    X.store(placeOf(Cs.L, X.I64, {Cs.E}), constInt(X.I64, 0));
    EXPECT_TRUE(hasRule(X.M, "V032")) << Cs.Name << "\n" << errorsOf(X.M);
  }
}

TEST(GIRVerifier, V032_IndexOfAString) {
  // P04.05: a byte of a string is a u8 place (a slice gives its element,
  // a string its bytes); a string has no field of a type but the three words.
  {
    PlaceFixture X;
    X.store(placeOf(X.LStr, X.U8, {elem(PlaceElem::Index, X.LI, X.U8)}),
            constInt(X.U8, 7));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // the byte recorded as an i64
    X.store(placeOf(X.LStr, X.I64, {elem(PlaceElem::Index, X.LI, X.I64)}),
            constInt(X.I64, 7));
    EXPECT_GIR_RULE(X.M, "V033");
  }
  {
    PlaceFixture X; // a constant index into a string: arrays only
    X.store(placeOf(X.LStr, X.U8, {elem(PlaceElem::ConstIndex, 0, X.U8)}),
            constInt(X.U8, 7));
    EXPECT_GIR_RULE(X.M, "V032");
  }
}

TEST(GIRVerifier, V033_ProjectionType) {
  PlaceFixture X; // field 0 of Pair is i64, not bool
  X.store(placeOf(X.LS, X.Bool, {elem(PlaceElem::Field, 0, X.Bool)}),
          constBool(X.C, true));
  EXPECT_GIR_RULE(X.M, "V033");
}

TEST(GIRVerifier, V034_IndexLocal) {
  {
    PlaceFixture X; // a bool index
    X.store(placeOf(X.LA, X.I64, {elem(PlaceElem::Index, X.LB, X.I64)}),
            constInt(X.I64, 0));
    EXPECT_GIR_RULE(X.M, "V034");
  }
  {
    PlaceFixture X; // no such local
    X.store(placeOf(X.LA, X.I64, {elem(PlaceElem::Index, 77, X.I64)}),
            constInt(X.I64, 0));
    EXPECT_GIR_RULE(X.M, "V034");
  }
}

TEST(GIRVerifier, V035_Deref) {
  PlaceFixture X; // deref of a bool
  X.store(placeOf(X.LB, X.I64, {elem(PlaceElem::Deref, 0, X.I64)}),
          constInt(X.I64, 0));
  EXPECT_GIR_RULE(X.M, "V035");
}

TEST(GIRVerifier, V036_StorageMarker) {
  PlaceFixture X;
  Statement S;
  S.K = Statement::StorageDead;
  S.L = 99;
  X.fn(X.P).Blocks[0].Stmts.push_back(S);
  EXPECT_GIR_RULE(X.M, "V036");
}

//===----------------------------------------------------------------------===//
// Constants: V040
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, GoodConstants) {
  PlaceFixture X;
  X.set(X.U8, useOf(constInt(X.U8, 255), X.U8));
  X.set(X.I64, useOf(constInt(X.I64, uint64_t(-1)), X.I64));
  X.set(X.Bool, useOf(constBool(X.C, true), X.Bool));
  X.set(X.F64, useOf(constFloat(X.F64, 1.5), X.F64));
  X.set(X.PI64, useOf(nullOf(X.PI64), X.PI64));
  X.set(X.Pair, useOf(zeroOf(X.Pair), X.Pair));
  X.set(X.Str, useOf(constStr(X.M, "hello"), X.Str));
  Type *ExtSig = X.fn(X.Ext).Sig;
  X.set(ExtSig, useOf(constFunc(X.M, X.Ext), ExtSig));
  EXPECT_GIR_VALID(X.M);
}

TEST(GIRVerifier, V040_BadConstants) {
  struct Case {
    const char *Name;
    std::function<Operand(PlaceFixture &)> Make;
  };
  const Case Cases[] = {
      {"integer constant of a bool type",
       [](PlaceFixture &X) {
         Operand O = constInt(X.I64, 1);
         O.C.Ty = X.Bool;
         return O;
       }},
      {"bits wider than the type",
       [](PlaceFixture &X) {
         Operand O = constInt(X.U8, 1);
         O.C.Bits = 256;
         return O;
       }},
      {"float constant of an integer type",
       [](PlaceFixture &X) {
         Operand O = constFloat(X.F64, 1);
         O.C.Ty = X.I64;
         return O;
       }},
      {"bool constant 2",
       [](PlaceFixture &X) {
         Operand O = constBool(X.C, true);
         O.C.Bits = 2;
         return O;
       }},
      {"string id out of range",
       [](PlaceFixture &X) {
         Operand O = constStr(X.M, "a");
         O.C.Bits = 42;
         return O;
       }},
      {"string constant of an integer type",
       [](PlaceFixture &X) {
         Operand O = constStr(X.M, "a");
         O.C.Ty = X.I64;
         return O;
       }},
      {"function constant of the wrong type",
       [](PlaceFixture &X) {
         Operand O = constFunc(X.M, X.Ext);
         O.C.Ty = X.C.func({}, X.Unit);
         return O;
       }},
      {"function id out of range",
       [](PlaceFixture &X) {
         Operand O = constFunc(X.M, X.Ext);
         O.C.Bits = 99;
         return O;
       }},
      {"null of an integer type",
       [](PlaceFixture &X) { return nullOf(X.I64); }},
      {"constant without a type",
       [](PlaceFixture &X) {
         Operand O = constInt(X.I64, 1);
         O.C.Ty = nullptr;
         return O;
       }},
  };
  for (const Case &Cs : Cases) {
    PlaceFixture X;
    Operand O = Cs.Make(X);
    Type *T = O.C.Ty ? O.C.Ty : X.I64;
    X.set(T, useOf(O, T));
    EXPECT_TRUE(hasRule(X.M, "V040")) << Cs.Name << "\n" << errorsOf(X.M);
  }
}

//===----------------------------------------------------------------------===//
// Rvalues: V050-V056
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, V050_AssignmentTypes) {
  {
    PlaceFixture X; // an i64 rvalue in a bool place
    X.append(X.P, 0, localPlace(X.LB, X.Bool),
             useOf(constInt(X.I64, 1), X.I64));
    EXPECT_GIR_RULE(X.M, "V050");
  }
  {
    PlaceFixture X; // `use` of an i64 that claims to give a bool
    X.append(X.P, 0, localPlace(X.LB, X.Bool),
             useOf(constInt(X.I64, 1), X.Bool));
    EXPECT_GIR_RULE(X.M, "V050");
  }
  {
    PlaceFixture X; // an rvalue without a type
    X.append(X.P, 0, localPlace(X.LB, X.Bool),
             useOf(constBool(X.C, true), nullptr));
    EXPECT_GIR_RULE(X.M, "V050");
  }
}

TEST(GIRVerifier, V051_Unary) {
  struct Case {
    UnOp Op;
    bool Ok;
    std::function<Operand(PlaceFixture &)> A;
    std::function<Type *(PlaceFixture &)> Res;
  };
  const Case Cases[] = {
      {UnOp::Neg, false, [](PlaceFixture &X) { return constBool(X.C, true); },
       [](PlaceFixture &X) { return X.Bool; }},
      {UnOp::Not, false, [](PlaceFixture &X) { return constInt(X.I64, 1); },
       [](PlaceFixture &X) { return X.I64; }},
      {UnOp::Compl, false, [](PlaceFixture &X) { return constFloat(X.F64, 1); },
       [](PlaceFixture &X) { return X.F64; }},
      {UnOp::Neg, false, [](PlaceFixture &X) { return constInt(X.I64, 1); },
       [](PlaceFixture &X) { return X.I32; }}, // result type differs
      {UnOp::Neg, true, [](PlaceFixture &X) { return constInt(X.I64, 1); },
       [](PlaceFixture &X) { return X.I64; }},
      {UnOp::Neg, true, [](PlaceFixture &X) { return constFloat(X.F64, 1); },
       [](PlaceFixture &X) { return X.F64; }},
      {UnOp::Not, true, [](PlaceFixture &X) { return constBool(X.C, true); },
       [](PlaceFixture &X) { return X.Bool; }},
      {UnOp::Compl, true, [](PlaceFixture &X) { return constInt(X.U8, 1); },
       [](PlaceFixture &X) { return X.U8; }},
  };
  for (const Case &Cs : Cases) {
    PlaceFixture X;
    Type *R = Cs.Res(X);
    X.set(R, un(Cs.Op, Cs.A(X), R));
    if (Cs.Ok)
      EXPECT_GIR_VALID(X.M);
    else
      EXPECT_GIR_RULE(X.M, "V051");
  }
}

TEST(GIRVerifier, V052_Binary) {
  auto Bad = [](std::function<void(PlaceFixture &)> Fill) {
    PlaceFixture X;
    Fill(X);
    EXPECT_GIR_RULE(X.M, "V052");
  };
  Bad([](PlaceFixture &X) { // different operand types
    X.set(X.I64,
          bin(BinOp::Add, constInt(X.I64, 1), constInt(X.I32, 1), X.I64));
  });
  Bad([](PlaceFixture &X) { // add on bool
    X.set(X.Bool,
          bin(BinOp::Add, constBool(X.C, true), constBool(X.C, true), X.Bool));
  });
  Bad([](PlaceFixture &X) { // rem on float
    X.set(X.F64,
          bin(BinOp::Rem, constFloat(X.F64, 1), constFloat(X.F64, 1), X.F64));
  });
  Bad([](PlaceFixture &X) { // and on bool (`&&` is a branch)
    X.set(X.Bool,
          bin(BinOp::And, constBool(X.C, true), constBool(X.C, true), X.Bool));
  });
  Bad([](PlaceFixture &X) { // eq on a struct
    X.set(X.Bool, bin(BinOp::Eq, copyOf(localPlace(X.LS, X.Pair)),
                      copyOf(localPlace(X.LS, X.Pair)), X.Bool));
  });
  Bad([](PlaceFixture &X) { // ordering on bool
    X.set(X.Bool,
          bin(BinOp::Lt, constBool(X.C, true), constBool(X.C, false), X.Bool));
  });
  Bad([](PlaceFixture &X) { // a comparison that gives an integer
    X.set(X.I64, bin(BinOp::Eq, constInt(X.I64, 1), constInt(X.I64, 1), X.I64));
  });
  Bad([](PlaceFixture &X) { // a shift of a float
    X.set(X.F64,
          bin(BinOp::Shl, constFloat(X.F64, 1), constInt(X.U8, 1), X.F64));
  });
  Bad([](PlaceFixture &X) { // a shift that gives the count's type
    X.set(X.U8, bin(BinOp::Shr, constInt(X.I64, 1), constInt(X.U8, 1), X.U8));
  });
  // Good: every operator on a type it allows.
  PlaceFixture X;
  for (BinOp Op : {BinOp::Add, BinOp::Sub, BinOp::Mul, BinOp::Div})
    X.set(X.F64, bin(Op, constFloat(X.F64, 1), constFloat(X.F64, 2), X.F64));
  for (BinOp Op : {BinOp::Add, BinOp::Rem, BinOp::And, BinOp::Or, BinOp::Xor,
                   BinOp::AndNot})
    X.set(X.I64, bin(Op, constInt(X.I64, 1), constInt(X.I64, 2), X.I64));
  for (BinOp Op : {BinOp::Shl, BinOp::Shr})
    X.set(X.I32, bin(Op, constInt(X.I32, 1), constInt(X.U64, 40), X.I32));
  for (BinOp Op : {BinOp::Eq, BinOp::Ne}) {
    X.set(X.Bool, bin(Op, nullOf(X.PI64), nullOf(X.PI64), X.Bool));
    X.set(X.Bool, bin(Op, constBool(X.C, true), constBool(X.C, true), X.Bool));
  }
  for (BinOp Op : {BinOp::Lt, BinOp::Le, BinOp::Gt, BinOp::Ge}) {
    X.set(X.Bool, bin(Op, constInt(X.I64, 1), constInt(X.I64, 2), X.Bool));
    X.set(X.Bool, bin(Op, constFloat(X.F64, 1), constFloat(X.F64, 2), X.Bool));
  }
  EXPECT_GIR_VALID(X.M);
}

TEST(GIRVerifier, V053_Cast) {
  auto Bad = [](CastKind K, std::function<Operand(PlaceFixture &)> A,
                std::function<Type *(PlaceFixture &)> To) {
    PlaceFixture X;
    Type *T = To(X);
    X.set(T, castTo(K, A(X), T));
    EXPECT_GIR_RULE(X.M, "V053");
  };
  Bad(
      CastKind::IntToFloat,
      [](PlaceFixture &X) { return constFloat(X.F64, 1); },
      [](PlaceFixture &X) { return X.F64; });
  Bad(
      CastKind::FloatToInt,
      [](PlaceFixture &X) { return constFloat(X.F64, 1); },
      [](PlaceFixture &X) { return X.F64; });
  Bad(
      CastKind::PtrToInt, [](PlaceFixture &X) { return constInt(X.I64, 1); },
      [](PlaceFixture &X) { return X.I64; });
  Bad(
      CastKind::IntToPtr, [](PlaceFixture &X) { return nullOf(X.PI64); },
      [](PlaceFixture &X) { return X.PI64; });
  // P04.06: an itab holds methods as plain pointers and a call turns one
  // back, so PtrToPtr is also the cast between a pointer and a function;
  // an integer is neither.
  {
    PlaceFixture X;
    Type *PU8 = X.C.ptr(X.C.u8());
    Type *FnTy = X.M.func(X.F).Sig;
    X.set(PU8, castTo(CastKind::PtrToPtr, constFunc(X.M, X.F), PU8));
    X.set(FnTy, castTo(CastKind::PtrToPtr, nullOf(PU8), FnTy));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X;
    Type *FnTy = X.M.func(X.F).Sig;
    X.set(FnTy, castTo(CastKind::PtrToPtr, constInt(X.I64, 1), FnTy));
    EXPECT_GIR_RULE(X.M, "V053");
  }
  {
    PlaceFixture X;
    X.set(X.I64, castTo(CastKind::PtrToInt, constFunc(X.M, X.F), X.I64));
    EXPECT_GIR_RULE(X.M, "V053");
  }
  // (The good side is EveryKindOfStatementAndTerminatorIsValid: each kind
  // with the types it names.)
}

TEST(GIRVerifier, V054_AddrOf) {
  {
    PlaceFixture X; // the address of a u64 with type *bool
    Type *PB = X.C.ptr(X.Bool);
    X.set(PB, addr(localPlace(X.LI, X.U64), PB));
    EXPECT_GIR_RULE(X.M, "V054");
  }
  {
    PlaceFixture X; // an address in a non-pointer
    X.set(X.I64, addr(localPlace(X.LI, X.U64), X.I64));
    EXPECT_GIR_RULE(X.M, "V054");
  }
  {
    PlaceFixture X;
    Type *PU = X.C.ptr(X.U64);
    X.set(PU, addr(localPlace(X.LI, X.U64), PU));
    EXPECT_GIR_VALID(X.M);
  }
}

TEST(GIRVerifier, V055_Aggregate) {
  auto Bad = [](const char *Name, std::function<void(PlaceFixture &)> Fill) {
    PlaceFixture X;
    Fill(X);
    EXPECT_TRUE(hasRule(X.M, "V055")) << Name << "\n" << errorsOf(X.M);
  };
  Bad("too few operands", [](PlaceFixture &X) {
    X.set(X.Pair, agg(AggKind::Struct, X.Pair, {constInt(X.I64, 1)}));
  });
  Bad("wrong operand type", [](PlaceFixture &X) {
    X.set(X.Pair, agg(AggKind::Struct, X.Pair,
                      {constInt(X.I64, 1), constInt(X.I64, 1)}));
  });
  Bad("array of the wrong length", [](PlaceFixture &X) {
    X.set(X.Arr, agg(AggKind::Array, X.Arr, {constInt(X.I64, 1)}));
  });
  Bad("kind that is not the type's", [](PlaceFixture &X) {
    X.set(X.Pair, agg(AggKind::Tuple, X.Pair,
                      {constInt(X.I64, 1), constBool(X.C, true)}));
  });
  Bad("struct without a body", [](PlaceFixture &X) {
    Type *S = X.C.structTy("test.Opaque");
    X.set(S, agg(AggKind::Struct, S, {}));
  });
  Bad("string with the wrong field", [](PlaceFixture &X) {
    X.set(X.Str,
          agg(AggKind::String, X.Str,
              {nullOf(X.C.ptr(X.U8)), constInt(X.I64, 0), constInt(X.U64, 0)}));
  });
}

TEST(GIRVerifier, V055_V032_TheFourWordSlice) {
  // P04.25 (ADR-0029): a slice is {ptr, len, cap, allocator}, with the
  // allocator word a *u8 (null: the global allocator). The three-word form of
  // P04.05 is no longer a slice; a view and a borrowed []T are the pair.
  auto SliceOps = [](PlaceFixture &X, Type *Elem, bool Alloc, Type *AllocTy) {
    std::vector<Operand> Ops = {nullOf(X.C.ptr(Elem)), constInt(X.U64, 0),
                                constInt(X.U64, 0)};
    if (Alloc)
      Ops.push_back(AllocTy->isPtr() ? nullOf(AllocTy) : constInt(AllocTy, 0));
    return Ops;
  };
  {
    PlaceFixture X; // good: four words
    Type *S = X.C.slice(X.I64);
    X.set(S, agg(AggKind::Slice, S, SliceOps(X, X.I64, true, X.C.ptr(X.U8))));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // bad: the three words of P04.05
    Type *S = X.C.slice(X.I64);
    X.set(S, agg(AggKind::Slice, S, SliceOps(X, X.I64, false, nullptr)));
    EXPECT_GIR_RULE(X.M, "V055");
  }
  {
    PlaceFixture X; // bad: the allocator word is not a *u8
    Type *S = X.C.slice(X.I64);
    X.set(S, agg(AggKind::Slice, S, SliceOps(X, X.I64, true, X.U64)));
    EXPECT_GIR_RULE(X.M, "V055");
  }
  {
    PlaceFixture X; // bad: nor a pointer of another type (the element's)
    Type *S = X.C.slice(X.I64);
    X.set(S, agg(AggKind::Slice, S, SliceOps(X, X.I64, true, X.PI64)));
    EXPECT_GIR_RULE(X.M, "V055");
  }
  {
    PlaceFixture X; // bad: five operands
    Type *S = X.C.slice(X.I64);
    std::vector<Operand> Ops = SliceOps(X, X.I64, true, X.C.ptr(X.U8));
    Ops.push_back(nullOf(X.C.ptr(X.U8)));
    X.set(S, agg(AggKind::Slice, S, Ops));
    EXPECT_GIR_RULE(X.M, "V055");
  }
  {
    PlaceFixture X; // bad: field 3 read as a u64 (it is the *u8 word)
    Type *S = X.C.slice(X.I64);
    LocalId L = X.local(S);
    X.set(X.U64,
          useOf(copyOf(placeOf(L, X.U64, {elem(PlaceElem::Field, 3, X.U64)})),
                X.U64));
    EXPECT_GIR_RULE(X.M, "V033");
  }
  {
    PlaceFixture X; // bad: a string stays three words
    X.set(X.Str, agg(AggKind::String, X.Str,
                     {nullOf(X.C.ptr(X.U8)), constInt(X.U64, 0),
                      constInt(X.U64, 0), nullOf(X.C.ptr(X.U8))}));
    EXPECT_GIR_RULE(X.M, "V055");
  }
  {
    PlaceFixture X; // good: the word is field 3, a *u8; a string has no field 3
    Type *S = X.C.slice(X.I64);
    LocalId L = X.local(S);
    Type *PU = X.C.ptr(X.U8);
    X.set(PU,
          useOf(copyOf(placeOf(L, PU, {elem(PlaceElem::Field, 3, PU)})), PU));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // bad: field 4 does not exist
    Type *S = X.C.slice(X.I64);
    LocalId L = X.local(S);
    X.set(X.U64,
          useOf(copyOf(placeOf(L, X.U64, {elem(PlaceElem::Field, 4, X.U64)})),
                X.U64));
    EXPECT_GIR_RULE(X.M, "V032");
  }
  {
    PlaceFixture X; // bad: a string has no allocator word
    LocalId L = X.local(X.Str);
    Type *PU = X.C.ptr(X.U8);
    X.set(PU,
          useOf(copyOf(placeOf(L, PU, {elem(PlaceElem::Field, 3, PU)})), PU));
    EXPECT_GIR_RULE(X.M, "V032");
  }
}

TEST(GIRVerifier, V057_AggregateBuiltInPlace) {
  {
    PlaceFixture X; // s = Pair{a[0], b}: the operands read other locals
    X.append(X.P, 0, localPlace(X.LS, X.Pair),
             agg(AggKind::Struct, X.Pair,
                 {copyOf(placeOf(X.LA, X.I64,
                                 {elem(PlaceElem::ConstIndex, 0, X.I64)})),
                  copyOf(localPlace(X.LB, X.Bool))}));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // s = Pair{s.0, b} would read a half-built s
    X.append(
        X.P, 0, localPlace(X.LS, X.Pair),
        agg(AggKind::Struct, X.Pair,
            {copyOf(placeOf(X.LS, X.I64, {elem(PlaceElem::Field, 0, X.I64)})),
             copyOf(localPlace(X.LB, X.Bool))}));
    EXPECT_GIR_RULE(X.M, "V057");
  }
  {
    PlaceFixture X; // *q = [3]i64{...}: another pointer may alias *q
    LocalId Q = X.local(X.C.ptr(X.Arr));
    X.append(X.P, 0, placeOf(Q, X.Arr, {elem(PlaceElem::Deref, 0, X.Arr)}),
             agg(AggKind::Array, X.Arr,
                 {constInt(X.I64, 1), constInt(X.I64, 2), constInt(X.I64, 3)}));
    EXPECT_GIR_RULE(X.M, "V057");
  }
  {
    PlaceFixture X; // a copy through a pointer is fine: it is a Use
    LocalId Q = X.local(X.C.ptr(X.Arr));
    X.store(placeOf(Q, X.Arr, {elem(PlaceElem::Deref, 0, X.Arr)}),
            copyOf(localPlace(X.LA, X.Arr)));
    EXPECT_GIR_VALID(X.M);
  }
}

TEST(GIRVerifier, V056_Len) {
  {
    PlaceFixture X; // len of an integer
    X.set(X.U64, lenOf(localPlace(X.LI, X.U64), X.U64));
    EXPECT_GIR_RULE(X.M, "V056");
  }
  {
    PlaceFixture X; // len gives u64, not i64
    X.set(X.I64, lenOf(localPlace(X.LStr, X.Str), X.I64));
    EXPECT_GIR_RULE(X.M, "V056");
  }
}

//===----------------------------------------------------------------------===//
// Terminators: V060-V064
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, V060_SwitchAndAssertOperands) {
  {
    GoodModule G; // a switch on a float
    G.termOf(G.F, 0).Discr = constFloat(G.F64, 1.0);
    EXPECT_GIR_RULE(G.M, "V060");
  }
  {
    PlaceFixture X; // an assert on an integer
    BlockId B1 = X.fn(X.P).addBlock();
    X.fn(X.P).Blocks[B1].Term = term(Terminator::Return);
    Terminator A = term(Terminator::Assert, B1);
    A.Discr = constInt(X.I64, 1);
    X.fn(X.P).Blocks[0].Term = A;
    EXPECT_GIR_RULE(X.M, "V060");
  }
}

TEST(GIRVerifier, V061_CalleeIsNotAFunction) {
  GoodModule G;
  G.termOf(G.G, 0).Discr = constInt(G.I64, 0);
  EXPECT_GIR_RULE(G.M, "V061");
}

TEST(GIRVerifier, V062_Arguments) {
  {
    GoodModule G; // ext takes one argument
    G.termOf(G.G, 0).Args.clear();
    EXPECT_GIR_RULE(G.M, "V062");
  }
  {
    GoodModule G;
    G.termOf(G.G, 0).Args[0] = constBool(G.C, true);
    EXPECT_GIR_RULE(G.M, "V062");
  }
}

TEST(GIRVerifier, V063_Destination) {
  {
    GoodModule G; // an i64 result in a bool place
    Local L;
    L.Ty = G.Bool;
    LocalId B = G.fn(G.G).addLocal(L);
    G.termOf(G.G, 0).Dest = localPlace(B, G.Bool);
    EXPECT_GIR_RULE(G.M, "V063");
  }
  {
    GoodModule G; // good: any place of the result type
    G.termOf(G.G, 0).Dest = localPlace(1, G.I64);
    EXPECT_GIR_VALID(G.M);
  }
}

TEST(GIRVerifier, V064_DropOfUnit) {
  GoodModule G;
  FuncId F = declare(G.M, "d", G.C.func({G.Unit}, G.Unit));
  FuncBuilder B(G.M, G.fn(F));
  B.drop(B.local(1));
  B.ret();
  EXPECT_GIR_RULE(G.M, "V064");
}

//===----------------------------------------------------------------------===//
// Spans: V070
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, V070_SourcePositions) {
  {
    GoodModule G; // the module has no files
    G.fn(G.F).Blocks[0].Stmts[0].Loc = {3, 1, 1};
    EXPECT_GIR_RULE(G.M, "V070");
  }
  {
    GoodModule G;
    G.termOf(G.F, 1).Loc = {1, 1, 1};
    EXPECT_GIR_RULE(G.M, "V070");
  }
  {
    GoodModule G;
    G.fn(G.Ext).Loc = {2, 1, 1};
    EXPECT_GIR_RULE(G.M, "V070");
  }
  {
    GoodModule G;
    uint32_t File = G.M.addFile("a.gopp") + 1;
    G.fn(G.F).Blocks[0].Stmts[0].Loc = {File, 1, 1};
    G.termOf(G.F, 1).Loc = {File, 2, 3};
    G.fn(G.Ext).Loc = {File, 4, 1};
    EXPECT_GIR_VALID(G.M);
    // A module that breaks V070 still prints (the dump of a bad file).
    G.termOf(G.F, 1).Loc = {7, 2, 3};
    EXPECT_NE(textOf(G.M).find("; ?:2:3"), std::string::npos);
  }
}

//===----------------------------------------------------------------------===//
// The error limit and the message format
//===----------------------------------------------------------------------===//

TEST(GIRVerifier, ErrorLimitAndFormat) {
  GoodModule G;
  for (int I = 0; I < 100; ++I) {
    BlockId B = G.fn(G.F).addBlock(); // 100 unreachable blocks
    G.termOf(G.F, B) = term(Terminator::Return);
  }
  EXPECT_EQ(verify(G.M, 10).size(), 10u);
  EXPECT_EQ(verify(G.M, 1000).size(), 100u);
  std::vector<VerifyError> One = verify(G.M, 1);
  ASSERT_EQ(One.size(), 1u);
  EXPECT_EQ(One[0].str(), "V022: fn f: bb3: bb3 is unreachable");
  std::string S;
  llvm::raw_string_ostream OS(S);
  EXPECT_FALSE(verify(G.M, OS));
  EXPECT_EQ(S.rfind("gir verifier: V022: fn f: bb3: ", 0), 0u) << S;
}

//===----------------------------------------------------------------------===//
// P04.04: assert values, globals, Option
//===----------------------------------------------------------------------===//

/// p: bb0 ends in an assert of kind K on a bool with Args.
void assertWith(PlaceFixture &X, AssertKind K, std::vector<Operand> Args) {
  BlockId B1 = X.fn(X.P).addBlock();
  X.fn(X.P).Blocks[B1].Term = term(Terminator::Return);
  Terminator A = term(Terminator::Assert, B1);
  A.Discr = constBool(X.C, true);
  A.AK = K;
  A.Args.assign(Args.begin(), Args.end());
  X.fn(X.P).Blocks[0].Term = A;
}

TEST(GIRVerifier, V065_AssertValues) {
  // A bounds assert shows the index and the length: two integers, the
  // length unsigned.
  {
    PlaceFixture X;
    assertWith(X, AssertKind::Bounds, {constInt(X.I64, 5), constInt(X.U64, 3)});
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // an unsigned index
    assertWith(X, AssertKind::Bounds, {constInt(X.U64, 5), constInt(X.U64, 3)});
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // no values
    assertWith(X, AssertKind::Bounds, {});
    EXPECT_GIR_RULE(X.M, "V065");
  }
  {
    PlaceFixture X; // an index of 32 bits: the runtime function takes 64
    assertWith(X, AssertKind::Bounds, {constInt(X.I32, 5), constInt(X.U64, 3)});
    EXPECT_GIR_RULE(X.M, "V065");
  }
  {
    PlaceFixture X; // and of 8 bits, unsigned
    assertWith(X, AssertKind::Bounds, {constInt(X.U8, 5), constInt(X.U64, 3)});
    EXPECT_GIR_RULE(X.M, "V065");
  }
  {
    PlaceFixture X; // a signed length
    assertWith(X, AssertKind::Bounds, {constInt(X.I64, 5), constInt(X.I64, 3)});
    EXPECT_GIR_RULE(X.M, "V065");
  }
  {
    PlaceFixture X; // an index that is not an integer
    assertWith(X, AssertKind::Bounds,
               {constFloat(X.F64, 5), constInt(X.U64, 3)});
    EXPECT_GIR_RULE(X.M, "V065");
  }
  // The slice-bounds kinds (P04.05) show the same two integers: the values a
  // message such as "slice bounds out of range [:7] with capacity 4" prints.
  for (AssertKind K :
       {AssertKind::SliceAlen, AssertKind::SliceAcap, AssertKind::SliceB,
        AssertKind::Slice3Alen, AssertKind::Slice3Acap, AssertKind::Slice3B,
        AssertKind::Slice3C, AssertKind::SliceConvert}) {
    PlaceFixture Good;
    assertWith(Good, K, {constInt(Good.I64, 7), constInt(Good.U64, 4)});
    EXPECT_GIR_VALID(Good.M);
    PlaceFixture Unsigned;
    assertWith(Unsigned, K,
               {constInt(Unsigned.U64, 7), constInt(Unsigned.U64, 4)});
    EXPECT_GIR_VALID(Unsigned.M);
    PlaceFixture None;
    assertWith(None, K, {});
    EXPECT_GIR_RULE(None.M, "V065");
    PlaceFixture Narrow;
    assertWith(Narrow, K, {constInt(Narrow.I32, 7), constInt(Narrow.U64, 4)});
    EXPECT_GIR_RULE(Narrow.M, "V065");
  }
  // The other kinds show nothing.
  for (AssertKind K : {AssertKind::DivByZero, AssertKind::NegativeShift,
                       AssertKind::Nil, AssertKind::Overflow}) {
    PlaceFixture Good;
    assertWith(Good, K, {});
    EXPECT_GIR_VALID(Good.M);
    PlaceFixture Bad;
    assertWith(Bad, K, {constInt(Bad.I64, 1)});
    EXPECT_GIR_RULE(Bad.M, "V065");
  }
}

/// p: a call of mk() -> Pair, its result going to Dest, then return.
void callMk(PlaceFixture &X, Place Dest) {
  FuncId Mk = declare(X.M, "mk", X.C.func({}, X.Pair));
  BlockId B1 = X.fn(X.P).addBlock();
  X.fn(X.P).Blocks[B1].Term = term(Terminator::Return);
  X.fn(X.P).Blocks[0].Term =
      callOf(constFunc(X.M, Mk), {}, std::move(Dest), B1);
}

TEST(GIRVerifier, V066_HiddenResultPointer) {
  {
    PlaceFixture X; // a fresh temporary: the builder's shape
    LocalId T = X.local(X.Pair);
    callMk(X, localPlace(T, X.Pair));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // a field of a temporary is a place of the frame as well
    Type *Outer = X.C.structTy("test.Outer");
    X.C.setStructBody(Outer, {X.Pair});
    LocalId F = X.local(Outer);
    callMk(X, placeOf(F, X.Pair, {elem(PlaceElem::Field, 0, X.Pair)}));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // a scalar result may go anywhere (no hidden pointer)
    FuncId One = declare(X.M, "one", X.C.func({}, X.I64));
    BlockId B1 = X.fn(X.P).addBlock();
    X.fn(X.P).Blocks[B1].Term = term(Terminator::Return);
    LocalId Q = X.local(X.PI64);
    X.fn(X.P).Blocks[0].Term =
        callOf(constFunc(X.M, One), {},
               placeOf(Q, X.I64, {elem(PlaceElem::Deref, 0, X.I64)}), B1);
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // into a named variable
    callMk(X, localPlace(X.LS, X.Pair));
    EXPECT_GIR_RULE(X.M, "V066");
  }
  {
    PlaceFixture X; // through a pointer: another pointer may alias it
    LocalId Q = X.local(X.C.ptr(X.Pair));
    callMk(X, placeOf(Q, X.Pair, {elem(PlaceElem::Deref, 0, X.Pair)}));
    EXPECT_GIR_RULE(X.M, "V066");
  }
  {
    PlaceFixture X; // into a temporary whose address is taken
    LocalId T = X.local(X.Pair);
    X.set(X.C.ptr(X.Pair), addr(localPlace(T, X.Pair), X.C.ptr(X.Pair)));
    callMk(X, localPlace(T, X.Pair));
    EXPECT_GIR_RULE(X.M, "V066");
  }
}

/// A declaration with the C convention and this signature.
FuncId cDecl(PlaceFixture &X, const std::string &Name,
             llvm::ArrayRef<Type *> Ps, Type *Ret) {
  FuncId F = declare(X.M, Name, X.C.func(Ps, Ret, false, CallConv::C));
  X.M.func(F).CC = CallConv::C;
  return F;
}

/// A C function pointer type (a callback, P04.18).
Type *cFn(PlaceFixture &X, llvm::ArrayRef<Type *> Ps, Type *R,
          bool Fails = false) {
  return X.C.func(Ps, R, Fails, CallConv::C);
}

/// The struct test.<Name> with these fields.
Type *structOf(PlaceFixture &X, const std::string &Name,
               llvm::ArrayRef<Type *> Fs) {
  Type *S = X.C.structTy("test." + Name);
  X.C.setStructBody(S, Fs);
  return S;
}

TEST(GIRVerifier, V075_AFunctionHasTheConventionOfItsType) {
  // P04.18: the calling convention is part of a function type, and a
  // function's own convention is its signature's. So a pointer to a function
  // (Constant::Func, typed by the signature) says how to call it, and the
  // types keep a Go++ function away from C and a C function away from a
  // Go++ call through a pointer of the other kind.
  auto Body = [](PlaceFixture &X, FuncId F) {
    FuncBuilder B(X.M, X.fn(F));
    B.assign(B.local(0), B.use(constInt(X.I32, 0)));
    B.ret();
  };
  for (CallConv Fn : {CallConv::Go, CallConv::C})
    for (CallConv Ty : {CallConv::Go, CallConv::C}) {
      PlaceFixture X;
      FuncId F = declare(X.M, "cc", X.C.func({X.I32}, X.I32, false, Ty));
      X.M.func(F).CC = Fn;
      Body(X, F);
      if (Fn == Ty)
        EXPECT_GIR_VALID(X.M);
      else
        EXPECT_GIR_RULE(X.M, "V075");
    }
  // A callback (a C function that takes a function): the parameter is a C
  // function pointer.
  auto CallTakes = [](PlaceFixture &X, Type *Param, Operand Arg) {
    FuncId Takes = cDecl(X, "takes", {Param}, X.Unit);
    BlockId B1 = X.fn(X.P).addBlock();
    X.fn(X.P).Blocks[B1].Term = term(Terminator::Return);
    LocalId T = X.local(X.Unit);
    X.fn(X.P).Blocks[0].Term = callOf(constFunc(X.M, Takes), {std::move(Arg)},
                                      localPlace(T, X.Unit), B1);
  };
  {
    PlaceFixture X; // the C-ABI thunk of a Go++ function: valid
    Type *Cb = X.C.func({X.I32}, X.I32, false, CallConv::C);
    FuncId Th = cDecl(X, "thunk", {X.I32}, X.I32);
    Body(X, Th);
    CallTakes(X, Cb, constFunc(X.M, Th));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // a variable of the C function type: its type says it
    Type *Cb = X.C.func({X.I32}, X.I32, false, CallConv::C);
    CallTakes(X, Cb, copyOf(localPlace(X.local(Cb), Cb)));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // the Go++ function itself: not the parameter's type
    Type *Cb = X.C.func({X.I32}, X.I32, false, CallConv::C);
    FuncId G = declare(X.M, "gofn", X.C.func({X.I32}, X.I32));
    Body(X, G);
    CallTakes(X, Cb, constFunc(X.M, G));
    EXPECT_GIR_RULE(X.M, "V062");
  }
  {
    PlaceFixture X; // a C function taking a Go++ function pointer
    Type *GoCb = X.C.func({X.I32}, X.I32);
    FuncId G = declare(X.M, "gofn", GoCb);
    Body(X, G);
    CallTakes(X, GoCb, constFunc(X.M, G));
    EXPECT_GIR_RULE(X.M, "V067");
  }
  {
    PlaceFixture X; // Go++ calling Go++ with a Go++ function: free
    Type *Cb = X.C.func({X.I32}, X.I32);
    FuncId G = declare(X.M, "gotakes", X.C.func({Cb}, X.Unit));
    FuncBuilder GB(X.M, X.fn(G));
    GB.ret();
    FuncId F2 = declare(X.M, "gofn", Cb);
    Body(X, F2);
    BlockId B1 = X.fn(X.P).addBlock();
    X.fn(X.P).Blocks[B1].Term = term(Terminator::Return);
    LocalId T = X.local(X.Unit);
    X.fn(X.P).Blocks[0].Term = callOf(constFunc(X.M, G), {constFunc(X.M, F2)},
                                      localPlace(T, X.Unit), B1);
    EXPECT_GIR_VALID(X.M);
  }
}

TEST(GIRVerifier, V076_ACallThroughACFunctionPointerPassesScalars) {
  // P04.18: a call through a C function pointer (an allocator's table entry,
  // ADR-0029) is lowered with the C ABI of scalars; an aggregate needs
  // Clang's classification of a function value, which is P04.07's. A direct
  // call of a C function and a call through a Go++ function pointer take
  // anything.
  auto CallThrough = [](PlaceFixture &X, Type *Fn, std::vector<Operand> Args) {
    BlockId B1 = X.fn(X.P).addBlock();
    X.fn(X.P).Blocks[B1].Term = term(Terminator::Return);
    LocalId T = X.local(Fn->ret());
    X.fn(X.P).Blocks[0].Term =
        callOf(copyOf(localPlace(X.local(Fn), Fn)), std::move(Args),
               localPlace(T, Fn->ret()), B1);
  };
  {
    PlaceFixture X; // scalars: valid
    Type *Fn = X.C.func({X.PI64, X.U64, X.Bool}, X.PI64, false, CallConv::C);
    CallThrough(X, Fn,
                {copyOf(localPlace(X.LP, X.PI64)), constInt(X.U64, 1),
                 constBool(X.C, true)});
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // a struct parameter
    Type *Fn = X.C.func({X.Pair}, X.Unit, false, CallConv::C);
    CallThrough(X, Fn, {copyOf(localPlace(X.LS, X.Pair))});
    EXPECT_GIR_RULE(X.M, "V076");
  }
  {
    PlaceFixture X; // a struct result
    Type *Fn = X.C.func({}, X.Pair, false, CallConv::C);
    CallThrough(X, Fn, {});
    EXPECT_GIR_RULE(X.M, "V076");
  }
  {
    PlaceFixture X; // through a Go++ function pointer: anything
    Type *Fn = X.C.func({X.Pair}, X.Pair);
    CallThrough(X, Fn, {copyOf(localPlace(X.LS, X.Pair))});
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // a direct call of a C function with a struct
    FuncId F = cDecl(X, "takespair", {X.Pair}, X.Unit);
    BlockId B1 = X.fn(X.P).addBlock();
    X.fn(X.P).Blocks[B1].Term = term(Terminator::Return);
    LocalId T = X.local(X.Unit);
    X.fn(X.P).Blocks[0].Term =
        callOf(constFunc(X.M, F), {copyOf(localPlace(X.LS, X.Pair))},
               localPlace(T, X.Unit), B1);
    EXPECT_GIR_VALID(X.M);
  }
}

/// A map as the lowering makes it (P04.05): a pointer to gopp.map.
static Type *mapPtr(PlaceFixture &X) {
  Type *S = X.C.structTy("gopp.map");
  if (!S->hasBody())
    X.C.setStructBody(S, {X.I64});
  return X.C.ptr(S);
}

TEST(GIRVerifier, V067_CSignatures) {
  // Accepted: scalars, pointers, structs of these (nested, with arrays inside,
  // also of length 0, empty ones, one that points to itself); a struct or
  // nothing as the result; a C function with a body (the entry point) that
  // takes and returns scalars.
  {
    PlaceFixture X;
    Type *Outer =
        structOf(X, "COuter",
                 {X.Pair, X.C.array(X.Pair, 3), X.C.ptr(X.I64), X.F64, X.Bool,
                  X.C.array(X.I32, 0), X.C.ptr(X.C.ptr(X.I32))});
    Type *Empty = structOf(X, "CEmpty", {});
    Type *Node = X.C.structTy("test.CNode");
    X.C.setStructBody(Node, {X.C.ptr(Node), X.I64});
    cDecl(X, "ok1", {Outer, X.I32, Empty, Node}, Outer);
    cDecl(X, "ok2", {}, Empty);
    cDecl(X, "ok3", {X.Bool, X.F64, X.C.ptr(X.Pair)}, X.Unit);
    FuncId Main = cDecl(X, "cmain", {X.I32}, X.I32);
    FuncBuilder B(X.M, X.fn(Main));
    B.assign(B.local(0), B.use(constInt(X.I32, 0)));
    B.ret();
    EXPECT_GIR_VALID(X.M);
  }
  // Refused: each is V067.
  auto Bad = [](void (*Make)(PlaceFixture &)) {
    PlaceFixture X;
    Make(X);
    EXPECT_GIR_RULE(X.M, "V067");
  };
  // A string, a struct that holds one, a slice, a tuple result, unit as a
  // parameter.
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {X.Str}, X.Unit); });
  Bad([](PlaceFixture &X) {
    cDecl(X, "bad", {}, structOf(X, "HasStr", {X.I32, X.Str}));
  });
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {X.C.slice(X.I32)}, X.Unit); });
  // A map (P04.05: a pointer to the runtime's gopp.map object), as a
  // parameter, a result and a field; a pointer to a map variable is a plain
  // pointer and is accepted.
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {mapPtr(X)}, X.Unit); });
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {}, mapPtr(X)); });
  Bad([](PlaceFixture &X) {
    cDecl(X, "bad", {structOf(X, "HasMap", {X.I32, mapPtr(X)})}, X.Unit);
  });
  {
    PlaceFixture X;
    cDecl(X, "okmap", {X.C.ptr(mapPtr(X))}, X.I32);
    EXPECT_GIR_VALID(X.M);
  }
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {}, X.C.tuple({X.I32, X.I32})); });
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {X.Unit}, X.Unit); });
  // Option[T] by value (M6: no C type until P07), alone, as a result and in a
  // struct; Option[*T] is a pointer type and is accepted above.
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {X.C.option(X.I32)}, X.Unit); });
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {}, X.C.option(X.Pair)); });
  Bad([](PlaceFixture &X) {
    cDecl(X, "bad", {structOf(X, "HasOpt", {X.I32, X.C.option(X.I32)})},
          X.Unit);
  });
  // An array alone: C passes it as a pointer.
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {X.C.array(X.I32, 4)}, X.Unit); });
  // A function value is a C type as a parameter only, where it is a callback
  // whose own parameters and result are C types (P04.18); as a result, a
  // field, or a callback with a string, a callback taking a function or one
  // that fails, it is not.
  // (A callback is a C function pointer: its type has the C convention.)
  {
    PlaceFixture X;
    cDecl(X, "okcb", {cFn(X, {}, X.Unit)}, X.Unit);
    cDecl(X, "okcb2", {cFn(X, {X.Pair, X.I32}, X.Pair), X.I64}, X.I32);
    EXPECT_GIR_VALID(X.M);
  }
  // A Go++ function pointer: C would call it with the C ABI (V075).
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {X.C.func({}, X.Unit)}, X.Unit); });
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {}, cFn(X, {}, X.Unit)); });
  Bad([](PlaceFixture &X) {
    cDecl(X, "bad", {structOf(X, "HasCb", {cFn(X, {}, X.Unit)})}, X.Unit);
  });
  Bad([](PlaceFixture &X) {
    cDecl(X, "bad", {cFn(X, {X.Str}, X.Unit)}, X.Unit);
  });
  Bad([](PlaceFixture &X) { cDecl(X, "bad", {cFn(X, {}, X.Str)}, X.Unit); });
  Bad([](PlaceFixture &X) {
    cDecl(X, "bad", {cFn(X, {cFn(X, {}, X.Unit)}, X.Unit)}, X.Unit);
  });
  Bad([](PlaceFixture &X) {
    cDecl(X, "bad", {cFn(X, {}, X.Unit, /*Fails=*/true)}, X.Unit);
  });
  // An array of zero-size elements (Clang cannot classify it), also of
  // zero-length arrays.
  Bad([](PlaceFixture &X) {
    Type *E = structOf(X, "ZEmpty", {});
    cDecl(X, "bad", {structOf(X, "ZArr", {X.C.array(E, 2)})}, X.Unit);
  });
  Bad([](PlaceFixture &X) {
    Type *Z = X.C.array(X.I32, 0);
    cDecl(X, "bad", {structOf(X, "ZArr2", {X.C.array(Z, 2)})}, X.Unit);
  });
  // Over the limits: more than 4096 fields written out (a DAG that doubles),
  // more than 1 GiB.
  Bad([](PlaceFixture &X) {
    Type *Prev = X.U8;
    for (int I = 0; I < 12; ++I)
      Prev = structOf(X, "Dag" + std::to_string(I), {Prev, Prev});
    cDecl(X, "bad", {Prev}, X.Unit);
  });
  Bad([](PlaceFixture &X) {
    Type *Big = structOf(X, "Big", {X.C.array(X.U8, (uint64_t(1) << 30) + 1)});
    cDecl(X, "bad", {Big}, X.Unit);
  });
  // A C function with a body may take and return a struct: the thunk of an
  // @Export function or a callback (P04.18).
  {
    PlaceFixture X;
    FuncId Id = cDecl(X, "cb", {X.Pair}, X.Unit);
    FuncBuilder B(X.M, X.fn(Id));
    B.ret();
    EXPECT_GIR_VALID(X.M);
  }
  // ... but not a type that is no C type (a string).
  Bad([](PlaceFixture &X) {
    FuncId Id = cDecl(X, "cb", {X.Str}, X.Unit);
    FuncBuilder B(X.M, X.fn(Id));
    B.ret();
  });
  // Nesting: 64 levels are a C type, 65 are not.
  for (unsigned Depth : {64u, 65u}) {
    PlaceFixture X;
    Type *Cur = X.I64;
    for (unsigned I = 0; I < Depth; ++I)
      Cur = structOf(X, "L" + std::to_string(I), {Cur});
    cDecl(X, "c", {Cur}, X.Unit);
    if (Depth == 64)
      EXPECT_GIR_VALID(X.M);
    else
      EXPECT_GIR_RULE(X.M, "V067");
  }
  // The Go convention takes anything.
  {
    PlaceFixture X;
    X.M.func(declare(X.M, "go", X.C.func({X.Str}, X.Str))).CC = CallConv::Go;
    EXPECT_GIR_VALID(X.M);
  }
}

Global globalOf(const char *Name, Type *Ty, Constant Init = Constant()) {
  Global G;
  G.Name = Name;
  G.Ty = Ty;
  G.Init = Init;
  return G;
}

TEST(GIRVerifier, V071_Globals) {
  {
    PlaceFixture X; // zero, an int, a string and a float are fine
    X.M.addGlobal(globalOf("vz", X.Pair));
    X.M.addGlobal(globalOf("vi", X.I64, constInt(X.I64, 7).C));
    X.M.addGlobal(globalOf("vs", X.Str, constStr(X.M, "text").C));
    X.M.addGlobal(globalOf("vf", X.F64, constFloat(X.F64, 0.5).C));
    X.M.addGlobal(globalOf("vb", X.Bool, constBool(X.C, true).C));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // a name that a function has
    X.M.addGlobal(globalOf("f", X.I64));
    EXPECT_GIR_RULE(X.M, "V071");
  }
  {
    PlaceFixture X; // two globals with one name
    X.M.addGlobal(globalOf("gl", X.I64));
    X.M.addGlobal(globalOf("gl", X.I64));
    EXPECT_GIR_RULE(X.M, "V071");
  }
  {
    PlaceFixture X; // an initializer of another type
    X.M.addGlobal(globalOf("gl", X.I64, constInt(X.I32, 7).C));
    EXPECT_GIR_RULE(X.M, "V071");
  }
  {
    PlaceFixture X; // an initializer that is an address
    X.M.addGlobal(globalOf("gl", X.C.ptr(X.I64), constFunc(X.M, X.F).C));
    EXPECT_GIR_RULE(X.M, "V071");
  }
  {
    PlaceFixture X; // a global of the unit type holds nothing
    X.M.addGlobal(globalOf("gl", X.Unit));
    EXPECT_GIR_RULE(X.M, "V071");
  }
  {
    PlaceFixture X; // an aggregate starts at zero (its init function sets it)
    Constant One = constInt(X.I64, 1).C;
    One.Ty = X.Pair;
    X.M.addGlobal(globalOf("gl", X.Pair, One));
    EXPECT_GIR_RULE(X.M, "V071");
  }
}

TEST(GIRVerifier, V072_StaticData) {
  auto Data = [](PlaceFixture &X, const char *Name, Type *Ty,
                 std::vector<Constant> Parts) {
    Global G = globalOf(Name, Ty);
    G.Parts = std::move(Parts);
    X.M.addGlobal(std::move(G));
  };
  Type *PU8 = nullptr;
  {
    // The good side: an itab-like array of pointers (a global, a function,
    // null), a descriptor-like tuple (a pointer, an integer, a string, a
    // whole zero part), and an array of strings.
    PlaceFixture X;
    PU8 = X.C.ptr(X.C.u8());
    uint32_t Tab = X.M.addGlobal(globalOf("tab", X.I64));
    Data(X, "itab", X.C.array(PU8, 3),
         {constGlobal(X.M, Tab).C, constFunc(X.M, X.F).C, nullOf(PU8).C});
    Data(X, "desc", X.C.tuple({PU8, X.U64, X.Str, X.Pair}),
         {constGlobal(X.M, Tab).C, constInt(X.U64, 5).C,
          constStr(X.M, "name").C, Constant{}});
    {
      Global &Z = X.M.Globals.back();
      Z.Parts.back().K = Constant::Zero;
      Z.Parts.back().Ty = X.Pair;
    }
    Data(X, "keys", X.C.array(X.Str, 2),
         {constStr(X.M, "a").C, constStr(X.M, "b").C});
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // too few parts
    PU8 = X.C.ptr(X.C.u8());
    Data(X, "itab", X.C.array(PU8, 3), {nullOf(PU8).C, nullOf(PU8).C});
    EXPECT_GIR_RULE(X.M, "V072");
  }
  {
    PlaceFixture X; // too many parts
    PU8 = X.C.ptr(X.C.u8());
    Data(X, "itab", X.C.array(PU8, 1), {nullOf(PU8).C, nullOf(PU8).C});
    EXPECT_GIR_RULE(X.M, "V072");
  }
  {
    PlaceFixture X; // an integer where a pointer goes
    PU8 = X.C.ptr(X.C.u8());
    Data(X, "itab", X.C.array(PU8, 1), {constInt(X.U64, 1).C});
    EXPECT_GIR_RULE(X.M, "V072");
  }
  {
    PlaceFixture X; // a string where an integer goes
    Data(X, "d", X.C.tuple({X.U64}), {constStr(X.M, "s").C});
    EXPECT_GIR_RULE(X.M, "V072");
  }
  {
    PlaceFixture X; // an integer of another width
    Data(X, "d", X.C.tuple({X.U64}), {constInt(X.I32, 1).C});
    EXPECT_GIR_RULE(X.M, "V072");
  }
  {
    PlaceFixture X; // static data and a scalar initializer
    Global G = globalOf("d", X.I64, constInt(X.I64, 7).C);
    G.Parts = {constInt(X.I64, 7).C};
    X.M.addGlobal(std::move(G));
    EXPECT_GIR_RULE(X.M, "V072");
  }
  {
    PlaceFixture X; // a function in a func-typed field is the same function
    Type *Fn = X.F ? X.M.func(X.F).Sig : nullptr;
    ASSERT_NE(Fn, nullptr);
    Data(X, "d", X.C.tuple({Fn}), {constFunc(X.M, X.F).C});
    EXPECT_GIR_VALID(X.M);
  }
}

TEST(GIRVerifier, GlobalAddressesAreConstants) {
  {
    PlaceFixture X;
    uint32_t G = X.M.addGlobal(globalOf("gl", X.I64));
    // _t = &g; *_t = 5; the address is a pointer to the global's type.
    Operand Addr = constGlobal(X.M, G);
    EXPECT_EQ(Addr.type(), X.C.ptr(X.I64));
    LocalId T = X.local(Addr.type());
    X.append(X.P, 0, localPlace(T, Addr.type()), useOf(Addr, Addr.type()));
    X.store(placeOf(T, X.I64, {elem(PlaceElem::Deref, 0, X.I64)}),
            constInt(X.I64, 5));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // a global that does not exist
    Operand Addr;
    Addr.C.K = Constant::Global;
    Addr.C.Bits = 3;
    Addr.C.Ty = X.C.ptr(X.I64);
    X.set(Addr.C.Ty, useOf(Addr, Addr.C.Ty));
    EXPECT_GIR_RULE(X.M, "V040");
  }
  {
    PlaceFixture X; // an address of the wrong type
    uint32_t G = X.M.addGlobal(globalOf("gl", X.I64));
    Operand Addr = constGlobal(X.M, G);
    Addr.C.Ty = X.C.ptr(X.I32);
    X.set(Addr.C.Ty, useOf(Addr, Addr.C.Ty));
    EXPECT_GIR_RULE(X.M, "V040");
  }
}

TEST(GIRVerifier, OptionIsATupleOfValueAndFlag) {
  {
    PlaceFixture X;
    Type *O = X.C.option(X.I64);
    EXPECT_EQ(O->numFields(), 2u);
    EXPECT_EQ(O->fieldType(0, X.C), X.I64);
    EXPECT_EQ(O->fieldType(1, X.C), X.Bool);
    X.set(O,
          agg(AggKind::Tuple, O, {constInt(X.I64, 3), constBool(X.C, true)}));
    X.set(O, useOf(zeroOf(O), O));
    LocalId Opt = X.local(O);
    X.store(localPlace(Opt, O), zeroOf(O));
    X.set(X.Bool, useOf(copyOf(placeOf(Opt, X.Bool,
                                       {elem(PlaceElem::Field, 1, X.Bool)})),
                        X.Bool));
    X.set(X.I64,
          useOf(copyOf(placeOf(Opt, X.I64, {elem(PlaceElem::Field, 0, X.I64)})),
                X.I64));
    EXPECT_GIR_VALID(X.M);
  }
  {
    PlaceFixture X; // the flag has the wrong type
    Type *O = X.C.option(X.I64);
    X.set(O, agg(AggKind::Tuple, O, {constInt(X.I64, 3), constInt(X.I64, 1)}));
    EXPECT_GIR_RULE(X.M, "V055");
  }
  {
    PlaceFixture X; // a field that Option does not have
    Type *O = X.C.option(X.I64);
    LocalId Opt = X.local(O);
    X.set(X.I64,
          useOf(copyOf(placeOf(Opt, X.I64, {elem(PlaceElem::Field, 2, X.I64)})),
                X.I64));
    EXPECT_GIR_RULE(X.M, "V032");
  }
}

TEST(GIRVerifier, V077_TheEntryIsTheFirstStatementsOfTheEntryBlock) {
  // P04.12: the statements of a function's entry (Statement::Prologue: a
  // borrowed string's or slice's variable, a named result's zero value) have
  // no line, so `break f` stops after them; they must all come first.
  // f's entry block has one statement (t = a + 1), its `then` block one.
  auto Entry = [](GoodModule &G) -> std::vector<Statement> & {
    return G.fn(G.F).Blocks[0].Stmts;
  };
  auto Nop = [] {
    Statement S;
    S.K = Statement::Nop;
    return S;
  };
  {
    GoodModule G; // the first statement of the entry block: valid
    Entry(G)[0].Prologue = true;
    EXPECT_GIR_VALID(G.M);
  }
  {
    GoodModule G; // two in a row, then the body: valid
    Statement P = Nop();
    P.Prologue = true;
    Entry(G)[0].Prologue = true;
    Entry(G).insert(Entry(G).begin(), P);
    EXPECT_GIR_VALID(G.M);
  }
  {
    GoodModule G; // after a statement of the body
    Entry(G).insert(Entry(G).begin(), Nop());
    Entry(G)[1].Prologue = true;
    EXPECT_GIR_RULE(G.M, "V077");
  }
  {
    GoodModule G; // the body, then the entry again
    Statement P = Nop();
    P.Prologue = true;
    Entry(G).push_back(P);
    EXPECT_GIR_RULE(G.M, "V077");
  }
  {
    GoodModule G; // outside the entry block (f's `then` block)
    G.fn(G.F).Blocks[1].Stmts[0].Prologue = true;
    EXPECT_GIR_RULE(G.M, "V077");
  }
}

} // namespace
