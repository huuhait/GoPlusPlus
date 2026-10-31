//===- Builder.cpp - Making GIR bodies ------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Builder.h"

using namespace gopp;
using namespace gopp::gir;
using llvm::ArrayRef;

FuncBuilder::FuncBuilder(Module &M, Function &F) : M(M), F(F) {
  assert(F.Locals.empty() && F.Blocks.empty() && "the body is made once");
  Local Ret;
  Ret.K = Local::Return;
  Ret.Ty = F.Sig->ret();
  F.addLocal(std::move(Ret));
  for (Type *P : F.Sig->elems()) {
    Local L;
    L.K = Local::Arg;
    L.Ty = P;
    F.addLocal(std::move(L));
  }
  Cur = F.addBlock();
}

LocalId FuncBuilder::newLocal(Type *Ty, Local::Kind K, llvm::StringRef Name,
                              bool Mutable) {
  Local L;
  L.K = K;
  L.Ty = Ty;
  L.Name = Name.str();
  L.Mutable = Mutable;
  L.Loc = Loc;
  L.Scope = Scope;
  return F.addLocal(std::move(L));
}

ScopeId FuncBuilder::newScope() {
  DebugScope S;
  S.K = DebugScope::Block;
  S.Parent = Scope;
  S.Loc = Loc;
  return F.addScope(S);
}

Place FuncBuilder::local(LocalId L) const {
  return localPlace(L, F.Locals[L].Ty);
}

Place FuncBuilder::deref(Place P) {
  assert(P.Ty->isPtr() && "deref of a non-pointer");
  PlaceElem E;
  E.K = PlaceElem::Deref;
  E.Ty = P.Ty->elem();
  P.Proj.push_back(E);
  P.Ty = E.Ty;
  return P;
}

Place FuncBuilder::global(uint32_t G) {
  Place Addr = temp(use(constGlobal(M, G)));
  return deref(Addr);
}

Place FuncBuilder::field(Place P, unsigned I) {
  PlaceElem E;
  E.K = PlaceElem::Field;
  E.Val = I;
  E.Ty = P.Ty->fieldType(I, M.Ctx);
  assert(E.Ty && "no such field");
  P.Proj.push_back(E);
  P.Ty = E.Ty;
  return P;
}

Place FuncBuilder::index(Place P, LocalId Idx) {
  PlaceElem E;
  E.K = PlaceElem::Index;
  E.Val = Idx;
  E.Ty = P.Ty->getKind() == Type::Kind::String ? M.Ctx.u8() : P.Ty->elem();
  P.Proj.push_back(E);
  P.Ty = E.Ty;
  return P;
}

Place FuncBuilder::constIndex(Place P, uint32_t I) {
  PlaceElem E;
  E.K = PlaceElem::ConstIndex;
  E.Val = I;
  E.Ty = P.Ty->elem();
  P.Proj.push_back(E);
  P.Ty = E.Ty;
  return P;
}

void FuncBuilder::assign(Place Dest, Rvalue RV) {
  assert(!terminated() && "statement after the terminator");
  Statement S;
  S.K = Statement::Assign;
  S.Dest = std::move(Dest);
  S.RV = std::move(RV);
  S.Loc = Loc;
  S.Scope = Scope;
  F.Blocks[Cur].Stmts.push_back(std::move(S));
}

void FuncBuilder::storageLive(LocalId L) {
  Statement S;
  S.K = Statement::StorageLive;
  S.L = L;
  S.Loc = Loc;
  S.Scope = Scope;
  F.Blocks[Cur].Stmts.push_back(std::move(S));
}

void FuncBuilder::storageDead(LocalId L) {
  Statement S;
  S.K = Statement::StorageDead;
  S.L = L;
  S.Loc = Loc;
  S.Scope = Scope;
  F.Blocks[Cur].Stmts.push_back(std::move(S));
}

Rvalue FuncBuilder::use(Operand A) {
  Rvalue R;
  R.K = Rvalue::Use;
  R.Ty = A.type();
  R.A = std::move(A);
  return R;
}

Rvalue FuncBuilder::binary(BinOp Op, Operand A, Operand B) {
  Rvalue R;
  R.K = Rvalue::Binary;
  R.Bin = Op;
  R.Ty = isComparison(Op) ? M.Ctx.boolean() : A.type();
  R.A = std::move(A);
  R.B = std::move(B);
  return R;
}

Rvalue FuncBuilder::unary(UnOp Op, Operand A) {
  Rvalue R;
  R.K = Rvalue::Unary;
  R.Un = Op;
  R.Ty = A.type();
  R.A = std::move(A);
  return R;
}

Rvalue FuncBuilder::cast(CastKind K, Operand A, Type *To) {
  Rvalue R;
  R.K = Rvalue::Cast;
  R.CastOp = K;
  R.Ty = To;
  R.A = std::move(A);
  return R;
}

Rvalue FuncBuilder::addrOf(Place P, bool Mut) {
  Rvalue R;
  R.K = Rvalue::AddrOf;
  R.Ty = M.Ctx.ptr(P.Ty);
  R.Mut = Mut;
  R.P = std::move(P);
  return R;
}

Rvalue FuncBuilder::aggregate(AggKind K, Type *Ty, ArrayRef<Operand> Ops) {
  Rvalue R;
  R.K = Rvalue::Aggregate;
  R.Agg = K;
  R.Ty = Ty;
  R.Ops.assign(Ops.begin(), Ops.end());
  return R;
}

Rvalue FuncBuilder::len(Place P) {
  Rvalue R;
  R.K = Rvalue::Len;
  R.Ty = M.Ctx.u64();
  R.P = std::move(P);
  return R;
}

Place FuncBuilder::temp(Rvalue RV) {
  LocalId T = newTemp(RV.Ty);
  Place P = local(T);
  assign(P, std::move(RV));
  return P;
}

void FuncBuilder::terminate(Terminator T) {
  assert(!terminated() && "block already has a terminator");
  T.Loc = Loc;
  T.Scope = Scope;
  T.Set = true;
  F.Blocks[Cur].Term = std::move(T);
}

void FuncBuilder::gotoBlock(BlockId Target) {
  Terminator T;
  T.K = Terminator::Goto;
  T.Target = Target;
  terminate(std::move(T));
}

void FuncBuilder::branch(Operand Cond, BlockId Then, BlockId Else) {
  Terminator T;
  T.K = Terminator::Switch;
  T.Discr = std::move(Cond);
  T.Cases.push_back({0, Else});
  T.Otherwise = Then;
  terminate(std::move(T));
}

void FuncBuilder::switchInt(Operand Discr, ArrayRef<SwitchCase> Cases,
                            BlockId Otherwise) {
  Terminator T;
  T.K = Terminator::Switch;
  T.Discr = std::move(Discr);
  T.Cases.assign(Cases.begin(), Cases.end());
  T.Otherwise = Otherwise;
  terminate(std::move(T));
}

void FuncBuilder::ret() {
  Terminator T;
  T.K = Terminator::Return;
  terminate(std::move(T));
}

void FuncBuilder::unreachable() {
  Terminator T;
  T.K = Terminator::Unreachable;
  terminate(std::move(T));
}

void FuncBuilder::resume() {
  Terminator T;
  T.K = Terminator::Resume;
  terminate(std::move(T));
}

BlockId FuncBuilder::call(Operand Callee, ArrayRef<Operand> Args, Place Dest,
                          UnwindAction Unwind, BlockId Next) {
  if (Next == NoId)
    Next = newBlockLikeCurrent();
  Terminator T;
  T.K = Terminator::Call;
  T.Discr = std::move(Callee);
  T.Args.assign(Args.begin(), Args.end());
  T.Dest = std::move(Dest);
  T.Target = Next;
  T.Unwind = Unwind;
  terminate(std::move(T));
  Cur = Next;
  return Next;
}

BlockId FuncBuilder::assertThat(Operand Cond, bool Expected, AssertKind K,
                                UnwindAction Unwind, BlockId Next,
                                llvm::ArrayRef<Operand> Args) {
  if (Next == NoId)
    Next = newBlockLikeCurrent();
  Terminator T;
  T.K = Terminator::Assert;
  T.Discr = std::move(Cond);
  T.Expected = Expected;
  T.AK = K;
  T.Target = Next;
  T.Unwind = Unwind;
  T.Args.assign(Args.begin(), Args.end());
  terminate(std::move(T));
  Cur = Next;
  return Next;
}

BlockId FuncBuilder::drop(Place P, UnwindAction Unwind, BlockId Next,
                          FuncId Glue) {
  if (Next == NoId)
    Next = newBlockLikeCurrent();
  Terminator T;
  T.K = Terminator::Drop;
  T.Dest = std::move(P);
  if (Glue != NoId) {
    T.Discr = constFunc(M, Glue);
  } else {
    T.Discr.K = Operand::Const;
    T.Discr.C.K = Constant::Zero;
    T.Discr.C.Ty = M.Ctx.unit();
  }
  T.Target = Next;
  T.Unwind = Unwind;
  terminate(std::move(T));
  Cur = Next;
  return Next;
}

void gir::removeDeadBlocks(Function &F) {
  if (F.Blocks.empty())
    return;
  std::vector<uint8_t> Live(F.Blocks.size(), 0);
  std::vector<BlockId> Work{0};
  Live[0] = 1;
  while (!Work.empty()) {
    BlockId B = Work.back();
    Work.pop_back();
    if (!F.Blocks[B].Term.Set)
      continue;
    forEachSuccessor(F.Blocks[B].Term, [&](BlockId &T) {
      if (T < Live.size() && !Live[T]) {
        Live[T] = 1;
        Work.push_back(T);
      }
    });
  }
  std::vector<BlockId> NewId(F.Blocks.size(), NoId);
  BlockId N = 0;
  for (BlockId B = 0; B < F.Blocks.size(); ++B)
    if (Live[B])
      NewId[B] = N++;
  if (N == F.Blocks.size())
    return;
  std::vector<BasicBlock> Kept;
  Kept.reserve(N);
  for (BlockId B = 0; B < F.Blocks.size(); ++B)
    if (Live[B])
      Kept.push_back(std::move(F.Blocks[B]));
  F.Blocks = std::move(Kept);
  for (BasicBlock &BB : F.Blocks)
    if (BB.Term.Set)
      forEachSuccessor(BB.Term, [&](BlockId &T) { T = NewId[T]; });
}
