//===- Sizes.cpp - the gc size model ----------------------------*- C++ -*-===//
//
// A port of types2's gcsizes.go for amd64 (the sizes the types2 tests use)
// and of hasVarSize (builtins.go). Go++ adds Option[T] (ADR-0018).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/DenseMap.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::SmallVector;
using llvm::SmallVectorImpl;

namespace {
constexpr int64_t WordSize = 8, MaxAlign = 8;
constexpr int64_t MaxInt64 = INT64_MAX;

int64_t alignTo(int64_t X, int64_t A) { return (X + A - 1) & ~(A - 1); }
} // namespace

int64_t Checker::gcAlignof(Type *T) {
  DepthGuard DG(RecDepth);
  if (RecDepth > MaxRecDepth)
    return 1;
  Type *U = under(T);
  switch (U->getKind()) {
  case Type::Kind::Array:
    return gcAlignof(cast<ArrayType>(U)->Elem);
  case Type::Kind::Struct: {
    int64_t Max = 1;
    for (Var *F : cast<StructType>(U)->Fields)
      Max = std::max(Max, gcAlignof(F->Ty));
    return Max;
  }
  case Type::Kind::Slice:
  case Type::Kind::Interface:
    return WordSize;
  case Type::Kind::Basic:
    if (cast<BasicType>(U)->BK == BasicKind::String)
      return WordSize;
    break;
  case Type::Kind::Option:
    return gcAlignof(cast<OptionType>(U)->Elem);
  default:
    break;
  }
  int64_t A = gcSizeof(T); // may be 0 or negative
  if (A < 1)
    return 1;
  // complex{64,128} are aligned like [2]float{32,64}.
  if (isComplex(T))
    A /= 2;
  return std::min(A, MaxAlign);
}

void Checker::gcOffsetsof(const StructType *S, SmallVectorImpl<int64_t> &Offs) {
  int64_t Off = 0;
  for (Var *F : S->Fields) {
    if (Off < 0) { // all remaining offsets are too large
      Offs.push_back(-1);
      continue;
    }
    Off = alignTo(Off, gcAlignof(F->Ty));
    Offs.push_back(Off);
    int64_t D = gcSizeof(F->Ty);
    if (D >= 0 && Off >= 0)
      Off += D; // ok to overflow to < 0
    else
      Off = -1; // F->Ty or Off is too large
  }
}

int64_t Checker::gcSizeof(Type *T) {
  DepthGuard DG(RecDepth);
  if (RecDepth > MaxRecDepth)
    return WordSize;
  Type *U = under(T);
  switch (U->getKind()) {
  case Type::Kind::Basic:
    switch (cast<BasicType>(U)->BK) {
    case BasicKind::Bool:
    case BasicKind::Int8:
    case BasicKind::Uint8:
      return 1;
    case BasicKind::Int16:
    case BasicKind::Uint16:
      return 2;
    case BasicKind::Int32:
    case BasicKind::Uint32:
    case BasicKind::Float32:
      return 4;
    case BasicKind::Int64:
    case BasicKind::Uint64:
    case BasicKind::Float64:
    case BasicKind::Complex64:
      return 8;
    case BasicKind::Complex128:
    case BasicKind::String:
      return 16;
    default:
      return WordSize;
    }
  case Type::Kind::Array: {
    auto *A = cast<ArrayType>(U);
    int64_t N = A->Len;
    if (N <= 0)
      return 0;
    int64_t ES = gcSizeof(A->Elem);
    if (ES < 0)
      return -1; // element too large
    if (ES == 0)
      return 0; // 0-size element
    if (ES > MaxInt64 / N)
      return -1; // ES * N overflows
    return ES * N;
  }
  case Type::Kind::Slice:
    return WordSize * 3;
  case Type::Kind::Struct: {
    auto *St = cast<StructType>(U);
    if (St->Fields.empty())
      return 0;
    SmallVector<int64_t, 8> Offs;
    gcOffsetsof(St, Offs);
    int64_t Off = Offs.back();
    int64_t Size = gcSizeof(St->Fields.back()->Ty);
    if (Off < 0 || Size < 0)
      return -1; // type too large
    // gc: the last field of a non-zero-sized struct is not allowed to have
    // size 0.
    if (Off > 0 && Size == 0)
      Size = 1;
    // gc: the size includes alignment padding.
    return alignTo(Off + Size, gcAlignof(T));
  }
  case Type::Kind::Interface:
    return WordSize * 2;
  case Type::Kind::Option: {
    // ADR-0018: a pointer kind's nil is its empty bit pattern; any other T
    // gets a hidden "is set" flag.
    auto *O = cast<OptionType>(U);
    int64_t ES = gcSizeof(O->Elem);
    if (ES < 0)
      return -1;
    if (O->usesNiche())
      return ES;
    return alignTo(ES + 1, gcAlignof(O->Elem));
  }
  default:
    return WordSize; // catch-all
  }
}

int64_t Checker::gcOffsetof(Type *T, ArrayRef<int> Index) {
  int64_t Off = 0;
  for (int I : Index) {
    auto *S = dyn_cast<StructType>(under(T));
    if (!S)
      return -1;
    SmallVector<int64_t, 8> Offs;
    gcOffsetsof(S, Offs);
    if (Offs[I] < 0)
      return -1;
    Off += Offs[I];
    if (Off < 0)
      return -1;
    T = S->Fields[I]->Ty;
  }
  return Off;
}

unsigned long long Checker::sizeofType(Type *T) {
  int64_t S = gcSizeof(T);
  return S < 0 ? WordSize : static_cast<unsigned long long>(S);
}

bool Checker::hasVarSize(Type *T, llvm::DenseMap<NamedType *, bool> *Seen) {
  // Cycles are only possible through named types: Seen detects them and
  // records the results of the types seen before.
  llvm::DenseMap<NamedType *, bool> Local;
  NamedType *Named = dyn_cast<NamedType>(T);
  if (Named) {
    if (!Seen)
      Seen = &Local;
    auto It = Seen->find(Named);
    if (It != Seen->end())
      return It->second;
    (*Seen)[Named] = true; // possibly cyclic until proven otherwise
  }
  bool Var = false;
  Type *U = under(T);
  switch (U->getKind()) {
  case Type::Kind::Array:
    Var = hasVarSize(cast<ArrayType>(U)->Elem, Seen);
    break;
  case Type::Kind::Struct:
    for (sema::Var *F : cast<StructType>(U)->Fields)
      if (hasVarSize(F->Ty, Seen)) {
        Var = true;
        break;
      }
    break;
  case Type::Kind::Interface:
    Var = isTypeParam(T);
    break;
  case Type::Kind::Option: // Go++: Option[T] has the size of T (and a flag)
    Var = hasVarSize(cast<OptionType>(U)->Elem, Seen);
    break;
  default:
    break;
  }
  if (Named)
    (*Seen)[Named] = Var;
  return Var;
}
