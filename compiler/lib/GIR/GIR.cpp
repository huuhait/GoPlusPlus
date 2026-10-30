//===- GIR.cpp - The GIR data structures ----------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/GIR.h"

#include <cstring>

using namespace gopp;
using namespace gopp::gir;

const char *gir::getBinOpName(BinOp Op) {
  switch (Op) {
  case BinOp::Add:
    return "add";
  case BinOp::Sub:
    return "sub";
  case BinOp::Mul:
    return "mul";
  case BinOp::Div:
    return "div";
  case BinOp::Rem:
    return "rem";
  case BinOp::And:
    return "and";
  case BinOp::Or:
    return "or";
  case BinOp::Xor:
    return "xor";
  case BinOp::AndNot:
    return "andnot";
  case BinOp::Shl:
    return "shl";
  case BinOp::Shr:
    return "shr";
  case BinOp::Eq:
    return "eq";
  case BinOp::Ne:
    return "ne";
  case BinOp::Lt:
    return "lt";
  case BinOp::Le:
    return "le";
  case BinOp::Gt:
    return "gt";
  case BinOp::Ge:
    return "ge";
  }
  return "?";
}

const char *gir::getUnOpName(UnOp Op) {
  switch (Op) {
  case UnOp::Neg:
    return "neg";
  case UnOp::Not:
    return "not";
  case UnOp::Compl:
    return "compl";
  }
  return "?";
}

const char *gir::getCastKindName(CastKind K) {
  switch (K) {
  case CastKind::IntToInt:
    return "int_to_int";
  case CastKind::IntToFloat:
    return "int_to_float";
  case CastKind::FloatToInt:
    return "float_to_int";
  case CastKind::FloatToFloat:
    return "float_to_float";
  case CastKind::PtrToInt:
    return "ptr_to_int";
  case CastKind::IntToPtr:
    return "int_to_ptr";
  case CastKind::PtrToPtr:
    return "ptr_to_ptr";
  }
  return "?";
}

const char *gir::getAggKindName(AggKind K) {
  switch (K) {
  case AggKind::Tuple:
    return "tuple";
  case AggKind::Array:
    return "array";
  case AggKind::String:
    return "string";
  case AggKind::Slice:
    return "slice";
  case AggKind::Struct:
    return "struct";
  }
  return "?";
}

const char *gir::getAssertKindName(AssertKind K) {
  switch (K) {
  case AssertKind::DivByZero:
    return "div_by_zero";
  case AssertKind::NegativeShift:
    return "negative_shift";
  case AssertKind::Bounds:
    return "bounds";
  case AssertKind::Nil:
    return "nil";
  case AssertKind::Overflow:
    return "overflow";
  }
  return "?";
}

bool gir::isComparison(BinOp Op) { return Op >= BinOp::Eq; }
bool gir::isShift(BinOp Op) { return Op == BinOp::Shl || Op == BinOp::Shr; }

uint32_t Module::addFile(llvm::StringRef Path) {
  auto [It, New] = FileIds.try_emplace(Path, Files.size());
  if (New)
    Files.push_back(Path.str());
  return It->second;
}

uint32_t Module::internString(llvm::StringRef S) {
  auto [It, New] = StringIds.try_emplace(S, Strings.size());
  if (New)
    Strings.push_back(S.str());
  return It->second;
}

FuncId Module::addFunction(std::unique_ptr<Function> F) {
  FuncId Id = Funcs.size();
  FuncIds[F->Name] = Id;
  Funcs.push_back(std::move(F));
  return Id;
}

FuncId Module::lookupFunction(llvm::StringRef Name) const {
  auto It = FuncIds.find(Name);
  return It == FuncIds.end() ? NoId : It->second;
}

Operand gir::copyOf(Place P) {
  Operand O;
  O.K = Operand::Copy;
  O.P = std::move(P);
  return O;
}

Operand gir::moveOf(Place P) {
  Operand O;
  O.K = Operand::Move;
  O.P = std::move(P);
  return O;
}

Operand gir::constInt(Type *Ty, uint64_t Bits) {
  Operand O;
  O.K = Operand::Const;
  O.C.K = Constant::Int;
  O.C.Ty = Ty;
  if (Ty->bits() < 64)
    Bits &= (uint64_t(1) << Ty->bits()) - 1;
  O.C.Bits = Bits;
  return O;
}

Operand gir::constBool(Context &C, bool V) {
  Operand O;
  O.K = Operand::Const;
  O.C.K = Constant::Bool;
  O.C.Ty = C.boolean();
  O.C.Bits = V;
  return O;
}

Operand gir::constFloat(Type *Ty, double V) {
  Operand O;
  O.K = Operand::Const;
  O.C.K = Constant::Float;
  O.C.Ty = Ty;
  if (Ty->bits() == 32)
    V = static_cast<float>(V);
  std::memcpy(&O.C.Bits, &V, sizeof V);
  return O;
}

Operand gir::constStr(Module &M, llvm::StringRef S) {
  Operand O;
  O.K = Operand::Const;
  O.C.K = Constant::Str;
  O.C.Ty = M.Ctx.string();
  O.C.Bits = M.internString(S);
  return O;
}

Operand gir::constFunc(const Module &M, FuncId F) {
  Operand O;
  O.K = Operand::Const;
  O.C.K = Constant::Func;
  O.C.Ty = M.func(F).Sig;
  O.C.Bits = F;
  return O;
}
