//===- Runtime.cpp - Functions the compiler writes in GIR -----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The bootstrap runtime (D4.11 e): until the Go++ runtime exists (P05), the
// functions a program needs from it are written here, in GIR, on top of the C
// `write` and `exit`:
//
//   print_u64, print_i64, print_hex, print_f64   Go's println formats
//   panic_<kind>                                 the failure of an Assert:
//       writes Go's first line (`panic: runtime error: ...`) to standard
//       error and exits with status 2. There is no unwinding yet: P04.26
//       turns the call into an unwinding panic (the code generator calls
//       these functions from one shared block per kind and function, ADR-0028
//       rule 3.3) and P05 adds the stack trace.
//   complex128div                                Go's runtime complex division
//
// Each is made on first use (a program that prints nothing has none of them).
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

#include <cmath>

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;

//===----------------------------------------------------------------------===//
// C functions the helpers call
//===----------------------------------------------------------------------===//

FuncId Lowering::ensureCFunc(StringRef Name, Type *Sig, SourceLoc At,
                             bool NoReturn, StringRef Who, StringRef Needs) {
  Sig = Ctx.withCallConv(Sig, CallConv::C);
  std::string Sym = symbolOf(SymbolKind::ExternC, CP.Pkg, Name);
  FuncId Id = M->lookupFunction(Sym);
  if (BadC.count(Name))
    return NoId;
  if (Id != NoId) {
    // The program declares it itself: the helpers share it if it is the
    // function they need.
    if (M->func(Id).Sig != Sig || !M->func(Id).isDeclaration()) {
      BadC.insert(Name);
      badExtern(At, Name, Who + " calls it, and needs " + Needs);
      return NoId;
    }
    return Id;
  }
  auto F = std::make_unique<Function>();
  F->Name = Sym;
  F->Sig = Sig;
  F->CC = CallConv::C;
  F->NoUnwind = true;
  F->NoReturn = NoReturn;
  return M->addFunction(std::move(F));
}

FuncId Lowering::ensureWrite(SourceLoc At) {
  if (WriteFn != NoId)
    return WriteFn;
  Type *Sig = Ctx.func({Ctx.i32(), Ctx.ptr(Ctx.u8()), Ctx.u64()}, Ctx.i64());
  return WriteFn = ensureCFunc("write", Sig, At, false, "println",
                               "func write(fd int32, p *byte, n uint) int");
}

FuncId Lowering::ensureExit(SourceLoc At) {
  if (ExitFn != NoId)
    return ExitFn;
  return ExitFn = ensureCFunc("exit", Ctx.func({Ctx.i32()}, Ctx.unit()), At,
                              true, "a panic", "func exit(status int32)");
}

//===----------------------------------------------------------------------===//
// A tiny emitter for the helpers
//===----------------------------------------------------------------------===//

namespace {

struct Emit {
  Emit(Lowering &L, Function &F) : L(L), M(*L.M), C(L.Ctx), FB(M, F) {}
  Lowering &L;
  Module &M;
  Context &C;
  FuncBuilder FB;

  Operand i32(uint64_t V) { return constInt(C.i32(), V); }
  Operand u8(uint64_t V) { return constInt(C.u8(), V); }
  Operand u64(uint64_t V) { return constInt(C.u64(), V); }
  Operand i64(uint64_t V) { return constInt(C.i64(), V); }
  Operand f64(double V) { return constFloat(C.floatTy(64), V); }
  Operand cp(const Place &P) { return copyOf(P); }
  Operand cl(LocalId L) { return copyOf(FB.local(L)); }
  Place bin(BinOp Op, Operand A, Operand B) {
    return FB.temp(FB.binary(Op, std::move(A), std::move(B)));
  }
  Place cast(CastKind K, Operand A, Type *T) {
    return FB.temp(FB.cast(K, std::move(A), T));
  }
  void set(LocalId L, Rvalue RV) { FB.assign(FB.local(L), std::move(RV)); }
  LocalId var(Type *T, const char *Name) {
    return FB.newLocal(T, Local::Var, Name);
  }

  /// write(fd, ptr, len), ignoring the result.
  void write(Operand Fd, Operand Ptr, Operand Len) {
    FuncId W = L.WriteFn;
    FB.call(constFunc(M, W), {std::move(Fd), std::move(Ptr), std::move(Len)},
            FB.local(FB.newTemp(C.i64())), {UnwindAction::Unreachable, NoId});
  }
  /// write(fd, text): the bytes of a constant string.
  void writeText(Operand Fd, StringRef Text) {
    Place S = FB.temp(FB.use(constStr(M, Text)));
    write(std::move(Fd), cp(FB.field(S, 0)), cp(FB.field(S, 1)));
  }
};

/// A new function with the symbol Name (a helper of the compiler).
Function *newHelper(Lowering &L, StringRef Name, Type *Sig, Linkage Link,
                    FuncId &Id) {
  auto Fn = std::make_unique<Function>();
  Fn->Name = symbolOf(SymbolKind::Runtime, L.CP.Pkg, Name);
  Fn->Sig = Sig;
  Fn->Link = Link;
  Fn->CC = CallConv::Go;
  Function *F = Fn.get();
  Id = L.M->addFunction(std::move(Fn));
  return F;
}

} // namespace

//===----------------------------------------------------------------------===//
// println helpers
//===----------------------------------------------------------------------===//

FuncId Lowering::ensurePrintU64(SourceLoc At) {
  if (PrintU != NoId)
    return PrintU;
  if (ensureWrite(At) == NoId)
    return NoId;
  Function *F = newHelper(*this, "print_u64",
                          Ctx.func({Ctx.i32(), Ctx.u64()}, Ctx.unit()),
                          Linkage::Internal, PrintU);
  // var buf [20]byte; i := 20
  // for { i--; buf[i] = byte(v%10 + '0'); v /= 10; if v == 0 { break } }
  // write(fd, &buf[i], 20-i)
  Emit E(*this, *F);
  FuncBuilder &FB = E.FB;
  LocalId Fd = FB.param(0), V = FB.param(1);
  F->Locals[Fd].Name = "fd";
  F->Locals[V].Name = "v";
  LocalId Buf = E.var(Ctx.array(Ctx.u8(), 20), "buf");
  LocalId Idx = E.var(Ctx.u64(), "i");
  E.set(Idx, FB.use(E.u64(20)));
  BlockId Loop = FB.newBlock(), Done = FB.newBlock();
  FB.gotoBlock(Loop);
  FB.setBlock(Loop);
  E.set(Idx, FB.binary(BinOp::Sub, E.cl(Idx), E.u64(1)));
  Place Digit = E.bin(BinOp::Rem, E.cl(V), E.u64(10));
  Place Narrow = E.cast(CastKind::IntToInt, E.cp(Digit), Ctx.u8());
  Place Ascii = E.bin(BinOp::Add, E.cp(Narrow), E.u8('0'));
  FB.assign(FB.index(FB.local(Buf), Idx), FB.use(E.cp(Ascii)));
  E.set(V, FB.binary(BinOp::Div, E.cl(V), E.u64(10)));
  Place More = E.bin(BinOp::Ne, E.cl(V), E.u64(0));
  FB.branch(E.cp(More), Loop, Done);
  FB.setBlock(Done);
  Place Ptr = FB.temp(FB.addrOf(FB.index(FB.local(Buf), Idx)));
  Place Count = E.bin(BinOp::Sub, E.u64(20), E.cl(Idx));
  E.write(E.cl(Fd), E.cp(Ptr), E.cp(Count));
  FB.ret();
  return PrintU;
}

FuncId Lowering::ensurePrintI64(SourceLoc At) {
  if (PrintI != NoId)
    return PrintI;
  FuncId U = ensurePrintU64(At);
  if (U == NoId)
    return NoId;
  Function *F = newHelper(*this, "print_i64",
                          Ctx.func({Ctx.i32(), Ctx.i64()}, Ctx.unit()),
                          Linkage::Internal, PrintI);
  // u := uint64(v); if v < 0 { write("-"); u = uint64(-v) }; print_u64(u)
  Emit E(*this, *F);
  FuncBuilder &FB = E.FB;
  LocalId Fd = FB.param(0), V = FB.param(1);
  F->Locals[Fd].Name = "fd";
  F->Locals[V].Name = "v";
  LocalId Uv = E.var(Ctx.u64(), "u");
  E.set(Uv, FB.cast(CastKind::IntToInt, E.cl(V), Ctx.u64()));
  Place Neg = E.bin(BinOp::Lt, E.cl(V), E.i64(0));
  BlockId Minus = FB.newBlock(), Digits = FB.newBlock();
  FB.branch(E.cp(Neg), Minus, Digits);
  FB.setBlock(Minus);
  E.writeText(E.cl(Fd), "-");
  Place Flipped = FB.temp(FB.unary(UnOp::Neg, E.cl(V)));
  E.set(Uv, FB.cast(CastKind::IntToInt, E.cp(Flipped), Ctx.u64()));
  FB.gotoBlock(Digits);
  FB.setBlock(Digits);
  FB.call(constFunc(*M, U), {E.cl(Fd), E.cl(Uv)},
          FB.local(FB.newTemp(Ctx.unit())), {UnwindAction::Continue, NoId});
  FB.ret();
  return PrintI;
}

FuncId Lowering::ensurePrintHex(SourceLoc At) {
  if (PrintHex != NoId)
    return PrintHex;
  if (ensureWrite(At) == NoId)
    return NoId;
  Function *F = newHelper(*this, "print_hex",
                          Ctx.func({Ctx.i32(), Ctx.u64()}, Ctx.unit()),
                          Linkage::Internal, PrintHex);
  // Go's printhex: "0x" and the digits without leading zeros ("0x0" for 0).
  Emit E(*this, *F);
  FuncBuilder &FB = E.FB;
  LocalId Fd = FB.param(0), V = FB.param(1);
  F->Locals[Fd].Name = "fd";
  F->Locals[V].Name = "v";
  LocalId Buf = E.var(Ctx.array(Ctx.u8(), 16), "buf");
  LocalId Idx = E.var(Ctx.u64(), "i");
  E.set(Idx, FB.use(E.u64(16)));
  E.writeText(E.cl(Fd), "0x");
  BlockId Loop = FB.newBlock(), Letter = FB.newBlock(), Store = FB.newBlock(),
          Done = FB.newBlock();
  FB.gotoBlock(Loop);
  FB.setBlock(Loop);
  E.set(Idx, FB.binary(BinOp::Sub, E.cl(Idx), E.u64(1)));
  Place Digit = E.bin(BinOp::And, E.cl(V), E.u64(15));
  Place D8 = E.cast(CastKind::IntToInt, E.cp(Digit), Ctx.u8());
  LocalId Ch = E.var(Ctx.u8(), "ch");
  E.set(Ch, FB.binary(BinOp::Add, E.cp(D8), E.u8('0')));
  Place Big = E.bin(BinOp::Gt, E.cp(D8), E.u8(9));
  FB.branch(E.cp(Big), Letter, Store);
  FB.setBlock(Letter);
  E.set(Ch, FB.binary(BinOp::Add, E.cl(Ch), E.u8('a' - '0' - 10)));
  FB.gotoBlock(Store);
  FB.setBlock(Store);
  FB.assign(FB.index(FB.local(Buf), Idx), FB.use(E.cl(Ch)));
  E.set(V, FB.binary(BinOp::Shr, E.cl(V), E.u64(4)));
  Place More = E.bin(BinOp::Ne, E.cl(V), E.u64(0));
  FB.branch(E.cp(More), Loop, Done);
  FB.setBlock(Done);
  Place Ptr = FB.temp(FB.addrOf(FB.index(FB.local(Buf), Idx)));
  Place Count = E.bin(BinOp::Sub, E.u64(16), E.cl(Idx));
  E.write(E.cl(Fd), E.cp(Ptr), E.cp(Count));
  FB.ret();
  return PrintHex;
}

FuncId Lowering::ensurePrintF64(SourceLoc At) {
  if (PrintF != NoId)
    return PrintF;
  if (ensureWrite(At) == NoId)
    return NoId;
  Type *F64 = Ctx.floatTy(64);
  Function *F =
      newHelper(*this, "print_f64", Ctx.func({Ctx.i32(), F64}, Ctx.unit()),
                Linkage::Internal, PrintF);
  // Go's printfloat (runtime/print.go): seven digits, `+d.dddddde+ddd`.
  Emit E(*this, *F);
  FuncBuilder &FB = E.FB;
  LocalId Fd = FB.param(0), V = FB.param(1);
  F->Locals[Fd].Name = "fd";
  F->Locals[V].Name = "v";
  const unsigned N = 7;
  Type *U8 = Ctx.u8(), *I64 = Ctx.i64();
  auto Special = [&](Operand Cond, StringRef Text) {
    BlockId Yes = FB.newBlock(), No = FB.newBlock();
    FB.branch(std::move(Cond), Yes, No);
    FB.setBlock(Yes);
    E.writeText(E.cl(Fd), Text);
    FB.ret();
    FB.setBlock(No);
  };
  Special(E.cp(E.bin(BinOp::Ne, E.cl(V), E.cl(V))), "NaN");
  Place VV = E.bin(BinOp::Add, E.cl(V), E.cl(V));
  Place Fixed = E.bin(BinOp::Eq, E.cp(VV), E.cl(V));
  {
    BlockId Inf = FB.newBlock(), Fin = FB.newBlock();
    FB.branch(E.cp(Fixed), Inf, Fin);
    FB.setBlock(Inf);
    BlockId Pos = FB.newBlock(), Neg = FB.newBlock(), Zero = FB.newBlock();
    Place IsPos = E.bin(BinOp::Gt, E.cl(V), E.f64(0));
    FB.branch(E.cp(IsPos), Pos, Zero);
    FB.setBlock(Pos);
    E.writeText(E.cl(Fd), "+Inf");
    FB.ret();
    FB.setBlock(Zero);
    Place IsNeg = E.bin(BinOp::Lt, E.cl(V), E.f64(0));
    FB.branch(E.cp(IsNeg), Neg, Fin);
    FB.setBlock(Neg);
    E.writeText(E.cl(Fd), "-Inf");
    FB.ret();
    FB.setBlock(Fin);
  }
  LocalId Buf = E.var(Ctx.array(U8, N + 7), "buf");
  LocalId Ex = E.var(I64, "e");
  LocalId Sign = E.var(U8, "sign");
  E.set(Ex, FB.use(E.i64(0)));
  E.set(Sign, FB.use(E.u8('+')));
  BlockId Zero = FB.newBlock(), NonZero = FB.newBlock(), Digits = FB.newBlock();
  FB.branch(E.cp(E.bin(BinOp::Eq, E.cl(V), E.f64(0))), Zero, NonZero);
  // v == 0: the sign of a negative zero.
  FB.setBlock(Zero);
  {
    BlockId NegZ = FB.newBlock();
    Place Inv = E.bin(BinOp::Div, E.f64(1), E.cl(V));
    FB.branch(E.cp(E.bin(BinOp::Lt, E.cp(Inv), E.f64(0))), NegZ, Digits);
    FB.setBlock(NegZ);
    E.set(Sign, FB.use(E.u8('-')));
    FB.gotoBlock(Digits);
  }
  FB.setBlock(NonZero);
  {
    BlockId Flip = FB.newBlock(), Norm = FB.newBlock();
    FB.branch(E.cp(E.bin(BinOp::Lt, E.cl(V), E.f64(0))), Flip, Norm);
    FB.setBlock(Flip);
    E.set(V, FB.unary(UnOp::Neg, E.cl(V)));
    E.set(Sign, FB.use(E.u8('-')));
    FB.gotoBlock(Norm);
    FB.setBlock(Norm);
    // for v >= 10 { e++; v /= 10 }
    BlockId UpH = FB.newBlock(), UpB = FB.newBlock(), DnH = FB.newBlock(),
            DnB = FB.newBlock(), Rnd = FB.newBlock();
    FB.gotoBlock(UpH);
    FB.setBlock(UpH);
    FB.branch(E.cp(E.bin(BinOp::Ge, E.cl(V), E.f64(10))), UpB, DnH);
    FB.setBlock(UpB);
    E.set(Ex, FB.binary(BinOp::Add, E.cl(Ex), E.i64(1)));
    E.set(V, FB.binary(BinOp::Div, E.cl(V), E.f64(10)));
    FB.gotoBlock(UpH);
    // for v < 1 { e--; v *= 10 }
    FB.setBlock(DnH);
    FB.branch(E.cp(E.bin(BinOp::Lt, E.cl(V), E.f64(1))), DnB, Rnd);
    FB.setBlock(DnB);
    E.set(Ex, FB.binary(BinOp::Sub, E.cl(Ex), E.i64(1)));
    E.set(V, FB.binary(BinOp::Mul, E.cl(V), E.f64(10)));
    FB.gotoBlock(DnH);
    // round: v += 5e-8 computed as Go does (h := 5.0; h /= 10, n times)
    FB.setBlock(Rnd);
    double H = 5.0;
    for (unsigned K = 0; K < N; ++K)
      H /= 10;
    E.set(V, FB.binary(BinOp::Add, E.cl(V), E.f64(H)));
    BlockId Carry = FB.newBlock();
    FB.branch(E.cp(E.bin(BinOp::Ge, E.cl(V), E.f64(10))), Carry, Digits);
    FB.setBlock(Carry);
    E.set(Ex, FB.binary(BinOp::Add, E.cl(Ex), E.i64(1)));
    E.set(V, FB.binary(BinOp::Div, E.cl(V), E.f64(10)));
    FB.gotoBlock(Digits);
  }
  FB.setBlock(Digits);
  // for i := 0; i < n; i++ { s := int(v); buf[i+2] = byte(s+'0'); v -=
  // float64(s); v *= 10 }
  LocalId K = E.var(I64, "i");
  E.set(K, FB.use(E.i64(0)));
  {
    BlockId Head = FB.newBlock(), Body = FB.newBlock(), After = FB.newBlock();
    FB.gotoBlock(Head);
    FB.setBlock(Head);
    FB.branch(E.cp(E.bin(BinOp::Lt, E.cl(K), E.i64(N))), Body, After);
    FB.setBlock(Body);
    Place Sv = E.cast(CastKind::FloatToInt, E.cl(V), I64);
    Place Ch = E.cast(CastKind::IntToInt, E.cp(Sv), U8);
    Place Asc = E.bin(BinOp::Add, E.cp(Ch), E.u8('0'));
    Place Pos = E.bin(BinOp::Add, E.cl(K), E.i64(2));
    LocalId PosL = FB.newTemp(I64);
    FB.assign(FB.local(PosL), FB.use(E.cp(Pos)));
    FB.assign(FB.index(FB.local(Buf), PosL), FB.use(E.cp(Asc)));
    Place Back = E.cast(CastKind::IntToFloat, E.cp(Sv), F64);
    E.set(V, FB.binary(BinOp::Sub, E.cl(V), E.cp(Back)));
    E.set(V, FB.binary(BinOp::Mul, E.cl(V), E.f64(10)));
    E.set(K, FB.binary(BinOp::Add, E.cl(K), E.i64(1)));
    FB.gotoBlock(Head);
    FB.setBlock(After);
  }
  auto Put = [&](uint64_t At_, Operand Val) {
    FB.assign(FB.constIndex(FB.local(Buf), At_), FB.use(std::move(Val)));
  };
  // buf[0] = sign; buf[1] = buf[2]; buf[2] = '.'; buf[n+2] = 'e'
  Put(0, E.cl(Sign));
  Put(1, E.cp(FB.constIndex(FB.local(Buf), 2)));
  Put(2, E.u8('.'));
  Put(N + 2, E.u8('e'));
  LocalId ESign = E.var(U8, "esign");
  E.set(ESign, FB.use(E.u8('+')));
  {
    BlockId NegE = FB.newBlock(), Join = FB.newBlock();
    FB.branch(E.cp(E.bin(BinOp::Lt, E.cl(Ex), E.i64(0))), NegE, Join);
    FB.setBlock(NegE);
    E.set(ESign, FB.use(E.u8('-')));
    E.set(Ex, FB.unary(UnOp::Neg, E.cl(Ex)));
    FB.gotoBlock(Join);
    FB.setBlock(Join);
  }
  Put(N + 3, E.cl(ESign));
  auto Digit = [&](Operand Val) {
    Place C8 = E.cast(CastKind::IntToInt, std::move(Val), U8);
    return E.cp(E.bin(BinOp::Add, E.cp(C8), E.u8('0')));
  };
  Put(N + 4, Digit(E.cp(E.bin(BinOp::Div, E.cl(Ex), E.i64(100)))));
  Place Tens = E.bin(BinOp::Div, E.cl(Ex), E.i64(10));
  Put(N + 5, Digit(E.cp(E.bin(BinOp::Rem, E.cp(Tens), E.i64(10)))));
  Put(N + 6, Digit(E.cp(E.bin(BinOp::Rem, E.cl(Ex), E.i64(10)))));
  Place Ptr = FB.temp(FB.addrOf(FB.constIndex(FB.local(Buf), 0)));
  E.write(E.cl(Fd), E.cp(Ptr), E.u64(N + 7));
  FB.ret();
  return PrintF;
}

//===----------------------------------------------------------------------===//
// Panics (the failure of an Assert)
//===----------------------------------------------------------------------===//

FuncId Lowering::ensurePanic(AssertKind K, bool Unsigned, SourceLoc At) {
  FuncId &Slot = Panics[static_cast<unsigned>(K)][Unsigned];
  if (Slot != NoId)
    return Slot;
  FuncId Write = ensureWrite(At), Exit = ensureExit(At);
  if (Write == NoId || Exit == NoId)
    return NoId;
  FuncId U = ensurePrintU64(At), S = ensurePrintI64(At);
  if (U == NoId || S == NoId)
    return NoId;
  bool Bounds = takesIndexAndLength(K);
  Type *Pos = Ctx.ptr(Ctx.u8());
  SmallVector<Type *, 3> Params{Pos};
  if (Bounds) {
    Params.push_back(Unsigned ? Ctx.u64() : Ctx.i64());
    Params.push_back(Ctx.u64());
  }
  std::string Name = std::string("panic_") + getAssertKindName(K) +
                     (Bounds && Unsigned ? "_u" : "");
  auto Fn = std::make_unique<Function>();
  Fn->Name = symbolOf(SymbolKind::Runtime, CP.Pkg, Name);
  Fn->Sig = Ctx.func(Params, Ctx.unit());
  Fn->Link = Linkage::LinkOnceODR;
  Fn->CC = CallConv::Go;
  Fn->NoReturn = true;
  Function *F = Fn.get();
  Slot = M->addFunction(std::move(Fn));
  Emit E(*this, *F);
  FuncBuilder &FB = E.FB;
  F->Locals[FB.param(0)].Name = "pos";
  Operand Fd = E.i32(2);
  E.writeText(Fd, "panic: runtime error: ");
  switch (K) {
  case AssertKind::DivByZero:
    E.writeText(Fd, "integer divide by zero");
    break;
  case AssertKind::NegativeShift:
    E.writeText(Fd, "negative shift amount");
    break;
  case AssertKind::Nil:
    E.writeText(Fd, "invalid memory address or nil pointer dereference");
    break;
  case AssertKind::Overflow:
    E.writeText(Fd, "integer overflow");
    break;
  case AssertKind::Bounds:
  case AssertKind::SliceAlen:
  case AssertKind::SliceAcap:
  case AssertKind::SliceB:
  case AssertKind::Slice3Alen:
  case AssertKind::Slice3Acap:
  case AssertKind::Slice3B:
  case AssertKind::Slice3C:
  case AssertKind::SliceConvert: {
    // Go's boundsErrorFmts: "index out of range [5] with length 3", "slice
    // bounds out of range [:7] with capacity 4", "[2:1]", ... A negative
    // value has no second number (boundsNegErrorFmts).
    struct Fmt {
      const char *Open, *Mid, *Tail, *NegClose;
    };
    Fmt Fm;
    switch (K) {
    case AssertKind::SliceAlen:
      Fm = {"slice bounds out of range [:", "] with length ", "", "]"};
      break;
    case AssertKind::SliceAcap:
      Fm = {"slice bounds out of range [:", "] with capacity ", "", "]"};
      break;
    case AssertKind::SliceB:
      Fm = {"slice bounds out of range [", ":", "]", ":]"};
      break;
    case AssertKind::Slice3Alen:
      Fm = {"slice bounds out of range [::", "] with length ", "", "]"};
      break;
    case AssertKind::Slice3Acap:
      Fm = {"slice bounds out of range [::", "] with capacity ", "", "]"};
      break;
    case AssertKind::Slice3B:
      Fm = {"slice bounds out of range [:", ":", "]", ":]"};
      break;
    case AssertKind::Slice3C:
      Fm = {"slice bounds out of range [", ":", ":]", "::]"};
      break;
    case AssertKind::SliceConvert:
      Fm = {"cannot convert slice with length ",
            " to array or pointer to array with length ", "", ""};
      break;
    default:
      Fm = {"index out of range [", "] with length ", "", "]"};
      break;
    }
    LocalId Idx = FB.param(1), Len = FB.param(2);
    E.writeText(Fd, Fm.Open);
    FuncId Print = Unsigned ? U : S;
    FB.call(constFunc(*M, Print), {Fd, E.cl(Idx)},
            FB.local(FB.newTemp(Ctx.unit())), {UnwindAction::Continue, NoId});
    if (Unsigned) {
      E.writeText(Fd, Fm.Mid);
    } else {
      BlockId NegB = FB.newBlock(), PosB = FB.newBlock(), Join = FB.newBlock();
      FB.branch(E.cp(E.bin(BinOp::Lt, E.cl(Idx), E.i64(0))), NegB, PosB);
      FB.setBlock(NegB);
      E.writeText(Fd, std::string(Fm.NegClose) + "\n");
      FB.call(constFunc(*M, Exit), {E.i32(2)}, FB.local(FB.newTemp(Ctx.unit())),
              {UnwindAction::Unreachable, NoId});
      FB.unreachable();
      FB.setBlock(PosB);
      E.writeText(Fd, Fm.Mid);
      FB.gotoBlock(Join);
      FB.setBlock(Join);
    }
    FB.call(constFunc(*M, U), {Fd, E.cl(Len)}, FB.local(FB.newTemp(Ctx.unit())),
            {UnwindAction::Continue, NoId});
    if (*Fm.Tail)
      E.writeText(Fd, Fm.Tail);
    break;
  }
  }
  E.writeText(Fd, "\n");
  FB.call(constFunc(*M, Exit), {E.i32(2)}, FB.local(FB.newTemp(Ctx.unit())),
          {UnwindAction::Unreachable, NoId});
  FB.unreachable();
  return Slot;
}

//===----------------------------------------------------------------------===//
// The panic of a `!` (ERR-9)
//===----------------------------------------------------------------------===//

// `panic_must(expr, pos string, err error)` writes the message of
// `errors.MustError` (ERR-9) to standard error and exits with status 2:
//
//   panic: must: <expr> at <pos>: <err.Error()>
//
// The three arguments are the fields of the value (`Expr`, `Pos`, `Err`).
// There is no `errors` package yet (P05.15 / P13), so no value is made: the
// message is all a program can observe while a panic cannot be recovered.
// P04.26 (unwinding) makes the call a throw of `MustError{Err, Expr, Pos}`
// and prints this line only for a panic nobody recovers; the call site, the
// arguments and the one shared block per function stay as they are
// (Body::bangPanicBlock). The error's trace (ERR-14) comes with the runtime.
FuncId Lowering::ensurePanicMust(SourceLoc At) {
  if (PanicMust != NoId)
    return PanicMust;
  FuncId Write = ensureWrite(At), Exit = ensureExit(At);
  if (Write == NoId || Exit == NoId)
    return NoId;
  Type *Str = Ctx.string(), *Err = Ctx.iface();
  Type *PU8 = Ctx.ptr(Ctx.u8());
  Function *F =
      newHelper(*this, "panic_must", Ctx.func({Str, Str, Err}, Ctx.unit()),
                Linkage::LinkOnceODR, PanicMust);
  F->NoReturn = true;
  Rt R(*M, *F);
  FuncBuilder &FB = R.FB;
  F->Locals[FB.param(0)].Name = "expr";
  F->Locals[FB.param(1)].Name = "pos";
  F->Locals[FB.param(2)].Name = "err";
  auto Put = [&](Val S) {
    Val P = R.snap(R.fld(S, 0)), N = R.snap(R.fld(S, 1));
    R.callC(Write, {R.i32(2), P, N});
  };
  auto Text = [&](StringRef T) { Put(Val{&R, constStr(*M, T)}); };
  Text("panic: must: ");
  Put(R.param(0));
  Text(" at ");
  Put(R.param(1));
  Text(": ");
  // err.Error(): slot 0 of the itab of `error` (word 0 of the itab is its
  // descriptor; a tagged pointer has its low bit cleared, as a call through
  // an interface does, Body::lowerMethodCall).
  Val E = R.param(2);
  Val It = R.snap(R.fld(E, 0)), Data = R.snap(R.fld(E, 1));
  Val Bits = R.to(Ctx.u64(), It);
  Val Tab = R.to(PU8, Bits - (Bits & 1));
  Val Fp = R.load(R.padd(Tab, R.u(8)), PU8);
  Type *FT = Ctx.func({PU8}, Str);
  Val Fn = R.tmp(FB.cast(CastKind::PtrToPtr, Fp.Op, FT));
  Put(R.snap(R.callIndirect(Fn, {Data})));
  Text("\n");
  R.callC(Exit, {R.i32(2)});
  FB.unreachable();
  R.finish();
  return PanicMust;
}

//===----------------------------------------------------------------------===//
// The panic of a unit out of range (UNI-2)
//===----------------------------------------------------------------------===//

// `panic_unit(unit string)`: a run-time unit descriptor that leaves the range
// of units (an exponent past 2^20, a factor whose numerator or denominator
// passes 64 bits: a chain of 25 generics that each pass `U*U` on) stops the
// program with
//
//   panic: unit out of range: <the unit as the generic code wrote it>
//
// and exit status 2.
FuncId Lowering::ensurePanicUnit(SourceLoc At) {
  if (PanicUnit != NoId)
    return PanicUnit;
  FuncId Write = ensureWrite(At), Exit = ensureExit(At);
  if (Write == NoId || Exit == NoId)
    return NoId;
  Type *Str = Ctx.string();
  Function *F = newHelper(*this, "panic_unit", Ctx.func({Str}, Ctx.unit()),
                          Linkage::LinkOnceODR, PanicUnit);
  F->NoReturn = true;
  F->NoUnwind = true;
  Rt R(*M, *F);
  FuncBuilder &FB = R.FB;
  F->Locals[FB.param(0)].Name = "unit";
  auto Put = [&](Val S) {
    Val P = R.snap(R.fld(S, 0)), N = R.snap(R.fld(S, 1));
    R.callC(Write, {R.i32(2), P, N});
  };
  Put(Val{&R, constStr(*M, "panic: unit out of range: ")});
  Put(R.param(0));
  Put(Val{&R, constStr(*M, "\n")});
  R.callC(Exit, {R.i32(2)});
  FB.unreachable();
  R.finish();
  return PanicUnit;
}

//===----------------------------------------------------------------------===//
// Run-time units (GEN-14, ADR-0025 Amendment)
//===----------------------------------------------------------------------===//

// A unit at run time is a descriptor in memory, self-describing (it does not
// depend on how many base units the program has):
//
//   +0   num    u64 \  the factor, a reduced fraction of two u64; den 0 marks a
//   +8   den    u64 /  factor that left the 64 bits
//   +16  count  u64    the number of base units the unit has (exponent != 0)
//   +24  pairs  { id u64, exp i64 } x count, sorted by id; the id of a base
//                      unit is a 63-bit hash of its qualified name
//                      (Lowering::unitBaseId)
//
// A generic that has a collapse site (Info::UnitSites) takes one pointer to
// such a descriptor for each of its unit parameters; a caller makes the
// callee's from the constants it knows and from its own descriptors with
// unit_mulpow (`U*V` is the descriptor of U times V, `1/U` U to the power -1).
// A site asks unit_site for the factor c(E) of GEN-14 (the factor of E where
// it has no base unit, 1 elsewhere), combined as the site's kind says.
// Everything is exact: fractions are kept reduced. An exponent that would pass
// +-2^20 (UNI-2) marks the descriptor out of range (count ~0, and so every
// product of it); a factor whose numerator or denominator would pass 64 bits
// is marked (denominator 0) and stays so. Nothing panics where a descriptor
// or a site's fraction is made (that is at the entry of the function, whether
// or not the branch runs): unit_site returns 0 / 0 for a site that needs an
// out-of-range unit or a marked factor, and the site that uses it tests the
// mark and calls panic_unit with its own text.

unsigned Lowering::unitK() {
  if (UnitBaseList.empty() && !UnitBasesDone) {
    UnitBasesDone = true;
    sema::unitBaseUnits(*CP.Ctx, UnitBaseList);
  }
  return std::max<size_t>(UnitBaseList.size(), 1);
}

uint64_t Lowering::unitBaseId(const S::Object *O) {
  // FNV-1a over "package path.name", 63 bits (the largest id is the end mark
  // of unit_mulpow's merge).
  uint64_t H = 0xcbf29ce484222325ull;
  auto Mix = [&](StringRef S) {
    for (unsigned char C : S) {
      H ^= C;
      H *= 0x100000001b3ull;
    }
  };
  if (O->Pkg)
    Mix(O->Pkg->Path.str());
  Mix(".");
  Mix(O->Name.str());
  return H & 0x7fffffffffffffffull;
}

Lowering::UnitRtFns *Lowering::unitRt(SourceLoc At) {
  if (UnitRt.Site != NoId)
    return &UnitRt;
  FuncId Panic = ensurePanicUnit(At);
  if (Panic == NoId)
    return nullptr;
  Type *U64 = Ctx.u64(), *I64 = Ctx.i64(), *Str = Ctx.string(),
       *PU8 = Ctx.ptr(Ctx.u8()), *F64 = Ctx.floatTy(64);
  Type *Pair = Ctx.tuple({U64, U64});
  const int64_t MaxExp = 1 << 20;

  // gcd(a, b): Euclid's algorithm on non-zero numbers.
  FuncId Gcd = NoId;
  {
    Function *F = newHelper(*this, "unit_gcd", Ctx.func({U64, U64}, U64),
                            Linkage::LinkOnceODR, Gcd);
    Rt R(*M, *F);
    Var A = R.var(U64, R.param(0)), B = R.var(U64, R.param(1));
    R.While([&] { return B != 0; },
            [&] {
              Val T = R.snap(A % B.v());
              A = B.v();
              B = T;
            });
    R.Return(A.v());
    R.finish();
  }

  // unit_ratmul(n1, d1, n2, d2, what) (n, d): n1/d1 * n2/d2, reduced.
  FuncId RatMul = NoId;
  {
    Function *F = newHelper(*this, "unit_ratmul",
                            Ctx.func({U64, U64, U64, U64, Str}, Pair),
                            Linkage::LinkOnceODR, RatMul);
    Rt R(*M, *F);
    Val N1 = R.param(0), D1 = R.param(1), N2 = R.param(2), D2 = R.param(3);
    // A fraction with the denominator 0 is a factor that left the 64 bits: it
    // stays so, and only a site that needs it panics (unit_site).
    R.If(R.lor(R.snap(D1 == 0), [&] { return R.snap(D2 == 0); }),
         [&] { R.Return(R.tuple(Pair, {R.u(0), R.u(0)})); });
    Val G1 = R.call(Gcd, {N1, D2}), G2 = R.call(Gcd, {N2, D1});
    Val A = R.snap(N1 / G1), B = R.snap(D2 / G1);
    Val C = R.snap(N2 / G2), E = R.snap(D1 / G2);
    Val N = R.snap(A * C), D = R.snap(E * B);
    // (Every part is at least 1, so the product wrapped if it does not divide
    // back.)
    Val Bad = R.lor(R.snap(N / A != C), [&] { return R.snap(D / E != B); });
    R.If(Bad, [&] { R.Return(R.tuple(Pair, {R.u(0), R.u(0)})); });
    R.Return(R.tuple(Pair, {N, D}));
    R.finish();
  }

  // unit_mulpow(dst, cap, a, b, e, what): dst = a * b^e. dst is not a or b;
  // it has room for cap base units.
  FuncId MulPow = NoId;
  {
    Function *F =
        newHelper(*this, "unit_mulpow",
                  Ctx.func({PU8, U64, PU8, PU8, Ctx.i32(), Str}, Ctx.unit()),
                  Linkage::LinkOnceODR, MulPow);
    Rt R(*M, *F);
    Val Dst = R.param(0), Cap = R.param(1), A = R.param(2), B = R.param(3);
    Val E = R.to(I64, R.param(4));
    Val What = R.param(5);
    const uint64_t End = ~uint64_t(0);
    auto Num = [&](Val P) { return R.load(P, U64); };
    auto Den = [&](Val P) { return R.load(R.padd(P, R.u(8)), U64); };
    auto Cnt = [&](Val P) { return R.load(R.padd(P, R.u(16)), U64); };
    auto Id = [&](Val P, Val I) {
      return R.load(R.padd(P, R.u(24) + I * 16), U64);
    };
    auto Ex = [&](Val P, Val I) {
      return R.load(R.padd(P, R.u(32) + I * 16), I64);
    };
    // A descriptor out of range (count End) makes every product of it so.
    auto MarkOut = [&] {
      R.store(Dst, U64, R.u(0));
      R.store(R.padd(Dst, R.u(8)), U64, R.u(0));
      R.store(R.padd(Dst, R.u(16)), U64, R.u(End));
    };
    R.If(R.lor(R.snap(Cnt(A) == End), [&] { return R.snap(Cnt(B) == End); }),
         [&] {
           MarkOut();
           R.Return();
         });
    // The factor: B to the power |e| by squaring, inverted if e < 0.
    Var Pn = R.var(U64, Num(B)), Pd = R.var(U64, Den(B));
    Var Left = R.var(I64, E);
    R.If(R.snap(E < R.i(0)), [&] {
      Left = R.snap(R.i(0) - E);
      Val T = R.snap(Pn.v());
      Pn = Pd.v();
      Pd = T;
    });
    // (A marked factor has the denominator 0; its inverse is marked too.)
    Var Rn = R.var(U64, R.u(1)), Rd = R.var(U64, R.u(1));
    R.While([&] { return Left.v() > R.i(0); },
            [&] {
              R.If(R.snap((Left.v() & R.i(1)) != R.i(0)), [&] {
                Val T = R.call(RatMul, {Rn.v(), Rd.v(), Pn.v(), Pd.v(), What});
                Rn = R.snap(R.fld(T, 0));
                Rd = R.snap(R.fld(T, 1));
              });
              Left = R.snap(Left.v() >> R.i(1));
              R.If(Left.v() > R.i(0), [&] {
                Val T = R.call(RatMul, {Pn.v(), Pd.v(), Pn.v(), Pd.v(), What});
                Pn = R.snap(R.fld(T, 0));
                Pd = R.snap(R.fld(T, 1));
              });
            });
    // (A marked factor whose numerator is 0 stays marked: den 0.)
    R.If(R.snap(Pn.v() == 0), [&] {
      Rn = R.u(0);
      Rd = R.u(0);
    });
    Val T = R.call(RatMul, {Num(A), Den(A), Rn.v(), Rd.v(), What});
    Val N = R.snap(R.fld(T, 0)), D = R.snap(R.fld(T, 1));
    // The base units: the merge of two lists sorted by id.
    Val CA = Cnt(A), CB = Cnt(B);
    Var I = R.var(U64, R.u(0)), J = R.var(U64, R.u(0)), K = R.var(U64, R.u(0));
    Var Out = R.var(R.Bool, R.boolean(false));
    R.While(
        [&] {
          return R.lor(R.snap(I.v() < CA), [&] { return R.snap(J.v() < CB); });
        },
        [&] {
          Var Ia = R.var(U64, R.u(End)), Ib = R.var(U64, R.u(End));
          R.If(R.snap(I.v() < CA), [&] { Ia = Id(A, I.v()); });
          R.If(R.snap(J.v() < CB), [&] { Ib = Id(B, J.v()); });
          Var Cur = R.var(U64, Ia.v());
          R.If(R.snap(Ib.v() < Ia.v()), [&] { Cur = Ib.v(); });
          Var X = R.var(I64, R.i(0));
          R.If(R.snap(Ia.v() == Cur.v()), [&] {
            X = Ex(A, I.v());
            I = R.snap(I.v() + 1);
          });
          R.If(R.snap(Ib.v() == Cur.v()), [&] {
            X = R.snap(X.v() + R.snap(E * Ex(B, J.v())));
            J = R.snap(J.v() + 1);
          });
          // (An exponent past +-2^20, or more base units than the slot has:
          // the descriptor is marked out of range, and the panic comes where
          // a site uses it, not here: a branch that is not taken is no error.)
          R.If(R.snap(R.lor(X.v() > R.i(MaxExp),
                            [&] { return R.snap(X.v() < R.i(-MaxExp)); })),
               [&] { Out = R.boolean(true); });
          R.If(R.snap(R.land(R.snap(X.v() != R.i(0)),
                             [&] { return R.snap(K.v() >= Cap); })),
               [&] { Out = R.boolean(true); });
          R.If(R.snap(R.land(R.snap(X.v() != R.i(0)),
                             [&] { return R.snap(K.v() < Cap); })),
               [&] {
                 R.store(R.padd(Dst, R.u(24) + K.v() * 16), U64, Cur.v());
                 R.store(R.padd(Dst, R.u(32) + K.v() * 16), I64, X.v());
                 K = R.snap(K.v() + 1);
               });
        });
    R.If(Out.v(), [&] {
      MarkOut();
      R.Return();
    });
    R.store(Dst, U64, N);
    R.store(R.padd(Dst, R.u(8)), U64, D);
    R.store(R.padd(Dst, R.u(16)), U64, K.v());
    R.finish();
  }

  // unit_site(kind, res, x, y, sp, sq, what) (n, d): the factor of a collapse
  // site times sp/sq. kind: 0 Mul, 1 Quo, 2 Conv, 3 Const.
  {
    Function *F =
        newHelper(*this, "unit_site",
                  Ctx.func({Ctx.i32(), PU8, PU8, PU8, U64, U64, Str}, Pair),
                  Linkage::LinkOnceODR, UnitRt.Site);
    Rt R(*M, *F);
    Val Kind = R.param(0), What = R.param(6);
    // c(D): its factor where it has no base unit, else 1.
    auto C = [&](Val P, Var &N, Var &D) {
      N = R.u(1);
      D = R.u(1);
      Val Cn = R.load(R.padd(P, R.u(16)), U64);
      R.If(R.lor(R.snap(Cn == 0), [&] { return R.snap(Cn == ~uint64_t(0)); }),
           [&] {
             N = R.load(P, U64);
             D = R.load(R.padd(P, R.u(8)), U64);
           });
    };
    Var Rn = R.var(U64), Rd = R.var(U64), Xn = R.var(U64), Xd = R.var(U64),
        Yn = R.var(U64), Yd = R.var(U64);
    C(R.param(1), Rn, Rd);
    C(R.param(2), Xn, Xd);
    C(R.param(3), Yn, Yd);
    auto Mul = [&](Var &N, Var &D, Val N2, Val D2) {
      Val T = R.call(RatMul, {N.v(), D.v(), N2, D2, What});
      N = R.snap(R.fld(T, 0));
      D = R.snap(R.fld(T, 1));
    };
    // Mul: c(Res) / (c(X) c(Y)); Quo: c(Res) c(Y) / c(X); Conv: c(Res) / c(X).
    // (A marked c has the denominator 0 in its numerator's place after the
    // swap: the product is marked, and so the panic below.)
    R.If(R.snap(Kind == 0), [&] {
      Mul(Rn, Rd, Xd.v(), Xn.v());
      Mul(Rn, Rd, Yd.v(), Yn.v());
    });
    R.If(R.snap(Kind == 1), [&] {
      Mul(Rn, Rd, Yn.v(), Yd.v());
      Mul(Rn, Rd, Xd.v(), Xn.v());
    });
    R.If(R.snap(Kind == 2), [&] { Mul(Rn, Rd, Xd.v(), Xn.v()); });
    Mul(Rn, Rd, R.param(4), R.param(5));
    // (No panic here: a factor out of range comes back as 0 / 0, and the site
    // that uses it panics, unit_site's caller.)
    R.If(
        R.snap(R.lor(R.snap(Rd.v() == 0), [&] { return R.snap(Rn.v() == 0); })),
        [&] { R.Return(R.tuple(Pair, {R.u(0), R.u(0)})); });
    R.Return(R.tuple(Pair, {Rn.v(), Rd.v()}));
    R.finish();
  }

  // unit_mul128(a, b) (hi, lo): the 128-bit product.
  FuncId Mul128 = NoId;
  {
    Function *F = newHelper(*this, "unit_mul128", Ctx.func({U64, U64}, Pair),
                            Linkage::LinkOnceODR, Mul128);
    Rt R(*M, *F);
    Val A = R.param(0), B = R.param(1);
    const uint64_t M32 = 0xffffffffull;
    Val Al = R.snap(A & M32), Ah = R.snap(A >> 32);
    Val Bl = R.snap(B & M32), Bh = R.snap(B >> 32);
    Val LL = R.snap(Al * Bl), LH = R.snap(Al * Bh);
    Val HL = R.snap(Ah * Bl), HH = R.snap(Ah * Bh);
    Val Mid = R.snap(R.snap(LL >> 32) + R.snap(LH & M32) + R.snap(HL & M32));
    Val Lo = R.snap(R.snap(LL & M32) | R.snap(Mid << 32));
    Val Hi =
        R.snap(HH + R.snap(LH >> 32) + R.snap(HL >> 32) + R.snap(Mid >> 32));
    R.Return(R.tuple(Pair, {Hi, Lo}));
    R.finish();
  }

  // unit_divmod128(hi, lo, d) (q, rem): (hi*2^64 + lo) / d, hi < d.
  FuncId DivMod128 = NoId;
  {
    Function *F =
        newHelper(*this, "unit_divmod128", Ctx.func({U64, U64, U64}, Pair),
                  Linkage::LinkOnceODR, DivMod128);
    Rt R(*M, *F);
    Val Lo = R.param(1), D = R.param(2);
    Var Rem = R.var(U64, R.param(0)), Q = R.var(U64, R.u(0));
    Var N = R.var(U64, R.u(64));
    R.While([&] { return R.snap(N.v() > R.u(0)); },
            [&] {
              N = R.snap(N.v() - 1);
              Val Carry = R.snap(Rem.v() >> 63);
              Val Bit = R.snap(R.snap(Lo >> N.v()) & 1);
              Rem = R.snap(R.snap(Rem.v() << 1) | Bit);
              Q = R.snap(Q.v() << 1);
              R.If(R.snap(R.lor(R.snap(Carry != 0),
                                [&] { return R.snap(Rem.v() >= D); })),
                   [&] {
                     Rem = R.snap(Rem.v() - D);
                     Q = R.snap(Q.v() | 1);
                   });
            });
    R.Return(R.tuple(Pair, {Q.v(), Rem.v()}));
    R.finish();
  }

  // unit_muldiv(r, n, d): floor(r * n / d) for r < d, the product in 128 bits.
  FuncId MulDiv = NoId;
  {
    Function *F =
        newHelper(*this, "unit_muldiv", Ctx.func({U64, U64, U64}, U64),
                  Linkage::LinkOnceODR, MulDiv);
    Rt R(*M, *F);
    Val Rr = R.param(0), N = R.param(1), D = R.param(2);
    Val P = R.call(Mul128, {Rr, N});
    Val Hi = R.snap(R.fld(P, 0)), Lo = R.snap(R.fld(P, 1));
    R.If(R.snap(Hi == 0), [&] { R.Return(R.snap(Lo / D)); });
    Val QR = R.call(DivMod128, {Hi, Lo, D});
    R.Return(R.snap(R.fld(QR, 0)));
    R.finish();
  }

  // unit_scale_int(x, signed, n, d): x * n / d truncated toward zero, exact
  // (the result wraps in 64 bits like every integer product): x / d * n plus
  // (x % d) * n / d with the second product in 128 bits.
  {
    Function *F = newHelper(*this, "unit_scale_int",
                            Ctx.func({U64, Ctx.boolean(), U64, U64, Str}, U64),
                            Linkage::LinkOnceODR, UnitRt.ScaleInt);
    Rt R(*M, *F);
    Val X = R.param(0), N = R.param(2), D = R.param(3);
    // (A fraction that left the 64 bits: marked as 0 / 0.)
    R.If(R.lor(R.snap(N == 0), [&] { return R.snap(D == 0); }),
         [&] { R.callNoReturn(Panic, {R.param(4)}); });
    Val Neg = R.land(R.param(1), [&] { return R.snap(R.to(I64, X) < R.i(0)); });
    Var Ax = R.var(U64, X);
    R.If(Neg, [&] { Ax = R.snap(R.u(0) - X); });
    Val Q = R.snap(Ax.v() / D), Rm = R.snap(Ax.v() % D);
    Val Part = R.call(MulDiv, {Rm, N, D});
    Var Res = R.var(U64, R.snap(R.snap(Q * N) + Part));
    R.If(Neg, [&] { Res = R.snap(R.u(0) - Res.v()); });
    R.Return(Res.v());
    R.finish();
  }

  // unit_bitlen(x): the number of bits of x >= 1.
  FuncId BitLen = NoId;
  {
    Function *F = newHelper(*this, "unit_bitlen", Ctx.func({U64}, U64),
                            Linkage::LinkOnceODR, BitLen);
    Rt R(*M, *F);
    Var X = R.var(U64, R.param(0)), N = R.var(U64, R.u(1));
    for (unsigned S : {32, 16, 8, 4, 2, 1})
      R.If(R.snap(R.snap(X.v() >> S) != 0), [&] {
        X = R.snap(X.v() >> S);
        N = R.snap(N.v() + S);
      });
    R.Return(N.v());
    R.finish();
  }

  // unit_pow2(k): 2^k as a double (|k| below 1000).
  FuncId Pow2 = NoId;
  {
    Function *F = newHelper(*this, "unit_pow2", Ctx.func({I64}, F64),
                            Linkage::LinkOnceODR, Pow2);
    Rt R(*M, *F);
    Val K = R.param(0);
    auto Fl = [&](double V) { return Val{&R, constFloat(F64, V)}; };
    Var Left = R.var(I64, K), Base = R.var(F64, Fl(2.0));
    Var Res = R.var(F64, Fl(1.0));
    R.If(R.snap(K < R.i(0)), [&] {
      Left = R.snap(R.i(0) - K);
      Base = Fl(0.5);
    });
    R.While([&] { return R.snap(Left.v() > R.i(0)); },
            [&] {
              R.If(R.snap(R.snap(Left.v() & R.i(1)) != R.i(0)),
                   [&] { Res = R.bin(BinOp::Mul, Res.v(), Base.v()); });
              Base = R.bin(BinOp::Mul, Base.v(), Base.v());
              Left = R.snap(Left.v() >> R.i(1));
            });
    R.Return(Res.v());
    R.finish();
  }

  // unit_ratio(n, d, p): n / d (n, d >= 1) rounded once to p significant bits
  // (to nearest, ties to even) as a double: the factor of a float32 (p = 24)
  // or float64 (p = 53) as the compiler rounds a constant factor.
  {
    Function *F =
        newHelper(*this, "unit_ratio", Ctx.func({U64, U64, U64, Str}, F64),
                  Linkage::LinkOnceODR, UnitRt.Ratio);
    Rt R(*M, *F);
    Val N = R.param(0), D = R.param(1), P = R.param(2);
    // (A factor out of range, 0 / 0, gives 0: the site that uses it panics
    // before it uses the value.)
    R.If(R.lor(R.snap(N == 0), [&] { return R.snap(D == 0); }),
         [&] { R.Return(Val{&R, constFloat(F64, 0.0)}); });
    Val Bn = R.call(BitLen, {N}), Bd = R.call(BitLen, {D});
    // q = n * 2^s / d has p + 2 or p + 3 bits.
    Val S = R.snap(R.snap(R.to(I64, P) + R.i(2)) + R.snap(R.to(I64, Bd)) -
                   R.to(I64, Bn));
    Var Q = R.var(U64, R.u(0)), Sticky = R.var(R.Bool, R.boolean(false));
    R.IfElse(
        R.snap(S >= R.i(0)),
        [&] {
          Var Hi = R.var(U64, R.u(0)), Lo = R.var(U64, R.u(0));
          Val Su = R.to(U64, S);
          R.IfElse(
              R.snap(Su >= 64), [&] { Hi = R.snap(N << R.snap(Su - 64)); },
              [&] {
                R.If(R.snap(Su == 0), [&] { Lo = N; });
                R.If(R.snap(Su != 0), [&] {
                  Hi = R.snap(N >> R.snap(R.u(64) - Su));
                  Lo = R.snap(N << Su);
                });
              });
          Val QR = R.call(DivMod128, {Hi.v(), Lo.v(), D});
          Q = R.snap(R.fld(QR, 0));
          Sticky = R.snap(R.fld(QR, 1) != 0);
        },
        [&] {
          Val T = R.to(U64, R.snap(R.i(0) - S));
          Val N2 = R.snap(N >> T);
          Val Lost = R.snap(N & R.snap(R.snap(R.u(1) << T) - 1));
          Q = R.snap(N2 / D);
          Sticky = R.snap(R.lor(R.snap(R.snap(N2 % D) != 0),
                                [&] { return R.snap(Lost != 0); }));
        });
    // Round q to p bits.
    Val Sh = R.snap(R.call(BitLen, {Q.v()}) - P);
    Val Low = R.snap(Q.v() & R.snap(R.snap(R.u(1) << Sh) - 1));
    Val Half = R.snap(R.u(1) << R.snap(Sh - 1));
    Var Q2 = R.var(U64, R.snap(Q.v() >> Sh));
    Val Up = R.lor(R.snap(Low > Half), [&] {
      return R.land(R.snap(Low == Half), [&] {
        return R.lor(Sticky.v(),
                     [&] { return R.snap(R.snap(Q2.v() & 1) != 0); });
      });
    });
    R.If(Up, [&] { Q2 = R.snap(Q2.v() + 1); });
    Val Pw = R.call(Pow2, {R.snap(R.to(I64, Sh) - S)});
    R.Return(R.bin(BinOp::Mul, R.to(F64, Q2.v()), Pw));
    R.finish();
  }

  UnitRt.RatMul = RatMul;
  UnitRt.MulPow = MulPow;
  return &UnitRt;
}

//===----------------------------------------------------------------------===//
// Equality glue of a checker type
//===----------------------------------------------------------------------===//

FuncId Lowering::eqGlue(S::Type *T, SourceLoc At) {
  if (holdsViewPart(T))
    return viewPartsGlue(T, /*Hash=*/false, At);
  Type *G = lowerType(T, At);
  if (!G)
    return NoId;
  if (!hasGlue(G)) {
    unsupported(At, "comparing a value that holds a slice or a function "
                    "(never comparable)");
    return NoId;
  }
  if (hasIface(G) &&
      (rt(RtFn::IfaceEq, At) == NoId || rt(RtFn::IfaceHash, At) == NoId))
    return NoId;
  std::string Sym = Syms.glue(sym::Kind::EqGlue, T);
  if (Sym.empty())
    Sym = runtimeSymbol("eq_" + G->str());
  return addEqGlue(*M, G, Sym);
}

FuncId Lowering::hashGlue(S::Type *T, SourceLoc At) {
  if (holdsViewPart(T))
    return viewPartsGlue(T, /*Hash=*/true, At);
  Type *G = lowerType(T, At);
  if (!G)
    return NoId;
  if (!hasGlue(G)) {
    unsupported(At, "hashing a value that holds a slice or a function "
                    "(never comparable)");
    return NoId;
  }
  if (hasIface(G) &&
      (rt(RtFn::IfaceEq, At) == NoId || rt(RtFn::IfaceHash, At) == NoId))
    return NoId;
  std::string Sym = Syms.glue(sym::Kind::HashGlue, T);
  if (Sym.empty())
    Sym = runtimeSymbol("hash_" + G->str());
  return addHashGlue(*M, G, Sym);
}

bool Lowering::holdsViewPart(S::Type *T) {
  if (auto It = ViewParts.find(T); It != ViewParts.end())
    return It->second;
  // (A walk with a memo: the types a type holds by value are finite and form
  // no cycle, a by-value cycle being an invalid recursive type.)
  bool R = false;
  S::Type *U = S::under(T);
  if (llvm::isa_and_nonnull<S::RefType>(U))
    R = true;
  else if (auto *ST = llvm::dyn_cast_or_null<S::StructType>(U))
    R = llvm::any_of(ST->Fields,
                     [&](const S::Var *F) { return holdsViewPart(F->Ty); });
  else if (auto *AT = llvm::dyn_cast_or_null<S::ArrayType>(U))
    R = holdsViewPart(AT->Elem);
  else if (auto *OT = llvm::dyn_cast_or_null<S::OptionType>(U))
    R = holdsViewPart(OT->Elem);
  return ViewParts[T] = R;
}

FuncId Lowering::viewPartsGlue(S::Type *T, bool Hash, SourceLoc At) {
  auto Key = std::make_pair(T, unsigned(Hash));
  if (auto It = ViewPartsGlues.find(Key); It != ViewPartsGlues.end())
    return It->second;
  Type *G = lowerType(T, At);
  if (!G)
    return NoId;
  std::string Sym =
      Syms.glue(Hash ? sym::Kind::HashGlue : sym::Kind::EqGlue, T);
  if (Sym.empty())
    Sym = runtimeSymbol((Hash ? "hash_" : "eq_") + G->str());
  if (FuncId Id = M->lookupFunction(Sym); Id != NoId)
    return ViewPartsGlues[Key] = Id;
  Type *PU8 = Ctx.ptr(Ctx.u8()), *U64 = Ctx.u64(), *PG = Ctx.ptr(G);
  auto Fn = std::make_unique<Function>();
  Fn->Name = Sym;
  Fn->Sig = Hash ? Ctx.func({PG, U64}, U64) : Ctx.func({PG, PG}, Ctx.boolean());
  Fn->Link = Linkage::LinkOnceODR;
  Fn->CC = CallConv::Go;
  Function *F = Fn.get();
  FuncId Id = M->addFunction(std::move(Fn));
  ViewPartsGlues[Key] = Id;
  Rt X(*M, *F);
  FuncBuilder &FB = X.FB;
  FB.setSpan(F->Loc);
  Var Ok = X.var(Ctx.boolean(), X.boolean(true), "ok");
  Var H = X.var(U64, Hash ? X.param(1) : X.u(0), "h");
  bool Bad = false;
  // One part: A and B point at it (B unused for a hash). A view's part is
  // the value it looks at: a string's bytes ({ptr, len}), any other value
  // through the pointer; any other part is eqGlue's (or hashGlue's).
  auto Part = [&](Val A, Val B, S::Type *PT) {
    S::Type *PU = S::under(PT);
    if (auto *R = llvm::dyn_cast<S::RefType>(PU)) {
      Type *EG = lowerType(R->Elem, At);
      if (!EG) {
        Bad = true;
        return;
      }
      if (EG->getKind() == Type::Kind::String) {
        auto Ptr = [&](Val P) { return X.load(P, PU8); };
        auto Len = [&](Val P) { return X.load(X.padd(P, X.u(8)), U64); };
        if (!Hash) {
          FuncId StrEq = rt(RtFn::StrEq, At);
          if (StrEq == NoId) {
            Bad = true;
            return;
          }
          X.If(Ok,
               [&] { Ok = X.call(StrEq, {Ptr(A), Len(A), Ptr(B), Len(B)}); });
          return;
        }
        FuncId Hs = hashGlue(CP.Ctx->typ(S::BasicKind::String), At);
        if (Hs == NoId) {
          Bad = true;
          return;
        }
        Var S = X.var(Ctx.string(), "s");
        FB.assign(FB.local(S.L),
                  FB.aggregate(AggKind::String, Ctx.string(),
                               {Ptr(A).Op, Len(A).Op, X.u(0).Op}));
        H = X.call(Hs, {X.addrOf(S), H});
        return;
      }
      FuncId E = Hash ? hashGlue(R->Elem, At) : eqGlue(R->Elem, At);
      if (E == NoId) {
        Bad = true;
        return;
      }
      Type *PE = Ctx.ptr(EG);
      if (Hash)
        H = X.call(E, {X.load(A, PE), H});
      else
        X.If(Ok, [&] { Ok = X.call(E, {X.load(A, PE), X.load(B, PE)}); });
      return;
    }
    FuncId E = Hash ? hashGlue(PT, At) : eqGlue(PT, At);
    if (E == NoId) {
      Bad = true;
      return;
    }
    if (Hash)
      H = X.call(E, {A, H});
    else
      X.If(Ok, [&] { Ok = X.call(E, {A, B}); });
  };
  Val A = X.param(0), B = Hash ? X.param(0) : X.param(1);
  S::Type *U = S::under(T);
  auto Addr = [&](Val P, auto Proj) { return X.addrOf(Proj(X.at(P, G))); };
  if (llvm::isa<S::RefType>(U)) {
    // (A view as the whole value: a map key of a view type.) The parts'
    // pointers are the parameters themselves.
    Part(A, B, T);
  } else if (auto *ST = llvm::dyn_cast<S::StructType>(U)) {
    for (unsigned K = 0; K < ST->Fields.size(); ++K) {
      auto Fld = [&](Place P) { return FB.field(std::move(P), K); };
      Part(Addr(A, Fld), Addr(B, Fld), ST->Fields[K]->Ty);
    }
  } else if (auto *AT = llvm::dyn_cast<S::ArrayType>(U)) {
    Var I = X.var(U64, X.u(0), "i");
    X.While([&] { return X.bin(BinOp::Lt, I, X.u(G->length())); },
            [&] {
              auto Elt = [&](Place P) { return FB.index(std::move(P), I.L); };
              Part(Addr(A, Elt), Addr(B, Elt), AT->Elem);
              I = X.bin(BinOp::Add, I, X.u(1));
            });
  } else if (auto *OT = llvm::dyn_cast<S::OptionType>(U);
             OT && G->getKind() == Type::Kind::Option) {
    // Both clear, or both set and the values equal (a hash: the flag, then
    // the value when set).
    auto Flag = [&](Val P) { return X.val(FB.field(X.at(P, G), 1)); };
    auto Pay = [&](Place P) { return FB.field(std::move(P), 0); };
    if (Hash) {
      Val Set = X.snap(Flag(A));
      H = X.call(hashGlue(CP.Ctx->typ(S::BasicKind::Bool), At),
                 {X.addrOf(FB.field(X.at(A, G), 1)), H});
      X.If(Set, [&] { Part(Addr(A, Pay), Addr(B, Pay), OT->Elem); });
    } else {
      Val FA = X.snap(Flag(A)), FBv = X.snap(Flag(B));
      Ok = X.bin(BinOp::Eq, FA, FBv);
      X.If(Ok, [&] {
        X.If(FA, [&] { Part(Addr(A, Pay), Addr(B, Pay), OT->Elem); });
      });
    }
  } else {
    Bad = true;
  }
  if (Bad) {
    unsupported(At, "comparing this value that holds a view");
    X.Return(Hash ? Val(H) : X.boolean(false));
  } else {
    X.Return(Hash ? Val(H) : Val(Ok));
  }
  X.finish();
  return Id;
}

//===----------------------------------------------------------------------===//
// Complex division (runtime/complex.go)
//===----------------------------------------------------------------------===//

FuncId Lowering::ensureComplexDiv(SourceLoc At) {
  (void)At;
  if (ComplexDiv != NoId)
    return ComplexDiv;
  Type *F64 = Ctx.floatTy(64);
  Type *C128 = Ctx.tuple({F64, F64});
  Function *F = newHelper(*this, "complex128div", Ctx.func({C128, C128}, C128),
                          Linkage::Internal, ComplexDiv);
  Emit E(*this, *F);
  FuncBuilder &FB = E.FB;
  Place N = FB.local(FB.param(0)), Mm = FB.local(FB.param(1));
  LocalId A = E.var(F64, "a"), B = E.var(F64, "b"), Cc = E.var(F64, "c"),
          D = E.var(F64, "d"), Re = E.var(F64, "e"), Im = E.var(F64, "f");
  E.set(A, FB.use(E.cp(FB.field(N, 0))));
  E.set(B, FB.use(E.cp(FB.field(N, 1))));
  E.set(Cc, FB.use(E.cp(FB.field(Mm, 0))));
  E.set(D, FB.use(E.cp(FB.field(Mm, 1))));
  auto Abs = [&](LocalId X) -> Operand {
    LocalId R = FB.newTemp(F64);
    BlockId Neg = FB.newBlock(), Pos = FB.newBlock(), Join = FB.newBlock();
    FB.branch(E.cp(E.bin(BinOp::Lt, E.cl(X), E.f64(0))), Neg, Pos);
    FB.setBlock(Neg);
    E.set(R, FB.unary(UnOp::Neg, E.cl(X)));
    FB.gotoBlock(Join);
    FB.setBlock(Pos);
    E.set(R, FB.use(E.cl(X)));
    FB.gotoBlock(Join);
    FB.setBlock(Join);
    return E.cl(R);
  };
  auto Mul = [&](Operand X, Operand Y) {
    return E.cp(E.bin(BinOp::Mul, X, Y));
  };
  auto Add = [&](Operand X, Operand Y) {
    return E.cp(E.bin(BinOp::Add, X, Y));
  };
  auto Sub = [&](Operand X, Operand Y) {
    return E.cp(E.bin(BinOp::Sub, X, Y));
  };
  auto Quo = [&](Operand X, Operand Y) {
    return E.cp(E.bin(BinOp::Div, X, Y));
  };
  // Smith's algorithm.
  BlockId Wide = FB.newBlock(), Tall = FB.newBlock(), Fix = FB.newBlock();
  Operand AbsC = Abs(Cc), AbsD = Abs(D);
  FB.branch(E.cp(E.bin(BinOp::Ge, AbsC, AbsD)), Wide, Tall);
  FB.setBlock(Wide);
  {
    Place Ratio = E.bin(BinOp::Div, E.cl(D), E.cl(Cc));
    Place Denom = FB.temp(FB.binary(
        BinOp::Add, E.cl(Cc), E.cp(E.bin(BinOp::Mul, E.cp(Ratio), E.cl(D)))));
    E.set(Re,
          FB.use(Quo(Add(E.cl(A), Mul(E.cl(B), E.cp(Ratio))), E.cp(Denom))));
    E.set(Im,
          FB.use(Quo(Sub(E.cl(B), Mul(E.cl(A), E.cp(Ratio))), E.cp(Denom))));
    FB.gotoBlock(Fix);
  }
  FB.setBlock(Tall);
  {
    Place Ratio = E.bin(BinOp::Div, E.cl(Cc), E.cl(D));
    Place Denom = FB.temp(FB.binary(
        BinOp::Add, E.cl(D), E.cp(E.bin(BinOp::Mul, E.cp(Ratio), E.cl(Cc)))));
    E.set(Re,
          FB.use(Quo(Add(Mul(E.cl(A), E.cp(Ratio)), E.cl(B)), E.cp(Denom))));
    E.set(Im,
          FB.use(Quo(Sub(Mul(E.cl(B), E.cp(Ratio)), E.cl(A)), E.cp(Denom))));
    FB.gotoBlock(Fix);
  }
  FB.setBlock(Fix);
  // if isNaN(e) && isNaN(f): correct to infinities and zeros as C99 G.5.1.
  BlockId Both = FB.newBlock(), Done = FB.newBlock(), Check2 = FB.newBlock();
  FB.branch(E.cp(E.bin(BinOp::Ne, E.cl(Re), E.cl(Re))), Check2, Done);
  FB.setBlock(Check2);
  FB.branch(E.cp(E.bin(BinOp::Ne, E.cl(Im), E.cl(Im))), Both, Done);
  FB.setBlock(Both);
  {
    double Inf = INFINITY;
    auto IsNaN = [&](LocalId X) {
      return E.cp(E.bin(BinOp::Ne, E.cl(X), E.cl(X)));
    };
    auto IsInf = [&](LocalId X) -> Operand {
      // x != x false, and x+x == x, and x != 0
      Place Twice = E.bin(BinOp::Add, E.cl(X), E.cl(X));
      Place Same = E.bin(BinOp::Eq, E.cp(Twice), E.cl(X));
      Place NZ = E.bin(BinOp::Ne, E.cl(X), E.f64(0));
      LocalId R = FB.newTemp(Ctx.boolean());
      E.set(R, FB.use(constBool(Ctx, false)));
      BlockId T1 = FB.newBlock(), J = FB.newBlock();
      FB.branch(E.cp(Same), T1, J);
      FB.setBlock(T1);
      E.set(R, FB.use(E.cp(NZ)));
      FB.gotoBlock(J);
      FB.setBlock(J);
      return E.cl(R);
    };
    auto IsFinite = [&](LocalId X) -> Operand {
      // finite: not NaN and not inf
      LocalId R = FB.newTemp(Ctx.boolean());
      E.set(R, FB.use(constBool(Ctx, false)));
      BlockId T1 = FB.newBlock(), J = FB.newBlock();
      FB.branch(E.cp(E.bin(BinOp::Eq, E.cl(X), E.cl(X))), T1, J);
      FB.setBlock(T1);
      E.set(R, FB.unary(UnOp::Not, IsInf(X)));
      FB.gotoBlock(J);
      FB.setBlock(J);
      return E.cl(R);
    };
    // a, b = real(n), imag(n); c, d = real(m), imag(m)
    BlockId C1 = FB.newBlock(), C1Y = FB.newBlock(), C2 = FB.newBlock(),
            C2Y = FB.newBlock(), C3 = FB.newBlock(), C3Y = FB.newBlock();
    // case m == 0 && (!isNaN(a) || !isNaN(b))
    Place MZero = E.bin(BinOp::Eq, E.cl(Cc), E.f64(0));
    LocalId MZ = FB.newTemp(Ctx.boolean());
    E.set(MZ, FB.use(constBool(Ctx, false)));
    {
      BlockId T1 = FB.newBlock(), J = FB.newBlock();
      FB.branch(E.cp(MZero), T1, J);
      FB.setBlock(T1);
      E.set(MZ, FB.use(E.cp(E.bin(BinOp::Eq, E.cl(D), E.f64(0)))));
      FB.gotoBlock(J);
      FB.setBlock(J);
    }
    FB.branch(E.cl(MZ), C1, C2);
    FB.setBlock(C1);
    {
      LocalId Any = FB.newTemp(Ctx.boolean());
      E.set(Any, FB.unary(UnOp::Not, IsNaN(A)));
      BlockId T1 = FB.newBlock(), J = FB.newBlock();
      FB.branch(E.cl(Any), J, T1);
      FB.setBlock(T1);
      E.set(Any, FB.unary(UnOp::Not, IsNaN(B)));
      FB.gotoBlock(J);
      FB.setBlock(J);
      FB.branch(E.cl(Any), C1Y, C2);
    }
    FB.setBlock(C1Y);
    {
      // copysign(inf, c): the sign bit of c (c is +-0 here)
      LocalId Sg = FB.newTemp(F64);
      Place Bits = FB.deref(FB.temp(
          FB.cast(CastKind::PtrToPtr, E.cp(FB.temp(FB.addrOf(FB.local(Cc)))),
                  Ctx.ptr(Ctx.u64()))));
      Place Top = E.bin(BinOp::Shr, E.cp(Bits), E.u64(63));
      BlockId Ng = FB.newBlock(), Ps = FB.newBlock(), J = FB.newBlock();
      FB.branch(E.cp(E.bin(BinOp::Ne, E.cp(Top), E.u64(0))), Ng, Ps);
      FB.setBlock(Ng);
      E.set(Sg, FB.use(E.f64(-Inf)));
      FB.gotoBlock(J);
      FB.setBlock(Ps);
      E.set(Sg, FB.use(E.f64(Inf)));
      FB.gotoBlock(J);
      FB.setBlock(J);
      E.set(Re, FB.binary(BinOp::Mul, E.cl(Sg), E.cl(A)));
      E.set(Im, FB.binary(BinOp::Mul, E.cl(Sg), E.cl(B)));
      FB.gotoBlock(Done);
    }
    FB.setBlock(C2);
    // case (isInf(a) || isInf(b)) && isFinite(c) && isFinite(d)
    {
      LocalId Cond = FB.newTemp(Ctx.boolean());
      E.set(Cond, FB.use(IsInf(A)));
      BlockId T1 = FB.newBlock(), J = FB.newBlock();
      FB.branch(E.cl(Cond), J, T1);
      FB.setBlock(T1);
      E.set(Cond, FB.use(IsInf(B)));
      FB.gotoBlock(J);
      FB.setBlock(J);
      BlockId T2 = FB.newBlock(), T3 = FB.newBlock(), J2 = FB.newBlock();
      FB.branch(E.cl(Cond), T2, J2);
      FB.setBlock(T2);
      E.set(Cond, FB.use(IsFinite(Cc)));
      FB.branch(E.cl(Cond), T3, J2);
      FB.setBlock(T3);
      E.set(Cond, FB.use(IsFinite(D)));
      FB.gotoBlock(J2);
      FB.setBlock(J2);
      FB.branch(E.cl(Cond), C2Y, C3);
    }
    auto Inf2One = [&](LocalId X) {
      // x = copysign(isInf(x) ? 1 : 0, x)
      LocalId R = FB.newTemp(F64);
      BlockId IsI = FB.newBlock(), NotI = FB.newBlock(), J = FB.newBlock();
      FB.branch(IsInf(X), IsI, NotI);
      FB.setBlock(IsI);
      {
        BlockId Ng = FB.newBlock(), Ps = FB.newBlock(), J3 = FB.newBlock();
        FB.branch(E.cp(E.bin(BinOp::Lt, E.cl(X), E.f64(0))), Ng, Ps);
        FB.setBlock(Ng);
        E.set(R, FB.use(E.f64(-1)));
        FB.gotoBlock(J3);
        FB.setBlock(Ps);
        E.set(R, FB.use(E.f64(1)));
        FB.gotoBlock(J3);
        FB.setBlock(J3);
        FB.gotoBlock(J);
      }
      FB.setBlock(NotI);
      {
        // copysign(0, x): +0 or -0 by the sign bit
        Place Bits = FB.deref(FB.temp(
            FB.cast(CastKind::PtrToPtr, E.cp(FB.temp(FB.addrOf(FB.local(X)))),
                    Ctx.ptr(Ctx.u64()))));
        Place Top = E.bin(BinOp::Shr, E.cp(Bits), E.u64(63));
        BlockId Ng = FB.newBlock(), Ps = FB.newBlock(), J3 = FB.newBlock();
        FB.branch(E.cp(E.bin(BinOp::Ne, E.cp(Top), E.u64(0))), Ng, Ps);
        FB.setBlock(Ng);
        E.set(R, FB.unary(UnOp::Neg, E.f64(0)));
        FB.gotoBlock(J3);
        FB.setBlock(Ps);
        E.set(R, FB.use(E.f64(0)));
        FB.gotoBlock(J3);
        FB.setBlock(J3);
        FB.gotoBlock(J);
      }
      FB.setBlock(J);
      E.set(X, FB.use(E.cl(R)));
    };
    FB.setBlock(C2Y);
    Inf2One(A);
    Inf2One(B);
    {
      Operand Ac = Mul(E.cl(A), E.cl(Cc)), Bd = Mul(E.cl(B), E.cl(D));
      E.set(Re, FB.binary(BinOp::Mul, E.f64(Inf), Add(Ac, Bd)));
      Operand Bc = Mul(E.cl(B), E.cl(Cc)), Ad = Mul(E.cl(A), E.cl(D));
      E.set(Im, FB.binary(BinOp::Mul, E.f64(Inf), Sub(Bc, Ad)));
      FB.gotoBlock(Done);
    }
    FB.setBlock(C3);
    // case (isInf(c) || isInf(d)) && isFinite(a) && isFinite(b)
    {
      LocalId Cond = FB.newTemp(Ctx.boolean());
      E.set(Cond, FB.use(IsInf(Cc)));
      BlockId T1 = FB.newBlock(), J = FB.newBlock();
      FB.branch(E.cl(Cond), J, T1);
      FB.setBlock(T1);
      E.set(Cond, FB.use(IsInf(D)));
      FB.gotoBlock(J);
      FB.setBlock(J);
      BlockId T2 = FB.newBlock(), T3 = FB.newBlock(), J2 = FB.newBlock();
      FB.branch(E.cl(Cond), T2, J2);
      FB.setBlock(T2);
      E.set(Cond, FB.use(IsFinite(A)));
      FB.branch(E.cl(Cond), T3, J2);
      FB.setBlock(T3);
      E.set(Cond, FB.use(IsFinite(B)));
      FB.gotoBlock(J2);
      FB.setBlock(J2);
      FB.branch(E.cl(Cond), C3Y, Done);
    }
    FB.setBlock(C3Y);
    Inf2One(Cc);
    Inf2One(D);
    {
      Operand Ac = Mul(E.cl(A), E.cl(Cc)), Bd = Mul(E.cl(B), E.cl(D));
      E.set(Re, FB.binary(BinOp::Mul, E.f64(0), Add(Ac, Bd)));
      Operand Bc = Mul(E.cl(B), E.cl(Cc)), Ad = Mul(E.cl(A), E.cl(D));
      E.set(Im, FB.binary(BinOp::Mul, E.f64(0), Sub(Bc, Ad)));
      FB.gotoBlock(Done);
    }
  }
  FB.setBlock(Done);
  FB.assign(FB.local(FB.returnPlace()),
            FB.aggregate(AggKind::Tuple, C128, {E.cl(Re), E.cl(Im)}));
  FB.ret();
  return ComplexDiv;
}
