//===- GoABI.cpp - The Go++ internal calling convention -------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/GoABI.h"

#include "llvm/IR/Attributes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"

#include <algorithm>

using namespace gopp;
using namespace gopp::goabi;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

Policy Policy::forTriple(const llvm::Triple &T) {
  Policy P;
  // swiftcc on both targets (ADR-0030: regcall's eleven integer result
  // registers were measured and rejected). What it returns in registers
  // without LLVM falling back to memory
  // (GoABITest.ReturnRegistersAreWhatLLVMHas): amd64 rax, rdx, rcx, r8 and
  // xmm0-3 (a fifth integer makes the result memory, a fifth double lands in an
  // x87 register); arm64 x0-x7 and v0-v7. Any other target returns every
  // aggregate through memory.
  P.CC = llvm::CallingConv::Swift;
  if (T.getArch() == llvm::Triple::x86_64) {
    P.RetInts = 4;
    P.RetFloats = 4;
  } else if (T.isAArch64()) {
    P.RetInts = 8;
    P.RetFloats = 8;
  }
  return P;
}

//===----------------------------------------------------------------------===//
// Flattening an aggregate into its scalars
//===----------------------------------------------------------------------===//

namespace {

constexpr unsigned CountCap = 255;

void addCount(uint16_t &A, uint16_t B) {
  A = static_cast<uint16_t>(std::min<unsigned>(A + B, CountCap));
}

} // namespace

/// The number of scalars of T of each class, by a walk that visits every
/// distinct type once (a DAG of structs that doubles at each level is not
/// walked as a tree) and keeps no recursion. An array of two or more
/// elements, and any type that is not an integer, a float, a pointer, an
/// array or a struct, is not flattenable (Go's ABIInternal flattens arrays of
/// length one only).
GoAbi::Count GoAbi::countOf(llvm::Type *Root) {
  if (auto It = Counts.find(Root); It != Counts.end())
    return It->second;
  struct Frame {
    llvm::Type *T;
    unsigned Next;
    unsigned Kids;
    Count Acc;
  };
  llvm::SmallVector<Frame, 16> Stack;
  Count Out;
  // Pushes T, or delivers its answer when it is a leaf or known.
  auto Deliver = [&](Count K) {
    if (Stack.empty()) {
      Out = K;
      return;
    }
    Count &A = Stack.back().Acc;
    addCount(A.Ints, K.Ints);
    addCount(A.Floats, K.Floats);
    A.Bad |= K.Bad;
  };
  auto Visit = [&](llvm::Type *T) {
    if (Work)
      ++*Work;
    if (auto It = Counts.find(T); It != Counts.end()) {
      Deliver(It->second);
      return;
    }
    Count K;
    if (T->isIntegerTy() || T->isPointerTy()) {
      K.Ints = 1;
    } else if (T->isFloatTy() || T->isDoubleTy()) {
      K.Floats = 1;
    } else if (auto *AT = dyn_cast<llvm::ArrayType>(T)) {
      if (AT->getNumElements() >= 2) {
        K.Bad = true;
      } else if (AT->getNumElements() == 1) {
        Stack.push_back({T, 0, 1, Count()});
        return;
      }
    } else if (auto *ST = dyn_cast<llvm::StructType>(T)) {
      if (ST->getNumElements()) {
        Stack.push_back({T, 0, ST->getNumElements(), Count()});
        return;
      }
    } else {
      K.Bad = true;
    }
    Counts[T] = K;
    Deliver(K);
  };
  Visit(Root);
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    if (F.Next < F.Kids) {
      llvm::Type *Kid =
          isa<llvm::ArrayType>(F.T)
              ? cast<llvm::ArrayType>(F.T)->getElementType()
              : cast<llvm::StructType>(F.T)->getElementType(F.Next);
      ++F.Next;
      Visit(Kid); // may push
      continue;
    }
    Count K = F.Acc;
    llvm::Type *T = F.T;
    Stack.pop_back();
    Counts[T] = K;
    Deliver(K);
  }
  return Out;
}

bool GoAbi::flatten(llvm::Type *Root, unsigned Max,
                    llvm::SmallVectorImpl<Part> &Out, unsigned &Ints,
                    unsigned &Floats) {
  Count N = countOf(Root);
  Ints = N.Ints;
  Floats = N.Floats;
  if (N.Bad || N.Ints + N.Floats > Max)
    return false;
  struct Frame {
    llvm::Type *T;
    uint64_t Base;
    unsigned Next;
  };
  llvm::SmallVector<Frame, 16> Stack;
  Stack.push_back({Root, 0, 0});
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    llvm::Type *T = F.T;
    if (F.Next == 0) {
      if (Work)
        ++*Work;
      Count K = Counts[T];
      if (K.Ints + K.Floats == 0) { // nothing in it (empty structs)
        Stack.pop_back();
        continue;
      }
      if (!isa<llvm::StructType>(T) && !isa<llvm::ArrayType>(T)) {
        Out.push_back({T, F.Base});
        Stack.pop_back();
        continue;
      }
    }
    if (auto *AT = dyn_cast<llvm::ArrayType>(T)) {
      if (F.Next == 0) {
        F.Next = 1;
        Stack.push_back({AT->getElementType(), F.Base, 0});
      } else {
        Stack.pop_back();
      }
      continue;
    }
    auto *ST = cast<llvm::StructType>(T);
    if (F.Next >= ST->getNumElements()) {
      Stack.pop_back();
      continue;
    }
    unsigned I = F.Next++;
    uint64_t Off = F.Base + DL.getStructLayout(ST)->getElementOffset(I);
    Stack.push_back({ST->getElementType(I), Off, 0});
  }
  return true;
}

//===----------------------------------------------------------------------===//
// The decision
//===----------------------------------------------------------------------===//

static bool narrowInt(const gir::Type *T) {
  return T->isBool() || (T->isInt() && T->bits() < 32);
}

const FnInfo &GoAbi::classify(const gir::Type *Sig) {
  std::unique_ptr<FnInfo> &Slot = Infos[Sig];
  if (Slot)
    return *Slot;
  auto FI = std::make_unique<FnInfo>();
  FI->CC = P.CC;
  llvm::SmallVector<llvm::Type *, 8> IR;
  llvm::SmallVector<llvm::AttributeSet, 8> PAttrs;
  llvm::Type *Ptr = llvm::PointerType::getUnqual(C);
  auto Push = [&](llvm::Type *T, llvm::AttributeSet AS = llvm::AttributeSet()) {
    IR.push_back(T);
    PAttrs.push_back(AS);
  };
  auto Ext = [&](const gir::Type *T) {
    llvm::AttrBuilder AB(C);
    AB.addAttribute(T->isSigned() ? llvm::Attribute::SExt
                                  : llvm::Attribute::ZExt);
    return AB;
  };

  // The result.
  const gir::Type *R = Sig->ret();
  ArgInfo &Ret = FI->Ret;
  Ret.Ty = R;
  llvm::Type *RetTy = llvm::Type::getVoidTy(C);
  llvm::AttributeSet RetAttrs;
  if (R->isUnit()) {
    Ret.K = Kind::Ignore;
  } else if (R->isScalar()) {
    Ret.K = Kind::Scalar;
    RetTy = cabi::valueType(C, R);
    if (narrowInt(R))
      RetAttrs = llvm::AttributeSet::get(C, Ext(R));
  } else {
    unsigned Ints = 0, Floats = 0;
    llvm::SmallVector<Part, 4> Parts;
    bool Flat = flatten(MemTy(R), P.RetInts + P.RetFloats, Parts, Ints, Floats);
    if (Flat && Parts.empty()) {
      Ret.K = Kind::Ignore;
    } else if (Flat && Ints <= P.RetInts && Floats <= P.RetFloats) {
      Ret.K = Kind::Direct;
      Ret.Parts.assign(Parts.begin(), Parts.end());
      if (Parts.size() == 1) {
        RetTy = Parts[0].Ty;
      } else {
        llvm::SmallVector<llvm::Type *, 4> Es;
        for (const Part &Pt : Parts)
          Es.push_back(Pt.Ty);
        RetTy = llvm::StructType::get(C, Es);
      }
    } else {
      Ret.K = Kind::Indirect;
      FI->SRet = IR.size();
      Push(Ptr);
    }
  }
  // A closure's code takes its environment as the first parameter of its
  // GIR type (Type::hasEnv); here it is no parameter of the others'.
  const bool HasEnv = Sig->hasEnv();
  if (HasEnv) {
    // swiftcc has a register for it that is no argument's (swiftself).
    FI->Env = IR.size();
    llvm::AttrBuilder AB(C);
    AB.addAttribute(llvm::Attribute::SwiftSelf);
    Push(Ptr, llvm::AttributeSet::get(C, AB));
  }

  // The parameters.
  for (const gir::Type *T : Sig->elems().drop_front(HasEnv ? 1 : 0)) {
    ArgInfo A;
    A.Ty = T;
    A.First = IR.size();
    if (T->isUnit()) {
      A.K = Kind::Ignore;
    } else if (T->isScalar()) {
      A.K = Kind::Scalar;
      Push(cabi::valueType(C, T), narrowInt(T)
                                      ? llvm::AttributeSet::get(C, Ext(T))
                                      : llvm::AttributeSet());
    } else {
      unsigned Ints = 0, Floats = 0;
      llvm::SmallVector<Part, 4> Parts;
      if (flatten(MemTy(T), P.MaxParamLeaves, Parts, Ints, Floats)) {
        if (Parts.empty()) {
          A.K = Kind::Ignore;
        } else {
          A.K = Kind::Direct;
          A.Parts.assign(Parts.begin(), Parts.end());
          for (const Part &Pt : Parts)
            Push(Pt.Ty);
        }
      } else {
        A.K = Kind::Indirect;
        Push(Ptr);
      }
    }
    A.Count = IR.size() - A.First;
    FI->Params.push_back(std::move(A));
  }
  FI->Type = llvm::FunctionType::get(RetTy, IR, false);
  FI->Attrs =
      llvm::AttributeList::get(C, llvm::AttributeSet(), RetAttrs, PAttrs);
  Slot = std::move(FI);
  return *Slot;
}

//===----------------------------------------------------------------------===//
// Lowering
//===----------------------------------------------------------------------===//

namespace {

llvm::Align commonAlign(llvm::Align A, uint64_t Off) {
  return Off ? llvm::commonAlignment(A, Off) : A;
}

llvm::Value *at(cabi::Env &E, llvm::Value *P, uint64_t Off) {
  if (!Off)
    return P;
  return E.B.CreateConstInBoundsGEP1_64(E.B.getInt8Ty(), P, Off);
}

} // namespace

std::vector<llvm::Value *> GoAbi::lowerArgs(cabi::Env &E, const FnInfo &FI,
                                            llvm::ArrayRef<cabi::Value> Args,
                                            cabi::Value RetDest,
                                            llvm::Value *EnvPtr) {
  std::vector<llvm::Value *> Out(FI.Type->getNumParams(), nullptr);
  if (FI.SRet >= 0)
    Out[FI.SRet] = RetDest.V;
  if (FI.Env >= 0)
    Out[FI.Env] =
        EnvPtr
            ? EnvPtr
            : llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(C));
  for (size_t I = 0; I < FI.Params.size(); ++I) {
    const ArgInfo &A = FI.Params[I];
    const cabi::Value &V = Args[I];
    switch (A.K) {
    case Kind::Scalar:
    case Kind::Indirect:
      Out[A.First] = V.V;
      break;
    case Kind::Direct:
      for (size_t K = 0; K < A.Parts.size(); ++K)
        Out[A.First + K] =
            E.B.CreateAlignedLoad(A.Parts[K].Ty, at(E, V.V, A.Parts[K].Offset),
                                  commonAlign(V.Known, A.Parts[K].Offset));
      break;
    case Kind::Ignore:
      break;
    }
  }
  return Out;
}

llvm::Value *GoAbi::lowerResult(cabi::Env &E, const FnInfo &FI,
                                llvm::CallBase *Call, cabi::Value RetDest) {
  const ArgInfo &R = FI.Ret;
  switch (R.K) {
  case Kind::Scalar:
    return Call;
  case Kind::Direct:
    for (size_t K = 0; K < R.Parts.size(); ++K) {
      llvm::Value *V = R.Parts.size() == 1
                           ? static_cast<llvm::Value *>(Call)
                           : E.B.CreateExtractValue(Call, unsigned(K));
      E.B.CreateAlignedStore(V, at(E, RetDest.V, R.Parts[K].Offset),
                             commonAlign(RetDest.Known, R.Parts[K].Offset));
    }
    return nullptr;
  default:
    return nullptr;
  }
}

std::vector<cabi::Value>
GoAbi::unpackParams(cabi::Env &E, const FnInfo &FI,
                    llvm::ArrayRef<llvm::Argument *> IRArgs) {
  std::vector<cabi::Value> Out;
  for (const ArgInfo &A : FI.Params) {
    cabi::Value V;
    V.Ty = A.Ty;
    switch (A.K) {
    case Kind::Scalar:
      V.V = IRArgs[A.First];
      break;
    case Kind::Indirect:
      // The caller's copy is the body's storage.
      V.V = IRArgs[A.First];
      V.Known = DL.getABITypeAlign(MemTy(A.Ty));
      break;
    case Kind::Direct: {
      llvm::Type *MT = MemTy(A.Ty);
      llvm::AllocaInst *Slot = E.Alloca(MT, DL.getABITypeAlign(MT), "param");
      for (size_t K = 0; K < A.Parts.size(); ++K)
        E.B.CreateAlignedStore(
            IRArgs[A.First + K], at(E, Slot, A.Parts[K].Offset),
            commonAlign(Slot->getAlign(), A.Parts[K].Offset));
      V.V = Slot;
      V.Known = Slot->getAlign();
      break;
    }
    case Kind::Ignore:
      break; // no value: the body makes a slot if it needs an address
    }
    Out.push_back(V);
  }
  return Out;
}

void GoAbi::packReturn(cabi::Env &E, const FnInfo &FI, cabi::Value Result) {
  const ArgInfo &R = FI.Ret;
  switch (R.K) {
  case Kind::Scalar:
    E.B.CreateRet(Result.V);
    return;
  case Kind::Direct: {
    llvm::Value *Agg = nullptr;
    for (size_t K = 0; K < R.Parts.size(); ++K) {
      llvm::Value *El = E.B.CreateAlignedLoad(
          R.Parts[K].Ty, at(E, Result.V, R.Parts[K].Offset),
          commonAlign(Result.Known, R.Parts[K].Offset));
      if (R.Parts.size() == 1) {
        Agg = El;
      } else {
        if (!Agg)
          Agg = llvm::PoisonValue::get(FI.Type->getReturnType());
        Agg = E.B.CreateInsertValue(Agg, El, unsigned(K));
      }
    }
    E.B.CreateRet(Agg);
    return;
  }
  default:
    E.B.CreateRetVoid();
    return;
  }
}
