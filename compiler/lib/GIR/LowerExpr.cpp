//===- LowerExpr.cpp - The checked AST to GIR: expressions ----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Places (what an lvalue is: lowerPlace and its helpers), values (lowerExpr),
// the operators with Go's checks (arith, D4.12), conversions, calls, Option
// and composite literals. Reads the checker's records only: the type of an
// expression is Info::typeOf, a constant its Val, what a name denotes
// Info::Uses, a field path Info::Selections.
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

#include <cstring>

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

Operand Body::errorValue(Context &C) {
  return failed(nullptr, constInt(C.i64(), 0));
}

Operand Body::zeroOfExpr(const ast::Expr *E) {
  if (S::Type *T = sTyOf(E))
    if (Type *G = L.lowerType(T, E->getBegin()))
      return zeroOf(G);
  return constInt(ctx().i64(), 0);
}

Operand Body::failed(const ast::Node *N, Operand V) {
  if (!L.Failed)
    L.internalError(N ? N->getBegin() : Pos,
                    "an expression could not be lowered and nothing said "
                    "why (a fact of the checker is missing)");
  return V;
}

bool Body::isNilValue(const ast::Expr *E) {
  const S::TypeAndValue *TV = I.lookup(E);
  if (!TV)
    return false;
  if (TV->M == S::Mode::NilValue)
    return true;
  auto *B = dyn_cast_or_null<S::BasicType>(TV->Ty);
  return B && B->BK == S::BasicKind::UntypedNil;
}

S::OptionType *Body::asOption(S::Type *T) {
  return T ? dyn_cast<S::OptionType>(S::under(T)) : nullptr;
}

Operand Body::zeroOf(Type *T) {
  if (T->isInt())
    return constInt(T, 0);
  if (T->isBool())
    return constBool(ctx(), false);
  if (T->isFloat())
    return constFloat(T, 0.0);
  Operand Z;
  Z.K = Operand::Const;
  Z.C.K = T->isPtr() ? Constant::Null : Constant::Zero;
  Z.C.Ty = T;
  return Z;
}

Place Body::placeOf(Operand Op) {
  if (Op.K == Operand::Const)
    return FB.temp(FB.use(Op));
  return Op.P;
}

LocalId Body::localOf(Operand Op) {
  // A plain temporary is its own local; anything else (a variable that may
  // change before the use, a field, a constant) is copied into a new one, so
  // an index or a pointer is the value at the time it was evaluated.
  if (Op.K == Operand::Copy && Op.P.Proj.empty() &&
      FB.function().Locals[Op.P.Local].K == Local::Temp)
    return Op.P.Local;
  LocalId T = FB.newTemp(Op.type());
  FB.assign(FB.local(T), FB.use(Op));
  return T;
}

//===----------------------------------------------------------------------===//
// Constants
//===----------------------------------------------------------------------===//

Operand Body::constOperand(const S::ConstValue &V, Type *Ty,
                           const ast::Node *N) {
  if (!Ty)
    return failed(N, constBool(ctx(), false));
  Constant C;
  if (L.constantOf(V, Ty, C)) {
    Operand O;
    O.K = Operand::Const;
    O.C = C;
    return O;
  }
  // A complex constant is a pair of floats.
  if (Ty->getKind() == Type::Kind::Tuple && Ty->numFields() == 2 &&
      Ty->elems()[0]->isFloat() && Ty->elems()[1]->isFloat()) {
    S::ConstValue CV = V.toComplex();
    if (!CV.isUnknown()) {
      Type *F = Ty->elems()[0];
      bool F32 = F->bits() == 32;
      double Re = F32 ? CV.real().float32() : CV.real().float64();
      double Im = F32 ? CV.imag().float32() : CV.imag().float64();
      Place P = FB.temp(FB.aggregate(AggKind::Tuple, Ty,
                                     {constFloat(F, Re), constFloat(F, Im)}));
      return copyOf(P);
    }
  }
  unsupported(N, "a constant of type " + Ty->str());
  return errorValue(ctx());
}

Operand Body::lowerAs(const ast::Expr *E, Type *Want) {
  if (isConst(E))
    return constOperand(*I.lookup(E)->Val, Want, E);
  return lowerExpr(E);
}

//===----------------------------------------------------------------------===//
// Option (D4.5, ADR-0018): a pointer kind is the pointer, any other T is
// Option<T> = T and a flag.
//===----------------------------------------------------------------------===//

Operand Body::nilOf(S::Type *Want, const ast::Node *N) {
  Type *G = Want ? L.lowerType(Want, N->getBegin()) : nullptr;
  if (!G)
    return errorValue(ctx());
  S::Type *WU = S::under(Want);
  if (!asOption(Want) && !isa<S::SliceType>(WU) && !isa<S::MapType>(WU) &&
      !G->isIface()) {
    unsupported(N, "nil of type " + G->str());
    return errorValue(ctx());
  }
  return zeroOf(G);
}

Operand Body::wrapSome(Operand V, S::Type *OptionTy, const ast::Node *N) {
  Type *G = L.lowerType(OptionTy, N->getBegin());
  if (!G)
    return errorValue(ctx());
  if (G->isPtr() || G->isIface())
    return V;
  at(N);
  Place P = FB.temp(
      FB.aggregate(AggKind::Tuple, G, {std::move(V), constBool(ctx(), true)}));
  return copyOf(P);
}

Operand Body::optionIsSet(Operand O, const ast::Node *N) {
  at(N);
  // An operand that could not be lowered (an error was reported) is not an
  // Option: no flag to read.
  if (!O.type()->isPtr() && !O.type()->isIface() &&
      O.type()->getKind() != Type::Kind::Option)
    return failed(N, constBool(ctx(), false));
  if (O.type()->isIface()) {
    Place P = placeOf(std::move(O));
    return copyOf(FB.temp(FB.binary(BinOp::Ne, copyOf(FB.field(P, 0)),
                                    zeroOf(ctx().ptr(ctx().u8())))));
  }
  if (O.type()->isPtr())
    return copyOf(
        FB.temp(FB.binary(BinOp::Ne, std::move(O), zeroOf(O.type()))));
  return copyOf(FB.field(placeOf(std::move(O)), 1));
}

Operand Body::optionPayload(Operand O) {
  if (O.type()->isPtr() || O.type()->isIface() ||
      O.type()->getKind() != Type::Kind::Option)
    return O;
  return copyOf(FB.field(placeOf(std::move(O)), 0));
}

Operand Body::valueFor(const ast::Expr *E, S::Type *Want) {
  if (isNilValue(E))
    return nilOf(Want, E);
  if (S::OptionType *O = asOption(Want)) {
    if (!asOption(sTyOf(E))) {
      Operand V = valueFor(E, O->Elem);
      return wrapSome(std::move(V), Want, E);
    }
    // An Option[T] or Option[J] where an Option[I] is wanted: Option[I] is
    // the interface itself (its nil the null itab), so a nil one stays nil
    // and a set one is converted (boxed, or its itab made for I).
    if (L.isInterface(O->Elem) && sTyOf(E) != Want)
      return toIface(E, O->Elem);
  }
  Type *G = L.lowerType(Want, E->getBegin());
  if (!G)
    return errorValue(ctx());
  if (G->isIface() && L.isInterface(Want))
    return toIface(E, Want);
  // A value for a View parameter or variable: the view of it.
  S::Type *WU = S::under(Want);
  S::Type *ES = sTyOf(E);
  if (isa<S::RefType>(WU))
    return makeView(E, Want);
  // A view where a string or a slice is borrowed: a borrowed parameter, a
  // re-pointed borrowed variable (PAR-2), a string key looked up (OWN-V4).
  if (ES && isa<S::RefType>(S::under(ES)) &&
      (G->getKind() == Type::Kind::String ||
       G->getKind() == Type::Kind::Slice)) {
    unsigned Before = L.Reported;
    Operand V = lowerExpr(E);
    if (L.Reported != Before)
      return zeroOf(G); // reported once, where it failed
    return borrowedSeq(std::move(V), G, E);
  }
  return lowerAs(E, G);
}

Operand Body::argFor(const ast::Expr *E, S::SignatureType *Sig, size_t K) {
  // (A borrowed map parameter is lowerCall's: mapArgAddr.)
  return argAs(valueFor(E, Sig->Params->Elems[K]), Sig, K, E);
}

Operand Body::mapArgAddr(const ast::Expr *E, LateOps &R) {
  // PAR-10: the address of the variable, field or element that holds the
  // map (a map entry is copied in and out around the call instead: lowerCall,
  // mapEntryArg); a value that is no place is in a temporary: what the callee
  // creates there is dropped with it. Under R's replay (lowerCall: the
  // address is taken after the other arguments, which may grow the
  // container it points into).
  ReplayScope Scope(*this, R);
  Place P = lowerPlace(E);
  if (!P.Ty)
    return errorValue(ctx());
  at(E);
  if (P.Ty == L.mapType())
    return copyOf(FB.temp(FB.addrOf(std::move(P), true)));
  return copyOf(std::move(P));
}

Operand Body::argAs(Operand V, S::SignatureType *Sig, size_t K,
                    const ast::Node *N) {
  Type *T = V.type();
  if (L.mapByPointer(Sig, K) && T == L.mapType()) {
    // A map that is no place: its own temporary (a result's temporary may
    // not have its address taken, V066).
    at(N);
    LocalId Tmp = FB.newTemp(T);
    FB.assign(FB.local(Tmp), FB.use(std::move(V)));
    return copyOf(FB.temp(FB.addrOf(FB.local(Tmp), true)));
  }
  if (Sig->isMove(K) || !T || T->getKind() != Type::Kind::Slice)
    return V;
  // A borrowed []T passes as a view, (ptr, len): the allocator word stays
  // behind (ADR-0029 point 7, D4.4), and the callee's capacity is its length
  // (SLC-12).
  Place P = placeOf(std::move(V));
  at(N);
  Type *VT = ctx().tuple({T->fieldType(0, ctx()), ctx().u64()});
  return copyOf(FB.temp(FB.aggregate(
      AggKind::Tuple, VT, {copyOf(FB.field(P, 0)), copyOf(FB.field(P, 1))})));
}

Operand Body::borrowedSeq(Operand V, Type *G, const ast::Node *N) {
  if (V.type() == G)
    return V;
  Type *VT = V.type();
  if (VT->getKind() != Type::Kind::Tuple || VT->numFields() != 2 ||
      !VT->elems()[0]->isPtr()) {
    unsupported(N, "a borrowed " + G->str() + " made from " + VT->str());
    return zeroOf(G);
  }
  Place P = placeOf(std::move(V));
  at(N);
  Operand Ptr = copyOf(FB.field(P, 0)), Len = copyOf(FB.field(P, 1));
  bool Str = G->getKind() == Type::Kind::String;
  if (Ptr.type() != G->fieldType(0, ctx()))
    Ptr = copyOf(FB.temp(
        FB.cast(CastKind::PtrToPtr, std::move(Ptr), G->fieldType(0, ctx()))));
  if (!Str) {
    // A borrowed []T owns nothing: no allocator (ADR-0029 point 7).
    Rt E(M, FB);
    return copyOf(FB.temp(FB.aggregate(
        AggKind::Slice, G,
        {std::move(Ptr), Len, Len, E.null(ctx().ptr(ctx().u8())).Op})));
  }
  return copyOf(FB.temp(FB.aggregate(
      AggKind::String, G, {std::move(Ptr), Len, constInt(ctx().u64(), 0)})));
}

Operand Body::lowerNarrowed(const ast::Ident *Id, S::Type *Payload) {
  (void)Payload;
  Place P = identPlace(Id);
  if (!P.Ty)
    return errorValue(ctx());
  return optionPayload(copyOf(P));
}

//===----------------------------------------------------------------------===//
// Places
//===----------------------------------------------------------------------===//

Place Body::identPlace(const ast::Ident *Id) {
  auto U = I.Uses.find(Id);
  if (U != I.Uses.end())
    if (auto *V = dyn_cast<S::Var>(U->second)) {
      // An `&` range variable or a parameter passed by a hidden pointer: the
      // place it points at.
      auto Al = Aliases.find(V);
      if (Al != Aliases.end()) {
        at(Id);
        return FB.deref(FB.local(Al->second));
      }
      auto It = Vars.find(V);
      if (It != Vars.end())
        return FB.local(It->second);
      auto G = L.Globals.find(V);
      if (G != L.Globals.end()) {
        at(Id);
        return FB.global(G->second);
      }
    }
  // A variable whose declaration was refused has been reported there.
  if (!L.Failed)
    unsupported(Id, "the name " + Id->Name.str());
  return Place();
}

Place Body::followFields(Place Base, S::Type *BaseTy, ArrayRef<int> Path,
                         const ast::Node *N, Pending *Pend) {
  S::Type *Cur = BaseTy;
  for (int Idx : Path) {
    S::Type *U = S::under(Cur);
    if (auto *P = dyn_cast<S::PointerType>(U)) {
      // The implicit indirection `p.x` is an operand of an assignment: the
      // pointer is copied (Body::deriveDest: before the stores), so a later
      // store to p in the same statement does not change where this place
      // is. An index it is read through is checked first.
      flushChecks(Pend);
      Base = FB.deref(FB.local(localOf(copyOf(std::move(Base)))));
      Cur = P->Elem;
      U = S::under(Cur);
    } else if (auto *Rf = dyn_cast<S::RefType>(U)) {
      // The same through a View[P].
      flushChecks(Pend);
      Base = FB.deref(FB.local(localOf(copyOf(std::move(Base)))));
      Cur = Rf->Elem;
      U = S::under(Cur);
    }
    auto *ST = dyn_cast<S::StructType>(U);
    if (!ST || Idx < 0 || size_t(Idx) >= ST->Fields.size()) {
      unsupported(N, "this field access");
      return Place();
    }
    Base = FB.field(std::move(Base), Idx);
    Cur = ST->Fields[Idx]->Ty;
  }
  return Base;
}

Place Body::selectorPlace(const ast::SelectorExpr *Sel, Pending *Pend) {
  auto It = I.Selections.find(Sel);
  if (It == I.Selections.end()) {
    unsupported(Sel, "a name from another package");
    return Place();
  }
  const S::Selection &SL = It->second;
  if (SL.K != S::Selection::FieldVal) {
    unsupported(Sel, "a method value or expression (P04.07)");
    return Place();
  }
  if (SL.ParentSteps) {
    unsupported(Sel, "a field of a parent type (P04.22)");
    return Place();
  }
  // x.a.b.c is one place, made by a loop: the parser does not count
  // selectors, so a chain (through a pointer to the same struct) can be far
  // longer than the nesting limit, and this must not recurse per link.
  SmallVector<const ast::SelectorExpr *, 8> Chain{Sel};
  const ast::Expr *Root = unparen(Sel->X);
  while (auto *Inner = dyn_cast<ast::SelectorExpr>(Root)) {
    auto In = I.Selections.find(Inner);
    if (isConst(Inner) || In == I.Selections.end() ||
        In->second.K != S::Selection::FieldVal || In->second.ParentSteps)
      break;
    Chain.push_back(Inner);
    Root = unparen(Inner->X);
  }
  Place Base = lowerPlace(Root, Pend);
  for (size_t K = Chain.size(); K > 0 && Base.Ty; --K) {
    const ast::SelectorExpr *S = Chain[K - 1];
    at(S);
    Base = followFields(std::move(Base), sTyOf(S->X),
                        I.Selections.find(S)->second.Index, S, Pend);
  }
  return Base;
}

void Body::check(Operand Cond, AssertKind K, bool Unsigned, const ast::Node *N,
                 ArrayRef<Operand> Args) {
  // The panic function the shared block calls (ADR-0028 rule 3.3).
  L.ensurePanic(K, Unsigned, N->getBegin());
  at(N);
  FB.assertThat(std::move(Cond), true, K, {}, NoId, Args);
}

void Body::indexCheck(Operand Idx, uint64_t Len, const ast::Node *N) {
  Type *IT = Idx.type();
  at(N);
  if (!(!IT->isSigned() && IT->bits() < 64 &&
        (uint64_t(1) << IT->bits()) <= Len)) {
    // idx < len as an unsigned compare: a negative index is a huge one. The
    // message shows the index as the number it is (signed or unsigned).
    Type *U64 = ctx().u64();
    Operand Wide = IT == U64
                       ? Idx
                       : copyOf(FB.temp(FB.cast(CastKind::IntToInt, Idx, U64)));
    Place Ok = FB.temp(FB.binary(BinOp::Lt, Wide, constInt(U64, Len)));
    Type *Shown = IT->isSigned() ? ctx().i64() : U64;
    Operand Val =
        IT == Shown ? Idx
                    : copyOf(FB.temp(FB.cast(CastKind::IntToInt, Idx, Shown)));
    check(copyOf(Ok), AssertKind::Bounds, !IT->isSigned(), N,
          {Val, constInt(U64, Len)});
  }
}

void Body::emitIndexChecks(const Pending &Checks) {
  for (const PendingCheck &C : Checks)
    if (C.LenLocal != NoId)
      indexCheckDyn(copyOf(FB.local(C.Idx)), copyOf(FB.local(C.LenLocal)), C.N);
    else
      indexCheck(copyOf(FB.local(C.Idx)), C.Len, C.N);
}

Place Body::checkedIndex(Place Arr, Operand Idx, uint64_t Len, bool IdxConst,
                         const ast::Node *N, Pending *Pend) {
  if (Pend) {
    // A destination: the index is evaluated now (a local), the check waits
    // for the assignment (Go spec, phase 2; it runs after the right sides).
    LocalId IL = localOf(std::move(Idx));
    if (!IdxConst)
      Pend->push_back({IL, Len, N});
    return FB.index(std::move(Arr), IL);
  }
  if (!IdxConst)
    indexCheck(Idx, Len, N);
  return FB.index(std::move(Arr), localOf(std::move(Idx)));
}

Place Body::indexPlace(const ast::IndexExpr *IE, Pending *Pend) {
  if (IE->Indices.size() != 1 || I.Operators.count(IE)) {
    unsupported(IE, "this index expression");
    return Place();
  }
  S::Type *XT = sTyOf(IE->X);
  if (isMapIndex(IE))
    return placeOf(mapRead(IE, false));
  SeqInfo SI = seqInfo(XT);
  if (SI.K == SeqKind::String || SI.K == SeqKind::Slice ||
      SI.K == SeqKind::ViewString || SI.K == SeqKind::ViewSlice)
    return seqIndexPlace(IE, SI, Pend);
  S::Type *U = XT ? S::under(XT) : nullptr;
  bool ViaPtr = false;
  if (auto *P = dyn_cast_or_null<S::PointerType>(U)) {
    U = S::under(P->Elem);
    ViaPtr = true;
  } else if (auto *Rf = dyn_cast_or_null<S::RefType>(U)) {
    U = S::under(Rf->Elem);
    ViaPtr = true;
  }
  auto *AT = dyn_cast_or_null<S::ArrayType>(U);
  if (!AT) {
    if (L.lowerType(XT, IE->getBegin()))
      unsupported(IE, "indexing this type");
    return Place();
  }
  Place Base;
  if (ViaPtr) {
    Operand P = lowerExpr(IE->X);
    Base = FB.deref(FB.local(localOf(std::move(P))));
  } else {
    Base = lowerPlace(IE->X, Pend);
  }
  if (!Base.Ty)
    return Place();
  const ast::Expr *Ix = IE->Indices[0];
  at(IE);
  if (isConst(Ix)) {
    S::ConstValue CV = I.lookup(Ix)->Val->toInt();
    int64_t K = 0;
    if (CV.isUnknown() || !CV.fitsInt64(K) || K < 0 || K >= AT->Len) {
      unsupported(IE, "this constant index");
      return Place();
    }
    return FB.constIndex(std::move(Base), uint32_t(K));
  }
  Operand Idx = lowerExpr(Ix);
  if (!Idx.type()->isInt()) {
    unsupported(Ix, "an index of type " + Idx.type()->str());
    return Place();
  }
  return checkedIndex(std::move(Base), std::move(Idx), AT->Len, false, IE,
                      Pend);
}

Place Body::starPlace(const ast::StarExpr *St) {
  Operand P = lowerExpr(St->X);
  if (!P.type()->isPtr()) {
    unsupported(St, "this dereference");
    return Place();
  }
  at(St);
  return FB.deref(FB.local(localOf(std::move(P))));
}

Place Body::lowerPlace(const ast::Expr *E, Pending *Pend) {
  E = unparen(E);
  switch (E->getKind()) {
  case ast::Node::Kind::Ident: {
    auto *Id = cast<ast::Ident>(E);
    if (!isConst(E) && !I.narrowedType(Id))
      return identPlace(Id);
    break;
  }
  case ast::Node::Kind::SelectorExpr:
    if (!isConst(E))
      return selectorPlace(cast<ast::SelectorExpr>(E), Pend);
    break;
  case ast::Node::Kind::IndexExpr:
    if (!isConst(E))
      return indexPlace(cast<ast::IndexExpr>(E), Pend);
    break;
  case ast::Node::Kind::StarExpr:
    return starPlace(cast<ast::StarExpr>(E));
  default:
    break;
  }
  // Not addressable: the value in a temporary.
  Operand V = lowerExpr(E);
  if (V.type() == nullptr)
    return Place();
  return placeOf(std::move(V));
}

//===----------------------------------------------------------------------===//
// Values
//===----------------------------------------------------------------------===//

Operand Body::lowerIdent(const ast::Ident *Id) {
  if (S::Type *N = I.narrowedType(Id))
    return lowerNarrowed(Id, N);
  Place P = identPlace(Id);
  if (!P.Ty)
    return errorValue(ctx());
  return copyOf(P);
}

Operand Body::lowerExpr(const ast::Expr *E) {
  // An operand evaluated before (recordOps): its value then.
  if (Replay) {
    auto It = Replay->Saved.find(E);
    if (It != Replay->Saved.end())
      return copyOf(FB.local(It->second));
  }
  if (isConst(E))
    return constOperand(*I.lookup(E)->Val, tyOf(E), E);
  if (I.UnitConvs.count(E)) {
    unsupported(E, "a unit conversion (P04.24)");
    return errorValue(ctx());
  }
  switch (E->getKind()) {
  case ast::Node::Kind::ParenExpr:
    return lowerExpr(cast<ast::ParenExpr>(E)->X);
  case ast::Node::Kind::Ident:
    return lowerIdent(cast<ast::Ident>(E));
  case ast::Node::Kind::BinaryExpr:
    return lowerBinary(cast<ast::BinaryExpr>(E));
  case ast::Node::Kind::UnaryExpr:
    return lowerUnary(cast<ast::UnaryExpr>(E));
  case ast::Node::Kind::CallExpr:
    return lowerCall(cast<ast::CallExpr>(E));
  case ast::Node::Kind::SelectorExpr:
  case ast::Node::Kind::IndexExpr:
  case ast::Node::Kind::StarExpr: {
    Place P = lowerPlace(E);
    if (!P.Ty)
      return errorValue(ctx());
    // What is read through a View is copied out or is a view again.
    S::Type *ES = sTyOf(E);
    if (ES && isa<S::RefType>(S::under(ES)) && P.Ty != tyOf(E))
      return viewOfPlace(std::move(P), ES, E);
    return copyOf(P);
  }
  case ast::Node::Kind::CompositeLit:
    return lowerCompositeLit(cast<ast::CompositeLit>(E));
  case ast::Node::Kind::SliceExpr:
    return lowerSliceExpr(cast<ast::SliceExpr>(E));
  case ast::Node::Kind::FuncLit:
    unsupported(E, "a function literal (P04.07)");
    break;
  case ast::Node::Kind::TypeAssertExpr:
    return lowerTypeAssert(cast<ast::TypeAssertExpr>(E), false);
  default:
    unsupported(E, std::string("the expression ") + E->getKindName());
    break;
  }
  return errorValue(ctx());
}

static bool binOpFor(tok::Kind T, BinOp &Op) {
  switch (T) {
  case tok::add:
    Op = BinOp::Add;
    return true;
  case tok::sub:
    Op = BinOp::Sub;
    return true;
  case tok::mul:
    Op = BinOp::Mul;
    return true;
  case tok::quo:
    Op = BinOp::Div;
    return true;
  case tok::rem:
    Op = BinOp::Rem;
    return true;
  case tok::and_:
    Op = BinOp::And;
    return true;
  case tok::or_:
    Op = BinOp::Or;
    return true;
  case tok::xor_:
    Op = BinOp::Xor;
    return true;
  case tok::and_not:
    Op = BinOp::AndNot;
    return true;
  case tok::shl:
    Op = BinOp::Shl;
    return true;
  case tok::shr:
    Op = BinOp::Shr;
    return true;
  case tok::eql:
    Op = BinOp::Eq;
    return true;
  case tok::neq:
    Op = BinOp::Ne;
    return true;
  case tok::lss:
    Op = BinOp::Lt;
    return true;
  case tok::leq:
    Op = BinOp::Le;
    return true;
  case tok::gtr:
    Op = BinOp::Gt;
    return true;
  case tok::geq:
    Op = BinOp::Ge;
    return true;
  default:
    return false;
  }
}

Operand Body::lowerLogical(const ast::BinaryExpr *B) {
  // t = X; if t (&&) / if !t (||): t = Y. The right side runs only when needed.
  Type *Bool = ctx().boolean();
  bool IsAnd = B->Op == tok::land;
  Operand X = lowerAs(B->X, Bool);
  LocalId T = FB.newTemp(Bool);
  at(B);
  FB.assign(FB.local(T), FB.use(X));
  BlockId Rhs = FB.newBlock(), Join = FB.newBlock();
  FB.branch(copyOf(FB.local(T)), IsAnd ? Rhs : Join, IsAnd ? Join : Rhs);
  FB.setBlock(Rhs);
  Operand Y = lowerAs(B->Y, Bool);
  FB.assign(FB.local(T), FB.use(Y));
  FB.gotoBlock(Join);
  FB.setBlock(Join);
  return copyOf(FB.local(T));
}

//===----------------------------------------------------------------------===//
// Operators and the checks of D4.12
//===----------------------------------------------------------------------===//

bool Body::arithChecks(BinOp Op, const Operand &Y, Type *XT, bool YConst,
                       const ast::Node *N) {
  if (!XT->isInt() && !XT->isFloat() && !(XT->isBool() && isComparison(Op)) &&
      !(XT->isPtr() && (Op == BinOp::Eq || Op == BinOp::Ne))) {
    unsupported(N, std::string("an operator on ") + XT->str());
    return false;
  }
  at(N);
  // Go's checks (D4.12): a zero divisor and a negative shift count panic.
  if ((Op == BinOp::Div || Op == BinOp::Rem) && XT->isInt() && !YConst) {
    Place NZ = FB.temp(FB.binary(BinOp::Ne, Y, constInt(XT, 0)));
    check(copyOf(NZ), AssertKind::DivByZero, false, N);
  }
  if (isShift(Op) && Y.type()->isSigned() && !YConst) {
    Place NN = FB.temp(FB.binary(BinOp::Ge, Y, constInt(Y.type(), 0)));
    check(copyOf(NN), AssertKind::NegativeShift, false, N);
  }
  return true;
}

Operand Body::arith(BinOp Op, Operand X, Operand Y, S::Type *T, bool YConst,
                    const ast::Node *N) {
  if (L.isComplexType(T))
    return complexArith(Op, std::move(X), std::move(Y), T, N);
  if (X.type()->getKind() == Type::Kind::String)
    return stringArith(Op, std::move(X), std::move(Y), N);
  if (!arithChecks(Op, Y, X.type(), YConst, N))
    return errorValue(ctx());
  return copyOf(FB.temp(FB.binary(Op, std::move(X), std::move(Y))));
}

Operand Body::complexArith(BinOp Op, Operand X, Operand Y, S::Type *T,
                           const ast::Node *N) {
  Type *CT = X.type();
  if (CT->getKind() != Type::Kind::Tuple || CT->numFields() != 2) {
    unsupported(N, "an operator on this type");
    return errorValue(ctx());
  }
  Type *F = CT->elems()[0];
  at(N);
  Place PX = placeOf(std::move(X)), PY = placeOf(std::move(Y));
  auto Part = [&](const Place &P, unsigned K) {
    return copyOf(FB.field(P, K));
  };
  auto Bin = [&](BinOp O, Operand A, Operand B) {
    return copyOf(FB.temp(FB.binary(O, std::move(A), std::move(B))));
  };
  Operand Re, Im;
  switch (Op) {
  case BinOp::Add:
  case BinOp::Sub:
    Re = Bin(Op, Part(PX, 0), Part(PY, 0));
    Im = Bin(Op, Part(PX, 1), Part(PY, 1));
    break;
  case BinOp::Mul: {
    // (a+bi)(c+di) = (ac-bd) + (ad+bc)i. Go computes the parts in float64
    // and rounds each once, also for complex64 (cmd/compile ssagen; float32
    // products would overflow where Go's do not).
    Type *Wide = ctx().floatTy(64);
    auto W = [&](Operand O) {
      return F == Wide
                 ? O
                 : copyOf(FB.temp(FB.cast(CastKind::FloatToFloat, O, Wide)));
    };
    Operand A = W(Part(PX, 0)), Bv = W(Part(PX, 1)), Cv = W(Part(PY, 0)),
            Dv = W(Part(PY, 1));
    Operand Ac = Bin(BinOp::Mul, A, Cv);
    Operand Bd = Bin(BinOp::Mul, Bv, Dv);
    Operand Ad = Bin(BinOp::Mul, A, Dv);
    Operand Bc = Bin(BinOp::Mul, Bv, Cv);
    Re = Bin(BinOp::Sub, Ac, Bd);
    Im = Bin(BinOp::Add, Ad, Bc);
    if (F != Wide) {
      Re = copyOf(FB.temp(FB.cast(CastKind::FloatToFloat, Re, F)));
      Im = copyOf(FB.temp(FB.cast(CastKind::FloatToFloat, Im, F)));
    }
    break;
  }
  case BinOp::Div: {
    // Go's runtime division (complex128div); complex64 divides as complex128.
    FuncId Div = L.ensureComplexDiv(N->getBegin());
    Type *C128 = ctx().tuple({ctx().floatTy(64), ctx().floatTy(64)});
    Place A = PX, B = PY;
    if (F->bits() == 32) {
      auto Wide = [&](const Place &P) {
        Operand R = copyOf(FB.temp(
            FB.cast(CastKind::FloatToFloat, Part(P, 0), ctx().floatTy(64))));
        Operand I_ = copyOf(FB.temp(
            FB.cast(CastKind::FloatToFloat, Part(P, 1), ctx().floatTy(64))));
        return FB.temp(FB.aggregate(AggKind::Tuple, C128, {R, I_}));
      };
      A = Wide(PX);
      B = Wide(PY);
    }
    Place Q = FB.local(FB.newTemp(C128));
    FB.call(constFunc(M, Div), {copyOf(A), copyOf(B)}, Q,
            {UnwindAction::Continue, NoId});
    Re = copyOf(FB.field(Q, 0));
    Im = copyOf(FB.field(Q, 1));
    if (F->bits() == 32) {
      Re = copyOf(FB.temp(FB.cast(CastKind::FloatToFloat, Re, F)));
      Im = copyOf(FB.temp(FB.cast(CastKind::FloatToFloat, Im, F)));
    }
    break;
  }
  case BinOp::Eq:
  case BinOp::Ne:
    return equality(Op == BinOp::Eq, copyOf(PX), copyOf(PY), T, N);
  default:
    unsupported(N, "this operator on complex numbers");
    return errorValue(ctx());
  }
  return copyOf(FB.temp(FB.aggregate(AggKind::Tuple, CT, {Re, Im})));
}

Operand Body::equality(bool Eq, Operand X, Operand Y, S::Type *T,
                       const ast::Node *N) {
  Type *G = X.type();
  at(N);
  if (G->isBool() || G->isInt() || G->isFloat() || G->isPtr())
    return copyOf(FB.temp(
        FB.binary(Eq ? BinOp::Eq : BinOp::Ne, std::move(X), std::move(Y))));
  if (G->getKind() == Type::Kind::String)
    return stringEqual(Eq, std::move(X), std::move(Y), N);
  FuncId Glue = L.eqGlue(T, N->getBegin());
  if (Glue == NoId)
    return failed(N, constBool(ctx(), false));
  // The glue takes addresses. A temporary (the result of a call, `f() ==
  // err`, or a part of one) is copied first: the address of a call's result
  // is never taken (V066).
  auto Addr = [&](Operand V) {
    Place P = placeOf(std::move(V));
    bool ThroughPtr = llvm::any_of(
        P.Proj, [](const PlaceElem &E) { return E.K == PlaceElem::Deref; });
    if (!ThroughPtr && FB.function().Locals[P.Local].K == Local::Temp) {
      LocalId C = FB.newTemp(P.Ty);
      FB.assign(FB.local(C), FB.use(copyOf(std::move(P))));
      P = FB.local(C);
    }
    return FB.temp(FB.addrOf(std::move(P)));
  };
  Place A = Addr(std::move(X)), B = Addr(std::move(Y));
  Place R = FB.local(FB.newTemp(ctx().boolean()));
  FB.call(constFunc(M, Glue), {copyOf(A), copyOf(B)}, R,
          {UnwindAction::Continue, NoId});
  if (Eq)
    return copyOf(R);
  return copyOf(FB.temp(FB.unary(UnOp::Not, copyOf(R))));
}

Operand Body::lowerBinary(const ast::BinaryExpr *B) {
  if (I.Operators.count(B)) {
    unsupported(B, "an operator method (P04.23)");
    return errorValue(ctx());
  }
  if (B->Op == tok::land || B->Op == tok::lor)
    return lowerLogical(B);
  BinOp Op;
  if (!binOpFor(B->Op, Op)) {
    unsupported(B, "this operator");
    return errorValue(ctx());
  }
  if (isComparison(Op)) {
    // The operands have the type of the one that is not a constant (or nil).
    const ast::Expr *Other = nullptr;
    if (isNilValue(B->Y))
      Other = B->X;
    else if (isNilValue(B->X))
      Other = B->Y;
    if (Other) {
      // `o == nil` on an Option: the flag is clear (or the pointer is null).
      Operand O = lowerExpr(Other);
      S::Type *OU = S::under(sTyOf(Other));
      if (O.type() && O.type()->isIface() && !asOption(sTyOf(Other))) {
        // An `error` or `any` is nil when it has no itab.
        at(B);
        Operand Itab = copyOf(FB.field(placeOf(std::move(O)), 0));
        return copyOf(
            FB.temp(FB.binary(Op, std::move(Itab), zeroOf(Itab.type()))));
      }
      if (isa_and_nonnull<S::SliceType>(OU) ||
          isa_and_nonnull<S::MapType>(OU)) {
        // A nil slice has no data pointer, a nil map no object.
        at(B);
        Operand Ptr =
            O.type()->isPtr() ? O : copyOf(FB.field(placeOf(std::move(O)), 0));
        return copyOf(
            FB.temp(FB.binary(Op, std::move(Ptr), zeroOf(Ptr.type()))));
      }
      if (!asOption(sTyOf(Other))) {
        unsupported(B, "a comparison with nil");
        return errorValue(ctx());
      }
      Operand Set = optionIsSet(std::move(O), B);
      if (Op == BinOp::Ne)
        return Set;
      return copyOf(FB.temp(FB.unary(UnOp::Not, std::move(Set))));
    }
    S::Type *OpS = isConst(B->X) ? sTyOf(B->Y) : sTyOf(B->X);
    // An Option compared with a plain value compares as Options.
    if (S::OptionType *O = asOption(sTyOf(B->X)); O && !asOption(sTyOf(B->Y)))
      OpS = sTyOf(B->X);
    else if (S::OptionType *O2 = asOption(sTyOf(B->Y));
             O2 && !asOption(sTyOf(B->X)))
      OpS = sTyOf(B->Y);
    // An interface against a value of another type: both as the interface
    // (the one with fewer methods when both are interfaces).
    {
      S::Type *XT = sTyOf(B->X), *YT = sTyOf(B->Y);
      bool XI = XT && L.isInterface(XT), YI = YT && L.isInterface(YT);
      if (XI || YI) {
        if (XI && YI) {
          const Lowering::IfaceMethods *XM = L.ifaceMethods(XT, B->getBegin());
          const Lowering::IfaceMethods *YM = L.ifaceMethods(YT, B->getBegin());
          if (!XM || !YM)
            return errorValue(ctx());
          OpS = YM->List.size() < XM->List.size() ? YT : XT;
        } else {
          OpS = XI ? XT : YT;
        }
      }
    }
    // A View[string] is compared as the string it shows (a borrow).
    if (auto *Rf = dyn_cast_or_null<S::RefType>(OpS ? S::under(OpS) : nullptr);
        Rf && isStringType(Rf->Elem))
      OpS = Rf->Elem;
    Operand X = valueFor(B->X, OpS), Y = valueFor(B->Y, OpS);
    if (Op == BinOp::Eq || Op == BinOp::Ne)
      return equality(Op == BinOp::Eq, std::move(X), std::move(Y), OpS, B);
    return arith(Op, std::move(X), std::move(Y), OpS, false, B);
  }
  S::Type *ResS = sTyOf(B);
  Type *ResTy = tyOf(B);
  if (!ResTy)
    return errorValue(ctx());
  if (Op == BinOp::Add && ResTy->getKind() == Type::Kind::String)
    return lowerStringAdd(B);
  Operand X, Y;
  if (isShift(Op)) {
    X = lowerAs(B->X, ResTy);
    Y = lowerExpr(B->Y);
  } else {
    X = lowerAs(B->X, ResTy);
    Y = lowerAs(B->Y, ResTy);
  }
  return arith(Op, std::move(X), std::move(Y), ResS, isConst(B->Y), B);
}

Operand Body::lowerUnary(const ast::UnaryExpr *U) {
  if (I.Operators.count(U)) {
    unsupported(U, "an operator method (P04.23)");
    return errorValue(ctx());
  }
  if (U->Op == tok::and_) {
    // &x of a variable, a field or an element: a raw pointer to it. A
    // composite literal would be allocated (P05).
    if (isa<ast::CompositeLit>(unparen(U->X))) {
      unsupported(U, "taking the address of a composite literal, which "
                     "allocates (P05)");
      return errorValue(ctx());
    }
    Place P = lowerPlace(U->X);
    if (!P.Ty)
      return errorValue(ctx());
    at(U);
    return copyOf(FB.temp(FB.addrOf(std::move(P), true)));
  }
  UnOp Op;
  switch (U->Op) {
  case tok::add:
    return lowerExpr(U->X);
  case tok::sub:
    Op = UnOp::Neg;
    break;
  case tok::not_:
    Op = UnOp::Not;
    break;
  case tok::xor_:
    Op = UnOp::Compl;
    break;
  default:
    unsupported(U, "this unary operator");
    return errorValue(ctx());
  }
  Operand X = lowerAs(U->X, tyOf(U));
  Type *T = X.type();
  if (Op == UnOp::Neg && L.isComplexType(sTyOf(U))) {
    at(U);
    Place P = placeOf(std::move(X));
    Operand Re = copyOf(FB.temp(FB.unary(UnOp::Neg, copyOf(FB.field(P, 0)))));
    Operand Im = copyOf(FB.temp(FB.unary(UnOp::Neg, copyOf(FB.field(P, 1)))));
    return copyOf(FB.temp(FB.aggregate(AggKind::Tuple, T, {Re, Im})));
  }
  if (!(T->isInt() || (T->isFloat() && Op == UnOp::Neg) ||
        (T->isBool() && Op == UnOp::Not))) {
    unsupported(U, std::string("a unary operator on ") + T->str());
    return errorValue(ctx());
  }
  at(U);
  return copyOf(FB.temp(FB.unary(Op, X)));
}

//===----------------------------------------------------------------------===//
// Conversions
//===----------------------------------------------------------------------===//

Operand Body::convert(const ast::CallExpr *C, Operand X, S::Type *FromS,
                      S::Type *ToS) {
  Type *To = L.lowerType(ToS, C->getBegin());
  if (!To)
    return errorValue(ctx());
  Type *From = X.type();
  at(C);
  if (From->isFloat() && To->isFloat()) {
    // An explicit float conversion rounds, and so stops a fused multiply-add
    // (Go spec, "Floating-point operators"); the code generator keeps it
    // even when the types are equal.
    return copyOf(FB.temp(FB.cast(CastKind::FloatToFloat, std::move(X), To)));
  }
  if (From == To)
    return X;
  CastKind K;
  if (From->isInt() && To->isInt())
    K = CastKind::IntToInt;
  else if (From->isInt() && To->isFloat())
    K = CastKind::IntToFloat;
  else if (From->isFloat() && To->isInt())
    K = CastKind::FloatToInt;
  else if (From->isPtr() && To->isPtr())
    K = CastKind::PtrToPtr;
  else if (From->isPtr() && To->isInt())
    K = CastKind::PtrToInt;
  else if (From->isInt() && To->isPtr())
    K = CastKind::IntToPtr;
  else if (L.isComplexType(ToS) && L.isComplexType(FromS) &&
           From->getKind() == Type::Kind::Tuple) {
    // complex64 <-> complex128: each part.
    Place P = placeOf(std::move(X));
    Type *F = To->elems()[0];
    Operand Re = copyOf(
        FB.temp(FB.cast(CastKind::FloatToFloat, copyOf(FB.field(P, 0)), F)));
    Operand Im = copyOf(
        FB.temp(FB.cast(CastKind::FloatToFloat, copyOf(FB.field(P, 1)), F)));
    return copyOf(FB.temp(FB.aggregate(AggKind::Tuple, To, {Re, Im})));
  } else if (From->getKind() == Type::Kind::String ||
             To->getKind() == Type::Kind::String ||
             From->getKind() == Type::Kind::Slice ||
             To->getKind() == Type::Kind::Slice ||
             (From->getKind() == Type::Kind::Tuple &&
              !L.isComplexType(FromS))) {
    return convertStrings(C, std::move(X), FromS, ToS);
  } else {
    unsupported(C, "a conversion from " + From->str() + " to " + To->str());
    return X;
  }
  return copyOf(FB.temp(FB.cast(K, std::move(X), To)));
}

Operand Body::lowerConversion(const ast::CallExpr *C) {
  if (C->Args.size() != 1) {
    unsupported(C, "this conversion");
    return errorValue(ctx());
  }
  S::Type *ToS = sTyOf(C);
  if (asOption(ToS) || isNilValue(C->Args[0]) || (ToS && L.isInterface(ToS)))
    return valueFor(C->Args[0], ToS);
  // View[T](x): the view of x, as for an implicit one (ADR-0020).
  if (ToS && isa<S::RefType>(S::under(ToS)))
    return makeView(C->Args[0], ToS);
  Type *To = tyOf(C);
  if (!To)
    return errorValue(ctx());
  S::Type *FromS = sTyOf(C->Args[0]);
  // A constant argument is made in the target type, unless no constant of
  // that type exists (a complex number is a pair; a pointer such as
  // unsafe.Pointer(uintptr(8)) is converted from the integer).
  // (A constant string converted to []byte or []rune stays a string.)
  Type *ConstTy = To->getKind() == Type::Kind::Slice
                      ? L.lowerType(FromS, C->getBegin())
                      : To;
  Operand X =
      isConst(C->Args[0]) && !L.isComplexType(ToS) && !To->isPtr() && ConstTy
          ? constOperand(*I.lookup(C->Args[0])->Val, ConstTy, C)
          : lowerExpr(C->Args[0]);
  return convert(C, std::move(X), FromS, ToS);
}

//===----------------------------------------------------------------------===//
// println / print
//===----------------------------------------------------------------------===//

void Body::callWrite(Operand Ptr, Operand Len, SourceLoc At) {
  FuncId W = L.ensureWrite(At);
  if (W == NoId)
    return;
  Place Dest = FB.local(FB.newTemp(ctx().i64()));
  Operand Fd = constInt(ctx().i32(), 1);
  FB.call(constFunc(M, W), {Fd, std::move(Ptr), std::move(Len)}, Dest,
          {UnwindAction::Unreachable, NoId});
}

void Body::writeString(StringRef Text, SourceLoc At) {
  Place S = FB.temp(FB.use(constStr(M, Text)));
  callWrite(copyOf(FB.field(S, 0)), copyOf(FB.field(S, 1)), At);
}

/// The text Go prints for a constant operand of type T (an integer in
/// decimal, a bool, a string's bytes). False after a report.
bool Body::constText(const ast::Expr *Arg, Type *T, std::string &Out) {
  const S::ConstValue &V = *I.lookup(Arg)->Val;
  if (T->getKind() == Type::Kind::String && V.kind() == S::ConstValue::String) {
    Out = V.stringVal();
    return true;
  }
  if (T->isBool() && V.kind() == S::ConstValue::Bool) {
    Out = V.boolVal() ? "true" : "false";
    return true;
  }
  S::ConstValue IV = V.toInt();
  if (T->isInt() && !IV.isUnknown()) {
    // The value has the type's width (a typed constant is in range).
    Out = IV.exactStr();
    return true;
  }
  unsupported(Arg, "printing this constant");
  return false;
}

/// Prints one value that is not folded (bool, integers, floats, complex
/// numbers, pointers, strings).
void Body::printValue(Operand V, S::Type *ST, SourceLoc At) {
  Type *T = V.type();
  Type *I32 = ctx().i32();
  Operand Fd = constInt(I32, 1);
  auto Call = [&](FuncId H, Operand Arg) {
    if (H == NoId)
      return;
    FB.call(constFunc(M, H), {Fd, std::move(Arg)},
            FB.local(FB.newTemp(ctx().unit())), {UnwindAction::Continue, NoId});
  };
  auto PrintFloat = [&](Operand F) {
    if (F.type()->bits() != 64)
      F = copyOf(FB.temp(
          FB.cast(CastKind::FloatToFloat, std::move(F), ctx().floatTy(64))));
    Call(L.ensurePrintF64(At), std::move(F));
  };
  if (T->isIface()) {
    // Go's form: (itab,data).
    Place P = placeOf(std::move(V));
    Type *U64 = ctx().u64();
    writeString("(", At);
    Call(L.ensurePrintHex(At),
         copyOf(FB.temp(
             FB.cast(CastKind::PtrToInt, copyOf(FB.field(P, 0)), U64))));
    writeString(",", At);
    Call(L.ensurePrintHex(At),
         copyOf(FB.temp(
             FB.cast(CastKind::PtrToInt, copyOf(FB.field(P, 1)), U64))));
    writeString(")", At);
    return;
  }
  switch (T->getKind()) {
  case Type::Kind::String:
    callWrite(copyOf(FB.field(V.P, 0)), copyOf(FB.field(V.P, 1)), At);
    return;
  case Type::Kind::Bool: {
    BlockId Yes = FB.newBlock(), No = FB.newBlock(), Join = FB.newBlock();
    FB.branch(V, Yes, No);
    FB.setBlock(Yes);
    writeString("true", At);
    FB.gotoBlock(Join);
    FB.setBlock(No);
    writeString("false", At);
    FB.gotoBlock(Join);
    FB.setBlock(Join);
    return;
  }
  case Type::Kind::Int: {
    bool Signed = T->isSigned();
    Type *Wide = Signed ? ctx().i64() : ctx().u64();
    if (T != Wide)
      V = copyOf(FB.temp(FB.cast(CastKind::IntToInt, V, Wide)));
    Call(Signed ? L.ensurePrintI64(At) : L.ensurePrintU64(At), std::move(V));
    return;
  }
  case Type::Kind::Float:
    PrintFloat(std::move(V));
    return;
  case Type::Kind::Ptr: {
    Operand Bits =
        copyOf(FB.temp(FB.cast(CastKind::PtrToInt, std::move(V), ctx().u64())));
    Call(L.ensurePrintHex(At), std::move(Bits));
    return;
  }
  case Type::Kind::Tuple:
    if (ST && isa<S::RefType>(S::under(ST)) &&
        T->elems()[0] == ctx().ptr(ctx().u8())) {
      // A View[string]: its bytes.
      Place P = placeOf(std::move(V));
      callWrite(copyOf(FB.field(P, 0)), copyOf(FB.field(P, 1)), At);
      return;
    }
    if (L.isComplexType(ST)) {
      // (+1.000000e+000+2.000000e+000i)
      Place P = placeOf(std::move(V));
      writeString("(", At);
      PrintFloat(copyOf(FB.field(P, 0)));
      PrintFloat(copyOf(FB.field(P, 1)));
      writeString("i)", At);
      return;
    }
    break;
  default:
    break;
  }
  L.unsupported(At, "printing a value of type " + T->str());
}

/// println and print (D4.11 e: the bootstrap writes to standard output; Go's
/// writes to standard error). Go evaluates every operand before the call, so
/// the first phase evaluates, left to right, the operands that are not
/// constants, each into its own temporary; the second phase writes. A
/// constant integer, bool or string is formatted here, and the text from one
/// value to the next (println's spaces and newline included) is one `write`.
void Body::lowerPrintln(const ast::CallExpr *C, bool Newline) {
  SourceLoc At = C->getBegin();
  struct Item {
    bool IsValue = false;
    std::string Text; // a constant
    Operand Val;      // a value
    S::Type *STy = nullptr;
  };
  SmallVector<Item, 4> Items;
  // println(f()) with a call of several results prints each of them.
  if (C->Args.size() == 1)
    if (auto *TT = dyn_cast_or_null<S::TupleType>(sTyOf(C->Args[0]))) {
      Operand T = lowerExpr(C->Args[0]);
      if (T.type()->getKind() != Type::Kind::Tuple)
        return;
      Place TP = placeOf(std::move(T));
      at(C);
      for (size_t K = 0; K < TT->Elems.size(); ++K) {
        Item It;
        It.IsValue = true;
        It.STy = TT->Elems[K];
        It.Val = copyOf(FB.temp(FB.use(copyOf(FB.field(TP, K)))));
        Items.push_back(std::move(It));
      }
    }
  for (const ast::Expr *Arg :
       Items.empty() ? C->Args : ArrayRef<ast::Expr *>()) {
    Type *T = tyOf(Arg);
    if (!T)
      return;
    Item It;
    It.STy = sTyOf(Arg);
    bool Printable = T->isInt() || T->isBool() || T->isFloat() || T->isPtr() ||
                     T->isIface() || T->getKind() == Type::Kind::String ||
                     (L.isComplexType(It.STy)) ||
                     (It.STy && isa<S::RefType>(S::under(It.STy)) &&
                      T->getKind() == Type::Kind::Tuple);
    if (!Printable || asOption(It.STy)) {
      unsupported(Arg, "printing a value of type " + T->str());
      return;
    }
    if (isConst(Arg) &&
        (T->isInt() || T->isBool() || T->getKind() == Type::Kind::String)) {
      if (!constText(Arg, T, It.Text))
        return;
    } else {
      It.IsValue = true;
      It.Val = lowerExpr(Arg);
      // A variable is read now, not when it is printed.
      if (It.Val.K != Operand::Const &&
          (FB.function().Locals[It.Val.P.Local].K != Local::Temp ||
           !It.Val.P.Proj.empty())) {
        at(C);
        It.Val = copyOf(FB.temp(FB.use(It.Val)));
      }
    }
    Items.push_back(std::move(It));
  }
  at(C);
  std::string Text;
  for (size_t K = 0; K < Items.size(); ++K) {
    if (K && Newline)
      Text += ' ';
    if (!Items[K].IsValue) {
      Text += Items[K].Text;
      continue;
    }
    if (!Text.empty())
      writeString(Text, At);
    Text.clear();
    Operand V = Items[K].Val;
    if (V.K == Operand::Const) // a value the lowering folded
      V = copyOf(FB.temp(FB.use(V)));
    printValue(V, Items[K].STy, At);
  }
  if (Newline)
    Text += '\n';
  if (!Text.empty())
    writeString(Text, At);
}

//===----------------------------------------------------------------------===//
// Calls
//===----------------------------------------------------------------------===//

Operand Body::lowerOptionMethod(const ast::CallExpr *C,
                                const ast::SelectorExpr *Sel,
                                const S::Func *F) {
  S::Type *OT = sTyOf(Sel->X);
  if (!asOption(OT)) {
    unsupported(C, "this method call");
    return errorValue(ctx());
  }
  Operand O = lowerExpr(Sel->X);
  Operand Set = optionIsSet(O, C);
  if (F->Name == "IsNil") {
    at(C);
    return copyOf(FB.temp(FB.unary(UnOp::Not, std::move(Set))));
  }
  // Get: (the value, whether it is set). An unset Option gives the zero
  // value, as its payload bits are zero.
  Type *Res = tyOf(C);
  if (!Res)
    return errorValue(ctx());
  Operand V = optionPayload(O);
  at(C);
  return copyOf(FB.temp(FB.aggregate(AggKind::Tuple, Res, {V, Set})));
}

Operand Body::lowerBuiltin(const ast::CallExpr *C, const S::Builtin *B) {
  using BI = S::BuiltinId;
  switch (B->Id) {
  case BI::Println:
  case BI::Print:
    lowerPrintln(C, B->Id == BI::Println);
    return unitValue();
  case BI::Len:
  case BI::Cap: {
    // The checker folds len/cap of an array when the operand has no calls;
    // otherwise the operand is evaluated for its effects and the length is
    // the type's.
    S::Type *T = C->Args.empty() ? nullptr : sTyOf(C->Args[0]);
    S::Type *U = T ? S::under(T) : nullptr;
    if (auto *P = dyn_cast_or_null<S::PointerType>(U))
      U = S::under(P->Elem);
    if (auto *AT = dyn_cast_or_null<S::ArrayType>(U)) {
      lowerExpr(C->Args[0]);
      return constInt(tyOf(C), uint64_t(AT->Len));
    }
    if (C->Args.empty()) {
      unsupported(C, "this built-in");
      return errorValue(ctx());
    }
    return lowerLenCap(C, B->Id == BI::Cap);
  }
  case BI::Real:
  case BI::Imag: {
    Operand X =
        lowerAs(C->Args[0], L.lowerType(sTyOf(C->Args[0]), C->getBegin()));
    if (X.type()->getKind() != Type::Kind::Tuple) {
      unsupported(C, "this built-in");
      return errorValue(ctx());
    }
    at(C);
    return copyOf(FB.field(placeOf(std::move(X)), B->Id == BI::Real ? 0 : 1));
  }
  case BI::Complex: {
    Type *CT = tyOf(C);
    if (!CT || CT->getKind() != Type::Kind::Tuple) {
      unsupported(C, "this built-in");
      return errorValue(ctx());
    }
    Type *F = CT->elems()[0];
    Operand Re = lowerAs(C->Args[0], F), Im = lowerAs(C->Args[1], F);
    at(C);
    return copyOf(FB.temp(FB.aggregate(AggKind::Tuple, CT, {Re, Im})));
  }
  case BI::Min:
  case BI::Max: {
    Type *T = tyOf(C);
    if (T && T->getKind() == Type::Kind::String)
      return lowerMinMaxString(C, B->Id == BI::Min);
    if (!T || !T->isInt()) {
      unsupported(C, std::string("the built-in ") + B->Name.str() +
                         " of this type");
      return errorValue(ctx());
    }
    LocalId R = FB.newTemp(T);
    Operand First = lowerAs(C->Args[0], T);
    at(C);
    FB.assign(FB.local(R), FB.use(First));
    for (size_t K = 1; K < C->Args.size(); ++K) {
      Operand V = lowerAs(C->Args[K], T);
      at(C);
      Place Better = FB.temp(FB.binary(B->Id == BI::Min ? BinOp::Lt : BinOp::Gt,
                                       V, copyOf(FB.local(R))));
      BlockId Take = FB.newBlock(), Join = FB.newBlock();
      FB.branch(copyOf(Better), Take, Join);
      FB.setBlock(Take);
      FB.assign(FB.local(R), FB.use(V));
      FB.gotoBlock(Join);
      FB.setBlock(Join);
    }
    return copyOf(FB.local(R));
  }
  case BI::New:
  case BI::NewUnique:
  case BI::NewShared:
  case BI::NewWeak:
    unsupported(C, "heap allocation (P05, P04.09)");
    break;
  case BI::Make:
    return lowerMake(C);
  case BI::Append:
    return lowerAppend(C);
  case BI::Copy:
    return lowerCopy(C);
  case BI::Delete:
    lowerDelete(C);
    return unitValue();
  case BI::Clear:
    lowerClear(C);
    return unitValue();
  case BI::Panic:
  case BI::Recover:
    unsupported(C, std::string("the built-in ") + B->Name.str() + " (P04.07)");
    break;
  case BI::Move:
  case BI::Clone:
    unsupported(C, std::string("the built-in ") + B->Name.str() + " (P04.09)");
    break;
  case BI::Close:
    unsupported(C, "the built-in close (P12)");
    break;
  default:
    unsupported(C, std::string("the built-in ") + B->Name.str());
    break;
  }
  return errorValue(ctx());
}

Operand Body::callWith(const ast::CallExpr *C, S::SignatureType *Sig,
                       Type *GSig, Operand CalleeOp, ArrayRef<Operand> Lead,
                       bool NoUnwind) {
  Operand Unit = unitValue();
  ArrayRef<Type *> Ps = GSig->elems().drop_front(Lead.size());
  SmallVector<Operand, 4> Args(Lead.begin(), Lead.end());
  const size_t Base = Lead.size();
  // A borrowed map's address is taken after the other arguments, as a
  // move out of a place is (EXP-3): it may point into a container one of
  // them grows. Its operands are evaluated in their turn.
  SmallVector<std::pair<size_t, LateOps>, 1> Late;
  auto AddArg = [&](size_t K, const ast::Expr *A) {
    if (L.mapByPointer(Sig, K) && !isNilValue(A)) {
      Late.push_back({K, LateOps()});
      recordOps(A, Late.back().second);
      Args.push_back(Operand());
      return;
    }
    Args.push_back(argFor(A, Sig, K));
  };
  // f(g()) where g returns several values (the one argument is a tuple; a
  // variadic f(a int, rest ...int) called as f(1) is not this form).
  auto *TT = C->Args.size() == 1
                 ? dyn_cast_or_null<S::TupleType>(sTyOf(C->Args[0]))
                 : nullptr;
  if (TT && Ps.size() > 1) {
    // The results of a call spread over a variadic parameter, `vf(g())`,
    // are packed into the slice by P04.18 (TODO P04.18).
    if (Sig->Variadic) {
      unsupported(C, "spreading the results of a call over a variadic "
                     "parameter (P04.18)");
      return Unit;
    }
    Operand T = lowerExpr(C->Args[0]);
    if (T.type()->getKind() != Type::Kind::Tuple ||
        T.type()->numFields() != Ps.size() || TT->Elems.size() != Ps.size()) {
      unsupported(C, "this call form");
      return Unit;
    }
    Place TP = placeOf(std::move(T));
    for (size_t K = 0; K < Ps.size(); ++K)
      Args.push_back(argAs(convertResult(copyOf(FB.field(TP, K)), TT->Elems[K],
                                         Sig->Params->Elems[K], C),
                           Sig, K, C));
  } else if (Sig->Variadic) {
    // f(a, b, xs...) passes the slice; f(a, b, x, y) packs x and y.
    size_t Fixed = Ps.size() - 1;
    if (C->Args.size() < Fixed) {
      unsupported(C, "this call form");
      return Unit;
    }
    for (size_t K = 0; K < Fixed; ++K)
      AddArg(K, C->Args[K]);
    if (C->EllipsisLoc.isValid()) {
      if (C->Args.size() != Ps.size()) {
        unsupported(C, "this call form");
        return Unit;
      }
      Args.push_back(argFor(C->Args.back(), Sig, Fixed));
    } else {
      Args.push_back(argAs(packVariadic(C, Sig, Fixed), Sig, Fixed, C));
    }
  } else if (C->EllipsisLoc.isValid() || Ps.size() != C->Args.size()) {
    unsupported(C, "this call form");
    return Unit;
  } else {
    for (size_t K = 0; K < C->Args.size(); ++K)
      AddArg(K, C->Args[K]);
  }
  // The map entries among them: one record each in an array of the
  // frame, copied in and aliased by one runtime call, copied out by
  // another after the call (mapEntryArg; linear GIR per call).
  unsigned NE = 0;
  for (auto &[K, R] : Late)
    if (auto *IE = dyn_cast<ast::IndexExpr>(unparen(C->Args[K]));
        IE && isMapIndex(IE))
      ++NE;
  Place Recs;
  Operand RecsAddr;
  if (NE) {
    at(C);
    Recs = FB.local(FB.newLocal(ctx().array(L.mapEntryArgType(), NE),
                                Local::Var, "entries"));
    RecsAddr = copyOf(FB.temp(FB.addrOf(FB.constIndex(Recs, 0), true)));
  }
  SmallVector<std::pair<size_t, unsigned>, 1> EntryArgs;
  for (auto &[K, R] : Late) {
    ++LowerSteps;
    auto *IE = dyn_cast<ast::IndexExpr>(unparen(C->Args[K]));
    if (IE && isMapIndex(IE)) {
      ReplayScope Scope(*this, R);
      unsigned Idx = EntryArgs.size();
      mapEntryArg(IE, Recs, Idx);
      EntryArgs.push_back({K, Idx});
      continue;
    }
    Args[Base + K] = argAs(mapArgAddr(C->Args[K], R), Sig, K, C->Args[K]);
  }
  if (NE) {
    // (Test-only: GIR for every pair, the quadratic control.)
    if (L.Opts.PairwiseEntryArgs)
      for (unsigned A = 0; A < NE; ++A)
        for (unsigned B = 0; B < A; ++B)
          FB.temp(FB.binary(BinOp::Eq,
                            copyOf(FB.field(FB.constIndex(Recs, A), 1)),
                            copyOf(FB.field(FB.constIndex(Recs, B), 1))));
    at(C);
    callRt(RtFn::MapEntryArgs, {RecsAddr, constInt(ctx().u64(), NE)}, C);
    for (auto &[K, Idx] : EntryArgs) {
      at(C->Args[K]);
      Args[Base + K] = argAs(copyOf(FB.field(FB.constIndex(Recs, Idx), 5)), Sig,
                             K, C->Args[K]);
    }
  }
  at(C);
  LocalId Res = FB.newTemp(GSig->ret());
  FB.call(
      std::move(CalleeOp), Args, FB.local(Res),
      {NoUnwind ? UnwindAction::Unreachable : UnwindAction::Continue, NoId});
  if (NE) {
    at(C);
    callRt(RtFn::MapEntryBack, {RecsAddr, constInt(ctx().u64(), NE)}, C);
  }
  return copyOf(FB.local(Res));
}

Operand Body::lowerCall(const ast::CallExpr *C) {
  const ast::Expr *Fun = unparen(C->Fun);
  const S::TypeAndValue *FT = I.lookup(Fun);
  Operand Unit = unitValue();
  if (C->isMust()) {
    unsupported(C, "a call with ! (P04.10)");
    return Unit;
  }
  if (FT && FT->M == S::Mode::TypeExpr)
    return lowerConversion(C);
  if (FT && FT->M == S::Mode::Builtin) {
    auto *Id = dyn_cast<ast::Ident>(Fun);
    auto U = Id ? I.Uses.find(Id) : I.Uses.end();
    if (U != I.Uses.end())
      if (auto *B = dyn_cast<S::Builtin>(U->second))
        return lowerBuiltin(C, B);
    // The functions of package unsafe (Sizeof, Alignof and Offsetof are
    // constants the checker folded).
    if (auto *Sel = dyn_cast<ast::SelectorExpr>(Fun)) {
      auto SU = I.Uses.find(Sel->Sel);
      if (SU != I.Uses.end())
        if (auto *B = dyn_cast<S::Builtin>(SU->second)) {
          if (B->Id == S::BuiltinId::Add && C->Args.size() == 2) {
            // Pointer(uintptr(ptr) + uintptr(len)): a negative len wraps.
            Type *U64 = ctx().u64();
            Operand P = lowerExpr(C->Args[0]);
            Operand N = lowerExpr(C->Args[1]);
            if (!P.type()->isPtr() || !N.type()->isInt()) {
              unsupported(C, "this unsafe.Add");
              return errorValue(ctx());
            }
            at(C);
            Operand PI =
                copyOf(FB.temp(FB.cast(CastKind::PtrToInt, std::move(P), U64)));
            if (N.type() != U64)
              N = copyOf(
                  FB.temp(FB.cast(CastKind::IntToInt, std::move(N), U64)));
            Operand Sum = copyOf(FB.temp(FB.binary(BinOp::Add, PI, N)));
            return copyOf(FB.temp(
                FB.cast(CastKind::IntToPtr, Sum, ctx().ptr(ctx().u8()))));
          }
          if (B->Id == S::BuiltinId::Slice || B->Id == S::BuiltinId::String ||
              B->Id == S::BuiltinId::SliceData ||
              B->Id == S::BuiltinId::StringData)
            return lowerUnsafeSeq(C, B->Id);
          unsupported(C, "unsafe." + B->Name.str());
          return errorValue(ctx());
        }
    }
    unsupported(C, "this built-in");
    return Unit;
  }
  // An Option's IsNil and Get.
  if (auto *Sel = dyn_cast<ast::SelectorExpr>(Fun)) {
    auto It = I.Selections.find(Sel);
    if (It != I.Selections.end() && It->second.K == S::Selection::MethodVal) {
      auto *F = dyn_cast_or_null<S::Func>(It->second.Obj);
      if (F && F->IsBuiltinMethod && (F->Name == "IsNil" || F->Name == "Get") &&
          C->Args.empty())
        return lowerOptionMethod(C, Sel, F);
      if (F)
        return lowerMethodCall(C, Sel, F, It->second);
      unsupported(C, "this method call");
      return Unit;
    }
  }
  auto *Id = dyn_cast<ast::Ident>(Fun);
  auto U = Id ? I.Uses.find(Id) : I.Uses.end();
  if (U != I.Uses.end()) {
    if (auto *F = dyn_cast<S::Func>(U->second)) {
      auto TI = L.TestIntrinsics.find(F);
      if (TI != L.TestIntrinsics.end())
        return lowerTestIntrinsic(C, F, TI->second);
      auto FI = L.FuncIds.find(F);
      if (FI == L.FuncIds.end() && L.Declined.count(F))
        return Unit; // its declaration was reported
      if (FI == L.FuncIds.end() || F->Recv || F->IsInterfaceMethod) {
        unsupported(C, "a call of " + F->Name.str());
        return Unit;
      }
      return callWith(C, F->sig(), M.func(FI->second).Sig,
                      constFunc(M, FI->second), {},
                      M.func(FI->second).NoUnwind);
    }
  }
  // A call of a value (a func value, a method value): its type says what is
  // missing, reported once (lowerType).
  if (!tyOf(Fun))
    return failed(Fun, Unit);
  unsupported(C, "this call");
  return Unit;
}

//===----------------------------------------------------------------------===//
// Composite literals
//===----------------------------------------------------------------------===//

Operand Body::lowerStructLit(const ast::CompositeLit *CL, S::StructType *ST,
                             Type *Ty) {
  size_t N = ST->Fields.size();
  SmallVector<Operand, 4> Ops(N);
  SmallVector<bool, 4> Set(N, false);
  bool Keyed = !CL->Elts.empty() && isa<ast::KeyValueExpr>(CL->Elts[0]);
  for (size_t K = 0; K < CL->Elts.size(); ++K) {
    const ast::Expr *E = CL->Elts[K];
    size_t Field = K;
    if (Keyed) {
      auto *KV = dyn_cast<ast::KeyValueExpr>(E);
      auto *Name = KV ? dyn_cast<ast::Ident>(KV->Key) : nullptr;
      if (!Name) {
        unsupported(E, "this struct literal");
        return errorValue(ctx());
      }
      // The field the checker resolved the key to (Info::Uses), found by
      // the struct's name index when it has one (n keys cost n log n).
      auto U = I.Uses.find(Name);
      const S::Object *Fld = U == I.Uses.end() ? nullptr : U->second;
      Field = N;
      if (ST->indexed()) {
        for (uint32_t F : ST->fieldsNamed(Name->Name.str()))
          if (ST->Fields[F] == Fld)
            Field = F;
      } else {
        for (size_t F = 0; F < N; ++F)
          if (ST->Fields[F] == Fld)
            Field = F;
      }
      if (Field == N) {
        unsupported(E, "this struct literal");
        return errorValue(ctx());
      }
      E = KV->Value;
    } else if (K >= N) {
      unsupported(E, "this struct literal");
      return errorValue(ctx());
    }
    Operand V = valueFor(E, ST->Fields[Field]->Ty);
    // Evaluated now, in source order: a later element must not change it.
    if (V.K == Operand::Copy &&
        !(V.P.Proj.empty() &&
          FB.function().Locals[V.P.Local].K == Local::Temp)) {
      at(E);
      V = copyOf(FB.temp(FB.use(V)));
    }
    Ops[Field] = std::move(V);
    Set[Field] = true;
  }
  for (size_t F = 0; F < N; ++F)
    if (!Set[F]) {
      Type *FT = Ty->fieldType(F, ctx());
      Ops[F] = zeroOf(FT);
    }
  at(CL);
  return copyOf(FB.temp(FB.aggregate(AggKind::Struct, Ty, Ops)));
}

Operand Body::lowerArrayLit(const ast::CompositeLit *CL, S::ArrayType *AT,
                            Type *Ty) {
  uint64_t Len = Ty->length();
  // The element at each index, in source order; a key sets the index.
  SmallVector<std::pair<uint64_t, const ast::Expr *>, 8> Elts;
  uint64_t Next = 0;
  bool Dense = true;
  for (const ast::Expr *E : CL->Elts) {
    if (auto *KV = dyn_cast<ast::KeyValueExpr>(E)) {
      const S::TypeAndValue *TV = I.lookup(KV->Key);
      int64_t K = 0;
      if (!TV || !TV->Val || !TV->Val->toInt().fitsInt64(K) || K < 0) {
        unsupported(E, "this array literal");
        return errorValue(ctx());
      }
      Next = K;
      E = KV->Value;
    }
    if (Next >= Len) {
      unsupported(E, "this array literal");
      return errorValue(ctx());
    }
    if (Next != Elts.size())
      Dense = false;
    Elts.push_back({Next++, E});
  }
  if (Dense && Elts.size() == Len && Len <= 32) {
    SmallVector<Operand, 8> Ops;
    for (auto &[Idx, E] : Elts) {
      Operand V = valueFor(E, AT->Elem);
      if (V.K == Operand::Copy &&
          !(V.P.Proj.empty() &&
            FB.function().Locals[V.P.Local].K == Local::Temp)) {
        at(E);
        V = copyOf(FB.temp(FB.use(V)));
      }
      Ops.push_back(std::move(V));
    }
    at(CL);
    return copyOf(FB.temp(FB.aggregate(AggKind::Array, Ty, Ops)));
  }
  // The zero array, then each element into its place.
  at(CL);
  Place Arr = FB.local(FB.newTemp(Ty));
  FB.assign(Arr, FB.use(zeroOf(Ty)));
  for (auto &[Idx, E] : Elts) {
    Operand V = valueFor(E, AT->Elem);
    at(E);
    FB.assign(FB.constIndex(Arr, uint32_t(Idx)), FB.use(V));
  }
  return copyOf(Arr);
}

Operand Body::lowerCompositeLit(const ast::CompositeLit *CL) {
  S::Type *ST = sTyOf(CL);
  Type *Ty = tyOf(CL);
  if (!Ty)
    return errorValue(ctx());
  S::Type *U = S::under(ST);
  if (auto *Str = dyn_cast<S::StructType>(U))
    return lowerStructLit(CL, Str, Ty);
  if (auto *Arr = dyn_cast<S::ArrayType>(U))
    return lowerArrayLit(CL, Arr, Ty);
  if (auto *Sl = dyn_cast<S::SliceType>(U))
    return sliceLiteral(CL, Sl, Ty);
  if (auto *Mp = dyn_cast<S::MapType>(U))
    return mapLiteral(CL, Mp);
  unsupported(CL, "this composite literal");
  return errorValue(ctx());
}
