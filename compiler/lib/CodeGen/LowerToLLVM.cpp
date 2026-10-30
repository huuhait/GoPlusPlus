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
// at or above the width, saturating float to integer, FMA contraction as gc
// does it on each target.
//
// A bool is i1 in registers and i8 in memory. An aggregate is never a
// first-class LLVM value: it lives in memory (isAggregate).
//
//===----------------------------------------------------------------------===//

#include "gopp/Basic/Diagnostics.h"
#include "gopp/Basic/NewStack.h"
#include "gopp/CodeGen/CABILower.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/GIR/GIR.h"
#include "gopp/GIR/Verify.h"
#include "gopp/Import/CABI.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
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

#include <map>
#include <optional>
#include <tuple>

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
  /// A declared C function: how its signature crosses the C ABI (P04.11,
  /// CABILower.h). Keyed by the function's index in the module.
  struct CFunction {
    cabi::CallInfo Info;
    cabi::IRSignature Sig;
  };
  llvm::DenseMap<G::FuncId, CFunction> CFuncs;
  /// Made for the first signature with an aggregate (scalars need no Clang).
  std::unique_ptr<cabi::Classifier> Classifier;
  /// The aggregates whose layout has been checked against Clang's.
  llvm::DenseSet<const G::Type *> CLayoutChecked;
  void cabiWork(uint64_t N = 1) {
    if (Opts.CABIWork)
      *Opts.CABIWork += N;
  }
  /// Scratch slots of the call being made (CABILower's temporaries): one per
  /// (type, alignment, use in this call), shared by all calls of the
  /// function, as ArgSlots is.
  std::map<std::tuple<llvm::Type *, uint64_t, unsigned>, llvm::AllocaInst *>
      CScratch;
  std::map<std::pair<llvm::Type *, uint64_t>, unsigned> CScratchUsed;
  llvm::Error classifyCFunctions();
  llvm::Error checkCLayout(const G::Type *T);
  llvm::AllocaInst *cScratchSlot(llvm::Type *Ty, llvm::Align A,
                                 const llvm::Twine &Name);
  llvm::SmallVector<llvm::Function *, 16> Funcs;
  llvm::SmallVector<llvm::GlobalVariable *, 16> Globals;
  llvm::SmallVector<llvm::GlobalVariable *, 16> Strings;
  llvm::SmallVector<llvm::DIFile *, 4> DIFiles;
  llvm::DISubprogram *SP = nullptr;

  // Per function.
  const G::Function *GF = nullptr;
  llvm::Function *LF = nullptr;
  /// The address of each local's storage: its alloca, or for a result that
  /// is an aggregate the caller's buffer, and for an aggregate parameter the
  /// caller's copy. Null for a local of unit type.
  llvm::SmallVector<llvm::Value *, 32> Slots;
  llvm::SmallVector<llvm::BasicBlock *, 16> Blocks;
  /// The shared panic block of each runtime panic function in this function
  /// (ADR-0028 rule 3.3: one per AssertKind, and Bounds has two, as a signed
  /// and an unsigned index print differently): every failing check stores
  /// its constant position and its values in the slots and branches here;
  /// the block calls the runtime once.
  struct PanicSite {
    llvm::BasicBlock *BB = nullptr;
    llvm::AllocaInst *Pos = nullptr;
    llvm::SmallVector<llvm::AllocaInst *, 2> Args;
  };
  llvm::DenseMap<G::FuncId, PanicSite> Panics;
  /// The {file, line, col} constant of each position a check names.
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, llvm::Constant *>
      Positions;
  llvm::AllocaInst *entryAlloca(llvm::Type *Ty, const llvm::Twine &Name);
  /// The slots that hold the callee's copies of aggregate arguments: one per
  /// (type, index among the aggregate arguments of a call), shared by all
  /// the calls of the function. The copies of a call are made right before
  /// it and dead when it returns, and the calls of a function are one after
  /// the other (an inner call is done before the outer one's copies are
  /// made), so only the arguments of one call must be apart.
  std::map<std::pair<llvm::Type *, unsigned>, llvm::AllocaInst *> ArgSlots;
  llvm::Constant *positionConst(const G::Span &S);
  /// A constant as it is stored in memory (a bool is a byte).
  llvm::Constant *memConstant(const G::Constant &K);

  /// FMA contraction (D4.12): gc fuses a float x*y with the + or - that
  /// reads it on arm64, never on amd64. On arm64 every float operation
  /// carries `contract`, and an explicit float conversion is an
  /// `llvm.arithmetic.fence` (the Go spec: a conversion rounds). The opt-0
  /// pipeline has no combiner that acts on `contract`, so the code generator
  /// contracts what the flags allow itself, as gc's SSA rewrite does
  /// (`FADDD a (FMULD x y)` with a single-use product): a product held in a
  /// temporary that is read once, by a + or - in the same block or in a block
  /// the product's block dominates, becomes `llvm.fma`, the right operand
  /// first when both are products, as gc's rules choose. A call, a bounds
  /// check or a division check ends a block, so `x*y + f()` and
  /// `a[i]*b[i] + c[i]` fuse across blocks. Across blocks the temporary must
  /// also be written once in the whole function (a write in between could
  /// not be seen), and the factors are values of the product's block, which
  /// dominates the use, so they are valid there. A named variable is never a
  /// product (`t := x*y; t+z` waits for P04.13), nor a temporary that is
  /// assigned again before the read (the entry is dropped).
  /// Products maps such a temporary to its two factors and its block; Uses
  /// counts the reads and Defs the writes of each local.
  bool ContractFP = false;
  struct ProductInfo {
    llvm::Value *X, *Y;
    G::BlockId Block;
  };
  llvm::DenseMap<G::LocalId, ProductInfo> Products;
  llvm::SmallVector<unsigned, 32> Uses, Defs;
  G::BlockId CurBlock = 0;
  /// The dominator tree of the blocks of the function (computed on the first
  /// cross-block candidate): DomIn/DomOut number its depth-first walk, so
  /// "A dominates B" is an interval test; an unreachable block has none.
  std::vector<uint32_t> DomIn, DomOut;
  bool DomReady = false;
  void computeDominators(const G::Function &F);
  bool dominates(G::BlockId A, G::BlockId B);
  void countUses(const G::Function &F);
  llvm::Value *contracted(const G::Rvalue &R, llvm::Value *X, llvm::Value *Y);
  /// The factors of the product that operand O holds, if it is a single-use
  /// product temporary that is still valid here (see Products).
  const ProductInfo *productOf(const G::Operand &O);

  /// Aggregates (strings, slices, arrays, tuples, structs, Options) live in
  /// memory: they are copied with memmove, zeroed with memset and built in
  /// place, and the internal convention (until P04.18, D4.4) passes them by
  /// address: a parameter is a pointer to the caller's own copy, a result is
  /// written through a hidden first parameter. LLVM never sees a first-class
  /// value of one (a [100000]int64 would not compile).
  static bool isAggregate(const G::Type *T) {
    switch (T->getKind()) {
    case G::Type::Kind::String:
    case G::Type::Kind::Slice:
    case G::Type::Kind::Array:
    case G::Type::Kind::Tuple:
    case G::Type::Kind::Struct:
    case G::Type::Kind::Option:
      return true;
    default:
      return false;
    }
  }
  /// Bytes a value of the type takes in memory.
  uint64_t sizeOf(const G::Type *T) {
    return LM.getDataLayout().getTypeAllocSize(memTy(T));
  }
  void copyAggregate(llvm::Value *Dst, llvm::Value *Src, const G::Type *T);
  void zeroAggregate(llvm::Value *Dst, const G::Type *T);
  /// Stores the constant into the aggregate at Dst.
  void storeAggregateConst(llvm::Value *Dst, const G::Constant &K);
  /// Makes the aggregate value of O live at Dst (a copy or the constant).
  void emitInto(llvm::Value *Dst, const G::Operand &O);
  /// Dest = R for an aggregate Dest: a copy, or an aggregate built in place
  /// (V057: no operand reads Dest's local).
  llvm::Error assignAggregate(const G::Place &Dest, const G::Rvalue &R);

  llvm::Error fail(const llvm::Twine &What) {
    return llvm::createStringError(
        std::errc::not_supported, "code generation: %s in %s",
        What.str().c_str(), GF ? GF->Name.c_str() : "the module");
  }

  /// memTy recurses once per level of the type (a chain of named types has
  /// thousands): a new stack every DeepWalk::Segment levels.
  DeepWalk MemWalk;
  llvm::Type *memTy(const G::Type *T);
  void prepareTypes();
  llvm::Error nestingError() const {
    return llvm::createStringError(
        std::errc::not_enough_memory, "%s",
        formatDiagnostic(getDiagFormat(diag::err_no_stack),
                         {std::to_string(NoStackAt)})
            .c_str());
  }
  /// The depth at which no stack could be had for memTy (0: none).
  unsigned NoStackAt = 0;
  llvm::Type *valTy(const G::Type *T) {
    return T->isBool() ? llvm::Type::getInt1Ty(C) : memTy(T);
  }
  llvm::FunctionType *fnType(const G::Type *Sig);
  llvm::Function *declare(G::FuncId Id, const G::Function &F);
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
  /// The symbol of the runtime function a failed Assert calls.
  static std::string panicName(const G::Terminator &T);
};

llvm::Type *Lowering::memTy(const G::Type *T) {
  auto It = MemTypes.find(T);
  if (It != MemTypes.end())
    return It->second;
  llvm::Type *R = nullptr;
  llvm::Type *I64 = llvm::Type::getInt64Ty(C);
  llvm::Type *Ptr = llvm::PointerType::getUnqual(C);
  bool Ran = MemWalk.level([&] {
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
    case G::Type::Kind::Option:
      R = llvm::StructType::get(C,
                                {memTy(T->elem()), llvm::Type::getInt8Ty(C)});
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
      // LLVM computes the layout of a struct, recursively through the structs
      // it holds, the first time it is asked, on whatever stack asks: a chain
      // of 60000 structs overflowed it. Asking here, right after the body, the
      // inner layouts are cached already, so each ask is one level deep and
      // runs inside the segments of MemWalk (R18).
      // (isSized is recursive the same way, and caches the answer.)
      (void)S->isSized();
      (void)LM.getDataLayout().getStructLayout(S);
      R = S;
      break;
    }
    }
  });
  if (!Ran) {
    // No stack for the rest of a chain of types this deep. R is a stand-in
    // that nothing uses: prepareTypes runs before any code is made, and stops
    // the lowering with the error (noStack).
    if (!NoStackAt)
      NoStackAt = MemWalk.failedAt();
    R = llvm::Type::getInt8Ty(C);
  }
  MemTypes[T] = R;
  return R;
}

/// The LLVM type of every type the module can name, before any code is made:
/// the types of globals, signatures and locals and what they hold, through
/// pointers too (memTy stops at a pointer). A chain of types too deep for the
/// stacks that can be had is found here, once, so no code is made with a
/// stand-in type (err_no_stack).
void Lowering::prepareTypes() {
  llvm::SmallVector<const G::Type *, 64> Work;
  llvm::DenseSet<const G::Type *> Seen;
  auto Add = [&](const G::Type *T) {
    if (T && Seen.insert(T).second)
      Work.push_back(T);
  };
  for (const G::Global &Gl : GM.Globals)
    Add(Gl.Ty);
  for (const auto &F : GM.Funcs) {
    Add(F->Sig);
    for (const G::Local &L : F->Locals)
      Add(L.Ty);
  }
  while (!Work.empty() && !NoStackAt) {
    const G::Type *T = Work.pop_back_val();
    if (!(T->getKind() == G::Type::Kind::Func))
      memTy(T);
    if (T->isPtr() || T->getKind() == G::Type::Kind::Slice ||
        T->getKind() == G::Type::Kind::Array ||
        T->getKind() == G::Type::Kind::Option)
      Add(T->elem());
    if ((T->getKind() == G::Type::Kind::Func))
      Add(T->ret());
    if ((T->getKind() == G::Type::Kind::Func) ||
        T->getKind() == G::Type::Kind::Tuple ||
        T->getKind() == G::Type::Kind::Struct)
      for (const G::Type *E : T->elems())
        Add(E);
  }
}

/// The internal convention until P04.18 (D4.4): a value that is not one
/// scalar goes by address. An aggregate parameter is a pointer to the
/// caller's own copy, which the callee uses as the storage of the parameter;
/// an aggregate result is written through a hidden first parameter, the
/// address of the result place. The C convention has scalars only (P04.11).
llvm::FunctionType *Lowering::fnType(const G::Type *Sig) {
  llvm::SmallVector<llvm::Type *, 4> Params;
  llvm::Type *Ptr = llvm::PointerType::getUnqual(C);
  bool Sret = isAggregate(Sig->ret());
  if (Sret)
    Params.push_back(Ptr);
  for (const G::Type *P : Sig->elems())
    Params.push_back(isAggregate(P) ? Ptr : valTy(P));
  llvm::Type *Ret = Sig->ret()->isUnit() || Sret ? llvm::Type::getVoidTy(C)
                                                 : valTy(Sig->ret());
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

/// Classifies every declared C function (P04.11). A signature of scalars only
/// has the classification Clang would give without asking Clang
/// (cabi::classifyScalars); Clang is asked from the first signature with an
/// aggregate on (its world is made once per process, M3), so a module that
/// passes no aggregate to C never starts it.
llvm::Error Lowering::classifyCFunctions() {
  const std::string Triple = LM.getTargetTriple().str();
  for (size_t I = 0; I < GM.Funcs.size(); ++I) {
    const G::Function &F = *GM.Funcs[I];
    if (F.CC != G::CallConv::C || !F.isDeclaration())
      continue;
    cabiWork();
    std::optional<cabi::CallInfo> CI = cabi::classifyScalars(Triple, F.Sig);
    if (!CI) {
      if (!Classifier) {
        cabi::ClassifierOptions CO;
        CO.Triple = Triple;
        CO.CPU = Opts.TargetCPU;
        CO.Features = Opts.TargetFeatures;
        auto Made = cabi::Classifier::create(CO);
        if (!Made)
          return Made.takeError();
        Classifier = std::move(*Made);
        if (Classifier->dataLayout() !=
            LM.getDataLayout().getStringRepresentation())
          return llvm::createStringError(
              std::errc::invalid_argument,
              "internal error: Clang's data layout for %s is %s, LLVM's is %s",
              Triple.c_str(), Classifier->dataLayout().c_str(),
              LM.getDataLayout().getStringRepresentation().c_str());
      }
      for (const G::Type *P : F.Sig->elems())
        if (llvm::Error E = checkCLayout(P))
          return E;
      if (llvm::Error E = checkCLayout(F.Sig->ret()))
        return E;
      CI = Classifier->classify(F.Sig);
    }
    if (llvm::Error E = cabi::checkSupported(*CI))
      return llvm::createStringError(
          std::errc::not_supported, "code generation: %s in %s",
          llvm::toString(std::move(E)).c_str(), F.Name.c_str());
    cabi::IRSignature Sig =
        cabi::irSignature(C, *CI, [&](const G::Type *T) { return memTy(T); });
    CFuncs[I] = {std::move(*CI), std::move(Sig)};
  }
  if (Classifier)
    cabiWork(Classifier->stats().TypesMapped);
  return llvm::Error::success();
}

/// Clang and Go++ lay out every aggregate that crosses the C ABI the same
/// way: size, alignment and the offset of every field (of T and of the
/// structs, arrays and Options inside it), checked before any code is made.
/// They agree by construction (gir::CTypeRules, memTy); a difference would be
/// a compiler bug that corrupts calls, so it is an internal error here. The
/// walk is bounded by gir::MaxCTypeDepth (the verifier's V067 ran first).
llvm::Error Lowering::checkCLayout(const G::Type *T) {
  if (!isAggregate(T) || !CLayoutChecked.insert(T).second)
    return llvm::Error::success();
  cabiWork();
  const llvm::DataLayout &DL = LM.getDataLayout();
  llvm::Type *MT = memTy(T);
  cabi::ClangLayout CL = Classifier->layoutOf(T);
  bool Same = CL.Size == DL.getTypeAllocSize(MT) &&
              CL.Align == DL.getABITypeAlign(MT).value();
  if (Same && (T->getKind() == G::Type::Kind::Struct || T->isOption())) {
    const llvm::StructLayout *SL =
        DL.getStructLayout(cast<llvm::StructType>(MT));
    Same = CL.Offsets.size() == MT->getStructNumElements();
    for (unsigned K = 0; Same && K < CL.Offsets.size(); ++K) {
      cabiWork();
      Same = CL.Offsets[K] == SL->getElementOffset(K);
    }
  }
  if (!Same)
    return llvm::createStringError(
        std::errc::invalid_argument,
        "internal error: Clang lays %s out in %llu bytes aligned to %llu, "
        "Go++ in %llu aligned to %llu, or puts a field elsewhere",
        T->str().c_str(), (unsigned long long)CL.Size,
        (unsigned long long)CL.Align,
        (unsigned long long)DL.getTypeAllocSize(MT),
        (unsigned long long)DL.getABITypeAlign(MT).value());
  if (T->getKind() == G::Type::Kind::Array || T->isOption())
    return checkCLayout(T->elem());
  for (const G::Type *F : T->elems())
    if (llvm::Error E = checkCLayout(F))
      return E;
  return llvm::Error::success();
}

llvm::AllocaInst *Lowering::cScratchSlot(llvm::Type *Ty, llvm::Align A,
                                         const llvm::Twine &Name) {
  unsigned &Used = CScratchUsed[{Ty, A.value()}];
  llvm::AllocaInst *&Slot = CScratch[{Ty, A.value(), Used++}];
  if (!Slot) {
    Slot = entryAlloca(Ty, Name);
    Slot->setAlignment(std::max(A, LM.getDataLayout().getABITypeAlign(Ty)));
  }
  return Slot;
}

llvm::Function *Lowering::declare(G::FuncId Id, const G::Function &F) {
  auto CFIt = CFuncs.find(Id);
  llvm::Function *LF = llvm::Function::Create(
      CFIt != CFuncs.end() ? CFIt->second.Sig.Type : fnType(F.Sig),
      F.Link == G::Linkage::Internal ? llvm::GlobalValue::InternalLinkage
      : F.Link == G::Linkage::LinkOnceODR
          ? llvm::GlobalValue::LinkOnceODRLinkage
          : llvm::GlobalValue::ExternalLinkage,
      F.Name, &LM);
  llvm::AttributeList AL = LF->getAttributes();
  if (CFIt != CFuncs.end())
    AL = CFIt->second.Sig.Attrs; // the C ABI's: extension, byval, sret, ...
  if (F.NoUnwind)
    AL = AL.addFnAttribute(C, llvm::Attribute::NoUnwind);
  if (F.NoReturn)
    AL = AL.addFnAttribute(C, llvm::Attribute::NoReturn);
  // Unwind tables for every function Go++ defines, so a stack trace works
  // through any frame (D4.6; clang's default on Linux).
  if (!F.isDeclaration())
    AL = AL.addFnAttribute(
        C, llvm::Attribute::getWithUWTableKind(C, llvm::UWTableKind::Async));
  if (CFIt == CFuncs.end()) {
    unsigned Sret = isAggregate(F.Sig->ret()) ? 1 : 0;
    for (unsigned I = 0; I < F.Sig->elems().size(); ++I)
      extAttr(C, AL, llvm::AttributeList::FirstArgIndex + Sret + I,
              F.Sig->elems()[I]);
    if (!F.Sig->ret()->isUnit() && !Sret)
      extAttr(C, AL, llvm::AttributeList::ReturnIndex, F.Sig->ret());
  }
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
      } else { // a slice or a string: the data pointer is field 0
        llvm::Value *Data =
            B.CreateLoad(llvm::PointerType::getUnqual(C),
                         B.CreateStructGEP(memTy(Cur), Addr, 0));
        llvm::Type *ElemTy = Cur->getKind() == G::Type::Kind::String
                                 ? llvm::Type::getInt8Ty(C)
                                 : memTy(Cur->elem());
        Addr = B.CreateInBoundsGEP(ElemTy, Data, {Idx});
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
  case G::Constant::Global:
    return Globals[K.Bits];
  }
  return nullptr;
}

llvm::Constant *Lowering::memConstant(const G::Constant &K) {
  if (K.K == G::Constant::Zero)
    return llvm::Constant::getNullValue(memTy(K.Ty));
  if (K.K == G::Constant::Bool)
    return B.getInt8(K.Bits);
  return cast<llvm::Constant>(constant(K));
}

llvm::AllocaInst *Lowering::entryAlloca(llvm::Type *Ty,
                                        const llvm::Twine &Name) {
  // Before the entry block's branch to the first block: every alloca of the
  // function is in the entry block (mem2reg, no stack growth in loops).
  llvm::BasicBlock &E = LF->getEntryBlock();
  return new llvm::AllocaInst(Ty, LM.getDataLayout().getAllocaAddrSpace(),
                              nullptr, Name, E.getTerminator()->getIterator());
}

/// The position of a check as a private constant {file, line, col} (the
/// layout the runtime's panic functions read; File is 1 + the index in
/// Module::Files, 0 for none). One constant per position of the module.
llvm::Constant *Lowering::positionConst(const G::Span &S) {
  llvm::Constant *&GV = Positions[{S.File, S.Line, S.Col}];
  if (GV)
    return GV;
  llvm::Type *I32 = B.getInt32Ty();
  llvm::StructType *PT = llvm::StructType::get(C, {I32, I32, I32});
  llvm::Constant *Init = llvm::ConstantStruct::get(
      PT, {B.getInt32(S.File), B.getInt32(S.Line), B.getInt32(S.Col)});
  auto *Var = new llvm::GlobalVariable(
      LM, PT, true, llvm::GlobalValue::PrivateLinkage, Init, ".pos");
  Var->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  GV = Var;
  return GV;
}

void Lowering::copyAggregate(llvm::Value *Dst, llvm::Value *Src,
                             const G::Type *T) {
  uint64_t N = sizeOf(T);
  if (N == 0)
    return;
  // memmove: `x = x` and overlapping parts of one array are legal Go.
  B.CreateMemMove(Dst, llvm::MaybeAlign(), Src, llvm::MaybeAlign(), N);
}

void Lowering::zeroAggregate(llvm::Value *Dst, const G::Type *T) {
  uint64_t N = sizeOf(T);
  if (N)
    B.CreateMemSet(Dst, B.getInt8(0), N, llvm::MaybeAlign());
}

void Lowering::storeAggregateConst(llvm::Value *Dst, const G::Constant &K) {
  if (K.K == G::Constant::Zero) {
    zeroAggregate(Dst, K.Ty);
    return;
  }
  // A string literal, the only other aggregate constant: {ptr, len, 0}.
  B.CreateStore(constant(K), Dst);
}

void Lowering::emitInto(llvm::Value *Dst, const G::Operand &O) {
  if (O.K == G::Operand::Const) {
    storeAggregateConst(Dst, O.C);
    return;
  }
  copyAggregate(Dst, placeAddr(O.P), O.P.Ty);
}

llvm::Error Lowering::assignAggregate(const G::Place &Dest,
                                      const G::Rvalue &R) {
  llvm::Value *Addr = placeAddr(Dest);
  switch (R.K) {
  case G::Rvalue::Use:
    emitInto(Addr, R.A);
    return llvm::Error::success();
  case G::Rvalue::Aggregate: {
    // Each element is built in place (V057: no operand reads Dest's local).
    llvm::Type *T = memTy(R.Ty);
    for (unsigned I = 0; I < R.Ops.size(); ++I) {
      const G::Operand &Op = R.Ops[I];
      llvm::Value *EA = R.Ty->getKind() == G::Type::Kind::Array
                            ? B.CreateConstInBoundsGEP2_64(T, Addr, 0, I)
                            : B.CreateStructGEP(T, Addr, I);
      if (isAggregate(Op.type())) {
        emitInto(EA, Op);
      } else if (!Op.type()->isUnit()) {
        llvm::Value *V = operand(Op);
        if (Op.type()->isBool())
          V = B.CreateZExt(V, B.getInt8Ty());
        B.CreateStore(V, EA);
      }
    }
    return llvm::Error::success();
  }
  default:
    return fail("an aggregate rvalue");
  }
}

llvm::Value *Lowering::operand(const G::Operand &O) {
  if (O.K == G::Operand::Const)
    return constant(O.C);
  if (O.P.Ty->isUnit()) // no storage: the empty value
    return llvm::ConstantStruct::get(llvm::StructType::get(C), {});
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
    // A float conversion rounds, so nothing is fused across it (Go spec,
    // "Floating-point operators"): where operations may be contracted, its
    // operand is fenced, also when the types are equal.
    if (ContractFP)
      V = B.CreateIntrinsic(llvm::Intrinsic::arithmetic_fence, {V->getType()},
                            {V});
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
    if (llvm::Value *F = contracted(R, X, Y))
      return F;
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
  case G::Rvalue::Aggregate:
    return fail("an aggregate that is not built in a place");
  case G::Rvalue::Len: {
    if (R.P.Ty->getKind() == G::Type::Kind::Array)
      return B.getInt64(R.P.Ty->length());
    return B.CreateLoad(B.getInt64Ty(),
                        B.CreateStructGEP(memTy(R.P.Ty), placeAddr(R.P), 1));
  }
  }
  return fail("an rvalue");
}

std::string Lowering::panicName(const G::Terminator &T) {
  std::string N = "panic_";
  N += G::getAssertKindName(T.AK);
  // An unsigned index is reported as the unsigned number it is.
  if (G::takesIndexAndLength(T.AK) && !T.Args.empty() &&
      !T.Args[0].type()->isSigned())
    N += "_u";
  return G::runtimeSymbol(N);
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
    if (R->isUnit() || isAggregate(R)) {
      B.CreateRetVoid(); // an aggregate was built in the caller's buffer
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
    auto CFIt =
        T.Discr.K == G::Operand::Const && T.Discr.C.K == G::Constant::Func
            ? CFuncs.find(T.Discr.C.Bits)
            : CFuncs.end();
    if (CFIt != CFuncs.end()) {
      // A C function: the C ABI decides how each argument and the result
      // travel (CABILower.h).
      const CFunction &CF = CFIt->second;
      llvm::SmallVector<cabi::Value, 4> Vals;
      unsigned AggIdx = 0;
      for (const G::Operand &A : T.Args) {
        cabi::Value V;
        V.Ty = A.type();
        if (!isAggregate(A.type())) {
          V.V = operand(A);
          Vals.push_back(V);
          continue;
        }
        // The callee owns its argument: a copy of its own (C may write to an
        // argument passed by pointer).
        llvm::Type *MT = memTy(A.type());
        llvm::AllocaInst *&Tmp = ArgSlots[{MT, AggIdx++}];
        if (!Tmp)
          Tmp = entryAlloca(MT, "arg");
        emitInto(Tmp, A);
        V.V = Tmp;
        V.Known = Tmp->getAlign();
        Vals.push_back(V);
      }
      cabi::Value Dest;
      Dest.Ty = Sig->ret();
      if (isAggregate(Sig->ret())) {
        Dest.V = placeAddr(T.Dest);
        Dest.Known = LM.getDataLayout().getABITypeAlign(memTy(Sig->ret()));
      }
      CScratchUsed.clear();
      auto MemTyFn = [this](const G::Type *T) { return memTy(T); };
      auto AllocaFn = [this](llvm::Type *Ty, llvm::Align A,
                             const llvm::Twine &Name) {
        return cScratchSlot(Ty, A, Name);
      };
      cabi::Env Env{B, LM.getDataLayout(), MemTyFn, AllocaFn};
      cabi::PreparedCall PC =
          cabi::lowerCArgs(Env, CF.Info, CF.Sig, Vals, Dest);
      cabiWork(PC.Args.size() + 1);
      llvm::CallInst *CI = B.CreateCall(CF.Sig.Type, Callee, PC.Args);
      // The call repeats the C ABI's attributes of the declaration
      // (extension, byval, sret, alignment), as the internal path does.
      CI->setAttributes(
          Funcs[T.Discr.C.Bits]->getAttributes().removeFnAttributes(C));
      if (T.Unwind.K == G::UnwindAction::Unreachable)
        CI->addFnAttr(llvm::Attribute::NoUnwind);
      llvm::Value *R = cabi::lowerCResult(Env, CF.Info, CF.Sig, PC, CI, Dest);
      if (R && !Sig->ret()->isUnit())
        storePlace(T.Dest, R);
      if (T.Target == G::NoId)
        B.CreateUnreachable();
      else
        B.CreateBr(Blocks[T.Target]);
      return llvm::Error::success();
    }
    llvm::SmallVector<llvm::Value *, 4> Args;
    bool Sret = isAggregate(Sig->ret());
    if (Sret)
      Args.push_back(placeAddr(T.Dest));
    unsigned AggIdx = 0;
    for (const G::Operand &A : T.Args) {
      if (!isAggregate(A.type())) {
        Args.push_back(operand(A));
        continue;
      }
      // The callee owns its parameter: it gets a copy (a constant is stored
      // in a scratch slot of this frame; the slot is reused by the next call
      // that passes this type in this position, ArgSlots).
      llvm::Type *MT = memTy(A.type());
      llvm::AllocaInst *&Tmp = ArgSlots[{MT, AggIdx++}];
      if (!Tmp)
        Tmp = entryAlloca(MT, "arg");
      emitInto(Tmp, A);
      Args.push_back(Tmp);
    }
    llvm::CallInst *CI = B.CreateCall(fnType(Sig), Callee, Args);
    // The ABI's extension of narrow arguments and results is repeated at the
    // call (the callee's own attributes stay on the callee).
    if (T.Discr.K == G::Operand::Const && T.Discr.C.K == G::Constant::Func)
      CI->setAttributes(
          Funcs[T.Discr.C.Bits]->getAttributes().removeFnAttributes(C));
    if (T.Unwind.K == G::UnwindAction::Unreachable)
      CI->addFnAttr(llvm::Attribute::NoUnwind);
    if (!Sig->ret()->isUnit() && !Sret)
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
    llvm::Value *Cond = operand(T.Discr);
    llvm::SmallVector<llvm::Value *, 2> Vals;
    for (const G::Operand &A : T.Args)
      Vals.push_back(operand(A));
    // The runtime's function for this kind, if the module has one (a module
    // built by hand may not: the failing edge is then a trap).
    G::FuncId Rt = GM.lookupFunction(panicName(T));
    if (Rt == G::NoId) {
      llvm::BasicBlock *Fail = llvm::BasicBlock::Create(C, "assert.fail", LF);
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
    PanicSite &PS = Panics[Rt];
    if (!PS.BB) {
      PS.BB = llvm::BasicBlock::Create(C, "panic", LF);
      PS.Pos = entryAlloca(llvm::PointerType::getUnqual(C), "panic.pos");
      for (llvm::Value *V : Vals)
        PS.Args.push_back(entryAlloca(V->getType(), "panic.arg"));
      llvm::IRBuilder<>::InsertPointGuard Guard(B);
      B.SetInsertPoint(PS.BB);
      // Shared by every check of the kind: no line of its own (the position
      // is the operand).
      setLoc(G::Span());
      llvm::SmallVector<llvm::Value *, 3> CallArgs;
      CallArgs.push_back(B.CreateLoad(llvm::PointerType::getUnqual(C), PS.Pos));
      for (llvm::AllocaInst *A : PS.Args)
        CallArgs.push_back(B.CreateLoad(A->getAllocatedType(), A));
      B.CreateCall(Funcs[Rt], CallArgs);
      B.CreateUnreachable();
    }
    // The failing edge: store the constant position and the values, then
    // branch to the shared block.
    llvm::BasicBlock *Fail = llvm::BasicBlock::Create(C, "assert.fail", LF);
    if (T.Expected)
      B.CreateCondBr(Cond, Blocks[T.Target], Fail);
    else
      B.CreateCondBr(Cond, Fail, Blocks[T.Target]);
    B.SetInsertPoint(Fail);
    B.CreateStore(positionConst(T.Loc), PS.Pos);
    for (size_t I = 0; I < Vals.size(); ++I)
      B.CreateStore(Vals[I], PS.Args[I]);
    B.CreateBr(PS.BB);
    return llvm::Error::success();
  }
  case G::Terminator::Drop:
    return fail("a drop (P04.09)");
  case G::Terminator::Resume:
    return fail("a resume (P04.07)");
  }
  return fail("a terminator");
}

void Lowering::countUses(const G::Function &F) {
  Uses.assign(F.Locals.size(), 0);
  Defs.assign(F.Locals.size(), 0);
  auto Def = [&](const G::Place &P) {
    if (P.Local != G::NoId && P.Local < Defs.size())
      ++Defs[P.Local];
  };
  auto Place = [&](const G::Place &P) {
    if (P.Local != G::NoId && P.Local < Uses.size())
      ++Uses[P.Local];
    for (const G::PlaceElem &E : P.Proj)
      if (E.K == G::PlaceElem::Index && E.Val < Uses.size())
        ++Uses[E.Val];
  };
  auto Op = [&](const G::Operand &O) {
    if (O.K != G::Operand::Const)
      Place(O.P);
  };
  for (const G::BasicBlock &BB : F.Blocks) {
    for (const G::Statement &S : BB.Stmts) {
      if (S.K != G::Statement::Assign)
        continue;
      Def(S.Dest);
      // A write through a projection reads the base (a pointer, an index).
      if (!S.Dest.Proj.empty())
        Place(S.Dest);
      Op(S.RV.A);
      Op(S.RV.B);
      for (const G::Operand &O : S.RV.Ops)
        Op(O);
      if (S.RV.K == G::Rvalue::AddrOf || S.RV.K == G::Rvalue::Len)
        Place(S.RV.P);
    }
    if (BB.Term.K == G::Terminator::Call)
      Def(BB.Term.Dest);
    Op(BB.Term.Discr);
    for (const G::Operand &O : BB.Term.Args)
      Op(O);
    if (BB.Term.K == G::Terminator::Drop)
      Place(BB.Term.Dest);
  }
  // The return place is read by Return.
  if (!Uses.empty())
    ++Uses[0];
}

void Lowering::computeDominators(const G::Function &F) {
  size_t N = F.Blocks.size();
  const uint32_t None = ~0u;
  // Reverse post-order of the blocks reachable from the entry (an explicit
  // stack), then the iterative algorithm of Cooper, Harvey and Kennedy.
  std::vector<std::vector<G::BlockId>> Succ(N);
  for (size_t I = 0; I < N; ++I)
    G::forEachSuccessor(F.Blocks[I].Term, [&](const G::BlockId &T) {
      if (T < N)
        Succ[I].push_back(T);
    });
  std::vector<G::BlockId> Post;
  std::vector<uint32_t> PostNum(N, None);
  std::vector<std::pair<G::BlockId, size_t>> Stack;
  std::vector<uint8_t> Seen(N, 0);
  Seen[0] = 1;
  Stack.push_back({0, 0});
  while (!Stack.empty()) {
    auto &[B, K] = Stack.back();
    if (K < Succ[B].size()) {
      G::BlockId T = Succ[B][K++];
      if (!Seen[T]) {
        Seen[T] = 1;
        Stack.push_back({T, 0});
      }
      continue;
    }
    PostNum[B] = Post.size();
    Post.push_back(B);
    Stack.pop_back();
  }
  std::vector<std::vector<G::BlockId>> Pred(N);
  for (G::BlockId B : Post)
    for (G::BlockId T : Succ[B])
      Pred[T].push_back(B);
  std::vector<uint32_t> IDom(N, None);
  IDom[0] = 0;
  auto Intersect = [&](G::BlockId A, G::BlockId B) {
    while (A != B) {
      while (PostNum[A] < PostNum[B])
        A = IDom[A];
      while (PostNum[B] < PostNum[A])
        B = IDom[B];
    }
    return A;
  };
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (size_t I = Post.size(); I-- > 0;) {
      G::BlockId B = Post[I];
      if (B == 0)
        continue;
      uint32_t New = None;
      for (G::BlockId P : Pred[B])
        if (IDom[P] != None)
          New = New == None ? P : Intersect(P, New);
      if (New != IDom[B]) {
        IDom[B] = New;
        Changed = true;
      }
    }
  }
  // Number the dominator tree.
  std::vector<std::vector<G::BlockId>> Kids(N);
  for (G::BlockId B : Post)
    if (B != 0 && IDom[B] != None)
      Kids[IDom[B]].push_back(B);
  DomIn.assign(N, None);
  DomOut.assign(N, None);
  uint32_t Clock = 0;
  std::vector<std::pair<G::BlockId, size_t>> Walk{{0, 0}};
  DomIn[0] = Clock++;
  while (!Walk.empty()) {
    auto &[B, K] = Walk.back();
    if (K < Kids[B].size()) {
      G::BlockId C = Kids[B][K++];
      DomIn[C] = Clock++;
      Walk.push_back({C, 0});
      continue;
    }
    DomOut[B] = Clock++;
    Walk.pop_back();
  }
  DomReady = true;
}

bool Lowering::dominates(G::BlockId A, G::BlockId B) {
  if (!DomReady)
    computeDominators(*GF);
  return DomIn[A] != ~0u && DomIn[B] != ~0u && DomIn[A] <= DomIn[B] &&
         DomOut[B] <= DomOut[A];
}

const Lowering::ProductInfo *Lowering::productOf(const G::Operand &O) {
  if (O.K == G::Operand::Const || !O.P.Proj.empty())
    return nullptr;
  auto It = Products.find(O.P.Local);
  if (It == Products.end())
    return nullptr;
  // The same block: nothing wrote the temporary since (the entry is
  // dropped by a write). Another block: the only write is the product's,
  // and its block dominates this one.
  const ProductInfo &P = It->second;
  if (P.Block != CurBlock &&
      (Defs[O.P.Local] != 1 || !dominates(P.Block, CurBlock)))
    return nullptr;
  return &P;
}

llvm::Value *Lowering::contracted(const G::Rvalue &R, llvm::Value *X,
                                  llvm::Value *Y) {
  if (!ContractFP || !R.A.type()->isFloat() ||
      (R.Bin != G::BinOp::Add && R.Bin != G::BinOp::Sub))
    return nullptr;
  auto Product = [&](const G::Operand &O) { return productOf(O); };
  llvm::Type *Ty = X->getType();
  auto Fma = [&](llvm::Value *P, llvm::Value *Q, llvm::Value *Z) {
    return B.CreateIntrinsic(llvm::Intrinsic::fma, {Ty}, {P, Q, Z});
  };
  // gc's rules (ARM64.rules) try the right operand first: x + a*b is FMADD,
  // x - a*b is FMSUB (x + -a*b); then the left one: a*b + y is FMADD again,
  // a*b - y is FNMSUB (a*b + -y). So a*b - c*d rounds a*b and fuses c*d.
  if (const auto *P = Product(R.B))
    return R.Bin == G::BinOp::Add ? Fma(P->X, P->Y, X)
                                  : Fma(B.CreateFNeg(P->X), P->Y, X);
  if (const auto *P = Product(R.A))
    return Fma(P->X, P->Y, R.Bin == G::BinOp::Add ? Y : B.CreateFNeg(Y));
  return nullptr;
}

llvm::Error Lowering::function(const G::Function &F, llvm::Function *Fn) {
  GF = &F;
  LF = Fn;
  Slots.clear();
  Blocks.clear();
  Products.clear();
  ArgSlots.clear();
  CScratch.clear();
  DomReady = false;
  Panics.clear();
  countUses(F);
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
  bool Sret = isAggregate(F.Sig->ret());
  for (size_t I = 0; I < F.Locals.size(); ++I) {
    const G::Local &L = F.Locals[I];
    bool Param = I >= 1 && I <= F.numParams();
    if (L.Ty->isUnit())
      Slots.push_back(nullptr);
    else if ((I == 0 && Sret) || (Param && isAggregate(L.Ty)))
      Slots.push_back(Fn->getArg(I == 0 ? 0 : (Sret ? 1 : 0) + I - 1));
    else
      Slots.push_back(B.CreateAlloca(memTy(L.Ty), nullptr, L.Name));
  }
  for (unsigned I = 0; I < F.numParams(); ++I) {
    G::Type *PT = F.Locals[1 + I].Ty;
    if (isAggregate(PT) || PT->isUnit())
      continue; // the caller's copy is the storage
    llvm::Value *A = Fn->getArg((Sret ? 1 : 0) + I);
    storePlace(G::localPlace(1 + I, PT), A);
  }
  B.CreateBr(Blocks[0]);
  for (size_t Bi = 0; Bi < F.Blocks.size(); ++Bi) {
    const G::BasicBlock &BB = F.Blocks[Bi];
    B.SetInsertPoint(Blocks[Bi]);
    CurBlock = Bi;
    if (BB.Catch)
      return fail("a catch block, the handler of `defer` (P04.07)");
    for (const G::Statement &S : BB.Stmts) {
      setLoc(S.Loc);
      switch (S.K) {
      case G::Statement::Assign: {
        Products.erase(S.Dest.Local);
        if (S.Dest.Ty->isUnit())
          break; // nothing to store; the operands have no effects
        if (isAggregate(S.Dest.Ty)) {
          if (llvm::Error E = assignAggregate(S.Dest, S.RV))
            return E;
          break;
        }
        llvm::Expected<llvm::Value *> V = rvalue(S.RV);
        if (!V)
          return V.takeError();
        storePlace(S.Dest, *V);
        if (ContractFP && S.RV.K == G::Rvalue::Binary &&
            S.RV.Bin == G::BinOp::Mul && S.RV.Ty->isFloat() &&
            S.Dest.Proj.empty() && F.Locals[S.Dest.Local].K == G::Local::Temp &&
            Uses[S.Dest.Local] == 1)
          if (auto *Mul = llvm::dyn_cast<llvm::BinaryOperator>(*V))
            Products[S.Dest.Local] = {Mul->getOperand(0), Mul->getOperand(1),
                                      static_cast<G::BlockId>(Bi)};
        // gc: FNEGD (FMULD x y) => FNMULD x y, and FADD/FSUB fuse an FNMUL
        // like an FMUL (ARM64.rules): the negation of a product is a product
        // of the negated first factor and the second (exactly -(x*y)).
        if (ContractFP && S.RV.K == G::Rvalue::Unary &&
            S.RV.Un == G::UnOp::Neg && S.RV.Ty->isFloat() &&
            S.Dest.Proj.empty() && F.Locals[S.Dest.Local].K == G::Local::Temp &&
            Uses[S.Dest.Local] == 1)
          if (const ProductInfo *P = productOf(S.RV.A)) {
            ProductInfo Negated{B.CreateFNeg(P->X), P->Y,
                                static_cast<G::BlockId>(Bi)};
            Products[S.Dest.Local] = Negated;
          }
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
  // gc contracts on arm64 and not on amd64 (D4.12): every float operation
  // on arm64 carries `contract`.
  ContractFP = LM.getTargetTriple().isAArch64();
  if (ContractFP) {
    llvm::FastMathFlags FMF;
    FMF.setAllowContract(true);
    B.setFastMathFlags(FMF);
  }
  prepareTypes();
  if (NoStackAt)
    return nestingError();
  for (const G::Global &Gl : GM.Globals) {
    llvm::Constant *Init = Gl.Init.K == G::Constant::Zero
                               ? llvm::Constant::getNullValue(memTy(Gl.Ty))
                               : memConstant(Gl.Init);
    auto *GV = new llvm::GlobalVariable(
        LM, memTy(Gl.Ty), Gl.ReadOnly,
        Gl.Link == G::Linkage::Internal ? llvm::GlobalValue::InternalLinkage
        : Gl.Link == G::Linkage::LinkOnceODR
            ? llvm::GlobalValue::LinkOnceODRLinkage
            : llvm::GlobalValue::ExternalLinkage,
        Init, Gl.Name);
    if (Gl.ReadOnly)
      GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    Globals.push_back(GV);
  }
  if (llvm::Error E = classifyCFunctions())
    return E;
  for (size_t I = 0; I < GM.Funcs.size(); ++I)
    Funcs.push_back(declare(I, *GM.Funcs[I]));
  for (size_t I = 0; I < GM.Funcs.size(); ++I)
    if (!GM.Funcs[I]->isDeclaration())
      if (llvm::Error E = function(*GM.Funcs[I], Funcs[I]))
        return E;
  if (NoStackAt)
    return nestingError();
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
  LowerToLLVMOptions O = Opts;
  O.TargetCPU = TM.getTargetCPU().str();
  O.TargetFeatures = TM.getTargetFeatureString().str();
  Lowering L(Ctx, M, *LM, O);
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
