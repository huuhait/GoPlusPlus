//===- gopp/GIR/Builder.h - Making GIR bodies -------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.01. The one way to make a body: the lowering of the checked AST
// (Lower.h), the helper functions the compiler writes in GIR, and the later
// passes (drop elaboration, monomorphization) all use it. It computes the
// result type of every rvalue, so the types a body carries are made in one
// place, and the verifier checks them again.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_GIR_BUILDER_H
#define GOPP_GIR_BUILDER_H

#include "gopp/GIR/GIR.h"

namespace gopp {
namespace gir {

class FuncBuilder {
public:
  /// Builds the body of F, which gets the return place and the parameter
  /// locals now and block 0 as the current block.
  FuncBuilder(Module &M, Function &F);

  Module &module() { return M; }
  Context &ctx() { return M.Ctx; }
  Function &function() { return F; }

  // Locals and places.
  LocalId newLocal(Type *Ty, Local::Kind K, llvm::StringRef Name = "",
                   bool Mutable = true);
  LocalId newTemp(Type *Ty) { return newLocal(Ty, Local::Temp); }
  Place local(LocalId L) const;
  Place deref(Place P);
  Place field(Place P, unsigned I);
  Place index(Place P, LocalId Idx);
  Place constIndex(Place P, uint32_t I);
  LocalId returnPlace() const { return 0; }
  LocalId param(unsigned I) const { return I + 1; }

  // Blocks.
  BlockId newBlock(bool Cleanup = false) { return F.addBlock(Cleanup); }
  /// A catch block (BasicBlock::Catch): the catch-all handler of `defer`.
  BlockId newCatchBlock() { return F.addBlock(true, true); }
  /// A new block of the same kind (normal, cleanup, catch) as the current one.
  BlockId newBlockLikeCurrent() {
    return F.addBlock(F.Blocks[Cur].Cleanup, F.Blocks[Cur].Catch);
  }
  void setBlock(BlockId B) { Cur = B; }
  BlockId block() const { return Cur; }
  /// The current block has its terminator.
  bool terminated() const { return F.Blocks[Cur].Term.Set; }

  /// The span given to the statements and terminators made from now on.
  void setSpan(Span S) { Loc = S; }
  Span span() const { return Loc; }

  // Statements.
  void assign(Place Dest, Rvalue RV);
  void storageLive(LocalId L);
  void storageDead(LocalId L);

  // Rvalues (each knows its type).
  Rvalue use(Operand A);
  Rvalue binary(BinOp Op, Operand A, Operand B);
  Rvalue unary(UnOp Op, Operand A);
  Rvalue cast(CastKind K, Operand A, Type *To);
  Rvalue addrOf(Place P, bool Mut = false);
  Rvalue aggregate(AggKind K, Type *Ty, llvm::ArrayRef<Operand> Ops);
  Rvalue len(Place P);

  /// Dest = Rv in one step, through a new temporary; returns it as a place.
  Place temp(Rvalue RV);

  // Terminators. Each one ends the current block.
  void gotoBlock(BlockId Target);
  /// if Cond goto Then else goto Else (a switch on a bool).
  void branch(Operand Cond, BlockId Then, BlockId Else);
  void switchInt(Operand Discr, llvm::ArrayRef<SwitchCase> Cases,
                 BlockId Otherwise);
  void ret();
  void unreachable();
  void resume();
  /// Dest = Callee(Args); the call continues in Next, which is made if
  /// NoId is passed (and becomes the current block). Returns the block the
  /// call continues in.
  BlockId call(Operand Callee, llvm::ArrayRef<Operand> Args, Place Dest,
               UnwindAction Unwind = {}, BlockId Next = NoId);
  BlockId assertThat(Operand Cond, bool Expected, AssertKind K,
                     UnwindAction Unwind = {}, BlockId Next = NoId);
  BlockId drop(Place P, UnwindAction Unwind = {}, BlockId Next = NoId);

private:
  void terminate(Terminator T);
  Module &M;
  Function &F;
  BlockId Cur = 0;
  Span Loc;
};

/// Removes the blocks that no edge reaches (D4.2 step 6) and renumbers the
/// rest in their old order.
void removeDeadBlocks(Function &F);

} // namespace gir
} // namespace gopp

#endif // GOPP_GIR_BUILDER_H
