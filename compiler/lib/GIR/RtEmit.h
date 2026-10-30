//===- RtEmit.h - A small emitter for the runtime functions ---------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The bootstrap runtime (strings, slices, maps, the allocator entry point) is
// written in GIR, function by function, when a program first needs it
// (RuntimeStr.cpp, RuntimeMap.cpp). Written with FuncBuilder alone these are
// pages of temporaries, so this header gives them a tiny expression language:
//
//   Val   a typed operand; `a + b`, `a < b`, ... make a temporary
//   Var   a mutable local; `x = v`
//   If, IfElse, While   structured control flow over lambdas
//
// Nothing here is a second IR: every call ends in FuncBuilder, and the
// verifier checks the result like any other GIR.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_LIB_GIR_RTEMIT_H
#define GOPP_LIB_GIR_RTEMIT_H

#include "gopp/GIR/Builder.h"

#include <functional>
#include <memory>

namespace gopp {
namespace gir {
namespace impl {

class Rt;

/// A value: an operand of a known type. Arithmetic needs the same type on
/// both sides (an integer literal on the right takes the left's type).
struct Val {
  Rt *R = nullptr;
  Operand Op;
  Type *ty() const { return Op.type(); }
};

/// A mutable local of the function.
struct Var {
  Rt *R = nullptr;
  LocalId L = NoId;
  Var &operator=(const Val &V);
  operator Val() const;
  Val v() const { return *this; }
};

class Rt {
public:
  /// A function of its own (the runtime functions).
  Rt(Module &M, Function &F)
      : M(M), C(M.Ctx), OwnFB(std::make_unique<FuncBuilder>(M, F)), FB(*OwnFB),
        F(F) {
    init();
  }
  /// The code goes into the function another builder is filling (the
  /// lowering of a body): straight-line code and local control flow.
  Rt(Module &M, FuncBuilder &Shared)
      : M(M), C(M.Ctx), FB(Shared), F(Shared.function()) {
    init();
  }
  void init() {
    U8 = C.u8();
    U64 = C.u64();
    I64 = C.i64();
    I32 = C.i32();
    Bool = C.boolean();
    PU8 = C.ptr(U8);
  }

  Module &M;
  Context &C;
  std::unique_ptr<FuncBuilder> OwnFB;
  FuncBuilder &FB;
  Function &F;
  Type *U8, *U64, *I64, *I32, *Bool, *PU8;

  // Constants.
  Val k(Type *T, uint64_t V) { return {this, constInt(T, V)}; }
  Val u(uint64_t V) { return k(U64, V); }
  Val i(int64_t V) { return k(I64, uint64_t(V)); }
  Val i32(int64_t V) { return k(I32, uint64_t(V)); }
  Val b8(uint64_t V) { return k(U8, V); }
  Val boolean(bool V) { return {this, constBool(C, V)}; }
  Val null(Type *Ptr) {
    Operand O;
    O.K = Operand::Const;
    O.C.K = Constant::Null;
    O.C.Ty = Ptr;
    return {this, O};
  }
  Val zero(Type *T) {
    Operand O;
    O.K = Operand::Const;
    O.C.K = Constant::Zero;
    O.C.Ty = T;
    return {this, O};
  }
  Val func(FuncId Id) { return {this, constFunc(M, Id)}; }

  // Parameters and locals.
  Val param(unsigned N) { return {this, copyOf(FB.local(FB.param(N)))}; }
  Place paramPlace(unsigned N) { return FB.local(FB.param(N)); }
  Var var(Type *T, const char *Name = "") {
    return {this, FB.newLocal(T, Local::Var, Name)};
  }
  Var var(Type *T, Val Init, const char *Name = "") {
    Var X = var(T, Name);
    X = Init;
    return X;
  }
  Val val(const Place &P) { return {this, copyOf(P)}; }
  /// Field K of an aggregate value that lives in a place (a call's result,
  /// a variable).
  Val fld(Val V, unsigned K) {
    Place P = V.Op.K == Operand::Const ? FB.temp(FB.use(V.Op)) : V.Op.P;
    return val(FB.field(P, K));
  }
  /// Marks the temporary that holds V as an owned value (P04.09 drops it
  /// unless it is moved).
  Val owned(Val V) {
    F.Locals[V.Op.P.Local].Owned = true;
    return V;
  }
  Val str(Val P, Val Len, Val Cap) {
    return tmp(
        FB.aggregate(AggKind::String, C.string(), {P.Op, Len.Op, Cap.Op}));
  }
  Val slice(Type *Elem, Val P, Val Len, Val Cap) {
    return tmp(
        FB.aggregate(AggKind::Slice, C.slice(Elem), {P.Op, Len.Op, Cap.Op}));
  }
  Val tuple(Type *T, std::initializer_list<Val> Parts) {
    llvm::SmallVector<Operand, 4> Ops;
    for (const Val &V : Parts)
      Ops.push_back(V.Op);
    return tmp(FB.aggregate(AggKind::Tuple, T, Ops));
  }
  Val tmp(Rvalue RV) { return {this, copyOf(FB.temp(std::move(RV)))}; }
  /// The value as a new temporary (a snapshot of a variable).
  Val snap(Val V) { return tmp(FB.use(V.Op)); }

  // Arithmetic.
  Val bin(BinOp Op, Val A, Val B) { return tmp(FB.binary(Op, A.Op, B.Op)); }
  Val un(UnOp Op, Val A) { return tmp(FB.unary(Op, A.Op)); }
  Val not_(Val A) { return un(UnOp::Not, A); }
  /// A converted to T: integers, pointers and floats, by their kinds.
  Val to(Type *T, Val A) {
    Type *S = A.ty();
    if (S == T)
      return A;
    CastKind K;
    if (S->isInt() && T->isInt())
      K = CastKind::IntToInt;
    else if (S->isPtr() && T->isPtr())
      K = CastKind::PtrToPtr;
    else if (S->isPtr() && T->isInt())
      K = CastKind::PtrToInt;
    else if (S->isInt() && T->isPtr())
      K = CastKind::IntToPtr;
    else if (S->isInt() && T->isFloat())
      K = CastKind::IntToFloat;
    else if (S->isFloat() && T->isInt())
      K = CastKind::FloatToInt;
    else
      K = CastKind::FloatToFloat;
    return tmp(FB.cast(K, A.Op, T));
  }

  // Memory. A pointer is any pointer type; the access says what it reads.
  /// ptr + Bytes (bytes).
  Val padd(Val P, Val Bytes) {
    Val A = to(U64, P);
    return to(P.ty(), bin(BinOp::Add, A, to(U64, Bytes)));
  }
  Place at(Val P, Type *T) {
    Val Q = to(C.ptr(T), P);
    return FB.deref(FB.local(localOf(Q)));
  }
  /// The value at P, read now (a snapshot, not a lazy place).
  Val load(Val P, Type *T) { return snap(val(at(P, T))); }
  void store(Val P, Type *T, Val V) { FB.assign(at(P, T), FB.use(V.Op)); }
  Val addrOf(Place P) { return tmp(FB.addrOf(std::move(P))); }
  /// The address of a local variable.
  Val addrOf(Var X) { return addrOf(FB.local(X.L)); }

  // Calls.
  Val call(FuncId Id, std::initializer_list<Val> Args) {
    Type *Ret = M.func(Id).Sig->ret();
    SmallVec Ops;
    for (const Val &A : Args)
      Ops.push_back(A.Op);
    LocalId R = FB.newTemp(Ret);
    FB.call(constFunc(M, Id), Ops, FB.local(R), {UnwindAction::Continue, NoId});
    return {this, copyOf(FB.local(R))};
  }
  /// A call of a C function (it cannot unwind).
  Val callC(FuncId Id, std::initializer_list<Val> Args) {
    Type *Ret = M.func(Id).Sig->ret();
    SmallVec Ops;
    for (const Val &A : Args)
      Ops.push_back(A.Op);
    LocalId R = FB.newTemp(Ret);
    FB.call(constFunc(M, Id), Ops, FB.local(R),
            {UnwindAction::Unreachable, NoId});
    return {this, copyOf(FB.local(R))};
  }
  /// A call that does not return.
  void callNoReturn(FuncId Id, std::initializer_list<Val> Args) {
    SmallVec Ops;
    for (const Val &A : Args)
      Ops.push_back(A.Op);
    FB.call(constFunc(M, Id), Ops, FB.local(FB.newTemp(M.func(Id).Sig->ret())),
            {UnwindAction::Unreachable, NoId});
    FB.unreachable();
    FB.setBlock(FB.newBlock()); // dead code after it is removed with the block
  }

  // Control flow.
  using Body = std::function<void()>;
  /// A && B and A || B: B is evaluated only when needed.
  Val land(Val A, const std::function<Val()> &B) {
    Var R_ = var(Bool);
    R_ = A;
    If(R_, [&] { R_ = B(); });
    return snap(R_);
  }
  Val lor(Val A, const std::function<Val()> &B) {
    Var R_ = var(Bool);
    R_ = A;
    If(not_(R_), [&] { R_ = B(); });
    return snap(R_);
  }
  void If(Val Cond, const Body &Then) {
    BlockId T = FB.newBlock(), J = FB.newBlock();
    FB.branch(Cond.Op, T, J);
    FB.setBlock(T);
    Then();
    if (!FB.terminated())
      FB.gotoBlock(J);
    FB.setBlock(J);
  }
  void IfElse(Val Cond, const Body &Then, const Body &Else) {
    BlockId T = FB.newBlock(), E = FB.newBlock(), J = FB.newBlock();
    FB.branch(Cond.Op, T, E);
    FB.setBlock(T);
    Then();
    if (!FB.terminated())
      FB.gotoBlock(J);
    FB.setBlock(E);
    Else();
    if (!FB.terminated())
      FB.gotoBlock(J);
    FB.setBlock(J);
  }
  /// while Cond() { Body() }; Break leaves the loop from inside Body.
  void While(const std::function<Val()> &Cond, const Body &Loop) {
    BlockId H = FB.newBlock(), Bd = FB.newBlock(), X = FB.newBlock();
    FB.gotoBlock(H);
    FB.setBlock(H);
    Val C = Cond();
    FB.branch(C.Op, Bd, X);
    FB.setBlock(Bd);
    Exits.push_back({X, H});
    Loop();
    Exits.pop_back();
    if (!FB.terminated())
      FB.gotoBlock(H);
    FB.setBlock(X);
  }
  /// Leaves the innermost While.
  void Break() {
    FB.gotoBlock(Exits.back().first);
    FB.setBlock(FB.newBlock());
  }
  void Continue() {
    FB.gotoBlock(Exits.back().second);
    FB.setBlock(FB.newBlock());
  }
  void Return() {
    FB.ret();
    FB.setBlock(FB.newBlock());
  }
  /// Return V (a scalar result: stored in the return place).
  void Return(Val V) {
    FB.assign(FB.local(FB.returnPlace()), FB.use(V.Op));
    Return();
  }
  /// End of a function: the current block returns (if it is not done).
  void finish() {
    if (!FB.terminated())
      FB.ret();
    removeDeadBlocks(F);
  }

  using SmallVec = llvm::SmallVector<Operand, 4>;

private:
  LocalId localOf(const Val &V) {
    if (V.Op.K == Operand::Copy && V.Op.P.Proj.empty())
      return V.Op.P.Local;
    LocalId T = FB.newTemp(V.ty());
    FB.assign(FB.local(T), FB.use(V.Op));
    return T;
  }
  std::vector<std::pair<BlockId, BlockId>> Exits;
};

inline Var &Var::operator=(const Val &V) {
  R->FB.assign(R->FB.local(L), R->FB.use(V.Op));
  return *this;
}
inline Var::operator Val() const { return R->val(R->FB.local(L)); }

// Operators. A literal on the right has the type of the left.
inline Val operator+(Val A, Val B) { return A.R->bin(BinOp::Add, A, B); }
inline Val operator-(Val A, Val B) { return A.R->bin(BinOp::Sub, A, B); }
inline Val operator*(Val A, Val B) { return A.R->bin(BinOp::Mul, A, B); }
inline Val operator/(Val A, Val B) { return A.R->bin(BinOp::Div, A, B); }
inline Val operator%(Val A, Val B) { return A.R->bin(BinOp::Rem, A, B); }
inline Val operator&(Val A, Val B) { return A.R->bin(BinOp::And, A, B); }
inline Val operator|(Val A, Val B) { return A.R->bin(BinOp::Or, A, B); }
inline Val operator^(Val A, Val B) { return A.R->bin(BinOp::Xor, A, B); }
inline Val operator<<(Val A, Val B) { return A.R->bin(BinOp::Shl, A, B); }
inline Val operator>>(Val A, Val B) { return A.R->bin(BinOp::Shr, A, B); }
inline Val operator==(Val A, Val B) { return A.R->bin(BinOp::Eq, A, B); }
inline Val operator!=(Val A, Val B) { return A.R->bin(BinOp::Ne, A, B); }
inline Val operator<(Val A, Val B) { return A.R->bin(BinOp::Lt, A, B); }
inline Val operator<=(Val A, Val B) { return A.R->bin(BinOp::Le, A, B); }
inline Val operator>(Val A, Val B) { return A.R->bin(BinOp::Gt, A, B); }
inline Val operator>=(Val A, Val B) { return A.R->bin(BinOp::Ge, A, B); }
inline Val operator+(Val A, uint64_t K) { return A + A.R->k(A.ty(), K); }
inline Val operator-(Val A, uint64_t K) { return A - A.R->k(A.ty(), K); }
inline Val operator*(Val A, uint64_t K) { return A * A.R->k(A.ty(), K); }
inline Val operator/(Val A, uint64_t K) { return A / A.R->k(A.ty(), K); }
inline Val operator%(Val A, uint64_t K) { return A % A.R->k(A.ty(), K); }
inline Val operator&(Val A, uint64_t K) { return A & A.R->k(A.ty(), K); }
inline Val operator|(Val A, uint64_t K) { return A | A.R->k(A.ty(), K); }
inline Val operator<<(Val A, uint64_t K) { return A << A.R->k(A.ty(), K); }
inline Val operator>>(Val A, uint64_t K) { return A >> A.R->k(A.ty(), K); }
inline Val operator==(Val A, uint64_t K) { return A == A.R->k(A.ty(), K); }
inline Val operator!=(Val A, uint64_t K) { return A != A.R->k(A.ty(), K); }
inline Val operator<(Val A, uint64_t K) { return A < A.R->k(A.ty(), K); }
inline Val operator<=(Val A, uint64_t K) { return A <= A.R->k(A.ty(), K); }
inline Val operator>(Val A, uint64_t K) { return A > A.R->k(A.ty(), K); }
inline Val operator>=(Val A, uint64_t K) { return A >= A.R->k(A.ty(), K); }

} // namespace impl
} // namespace gir
} // namespace gopp

#endif // GOPP_LIB_GIR_RTEMIT_H
