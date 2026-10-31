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

/// The counts of the shared handles (P04.09) stay calls: a release has
/// several branches, and one inlined into a prologue that LLVM shrink-wraps
/// made it write a line-table row of file 1 into a DWARF 5 table that has
/// only file 0 (`llvm-dwarfdump --verify`, run/core-opt-matrix at opt 3).
void noInline(Function &F) { F.Inline = Function::InlineHint::Never; }

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
    // (The key and the value own nothing: no drop and no clone glue.)
    Type *DropT = Ctx.func({PU8}, Ctx.unit());
    Type *CloneT = Ctx.func({PU8, PU8}, Ctx.unit());
    auto ZeroOf = [&](Type *T) {
      Operand Z;
      Z.K = Operand::Const;
      Z.C.K = Constant::Zero;
      Z.C.Ty = T;
      return Z;
    };
    FB.assign(Info,
              FB.aggregate(AggKind::Struct, InfoS,
                           {constInt(U64, 16), constInt(U64, 8),
                            constFunc(*M, KeyHash), constFunc(*M, KeyEq),
                            ZeroOf(DropT), ZeroOf(DropT), ZeroOf(CloneT)}));
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
  case RtFn::PanicKeep: {
    if (!Need({"write", "exit"}))
      return NoId;
    Function *F =
        makeFn(*this, "panic_keep", Ctx.func({PU8}, Unit), Slot, true);
    X E(*this, *F);
    E.At = At;
    // TYP-7: a value of a type that cannot be cloned is passed to an
    // interface parameter as a borrow; the callee may not keep it. (A
    // compile error once the callee's "keeps it" bit exists: TODO P04.29.)
    E.write(2, "panic: runtime error: an interface parameter that holds a "
               "borrowed ");
    E.writeStr(2, E.ti(E.param(0), TI::Name));
    E.write(2, " was kept, and the type has no Clone (TYP-7)\n");
    E.die();
    E.finish();
    return Slot;
  }
  case RtFn::IfaceRelease: {
    // The drop glue of an interface value (TYP-5): the value is a borrowed
    // one (the itab's tag bit: nothing is ours), one in the data word (a
    // trivial value, TYP-8: nothing to do), or a box whose count is the word
    // before the value: the last reference drops the value with the
    // descriptor's glue and frees the box. The place is left zero.
    if (!Dep({RtFn::MemFree}))
      return NoId;
    Function *F = makeFn(*this, "iface_release", Ctx.func({PU8}, Unit), Slot);
    noInline(*F);
    X E(*this, *F);
    E.At = At;
    FuncBuilder &FB = E.FB;
    Place IP = E.at(E.param(0), Iface);
    Val Itab = E.ld(FB.field(IP, 0));
    Val Data = E.ld(FB.field(IP, 1));
    FB.assign(IP, FB.use(E.zero(Iface).Op));
    E.If(Itab == E.null(PU8), [&] { E.Return(); });
    E.If((E.to(U64, Itab) & 1) != 0, [&] { E.Return(); });
    Val Ti = E.load(Itab, PU8);
    E.If((E.ld(E.ti(Ti, TI::Flags)) & TI::FlagInline) != 0,
         [&] { E.Return(); });
    E.If(Data == E.null(PU8), [&] { E.Return(); }); // (a value of size 0)
    Val Hdr = E.snap(E.to(PU8, E.to(U64, Data) - TI::BoxHeader));
    Val Rc = E.load(Hdr, U64);
    E.If(Rc > 1, [&] {
      E.store(Hdr, U64, Rc - 1);
      E.Return();
    });
    Val Drop = E.ld(E.ti(Ti, TI::Drop));
    E.If(Drop != E.null(PU8),
         [&] { E.callPtr(Drop, Ctx.func({PU8}, Unit), {Data}); });
    E.rt(RtFn::MemFree, {Hdr});
    E.finish();
    return Slot;
  }
  case RtFn::IfaceClone:
  case RtFn::IfacePass:
  case RtFn::IfaceTake: {
    // A copy of an interface value (OWN-C3): one more reference to its box.
    // A borrowed value (the tag bit, an interface argument made from a value
    // the caller still owns, TYP-7) stays borrowed when it is passed on
    // (IfacePass: the callee's frame ends first); kept anywhere else
    // (IfaceClone) it is cloned into a box of its own, with a count of 1.
    // IfaceTake is a move (the caller zeroes the source): the reference
    // changes hands with no retain, and a borrowed value is cloned as
    // IfaceClone does, so no tagged value outlives its call (P04.09 R1).
    const bool Pass = Which == RtFn::IfacePass;
    const bool Take = Which == RtFn::IfaceTake;
    if (!Dep({RtFn::MemResize, RtFn::PanicKeep}) || !Need({"memmove"}))
      return NoId;
    Function *F = makeFn(*this,
                         Pass   ? "iface_pass"
                         : Take ? "iface_take"
                                : "iface_clone",
                         Ctx.func({PU8, PU8}, Unit), Slot);
    noInline(*F);
    X E(*this, *F);
    E.At = At;
    FuncBuilder &FB = E.FB;
    Place DP = E.at(E.param(0), Iface), SP = E.at(E.param(1), Iface);
    Val Itab = E.ld(FB.field(SP, 0));
    Val Data = E.ld(FB.field(SP, 1));
    FB.assign(DP, FB.use(copyOf(SP)));
    E.If(Itab == E.null(PU8), [&] { E.Return(); });
    Val Bits = E.to(U64, Itab);
    E.If((Bits & 1) != 0, [&] {
      if (Pass)
        E.Return();
      Val Ti = E.load(E.to(PU8, Bits - 1), PU8);
      E.If((E.ld(E.ti(Ti, TI::Flags)) & TI::FlagNoClone) != 0,
           [&] { E.rt(RtFn::PanicKeep, {Ti}); });
      Val Size = E.ld(E.ti(Ti, TI::Size));
      Val Raw = E.rt(RtFn::MemResize,
                     {E.null(PU8), Size + TI::BoxHeader, E.boolean(false)});
      E.store(Raw, U64, E.u(1));
      Val Box = E.snap(E.padd(Raw, E.u(TI::BoxHeader)));
      Val Cl = E.ld(E.ti(Ti, TI::Clone));
      E.IfElse(
          Cl != E.null(PU8),
          [&] { E.callPtr(Cl, Ctx.func({PU8, PU8}, Unit), {Box, Data}); },
          [&] { E.libc("memmove", {Box, Data, Size}); });
      FB.assign(FB.field(DP, 0), FB.use(E.to(PU8, Bits - 1).Op));
      FB.assign(FB.field(DP, 1), FB.use(Box.Op));
      E.Return();
    });
    if (Take) {
      E.finish();
      return Slot;
    }
    Val Ti = E.load(Itab, PU8);
    E.If((E.ld(E.ti(Ti, TI::Flags)) & TI::FlagInline) != 0,
         [&] { E.Return(); });
    E.If(Data == E.null(PU8), [&] { E.Return(); });
    Val Hdr = E.snap(E.to(PU8, E.to(U64, Data) - TI::BoxHeader));
    E.store(Hdr, U64, E.load(Hdr, U64) + 1);
    E.finish();
    return Slot;
  }
  case RtFn::FuncRelease:
  case RtFn::CellRelease: {
    // The end of one reference to a func value's environment or to a closure
    // cell (CLO-2, OWN-C3): both have two words before what the pointer
    // shows, the count and the function that drops the contents (null: none).
    // An environment in the frame (CLO-1) has the immortal count. The place
    // (the func value, or the cell pointer) is left zero.
    const bool Cell = Which == RtFn::CellRelease;
    if (!Dep({RtFn::MemFree}))
      return NoId;
    Function *F = makeFn(*this, Cell ? "cell_release" : "func_release",
                         Ctx.func({PU8}, Unit), Slot);
    noInline(*F);
    X E(*this, *F);
    E.At = At;
    FuncBuilder &FB = E.FB;
    Val Ptr;
    if (Cell) {
      Place PP = E.at(E.param(0), PU8);
      Ptr = E.ld(PP);
      FB.assign(PP, FB.use(E.null(PU8).Op));
    } else {
      Type *FV = Ctx.tuple({PU8, PU8});
      Place FP = E.at(E.param(0), FV);
      Ptr = E.ld(FB.field(FP, 1));
      FB.assign(FP, FB.use(E.zero(FV).Op));
    }
    E.If(Ptr == E.null(PU8), [&] { E.Return(); });
    Val Hdr = E.snap(E.to(PU8, E.to(U64, Ptr) - TI::RcHeader));
    Val Rc = E.load(Hdr, U64);
    E.If(Rc >= TI::Immortal, [&] { E.Return(); });
    E.If(Rc > 1, [&] {
      E.store(Hdr, U64, Rc - 1);
      E.Return();
    });
    Val Drop = E.load(E.padd(Hdr, E.u(8)), PU8);
    E.If(Drop != E.null(PU8),
         [&] { E.callPtr(Drop, Ctx.func({PU8}, Unit), {Ptr}); });
    E.rt(RtFn::MemFree, {Hdr});
    E.finish();
    return Slot;
  }
  case RtFn::PackFree: {
    // A variadic pack lent to a borrowed `...T` parameter: its buffer is the
    // caller's, its elements are borrowed (P04.09). Left zero.
    if (!Dep({RtFn::MemFree}))
      return NoId;
    Function *F = makeFn(*this, "pack_free", Ctx.func({PU8}, Unit), Slot);
    X E(*this, *F);
    E.At = At;
    Place PP = E.at(E.param(0), PU8);
    Val Buf = E.ld(PP);
    E.If(Buf != E.null(PU8), [&] { E.rt(RtFn::MemFree, {Buf}); });
    E.libc("memset", {E.param(0), E.i32(0), E.u(32)});
    E.finish();
    return Slot;
  }
  case RtFn::FuncClone:
  case RtFn::CellRetain: {
    // One more reference (a copy of a func value; an environment that
    // captures a cell).
    const bool Cell = Which == RtFn::CellRetain;
    Function *F =
        Cell ? makeFn(*this, "cell_retain", Ctx.func({PU8}, Unit), Slot)
             : makeFn(*this, "func_clone", Ctx.func({PU8, PU8}, Unit), Slot);
    noInline(*F);
    X E(*this, *F);
    E.At = At;
    FuncBuilder &FB = E.FB;
    Val Ptr;
    if (Cell) {
      Ptr = E.param(0);
    } else {
      Type *FV = Ctx.tuple({PU8, PU8});
      Place DP = E.at(E.param(0), FV), SP = E.at(E.param(1), FV);
      FB.assign(DP, FB.use(copyOf(SP)));
      Ptr = E.ld(FB.field(SP, 1));
    }
    E.If(Ptr == E.null(PU8), [&] { E.Return(); });
    Val Hdr = E.snap(E.to(PU8, E.to(U64, Ptr) - TI::RcHeader));
    Val Rc = E.load(Hdr, U64);
    E.If(Rc < TI::Immortal, [&] { E.store(Hdr, U64, Rc + 1); });
    E.finish();
    return Slot;
  }
  default:
    break;
  }
  return NoId;
}
