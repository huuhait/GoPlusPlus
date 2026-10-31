//===- LowerIface.cpp - Methods, interfaces, type descriptors -------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The static data of dynamic dispatch (docs/design/gir.md section 15,
// "Interfaces"): one type descriptor per dynamic type, its method table, one
// itab per (type, interface) pair, one descriptor per interface, and the
// thunks an itab calls. All are LinkOnceODR globals named by P04.17, so two
// packages that make the same one agree on it (pointer identity of a
// descriptor is type identity).
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"
#include "RtEmit.h"

#include "gopp/Basic/Unicode.h"
#include "gopp/GIR/Glue.h"
#include "gopp/Mangle/SymbolName.h"

#include "llvm/ADT/SmallPtrSet.h"

#include <algorithm>
#include <optional>

using namespace gopp;
using namespace gopp::gir;
using namespace gopp::gir::impl;

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

//===----------------------------------------------------------------------===//
// Method sets
//===----------------------------------------------------------------------===//

bool Lowering::isInterface(S::Type *T) {
  auto *IT = dyn_cast_or_null<S::InterfaceType>(S::under(T));
  return IT && IT->Sp == S::InterfaceType::None && !IT->Implicit &&
         !IT->Extends;
}

bool Lowering::methodSupported(const S::Func *F, SourceLoc At) {
  if (F->MethodMark != S::Func::Mark::None) {
    unsupported(At, "a method with @Virtual, @Abstract or @Override (P04.22)");
    return false;
  }
  // (A method of a generic type is lowered per instance, P04.08; a generic
  // method of a plain type is an instance of its own, P04.28.)
  if (!F->OwnTParams.empty()) {
    unsupported(At, "a generic method (P04.28)");
    return false;
  }
  return true;
}

std::string Lowering::methodKey(const S::Func *F) {
  auto It = MethodKeys.find(F);
  if (It != MethodKeys.end())
    return It->second;
  std::string K = F->Name.str();
  K += '\x01';
  if (!S::isExportedName(F->Name) && F->Pkg)
    K += Syms.Names.pathOf(F->Pkg).str();
  K += '\x01';
  if (const sym::Node *N = Syms.Names.type(F->Ty))
    K += sym::mangleType(N);
  else
    K += "?";
  return MethodKeys[F] = K;
}

namespace {
void sortByKey(llvm::SmallVectorImpl<Lowering::MethodEntry> &Ms) {
  std::sort(Ms.begin(), Ms.end(),
            [](const Lowering::MethodEntry &A, const Lowering::MethodEntry &B) {
              return A.Key < B.Key;
            });
}
} // namespace

const Lowering::IfaceMethods *Lowering::ifaceMethods(S::Type *I, SourceLoc At) {
  S::Type *U = S::under(I);
  auto Hit = IfaceMethodCache.find(U);
  if (Hit != IfaceMethodCache.end())
    return Hit->second.get();
  if (IfaceBad.count(U))
    return nullptr;
  auto Bad = [&](const char *Why) -> const IfaceMethods * {
    IfaceBad.insert(U);
    unsupported(At, Why);
    return nullptr;
  };
  if (!isInterface(U))
    return Bad("an interface that is a constraint, used as a type");
  // The checker's list: embedded interfaces flattened, a method that two of
  // them name once (its type set).
  auto Out = std::make_unique<IfaceMethods>();
  for (const S::Func *M : Methods.interfaceMethods(U)) {
    ++LowerSteps;
    if (M->isOperator())
      return Bad("an interface with an operator element (P04.23)");
    Out->List.push_back({M, methodKey(M)});
  }
  sortByKey(Out->List);
  for (unsigned K = 0; K < Out->List.size(); ++K)
    Out->Slot[Out->List[K].Key] = K;
  return (IfaceMethodCache[U] = std::move(Out)).get();
}

unsigned Lowering::ifaceSlot(const IfaceMethods &Ms, const S::Func *F) {
  ++LowerSteps;
  const std::string Key = methodKey(F);
  if (Opts.ScanItabSlots) {
    // (The control of the stress test: the scan the table lookup replaces.)
    for (unsigned K = 0; K < Ms.List.size(); ++K) {
      ++LowerSteps;
      if (Ms.List[K].Key == Key)
        return K;
    }
    return ~0u;
  }
  auto It = Ms.Slot.find(Key);
  return It == Ms.Slot.end() ? ~0u : It->second;
}

bool Lowering::sameItabs(const IfaceMethods &A, const IfaceMethods &B) {
  ++LowerSteps;
  if (&A == &B)
    return true;
  if (A.List.size() != B.List.size())
    return false;
  auto Hit = SameItabCache.find({&A, &B});
  if (Hit != SameItabCache.end())
    return Hit->second;
  bool Same = true;
  for (size_t K = 0; K < A.List.size() && Same; ++K) {
    ++LowerSteps;
    Same = A.List[K].Key == B.List[K].Key;
  }
  SameItabCache[{&A, &B}] = Same;
  return Same;
}

const SmallVector<Lowering::MethodEntry, 4> *Lowering::methodSet(S::Type *Dyn,
                                                                 SourceLoc At) {
  auto Hit = MethodSetCache.find(Dyn);
  if (Hit != MethodSetCache.end())
    return Hit->second.get();
  if (MethodSetBad.count(Dyn))
    return nullptr;
  SmallVector<S::MethodHit, 8> Hits;
  Methods.methodSet(Dyn, Hits);
  auto Out = std::make_unique<SmallVector<MethodEntry, 4>>();
  for (const S::MethodHit &H : Hits) {
    ++LowerSteps;
    if (H.ParentSteps || !H.Path.empty()) {
      MethodSetBad.insert(Dyn);
      unsupported(At, "a value of a type with an inherited method (" +
                          H.F->Name.str() + ") in an interface (P04.22)");
      return nullptr;
    }
    Out->push_back({H.F, methodKey(H.F)});
  }
  sortByKey(*Out);
  return (MethodSetCache[Dyn] = std::move(Out)).get();
}

//===----------------------------------------------------------------------===//
// Go's spelling of a type, and comparability
//===----------------------------------------------------------------------===//

std::string Lowering::rtypeShown(S::Type *T) {
  if (Methods.extent(T).Size <= MaxNameParts)
    return rtypeName(T);
  if (auto *N = dyn_cast<S::NamedType>(T))
    return (N->Obj->Pkg ? N->Obj->Pkg->Name.str() + "." : std::string()) +
           N->Obj->Name.str() + "[...]";
  return "...";
}

std::string Lowering::rtypeName(S::Type *T, bool InArg) {
  switch (T->getKind()) {
  case S::Type::Kind::Basic: {
    // A unit type is spelled with its unit, `float64[KM]` (UNI-12); the units
    // of package main are bare, any other is `pkg.KM`, so the name never
    // depends on the package that lowers the type.
    if (cast<S::BasicType>(T)->Unit)
      return unitSpeller().text(cast<S::BasicType>(T),
                                CP.Pkg && CP.Pkg->Name == "main" ? CP.Pkg
                                                                 : nullptr);
    switch (cast<S::BasicType>(T)->BK) {
    case S::BasicKind::Bool:
      return "bool";
    case S::BasicKind::Int:
      return "int";
    case S::BasicKind::Int8:
      return "int8";
    case S::BasicKind::Int16:
      return "int16";
    case S::BasicKind::Int32:
      return "int32";
    case S::BasicKind::Int64:
      return "int64";
    case S::BasicKind::Uint:
      return "uint";
    case S::BasicKind::Uint8:
      return "uint8";
    case S::BasicKind::Uint16:
      return "uint16";
    case S::BasicKind::Uint32:
      return "uint32";
    case S::BasicKind::Uint64:
      return "uint64";
    case S::BasicKind::Uintptr:
      return "uintptr";
    case S::BasicKind::Float32:
      return "float32";
    case S::BasicKind::Float64:
      return "float64";
    case S::BasicKind::Complex64:
      return "complex64";
    case S::BasicKind::Complex128:
      return "complex128";
    case S::BasicKind::String:
      return "string";
    case S::BasicKind::UnsafePointer:
      return "unsafe.Pointer";
    default:
      return S::typeString(T);
    }
  }
  case S::Type::Kind::Named: {
    auto *N = cast<S::NamedType>(T);
    std::string R;
    if (N->Obj->Pkg)
      R = N->Obj->Pkg->Name.str() + ".";
    R += N->Obj->Name.str();
    // An instance of a generic type: `main.Pair[int,string]`, as Go's
    // reflect spells it (the arguments in full, no space).
    if (!N->TArgs.empty()) {
      R += "[";
      for (size_t K = 0; K < N->TArgs.size(); ++K)
        R += (K ? "," : "") + rtypeName(N->TArgs[K], /*InArg=*/true);
      R += "]";
    }
    return R;
  }
  case S::Type::Kind::Pointer:
    return "*" + rtypeName(cast<S::PointerType>(T)->Elem, InArg);
  case S::Type::Kind::Slice:
    return "[]" + rtypeName(cast<S::SliceType>(T)->Elem, InArg);
  case S::Type::Kind::Array:
    return "[" + std::to_string(cast<S::ArrayType>(T)->Len) + "]" +
           rtypeName(cast<S::ArrayType>(T)->Elem, InArg);
  case S::Type::Kind::Map:
    return "map[" + rtypeName(cast<S::MapType>(T)->Key, InArg) + "]" +
           rtypeName(cast<S::MapType>(T)->Elem, InArg);
  case S::Type::Kind::Option:
    return "Option[" + rtypeName(cast<S::OptionType>(T)->Elem, InArg) + "]";
  case S::Type::Kind::Ref:
    return "View[" + rtypeName(cast<S::RefType>(T)->Elem, InArg) + "]";
  case S::Type::Kind::Interface: {
    // Go's spelling: the methods (embedded interfaces flattened) in the
    // order of Go's type descriptors, exported names first, then by name;
    // an unexported name with its package: `interface { M(int) string;
    // main.m() }`.
    SmallVector<const S::Func *, 8> Ms;
    for (const S::Func *F : Methods.interfaceMethods(T))
      Ms.push_back(F);
    if (Ms.empty())
      return "interface {}";
    std::stable_sort(
        Ms.begin(), Ms.end(), [](const S::Func *A, const S::Func *B) {
          bool EA = S::isExportedName(A->Name), EB = S::isExportedName(B->Name);
          if (EA != EB)
            return EA;
          return A->Name < B->Name;
        });
    std::string R = "interface { ";
    for (size_t K = 0; K < Ms.size(); ++K) {
      if (K)
        R += "; ";
      if (!S::isExportedName(Ms[K]->Name) && Ms[K]->Pkg)
        R += Ms[K]->Pkg->Name.str() + ".";
      R += Ms[K]->Name.str() + sigText(Ms[K]->sig(), InArg);
    }
    return R + " }";
  }
  case S::Type::Kind::Struct: {
    auto *ST = cast<S::StructType>(T);
    if (ST->Fields.empty())
      return "struct {}";
    std::string R = "struct { ";
    bool First = true;
    for (const S::Var *F : ST->Fields) {
      if (!First)
        R += "; ";
      First = false;
      // In a type argument Go qualifies an unexported field name with its
      // package (`struct { main.a int }`).
      if (InArg && !S::isExportedName(F->Name) && F->Pkg)
        R += F->Pkg->Name.str() + ".";
      R += F->Name.str() + " " + rtypeName(F->Ty, InArg);
      if (!F->Tag.empty()) // `A int "json:\"a\""`: strconv.Quote's tag
        R += " " + unicode::quote(F->Tag);
    }
    return R + " }";
  }
  case S::Type::Kind::Signature:
    return "func" + sigText(cast<S::SignatureType>(T), InArg);
  default:
    return S::typeString(T);
  }
}

std::string Lowering::sigText(S::SignatureType *SG, bool InArg) {
  std::string R = "(";
  const size_t NP = SG->Params->size();
  for (size_t K = 0; K < NP; ++K) {
    if (K)
      R += ", ";
    S::Type *P = SG->Params->Elems[K];
    // (A variadic parameter is a slice: Go spells it `...T`.)
    if (SG->Variadic && K + 1 == NP)
      if (auto *Sl = dyn_cast<S::SliceType>(S::under(P))) {
        R += "..." + rtypeName(Sl->Elem, InArg);
        continue;
      }
    R += rtypeName(P, InArg);
  }
  R += ")";
  if (SG->Results->size() == 1)
    R += " " + rtypeName(SG->Results->Elems[0], InArg);
  else if (SG->Results->size() > 1) {
    R += " (";
    for (size_t K = 0; K < SG->Results->size(); ++K) {
      if (K)
        R += ", ";
      R += rtypeName(SG->Results->Elems[K], InArg);
    }
    R += ")";
  }
  return R;
}

bool Lowering::comparable(S::Type *T) {
  // The checker decides (Go's comparable, with an interface part decided at
  // run time). A view compares as the value it shows (OWN-V), also inside a
  // struct, an array or an Option (eqGlue: viewPartsGlue, review R7).
  return Methods.comparable(T);
}

//===----------------------------------------------------------------------===//
// Static data
//===----------------------------------------------------------------------===//

Type *Lowering::mtEntType() {
  return Ctx.tuple({Ctx.string(), Ctx.ptr(Ctx.u8())});
}

Type *Lowering::tiType() {
  Type *P = Ctx.ptr(Ctx.u8()), *U64 = Ctx.u64(), *U32 = Ctx.intTy(32, false);
  return Ctx.tuple(
      {P, U64, Ctx.string(), P, P, U32, U32, U64, P, P, P, U64, P, P, P});
}

namespace {
Constant partNull(Type *Ptr) {
  Constant C;
  C.K = Constant::Null;
  C.Ty = Ptr;
  return C;
}
Constant partGlobal(Module &M, uint32_t G) { return constGlobal(M, G).C; }
Constant partFunc(Module &M, FuncId F) { return constFunc(M, F).C; }
Constant partInt(Type *Ty, uint64_t V) { return constInt(Ty, V).C; }
Constant partStr(Module &M, StringRef S) { return constStr(M, S).C; }

/// reflect.Kind of a checker type.
unsigned reflectKind(S::Type *T) {
  S::Type *U = S::under(T);
  switch (U->getKind()) {
  case S::Type::Kind::Basic:
    switch (cast<S::BasicType>(U)->BK) {
    case S::BasicKind::Bool:
      return 1;
    case S::BasicKind::Int:
      return 2;
    case S::BasicKind::Int8:
      return 3;
    case S::BasicKind::Int16:
      return 4;
    case S::BasicKind::Int32:
      return 5;
    case S::BasicKind::Int64:
      return 6;
    case S::BasicKind::Uint:
      return 7;
    case S::BasicKind::Uint8:
      return 8;
    case S::BasicKind::Uint16:
      return 9;
    case S::BasicKind::Uint32:
      return 10;
    case S::BasicKind::Uint64:
      return 11;
    case S::BasicKind::Uintptr:
      return 12;
    case S::BasicKind::Float32:
      return 13;
    case S::BasicKind::Float64:
      return 14;
    case S::BasicKind::Complex64:
      return 15;
    case S::BasicKind::Complex128:
      return 16;
    case S::BasicKind::String:
      return 24;
    case S::BasicKind::UnsafePointer:
      return 26;
    default:
      return 0;
    }
  case S::Type::Kind::Array:
    return 17;
  case S::Type::Kind::Chan:
    return 18;
  case S::Type::Kind::Signature:
    return 19;
  case S::Type::Kind::Interface:
    return 20;
  case S::Type::Kind::Map:
    return 21;
  case S::Type::Kind::Pointer:
    return 22;
  case S::Type::Kind::Slice:
    return 23;
  case S::Type::Kind::Struct:
    return 25;
  case S::Type::Kind::Option:
    return 27;
  case S::Type::Kind::Ref:
    return 28;
  default:
    return 0;
  }
}
} // namespace

uint32_t Lowering::ifaceDesc(S::Type *I, SourceLoc At) {
  const IfaceMethods *IM = ifaceMethods(I, At);
  if (!IM)
    return ~0u;
  const SmallVector<MethodEntry, 4> *Ms = &IM->List;
  // `_GPTn<interface>`: "interface descriptor for main.I" (P04.17).
  std::string Name = Syms.glue(sym::Kind::IfaceDesc, I);
  if (Name.empty())
    Name = runtimeSymbol("idesc_" + rtypeName(I));
  auto It = StaticData.find(Name);
  if (It != StaticData.end())
    return It->second;
  Type *Str = Ctx.string(), *U64 = Ctx.u64();
  Global G;
  G.Name = Name;
  G.Ty = Ctx.tuple({Str, U64, Ctx.array(Str, std::max<size_t>(Ms->size(), 1))});
  G.Link = Linkage::LinkOnceODR;
  G.ReadOnly = true;
  G.Parts.push_back(partStr(*M, rtypeShown(I)));
  G.Parts.push_back(partInt(U64, Ms->size()));
  for (const MethodEntry &E : *Ms)
    G.Parts.push_back(partStr(*M, E.Key));
  if (Ms->empty())
    G.Parts.push_back(partStr(*M, ""));
  uint32_t Id = M->addGlobal(std::move(G));
  return StaticData[Name] = Id;
}

FuncId Lowering::ifaceThunk(S::Type *Dyn, const S::Func *F, SourceLoc At) {
  // A method of an instance of a generic type that no call names: the table
  // lists the whole method set. The walk took it where a value of the type
  // became an interface value (Info::IfaceConversions) or a type test named
  // the type, so `gopp check` saw it too; one it did not take is taken now
  // (P04.08).
  if (F->RecvInst && !FuncIds.count(F))
    ensureMethod(F, At);
  auto RealIt = FuncIds.find(F);
  if (RealIt == FuncIds.end())
    return NoId;
  // Named by the method it calls, as its own symbol names it (appendix A.5:
  // a method belongs to its type, whatever its receiver). A value method in
  // the table of a pointer to its type reads the value through the data word
  // and is a thunk of its own (Tp); every other one is an IfaceThunk (TI).
  bool DynPtr = isa<S::PointerType>(Dyn);
  const sym::Node *MN = Syms.Names.func(F);
  std::string Name =
      MN ? sym::mangle(Syms.Ctx.wrapper(DynPtr && !F->HasPtrRecv
                                            ? sym::Kind::IfacePtrThunk
                                            : sym::Kind::IfaceThunk,
                                        MN))
         : runtimeSymbol("thunk_" + rtypeName(Dyn) + "." + F->Name.str());
  auto Hit = Thunks.find(Name);
  if (Hit != Thunks.end())
    return Hit->second;
  FuncId Real = RealIt->second;
  Type *RealSig = M->func(Real).Sig;
  Type *PU8 = Ctx.ptr(Ctx.u8());
  Type *RecvG = lowerType(DynPtr ? cast<S::PointerType>(Dyn)->Elem : Dyn, At);
  if (!RecvG)
    return NoId;
  SmallVector<Type *, 4> Params{PU8};
  for (Type *P : RealSig->elems().drop_front(1))
    Params.push_back(P);
  auto Fn = std::make_unique<Function>();
  Fn->Name = Name;
  Fn->Sig = Ctx.func(Params, RealSig->ret());
  Fn->Link = Linkage::LinkOnceODR;
  Fn->CC = CallConv::Go;
  Fn->NoUnwind = M->func(Real).NoUnwind;
  // The thunk and each of its statements are at the method's declaration,
  // so a debugger shows that line for its frame, never a line of the code
  // before it (P04.12, review R8); it stays artificial (its symbol says it
  // is a thunk, DebugInfoBuilder::subprogramOf).
  Fn->Loc = M->func(Real).Loc;
  Function *FP = Fn.get();
  FuncId Id = M->addFunction(std::move(Fn));
  Thunks[Name] = Id;
  Rt E(*M, *FP);
  FuncBuilder &FB = E.FB;
  FB.setSpan(FP->Loc);
  Val Data = E.param(0);
  // The receiver as passOfRecv says. A pointer the interface holds (`&e`)
  // makes the pointed-to value the place (PAR-1). A value the interface
  // holds, in its box or its data word, is immutable all the way down
  // (TYP-5), so a Place receiver is the thunk's own copy of it: a map the
  // method makes in a nil map there is dropped with the copy, as with Go's
  // value receiver, and the box stays as it was.
  const Pass RP = passOfRecv(F);
  Operand Recv;
  LocalId PlaceCopy = NoId; // the thunk's copy of a boxed Place receiver
  if (DynPtr) {
    Recv = F->HasPtrRecv || RP == Pass::Place ? E.to(Ctx.ptr(RecvG), Data).Op
                                              : copyOf(E.at(Data, RecvG));
  } else if (isInline(Dyn, RecvG)) {
    // The value is in the data word, in its low bytes; W is the copy.
    Var W = E.var(Ctx.u64(), "w");
    W = E.to(Ctx.u64(), Data);
    Recv = RP == Pass::Place ? E.to(Ctx.ptr(RecvG), E.addrOf(W)).Op
                             : copyOf(E.at(E.addrOf(W), RecvG));
  } else if (RP == Pass::Place) {
    Var Copy = E.var(RecvG, E.val(E.at(Data, RecvG)), "recv");
    Recv = E.addrOf(Copy).Op;
    PlaceCopy = Copy.L;
  } else if (FuncId CG = isSharedHandle(Dyn) && needsDrop(Dyn)
                             ? cloneGlue(Dyn, At)
                             : NoId;
             CG != NoId) {
    // A value receiver the method owns (a shared handle, OWN-C3): a retained
    // copy of the boxed one, which the method drops.
    Var Copy = E.var(RecvG, E.zero(RecvG), "recv");
    E.call(CG, {E.to(PU8, E.addrOf(Copy)), Data});
    Recv = E.val(FB.local(Copy.L)).Op;
  } else {
    Recv = copyOf(E.at(Data, RecvG));
  }
  // A borrowed string or slice receiver takes (ptr, len) of the value.
  if (RP == Pass::View) {
    Place VP = Recv.P; // (a place: the box or the pointed-to value)
    Recv = copyOf(FB.temp(
        FB.aggregate(AggKind::Tuple, RealSig->elems()[0],
                     {copyOf(FB.field(VP, 0)), copyOf(FB.field(VP, 1))})));
  }
  SmallVector<Operand, 4> Args{Recv};
  for (size_t K = 1; K < Params.size(); ++K)
    Args.push_back(copyOf(FB.local(FB.param(K))));
  LocalId R = FB.newTemp(RealSig->ret());
  FB.call(constFunc(*M, Real), Args, FB.local(R),
          {M->func(Real).NoUnwind ? UnwindAction::Unreachable
                                  : UnwindAction::Continue,
           NoId});
  // A map the method made in a nil map of the copy belongs to no one: the
  // box stays as it was (TYP-5), so it is freed here (P04.18 review R9).
  if (PlaceCopy != NoId)
    if (FuncId NM = newMapsGlue(Dyn, At); NM != NoId)
      E.call(NM, {E.to(PU8, E.addrOf(FB.local(PlaceCopy))), Data});
  if (RealSig->ret()->isUnit())
    E.Return();
  else
    E.Return(E.val(FB.local(R)));
  E.finish();
  return Id;
}

std::pair<FuncId, FuncId> Lowering::viewGlue(S::RefType *RT, Type *G,
                                             SourceLoc At) {
  auto Hit = ViewGlues.find(RT);
  if (Hit != ViewGlues.end())
    return Hit->second;
  std::pair<FuncId, FuncId> R{NoId, NoId};
  S::Type *E = RT->Elem;
  // A view of a string is {ptr, len} in a box; any other view of a
  // comparable value is a pointer in the data word. A view of a slice or a
  // map compares as one: not at all.
  bool Str = G->getKind() == Type::Kind::Tuple && isStringType(E);
  if (!(Str || G->isPtr()) || !comparable(E))
    return ViewGlues[RT] = R;
  FuncId Eq = NoId, Hs = NoId;
  if (Str) {
    Eq = rt(RtFn::StrEq, At);
    Hs = hashGlue(CP.Ctx->typ(S::BasicKind::String), At);
  } else {
    Eq = eqGlue(E, At);
    Hs = hashGlue(E, At);
  }
  if (Eq == NoId || Hs == NoId)
    return ViewGlues[RT] = R;
  Type *PU8 = Ctx.ptr(Ctx.u8()), *U64 = Ctx.u64();
  unsigned N = ViewGlues.size();
  for (int Which = 0; Which < 2; ++Which) {
    auto Fn = std::make_unique<Function>();
    Fn->Name = symbolOf(SymbolKind::Runtime, CP.Pkg,
                        std::string(Which ? "view_hash." : "view_eq.") +
                            std::to_string(N));
    Fn->Sig =
        Which ? Ctx.func({PU8, U64}, U64) : Ctx.func({PU8, PU8}, Ctx.boolean());
    Fn->Link = Linkage::Internal;
    Fn->CC = CallConv::Go;
    Function *F = Fn.get();
    (Which ? R.second : R.first) = M->addFunction(std::move(Fn));
    Rt X(*M, *F);
    // (a and b point to the values: the box, or the data word that holds
    // the pointer.)
    if (Str) {
      auto Ptr = [&](Val P) { return X.load(P, PU8); };
      auto Len = [&](Val P) { return X.load(X.padd(P, X.u(8)), U64); };
      if (!Which) {
        Val A = X.param(0), B = X.param(1);
        X.Return(X.call(Eq, {Ptr(A), Len(A), Ptr(B), Len(B)}));
      } else {
        Val P = X.param(0);
        Val Pp = Ptr(P), Ln = Len(P);
        LocalId SL = X.FB.newTemp(Ctx.string());
        X.FB.assign(X.FB.local(SL),
                    X.FB.aggregate(AggKind::String, Ctx.string(),
                                   {Pp.Op, Ln.Op, X.u(0).Op}));
        X.Return(X.call(Hs, {X.addrOf(X.FB.local(SL)), X.param(1)}));
      }
    } else {
      Type *PG = G; // a pointer to the value shown
      if (!Which)
        X.Return(X.call(Eq, {X.load(X.param(0), PG), X.load(X.param(1), PG)}));
      else
        X.Return(X.call(Hs, {X.load(X.param(0), PG), X.param(1)}));
    }
    X.finish();
  }
  return ViewGlues[RT] = R;
}

uint32_t Lowering::typeInfo(S::Type *T, SourceLoc At) {
  std::string Name = Syms.glue(sym::Kind::TypeInfo, T);
  if (Name.empty())
    Name = runtimeSymbol("typeinfo_" + rtypeName(T));
  auto It = StaticData.find(Name);
  if (It != StaticData.end())
    return It->second;
  Type *G = lowerType(T, At);
  if (!G)
    return ~0u;
  Type *PU8 = Ctx.ptr(Ctx.u8()), *U64 = Ctx.u64(), *U32 = Ctx.intTy(32, false);
  Global Gl;
  Gl.Name = Name;
  Gl.Ty = tiType();
  Gl.Link = Linkage::LinkOnceODR;
  Gl.ReadOnly = true;
  uint32_t Id = M->addGlobal(std::move(Gl));
  StaticData[Name] = Id; // (before the parts: a method may mention T)
  // The method table (a method declined where it was declared, which
  // reported it, has no thunk and is left out).
  const SmallVector<MethodEntry, 4> *Ms = methodSet(T, At);
  if (!Ms)
    return ~0u;
  uint32_t Table = ~0u;
  SmallVector<Constant, 8> TableParts;
  SmallVector<MethodEntry, 4> Kept;
  for (const MethodEntry &E : *Ms) {
    FuncId Th = ifaceThunk(T, E.F, At);
    if (Th == NoId)
      continue;
    Kept.push_back(E);
    TableParts.push_back(partStr(*M, E.Key));
    TableParts.push_back(partFunc(*M, Th));
  }
  if (!Kept.empty()) {
    Global Tb;
    // `_GPTm<type>`: "method table for main.T" (P04.17).
    Tb.Name = Syms.glue(sym::Kind::MethodTable, T);
    if (Tb.Name.empty())
      Tb.Name = runtimeSymbol("methods_" + rtypeName(T));
    Tb.Ty = Ctx.array(mtEntType(), Kept.size());
    Tb.Link = Linkage::LinkOnceODR;
    Tb.ReadOnly = true;
    Tb.Parts.assign(TableParts.begin(), TableParts.end());
    Table = M->addGlobal(std::move(Tb));
  }
  // Equality and hash, if the type has them (a view: those of the value it
  // shows).
  Constant EqC = partNull(PU8), HashC = partNull(PU8);
  if (auto *RT = dyn_cast<S::RefType>(S::under(T))) {
    auto [Eq, Hs] = viewGlue(RT, G, At);
    if (Eq != NoId && Hs != NoId) {
      EqC = partFunc(*M, Eq);
      HashC = partFunc(*M, Hs);
    }
  } else if (comparable(T) && hasGlue(G)) {
    FuncId Eq = eqGlue(T, At), Hs = hashGlue(T, At);
    if (Eq != NoId)
      EqC = partFunc(*M, Eq);
    if (Hs != NoId)
      HashC = partFunc(*M, Hs);
  }
  std::vector<Constant> P;
  P.push_back(partGlobal(*M, Id));
  P.push_back(partInt(U64, layout(G).Size));
  P.push_back(partStr(*M, rtypeShown(T)));
  P.push_back(EqC);
  P.push_back(HashC);
  P.push_back(partInt(U32, reflectKind(T)));
  // The glue a box of the value needs (P04.09): its drop, and its clone for
  // a borrowed interface argument the callee keeps (TYP-7); a type with a
  // `drop` method and no Clone() cannot be kept that way.
  const bool Owns = needsDrop(T);
  const bool CanClone = !Owns || Methods.cloneable(T);
  uint32_t Flags =
      (isInline(T, G) ? TI::FlagInline : 0) | (CanClone ? 0 : TI::FlagNoClone);
  FuncId DropG = Owns ? dropGlue(T, At) : NoId;
  FuncId CloneG = Owns && CanClone ? cloneGlue(T, At) : NoId;
  P.push_back(partInt(U32, Flags));
  P.push_back(partInt(U64, Kept.size()));
  P.push_back(Table == ~0u ? partNull(PU8) : partGlobal(*M, Table));
  P.push_back(partNull(PU8)); // Elem (P05)
  P.push_back(partNull(PU8)); // Fields (P05)
  P.push_back(partInt(U64, 0));
  P.push_back(partNull(PU8)); // VTable (P04.22)
  P.push_back(DropG == NoId ? partNull(PU8) : partFunc(*M, DropG));
  P.push_back(CloneG == NoId ? partNull(PU8) : partFunc(*M, CloneG));
  M->Globals[Id].Parts = std::move(P);
  return Id;
}

uint32_t Lowering::itab(S::Type *Dyn, S::Type *I, SourceLoc At) {
  const IfaceMethods *IM = ifaceMethods(I, At);
  if (!IM)
    return ~0u;
  uint32_t Ti = typeInfo(Dyn, At);
  if (Ti == ~0u)
    return ~0u;
  const SmallVector<MethodEntry, 4> *Ims = &IM->List;
  if (Ims->empty())
    return Ti; // the descriptor is the itab of (T, any)
  const sym::Node *DN = Syms.Names.type(Dyn), *IN = Syms.Names.type(I);
  std::string Name =
      DN && IN ? sym::mangle(Syms.Ctx.itab(DN, IN))
               : runtimeSymbol("itab_" + rtypeName(Dyn) + "_" + rtypeName(I));
  auto It = StaticData.find(Name);
  if (It != StaticData.end())
    return It->second;
  const SmallVector<MethodEntry, 4> *Have = methodSet(Dyn, At);
  if (!Have)
    return ~0u;
  Type *PU8 = Ctx.ptr(Ctx.u8());
  Global G;
  G.Name = Name;
  G.Ty = Ctx.array(PU8, Ims->size() + 1);
  G.Link = Linkage::LinkOnceODR;
  G.ReadOnly = true;
  G.Parts.push_back(partGlobal(*M, Ti));
  for (const MethodEntry &E : *Ims) {
    ++LowerSteps;
    // (Have is sorted by key: a binary search, so an interface of n methods
    // against a type of m is n log m, not n * m.)
    auto H = std::lower_bound(
        Have->begin(), Have->end(), E.Key,
        [](const MethodEntry &X, const std::string &K) { return X.Key < K; });
    if (H == Have->end() || H->Key != E.Key) {
      // (The checker made sure Dyn implements I: an internal error.)
      unsupported(At, "a conversion to " + rtypeName(I) + " of " +
                          rtypeName(Dyn) + ", whose method set lacks " +
                          E.F->Name.str());
      return ~0u;
    }
    FuncId Th = ifaceThunk(Dyn, H->F, At);
    if (Th == NoId) {
      // A method declined where it was declared was reported there.
      if (!Declined.count(H->F))
        unsupported(At, "the method " + H->F->Name.str() + " of " +
                            rtypeName(Dyn) + " in an interface");
      return ~0u;
    }
    G.Parts.push_back(partFunc(*M, Th));
  }
  uint32_t Id = M->addGlobal(std::move(G));
  return StaticData[Name] = Id;
}

//===----------------------------------------------------------------------===//
// Calls and conversions in a body
//===----------------------------------------------------------------------===//

Type *Lowering::methodSig(S::SignatureType *Sig, Type *Lead, SourceLoc At) {
  SmallVector<Type *, 4> Params{Lead};
  for (size_t K = 0; K < Sig->Params->Elems.size(); ++K) {
    Type *T = lowerType(Sig->Params->Elems[K], At);
    if (!T)
      return nullptr;
    Params.push_back(paramType(Sig, K, T));
  }
  Type *Ret = Ctx.unit();
  if (Sig->Results->size() == 1)
    Ret = lowerType(Sig->Results->Elems[0], At);
  else if (Sig->Results->size() > 1)
    Ret = lowerType(Sig->Results, At);
  if (!Ret)
    return nullptr;
  return Ctx.func(Params, Ret);
}

Operand Body::ifaceFrom(Operand Itab, Operand Data) {
  return copyOf(FB.temp(FB.aggregate(AggKind::Struct, ctx().iface(),
                                     {std::move(Itab), std::move(Data)})));
}

Operand Body::globalPtr(uint32_t G) {
  return copyOf(FB.temp(
      FB.cast(CastKind::PtrToPtr, constGlobal(M, G), ctx().ptr(ctx().u8()))));
}

Operand Body::lowerMethodCall(const ast::CallExpr *C,
                              const ast::SelectorExpr *Sel, const S::Func *F,
                              const S::Selection &SL) {
  // (What a refused call gives: the zero of its result, no cascade.)
  auto Unit = [&] { return zeroOfExpr(C); };
  if (SL.ParentSteps || SL.Index.size() > 1) {
    unsupported(C, "a method promoted from a parent type (P04.22)");
    return Unit();
  }
  S::Type *BT = sTyOf(Sel->X);
  if (!BT)
    return failed(Sel->X, Unit());
  S::SignatureType *Sig = sigOf(F);
  Type *PU8 = ctx().ptr(ctx().u8());
  if (!F->IsInterfaceMethod) {
    auto FI = L.FuncIds.find(F);
    if (FI == L.FuncIds.end()) {
      if (!L.Declined.count(F))
        unsupported(C, "a call of " + F->Name.str());
      return Unit();
    }
    bool BasePtr = isa<S::PointerType>(BT);
    const Lowering::Pass RP = L.passOfRecv(F);
    Operand Recv;
    std::optional<LateRecv> Late;
    const size_t RecvCopies = PlaceCopies.size();
    if (F->HasPtrRecv) {
      if (BasePtr) {
        Recv = lowerExpr(Sel->X);
      } else {
        Place P = lowerPlace(Sel->X);
        if (!P.Ty)
          return failed(Sel->X, Unit());
        at(C);
        Recv = copyOf(FB.temp(FB.addrOf(std::move(P))));
      }
    } else if (RP == Lowering::Pass::Place) {
      // A borrowed receiver that holds a string, slice or map is the
      // caller's place (PAR-1, PAR-10), as a borrowed parameter of its type
      // is: through a pointer, the pointer; else placeHow decides, as for
      // an argument (its address taken after the arguments, a copy, or
      // refused).
      if (BasePtr) {
        Recv = lowerExpr(Sel->X);
      } else {
        S::Type *RT = L.recvTypeOf(F);
        if (!RT)
          return failed(C, Unit());
        switch (placeHow(Sel->X, RT, BT == RT)) {
        case PlaceHow::Address:
          Late.emplace();
          Late->E = Sel->X;
          Late->Map = isa<S::MapType>(S::under(RT));
          recordOps(Sel->X, Late->Ops);
          break;
        case PlaceHow::Copy: {
          Operand V = lowerExpr(Sel->X);
          if (!V.type())
            return failed(Sel->X, Unit());
          Recv = addrOfCopy(std::move(V), Sel->X, RT);
          break;
        }
        case PlaceHow::Refuse:
          // A map the method makes in it would be lost with the copy a map
          // entry is (only one of a map type is copied in and out); TODO
          // P04.09 item 6 copies the others in and out.
          unsupported(Sel->X, "a value that holds a map passed from a map "
                              "entry (P04.09)");
          return Unit();
        }
      }
    } else if (BasePtr) {
      Operand P = lowerExpr(Sel->X);
      if (!P.type() || !P.type()->isPtr())
        return failed(Sel->X, Unit());
      at(C);
      Recv = copyOf(FB.deref(FB.local(localOf(std::move(P)))));
    } else {
      Recv = lowerExpr(Sel->X);
    }
    if (!Late && !Recv.type())
      return failed(Sel->X, Unit());
    // A receiver the method owns (P04.09): a `move` receiver takes the value
    // (OWN-R3), a shared handle gets a copy (OWN-C3).
    if (!Late && !F->HasPtrRecv && RP == Lowering::Pass::Value)
      if (S::Type *RT = L.recvTypeOf(F); RT && L.needsDrop(RT)) {
        if (F->MoveRecv)
          Recv = takeValue(std::move(Recv), RT, /*Move=*/true);
        else if (L.isSharedHandle(RT))
          Recv = passValue(std::move(Recv), RT);
      }
    if (RP == Lowering::Pass::View) {
      // A borrowed string or slice receiver passes as (ptr, len), read now,
      // before the arguments (ADR-0029 point 7).
      Type *VT = L.viewTypeOf(Recv.type());
      Place P = placeOf(std::move(Recv));
      at(C);
      Recv = copyOf(FB.temp(
          FB.aggregate(AggKind::Tuple, VT,
                       {copyOf(FB.field(P, 0)), copyOf(FB.field(P, 1))})));
    } else if (!Late && Recv.K != Operand::Const && !C->Args.empty()) {
      at(C);
      Recv = copyOf(FB.temp(FB.use(std::move(Recv))));
    }
    const Function &Real = M.func(FI->second);
    Operand R = callWith(C, Sig, Real.Sig, constFunc(M, FI->second), {Recv},
                         Real.NoUnwind, Late ? &*Late : nullptr);
    freeNewMaps(RecvCopies); // (the receiver's copy, made before)
    return R;
  }
  // Through the itab.
  const Lowering::IfaceMethods *Ms = L.ifaceMethods(BT, C->getBegin());
  if (!Ms)
    return failed(C, Unit());
  const unsigned Slot = L.ifaceSlot(*Ms, F);
  if (Slot == ~0u) {
    unsupported(C, "a call of the interface method " + F->Name.str());
    return Unit();
  }
  Type *FT = L.methodSig(Sig, PU8, C->getBegin());
  if (!FT)
    return failed(C, Unit());
  Operand X = lowerExpr(Sel->X);
  if (!X.type() || !X.type()->isIface())
    return failed(Sel->X, Unit());
  Place XP = placeOf(std::move(X));
  at(C);
  Operand Itab = copyOf(FB.temp(FB.use(copyOf(FB.field(XP, 0)))));
  Operand Data = copyOf(FB.temp(FB.use(copyOf(FB.field(XP, 1)))));
  Rt E(M, FB);
  // A nil interface (an `error`) has no method: Go's nil dereference.
  Val It{&E, Itab};
  check(E.not_(It == E.null(PU8)).Op, AssertKind::Nil, false, C);
  Val Bits = E.to(ctx().u64(), It);
  Val Tab = E.to(PU8, Bits - (Bits & 1));
  Val Fp = E.load(E.padd(Tab, E.u(8 * (uint64_t(Slot) + 1))), PU8);
  LocalId FnL = FB.newTemp(FT);
  FB.assign(FB.local(FnL), FB.cast(CastKind::PtrToPtr, Fp.Op, FT));
  return callWith(C, Sig, FT, copyOf(FB.local(FnL)), {Data}, false);
}

Operand Body::boxAs(Operand V, S::Type *Dyn, S::Type *Want,
                    const ast::Node *N) {
  Type *G = V.type();
  if (!G)
    return failed(N, zeroOf(ctx().iface()));
  uint32_t Tab = L.itab(Dyn, Want, N->getBegin());
  if (Tab == ~0u)
    return failed(N, zeroOf(ctx().iface()));
  Type *PU8 = ctx().ptr(ctx().u8()), *U64 = ctx().u64();
  Rt E(M, FB);
  at(N);
  // Who owns what the interface holds (P04.09). A value that goes to a new
  // owner (a store, a return: TYP-6; ownedValue's conversion), or that is
  // fresh (a call's result, a literal, `move(x)`), moves into a counted box
  // (TYP-5): the box owns it and the last copy of the interface drops it. A
  // value the program still owns, converted for a parameter or a comparison
  // (TYP-7), is lent: a copy in this frame and the borrow tag on the itab
  // word, (itab | 1, &copy), no box; a copy of it that is kept is cloned into
  // a box then (gopp.iface_clone).
  const bool Fresh = V.K == Operand::Move || isFresh(V);
  const bool Owner = Fresh || (OwnedConv && N == OwnedConv);
  Operand Data;
  bool Boxed = false, Lent = false;
  if (L.layout(G).Size == 0) {
    Data = E.null(PU8).Op;
  } else if (L.isInline(Dyn, G)) {
    if (G->isPtr()) {
      Data = E.to(PU8, Val{&E, V}).Op;
    } else {
      // The value in the low bytes of a zeroed word.
      Var W = E.var(U64, E.u(0), "box");
      E.store(E.to(ctx().ptr(G), E.addrOf(W)), G, Val{&E, V});
      Data = E.to(PU8, W).Op;
    }
  } else if (!Owner) {
    LocalId Copy = FB.newTemp(G);
    FB.assign(FB.local(Copy), FB.use(std::move(V)));
    Data = E.to(PU8, E.addrOf(FB.local(Copy))).Op;
    Lent = true;
  } else {
    if (V.K == Operand::Copy && !isFresh(V) && L.needsDrop(Dyn))
      V = cloneValue(std::move(V), Dyn, N); // (the program keeps its own)
    if (V.K == Operand::Copy && isFresh(V))
      V.K = Operand::Move;
    at(N);
    uint64_t Size = L.layout(G).Size;
    Operand Raw = callRt(
        RtFn::MemResize,
        {E.null(PU8).Op, E.u(Size + TI::BoxHeader).Op, constBool(ctx(), false)},
        N);
    E.store(Val{&E, Raw}, U64, E.u(1));
    Operand Box = E.snap(E.padd(Val{&E, Raw}, E.u(TI::BoxHeader))).Op;
    E.store(Val{&E, Box}, G, Val{&E, V});
    Data = Box;
    Boxed = true;
  }
  Operand Itab = globalPtr(Tab);
  if (Lent)
    Itab = E.to(PU8, E.to(U64, Val{&E, Itab}) | 1).Op;
  Operand R = ifaceFrom(std::move(Itab), std::move(Data));
  // The new reference is the statement's until a store or a call takes it
  // (a value in the data word holds none: owned all the same, it is moved
  // on with no retain).
  if (!Lent)
    R = ownTemp(std::move(R), Want);
  (void)Boxed;
  return R;
}

Operand Body::ifaceConvert(Operand X, S::Type *From, S::Type *To,
                           const ast::Node *N) {
  const Lowering::IfaceMethods *Fm = L.ifaceMethods(From, N->getBegin());
  const Lowering::IfaceMethods *Tm = L.ifaceMethods(To, N->getBegin());
  if (!Fm || !Tm)
    return failed(N, zeroOf(ctx().iface()));
  if (L.sameItabs(*Fm, *Tm))
    return X; // one GIR type, one itab layout
  const bool ToAny = Tm->List.empty();
  uint32_t Desc = ToAny ? ~0u : L.ifaceDesc(To, N->getBegin());
  if (!ToAny && Desc == ~0u)
    return failed(N, zeroOf(ctx().iface()));
  Type *PU8 = ctx().ptr(ctx().u8()), *U64 = ctx().u64();
  // A fresh value's reference moves to the converted one (P04.09).
  const bool Fresh = isFresh(X);
  Place XP = placeOf(std::move(X));
  at(N);
  Rt E(M, FB);
  Val It = E.snap(E.val(FB.field(XP, 0)));
  Val Dt = E.snap(E.val(FB.field(XP, 1)));
  if (Fresh)
    FB.assign(XP, FB.use(zeroOf(ctx().iface())));
  Var R = E.var(ctx().iface(), "conv");
  R = E.zero(ctx().iface());
  E.If(It != E.null(PU8), [&] {
    Val Bits = E.to(U64, It);
    Val Tag = E.snap(Bits & 1);
    Val Ti = E.load(E.to(PU8, Bits - Tag), PU8);
    Val NewTab = Ti;
    if (!ToAny)
      NewTab = Val{&E, callRt(RtFn::GetItab, {globalPtr(Desc), Ti.Op}, N)};
    Val Tagged = E.to(PU8, E.to(U64, NewTab) | Tag);
    R = E.tmp(FB.aggregate(AggKind::Struct, ctx().iface(), {Tagged.Op, Dt.Op}));
  });
  if (Fresh)
    return ownTemp(copyOf(FB.local(R.L)), To);
  return copyOf(FB.local(R.L));
}

Operand Body::toIface(const ast::Expr *E, S::Type *Want) {
  S::Type *ES = sTyOf(E);
  if (!ES)
    return failed(E, zeroOf(ctx().iface()));
  // The dynamic type first: one that cannot be lowered (a type
  // with no GIR type yet) is reported once, as the type, and the operand is
  // left alone (lowering it would report it a second time).
  if (!L.lowerType(ES, E->getBegin()))
    return failed(E, zeroOf(ctx().iface()));
  Operand X = lowerExpr(E);
  if (!X.type())
    return failed(E, zeroOf(ctx().iface()));
  return toIfaceValue(std::move(X), ES, Want, E);
}

Operand Body::toIfaceValue(Operand X, S::Type *ES, S::Type *Want,
                           const ast::Node *N) {
  if (S::OptionType *O = asOption(ES)) {
    // An Option[J]: the null itab is the Option's nil, the same words.
    if (L.isInterface(O->Elem)) {
      if (!X.type()->isIface())
        return failed(N, zeroOf(ctx().iface()));
      return ifaceConvert(std::move(X), O->Elem, Want, N);
    }
    // Option[T] to an interface: nil stays nil, else the payload boxed.
    Place P = placeOf(std::move(X));
    Operand Set = optionIsSet(copyOf(P), N);
    Rt R(M, FB);
    Var Res = R.var(ctx().iface(), "opt");
    Res = R.zero(ctx().iface());
    R.If(Val{&R, Set}, [&] {
      Operand Pay = optionPayload(copyOf(P));
      Res = Val{&R, boxAs(Pay, O->Elem, Want, N)};
    });
    return copyOf(FB.local(Res.L));
  }
  if (L.isInterface(ES)) {
    if (!X.type()->isIface())
      return failed(N, zeroOf(ctx().iface()));
    return ifaceConvert(std::move(X), ES, Want, N);
  }
  return boxAs(std::move(X), ES, Want, N);
}

Operand Body::convertResult(Operand V, S::Type *From, S::Type *To,
                            const ast::Node *N) {
  if (!To || From == To || !V.type())
    return V;
  S::OptionType *TO = asOption(To);
  S::Type *ToI = TO ? TO->Elem : To;
  // (A conversion needs the type it converts from: no checker fact for it
  // must not pass the value on unconverted.)
  if (!From && (TO || L.isInterface(ToI)))
    return failed(N, std::move(V));
  if (!From)
    return V;
  if (L.isInterface(ToI))
    return toIfaceValue(std::move(V), From, ToI, N);
  if (TO && !asOption(From))
    return wrapSome(std::move(V), To, N);
  return V;
}

//===----------------------------------------------------------------------===//
// Type assertions and type switches
//===----------------------------------------------------------------------===//

Operand Body::dynTypeInfo(Place IP) {
  Rt E(M, FB);
  Type *PU8 = ctx().ptr(ctx().u8()), *U64 = ctx().u64();
  Val It = E.snap(E.val(FB.field(IP, 0)));
  Var R = E.var(PU8, "dyn");
  R = E.null(PU8);
  E.If(It != E.null(PU8), [&] {
    Val Bits = E.to(U64, It);
    R = E.load(E.to(PU8, Bits - (Bits & 1)), PU8);
  });
  return E.snap(R).Op;
}

Place Body::boxedPlace(Place IP, S::Type *T, Type *G) {
  Rt E(M, FB);
  if (L.layout(G).Size == 0)
    return placeOf(zeroOf(G));
  if (L.isInline(T, G))
    return E.at(E.addrOf(FB.field(IP, 1)), G);
  return E.at(E.snap(E.val(FB.field(IP, 1))), G);
}

Operand Body::typeMatches(Operand Ti, S::Type *T, const ast::Node *N) {
  Rt E(M, FB);
  Type *PU8 = ctx().ptr(ctx().u8());
  Val V{&E, Ti};
  if (L.isInterface(T)) {
    const Lowering::IfaceMethods *Ms = L.ifaceMethods(T, N->getBegin());
    if (!Ms)
      return failed(N, constBool(ctx(), false));
    if (Ms->List.empty())
      return (V != E.null(PU8)).Op;
    uint32_t Desc = L.ifaceDesc(T, N->getBegin());
    if (Desc == ~0u)
      return failed(N, constBool(ctx(), false));
    return E
        .land(V != E.null(PU8),
              [&] {
                return Val{&E, callRt(RtFn::GetItab, {globalPtr(Desc), Ti},
                                      N)} != E.null(PU8);
              })
        .Op;
  }
  uint32_t Id = L.typeInfo(T, N->getBegin());
  if (Id == ~0u)
    return failed(N, constBool(ctx(), false));
  return (V == Val{&E, globalPtr(Id)}).Op;
}

Operand Body::lowerTypeAssert(const ast::TypeAssertExpr *TA, bool CommaOk,
                              S::Type *WantVal) {
  S::Type *XS = sTyOf(TA->X);
  S::Type *T = TA->Type ? sTyOf(TA->Type) : nullptr;
  S::Type *ResS = sTyOf(TA);
  // `s, ok := x.(string)` binds a view of the box (TYP-9): the variable's
  // type says so, the expression's does not.
  if (WantVal && isa<S::RefType>(S::under(WantVal)))
    ResS = WantVal;
  // What a refused assertion gives: the zero of its result, with the flag
  // of a comma-ok one (no cascade into the code that uses it).
  auto Fail = [&] {
    Type *VG = ResS ? L.lowerType(ResS, TA->getBegin()) : nullptr;
    if (!VG)
      return errorAt(TA);
    return failed(TA,
                  zeroOf(CommaOk ? ctx().tuple({VG, ctx().boolean()}) : VG));
  };
  if (!XS || !T || !ResS)
    return Fail();
  if (!L.isInterface(XS)) {
    unsupported(TA, "a downcast of a parent pointer (P04.22)");
    return Fail();
  }
  Type *ResG = L.lowerType(ResS, TA->getBegin());
  Type *TG = L.lowerType(T, TA->getBegin());
  if (!ResG || !TG)
    return Fail();
  Operand X = lowerExpr(TA->X);
  if (!X.type() || !X.type()->isIface())
    return Fail();
  at(TA);
  // (A variable of its own: the address of its data word is taken.)
  LocalId SWL = FB.newLocal(X.type(), Local::Var, "x");
  FB.assign(FB.local(SWL), FB.use(std::move(X)));
  Place SW = FB.local(SWL);
  Operand Ti = dynTypeInfo(SW);
  Operand Ok = typeMatches(Ti, T, TA);
  const bool TIface = L.isInterface(T);
  auto Extract = [&]() -> Operand {
    if (TIface)
      return ifaceConvert(copyOf(SW), XS, T, TA);
    Place PV = boxedPlace(SW, T, TG);
    if (ResG == TG)
      return copyOf(PV);
    return viewOfPlace(std::move(PV), ResS, TA);
  };
  Rt E(M, FB);
  if (CommaOk) {
    Type *Tup = ctx().tuple({ResG, ctx().boolean()});
    Var Res = E.var(Tup, "ta");
    Res = E.zero(Tup);
    E.If(Val{&E, Ok}, [&] {
      Operand V = Extract();
      Res =
          E.tmp(FB.aggregate(AggKind::Tuple, Tup, {V, constBool(ctx(), true)}));
    });
    return copyOf(FB.local(Res.L));
  }
  Type *PU8 = ctx().ptr(ctx().u8());
  E.If(E.not_(Val{&E, Ok}), [&] {
    auto Text = [&](const std::string &S) {
      return copyOf(FB.temp(FB.use(constStr(M, S))));
    };
    Operand Desc = E.null(PU8).Op;
    const Lowering::IfaceMethods *TM =
        TIface ? L.ifaceMethods(T, TA->getBegin()) : nullptr;
    if (TM && !TM->List.empty()) {
      uint32_t D = L.ifaceDesc(T, TA->getBegin());
      if (D != ~0u)
        Desc = globalPtr(D);
    }
    std::string Inter = TIface ? "interface" : L.rtypeShown(XS);
    callFn(L.rt(RtFn::PanicAssert, TA->getBegin()),
           {Text(Inter), Ti, Text(L.rtypeShown(T)), Desc});
    FB.unreachable();
    FB.setBlock(FB.newBlock());
  });
  return Extract();
}

void Body::lowerTypeSwitch(const ast::TypeSwitchStmt *TS, std::string Label) {
  pushScope();
  if (TS->Init)
    lowerStmt(TS->Init, /*KeepTemps=*/true);
  const ast::TypeAssertExpr *Guard = nullptr;
  if (auto *AS = dyn_cast<ast::AssignStmt>(TS->Assign))
    Guard = AS->Rhs.size() == 1 ? dyn_cast<ast::TypeAssertExpr>(AS->Rhs[0])
                                : nullptr;
  else if (auto *ES = dyn_cast<ast::ExprStmt>(TS->Assign))
    Guard = dyn_cast<ast::TypeAssertExpr>(unparen(ES->X));
  S::Type *XS = Guard ? sTyOf(Guard->X) : nullptr;
  if (!Guard || !XS) {
    failed(TS, Operand());
    popScope();
    return;
  }
  if (!L.isInterface(XS)) {
    unsupported(TS, "a type switch on a parent pointer (P04.22)");
    popScope();
    return;
  }
  Operand X = lowerExpr(Guard->X);
  if (!X.type() || !X.type()->isIface()) {
    failed(Guard->X, Operand());
    popScope();
    return;
  }
  at(TS);
  LocalId SWL = FB.newLocal(X.type(), Local::Var, "x");
  FB.assign(FB.local(SWL), FB.use(std::move(X)));
  Place SW = FB.local(SWL);
  Operand Ti = dynTypeInfo(SW);
  Operand TiLocal = copyOf(FB.local(localOf(std::move(Ti))));
  ArrayRef<ast::Stmt *> Clauses = TS->Body->List;
  SmallVector<BlockId, 8> Bodies;
  BlockId Default = NoId;
  for (size_t K = 0; K < Clauses.size(); ++K) {
    Bodies.push_back(FB.newBlock());
    if (cast<ast::CaseClause>(Clauses[K])->IsDefault)
      Default = Bodies.back();
  }
  BlockId Exit = FB.newBlock();
  Type *PU8 = ctx().ptr(ctx().u8());
  // The tests, in source order.
  for (size_t K = 0; K < Clauses.size(); ++K) {
    auto *CC = cast<ast::CaseClause>(Clauses[K]);
    for (const ast::Expr *Ex : CC->List) {
      ++LowerSteps;
      at(Ex);
      Operand Hit;
      if (isNilValue(Ex)) {
        Rt E(M, FB);
        Hit = (Val{&E, TiLocal} == E.null(PU8)).Op;
      } else {
        S::Type *CT = sTyOf(Ex);
        if (!CT) {
          failed(Ex, Operand()); // (a case left out would be wrong)
          continue;
        }
        Hit = typeMatches(TiLocal, CT, Ex);
      }
      BlockId Next = FB.newBlock();
      FB.branch(Hit, Bodies[K], Next);
      FB.setBlock(Next);
    }
  }
  FB.gotoBlock(Default != NoId ? Default : Exit);
  Targets.push_back({Exit, NoId, Scopes.size(), std::move(Label)});
  for (size_t K = 0; K < Clauses.size(); ++K) {
    auto *CC = cast<ast::CaseClause>(Clauses[K]);
    FB.setBlock(Bodies[K]);
    pushScope(true, CC);
    auto Imp = I.Implicits.find(CC);
    if (Imp != I.Implicits.end()) {
      if (auto *V = dyn_cast_or_null<S::Var>(Imp->second)) {
        // (At an instance the variable's type is the instance's: `case T`
        // binds a T, P04.08.)
        S::Type *VT = tyOfObj(V);
        Type *VG = L.lowerType(VT, CC->getBegin());
        if (VG) {
          at(CC);
          Operand Val_;
          S::Type *CT = CC->List.size() == 1 && !isNilValue(CC->List[0])
                            ? sTyOf(CC->List[0])
                            : nullptr;
          if (!CT) {
            Val_ = copyOf(SW);
          } else if (L.isInterface(CT)) {
            Val_ = ifaceConvert(copyOf(SW), XS, CT, CC);
          } else if (Type *CG = L.lowerType(CT, CC->getBegin())) {
            Place PV = boxedPlace(SW, CT, CG);
            Val_ = VG == CG ? copyOf(PV) : viewOfPlace(std::move(PV), VT, CC);
          } else {
            Val_ = failed(CC, zeroOf(VG));
          }
          // (A box binding a closure that outlives the call holds:
          // ownBoxCopy, review R5.)
          Val_ = ownBoxCopy(std::move(Val_), V, CC);
          // A binding that is a copy (a shared handle, TYP-9) owns it: a
          // retain (P04.09). A borrowed one is a view of the box.
          if (modeOf(V) == S::Var::Owned)
            Val_ = takeValue(std::move(Val_), VT);
          VarSlot Sl = declareSlot(V, CC, V->Name.str(), VG);
          FB.assign(Sl.P, FB.use(Val_));
        }
      }
    }
    for (const ast::Stmt *St : CC->Body)
      lowerStmt(St);
    popScope();
    if (!FB.terminated())
      FB.gotoBlock(Exit);
  }
  Targets.pop_back();
  FB.setBlock(Exit);
  popScope();
}
