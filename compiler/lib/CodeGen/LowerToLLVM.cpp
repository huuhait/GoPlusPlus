//===- LowerToLLVM.cpp - GIR to LLVM IR -----------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// One alloca per local in the entry block (mem2reg builds the SSA, D4.1), one
// LLVM block per GIR block, one LLVM instruction sequence per statement. The
// operations carry Go semantics (D4.12): the signed -1 divisor, shift counts
// at or above the width, saturating float to integer.
//
// A bool is i1 in registers and i8 in memory.
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/Verify.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"

using namespace gopp;
using llvm::cast;
using llvm::isa;

namespace {

namespace G = gopp::gir;

class Lowering {
public:
  Lowering(llvm::LLVMContext &C, const G::Module &GM, llvm::Module &LM,
           const LowerToLLVMOptions &Opts)
      : C(C), GM(GM), LM(LM), B(C), Opts(Opts), DIB(LM) {}

  llvm::Error run();

private:
  llvm::LLVMContext &C;
  const G::Module &GM;
  llvm::Module &LM;
  llvm::IRBuilder<> B;
  const LowerToLLVMOptions &Opts;
  llvm::DIBuilder DIB;
  llvm::DenseMap<const G::Type *, llvm::Type *> MemTypes;
  llvm::SmallVector<llvm::Function *, 16> Funcs;
  llvm::SmallVector<llvm::GlobalVariable *, 16> Strings;
  llvm::SmallVector<llvm::DIFile *, 4> DIFiles;
  llvm::DISubprogram *SP = nullptr;

  // Per function.
  const G::Function *GF = nullptr;
  llvm::Function *LF = nullptr;
  llvm::SmallVector<llvm::AllocaInst *, 32> Slots;
  llvm::SmallVector<llvm::BasicBlock *, 16> Blocks;

  llvm::Error fail(const llvm::Twine &What) {
    return llvm::createStringError(
        std::errc::not_supported, "code generation: %s in %s",
        What.str().c_str(), GF ? GF->Name.c_str() : "the module");
  }

  llvm::Type *memTy(const G::Type *T);
  llvm::Type *valTy(const G::Type *T) {
    return T->isBool() ? llvm::Type::getInt1Ty(C) : memTy(T);
  }
  llvm::FunctionType *fnType(const G::Type *Sig);
  llvm::Function *declare(const G::Function &F);
  llvm::Constant *stringConst(uint64_t Id);
  void setLoc(const G::Span &S);

  llvm::Value *placeAddr(const G::Place &P);
  llvm::Value *loadPlace(const G::Place &P);
  void storePlace(const G::Place &P, llvm::Value *V);
  llvm::Value *operand(const G::Operand &O);
  llvm::Value *constant(const G::Constant &K);
  llvm::Expected<llvm::Value *> rvalue(const G::Rvalue &R);
  llvm::Value *binary(G::BinOp Op, const G::Type *T, llvm::Value *X,
                      llvm::Value *Y);
  llvm::Value *shift(G::BinOp Op, const G::Type *T, llvm::Value *X,
                     const G::Type *CT, llvm::Value *Cnt);
  llvm::Value *cast(G::CastKind K, const G::Type *From, const G::Type *To,
                    llvm::Value *V);
  llvm::Error function(const G::Function &F, llvm::Function *LF);
  llvm::Error terminator(const G::Terminator &T);
};

llvm::Type *Lowering::memTy(const G::Type *T) {
  auto It = MemTypes.find(T);
  if (It != MemTypes.end())
    return It->second;
  llvm::Type *R = nullptr;
  llvm::Type *I64 = llvm::Type::getInt64Ty(C);
  llvm::Type *Ptr = llvm::PointerType::getUnqual(C);
  switch (T->getKind()) {
  case G::Type::Kind::Unit:
    R = llvm::StructType::get(C);
    break;
  case G::Type::Kind::Bool:
    R = llvm::Type::getInt8Ty(C);
    break;
  case G::Type::Kind::Int:
    R = llvm::Type::getIntNTy(C, T->bits());
    break;
  case G::Type::Kind::Float:
    R = T->bits() == 32 ? llvm::Type::getFloatTy(C)
                        : llvm::Type::getDoubleTy(C);
    break;
  case G::Type::Kind::String:
  case G::Type::Kind::Slice:
    R = llvm::StructType::get(C, {Ptr, I64, I64});
    break;
  case G::Type::Kind::Ptr:
  case G::Type::Kind::Func:
    R = Ptr;
    break;
  case G::Type::Kind::Array:
    R = llvm::ArrayType::get(memTy(T->elem()), T->length());
    break;
  case G::Type::Kind::Tuple: {
    llvm::SmallVector<llvm::Type *, 4> Fs;
    for (const G::Type *E : T->elems())
      Fs.push_back(memTy(E));
    R = llvm::StructType::get(C, Fs);
    break;
  }
  case G::Type::Kind::Struct: {
    auto *S = llvm::StructType::create(C, T->name());
    MemTypes[T] = S; // a recursive struct reaches itself through a pointer
    llvm::SmallVector<llvm::Type *, 4> Fs;
    for (const G::Type *E : T->elems())
      Fs.push_back(memTy(E));
    S->setBody(Fs);
    R = S;
    break;
  }
  }
  MemTypes[T] = R;
  return R;
}

llvm::FunctionType *Lowering::fnType(const G::Type *Sig) {
  llvm::SmallVector<llvm::Type *, 4> Params;
  for (const G::Type *P : Sig->elems())
    Params.push_back(valTy(P));
  llvm::Type *Ret =
      Sig->ret()->isUnit() ? llvm::Type::getVoidTy(C) : valTy(Sig->ret());
  return llvm::FunctionType::get(Ret, Params, false);
}

/// The C ABI wants the extension of an integer narrower than 32 bits spelled
/// out (clang does the same).
static void extAttr(llvm::LLVMContext &C, llvm::AttributeList &AL, unsigned Idx,
                    const G::Type *T) {
  if (T->isBool() || (T->isInt() && T->bits() < 32))
    AL = AL.addAttributeAtIndex(
        C, Idx,
        llvm::Attribute::get(C, T->isSigned() ? llvm::Attribute::SExt
                                              : llvm::Attribute::ZExt));
}

llvm::Function *Lowering::declare(const G::Function &F) {
  llvm::Function *LF = llvm::Function::Create(
      fnType(F.Sig),
      F.Link == G::Linkage::Internal ? llvm::GlobalValue::InternalLinkage
      : F.Link == G::Linkage::LinkOnceODR
          ? llvm::GlobalValue::LinkOnceODRLinkage
          : llvm::GlobalValue::ExternalLinkage,
      F.Name, &LM);
  llvm::AttributeList AL = LF->getAttributes();
  if (F.NoUnwind)
    AL = AL.addFnAttribute(C, llvm::Attribute::NoUnwind);
  if (F.NoReturn)
    AL = AL.addFnAttribute(C, llvm::Attribute::NoReturn);
  // Unwind tables for every function Go++ defines, so a stack trace works
  // through any frame (D4.6; clang's default on Linux).
  if (!F.isDeclaration())
    AL = AL.addFnAttribute(
        C, llvm::Attribute::getWithUWTableKind(C, llvm::UWTableKind::Async));
  for (unsigned I = 0; I < F.Sig->elems().size(); ++I)
    extAttr(C, AL, llvm::AttributeList::FirstArgIndex + I, F.Sig->elems()[I]);
  if (!F.Sig->ret()->isUnit())
    extAttr(C, AL, llvm::AttributeList::ReturnIndex, F.Sig->ret());
  LF->setAttributes(AL);
  return LF;
}

llvm::Constant *Lowering::stringConst(uint64_t Id) {
  if (Strings.size() <= Id)
    Strings.resize(GM.Strings.size(), nullptr);
  if (!Strings[Id]) {
    llvm::Constant *Init =
        llvm::ConstantDataArray::getString(C, GM.Strings[Id], true);
    auto *GV = new llvm::GlobalVariable(LM, Init->getType(), true,
                                        llvm::GlobalValue::PrivateLinkage, Init,
                                        ".str." + llvm::Twine(Id));
    GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    Strings[Id] = GV;
  }
  return Strings[Id];
}

void Lowering::setLoc(const G::Span &S) {
  if (!Opts.DebugInfo || !SP) {
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    return;
  }
  // Code the compiler made has no position: line 0, as in clang.
  B.SetCurrentDebugLocation(llvm::DILocation::get(C, S.isValid() ? S.Line : 0,
                                                  S.isValid() ? S.Col : 0, SP));
}

llvm::Value *Lowering::placeAddr(const G::Place &P) {
  llvm::Value *Addr = Slots[P.Local];
  const G::Type *Cur = GF->Locals[P.Local].Ty;
  for (const G::PlaceElem &E : P.Proj) {
    switch (E.K) {
    case G::PlaceElem::Deref:
      Addr = B.CreateLoad(llvm::PointerType::getUnqual(C), Addr);
      break;
    case G::PlaceElem::Field:
      Addr = B.CreateStructGEP(memTy(Cur), Addr, E.Val);
      break;
    case G::PlaceElem::Index: {
      const G::Local &IL = GF->Locals[E.Val];
      llvm::Value *Idx = B.CreateLoad(memTy(IL.Ty), Slots[E.Val]);
      Idx = IL.Ty->isSigned() ? B.CreateSExtOrTrunc(Idx, B.getInt64Ty())
                              : B.CreateZExtOrTrunc(Idx, B.getInt64Ty());
      if (Cur->getKind() == G::Type::Kind::Array) {
        Addr = B.CreateInBoundsGEP(memTy(Cur), Addr, {B.getInt64(0), Idx});
      } else { // a slice: the data pointer is field 0
        llvm::Value *Data =
            B.CreateLoad(llvm::PointerType::getUnqual(C),
                         B.CreateStructGEP(memTy(Cur), Addr, 0));
        Addr = B.CreateInBoundsGEP(memTy(Cur->elem()), Data, {Idx});
      }
      break;
    }
    case G::PlaceElem::ConstIndex:
      Addr = B.CreateInBoundsGEP(memTy(Cur), Addr,
                                 {B.getInt64(0), B.getInt64(E.Val)});
      break;
    case G::PlaceElem::Downcast:
      break; // the verifier rejects it until the types exist
    }
    Cur = E.Ty;
  }
  return Addr;
}

llvm::Value *Lowering::loadPlace(const G::Place &P) {
  llvm::Value *V = B.CreateLoad(memTy(P.Ty), placeAddr(P));
  return P.Ty->isBool()
             ? B.CreateICmpNE(V, llvm::ConstantInt::get(V->getType(), 0))
             : V;
}

void Lowering::storePlace(const G::Place &P, llvm::Value *V) {
  if (P.Ty->isUnit())
    return;
  if (P.Ty->isBool())
    V = B.CreateZExt(V, B.getInt8Ty());
  B.CreateStore(V, placeAddr(P));
}

llvm::Value *Lowering::constant(const G::Constant &K) {
  switch (K.K) {
  case G::Constant::Int:
    return llvm::ConstantInt::get(valTy(K.Ty), K.Bits);
  case G::Constant::Float: {
    double D;
    std::memcpy(&D, &K.Bits, sizeof D);
    return llvm::ConstantFP::get(valTy(K.Ty), D);
  }
  case G::Constant::Bool:
    return B.getInt1(K.Bits);
  case G::Constant::Str: {
    // {ptr, len, cap = 0}: a literal is in read-only data and never freed.
    llvm::Type *ST = memTy(K.Ty);
    return llvm::ConstantStruct::get(cast<llvm::StructType>(ST),
                                     {stringConst(K.Bits),
                                      B.getInt64(GM.Strings[K.Bits].size()),
                                      B.getInt64(0)});
  }
  case G::Constant::Func:
    return Funcs[K.Bits];
  case G::Constant::Null:
    return llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(C));
  case G::Constant::Zero:
    return llvm::Constant::getNullValue(valTy(K.Ty));
  }
  return nullptr;
}

llvm::Value *Lowering::operand(const G::Operand &O) {
  if (O.K == G::Operand::Const)
    return constant(O.C);
  return loadPlace(O.P);
}

llvm::Value *Lowering::shift(G::BinOp Op, const G::Type *T, llvm::Value *X,
                             const G::Type *CT, llvm::Value *Cnt) {
  // Go: a count >= the width gives 0, or the sign fill for >> of a signed
  // value. The comparison is unsigned in the count's own type (a negative
  // count was rejected by an Assert before).
  unsigned W = T->bits();
  llvm::Value *Big =
      B.CreateICmpUGE(Cnt, llvm::ConstantInt::get(Cnt->getType(), W));
  llvm::Value *C2 = B.CreateZExtOrTrunc(Cnt, X->getType());
  (void)CT;
  if (Op == G::BinOp::Shl)
    return B.CreateSelect(Big, llvm::ConstantInt::get(X->getType(), 0),
                          B.CreateShl(X, C2));
  if (T->isSigned()) {
    llvm::Value *Safe =
        B.CreateSelect(Big, llvm::ConstantInt::get(X->getType(), W - 1), C2);
    return B.CreateAShr(X, Safe);
  }
  return B.CreateSelect(Big, llvm::ConstantInt::get(X->getType(), 0),
                        B.CreateLShr(X, C2));
}

llvm::Value *Lowering::binary(G::BinOp Op, const G::Type *T, llvm::Value *X,
                              llvm::Value *Y) {
  bool F = T->isFloat(), S = T->isInt() && T->isSigned();
  switch (Op) {
  case G::BinOp::Add:
    return F ? B.CreateFAdd(X, Y) : B.CreateAdd(X, Y);
  case G::BinOp::Sub:
    return F ? B.CreateFSub(X, Y) : B.CreateSub(X, Y);
  case G::BinOp::Mul:
    return F ? B.CreateFMul(X, Y) : B.CreateMul(X, Y);
  case G::BinOp::Div:
  case G::BinOp::Rem: {
    if (F)
      return B.CreateFDiv(X, Y);
    bool IsDiv = Op == G::BinOp::Div;
    if (!S)
      return IsDiv ? B.CreateUDiv(X, Y) : B.CreateURem(X, Y);
    // MinInt / -1 is MinInt and MinInt % -1 is 0 in Go (D4.12); in LLVM they
    // are undefined. Divide by 1 instead of -1, then fix the result.
    llvm::Value *MinusOne =
        B.CreateICmpEQ(Y, llvm::ConstantInt::get(Y->getType(), -1, true));
    llvm::Value *Safe =
        B.CreateSelect(MinusOne, llvm::ConstantInt::get(Y->getType(), 1), Y);
    if (IsDiv)
      return B.CreateSelect(MinusOne, B.CreateNeg(X), B.CreateSDiv(X, Safe));
    return B.CreateSelect(MinusOne, llvm::ConstantInt::get(X->getType(), 0),
                          B.CreateSRem(X, Safe));
  }
  case G::BinOp::And:
    return B.CreateAnd(X, Y);
  case G::BinOp::Or:
    return B.CreateOr(X, Y);
  case G::BinOp::Xor:
    return B.CreateXor(X, Y);
  case G::BinOp::AndNot:
    return B.CreateAnd(X, B.CreateNot(Y));
  case G::BinOp::Shl:
  case G::BinOp::Shr:
    return nullptr; // handled by shift()
  case G::BinOp::Eq:
    return F ? B.CreateFCmpOEQ(X, Y) : B.CreateICmpEQ(X, Y);
  case G::BinOp::Ne:
    return F ? B.CreateFCmpUNE(X, Y) : B.CreateICmpNE(X, Y);
  case G::BinOp::Lt:
    return F   ? B.CreateFCmpOLT(X, Y)
           : S ? B.CreateICmpSLT(X, Y)
               : B.CreateICmpULT(X, Y);
  case G::BinOp::Le:
    return F   ? B.CreateFCmpOLE(X, Y)
           : S ? B.CreateICmpSLE(X, Y)
               : B.CreateICmpULE(X, Y);
  case G::BinOp::Gt:
    return F   ? B.CreateFCmpOGT(X, Y)
           : S ? B.CreateICmpSGT(X, Y)
               : B.CreateICmpUGT(X, Y);
  case G::BinOp::Ge:
    return F   ? B.CreateFCmpOGE(X, Y)
           : S ? B.CreateICmpSGE(X, Y)
               : B.CreateICmpUGE(X, Y);
  }
  return nullptr;
}

llvm::Value *Lowering::cast(G::CastKind K, const G::Type *From,
                            const G::Type *To, llvm::Value *V) {
  llvm::Type *DT = valTy(To);
  switch (K) {
  case G::CastKind::IntToInt:
    if (To->bits() == From->bits())
      return V;
    if (To->bits() < From->bits())
      return B.CreateTrunc(V, DT);
    return From->isSigned() ? B.CreateSExt(V, DT) : B.CreateZExt(V, DT);
  case G::CastKind::IntToFloat:
    return From->isSigned() ? B.CreateSIToFP(V, DT) : B.CreateUIToFP(V, DT);
  case G::CastKind::FloatToInt: {
    // Saturating, NaN gives 0 (D4.12).
    llvm::Intrinsic::ID Id = To->isSigned() ? llvm::Intrinsic::fptosi_sat
                                            : llvm::Intrinsic::fptoui_sat;
    return B.CreateIntrinsic(Id, {DT, V->getType()}, {V});
  }
  case G::CastKind::FloatToFloat:
    if (To->bits() == From->bits())
      return V;
    return To->bits() > From->bits() ? B.CreateFPExt(V, DT)
                                     : B.CreateFPTrunc(V, DT);
  case G::CastKind::PtrToInt:
    return B.CreatePtrToInt(V, DT);
  case G::CastKind::IntToPtr:
    return B.CreateIntToPtr(V, DT);
  case G::CastKind::PtrToPtr:
    return V;
  }
  return V;
}

llvm::Expected<llvm::Value *> Lowering::rvalue(const G::Rvalue &R) {
  switch (R.K) {
  case G::Rvalue::Use:
    return operand(R.A);
  case G::Rvalue::AddrOf:
    return placeAddr(R.P);
  case G::Rvalue::Binary: {
    llvm::Value *X = operand(R.A), *Y = operand(R.B);
    const G::Type *T = R.A.type();
    if (G::isShift(R.Bin))
      return shift(R.Bin, T, X, R.B.type(), Y);
    return binary(R.Bin, T, X, Y);
  }
  case G::Rvalue::Unary: {
    llvm::Value *X = operand(R.A);
    switch (R.Un) {
    case G::UnOp::Neg:
      return R.Ty->isFloat() ? B.CreateFNeg(X) : B.CreateNeg(X);
    case G::UnOp::Not:
      return B.CreateNot(X);
    case G::UnOp::Compl:
      return B.CreateNot(X);
    }
    return X;
  }
  case G::Rvalue::Cast:
    return cast(R.CastOp, R.A.type(), R.Ty, operand(R.A));
  case G::Rvalue::Aggregate: {
    llvm::Type *T = memTy(R.Ty);
    llvm::Value *V = llvm::PoisonValue::get(T);
    for (unsigned I = 0; I < R.Ops.size(); ++I) {
      llvm::Value *E = operand(R.Ops[I]);
      if (R.Ops[I].type()->isBool())
        E = B.CreateZExt(E, B.getInt8Ty());
      V = B.CreateInsertValue(V, E, I);
    }
    return V;
  }
  case G::Rvalue::Len: {
    if (R.P.Ty->getKind() == G::Type::Kind::Array)
      return B.getInt64(R.P.Ty->length());
    return B.CreateLoad(B.getInt64Ty(),
                        B.CreateStructGEP(memTy(R.P.Ty), placeAddr(R.P), 1));
  }
  }
  return fail("an rvalue");
}

llvm::Error Lowering::terminator(const G::Terminator &T) {
  setLoc(T.Loc);
  // Both need a landing pad (`invoke`, D4.6): P04.07. A plain call would
  // change what a panic does.
  auto Unwindless = [&](const G::UnwindAction &U) -> llvm::Error {
    if (U.K == G::UnwindAction::Cleanup)
      return fail("an unwind edge to a cleanup block (P04.07)");
    if (U.K == G::UnwindAction::Terminate)
      return fail("an unwind action that terminates (P04.07)");
    return llvm::Error::success();
  };
  switch (T.K) {
  case G::Terminator::Goto:
    B.CreateBr(Blocks[T.Target]);
    return llvm::Error::success();
  case G::Terminator::Switch: {
    llvm::Value *D = operand(T.Discr);
    if (T.Cases.size() == 1 && T.Cases[0].Value == 0 &&
        D->getType()->isIntegerTy(1)) {
      B.CreateCondBr(D, Blocks[T.Otherwise], Blocks[T.Cases[0].Target]);
      return llvm::Error::success();
    }
    llvm::SwitchInst *S =
        B.CreateSwitch(D, Blocks[T.Otherwise], T.Cases.size());
    for (const G::SwitchCase &Cs : T.Cases)
      S->addCase(llvm::ConstantInt::get(cast<llvm::IntegerType>(D->getType()),
                                        Cs.Value),
                 Blocks[Cs.Target]);
    return llvm::Error::success();
  }
  case G::Terminator::Return: {
    G::Type *R = GF->Sig->ret();
    if (R->isUnit()) {
      B.CreateRetVoid();
    } else {
      G::Place P = G::localPlace(0, R);
      B.CreateRet(loadPlace(P));
    }
    return llvm::Error::success();
  }
  case G::Terminator::Unreachable:
    B.CreateUnreachable();
    return llvm::Error::success();
  case G::Terminator::Call: {
    if (llvm::Error E = Unwindless(T.Unwind))
      return E;
    const G::Type *Sig = T.Discr.type();
    llvm::Value *Callee = operand(T.Discr);
    llvm::SmallVector<llvm::Value *, 4> Args;
    for (const G::Operand &A : T.Args)
      Args.push_back(operand(A));
    llvm::CallInst *CI = B.CreateCall(fnType(Sig), Callee, Args);
    // The ABI's extension of narrow arguments and results is repeated at the
    // call (the callee's own attributes stay on the callee).
    if (T.Discr.K == G::Operand::Const && T.Discr.C.K == G::Constant::Func)
      CI->setAttributes(
          Funcs[T.Discr.C.Bits]->getAttributes().removeFnAttributes(C));
    if (T.Unwind.K == G::UnwindAction::Unreachable)
      CI->addFnAttr(llvm::Attribute::NoUnwind);
    if (!Sig->ret()->isUnit())
      storePlace(T.Dest, CI);
    if (T.Target == G::NoId)
      B.CreateUnreachable();
    else
      B.CreateBr(Blocks[T.Target]);
    return llvm::Error::success();
  }
  case G::Terminator::Assert: {
    if (llvm::Error E = Unwindless(T.Unwind))
      return E;
    // The failing edge is a trap until the runtime has panics (P04.04).
    llvm::BasicBlock *Fail = llvm::BasicBlock::Create(C, "assert.fail", LF);
    llvm::Value *Cond = operand(T.Discr);
    if (T.Expected)
      B.CreateCondBr(Cond, Blocks[T.Target], Fail);
    else
      B.CreateCondBr(Cond, Fail, Blocks[T.Target]);
    B.SetInsertPoint(Fail);
    B.CreateIntrinsic(llvm::Intrinsic::trap, llvm::ArrayRef<llvm::Type *>(),
                      llvm::ArrayRef<llvm::Value *>());
    B.CreateUnreachable();
    return llvm::Error::success();
  }
  case G::Terminator::Drop:
    return fail("a drop (P04.09)");
  case G::Terminator::Resume:
    return fail("a resume (P04.07)");
  }
  return fail("a terminator");
}

llvm::Error Lowering::function(const G::Function &F, llvm::Function *Fn) {
  GF = &F;
  LF = Fn;
  Slots.clear();
  Blocks.clear();
  SP = nullptr;
  if (Opts.DebugInfo) {
    uint32_t FileIdx = F.Loc.isValid() ? F.Loc.File - 1 : 0;
    llvm::DISubroutineType *Ty =
        DIB.createSubroutineType(DIB.getOrCreateTypeArray({}));
    SP =
        DIB.createFunction(DIFiles.empty() ? nullptr : DIFiles[FileIdx], F.Name,
                           F.Name, DIFiles.empty() ? nullptr : DIFiles[FileIdx],
                           F.Loc.Line, Ty, F.Loc.Line, llvm::DINode::FlagZero,
                           llvm::DISubprogram::SPFlagDefinition);
    Fn->setSubprogram(SP);
  }
  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(C, "entry", Fn);
  for (size_t I = 0; I < F.Blocks.size(); ++I)
    Blocks.push_back(llvm::BasicBlock::Create(C, "bb" + llvm::Twine(I), Fn));
  B.SetInsertPoint(Entry);
  setLoc(F.Loc);
  for (const G::Local &L : F.Locals)
    Slots.push_back(L.Ty->isUnit()
                        ? nullptr
                        : B.CreateAlloca(memTy(L.Ty), nullptr, L.Name));
  for (unsigned I = 0; I < F.numParams(); ++I) {
    llvm::Value *A = Fn->getArg(I);
    G::Place P = G::localPlace(1 + I, F.Locals[1 + I].Ty);
    storePlace(P, A);
  }
  B.CreateBr(Blocks[0]);
  for (size_t Bi = 0; Bi < F.Blocks.size(); ++Bi) {
    const G::BasicBlock &BB = F.Blocks[Bi];
    B.SetInsertPoint(Blocks[Bi]);
    if (BB.Catch)
      return fail("a catch block, the handler of `defer` (P04.07)");
    for (const G::Statement &S : BB.Stmts) {
      setLoc(S.Loc);
      switch (S.K) {
      case G::Statement::Assign: {
        llvm::Expected<llvm::Value *> V = rvalue(S.RV);
        if (!V)
          return V.takeError();
        storePlace(S.Dest, *V);
        break;
      }
      case G::Statement::StorageLive:
      case G::Statement::StorageDead:
      case G::Statement::Nop:
        break;
      }
    }
    if (llvm::Error E = terminator(BB.Term))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error Lowering::run() {
  if (Opts.DebugInfo) {
    LM.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
    LM.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                     llvm::DEBUG_METADATA_VERSION);
    for (const std::string &F : GM.Files)
      DIFiles.push_back(DIB.createFile(llvm::sys::path::filename(F),
                                       llvm::sys::path::parent_path(F)));
    if (DIFiles.empty())
      DIFiles.push_back(DIB.createFile("<unknown>", ""));
    // Line tables only (D4.7); variables and types come with P04.12.
    DIB.createCompileUnit(llvm::dwarf::DW_LANG_C11, DIFiles[0], "gopp", false,
                          "", 0, "", llvm::DICompileUnit::LineTablesOnly);
  }
  for (const auto &F : GM.Funcs)
    Funcs.push_back(declare(*F));
  for (size_t I = 0; I < GM.Funcs.size(); ++I)
    if (!GM.Funcs[I]->isDeclaration())
      if (llvm::Error E = function(*GM.Funcs[I], Funcs[I]))
        return E;
  GF = nullptr;
  if (Opts.DebugInfo)
    DIB.finalize();
  return llvm::Error::success();
}

} // namespace

llvm::Expected<std::unique_ptr<llvm::Module>>
gopp::lowerToLLVM(llvm::LLVMContext &Ctx, const gir::Module &M,
                  const llvm::TargetMachine &TM,
                  const LowerToLLVMOptions &Opts) {
  // The lowering indexes GIR without checking it: a module someone built by
  // hand is verified here, as the builder's output is (P04.01).
  std::vector<gir::VerifyError> Errs = gir::verify(M, 5);
  if (!Errs.empty()) {
    std::string Msg;
    for (const gir::VerifyError &E : Errs)
      Msg += "\n  " + E.str();
    return llvm::createStringError(std::errc::invalid_argument,
                                   "internal error: the GIR is invalid:%s",
                                   Msg.c_str());
  }
  auto LM =
      std::make_unique<llvm::Module>(M.Name.empty() ? "gopp" : M.Name, Ctx);
  LM->setTargetTriple(TM.getTargetTriple());
  LM->setDataLayout(TM.createDataLayout());
  Lowering L(Ctx, M, *LM, Opts);
  if (llvm::Error E = L.run())
    return std::move(E);
  std::string Problems;
  llvm::raw_string_ostream PS(Problems);
  if (llvm::verifyModule(*LM, &PS))
    return llvm::createStringError(
        std::errc::invalid_argument,
        "internal error: the generated LLVM IR is invalid: %s",
        Problems.c_str());
  return std::move(LM);
}
