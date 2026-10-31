//===- RuntimeIface.cpp - The bootstrap runtime: interfaces ---------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What interface values need at run time, written in GIR on top of libc like
// the string and map runtime (RuntimeStr.cpp). The layouts are the contract
// (docs/design/gir.md section 15, "Interfaces"):
//
//   interface value   gopp.iface { itab *u8, data *u8 }. The low bit of itab
//                     is reserved for the tag of a borrowed value (D1.12,
//                     D4.4; TODO P04.09): every reader masks it.
//   itab              [ typeinfo *u8, method 1 *u8, method 2 *u8, ... ] in the
//                     order of the interface's methods sorted by key. The
//                     itab of (T, any) is the descriptor itself: the first
//                     field of a descriptor is its own address.
//   type descriptor   gopp.typeinfo (LowerImpl.h, TI::).
//   method table      NMethods x { key string, fn *u8 }, sorted by key.
//   interface desc    { name string, n u64, n x key string }, keys sorted.
//
//   gopp.get_itab     builds (and caches) the itab of a descriptor for an
//                     interface descriptor at run time: an assertion to an
//                     interface type, a conversion between interface types.
//                     The cache is a map (the map runtime) from the pair to
//                     the itab, or to null when the type lacks a method, so a
//                     pair is built once and memory is bounded by the pairs;
//                     the itabs are never freed. It is not synchronised: P12
//                     makes it safe (gir.md, TODO P12.06).
//   gopp.iface_eq,
//   gopp.iface_hash   Go's == and hash on the dynamic type; an uncomparable
//                     one panics (`comparing uncomparable type []int`).
//   gopp.panic_assert the failure of a type assertion, with Go's message.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;

namespace {

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
  void write(int Fd, StringRef T) {
    Place S = FB.temp(FB.use(constStr(M, T)));
    libc("write", {i32(Fd), val(FB.field(S, 0)), val(FB.field(S, 1))});
  }
  /// The bytes of a string in a place.
  void writeStr(int Fd, const Place &S) {
    libc("write",
         {i32(Fd), snap(val(FB.field(S, 0))), snap(val(FB.field(S, 1)))});
  }
  /// Field K of the descriptor Ti.
  Place ti(Val Ti, unsigned K) { return FB.field(at(Ti, L.tiType()), K); }
  Val ld(const Place &P) { return snap(val(P)); }
  void die() { callNoReturn(L.libc("exit", At), {i32(2)}); }
  /// The pointer Fn, called as a function of type Sig.
  Val callPtr(Val Fn, Type *Sig, std::initializer_list<Val> Args) {
    LocalId FnL = FB.newTemp(Sig);
    FB.assign(FB.local(FnL), FB.cast(CastKind::PtrToPtr, Fn.Op, Sig));
    Rt::SmallVec Ops;
    for (const Val &A : Args)
      Ops.push_back(A.Op);
    LocalId R = FB.newTemp(Sig->ret());
    FB.call(copyOf(FB.local(FnL)), Ops, FB.local(R),
            {UnwindAction::Continue, NoId});
    return val(FB.local(R));
  }
  /// The key K of the interface descriptor Desc (a string place).
  Place descKey(Val Desc, Val K) {
    return at(padd(Desc, K * 24 + 32), C.string());
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
  Fn->NoUnwind = NoReturn;
  Function *F = Fn.get();
  Slot = L.M->addFunction(std::move(Fn));
  return F;
}

} // namespace

uint32_t Lowering::itabCacheGlobal() {
  if (ItabCache != ~0u)
    return ItabCache;
  Global G;
  G.Name = symbolOf(SymbolKind::Runtime, CP.Pkg, "itab_cache");
  G.Ty = mapType(); // made by the first map_assign
  G.Link = Linkage::Internal;
  ItabCache = M->addGlobal(std::move(G));
  return ItabCache;
}

uint32_t Lowering::plainHashFlag() {
  if (PlainHash != ~0u)
    return PlainHash;
  Global G;
  G.Name = symbolOf(SymbolKind::Runtime, CP.Pkg, "plain_hash");
  G.Ty = Ctx.u64();
  G.Link = Linkage::Internal;
  PlainHash = M->addGlobal(std::move(G));
  return PlainHash;
}

FuncId Lowering::buildIfaceRuntime(RtFn Which, SourceLoc At) {
  FuncId &Slot = RtIds[unsigned(Which)];
  Type *U8 = Ctx.u8(), *U64 = Ctx.u64();
  Type *PU8 = Ctx.ptr(U8), *Bool = Ctx.boolean(), *Str = Ctx.string();
  Type *Unit = Ctx.unit(), *Iface = Ctx.iface();
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
  switch (Which) {
  case RtFn::GetItab: {
    if (!Dep({RtFn::MemResize, RtFn::MemFree, RtFn::StrCmp, RtFn::MapGet,
              RtFn::MapAssign}))
      return NoId;
    // The cache is a map from the key {interface descriptor, type
    // descriptor} (two words) to the itab, or to null when the type lacks a
    // method (Go's itabTable keeps both): each pair is built once, so memory
    // is bounded by the pairs a program meets, never by the conversions.
    FuncId KeyHash = NoId, KeyEq = NoId;
    {
      X E(*this,
          *makeFn(*this, "itab_key_hash", Ctx.func({PU8, U64}, U64), KeyHash));
      Val A = E.load(E.param(0), U64),
          B = E.load(E.padd(E.param(0), E.u(8)), U64);
      Val V = E.snap((A ^ (B * 0x9e3779b97f4a7c15ull) ^ E.param(1)) *
                     0xff51afd7ed558ccdull);
      E.Return(V ^ (V >> 33));
      E.finish();
    }
    {
      X E(*this,
          *makeFn(*this, "itab_key_eq", Ctx.func({PU8, PU8}, Bool), KeyEq));
      Val A = E.param(0), B = E.param(1);
      E.Return(E.land(E.load(A, U64) == E.load(B, U64), [&] {
        return E.load(E.padd(A, E.u(8)), U64) == E.load(E.padd(B, E.u(8)), U64);
      }));
      E.finish();
    }
    Function *F = makeFn(*this, "get_itab", Ctx.func({PU8, PU8}, PU8), Slot);
    X E(*this, *F);
    E.At = At;
    FuncBuilder &FB = E.FB;
    Type *Ent = mtEntType(), *InfoS = mapInfoType(), *MapPtr = mapType();
    Val Desc = E.param(0), Ti = E.param(1);
    Type *KeyT = Ctx.tuple({PU8, PU8});
    Place Key = FB.local(FB.newTemp(KeyT));
    FB.assign(Key, FB.aggregate(AggKind::Tuple, KeyT, {Desc.Op, Ti.Op}));
    Place Info = FB.local(FB.newTemp(InfoS));
    FB.assign(Info,
              FB.aggregate(AggKind::Struct, InfoS,
                           {constInt(U64, 16), constInt(U64, 8),
                            constFunc(*M, KeyHash), constFunc(*M, KeyEq)}));
    Val InfoP = E.addrOf(Info);
    Val KeyP = E.to(PU8, E.addrOf(Key));
    Val CacheP = E.addrOf(FB.global(itabCacheGlobal()));
    Val Hit = E.rt(RtFn::MapGet, {InfoP, E.load(CacheP, MapPtr), KeyP});
    E.If(Hit != E.null(PU8), [&] { E.Return(E.load(Hit, PU8)); });
    // Remember R for the pair, and return it.
    auto Done = [&](Val R) {
      Val SlotP = E.rt(RtFn::MapAssign, {InfoP, CacheP, KeyP});
      E.store(SlotP, PU8, R);
      E.Return(R);
    };
    Val N = E.load(E.padd(Desc, E.u(24)), U64);
    Val Nm = E.ld(E.ti(Ti, TI::NMethods));
    Val Mt = E.ld(E.ti(Ti, TI::Methods));
    Val It = E.snap(
        E.rt(RtFn::MemResize, {E.null(PU8), (N + 1) * 8, E.boolean(false)}));
    E.store(It, PU8, Ti);
    // One walk of the two key-sorted lists; a key the type lacks: null.
    auto Lacks = [&] {
      E.rt(RtFn::MemFree, {It});
      Done(E.null(PU8));
    };
    Var K = E.var(U64, E.u(0), "k"), J = E.var(U64, E.u(0), "j");
    E.While([&] { return K < N; },
            [&] {
              Place DK = E.descKey(Desc, K);
              Val KP = E.ld(FB.field(DK, 0)), KL = E.ld(FB.field(DK, 1));
              E.While([&] { return E.boolean(true); },
                      [&] {
                        E.If(J >= Nm, Lacks);
                        Place En = E.at(E.padd(Mt, J * 32), Ent);
                        Place EK = FB.field(En, 0);
                        Val C =
                            E.rt(RtFn::StrCmp, {E.ld(FB.field(EK, 0)),
                                                E.ld(FB.field(EK, 1)), KP, KL});
                        E.If(C == 0, [&] {
                          E.store(E.padd(It, (K + 1) * 8), PU8,
                                  E.ld(FB.field(En, 1)));
                          J = J + 1;
                          E.Break();
                        });
                        E.If(C > 0, Lacks);
                        J = J + 1;
                      });
              K = K + 1;
            });
    Done(It);
    E.finish();
    return Slot;
  }
  case RtFn::PanicUncomparable: {
    if (!Need({"write", "exit"}))
      return NoId;
    Function *F = makeFn(*this, "panic_uncomparable",
                         Ctx.func({PU8, Bool}, Unit), Slot, true);
    X E(*this, *F);
    E.At = At;
    // (gopp.map_key_check sets the flag: Go's nil or empty map access says
    // "hash of unhashable type: T", every other hash and == the runtime
    // error.)
    Val Plain = E.load(Val{&E, constGlobal(*M, plainHashFlag())}, Ctx.u64());
    E.IfElse(
        E.land(E.param(1), [&] { return Plain != 0; }),
        [&] { E.write(2, "panic: hash of unhashable type: "); },
        [&] {
          E.write(2, "panic: runtime error: ");
          E.IfElse(
              E.param(1), [&] { E.write(2, "hash of unhashable type "); },
              [&] { E.write(2, "comparing uncomparable type "); });
        });
    E.writeStr(2, E.ti(E.param(0), TI::Name));
    E.write(2, "\n");
    E.die();
    E.finish();
    return Slot;
  }
  case RtFn::IfaceEq:
  case RtFn::IfaceHash: {
    bool Eq = Which == RtFn::IfaceEq;
    if (!Dep({RtFn::PanicUncomparable}) || !Need({"exit"}))
      return NoId;
    Function *F =
        Eq ? makeFn(*this, "iface_eq",
                    Ctx.func({Ctx.ptr(Iface), Ctx.ptr(Iface)}, Bool), Slot)
           : makeFn(*this, "iface_hash", Ctx.func({Ctx.ptr(Iface), U64}, U64),
                    Slot);
    X E(*this, *F);
    E.At = At;
    FuncBuilder &FB = E.FB;
    // The descriptor of the dynamic type, or null.
    auto Dyn = [&](Place IP) {
      Val Itab = E.ld(FB.field(IP, 0));
      Var R = E.var(PU8, "ti");
      R = E.null(PU8);
      E.If(Itab != E.null(PU8),
           [&] { R = E.load(E.to(PU8, E.to(U64, Itab) & ~uint64_t(1)), PU8); });
      return E.snap(R);
    };
    // The address of the value: the data word itself when it is inline.
    auto ValuePtr = [&](Val T, Place IP) {
      Var R = E.var(PU8, "p");
      R = E.ld(FB.field(IP, 1));
      E.If((E.ld(E.ti(T, TI::Flags)) & 1) != 0,
           [&] { R = E.to(PU8, E.addrOf(FB.field(IP, 1))); });
      return E.snap(R);
    };
    Place A = E.at(E.param(0), Iface);
    Val Ta = Dyn(A);
    if (Eq) {
      Place B = E.at(E.param(1), Iface);
      Val Tb = Dyn(B);
      E.If(Ta != Tb, [&] { E.Return(E.boolean(false)); });
      E.If(Ta == E.null(PU8), [&] { E.Return(E.boolean(true)); });
      Val Fn = E.ld(E.ti(Ta, TI::Eq));
      E.If(Fn == E.null(PU8), [&] {
        E.callNoReturn(rt(RtFn::PanicUncomparable, At), {Ta, E.boolean(false)});
      });
      Val Pa = ValuePtr(Ta, A), Pb = ValuePtr(Ta, B);
      E.Return(E.callPtr(Fn, Ctx.func({PU8, PU8}, Bool), {Pa, Pb}));
    } else {
      E.If(Ta == E.null(PU8), [&] { E.Return(E.param(1)); });
      Val Fn = E.ld(E.ti(Ta, TI::Hash));
      E.If(Fn == E.null(PU8), [&] {
        E.callNoReturn(rt(RtFn::PanicUncomparable, At), {Ta, E.boolean(true)});
      });
      Val Pa = ValuePtr(Ta, A);
      E.Return(E.callPtr(Fn, Ctx.func({PU8, U64}, U64), {Pa, E.param(1)}));
    }
    E.finish();
    return Slot;
  }
  case RtFn::PanicAssert: {
    if (!Need({"write", "exit"}) || !Dep({RtFn::StrCmp}))
      return NoId;
    Function *F = makeFn(*this, "panic_assert",
                         Ctx.func({Str, PU8, Str, PU8}, Unit), Slot, true);
    F->NoUnwind = false; // (it calls str_cmp; it never returns either way)
    X E(*this, *F);
    E.At = At;
    FuncBuilder &FB = E.FB;
    Type *Ent = mtEntType();
    Place Inter = E.paramPlace(0), Asserted = E.paramPlace(2);
    Val Dyn = E.param(1), Desc = E.param(3);
    E.write(2, "panic: interface conversion: ");
    E.IfElse(
        Dyn == E.null(PU8),
        [&] {
          E.writeStr(2, Inter);
          E.write(2, " is nil, not ");
          E.writeStr(2, Asserted);
        },
        [&] {
          E.IfElse(
              Desc == E.null(PU8),
              [&] {
                E.writeStr(2, Inter);
                E.write(2, " is ");
                E.writeStr(2, E.ti(Dyn, TI::Name));
                E.write(2, ", not ");
                E.writeStr(2, Asserted);
              },
              [&] {
                // The first method of the interface the type lacks.
                E.writeStr(2, E.ti(Dyn, TI::Name));
                E.write(2, " is not ");
                E.writeStr(2, Asserted);
                E.write(2, ": missing method ");
                Val N = E.load(E.padd(Desc, E.u(24)), U64);
                Val Nm = E.ld(E.ti(Dyn, TI::NMethods));
                Val Mt = E.ld(E.ti(Dyn, TI::Methods));
                Var K = E.var(U64, E.u(0), "k"), J = E.var(U64, E.u(0), "j");
                E.While(
                    [&] { return K < N; },
                    [&] {
                      Place Key = E.descKey(Desc, K);
                      Val KP = E.ld(FB.field(Key, 0)),
                          KL = E.ld(FB.field(Key, 1));
                      Var Present = E.var(Bool, E.boolean(false), "present");
                      E.While([&] { return J < Nm; },
                              [&] {
                                Place En = E.at(E.padd(Mt, J * 32), Ent);
                                Place EK = FB.field(En, 0);
                                Val C = E.rt(RtFn::StrCmp,
                                             {E.ld(FB.field(EK, 0)),
                                              E.ld(FB.field(EK, 1)), KP, KL});
                                E.If(C == 0, [&] {
                                  Present = E.boolean(true);
                                  J = J + 1;
                                  E.Break();
                                });
                                E.If(C > 0, [&] { E.Break(); });
                                J = J + 1;
                              });
                      E.If(E.not_(Present), [&] {
                        // The name: the key up to the first 0x01 byte.
                        Var Len = E.var(U64, E.u(0), "len");
                        E.While(
                            [&] {
                              return E.land(Len < KL, [&] {
                                return E.load(E.padd(KP, Len), U8) != E.b8(1);
                              });
                            },
                            [&] { Len = Len + 1; });
                        E.libc("write", {E.i32(2), KP, Len});
                        E.write(2, "\n");
                        E.die();
                      });
                      K = K + 1;
                    });
              });
        });
    E.write(2, "\n");
    E.die();
    E.finish();
    return Slot;
  }
  default:
    break;
  }
  return NoId;
}
