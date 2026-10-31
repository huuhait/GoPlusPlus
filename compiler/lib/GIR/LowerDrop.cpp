//===- LowerDrop.cpp - Ownership: owned values, moves, drop glue ----------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.09 (gir.md section 7). What the lowering tells drop elaboration
// (DropElab.cpp) and how:
//
//   - A variable or parameter that owns its value, and every temporary that
//     holds a value just made, is an *owned local* (`Local::Owned`) with the
//     drop glue of its type (`Local::Glue`). A statement ends the temporaries
//     it made (StorageDead; dropTempsSince); a scope ends its variables
//     (exitScopes).
//   - A value given to a new owner is read with `Move` (ownedValue): the
//     source is a place that gets zeroed, or a fresh temporary that is handed
//     on. A store that replaces a live owner says so (`Statement::DropOld`).
//   - The drop glue of a type (Lowering::dropGlue) drops what the value owns
//     in the order of OWN-R1 and leaves all-zero bytes.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

#include "gopp/AST/ASTDumper.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

//===----------------------------------------------------------------------===//
// Drop glue
//===----------------------------------------------------------------------===//

bool Lowering::needsDrop(S::Type *T) {
  if (!T)
    return false;
  if (auto It = NeedsDropMemo.find(T); It != NeedsDropMemo.end())
    return It->second;
  bool R = false;
  if (auto *TT = llvm::dyn_cast<S::TupleType>(T)) {
    // The results of a call (P04.09): what one of them owns.
    for (S::Type *E : TT->Elems)
      R = R || needsDrop(E);
  } else {
    // OWN-C1: an owning value is dropped, a shared handle released; a
    // trivial one (a view too) needs nothing. The checker's category, on
    // the instance's types.
    R = Methods.isOwning(T) || Methods.isSharedHandle(T);
  }
  NeedsDropMemo[T] = R;
  return R;
}

FuncId Lowering::dropGlue(S::Type *T, SourceLoc At) {
  return glueFor(T, At, /*Clone=*/false);
}

FuncId Lowering::cloneGlue(S::Type *T, SourceLoc At) {
  return glueFor(T, At, /*Clone=*/true);
}

bool Lowering::reachesUserGlue(S::Type *Root) {
  // Does a value of Root hold, at any depth, a value whose type has a `drop`
  // or a `Clone() T` method the program wrote (what the glue of Root calls)?
  // The parts its glue walks (forEachGluePart; a shared handle ends the
  // walk: its glue is no program's). One walk over
  // the types not decided yet records who holds whom, then the answer runs
  // back from the types with such a method to all that hold them, and every
  // type walked keeps its answer: each type is walked once, so a chain of N
  // types costs O(N) (a type's glue asks for each of its parts), and a cycle
  // (`type T struct{ xs []T }`) needs no special case.
  if (Opts.RewalkUserGlue)
    UserGlueMemo.clear();
  if (auto It = UserGlueMemo.find(Root); It != UserGlueMemo.end())
    return It->second;
  llvm::SmallVector<S::Type *, 16> Walked{Root};
  llvm::SmallPtrSet<S::Type *, 16> Seen{Root};
  llvm::DenseMap<S::Type *, llvm::SmallVector<S::Type *, 2>> HeldBy;
  llvm::SmallVector<S::Type *, 16> Yes;
  for (size_t K = 0; K < Walked.size(); ++K) {
    ++LowerSteps; // test-only: counted
    S::Type *T = Walked[K];
    if (!needsDrop(T) || isSharedHandle(T))
      continue;
    if (Methods.dropMethod(T) || Methods.cloneMethod(T)) {
      Yes.push_back(T);
      continue;
    }
    auto Part = [&](S::Type *P) {
      ++LowerSteps;
      if (!P)
        return;
      if (auto It = UserGlueMemo.find(P); It != UserGlueMemo.end()) {
        if (It->second)
          Yes.push_back(T);
        return;
      }
      HeldBy[P].push_back(T);
      if (Seen.insert(P).second)
        Walked.push_back(P);
    };
    forEachGluePart(T, Part);
  }
  llvm::SmallPtrSet<S::Type *, 16> Reaches;
  while (!Yes.empty()) {
    S::Type *T = Yes.pop_back_val();
    if (!Reaches.insert(T).second)
      continue;
    if (auto It = HeldBy.find(T); It != HeldBy.end())
      Yes.append(It->second.begin(), It->second.end());
  }
  for (S::Type *T : Walked)
    UserGlueMemo[T] = Reaches.count(T);
  return Reaches.count(Root);
}

S::Type *Lowering::glueKey(S::Type *T) {
  // Types that differ only in their units are one GIR type (UNI-9, P04.24:
  // unitFree). Their glue is shared, the first of them naming it, unless it
  // calls a method the program wrote (a `drop` or a `Clone() T` at any
  // depth): that method is the instance's own, with its own units (spec 17:
  // the dynamic type keeps them), so the glue is keyed by the whole type
  // then (review R17).
  if (reachesUserGlue(T))
    return T;
  if (const sym::Node *F = unitFree(T)) {
    auto [It, New] = UnitFreeGlueKeys.try_emplace(F, T);
    if (!New)
      return It->second;
  }
  return T;
}

FuncId Lowering::glueFor(S::Type *T, SourceLoc At, bool Clone) {
  if (!needsDrop(T))
    return NoId;
  T = glueKey(T);
  auto &Memo = Clone ? CloneGlues : DropGlues;
  if (auto It = Memo.find(T); It != Memo.end())
    return It->second;
  Type *G = lowerType(T, At);
  if (!G)
    return NoId;
  // An interface value and a func value are one GIR type each: their glue is
  // the runtime's (a release; a retain for the clone).
  if (G->isIface() || G->isFuncValue()) {
    RtFn Fn = G->isIface() ? (Clone ? RtFn::IfaceClone : RtFn::IfaceRelease)
                           : (Clone ? RtFn::FuncClone : RtFn::FuncRelease);
    FuncId Id = rt(Fn, At);
    Memo[T] = Id;
    return Id;
  }
  std::string Sym =
      Syms.glue(Clone ? sym::Kind::CloneGlue : sym::Kind::DropGlue, T);
  if (Sym.empty())
    Sym = runtimeSymbol((Clone ? "clone_" : "drop_") + G->str());
  FuncId Id = M->lookupFunction(Sym);
  if (Id == NoId) {
    auto Fn = std::make_unique<Function>();
    Fn->Name = Sym;
    Type *PU8 = Ctx.ptr(Ctx.u8());
    Fn->Sig =
        Clone ? Ctx.func({PU8, PU8}, Ctx.unit()) : Ctx.func({PU8}, Ctx.unit());
    Fn->Link = Linkage::LinkOnceODR;
    Fn->CC = CallConv::Go;
    Id = M->addFunction(std::move(Fn));
    DropPending.push_back({T, Id, At, Clone});
  }
  Memo[T] = Id;
  if (!BuildingDrops) {
    // The bodies, one after the other: a body asks for the glue of its parts,
    // which only adds them to the work list.
    BuildingDrops = true;
    while (!DropPending.empty()) {
      DropWork W = DropPending.back();
      DropPending.pop_back();
      if (W.NewMaps)
        buildNewMapsGlue(W);
      else if (W.Clone)
        buildCloneGlue(W);
      else
        buildDropGlue(W);
    }
    BuildingDrops = false;
  }
  return Id;
}

FuncId Lowering::newMapsGlue(S::Type *T, SourceLoc At) {
  if (!T || !holdsOwned(T, /*MapsOnly=*/true))
    return NoId;
  T = glueKey(T);
  if (auto It = NewMapsGlues.find(T); It != NewMapsGlues.end())
    return It->second;
  Type *G = lowerType(T, At);
  if (!G)
    return NoId;
  std::string Sym = runtimeSymbol("newmaps_" + G->str());
  FuncId Id = M->lookupFunction(Sym);
  if (Id == NoId) {
    auto Fn = std::make_unique<Function>();
    Fn->Name = Sym;
    Type *PU8 = Ctx.ptr(Ctx.u8());
    Fn->Sig = Ctx.func({PU8, PU8}, Ctx.unit());
    Fn->Link = Linkage::Internal;
    Fn->CC = CallConv::Go;
    Id = M->addFunction(std::move(Fn));
    DropWork W{T, Id, At};
    W.NewMaps = true;
    DropPending.push_back(W);
  }
  NewMapsGlues[T] = Id;
  if (!BuildingDrops) {
    BuildingDrops = true;
    while (!DropPending.empty()) {
      DropWork X = DropPending.back();
      DropPending.pop_back();
      if (X.NewMaps)
        buildNewMapsGlue(X);
      else if (X.Clone)
        buildCloneGlue(X);
      else
        buildDropGlue(X);
    }
    BuildingDrops = false;
  }
  return Id;
}

void Lowering::buildNewMapsGlue(const DropWork &W) {
  S::Type *T = W.T;
  SourceLoc At = W.At;
  Function &F = M->func(W.Id);
  Type *G = lowerType(T, At);
  Type *PU8 = Ctx.ptr(Ctx.u8()), *U64 = Ctx.u64();
  Rt X(*M, F);
  FuncBuilder &FB = X.FB;
  FB.setSpan(F.Loc);
  Val Cp = X.param(0), Or = X.param(1);
  auto C0 = [&] { return X.at(Cp, G); };
  auto O0 = [&] { return X.at(Or, G); };
  auto Child = [&](S::Type *CT, Val A, Val B) {
    FuncId C = newMapsGlue(CT, At);
    if (C != NoId)
      X.call(C, {X.to(PU8, A), X.to(PU8, B)});
  };
  S::Type *U = S::under(T);
  if (llvm::isa_and_nonnull<S::MapType>(U)) {
    // Not the original's map: one the method made in a nil one.
    Val A = X.snap(X.val(C0())), B = X.snap(X.val(O0()));
    X.If(A != B, [&] {
      if (FuncId D = dropGlue(T, At); D != NoId)
        X.call(D, {Cp});
    });
  } else if (auto *ST = dyn_cast_or_null<S::StructType>(U)) {
    for (size_t K = 0; K < ST->Fields.size(); ++K)
      Child(ST->Fields[K]->Ty, X.addrOf(FB.field(C0(), K)),
            X.addrOf(FB.field(O0(), K)));
  } else if (auto *AT = dyn_cast_or_null<S::ArrayType>(U)) {
    Var I = X.var(U64, X.u(0), "i");
    X.While([&] { return X.bin(BinOp::Lt, I, X.u(G->length())); },
            [&] {
              Child(AT->Elem, X.addrOf(FB.index(C0(), I.L)),
                    X.addrOf(FB.index(O0(), I.L)));
              I = I + 1;
            });
  } else if (auto *OT = dyn_cast_or_null<S::OptionType>(U)) {
    if (G->getKind() == Type::Kind::Option) {
      Val Set = X.snap(X.val(FB.field(O0(), 1)));
      X.If(Set, [&] {
        Child(OT->Elem, X.addrOf(FB.field(C0(), 0)),
              X.addrOf(FB.field(O0(), 0)));
      });
    }
  }
  X.finish();
}

void Lowering::buildDropGlue(const DropWork &W) {
  S::Type *T = W.T;
  SourceLoc At = W.At;
  Function &F = M->func(W.Id);
  Type *G = lowerType(T, At);
  Type *PU8 = Ctx.ptr(Ctx.u8()), *U64 = Ctx.u64();
  Rt X(*M, F);
  FuncBuilder &FB = X.FB;
  FB.setSpan(F.Loc);
  Val P = X.param(0);
  auto Obj = [&] { return X.at(P, G); };
  auto Child = [&](S::Type *CT, Val Addr) {
    FuncId C = dropGlue(CT, At);
    if (C != NoId)
      X.call(C, {X.to(PU8, Addr)});
  };
  // The user's `drop` first (OWN-R1), unless the value is all zero bytes
  // (OWN-R5: a moved-from place, a dropped one, a zero value).
  if (const S::Func *DM = Methods.dropMethod(T)) {
    bool Have = FuncIds.count(DM) || ensureMethod(DM, At);
    FuncId DF = Have ? FuncIds[DM] : NoId;
    FuncId Zero = rt(RtFn::IsZero, At);
    if (DF != NoId && Zero != NoId) {
      Type *Param = M->func(DF).Sig->elems()[0];
      X.If(X.not_(X.call(Zero, {P, X.u(layout(G).Size)})),
           [&] { X.call(DF, {X.to(Param, P)}); });
    }
  }
  S::Type *U = S::under(T);
  if (isStringType(U)) {
    // A literal has capacity 0 and owns nothing.
    Place O = Obj();
    Val Cap = X.snap(X.val(FB.field(O, 2)));
    X.If(Cap != 0, [&] {
      FuncId Free = rt(RtFn::MemFree, At);
      if (Free != NoId)
        X.call(Free, {X.to(PU8, X.val(FB.field(Obj(), 0)))});
    });
  } else if (auto *SL = dyn_cast_or_null<S::SliceType>(U)) {
    Type *EG = lowerType(SL->Elem, At);
    uint64_t Esz = EG ? layout(EG).Size : 0;
    Place O = Obj();
    Val Ptr = X.snap(X.val(FB.field(O, 0))),
        Len = X.snap(X.val(FB.field(O, 1))),
        Cap = X.snap(X.val(FB.field(O, 2))),
        Word = X.snap(X.val(FB.field(O, 3)));
    if (EG && needsDrop(SL->Elem)) {
      Var I = X.var(U64, X.u(0), "i");
      X.While([&] { return X.bin(BinOp::Lt, I, Len); },
              [&] {
                Child(SL->Elem, X.padd(X.to(PU8, Ptr), I * Esz));
                I = I + 1;
              });
    }
    FuncId Free = rt(RtFn::SliceFree, At);
    if (Free != NoId && EG)
      X.call(Free, {Word, X.to(PU8, Ptr), Cap, X.u(Esz)});
  } else if (auto *MT = dyn_cast_or_null<S::MapType>(U)) {
    FuncId Free = rt(RtFn::MapFree, At);
    SmallVector<Operand, 7> Ops;
    if (Free != NoId && mapInfoOperands(MT, At, Ops)) {
      Type *IS = mapInfoType();
      Place Info = FB.local(FB.newTemp(IS));
      FB.assign(Info, FB.aggregate(AggKind::Struct, IS, Ops));
      Val InfoP = X.addrOf(Info);
      Val Mp = X.snap(X.val(Obj()));
      X.If(Mp != X.null(Mp.ty()), [&] { X.call(Free, {InfoP, Mp}); });
    }
  } else if (auto *ST = dyn_cast_or_null<S::StructType>(U)) {
    // The fields in reverse declaration order (OWN-R1).
    for (size_t K = ST->Fields.size(); K > 0; --K)
      if (needsDrop(ST->Fields[K - 1]->Ty))
        Child(ST->Fields[K - 1]->Ty, X.addrOf(FB.field(Obj(), K - 1)));
  } else if (auto *AT = dyn_cast_or_null<S::ArrayType>(U)) {
    if (needsDrop(AT->Elem)) {
      Var I = X.var(U64, X.u(0), "i");
      X.While([&] { return X.bin(BinOp::Lt, I, X.u(G->length())); },
              [&] {
                Child(AT->Elem, X.addrOf(FB.index(Obj(), I.L)));
                I = I + 1;
              });
    }
  } else if (auto *OT = dyn_cast_or_null<S::OptionType>(U)) {
    if (G->getKind() == Type::Kind::Option) {
      Val Set = X.snap(X.val(FB.field(Obj(), 1)));
      X.If(Set, [&] { Child(OT->Elem, X.addrOf(FB.field(Obj(), 0))); });
    }
  } else if (auto *TT = dyn_cast_or_null<S::TupleType>(U)) {
    for (size_t K = TT->Elems.size(); K > 0; --K)
      if (needsDrop(TT->Elems[K - 1]))
        Child(TT->Elems[K - 1], X.addrOf(FB.field(Obj(), K - 1)));
  }
  // All-zero bytes: the drop of the value is done (OWN-R5).
  FB.assign(Obj(), FB.use(X.zero(G).Op));
  X.finish();
}

void Lowering::buildCloneGlue(const DropWork &W) {
  S::Type *T = W.T;
  SourceLoc At = W.At;
  Function &F = M->func(W.Id);
  Type *G = lowerType(T, At);
  Type *PU8 = Ctx.ptr(Ctx.u8()), *U64 = Ctx.u64();
  Rt X(*M, F);
  FuncBuilder &FB = X.FB;
  FB.setSpan(F.Loc);
  Val Dst = X.param(0), Src = X.param(1);
  auto D = [&] { return X.at(Dst, G); };
  auto S0 = [&] { return X.at(Src, G); };
  auto Child = [&](S::Type *CT, Val To, Val From) {
    FuncId C = cloneGlue(CT, At);
    if (C != NoId)
      X.call(C, {X.to(PU8, To), X.to(PU8, From)});
  };
  // A `Clone() T` the program wrote is the clone (OWN-R6): a type with a
  // `drop` method decides how its resource is copied.
  if (const S::Func *CM = Methods.cloneMethod(T)) {
    FuncId CF =
        (FuncIds.count(CM) || ensureMethod(CM, At)) ? FuncIds[CM] : NoId;
    if (CF != NoId) {
      Type *P0 = M->func(CF).Sig->elems()[0];
      Val Recv;
      if (P0 == G)
        Recv = X.val(S0());
      else if (P0->isPtr() && P0->elem() == G)
        Recv = X.to(P0, Src);
      else // a borrowed string or slice: (ptr, len)
        Recv = X.tmp(FB.aggregate(
            AggKind::Tuple, P0,
            {copyOf(FB.field(S0(), 0)), copyOf(FB.field(S0(), 1))}));
      Val R = X.call(CF, {Recv});
      FB.assign(D(), FB.use(R.Op));
    }
    X.finish();
    return;
  }
  // The bits first (what owns nothing is right as it is), then the owning
  // parts are made anew over them.
  FB.assign(D(), FB.use(X.val(S0()).Op));
  S::Type *U = S::under(T);
  if (isStringType(U)) {
    // A literal (capacity 0) owns nothing: the copy shares it.
    Val Cap = X.snap(X.val(FB.field(S0(), 2)));
    X.If(Cap != 0, [&] {
      FuncId From = rt(RtFn::StrFromBytes, At);
      if (From == NoId)
        return;
      Val Len = X.snap(X.val(FB.field(S0(), 1)));
      Val Ptr = X.snap(X.to(PU8, X.val(FB.field(S0(), 0))));
      Val Copy = X.call(From, {Ptr, Len});
      FB.assign(D(), FB.use(Copy.Op));
    });
  } else if (auto *SL = dyn_cast_or_null<S::SliceType>(U)) {
    Type *EG = lowerType(SL->Elem, At);
    uint64_t Esz = EG ? layout(EG).Size : 0;
    Val Ptr = X.snap(X.to(PU8, X.val(FB.field(S0(), 0))));
    X.If(Ptr != X.null(PU8), [&] {
      FuncId Clone = rt(RtFn::SliceClone, At);
      if (Clone == NoId || !EG)
        return;
      Val Len = X.snap(X.val(FB.field(S0(), 1)));
      Val Word = X.snap(X.val(FB.field(S0(), 3)));
      Val Buf = X.snap(X.call(Clone, {Word, Ptr, Len, X.u(Esz)}));
      FB.assign(FB.field(D(), 0), FB.use(X.to(Ctx.ptr(EG), Buf).Op));
      FB.assign(FB.field(D(), 2), FB.use(Len.Op));
      if (needsDrop(SL->Elem)) {
        Var I = X.var(U64, X.u(0), "i");
        X.While([&] { return X.bin(BinOp::Lt, I, Len); },
                [&] {
                  Val Off = I * Esz;
                  Child(SL->Elem, X.padd(Buf, Off), X.padd(Ptr, Off));
                  I = I + 1;
                });
      }
    });
  } else if (auto *MT = dyn_cast_or_null<S::MapType>(U)) {
    // A new map in the allocator of the source (SLC-3, ADR-0029) with every
    // entry: map_assign clones the key (the info's key glue, OWN-V4a), the
    // value is cloned into the new entry.
    FB.assign(D(), FB.use(X.null(G).Op));
    Val Mp = X.snap(X.val(S0()));
    X.If(Mp != X.null(G), [&] {
      SmallVector<Operand, 7> Ops;
      FuncId New = rt(RtFn::MapNew, At), Assign = rt(RtFn::MapAssign, At),
             Init = rt(RtFn::MapIterInit, At), Next = rt(RtFn::MapIterNext, At),
             Done = rt(RtFn::MapIterDone, At);
      Type *VG = lowerType(MT->Elem, At);
      if (New == NoId || Assign == NoId || Init == NoId || Next == NoId ||
          Done == NoId || !VG || !mapInfoOperands(MT, At, Ops))
        return;
      Type *IS = mapInfoType();
      Place Info = FB.local(FB.newTemp(IS));
      FB.assign(Info, FB.aggregate(AggKind::Struct, IS, Ops));
      Val InfoP = X.snap(X.addrOf(Info));
      Place Hdr = X.at(X.to(PU8, Mp), mapType()->elem());
      Val Count = X.snap(X.val(FB.field(Hdr, 0)));
      Val Word = X.snap(X.val(FB.field(Hdr, 7)));
      Val NewM = X.call(New, {InfoP, Count, Word});
      FB.assign(D(), FB.use(NewM.Op));
      Var It = X.var(mapIterType(), X.zero(mapIterType()), "it");
      Var KP = X.var(PU8, X.null(PU8), "kp"),
          VP = X.var(PU8, X.null(PU8), "vp");
      X.call(Init, {X.addrOf(It), Mp});
      X.While(
          [&] {
            return X.call(Next,
                          {InfoP, X.addrOf(It), X.addrOf(KP), X.addrOf(VP)});
          },
          [&] {
            Val Slot =
                X.snap(X.call(Assign, {InfoP, X.to(Ctx.ptr(G), Dst), KP}));
            FuncId VC = cloneGlue(MT->Elem, At);
            if (VC != NoId)
              X.call(VC, {Slot, VP});
            else
              FB.assign(X.at(Slot, VG), FB.use(copyOf(X.at(VP, VG))));
          });
      X.call(Done, {X.addrOf(It)});
    });
  } else if (auto *ST = dyn_cast_or_null<S::StructType>(U)) {
    for (size_t K = 0; K < ST->Fields.size(); ++K)
      if (needsDrop(ST->Fields[K]->Ty))
        Child(ST->Fields[K]->Ty, X.addrOf(FB.field(D(), K)),
              X.addrOf(FB.field(S0(), K)));
  } else if (auto *AT = dyn_cast_or_null<S::ArrayType>(U)) {
    if (needsDrop(AT->Elem)) {
      Var I = X.var(U64, X.u(0), "i");
      X.While([&] { return X.bin(BinOp::Lt, I, X.u(G->length())); },
              [&] {
                Child(AT->Elem, X.addrOf(FB.index(D(), I.L)),
                      X.addrOf(FB.index(S0(), I.L)));
                I = I + 1;
              });
    }
  } else if (auto *OT = dyn_cast_or_null<S::OptionType>(U)) {
    if (G->getKind() == Type::Kind::Option) {
      Val Set = X.snap(X.val(FB.field(S0(), 1)));
      X.If(Set, [&] {
        Child(OT->Elem, X.addrOf(FB.field(D(), 0)),
              X.addrOf(FB.field(S0(), 0)));
      });
    }
  } else if (auto *TT = dyn_cast_or_null<S::TupleType>(U)) {
    for (size_t K = 0; K < TT->Elems.size(); ++K)
      if (needsDrop(TT->Elems[K]))
        Child(TT->Elems[K], X.addrOf(FB.field(D(), K)),
              X.addrOf(FB.field(S0(), K)));
  }
  X.finish();
}

//===----------------------------------------------------------------------===//
// Owned locals and values
//===----------------------------------------------------------------------===//

void Body::ownLocal(LocalId Loc, S::Type *T) {
  FuncId Glue = L.dropGlue(T, Pos);
  if (Glue == NoId)
    return;
  Local &Lo = FB.function().Locals[Loc];
  Lo.Owned = true;
  Lo.Glue = Glue;
}

Operand Body::ownTemp(Operand V, S::Type *T) {
  Function &F = FB.function();
  if (V.K != Operand::Copy || !L.needsDrop(T))
    return V;
  // (Already the owner's own temporary: a clone, a move.)
  if (V.P.Proj.empty() && F.Locals[V.P.Local].K == Local::Temp &&
      F.Locals[V.P.Local].Owned)
    return V;
  LocalId Tmp;
  if (V.P.Proj.empty() && F.Locals[V.P.Local].K == Local::Temp &&
      !F.Locals[V.P.Local].Owned) {
    Tmp = V.P.Local;
  } else {
    Tmp = FB.newTemp(V.type());
    FB.assign(FB.local(Tmp), FB.use(V));
    V = copyOf(FB.local(Tmp));
  }
  ownLocal(Tmp, T);
  if (!F.Locals[Tmp].Owned)
    return V;
  Scopes.back().Locals.push_back(Tmp);
  FB.storageLive(Tmp);
  return V;
}

bool Body::makesOwned(const ast::Expr *E) {
  E = unparen(E);
  switch (E->getKind()) {
  case ast::Node::Kind::CompositeLit:
    return true;
  case ast::Node::Kind::BinaryExpr:
    return true;
  case ast::Node::Kind::CallExpr: {
    auto *C = llvm::cast<ast::CallExpr>(E);
    const ast::Expr *Fn = unparen(C->Fun);
    // `move(x)` reads a place; the other built-ins and the calls of
    // functions and methods make a value.
    if (auto *Id = dyn_cast<ast::Ident>(Fn)) {
      auto U = I.Uses.find(Id);
      if (U != I.Uses.end())
        if (auto *B = dyn_cast<S::Builtin>(U->second))
          return B->Id != S::BuiltinId::Move;
    }
    // `o.Get()` borrows the Option's value (OWN-P9): no new value.
    if (auto *Sel = dyn_cast<ast::SelectorExpr>(Fn))
      if (const S::Selection *SL = selOf(Sel))
        if (auto *MF = dyn_cast_or_null<S::Func>(SL->Obj);
            MF && MF->IsBuiltinMethod && MF->Name != "Clone")
          return false;
    // A conversion that keeps the representation (`Name(s)`) hands the
    // operand on; one that copies (`string(b)`) makes a value.
    if (C->Args.size() == 1 && I.lookup(Fn) &&
        I.lookup(Fn)->M == S::Mode::TypeExpr) {
      S::Type *To = sTyOf(E), *From = sTyOf(C->Args[0]);
      if (!To || !From)
        return false;
      // An Option or an interface holds the operand itself (wrapped, or in
      // a box boxAs owns): nothing else is made.
      if (asOption(To) || L.isInterface(To))
        return false;
      bool ToStr = isStringType(To), FromStr = isStringType(From);
      if (ToStr != FromStr)
        return true;
      return isa<S::SliceType>(S::under(To)) !=
             isa<S::SliceType>(S::under(From));
    }
    return true;
  }
  default:
    return false;
  }
}

Operand Body::afterExpr(const ast::Expr *E, Operand V) {
  const ast::Expr *U = unparen(E);
  const bool Take = TakeNode && TakeNode == U;
  if (Take)
    TakeNode = nullptr;
  if (V.K == Operand::Const)
    return V;
  S::Type *T = sTyOf(U);
  if (!T || !L.needsDrop(T))
    return V;
  if (makesOwned(U))
    V = ownTemp(std::move(V), T);
  if (Take)
    V = takeValue(std::move(V), T, /*Move=*/MoveNode == U);
  return V;
}

bool Body::isFresh(const Operand &V) {
  if (V.K != Operand::Copy && V.K != Operand::Move)
    return false;
  const Function &F = FB.function();
  if (V.P.Local >= F.Locals.size())
    return false;
  const Local &Lo = F.Locals[V.P.Local];
  if (Lo.K != Local::Temp || !Lo.Owned)
    return false;
  for (const PlaceElem &E : V.P.Proj)
    if (E.K != PlaceElem::Field)
      return false;
  return true;
}

Operand Body::takeValue(Operand V, S::Type *T, bool Move) {
  if (V.K != Operand::Copy || !L.needsDrop(T))
    return V;
  // A fresh value is handed on; an owning place gives its value up (OWN-R3,
  // R4); a shared handle is copied with a retain (OWN-C3), unless `move`
  // takes it.
  if (isFresh(V) || Move || !L.isSharedHandle(T)) {
    if (!isFresh(V) && V.type() && V.type()->isIface())
      return takeIface(std::move(V), T);
    V.K = Operand::Move;
    return V;
  }
  Operand C = cloneValue(std::move(V), T, nullptr);
  if (C.K == Operand::Copy)
    C.K = Operand::Move;
  return C;
}

Operand Body::takeIface(Operand V, S::Type *T) {
  // The one rule for an interface value that leaves its place by a move
  // (`move(v)`, a parameter into its closure cell): it may be a value lent
  // to this function (TYP-7: `(itab | 1, &copy)`, the copy in the caller's
  // frame), which must not outlive the call; gopp.iface_take clones such a
  // value into a box of its own and hands any other reference on (P04.09
  // R1). The place gives its value up as for any move (MoveCheck sees it).
  Type *G = V.type();
  FuncId Take = L.rt(RtFn::IfaceTake, Pos);
  V.K = Operand::Move;
  if (Take == NoId)
    return V;
  LocalId Src = FB.newTemp(G);
  FB.assign(FB.local(Src), FB.use(std::move(V)));
  LocalId Dst = FB.newTemp(G);
  FB.assign(FB.local(Dst), FB.use(zeroOf(G)));
  Rt E(M, FB);
  Type *PU8 = ctx().ptr(ctx().u8());
  callFn(Take, {E.to(PU8, E.addrOf(FB.local(Dst))).Op,
                E.to(PU8, E.addrOf(FB.local(Src))).Op});
  Operand R = ownTemp(copyOf(FB.local(Dst)), T);
  if (R.K == Operand::Copy)
    R.K = Operand::Move;
  return R;
}

Operand Body::passValue(Operand V, S::Type *T) {
  if (V.K != Operand::Copy || !L.isSharedHandle(T))
    return V;
  if (isFresh(V)) {
    V.K = Operand::Move;
    return V;
  }
  Type *G = V.type();
  if (!G || !G->isIface())
    return takeValue(std::move(V), T);
  // An interface argument: a retain, and a borrowed value (TYP-7) stays
  // borrowed for the call.
  FuncId Pass = L.rt(RtFn::IfacePass, Pos);
  if (Pass == NoId)
    return V;
  Place Src = placeOf(std::move(V));
  Rt E(M, FB);
  Type *PU8 = ctx().ptr(ctx().u8());
  LocalId Dst = FB.newTemp(G);
  FB.assign(FB.local(Dst), FB.use(zeroOf(G)));
  callFn(Pass,
         {E.to(PU8, E.addrOf(FB.local(Dst))).Op, E.to(PU8, E.addrOf(Src)).Op});
  Operand R = ownTemp(copyOf(FB.local(Dst)), T);
  if (R.K == Operand::Copy)
    R.K = Operand::Move;
  return R;
}

Operand Body::cloneValue(Operand V, S::Type *T, const ast::Node *At) {
  Type *G = V.type();
  if (T && !L.isSharedHandle(T) && L.needsDrop(T) && !L.Methods.cloneable(T)) {
    // (The checker reports every copy of such a value: E0267 written, E0452
    // made by the language; a byte copy would drop it twice, P04.09 R5.)
    L.internalError(At ? At->getBegin() : Pos,
                    "a clone of a value whose type cannot be cloned");
    return V;
  }
  FuncId CG = G ? L.cloneGlue(T, At ? At->getBegin() : Pos) : NoId;
  if (CG == NoId)
    return V;
  if (At)
    at(At);
  Place P = placeOf(std::move(V));
  Rt E(M, FB);
  Type *PU8 = ctx().ptr(ctx().u8());
  Place Dst = FB.local(FB.newTemp(G));
  FB.assign(Dst, FB.use(zeroOf(G)));
  callFn(CG, {E.to(PU8, E.addrOf(Dst)).Op, E.to(PU8, E.addrOf(P)).Op});
  return ownTemp(copyOf(Dst), T);
}

Operand Body::ownedValue(const ast::Expr *E, S::Type *Want) {
  const ast::Expr *Saved = TakeNode, *SavedConv = OwnedConv;
  TakeNode = unparen(E);
  OwnedConv = unparen(E);
  Operand V = valueFor(E, Want);
  TakeNode = Saved;
  OwnedConv = SavedConv;
  // A conversion of E (an interface box, an Option of a fresh value) is a
  // fresh value too: handed on.
  if (V.K == Operand::Copy && isFresh(V))
    V.K = Operand::Move;
  else if (V.K == Operand::Copy && Want && MoveNode != unparen(E) &&
           L.isSharedHandle(Want) && L.needsDrop(Want))
    // (An interface value converted from another one shares its box.)
    V = takeValue(std::move(V), Want);
  return V;
}

Operand Body::holdValue(Operand V, S::Type *T) {
  // A value taken now and stored later in the statement (a right side of a
  // tuple assignment, an append's element): an owning one is held by an
  // owned temporary of the statement until its store, so a `!` return in
  // between drops it (P04.09 R7); any other is a plain snapshot.
  if (V.K != Operand::Move || !T || !L.needsDrop(T)) {
    Type *G = V.type();
    LocalId Tmp = FB.newTemp(G);
    FB.assign(FB.local(Tmp), FB.use(std::move(V)));
    return copyOf(FB.local(Tmp));
  }
  if (isFresh(V))
    return V;
  Type *G = V.type();
  LocalId Tmp = FB.newTemp(G);
  FB.assign(FB.local(Tmp), FB.use(std::move(V)));
  Operand R = ownTemp(copyOf(FB.local(Tmp)), T);
  if (R.K == Operand::Copy)
    R.K = Operand::Move;
  return R;
}

void Body::dropTempsSince(size_t Depth, size_t Mark) {
  if (Depth == 0 || Depth > Scopes.size())
    return;
  Function &F = FB.function();
  SmallVector<LocalId, 4> &Locs = Scopes[Depth - 1].Locals;
  SmallVector<LocalId, 4> Dead;
  size_t Out = Mark;
  for (size_t K = Mark; K < Locs.size(); ++K) {
    ++LowerSteps;
    LocalId Lo = Locs[K];
    if (F.Locals[Lo].Owned && F.Locals[Lo].K == Local::Temp)
      Dead.push_back(Lo);
    else
      Locs[Out++] = Lo;
  }
  Locs.resize(Out);
  if (FB.terminated())
    return;
  for (size_t K = Dead.size(); K > 0; --K)
    FB.storageDead(Dead[K - 1]);
}

Operand Body::mapEntryOwned(const ast::IndexExpr *IE, bool CommaOk, bool Move) {
  S::MapType *MT = mapTypeOf(sTyOf(IE->X));
  Type *VG = MT ? L.lowerType(MT->Elem, IE->getBegin()) : nullptr;
  if (!VG)
    return errorValue(ctx());
  // `move(m[k])` takes the value out and removes the entry (OWN-R3a);
  // `clone(m[k])` copies it. The comma-ok form says whether k was there
  // (OWN-R9). The value is fresh: the statement's until a new owner takes
  // it (a blank one drops it).
  Rt E(M, FB);
  Val Mv = E.snap(Val{&E, mapValue(IE->X)});
  Operand K = valueFor(IE->Indices[0], MT->Key);
  at(IE);
  Place KP = FB.temp(FB.use(K));
  Operand Info = mapInfoPtr(MT, IE);
  Operand KA = bytesAddr(KP);
  mapKeyCheck(MT, Info, Mv.Op, KA, IE);
  Val P = E.snap(Val{&E, callRt(RtFn::MapGet, {Info, Mv.Op, KA}, IE)});
  Type *TT = ctx().tuple({VG, ctx().boolean()});
  LocalId R = FB.newTemp(TT);
  FB.assign(FB.local(R), FB.use(zeroOf(TT)));
  E.If(P != E.null(P.ty()), [&] {
    Place Slot = E.at(P, VG);
    if (Move) {
      FB.assign(FB.field(FB.local(R), 0), FB.use(copyOf(Slot)));
      // (The entry keeps zero bytes, which the delete drops: nothing.)
      FB.assign(Slot, FB.use(zeroOf(VG)));
      callRt(RtFn::MapDelete, {Info, Mv.Op, KA}, IE);
    } else if (FuncId CG = L.cloneGlue(MT->Elem, IE->getBegin()); CG != NoId) {
      Type *PU8 = ctx().ptr(ctx().u8());
      callFn(CG, {E.to(PU8, E.addrOf(FB.field(FB.local(R), 0))).Op,
                  E.to(PU8, P).Op});
    } else {
      FB.assign(FB.field(FB.local(R), 0), FB.use(copyOf(Slot)));
    }
    FB.assign(FB.field(FB.local(R), 1), FB.use(constBool(ctx(), true)));
  });
  // Inside `for k, v := range m`, moving the current entry moves `v` too
  // (ADR-0016 point 5): reading v later is a use after move.
  if (Move)
    markRangeValueMoved(IE);
  // (The value is field 0 of the pair, at its start: its drop glue drops
  // the pair.)
  if (FuncId G = L.dropGlue(MT->Elem, IE->getBegin()); G != NoId) {
    Local &Lo = FB.function().Locals[R];
    Lo.Owned = true;
    Lo.Glue = G;
    Scopes.back().Locals.push_back(R);
    FB.storageLive(R);
  }
  if (CommaOk)
    return copyOf(FB.local(R));
  return copyOf(FB.field(FB.local(R), 0));
}

void Body::markRangeValueMoved(const ast::IndexExpr *IE) {
  auto *MId = dyn_cast<ast::Ident>(unparen(IE->X));
  auto *KId = IE->Indices.size() == 1
                  ? dyn_cast<ast::Ident>(unparen(IE->Indices[0]))
                  : nullptr;
  if (!MId || !KId)
    return;
  auto Obj = [&](const ast::Ident *Id) -> const S::Object * {
    auto U = I.Uses.find(Id);
    return U == I.Uses.end() ? nullptr : U->second;
  };
  for (size_t K = MapRanges.size(); K > 0; --K) {
    const MapRange &MR = MapRanges[K - 1];
    if (MR.Map != Obj(MId) || MR.Key != Obj(KId) || !MR.Val.Ty)
      continue;
    // (A Move of the borrowed variable: it owns nothing, so the zero bytes
    // elaboration leaves in it are all it does.)
    Operand Mv = copyOf(MR.Val);
    Mv.K = Operand::Move;
    at(IE);
    FB.assign(FB.local(FB.newTemp(MR.Val.Ty)), FB.use(std::move(Mv)));
    return;
  }
}

Operand Body::lowerMoveBuiltin(const ast::CallExpr *C) {
  const ast::Expr *Arg = C->Args[0];
  if (auto *IE = dyn_cast<ast::IndexExpr>(unparen(Arg)); IE && isMapIndex(IE))
    return mapEntryOwned(IE, /*CommaOk=*/false, /*Move=*/true);
  S::Type *T = sTyOf(Arg);
  Type *G = T ? L.lowerType(T, C->getBegin()) : nullptr;
  if (!G)
    return errorValue(ctx());
  // A view is copied and the place stays as it is (OWN-V1, OWN-R5: a zero
  // view does not exist); so is a value that owns nothing.
  if (!L.needsDrop(T) || llvm::isa<S::RefType>(S::under(T)))
    return lowerExpr(Arg);
  // The value is a new owned temporary: the place gave it up (a shared
  // handle too: `move` takes, it does not copy).
  const ast::Expr *Saved = MoveNode;
  MoveNode = unparen(Arg);
  Operand V = ownedValue(Arg, T);
  MoveNode = Saved;
  at(C);
  LocalId Tmp = FB.newTemp(G);
  FB.assign(FB.local(Tmp), FB.use(std::move(V)));
  return ownTemp(copyOf(FB.local(Tmp)), T);
}

Operand Body::lowerCloneOf(const ast::Expr *Src, const ast::Node *At) {
  // The clone has the call's type: `v.Clone()` of a View[string] is a string.
  auto *Call = dyn_cast<ast::Expr>(At);
  S::Type *T = Call && sTyOf(Call) ? sTyOf(Call) : sTyOf(Src);
  Type *G = T ? L.lowerType(T, At->getBegin()) : nullptr;
  if (!G)
    return errorValue(ctx());
  if (L.cloneGlue(T, At->getBegin()) == NoId) // owns nothing: a copy
    return lowerExpr(Src);
  Operand V = lowerExpr(Src);
  if (V.type() && V.type() != G &&
      (G->getKind() == Type::Kind::String ||
       G->getKind() == Type::Kind::Slice)) {
    // A borrowed string or []T, or a view of one, is (ptr, len) (PAR-8): the
    // clone copies its bytes, whatever owns them.
    at(At);
    if (G->getKind() == Type::Kind::String) {
      Place P = placeOf(std::move(V));
      Rt E(M, FB);
      Operand R = callRt(RtFn::StrFromBytes,
                         {E.to(ctx().ptr(ctx().u8()), E.val(FB.field(P, 0))).Op,
                          copyOf(FB.field(P, 1))},
                         At);
      return ownTemp(copyOf(FB.temp(FB.use(std::move(R)))), T);
    }
    V = borrowedSeq(std::move(V), G, At);
  }
  return cloneValue(std::move(V), T, At);
}

//===----------------------------------------------------------------------===//
// Which bodies `gopp check` lowers (P04.09, manager decision M3)
//===----------------------------------------------------------------------===//

bool Body::mayMoveVariable(const ast::Node *Root) {
  // A use after a move needs a move out of a variable: a variable, or a path
  // into one, of an owning type (a shared handle only by `move(x)`, OWN-C3),
  // read where a new owner takes it (OWN-R3). A body without one has nothing
  // the check could report, and `gopp check` does not lower it. `return x`
  // of one result ends its path, and `p = append(p, ...)` refills the place
  // it takes from: neither is followed by a use. One walk over the body,
  // nested literals included, at the instance's types (sTyOf).
  // (The shape first, a walk of the syntax; the names and types only for a
  // path into a variable.)
  auto PathRoot = [&](const ast::Expr *E) -> const ast::Ident * {
    E = unparen(E);
    for (;;) {
      ++LowerSteps;
      if (auto *Sel = dyn_cast<ast::SelectorExpr>(E))
        E = unparen(Sel->X);
      else if (auto *IE = dyn_cast<ast::IndexExpr>(E))
        E = unparen(IE->X);
      else if (auto *SE = dyn_cast<ast::StarExpr>(E))
        E = unparen(SE->X);
      else
        return dyn_cast<ast::Ident>(E);
    }
  };
  auto VarPath = [&](const ast::Expr *E) {
    const ast::Ident *Id = PathRoot(E);
    if (!Id)
      return false;
    auto U = I.Uses.find(Id);
    if (U == I.Uses.end() || !llvm::isa_and_nonnull<S::Var>(U->second))
      return false;
    // (Each selector of the path is a field.)
    for (E = unparen(E); E != Id;) {
      ++LowerSteps;
      if (auto *Sel = dyn_cast<ast::SelectorExpr>(E)) {
        const S::Selection *SL = selOf(Sel);
        if (!SL || SL->K != S::Selection::FieldVal)
          return false;
        E = unparen(Sel->X);
      } else if (auto *IE = dyn_cast<ast::IndexExpr>(E)) {
        E = unparen(IE->X);
      } else {
        E = unparen(llvm::cast<ast::StarExpr>(E)->X);
      }
    }
    return true;
  };
  // A conversion that hands its operand on takes the operand: `MyS(a)`,
  // `any(a)` (TYP-6), `Option[T](a)` (makesOwned).
  auto Unconv = [&](const ast::Expr *E) {
    E = unparen(E);
    while (auto *C = dyn_cast<ast::CallExpr>(E)) {
      ++LowerSteps;
      const S::TypeAndValue *TV =
          C->Args.size() == 1 ? I.lookup(C->Fun) : nullptr;
      if (!TV || TV->M != S::Mode::TypeExpr)
        break;
      S::Type *To = sTyOf(C);
      if (!To || !L.needsDrop(To) || makesOwned(C))
        break;
      E = unparen(C->Args[0]);
    }
    return E;
  };
  auto Taken = [&](const ast::Expr *E) {
    if (!E)
      return false;
    E = Unconv(E);
    if (!PathRoot(E))
      return false;
    S::Type *T = sTyOf(E);
    return T && L.needsDrop(T) && !L.isSharedHandle(T) && VarPath(E);
  };
  auto BuiltinOf = [&](const ast::CallExpr *C) -> const S::Builtin * {
    auto *F = dyn_cast<ast::Ident>(unparen(C->Fun));
    auto U = F ? I.Uses.find(F) : I.Uses.end();
    return U == I.Uses.end() ? nullptr : dyn_cast<S::Builtin>(U->second);
  };
  auto MoveRecv = [&](const ast::SelectorExpr *Sel) {
    const S::Selection *SL = selOf(Sel);
    auto *F = SL ? dyn_cast_or_null<S::Func>(SL->Obj) : nullptr;
    return F && F->MoveRecv;
  };
  // Does the call move an argument out of a variable (a `move` parameter)?
  auto ArgsMove = [&](const ast::CallExpr *C) {
    // (Only a path into a variable, maybe converted, can be taken: a call
    // with none needs no more.)
    bool Any = false;
    for (const ast::Expr *A : C->Args)
      Any = Any || PathRoot(Unconv(A));
    if (!Any)
      return false;
    auto *Sig =
        dyn_cast_or_null<S::SignatureType>(S::under(sTyOf(unparen(C->Fun))));
    if (!Sig || Sig->Params->Elems.empty())
      return false;
    const size_t NP = Sig->Params->Elems.size();
    for (size_t K = 0; K < C->Args.size(); ++K)
      if (Sig->isMove(K < NP ? K : NP - 1) && Taken(C->Args[K]))
        return true;
    return false;
  };
  // An operator on a user type is the method call the checker resolved
  // (Info::Operators, P04.23: `x + y`, `-x`, `x(y)`, a conversion of a
  // @Convert method, `x op= y`, `a[i] = v`): it moves what the call does, a
  // `move` receiver or a `move` parameter (R8).
  auto OpMoves = [&](const ast::CallExpr *C) {
    if (!C)
      return false;
    auto *Sel = dyn_cast<ast::SelectorExpr>(C->Fun);
    return (Sel && MoveRecv(Sel)) || ArgsMove(C);
  };
  // Is the node a move out of a variable?
  auto Moves = [&](const ast::Node *N) {
    // Every call the source does not write (the checker's records of the
    // calls it made, Checker::synthCall: Info::Operators, and the comparison
    // of a `switch` tag with a case value, Info::CaseOperators, R16).
    if (!I.Operators.empty())
      if (const S::OperatorUse *U = opUse(N); U && opResolved(*U))
        if (OpMoves(U->Call) || OpMoves(U->SetCall))
          return true;
    if (!I.CaseOperators.empty())
      if (auto *E = dyn_cast<ast::Expr>(N))
        if (auto It = I.CaseOperators.find(E);
            It != I.CaseOperators.end() && opResolved(It->second))
          if (OpMoves(It->second.Call) || OpMoves(It->second.SetCall))
            return true;
    if (auto *A = dyn_cast<ast::AssignStmt>(N)) {
      if (A->Op != tok::assign && A->Op != tok::define)
        return false;
      if (A->Rhs.size() != A->Lhs.size())
        return false;
      for (const ast::Expr *R : A->Rhs)
        if (Taken(R) && !I.SelfMoves.count(unparen(R)))
          return true;
      return false;
    }
    if (auto *VS = dyn_cast<ast::ValueSpec>(N)) {
      for (const ast::Expr *V : VS->Values)
        if (Taken(V))
          return true;
      return false;
    }
    if (auto *CL = dyn_cast<ast::CompositeLit>(N)) {
      for (const ast::Expr *E : CL->Elts) {
        if (auto *KV = dyn_cast<ast::KeyValueExpr>(E))
          E = KV->Value;
        if (Taken(E))
          return true;
      }
      return false;
    }
    if (auto *Sel = dyn_cast<ast::SelectorExpr>(N))
      return MoveRecv(Sel); // a call or a method value of a `move` receiver
    if (auto *C = dyn_cast<ast::CallExpr>(N)) {
      const S::Builtin *B = BuiltinOf(C);
      if (B && B->Id == S::BuiltinId::Move)
        return true;
      if (B && B->Id == S::BuiltinId::Append) {
        // The base too, unless the result goes back into it (`b :=
        // append(a, x)` moves a; `p = append(p, x)` is Info::SelfMoves).
        for (size_t K = I.SelfMoves.count(unparen(C)) ? 1 : 0;
             K < C->Args.size(); ++K)
          if (Taken(C->Args[K]))
            return true;
        return false;
      }
      return ArgsMove(C);
    }
    if (auto *Snd = dyn_cast<ast::SendStmt>(N))
      return Taken(Snd->Value);
    if (auto *Ret = dyn_cast<ast::ReturnStmt>(N)) {
      // One result ends its path; with two, the second can take what the
      // first took (`return s, s`).
      if (Ret->Results.size() < 2)
        return false;
      for (const ast::Expr *E : Ret->Results)
        if (Taken(E))
          return true;
    }
    return false;
  };
  SmallVector<const ast::Node *, 64> Stack{Root};
  while (!Stack.empty()) {
    const ast::Node *N = Stack.pop_back_val();
    ++LowerSteps;
    if (!N)
      continue;
    if (Moves(N))
      return true;
    if (L.Opts.RescanMoveFilter)
      // (Test-only: a walk of everything before this node at each node, the
      // quadratic control of the stress test.)
      LowerSteps += Stack.size();
    ast::forEachChild(
        N, [&](const char *, const ast::Node *C) { Stack.push_back(C); });
  }
  return false;
}
