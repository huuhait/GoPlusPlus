//===- LowerTest.cpp - The test intrinsics of the allocator word ----------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.25 (ADR-0029). Until P05.03 has real arenas, a test needs a second
// allocator to show that a slice or a map grows and frees through the
// allocator named by its own word. With LowerOptions::TestIntrinsics an
// `@Extern("C")` declaration named gopp_test_* is one of these (the
// signatures are the test's; the result type says the element type):
//
//   gopp_test_make_slice(which, len, cap int) []T   a zeroed slice in allocator
//                                                    `which`
//   gopp_test_make_map(which int) map[K]V           a map made in it
//   gopp_test_clone_slice(s []T) []T                the runtime's clone
//                                                    (stays in s's allocator)
//   gopp_test_free_slice(s []T)                     the runtime's free of the
//   gopp_test_free_map(m map[K]V)                   buffer / of the map (what
//                                                    P04.09's drop glue calls)
//   gopp_test_count(which, what int) int            calls so far: what 0 alloc,
//                                                    1 resize, 2 free
//   gopp_test_word(s []T) int, gopp_test_word_map(m map[K]V) int
//                                                   which allocator an owned
//                                                    slice or a map has
//
// `which`: 0 is the global allocator (a null word; its counters are those of
// mem_resize and mem_free), 1 and 2 are two counting objects (the table of
// P05 D5.3, then one counter per entry). The `which` of make_slice and
// make_map may also be declared `*byte`: then it is an allocator object of
// the test's own (tests/unit/GIR/AllocatorTest.cpp passes one written in C++).
// Reading the word of a view or of a borrowed slice is refused: it has none.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace {
/// The table entries of the counting objects are stored where they are used:
/// the objects are zero-initialized globals.
void initAllocs(Lowering &L, Rt &E) {
  FuncId Fns[3] = {L.rt(RtFn::TestAlloc, SourceLoc()),
                   L.rt(RtFn::TestResize, SourceLoc()),
                   L.rt(RtFn::TestFree, SourceLoc())};
  for (unsigned W = 1; W <= 2; ++W) {
    Val A{&E, constGlobal(*L.M, L.testAllocGlobal(W))};
    for (unsigned K = 0; K < 3; ++K) {
      if (Fns[K] == NoId)
        return;
      Place Entry = E.FB.field(E.FB.field(E.at(A, L.testAllocType()), 0), K);
      E.FB.assign(Entry, E.FB.use(E.func(Fns[K]).Op));
    }
  }
}
} // namespace

Operand Body::lowerTestIntrinsic(const ast::CallExpr *C, const S::Func *F,
                                 Lowering::TestFn Fn) {
  using TF = Lowering::TestFn;
  Rt E(M, FB);
  Type *PU8 = ctx().ptr(ctx().u8()), *U64 = ctx().u64(), *I64 = ctx().i64();
  bool NoResult = Fn == TF::FreeSlice || Fn == TF::FreeMap;
  S::Type *RS = NoResult ? nullptr : sTyOf(C);
  Type *G = NoResult ? nullptr : tyOf(C);
  size_t Want = Fn == TF::MakeSlice ? 3 : (Fn == TF::Count) ? 2 : 1;
  if (C->Args.size() != Want || (!G && !NoResult)) {
    unsupported(C, "this test intrinsic call");
    return unitValue();
  }
  // The declared type of the argument (a map entry passed on has none of its
  // own in the checker's table).
  auto ParamTy = [&](size_t K) -> S::Type * {
    S::SignatureType *Sig = sigOf(F);
    return K < Sig->Params->Elems.size() ? Sig->Params->Elems[K] : nullptr;
  };
  auto Bad = [&](const char *Why) {
    unsupported(C, Why);
    return NoResult ? unitValue() : zeroOf(G);
  };
  // The allocator object for `which` (0: null, the global allocator).
  auto WordFor = [&](Val Which) {
    Var W = E.var(PU8, E.null(PU8), "w");
    for (unsigned K = 1; K <= 2; ++K)
      E.If(Which == E.i(K), [&] {
        W = E.to(PU8, Val{&E, constGlobal(M, L.testAllocGlobal(K))});
      });
    return E.snap(W);
  };
  // The first argument of make_slice and make_map is `which`, or, declared as
  // a pointer, an allocator object of the test's own (AllocatorTest.cpp
  // passes one written in C++). Read first, as the call's order says.
  bool ByObject = ParamTy(0) && isa<S::PointerType>(S::under(ParamTy(0)));
  auto WhichArg = [&] {
    return E.snap(ByObject ? E.to(PU8, Val{&E, lowerExpr(C->Args[0])})
                           : Val{&E, intArg(C->Args[0])});
  };
  // Which allocator a word is: 0 the global one, 1 and 2 the counting
  // objects, 9 another.
  auto IndexOf = [&](Val Word) {
    Var R = E.var(I64, E.i(9), "which");
    E.If(Word == E.null(PU8), [&] { R = E.i(0); });
    for (unsigned K = 1; K <= 2; ++K)
      E.If(Word == E.to(PU8, Val{&E, constGlobal(M, L.testAllocGlobal(K))}),
           [&] { R = E.i(K); });
    return E.snap(R);
  };
  switch (Fn) {
  case TF::MakeSlice: {
    auto *SG = dyn_cast_or_null<S::SliceType>(RS ? S::under(RS) : nullptr);
    if (!SG || G->getKind() != Type::Kind::Slice)
      return Bad("a test make_slice that does not give a slice");
    Val Which = WhichArg();
    Val Len = E.snap(Val{&E, intArg(C->Args[1])});
    Val Cap = E.snap(Val{&E, intArg(C->Args[2])});
    at(C);
    initAllocs(L, E);
    Val W = ByObject ? Which : WordFor(Which);
    Type *ElemG = G->elem();
    // The checks of `make` (Go's makeslice): 0 <= len <= cap, cap * size
    // below the limit.
    Operand Buf =
        callRt(RtFn::MakeSlice,
               {W.Op, E.u(L.layout(ElemG).Size).Op, Len.Op, Cap.Op}, C);
    Val P = E.to(ctx().ptr(ElemG), Val{&E, Buf});
    return E.slice(ElemG, P, E.to(U64, Len), E.to(U64, Cap), W).Op;
  }
  case TF::MakeMap: {
    auto *MT = dyn_cast_or_null<S::MapType>(RS ? S::under(RS) : nullptr);
    if (!MT)
      return Bad("a test make_map that does not give a map");
    Val Which = WhichArg();
    at(C);
    initAllocs(L, E);
    Val W = ByObject ? Which : WordFor(Which);
    Operand Info = mapInfoPtr(MT, C);
    return callRt(RtFn::MapNew, {Info, E.u(0).Op, W.Op}, C);
  }
  case TF::CloneSlice:
  case TF::FreeSlice: {
    S::Type *AS = ParamTy(0);
    if (!AS || !isa<S::SliceType>(S::under(AS)))
      return Bad("a test slice routine on a value that is no owned slice");
    Type *SG = L.lowerType(AS, C->getBegin());
    if (!SG || SG->getKind() != Type::Kind::Slice)
      return Bad("a test slice routine on this type");
    Place H = placeOf(valueFor(C->Args[0], AS));
    at(C);
    Type *ElemG = SG->elem();
    uint64_t Esz = L.layout(ElemG).Size;
    Val Ptr = E.snap(E.to(PU8, E.val(FB.field(H, 0))));
    Val Len = E.snap(E.val(FB.field(H, 1)));
    Val Cap = E.snap(E.val(FB.field(H, 2)));
    Val W = E.snap(E.val(FB.field(H, 3)));
    if (Fn == TF::FreeSlice) {
      callRt(RtFn::SliceFree, {W.Op, Ptr.Op, Cap.Op, E.u(Esz).Op}, C);
      // The slice is given up: its owner must not free it again (OWN-R5).
      FB.assign(H, FB.use(zeroOf(SG)));
      return unitValue();
    }
    Operand Buf =
        callRt(RtFn::SliceClone, {W.Op, Ptr.Op, Len.Op, E.u(Esz).Op}, C);
    return E.slice(ElemG, E.to(ctx().ptr(ElemG), Val{&E, Buf}), Len, Len, W).Op;
  }
  case TF::FreeMap: {
    auto *MT = dyn_cast_or_null<S::MapType>(S::under(ParamTy(0)));
    if (!MT)
      return Bad("a test free_map on a value that is no map");
    Place MP = placeOf(lowerExpr(C->Args[0]));
    at(C);
    callRt(RtFn::MapFree, {mapInfoPtr(MT, C), copyOf(MP)}, C);
    FB.assign(MP, FB.use(zeroOf(MP.Ty)));
    return unitValue();
  }
  case TF::Count: {
    Val Which = E.snap(Val{&E, intArg(C->Args[0])});
    Val What = E.snap(E.to(U64, Val{&E, intArg(C->Args[1])}));
    at(C);
    initAllocs(L, E);
    Val Obj = WordFor(Which);
    Var R = E.var(U64, E.u(0), "n");
    uint64_t Off = L.layout(L.allocTableType()).Size;
    E.IfElse(
        Obj == E.null(PU8),
        [&] {
          Val Calls = {&E, constGlobal(M, L.testGlobalCalls())};
          R = E.load(E.padd(Calls, What * 8), U64);
        },
        [&] { R = E.load(E.padd(Obj, What * 8 + Off), U64); });
    return E.to(G, E.snap(R)).Op;
  }
  case TF::Word: {
    const ast::Expr *Arg = C->Args[0];
    S::Type *AS = ParamTy(0);
    S::Type *ArgS = sTyOf(Arg);
    if (!AS)
      return Bad("a test word of this value");
    if (ArgS && isa<S::RefType>(S::under(ArgS)))
      return Bad("the allocator word of a view or a borrowed slice: it has "
                 "none (ADR-0029 point 7)");
    // A borrowed parameter is a view that was given a header of its own.
    if (auto *Id = dyn_cast<ast::Ident>(unparen(Arg))) {
      auto U = I.Uses.find(Id);
      if (U != I.Uses.end() && Obj)
        for (size_t K = 0; K < Obj->Params.size(); ++K)
          if (Obj->Params[K] == U->second && L.sliceBorrowed(sigOf(Obj), K))
            return Bad("the allocator word of a view or a borrowed slice: it "
                       "has none (ADR-0029 point 7)");
    }
    if (isa<S::SliceType>(S::under(AS))) {
      Place H = placeOf(valueFor(Arg, AS));
      at(C);
      return E.to(G, IndexOf(E.snap(E.val(FB.field(H, 3))))).Op;
    }
    if (auto *MT = dyn_cast<S::MapType>(S::under(AS))) {
      (void)MT;
      Val Mv = E.snap(Val{&E, lowerExpr(Arg)});
      at(C);
      Var Wd = E.var(PU8, E.null(PU8), "w");
      E.If(Mv != E.null(Mv.ty()),
           [&] { Wd = E.val(FB.field(E.at(Mv, L.mapType()->elem()), 7)); });
      return E.to(G, IndexOf(E.snap(Wd))).Op;
    }
    return Bad("a test word of a value that is no owned slice or map");
  }
  }
  return unitValue();
}
