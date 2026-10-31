//===- gopp/CodeGen/GoABI.h - The Go++ internal calling convention -*- C++
//-*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.18 (D4.4, docs/design/calling-convention.md). The ONE place that decides
// how a Go++ function called by Go++ takes its parameters and gives its
// result. CABILower.h is the same thing for the C ABI (Clang decides, Go++
// lowers); here Go++ decides, so the whole decision is this class:
//
//   classify(Sig)      the decision for a GIR function type (cached);
//   lowerArgs/lowerResult           the caller's side;
//   unpackParams/packReturn         the callee's side.
//
// The four lowering functions mirror cabi::lowerCArgs / lowerCResult /
// unpackCParams / packCReturn and share their Env and Value, so a thunk is
// one side of one convention against the other side of the other (a C-ABI
// thunk unpacks with cabi::unpackCParams and calls with lowerArgs).
//
// The rules, for a GIR type T (layout-blind: they read the LLVM memory type,
// so a change of layout, such as ADR-0029's four-word slice, changes nothing
// here):
//
//   unit, or an aggregate with no scalar inside   Ignore: no IR argument;
//   a scalar (bool, integer, float, pointer, func) Scalar: one IR argument,
//                                                  the register value;
//   an aggregate of at most MaxParamLeaves scalars
//   (arrays of one element flatten, longer arrays do not)
//                                                  Direct: one IR argument
//                                                  per scalar, in order;
//   any other aggregate                            Indirect: a pointer to the
//                                                  callee's own copy.
//
// A result is Direct when its scalars fit the target's return registers
// (Policy::RetInts, RetFloats: what the target's LLVM convention returns
// without falling back to memory, measured by
// tests/unit/CodeGen/GoABITest.cpp); else it goes through a hidden first
// pointer. A Direct result with one scalar is that scalar, with several a
// literal struct of them. The LLVM calling convention of every Go++ function
// is Policy::CC, `swiftcc` on both targets (ADR-0030): four integer and four
// float result registers on amd64 (the C convention has two), eight and
// eight on arm64, so `(int, error)` comes back in registers on both and
// `(string, error)` (five words) through memory on amd64. Like C, swiftcc
// keeps no XMM or vector register across a call. A closure's environment
// (a Func type with hasEnv(), P04.07) is the `swiftself` register, which is no
// argument's.
//
// What plugs in here later, without a change to the rules:
//   P04.06  a method call through an itab: classify the method's signature,
//           the receiver is the first parameter (the itab's `data`);
//           interface values are Tuple(ptr, ptr), Direct;
//   P04.07  closures (done): a Func type with hasEnv() puts its first
//           parameter in `swiftself` (classify), lowerArgs(..., Env);
//           panics: whether a call unwinds is the call's business (invoke),
//           not the convention's;
//   P04.08  generic instances: classify the instantiated signature;
//   P04.10  `!` returning (T, error): the result is the Tuple GIR makes,
//           Direct while it fits;
//   P12     goroutine entry: a thunk whose signature is `func(ptr)`, built
//           with the same four functions.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_CODEGEN_GOABI_H
#define GOPP_CODEGEN_GOABI_H

#include "gopp/CodeGen/CABILower.h"
#include "gopp/GIR/Type.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/TargetParser/Triple.h"

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace gopp {
namespace goabi {

/// One scalar of a Direct value: its LLVM type in memory and its byte offset.
struct Part {
  llvm::Type *Ty = nullptr;
  uint64_t Offset = 0;
};

enum class Kind : uint8_t { Ignore, Scalar, Direct, Indirect };

struct ArgInfo {
  Kind K = Kind::Ignore;
  const gir::Type *Ty = nullptr;
  llvm::SmallVector<Part, 4> Parts; // Direct
  unsigned First = 0, Count = 0;    // the IR arguments (Indirect result: none)
};

/// The decision for one function type.
struct FnInfo {
  ArgInfo Ret; // Indirect: written through the hidden pointer SRet
  std::vector<ArgInfo> Params;
  llvm::FunctionType *Type = nullptr;
  /// Extension attributes of the scalars (the C ABI of the platform wants the
  /// extension of a narrow integer spelled out; Go++ keeps clang's habit).
  llvm::AttributeList Attrs;
  llvm::CallingConv::ID CC = llvm::CallingConv::Swift;
  int SRet = -1; // IR index of the hidden result pointer, or -1
  int Env = -1;  // IR index of the environment, or -1
};

/// What the target and the rules allow.
struct Policy {
  unsigned MaxParamLeaves = 4;
  /// Integer-class and float-class scalars a result can have and stay in
  /// registers.
  unsigned RetInts = 0, RetFloats = 0;
  /// The LLVM calling convention of a Go++ function on the target (swiftcc
  /// on both, ADR-0030).
  llvm::CallingConv::ID CC = llvm::CallingConv::Swift;
  static Policy forTriple(const llvm::Triple &T);
};

class GoAbi {
public:
  /// MemTy is the memory type of a GIR type (a bool is i8, an aggregate its
  /// struct): the one LowerToLLVM uses.
  GoAbi(llvm::LLVMContext &C, const llvm::DataLayout &DL, Policy P,
        std::function<llvm::Type *(const gir::Type *)> MemTy)
      : C(C), DL(DL), P(P), MemTy(std::move(MemTy)) {}

  /// The decision for Sig (a Func type). A closure's code has the
  /// environment as the first parameter of its type (Type::hasEnv): it is
  /// the `swiftself` register (FnInfo::Env), no argument's, and Params
  /// are the others. Cached by the uniqued type, so valid as long as the GIR
  /// context lives.
  const FnInfo &classify(const gir::Type *Sig);

  /// The caller: the IR arguments. An aggregate Args[I] is the address of its
  /// bytes (an Indirect one must be the callee's own copy, the callee may
  /// write to it); RetDest is where an aggregate result goes (an Indirect
  /// result is written there by the callee). EnvPtr is the environment.
  std::vector<llvm::Value *> lowerArgs(cabi::Env &E, const FnInfo &FI,
                                       llvm::ArrayRef<cabi::Value> Args,
                                       cabi::Value RetDest,
                                       llvm::Value *EnvPtr = nullptr);
  /// After the call: a scalar result is returned for the caller to store; a
  /// Direct aggregate is written to RetDest (null returned); else null.
  llvm::Value *lowerResult(cabi::Env &E, const FnInfo &FI, llvm::CallBase *Call,
                           cabi::Value RetDest);

  /// The callee: the Go++ values of the IR arguments. A Direct aggregate is
  /// in a fresh slot made by E.Alloca (the body's own copy); an Indirect one
  /// is the caller's copy, which the body owns.
  std::vector<cabi::Value>
  unpackParams(cabi::Env &E, const FnInfo &FI,
               llvm::ArrayRef<llvm::Argument *> IRArgs);
  /// Returns Result from the function: a scalar's value, or the address of
  /// a Direct aggregate result's bytes (an Indirect one was built in place).
  void packReturn(cabi::Env &E, const FnInfo &FI, cabi::Value Result);

  /// Test-only work counter: one per LLVM type visited.
  uint64_t *Work = nullptr;

  /// The scalars of a memory type in order, when there are at most Max of
  /// them (integer-class Ints, float-class Floats); false otherwise.
  bool flatten(llvm::Type *T, unsigned Max, llvm::SmallVectorImpl<Part> &Out,
               unsigned &Ints, unsigned &Floats);

private:
  struct Count {
    uint16_t Ints = 0, Floats = 0;
    bool Bad = false; // not flattenable
  };
  Count countOf(llvm::Type *T);
  llvm::LLVMContext &C;
  const llvm::DataLayout &DL;
  Policy P;
  std::function<llvm::Type *(const gir::Type *)> MemTy;
  llvm::DenseMap<llvm::Type *, Count> Counts;
  std::map<const gir::Type *, std::unique_ptr<FnInfo>> Infos;
};

} // namespace goabi
} // namespace gopp

#endif // GOPP_CODEGEN_GOABI_H
