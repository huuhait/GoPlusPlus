//===- Lower.cpp - The checked AST to GIR: module level -------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The module: checker types to GIR types (lowerType, the one place), function
// and variable declarations, the package's init function in the checker's
// initialization order, and the C entry point. Bodies are in LowerExpr.cpp and
// LowerStmt.cpp; the helper functions the compiler writes are in Runtime.cpp.
// See LowerImpl.h for the list of decision points.
//
// The builder recurses once per level of the checked AST; the parser bounds
// that (ParseOptions::MaxHeight), and lowerPackage runs the builder on a
// thread with a stack sized for the deepest legal tree (runOnBigStack).
//
//===----------------------------------------------------------------------===//

#include "LowerImpl.h"

#include "gopp/GIR/Verify.h"

#include "llvm/Support/raw_ostream.h"

#include <algorithm>
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
// Reports
//===----------------------------------------------------------------------===//

void Lowering::unsupported(SourceLoc At, const llvm::Twine &What) {
  Failed = true;
  ++Reported;
  Diags.report(At, diag::err_gir_unsupported) << What.str();
}

void Lowering::badExtern(SourceLoc At, StringRef Name, const llvm::Twine &Why) {
  Failed = true;
  ++Reported;
  Diags.report(At, diag::err_gir_extern_bad) << Name.str() << Why.str();
}

Span Lowering::spanOf(SourceLoc L) {
  Span Sp;
  if (!L.isValid())
    return Sp;
  PresumedLoc P = SM.getPresumedLoc(L);
  if (!P.isValid())
    return Sp;
  Sp.File = M->addFile(P.Filename) + 1;
  Sp.Line = P.Line;
  Sp.Col = P.Column;
  return Sp;
}

//===----------------------------------------------------------------------===//
// Types: the one place a checker type becomes a GIR type
//===----------------------------------------------------------------------===//

/// What a type that is not lowered yet is, and the task that adds it.
static const char *kindName(S::Type::Kind K) {
  switch (K) {
  case S::Type::Kind::Basic:
    return "this basic type";
  case S::Type::Kind::Pointer:
    return "pointer";
  case S::Type::Kind::Slice:
    return "a slice (P04.05)";
  case S::Type::Kind::Array:
    return "array";
  case S::Type::Kind::Map:
    return "a map (P04.05)";
  case S::Type::Kind::Chan:
    return "a channel (P12)";
  case S::Type::Kind::Ref:
    return "a view (P04.05)";
  case S::Type::Kind::Tuple:
    return "tuple";
  case S::Type::Kind::Signature:
    return "a function value (P04.07)";
  case S::Type::Kind::Struct:
    return "struct";
  case S::Type::Kind::Interface:
    return "an interface (P04.06)";
  case S::Type::Kind::Union:
    return "a union";
  case S::Type::Kind::Named:
    return "named type";
  case S::Type::Kind::TypeParam:
    return "a type parameter (P04.08)";
  case S::Type::Kind::Option:
    return "Option";
  }
  return "type";
}

bool Lowering::isComplexType(S::Type *T) {
  auto *B = dyn_cast<S::BasicType>(S::under(T));
  return B && (B->BK == S::BasicKind::Complex64 ||
               B->BK == S::BasicKind::Complex128 ||
               B->BK == S::BasicKind::UntypedComplex);
}

bool Lowering::isRawPointer(S::Type *T) {
  if (auto *N = dyn_cast<S::NamedType>(T))
    if (N->Ptr != S::PtrKind::None)
      return false;
  return isa_and_nonnull<S::PointerType>(S::under(T));
}

Type *Lowering::lowerType(S::Type *T, SourceLoc At) {
  if (!T) {
    unsupported(At, "an expression without a type");
    return nullptr;
  }
  // A type that cannot be lowered is reported once, at its first use in
  // source order; later uses get null without a second report. A type whose
  // lowering is under way is told apart from one that failed: a cycle that
  // reaches a defined type again through no struct (`type A [1]*A`,
  // `type O Option[*O]`) has no GIR type that can name itself (the struct is
  // the one nominal type), so it is refused here, once, naming the type.
  auto [It, New] = TypeCache.try_emplace(T, nullptr);
  if (!New) {
    auto Under = InProgress.find(T);
    if (It->second || Under == InProgress.end())
      return It->second;
    // T is under way and reached again. If a struct began after T did, the
    // cycle goes through it: its GIR struct exists (made before its body), so
    // T is lowered again right here and ends at that struct. Otherwise
    // nothing can name the type being made.
    if (StructsActive > Under->second)
      return lowerTypeOnce(T, At);
    if (RecursiveReported.insert(T).second)
      unsupported(At,
                  "the type " + S::typeString(T) +
                      ", which refers to itself through no struct (P04.05),");
    return nullptr;
  }
  Type *R = nullptr;
  unsigned Before = Reported;
  InProgress[T] = StructsActive;
  if (!TypeWalk.level([&] { R = lowerTypeOnce(T, At); })) {
    // No stack for the rest of a chain of types this deep: nothing was
    // lowered here. Said once; the lowering goes on without the type (null,
    // as for any type that cannot be lowered) and gives no module.
    if (!NoStack) {
      NoStack = true;
      Failed = true;
      ++Reported;
      Diags.report(At, diag::err_no_stack)
          << std::to_string(TypeWalk.failedAt());
    }
  }
  InProgress.erase(T);
  // Never a null without a report: a function whose type is null is left out
  // of the module, and nothing may leave one out silently.
  if (!R && Reported == Before && !Failed)
    unsupported(At, "the type " + S::typeString(T));
  TypeCache[T] = R;
  return R;
}

Type *Lowering::lowerOption(S::OptionType *O, SourceLoc At) {
  // D4.5: the nil bit pattern of a pointer kind is nil; any other T gets a
  // flag byte after it. Interfaces and funcs (P04.06, P04.07) reach lowerType
  // below and are refused there.
  if (O->usesNiche()) {
    if (!isRawPointer(O->Elem)) {
      unsupported(At, "an Option of a Unique, Shared or Weak pointer "
                      "(P05, P04.09)");
      return nullptr;
    }
    return lowerType(O->Elem, At);
  }
  Type *E = lowerType(O->Elem, At);
  return E ? Ctx.option(E) : nullptr;
}

Type *Lowering::lowerStruct(S::Type *Whole, S::StructType *ST,
                            S::NamedType *Named, SourceLoc At) {
  if (auto It = StructCache.find(ST); It != StructCache.end())
    return It->second;
  // A defined type and its underlying struct are one GIR struct, so a
  // conversion between two defined types of one struct is no code. The name
  // is the readable symbol of the first type that reaches it.
  std::string Name;
  if (Named) {
    if (const sym::Node *N = Syms.Names.type(Named))
      Name = sym::print(N).Text;
    else
      Name = (Named->Obj->Pkg ? Named->Obj->Pkg->Path.str() + "." : "") +
             Named->Obj->Name.str();
  } else {
    Name = "struct." + std::to_string(AnonStructs++);
  }
  if (!StructNames.insert(Name).second) {
    unsigned K = 2;
    while (!StructNames.insert(Name + "#" + std::to_string(K)).second)
      ++K;
    Name += "#" + std::to_string(K);
  }
  Type *Str = Ctx.structTy(Name);
  StructCache[ST] = Str;
  ++StructsActive;
  struct Done {
    unsigned &N;
    ~Done() { --N; }
  } StructDone{StructsActive};
  TypeCache[Whole] = Str; // a field may point back to it
  SmallVector<Type *, 4> Fields;
  for (S::Var *F : ST->Fields) {
    if (F->IsParent) {
      unsupported(At, "a type that extends another (P04.22)");
      return nullptr;
    }
    Type *FT = lowerType(F->Ty, At);
    if (!FT)
      return nullptr;
    Fields.push_back(FT);
  }
  Ctx.setStructBody(Str, Fields);
  return Str;
}

Type *Lowering::lowerTypeOnce(S::Type *T, SourceLoc At) {
  Type *R = nullptr;
  unsigned PB = Opts.PointerBits;
  switch (T->getKind()) {
  case S::Type::Kind::Basic: {
    auto *B = cast<S::BasicType>(T);
    switch (B->BK) {
    case S::BasicKind::Bool:
    case S::BasicKind::UntypedBool:
      R = Ctx.boolean();
      break;
    case S::BasicKind::Int:
    case S::BasicKind::UntypedInt:
      R = Ctx.intTy(PB, true);
      break;
    case S::BasicKind::Int8:
      R = Ctx.intTy(8, true);
      break;
    case S::BasicKind::Int16:
      R = Ctx.intTy(16, true);
      break;
    case S::BasicKind::Int32:
    case S::BasicKind::UntypedRune:
      R = Ctx.intTy(32, true);
      break;
    case S::BasicKind::Int64:
      R = Ctx.intTy(64, true);
      break;
    case S::BasicKind::Uint:
    case S::BasicKind::Uintptr:
      R = Ctx.intTy(PB, false);
      break;
    case S::BasicKind::Uint8:
      R = Ctx.intTy(8, false);
      break;
    case S::BasicKind::Uint16:
      R = Ctx.intTy(16, false);
      break;
    case S::BasicKind::Uint32:
      R = Ctx.intTy(32, false);
      break;
    case S::BasicKind::Uint64:
      R = Ctx.intTy(64, false);
      break;
    case S::BasicKind::Float32:
      R = Ctx.floatTy(32);
      break;
    case S::BasicKind::Float64:
    case S::BasicKind::UntypedFloat:
      R = Ctx.floatTy(64);
      break;
    case S::BasicKind::Complex64:
      R = Ctx.tuple({Ctx.floatTy(32), Ctx.floatTy(32)});
      break;
    case S::BasicKind::Complex128:
    case S::BasicKind::UntypedComplex:
      R = Ctx.tuple({Ctx.floatTy(64), Ctx.floatTy(64)});
      break;
    case S::BasicKind::String:
    case S::BasicKind::UntypedString:
      R = Ctx.string();
      break;
    case S::BasicKind::UnsafePointer:
      R = Ctx.ptr(Ctx.u8());
      break;
    default:
      unsupported(At, std::string("the type ") + B->Name);
      return nullptr;
    }
    break;
  }
  case S::Type::Kind::Pointer: {
    Type *E = lowerType(cast<S::PointerType>(T)->Elem, At);
    if (!E)
      return nullptr;
    R = Ctx.ptr(E);
    break;
  }
  case S::Type::Kind::Array: {
    auto *A = cast<S::ArrayType>(T);
    Type *E = lowerType(A->Elem, At);
    if (!E)
      return nullptr;
    R = Ctx.array(E, A->Len < 0 ? 0 : A->Len);
    break;
  }
  case S::Type::Kind::Struct:
    return lowerStruct(T, cast<S::StructType>(T), nullptr, At);
  case S::Type::Kind::Option:
    return lowerOption(cast<S::OptionType>(T), At);
  case S::Type::Kind::Tuple: {
    SmallVector<Type *, 4> Elems;
    for (S::Type *E : cast<S::TupleType>(T)->Elems) {
      Type *L = lowerType(E, At);
      if (!L)
        return nullptr;
      Elems.push_back(L);
    }
    R = Ctx.tuple(Elems);
    break;
  }
  case S::Type::Kind::Named: {
    auto *N = cast<S::NamedType>(T);
    if (N->Ptr != S::PtrKind::None) {
      unsupported(At, "Unique, Shared and Weak pointers (P05, P04.09)");
      return nullptr;
    }
    if (N->Orig || !N->TParams.empty()) {
      unsupported(At, "a generic type (P04.08)");
      return nullptr;
    }
    S::Type *U = S::under(N);
    if (!U) {
      unsupported(At, "the defined type " + N->Obj->Name.str());
      return nullptr;
    }
    if (auto *ST = dyn_cast<S::StructType>(U))
      return lowerStruct(T, ST, N, At);
    R = lowerType(U, At);
    break;
  }
  default:
    unsupported(At, kindName(T->getKind()));
    return nullptr;
  }
  return R;
}

void Lowering::declareLocalType(const S::TypeName *TN, const sym::Node *Scope,
                                unsigned Ordinal) {
  Syms.Names.declareLocalType(TN, Scope, Ordinal);
}

//===----------------------------------------------------------------------===//
// Functions of the module
//===----------------------------------------------------------------------===//

static const ast::Attribute *findAttr(const ast::FuncDecl *FD, StringRef Name) {
  for (const ast::Attribute *A : FD->Attrs)
    if (!A->Pkg && A->Name && A->Name->Name.str() == Name)
      return A;
  return nullptr;
}

static bool hasAttr(const ast::FuncDecl *FD, StringRef Name) {
  return findAttr(FD, Name) != nullptr;
}

/// The @Extern attribute of FD, or null.
static const ast::Attribute *externAttr(const ast::FuncDecl *FD) {
  return findAttr(FD, "Extern");
}

/// Why an @Extern attribute is not `@Extern("C")` on a function without a
/// body; empty if it is.
static std::string externProblem(const ast::FuncDecl *FD,
                                 const ast::Attribute *A,
                                 const SourceManager &SM) {
  if (!A->HasArgs || A->Args.size() != 1 || A->Args[0].Kind != tok::string_lit)
    return "write @Extern(\"C\")";
  StringRef Buf = SM.getBuffer(SM.getFileID(A->Args[0].Loc));
  std::string ABI;
  if (!sema::unquoteString(
          Buf.substr(SM.getFileOffset(A->Args[0].Loc), A->Args[0].Length),
          ABI) ||
      ABI != "C")
    return "only the \"C\" ABI exists";
  if (FD->Body)
    return "a C function has no body";
  return "";
}

static bool isInitDecl(const ast::FuncDecl *FD) {
  return !FD->Recv && FD->Name->Name.str() == "init";
}

void Lowering::declareFunction(const ast::FuncDecl *FD) {
  StringRef Name = FD->Name->Name.str();
  auto D = I.Defs.find(FD->Name);
  auto *Obj =
      D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::Func>(D->second);
  if (FD->Recv) {
    unsupported(FD->getBegin(), "a method (P04.06)");
    return;
  }
  if (Name == "_")
    return;
  if (hasAttr(FD, "Export")) {
    // C calling Go++ needs a thunk with the C ABI (P04.18). Declined: a call
    // of the function from Go++ is not reported again.
    if (Obj)
      Declined.insert(Obj);
    unsupported(FD->getBegin(), "an @Export function (P04.18)");
    return;
  }
  if (isInitDecl(FD)) {
    // Run before main.main, in source order after the sorted files.
    if (!FD->Body) {
      unsupported(FD->getBegin(), "an init function without a body");
      return;
    }
    unsigned Ordinal = InitDecls.size();
    InitDecls.push_back(FD);
    auto F = std::make_unique<Function>();
    F->Name = symbolOf(SymbolKind::InitFunc, CP.Pkg, "", Ordinal);
    F->Sig = Ctx.func({}, Ctx.unit());
    F->Link = Linkage::Internal;
    F->CC = CallConv::Go;
    F->Loc = spanOf(FD->getBegin());
    InitIds[FD] = M->addFunction(std::move(F));
    return;
  }
  if (!Obj)
    return;
  // Every return below that is not the last one has reported an error.
  Declined.insert(Obj);
  const ast::Attribute *Ext = externAttr(FD);
  if (Ext) {
    std::string Why = externProblem(FD, Ext, SM);
    if (!Why.empty()) {
      badExtern(Ext->getBegin(), Name, Why);
      return;
    }
  } else if (!FD->Body) {
    unsupported(
        FD->getBegin(),
        "a function without a body (a C function needs @Extern(\"C\"))");
    return;
  }
  if (FD->Type->TypeParams && FD->Type->TypeParams->numFields()) {
    unsupported(FD->getBegin(), "a generic function (P04.08)");
    return;
  }
  S::SignatureType *Sig = Obj->sig();
  if (Sig->Fails) {
    unsupported(FD->getBegin(), "a function that fails with ! (P04.10)");
    return;
  }
  if (Sig->Variadic) {
    unsupported(FD->getBegin(), "a variadic function (P04.05)");
    return;
  }
  // C calling Go++ (a callback, an exported function) needs thunks with the C
  // ABI: P04.18. Said here, before the generic "function value" message.
  if (Ext) {
    auto IsFunc = [](S::Type *T) {
      return isa_and_nonnull<S::SignatureType>(S::under(T));
    };
    for (S::Type *P : Sig->Params->Elems)
      if (IsFunc(P)) {
        unsupported(FD->getBegin(),
                    "a function passed to C: a callback (P04.18)");
        return;
      }
    if (Sig->Results->size() == 1 && IsFunc(Sig->Results->Elems[0])) {
      unsupported(FD->getBegin(), "a function returned from C (P04.18)");
      return;
    }
  }
  SmallVector<Type *, 4> Params;
  for (S::Type *P : Sig->Params->Elems) {
    Type *T = lowerType(P, FD->getBegin());
    if (!T)
      return;
    Params.push_back(T);
  }
  Type *Ret = Ctx.unit();
  if (Sig->Results->size() == 1)
    Ret = lowerType(Sig->Results->Elems[0], FD->getBegin());
  else if (Sig->Results->size() > 1)
    Ret = lowerType(Sig->Results, FD->getBegin());
  if (!Ret)
    return;
  if (Ext) {
    // The types C can take by value (P04.11): gir::CTypeRules decides.
    auto Refuse = [&](Type *T, gir::CUse Use, const char *What) {
      gir::CVerdict V = CRules.check(T, Use);
      // Several results and one complex result are both Tuple{f, f}; here
      // the source tells them apart.
      if (Use == gir::CUse::Result && T->getKind() == Type::Kind::Tuple &&
          !V.ok())
        V.Why = Sig->Results->size() > 1
                    ? "a C function has one result (return a struct)"
                    : "complex numbers have no C mapping yet (P07)";
      if (V.ok())
        return false;
      if (V.K == gir::CVerdict::Unsupported)
        unsupported(FD->getBegin(), V.Why);
      else
        badExtern(FD->getBegin(), Name,
                  std::string(What) + " type " + T->str() +
                      " cannot cross the C ABI: " + V.Why);
      return true;
    };
    for (Type *T : Params)
      if (Refuse(T, gir::CUse::Param, "parameter"))
        return;
    if (Refuse(Ret, gir::CUse::Result, "result"))
      return;
  }
  auto F = std::make_unique<Function>();
  if (Ext) {
    F->Name = symbolOf(SymbolKind::ExternC, CP.Pkg, Name);
  } else {
    F->Name = Syms.func(Obj);
    if (F->Name.empty())
      F->Name = symbolOf(SymbolKind::Func, CP.Pkg, Name);
  }
  if (Ext && F->Name == symbolOf(SymbolKind::Entry, CP.Pkg, "")) {
    badExtern(Ext->getBegin(), Name,
              "the C symbol " + F->Name + " is the program's entry point");
    return;
  }
  if (M->lookupFunction(F->Name) != NoId) {
    unsupported(FD->getBegin(), "a second function named " + Name.str());
    return;
  }
  F->Sig = Ctx.func(Params, Ret);
  F->CC = Ext ? CallConv::C : CallConv::Go;
  F->NoUnwind = Ext;
  F->Loc = spanOf(FD->getBegin());
  FuncIds[Obj] = M->addFunction(std::move(F));
  Declined.erase(Obj);
}

void Lowering::lowerBody(const ast::FuncDecl *FD) {
  if (!FD->Body)
    return;
  const S::Func *Obj = nullptr;
  FuncId Id = NoId;
  unsigned Ordinal = 0;
  if (isInitDecl(FD)) {
    auto It = InitIds.find(FD);
    if (It == InitIds.end())
      return;
    Id = It->second;
    Ordinal =
        std::find(InitDecls.begin(), InitDecls.end(), FD) - InitDecls.begin();
  } else {
    auto D = I.Defs.find(FD->Name);
    Obj = D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::Func>(D->second);
    if (!Obj)
      return;
    auto It = FuncIds.find(Obj);
    if (It == FuncIds.end())
      return;
    Id = It->second;
  }
  Function &F = M->func(Id);
  Body B(*this, F, Obj);
  // The scope of the types declared in this body (their symbols name it).
  B.ScopeNode = Obj ? Syms.funcNode(Obj)
                    : Syms.Ctx.initFunc(Syms.Names.pathOf(CP.Pkg), Ordinal);
  B.lower(FD);
}

//===----------------------------------------------------------------------===//
// Package variables, init order
//===----------------------------------------------------------------------===//

bool Lowering::constantOf(const S::ConstValue &V, Type *Ty, Constant &Out) {
  Out.Ty = Ty;
  switch (Ty->getKind()) {
  case Type::Kind::Bool:
    if (V.kind() != S::ConstValue::Bool)
      return false;
    Out.K = Constant::Bool;
    Out.Bits = V.boolVal();
    return true;
  case Type::Kind::String:
    if (V.kind() != S::ConstValue::String)
      return false;
    Out.K = Constant::Str;
    Out.Bits = M->internString(V.stringVal());
    return true;
  case Type::Kind::Int: {
    S::ConstValue IV = V.toInt();
    if (IV.isUnknown())
      return false;
    Out.K = Constant::Int;
    uint64_t Bits = IV.intVal().sextOrTrunc(64).getZExtValue();
    if (Ty->bits() < 64)
      Bits &= (uint64_t(1) << Ty->bits()) - 1;
    Out.Bits = Bits;
    return true;
  }
  case Type::Kind::Float: {
    S::ConstValue FV = V.toFloat();
    if (FV.isUnknown())
      return false;
    Out.K = Constant::Float;
    double D =
        Ty->bits() == 32 ? static_cast<double>(FV.float32()) : FV.float64();
    std::memcpy(&Out.Bits, &D, sizeof D);
    return true;
  }
  default:
    return false;
  }
}

void Lowering::declareGlobals() {
  for (const ast::File *File : Files)
    for (const ast::Decl *D : File->Decls) {
      auto *GD = dyn_cast<ast::GenDecl>(D);
      if (!GD || GD->Keyword != tok::kw_var || GD->IsUnit)
        continue;
      for (const ast::Spec *Sp : GD->Specs) {
        auto *VS = cast<ast::ValueSpec>(Sp);
        for (size_t K = 0; K < VS->Names.size(); ++K) {
          const ast::Ident *Id = VS->Names[K];
          if (Id->Name.str() == "_")
            continue;
          auto Def = I.Defs.find(Id);
          auto *V = Def == I.Defs.end() ? nullptr
                                        : dyn_cast_or_null<S::Var>(Def->second);
          if (!V)
            continue;
          Type *Ty = lowerType(V->Ty, Id->getBegin());
          if (!Ty)
            continue;
          Global G;
          G.Name = Syms.var(V);
          if (G.Name.empty())
            G.Name = symbolOf(SymbolKind::Var, CP.Pkg, V->Name);
          G.Ty = Ty;
          G.Loc = spanOf(Id->getBegin());
          // A constant initializer is static data (a constant string is
          // read-only data); anything else is set by the init function.
          if (VS->Values.size() == VS->Names.size()) {
            const S::TypeAndValue *TV = I.lookup(VS->Values[K]);
            Constant C;
            if (TV && TV->Val && TV->M == S::Mode::Constant &&
                constantOf(*TV->Val, Ty, C)) {
              G.Init = C;
              StaticInit.insert(V);
            }
          }
          Globals[V] = M->addGlobal(std::move(G));
        }
      }
    }
}

FuncId Lowering::lowerPackageInit() {
  SmallVector<S::InitEntry, 8> Order;
  for (const S::InitEntry &E : CP.InitOrder) {
    bool Dynamic = false;
    for (S::Var *V : E.Lhs)
      if (!StaticInit.count(V))
        Dynamic = true;
    if (Dynamic)
      Order.push_back(E);
  }
  SmallVector<FuncId, 4> Inits;
  for (const ast::FuncDecl *FD : InitDecls)
    Inits.push_back(InitIds[FD]);
  if (Order.empty() && Inits.empty())
    return NoId;
  auto F = std::make_unique<Function>();
  F->Name = Syms.pkgInit(CP.Pkg);
  F->Sig = Ctx.func({}, Ctx.unit());
  F->Link = Linkage::Internal;
  F->CC = CallConv::Go;
  Function &Fn = *F;
  FuncId Id = M->addFunction(std::move(F));
  Body B(*this, Fn, nullptr);
  B.ScopeNode = Syms.Ctx.pkgInit(Syms.Names.pathOf(CP.Pkg));
  B.lowerInit(Order, Inits);
  return Id;
}

void Lowering::addEntry(const ast::FuncDecl *Main, FuncId PkgInit) {
  auto D = I.Defs.find(Main->Name);
  auto *Obj =
      D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::Func>(D->second);
  auto It = Obj ? FuncIds.find(Obj) : FuncIds.end();
  if (It == FuncIds.end())
    return;
  auto F = std::make_unique<Function>();
  F->Name = symbolOf(SymbolKind::Entry, CP.Pkg, "");
  F->Sig = Ctx.func({}, Ctx.i32());
  F->CC = CallConv::C;
  F->Loc = M->func(It->second).Loc;
  Function &Fn = *F;
  M->addFunction(std::move(F));
  FuncBuilder FB(*M, Fn);
  FB.setSpan(Fn.Loc);
  if (PkgInit != NoId)
    FB.call(constFunc(*M, PkgInit), {}, FB.local(FB.newTemp(Ctx.unit())),
            {UnwindAction::Continue, NoId});
  FB.call(constFunc(*M, It->second), {}, FB.local(FB.newTemp(Ctx.unit())),
          {UnwindAction::Continue, NoId});
  FB.assign(FB.local(FB.returnPlace()), FB.use(constInt(Ctx.i32(), 0)));
  FB.ret();
}

std::unique_ptr<Module> Lowering::run() {
  const ast::FuncDecl *Main = nullptr;
  // The files in the order the checker was given them (a directory's are
  // sorted by name, the command line's are as written): the init functions
  // run in this order, as the variables' declaration order does (spec,
  // "Package initialization"; `go run c.go a.go` runs c's first).
  for (const ast::File *File : Files) {
    for (const ast::Decl *D : File->Decls) {
      if (auto *FD = dyn_cast<ast::FuncDecl>(D)) {
        declareFunction(FD);
        if (!FD->Recv && FD->Name->Name.str() == "main")
          Main = FD;
      } else if (auto *GD = dyn_cast<ast::GenDecl>(D)) {
        // Constants are folded by the checker; types are lowered where they
        // are used (lowerType); variables are declared below.
        if (GD->Keyword == tok::kw_import || GD->Keyword == tok::kw_type ||
            GD->Keyword == tok::kw_var ||
            (GD->Keyword == tok::kw_const && !GD->IsUnit))
          continue;
        unsupported(GD->getBegin(), "this package-level declaration");
      } else {
        unsupported(D->getBegin(), "this declaration");
      }
    }
  }
  declareGlobals();
  // A function that was declined has had its error reported: none leaves the
  // module without one (a bug in a step that returned null silently).
  if (!Declined.empty() && !Failed)
    unsupported(Files.empty() ? SourceLoc() : Files[0]->PackageLoc,
                "a function that could not be lowered");
  for (const ast::File *File : Files)
    for (const ast::Decl *D : File->Decls)
      if (auto *FD = dyn_cast<ast::FuncDecl>(D))
        lowerBody(FD);
  FuncId PkgInit = Failed ? NoId : lowerPackageInit();
  if (Opts.Entry && CP.Pkg && CP.Pkg->Name == "main") {
    if (Main) {
      addEntry(Main, PkgInit);
    } else {
      Failed = true;
      Diags.report(Files.empty() ? SourceLoc() : Files[0]->PackageLoc,
                   diag::err_gir_no_main);
    }
  }
  if (Failed)
    return nullptr;
  std::vector<VerifyError> Errs = verify(*M, 5);
  for (const VerifyError &E : Errs)
    Diags.report(SourceLoc(), diag::err_gir_verify) << E.str();
  if (!Errs.empty())
    return nullptr;
  return std::move(M);
}

/// The builder recurses once per level of the checked AST, which the parser
/// lets be ParseOptions::MaxHeight (1024) levels tall. In a sanitizer build a
/// level costs up to 7.1 KB of stack (measured at the parser's limit: a
/// left-deep `x + x + ...` or `b && b && ...` chain needs 7.1 MB, the other
/// forms less), so the builder runs on a thread with four times that, as the
/// checker does (P03.26). Only the pages that are touched use memory.
constexpr size_t LowerStackBytes = 32u << 20;

std::unique_ptr<Module> gir::lowerPackage(Context &Ctx, const SourceManager &SM,
                                          const sema::CheckedPackage &CP,
                                          ArrayRef<const ast::File *> Files,
                                          DiagnosticsEngine &Diags,
                                          const LowerOptions &Opts) {
  std::unique_ptr<Module> M;
  runOnNewStackOrHere(
      [&] { M = Lowering(Ctx, SM, CP, Files, Diags, Opts).run(); },
      LowerStackBytes);
  return M;
}
