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
#include "gopp/Basic/WorkCount.h"
#include "gopp/CodeGen/CABILower.h"
#include "gopp/CodeGen/CodeGen.h"
#include "gopp/CodeGen/DebugInfo.h"
#include "gopp/CodeGen/GoABI.h"
#include "gopp/GIR/GIR.h"
#include "gopp/GIR/Verify.h"
#include "gopp/Import/CABI.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/bit.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Path.h"
#include "llvm/Target/TargetMachine.h"

#include <algorithm>
#include <cassert>
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
      : C(C), GM(GM), LM(LM), B(C), Opts(Opts),
        DI(
            LM, GM, [this](const G::Type *T) { return memTy(T); },
            Opts.DwarfLinkageNames, Opts.Optimized) {}

  llvm::Error run();

private:
  llvm::LLVMContext &C;
  const G::Module &GM;
  llvm::Module &LM;
  llvm::IRBuilder<> B;
  const LowerToLLVMOptions &Opts;
  /// All debug metadata is made by this (DebugInfo.h); unused without
  /// Opts.DebugInfo.
  DebugInfoBuilder DI;
  llvm::DenseMap<const G::Type *, llvm::Type *> MemTypes;
  /// A declared C function: how its signature crosses the C ABI (P04.11,
  /// CABILower.h). Keyed by the function's index in the module.
  struct CFunction {
    cabi::CallInfo Info;
    cabi::IRSignature Sig;
  };
  llvm::DenseMap<G::FuncId, CFunction> CFuncs;
  /// The C ABI of a call through a C function pointer, by its Func type
  /// (P04.18: the type says the convention, V075).
  std::map<const G::Type *, CFunction> CPointers;
  /// The convention of Go++ calling Go++ (P04.18, GoABI.h): the one place
  /// that decides how a parameter and a result travel.
  std::unique_ptr<goabi::GoAbi> Abi;
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
  /// Where a convention's temporaries go: the entry block (a prologue
  /// has no terminator yet, so it allocates at the insert point), or the
  /// slots shared by the calls of the function (cScratchSlot).
  enum AllocaWhere { AllocaPrologue, AllocaEntry, AllocaScratch };
  cabi::Env makeEnv(AllocaWhere W) {
    auto MemTyFn = [this](const G::Type *T) { return memTy(T); };
    auto AllocaFn = [this, W](llvm::Type *Ty, llvm::Align A,
                              const llvm::Twine &Name) -> llvm::AllocaInst * {
      if (W == AllocaScratch)
        return cScratchSlot(Ty, A, Name);
      llvm::AllocaInst *Slot = W == AllocaEntry
                                   ? entryAlloca(Ty, Name)
                                   : B.CreateAlloca(Ty, nullptr, Name);
      Slot->setAlignment(std::max(A, LM.getDataLayout().getABITypeAlign(Ty)));
      return Slot;
    };
    return cabi::Env{B, LM.getDataLayout(), MemTyFn, AllocaFn};
  }
  /// The function being made, and (when it has the C convention: a thunk or
  /// the entry point) its IR arguments.
  G::FuncId GFId = G::NoId;
  llvm::SmallVector<llvm::Argument *, 8> CArgs;
  llvm::Error classifyCFunctions();
  llvm::Error checkCLayout(const G::Type *T);
  llvm::AllocaInst *cScratchSlot(llvm::Type *Ty, llvm::Align A,
                                 const llvm::Twine &Name);
  llvm::SmallVector<llvm::Function *, 16> Funcs;
  llvm::SmallVector<llvm::GlobalVariable *, 16> Globals;
  llvm::SmallVector<llvm::GlobalVariable *, 16> Strings;

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
  llvm::Constant *staticData(const G::Global &Gl);

  /// FMA contraction (D4.12): gc fuses a float x*y with the + or - that
  /// reads it on arm64, never on amd64. The code generator is the only place
  /// that decides: it writes `llvm.fma` where gc fuses, and no float
  /// instruction carries `contract` or any other fast-math flag, so no LLVM
  /// pass and no selector can fuse anything else, and the result of a
  /// program is the same at every `opt` level and with every pipeline
  /// (docs/design/pipelines.md section 4). A float conversion needs no fence
  /// for the same reason: a product that a conversion rounds is not a product
  /// to the matcher below.
  ///
  /// What is matched follows gc's SSA rewrite `FADDD a (FMULD x y)`: a float
  /// *product* held in a local that a + or - reads. A product is a float `*`,
  /// the negation of a product (gc: FNEGD (FMULD x y) => FNMULD x y, exactly
  /// -(x*y)) or a copy of one (`t := x*y` is `tmp = x*y; t = copy tmp` in
  /// GIR, the same SSA value in gc), written to a whole local that is a
  /// temporary read once (the value of a sub-expression), or a variable,
  /// parameter or result whose address is never taken (gc keeps it in SSA
  /// values); a variable may be read any number of times, and each + or -
  /// fuses (gc keeps the product for its other readers). The copies that GIR
  /// inlining makes for a parameter or a result are not products
  /// (Statement::Boundary), and a call's result never is, so inlining (GIR
  /// inlining in the `opt` pipelines, LLVM's above `opt 0`) cannot create a
  /// fusion. Both operands products: the right one first, as gc's rules try
  /// them.
  ///
  /// Which write a read sees, as gc's SSA would: the blocks of a function
  /// with a float product are lowered in the preorder of the dominator tree,
  /// and Products is scoped by that tree (ProductLog undoes a block's
  /// changes when the walk leaves its subtree). So at a read, Products holds
  /// the last product write of a local in the reading block or in a block
  /// that dominates it, and nothing that a write in between replaced or
  /// ended. Another write in a block on a path from the product's block to
  /// the reader, which dominates neither, is excluded by unwrittenBetween
  /// (a phi in gc). The factors are values of the product's block, which
  /// dominates the read, so they are valid there. A call, a bounds check or
  /// a division check ends a block, so `x*y + f()` and `a[i]*b[i] + c[i]`
  /// fuse across blocks.
  ///
  /// Before its ARM64 rules, gc rewrites with its generic rules (generic.rules
  /// "floating point optimizations") on what its SSA knows to be constant, and
  /// some of them change whether there is a product: `x*1` is x, `x*-1` is
  /// `-x`, `x*2` is `x+x` (no product, but a sum that may fuse a product x
  /// holds), `x/c` with an exact reciprocal (a power of two) is `x*(1/c)` (a
  /// product), and an operation on constants is folded. mulShape decides
  /// these the same way. The constants are the GIR constants and the values
  /// of locals (Products holds them too, scoped the same way): a float local
  /// whose one reaching write is a constant, a copy of one, a negation, sum,
  /// difference, product or quotient of constants that is not NaN, or a
  /// conversion of one between float types or from a signed integer local
  /// written with a constant. Not followed (gc's phielim and sccp do): a
  /// local written with the same constant on several paths (a phi of equal
  /// constants), integer copies and arithmetic, and unsigned conversions.
  bool FuseFMA = false;
  /// Lowering in dominator order with Products scoped (FuseFMA and the
  /// function has a float * or /).
  bool Scoped = false;
  struct ProductInfo {
    llvm::Value *X, *Y;
    G::BlockId Block; // where the local was written
    /// Not a product but a known constant: a float (KF) or a signed integer.
    enum : uint8_t { Product, FloatConst, IntConst } Kind = Product;
    double KF = 0;
    int64_t KI = 0;
  };
  llvm::DenseMap<G::LocalId, ProductInfo> Products;
  /// The old value of every change of Products, and per open block of the
  /// dominator-tree walk where its changes start in the log.
  std::vector<std::pair<G::LocalId, std::optional<ProductInfo>>> ProductLog;
  std::vector<std::pair<G::BlockId, size_t>> ProductScopes;
  /// Sets (or with nullopt ends) the product of L, in the open block.
  void setProduct(G::LocalId L, std::optional<ProductInfo> P);
  /// Closes the blocks of the walk that do not dominate B, then opens B.
  void enterProductScope(G::BlockId B);
  /// Uses counts the reads of each local, AddrTaken marks the locals whose
  /// address is taken, DefSites lists every write (local, block).
  llvm::SmallVector<unsigned, 32> Uses;
  llvm::SmallVector<bool, 32> AddrTaken;
  std::vector<std::pair<G::LocalId, G::BlockId>> DefSites;
  /// The function has a float multiplication or division: without one
  /// nothing fuses, and the blocks are lowered in index order.
  bool HasFloatMul = false;
  /// Can a product written to local L be tracked (see FuseFMA)?
  bool trackable(G::LocalId L) const;
  G::BlockId CurBlock = 0;
  /// The dominator tree of the blocks of the function: DomIn/DomOut number
  /// its depth-first walk, so "A dominates B" is an interval test; an
  /// unreachable block has none. LowerOrder is the walk's preorder, then the
  /// unreachable blocks.
  std::vector<uint32_t> DomIn, DomOut;
  std::vector<G::BlockId> LowerOrder;
  bool DomReady = false;
  void computeDominators(const G::Function &F);
  bool dominates(G::BlockId A, G::BlockId B);
  /// The strongly connected components of the blocks reachable from the
  /// entry, numbered so that every edge goes to an equal or greater number
  /// (Topo; ~0u for a block no path from the entry reaches). InCycle: the
  /// block's component has several blocks, or it jumps to itself.
  std::vector<uint32_t> Topo;
  std::vector<uint8_t> InCycle;
  /// The successors of every block (in range ones only).
  std::vector<std::vector<G::BlockId>> SuccOf;
  bool TopoReady = false;
  void computeTopo(const G::Function &F);
  /// Per local: the Topo of every block that writes it, sorted (a block no
  /// path reaches is left out); WritesInBlock: the writes of (local, block).
  std::vector<std::vector<uint32_t>> WriteTopos;
  llvm::DenseMap<uint64_t, uint32_t> WritesInBlock;
  bool DefIndexReady = false;
  void buildDefIndex();
  /// Can the product that the last write of L in block Bd made be read in
  /// block R, which Bd strictly dominates? Only if no other write of L can
  /// run between them: such a write is in a block W with a path
  /// Bd -> W -> R, so with Topo[Bd] <= Topo[W] <= Topo[R]. The test counts
  /// the writes in that range of the sorted numbers (two binary searches),
  /// minus those of Bd itself (they come before the product's write, which
  /// is Bd's last) and of R when R is on no cycle (they come after the read;
  /// one before it would have ended the entry).
  ///
  /// The count is exact when it finds nothing. Otherwise the writes may lie
  /// on paths that never lead from Bd to R (a path back through Bd writes
  /// the product again). Then a walk decides exactly: a write counts only if
  /// Bd reaches it, and it reaches R, without a path through Bd. The walk
  /// enters only blocks with Topo <= Topo[R] (no other block lies on a path
  /// to R), so it covers the blocks between Bd and R, not the rest of the
  /// function: N sibling loops cost N walks of one loop each. One walk serves
  /// every read of (L, Bd) up to its bound (Tainted; a read past the bound
  /// walks again with the larger one). All the walks of a function share a
  /// budget linear in its size (WalkBudget); a read after it is spent counts
  /// as written (conservative, never wrong). Only a function with very many
  /// variables that each need a walk over one large loop spends it.
  bool unwrittenBetween(G::LocalId L, G::BlockId Bd, G::BlockId R);
  /// The blocks up to Topo Bound that a write of L reaches without passing
  /// Bd, after Bd (Spent: the budget ran out, nothing is known).
  struct TaintInfo {
    uint32_t Bound = 0;
    bool Spent = false;
    llvm::DenseSet<G::BlockId> Blocks;
  };
  const TaintInfo &taintedBy(G::LocalId L, G::BlockId Bd, uint32_t Bound);
  llvm::DenseMap<uint64_t, TaintInfo> Tainted;
  /// The marks of the two walks (a block is seen when its mark is the
  /// walk's epoch), sized once per function, so a walk costs what it visits.
  std::vector<uint32_t> SeenReached, SeenAfter;
  uint32_t WalkEpoch = 0;
  uint64_t WalkBudget = 0;
  /// The counter of LowerToLLVMOptions::Counts for one kind of work, or a
  /// scratch one when the caller counts nothing.
  uint64_t &workCount(uint64_t LowerWorkCounts::*Kind) {
    return Opts.Counts ? Opts.Counts->*Kind : NoCount;
  }
  uint64_t NoCount = 0;
  void countUses(const G::Function &F);
  llvm::Value *contracted(const G::Rvalue &R, llvm::Value *X, llvm::Value *Y);
  /// The factors of the product that operand O holds, if it is a local with
  /// a product that is still valid here (see Products).
  const ProductInfo *productOf(const G::Operand &O);
  /// What a local operand holds here (a product or a constant), if known;
  /// with Want, only a fact of that kind (the walk that proves it valid runs
  /// only then, so a constant never costs a product's walk budget).
  const ProductInfo *factOf(const G::Operand &O, int Want = -1);
  /// The value of O if it is a constant here (a GIR constant or a local).
  std::optional<double> floatConstOf(const G::Operand &O);
  std::optional<int64_t> intConstOf(const G::Operand &O);
  /// What gc's generic rules make of a float * or / (see FuseFMA).
  struct MulShape {
    enum Kind : uint8_t {
      None,    // no product (a division without an exact reciprocal)
      Folded,  // a constant, Value
      Product, // X times Y, or with FromDiv X times the constant Factor
      Same,    // *1: the value of X
      Negated, // *-1: -X
      Doubled, // *2: X+X
    } K = None;
    const G::Operand *X = nullptr;
    double Value = 0, Factor = 0;
    bool FromDiv = false;
  };
  MulShape mulShape(const G::Rvalue &R);
  /// What the destination of S holds after it (V: the value stored).
  std::optional<ProductInfo> factOfAssign(const G::Statement &S, llvm::Value *V,
                                          G::BlockId Bi);

  /// Aggregates (strings, slices, arrays, tuples, structs, Options) live in
  /// memory: they are copied with memmove, zeroed with memset and built in
  /// place. LLVM never sees a first-class value of one (a [100000]int64
  /// would not compile); the calling convention (GoABI.h) passes the small
  /// ones as their scalars and the others by address.
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
  llvm::Function *declare(G::FuncId Id, const G::Function &F);
  llvm::Constant *stringConst(uint64_t Id);
  void setLoc(const G::Span &S, G::ScopeId Scope);
  void keepInMemory(llvm::Value *Slot);

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
  llvm::Error function(G::FuncId Id, const G::Function &F, llvm::Function *LF);
  llvm::Error terminator(const G::Terminator &T);
  /// Makes a call: the only place that does (P04.18). P04.07 makes it an
  /// invoke when U unwinds into this function.
  llvm::CallBase *emitCall(llvm::FunctionType *FT, llvm::Value *Callee,
                           llvm::ArrayRef<llvm::Value *> Args,
                           llvm::CallingConv::ID CC, llvm::AttributeList AL,
                           const G::UnwindAction &U);
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
      R = llvm::StructType::get(C, {Ptr, I64, I64});
      break;
    case G::Type::Kind::Slice:
      R = llvm::StructType::get(C, {Ptr, I64, I64, Ptr});
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

/// Classifies every declared C function (P04.11). A signature of scalars only
/// has the classification Clang would give without asking Clang
/// (cabi::classifyScalars); Clang is asked from the first signature with an
/// aggregate on (its world is made once per process, M3), so a module that
/// passes no aggregate to C never starts it.
llvm::Error Lowering::classifyCFunctions() {
  const std::string Triple = LM.getTargetTriple().str();
  for (size_t I = 0; I < GM.Funcs.size(); ++I) {
    const G::Function &F = *GM.Funcs[I];
    if (F.CC != G::CallConv::C)
      continue; // (a C function with a body is a thunk or the entry point)
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

/// The C functions whose built-in meaning LLVM may not use: the allocation
/// functions. LLVM's optimizer knows them (TargetLibraryInfo, MemoryBuiltins)
/// and above opt 0 deletes an allocation whose memory does not escape,
/// together with the runtime's out-of-memory check, so an opt level would
/// change whether a program fails (docs/design/pipelines.md section 1).
/// `nobuiltin` on the declaration (clang's -fno-builtin-malloc) keeps every
/// call, also one an inlined `gopp.mem_resize` brings into its caller. The
/// other C functions the runtime calls keep their built-in meaning: LLVM may
/// fold memmove, memset and memcmp (the same bytes result), and it never
/// deletes a write or an exit.
static bool isCAllocationFunction(llvm::StringRef Name) {
  return llvm::StringSwitch<bool>(Name)
      .Cases(
          {"malloc", "calloc", "realloc", "reallocf", "reallocarray", "free"},
          true)
      .Cases({"aligned_alloc", "memalign", "valloc", "pvalloc",
              "posix_memalign", "strdup", "strndup"},
             true)
      .Default(false);
}

llvm::Function *Lowering::declare(G::FuncId Id, const G::Function &F) {
  auto CFIt = CFuncs.find(Id);
  const goabi::FnInfo *GI =
      CFIt == CFuncs.end() ? &Abi->classify(F.Sig) : nullptr;
  llvm::Function *LF = llvm::Function::Create(
      CFIt != CFuncs.end() ? CFIt->second.Sig.Type : GI->Type,
      F.Link == G::Linkage::Internal ? llvm::GlobalValue::InternalLinkage
      : F.Link == G::Linkage::LinkOnceODR
          ? llvm::GlobalValue::LinkOnceODRLinkage
          : llvm::GlobalValue::ExternalLinkage,
      F.Name, &LM);
  llvm::AttributeList AL = LF->getAttributes();
  if (CFIt != CFuncs.end()) {
    AL = CFIt->second.Sig.Attrs; // the C ABI's: extension, byval, sret, ...
  } else {
    AL = GI->Attrs; // the extension of narrow scalars (GoABI.h)
    LF->setCallingConv(GI->CC);
  }
  if (F.CC == G::CallConv::C && F.isDeclaration() &&
      isCAllocationFunction(F.Name))
    AL = AL.addFnAttribute(C, llvm::Attribute::NoBuiltin);
  if (F.NoUnwind)
    AL = AL.addFnAttribute(C, llvm::Attribute::NoUnwind);
  if (F.NoReturn)
    AL = AL.addFnAttribute(C, llvm::Attribute::NoReturn);
  // Unwind tables for every function Go++ defines, so a stack trace works
  // through any frame (D4.6; clang's default on Linux).
  if (!F.isDeclaration())
    AL = AL.addFnAttribute(
        C, llvm::Attribute::getWithUWTableKind(C, llvm::UWTableKind::Async));
  // Unoptimized code with DWARF keeps the frame pointer, as Clang's `-O0`:
  // gdb's amd64 prologue analyser (which trusts the line table only for a
  // producer that starts with "clang ") then finds `push %rbp; mov %rsp,
  // %rbp` and `step` into a function goes on to the end of the declaration
  // line's row, past the stores into the parameters' slots. Without it gdb
  // stops at the first instruction, before them (debug-info.md section 3).
  if (!F.isDeclaration() && Opts.DebugInfo && !Opts.Optimized)
    AL = AL.addFnAttribute(C, "frame-pointer", "all");
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

void Lowering::setLoc(const G::Span &S, G::ScopeId Scope) {
  if (!Opts.DebugInfo) {
    B.SetCurrentDebugLocation(llvm::DebugLoc());
    return;
  }
  B.SetCurrentDebugLocation(DI.location(S, Scope));
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

/// The value of a global with static data (Global::Parts, V072): the leaves
/// of its type in order, built bottom up with a stack of its own.
llvm::Constant *Lowering::staticData(const G::Global &Gl) {
  struct Frame {
    const G::Type *T;
    llvm::SmallVector<const G::Type *, 4> Kids;
    llvm::SmallVector<llvm::Constant *, 4> Vals;
    bool Open = false;
  };
  llvm::SmallVector<Frame, 8> Stack;
  Stack.push_back({Gl.Ty, {}, {}, false});
  size_t Pos = 0;
  llvm::Constant *Done = nullptr;
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    llvm::Constant *Val = nullptr;
    if (!F.Open) {
      F.Open = true;
      const G::Type *T = F.T;
      const G::Constant &K = Gl.Parts[Pos < Gl.Parts.size() ? Pos : 0];
      bool Compound = (T->getKind() == G::Type::Kind::Struct && T->hasBody()) ||
                      T->getKind() == G::Type::Kind::Tuple ||
                      T->getKind() == G::Type::Kind::Array;
      if (T->isUnit()) {
        Val = llvm::Constant::getNullValue(memTy(T));
      } else if (K.K == G::Constant::Zero && K.Ty == T) {
        ++Pos;
        Val = llvm::Constant::getNullValue(memTy(T));
      } else if (Compound && !T->isRec()) {
        if (T->getKind() == G::Type::Kind::Array)
          F.Kids.assign(T->length(), T->elem());
        else
          F.Kids.assign(T->elems().begin(), T->elems().end());
      } else {
        Val = memConstant(Gl.Parts[Pos++]);
      }
    }
    if (!Val) {
      if (F.Vals.size() < F.Kids.size()) {
        const G::Type *Kid = F.Kids[F.Vals.size()];
        Stack.push_back({Kid, {}, {}, false});
        continue;
      }
      llvm::Type *MT = memTy(F.T);
      if (F.T->getKind() == G::Type::Kind::Array)
        Val = llvm::ConstantArray::get(cast<llvm::ArrayType>(MT), F.Vals);
      else
        Val = llvm::ConstantStruct::get(cast<llvm::StructType>(MT), F.Vals);
    }
    Stack.pop_back();
    if (Stack.empty())
      Done = Val;
    else
      Stack.back().Vals.push_back(Val);
  }
  return Done;
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
    // A float conversion rounds. Nothing fuses across it: the matcher
    // (FuseFMA) sees a conversion, not a product, and no instruction can be
    // contracted by LLVM.
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
    if (Scoped && T->isFloat() &&
        (R.Bin == G::BinOp::Mul || R.Bin == G::BinOp::Div)) {
      // gc: x*2 => x+x (also x/0.5), a sum that fuses a product x holds.
      MulShape Sh = mulShape(R);
      if (Sh.K == MulShape::Doubled) {
        G::Rvalue Sum = R;
        Sum.Bin = G::BinOp::Add;
        Sum.A = *Sh.X;
        Sum.B = *Sh.X;
        llvm::Value *XV = Sh.X == &R.A ? X : Y;
        if (llvm::Value *F = contracted(Sum, XV, XV))
          return F;
        return B.CreateFAdd(XV, XV);
      }
    }
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
  setLoc(T.Loc, T.Scope);
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
    cabi::Value Res;
    Res.Ty = R;
    if (!R->isUnit() && !isAggregate(R))
      Res.V = loadPlace(G::localPlace(0, R));
    else if (!R->isUnit()) {
      Res.V = Slots[0];
      Res.Known = LM.getDataLayout().getABITypeAlign(memTy(R));
    }
    // (A scratch copy of the result goes to the entry block, as every
    // alloca does.)
    cabi::Env Env = makeEnv(AllocaEntry);
    auto CFIt = CFuncs.find(GFId);
    if (CFIt != CFuncs.end()) // a thunk or the entry point: the C ABI
      cabi::packCReturn(Env, CFIt->second.Info, CFIt->second.Sig,
                        llvm::ArrayRef<llvm::Argument *>(CArgs), Res);
    else
      Abi->packReturn(Env, Abi->classify(GF->Sig), Res);
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
    bool Direct =
        T.Discr.K == G::Operand::Const && T.Discr.C.K == G::Constant::Func;
    auto CFIt = Direct ? CFuncs.find(T.Discr.C.Bits) : CFuncs.end();
    const CFunction *CFn = CFIt != CFuncs.end() ? &CFIt->second : nullptr;
    if (!CFn && Sig->callConv() == G::CallConv::C) {
      // A call through a C function pointer (an allocator's table entry,
      // ADR-0029): the C ABI of the pointer's type. Its scalars are
      // classified without Clang; an aggregate needs P04.07 (V076).
      auto [It, New] = CPointers.try_emplace(Sig);
      if (New) {
        cabiWork();
        std::optional<cabi::CallInfo> CI =
            cabi::classifyScalars(LM.getTargetTriple().str(), Sig);
        if (!CI)
          return llvm::createStringError(
              std::errc::not_supported,
              "internal error: a call through a C function pointer of type "
              "%s, which is not scalars only (V076)",
              Sig->str().c_str());
        cabi::IRSignature IS = cabi::irSignature(
            C, *CI, [&](const G::Type *Ty) { return memTy(Ty); });
        It->second = {std::move(*CI), std::move(IS)};
      }
      CFn = &It->second;
    }
    if (CFn) {
      // A C function: the C ABI decides how each argument and the result
      // travel (CABILower.h).
      const CFunction &CF = *CFn;
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
      cabi::Env Env = makeEnv(AllocaScratch);
      cabi::PreparedCall PC =
          cabi::lowerCArgs(Env, CF.Info, CF.Sig, Vals, Dest);
      cabiWork(PC.Args.size() + 1);
      // The call repeats the C ABI's attributes of the declaration
      // (extension, byval, sret, alignment), as the internal path does; a
      // call through a pointer has those of the pointer's type.
      llvm::CallBase *CI = emitCall(
          CF.Sig.Type, Callee, PC.Args, llvm::CallingConv::C,
          Direct ? Funcs[T.Discr.C.Bits]->getAttributes().removeFnAttributes(C)
                 : CF.Sig.Attrs,
          T.Unwind);
      llvm::Value *R = cabi::lowerCResult(Env, CF.Info, CF.Sig, PC, CI, Dest);
      if (R && !Sig->ret()->isUnit())
        storePlace(T.Dest, R);
      if (T.Target == G::NoId)
        B.CreateUnreachable();
      else
        B.CreateBr(Blocks[T.Target]);
      return llvm::Error::success();
    }
    // Go++ calling Go++ (GoABI.h), directly or through a pointer of a Go++
    // function type.
    const goabi::FnInfo &FI = Abi->classify(Sig);
    llvm::SmallVector<cabi::Value, 4> Vals;
    unsigned AggIdx = 0;
    for (size_t K = 0; K < T.Args.size(); ++K) {
      const G::Operand &A = T.Args[K];
      cabi::Value V;
      V.Ty = A.type();
      switch (FI.Params[K].K) {
      case goabi::Kind::Ignore:
        break;
      case goabi::Kind::Scalar:
        V.V = operand(A);
        break;
      case goabi::Kind::Direct:
        if (A.K != G::Operand::Const) {
          // Read where it is: the callee gets the scalars, its own copy.
          V.V = placeAddr(A.P);
          V.Known = LM.getDataLayout().getABITypeAlign(memTy(A.type()));
          break;
        }
        [[fallthrough]];
      case goabi::Kind::Indirect: {
        // The callee owns its parameter: it gets a copy (a constant is
        // stored in a scratch slot of this frame; the slot is reused by the
        // next call that passes this type in this position, ArgSlots).
        llvm::Type *MT = memTy(A.type());
        llvm::AllocaInst *&Tmp = ArgSlots[{MT, AggIdx++}];
        if (!Tmp)
          Tmp = entryAlloca(MT, "arg");
        emitInto(Tmp, A);
        V.V = Tmp;
        V.Known = Tmp->getAlign();
        break;
      }
      }
      Vals.push_back(V);
    }
    cabi::Value Dest;
    Dest.Ty = Sig->ret();
    if (isAggregate(Sig->ret()) && FI.Ret.K != goabi::Kind::Ignore) {
      Dest.V = placeAddr(T.Dest);
      Dest.Known = LM.getDataLayout().getABITypeAlign(memTy(Sig->ret()));
    }
    cabi::Env Env = makeEnv(AllocaScratch);
    std::vector<llvm::Value *> Args = Abi->lowerArgs(Env, FI, Vals, Dest);
    // The extension of narrow scalars is repeated at the call (the callee's
    // own attributes stay on the callee). A call through a pointer (a map's
    // hash, an itab slot, P04.06) gets what every definition of its
    // signature has, a thunk's included: one classification per signature.
    llvm::CallBase *CI =
        emitCall(FI.Type, Callee, Args, FI.CC, FI.Attrs, T.Unwind);
    llvm::Value *R = Abi->lowerResult(Env, FI, CI, Dest);
    if (R && !Sig->ret()->isUnit())
      storePlace(T.Dest, R);
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
      setLoc(G::Span(), 0);
      llvm::SmallVector<llvm::Value *, 3> CallArgs;
      CallArgs.push_back(B.CreateLoad(llvm::PointerType::getUnqual(C), PS.Pos));
      for (llvm::AllocaInst *A : PS.Args)
        CallArgs.push_back(B.CreateLoad(A->getAllocatedType(), A));
      // (A panic does not unwind until P04.07, which gives this call the
      // unwind action of its checks.)
      emitCall(Funcs[Rt]->getFunctionType(), Funcs[Rt], CallArgs,
               Funcs[Rt]->getCallingConv(), llvm::AttributeList(),
               G::UnwindAction());
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
  AddrTaken.assign(F.Locals.size(), false);
  DefSites.clear();
  HasFloatMul = false;
  G::BlockId DefBlock = 0;
  auto Def = [&](const G::Place &P) {
    if (P.Local != G::NoId && P.Local < Uses.size())
      DefSites.push_back({P.Local, DefBlock});
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
  for (size_t Bi = 0; Bi < F.Blocks.size(); ++Bi) {
    const G::BasicBlock &BB = F.Blocks[Bi];
    DefBlock = static_cast<G::BlockId>(Bi);
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
      if (S.RV.K == G::Rvalue::AddrOf && S.RV.P.Local < AddrTaken.size())
        AddrTaken[S.RV.P.Local] = true;
      if (S.RV.K == G::Rvalue::Binary &&
          (S.RV.Bin == G::BinOp::Mul || S.RV.Bin == G::BinOp::Div) &&
          S.RV.Ty->isFloat())
        HasFloatMul = true;
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

bool Lowering::trackable(G::LocalId L) const {
  if (GF->Locals[L].K == G::Local::Temp)
    return Uses[L] == 1;
  return !AddrTaken[L]; // a variable, a parameter, the result
}

void Lowering::computeDominators(const G::Function &F) {
  uint64_t &Steps = workCount(&LowerWorkCounts::FmaDomSteps);
  size_t N = F.Blocks.size();
  const uint32_t None = ~0u;
  // Reverse post-order of the blocks reachable from the entry (an explicit
  // stack), then the iterative algorithm of Cooper, Harvey and Kennedy.
  std::vector<std::vector<G::BlockId>> &Succ = SuccOf;
  Succ.assign(N, {});
  size_t Edges = 0;
  for (size_t I = 0; I < N; ++I)
    G::forEachSuccessor(F.Blocks[I].Term, [&](const G::BlockId &T) {
      ++Steps;
      if (T < N) {
        Succ[I].push_back(T);
        ++Edges;
      }
    });
  WalkBudget = 8 * (N + Edges) + 64;
  std::vector<G::BlockId> Post;
  std::vector<uint32_t> PostNum(N, None);
  std::vector<std::pair<G::BlockId, size_t>> Stack;
  std::vector<uint8_t> Seen(N, 0);
  Seen[0] = 1;
  Stack.push_back({0, 0});
  while (!Stack.empty()) {
    ++Steps;
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
    for (G::BlockId T : counted(Steps, Succ[B]))
      Pred[T].push_back(B);
  std::vector<uint32_t> IDom(N, None);
  IDom[0] = 0;
  auto Intersect = [&](G::BlockId A, G::BlockId B) {
    while (A != B) {
      while (PostNum[A] < PostNum[B]) {
        ++Steps;
        A = IDom[A];
      }
      while (PostNum[B] < PostNum[A]) {
        ++Steps;
        B = IDom[B];
      }
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
      for (G::BlockId P : counted(Steps, Pred[B]))
        if (IDom[P] != None)
          New = New == None ? P : Intersect(P, New);
      if (New != IDom[B]) {
        IDom[B] = New;
        Changed = true;
      }
    }
  }
  // Number the dominator tree; its preorder is the lowering order.
  std::vector<std::vector<G::BlockId>> Kids(N);
  for (G::BlockId B : counted(Steps, Post))
    if (B != 0 && IDom[B] != None)
      Kids[IDom[B]].push_back(B);
  DomIn.assign(N, None);
  DomOut.assign(N, None);
  LowerOrder.assign(1, 0);
  uint32_t Clock = 0;
  std::vector<std::pair<G::BlockId, size_t>> Walk{{0, 0}};
  DomIn[0] = Clock++;
  while (!Walk.empty()) {
    ++Steps;
    auto &[B, K] = Walk.back();
    if (K < Kids[B].size()) {
      G::BlockId C = Kids[B][K++];
      DomIn[C] = Clock++;
      LowerOrder.push_back(C);
      Walk.push_back({C, 0});
      continue;
    }
    DomOut[B] = Clock++;
    Walk.pop_back();
  }
  for (size_t I = 0; I < N; ++I)
    if (DomIn[I] == None)
      LowerOrder.push_back(static_cast<G::BlockId>(I));
  DomReady = true;
}

bool Lowering::dominates(G::BlockId A, G::BlockId B) {
  if (!DomReady)
    computeDominators(*GF);
  return DomIn[A] != ~0u && DomIn[B] != ~0u && DomIn[A] <= DomIn[B] &&
         DomOut[B] <= DomOut[A];
}

void Lowering::computeTopo(const G::Function &F) {
  // Tarjan's algorithm on an explicit stack: one visit per block and edge.
  uint64_t &Steps = workCount(&LowerWorkCounts::FmaSccSteps);
  size_t N = F.Blocks.size();
  const uint32_t Unset = ~0u;
  std::vector<std::vector<G::BlockId>> Succ(N);
  for (size_t I = 0; I < N; ++I)
    G::forEachSuccessor(F.Blocks[I].Term, [&](const G::BlockId &T) {
      ++Steps;
      if (T < N)
        Succ[I].push_back(T);
    });
  std::vector<uint32_t> Index(N, Unset), Low(N, 0), Scc(N, Unset);
  std::vector<uint8_t> OnStack(N, 0);
  std::vector<G::BlockId> Stack, Members;
  std::vector<std::pair<G::BlockId, size_t>> Call;
  InCycle.assign(N, 0);
  uint32_t Counter = 0, NumScc = 0;
  auto Visit = [&](G::BlockId B) {
    Index[B] = Low[B] = Counter++;
    Stack.push_back(B);
    OnStack[B] = 1;
    Call.push_back({B, 0});
  };
  if (N)
    Visit(0);
  while (!Call.empty()) {
    ++Steps;
    G::BlockId B = Call.back().first;
    if (Call.back().second < Succ[B].size()) {
      G::BlockId T = Succ[B][Call.back().second++];
      if (Index[T] == Unset)
        Visit(T);
      else if (OnStack[T])
        Low[B] = std::min(Low[B], Index[T]);
      continue;
    }
    if (Low[B] == Index[B]) {
      // B is the root of a component: its members are on Stack down to B.
      Members.clear();
      G::BlockId W;
      do {
        W = Stack.back();
        Stack.pop_back();
        OnStack[W] = 0;
        Scc[W] = NumScc;
        Members.push_back(W);
      } while (W != B);
      bool Cycle = Members.size() > 1;
      for (G::BlockId T : Succ[B])
        Cycle |= T == B; // a block that jumps to itself
      for (G::BlockId M : counted(Steps, Members))
        InCycle[M] = Cycle;
      ++NumScc;
    }
    Call.pop_back();
    if (!Call.empty())
      Low[Call.back().first] = std::min(Low[Call.back().first], Low[B]);
  }
  // Tarjan finishes the components in reverse topological order.
  Topo.assign(N, Unset);
  for (size_t I = 0; I < N; ++I)
    if (Scc[I] != Unset)
      Topo[I] = NumScc - 1 - Scc[I];
  TopoReady = true;
}

void Lowering::buildDefIndex() {
  if (!TopoReady)
    computeTopo(*GF);
  WriteTopos.assign(GF->Locals.size(), {});
  WritesInBlock.clear();
  // A write in a block no path reaches never runs: it is left out.
  for (const auto &[L, Blk] :
       counted(workCount(&LowerWorkCounts::FmaDefSteps), DefSites))
    if (Topo[Blk] != ~0u) {
      WriteTopos[L].push_back(Topo[Blk]);
      ++WritesInBlock[(uint64_t(L) << 32) | Blk];
    }
  // Sorting is n log n in the writes of one local, counted as its size.
  for (std::vector<uint32_t> &V :
       counted(workCount(&LowerWorkCounts::FmaDefSteps), WriteTopos))
    std::sort(V.begin(), V.end());
  DefIndexReady = true;
}

bool Lowering::unwrittenBetween(G::LocalId L, G::BlockId Bd, G::BlockId R) {
  if (!DefIndexReady)
    buildDefIndex();
  if (Topo[Bd] == ~0u || Topo[R] == ~0u)
    return false;
  const std::vector<uint32_t> &V = WriteTopos[L];
  size_t In = std::upper_bound(V.begin(), V.end(), Topo[R]) -
              std::lower_bound(V.begin(), V.end(), Topo[Bd]);
  size_t Own = WritesInBlock.lookup((uint64_t(L) << 32) | Bd);
  if (!InCycle[R])
    Own += WritesInBlock.lookup((uint64_t(L) << 32) | R);
  if (In == Own)
    return true;
  const TaintInfo &T = taintedBy(L, Bd, Topo[R]);
  return !T.Spent && !T.Blocks.count(R);
}

const Lowering::TaintInfo &Lowering::taintedBy(G::LocalId L, G::BlockId Bd,
                                               uint32_t Bound) {
  auto [It, New] = Tainted.try_emplace((uint64_t(L) << 32) | Bd);
  TaintInfo &T = It->second;
  if (!New && T.Bound >= Bound)
    return T;
  T.Bound = Bound;
  T.Spent = false;
  T.Blocks.clear();
  uint64_t &Steps = workCount(&LowerWorkCounts::FmaWalkSteps);
  size_t N = SuccOf.size();
  if (SeenReached.size() != N) {
    SeenReached.assign(N, 0);
    SeenAfter.assign(N, 0);
  }
  uint32_t Epoch = ++WalkEpoch;
  auto charge = [&]() {
    if (WalkBudget == 0)
      return !(T.Spent = true);
    --WalkBudget;
    ++Steps;
    return true;
  };
  // The blocks after Bd (not through Bd) up to the bound, and the writes of
  // L among them; then the blocks after those writes.
  std::vector<G::BlockId> Work{Bd}, Writes;
  while (!Work.empty() && !T.Spent) {
    G::BlockId B = Work.back();
    Work.pop_back();
    for (G::BlockId S : SuccOf[B]) {
      if (!charge())
        break;
      if (S == Bd || Topo[S] > Bound || SeenReached[S] == Epoch)
        continue;
      SeenReached[S] = Epoch;
      Work.push_back(S);
      if (WritesInBlock.lookup((uint64_t(L) << 32) | S))
        Writes.push_back(S);
    }
  }
  Work = std::move(Writes);
  while (!Work.empty() && !T.Spent) {
    G::BlockId B = Work.back();
    Work.pop_back();
    for (G::BlockId S : SuccOf[B]) {
      if (!charge())
        break;
      if (S == Bd || Topo[S] > Bound || SeenAfter[S] == Epoch)
        continue;
      SeenAfter[S] = Epoch;
      T.Blocks.insert(S);
      Work.push_back(S);
    }
  }
  return T;
}

void Lowering::setProduct(G::LocalId L, std::optional<ProductInfo> P) {
  auto It = Products.find(L);
  if (It == Products.end() && !P)
    return;
  ProductLog.push_back({L, It == Products.end()
                               ? std::nullopt
                               : std::optional<ProductInfo>(It->second)});
  if (P)
    Products[L] = *P;
  else
    Products.erase(It);
}

void Lowering::enterProductScope(G::BlockId B) {
  // The open blocks are the dominators of the last block entered (the walk
  // is a preorder of the dominator tree); those that do not dominate B are
  // done. An unreachable block dominates nothing and is dominated by
  // nothing, so it sees only its own products.
  uint64_t &Steps = workCount(&LowerWorkCounts::FmaDomSteps);
  while (!ProductScopes.empty() && !dominates(ProductScopes.back().first, B)) {
    size_t Start = ProductScopes.back().second;
    while (ProductLog.size() > Start) {
      ++Steps;
      auto &[L, Old] = ProductLog.back();
      if (Old)
        Products[L] = *Old;
      else
        Products.erase(L);
      ProductLog.pop_back();
    }
    ProductScopes.pop_back();
  }
  ProductScopes.push_back({B, ProductLog.size()});
}

const Lowering::ProductInfo *Lowering::productOf(const G::Operand &O) {
  return factOf(O, ProductInfo::Product);
}

const Lowering::ProductInfo *Lowering::factOf(const G::Operand &O, int Want) {
  if (O.K == G::Operand::Const || !O.P.Proj.empty())
    return nullptr;
  auto It = Products.find(O.P.Local);
  if (It == Products.end() || (Want >= 0 && It->second.Kind != Want))
    return nullptr;
  // This block wrote it (and nothing since: a write ends the entry), or a
  // block that dominates this one did (Products is scoped by the dominator
  // tree), and then no other write may lie on a path between the two.
  const ProductInfo &P = It->second;
  assert((P.Block == CurBlock || dominates(P.Block, CurBlock)) &&
         "a product of a block that does not dominate the reader");
  if (P.Block != CurBlock && !unwrittenBetween(O.P.Local, P.Block, CurBlock))
    return nullptr;
  return &P;
}

std::optional<double> Lowering::floatConstOf(const G::Operand &O) {
  if (O.K == G::Operand::Const) {
    if (!O.C.Ty->isFloat())
      return std::nullopt;
    if (O.C.K == G::Constant::Zero)
      return 0.0;
    if (O.C.K == G::Constant::Float)
      return llvm::bit_cast<double>(O.C.Bits);
    return std::nullopt;
  }
  if (const ProductInfo *P = factOf(O, ProductInfo::FloatConst))
    return P->KF;
  return std::nullopt;
}

std::optional<int64_t> Lowering::intConstOf(const G::Operand &O) {
  if (O.K == G::Operand::Const) {
    if (!O.C.Ty->isInt() || !O.C.Ty->isSigned())
      return std::nullopt;
    if (O.C.K == G::Constant::Zero)
      return 0;
    if (O.C.K == G::Constant::Int)
      return llvm::SignExtend64(O.C.Bits, O.C.Ty->bits());
    return std::nullopt;
  }
  if (const ProductInfo *P = factOf(O, ProductInfo::IntConst))
    return P->KI;
  return std::nullopt;
}

/// gc's reciprocalExact64/32 (rewrite.go): c is a normal power of two whose
/// reciprocal is one too.
static bool reciprocalExact(double C, bool F32) {
  if (F32) {
    uint32_t Bits = llvm::bit_cast<uint32_t>(static_cast<float>(C));
    uint32_t Exp = Bits >> 23 & 0xff;
    return (Bits & ((1u << 23) - 1)) == 0 && Exp != 0 && Exp != 0xff &&
           Exp != 0xfe;
  }
  uint64_t Bits = llvm::bit_cast<uint64_t>(C);
  uint64_t Exp = Bits >> 52 & 0x7ff;
  return (Bits & ((uint64_t(1) << 52) - 1)) == 0 && Exp != 0 && Exp != 0x7ff &&
         Exp != 0x7fe;
}

/// A constant operation as gc folds it: in the type's precision, and only
/// when the result is not NaN (generic.rules: `c*d == c*d`).
static std::optional<double> foldFloat(G::BinOp Op, double A, double B,
                                       bool F32) {
  double R = 0;
  auto Do = [&](auto X, auto Y) -> double {
    switch (Op) {
    case G::BinOp::Add:
      return X + Y;
    case G::BinOp::Sub:
      return X - Y;
    case G::BinOp::Mul:
      return X * Y;
    default:
      return X / Y;
    }
  };
  R = F32 ? Do(static_cast<float>(A), static_cast<float>(B)) : Do(A, B);
  if (R != R)
    return std::nullopt;
  return R;
}

Lowering::MulShape Lowering::mulShape(const G::Rvalue &R) {
  MulShape Sh;
  bool F32 = R.Ty->bits() == 32;
  std::optional<double> KA = floatConstOf(R.A), KB = floatConstOf(R.B);
  // The rules in gc's order: folding first, then x/c => x*(1/c), then the
  // rules of *1, *-1 and *2, which match the constant on either side.
  if (KA && KB)
    if (auto V = foldFloat(R.Bin, *KA, *KB, F32)) {
      Sh.K = MulShape::Folded;
      Sh.Value = *V;
      return Sh;
    }
  Sh.X = &R.A;
  std::optional<double> K = KB;
  if (R.Bin == G::BinOp::Div) {
    if (!KB || !reciprocalExact(*KB, F32))
      return Sh; // None: a division is no product
    K = F32 ? static_cast<double>(1.0f / static_cast<float>(*KB)) : 1.0 / *KB;
    Sh.FromDiv = true;
    if (KA)
      if (auto V = foldFloat(G::BinOp::Mul, *KA, *K, F32)) {
        Sh.K = MulShape::Folded;
        Sh.Value = *V;
        return Sh;
      }
  } else if (!KB && KA) {
    Sh.X = &R.B;
    K = KA;
  }
  Sh.K = MulShape::Product;
  if (K) {
    Sh.Factor = *K;
    if (*K == 1.0)
      Sh.K = MulShape::Same;
    else if (*K == -1.0)
      Sh.K = MulShape::Negated;
    else if (*K == 2.0)
      Sh.K = MulShape::Doubled;
  }
  return Sh;
}

std::optional<Lowering::ProductInfo>
Lowering::factOfAssign(const G::Statement &S, llvm::Value *V, G::BlockId Bi) {
  const G::Rvalue &R = S.RV;
  auto FloatK = [&](double K) {
    ProductInfo P{nullptr, nullptr, Bi};
    P.Kind = ProductInfo::FloatConst;
    P.KF = R.Ty->bits() == 32 ? static_cast<double>(static_cast<float>(K)) : K;
    return P;
  };
  auto Copy = [&](const ProductInfo *P) -> std::optional<ProductInfo> {
    if (!P)
      return std::nullopt;
    ProductInfo C = *P;
    C.Block = Bi;
    return C;
  };
  // gc: FNEGD (FMULD x y) => FNMULD x y, and FADD/FSUB fuse an FNMUL like
  // an FMUL (ARM64.rules; exactly -(x*y)); a constant other than 0 is folded.
  auto Negated = [&](const G::Operand &O) -> std::optional<ProductInfo> {
    if (const ProductInfo *P = productOf(O))
      return ProductInfo{B.CreateFNeg(P->X), P->Y, Bi};
    if (auto K = floatConstOf(O); K && *K != 0)
      return FloatK(-*K);
    return std::nullopt;
  };
  if (R.Ty->isInt()) {
    // The integer constants a conversion to a float may fold: a local written
    // with a constant (not a copy of one, so that only conversions look up
    // integer facts).
    if (R.K == G::Rvalue::Use && !S.Boundary && R.Ty->isSigned() &&
        R.A.K == G::Operand::Const)
      if (auto K = intConstOf(R.A)) {
        ProductInfo P{nullptr, nullptr, Bi};
        P.Kind = ProductInfo::IntConst;
        P.KI = *K;
        return P;
      }
    return std::nullopt;
  }
  if (!R.Ty->isFloat())
    return std::nullopt;
  switch (R.K) {
  case G::Rvalue::Use:
    // A copy is the same SSA value in gc, except across an inlined call
    // (Boundary).
    if (S.Boundary)
      return std::nullopt;
    if (R.A.K == G::Operand::Const) {
      if (auto K = floatConstOf(R.A))
        return FloatK(*K);
      return std::nullopt;
    }
    return Copy(factOf(R.A));
  case G::Rvalue::Unary:
    return R.Un == G::UnOp::Neg ? Negated(R.A) : std::nullopt;
  case G::Rvalue::Cast:
    if (R.CastOp == G::CastKind::FloatToFloat)
      if (auto K = floatConstOf(R.A))
        return FloatK(*K);
    if (R.CastOp == G::CastKind::IntToFloat && R.A.type()->isSigned())
      if (auto K = intConstOf(R.A))
        return FloatK(static_cast<double>(*K));
    return std::nullopt;
  case G::Rvalue::Binary:
    break;
  default:
    return std::nullopt;
  }
  if (R.Bin == G::BinOp::Add || R.Bin == G::BinOp::Sub) {
    // The second operand is looked up only when the first is a constant.
    if (auto KA = floatConstOf(R.A))
      if (auto KB = floatConstOf(R.B))
        if (auto F = foldFloat(R.Bin, *KA, *KB, R.Ty->bits() == 32))
          return FloatK(*F);
    return std::nullopt;
  }
  if (R.Bin != G::BinOp::Mul && R.Bin != G::BinOp::Div)
    return std::nullopt;
  MulShape Sh = mulShape(R);
  switch (Sh.K) {
  case MulShape::Folded:
    return FloatK(Sh.Value);
  case MulShape::Same:
    return Copy(factOf(*Sh.X));
  case MulShape::Negated:
    return Negated(*Sh.X);
  case MulShape::Product:
    if (auto *Op = llvm::dyn_cast<llvm::BinaryOperator>(V))
      return Sh.FromDiv
                 ? ProductInfo{Op->getOperand(0),
                               llvm::ConstantFP::get(V->getType(), Sh.Factor),
                               Bi}
                 : ProductInfo{Op->getOperand(0), Op->getOperand(1), Bi};
    return std::nullopt;
  case MulShape::None:
  case MulShape::Doubled: // x+x is a sum
    return std::nullopt;
  }
  return std::nullopt;
}

llvm::Value *Lowering::contracted(const G::Rvalue &R, llvm::Value *X,
                                  llvm::Value *Y) {
  if (!FuseFMA || !R.A.type()->isFloat() ||
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

llvm::Error Lowering::function(G::FuncId Id, const G::Function &F,
                               llvm::Function *Fn) {
  GF = &F;
  GFId = Id;
  LF = Fn;
  Slots.clear();
  Blocks.clear();
  Products.clear();
  ProductLog.clear();
  ProductScopes.clear();
  ArgSlots.clear();
  CScratch.clear();
  DomReady = false;
  TopoReady = false;
  DefIndexReady = false;
  Tainted.clear();
  SeenReached.clear();
  SeenAfter.clear();
  Panics.clear();
  countUses(F);
  if (Opts.DebugInfo)
    DI.beginFunction(Id, *Fn);
  llvm::BasicBlock *Entry = llvm::BasicBlock::Create(C, "entry", Fn);
  for (size_t I = 0; I < F.Blocks.size(); ++I)
    Blocks.push_back(llvm::BasicBlock::Create(C, "bb" + llvm::Twine(I), Fn));
  B.SetInsertPoint(Entry);
  // The entry block puts the parameters where their variables say they are
  // (their slots). It has no location, as in Clang: it stays in the row of
  // the function's first line, and the first instruction with a line, where
  // LLVM puts `prologue_end` and the debuggers stop at `break f`, comes after
  // it, so every parameter is in its slot there (debug/params-break.test).
  B.SetCurrentDebugLocation(llvm::DebugLoc());
  // The parameters and the result place, as the convention hands them over:
  // the values of the IR arguments (an aggregate: the address of its bytes).
  auto CFIt = CFuncs.find(Id);
  const goabi::FnInfo *GI =
      CFIt == CFuncs.end() ? &Abi->classify(F.Sig) : nullptr;
  CArgs.clear();
  for (llvm::Argument &A : Fn->args())
    CArgs.push_back(&A);
  std::vector<cabi::Value> Params;
  {
    cabi::Env Env = makeEnv(AllocaPrologue);
    Params = GI ? Abi->unpackParams(Env, *GI, CArgs)
                : cabi::unpackCParams(Env, CFIt->second.Info, CFIt->second.Sig,
                                      CArgs);
  }
  bool SRetResult = GI ? GI->Ret.K == goabi::Kind::Indirect : false;
  for (size_t I = 0; I < F.Locals.size(); ++I) {
    const G::Local &L = F.Locals[I];
    bool Param = I >= 1 && I <= F.numParams();
    if (L.Ty->isUnit()) {
      Slots.push_back(nullptr);
    } else if (I == 0 && SRetResult) {
      Slots.push_back(Fn->getArg(GI->SRet)); // built in the caller's buffer
    } else if (Param && isAggregate(L.Ty) && Params[I - 1].V) {
      Slots.push_back(Params[I - 1].V); // the body's copy
    } else {
      Slots.push_back(B.CreateAlloca(memTy(L.Ty), nullptr, L.Name));
    }
  }
  for (unsigned I = 0; I < F.numParams(); ++I) {
    G::Type *PT = F.Locals[1 + I].Ty;
    if (isAggregate(PT) || PT->isUnit())
      continue;
    storePlace(G::localPlace(1 + I, PT), Params[I].V);
  }
  llvm::Instruction *EntryBr = B.CreateBr(Blocks[0]);
  // Every named parameter and variable is declared at its storage for the
  // whole function (the debugger shows it in its scope, or in the scope of
  // the inlined callee it came from).
  llvm::SmallVector<llvm::Value *, 16> Kept;
  if (Opts.DebugInfo)
    for (size_t I = 0; I < F.Locals.size(); ++I)
      if (llvm::Value *Slot = DI.declareLocal(I, Slots[I], EntryBr))
        Kept.push_back(Slot);
  // With a float product to fuse, in the preorder of the dominator tree, so
  // that Products can be scoped by it (see FuseFMA); else in index order.
  Scoped = FuseFMA && HasFloatMul;
  if (Scoped)
    computeDominators(F);
  for (size_t K = 0; K < F.Blocks.size(); ++K) {
    G::BlockId Bi = Scoped ? LowerOrder[K] : static_cast<G::BlockId>(K);
    const G::BasicBlock &BB = F.Blocks[Bi];
    B.SetInsertPoint(Blocks[Bi]);
    CurBlock = Bi;
    if (Scoped)
      enterProductScope(Bi);
    if (BB.Catch)
      return fail("a catch block, the handler of `defer` (P04.07)");
    for (const G::Statement &S : BB.Stmts) {
      // The rest of the entry has no line either (Statement::Prologue).
      if (S.Prologue)
        B.SetCurrentDebugLocation(llvm::DebugLoc());
      else
        setLoc(S.Loc, S.Scope);
      switch (S.K) {
      case G::Statement::Assign: {
        if (S.Dest.Ty->isUnit()) {
          setProduct(S.Dest.Local, std::nullopt);
          break; // nothing to store; the operands have no effects
        }
        if (isAggregate(S.Dest.Ty)) {
          setProduct(S.Dest.Local, std::nullopt);
          if (llvm::Error E = assignAggregate(S.Dest, S.RV))
            return E;
          break;
        }
        llvm::Expected<llvm::Value *> V = rvalue(S.RV);
        if (!V)
          return V.takeError();
        // What the destination holds, a product or a constant (FuseFMA):
        // decided before the old entry goes, since `t = t + z` and `t = -t`
        // read the t that was there (factOfAssign).
        std::optional<ProductInfo> New;
        if (Scoped && S.Dest.Proj.empty()) {
          New = factOfAssign(S, *V, Bi);
          G::LocalId L = S.Dest.Local;
          if (New && (New->Kind == ProductInfo::Product ? !trackable(L)
                                                        : AddrTaken[L]))
            New.reset();
        }
        storePlace(S.Dest, *V);
        setProduct(S.Dest.Local, New);
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
    // A call writes its destination after the statements of its block.
    if (BB.Term.K == G::Terminator::Call && BB.Term.Dest.Local != G::NoId)
      setProduct(BB.Term.Dest.Local, std::nullopt);
  }
  // `opt 0` with DWARF: the variables stay in memory as the code wrote them.
  if (Opts.KeepVariables)
    for (llvm::Value *Slot : Kept)
      keepInMemory(Slot);
  return llvm::Error::success();
}

/// Marks the accesses of a variable's slot volatile (P04.12, M2 of the team
/// build; docs/design/pipelines.md section 7): its loads and stores, and the
/// memset and memcpy that write it. The passes of `opt 0` (SROA, and
/// instcombine, which also promotes a slot and drops a store that the next
/// one overwrites) then leave the variable and each of its stores as the code
/// wrote them, so a debugger prints, at any line, the value the statements
/// before it left. Volatile tells the optimizer only that an access must
/// happen as written, which is true of every access, so no pass can use it
/// to change what the program does. A memcpy out of the variable only reads
/// it and stays plain: a volatile one would pin its destination too, and the
/// temporaries are the optimizer's (SROA removes the copies of aggregates, a
/// 512 KB array passed 40 times in run/core-argslots.test), as are the big
/// copies of an inlined callee's variables (DebugInfoBuilder::declareLocal).
void Lowering::keepInMemory(llvm::Value *Slot) {
  llvm::SmallVector<llvm::Value *, 8> Work{Slot};
  llvm::SmallPtrSet<llvm::Value *, 8> Seen{Slot};
  while (!Work.empty()) {
    llvm::Value *V = Work.pop_back_val();
    for (llvm::Use &U : V->uses()) {
      llvm::User *Usr = U.getUser();
      if (auto *L = llvm::dyn_cast<llvm::LoadInst>(Usr)) {
        L->setVolatile(true);
      } else if (auto *S = llvm::dyn_cast<llvm::StoreInst>(Usr)) {
        if (S->getPointerOperand() == V)
          S->setVolatile(true);
      } else if (auto *GEP = llvm::dyn_cast<llvm::GetElementPtrInst>(Usr)) {
        if (Seen.insert(GEP).second)
          Work.push_back(GEP);
      } else if (auto *MI = llvm::dyn_cast<llvm::MemIntrinsic>(Usr)) {
        if (U.getOperandNo() == 0) // the destination
          MI->setVolatile(B.getInt1(true));
      }
    }
  }
}

llvm::Error Lowering::run() {
  if (Opts.DebugInfo)
    DI.start();
  // gc contracts on arm64 and not on amd64 (D4.12); see FuseFMA.
  FuseFMA = LM.getTargetTriple().isAArch64();
  prepareTypes();
  if (NoStackAt)
    return nestingError();
  for (const G::Global &Gl : GM.Globals) {
    // The static data (Global::Parts) names functions and other globals: its
    // value is set below, once they exist.
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
    // (A table with pointers in it is a type descriptor or an itab: its
    // address is its identity, so it is never merged with another.)
    if (Gl.ReadOnly && Gl.Parts.empty())
      GV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    Globals.push_back(GV);
    if (Opts.DebugInfo)
      DI.declareGlobal(Gl, GV);
  }
  Abi = std::make_unique<goabi::GoAbi>(
      C, LM.getDataLayout(), goabi::Policy::forTriple(LM.getTargetTriple()),
      [this](const G::Type *T) { return memTy(T); });
  Abi->Work = Opts.CABIWork;
  if (llvm::Error E = classifyCFunctions())
    return E;
  for (size_t I = 0; I < GM.Funcs.size(); ++I)
    Funcs.push_back(declare(I, *GM.Funcs[I]));
  for (size_t I = 0; I < GM.Globals.size(); ++I)
    if (!GM.Globals[I].Parts.empty())
      Globals[I]->setInitializer(staticData(GM.Globals[I]));
  for (size_t I = 0; I < GM.Funcs.size(); ++I)
    if (!GM.Funcs[I]->isDeclaration())
      if (llvm::Error E = function(I, *GM.Funcs[I], Funcs[I]))
        return E;
  if (NoStackAt)
    return nestingError();
  GF = nullptr;
  if (Opts.DebugInfo) {
    // finish makes the structs that declarations stand for: it can fail too.
    DI.finish();
    if (DI.failedAt()) {
      NoStackAt = DI.failedAt();
      return nestingError();
    }
    if (Opts.Counts) {
      Opts.Counts->DITypeSteps += DI.counts().TypeSteps;
      Opts.Counts->DIVariableSteps += DI.counts().VariableSteps;
      Opts.Counts->DILocationSteps += DI.counts().LocationSteps;
      Opts.Counts->DIScopeSteps += DI.counts().ScopeSteps;
    }
  }
  return llvm::Error::success();
}

} // namespace

llvm::CallBase *Lowering::emitCall(llvm::FunctionType *FT, llvm::Value *Callee,
                                   llvm::ArrayRef<llvm::Value *> Args,
                                   llvm::CallingConv::ID CC,
                                   llvm::AttributeList AL,
                                   const G::UnwindAction &U) {
  // An unwind edge into this function (Cleanup, Terminate) makes an invoke:
  // P04.07, until which terminator() refuses them, so this is a call.
  // (swiftcc keeps across a call the registers C keeps, which the unwinder
  // restores as it does for C: rbx, rbp and r12-r15 on amd64, no XMM
  // register; x19-x28, fp, lr and d8-d15 on arm64. An invoke needs nothing
  // more, ADR-0030.)
  llvm::CallInst *Call = B.CreateCall(FT, Callee, Args);
  Call->setCallingConv(CC);
  Call->setAttributes(AL);
  if (U.K == G::UnwindAction::Unreachable)
    Call->addFnAttr(llvm::Attribute::NoUnwind);
  return Call;
}

llvm::Error gopp::checkCalls(const llvm::Module &M) {
  for (const llvm::Function &F : M)
    for (const llvm::BasicBlock &BB : F)
      for (const llvm::Instruction &I : BB) {
        const auto *Call = llvm::dyn_cast<llvm::CallBase>(&I);
        if (!Call)
          continue;
        // A direct call must use its callee's convention: LLVM's verifier
        // does not say so, and the program would run with its arguments in
        // the wrong registers (swiftcc and ccc put them elsewhere).
        if (const llvm::Function *Callee = Call->getCalledFunction())
          if (!Callee->isIntrinsic() &&
              Call->getCallingConv() != Callee->getCallingConv())
            return llvm::createStringError(
                std::errc::invalid_argument,
                "internal error: the call of %s in %s does not use its "
                "calling convention",
                Callee->getName().str().c_str(), F.getName().str().c_str());
      }
  // P04.06: static data (an itab, a method table, a type
  // descriptor's == and hash) holds the functions Go++ calls through a
  // pointer of a Go++ function type (an itab slot), so each must have the
  // Go++ convention of the target: a thunk or glue of another convention
  // would get its arguments in other registers. (A C function's address goes
  // only to C, or to a typed allocator table entry at run time.)
  const llvm::CallingConv::ID GoCC =
      goabi::Policy::forTriple(M.getTargetTriple()).CC;
  llvm::SmallVector<const llvm::Constant *, 16> Work;
  llvm::SmallPtrSet<const llvm::Constant *, 32> Seen;
  for (const llvm::GlobalVariable &GV : M.globals()) {
    if (!GV.hasInitializer() || GV.getName().starts_with("llvm."))
      continue;
    Work.push_back(GV.getInitializer());
    while (!Work.empty()) {
      const llvm::Constant *K = Work.pop_back_val();
      if (const auto *Fn = llvm::dyn_cast<llvm::Function>(K)) {
        if (Fn->getCallingConv() != GoCC)
          return llvm::createStringError(
              std::errc::invalid_argument,
              "internal error: the static data %s holds %s, which does not "
              "have the Go++ calling convention of the target (it is called "
              "through a pointer of a Go++ function type)",
              GV.getName().str().c_str(), Fn->getName().str().c_str());
        continue;
      }
      // (Another global's address: its own initializer is walked.)
      if (llvm::isa<llvm::GlobalValue>(K))
        continue;
      for (const llvm::Use &U : K->operands())
        if (const auto *Op = llvm::dyn_cast<llvm::Constant>(U.get());
            Op && Seen.insert(Op).second)
          Work.push_back(Op);
    }
  }
  return llvm::Error::success();
}

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
  O.Optimized = TM.getOptLevel() != llvm::CodeGenOptLevel::None;
  Lowering L(Ctx, M, *LM, O);
  if (llvm::Error E = L.run())
    return std::move(E);
  // Every call as the conventions want it: an internal error here, never a
  // wrong program.
  if (llvm::Error E = checkCalls(*LM))
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
