//===- CABILower.cpp - Lowering for ABIArgInfo ----------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/CABILower.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/MathExtras.h"

using namespace gopp;
using namespace gopp::cabi;
using llvm::cast;

namespace {

using AK = ArgInfo::Kind;

bool isAgg(const gir::Type *T) {
  switch (T->getKind()) {
  case gir::Type::Kind::Struct:
  case gir::Type::Kind::Option:
  case gir::Type::Kind::Array:
  case gir::Type::Kind::Tuple:
  case gir::Type::Kind::String:
  case gir::Type::Kind::Slice:
    return true;
  default:
    return false;
  }
}

/// The IR type an argument or result is passed as, when it is one value.
llvm::Type *irType(llvm::LLVMContext &C, const ArgInfo &A) {
  return A.HasCoerce ? A.Coerce.get(C) : valueType(C, A.Ty);
}

/// The leaves an Expand argument is made of: one IR argument per scalar of
/// the type, in field order. Ty is null for the flag byte of an Option.
struct Leaf {
  uint64_t Off;
  const gir::Type *Ty;
};

constexpr unsigned MaxLeaves = 256;

/// Offsets need a layout (DL); without one every offset is 0 and only the
/// types and their number are meaningful. The walk stops once it has more
/// leaves than MaxLeaves (a type is at most gir::MaxCTypeDepth deep).
void collectLeaves(const gir::Type *T, uint64_t Base,
                   const llvm::DataLayout *DL,
                   llvm::function_ref<llvm::Type *(const gir::Type *)> MemTy,
                   std::vector<Leaf> &Out) {
  if (Out.size() > MaxLeaves)
    return;
  auto Off = [&](const gir::Type *Agg, unsigned I) -> uint64_t {
    if (!DL)
      return 0;
    return DL->getStructLayout(cast<llvm::StructType>(MemTy(Agg)))
        ->getElementOffset(I);
  };
  switch (T->getKind()) {
  case gir::Type::Kind::Struct:
  case gir::Type::Kind::Tuple:
    for (unsigned I = 0; I < T->elems().size(); ++I)
      collectLeaves(T->elems()[I], Base + Off(T, I), DL, MemTy, Out);
    return;
  case gir::Type::Kind::Option:
    collectLeaves(T->elem(), Base + Off(T, 0), DL, MemTy, Out);
    Out.push_back({Base + Off(T, 1), nullptr});
    return;
  case gir::Type::Kind::Array: {
    uint64_t Sz = DL ? DL->getTypeAllocSize(MemTy(T->elem())) : 0;
    for (uint64_t I = 0; I < T->length() && Out.size() <= MaxLeaves; ++I)
      collectLeaves(T->elem(), Base + I * Sz, DL, MemTy, Out);
    return;
  }
  default:
    Out.push_back({Base, T});
    return;
  }
}

unsigned leavesOf(const gir::Type *T) {
  std::vector<Leaf> Ls;
  collectLeaves(T, 0, nullptr, nullptr, Ls);
  return Ls.size();
}

llvm::Type *leafType(llvm::LLVMContext &C, const Leaf &L) {
  return L.Ty ? valueType(C, L.Ty) : llvm::Type::getInt8Ty(C);
}

} // namespace

llvm::Type *cabi::valueType(llvm::LLVMContext &C, const gir::Type *T) {
  switch (T->getKind()) {
  case gir::Type::Kind::Bool:
    return llvm::Type::getInt1Ty(C);
  case gir::Type::Kind::Int:
    return llvm::IntegerType::get(C, T->bits());
  case gir::Type::Kind::Float:
    return T->bits() == 32 ? llvm::Type::getFloatTy(C)
                           : llvm::Type::getDoubleTy(C);
  case gir::Type::Kind::Unit:
    return llvm::Type::getVoidTy(C);
  default:
    return llvm::PointerType::getUnqual(C); // Ptr, Func
  }
}

llvm::Error cabi::checkSupported(const CallInfo &CI) {
  auto Bad = [&](const ArgInfo &A, llvm::StringRef Where) {
    return llvm::createStringError(
        std::errc::not_supported,
        "the C ABI of %s is %s, which no Go++ target uses", Where.str().c_str(),
        A.kindName());
  };
  auto Check = [&](const ArgInfo &A, llvm::StringRef Where) -> llvm::Error {
    switch (A.K) {
    case AK::InAlloca:
    case AK::IndirectAliased:
    case AK::TargetSpecific:
      return Bad(A, Where);
    case AK::Expand:
      if (leavesOf(A.Ty) > MaxLeaves)
        return llvm::createStringError(
            std::errc::not_supported,
            "the C ABI of %s expands into more than %u arguments",
            Where.str().c_str(), MaxLeaves);
      return llvm::Error::success();
    default:
      return llvm::Error::success();
    }
  };
  if (CI.Ret.K == AK::Expand)
    return Bad(CI.Ret, "a result");
  if (llvm::Error E = Check(CI.Ret, "a result"))
    return E;
  for (size_t I = 0; I < CI.Args.size(); ++I)
    if (llvm::Error E = Check(CI.Args[I], "argument " + std::to_string(I + 1)))
      return E;
  return llvm::Error::success();
}

//===----------------------------------------------------------------------===//
// The IR signature
//===----------------------------------------------------------------------===//

static void addExt(llvm::AttrBuilder &AB, const ArgInfo &A) {
  if (A.K == AK::Extend) {
    if (A.SExt)
      AB.addAttribute(llvm::Attribute::SExt);
    if (A.ZExt)
      AB.addAttribute(llvm::Attribute::ZExt);
  }
  if (A.InReg &&
      (A.K == AK::Direct || A.K == AK::Extend || A.K == AK::Indirect))
    AB.addAttribute(llvm::Attribute::InReg);
  if (A.StackAlign && (A.K == AK::Direct || A.K == AK::Extend))
    AB.addStackAlignmentAttr(llvm::Align(A.StackAlign));
}

/// The type a CoerceAndExpand result is returned as: the one element, or a
/// struct of the elements that are not padding.
static llvm::Type *unpaddedType(llvm::LLVMContext &C, const ArgInfo &A) {
  auto *ST = cast<llvm::StructType>(A.Coerce.get(C));
  llvm::SmallVector<llvm::Type *, 4> Es;
  for (unsigned I : A.ExpandIdx)
    Es.push_back(ST->getElementType(I));
  return Es.size() == 1 ? Es[0] : llvm::StructType::get(C, Es);
}

IRSignature
cabi::irSignature(llvm::LLVMContext &C, const CallInfo &CI,
                  llvm::function_ref<llvm::Type *(const gir::Type *)> MemTy) {
  IRSignature S;
  llvm::SmallVector<llvm::Type *, 8> Params;
  llvm::SmallVector<llvm::AttributeSet, 8> PAttrs;
  llvm::Type *Ptr = llvm::PointerType::getUnqual(C);
  auto Push = [&](llvm::Type *T, const llvm::AttrBuilder &AB) {
    Params.push_back(T);
    PAttrs.push_back(llvm::AttributeSet::get(C, AB));
  };

  llvm::Type *RetTy = llvm::Type::getVoidTy(C);
  llvm::AttrBuilder RetAB(C);
  const ArgInfo &R = CI.Ret;
  switch (R.K) {
  case AK::Direct:
  case AK::Extend:
    RetTy = irType(C, R);
    addExt(RetAB, R);
    break;
  case AK::CoerceAndExpand:
    RetTy = unpaddedType(C, R);
    break;
  case AK::Indirect: {
    llvm::AttrBuilder AB(C);
    AB.addStructRetAttr(MemTy(R.Ty));
    if (R.Align)
      AB.addAlignmentAttr(llvm::Align(R.Align));
    if (R.InReg)
      AB.addAttribute(llvm::Attribute::InReg);
    S.SRet = Params.size();
    Push(Ptr, AB);
    break;
  }
  default:
    break; // Ignore (and the refused kinds)
  }

  for (const ArgInfo &A : CI.Args) {
    IRSignature::Slot Sl;
    if (A.HasPadding) {
      llvm::AttrBuilder AB(C);
      if (A.PaddingInReg)
        AB.addAttribute(llvm::Attribute::InReg);
      Push(A.Padding.get(C), AB);
    }
    Sl.First = Params.size();
    switch (A.K) {
    case AK::Direct: {
      llvm::AttrBuilder AB(C);
      addExt(AB, A);
      llvm::Type *T = irType(C, A);
      if (A.Flatten && A.HasCoerce && T->isStructTy()) {
        for (llvm::Type *E : cast<llvm::StructType>(T)->elements())
          Push(E, AB);
      } else {
        Push(T, AB);
      }
      break;
    }
    case AK::Extend: {
      llvm::AttrBuilder AB(C);
      addExt(AB, A);
      Push(irType(C, A), AB);
      break;
    }
    case AK::Indirect: {
      llvm::AttrBuilder AB(C);
      if (A.ByVal)
        AB.addByValAttr(MemTy(A.Ty));
      else
        AB.addAttribute(llvm::Attribute::getWithDeadOnReturnInfo(
            C, llvm::DeadOnReturnInfo())); // the callee's own copy
      if (A.Align)
        AB.addAlignmentAttr(llvm::Align(A.Align));
      if (A.InReg)
        AB.addAttribute(llvm::Attribute::InReg);
      Push(Ptr, AB);
      break;
    }
    case AK::Expand: {
      std::vector<Leaf> Ls;
      collectLeaves(A.Ty, 0, nullptr, MemTy, Ls);
      for (const Leaf &L : Ls)
        Push(leafType(C, L), llvm::AttrBuilder(C));
      break;
    }
    case AK::CoerceAndExpand: {
      auto *ST = cast<llvm::StructType>(A.Coerce.get(C));
      for (unsigned I : A.ExpandIdx)
        Push(ST->getElementType(I), llvm::AttrBuilder(C));
      break;
    }
    default:
      break; // Ignore (and the refused kinds)
    }
    Sl.Count = Params.size() - Sl.First;
    S.Args.push_back(Sl);
  }
  S.Type = llvm::FunctionType::get(RetTy, Params, false);
  S.Attrs = llvm::AttributeList::get(C, llvm::AttributeSet(),
                                     llvm::AttributeSet::get(C, RetAB), PAttrs);
  return S;
}

//===----------------------------------------------------------------------===//
// Memory <-> IR values
//===----------------------------------------------------------------------===//

namespace {

using llvm::Align;

Align commonAlign(Align A, uint64_t Off) {
  return Off ? llvm::commonAlignment(A, Off) : A;
}

llvm::Value *at(Env &E, llvm::Value *P, uint64_t Off) {
  if (!Off)
    return P;
  return E.B.CreateConstInBoundsGEP1_64(E.B.getInt8Ty(), P, Off);
}

/// A scalar register value as the IR type an ABIArgInfo wants: the same type,
/// an extension or truncation for integers, and a round trip through memory
/// for the rest.
llvm::Value *toIR(Env &E, llvm::Value *V, const gir::Type *Ty,
                  llvm::Type *IRTy) {
  llvm::Type *VT = V->getType();
  if (VT == IRTy)
    return V;
  if (VT->isIntegerTy() && IRTy->isIntegerTy()) {
    if (VT->getIntegerBitWidth() > IRTy->getIntegerBitWidth())
      return E.B.CreateTrunc(V, IRTy);
    return Ty->isInt() && Ty->isSigned() ? E.B.CreateSExt(V, IRTy)
                                         : E.B.CreateZExt(V, IRTy);
  }
  uint64_t Sz =
      std::max(E.DL.getTypeAllocSize(VT), E.DL.getTypeAllocSize(IRTy));
  llvm::AllocaInst *Tmp = E.Alloca(
      llvm::ArrayType::get(E.B.getInt8Ty(), Sz),
      std::max(E.DL.getABITypeAlign(VT), E.DL.getABITypeAlign(IRTy)), "coerce");
  E.B.CreateMemSet(Tmp, E.B.getInt8(0), Sz, Tmp->getAlign());
  E.B.CreateAlignedStore(V, Tmp, E.DL.getABITypeAlign(VT));
  return E.B.CreateAlignedLoad(IRTy, Tmp, E.DL.getABITypeAlign(IRTy));
}

/// The reverse of toIR: the register value of the Go++ scalar type.
llvm::Value *fromIR(Env &E, llvm::Value *V, const gir::Type *Ty) {
  llvm::Type *Want = valueType(E.B.getContext(), Ty);
  llvm::Type *VT = V->getType();
  if (VT == Want)
    return V;
  if (VT->isIntegerTy() && Want->isIntegerTy()) {
    if (VT->getIntegerBitWidth() > Want->getIntegerBitWidth())
      return E.B.CreateTrunc(V, Want);
    return Ty->isInt() && Ty->isSigned() ? E.B.CreateSExt(V, Want)
                                         : E.B.CreateZExt(V, Want);
  }
  uint64_t Sz =
      std::max(E.DL.getTypeAllocSize(VT), E.DL.getTypeAllocSize(Want));
  llvm::AllocaInst *Tmp = E.Alloca(
      llvm::ArrayType::get(E.B.getInt8Ty(), Sz),
      std::max(E.DL.getABITypeAlign(VT), E.DL.getABITypeAlign(Want)), "coerce");
  E.B.CreateMemSet(Tmp, E.B.getInt8(0), Sz, Tmp->getAlign());
  E.B.CreateAlignedStore(V, Tmp, E.DL.getABITypeAlign(VT));
  return E.B.CreateAlignedLoad(Want, Tmp, E.DL.getABITypeAlign(Want));
}

/// Memory that can be read as ST: the value's own memory when it is at least
/// as big as ST (from byte Off), else a zeroed copy, so nothing past the
/// value is read.
struct View {
  llvm::Value *P;
  Align A;
};
View readView(Env &E, Value V, llvm::Type *MT, llvm::Type *ST, uint64_t Off) {
  uint64_t Have = E.DL.getTypeAllocSize(MT), Need = E.DL.getTypeAllocSize(ST);
  if (Off <= Have && Need <= Have - Off)
    return {at(E, V.V, Off), commonAlign(V.Known, Off)};
  Align TA = std::max(E.DL.getABITypeAlign(ST), E.DL.getABITypeAlign(MT));
  llvm::AllocaInst *Tmp = E.Alloca(ST, TA, "coerce");
  E.B.CreateMemSet(Tmp, E.B.getInt8(0), Need, TA);
  if (Off < Have)
    E.B.CreateMemCpy(Tmp, TA, at(E, V.V, Off), commonAlign(V.Known, Off),
                     Have - Off);
  return {Tmp, TA};
}

/// Memory to write ST into, for storage of type MT at V from byte Off: the
/// storage itself when ST fits, else a temporary that finish() copies the
/// part that fits from.
struct WriteView {
  Env &E;
  Value Dst;
  llvm::Type *MT;
  uint64_t Off;
  llvm::Value *P;
  Align A;
  llvm::Value *Tmp = nullptr;
  uint64_t CopyBytes = 0;
  WriteView(Env &E, Value Dst, llvm::Type *MT, llvm::Type *ST, uint64_t Off)
      : E(E), Dst(Dst), MT(MT), Off(Off) {
    uint64_t Have = E.DL.getTypeAllocSize(MT), Need = E.DL.getTypeAllocSize(ST);
    if (Off <= Have && Need <= Have - Off) {
      P = at(E, Dst.V, Off);
      A = commonAlign(Dst.Known, Off);
      return;
    }
    A = std::max(E.DL.getABITypeAlign(ST), E.DL.getABITypeAlign(MT));
    llvm::AllocaInst *T = E.Alloca(ST, A, "coerce");
    E.B.CreateMemSet(T, E.B.getInt8(0), Need, A);
    P = Tmp = T;
    CopyBytes = Off < Have ? Have - Off : 0;
  }
  void finish() {
    if (Tmp && CopyBytes)
      E.B.CreateMemCpy(at(E, Dst.V, Off), commonAlign(Dst.Known, Off), Tmp, A,
                       CopyBytes);
  }
};

/// The value of the memory of V (type MT) as the first-class type CT, from
/// byte Off.
llvm::Value *loadCoerced(Env &E, Value V, llvm::Type *MT, llvm::Type *CT,
                         uint64_t Off) {
  View W = readView(E, V, MT, CT, Off);
  return E.B.CreateAlignedLoad(CT, W.P, W.A);
}

/// Stores the first-class Val into the storage of V from byte Off.
void storeCoerced(Env &E, llvm::Value *Val, Value Dst, llvm::Type *MT,
                  uint64_t Off) {
  WriteView W(E, Dst, MT, Val->getType(), Off);
  E.B.CreateAlignedStore(Val, W.P, W.A);
  W.finish();
}

llvm::Value *loadLeaf(Env &E, Value V, const Leaf &L) {
  llvm::LLVMContext &C = E.B.getContext();
  llvm::Value *P = at(E, V.V, L.Off);
  Align A = commonAlign(V.Known, L.Off);
  if (!L.Ty)
    return E.B.CreateAlignedLoad(E.B.getInt8Ty(), P, A);
  if (L.Ty->isBool())
    return E.B.CreateICmpNE(E.B.CreateAlignedLoad(E.B.getInt8Ty(), P, A),
                            E.B.getInt8(0));
  return E.B.CreateAlignedLoad(valueType(C, L.Ty), P, A);
}

void storeLeaf(Env &E, llvm::Value *Val, Value Dst, const Leaf &L) {
  llvm::Value *P = at(E, Dst.V, L.Off);
  if (L.Ty && L.Ty->isBool())
    Val = E.B.CreateZExt(Val, E.B.getInt8Ty());
  E.B.CreateAlignedStore(Val, P, commonAlign(Dst.Known, L.Off));
}

Align naturalAlign(Env &E, const gir::Type *T) {
  return E.DL.getABITypeAlign(E.MemTy(T));
}

/// The alignment an indirect argument or result must have.
Align neededAlign(Env &E, const ArgInfo &A) {
  return std::max(naturalAlign(E, A.Ty), Align(A.Align ? A.Align : 1));
}

} // namespace

//===----------------------------------------------------------------------===//
// Calling C
//===----------------------------------------------------------------------===//

PreparedCall cabi::lowerCArgs(Env &E, const CallInfo &CI,
                              const IRSignature &Sig,
                              llvm::ArrayRef<Value> Args, Value RetDest) {
  PreparedCall P;
  llvm::LLVMContext &C = E.B.getContext();
  if (CI.Ret.K == AK::Indirect) {
    // The callee writes the result while it runs: not into the destination,
    // which an argument may point at (`x = f(&x)`), but into a slot of its
    // own that lowerCResult copies from.
    llvm::Type *MT = E.MemTy(CI.Ret.Ty);
    P.SRetSlot = E.Alloca(MT, neededAlign(E, CI.Ret), "sret");
    P.Args.push_back(P.SRetSlot);
  }
  (void)Sig;
  (void)RetDest;
  for (size_t I = 0; I < CI.Args.size(); ++I) {
    const ArgInfo &A = CI.Args[I];
    const Value &V = Args[I];
    if (A.HasPadding)
      P.Args.push_back(llvm::PoisonValue::get(A.Padding.get(C)));
    llvm::Type *MT = isAgg(A.Ty) ? E.MemTy(A.Ty) : nullptr;
    switch (A.K) {
    case AK::Direct:
    case AK::Extend:
      if (!MT) {
        P.Args.push_back(toIR(E, V.V, A.Ty, irType(C, A)));
        break;
      }
      if (A.Flatten && A.Coerce.K == LTy::Struct) {
        auto *ST = cast<llvm::StructType>(A.Coerce.get(C));
        View W = readView(E, V, MT, ST, A.Offset);
        const llvm::StructLayout *SL = E.DL.getStructLayout(ST);
        for (unsigned K = 0; K < ST->getNumElements(); ++K)
          P.Args.push_back(E.B.CreateAlignedLoad(
              ST->getElementType(K), at(E, W.P, SL->getElementOffset(K)),
              commonAlign(W.A, SL->getElementOffset(K))));
      } else {
        P.Args.push_back(loadCoerced(E, V, MT, A.Coerce.get(C), A.Offset));
      }
      break;
    case AK::Indirect: {
      Align Need = neededAlign(E, A);
      if (V.Known >= Need) {
        P.Args.push_back(V.V);
        break;
      }
      llvm::AllocaInst *Tmp = E.Alloca(MT, Need, "indirect");
      E.B.CreateMemCpy(Tmp, Need, V.V, V.Known, E.DL.getTypeAllocSize(MT));
      P.Args.push_back(Tmp);
      break;
    }
    case AK::Expand: {
      std::vector<Leaf> Ls;
      collectLeaves(A.Ty, 0, &E.DL, E.MemTy, Ls);
      for (const Leaf &L : Ls)
        P.Args.push_back(loadLeaf(E, V, L));
      break;
    }
    case AK::CoerceAndExpand: {
      auto *ST = cast<llvm::StructType>(A.Coerce.get(C));
      View W = readView(E, V, MT, ST, 0);
      const llvm::StructLayout *SL = E.DL.getStructLayout(ST);
      for (unsigned K : A.ExpandIdx)
        P.Args.push_back(E.B.CreateAlignedLoad(
            ST->getElementType(K), at(E, W.P, SL->getElementOffset(K)),
            commonAlign(W.A, SL->getElementOffset(K))));
      break;
    }
    default:
      break; // Ignore (the refused kinds never get here)
    }
  }
  assert(P.Args.size() == Sig.Type->getNumParams() && "IR argument count");
  return P;
}

llvm::Value *cabi::lowerCResult(Env &E, const CallInfo &CI,
                                const IRSignature &Sig, PreparedCall &P,
                                llvm::CallBase *Call, Value RetDest) {
  (void)Sig;
  const ArgInfo &R = CI.Ret;
  llvm::LLVMContext &C = E.B.getContext();
  switch (R.K) {
  case AK::Direct:
  case AK::Extend:
    if (!isAgg(R.Ty))
      return fromIR(E, Call, R.Ty);
    storeCoerced(E, Call, RetDest, E.MemTy(R.Ty), R.Offset);
    return nullptr;
  case AK::Indirect: {
    llvm::Type *MT = E.MemTy(R.Ty);
    E.B.CreateMemCpy(RetDest.V, RetDest.Known, P.SRetSlot,
                     cast<llvm::AllocaInst>(P.SRetSlot)->getAlign(),
                     E.DL.getTypeAllocSize(MT));
    return nullptr;
  }
  case AK::CoerceAndExpand: {
    auto *ST = cast<llvm::StructType>(R.Coerce.get(C));
    WriteView W(E, RetDest, E.MemTy(R.Ty), ST, 0);
    const llvm::StructLayout *SL = E.DL.getStructLayout(ST);
    for (size_t K = 0; K < R.ExpandIdx.size(); ++K) {
      llvm::Value *El = R.ExpandIdx.size() == 1
                            ? static_cast<llvm::Value *>(Call)
                            : E.B.CreateExtractValue(Call, unsigned(K));
      uint64_t Off = SL->getElementOffset(R.ExpandIdx[K]);
      E.B.CreateAlignedStore(El, at(E, W.P, Off), commonAlign(W.A, Off));
    }
    W.finish();
    return nullptr;
  }
  default:
    return nullptr; // Ignore / void
  }
}

//===----------------------------------------------------------------------===//
// Being called by C (P04.18)
//===----------------------------------------------------------------------===//

std::vector<Value>
cabi::unpackCParams(Env &E, const CallInfo &CI, const IRSignature &Sig,
                    llvm::ArrayRef<llvm::Argument *> IRArgs) {
  std::vector<Value> Out;
  llvm::LLVMContext &C = E.B.getContext();
  for (size_t I = 0; I < CI.Args.size(); ++I) {
    const ArgInfo &A = CI.Args[I];
    IRSignature::Slot Sl = Sig.Args[I];
    Value V;
    V.Ty = A.Ty;
    if (!isAgg(A.Ty)) {
      // A scalar: one IR argument (a refused or ignored kind has none).
      V.V = Sl.Count ? fromIR(E, IRArgs[Sl.First], A.Ty)
                     : llvm::PoisonValue::get(valueType(C, A.Ty));
      Out.push_back(V);
      continue;
    }
    llvm::Type *MT = E.MemTy(A.Ty);
    if (A.K == AK::Indirect) {
      // The caller's copy (byval: a copy made for this call) is the storage.
      V.V = IRArgs[Sl.First];
      V.Known = neededAlign(E, A);
      Out.push_back(V);
      continue;
    }
    Align Nat = naturalAlign(E, A.Ty);
    llvm::AllocaInst *Slot = E.Alloca(MT, Nat, "param");
    V.V = Slot;
    V.Known = Slot->getAlign();
    switch (A.K) {
    case AK::Direct:
    case AK::Extend:
      if (A.Flatten && A.Coerce.K == LTy::Struct) {
        auto *ST = cast<llvm::StructType>(A.Coerce.get(C));
        WriteView W(E, V, MT, ST, A.Offset);
        const llvm::StructLayout *SL = E.DL.getStructLayout(ST);
        for (unsigned K = 0; K < ST->getNumElements(); ++K)
          E.B.CreateAlignedStore(IRArgs[Sl.First + K],
                                 at(E, W.P, SL->getElementOffset(K)),
                                 commonAlign(W.A, SL->getElementOffset(K)));
        W.finish();
      } else {
        storeCoerced(E, IRArgs[Sl.First], V, MT, A.Offset);
      }
      break;
    case AK::Expand: {
      std::vector<Leaf> Ls;
      collectLeaves(A.Ty, 0, &E.DL, E.MemTy, Ls);
      for (size_t K = 0; K < Ls.size(); ++K)
        storeLeaf(E, IRArgs[Sl.First + K], V, Ls[K]);
      break;
    }
    case AK::CoerceAndExpand: {
      auto *ST = cast<llvm::StructType>(A.Coerce.get(C));
      WriteView W(E, V, MT, ST, 0);
      const llvm::StructLayout *SL = E.DL.getStructLayout(ST);
      for (size_t K = 0; K < A.ExpandIdx.size(); ++K) {
        uint64_t Off = SL->getElementOffset(A.ExpandIdx[K]);
        E.B.CreateAlignedStore(IRArgs[Sl.First + K], at(E, W.P, Off),
                               commonAlign(W.A, Off));
      }
      W.finish();
      break;
    }
    default:
      break; // Ignore: a zero-size slot
    }
    Out.push_back(V);
  }
  return Out;
}

void cabi::packCReturn(Env &E, const CallInfo &CI, const IRSignature &Sig,
                       llvm::ArrayRef<llvm::Argument *> IRArgs, Value Result) {
  const ArgInfo &R = CI.Ret;
  llvm::LLVMContext &C = E.B.getContext();
  switch (R.K) {
  case AK::Direct:
  case AK::Extend:
    if (!isAgg(R.Ty))
      E.B.CreateRet(toIR(E, Result.V, R.Ty, irType(C, R)));
    else
      E.B.CreateRet(
          loadCoerced(E, Result, E.MemTy(R.Ty), R.Coerce.get(C), R.Offset));
    return;
  case AK::Indirect: {
    llvm::Type *MT = E.MemTy(R.Ty);
    Align Need = neededAlign(E, R);
    E.B.CreateMemCpy(IRArgs[Sig.SRet], Need, Result.V, Result.Known,
                     E.DL.getTypeAllocSize(MT));
    E.B.CreateRetVoid();
    return;
  }
  case AK::CoerceAndExpand: {
    auto *ST = cast<llvm::StructType>(R.Coerce.get(C));
    View W = readView(E, Result, E.MemTy(R.Ty), ST, 0);
    const llvm::StructLayout *SL = E.DL.getStructLayout(ST);
    llvm::Type *RT = unpaddedType(C, R);
    llvm::Value *Agg = llvm::PoisonValue::get(RT);
    for (size_t K = 0; K < R.ExpandIdx.size(); ++K) {
      uint64_t Off = SL->getElementOffset(R.ExpandIdx[K]);
      llvm::Value *El =
          E.B.CreateAlignedLoad(ST->getElementType(R.ExpandIdx[K]),
                                at(E, W.P, Off), commonAlign(W.A, Off));
      if (R.ExpandIdx.size() == 1)
        Agg = El;
      else
        Agg = E.B.CreateInsertValue(Agg, El, unsigned(K));
    }
    E.B.CreateRet(Agg);
    return;
  }
  default:
    E.B.CreateRetVoid();
    return;
  }
}
