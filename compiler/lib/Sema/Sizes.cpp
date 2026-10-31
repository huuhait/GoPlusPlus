//===- Sizes.cpp - the gc size model ----------------------------*- C++ -*-===//
//
// A port of types2's gcsizes.go for amd64 (the sizes the types2 tests use)
// and of hasVarSize (builtins.go). Go++ adds Option[T] (ADR-0018).
//
// Go++ code (not CheckOptions::GoCompat) follows the layout of D4.5 and of
// the code generator (LLVM's data layout for the GIR types): `string` is
// {ptr, len, cap} = 24 bytes (ADR-0026), an owned `[]T` is {ptr, len, cap,
// allocator} = 32 (ADR-0029), a func value is {code, env} = 16, and
// a struct whose last field has size 0 gets no padding byte (gc adds one).
// P04.04 tests the two agree for a generated table of types, on both targets.
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

void Checker::gcOffsetsof(const StructType *S, SmallVectorImpl<int64_t> &Offs) {
  int64_t Off = 0;
  for (Var *F : S->Fields) {
    if (Off < 0) { // all remaining offsets are too large
      Offs.push_back(-1);
      continue;
    }
    Layout L = gcLayout(F->Ty);
    Off = alignTo(Off, L.Align);
    Offs.push_back(Off);
    if (L.Size >= 0 && Off >= 0)
      Off += L.Size; // ok to overflow to < 0
    else
      Off = -1; // F->Ty or Off is too large
  }
}

/// The size and alignment of a type that has no parts to walk (a basic type,
/// a slice, an interface, a view, anything that is one word): gcsizes.go.
static Checker::Layout leafLayout(Type *U, bool Complex, bool Go) {
  switch (U->getKind()) {
  case Type::Kind::Slice:
    // ADR-0029: {ptr, len, cap, allocator} for Go++ (gc: three words).
    return {Go ? WordSize * 3 : WordSize * 4, WordSize};
  case Type::Kind::Interface:
    return {WordSize * 2, WordSize};
  case Type::Kind::Signature:
    // D4.5: a func value is {code*, env*}. (gc: one word.)
    return {Go ? WordSize : WordSize * 2, WordSize};
  case Type::Kind::Ref: {
    // ADR-0020 point 7: a view of a string or a slice (named or not, and the
    // borrowed []T of slicing) is (ptr, len); every other view is one
    // pointer.
    Type *EU = under(cast<RefType>(U)->Elem);
    bool PtrLen =
        isa<SliceType>(EU) ||
        (isa<BasicType>(EU) && cast<BasicType>(EU)->BK == BasicKind::String);
    return {PtrLen ? WordSize * 2 : WordSize, WordSize};
  }
  case Type::Kind::Basic: {
    int64_t Size;
    switch (cast<BasicType>(U)->BK) {
    case BasicKind::Bool:
    case BasicKind::Int8:
    case BasicKind::Uint8:
      Size = 1;
      break;
    case BasicKind::Int16:
    case BasicKind::Uint16:
      Size = 2;
      break;
    case BasicKind::Int32:
    case BasicKind::Uint32:
    case BasicKind::Float32:
      Size = 4;
      break;
    case BasicKind::Int64:
    case BasicKind::Uint64:
    case BasicKind::Float64:
    case BasicKind::Complex64:
      Size = 8;
      break;
    case BasicKind::Complex128:
      Size = 16;
      break;
    case BasicKind::String:
      return {Go ? 16 : 24, WordSize}; // D4.5: {ptr, len, cap}
    default:
      Size = WordSize;
      break;
    }
    // complex{64,128} are aligned like [2]float{32,64}.
    return {Size, std::min(Complex ? Size / 2 : Size, MaxAlign)};
  }
  default: // catch-all
    return {WordSize, WordSize};
  }
}

Checker::Layout Checker::gcLayout(Type *Root) {
  // A named type (or instance) is a node of the walk, memoized on the type
  // when the answer is exact and in Local for the rest of this query. A
  // frame is a type with parts: an array, a struct, an Option or a named
  // type (its one part is its underlying type); the parts are laid out
  // first, then the frame combines them. A named type met again while it is
  // on the stack is a by-value cycle (an invalid type, reported by its
  // declaration): it counts one word, and nothing the cut touched is stored.
  // So is an instance of a generic type that holds an instance of itself by
  // value: its instances grow without end, so none is expanded. Neither is
  // asked for by unsafe.Sizeof and its kin (hasVarSize is true for both), so
  // the word is only a fallback; the walk of any other type is exact however
  // deep it goes.
  struct Frame {
    Type *T;           // the type
    NamedType *N;      // named: the node, else null
    Type *U;           // the type with parts (T, or under(T) for a named)
    unsigned FirstKid; // index of the first part in Kids
    size_t NumKids;    // number of parts
    size_t Done;       // parts laid out so far
    unsigned CutsAtStart;
  };
  // Local: a type laid out in this query, and whether a cut was below it
  // (its answer then depends on where the walk entered the cycle, so a type
  // above it is not stored either).
  llvm::DenseMap<NamedType *, std::pair<Layout, bool>> Local;
  llvm::DenseMap<NamedType *, bool> Grow;
  llvm::SmallPtrSet<NamedType *, 16> Busy;
  SmallVector<Frame, 32> St;
  // The parts of each frame, as one flat list (a frame's parts are the last
  // NumKids entries of Parts while it is on top) and their layouts.
  SmallVector<Type *, 64> Parts;
  SmallVector<Layout, 64> Lays;
  unsigned Cuts = 0;
  bool Exact = true; // every named type met so far is declared

  auto Grows = [&](NamedType *Inst) {
    NamedType *O = Inst->origin();
    if (auto It = Grow.find(O); It != Grow.end())
      return It->second;
    uint8_t St = O->GrowState.load(std::memory_order_acquire);
    bool G;
    if (St)
      G = St == 2;
    else {
      G = (varParams(O) >> 63) != 0;
      if (typesFinal())
        O->GrowState.store(G ? 2 : 1, std::memory_order_release);
    }
    return Grow[O] = G;
  };

  // Lays T out now if it needs no walk (true, with R), else pushes it.
  auto Visit = [&](Type *T, Layout &R) -> bool {
    NamedType *N = dyn_cast<NamedType>(T);
    Type *U = T;
    if (N) {
      if (N->LayoutState.load(std::memory_order_acquire) == 1) {
        R = {N->LayoutSize.load(std::memory_order_relaxed),
             N->LayoutAlign.load(std::memory_order_relaxed)};
        return true;
      }
      if (auto It = Local.find(N); It != Local.end()) {
        R = It->second.first;
        if (It->second.second)
          ++Cuts;
        return true;
      }
      if (Busy.count(N) || (N->Orig && Grows(N))) {
        ++Cuts;
        R = {WordSize, WordSize};
        return true;
      }
      if (!declFinished(N))
        Exact = false;
      U = under(T);
    }
    switch (U->getKind()) {
    case Type::Kind::Array:
    case Type::Kind::Option:
    case Type::Kind::Struct:
      break;
    default: {
      R = leafLayout(U, isComplex(T), S.Opts.GoCompat);
      if (N) // a named leaf (cheap, but memoize the ones in Local)
        Local[N] = {R, false};
      return true;
    }
    }
    Frame F{T, N, U, static_cast<unsigned>(Parts.size()), 0, 0, Cuts};
    switch (U->getKind()) {
    case Type::Kind::Array:
      Parts.push_back(cast<sema::ArrayType>(U)->Elem);
      break;
    case Type::Kind::Option:
      Parts.push_back(cast<OptionType>(U)->Elem);
      break;
    default:
      for (Var *Fld : cast<StructType>(U)->Fields)
        Parts.push_back(Fld->Ty);
      break;
    }
    F.NumKids = Parts.size() - F.FirstKid;
    if (N)
      Busy.insert(N);
    St.push_back(F);
    return false;
  };

  Layout Result;
  if (Visit(Root, Result))
    return Result;
  while (true) {
    Frame &F = St.back();
    if (F.Done < F.NumKids) {
      Layout R;
      if (Visit(Parts[F.FirstKid + F.Done], R)) {
        Lays.push_back(R);
        ++St.back().Done;
      }
      continue; // (a pushed frame lays out its parts, then delivers here)
    }
    // All parts are laid out: combine.
    const Layout *Kids = Lays.data() + (Lays.size() - F.NumKids);
    Layout R;
    switch (F.U->getKind()) {
    case Type::Kind::Array: {
      auto *A = cast<sema::ArrayType>(F.U);
      int64_t N = A->Len, ES = Kids[0].Size;
      R.Align = Kids[0].Align;
      if (N <= 0)
        R.Size = 0;
      else if (ES < 0)
        R.Size = -1; // element too large
      else if (ES == 0)
        R.Size = 0; // 0-size element
      else if (ES > MaxInt64 / N)
        R.Size = -1; // ES * N overflows
      else
        R.Size = ES * N;
      break;
    }
    case Type::Kind::Option: {
      // ADR-0018: a pointer kind's nil is its empty bit pattern; any other T
      // gets a hidden "is set" flag.
      auto *O = cast<OptionType>(F.U);
      R.Align = Kids[0].Align;
      if (Kids[0].Size < 0)
        R.Size = -1;
      else if (O->usesNiche() || (!S.Opts.GoCompat && neverNilKind(O->Elem)))
        // (D4.5 and ADR-0018 point 3: also an interface with methods and a
        // func type: the null itab / code pointer is nil.)
        R.Size = Kids[0].Size;
      else
        R.Size = alignTo(Kids[0].Size + 1, Kids[0].Align);
      break;
    }
    default: { // a struct
      if (F.NumKids == 0) {
        R = {0, 1};
        break;
      }
      int64_t Align = 1;
      for (size_t K = 0; K < F.NumKids; ++K)
        Align = std::max(Align, Kids[K].Align);
      // gcOffsetsof, on the layouts of the fields.
      int64_t Off = 0, Last = 0;
      for (size_t K = 0; K < F.NumKids; ++K) {
        if (Off < 0) { // all remaining offsets are too large
          Last = -1;
          continue;
        }
        Off = alignTo(Off, Kids[K].Align);
        Last = Off;
        int64_t D = Kids[K].Size;
        if (D >= 0 && Off >= 0)
          Off += D; // ok to overflow to < 0
        else
          Off = -1; // the field or Off is too large
      }
      int64_t Size = Kids[F.NumKids - 1].Size;
      if (Last < 0 || Size < 0) {
        R = {-1, Align}; // type too large
        break;
      }
      // gc: the last field of a non-zero-sized struct is not allowed to have
      // size 0.
      if (Last > 0 && Size == 0 && S.Opts.GoCompat)
        Size = 1;
      // gc: the size includes alignment padding.
      R = {alignTo(Last + Size, Align), Align};
      break;
    }
    }
    NamedType *N = F.N;
    bool Own = F.CutsAtStart == Cuts;
    Lays.resize(Lays.size() - F.NumKids);
    Parts.resize(F.FirstKid);
    St.pop_back();
    if (N) {
      Busy.erase(N);
      Local[N] = {R, !Own};
      if (Own && Exact && declFinished(N)) {
        N->LayoutSize.store(R.Size, std::memory_order_relaxed);
        N->LayoutAlign.store(R.Align, std::memory_order_relaxed);
        N->LayoutState.store(1, std::memory_order_release);
      }
    }
    if (St.empty())
      return R;
    Lays.push_back(R);
    ++St.back().Done;
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

bool Checker::hasVarSize(Type *Root, llvm::DenseMap<NamedType *, bool> *Seen) {
  // Cycles are only possible through named types: Seen detects them and
  // records the results of the types seen before. An instance is answered
  // from its generic declaration (varParams: the type parameters it holds by
  // value) and its arguments, never expanded: an instance that holds
  // another instance of its own generic type by value (`type N0[P any]
  // [1][0]N0[[0]P]`) grows without end, and the declaration check may ask
  // before any cycle check has run (review R23). Walked with an explicit
  // stack: the parts of a type may be nested to any depth.
  llvm::DenseMap<NamedType *, bool> Local;
  if (!Seen)
    Seen = &Local;
  struct Frame {
    Type *T;
    NamedType *Named; // a declared (not instantiated) named type, else null
    SmallVector<Type *, 4> Kids;
    size_t Next = 0;
  };
  SmallVector<Frame, 16> St;
  // Starts the walk of T: true if its answer is known now (in Known), else a
  // frame was pushed.
  auto Start = [&](Type *T, bool &Known) -> bool {
    Known = false;
    NamedType *Named = dyn_cast<NamedType>(T);
    Frame F{T, nullptr, {}, 0};
    if (Named && Named->Orig) {
      uint64_t Mask = varParams(Named->Orig);
      if (Mask >> 63) {
        Known = true;
        return true;
      }
      for (unsigned J = 0; J < Named->TArgs.size() && J < 63; ++J)
        if (Mask & (uint64_t(1) << J))
          F.Kids.push_back(Named->TArgs[J]);
      St.push_back(std::move(F));
      return false;
    }
    if (Named) {
      auto It = Seen->find(Named);
      if (It != Seen->end()) {
        Known = It->second;
        return true;
      }
      (*Seen)[Named] = true; // possibly cyclic until proven otherwise
      F.Named = Named;
    }
    Type *U = under(T);
    switch (U->getKind()) {
    case Type::Kind::Array:
      F.Kids.push_back(cast<sema::ArrayType>(U)->Elem);
      break;
    case Type::Kind::Struct:
      for (sema::Var *Fld : cast<StructType>(U)->Fields)
        F.Kids.push_back(Fld->Ty);
      break;
    case Type::Kind::Interface:
      if (isTypeParam(T)) {
        Known = true;
        if (Named)
          (*Seen)[Named] = true;
        return true;
      }
      break;
    case Type::Kind::Option: // Go++: Option[T] has the size of T (and a flag)
      F.Kids.push_back(cast<OptionType>(U)->Elem);
      break;
    default:
      break;
    }
    if (F.Kids.empty()) {
      if (Named)
        (*Seen)[Named] = false;
      return true; // Known stays false: no variable size
    }
    St.push_back(std::move(F));
    return false;
  };
  bool Known;
  if (Start(Root, Known))
    return Known;
  while (true) {
    Frame &F = St.back();
    if (F.Next < F.Kids.size()) {
      Type *K = F.Kids[F.Next++];
      bool KnownK;
      if (Start(K, KnownK) && KnownK) {
        // One part of variable size makes every type above it so.
        for (Frame &A : St)
          if (A.Named)
            (*Seen)[A.Named] = true;
        return true;
      }
      continue;
    }
    // No part is of variable size.
    if (F.Named)
      (*Seen)[F.Named] = false;
    St.pop_back();
    if (St.empty())
      return false;
  }
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
