//===- LowerFunc.cpp - Func values and closures ---------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P04.07 (docs/design/gir.md section 15, "Closures and func values"):
//
//   func value   Tuple(Func(env *u8, params...) -> ret [Go, env], *u8): the
//                code and its environment. A top-level function is a value
//                whose code is an adapter (the function with the environment
//                parameter in front, ignored) and whose environment is null.
//   closure      the same pair; the environment points at a tuple of pointers,
//                one to the storage of each captured variable (a frame slot
//                when the literal does not escape, a heap cell when it does).
//   method value the environment holds the receiver, bound where the value is
//                made (Go, CLO-5): a pointer for a pointer method, a copy for
//                a value method of a type that owns nothing (CLO-5a), the
//                itab's function and the data word for an interface; the code
//                is a thunk that loads it.
//
// What a func value may point to in a frame is decided in one place,
// Body::mayHold (gir.md section 15, "What a func value may point to").
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

#include "gopp/Mangle/SymbolName.h"

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

//===----------------------------------------------------------------------===//
// Adapters of top-level functions
//===----------------------------------------------------------------------===//

FuncId Lowering::funcAdapter(const S::Func *F, SourceLoc At) {
  auto FI = FuncIds.find(F);
  if (FI == FuncIds.end()) {
    if (!Declined.count(F)) // (a declined function has been reported)
      unsupported(At, "the function " + F->Name.str() + " as a value");
    return NoId;
  }
  return adapterOf(FI->second, Syms.Names.func(F));
}

FuncId Lowering::instanceAdapter(const GenericInst &GI) {
  // An instance of a generic function as a value (P04.08): the same adapter,
  // named by the instance's symbol (`TE` of `F[int]`).
  auto II = InstIds.find(&GI);
  if (II == InstIds.end())
    return NoId; // (the instance was refused where it is declared)
  return adapterOf(II->second, Syms.Names.funcInstance(GI.Decl, GI.TArgs));
}

FuncId Lowering::adapterOf(FuncId Real, const sym::Node *MN) {
  // The code of the func value of F: F with the environment in front, the
  // shape of a method expression's code, so it has that wrapper's name
  // (appendix A.5, `TE`). linkonce_odr: every package that takes F as a
  // value makes the same code, and the copies fold.
  // (Without a symbol node, the name of the function's own symbol keeps the
  // adapter's unique: never a name built from a spelling, review R3.)
  std::string Name =
      MN ? sym::mangle(Syms.Ctx.wrapper(sym::Kind::MethodExpr, MN))
         : runtimeSymbol("fnval_" + M->func(Real).Name);
  auto Hit = Thunks.find(Name);
  if (Hit != Thunks.end())
    return Hit->second;
  const Function &RF = M->func(Real);
  Type *PU8 = Ctx.ptr(Ctx.u8());
  SmallVector<Type *, 4> Params{PU8};
  for (Type *P : RF.Sig->elems())
    Params.push_back(P);
  auto Fn = std::make_unique<Function>();
  Fn->Name = Name;
  Fn->Sig = Ctx.func(Params, RF.Sig->ret(), false, CallConv::Go, true);
  Fn->Link = Linkage::LinkOnceODR;
  Fn->CC = CallConv::Go;
  Fn->NoUnwind = RF.NoUnwind;
  Fn->Loc = RF.Loc; // (artificial: its symbol says it is a wrapper)
  Function *FP = Fn.get();
  FuncId Id = M->addFunction(std::move(Fn));
  Thunks[Name] = Id;
  Rt E(*M, *FP);
  FuncBuilder &FB = E.FB;
  FB.setSpan(FP->Loc);
  SmallVector<Operand, 4> Args;
  for (size_t K = 1; K < Params.size(); ++K)
    Args.push_back(copyOf(FB.local(FB.param(K))));
  LocalId R = FB.newTemp(RF.Sig->ret());
  FB.call(
      constFunc(*M, Real), Args, FB.local(R),
      {RF.NoUnwind ? UnwindAction::Unreachable : UnwindAction::Continue, NoId});
  if (RF.Sig->ret()->isUnit())
    E.Return();
  else
    E.Return(E.val(FB.local(R)));
  E.finish();
  return Id;
}

//===----------------------------------------------------------------------===//
// Values
//===----------------------------------------------------------------------===//

Operand Body::funcValue(FuncId Code, Operand Env, Type *VT) {
  return copyOf(FB.temp(
      FB.aggregate(AggKind::Tuple, VT, {constFunc(M, Code), std::move(Env)})));
}

Operand Body::funcValueOfFunc(const S::Func *F, const ast::Node *N,
                              const ast::Expr *Name) {
  if (F->Recv || F->IsInterfaceMethod || F->IsBuiltinMethod) {
    unsupported(N, "this function value");
    return errorValue(ctx());
  }
  Type *VT = nullptr;
  if (auto *E = dyn_cast<ast::Expr>(N))
    VT = tyOf(E);
  if (!VT || !VT->isFuncValue())
    return errorValue(ctx());
  FuncId A = NoId;
  if (L.isGenericDecl(F)) {
    // An instance: the walk found it where Name is (the same decision for
    // the check and the build).
    const GenericInst *GI = L.Walk.funcRef(CurInst, Name);
    if (!GI) {
      if (!L.Failed)
        L.internalError(N->getBegin(), "the instance of " + F->Name.str() +
                                           " is missing (the walk did not "
                                           "find it)");
      return errorValue(ctx());
    }
    A = L.instanceAdapter(*GI);
  } else {
    A = L.funcAdapter(F, N->getBegin());
  }
  if (A == NoId)
    return errorValue(ctx());
  if (M.func(A).Sig != VT->elems()[0]) {
    unsupported(N, "the function " + F->Name.str() + " as a value");
    return errorValue(ctx());
  }
  at(N);
  return funcValue(A, zeroOf(ctx().ptr(ctx().u8())), VT);
}

//===----------------------------------------------------------------------===//
// What a func value may point to
//===----------------------------------------------------------------------===//

void Body::refuseBorrow(const ast::Node *N, const llvm::Twine &Held,
                        const llvm::Twine &Who, StringRef Fix) {
  L.unsupported(N->getBegin(),
                "a closure or method value that outlives the call and holds " +
                    Held + " (CLO-3a, P04.20)",
                "the program breaks CLO-3a: " + Who +
                    ", and the closure or method value would outlive it (the "
                    "borrow checker, P04.20, makes this a compile error); " +
                    Fix);
}

bool Body::mayHold(const S::Var *V, bool Escapes, const ast::Node *N) {
  if (!Escapes)
    // It cannot outlive this call (CLO-1): a pointer to the variable's own
    // storage is fine, whatever it is (a frame slot, a cell, the caller's
    // place of a borrowed parameter, the element of an `&` range variable).
    return true;
  // CLO-3a: a variable whose value holds a borrow (OWN-B1) refers to storage
  // it does not own, and a cell would outlive that borrow: a borrowed
  // variable (a borrowed parameter or receiver, a range variable or a
  // type-switch binding of an owning type), an `&` range variable, a view or
  // borrowed slice (a View parameter, a local made by slicing), and a
  // variable whose type holds a view inside it (a `@View` struct, an array,
  // Option, slice, map or channel of views). A Place parameter points into
  // the caller's frame, a slice of a local array or an `&` element of one
  // into this frame. The checker's facts and its own walk over the types say
  // which (`MethodSets::holdsBorrow`: `Var::isBorrow`, and
  // `Checker::containsView` on the variable's type). The borrow checker
  // makes it a compile error (OWN-B5, P04.20); until then it is one E0439
  // that says so.
  // (In generic code, the variable's type and mode at the instance: a `T`
  // is borrowed, or holds a view, when the type argument makes it one, by
  // the rule plain code uses: modeOf, decision M6 and review R5.)
  S::Type *VT = tyOfObj(V);
  const S::Var::Borrow Mode = modeOf(V);
  const bool Held =
      CurInst ? L.Methods.holdsBorrowAt(V, VT) : L.Methods.holdsBorrow(V);
  if (Held) {
    // What the borrow is, and CLO-3a's fix (never `.Clone()` here; a copy
    // never reuses the name, RNG-8; copying a view takes a conversion).
    const bool Alias = Mode == S::Var::Alias;
    const bool Borrowed = Mode == S::Var::Borrowed;
    const bool Itself =
        Alias || Borrowed || llvm::isa_and_nonnull<S::RefType>(VT);
    StringRef What = Alias      ? "the & range variable "
                     : Borrowed ? "the borrowed variable "
                     : Itself   ? "the view or borrowed slice "
                                : "the variable ";
    StringRef Fix = "copy it into a differently named local";
    if (Alias)
      Fix = "range by value, or copy the element into a differently named "
            "local";
    else if (llvm::isa_and_nonnull<S::RefType>(VT))
      Fix = "copy it into an owned local with a conversion (`string(v)`, "
            "`[]T(v)`: OWN-V3)";
    else if (V->IsRecv)
      Fix = "make the receiver `move`, or copy it into a local";
    else if (V->IsParam)
      Fix = "make the parameter `move`, or copy it into a local";
    else if (!Itself)
      Fix = "copy what its views show into owned values (a conversion "
            "such as `string(v)` or `[]T(v)` copies one: OWN-V3)";
    std::string Name = V->Name.str();
    // (In generic code, the instance whose type argument makes it a borrow:
    // the same literal is fine in another, decision M6.)
    std::string In =
        CurInst ? " (in the instance " + L.Walk.nameOf(*CurInst) + ")" : "";
    refuseBorrow(N,
                 What + Name + (Itself ? "" : ", whose type holds a view") + In,
                 Name + (Itself ? " is a borrow"
                                : " holds a borrow (a view inside its value)"),
                 Fix);
    return false;
  }
  // Everything else it holds lives in a cell (Escape.cpp put it in Cells;
  // declareSlot, cellParams and bindResults made it one). A frame slot here
  // would dangle once this call returns: never silently.
  if (!Facts || !Facts->Cells.count(V) || Vars.count(V)) {
    L.internalError(N->getBegin(), "the variable " + V->Name.str() +
                                       " that an escaping closure holds has "
                                       "no cell");
    return false;
  }
  return true;
}

Operand Body::varAddr(const S::Var *V, const ast::Node *N) {
  at(N);
  if (auto Al = Aliases.find(V); Al != Aliases.end())
    return copyOf(FB.local(Al->second));
  auto It = Vars.find(V);
  if (It == Vars.end()) {
    unsupported(N, "the captured variable " + V->Name.str());
    return errorValue(ctx());
  }
  return copyOf(FB.temp(FB.addrOf(FB.local(It->second), true)));
}

Operand Body::ownBoxCopy(Operand View, const S::Var *V, const ast::Node *N) {
  // TYP-9: a box binding is a view of the box. Where plain code binds a copy
  // instead (the case type is a plain copy at this instance: modeOf), a
  // closure that outlives the call and holds the binding holds a copy of its
  // own, never the box (review R5): the view points at a heap copy made
  // here, as plain code's binding is a heap cell then. (A binding no such
  // closure holds stays a view of the box: it does not outlive the call.)
  if (!CurInst || !V || !V->BoxBinding || !Facts || !Facts->Cells.count(V) ||
      modeOf(V) != S::Var::Owned)
    return View;
  Type *PT = View.type();
  if (!PT || !PT->isPtr() || !isa<S::RefType>(tyOfObj(V)))
    return View;
  Operand Val = copyOf(FB.deref(FB.local(localOf(std::move(View)))));
  Operand Raw = envPtr(PT->elem(), std::move(Val), /*Heap=*/true, N);
  at(N);
  return copyOf(FB.temp(FB.cast(CastKind::PtrToPtr, std::move(Raw), PT)));
}

Operand Body::envPtr(Type *EnvT, Operand Val, bool Heap, const ast::Node *N) {
  Type *PU8 = ctx().ptr(ctx().u8());
  at(N);
  if (Heap) {
    // On the heap (it leaks until P04.09 gives it a drop).
    Operand Raw = callFn(
        L.rt(RtFn::MemResize, N->getBegin()),
        {zeroOf(PU8),
         constInt(ctx().u64(), std::max<uint64_t>(L.layout(EnvT).Size, 1)),
         constBool(ctx(), true)});
    LocalId EP = localOf(copyOf(
        FB.temp(FB.cast(CastKind::PtrToPtr, std::move(Raw), ctx().ptr(EnvT)))));
    FB.assign(FB.deref(FB.local(EP)), FB.use(std::move(Val)));
    return copyOf(
        FB.temp(FB.cast(CastKind::PtrToPtr, copyOf(FB.local(EP)), PU8)));
  }
  // In the frame, for as long as the value is used (it does not escape).
  LocalId Tmp = FB.newTemp(EnvT);
  FB.assign(FB.local(Tmp), FB.use(std::move(Val)));
  return copyOf(
      FB.temp(FB.cast(CastKind::PtrToPtr,
                      copyOf(FB.temp(FB.addrOf(FB.local(Tmp), true))), PU8)));
}

//===----------------------------------------------------------------------===//
// Function literals
//===----------------------------------------------------------------------===//

Operand Body::lowerFuncLit(const ast::FuncLit *Lit) {
  S::Type *ST = sTyOf(Lit);
  Type *VT = tyOf(Lit);
  auto *Sig = ST ? dyn_cast<S::SignatureType>(S::under(ST)) : nullptr;
  auto FIt =
      Facts ? Facts->Lits.find(Lit) : decltype(Facts->Lits)::const_iterator();
  if (!VT || !Sig || !Facts || FIt == Facts->Lits.end() || !VT->isFuncValue()) {
    if (VT)
      unsupported(Lit, "this function literal");
    return errorValue(ctx());
  }
  const LitFacts &LF = FIt->second;
  // A literal in the initializer of a blank package variable (`var _ =
  // ...`) has no entity to be named by (appendix A.5).
  if (!ScopeNode) {
    unsupported(Lit, "a function literal in the initializer of a blank "
                     "variable (P04.27)");
    return errorValue(ctx());
  }
  // What it may hold, decided before its body is lowered: a refused literal
  // is one report, and the literals inside it say nothing more.
  for (const S::Var *V : LF.Captures) {
    ++EscapeSteps;
    if (!mayHold(V, LF.Escapes, Lit))
      return errorValue(ctx());
  }
  // The closure function: a function of its own, named by its place in the
  // enclosing function (sym::Context::closure).
  const sym::Node *CN = L.Syms.Ctx.closure(ScopeNode, LF.Ordinal);
  auto Fn = std::make_unique<Function>();
  Fn->Name = sym::mangle(CN);
  Fn->Sig = VT->elems()[0];
  Fn->Link = Linkage::Internal;
  Fn->CC = CallConv::Go;
  Fn->Loc = L.spanOf(Lit->getBegin());
  FuncId Id = M.addFunction(std::move(Fn));
  {
    // (In generic code, with the instance's types: the closure belongs to
    // the instance, decision M6.)
    Body B(L, M.func(Id), nullptr, CurInst);
    B.ScopeNode = CN;
    B.Facts = Facts;
    B.lowerLit(Lit, LF, Sig);
  }
  // The environment: a pointer to each captured variable's storage.
  Type *PU8 = ctx().ptr(ctx().u8());
  Operand Env = zeroOf(PU8);
  if (!LF.Captures.empty()) {
    SmallVector<Operand, 4> Ptrs;
    SmallVector<Type *, 4> PTys;
    for (const S::Var *V : LF.Captures) {
      ++EscapeSteps;
      Operand P = varAddr(V, Lit);
      PTys.push_back(P.type());
      Ptrs.push_back(std::move(P));
    }
    Type *EnvT = ctx().tuple(PTys);
    at(Lit);
    Env =
        envPtr(EnvT, copyOf(FB.temp(FB.aggregate(AggKind::Tuple, EnvT, Ptrs))),
               LF.Escapes, Lit);
  }
  at(Lit);
  return funcValue(Id, std::move(Env), VT);
}

void Body::lowerLit(const ast::FuncLit *Lit, const LitFacts &LF,
                    S::SignatureType *Sig) {
  Function &F = FB.function();
  at(Lit);
  pushScope(false);
  FnSig = Sig;
  const unsigned Lead = 1; // the environment
  if (Sig->Params->Elems.size() + Lead != F.numParams()) {
    unsupported(Lit, "this parameter list");
    return;
  }
  // The captured variables: the environment points at a tuple of pointers.
  // Reading them is the function's entry (Statement::Prologue, V077), so a
  // debugger that stops at the closure shows them already.
  if (!LF.Captures.empty()) {
    const size_t Mark = entryMark();
    SmallVector<Type *, 4> PTys;
    for (const S::Var *V : LF.Captures) {
      ++EscapeSteps;
      // (The type the enclosing function's varAddr gives: a pointer to the
      // variable, also for a cell and for a Place parameter.)
      Type *T = L.lowerType(tyOfObj(V), Lit->getBegin());
      if (!T)
        return;
      PTys.push_back(ctx().ptr(T));
    }
    Type *EnvT = ctx().tuple(PTys);
    LocalId EP = localOf(copyOf(FB.temp(FB.cast(
        CastKind::PtrToPtr, copyOf(FB.local(FB.param(0))), ctx().ptr(EnvT)))));
    for (size_t K = 0; K < LF.Captures.size(); ++K) {
      ++EscapeSteps;
      const S::Var *V = LF.Captures[K];
      // The debugger shows the variable by its name, as the place the
      // pointer points to (Local::Deref).
      LocalId PL = FB.newLocal(PTys[K], Local::Var, V->Name.str());
      F.Locals[PL].Source = true;
      F.Locals[PL].Deref = true;
      FB.storageLive(PL);
      FB.assign(FB.local(PL),
                FB.use(copyOf(FB.field(FB.deref(FB.local(EP)), K))));
      Aliases[V] = PL;
      Scopes.back().Locals.push_back(PL);
    }
    prologueSince(Mark);
  }
  // The parameters, in the order of the signature.
  SmallVector<S::Var *, 4> Params;
  unsigned K = 0;
  if (Lit->Type->Params)
    for (const ast::Field *Fld : Lit->Type->Params->List) {
      size_t N = Fld->Names.empty() ? 1 : Fld->Names.size();
      for (size_t J = 0; J < N; ++J, ++K) {
        S::Var *V = nullptr;
        if (!Fld->Names.empty()) {
          auto D = I.Defs.find(Fld->Names[J]);
          if (D != I.Defs.end())
            V = dyn_cast_or_null<S::Var>(D->second);
        }
        if (!V)
          continue; // unnamed or `_`
        bindParam(K + Lead, V, L.passOf(Sig, K), Lit->getBegin());
        Params.push_back(V);
      }
    }
  for (S::Type *T : Sig->Results->Elems)
    ResultTys.push_back(T);
  // Named results (all or none of the declared ones; the error of a `func!`
  // literal is none of them, ERR-3).
  SmallVector<ResultVar, 2> Rs;
  if (Lit->Type->Results)
    for (const ast::Field *Fld : Lit->Type->Results->List)
      for (const ast::Ident *Nm : Fld->Names) {
        auto D = I.Defs.find(Nm);
        const S::Var *V =
            D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::Var>(D->second);
        if (S::Type *T = V ? tyOfObj(V) : nullptr)
          Rs.push_back({V, T, V->Name});
      }
  const size_t Declared = Sig->Results->size() - (Sig->Fails ? 1 : 0);
  if (!Rs.empty() && Rs.size() == Declared && !bindResults(Rs, Lit->getBegin()))
    return;
  cellParams(Params);
  enterCells(Lit->Body);
  RootBody = Lit->Body;
  ShadowBody = true;
  lowerBlock(Lit->Body);
  endBody(Lit->Body);
  Scopes.pop_back();
  finish();
}

//===----------------------------------------------------------------------===//
// Calls through func values
//===----------------------------------------------------------------------===//

Operand Body::lowerCallValue(const ast::CallExpr *C) {
  const ast::Expr *Fun = unparen(C->Fun);
  S::Type *FT = sTyOf(Fun);
  auto *Sig = FT ? dyn_cast<S::SignatureType>(S::under(FT)) : nullptr;
  if (!Sig) {
    unsupported(C, "this call");
    return zeroOfExpr(C);
  }
  // The function value is evaluated before the arguments (Go). A func value
  // is never nil (OWN-P9): an `Option[func]` is checked before the call.
  Operand V = lowerExpr(Fun);
  if (!V.type()->isFuncValue())
    return failed(Fun, zeroOfExpr(C));
  at(C);
  Place P = placeOf(std::move(V));
  LocalId Code = localOf(copyOf(FB.field(P, 0)));
  LocalId Env = localOf(copyOf(FB.field(P, 1)));
  Type *CodeTy = FB.function().Locals[Code].Ty;
  return callWith(C, Sig, CodeTy, copyOf(FB.local(Code)),
                  {copyOf(FB.local(Env))}, /*NoUnwind=*/false);
}

//===----------------------------------------------------------------------===//
// Method values and method expressions
//===----------------------------------------------------------------------===//

/// The code of the method value `x.M`: (env, params...) -> results. A method
/// of a concrete type finds its receiver in the environment, as the method
/// takes it: a pointer for a pointer method, the value for a value method
/// (one of a type that owns nothing: CLO-5a refuses the others). An
/// interface method finds the code pointer and the data word the itab call
/// needs, looked up when the value was made (Go does the same: a nil
/// interface panics there).
FuncId Lowering::methodValueThunk(const S::Func *F, Type *CodeTy,
                                  SourceLoc At) {
  // An interface method has no symbol of its own, and its thunk does not
  // depend on the interface: it calls the code pointer its environment holds.
  // So one thunk serves every interface method of one code type (two local
  // interfaces may both be called `I` and have a `Get` of two types: review
  // R3), internal to the module, named in the order they are made.
  if (F->IsInterfaceMethod)
    if (auto Hit = IfaceValueThunks.find(CodeTy); Hit != IfaceValueThunks.end())
      return Hit->second;
  const sym::Node *MN = F->IsInterfaceMethod ? nullptr : Syms.Names.func(F);
  std::string Name;
  if (MN)
    Name = sym::mangle(Syms.Ctx.wrapper(sym::Kind::MethodValue, MN));
  else if (F->IsInterfaceMethod)
    Name =
        runtimeSymbol("ifacevalue_" + std::to_string(IfaceValueThunks.size()));
  else if (auto FI = FuncIds.find(F); FI != FuncIds.end())
    Name = runtimeSymbol("mval_" + M->func(FI->second).Name);
  if (!Name.empty())
    if (auto Hit = Thunks.find(Name); Hit != Thunks.end())
      return Hit->second;
  Type *PU8 = Ctx.ptr(Ctx.u8());
  FuncId Real = NoId;
  Span Loc;
  bool NoUnwind = false;
  if (!F->IsInterfaceMethod) {
    auto FI = FuncIds.find(F);
    if (FI == FuncIds.end()) {
      if (!Declined.count(F))
        unsupported(At, "the method " + F->Name.str() + " as a value");
      return NoId;
    }
    Real = FI->second;
    Loc = M->func(Real).Loc;
    NoUnwind = M->func(Real).NoUnwind;
  }
  auto Fn = std::make_unique<Function>();
  Fn->Name = Name;
  Fn->Sig = CodeTy;
  Fn->Link = F->IsInterfaceMethod ? Linkage::Internal : Linkage::LinkOnceODR;
  Fn->CC = CallConv::Go;
  Fn->NoUnwind = NoUnwind;
  Fn->Loc = Loc;
  Function *FP = Fn.get();
  FuncId Id = M->addFunction(std::move(Fn));
  if (F->IsInterfaceMethod)
    IfaceValueThunks[CodeTy] = Id;
  else
    Thunks[Name] = Id;
  Rt E(*M, *FP);
  FuncBuilder &FB = E.FB;
  FB.setSpan(FP->Loc);
  Val Env = E.param(0);
  SmallVector<Operand, 4> Args;
  Operand Callee;
  if (F->IsInterfaceMethod) {
    Type *FT = methodSig(F->sig(), PU8, At);
    if (!FT)
      return Id;
    // The environment is (code, data): the itab's entry and the data word.
    Place Pair = E.at(Env, Ctx.tuple({PU8, PU8}));
    Val Fp = E.snap(E.val(FB.field(Pair, 0)));
    Val Data = E.snap(E.val(FB.field(Pair, 1)));
    LocalId FnL = FB.newTemp(FT);
    FB.assign(FB.local(FnL), FB.cast(CastKind::PtrToPtr, Fp.Op, FT));
    Callee = copyOf(FB.local(FnL));
    Args.push_back(Data.Op);
  } else {
    // The pointer, or the copy of the value, the method value bound.
    Type *P0 = M->func(Real).Sig->elems()[0];
    Args.push_back(E.load(Env, P0).Op);
    Callee = constFunc(*M, Real);
  }
  for (size_t K = 1; K < CodeTy->elems().size(); ++K)
    Args.push_back(copyOf(FB.local(FB.param(K))));
  LocalId R = FB.newTemp(CodeTy->ret());
  FB.call(
      Callee, Args, FB.local(R),
      {NoUnwind ? UnwindAction::Unreachable : UnwindAction::Continue, NoId});
  if (CodeTy->ret()->isUnit())
    E.Return();
  else
    E.Return(E.val(FB.local(R)));
  E.finish();
  return Id;
}

/// The code of the method expression `T.M` / `(*T).M`: the method with the
/// environment in front (`TE` of the method). `(*T).M` of a value method
/// reads the value through its first argument, as the interface thunk of a
/// pointer does (`TE` of the method's `Tp`), and passes it as the method
/// takes it (passOfRecv): a static call on a pointer passes the pointer for
/// a Place receiver (calling-convention.md).
FuncId Lowering::methodExprThunk(const S::Func *F, Type *CodeTy,
                                 bool ViaPointer, SourceLoc At) {
  auto FI = FuncIds.find(F);
  if (FI == FuncIds.end()) {
    if (!Declined.count(F))
      unsupported(At, "the method " + F->Name.str() + " as a value");
    return NoId;
  }
  const sym::Node *MN = Syms.Names.func(F);
  if (MN && ViaPointer)
    MN = Syms.Ctx.wrapper(sym::Kind::IfacePtrThunk, MN);
  std::string Name =
      MN ? sym::mangle(Syms.Ctx.wrapper(sym::Kind::MethodExpr, MN))
         : runtimeSymbol("mexpr_" + M->func(FI->second).Name +
                         (ViaPointer ? "_p" : ""));
  if (auto Hit = Thunks.find(Name); Hit != Thunks.end())
    return Hit->second;
  FuncId Real = FI->second;
  const Function &RF = M->func(Real);
  Type *P0 = RF.Sig->elems()[0];
  ArrayRef<Type *> Ps = CodeTy->elems();
  // (env, receiver, params...): the receiver is P0, or a pointer to the value
  // P0 takes.
  bool Ok =
      Ps.size() == RF.Sig->elems().size() + 1 && CodeTy->ret() == RF.Sig->ret();
  for (size_t K = 1; Ok && K < RF.Sig->elems().size(); ++K)
    Ok = Ps[K + 1] == RF.Sig->elems()[K];
  Type *Held = nullptr; // the value type behind the pointer
  if (Ok && ViaPointer) {
    Held = lowerType(F->Recv->Ty, At);
    Ok = Held && Ps[1] == Ctx.ptr(Held);
  } else if (Ok) {
    Ok = Ps[1] == P0;
  }
  if (!Ok) {
    unsupported(At, "the method " + F->Name.str() + " as a value");
    return NoId;
  }
  auto Fn = std::make_unique<Function>();
  Fn->Name = Name;
  Fn->Sig = CodeTy;
  Fn->Link = Linkage::LinkOnceODR;
  Fn->CC = CallConv::Go;
  Fn->NoUnwind = RF.NoUnwind;
  Fn->Loc = RF.Loc;
  Function *FP = Fn.get();
  FuncId Id = M->addFunction(std::move(Fn));
  Thunks[Name] = Id;
  Rt E(*M, *FP);
  FuncBuilder &FB = E.FB;
  FB.setSpan(FP->Loc);
  SmallVector<Operand, 4> Args;
  if (Held) {
    // A value method through a pointer: the value as the method takes it.
    Val Ptr = E.param(1);
    const Pass RP = passOfRecv(F);
    if (RP == Pass::Value)
      Args.push_back(copyOf(E.at(Ptr, Held)));
    else if (RP == Pass::Place)
      Args.push_back(Ptr.Op);
    else {
      Place HP = E.at(Ptr, Held);
      Args.push_back(copyOf(FB.temp(
          FB.aggregate(AggKind::Tuple, P0,
                       {copyOf(FB.field(HP, 0)), copyOf(FB.field(HP, 1))}))));
    }
  } else {
    Args.push_back(copyOf(FB.local(FB.param(1))));
  }
  for (size_t K = 2; K < Ps.size(); ++K)
    Args.push_back(copyOf(FB.local(FB.param(K))));
  LocalId R = FB.newTemp(RF.Sig->ret());
  FB.call(
      constFunc(*M, Real), Args, FB.local(R),
      {RF.NoUnwind ? UnwindAction::Unreachable : UnwindAction::Continue, NoId});
  if (RF.Sig->ret()->isUnit())
    E.Return();
  else
    E.Return(E.val(FB.local(R)));
  E.finish();
  return Id;
}

Operand Body::lowerMethodValue(const ast::SelectorExpr *Sel,
                               const S::Selection &SL) {
  auto *F = dyn_cast_or_null<S::Func>(SL.Obj);
  Type *VT = tyOf(Sel);
  if (!F || F->IsBuiltinMethod || !VT || !VT->isFuncValue()) {
    if (VT)
      unsupported(Sel, "this method value");
    return errorValue(ctx());
  }
  if (SL.ParentSteps || SL.Index.size() > 1) {
    unsupported(Sel, "a method promoted from a parent type (P04.22)");
    return errorValue(ctx());
  }
  Type *PU8 = ctx().ptr(ctx().u8());
  Type *CodeTy = VT->elems()[0];
  if (SL.K == S::Selection::MethodExpr) {
    if (F->IsInterfaceMethod) {
      unsupported(Sel, "a method expression of an interface type (P04.27)");
      return errorValue(ctx());
    }
    const bool ViaPointer =
        SL.Recv && isa<S::PointerType>(S::under(SL.Recv)) && !F->HasPtrRecv;
    FuncId A = L.methodExprThunk(F, CodeTy, ViaPointer, Sel->getBegin());
    if (A == NoId)
      return errorValue(ctx());
    at(Sel);
    return funcValue(A, zeroOf(PU8), VT);
  }
  // x.M: a closure over its receiver (CLO-5), bound now. Whether it outlives
  // this call is the analysis' (Escape.cpp): its environment is a frame
  // temporary or a heap block.
  auto MIt =
      Facts ? Facts->Lits.find(Sel) : decltype(Facts->Lits)::const_iterator();
  const bool Known = Facts && MIt != Facts->Lits.end();
  const bool Escapes = !Known || MIt->second.Escapes;
  S::Type *BT = sTyOf(Sel->X);
  if (!BT)
    return failed(Sel->X, errorValue(ctx()));
  Type *StoreTy = nullptr;
  Operand SV;
  if (F->IsInterfaceMethod) {
    const Lowering::IfaceMethods *Ms = L.ifaceMethods(BT, Sel->getBegin());
    if (!Ms)
      return errorValue(ctx());
    const unsigned Slot = L.ifaceSlot(*Ms, F);
    if (Slot == ~0u) {
      unsupported(Sel, "the interface method " + F->Name.str() + " as a value");
      return errorValue(ctx());
    }
    Operand X = lowerExpr(Sel->X);
    if (!X.type() || !X.type()->isIface())
      return failed(Sel->X, errorValue(ctx()));
    Place XP = placeOf(std::move(X));
    at(Sel);
    Operand Itab = copyOf(FB.temp(FB.use(copyOf(FB.field(XP, 0)))));
    Operand Data = copyOf(FB.temp(FB.use(copyOf(FB.field(XP, 1)))));
    Rt E(M, FB);
    // `i.M` of a nil interface (`error`, `any`) panics where it is made (Go).
    Val It{&E, Itab};
    check(E.not_(It == E.null(PU8)).Op, AssertKind::Nil, false, Sel);
    // (The low bit of the itab word is the borrow tag, P04.06.)
    Val Bits = E.to(ctx().u64(), It);
    Val Tab = E.to(PU8, Bits - (Bits & 1));
    Val Fp = E.load(E.padd(Tab, E.u(8 * (uint64_t(Slot) + 1))), PU8);
    StoreTy = ctx().tuple({PU8, PU8});
    SV = copyOf(FB.temp(FB.aggregate(AggKind::Tuple, StoreTy, {Fp.Op, Data})));
  } else {
    auto FI = L.FuncIds.find(F);
    if (FI == L.FuncIds.end()) {
      if (!L.Declined.count(F))
        unsupported(Sel, "the method " + F->Name.str() + " as a value");
      return errorValue(ctx());
    }
    if (F->MoveRecv) {
      // Each call would consume the one bound receiver.
      unsupported(Sel, "a method value of a method with a move receiver "
                       "(P04.09)");
      return errorValue(ctx());
    }
    if (!F->HasPtrRecv && F->Recv && L.recvModeOf(F) == S::Var::Borrowed) {
      // CLO-5a: a value receiver of an owning type would need a copy no one
      // writes (or would see later writes): a compile error with the fix-its
      // `x.Clone().M` and `move(x).M`, which are not lowered either.
      unsupported(Sel, "a method value of a value method of an owning type "
                       "(CLO-5a, P04.27)");
      return errorValue(ctx());
    }
    bool BasePtr = isa<S::PointerType>(BT);
    if (F->HasPtrRecv) {
      if (BasePtr) {
        SV = lowerExpr(Sel->X);
      } else {
        // The address of x (CLO-5: as in Go, `x.M` binds &x): the variable
        // whose storage it is in, if any, is what the method value holds.
        if (Known)
          for (const S::Var *V : MIt->second.Captures)
            if (!mayHold(V, Escapes, Sel))
              return errorValue(ctx());
        Place P = lowerPlace(Sel->X);
        if (!P.Ty)
          return failed(Sel->X, errorValue(ctx()));
        at(Sel);
        SV = copyOf(FB.temp(FB.addrOf(std::move(P), true)));
      }
      StoreTy = SV.type();
    } else {
      // The receiver is copied into the environment (CLO-5). A copy that
      // holds a view is a borrow (OWN-B1), which a method value that
      // outlives the call may not hold (CLO-3a), as with a captured
      // variable (mayHold).
      S::Type *RecvTy = L.recvTypeOf(F);
      if (Escapes && L.Methods.holdsView(RecvTy)) {
        refuseBorrow(Sel, "a copy of its receiver, whose type holds a view",
                     "the copy holds a borrow (a view inside its value)",
                     "copy what its views show into owned values (a "
                     "conversion such as `string(v)` or `[]T(v)` copies one: "
                     "OWN-V3)");
        return errorValue(ctx());
      }
      StoreTy = L.lowerType(RecvTy, Sel->getBegin());
      if (!StoreTy)
        return errorValue(ctx());
      if (BasePtr) {
        Operand P = lowerExpr(Sel->X);
        if (!P.type() || !P.type()->isPtr())
          return failed(Sel->X, errorValue(ctx()));
        at(Sel);
        SV = copyOf(FB.deref(FB.local(localOf(std::move(P)))));
      } else {
        SV = lowerExpr(Sel->X);
      }
    }
    if (!SV.type() || SV.type() != StoreTy)
      return failed(Sel->X, errorValue(ctx()));
  }
  FuncId A = L.methodValueThunk(F, CodeTy, Sel->getBegin());
  if (A == NoId)
    return errorValue(ctx());
  Operand Env = envPtr(StoreTy, std::move(SV), Escapes, Sel);
  at(Sel);
  return funcValue(A, std::move(Env), VT);
}
