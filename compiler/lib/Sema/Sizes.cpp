//===- Sizes.cpp - the gc size model ----------------------------*- C++ -*-===//
//
// A port of types2's gcsizes.go for amd64 (the sizes the types2 tests use)
// and of hasVarSize (builtins.go). Go++ adds Option[T] (ADR-0018).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/DenseMap.h"

#include <functional>

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
  case Type::Kind::Ref:
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
  case Type::Kind::Ref: {
    // ADR-0020 point 7: a view of a string or a slice (named or not, and the
    // borrowed []T of slicing) is (ptr, len); every other view is one
    // pointer.
    Type *EU = under(cast<RefType>(U)->Elem);
    bool PtrLen =
        isa<SliceType>(EU) ||
        (isa<BasicType>(EU) && cast<BasicType>(EU)->BK == BasicKind::String);
    return PtrLen ? WordSize * 2 : WordSize;
  }
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
  // records the results of the types seen before. An instance is answered
  // from its generic declaration (varParams: the type parameters it holds by
  // value) and its arguments, never expanded: an instance that holds
  // another instance of its own generic type by value (`type N0[P any]
  // [1][0]N0[[0]P]`) grows without end, and the declaration check may ask
  // before any cycle check has run (review R23).
  llvm::DenseMap<NamedType *, bool> Local;
  NamedType *Named = dyn_cast<NamedType>(T);
  if (Named && Named->Orig) {
    uint64_t Mask = varParams(Named->Orig);
    if (Mask >> 63)
      return true;
    for (unsigned J = 0; J < Named->TArgs.size() && J < 63; ++J)
      if ((Mask & (uint64_t(1) << J)) && hasVarSize(Named->TArgs[J], Seen))
        return true;
    return false;
  }
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

/// The type parameters of generic type O (bit per index below 63) that its
/// right-hand side holds by value, through arrays, structs, Options and the
/// parameters other generic types hold; bit 63 means "of variable size
/// whatever the arguments": a parameter at index 63 or more, a type
/// parameter of something else, or a generic type that holds an instance of
/// itself by value, directly or through others (`type T[P any] struct{
/// T[P] }`): a cycle counts as variable size, as in Go's hasVarSize. Each
/// declaration is walked once per query, with no expansion.
uint64_t Checker::varParams(NamedType *O) {
  constexpr uint64_t Always = uint64_t(1) << 63;
  llvm::DenseMap<NamedType *, uint64_t> Done;
  llvm::SmallPtrSet<NamedType *, 8> Busy;
  std::function<uint64_t(NamedType *)> Decl = [&](NamedType *G) -> uint64_t {
    if (auto It = Done.find(G); It != Done.end())
      return It->second;
    if (!Busy.insert(G).second)
      return Always; // G holds an instance of itself by value
    uint64_t M = 0;
    llvm::SmallPtrSet<const Type *, 16> Met;
    SmallVector<Type *, 16> W{G->Underlying};
    while (!W.empty() && !(M & Always)) {
      Type *X = W.pop_back_val();
      if (!X || !isValid(X) || !Met.insert(X).second)
        continue;
      switch (X->getKind()) {
      case Type::Kind::TypeParam: {
        auto *P = cast<TypeParamType>(X);
        bool Own = P->Index < 63 && P->Index < G->TParams.size() &&
                   G->TParams[P->Index] == P;
        M |= Own ? uint64_t(1) << P->Index : Always;
        break;
      }
      case Type::Kind::Array:
        W.push_back(cast<ArrayType>(X)->Elem);
        break;
      case Type::Kind::Option:
        W.push_back(cast<OptionType>(X)->Elem);
        break;
      case Type::Kind::Struct:
        for (sema::Var *F : cast<StructType>(X)->Fields)
          W.push_back(F->Ty);
        break;
      case Type::Kind::Named: {
        auto *N = cast<NamedType>(X);
        if (!N->Orig) {
          if (N->TParams.empty())
            M |= hasVarSize(N) ? Always : 0;
          break;
        }
        uint64_t Sub = Decl(N->Orig);
        if (Sub & Always) {
          M |= Always;
          break;
        }
        for (unsigned J = 0; J < N->TArgs.size() && J < 63; ++J)
          if (Sub & (uint64_t(1) << J))
            W.push_back(N->TArgs[J]);
        break;
      }
      default:
        break;
      }
    }
    Busy.erase(G);
    Done[G] = M;
    return M;
  };
  return Decl(O);
}
