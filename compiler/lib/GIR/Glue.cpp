//===- Glue.cpp - Equality and hash glue ----------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Glue.h"

#include "gopp/GIR/Builder.h"

#include <cmath>
#include <cstring>
#include <map>

using namespace gopp;
using namespace gopp::gir;

namespace {

/// An array of at most this many elements is compared element by element
/// without a loop.
constexpr uint64_t UnrollLimit = 4;

std::unique_ptr<Function> glueFunction(llvm::StringRef Symbol, Type *Sig) {
  auto F = std::make_unique<Function>();
  F->Name = Symbol.str();
  F->Sig = Sig;
  F->Link = Linkage::LinkOnceODR;
  F->CC = CallConv::Go;
  return F;
}

/// A level of nesting deeper than this inside one glue function is compared
/// (hashed) by a function of its own: a chain of named types would otherwise
/// make one function with a place thousands of projections long (quadratic in
/// the chain) and recurse once per link (P04.04, review R6).
constexpr unsigned InlineDepth = 24;

/// The functions a request made for the parts of its type that are nested too
/// deep, and the bodies still to build (a list, not a recursion: the bodies
/// are built one after the other).
struct Parts {
  struct Item {
    Type *T;
    FuncId Id;
    bool Hash;
  };
  std::string Base;
  unsigned Next = 0;
  std::vector<Item> Work;
  std::map<std::pair<Type *, bool>, FuncId> Made;
};

class Glue {
public:
  Glue(Module &M, Function &F, Parts &Ps) : FB(M, F), C(M.Ctx), Ps(Ps) {}

  /// Emits code that jumps to Fail unless A == B; the current block goes on
  /// when they are equal.
  void eq(const Place &A, const Place &B, BlockId Fail) {
    ++Inline;
    if (Inline > InlineDepth && nested(A.Ty))
      callPart(A, B, Fail);
    else
      eqOne(A, B, Fail);
    --Inline;
  }

  /// Mixes the value in P into the accumulator H (a u64 local).
  void hash(const Place &P, LocalId H) {
    ++Inline;
    if (Inline > InlineDepth && nested(P.Ty))
      callPart(P, P, NoId, H);
    else
      hashOne(P, H);
    --Inline;
  }

  FuncBuilder FB;

private:
  Context &C;
  /// The levels of nesting inside this function: eq and hash recurse at most
  /// InlineDepth deep, a deeper part is a call (callPart).
  Parts &Ps;
  unsigned Inline = 0;

  /// Does T hold other types (it may be cut out into a function of its own)?
  static bool nested(const Type *T) {
    switch (T->getKind()) {
    case Type::Kind::Struct:
    case Type::Kind::Tuple:
    case Type::Kind::Array:
    case Type::Kind::Option:
      return true;
    default:
      return false;
    }
  }

  /// The function that compares (hashes) values of T, made on first use; its
  /// body is built from Ps.Work.
  FuncId part(Type *T, bool Hash) {
    auto [It, New] = Ps.Made.try_emplace({T, Hash}, NoId);
    if (!New)
      return It->second;
    Type *P = C.ptr(T);
    auto Fn = glueFunction(Ps.Base + ".part" + std::to_string(Ps.Next++),
                           Hash ? C.func({P, C.u64()}, C.u64())
                                : C.func({P, P}, C.boolean()));
    Fn->Link = Linkage::Internal;
    It->second = FB.module().addFunction(std::move(Fn));
    Ps.Work.push_back({T, It->second, Hash});
    return It->second;
  }

  /// eq: jumps to Fail unless the part is equal; hash: H = hash(part, H).
  void callPart(const Place &A, const Place &B, BlockId Fail,
                LocalId H = NoId) {
    bool Hash = H != NoId;
    FuncId Id = part(A.Ty, Hash);
    Place PA = FB.temp(FB.addrOf(A));
    if (Hash) {
      FB.call(constFunc(FB.module(), Id), {copyOf(PA), copyOf(FB.local(H))},
              FB.local(H));
      return;
    }
    Place PB = FB.temp(FB.addrOf(B));
    Place Same = FB.local(FB.newTemp(C.boolean()));
    FB.call(constFunc(FB.module(), Id), {copyOf(PA), copyOf(PB)}, Same);
    BlockId Cont = FB.newBlock();
    FB.branch(copyOf(Same), Cont, Fail);
    FB.setBlock(Cont);
  }

  void eqOne(const Place &A, const Place &B, BlockId Fail) {
    Type *T = A.Ty;
    switch (T->getKind()) {
    case Type::Kind::Unit:
      return;
    case Type::Kind::Bool:
    case Type::Kind::Int:
    case Type::Kind::Float:
    case Type::Kind::Ptr: {
      Place Same = FB.temp(FB.binary(BinOp::Eq, copyOf(A), copyOf(B)));
      BlockId Next = FB.newBlock();
      FB.branch(copyOf(Same), Next, Fail);
      FB.setBlock(Next);
      return;
    }
    case Type::Kind::Struct:
    case Type::Kind::Tuple:
      for (unsigned I = 0; I < T->numFields(); ++I)
        eq(FB.field(A, I), FB.field(B, I), Fail);
      return;
    case Type::Kind::Option: {
      // Both flags clear, or both set and the payloads equal.
      Place FA = FB.field(A, 1), FBl = FB.field(B, 1);
      Place Same = FB.temp(FB.binary(BinOp::Eq, copyOf(FA), copyOf(FBl)));
      BlockId Cmp = FB.newBlock(), Join = FB.newBlock(), Flags = FB.newBlock();
      FB.branch(copyOf(Same), Flags, Fail);
      FB.setBlock(Flags);
      FB.branch(copyOf(FA), Cmp, Join);
      FB.setBlock(Cmp);
      eq(FB.field(A, 0), FB.field(B, 0), Fail);
      FB.gotoBlock(Join);
      FB.setBlock(Join);
      return;
    }
    case Type::Kind::Array:
      arrayEq(T->length(), A, B, Fail);
      return;
    case Type::Kind::String:
      stringEq(A, B, Fail);
      return;
    case Type::Kind::Slice:
    case Type::Kind::Func:
      return; // hasGlue refused the type
    }
  }

  void hashOne(const Place &P, LocalId H) {
    Type *T = P.Ty;
    switch (T->getKind()) {
    case Type::Kind::Unit:
      return;
    case Type::Kind::Bool: {
      Place Byte = FB.deref(FB.temp(FB.cast(
          CastKind::PtrToPtr, copyOf(FB.temp(FB.addrOf(P))), C.ptr(C.u8()))));
      mix(H,
          copyOf(FB.temp(FB.cast(CastKind::IntToInt, copyOf(Byte), C.u64()))));
      return;
    }
    case Type::Kind::Int:
      mix(H, T == C.u64() ? copyOf(P)
                          : copyOf(FB.temp(FB.cast(CastKind::IntToInt,
                                                   copyOf(P), C.u64()))));
      return;
    case Type::Kind::Ptr:
      mix(H, copyOf(FB.temp(FB.cast(CastKind::PtrToInt, copyOf(P), C.u64()))));
      return;
    case Type::Kind::Float:
      hashFloat(P, H);
      return;
    case Type::Kind::Struct:
    case Type::Kind::Tuple:
      for (unsigned I = 0; I < T->numFields(); ++I)
        hash(FB.field(P, I), H);
      return;
    case Type::Kind::Option: {
      hash(FB.field(P, 1), H);
      BlockId Set = FB.newBlock(), Join = FB.newBlock();
      FB.branch(copyOf(FB.field(P, 1)), Set, Join);
      FB.setBlock(Set);
      hash(FB.field(P, 0), H);
      FB.gotoBlock(Join);
      FB.setBlock(Join);
      return;
    }
    case Type::Kind::Array: {
      uint64_t N = T->length();
      if (N <= UnrollLimit) {
        for (uint64_t I = 0; I < N; ++I)
          hash(FB.constIndex(P, I), H);
        return;
      }
      LocalId Idx = FB.newLocal(C.u64(), Local::Temp);
      FB.assign(FB.local(Idx), FB.use(constInt(C.u64(), 0)));
      BlockId Head = FB.newBlock(), Body = FB.newBlock(), Exit = FB.newBlock();
      FB.gotoBlock(Head);
      FB.setBlock(Head);
      Place More = FB.temp(
          FB.binary(BinOp::Lt, copyOf(FB.local(Idx)), constInt(C.u64(), N)));
      FB.branch(copyOf(More), Body, Exit);
      FB.setBlock(Body);
      hash(FB.index(P, Idx), H);
      FB.assign(FB.local(Idx), FB.binary(BinOp::Add, copyOf(FB.local(Idx)),
                                         constInt(C.u64(), 1)));
      FB.gotoBlock(Head);
      FB.setBlock(Exit);
      return;
    }
    case Type::Kind::String:
      stringHash(P, H);
      return;
    case Type::Kind::Slice:
    case Type::Kind::Func:
      return;
    }
  }

  /// The loop `for i < len`: Body(i) runs with the index local; the caller
  /// continues in the block after the loop.
  template <typename F> void countLoop(Operand Len, F Body) {
    LocalId Idx = FB.newLocal(C.u64(), Local::Temp);
    FB.assign(FB.local(Idx), FB.use(constInt(C.u64(), 0)));
    BlockId Head = FB.newBlock(), Bd = FB.newBlock(), Exit = FB.newBlock();
    FB.gotoBlock(Head);
    FB.setBlock(Head);
    Place More =
        FB.temp(FB.binary(BinOp::Lt, copyOf(FB.local(Idx)), std::move(Len)));
    FB.branch(copyOf(More), Bd, Exit);
    FB.setBlock(Bd);
    Body(Idx);
    FB.assign(FB.local(Idx), FB.binary(BinOp::Add, copyOf(FB.local(Idx)),
                                       constInt(C.u64(), 1)));
    FB.gotoBlock(Head);
    FB.setBlock(Exit);
  }

  /// Equal lengths, then equal bytes (Go's string ==).
  void stringEq(const Place &A, const Place &B, BlockId Fail) {
    Place SameLen = FB.temp(
        FB.binary(BinOp::Eq, copyOf(FB.field(A, 1)), copyOf(FB.field(B, 1))));
    BlockId Cmp = FB.newBlock();
    FB.branch(copyOf(SameLen), Cmp, Fail);
    FB.setBlock(Cmp);
    // A string is a copy of its bytes; the same pointer is the same bytes.
    countLoop(copyOf(FB.field(A, 1)), [&](LocalId Idx) {
      Place Same = FB.temp(FB.binary(BinOp::Eq, copyOf(FB.index(A, Idx)),
                                     copyOf(FB.index(B, Idx))));
      BlockId Next = FB.newBlock();
      FB.branch(copyOf(Same), Next, Fail);
      FB.setBlock(Next);
    });
  }

  /// The length, then every byte.
  void stringHash(const Place &P, LocalId H) {
    mix(H, copyOf(FB.field(P, 1)));
    countLoop(copyOf(FB.field(P, 1)), [&](LocalId Idx) {
      mix(H, copyOf(FB.temp(FB.cast(CastKind::IntToInt,
                                    copyOf(FB.index(P, Idx)), C.u64()))));
    });
  }

  void arrayEq(uint64_t N, const Place &A, const Place &B, BlockId Fail) {
    if (N <= UnrollLimit) {
      for (uint64_t I = 0; I < N; ++I)
        eq(FB.constIndex(A, I), FB.constIndex(B, I), Fail);
      return;
    }
    LocalId Idx = FB.newLocal(C.u64(), Local::Temp);
    FB.assign(FB.local(Idx), FB.use(constInt(C.u64(), 0)));
    BlockId Head = FB.newBlock(), Body = FB.newBlock(), Exit = FB.newBlock();
    FB.gotoBlock(Head);
    FB.setBlock(Head);
    Place More = FB.temp(
        FB.binary(BinOp::Lt, copyOf(FB.local(Idx)), constInt(C.u64(), N)));
    FB.branch(copyOf(More), Body, Exit);
    FB.setBlock(Body);
    eq(FB.index(A, Idx), FB.index(B, Idx), Fail);
    FB.assign(FB.local(Idx), FB.binary(BinOp::Add, copyOf(FB.local(Idx)),
                                       constInt(C.u64(), 1)));
    FB.gotoBlock(Head);
    FB.setBlock(Exit);
  }

  /// H = (H ^ V) * K; H ^= H >> 32.
  void mix(LocalId H, Operand V) {
    Type *U = C.u64();
    Place X = FB.temp(FB.binary(BinOp::Xor, copyOf(FB.local(H)), std::move(V)));
    Place Y = FB.temp(
        FB.binary(BinOp::Mul, copyOf(X), constInt(U, 0x9E3779B97F4A7C15ull)));
    Place Z = FB.temp(FB.binary(BinOp::Shr, copyOf(Y), constInt(U, 32)));
    FB.assign(FB.local(H), FB.binary(BinOp::Xor, copyOf(Y), copyOf(Z)));
  }

  /// +0 and -0 hash alike (they are ==); every NaN hashes the same.
  void hashFloat(const Place &P, LocalId H) {
    Type *T = P.Ty;
    Type *Bits = C.intTy(T->bits(), false);
    LocalId V = FB.newLocal(C.u64(), Local::Temp);
    BlockId Zero = FB.newBlock(), NotZero = FB.newBlock(), Nan = FB.newBlock(),
            Plain = FB.newBlock(), Join = FB.newBlock();
    Place IsZero = FB.temp(FB.binary(BinOp::Eq, copyOf(P), constFloat(T, 0.0)));
    FB.branch(copyOf(IsZero), Zero, NotZero);
    FB.setBlock(Zero);
    FB.assign(FB.local(V), FB.use(constInt(C.u64(), 0)));
    FB.gotoBlock(Join);
    FB.setBlock(NotZero);
    Place IsNan = FB.temp(FB.binary(BinOp::Ne, copyOf(P), copyOf(P)));
    FB.branch(copyOf(IsNan), Nan, Plain);
    FB.setBlock(Nan);
    FB.assign(FB.local(V), FB.use(constInt(C.u64(), 0x7ff8000000000001ull)));
    FB.gotoBlock(Join);
    FB.setBlock(Plain);
    Place Raw = FB.deref(FB.temp(FB.cast(
        CastKind::PtrToPtr, copyOf(FB.temp(FB.addrOf(P))), C.ptr(Bits))));
    FB.assign(FB.local(V), FB.cast(CastKind::IntToInt, copyOf(Raw), C.u64()));
    FB.gotoBlock(Join);
    FB.setBlock(Join);
    mix(H, copyOf(FB.local(V)));
  }
};

FuncId existing(Module &M, llvm::StringRef Symbol) {
  return M.lookupFunction(Symbol);
}

} // namespace

bool gir::hasGlue(const Type *T) {
  llvm::SmallVector<const Type *, 8> Work{T};
  while (!Work.empty()) {
    const Type *C = Work.pop_back_val();
    switch (C->getKind()) {
    case Type::Kind::Slice:
    case Type::Kind::Func:
      return false;
    case Type::Kind::Array:
    case Type::Kind::Option:
      Work.push_back(C->elem());
      break;
    case Type::Kind::Struct:
    case Type::Kind::Tuple:
      Work.append(C->elems().begin(), C->elems().end());
      break;
    default:
      break;
    }
  }
  return true;
}

/// The body of the function Id, which compares two values of T (no part
/// function of T's own is made here: the parts are queued in Ps).
static void buildEq(Module &M, FuncId Id, Parts &Ps) {
  Function *F = M.Funcs[Id].get();
  Glue G(M, *F, Ps);
  FuncBuilder &FB = G.FB;
  Context &C = M.Ctx;
  BlockId Fail = FB.newBlock();
  G.eq(FB.deref(FB.local(FB.param(0))), FB.deref(FB.local(FB.param(1))), Fail);
  FB.assign(FB.local(FB.returnPlace()), FB.use(constBool(C, true)));
  FB.ret();
  FB.setBlock(Fail);
  FB.assign(FB.local(FB.returnPlace()), FB.use(constBool(C, false)));
  FB.ret();
  // A type with nothing to compare (struct{}, [0]T) never fails (V022).
  removeDeadBlocks(*F);
}

static void buildHash(Module &M, FuncId Id, Parts &Ps) {
  Function *F = M.Funcs[Id].get();
  Glue G(M, *F, Ps);
  FuncBuilder &FB = G.FB;
  Context &C = M.Ctx;
  LocalId H = FB.newLocal(C.u64(), Local::Var, "h");
  FB.assign(FB.local(H), FB.use(copyOf(FB.local(FB.param(1)))));
  G.hash(FB.deref(FB.local(FB.param(0))), H);
  FB.assign(FB.local(FB.returnPlace()), FB.use(copyOf(FB.local(H))));
  FB.ret();
}

/// Builds the bodies of the part functions of a request, one after the other
/// (a part may queue more).
static void buildParts(Module &M, Parts &Ps) {
  while (!Ps.Work.empty()) {
    Parts::Item I = Ps.Work.back();
    Ps.Work.pop_back();
    if (I.Hash)
      buildHash(M, I.Id, Ps);
    else
      buildEq(M, I.Id, Ps);
  }
}

FuncId gir::addEqGlue(Module &M, Type *T, llvm::StringRef Symbol) {
  if (FuncId Id = existing(M, Symbol); Id != NoId)
    return Id;
  if (!hasGlue(T))
    return NoId;
  Context &C = M.Ctx;
  Type *P = C.ptr(T);
  FuncId Id = M.addFunction(glueFunction(Symbol, C.func({P, P}, C.boolean())));
  Parts Ps;
  Ps.Base = Symbol.str();
  buildEq(M, Id, Ps);
  buildParts(M, Ps);
  return Id;
}

FuncId gir::addHashGlue(Module &M, Type *T, llvm::StringRef Symbol) {
  if (FuncId Id = existing(M, Symbol); Id != NoId)
    return Id;
  if (!hasGlue(T))
    return NoId;
  Context &C = M.Ctx;
  FuncId Id =
      M.addFunction(glueFunction(Symbol, C.func({C.ptr(T), C.u64()}, C.u64())));
  Parts Ps;
  Ps.Base = Symbol.str();
  buildHash(M, Id, Ps);
  buildParts(M, Ps);
  return Id;
}
