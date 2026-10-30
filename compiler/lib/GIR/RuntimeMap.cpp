//===- RuntimeMap.cpp - The temporary map runtime -------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A map for the bootstrap, written in GIR, until P05.06 replaces it with the
// Swiss table. P05.06 keeps the INTERFACE below (the calls that LowerStr.cpp
// emits), not the data structure. The interface is the one the spec fixes:
//
//   - every write passes the ADDRESS of the variable that holds the map
//     (`**Map`), because the first write to a nil map creates it there
//     (ADR-0018 amendment point 5: no nil-map panic); a read passes the map;
//   - the runtime does not know K and V: a `MapInfo` {key size, value size,
//     hash, eq} built by the caller describes them; keys and values are
//     passed and returned by address;
//   - `len(m)` is the first word of the object, so it is a load (nil: 0);
//   - an iterator is a small struct in the caller's frame. The map counts its
//     running iterators (RNG-4): map_iter_init adds one, and the iteration
//     ends once, when map_iter_next runs out or when map_iter_done runs (the
//     lowering calls it on every way out of the loop: break, continue or
//     break of an outer loop, return, goto). Inserting a new key while the
//     count is not zero panics with "map insert during iteration", whatever
//     the size of the table; writing a key that is there is no insert;
//   - the map-entry arguments of a call are records in the caller's frame:
//     map_entry_args copies them in and resolves which are the same entry,
//     map_entry_back copies out after the call (gir.md section 15).
//     (A panic does not unwind until P04.07, which ends the iteration on the
//     unwind path too.)
//
// The object: count, then a dense array of entries in insertion order (hash
// word, key, value, each part 8-byte aligned; a hash word of 0 is a deleted
// entry) and an open-addressing index of entry numbers. Iteration walks the
// entries that were there when it began, from a random one, around to the
// one before it (spec chapter 16: the order is random, as in Go; a new
// pseudo-random start for every range, from a state seeded once per run
// with a stack address). Deleting during a `range` is safe, an entry is seen
// once, and a deleted one is not seen. Tests must not rely on the order.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;

namespace {

constexpr uint64_t LiveBit = uint64_t(1) << 63;
constexpr uint64_t HashSeed = 0x2545F4914F6CDD1Dull;

} // namespace

Type *Lowering::mapType() {
  Type *S = Ctx.structTy("gopp.map");
  if (!S->hasBody()) {
    Type *U = Ctx.u64(), *P = Ctx.ptr(Ctx.u8());
    // count, nents, ecap, icap, entries, index, iters (running iterations)
    Ctx.setStructBody(S, {U, U, U, U, P, P, U});
  }
  return Ctx.ptr(S);
}

Type *Lowering::mapInfoType() {
  Type *S = Ctx.structTy("gopp.mapinfo");
  if (!S->hasBody()) {
    Type *U = Ctx.u64(), *P = Ctx.ptr(Ctx.u8());
    Ctx.setStructBody(
        S, {U, U, Ctx.func({P, U}, U), Ctx.func({P, P}, Ctx.boolean())});
  }
  return S;
}

Type *Lowering::mapEntryArgType() {
  Type *S = Ctx.structTy("gopp.mapentryarg");
  if (!S->hasBody()) {
    Type *U = Ctx.u64(), *P = Ctx.ptr(Ctx.u8()), *B = Ctx.boolean();
    // info, the map object, the key, the key kind (its eq glue's id), the
    // entry's map (copied in), the map the callee gets (an earlier equal
    // entry's), the entry was there and nil, this one copies out
    Ctx.setStructBody(S, {Ctx.ptr(mapInfoType()), mapType(), P, U, mapType(),
                          Ctx.ptr(mapType()), B, B});
  }
  return S;
}

Type *Lowering::mapIterType() {
  Type *S = Ctx.structTy("gopp.mapiter");
  if (!S->hasBody()) {
    Type *U = Ctx.u64();
    // the map (null once the iteration ended), the steps taken, the entries
    // there were at the start, the entry it began at
    Ctx.setStructBody(S, {mapType(), U, U, U});
  }
  return S;
}

std::pair<FuncId, FuncId> Lowering::mapKeyFns(S::Type *K, SourceLoc At) {
  Type *G = lowerType(K, At);
  if (!G)
    return {NoId, NoId};
  auto It = KeyFns.find(G);
  if (It != KeyFns.end())
    return It->second;
  FuncId Hash = hashGlue(K, At), Eq = eqGlue(K, At);
  if (Hash == NoId || Eq == NoId)
    return {NoId, NoId};
  Type *U = Ctx.u64(), *P = Ctx.ptr(Ctx.u8()), *B = Ctx.boolean();
  std::pair<FuncId, FuncId> R;
  unsigned N = KeyFns.size();
  for (int Which = 0; Which < 2; ++Which) {
    auto Fn = std::make_unique<Function>();
    Fn->Name = symbolOf(SymbolKind::Runtime, CP.Pkg,
                        std::string(Which ? "key_eq." : "key_hash.") +
                            std::to_string(N));
    Fn->Sig = Which ? Ctx.func({P, P}, B) : Ctx.func({P, U}, U);
    Fn->Link = Linkage::Internal;
    Fn->CC = CallConv::Go;
    Function *F = Fn.get();
    (Which ? R.second : R.first) = M->addFunction(std::move(Fn));
    Rt E(*M, *F);
    Type *PG = Ctx.ptr(G);
    if (Which) {
      E.Return(E.call(Eq, {E.to(PG, E.param(0)), E.to(PG, E.param(1))}));
    } else {
      E.Return(E.call(Hash, {E.to(PG, E.param(0)), E.param(1)}));
    }
    E.finish();
  }
  KeyFns[G] = R;
  return R;
}

namespace {

/// An emitter that knows the layout of the map object and of MapInfo.
struct MapX : Rt {
  MapX(Lowering &L, Function &F)
      : Rt(*L.M, F), L(L), MapPtr(L.mapType()), MapS(MapPtr->elem()),
        InfoS(L.mapInfoType()) {}
  Lowering &L;
  SourceLoc At;
  Type *MapPtr, *MapS, *InfoS;

  Val rt(RtFn Fn, std::initializer_list<Val> Args) {
    return call(L.rt(Fn, At), Args);
  }
  Val libc(StringRef Name, std::initializer_list<Val> Args) {
    return callC(L.libc(Name, At), Args);
  }
  void panicText(int Kind, StringRef T) {
    Place S = FB.temp(FB.use(constStr(M, T)));
    callNoReturn(L.rt(RtFn::PanicText, At),
                 {i32(Kind), val(FB.field(S, 0)), val(FB.field(S, 1))});
  }
  /// Field K of the map object / of the info.
  Place mf(Val M_, unsigned K) { return FB.field(at(M_, MapS), K); }
  Place inf(Val I, unsigned K) { return FB.field(at(I, InfoS), K); }
  Val get(Val M_, unsigned K) { return snap(val(mf(M_, K))); }
  void set(Val M_, unsigned K, Val V) { FB.assign(mf(M_, K), FB.use(V.Op)); }
  Val align8(Val N) { return (N + 7) & ~uint64_t(7); }
  /// The layout of an entry: key offset 8, value offset 8 + ka, size esz.
  struct Ent {
    Val Ka, Va, Esz;
  };
  Ent ent(Val Info) {
    Val Ka = align8(snap(val(inf(Info, 0)))),
        Va = align8(snap(val(inf(Info, 1))));
    return {Ka, Va, Ka + Va + 8};
  }
  Val callIndirect(Val Fn, std::initializer_list<Val> Args) {
    SmallVec Ops;
    for (const Val &A : Args)
      Ops.push_back(A.Op);
    LocalId R = FB.newTemp(Fn.ty()->ret());
    FB.call(Fn.Op, Ops, FB.local(R), {UnwindAction::Continue, NoId});
    return {this, copyOf(FB.local(R))};
  }
  Val hashOf(Val Info, Val Key) {
    Val H = callIndirect(snap(val(inf(Info, 2))), {Key, u(HashSeed)});
    return H | LiveBit;
  }
};

} // namespace

FuncId Lowering::buildMapRuntime(RtFn Which, SourceLoc At) {
  FuncId &Slot = RtIds[unsigned(Which)];
  Type *U64 = Ctx.u64(), *Bool = Ctx.boolean(), *Unit = Ctx.unit();
  Type *PU8 = Ctx.ptr(Ctx.u8());
  Type *MapPtr = mapType(), *InfoPtr = Ctx.ptr(mapInfoType());
  Type *IterPtr = Ctx.ptr(mapIterType());
  auto Dep = [&](std::initializer_list<RtFn> Fns) {
    for (RtFn F : Fns)
      if (rt(F, At) == NoId)
        return false;
    return true;
  };
  auto Need = [&](std::initializer_list<const char *> Names) {
    for (const char *N : Names)
      if (libc(N, At) == NoId)
        return false;
    return true;
  };
  auto Make = [&](const char *Name, Type *Sig) {
    auto Fn = std::make_unique<Function>();
    Fn->Name = symbolOf(SymbolKind::Runtime, CP.Pkg, Name);
    Fn->Sig = Sig;
    Fn->Link = Linkage::Internal;
    Fn->CC = CallConv::Go;
    Function *F = Fn.get();
    Slot = M->addFunction(std::move(Fn));
    return F;
  };
  switch (Which) {
  case RtFn::MapNew: {
    if (!Dep({RtFn::MemResize}))
      return NoId;
    MapX E(*this, *Make("map_new", Ctx.func({InfoPtr, U64}, MapPtr)));
    E.At = At;
    // The tables are made by the first insert; hint only reserves nothing.
    Val Obj =
        E.rt(RtFn::MemResize,
             {E.null(PU8), E.u(layout(MapPtr->elem()).Size), E.boolean(true)});
    E.Return(E.to(MapPtr, Obj));
    E.finish();
    return Slot;
  }
  case RtFn::MapFind: {
    // The entry with hash H and key Key, or null.
    MapX E(*this,
           *Make("map_find", Ctx.func({InfoPtr, MapPtr, PU8, U64}, PU8)));
    E.At = At;
    Val Info = E.param(0), M_ = E.param(1), Key = E.param(2), H = E.param(3);
    Val Icap = E.get(M_, 3);
    E.If(Icap == 0, [&] { E.Return(E.null(PU8)); });
    auto En = E.ent(Info);
    Val Mask = Icap - 1, Idx = E.get(M_, 5), Ents = E.get(M_, 4);
    Var I = E.var(U64, H & Mask, "i");
    E.While([&] { return E.boolean(true); },
            [&] {
              Val Slot_ = E.load(E.padd(Idx, I * 8), U64);
              E.If(Slot_ == 0, [&] { E.Return(E.null(PU8)); });
              Val Ep = E.padd(Ents, (Slot_ - 1) * En.Esz);
              Val Same = E.land(E.load(Ep, U64) == H, [&] {
                return E.callIndirect(E.snap(E.val(E.inf(Info, 3))),
                                      {E.padd(Ep, E.u(8)), Key});
              });
              E.If(Same, [&] { E.Return(Ep); });
              I = (I + 1) & Mask;
            });
    E.Return(E.null(PU8));
    E.finish();
    return Slot;
  }
  case RtFn::MapGrow: {
    if (!Dep({RtFn::MemResize, RtFn::MemFree}) || !Need({"memmove"}))
      return NoId;
    MapX E(*this, *Make("map_grow", Ctx.func({InfoPtr, MapPtr}, Unit)));
    E.At = At;
    Val Info = E.param(0), M_ = E.param(1);
    auto En = E.ent(Info);
    Val Ecap = E.get(M_, 2), Live = E.get(M_, 0), Nents = E.get(M_, 1);
    // First table: 8 entries; half or more dead: compact in place of
    // doubling; else double.
    Var NewCap = E.var(U64, Ecap * 2, "newcap");
    E.If(Ecap == 0, [&] { NewCap = E.u(8); });
    E.If(E.land(Ecap != 0, [&] { return Live * 2 <= Ecap; }),
         [&] { NewCap = Ecap; });
    Val NewEnts =
        E.rt(RtFn::MemResize, {E.null(PU8), NewCap * En.Esz, E.boolean(true)});
    Val NewIdx =
        E.rt(RtFn::MemResize, {E.null(PU8), NewCap * 16, E.boolean(true)});
    Val Mask = NewCap * 2 - 1, OldEnts = E.get(M_, 4);
    Var N = E.var(U64, E.u(0), "n"), K = E.var(U64, E.u(0), "k");
    E.While([&] { return K < Nents; },
            [&] {
              Val Ep = E.padd(OldEnts, K * En.Esz);
              Val H = E.load(Ep, U64);
              E.If(H != 0, [&] {
                E.libc("memmove", {E.padd(NewEnts, N * En.Esz), Ep, En.Esz});
                Var I = E.var(U64, H & Mask, "i");
                E.While([&] { return E.load(E.padd(NewIdx, I * 8), U64) != 0; },
                        [&] { I = (I + 1) & Mask; });
                E.store(E.padd(NewIdx, I * 8), U64, N + 1);
                N = N + 1;
              });
              K = K + 1;
            });
    E.If(OldEnts != E.null(PU8), [&] {
      E.rt(RtFn::MemFree, {OldEnts});
      E.rt(RtFn::MemFree, {E.get(M_, 5)});
    });
    E.set(M_, 4, NewEnts);
    E.set(M_, 5, NewIdx);
    E.set(M_, 2, NewCap);
    E.set(M_, 3, NewCap * 2);
    E.set(M_, 1, N);
    E.finish();
    return Slot;
  }
  case RtFn::MapGet: {
    if (!Dep({RtFn::MapFind}))
      return NoId;
    MapX E(*this, *Make("map_get", Ctx.func({InfoPtr, MapPtr, PU8}, PU8)));
    E.At = At;
    Val Info = E.param(0), M_ = E.param(1), Key = E.param(2);
    E.If(M_ == E.null(MapPtr), [&] { E.Return(E.null(PU8)); });
    E.If(E.get(M_, 0) == 0, [&] { E.Return(E.null(PU8)); });
    auto En = E.ent(Info);
    Val Ep = E.rt(RtFn::MapFind, {Info, M_, Key, E.hashOf(Info, Key)});
    E.If(Ep == E.null(PU8), [&] { E.Return(E.null(PU8)); });
    E.Return(E.padd(Ep, E.u(8) + En.Ka));
    E.finish();
    return Slot;
  }
  case RtFn::MapAssign: {
    if (!Dep({RtFn::MapNew, RtFn::MapFind, RtFn::MapGrow, RtFn::PanicText}) ||
        !Need({"memmove", "memset"}))
      return NoId;
    MapX E(*this,
           *Make("map_assign", Ctx.func({InfoPtr, Ctx.ptr(MapPtr), PU8}, PU8)));
    E.At = At;
    Val Info = E.param(0), Mp = E.param(1), Key = E.param(2);
    // The first write creates the map in the variable (no nil-map panic).
    Var M_ = E.var(MapPtr, E.load(Mp, MapPtr), "m");
    E.If(M_ == E.null(MapPtr), [&] {
      M_ = E.rt(RtFn::MapNew, {Info, E.u(0)});
      E.store(Mp, MapPtr, M_);
    });
    auto En = E.ent(Info);
    Val H = E.hashOf(Info, Key);
    Val Found = E.rt(RtFn::MapFind, {Info, M_, Key, H});
    E.If(Found != E.null(PU8),
         [&] { E.Return(E.padd(Found, E.u(8) + En.Ka)); });
    // A new key while a range over the map runs (RNG-4): every time.
    E.If(E.get(M_, 6) != 0,
         [&] { E.panicText(1, "map insert during iteration"); });
    E.If(E.get(M_, 1) >= E.get(M_, 2),
         [&] { E.rt(RtFn::MapGrow, {Info, M_}); });
    Val Nents = E.get(M_, 1), Ents = E.get(M_, 4), Mask = E.get(M_, 3) - 1;
    Val Ep = E.padd(Ents, Nents * En.Esz);
    E.store(Ep, U64, H);
    E.libc("memmove", {E.padd(Ep, E.u(8)), Key, E.snap(E.val(E.inf(Info, 0)))});
    E.libc("memset", {E.padd(Ep, E.u(8) + En.Ka), E.i32(0), En.Va});
    Var I = E.var(U64, H & Mask, "i");
    Val Idx = E.get(M_, 5);
    E.While([&] { return E.load(E.padd(Idx, I * 8), U64) != 0; },
            [&] { I = (I + 1) & Mask; });
    E.store(E.padd(Idx, I * 8), U64, Nents + 1);
    E.set(M_, 1, Nents + 1);
    E.set(M_, 0, E.get(M_, 0) + 1);
    E.Return(E.padd(Ep, E.u(8) + En.Ka));
    E.finish();
    return Slot;
  }
  case RtFn::MapDelete: {
    if (!Dep({RtFn::MapFind}) || !Need({"memset"}))
      return NoId;
    MapX E(*this, *Make("map_delete", Ctx.func({InfoPtr, MapPtr, PU8}, Unit)));
    E.At = At;
    Val Info = E.param(0), M_ = E.param(1), Key = E.param(2);
    E.If(M_ == E.null(MapPtr), [&] { E.Return(); });
    E.If(E.get(M_, 0) == 0, [&] { E.Return(); });
    Val Ep = E.rt(RtFn::MapFind, {Info, M_, Key, E.hashOf(Info, Key)});
    E.If(Ep == E.null(PU8), [&] { E.Return(); });
    E.store(Ep, U64, E.u(0));
    Val Left = E.get(M_, 0) - 1;
    E.set(M_, 0, Left);
    // Empty: start again from entry 0 (an iterator past the end stops).
    E.If(Left == 0, [&] {
      E.set(M_, 1, E.u(0));
      E.libc("memset", {E.get(M_, 5), E.i32(0), E.get(M_, 3) * 8});
    });
    E.finish();
    return Slot;
  }
  case RtFn::MapClear: {
    if (!Need({"memset"}))
      return NoId;
    MapX E(*this, *Make("map_clear", Ctx.func({InfoPtr, MapPtr}, Unit)));
    E.At = At;
    Val M_ = E.param(1);
    E.If(M_ == E.null(MapPtr), [&] { E.Return(); });
    E.If(E.get(M_, 3) != 0,
         [&] { E.libc("memset", {E.get(M_, 5), E.i32(0), E.get(M_, 3) * 8}); });
    E.set(M_, 0, E.u(0));
    E.set(M_, 1, E.u(0));
    E.finish();
    return Slot;
  }
  case RtFn::MapIterInit: {
    if (!Dep({RtFn::MapRand}))
      return NoId;
    MapX E(*this, *Make("map_iter_init", Ctx.func({IterPtr, MapPtr}, Unit)));
    E.At = At;
    Val It = E.param(0), M_ = E.param(1);
    Type *IS = mapIterType();
    auto Set = [&](unsigned K, Val V) {
      E.FB.assign(E.FB.field(E.at(It, IS), K), E.FB.use(V.Op));
    };
    Set(0, M_);
    Set(1, E.u(0));
    Set(2, E.u(0));
    Set(3, E.u(0));
    E.If(M_ != E.null(MapPtr), [&] {
      // One more running iteration (RNG-4); the entries there are now, from
      // a random one (spec chapter 16).
      E.set(M_, 6, E.get(M_, 6) + 1);
      Val N = E.get(M_, 1);
      Set(2, N);
      E.If(N != 0, [&] { Set(3, E.rt(RtFn::MapRand, {}) % N); });
    });
    E.finish();
    return Slot;
  }
  case RtFn::MapIterNext: {
    if (!Dep({RtFn::MapIterDone}))
      return NoId;
    MapX E(*this, *Make("map_iter_next",
                        Ctx.func({InfoPtr, IterPtr, Ctx.ptr(PU8), Ctx.ptr(PU8)},
                                 Bool)));
    E.At = At;
    Val Info = E.param(0), It = E.param(1);
    Type *IS = mapIterType();
    auto ItF = [&](unsigned K) { return E.FB.field(E.at(It, IS), K); };
    Val M_ = E.snap(E.val(ItF(0)));
    E.If(M_ == E.null(MapPtr), [&] { E.Return(E.boolean(false)); });
    auto En = E.ent(Info);
    Val End = E.snap(E.val(ItF(2))), Start = E.snap(E.val(ItF(3)));
    E.While([&] { return E.snap(E.val(ItF(1))) < End; },
            [&] {
              Val Step = E.snap(E.val(ItF(1)));
              E.FB.assign(ItF(1), E.FB.use((Step + 1).Op));
              Var Pos = E.var(U64, Start + Step, "pos");
              E.If(Pos >= End, [&] { Pos = Pos - End; });
              // Past the entries the map has now (it was emptied: delete of
              // the last entry, clear): gone.
              E.If(Pos < E.get(M_, 1), [&] {
                Val Ep = E.padd(E.get(M_, 4), Pos * En.Esz);
                E.If(E.load(Ep, U64) != 0, [&] {
                  E.store(E.param(2), PU8, E.padd(Ep, E.u(8)));
                  E.store(E.param(3), PU8, E.padd(Ep, E.u(8) + En.Ka));
                  E.Return(E.boolean(true));
                });
              });
            });
    // Every entry was seen: the iteration ends here.
    E.rt(RtFn::MapIterDone, {It});
    E.Return(E.boolean(false));
    E.finish();
    return Slot;
  }
  case RtFn::MapIterDone: {
    MapX E(*this, *Make("map_iter_done", Ctx.func({IterPtr}, Unit)));
    E.At = At;
    Val It = E.param(0);
    Type *IS = mapIterType();
    Place MF = E.FB.field(E.at(It, IS), 0);
    Val M_ = E.snap(E.val(MF));
    E.If(M_ != E.null(MapPtr), [&] {
      E.set(M_, 6, E.get(M_, 6) - 1);
      E.FB.assign(E.FB.field(E.at(It, IS), 0), E.FB.use(E.null(MapPtr).Op));
    });
    E.finish();
    return Slot;
  }
  case RtFn::MapRand: {
    // splitmix64 over a state of the module, seeded on first use from the
    // address of a stack slot (different on every run where the stack is
    // randomized; the starts differ from one range to the next anyway).
    if (MapSeed == ~0u) {
      Global G;
      G.Name = symbolOf(SymbolKind::Runtime, CP.Pkg, "map_seed");
      G.Ty = U64;
      G.Link = Linkage::Internal;
      MapSeed = M->addGlobal(std::move(G));
    }
    MapX E(*this, *Make("map_rand", Ctx.func({}, U64)));
    E.At = At;
    Val State{&E, constGlobal(*M, MapSeed)};
    Var R = E.var(U64, E.load(State, U64), "r");
    Var Here = E.var(U64, E.u(0), "here");
    E.If(R == 0, [&] { R = (E.to(U64, E.addrOf(Here)) ^ E.u(HashSeed)) | 1; });
    R = R + 0x9E3779B97F4A7C15ull;
    E.store(State, U64, R);
    Var Z = E.var(U64, R, "z");
    Z = (Z ^ (Z.v() >> 30)) * 0xBF58476D1CE4E5B9ull;
    Z = (Z ^ (Z.v() >> 27)) * 0x94D049BB133111EBull;
    E.Return(Z ^ (Z.v() >> 31));
    E.finish();
    return Slot;
  }
  case RtFn::MapEntryArgs: {
    // The map-entry arguments of one call (gir.md section 15): each entry's
    // map is copied in (nil when the key is absent: an argument never
    // inserts), then each entry that is an earlier one (the same map object,
    // not nil, the same key kind and an equal key) gets that one's slot, so
    // the parameters alias as `two(x, x)` does, and only the first copies
    // out. Up to 8 entries are compared pairwise; more go through a hash
    // table of the entries met so far, so a call costs O(n) expected.
    if (!Dep({RtFn::MapGet, RtFn::MemResize, RtFn::MemFree}))
      return NoId;
    Type *RecS = mapEntryArgType();
    MapX E(*this,
           *Make("map_entry_args", Ctx.func({Ctx.ptr(RecS), U64}, Unit)));
    E.At = At;
    Val Recs = E.param(0), N = E.param(1);
    uint64_t Rsz = layout(RecS).Size;
    auto Rec = [&](Val I) { return E.snap(E.padd(Recs, I * Rsz)); };
    auto Fld = [&](Val R, unsigned K) { return E.FB.field(E.at(R, RecS), K); };
    auto Get = [&](Val R, unsigned K) { return E.snap(E.val(Fld(R, K))); };
    auto Set = [&](Val R, unsigned K, Val V) {
      E.FB.assign(Fld(R, K), E.FB.use(V.Op));
    };
    // Copy in.
    Var I = E.var(U64, E.u(0), "i");
    E.While([&] { return I < N; },
            [&] {
              Val R = Rec(I);
              Val P = E.rt(RtFn::MapGet, {Get(R, 0), Get(R, 1), Get(R, 2)});
              Set(R, 4, E.null(MapPtr));
              Set(R, 6, E.boolean(false));
              E.If(P != E.null(PU8), [&] {
                Val V = E.load(P, MapPtr);
                Set(R, 4, V);
                Set(R, 6, V == E.null(MapPtr));
              });
              Set(R, 5, E.addrOf(Fld(R, 4)));
              Set(R, 7, E.boolean(true));
              I = I + 1;
            });
    // The same entry as R2: the same map (not nil), key kind and key.
    auto Same = [&](Val R, Val R2) {
      return E.land(Get(R2, 1) == Get(R, 1), [&] {
        return E.land(Get(R2, 3) == Get(R, 3), [&] {
          Val Info = Get(R, 0);
          return E.callIndirect(E.snap(E.val(E.inf(Info, 3))),
                                {Get(R2, 2), Get(R, 2)});
        });
      });
    };
    auto Alias = [&](Val R, Val R2) {
      Set(R, 5, Get(R2, 5));
      Set(R, 7, E.boolean(false));
    };
    E.IfElse(
        N <= 8,
        [&] {
          I = E.u(1);
          E.While([&] { return I < N; },
                  [&] {
                    Val R = Rec(I);
                    E.If(Get(R, 1) != E.null(MapPtr), [&] {
                      Var J = E.var(U64, E.u(0), "j");
                      E.While([&] { return J < I; },
                              [&] {
                                Val R2 = Rec(J);
                                // The first equal one owns its slot (it met no
                                // earlier one).
                                E.If(E.land(Get(R2, 7),
                                            [&] { return Same(R, R2); }),
                                     [&] {
                                       Alias(R, R2);
                                       E.Break();
                                     });
                                J = J + 1;
                              });
                    });
                    I = I + 1;
                  });
        },
        [&] {
          // Open addressing over the owners, at least twice n slots.
          Var Cap = E.var(U64, E.u(16), "cap");
          E.While([&] { return Cap < N * 2; }, [&] { Cap = Cap * 2; });
          Val CapV = E.snap(Cap);
          Val Tab =
              E.rt(RtFn::MemResize, {E.null(PU8), CapV * 8, E.boolean(true)});
          Val Mask = CapV - 1;
          I = E.u(0);
          E.While([&] { return I < N; },
                  [&] {
                    Val R = Rec(I);
                    Val Mo = Get(R, 1);
                    E.If(Mo != E.null(MapPtr), [&] {
                      Val Info = Get(R, 0);
                      Val H = E.callIndirect(E.snap(E.val(E.inf(Info, 2))),
                                             {Get(R, 2), E.u(HashSeed)}) ^
                              (E.to(U64, Mo) * 0x9E3779B97F4A7C15ull) ^
                              Get(R, 3);
                      Var Slot_ = E.var(U64, H & Mask, "s");
                      E.While([&] { return E.boolean(true); },
                              [&] {
                                Val At_ = E.padd(Tab, Slot_ * 8);
                                Val Ent = E.load(At_, U64);
                                E.If(Ent == 0, [&] {
                                  E.store(At_, U64, I + 1);
                                  E.Break();
                                });
                                Val R2 = Rec(Ent - 1);
                                E.If(Same(R, R2), [&] {
                                  Alias(R, R2);
                                  E.Break();
                                });
                                Slot_ = (Slot_ + 1) & Mask;
                              });
                    });
                    I = I + 1;
                  });
          E.rt(RtFn::MemFree, {Tab});
        });
    E.finish();
    return Slot;
  }
  case RtFn::MapEntryBack: {
    // Copy out, once per entry (its owner): a map the callee created for an
    // entry that was there and nil goes into the map object it was read
    // from, if the key is still there and its map still nil.
    if (!Dep({RtFn::MapGet}))
      return NoId;
    Type *RecS = mapEntryArgType();
    MapX E(*this,
           *Make("map_entry_back", Ctx.func({Ctx.ptr(RecS), U64}, Unit)));
    E.At = At;
    Val Recs = E.param(0), N = E.param(1);
    uint64_t Rsz = layout(RecS).Size;
    Var I = E.var(U64, E.u(0), "i");
    E.While([&] { return I < N; },
            [&] {
              Val R = E.snap(E.padd(Recs, I * Rsz));
              auto Get = [&](unsigned K) {
                return E.snap(E.val(E.FB.field(E.at(R, RecS), K)));
              };
              Val V = Get(4);
              E.If(E.land(Get(7),
                          [&] {
                            return E.land(Get(6),
                                          [&] { return V != E.null(MapPtr); });
                          }),
                   [&] {
                     Val P = E.rt(RtFn::MapGet, {Get(0), Get(1), Get(2)});
                     E.If(P != E.null(PU8), [&] {
                       E.If(E.load(P, MapPtr) == E.null(MapPtr),
                            [&] { E.store(P, MapPtr, V); });
                     });
                   });
              I = I + 1;
            });
    E.finish();
    return Slot;
  }
  default:
    break;
  }
  return NoId;
}
