//===- RuntimeStr.cpp - The bootstrap runtime: memory, strings ------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Until the Go++ runtime exists (P05), what strings and slices need from it
// is written here in GIR, on top of libc (D4.11 e). Each function is made on
// first use by Lowering::rt and is internal to the module. The calls the
// lowering makes (LowerStr.cpp) are the contract; P05 changes the bodies:
//
//   gopp.mem_resize    THE allocator entry point. Every byte a string, slice
//                      or map owns comes from here (ADR-0027: P05 makes it
//                      mimalloc; today malloc, calloc and realloc). Frees come
//                      with drop elaboration (P04.09), so until then an owned
//                      value that goes out of scope leaks (gir.md section 15).
//   gopp.panic_text    a runtime panic with a fixed text: Go's first line on
//                      standard error, exit status 2.
//   str_*, *_from_*    concatenation, comparison, conversions (Go's rules,
//                      UTF-8 as runtime/utf8.go does it).
//   round_up_size,
//   grow_slice         append's growth, as gc's growslice: the same capacity
//                      for the same element size on both targets, through
//                      Go's malloc size classes (internal/runtime/gc).
//   make_slice         the checks of make([]T, len, cap) and a zeroed buffer.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;

namespace {

/// Go's malloc size classes (internal/runtime/gc/sizeclasses.go).
constexpr uint16_t SizeClasses[] = {
    0,     8,     16,    24,    32,    48,    64,    80,    96,    112,
    128,   144,   160,   176,   192,   208,   224,   240,   256,   288,
    320,   352,   384,   416,   448,   480,   512,   576,   640,   704,
    768,   896,   1024,  1152,  1280,  1408,  1536,  1792,  2048,  2304,
    2688,  3072,  3200,  3456,  4096,  4864,  5376,  6144,  6528,  6784,
    6912,  8192,  9472,  9728,  10240, 10880, 12288, 13568, 14336, 16384,
    18432, 19072, 20480, 21760, 24576, 27264, 28672, 32768};
constexpr uint64_t MaxSmallSize = 32768, MallocHeaderSize = 8,
                   MinSizeForMallocHeader = 512, PageSize = 8192,
                   MaxAlloc = uint64_t(1) << 48;

/// An emitter for a runtime function, with the libc and runtime functions
/// it calls.
struct X : Rt {
  X(Lowering &L, Function &F) : Rt(*L.M, F), L(L) {}
  Lowering &L;
  SourceLoc At;

  Val rt(RtFn Fn, std::initializer_list<Val> Args) {
    return call(L.rt(Fn, At), Args);
  }
  Val libc(StringRef Name, std::initializer_list<Val> Args) {
    return callC(L.libc(Name, At), Args);
  }
  /// The text of a constant string: pointer and length.
  std::pair<Val, Val> text(StringRef T) {
    Place S = FB.temp(FB.use(constStr(M, T)));
    return {val(FB.field(S, 0)), val(FB.field(S, 1))};
  }
  void write(int Fd, StringRef T) {
    auto [P, N] = text(T);
    libc("write", {i32(Fd), P, N});
  }
  void panicText(int Kind, StringRef T) {
    auto [P, N] = text(T);
    callNoReturn(L.rt(RtFn::PanicText, At), {i32(Kind), P, N});
  }
  void retString(Val P, Val Len, Val Cap) {
    FB.assign(
        FB.local(FB.returnPlace()),
        FB.aggregate(AggKind::String, C.string(), {P.Op, Len.Op, Cap.Op}));
    Return();
  }
  void retSlice(Type *Elem, Val P, Val Len, Val Cap) {
    FB.assign(
        FB.local(FB.returnPlace()),
        FB.aggregate(AggKind::Slice, C.slice(Elem), {P.Op, Len.Op, Cap.Op}));
    Return();
  }
  void retTuple(Type *T, Val A, Val B) {
    FB.assign(FB.local(FB.returnPlace()),
              FB.aggregate(AggKind::Tuple, T, {A.Op, B.Op}));
    Return();
  }
};

Function *makeFn(Lowering &L, StringRef Name, Type *Sig, FuncId &Slot,
                 bool NoReturn = false) {
  auto Fn = std::make_unique<Function>();
  Fn->Name = symbolOf(SymbolKind::Runtime, L.CP.Pkg, Name);
  Fn->Sig = Sig;
  Fn->Link = Linkage::Internal;
  Fn->CC = CallConv::Go;
  Fn->NoReturn = NoReturn;
  Fn->NoUnwind = NoReturn; // a fatal panic does not unwind
  Function *F = Fn.get();
  Slot = L.M->addFunction(std::move(Fn));
  return F;
}

} // namespace

//===----------------------------------------------------------------------===//
// libc
//===----------------------------------------------------------------------===//

FuncId Lowering::libc(StringRef Name, SourceLoc At) {
  auto It = LibcIds.find(Name);
  if (It != LibcIds.end())
    return It->second;
  Type *P = Ctx.ptr(Ctx.u8()), *U = Ctx.u64();
  Type *Sig = nullptr;
  StringRef Needs;
  if (Name == "write") {
    LibcIds[Name] = ensureWrite(At);
    return LibcIds[Name];
  } else if (Name == "malloc") {
    Sig = Ctx.func({U}, P);
    Needs = "func malloc(n uint) *byte";
  } else if (Name == "calloc") {
    Sig = Ctx.func({U, U}, P);
    Needs = "func calloc(n uint, size uint) *byte";
  } else if (Name == "realloc") {
    Sig = Ctx.func({P, U}, P);
    Needs = "func realloc(p *byte, n uint) *byte";
  } else if (Name == "free") {
    Sig = Ctx.func({P}, Ctx.unit());
    Needs = "func free(p *byte)";
  } else if (Name == "memset") {
    Sig = Ctx.func({P, Ctx.i32(), U}, P);
    Needs = "func memset(p *byte, c int32, n uint) *byte";
  } else if (Name == "memmove") {
    Sig = Ctx.func({P, P, U}, P);
    Needs = "func memmove(dst *byte, src *byte, n uint) *byte";
  } else if (Name == "memcmp") {
    Sig = Ctx.func({P, P, U}, Ctx.i32());
    Needs = "func memcmp(a *byte, b *byte, n uint) int32";
  } else if (Name == "exit") {
    LibcIds[Name] = ensureExit(At);
    return LibcIds[Name];
  } else {
    return NoId;
  }
  FuncId Id = ensureCFunc(Name, Sig, At, false, "the runtime", Needs);
  LibcIds[Name] = Id;
  return Id;
}

Layout Lowering::layout(Type *T) {
  auto It = Layouts.find(T);
  if (It != Layouts.end())
    return It->second;
  Layout L = layoutOf(T);
  Layouts[T] = L;
  return L;
}

//===----------------------------------------------------------------------===//
// The runtime functions
//===----------------------------------------------------------------------===//

FuncId Lowering::rt(RtFn Which, SourceLoc At) {
  FuncId &Slot = RtIds[unsigned(Which)];
  if (Slot != NoId)
    return Slot;
  Type *U8 = Ctx.u8(), *U64 = Ctx.u64(), *I64 = Ctx.i64(), *I32 = Ctx.i32();
  Type *PU8 = Ctx.ptr(U8), *Bool = Ctx.boolean(), *Str = Ctx.string();
  Type *Unit = Ctx.unit();
  // The C functions and the other runtime functions a body calls are made
  // first, so a failed one stops here.
  auto Need = [&](std::initializer_list<const char *> Names) {
    for (const char *N : Names)
      if (libc(N, At) == NoId)
        return false;
    return true;
  };
  auto Dep = [&](std::initializer_list<RtFn> Fns) {
    for (RtFn F : Fns)
      if (rt(F, At) == NoId)
        return false;
    return true;
  };
  if (Which >= RtFn::MapNew) {
    FuncId Id = buildMapRuntime(Which, At);
    return Id;
  }
  switch (Which) {
  case RtFn::MemResize: {
    if (!Need({"malloc", "calloc", "realloc"}) || !Dep({RtFn::PanicText}))
      return NoId;
    Function *F =
        makeFn(*this, "mem_resize", Ctx.func({PU8, U64, Bool}, PU8), Slot);
    X E(*this, *F);
    E.At = At;
    // p == null: a new block (zeroed on request); else realloc. Size 0 is
    // one byte, so a block is never null (`make([]int, 0)` is not nil).
    Val Sz = E.param(1);
    Var N = E.var(U64, Sz, "n");
    E.If(N == 0, [&] { N = E.u(1); });
    Var R = E.var(PU8, "r");
    E.IfElse(
        E.param(0) == E.null(PU8),
        [&] {
          E.IfElse(
              E.param(2), [&] { R = E.libc("calloc", {E.u(1), N}); },
              [&] { R = E.libc("malloc", {N}); });
        },
        [&] { R = E.libc("realloc", {E.param(0), N}); });
    E.If(R == E.null(PU8), [&] { E.panicText(2, "runtime: out of memory"); });
    E.Return(R);
    E.finish();
    return Slot;
  }
  case RtFn::MemFree: {
    if (!Need({"free"}))
      return NoId;
    Function *F = makeFn(*this, "mem_free", Ctx.func({PU8}, Unit), Slot);
    X E(*this, *F);
    E.At = At;
    E.libc("free", {E.param(0)});
    E.finish();
    return Slot;
  }
  case RtFn::PanicText: {
    if (!Need({"write", "exit"}))
      return NoId;
    Function *F = makeFn(*this, "panic_text", Ctx.func({I32, PU8, U64}, Unit),
                         Slot, true);
    X E(*this, *F);
    E.At = At;
    // kind 0: "panic: runtime error: text", 1: "panic: text", 2: "fatal
    // error: text" (the Go runtime's own failures).
    E.IfElse(
        E.param(0) == 2, [&] { E.write(2, "fatal error: "); },
        [&] {
          E.write(2, "panic: ");
          E.If(E.param(0) == 0, [&] { E.write(2, "runtime error: "); });
        });
    E.libc("write", {E.i32(2), E.param(1), E.param(2)});
    E.write(2, "\n");
    E.callNoReturn(libc("exit", At), {E.i32(2)});
    E.finish();
    return Slot;
  }
  case RtFn::StrConcat: {
    if (!Need({"memmove"}) || !Dep({RtFn::MemResize}))
      return NoId;
    Function *F =
        makeFn(*this, "str_concat", Ctx.func({Ctx.ptr(Str), U64}, Str), Slot);
    X E(*this, *F);
    E.At = At;
    Val Parts = E.param(0), N = E.param(1);
    Var Total = E.var(U64, E.u(0), "total"), K = E.var(U64, E.u(0), "k");
    E.While([&] { return K < N; },
            [&] {
              Val Len = E.load(E.padd(Parts, K * 24 + 8), U64);
              Total = Total + Len;
              K = K + 1;
            });
    E.If(Total == 0, [&] { E.retString(E.null(PU8), E.u(0), E.u(0)); });
    Val Buf = E.rt(RtFn::MemResize, {E.null(PU8), Total, E.boolean(false)});
    Var Off = E.var(U64, E.u(0), "off");
    K = E.u(0);
    E.While([&] { return K < N; },
            [&] {
              Val Sp = E.padd(Parts, K * 24);
              Val P = E.load(Sp, PU8), Len = E.load(E.padd(Sp, E.u(8)), U64);
              E.libc("memmove", {E.padd(Buf, Off), P, Len});
              Off = Off + Len;
              K = K + 1;
            });
    E.retString(Buf, Total, Total);
    E.finish();
    return Slot;
  }
  case RtFn::StrEq: {
    if (!Need({"memcmp"}))
      return NoId;
    Function *F =
        makeFn(*this, "str_eq", Ctx.func({PU8, U64, PU8, U64}, Bool), Slot);
    X E(*this, *F);
    E.At = At;
    Val An = E.param(1);
    E.If(An != E.param(3), [&] { E.Return(E.boolean(false)); });
    E.If(An == 0, [&] { E.Return(E.boolean(true)); });
    E.If(E.param(0) == E.param(2), [&] { E.Return(E.boolean(true)); });
    E.Return(E.libc("memcmp", {E.param(0), E.param(2), An}) == E.i32(0));
    E.finish();
    return Slot;
  }
  case RtFn::StrCmp: {
    if (!Need({"memcmp"}))
      return NoId;
    Function *F =
        makeFn(*this, "str_cmp", Ctx.func({PU8, U64, PU8, U64}, I64), Slot);
    X E(*this, *F);
    E.At = At;
    Val An = E.param(1), Bn = E.param(3);
    Var N = E.var(U64, An, "n");
    E.If(Bn < An, [&] { N = Bn; });
    E.If(N != 0, [&] {
      Val R = E.libc("memcmp", {E.param(0), E.param(2), N});
      E.If(R < E.i32(0), [&] { E.Return(E.i(-1)); });
      E.If(R > E.i32(0), [&] { E.Return(E.i(1)); });
    });
    E.If(An < Bn, [&] { E.Return(E.i(-1)); });
    E.If(An > Bn, [&] { E.Return(E.i(1)); });
    E.Return(E.i(0));
    E.finish();
    return Slot;
  }
  case RtFn::StrFromBytes: {
    if (!Need({"memmove"}) || !Dep({RtFn::MemResize}))
      return NoId;
    Function *F =
        makeFn(*this, "str_from_bytes", Ctx.func({PU8, U64}, Str), Slot);
    X E(*this, *F);
    E.At = At;
    Val N = E.param(1);
    E.If(N == 0, [&] { E.retString(E.null(PU8), E.u(0), E.u(0)); });
    Val Buf = E.rt(RtFn::MemResize, {E.null(PU8), N, E.boolean(false)});
    E.libc("memmove", {Buf, E.param(0), N});
    E.retString(Buf, N, N);
    E.finish();
    return Slot;
  }
  case RtFn::BytesFromStr: {
    if (!Need({"memmove"}) || !Dep({RtFn::MemResize, RtFn::RoundUpSize}))
      return NoId;
    Function *F = makeFn(*this, "bytes_from_str",
                         Ctx.func({PU8, U64}, Ctx.slice(U8)), Slot);
    X E(*this, *F);
    E.At = At;
    Val N = E.param(1);
    // gc's rawbyteslice: the capacity is the size class.
    Val Cap = E.rt(RtFn::RoundUpSize, {N, E.boolean(true)});
    Val Buf = E.rt(RtFn::MemResize, {E.null(PU8), Cap, E.boolean(true)});
    E.If(N != 0, [&] { E.libc("memmove", {Buf, E.param(0), N}); });
    E.retSlice(U8, Buf, N, Cap);
    E.finish();
    return Slot;
  }
  case RtFn::DecodeRune: {
    // runtime/utf8.go decoderune.
    Type *Ret = Ctx.tuple({I32, U64});
    Function *F =
        makeFn(*this, "decode_rune", Ctx.func({PU8, U64, U64}, Ret), Slot);
    X E(*this, *F);
    E.At = At;
    Val P = E.param(0), N = E.param(1), K = E.param(2);
    auto Fail = [&] { E.retTuple(Ret, E.i32(0xFFFD), K + 1); };
    E.If(K >= N, Fail);
    Val B0 = E.load(E.padd(P, K), U8);
    Val Rem = N - K;
    auto Cont = [&](uint64_t Off) {
      Val B = E.load(E.padd(P, K + Off), U8);
      return (B & 0xC0) == 0x80;
    };
    auto Part = [&](uint64_t Off, uint64_t Shift) {
      Val B = E.to(I32, E.load(E.padd(P, K + Off), U8)) & 0x3F;
      return Shift ? (B << Shift) : B;
    };
    // 0080-07FF
    E.If(E.land(B0 >= 0xC0, [&] { return B0 < 0xE0; }), [&] {
      E.If(E.land(Rem > 1, [&] { return Cont(1); }), [&] {
        Val R = ((E.to(I32, B0) & 0x1F) << 6) | Part(1, 0);
        E.If(R > E.i32(0x7F), [&] { E.retTuple(Ret, R, K + 2); });
      });
      Fail();
    });
    // 0800-FFFF
    E.If(E.land(B0 >= 0xE0, [&] { return B0 < 0xF0; }), [&] {
      E.If(E.land(Rem > 2,
                  [&] { return E.land(Cont(1), [&] { return Cont(2); }); }),
           [&] {
             Val R = ((E.to(I32, B0) & 0x0F) << 12) | Part(1, 6) | Part(2, 0);
             Val NotSur =
                 E.lor(R < E.i32(0xD800), [&] { return R > E.i32(0xDFFF); });
             E.If(E.land(R > E.i32(0x7FF), [&] { return NotSur; }),
                  [&] { E.retTuple(Ret, R, K + 3); });
           });
      Fail();
    });
    // 10000-10FFFF
    E.If(E.land(B0 >= 0xF0, [&] { return B0 < 0xF8; }), [&] {
      E.If(E.land(Rem > 3,
                  [&] {
                    return E.land(Cont(1), [&] {
                      return E.land(Cont(2), [&] { return Cont(3); });
                    });
                  }),
           [&] {
             Val R = ((E.to(I32, B0) & 0x07) << 18) | Part(1, 12) | Part(2, 6) |
                     Part(3, 0);
             E.If(E.land(R > E.i32(0xFFFF),
                         [&] { return R <= E.i32(0x10FFFF); }),
                  [&] { E.retTuple(Ret, R, K + 4); });
           });
      Fail();
    });
    Fail();
    E.finish();
    return Slot;
  }
  case RtFn::EncodeRune: {
    // runtime/utf8.go encoderune; an invalid rune is U+FFFD.
    Function *F = makeFn(*this, "encode_rune", Ctx.func({PU8, I32}, U64), Slot);
    X E(*this, *F);
    E.At = At;
    Val Buf = E.param(0);
    Var R = E.var(I32, E.param(1), "r");
    Val Bad = E.lor(R < E.i32(0), [&] {
      return E.lor(R > E.i32(0x10FFFF), [&] {
        return E.land(R >= E.i32(0xD800), [&] { return R <= E.i32(0xDFFF); });
      });
    });
    E.If(Bad, [&] { R = E.i32(0xFFFD); });
    auto Put = [&](uint64_t Off, Val V) {
      E.store(E.padd(Buf, E.u(Off)), U8, E.to(U8, V));
    };
    E.If(R < E.i32(0x80), [&] {
      Put(0, R);
      E.Return(E.u(1));
    });
    E.If(R < E.i32(0x800), [&] {
      Put(0, (R >> 6) | 0xC0);
      Put(1, (R & 0x3F) | 0x80);
      E.Return(E.u(2));
    });
    E.If(R < E.i32(0x10000), [&] {
      Put(0, (R >> 12) | 0xE0);
      Put(1, ((R >> 6) & 0x3F) | 0x80);
      Put(2, (R & 0x3F) | 0x80);
      E.Return(E.u(3));
    });
    Put(0, (R >> 18) | 0xF0);
    Put(1, ((R >> 12) & 0x3F) | 0x80);
    Put(2, ((R >> 6) & 0x3F) | 0x80);
    Put(3, (R & 0x3F) | 0x80);
    E.Return(E.u(4));
    E.finish();
    return Slot;
  }
  case RtFn::RunesFromStr: {
    if (!Dep({RtFn::MemResize, RtFn::RoundUpSize, RtFn::DecodeRune}))
      return NoId;
    Function *F = makeFn(*this, "runes_from_str",
                         Ctx.func({PU8, U64}, Ctx.slice(I32)), Slot);
    X E(*this, *F);
    E.At = At;
    Val P = E.param(0), N = E.param(1);
    // Two passes: count the runes, then decode them (stringtoslicerune).
    Var Count = E.var(U64, E.u(0), "count");
    Var Fill = E.var(U64, E.u(0), "fill");
    Var Buf = E.var(PU8, "buf");
    Var Cap = E.var(U64, E.u(0), "cap");
    for (int Pass = 0; Pass < 2; ++Pass) {
      Var Pos = E.var(U64, E.u(0), "i");
      E.While([&] { return Pos < N; },
              [&] {
                Var R = E.var(I32, E.i32(0));
                E.IfElse(
                    E.load(E.padd(P, Pos), U8) < 0x80,
                    [&] {
                      R = E.to(I32, E.load(E.padd(P, Pos), U8));
                      Pos = Pos + 1;
                    },
                    [&] {
                      Val T = E.rt(RtFn::DecodeRune, {P, N, Pos});
                      R = E.fld(T, 0);
                      Pos = E.fld(T, 1);
                    });
                if (Pass == 0) {
                  Count = Count + 1;
                } else {
                  E.store(E.padd(Buf, Fill * 4), I32, R);
                  Fill = Fill + 1;
                }
              });
      if (Pass == 0) {
        Cap = E.rt(RtFn::RoundUpSize, {Count * 4, E.boolean(true)}) / 4;
        Buf = E.rt(RtFn::MemResize, {E.null(PU8), Cap * 4, E.boolean(true)});
      }
    }
    E.retSlice(I32, E.to(Ctx.ptr(I32), Buf), Count, Cap);
    E.finish();
    return Slot;
  }
  case RtFn::StrFromRunes: {
    if (!Need({"memmove"}) || !Dep({RtFn::MemResize, RtFn::EncodeRune}))
      return NoId;
    Function *F = makeFn(*this, "str_from_runes",
                         Ctx.func({Ctx.ptr(I32), U64}, Str), Slot);
    X E(*this, *F);
    E.At = At;
    Val P = E.param(0), N = E.param(1);
    LocalId Scratch = E.FB.newLocal(Ctx.array(U8, 4), Local::Var, "scratch");
    Val Sp = E.addrOf(E.FB.constIndex(E.FB.local(Scratch), 0));
    Var Total = E.var(U64, E.u(0), "total"), K = E.var(U64, E.u(0), "k");
    E.While([&] { return K < N; },
            [&] {
              Total = Total + E.rt(RtFn::EncodeRune,
                                   {Sp, E.load(E.padd(P, K * 4), I32)});
              K = K + 1;
            });
    E.If(Total == 0, [&] { E.retString(E.null(PU8), E.u(0), E.u(0)); });
    Val Buf = E.rt(RtFn::MemResize, {E.null(PU8), Total, E.boolean(false)});
    Var Off = E.var(U64, E.u(0), "off");
    K = E.u(0);
    E.While([&] { return K < N; },
            [&] {
              Off =
                  Off + E.rt(RtFn::EncodeRune,
                             {E.padd(Buf, Off), E.load(E.padd(P, K * 4), I32)});
              K = K + 1;
            });
    E.retString(Buf, Total, Total);
    E.finish();
    return Slot;
  }
  case RtFn::StrFromRune: {
    if (!Need({"memmove"}) || !Dep({RtFn::MemResize, RtFn::EncodeRune}))
      return NoId;
    Function *F = makeFn(*this, "str_from_rune", Ctx.func({I64}, Str), Slot);
    X E(*this, *F);
    E.At = At;
    // intstring: a value that is no int32 is U+FFFD.
    Val V = E.param(0);
    Var R = E.var(I32, E.i32(0xFFFD), "r");
    Val Narrow = E.to(I32, V);
    E.If(E.to(I64, Narrow) == V, [&] { R = Narrow; });
    LocalId Scratch = E.FB.newLocal(Ctx.array(U8, 4), Local::Var, "scratch");
    Val Sp = E.addrOf(E.FB.constIndex(E.FB.local(Scratch), 0));
    Val N = E.rt(RtFn::EncodeRune, {Sp, R});
    Val Buf = E.rt(RtFn::MemResize, {E.null(PU8), N, E.boolean(false)});
    E.libc("memmove", {Buf, Sp, N});
    E.retString(Buf, N, N);
    E.finish();
    return Slot;
  }
  case RtFn::RoundUpSize: {
    // runtime.roundupsize (Go 1.22 and later: a block that may hold pointers
    // and is over 512 bytes has an 8-byte header the caller does not see).
    Function *F =
        makeFn(*this, "round_up_size", Ctx.func({U64, Bool}, U64), Slot);
    X E(*this, *F);
    E.At = At;
    Val Size = E.param(0);
    E.IfElse(
        Size <= MaxSmallSize - MallocHeaderSize,
        [&] {
          Var Req = E.var(U64, Size, "req");
          E.If(E.land(E.not_(E.param(1)),
                      [&] { return Req > MinSizeForMallocHeader; }),
               [&] { Req = Req + MallocHeaderSize; });
          Val Hdr = Req - Size;
          for (uint16_t C : SizeClasses)
            E.If(Req <= uint64_t(C), [&] { E.Return(E.u(C) - Hdr); });
          E.Return(Size); // not reached: the last class is the largest size
        },
        [&] {
          // A large object: whole pages.
          Val Req = Size + (PageSize - 1);
          E.If(Req < Size, [&] { E.Return(Size); });
          E.Return(Req & ~(PageSize - 1));
        });
    E.finish();
    return Slot;
  }
  case RtFn::GrowSlice: {
    if (!Need({"memmove"}) ||
        !Dep({RtFn::MemResize, RtFn::RoundUpSize, RtFn::PanicText}))
      return NoId;
    Type *Ret = Ctx.tuple({PU8, U64});
    Function *F = makeFn(*this, "grow_slice",
                         Ctx.func({PU8, U64, U64, U64, U64, Bool}, Ret), Slot);
    X E(*this, *F);
    E.At = At;
    Val P = E.param(0), Len = E.param(1), Cap = E.param(2), NewLen = E.param(3),
        Esz = E.param(4);
    // A zero-size element needs no memory.
    E.If(Esz == 0, [&] {
      E.retTuple(Ret,
                 E.rt(RtFn::MemResize, {E.null(PU8), E.u(0), E.boolean(false)}),
                 NewLen);
    });
    // nextslicecap
    Var NewCap = E.var(U64, Cap, "newcap");
    Val Dbl = Cap + Cap;
    E.IfElse(
        NewLen > Dbl, [&] { NewCap = NewLen; },
        [&] {
          E.IfElse(
              Cap < 256, [&] { NewCap = Dbl; },
              [&] {
                Var More = E.var(Bool, E.boolean(true));
                E.While([&] { return More; },
                        [&] {
                          NewCap = NewCap + ((NewCap + 768) >> 2);
                          E.If(NewCap >= NewLen,
                               [&] { More = E.boolean(false); });
                        });
                // newcap overflowed (as an int): the length wanted.
                E.If(E.to(I64, NewCap) <= E.i(0), [&] { NewCap = NewLen; });
              });
        });
    E.If(NewCap > E.u(MaxAlloc) / Esz,
         [&] { E.panicText(0, "growslice: len out of range"); });
    Val CapMem = E.rt(RtFn::RoundUpSize, {NewCap * Esz, E.param(5)});
    NewCap = CapMem / Esz;
    Val Bytes = NewCap * Esz;
    Val Buf = E.rt(RtFn::MemResize, {E.null(PU8), Bytes, E.boolean(true)});
    E.If(Len != 0, [&] { E.libc("memmove", {Buf, P, Len * Esz}); });
    E.retTuple(Ret, Buf, NewCap);
    E.finish();
    return Slot;
  }
  case RtFn::MakeSlice: {
    if (!Dep({RtFn::MemResize, RtFn::PanicText}))
      return NoId;
    Function *F =
        makeFn(*this, "make_slice", Ctx.func({U64, I64, I64}, PU8), Slot);
    X E(*this, *F);
    E.At = At;
    Val Esz = E.param(0), Len = E.param(1), Cap = E.param(2);
    auto Fits = [&](Val N) {
      return E.land(N >= E.i(0), [&] {
        return E.lor(Esz == 0,
                     [&] { return E.to(U64, N) <= E.u(MaxAlloc) / Esz; });
      });
    };
    Val CapOk = Fits(Cap);
    Val Ok = E.land(CapOk, [&] {
      return E.land(Len >= E.i(0), [&] { return Len <= Cap; });
    });
    E.If(E.not_(Ok), [&] {
      E.If(E.not_(Fits(Len)),
           [&] { E.panicText(0, "makeslice: len out of range"); });
      E.panicText(0, "makeslice: cap out of range");
    });
    E.Return(E.rt(RtFn::MemResize,
                  {E.null(PU8), Esz * E.to(U64, Cap), E.boolean(true)}));
    E.finish();
    return Slot;
  }
  default:
    break;
  }
  return NoId;
}
