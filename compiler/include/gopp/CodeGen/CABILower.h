//===- gopp/CodeGen/CABILower.h - Lowering for ABIArgInfo -------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.11 (D4.4, docs/design/c-abi.md). Go++'s own lowering of what Clang's
// ABI code decided (cabi::CallInfo, Import/CABI.h) into LLVM IR, for every
// ABIArgInfo kind, for arguments and for results. One set of rules, two
// directions:
//
//   calling C   (this task)    lowerCArgs, then the call, then lowerCResult
//   being called by C (P04.18, @Export and callback thunks)
//                              unpackCParams, then the body, then
//                              packCReturn
//
// The four functions share the layout of IR arguments (IRSignature) and the
// two memory primitives, so what one direction packs the other unpacks with
// the same code path: a value is loaded as the coerce type (a copy into a
// temporary when the type is smaller than the coerce type, so no byte beyond
// the value is read) and stored back the same way.
//
// A Go++ value is a scalar (a register value; a bool is i1) or an aggregate
// (the address of its memory, as in LowerToLLVM). Kinds that no Go++ target
// can produce (InAlloca, IndirectAliased, TargetSpecific) are refused by
// checkSupported before any IR is made.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_CODEGEN_CABILOWER_H
#define GOPP_CODEGEN_CABILOWER_H

#include "gopp/Import/CABI.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/Error.h"

#include <functional>
#include <vector>

namespace gopp {
namespace cabi {

/// A Go++ value passed to or from C.
struct Value {
  const gir::Type *Ty = nullptr;
  /// A scalar: its register value. An aggregate: the address of its storage.
  llvm::Value *V = nullptr;
  /// The alignment the address is known to have (aggregates).
  llvm::Align Known = llvm::Align(1);
};

/// What the lowering asks of the code generator that owns the module.
struct Env {
  llvm::IRBuilder<> &B;
  const llvm::DataLayout &DL;
  /// The LLVM type of a Go++ type in memory (a bool is i8, an aggregate its
  /// struct).
  std::function<llvm::Type *(const gir::Type *)> MemTy;
  /// A stack slot in the entry block of the function being made.
  std::function<llvm::AllocaInst *(llvm::Type *, llvm::Align,
                                   const llvm::Twine &)>
      Alloca;
};

/// The IR function type and attributes of a classified signature, and where
/// each argument's IR arguments are.
struct IRSignature {
  llvm::FunctionType *Type = nullptr;
  /// Attributes of the result and the parameters (none of the function).
  llvm::AttributeList Attrs;
  struct Slot {
    unsigned First = 0, Count = 0;
  };
  /// IR index of the hidden result pointer, or -1.
  int SRet = -1;
  std::vector<Slot> Args;
};

/// Refuses the kinds no Go++ target reaches, naming the kind.
llvm::Error checkSupported(const CallInfo &CI);

/// The scalar value type: i1 for a bool, the integer, float or pointer.
llvm::Type *valueType(llvm::LLVMContext &C, const gir::Type *T);

IRSignature
irSignature(llvm::LLVMContext &C, const CallInfo &CI,
            llvm::function_ref<llvm::Type *(const gir::Type *)> MemTy);

/// The IR arguments of a call of C. RetDest is where an aggregate result goes
/// (unused for a scalar or void result). An aggregate in Args must be the
/// callee's own copy: C may write to it when it is passed by a plain pointer.
struct PreparedCall {
  std::vector<llvm::Value *> Args;
  /// The slot the result is really written to when RetDest cannot be used.
  llvm::Value *SRetSlot = nullptr;
};
PreparedCall lowerCArgs(Env &E, const CallInfo &CI, const IRSignature &Sig,
                        llvm::ArrayRef<Value> Args, Value RetDest);
/// After the call: a scalar result is returned for the caller to store; an
/// aggregate result is written to RetDest (null returned); void gives null.
llvm::Value *lowerCResult(Env &E, const CallInfo &CI, const IRSignature &Sig,
                          PreparedCall &P, llvm::CallBase *Call, Value RetDest);

/// The reverse (P04.18): the Go++ values of the IR arguments of a function C
/// calls. Aggregates are in fresh stack slots (or the caller's memory, for
/// an indirect argument), which the body owns.
std::vector<Value> unpackCParams(Env &E, const CallInfo &CI,
                                 const IRSignature &Sig,
                                 llvm::ArrayRef<llvm::Argument *> IRArgs);
/// The reverse of lowerCResult: returns Result from a function C calls. For an
/// indirect result the value is copied through the hidden pointer.
void packCReturn(Env &E, const CallInfo &CI, const IRSignature &Sig,
                 llvm::ArrayRef<llvm::Argument *> IRArgs, Value Result);

} // namespace cabi
} // namespace gopp

#endif // GOPP_CODEGEN_CABILOWER_H
