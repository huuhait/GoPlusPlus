//===- LowerStr.cpp - Strings, slices, views, maps --------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.05. The expressions and statements that work on a string, a slice, a
// View or a map: indexing and slicing with Go's checks and panic messages,
// `+`, comparisons, conversions, `make`, `append`, `copy`, `len`, `cap`,
// `delete`, `clear`, literals, variadic calls and map entries.
//
// How the values look (gir.md section 15):
//   string      String(ptr, len, cap), built by an aggregate;
//   []T         Slice(ptr, len, cap, allocator) (ADR-0029: the allocator word
//               is null for the global allocator);
//   View[string], View[[]T]    Tuple(ptr, len);   View[T] otherwise: *T; a
//               borrowed []T parameter is a Tuple(ptr, len) too;
//   map[K]V     a pointer to the runtime's object (RuntimeMap.cpp).
// Every one of them is read and written in memory, as the other aggregates
// are. What needs the runtime (allocation, concatenation, growth, UTF-8, the
// map) is a call of a function from RuntimeStr.cpp / RuntimeMap.cpp.
//
// A value the runtime allocates is returned in a temporary marked `Owned`
// (Local::Owned): drop elaboration (P04.09) drops it at the end of the
// statement unless it was moved to a variable. Until then it leaks.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

#include "gopp/GIR/Glue.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::isa_and_nonnull;

//===----------------------------------------------------------------------===//
// Small helpers
//===----------------------------------------------------------------------===//

Body::SeqInfo Body::seqInfo(S::Type *T) {
  SeqInfo R;
  if (!T)
    return R;
  S::Type *U = S::under(T);
  bool View = false;
  if (auto *Rf = dyn_cast<S::RefType>(U)) {
    U = S::under(Rf->Elem);
    View = true;
  }
  if (isStringType(U)) {
    R.K = View ? SeqKind::ViewString : SeqKind::String;
  } else if (auto *Sl = dyn_cast<S::SliceType>(U)) {
    R.K = View ? SeqKind::ViewSlice : SeqKind::Slice;
    R.Elem = Sl->Elem;
  } else if (auto *A = dyn_cast<S::ArrayType>(U)) {
    R.K = View ? SeqKind::PtrArray : SeqKind::Array;
    R.Elem = A->Elem;
  } else if (auto *P = dyn_cast<S::PointerType>(U)) {
    if (auto *A2 = dyn_cast<S::ArrayType>(S::under(P->Elem))) {
      R.K = SeqKind::PtrArray;
      R.Elem = A2->Elem;
    }
  }
  return R;
}

Place Body::seqPlace(Operand V, const SeqInfo &SI) {
  if (SI.K == SeqKind::String || SI.K == SeqKind::Slice)
    return placeOf(std::move(V));
  // A view is the pointer and the length: a String or Slice of the same
  // length and capacity for the code that follows.
  Place P = placeOf(std::move(V));
  Operand Ptr = copyOf(FB.field(P, 0)), Len = copyOf(FB.field(P, 1));
  Type *T = SI.K == SeqKind::ViewString ? ctx().string()
                                        : ctx().slice(P.Ty->elems()[0]->elem());
  // (A view has no allocator word: the slice made from it has none.)
  if (SI.K == SeqKind::ViewSlice) {
    Rt E(M, FB);
    return FB.temp(FB.aggregate(
        AggKind::Slice, T, {Ptr, Len, Len, E.null(ctx().ptr(ctx().u8())).Op}));
  }
  return FB.temp(FB.aggregate(AggKind::String, T, {Ptr, Len, Len}));
}

Operand Body::callFn(FuncId F, ArrayRef<Operand> Args, bool Owned) {
  if (F == NoId)
    return errorValue(ctx());
  LocalId R = FB.newTemp(M.func(F).Sig->ret());
  if (Owned)
    FB.function().Locals[R].Owned = true;
  FB.call(constFunc(M, F), Args, FB.local(R), {UnwindAction::Continue, NoId});
  return copyOf(FB.local(R));
}

Operand Body::callRt(RtFn F, ArrayRef<Operand> Args, const ast::Node *N,
                     bool Owned) {
  return callFn(L.rt(F, N->getBegin()), Args, Owned);
}

Operand Body::u64Of(Operand V) {
  Type *U64 = ctx().u64();
  if (V.type() == U64)
    return V;
  return copyOf(FB.temp(FB.cast(CastKind::IntToInt, std::move(V), U64)));
}

Operand Body::intArg(const ast::Expr *E) {
  Type *I64 = ctx().i64();
  Operand V =
      isConst(E) ? constOperand(*I.lookup(E)->Val, I64, E) : lowerExpr(E);
  if (!V.type()->isInt()) {
    unsupported(E, "an argument of type " + V.type()->str());
    return errorValue(ctx());
  }
  if (V.type() == I64)
    return V;
  return copyOf(FB.temp(FB.cast(CastKind::IntToInt, std::move(V), I64)));
}

/// Can a value of T hold a pointer (the allocator's size classes differ for
/// a block the garbage collector does not scan, which append's growth copies)?
bool Body::hasPointers(Type *T) {
  SmallVector<Type *, 8> Work{T};
  llvm::SmallPtrSet<Type *, 8> Seen;
  while (!Work.empty()) {
    Type *C = Work.pop_back_val();
    if (!Seen.insert(C).second)
      continue;
    switch (C->getKind()) {
    case Type::Kind::Ptr:
    case Type::Kind::String:
    case Type::Kind::Slice:
    case Type::Kind::Func:
      return true;
    case Type::Kind::Array:
    case Type::Kind::Option:
      Work.push_back(C->elem());
      break;
    case Type::Kind::Struct:
    case Type::Kind::Tuple:
      for (Type *E : C->elems())
        Work.push_back(E);
      break;
    default:
      break;
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Indexing
//===----------------------------------------------------------------------===//

void Body::indexCheckDyn(Operand Idx, Operand Len, const ast::Node *N) {
  Type *IT = Idx.type();
  at(N);
  Type *U64 = ctx().u64();
  Operand Wide =
      IT == U64 ? Idx : copyOf(FB.temp(FB.cast(CastKind::IntToInt, Idx, U64)));
  Place Ok = FB.temp(FB.binary(BinOp::Lt, Wide, Len));
  Type *Shown = IT->isSigned() ? ctx().i64() : U64;
  Operand Val = IT == Shown
                    ? Idx
                    : copyOf(FB.temp(FB.cast(CastKind::IntToInt, Idx, Shown)));
  check(copyOf(Ok), AssertKind::Bounds, !IT->isSigned(), N, {Val, Len});
}

Place Body::seqIndexPlace(const ast::IndexExpr *IE, const SeqInfo &SI,
                          Pending *Pend) {
  Place Q;
  if (SI.K == SeqKind::String || SI.K == SeqKind::Slice)
    Q = lowerPlace(IE->X, Pend);
  else
    Q = seqPlace(lowerExpr(IE->X), SI);
  if (!Q.Ty)
    return Place();
  at(IE);
  // A destination: the header as it is now (Body::deriveDest: after the
  // right sides, so a buffer they moved is not written; before the stores,
  // so a later store to the variable in the same statement does not change
  // which element this is). It is read through the element of an outer
  // index, which is checked first.
  if (Pend) {
    flushChecks(Pend);
    Q = FB.temp(FB.use(copyOf(Q)));
  }
  Operand Idx = lowerExpr(IE->Indices[0]);
  if (!Idx.type()->isInt()) {
    unsupported(IE->Indices[0], "an index of type " + Idx.type()->str());
    return Place();
  }
  at(IE);
  Operand Len = copyOf(FB.temp(FB.len(Q)));
  LocalId IL = localOf(std::move(Idx));
  if (Pend) {
    Pend->push_back({IL, 0, IE, localOf(Len)});
    return FB.index(std::move(Q), IL);
  }
  indexCheckDyn(copyOf(FB.local(IL)), Len, IE);
  return FB.index(std::move(Q), IL);
}

//===----------------------------------------------------------------------===//
// Slice expressions (Go spec "Slice expressions"; runtime/panic.go)
//===----------------------------------------------------------------------===//

Operand Body::lowerSliceExpr(const ast::SliceExpr *SE, ShrinkMove *Move) {
  bool Shrink = Move != nullptr;
  S::Type *XS = sTyOf(SE->X);
  SeqInfo SI = seqInfo(XS);
  Type *ResG = Shrink ? L.lowerType(XS, SE->getBegin()) : tyOf(SE);
  if (!ResG)
    return errorValue(ctx());
  // (After a report the result is a zero of its type, so the code that uses
  // it is still well formed.)
  if (SI.K == SeqKind::None) {
    unsupported(SE, "slicing this type");
    return zeroOf(ResG);
  }
  // SLC-13: the three-index form is defined for arrays only in v0 (the
  // checker already refuses it on a string).
  if (SE->Slice3 && SI.K != SeqKind::Array && SI.K != SeqKind::PtrArray) {
    unsupported(SE, "a 3-index slice of a slice or a view (spec SLC-13: "
                    "arrays only in v0)");
    return zeroOf(ResG);
  }
  Rt E(M, FB);
  using V = Val;
  Type *U64 = ctx().u64();
  V Ptr, Len, Cap, OwnCap;
  V Word = E.null(ctx().ptr(ctx().u8())); // the allocator of an owned slice
  Type *ElemG = nullptr;
  bool ByLength = true; // "with length" (string, array), else "with capacity"
  switch (SI.K) {
  case SeqKind::String:
  case SeqKind::ViewString:
  case SeqKind::Slice:
  case SeqKind::ViewSlice: {
    Place Q = SI.K == SeqKind::String || SI.K == SeqKind::Slice
                  ? lowerPlace(SE->X)
                  : seqPlace(lowerExpr(SE->X), SI);
    if (!Q.Ty)
      return failed(SE->X, zeroOf(ResG));
    at(SE);
    // The base as it is now: the bounds may be calls that change it.
    Place H = FB.temp(FB.use(copyOf(Q)));
    Ptr = E.val(FB.field(H, 0));
    Len = E.val(FB.field(H, 1));
    bool Str = SI.K == SeqKind::String || SI.K == SeqKind::ViewString;
    Cap = Str || SI.K == SeqKind::ViewSlice ? Len : E.val(FB.field(H, 2));
    OwnCap = Cap;
    // Read where it is used, from the snapshot H: an in-place re-slice keeps
    // the word; a view made from the slice never reads it (ADR-0029 point 7).
    if (SI.K == SeqKind::Slice)
      Word = E.val(FB.field(H, 3));
    ElemG = Str ? ctx().u8() : H.Ty->elem();
    ByLength = Str;
    break;
  }
  case SeqKind::Array: {
    Place A = lowerPlace(SE->X);
    if (!A.Ty)
      return failed(SE->X, zeroOf(ResG));
    at(SE);
    ElemG = A.Ty->elem();
    Ptr = E.to(ctx().ptr(ElemG), E.addrOf(A));
    Len = Cap = E.u(A.Ty->length());
    break;
  }
  case SeqKind::PtrArray: {
    Operand P = lowerExpr(SE->X);
    if (!P.type()->isPtr() ||
        P.type()->elem()->getKind() != Type::Kind::Array) {
      unsupported(SE, "slicing this type");
      return zeroOf(ResG);
    }
    at(SE);
    Type *AT = P.type()->elem();
    ElemG = AT->elem();
    Ptr = E.to(ctx().ptr(ElemG), E.snap(V{&E, P}));
    Len = Cap = E.u(AT->length());
    break;
  }
  case SeqKind::None:
    break;
  }
  uint64_t Esz = L.layout(ElemG).Size;
  // The bounds, evaluated left to right; each as a number (W) and as it is
  // shown by a failed check (Sh: signed or unsigned like its type).
  struct Bound {
    bool Given = false;
    V W, Sh;
    bool Unsigned = false;
  };
  Bound Bo[3];
  const ast::Expr *Ex[3] = {SE->Lo, SE->Hi, SE->Max};
  for (int K = 0; K < 3; ++K) {
    if (!Ex[K])
      continue;
    Operand O = isConst(Ex[K])
                    ? constOperand(*I.lookup(Ex[K])->Val, ctx().i64(), Ex[K])
                    : lowerExpr(Ex[K]);
    if (!O.type()->isInt()) {
      unsupported(Ex[K], "a bound of type " + O.type()->str());
      return zeroOf(ResG);
    }
    at(SE);
    V Raw = E.snap(V{&E, O});
    Bo[K].Given = true;
    Bo[K].Unsigned = !O.type()->isSigned();
    Bo[K].W = E.to(U64, Raw);
    Bo[K].Sh = E.to(Bo[K].Unsigned ? U64 : ctx().i64(), Raw);
  }
  at(SE);
  V Zero = E.u(0);
  V Lo = Bo[0].Given ? Bo[0].W : Zero;
  V Hi = Bo[1].Given ? Bo[1].W : Len;
  V Mx = Bo[2].Given ? Bo[2].W : Cap;
  auto Check = [&](V Ok, AssertKind K, V First, bool Unsigned, V Second) {
    check(Ok.Op, K, Unsigned, SE, {First.Op, Second.Op});
  };
  if (SE->Slice3) {
    Check(Mx <= Cap, ByLength ? AssertKind::Slice3Alen : AssertKind::Slice3Acap,
          Bo[2].Sh, Bo[2].Unsigned, Cap);
    Check(Hi <= Mx, AssertKind::Slice3B, Bo[1].Sh, Bo[1].Unsigned, Mx);
    Check(Lo <= Hi, AssertKind::Slice3C, Bo[0].Given ? Bo[0].Sh : Zero,
          Bo[0].Given ? Bo[0].Unsigned : true, Hi);
  } else {
    if (Bo[1].Given) {
      Check(Hi <= Cap, ByLength ? AssertKind::SliceAlen : AssertKind::SliceAcap,
            Bo[1].Sh, Bo[1].Unsigned, Cap);
      // SLC-13: the elements of an owned slice past its length are not live
      // (`s = s[:0]` dropped them), so a bound past the length panics, also
      // within the capacity. Past the capacity it is Go's message above.
      if (SI.K == SeqKind::Slice)
        Check(Hi <= Len, AssertKind::SliceAlen, Bo[1].Sh, Bo[1].Unsigned, Len);
    }
    if (Bo[0].Given)
      Check(Lo <= Hi, AssertKind::SliceB, Bo[0].Sh, Bo[0].Unsigned, Hi);
  }
  V NewLen = Hi - Lo;
  V NewCap = Mx - Lo;
  // The pointer moves by lo elements, but not to the end of the block when
  // nothing is left (it would point into the next object).
  Var Delta = E.var(U64, E.u(0), "delta");
  E.If((ResG->getKind() == Type::Kind::Slice ? NewCap : NewLen) != 0,
       [&] { Delta = Lo * Esz; });
  V NewPtr = E.padd(Ptr, E.snap(Delta));
  if (Shrink) {
    if (SI.K != SeqKind::Slice || ResG->getKind() != Type::Kind::Slice) {
      unsupported(SE, "this re-slice");
      return zeroOf(ResG);
    }
    // The kept elements move down to the start of the block when the
    // statement stores the header; the capacity stays.
    Type *PU8 = ctx().ptr(ctx().u8());
    Move->Dst = E.to(PU8, Ptr).Op;
    Move->Src = E.to(PU8, NewPtr).Op;
    Move->Bytes = (NewLen * Esz).Op;
    // (The same slice, shorter: it keeps its allocator.)
    return E.slice(ElemG, Ptr, NewLen, OwnCap, Word).Op;
  }
  switch (ResG->getKind()) {
  case Type::Kind::Slice:
    return E.slice(ElemG, NewPtr, NewLen, NewCap).Op;
  case Type::Kind::String:
    // A part of a string owns no buffer (capacity 0, as a literal).
    return E.str(NewPtr, NewLen, E.u(0)).Op;
  case Type::Kind::Tuple:
    return E.tuple(ResG, {NewPtr, NewLen}).Op;
  default:
    unsupported(SE, "slicing to " + ResG->str());
    return zeroOf(ResG);
  }
}

//===----------------------------------------------------------------------===//
// Strings: +, comparisons
//===----------------------------------------------------------------------===//

/// The parts joined into a new string (one allocation, however many).
Operand Body::concat(ArrayRef<Operand> Parts, const ast::Node *N) {
  if (Parts.size() == 1)
    return Parts[0];
  Rt E(M, FB);
  Type *Str = ctx().string();
  at(N);
  Place Arr = FB.local(FB.newTemp(ctx().array(Str, Parts.size())));
  for (size_t K = 0; K < Parts.size(); ++K)
    FB.assign(FB.constIndex(Arr, uint32_t(K)), FB.use(Parts[K]));
  Val P = E.to(ctx().ptr(Str), E.addrOf(Arr));
  return callRt(RtFn::StrConcat, {P.Op, constInt(ctx().u64(), Parts.size())}, N,
                /*Owned=*/true);
}

/// `a + b + c + ...` is one concatenation: the left-deep chain is flattened
/// by a loop (a chain of thousands of operands must not recurse or copy the
/// prefix once per `+`).
Operand Body::lowerStringAdd(const ast::BinaryExpr *B) {
  SmallVector<const ast::Expr *, 8> Leaves;
  const ast::Expr *Cur = B;
  while (true) {
    auto *BE = dyn_cast<ast::BinaryExpr>(unparen(Cur));
    if (!BE || BE->Op != tok::add || isConst(BE) || hasOpMethod(BE) ||
        !isStringType(sTyOf(BE)))
      break;
    Leaves.push_back(BE->Y);
    Cur = BE->X;
  }
  Leaves.push_back(Cur);
  std::reverse(Leaves.begin(), Leaves.end());
  Type *Str = ctx().string();
  SmallVector<Operand, 8> Parts;
  for (const ast::Expr *Leaf : Leaves) {
    Operand V = valueFor(Leaf, sTyOf(B));
    if (!V.type() || V.type() != Str)
      return errorValue(ctx());
    // Evaluated now: a later operand may change a variable read here.
    if (V.K == Operand::Copy &&
        !(V.P.Proj.empty() &&
          FB.function().Locals[V.P.Local].K == Local::Temp)) {
      at(Leaf);
      V = copyOf(FB.temp(FB.use(V)));
    }
    Parts.push_back(std::move(V));
  }
  return concat(Parts, B);
}

Operand Body::stringEqual(bool Eq, Operand X, Operand Y, const ast::Node *N) {
  Place PX = placeOf(std::move(X)), PY = placeOf(std::move(Y));
  at(N);
  Operand R = callRt(RtFn::StrEq,
                     {copyOf(FB.field(PX, 0)), copyOf(FB.field(PX, 1)),
                      copyOf(FB.field(PY, 0)), copyOf(FB.field(PY, 1))},
                     N);
  if (Eq)
    return R;
  return copyOf(FB.temp(FB.unary(UnOp::Not, std::move(R))));
}

Operand Body::stringArith(BinOp Op, Operand X, Operand Y, const ast::Node *N) {
  if (Op == BinOp::Add)
    return concat({X, Y}, N);
  if (!isComparison(Op)) {
    unsupported(N, "this operator on strings");
    return errorValue(ctx());
  }
  if (Op == BinOp::Eq || Op == BinOp::Ne)
    return stringEqual(Op == BinOp::Eq, std::move(X), std::move(Y), N);
  Place PX = placeOf(std::move(X)), PY = placeOf(std::move(Y));
  at(N);
  Operand C = callRt(RtFn::StrCmp,
                     {copyOf(FB.field(PX, 0)), copyOf(FB.field(PX, 1)),
                      copyOf(FB.field(PY, 0)), copyOf(FB.field(PY, 1))},
                     N);
  return copyOf(FB.temp(FB.binary(Op, std::move(C), constInt(ctx().i64(), 0))));
}

//===----------------------------------------------------------------------===//
// len, cap, min, max
//===----------------------------------------------------------------------===//

Operand Body::lowerLenCap(const ast::CallExpr *C, bool Cap) {
  const ast::Expr *Arg = C->Args[0];
  S::Type *T = sTyOf(Arg);
  Type *Res = tyOf(C);
  if (!Res)
    return errorValue(ctx());
  Rt E(M, FB);
  if (mapTypeOf(T)) {
    Operand Mp = mapValue(Arg);
    at(C);
    Val Mv = E.snap(Val{&E, Mp});
    Var R = E.var(ctx().u64(), E.u(0), "n");
    E.If(Mv != E.null(Mv.ty()),
         [&] { R = E.val(FB.field(E.at(Mv, L.mapType()->elem()), 0)); });
    return E.to(Res, E.snap(R)).Op;
  }
  SeqInfo SI = seqInfo(T);
  if (SI.K == SeqKind::None || SI.K == SeqKind::Array ||
      SI.K == SeqKind::PtrArray) {
    unsupported(C, std::string("the built-in ") + (Cap ? "cap" : "len") +
                       " of this type");
    return errorValue(ctx());
  }
  Place Q = SI.K == SeqKind::String || SI.K == SeqKind::Slice
                ? placeOf(lowerExpr(Arg))
                : seqPlace(lowerExpr(Arg), SI);
  at(C);
  Operand N = Cap && SI.K == SeqKind::Slice ? copyOf(FB.field(Q, 2))
                                            : copyOf(FB.temp(FB.len(Q)));
  return E.to(Res, E.snap(Val{&E, N})).Op;
}

Operand Body::lowerMinMaxString(const ast::CallExpr *C, bool Min) {
  Type *Str = ctx().string();
  LocalId R = FB.newTemp(Str);
  Operand First = lowerAs(C->Args[0], Str);
  at(C);
  FB.assign(FB.local(R), FB.use(First));
  for (size_t K = 1; K < C->Args.size(); ++K) {
    Operand V = lowerAs(C->Args[K], Str);
    at(C);
    Place VP = FB.temp(FB.use(V));
    Operand Better = stringArith(Min ? BinOp::Lt : BinOp::Gt, copyOf(VP),
                                 copyOf(FB.local(R)), C);
    BlockId Take = FB.newBlock(), Join = FB.newBlock();
    FB.branch(std::move(Better), Take, Join);
    FB.setBlock(Take);
    FB.assign(FB.local(R), FB.use(copyOf(VP)));
    FB.gotoBlock(Join);
    FB.setBlock(Join);
  }
  return copyOf(FB.local(R));
}

//===----------------------------------------------------------------------===//
// make, append, copy, delete, clear
//===----------------------------------------------------------------------===//

Operand Body::lowerMake(const ast::CallExpr *C) {
  S::Type *RS = sTyOf(C);
  S::Type *U = RS ? S::under(RS) : nullptr;
  Type *G = tyOf(C);
  if (!G)
    return errorValue(ctx());
  Rt E(M, FB);
  if (auto *MT = dyn_cast_or_null<S::MapType>(U)) {
    Operand Hint =
        C->Args.size() > 1 ? intArg(C->Args[1]) : constInt(ctx().i64(), 0);
    at(C);
    Operand Info = mapInfoPtr(MT, C);
    return callRt(RtFn::MapNew,
                  {Info, u64Of(Hint), E.null(ctx().ptr(ctx().u8())).Op}, C,
                  /*Owned=*/true);
  }
  if (!isa_and_nonnull<S::SliceType>(U) || C->Args.size() < 2) {
    unsupported(C, "this make");
    return errorValue(ctx());
  }
  Type *ElemG = G->elem();
  Val Len = E.snap(Val{&E, intArg(C->Args[1])});
  Val Cap = C->Args.size() > 2 ? E.snap(Val{&E, intArg(C->Args[2])}) : Len;
  at(C);
  Operand Buf = callRt(RtFn::MakeSlice,
                       {E.null(ctx().ptr(ctx().u8())).Op,
                        E.u(L.layout(ElemG).Size).Op, Len.Op, Cap.Op},
                       C);
  Val P = E.to(ctx().ptr(ElemG), Val{&E, Buf});
  return E
      .owned(E.slice(ElemG, P, E.to(ctx().u64(), Len), E.to(ctx().u64(), Cap)))
      .Op;
}

Operand Body::lowerAppend(const ast::CallExpr *C) {
  S::Type *RS = sTyOf(C);
  Type *SG = tyOf(C);
  auto *SliceS = RS ? dyn_cast<S::SliceType>(S::under(RS)) : nullptr;
  if (!SG || !SliceS || SG->getKind() != Type::Kind::Slice || C->Args.empty()) {
    unsupported(C, "this append");
    return errorValue(ctx());
  }
  Rt E(M, FB);
  Type *ElemG = SG->elem();
  Type *U64 = ctx().u64();
  // The slice's operands now (a call, an index), its header after the other
  // arguments (EXP-3: `p = append(p, f())` moves p after f has run, so what
  // f appended to p is kept; gc reads it then too).
  LateOps BaseOps;
  recordOps(C->Args[0], BaseOps);
  // The values to add (evaluated before anything is stored), or the slice.
  SmallVector<Operand, 4> Vals;
  Val SrcPtr, N;
  bool Spread = C->EllipsisLoc.isValid();
  if (Spread) {
    if (C->Args.size() != 2) {
      unsupported(C, "this append");
      return errorValue(ctx());
    }
    S::Type *XS = sTyOf(C->Args[1]);
    SeqInfo SI = seqInfo(XS);
    if (SI.K != SeqKind::String && SI.K != SeqKind::ViewString &&
        SI.K != SeqKind::Slice && SI.K != SeqKind::ViewSlice) {
      unsupported(C, "append of this type");
      return errorValue(ctx());
    }
    Place Q = seqPlace(lowerExpr(C->Args[1]), SI);
    at(C);
    SrcPtr = E.snap(E.val(FB.field(Q, 0)));
    N = E.snap(E.val(FB.field(Q, 1)));
  } else {
    for (size_t K = 1; K < C->Args.size(); ++K) {
      Operand V = valueFor(C->Args[K], SliceS->Elem);
      at(C->Args[K]);
      Vals.push_back(copyOf(FB.temp(FB.use(V))));
    }
    N = E.u(Vals.size());
  }
  Operand Base;
  {
    ReplayScope Scope(*this, BaseOps);
    Base = valueFor(C->Args[0], RS);
  }
  at(C);
  Place H = FB.temp(FB.use(Base));
  Val Ptr0 = E.val(FB.field(H, 0)), Len0 = E.val(FB.field(H, 1)),
      Cap0 = E.val(FB.field(H, 2)), Word0 = E.val(FB.field(H, 3));
  uint64_t Esz = L.layout(ElemG).Size;
  Val NewLen = Len0 + N;
  Var P = E.var(ctx().ptr(ElemG), Ptr0, "p");
  Var Cap = E.var(U64, Cap0, "cap");
  E.If(NewLen > Cap0, [&] {
    Operand G = callRt(RtFn::GrowSlice,
                       {Word0.Op, E.to(ctx().ptr(ctx().u8()), Ptr0).Op, Len0.Op,
                        Cap0.Op, NewLen.Op, E.u(Esz).Op,
                        constBool(ctx(), !hasPointers(ElemG))},
                       C);
    Place GP = placeOf(G);
    P = E.to(ctx().ptr(ElemG), E.val(FB.field(GP, 0)));
    Cap = E.val(FB.field(GP, 1));
  });
  Val Base1 = E.snap(P);
  if (Spread) {
    E.If(N != 0, [&] {
      Val Dst = E.padd(E.to(ctx().ptr(ctx().u8()), Base1), Len0 * Esz);
      callFn(L.libc("memmove", C->getBegin()),
             {Dst.Op, E.to(ctx().ptr(ctx().u8()), SrcPtr).Op, (N * Esz).Op});
    });
  } else {
    for (size_t K = 0; K < Vals.size(); ++K) {
      Val At = E.padd(Base1, (Len0 + K) * Esz);
      FB.assign(E.at(At, ElemG), FB.use(Vals[K]));
    }
  }
  // The result lives in the allocator of the slice it grew from.
  return E.slice(ElemG, Base1, NewLen, E.snap(Cap), E.snap(Word0)).Op;
}

Operand Body::lowerCopy(const ast::CallExpr *C) {
  if (C->Args.size() != 2) {
    unsupported(C, "this copy");
    return errorValue(ctx());
  }
  SeqInfo D = seqInfo(sTyOf(C->Args[0])), Sr = seqInfo(sTyOf(C->Args[1]));
  if ((D.K != SeqKind::Slice && D.K != SeqKind::ViewSlice) ||
      (Sr.K != SeqKind::String && Sr.K != SeqKind::ViewString &&
       Sr.K != SeqKind::Slice && Sr.K != SeqKind::ViewSlice)) {
    unsupported(C, "copy of these types");
    return errorValue(ctx());
  }
  Rt E(M, FB);
  Place DQ = seqPlace(lowerExpr(C->Args[0]), D);
  Place DH = FB.temp(FB.use(copyOf(DQ)));
  Place SQ = seqPlace(lowerExpr(C->Args[1]), Sr);
  at(C);
  Type *ElemG = DH.Ty->elem();
  Val DPtr = E.val(FB.field(DH, 0)), DLen = E.val(FB.field(DH, 1));
  Val SPtr = E.snap(E.val(FB.field(SQ, 0))),
      SLen = E.snap(E.val(FB.field(SQ, 1)));
  Var N = E.var(ctx().u64(), DLen, "n");
  E.If(SLen < DLen, [&] { N = SLen; });
  Val Nb = E.snap(N);
  E.If(Nb != 0, [&] {
    callFn(L.libc("memmove", C->getBegin()),
           {E.to(ctx().ptr(ctx().u8()), DPtr).Op,
            E.to(ctx().ptr(ctx().u8()), SPtr).Op,
            (Nb * L.layout(ElemG).Size).Op});
  });
  return E.to(ctx().i64(), Nb).Op;
}

void Body::lowerDelete(const ast::CallExpr *C) {
  auto *MT = C->Args.size() == 2
                 ? dyn_cast_or_null<S::MapType>(S::under(sTyOf(C->Args[0])))
                 : nullptr;
  if (!MT) {
    unsupported(C, "this delete");
    return;
  }
  Rt E(M, FB);
  Operand Mp = lowerExpr(C->Args[0]);
  Val Mv = E.snap(Val{&E, Mp});
  Operand K = valueFor(C->Args[1], MT->Key);
  at(C);
  Place KP = FB.temp(FB.use(K));
  Operand Info = mapInfoPtr(MT, C);
  Operand KA = bytesAddr(KP);
  mapKeyCheck(MT, Info, Mv.Op, KA, C);
  callRt(RtFn::MapDelete, {Info, Mv.Op, KA}, C);
}

void Body::mapKeyCheck(S::MapType *MT, const Operand &Info, const Operand &Mp,
                       const Operand &KeyAddr, const ast::Node *N) {
  // Only a key that may fail to hash (it holds an interface: Go's
  // HashMightPanic) is hashed in a nil or empty map.
  Type *KG = L.lowerType(MT->Key, N->getBegin());
  if (KG && hasIface(KG))
    callRt(RtFn::MapKeyCheck, {Info, Mp, KeyAddr}, N);
}

void Body::lowerClear(const ast::CallExpr *C) {
  if (C->Args.size() != 1) {
    unsupported(C, "this clear");
    return;
  }
  S::Type *T = sTyOf(C->Args[0]);
  Rt E(M, FB);
  if (auto *MT = dyn_cast_or_null<S::MapType>(S::under(T))) {
    Operand Mp = lowerExpr(C->Args[0]);
    at(C);
    Operand Info = mapInfoPtr(MT, C);
    callRt(RtFn::MapClear, {Info, Mp}, C);
    return;
  }
  SeqInfo SI = seqInfo(T);
  if (SI.K != SeqKind::Slice && SI.K != SeqKind::ViewSlice) {
    unsupported(C, "clear of this type");
    return;
  }
  Place Q = seqPlace(lowerExpr(C->Args[0]), SI);
  at(C);
  Val P = E.val(FB.field(Q, 0)), N = E.val(FB.field(Q, 1));
  uint64_t Esz = L.layout(Q.Ty->elem()).Size;
  E.If(N != 0, [&] {
    callFn(L.libc("memset", C->getBegin()),
           {E.to(ctx().ptr(ctx().u8()), P).Op, E.i32(0).Op, (N * Esz).Op});
  });
}

//===----------------------------------------------------------------------===//
// Maps
//===----------------------------------------------------------------------===//

S::MapType *Body::mapTypeOf(S::Type *T) {
  if (!T)
    return nullptr;
  S::Type *U = S::under(T);
  if (auto *Rf = dyn_cast<S::RefType>(U))
    U = S::under(Rf->Elem);
  return dyn_cast<S::MapType>(U);
}

bool Body::isMapIndex(const ast::IndexExpr *IE) {
  return mapTypeOf(sTyOf(IE->X)) && IE->Indices.size() == 1 && !hasOpMethod(IE);
}

Operand Body::mapValue(const ast::Expr *E) {
  Operand V = lowerExpr(E);
  S::Type *T = sTyOf(E);
  if (T && isa<S::RefType>(S::under(T)) && V.type()->isPtr())
    return copyOf(FB.deref(FB.local(localOf(std::move(V)))));
  return V;
}

/// The MapInfo the runtime is told about K and V by: sizes and the hash and
/// equality functions (the glue of P04.04), built where the map is used.
Operand Body::mapInfoPtr(S::MapType *MT, const ast::Node *N) {
  Type *KG = L.lowerType(MT->Key, N->getBegin());
  Type *VG = L.lowerType(MT->Elem, N->getBegin());
  if (!KG || !VG)
    return errorValue(ctx());
  auto [Hash, Eq] = L.mapKeyFns(MT->Key, N->getBegin());
  if (Hash == NoId || Eq == NoId)
    return errorValue(ctx());
  Type *IS = L.mapInfoType(), *U64 = ctx().u64();
  Place Info = FB.local(FB.newTemp(IS));
  at(N);
  FB.assign(Info, FB.aggregate(AggKind::Struct, IS,
                               {constInt(U64, L.layout(KG).Size),
                                constInt(U64, L.layout(VG).Size),
                                constFunc(M, Hash), constFunc(M, Eq)}));
  return copyOf(FB.temp(FB.addrOf(Info)));
}

Operand Body::bytesAddr(Place P) {
  Operand A = copyOf(FB.temp(FB.addrOf(std::move(P))));
  return copyOf(FB.temp(
      FB.cast(CastKind::PtrToPtr, std::move(A), ctx().ptr(ctx().u8()))));
}

/// m[k], or (m[k], ok): the zero value when the key is absent.
Operand Body::mapRead(const ast::IndexExpr *IE, bool CommaOk) {
  S::MapType *MT = mapTypeOf(sTyOf(IE->X));
  Type *VG = L.lowerType(MT->Elem, IE->getBegin());
  if (!VG)
    return errorValue(ctx());
  Rt E(M, FB);
  Val Mv = E.snap(Val{&E, mapValue(IE->X)});
  Operand K = valueFor(IE->Indices[0], MT->Key);
  at(IE);
  Place KP = FB.temp(FB.use(K));
  Operand Info = mapInfoPtr(MT, IE);
  Operand KA = bytesAddr(KP);
  mapKeyCheck(MT, Info, Mv.Op, KA, IE);
  Val P = E.snap(Val{&E, callRt(RtFn::MapGet, {Info, Mv.Op, KA}, IE)});
  LocalId R = FB.newTemp(VG);
  FB.assign(FB.local(R), FB.use(zeroOf(VG)));
  E.If(P != E.null(P.ty()),
       [&] { FB.assign(FB.local(R), FB.use(copyOf(E.at(P, VG)))); });
  if (!CommaOk)
    return copyOf(FB.local(R));
  Val Ok = E.snap(P != E.null(P.ty()));
  Type *TT = ctx().tuple({VG, ctx().boolean()});
  return E.tuple(TT, {E.val(FB.local(R)), Ok}).Op;
}

/// `m[k] = v` and `mm[a][b] = v`: the address of the variable that holds the
/// outer map (taken now, Body::deriveDest) and the keys, outer to inner. The
/// entries are made when the value is stored (resolveDest): `mm[a][b] = v`
/// makes a's entry, and creates its nil inner map, then (ADR-0018 amendment
/// point 5).
void Body::lowerMapDest(const ast::IndexExpr *IE, Dest &D) {
  SmallVector<const ast::IndexExpr *, 2> Chain{IE};
  const ast::Expr *Owner = unparen(IE->X);
  while (auto *In = dyn_cast<ast::IndexExpr>(Owner)) {
    if (!isMapIndex(In))
      break;
    Chain.push_back(In);
    Owner = unparen(In->X);
  }
  // The owner is read through (its address is used by map_assign): any index
  // in it is checked now, as gc does.
  Place OP = lowerPlace(Owner);
  if (!OP.Ty)
    return;
  at(IE);
  D.MapAddr = copyOf(FB.temp(FB.addrOf(std::move(OP), true)));
  for (size_t K = Chain.size(); K > 0; --K) {
    const ast::IndexExpr *In = Chain[K - 1];
    S::MapType *MT = mapTypeOf(sTyOf(In->X));
    Operand Key = valueFor(In->Indices[0], MT->Key);
    at(In);
    D.MapKeys.push_back({MT, bytesAddr(FB.temp(FB.use(Key)))});
  }
}

void Body::resolveDest(Dest &D, const ast::Node *N) {
  if (D.MapKeys.empty())
    return;
  Rt E(M, FB);
  Operand Addr = D.MapAddr;
  for (size_t K = 0; K < D.MapKeys.size(); ++K) {
    S::MapType *MT = D.MapKeys[K].MT;
    Type *VG = L.lowerType(MT->Elem, N->getBegin());
    if (!VG)
      break;
    Operand Info = mapInfoPtr(MT, N);
    Operand P = callRt(RtFn::MapAssign, {Info, Addr, D.MapKeys[K].KeyAddr}, N);
    at(N);
    Place Slot = E.at(Val{&E, P}, VG);
    if (K + 1 == D.MapKeys.size())
      D.P = std::move(Slot);
    else // the inner map's variable is this entry's value
      Addr = copyOf(FB.temp(FB.addrOf(std::move(Slot), true)));
  }
  D.MapKeys.clear();
}

void Body::mapEntryArg(const ast::IndexExpr *IE, const Place &Recs,
                       unsigned Idx) {
  S::MapType *MT = mapTypeOf(sTyOf(IE->X));
  Operand Info = mapInfoPtr(MT, IE);
  Operand Mv = mapValue(IE->X);
  Operand K = valueFor(IE->Indices[0], MT->Key);
  auto [Hash, Eq] = L.mapKeyFns(MT->Key, IE->getBegin());
  (void)Hash;
  at(IE);
  Place KP = FB.temp(FB.use(K));
  Place R = FB.constIndex(Recs, Idx);
  FB.assign(FB.field(R, 0), FB.use(Info));
  FB.assign(FB.field(R, 1), FB.use(Mv));
  FB.assign(FB.field(R, 2), FB.use(bytesAddr(KP)));
  // The key kind: the id of its eq glue (one per key layout).
  FB.assign(FB.field(R, 3), FB.use(constInt(ctx().u64(), Eq)));
}

//===----------------------------------------------------------------------===//
// Literals
//===----------------------------------------------------------------------===//

Operand Body::sliceLiteral(const ast::CompositeLit *CL, S::SliceType *ST,
                           Type *Ty) {
  // The element at each index (a key sets the index), then the length.
  SmallVector<std::pair<uint64_t, const ast::Expr *>, 8> Elts;
  uint64_t Next = 0, Len = 0;
  for (const ast::Expr *E : CL->Elts) {
    if (auto *KV = dyn_cast<ast::KeyValueExpr>(E)) {
      const S::TypeAndValue *TV = I.lookup(KV->Key);
      int64_t K = 0;
      if (!TV || !TV->Val || !TV->Val->toInt().fitsInt64(K) || K < 0) {
        unsupported(E, "this slice literal");
        return errorValue(ctx());
      }
      Next = K;
      E = KV->Value;
    }
    Elts.push_back({Next, E});
    Len = std::max(Len, ++Next);
  }
  Type *ElemG = Ty->elem();
  Rt E(M, FB);
  at(CL);
  uint64_t Esz = L.layout(ElemG).Size;
  Operand Buf = callRt(RtFn::MemResize,
                       {E.null(ctx().ptr(ctx().u8())).Op, E.u(Len * Esz).Op,
                        constBool(ctx(), true)},
                       CL);
  Val P = E.snap(E.to(ctx().ptr(ElemG), Val{&E, Buf}));
  if (Len) {
    Place Arr = E.at(P, ctx().array(ElemG, Len));
    for (auto &[Idx, Ex] : Elts) {
      Operand V = valueFor(Ex, ST->Elem);
      at(Ex);
      FB.assign(FB.constIndex(Arr, uint32_t(Idx)), FB.use(V));
    }
  }
  at(CL);
  return E.owned(E.slice(ElemG, P, E.u(Len), E.u(Len))).Op;
}

Operand Body::mapLiteral(const ast::CompositeLit *CL, S::MapType *MT) {
  Type *VG = L.lowerType(MT->Elem, CL->getBegin());
  if (!VG)
    return errorValue(ctx());
  Rt E(M, FB);
  at(CL);
  LocalId Mv = FB.newTemp(L.mapType());
  FB.function().Locals[Mv].Owned = true;
  Operand Info = mapInfoPtr(MT, CL);
  FB.assign(FB.local(Mv),
            FB.use(callRt(RtFn::MapNew,
                          {Info, constInt(ctx().u64(), CL->Elts.size()),
                           E.null(ctx().ptr(ctx().u8())).Op},
                          CL)));
  Operand Addr = copyOf(FB.temp(FB.addrOf(FB.local(Mv), true)));
  for (const ast::Expr *Ex : CL->Elts) {
    auto *KV = dyn_cast<ast::KeyValueExpr>(Ex);
    if (!KV) {
      unsupported(Ex, "this map literal");
      return errorValue(ctx());
    }
    Operand K = valueFor(KV->Key, MT->Key);
    at(Ex);
    Place KP = FB.temp(FB.use(K));
    Operand V = valueFor(KV->Value, MT->Elem);
    at(Ex);
    Place VP = FB.temp(FB.use(V));
    Operand Inf = mapInfoPtr(MT, Ex);
    Operand Slot = callRt(RtFn::MapAssign, {Inf, Addr, bytesAddr(KP)}, Ex);
    FB.assign(E.at(Val{&E, Slot}, VG), FB.use(copyOf(VP)));
  }
  return copyOf(FB.local(Mv));
}

//===----------------------------------------------------------------------===//
// Views (D4.5, ADR-0020)
//===----------------------------------------------------------------------===//

Operand Body::viewOfPlace(Place P, S::Type *ViewS, const ast::Node *N) {
  Type *VG = L.lowerType(ViewS, N->getBegin());
  if (!VG)
    return errorValue(ctx());
  Rt E(M, FB);
  if (VG->getKind() == Type::Kind::Tuple) {
    if (P.Ty->getKind() != Type::Kind::String &&
        P.Ty->getKind() != Type::Kind::Slice) {
      unsupported(N, "a view of a value of type " + P.Ty->str());
      return zeroOf(VG);
    }
    return E.tuple(VG, {E.val(FB.field(P, 0)), E.val(FB.field(P, 1))}).Op;
  }
  return E.addrOf(std::move(P)).Op;
}

Operand Body::makeView(const ast::Expr *Ex, S::Type *Want) {
  Type *VG = L.lowerType(Want, Ex->getBegin());
  if (!VG)
    return errorValue(ctx());
  S::Type *Target = cast<S::RefType>(S::under(Want))->Elem;
  // The checker may have recorded the view type for an operand that is the
  // value itself (an implicit conversion), so the lowered value says which
  // it is: a view already (same type), or a value to take a view of.
  Place P;
  unsigned Before = L.Reported;
  if (isConst(Ex))
    P = placeOf(lowerAs(Ex, L.lowerType(Target, Ex->getBegin())));
  else
    P = lowerPlace(Ex);
  // (An operand that was reported is not reported again as a view.)
  if (!P.Ty || L.Reported != Before)
    return failed(Ex, zeroOf(VG));
  at(Ex);
  if (P.Ty == VG)
    return copyOf(std::move(P));
  return viewOfPlace(std::move(P), Want, Ex);
}

//===----------------------------------------------------------------------===//
// Conversions of strings and slices
//===----------------------------------------------------------------------===//

Operand Body::convertStrings(const ast::CallExpr *C, Operand X, S::Type *FromS,
                             S::Type *ToS) {
  Type *To = L.lowerType(ToS, C->getBegin());
  if (!To)
    return errorValue(ctx());
  Type *From = X.type();
  SeqInfo FS = seqInfo(FromS), TS = seqInfo(ToS);
  Rt E(M, FB);
  at(C);
  Type *U8 = ctx().u8(), *I32 = ctx().i32();
  auto IsSlice = [](SeqKind K) {
    return K == SeqKind::Slice || K == SeqKind::ViewSlice;
  };
  auto IsStr = [](SeqKind K) {
    return K == SeqKind::String || K == SeqKind::ViewString;
  };
  if (TS.K == SeqKind::String) {
    if (From->isInt()) {
      Operand V =
          From == ctx().i64()
              ? X
              : copyOf(FB.temp(FB.cast(CastKind::IntToInt, X, ctx().i64())));
      return callRt(RtFn::StrFromRune, {V}, C, true);
    }
    if (IsStr(FS.K) || IsSlice(FS.K)) {
      Place Q = seqPlace(X, FS);
      Type *Elem = IsStr(FS.K) ? U8 : Q.Ty->elem();
      if (Elem == U8)
        return callRt(RtFn::StrFromBytes,
                      {copyOf(FB.field(Q, 0)), copyOf(FB.field(Q, 1))}, C,
                      true);
      if (Elem == I32)
        return callRt(RtFn::StrFromRunes,
                      {copyOf(FB.field(Q, 0)), copyOf(FB.field(Q, 1))}, C,
                      true);
    }
  } else if (TS.K == SeqKind::Slice && IsStr(FS.K)) {
    Place Q = seqPlace(X, FS);
    Type *Elem = To->elem();
    if (Elem == U8)
      return callRt(RtFn::BytesFromStr,
                    {copyOf(FB.field(Q, 0)), copyOf(FB.field(Q, 1))}, C, true);
    if (Elem == I32)
      return callRt(RtFn::RunesFromStr,
                    {copyOf(FB.field(Q, 0)), copyOf(FB.field(Q, 1))}, C, true);
  } else if (TS.K == SeqKind::Slice && FS.K == SeqKind::ViewSlice &&
             From->elems()[0]->elem() == To->elem()) {
    // A copy of what the view shows.
    Place Q = seqPlace(X, FS);
    Type *Elem = To->elem();
    uint64_t Esz = L.layout(Elem).Size;
    Val N = E.snap(E.val(FB.field(Q, 1)));
    Val Src = E.snap(E.val(FB.field(Q, 0)));
    Operand Buf = callRt(
        RtFn::MemResize,
        {E.null(ctx().ptr(U8)).Op, (N * Esz).Op, constBool(ctx(), false)}, C);
    E.If(N != 0, [&] {
      callFn(L.libc("memmove", C->getBegin()),
             {Buf, E.to(ctx().ptr(U8), Src).Op, (N * Esz).Op});
    });
    return E.owned(E.slice(Elem, E.to(ctx().ptr(Elem), Val{&E, Buf}), N, N)).Op;
  } else if ((FS.K == SeqKind::Slice || FS.K == SeqKind::ViewSlice) &&
             (To->getKind() == Type::Kind::Array ||
              (To->isPtr() && To->elem()->getKind() == Type::Kind::Array))) {
    // [N]T(s) and (*[N]T)(s): s must hold N elements (Go's
    // panicSliceConvert); the array is a copy of them, the pointer points
    // at the first.
    Type *AT = To->isPtr() ? To->elem() : To;
    uint64_t N = AT->length();
    Place Q = seqPlace(X, FS);
    Val Ptr = E.snap(E.val(FB.field(Q, 0)));
    Val Len = E.snap(E.val(FB.field(Q, 1)));
    check((Len >= N).Op, AssertKind::SliceConvert, /*Unsigned=*/true, C,
          {Len.Op, E.u(N).Op});
    if (To->isPtr())
      return E.to(To, Ptr).Op;
    Place Arr = FB.local(FB.newTemp(AT));
    FB.assign(Arr, FB.use(zeroOf(AT)));
    uint64_t Bytes = N * L.layout(AT->elem()).Size;
    if (Bytes)
      callFn(L.libc("memmove", C->getBegin()),
             {E.to(ctx().ptr(U8), E.addrOf(Arr)).Op,
              E.to(ctx().ptr(U8), Ptr).Op, E.u(Bytes).Op});
    return copyOf(Arr);
  }
  unsupported(C, "a conversion from " + From->str() + " to " + To->str());
  return errorValue(ctx());
}

//===----------------------------------------------------------------------===//
// Variadic calls
//===----------------------------------------------------------------------===//

/// The arguments from index Fixed on, as the slice the variadic parameter is.
Operand Body::packVariadic(const ast::CallExpr *C, S::SignatureType *Sig,
                           size_t Fixed) {
  return packVariadicWith(C, Sig, C->Args.size() - Fixed,
                          [&](size_t K, S::Type *Elem) {
                            return valueFor(C->Args[Fixed + K], Elem);
                          });
}

Operand Body::packVariadicWith(
    const ast::CallExpr *C, S::SignatureType *Sig, size_t N,
    llvm::function_ref<Operand(size_t K, S::Type *Elem)> ElemAt) {
  S::Type *ParamS = Sig->Params->Elems.back();
  auto *SliceS = dyn_cast<S::SliceType>(S::under(ParamS));
  Type *SG = L.lowerType(ParamS, C->getBegin());
  if (!SliceS || !SG)
    return errorValue(ctx());
  if (N == 0)
    return zeroOf(SG);
  Rt E(M, FB);
  Type *ElemG = SG->elem();
  at(C);
  Operand Buf =
      callRt(RtFn::MemResize,
             {E.null(ctx().ptr(ctx().u8())).Op,
              E.u(N * L.layout(ElemG).Size).Op, constBool(ctx(), false)},
             C);
  Val P = E.snap(E.to(ctx().ptr(ElemG), Val{&E, Buf}));
  Place Arr = E.at(P, ctx().array(ElemG, N));
  for (size_t K = 0; K < N; ++K) {
    Operand V = ElemAt(K, SliceS->Elem);
    at(C);
    FB.assign(FB.constIndex(Arr, uint32_t(K)), FB.use(V));
  }
  return E.owned(E.slice(ElemG, P, E.u(N), E.u(N))).Op;
}

//===----------------------------------------------------------------------===//
// unsafe.Slice, unsafe.String, unsafe.SliceData, unsafe.StringData
//===----------------------------------------------------------------------===//

/// Go's checks (runtime/unsafe.go): a negative length, a length whose bytes
/// would run past the end of the address space, and a nil pointer with a
/// length that is not zero panic. The slice or string owns nothing it shows
/// (a string's capacity is 0).
Operand Body::lowerUnsafeSeq(const ast::CallExpr *C, S::BuiltinId Id) {
  Type *Res = tyOf(C);
  if (!Res)
    return errorValue(ctx());
  Rt E(M, FB);
  if (Id == S::BuiltinId::SliceData || Id == S::BuiltinId::StringData) {
    SeqInfo SI = seqInfo(sTyOf(C->Args[0]));
    if (SI.K == SeqKind::None || SI.K == SeqKind::Array ||
        SI.K == SeqKind::PtrArray || !Res->isPtr()) {
      unsupported(C, "this unsafe call");
      return zeroOf(Res);
    }
    Place Q = seqPlace(lowerExpr(C->Args[0]), SI);
    at(C);
    return E.to(Res, E.val(FB.field(Q, 0))).Op;
  }
  bool IsSlice = Id == S::BuiltinId::Slice;
  // unsafe.Slice is a borrowed []T (SLC-8): the pointer and the length.
  bool AsPair = IsSlice && Res->getKind() == Type::Kind::Tuple &&
                Res->numFields() == 2 && Res->elems()[0]->isPtr();
  if (IsSlice ? Res->getKind() != Type::Kind::Slice && !AsPair
              : Res->getKind() != Type::Kind::String) {
    unsupported(C, "this unsafe call");
    return zeroOf(Res);
  }
  Operand P = lowerExpr(C->Args[0]);
  Operand N = intArg(C->Args[1]);
  if (!P.type()->isPtr())
    return failed(C->Args[0], zeroOf(Res));
  at(C);
  Type *U64 = ctx().u64();
  Val Ptr = E.snap(Val{&E, P}), Len = E.snap(Val{&E, N});
  Type *Elem = AsPair    ? Res->elems()[0]->elem()
               : IsSlice ? Res->elem()
                         : ctx().u8();
  uint64_t Esz = L.layout(Elem).Size;
  std::string Fn = IsSlice ? "unsafe.Slice: " : "unsafe.String: ";
  FuncId Panic = L.rt(RtFn::PanicText, C->getBegin());
  if (Panic == NoId)
    return failed(C, zeroOf(Res));
  auto Fail = [&](const std::string &Why) {
    Place S = FB.temp(FB.use(constStr(M, Fn + Why)));
    E.callNoReturn(Panic,
                   {E.i32(0), E.val(FB.field(S, 0)), E.val(FB.field(S, 1))});
  };
  Val IsNil = E.snap(Ptr == E.null(Ptr.ty()));
  E.If(Len < E.i(0), [&] { Fail("len out of range"); });
  Val U = E.to(U64, Len);
  if (Esz == 0) {
    E.If(E.land(IsNil, [&] { return U != 0; }),
         [&] { Fail("ptr is nil and len is not zero"); });
  } else {
    // mem = esz * len, and it must not overflow nor pass -uintptr(ptr).
    Val Over = U > E.u(~uint64_t(0) / Esz);
    Val Room = E.u(0) - E.to(U64, Ptr);
    E.If(E.lor(Over, [&] { return U * Esz > Room; }), [&] {
      E.If(IsNil, [&] { Fail("ptr is nil and len is not zero"); });
      Fail("len out of range");
    });
  }
  if (AsPair)
    return E.tuple(Res, {E.to(ctx().ptr(Elem), Ptr), U}).Op;
  if (IsSlice)
    return E.slice(Elem, E.to(ctx().ptr(Elem), Ptr), U, U).Op;
  return E.str(E.to(ctx().ptr(ctx().u8()), Ptr), U, E.u(0)).Op;
}
