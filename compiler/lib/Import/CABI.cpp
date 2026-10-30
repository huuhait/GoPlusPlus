//===- CABI.cpp - C ABI classification through Clang ----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Import/CABI.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/RecordLayout.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "clang/CodeGen/CodeGenABITypes.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendOptions.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <map>
#include <mutex>

using namespace gopp;
using namespace gopp::cabi;

//===----------------------------------------------------------------------===//
// LTy
//===----------------------------------------------------------------------===//

std::string LTy::str() const {
  switch (K) {
  case Void:
    return "void";
  case Int:
    return "i" + std::to_string(Bits);
  case Half:
    return "half";
  case BFloat:
    return "bfloat";
  case Float:
    return "float";
  case Double:
    return "double";
  case X86FP80:
    return "x86_fp80";
  case FP128:
    return "fp128";
  case Ptr:
    return "ptr";
  case Vector:
    return "<" + std::to_string(Count) + " x " + Elems[0].str() + ">";
  case Array:
    return "[" + std::to_string(Count) + " x " + Elems[0].str() + "]";
  case Struct: {
    std::string S = Packed ? "<{" : "{";
    for (size_t I = 0; I < Elems.size(); ++I)
      S += (I ? ", " : " ") + Elems[I].str();
    return S + (Packed ? " }>" : " }");
  }
  }
  return "?";
}

llvm::Type *LTy::get(llvm::LLVMContext &C) const {
  switch (K) {
  case Void:
    return llvm::Type::getVoidTy(C);
  case Int:
    return llvm::IntegerType::get(C, Bits);
  case Half:
    return llvm::Type::getHalfTy(C);
  case BFloat:
    return llvm::Type::getBFloatTy(C);
  case Float:
    return llvm::Type::getFloatTy(C);
  case Double:
    return llvm::Type::getDoubleTy(C);
  case X86FP80:
    return llvm::Type::getX86_FP80Ty(C);
  case FP128:
    return llvm::Type::getFP128Ty(C);
  case Ptr:
    return llvm::PointerType::getUnqual(C);
  case Vector:
    return llvm::FixedVectorType::get(Elems[0].get(C), Count);
  case Array:
    return llvm::ArrayType::get(Elems[0].get(C), Count);
  case Struct: {
    llvm::SmallVector<llvm::Type *, 4> Fs;
    for (const LTy &E : Elems)
      Fs.push_back(E.get(C));
    return llvm::StructType::get(C, Fs, Packed);
  }
  }
  return nullptr;
}

const char *ArgInfo::kindName() const {
  switch (K) {
  case Kind::Direct:
    return "direct";
  case Kind::Extend:
    return "extend";
  case Kind::Indirect:
    return "indirect";
  case Kind::IndirectAliased:
    return "indirect-aliased";
  case Kind::Ignore:
    return "ignore";
  case Kind::Expand:
    return "expand";
  case Kind::CoerceAndExpand:
    return "coerce-and-expand";
  case Kind::InAlloca:
    return "inalloca";
  case Kind::TargetSpecific:
    return "target-specific";
  }
  return "?";
}

std::string ArgInfo::str(bool IsResult) const {
  std::string S = kindName();
  if (HasCoerce || K == Kind::CoerceAndExpand)
    S += " " + Coerce.str();
  if (Flatten && Coerce.K == LTy::Struct)
    S += " flatten";
  if (K == Kind::Extend)
    S += SExt ? " signext" : ZExt ? " zeroext" : "";
  if (Offset)
    S += " offset " + std::to_string(Offset);
  if (StackAlign)
    S += " alignstack " + std::to_string(StackAlign);
  if (K == Kind::Indirect) {
    if (!IsResult)
      S += ByVal ? " byval" : " copy";
    if (Align)
      S += " align " + std::to_string(Align);
    if (Realign)
      S += " realign";
  }
  if (InReg)
    S += " inreg";
  if (HasPadding)
    S += " padding " + Padding.str();
  return S;
}

std::string CallInfo::str() const {
  std::string S = "ret: " + Ret.str(true);
  for (size_t I = 0; I < Args.size(); ++I)
    S += "\narg " + std::to_string(I) + ": " + Args[I].str();
  return S;
}

//===----------------------------------------------------------------------===//
// The Clang worlds: one per target per process (M3)
//===----------------------------------------------------------------------===//

namespace {

/// What Clang needs to classify for one target: a compiler instance with the
/// target and an AST context to make C types in, and the code generator whose
/// CodeGenModule CodeGenABITypes wants (it never generates anything here).
/// Made the first time a process asks for the target and kept until the
/// process ends, so its cost is paid once however many modules the process
/// compiles. Clang is not thread-safe: every use holds Mu.
struct World {
  std::mutex Mu;
  llvm::LLVMContext LLCtx; // Clang's IR types; outlives CG and CI
  std::unique_ptr<clang::CompilerInstance> CI;
  std::unique_ptr<clang::CodeGenerator> CG;
  clang::ASTContext *AC = nullptr;
  std::string DataLayout;
  /// The C records made so far, by their canonical field types: a structure
  /// is one record for the life of the process, whichever module (and
  /// gir::Context) asks, so the world grows with the distinct structures and
  /// not with the modules compiled.
  std::map<std::vector<const void *>, clang::QualType> Records;
  unsigned Names = 0;
};

struct Worlds {
  std::mutex Mu;
  std::map<std::string, std::unique_ptr<World>> ByTarget;
  unsigned Made = 0;
};

/// The worlds of this process, made on first use.
Worlds &worlds() {
  static Worlds W;
  return W;
}

llvm::Expected<std::unique_ptr<World>> makeWorld(const ClassifierOptions &O) {
  auto W = std::make_unique<World>();
  auto Inv = std::make_shared<clang::CompilerInvocation>();
  W->CI = std::make_unique<clang::CompilerInstance>(Inv);
  W->CI->createDiagnostics(new clang::IgnoringDiagConsumer());
  std::vector<std::string> Strs = {"-triple", O.Triple, "-x", "c",
                                   "-std=gnu17"};
  if (!O.CPU.empty()) {
    Strs.push_back("-target-cpu");
    Strs.push_back(O.CPU);
  }
  llvm::SmallVector<llvm::StringRef, 8> Fs;
  llvm::StringRef(O.Features).split(Fs, ',', -1, false);
  for (llvm::StringRef F : Fs) {
    Strs.push_back("-target-feature");
    Strs.push_back(F.str());
  }
  Strs.push_back("cabi.c");
  std::vector<const char *> Args;
  for (const std::string &A : Strs)
    Args.push_back(A.c_str());
  clang::CompilerInstance &CI = *W->CI;
  if (!clang::CompilerInvocation::CreateFromArgs(CI.getInvocation(), Args,
                                                 CI.getDiagnostics()))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "cannot set up Clang for " + O.Triple);
  CI.setVirtualFileSystem(llvm::vfs::getRealFileSystem());
  CI.setTarget(clang::TargetInfo::CreateTargetInfo(
      CI.getDiagnostics(), CI.getInvocation().getTargetOpts()));
  if (!CI.hasTarget())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "Clang has no target " + O.Triple);
  CI.createFileManager();
  CI.createSourceManager();
  CI.createPreprocessor(clang::TU_Complete);
  CI.createASTContext();
  W->AC = &CI.getASTContext();
  W->CG = clang::CreateLLVMCodeGen(CI, "cabi", W->LLCtx);
  W->CG->Initialize(*W->AC);
  W->DataLayout = W->CG->GetModule()->getDataLayoutStr();
  return std::move(W);
}

/// The world of O's target, made on the first ask of the process.
llvm::Expected<World *> worldFor(const ClassifierOptions &O) {
  Worlds &Ws = worlds();
  std::string Key = O.Triple + "\n" + O.CPU + "\n" + O.Features;
  std::lock_guard<std::mutex> G(Ws.Mu);
  auto It = Ws.ByTarget.find(Key);
  if (It != Ws.ByTarget.end())
    return It->second.get();
  auto W = makeWorld(O);
  if (!W)
    return W.takeError();
  ++Ws.Made;
  return (Ws.ByTarget[Key] = std::move(*W)).get();
}

} // namespace

unsigned cabi::clangWorldsMade() {
  Worlds &Ws = worlds();
  std::lock_guard<std::mutex> G(Ws.Mu);
  return Ws.Made;
}

//===----------------------------------------------------------------------===//
// A classifier: the GIR types of one module in a world
//===----------------------------------------------------------------------===//

struct Classifier::Impl {
  ClassifierOptions Opts;
  World *W = nullptr;
  /// The C type of a GIR type. Real has T's own fields (its layout); ABI is
  /// what Clang classifies: the same, except that a struct of more than
  /// FlatSize bytes is a flat record of the same size and alignment (as is
  /// then every struct that holds one, being larger still).
  struct Mapped {
    clang::QualType Real, ABI;
  };
  /// Keyed by GIR types (state of this classifier, guarded by W->Mu).
  llvm::DenseMap<const gir::Type *, Mapped> Map;
  llvm::DenseMap<const gir::Type *, std::unique_ptr<CallInfo>> Cache;
  ClassifierStats Stats;

  Mapped map(const gir::Type *T);
  clang::QualType record(llvm::ArrayRef<clang::QualType> Fields);
  clang::QualType array(clang::QualType Elem, uint64_t Len);
  LTy ltyOf(llvm::Type *T);
  ArgInfo argInfo(const clang::CodeGen::ABIArgInfo &AI, const gir::Type *Ty,
                  clang::QualType QT);
};

clang::QualType Classifier::Impl::array(clang::QualType Elem, uint64_t Len) {
  return W->AC->getConstantArrayType(Elem, llvm::APInt(64, Len), nullptr,
                                     clang::ArraySizeModifier::Normal, 0);
}

/// A C struct with fields f0, f1, ... of these types; one per structure in
/// the world.
clang::QualType
Classifier::Impl::record(llvm::ArrayRef<clang::QualType> Fields) {
  clang::ASTContext &AC = *W->AC;
  std::vector<const void *> Key;
  for (clang::QualType F : Fields)
    Key.push_back(AC.getCanonicalType(F).getAsOpaquePtr());
  clang::QualType &Slot = W->Records[std::move(Key)];
  if (!Slot.isNull())
    return Slot;
  clang::IdentifierInfo *Name =
      &AC.Idents.get("gopp_t" + std::to_string(W->Names++));
  auto *RD = clang::RecordDecl::Create(
      AC, clang::TagTypeKind::Struct, AC.getTranslationUnitDecl(),
      clang::SourceLocation(), clang::SourceLocation(), Name);
  RD->startDefinition();
  unsigned N = 0;
  for (clang::QualType FT : Fields) {
    auto *FD = clang::FieldDecl::Create(
        AC, RD, clang::SourceLocation(), clang::SourceLocation(),
        &AC.Idents.get("f" + std::to_string(N++)), FT,
        AC.getTrivialTypeSourceInfo(FT), nullptr, false, clang::ICIS_NoInit);
    RD->addDecl(FD);
  }
  RD->completeDefinition();
  Slot = AC.getCanonicalTagType(RD);
  return Slot;
}

Classifier::Impl::Mapped Classifier::Impl::map(const gir::Type *T) {
  using K = gir::Type::Kind;
  if (auto It = Map.find(T); It != Map.end())
    return It->second;
  ++Stats.TypesMapped;
  clang::ASTContext &AC = *W->AC;
  Mapped R;
  switch (T->getKind()) {
  case K::Unit:
    R.Real = AC.VoidTy;
    break;
  case K::Bool:
    R.Real = AC.BoolTy;
    break;
  case K::Int:
    R.Real = AC.getIntTypeForBitwidth(T->bits(), T->isSigned());
    break;
  case K::Float:
    R.Real = T->bits() == 32 ? AC.FloatTy : AC.DoubleTy;
    break;
  case K::Ptr:
  case K::Func:
    R.Real = AC.VoidPtrTy; // the pointee does not matter to the ABI
    break;
  case K::Array: {
    Mapped E = map(T->elem());
    R.Real = array(E.Real, T->length());
    R.ABI = E.ABI == E.Real ? R.Real : array(E.ABI, T->length());
    break;
  }
  case K::Struct:
  case K::Option: {
    // An Option[T] is `struct { T v; unsigned char set; }` (D4.5).
    llvm::SmallVector<clang::QualType, 4> Real, ABI;
    auto Add = [&](const Mapped &F) {
      Real.push_back(F.Real);
      ABI.push_back(F.ABI);
    };
    if (T->isOption()) {
      Add(map(T->elem()));
      Add({AC.UnsignedCharTy, AC.UnsignedCharTy});
    } else {
      for (const gir::Type *F : T->elems())
        Add(map(F));
    }
    R.Real = record(Real);
    clang::TypeInfoChars TI = AC.getTypeInfoInChars(R.Real);
    uint64_t Size = TI.Width.getQuantity(), Align = TI.Align.getQuantity();
    if (Opts.FlatSize && Size > Opts.FlatSize && llvm::isPowerOf2_64(Align) &&
        Align <= 8)
      R.ABI = record(
          {array(AC.getIntTypeForBitwidth(8 * Align, false), Size / Align)});
    else
      R.ABI = Real == ABI ? R.Real : record(ABI);
    break;
  }
  case K::String:
  case K::Slice:
  case K::Tuple:
    break; // not C types: the caller checked (gir::CTypeRules)
  }
  if (R.ABI.isNull())
    R.ABI = R.Real;
  Map[T] = R;
  return R;
}

LTy Classifier::Impl::ltyOf(llvm::Type *T) {
  LTy R;
  switch (T->getTypeID()) {
  case llvm::Type::VoidTyID:
    R.K = LTy::Void;
    break;
  case llvm::Type::IntegerTyID:
    R.K = LTy::Int;
    R.Bits = T->getIntegerBitWidth();
    break;
  case llvm::Type::HalfTyID:
    R.K = LTy::Half;
    break;
  case llvm::Type::BFloatTyID:
    R.K = LTy::BFloat;
    break;
  case llvm::Type::FloatTyID:
    R.K = LTy::Float;
    break;
  case llvm::Type::DoubleTyID:
    R.K = LTy::Double;
    break;
  case llvm::Type::X86_FP80TyID:
    R.K = LTy::X86FP80;
    break;
  case llvm::Type::FP128TyID:
    R.K = LTy::FP128;
    break;
  case llvm::Type::PointerTyID:
    R.K = LTy::Ptr;
    break;
  case llvm::Type::FixedVectorTyID:
    R.K = LTy::Vector;
    R.Count = llvm::cast<llvm::FixedVectorType>(T)->getNumElements();
    R.Elems.push_back(
        ltyOf(llvm::cast<llvm::FixedVectorType>(T)->getElementType()));
    break;
  case llvm::Type::ArrayTyID:
    R.K = LTy::Array;
    R.Count = T->getArrayNumElements();
    R.Elems.push_back(ltyOf(T->getArrayElementType()));
    break;
  case llvm::Type::StructTyID: {
    auto *ST = llvm::cast<llvm::StructType>(T);
    R.K = LTy::Struct;
    R.Packed = ST->isPacked();
    for (llvm::Type *E : ST->elements())
      R.Elems.push_back(ltyOf(E));
    break;
  }
  default:
    // Scalable vectors, target extension types: only a target-specific
    // ABIArgInfo names them, and that kind is refused.
    R.K = LTy::Void;
    break;
  }
  return R;
}

ArgInfo Classifier::Impl::argInfo(const clang::CodeGen::ABIArgInfo &AI,
                                  const gir::Type *Ty, clang::QualType QT) {
  using AK = ArgInfo::Kind;
  using CAI = clang::CodeGen::ABIArgInfo;
  ArgInfo R;
  R.Ty = Ty;
  switch (AI.getKind()) {
  case CAI::Direct:
    R.K = AK::Direct;
    break;
  case CAI::Extend:
    R.K = AK::Extend;
    break;
  case CAI::Indirect:
    R.K = AK::Indirect;
    break;
  case CAI::IndirectAliased:
    R.K = AK::IndirectAliased;
    break;
  case CAI::Ignore:
    R.K = AK::Ignore;
    break;
  case CAI::Expand:
    R.K = AK::Expand;
    break;
  case CAI::CoerceAndExpand:
    R.K = AK::CoerceAndExpand;
    break;
  case CAI::TargetSpecific:
    R.K = AK::TargetSpecific;
    break;
  case CAI::InAlloca:
    R.K = AK::InAlloca;
    break;
  }
  switch (AI.getKind()) {
  case CAI::Direct:
  case CAI::Extend: {
    if (llvm::Type *C = AI.getCoerceToType()) {
      R.HasCoerce = true;
      R.Coerce = ltyOf(C);
    }
    R.Offset = AI.getDirectOffset();
    R.StackAlign = AI.getDirectAlign();
    R.InReg = AI.getInReg();
    if (AI.isDirect())
      R.Flatten = AI.getCanBeFlattened();
    if (AI.isExtend()) {
      R.SExt = AI.isSignExt();
      R.ZExt = AI.isZeroExt();
    }
    if (llvm::Type *P = AI.getPaddingType()) {
      R.HasPadding = true;
      R.Padding = ltyOf(P);
      R.PaddingInReg = AI.getPaddingInReg();
    }
    // A Direct aggregate with no coerce type is passed as its own LLVM
    // type (the record's body).
    if (QT->isRecordType() && !R.HasCoerce) {
      R.HasCoerce = true;
      R.Coerce = ltyOf(clang::CodeGen::convertTypeForMemory(W->CG->CGM(), QT));
    }
    break;
  }
  case CAI::Indirect:
  case CAI::IndirectAliased:
    R.Align = AI.getIndirectAlign().getQuantity();
    R.Realign = AI.getIndirectRealign();
    R.AddrSpace = AI.getIndirectAddrSpace();
    if (AI.isIndirect()) {
      R.ByVal = AI.getIndirectByVal();
      R.InReg = AI.getInReg();
      R.SRetAfterThis = AI.isSRetAfterThis();
    }
    break;
  case CAI::CoerceAndExpand: {
    R.HasCoerce = true;
    R.Coerce = ltyOf(AI.getCoerceAndExpandType());
    llvm::StructType *ST = AI.getCoerceAndExpandType();
    for (unsigned I = 0; I < ST->getNumElements(); ++I)
      if (!CAI::isPaddingForCoerceAndExpand(ST->getElementType(I)))
        R.ExpandIdx.push_back(I);
    break;
  }
  case CAI::InAlloca:
    R.InAllocaField = AI.getInAllocaFieldIndex();
    R.InAllocaIndirect = AI.getInAllocaIndirect();
    R.InAllocaSRet = AI.getInAllocaSRet();
    break;
  case CAI::Ignore:
  case CAI::Expand:
  case CAI::TargetSpecific:
    break;
  }
  return R;
}

//===----------------------------------------------------------------------===//
// Classifier
//===----------------------------------------------------------------------===//

Classifier::Classifier() = default;
Classifier::~Classifier() = default;

llvm::Expected<std::unique_ptr<Classifier>>
Classifier::create(const ClassifierOptions &O) {
  auto W = worldFor(O);
  if (!W)
    return W.takeError();
  std::unique_ptr<Classifier> C(new Classifier());
  C->I = std::make_unique<Impl>();
  C->I->Opts = O;
  C->I->W = *W;
  return std::move(C);
}

const CallInfo &Classifier::classify(const gir::Type *Sig) {
  std::lock_guard<std::mutex> G(I->W->Mu);
  ++I->Stats.Asked;
  if (auto It = I->Cache.find(Sig); It != I->Cache.end())
    return *It->second;
  ++I->Stats.Classified;
  clang::ASTContext &AC = *I->W->AC;
  llvm::SmallVector<clang::CanQualType, 8> Args;
  for (const gir::Type *P : Sig->elems())
    Args.push_back(
        clang::CanQualType::CreateUnsafe(AC.getCanonicalType(I->map(P).ABI)));
  clang::CanQualType Ret = clang::CanQualType::CreateUnsafe(
      AC.getCanonicalType(I->map(Sig->ret()).ABI));
  clang::FunctionType::ExtInfo EI(I->Opts.Swift ? clang::CC_Swift
                                                : clang::CC_C);
  const clang::CodeGen::CGFunctionInfo &FI =
      clang::CodeGen::arrangeFreeFunctionCall(
          I->W->CG->CGM(), Ret, Args, EI, clang::CodeGen::RequiredArgs::All);
  auto CI = std::make_unique<CallInfo>();
  CI->Ret = I->argInfo(FI.getReturnInfo(), Sig->ret(), clang::QualType(Ret));
  unsigned N = 0;
  for (const auto &A : FI.arguments()) {
    CI->Args.push_back(I->argInfo(A.info, Sig->elems()[N], A.type));
    ++N;
  }
  return *(I->Cache[Sig] = std::move(CI));
}

ClangLayout Classifier::layoutOf(const gir::Type *T) {
  std::lock_guard<std::mutex> G(I->W->Mu);
  clang::ASTContext &AC = *I->W->AC;
  clang::QualType QT = I->map(T).Real;
  ClangLayout L;
  clang::TypeInfo TI = AC.getTypeInfo(QT);
  L.Size = TI.Width / 8;
  L.Align = TI.Align / 8;
  if (const auto *RT = QT->getAsCanonical<clang::RecordType>()) {
    const clang::RecordDecl *RD = RT->getDecl()->getDefinitionOrSelf();
    const clang::ASTRecordLayout &RL = AC.getASTRecordLayout(RD);
    for (unsigned F = 0; F < RL.getFieldCount(); ++F)
      L.Offsets.push_back(RL.getFieldOffset(F) / 8);
  }
  return L;
}

std::string Classifier::dataLayout() { return I->W->DataLayout; }

ClassifierStats Classifier::stats() {
  std::lock_guard<std::mutex> G(I->W->Mu);
  return I->Stats;
}

//===----------------------------------------------------------------------===//
// Scalars without Clang (M3)
//===----------------------------------------------------------------------===//

std::optional<CallInfo> cabi::classifyScalars(llvm::StringRef Triple,
                                              const gir::Type *Sig) {
  llvm::Triple T(Triple);
  bool Amd64 = T.isOSLinux() && T.getArch() == llvm::Triple::x86_64;
  bool Arm64 = T.isOSLinux() && T.getArch() == llvm::Triple::aarch64;
  if (!Amd64 && !Arm64)
    return std::nullopt;
  // What Clang's arrangement gives a scalar: its own IR type as the coerce
  // type, Direct (flattenable, as getDirect() makes it), or on amd64 Extend
  // for a bool and an integer narrower than 32 bits (X86_64ABIInfo); AAPCS64
  // on Linux leaves the extension to the callee (AArch64ABIInfo extends
  // only for Apple's arm64).
  auto One = [&](const gir::Type *Ty, ArgInfo &A) {
    A.Ty = Ty;
    LTy L;
    switch (Ty->getKind()) {
    case gir::Type::Kind::Unit:
      A.K = ArgInfo::Kind::Ignore; // a void result
      return true;
    case gir::Type::Kind::Bool:
      L.K = LTy::Int;
      L.Bits = 1;
      break;
    case gir::Type::Kind::Int:
      L.K = LTy::Int;
      L.Bits = Ty->bits();
      break;
    case gir::Type::Kind::Float:
      L.K = Ty->bits() == 32 ? LTy::Float : LTy::Double;
      break;
    case gir::Type::Kind::Ptr:
      L.K = LTy::Ptr;
      break;
    default:
      return false; // an aggregate: Clang's
    }
    A.HasCoerce = true;
    A.Coerce = L;
    if (Amd64 && (Ty->isBool() || (Ty->isInt() && Ty->bits() < 32))) {
      A.K = ArgInfo::Kind::Extend;
      (Ty->isInt() && Ty->isSigned() ? A.SExt : A.ZExt) = true;
    } else {
      A.K = ArgInfo::Kind::Direct;
      A.Flatten = true;
    }
    return true;
  };
  CallInfo CI;
  if (!One(Sig->ret(), CI.Ret))
    return std::nullopt;
  for (const gir::Type *P : Sig->elems()) {
    CI.Args.emplace_back();
    if (!One(P, CI.Args.back()))
      return std::nullopt;
  }
  return CI;
}
