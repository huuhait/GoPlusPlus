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

thread_local uint64_t gopp::gir::impl::LowerSteps = 0;
thread_local uint64_t gopp::gir::impl::EscapeSteps = 0;

/// The steps of the lowerings this thread asked for (each runs on a thread
/// of its own, lowerPackage).
static thread_local uint64_t CallerSteps = 0;

static thread_local uint64_t CallerEscape = 0;

uint64_t gir::loweringSteps() { return CallerSteps; }
uint64_t gir::escapeSteps() { return CallerEscape; }

/// The steps of the instance walks (instanceSteps).
static thread_local uint64_t CallerInstanceSteps = 0;
static thread_local uint64_t WalkSteps = 0;

uint64_t gir::instanceSteps() { return CallerInstanceSteps; }
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
  // Generic code is lowered once per instance: a construct that is refused is
  // said once, at its place (P04.08).
  std::string Text = What.str();
  if (!Said.insert({At.getRaw(), Text}).second)
    return;
  Diags.report(At, diag::err_gir_unsupported) << Text;
}

void Lowering::unsupported(SourceLoc At, const llvm::Twine &What,
                           const llvm::Twine &Note) {
  Failed = true;
  ++Reported;
  Diags.report(At, diag::err_gir_unsupported).note(At, Note) << What.str();
}

void Lowering::internalError(SourceLoc At, const llvm::Twine &What) {
  Failed = true;
  ++Reported;
  Diags.report(At, diag::err_gir_verify) << What.str();
}

void Lowering::badExtern(SourceLoc At, StringRef Name, const llvm::Twine &Why) {
  Failed = true;
  ++Reported;
  Diags.report(At, diag::err_gir_extern_bad) << Name.str() << Why.str();
}

void Lowering::badExport(SourceLoc At, StringRef Name, const llvm::Twine &Why) {
  Failed = true;
  ++Reported;
  Diags.report(At, diag::err_gir_export_bad) << Name.str() << Why.str();
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
    return "a slice";
  case S::Type::Kind::Array:
    return "array";
  case S::Type::Kind::Map:
    return "a map";
  case S::Type::Kind::Chan:
    return "a channel (P12)";
  case S::Type::Kind::Ref:
    return "a view";
  case S::Type::Kind::Tuple:
    return "tuple";
  case S::Type::Kind::Signature:
    return "a function value";
  case S::Type::Kind::Struct:
    return "struct";
  case S::Type::Kind::Interface:
    return "an interface";
  case S::Type::Kind::Union:
    return "a union";
  case S::Type::Kind::Named:
    return "named type";
  case S::Type::Kind::TypeParam:
    return "a type parameter";
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
      return It->second ? Ctx.canonical(It->second) : nullptr;
    // T is under way and reached again. If a struct began after T did, the
    // cycle goes through it: its GIR struct exists (made before its body), so
    // T is lowered again right here and ends at that struct. Otherwise the
    // cycle goes through no struct (`type S []S`): T gets a self-naming type
    // (Type.h) that the parts name, and its body is set when T is done.
    if (StructsActive > Under->second)
      return lowerTypeOnce(T, At);
    // The key and value of a map are lowered to find what cannot be (the map
    // itself is one type for all of them): a type under way is just there.
    if (MapDepth > 0)
      return Ctx.unit();
    if (isa<S::NamedType>(T))
      return recShell(T);
    // A type with no name (`*L` in `type L [1]*L`) is lowered again: the
    // cycle ends at the named type, which is under way and is the shell.
    if (RebuildDepth < 8) {
      ++RebuildDepth;
      Type *Again = lowerTypeOnce(T, At);
      --RebuildDepth;
      return Again;
    }
    if (RecursiveReported.insert(T).second)
      unsupported(At, "the type " + S::typeString(T) +
                          ", which refers to itself through no named type,");
    return nullptr;
  }
  // A type of generic code is lowered at an instance, with its type
  // arguments applied (Body::inst): a type parameter here is a missing
  // substitution, never a type to guess (P04.08).
  Methods.expand(T);
  if (Methods.mentionsTypeParam(T)) {
    // (A local type of generic code is refused where it is declared, P04.28:
    // its uses are no second error.)
    if (!Failed)
      internalError(At, "the type " + S::typeString(T) +
                            " still names a type parameter (not substituted "
                            "for the instance)");
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
  if (auto RS = RecShells.find(T); RS != RecShells.end() && R) {
    // T reached itself: the type that came out is the body of the shell.
    if (Ctx.setRecBody(RS->second, R)) {
      R = RS->second;
    } else {
      unsupported(At, "the type " + S::typeString(T) +
                          ", which refers to itself and has the same shape "
                          "as another such type,");
      R = nullptr;
    }
  }
  // Never a null without a report: a function whose type is null is left out
  // of the module, and nothing may leave one out silently.
  if (!R && Reported == Before && !Failed)
    unsupported(At, "the type " + S::typeString(T));
  TypeCache[T] = R;
  return R;
}

/// Does a type argument of Named, or one inside one, hold a unit (a unit
/// argument, or a number type with a unit)?
bool Lowering::namesAUnit(S::NamedType *Named) {
  if (!CP.Ctx->unitsSeen())
    return false;
  // Iterative over the named arguments; each type is looked at once.
  SmallVector<S::Type *, 8> Work(Named->TArgs.begin(), Named->TArgs.end());
  llvm::SmallPtrSet<S::Type *, 16> Seen;
  while (!Work.empty()) {
    S::Type *T = Work.pop_back_val();
    if (!Seen.insert(T).second)
      continue;
    if (auto *B = dyn_cast<S::BasicType>(T)) {
      if (B->Unit)
        return true;
    } else if (auto *N = dyn_cast<S::NamedType>(T)) {
      Work.append(N->TArgs.begin(), N->TArgs.end());
    } else if (auto *P = dyn_cast<S::PointerType>(T)) {
      Work.push_back(P->Elem);
    } else if (auto *Sl = dyn_cast<S::SliceType>(T)) {
      Work.push_back(Sl->Elem);
    } else if (auto *A = dyn_cast<S::ArrayType>(T)) {
      Work.push_back(A->Elem);
    }
  }
  return false;
}

const sym::Node *Lowering::unitFree(S::Type *T) {
  // (A program without units: every type is its own.)
  if (!CP.Ctx->unitsSeen())
    return nullptr;
  const sym::Node *Root = Syms.Names.type(T);
  if (!Root)
    return nullptr;
  // One iterative post-order walk over the node DAG, memoized: each node is
  // rebuilt once. (Test-only: the stress test's control forgets.)
  if (Opts.ForgetErasedUnits)
    UnitFreeMemo.clear();
  sym::Context &C = Syms.Ctx;
  struct Frame {
    const sym::Node *N;
    size_t Next = 0;
  };
  SmallVector<Frame, 16> Stack{{Root}};
  SmallVector<const sym::Node *, 8> Kids;
  while (!Stack.empty()) {
    ++LowerSteps; // test-only: counted
    Frame &Fr = Stack.back();
    const sym::Node *N = Fr.N;
    if (UnitFreeMemo.count(N)) {
      Stack.pop_back();
      continue;
    }
    if (Fr.Next < N->Kids.size()) {
      const sym::Node *K = N->Kids[Fr.Next++];
      if (!UnitFreeMemo.count(K))
        Stack.push_back({K});
      continue;
    }
    const sym::Node *R = N;
    if (N->K == sym::Kind::UnitType) {
      // The number type: the kind whose tag is the letter.
      for (unsigned K = unsigned(sym::Kind::Int);
           K <= unsigned(sym::Kind::Complex128); ++K)
        if (sym::kindPrefix(sym::Kind(K))[0] == N->Sub)
          R = C.basic(sym::Kind(K));
    } else if (N->K == sym::Kind::Unit) {
      R = C.named("", "unit");
    } else if (!N->Kids.empty()) {
      Kids.clear();
      bool Same = true;
      for (const sym::Node *K : N->Kids) {
        Kids.push_back(UnitFreeMemo[K]);
        Same &= Kids.back() == K;
      }
      if (!Same)
        R = C.make(N->K, Kids, N->Sub, N->Num, N->Aux, N->A, N->B, N->C);
    }
    UnitFreeMemo[N] = R;
    Stack.pop_back();
  }
  return UnitFreeMemo[Root];
}

bool Lowering::isUnitArg(S::Type *T) {
  auto *B = dyn_cast<S::BasicType>(T);
  return B && B->BK == S::BasicKind::UnitArg;
}

bool Lowering::hasUnitArg(S::NamedType *Named) {
  return llvm::any_of(Named->TArgs, [&](S::Type *A) { return isUnitArg(A); });
}

std::string Lowering::uniqueTypeName(S::NamedType *Named) {
  std::string Name;
  if (Named) {
    // (A readable label: an instance written as a DAG of 2^n parts, an alias
    // chain of P[A, A], stops at MaxTypeName bytes and `...`.)
    if (hasUnitArg(Named))
      // (The layout of every unit it can have: `main.Vec2[unit]`.)
      Name = rtypeShown(Named)
                 .substr(0, MaxTypeName)
                 .substr(0, rtypeShown(Named).find('[')) +
             "[unit]";
    else if (const sym::Node *F = namesAUnit(Named) ? unitFree(Named) : nullptr)
      // (A unit inside an argument, `Box[float64[KM]]`: one struct for every
      // unit, named with the units erased, `main.Box[float64]`, the name of
      // the struct of `Box[float64]` too, whichever comes first; P04.24.)
      Name = sym::print(F, MaxTypeName).Text;
    else if (const sym::Node *N = Syms.Names.type(Named))
      Name = sym::print(N, MaxTypeName).Text;
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
  return Name;
}

Type *Lowering::recShell(S::Type *T) {
  auto [It, New] = RecShells.try_emplace(T, nullptr);
  if (New)
    It->second = Ctx.recTy(uniqueTypeName(cast<S::NamedType>(T)));
  return It->second;
}

Type *Lowering::lowerOption(S::OptionType *O, SourceLoc At) {
  // D4.5: the nil bit pattern of a pointer kind is nil; any other T gets a
  // flag byte after it. An interface is two words and its nil is a null
  // itab, so Option[I] is the interface type itself (D4.5). A func value is
  // (code, env) and its nil a null code pointer: Option[F] is F's own type
  // (P04.07, gir.md section 15).
  if (isInterface(O->Elem) || isa<S::SignatureType>(S::under(O->Elem)))
    return lowerType(O->Elem, At);
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
  // (A struct type that differs from another only in its units is its GIR
  // struct, UnitFreeTypes.)
  const sym::Node *Free = Named ? nullptr : unitFree(Whole);
  if (Free)
    if (auto It = UnitFreeStructs.find(Free); It != UnitFreeStructs.end())
      return StructCache[ST] = It->second;
  // A defined type and its underlying struct are one GIR struct, so a
  // conversion between two defined types of one struct is no code. The name
  // is the readable symbol of the first type that reaches it.
  std::string Name = uniqueTypeName(Named);
  Type *Str = Ctx.structTy(Name);
  StructCache[ST] = Str;
  if (Free)
    UnitFreeStructs[Free] = Str;
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
  // The debugger shows the fields by their names (DebugInfo.h).
  SmallVector<llvm::StringRef, 4> Names;
  for (S::Var *F : ST->Fields)
    Names.push_back(F->Name);
  Ctx.setFieldNames(Str, Names);
  // An instance of a generic type: the debugger spells the arguments. (Not
  // when one is the struct itself: the instances of a generic type whose
  // fields do not use its type parameters have one struct, the checker's,
  // `P[int, int]` and `P[P[int, int], int]` of a `struct{ n int }`; the
  // debugger then shows the name of the first, P04.08.)
  if (Named && !Named->TArgs.empty()) {
    SmallVector<Type *, 4> Args;
    // (A unit argument has no GIR type: units are erased, UNI-9.)
    size_t Units = 0;
    for (S::Type *A : Named->TArgs) {
      if (auto *UB = dyn_cast<S::BasicType>(A);
          UB && UB->BK == S::BasicKind::UnitArg) {
        ++Units;
        continue;
      }
      if (Type *G = lowerType(A, At); G && G != Str)
        Args.push_back(G);
    }
    if (Args.size() + Units == Named->TArgs.size() && !Units)
      Ctx.setTypeArgs(Str, Args);
  }
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
  case S::Type::Kind::Slice: {
    Type *E = lowerType(cast<S::SliceType>(T)->Elem, At);
    if (!E)
      return nullptr;
    R = Ctx.slice(E);
    break;
  }
  case S::Type::Kind::Map: {
    // The runtime object, one type for every key and value type (the
    // runtime knows them by a MapInfo, RuntimeMap.cpp); they are lowered
    // here so a type that cannot be lowered is reported at the map.
    auto *Mp = cast<S::MapType>(T);
    ++MapDepth;
    bool Ok = lowerType(Mp->Key, At) && lowerType(Mp->Elem, At);
    --MapDepth;
    if (!Ok)
      return nullptr;
    R = mapType();
    break;
  }
  case S::Type::Kind::Ref: {
    // A view (D4.5): a string or a slice is its data pointer and length,
    // anything else the address of the value.
    S::Type *Target = cast<S::RefType>(T)->Elem;
    S::Type *U = S::under(Target);
    if (isa<S::SliceType>(U) || isStringType(U)) {
      Type *E = isStringType(U) ? Ctx.u8()
                                : lowerType(cast<S::SliceType>(U)->Elem, At);
      if (!E)
        return nullptr;
      R = Ctx.tuple({Ctx.ptr(E), Ctx.u64()});
      break;
    }
    Type *E = lowerType(Target, At);
    if (!E)
      return nullptr;
    R = Ctx.ptr(E);
    break;
  }
  case S::Type::Kind::Interface:
    if (!isInterface(T)) {
      unsupported(At, "an interface that is a constraint, used as a type");
      return nullptr;
    }
    R = Ctx.iface();
    break;
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
    // An instance of a generic type is one type for each content of its type
    // arguments, however the checker spells it (`Box[byte]` and `Box[uint8]`
    // are two checker types): one GIR struct, one name, as its symbol (the
    // key of its methods' instances, Instances.h) is one.
    // Instances that differ only in their unit arguments (`Vec2[KM]`,
    // `Vec2[M]`, and the `Vec2[]` of generic code that takes the units at
    // run time) have one layout: one GIR struct (units are erased, UNI-9;
    // the checker's types stay apart in the type descriptors).
    if (N->Orig && !N->TArgs.empty())
      if (const sym::Node *F = unitFree(N)) {
        auto [It, New] = UnitFreeTypes.try_emplace(F, N);
        if (!New && It->second != N)
          return lowerType(It->second, At);
      }
    if (N->Orig && !N->TArgs.empty())
      if (const sym::Node *SN = Syms.Names.type(N)) {
        auto [It, New] = InstanceTypes.try_emplace(sym::mangleType(SN), N);
        if (!New && It->second != N && Methods.identical(It->second, N))
          return lowerType(It->second, At);
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
  case S::Type::Kind::Signature: {
    // A func value is the pair (code, env) (P04.07, gir.md section 15). A
    // `func!` type is `func(...) (..., error)` (ERR-2): methodSig makes the
    // error the last result, as for a declared `!` function.
    auto *SG = cast<S::SignatureType>(T);
    Type *PU8 = Ctx.ptr(Ctx.u8());
    Type *Code = methodSig(SG, PU8, At);
    if (!Code)
      return nullptr;
    R = Ctx.tuple({Ctx.func(Code->elems(), Code->ret(), false, CallConv::Go,
                            /*Env=*/true),
                   PU8});
    break;
  }
  default:
    unsupported(At, kindName(T->getKind()));
    return nullptr;
  }
  return R;
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

/// `@Inline` / `@NoInline` (P04.23). Both on one function: the safer one,
/// `@NoInline`, wins.
static Function::InlineHint inlineHintOf(const ast::FuncDecl *FD) {
  if (!FD->Body)
    return Function::InlineHint::None;
  if (findAttr(FD, "NoInline"))
    return Function::InlineHint::Never;
  if (findAttr(FD, "Inline"))
    return Function::InlineHint::Always;
  return Function::InlineHint::None;
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

/// Why a value of the checker type T cannot cross the C ABI because it is,
/// or holds by value, a Go++ string, slice, map or view (P04.05 review R5);
/// null if it holds none. Decided on the checker's types, before lowering:
/// a map is one pointer in GIR and a view of a struct is too, so the GIR
/// types cannot tell. Pointers are not followed (C gets an address).
static const char *goppOnlyLayout(S::Type *T) {
  SmallVector<S::Type *, 8> Work{T};
  llvm::SmallPtrSet<S::Type *, 8> Seen;
  while (!Work.empty()) {
    S::Type *U = S::under(Work.pop_back_val());
    if (!U || !Seen.insert(U).second)
      continue;
    if (isStringType(U))
      return "a string has no C layout (pass a *byte and a length)";
    if (isa<S::SliceType>(U))
      return "a slice has no C layout (pass a *T and a length)";
    if (isa<S::MapType>(U))
      return "a map has no C layout";
    if (isa<S::RefType>(U))
      return "a view has no C layout (pass a *T)";
    if (auto *ST = dyn_cast<S::StructType>(U))
      for (S::Var *F : ST->Fields)
        Work.push_back(F->Ty);
    else if (auto *AT = dyn_cast<S::ArrayType>(U))
      Work.push_back(AT->Elem);
    // (An Option[T] by value is CTypeRules' to refuse, in its own words.)
  }
  return nullptr;
}

bool Lowering::holdsOwned(S::Type *Root, bool MapsOnly) {
  llvm::DenseMap<S::Type *, bool> &Memo = MapsOnly ? MapMemo : OwnedMemo;
  if (auto It = Memo.find(Root); It != Memo.end())
    return It->second;
  struct Frame {
    S::Type *T;
    SmallVector<S::Type *, 4> Kids;
    size_t Next = 0;
    bool Acc = false;
  };
  SmallVector<Frame, 16> Stack;
  llvm::SmallPtrSet<S::Type *, 16> Open; // (a cycle by value is an error)
  bool Result = false;
  // Delivers the answer of a finished type to its parent.
  auto Done = [&](bool V) {
    if (Stack.empty())
      Result = V;
    else
      Stack.back().Acc |= V;
  };
  auto Visit = [&](S::Type *T) {
    ++LowerSteps;
    if (auto It = Memo.find(T); It != Memo.end())
      return Done(It->second);
    S::Type *U = S::under(T);
    if (!U)
      return Done(false);
    if (isa<S::MapType>(U) ||
        (!MapsOnly && (isStringType(U) || isa<S::SliceType>(U)))) {
      Memo[T] = true;
      return Done(true);
    }
    Frame F;
    F.T = T;
    if (auto *ST = dyn_cast<S::StructType>(U)) {
      for (S::Var *Fl : ST->Fields)
        F.Kids.push_back(Fl->Ty);
    } else if (auto *AT = dyn_cast<S::ArrayType>(U)) {
      F.Kids.push_back(AT->Elem);
    } else if (auto *OT = dyn_cast<S::OptionType>(U)) {
      F.Kids.push_back(OT->Elem);
    }
    if (F.Kids.empty() || !Open.insert(T).second) {
      Memo[T] = false;
      return Done(false);
    }
    Stack.push_back(std::move(F));
  };
  Visit(Root);
  while (!Stack.empty()) {
    Frame &F = Stack.back();
    if (!F.Acc && F.Next < F.Kids.size()) {
      S::Type *Kid = F.Kids[F.Next++];
      Visit(Kid);
      continue;
    }
    S::Type *T = F.T;
    bool V = F.Acc;
    Stack.pop_back();
    Open.erase(T);
    Memo[T] = V;
    Done(V);
  }
  return Result;
}

Lowering::Pass Lowering::passOfType(S::Type *T, bool Move) {
  if (Move)
    return Pass::Value;
  // A borrowed `[]T` is P04.25's pair (sliceBorrowed, ADR-0029 point 7); a
  // borrowed string is the same pair.
  if (isa<S::SliceType>(S::under(T)) || isStringType(S::under(T)))
    return Pass::View;
  return holdsOwned(T) ? Pass::Place : Pass::Value;
}

Lowering::Pass Lowering::passOf(S::SignatureType *Sig, size_t K) {
  if (K >= Sig->Params->Elems.size())
    return Pass::Value;
  return passOfType(Sig->Params->Elems[K], Sig->isMove(K));
}

S::Type *Lowering::recvTypeOf(const S::Func *F) {
  if (!F->Recv)
    return nullptr;
  if (!F->RecvInst)
    return F->Recv->Ty;
  // A method of an instance of a generic type: its declaration's receiver
  // type names the declaration's own parameters.
  const GenericInst *In = Walk.methodInst(F);
  if (!In) {
    internalError(F->Pos, "the method " + F->Name.str() + " of " +
                              S::typeString(F->RecvInst) +
                              " is no instance the walk found");
    return nullptr;
  }
  return Walk.inst(const_cast<GenericInst *>(In), F->Recv->Ty);
}

S::SignatureType *Lowering::instanceSig(const GenericInst *GI) {
  return cast<S::SignatureType>(
      Walk.inst(const_cast<GenericInst *>(GI), GI->Decl->Ty));
}

Lowering::Pass Lowering::passOfRecvAt(const S::Func *F, S::Type *RecvTy) {
  if (!F->Recv || F->HasPtrRecv || F->MoveRecv || !RecvTy)
    return Pass::Value;
  return passOfType(RecvTy, false);
}

S::Var::Borrow Lowering::recvModeOf(const S::Func *F) {
  if (!F->Recv)
    return S::Var::Owned;
  return F->RecvInst ? Methods.modeAt(F->Recv, recvTypeOf(F)) : F->Recv->Mode;
}

Lowering::Pass Lowering::passOfRecv(const S::Func *F) {
  if (!F->Recv || F->HasPtrRecv || F->MoveRecv)
    return Pass::Value;
  return passOfRecvAt(F, recvTypeOf(F));
}

static std::string reservedNameWhy(StringRef Name) {
  return "the C name " + Name.str() +
         " starts with the reserved prefix _GP, which is the prefix of every "
         "Go++ symbol (SYM-4); choose another name";
}

/// Why an @Export attribute is not `@Export("C")` on a function with a body;
/// empty if it is.
static std::string exportProblem(const ast::FuncDecl *FD,
                                 const ast::Attribute *A,
                                 const SourceManager &SM) {
  if (!A->HasArgs || A->Args.size() != 1 || A->Args[0].Kind != tok::string_lit)
    return "write @Export(\"C\")";
  StringRef Buf = SM.getBuffer(SM.getFileID(A->Args[0].Loc));
  std::string ABI;
  if (!sema::unquoteString(
          Buf.substr(SM.getFileOffset(A->Args[0].Loc), A->Args[0].Length),
          ABI) ||
      ABI != "C") {
    // `@Export("name")` is the spelling of other languages: the argument is
    // the ABI, and the C name is the function's own (appendix A.8).
    std::string Why = "only the \"C\" ABI exists, and the C name is the "
                      "function's own name";
    if (sym::isReservedName(ABI))
      Why += "; " + reservedNameWhy(ABI);
    return Why;
  }
  if (!FD->Body)
    return "an exported function needs a body";
  StringRef Name = FD->Name->Name.str();
  if (sym::isReservedName(Name))
    return reservedNameWhy(Name);
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
  // C has no name to call `_` by, and an init function runs once, before
  // main: neither can be exported (P04.18).
  if (const ast::Attribute *Exp = findAttr(FD, "Export");
      Exp && (Name == "_" || isInitDecl(FD))) {
    badExport(Exp->getBegin(), Name,
              Name == "_" ? "the blank function has no name C could call"
                          : "an init function runs once before main; C "
                            "cannot call it");
    return;
  }
  if (Name == "_")
    return;
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
  // Generic code is declared and lowered per instance (Instances.h).
  if (isGenericDecl(Obj))
    return;
  // Every return below that is not the last one has reported an error.
  Declined.insert(Obj);
  const ast::Attribute *Ext = externAttr(FD);
  if (Ext && FD->Recv) {
    badExtern(Ext->getBegin(), Name, "a method cannot be a C function");
    return;
  }
  if (Ext && Opts.TestIntrinsics && Name.starts_with("gopp_test_")) {
    // A test intrinsic, not a C function (ADR-0029, P04.25).
    // The name is the stem, then perhaps `_` and a suffix: a program that
    // uses several element types declares one function per type.
    static const std::pair<const char *, TestFn> Table[] = {
        {"gopp_test_make_slice", TestFn::MakeSlice},
        {"gopp_test_make_map", TestFn::MakeMap},
        {"gopp_test_clone_slice", TestFn::CloneSlice},
        {"gopp_test_free_slice", TestFn::FreeSlice},
        {"gopp_test_free_map", TestFn::FreeMap},
        {"gopp_test_count", TestFn::Count},
        {"gopp_test_word", TestFn::Word}};
    for (const auto &[N, K] : Table)
      if (Name == N || (Name.starts_with(N) && Name[strlen(N)] == '_')) {
        Declined.erase(Obj);
        TestIntrinsics[Obj] = K;
        return;
      }
    unsupported(FD->getBegin(), "the test intrinsic " + Name);
    return;
  }
  const ast::Attribute *Exp = findAttr(FD, "Export");
  if (Exp && FD->Recv) {
    badExport(Exp->getBegin(), Name,
              "a method cannot be a C function (export a function that "
              "calls it)");
    return;
  }
  // C-visible names are never ours (SYM-4, appendix A.8): `sym::isReservedName`
  // is the one test.
  if (Ext && sym::isReservedName(Name)) {
    badExtern(Ext->getBegin(), Name, reservedNameWhy(Name));
    return;
  }
  if (Ext && Exp) {
    badExport(Exp->getBegin(), Name,
              "a function is @Extern(\"C\") (defined in C) or @Export(\"C\") "
              "(defined here), not both");
    return;
  }
  if (Exp) {
    std::string Why = exportProblem(FD, Exp, SM);
    if (!Why.empty()) {
      badExport(Exp->getBegin(), Name, Why);
      return;
    }
  }
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
  S::SignatureType *Sig = Obj->sig();
  if (FD->Recv && !methodSupported(Obj, FD->getBegin()))
    return;
  // A Go++ string, slice, map or view in a C signature (also inside a struct
  // or an array passed by value): one decision, on the checker's types.
  const bool ToC = Ext || Exp;
  auto BadC = [&](const llvm::Twine &Why) {
    if (Exp)
      badExport(FD->getBegin(), Name, Why);
    else
      badExtern(FD->getBegin(), Name, Why);
  };
  auto IsFunc = [](S::Type *T) {
    return isa_and_nonnull<S::SignatureType>(S::under(T));
  };
  // A C function has no error result: `!` is Go++'s (the interface word).
  if (ToC && Sig->Fails) {
    BadC("a C function cannot fail with ! (C has no error result; return a "
         "status code)");
    return;
  }
  if (ToC) {
    auto Refuse = [&](S::Type *T, const char *What) {
      const char *Why = goppOnlyLayout(T);
      if (!Why)
        return false;
      BadC(llvm::Twine(What) + " type " + S::typeString(T) +
           " cannot cross the C ABI: " + Why);
      return true;
    };
    for (S::Type *P : Sig->Params->Elems)
      if (Refuse(P, "parameter"))
        return;
    if (Sig->Results->size() == 1 && Refuse(Sig->Results->Elems[0], "result"))
      return;
  }
  // A function value in a C signature: a parameter of an @Extern function is
  // a callback (a top-level Go++ function, wrapped in a C-ABI thunk where it
  // is passed, P04.18). Calling a function value, which an @Export function
  // would do with one (P07.13), and returning one (P07.13, P07.08) are not
  // lowered: a closure has an environment, C has no place for it.
  if (ToC) {
    if (Exp)
      for (S::Type *P : Sig->Params->Elems)
        if (IsFunc(P)) {
          unsupported(FD->getBegin(),
                      "a function parameter of an @Export function (P07.13)");
          return;
        }
    if (Sig->Results->size() == 1 && IsFunc(Sig->Results->Elems[0])) {
      unsupported(FD->getBegin(),
                  Exp ? "a function returned from an @Export function (P07.13)"
                      : "a function returned from C (P07.08)");
      return;
    }
  }
  SmallVector<Type *, 4> Params;
  if (FD->Recv) {
    // The receiver is parameter 0 (PAR-5), passed as passOfRecv says: a
    // pointer receiver is the pointer; a borrowed value receiver is a
    // pointer to the caller's place, a (ptr, len) view or the value, as a
    // borrowed parameter of its type.
    Type *T = lowerType(Obj->Recv->Ty, FD->getBegin());
    if (!T)
      return;
    Params.push_back(passType(passOfRecv(Obj), T));
  }
  const size_t Lead = Params.size();
  for (S::Type *P : Sig->Params->Elems) {
    Type *T = Ext && IsFunc(P) ? callbackType(P, FD->getBegin(), Name)
                               : lowerType(P, FD->getBegin());
    if (!T)
      return;
    if (!ToC)
      T = paramType(Sig, Params.size() - Lead, T);
    Params.push_back(T);
  }
  Type *Ret = Ctx.unit();
  if (Sig->Results->size() == 1)
    Ret = lowerType(Sig->Results->Elems[0], FD->getBegin());
  else if (Sig->Results->size() > 1)
    Ret = lowerType(Sig->Results, FD->getBegin());
  if (!Ret)
    return;
  if (ToC) {
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
        BadC(std::string(What) + " type " + T->str() +
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
  // The C symbol of an @Export function is its declared name, exactly: the
  // thunk's (the function itself keeps its Go++ symbol).
  std::string CName = Exp ? symbolOf(SymbolKind::ExternC, CP.Pkg, Name) : "";
  if ((Ext && F->Name == symbolOf(SymbolKind::Entry, CP.Pkg, "")) ||
      (Exp && CName == symbolOf(SymbolKind::Entry, CP.Pkg, ""))) {
    BadC("the C symbol " + (Ext ? F->Name : CName) +
         " is the program's entry point");
    return;
  }
  // The C name may be another function's: one declared here, or a C
  // library function the runtime calls (println's `write`; what the runtime
  // declares when a body needs it, after every declaration), whose place the
  // exported thunk would take for the whole program.
  if (Exp && (M->lookupFunction(CName) != NoId || isRuntimeCName(CName))) {
    badExport(Exp->getBegin(), Name,
              "the C symbol " + CName + " is used by another function" +
                  (isRuntimeCName(CName)
                       ? ", the C library's, which the Go++ runtime calls"
                       : ""));
    return;
  }
  if (M->lookupFunction(F->Name) != NoId) {
    unsupported(FD->getBegin(), "a second function named " + Name.str());
    return;
  }
  F->CC = Ext ? CallConv::C : CallConv::Go;
  F->Sig = Ctx.func(Params, Ret, false, F->CC);
  F->NoUnwind = Ext;
  F->Inline = inlineHintOf(FD);
  F->Loc = spanOf(FD->getBegin());
  FuncId Id = M->addFunction(std::move(F));
  FuncIds[Obj] = Id;
  if (Exp)
    makeCThunk(Id, CName, Linkage::External);
  Declined.erase(Obj);
}

Type *Lowering::callbackType(S::Type *T, SourceLoc At, StringRef Fn) {
  auto *SG = cast<S::SignatureType>(S::under(T));
  if (SG->Fails) {
    badExtern(At, Fn,
              "a C callback cannot fail with ! (C has no error result; "
              "return a status code)");
    return nullptr;
  }
  auto IsFunc = [](S::Type *X) {
    return isa_and_nonnull<S::SignatureType>(S::under(X));
  };
  SmallVector<Type *, 4> Ps;
  for (S::Type *P : SG->Params->Elems) {
    if (IsFunc(P)) {
      unsupported(At, "a callback that takes a function (P07.08)");
      return nullptr;
    }
    if (const char *Why = goppOnlyLayout(P)) {
      badExtern(At, Fn,
                "callback parameter type " + S::typeString(P) +
                    " cannot cross the C ABI: " + Why);
      return nullptr;
    }
    Type *G = lowerType(P, At);
    if (!G)
      return nullptr;
    Ps.push_back(G);
  }
  Type *Ret = Ctx.unit();
  if (SG->Results->size() > 1) {
    badExtern(At, Fn, "a C callback has one result (return a struct)");
    return nullptr;
  }
  if (SG->Results->size() == 1) {
    S::Type *R = SG->Results->Elems[0];
    if (IsFunc(R)) {
      unsupported(At, "a callback that returns a function (P07.08)");
      return nullptr;
    }
    if (const char *Why = goppOnlyLayout(R)) {
      badExtern(At, Fn,
                "callback result type " + S::typeString(R) +
                    " cannot cross the C ABI: " + Why);
      return nullptr;
    }
    Ret = lowerType(R, At);
    if (!Ret)
      return nullptr;
  }
  // A C function pointer: what C calls through it has the C ABI.
  return Ctx.func(Ps, Ret, false, CallConv::C);
}

bool Lowering::checkCallbackTarget(const S::Func *Fn, FuncId Target,
                                   SourceLoc At) {
  S::SignatureType *Sig = Fn->sig();
  StringRef Name = Fn->Name;
  const Function &T = M->func(Target);
  auto IsFunc = [](S::Type *X) {
    return isa_and_nonnull<S::SignatureType>(S::under(X));
  };
  auto Bad = [&](const llvm::Twine &Why) {
    badExtern(At, Name,
              "the function " + Name.str() + " cannot be called by C: " + Why);
    return false;
  };
  if (Sig->Variadic)
    return Bad("it is variadic");
  size_t K = 0;
  for (S::Type *P : Sig->Params->Elems) {
    ++K;
    if (IsFunc(P)) {
      unsupported(At, "a callback that takes a function (P07.08)");
      return false;
    }
    if (const char *Why = goppOnlyLayout(P))
      return Bad("parameter " + std::to_string(K) + " of type " +
                 S::typeString(P) + ": " + Why);
  }
  if (Sig->Results->size() > 1)
    return Bad("a C function has one result (return a struct)");
  if (Sig->Results->size() == 1) {
    S::Type *R = Sig->Results->Elems[0];
    if (IsFunc(R)) {
      unsupported(At, "a callback that returns a function (P07.08)");
      return false;
    }
    if (const char *Why = goppOnlyLayout(R))
      return Bad(std::string("the result of type ") + S::typeString(R) + ": " +
                 Why);
  }
  K = 0;
  for (Type *P : T.Sig->elems()) {
    gir::CVerdict V = CRules.check(P, gir::CUse::Param);
    if (!V.ok())
      return Bad("parameter " + std::to_string(++K) + ": " + V.Why);
    ++K;
  }
  gir::CVerdict V = CRules.check(T.Sig->ret(), gir::CUse::Result);
  if (!V.ok())
    return Bad("the result: " + V.Why);
  return true;
}

/// The C-ABI function that calls the Go++ function Target (P04.18, D4.4): its
/// parameters and result are the target's, which are C types (CTypeRules
/// decided before), its calling convention is the C one, so the code
/// generator unpacks the parameters with the C ABI (cabi::unpackCParams) and
/// packs the result (cabi::packCReturn). Used for an @Export function (named
/// by its C name) and for a Go++ function passed to C (named `_GPTC...`).
FuncId Lowering::makeCThunk(FuncId Target, const std::string &Name,
                            Linkage Link) {
  const Function &T = M->func(Target);
  auto F = std::make_unique<Function>();
  F->Name = Name;
  F->Sig = Ctx.withCallConv(T.Sig, CallConv::C);
  F->Link = Link;
  F->CC = CallConv::C;
  F->Loc = T.Loc;
  Type *Ret = T.Sig->ret();
  unsigned NParams = T.Sig->elems().size();
  FuncId Id = M->addFunction(std::move(F));
  Function &Fn = M->func(Id);
  FuncBuilder FB(*M, Fn);
  FB.setSpan(Fn.Loc);
  SmallVector<Operand, 4> Args;
  for (unsigned K = 0; K < NParams; ++K)
    Args.push_back(copyOf(FB.local(FB.param(K))));
  LocalId Res = FB.newTemp(Ret);
  FB.call(constFunc(*M, Target), Args, FB.local(Res),
          {UnwindAction::Continue, NoId});
  if (!Ret->isUnit())
    FB.assign(FB.local(FB.returnPlace()), FB.use(copyOf(FB.local(Res))));
  FB.ret();
  return Id;
}

/// The thunk for passing the top-level function Target to C (once per
/// function). Its symbol is the function's own, wrapped (`_GPTC...`, appendix
/// A.5), and its body is the same wherever it is made, so it is linkonce_odr
/// like the glue: every package that passes the function to C gives C the
/// same pointer (C code that registers and later unregisters a callback by
/// its address sees one function).
FuncId Lowering::callbackThunk(const S::Func *Fn, FuncId Target) {
  auto [It, New] = CThunks.try_emplace(Target, NoId);
  if (New) {
    std::string Name = Syms.thunk(Fn);
    if (Name.empty())
      Name = M->func(Target).Name + "$C";
    It->second = makeCThunk(Target, Name, Linkage::LinkOnceODR);
  }
  return It->second;
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
    if (!Obj || isGenericDecl(Obj))
      return;
    auto It = FuncIds.find(Obj);
    if (It == FuncIds.end())
      return;
    Id = It->second;
  }
  Function &F = M->func(Id);
  Body B(*this, F, Obj);
  // The scope of the function literals declared in this body: their symbols
  // name it. (Its types were named before the instance walk, with the same
  // scopes: declareLocalTypes.)
  B.ScopeNode = Obj ? Syms.funcNode(Obj)
                    : Syms.Ctx.initFunc(Syms.Names.pathOf(CP.Pkg), Ordinal);
  // The function literals inside: what they capture, which escape (P04.07).
  EntityFacts Facts;
  SmallVector<const S::Var *, 8> Outer;
  if (Obj) {
    Outer.append(Obj->Params.begin(), Obj->Params.end());
    Outer.append(Obj->Results.begin(), Obj->Results.end());
    if (Obj->Recv)
      Outer.push_back(Obj->Recv);
  }
  analyzeEntity({FD->Body}, FD->Body, Outer, Keeps, FuncHolders, I, Facts);
  B.Facts = &Facts;
  B.lower(FD);
}

//===----------------------------------------------------------------------===//
// Instances of generic code (P04.08)
//===----------------------------------------------------------------------===//

void Lowering::declareInstance(GenericInst &In) {
  if (In.Bad)
    return;
  const S::Func *Obj = In.Decl;
  const ast::FuncDecl *FD = Obj->Decl;
  if (!FD)
    return;
  SourceLoc At = FD->getBegin();
  if (!FD->Body) {
    unsupported(At, "a generic function without a body");
    return;
  }
  if (externAttr(FD) || findAttr(FD, "Export")) {
    unsupported(At, "an @Extern or @Export attribute on generic code (P04.28)");
    return;
  }
  if (FD->Recv && !methodSupported(Obj, At))
    return;
  // (A function that fails with `!` has the failure as its last result, as
  // in plain code: P04.10.)
  auto *Sig = cast<S::SignatureType>(Walk.inst(&In, Obj->Ty));
  SmallVector<Type *, 4> Params;
  if (FD->Recv) {
    // The receiver is parameter 0, passed as the instance's receiver type
    // says (passOfRecvAt: the same rule as plain code, on real types).
    S::Type *RT = Walk.inst(&In, Obj->Recv->Ty);
    Type *T = lowerType(RT, At);
    if (!T)
      return;
    Params.push_back(passType(passOfRecvAt(Obj, RT), T));
  }
  const size_t Lead = Params.size();
  for (S::Type *P : Sig->Params->Elems) {
    Type *T = lowerType(P, At);
    if (!T)
      return;
    Params.push_back(paramType(Sig, Params.size() - Lead, T));
  }
  // The run-time unit descriptors the instance takes (GEN-14), after the
  // declared parameters.
  for (size_t K = 0; K < In.DictParams.size(); ++K)
    Params.push_back(Ctx.ptr(Ctx.u8()));
  Type *Ret = Ctx.unit();
  if (Sig->Results->size() == 1)
    Ret = lowerType(Sig->Results->Elems[0], At);
  else if (Sig->Results->size() > 1)
    Ret = lowerType(Sig->Results, At);
  if (!Ret)
    return;
  auto F = std::make_unique<Function>();
  F->Name = In.Symbol;
  // One home per instance: every package that needs it emits the same
  // function under the same symbol, and the linker keeps one (D4.3).
  F->Link = Linkage::LinkOnceODR;
  F->CC = CallConv::Go;
  F->Sig = Ctx.func(Params, Ret, false, F->CC);
  F->Inline = inlineHintOf(FD);
  F->Loc = spanOf(At);
  if (M->lookupFunction(F->Name) != NoId) {
    unsupported(At, "a second function named " + F->Name);
    return;
  }
  FuncId Id = M->addFunction(std::move(F));
  InstIds[&In] = Id;
  if (In.Method) {
    // A method is called by its checker object. An instance that generic code
    // shared across unit arguments asked for (In.Shared) has the types of the
    // one with real units and, refused otherwise, the same code: a call goes
    // to the one with real units when there is one.
    auto Bind = [&](const S::Func *F) {
      auto [It, New] = FuncIds.try_emplace(F, Id);
      if (!New && !In.Shared)
        It->second = Id;
    };
    Bind(In.Method);
    for (const S::Func *A : In.Aliases)
      Bind(A);
  }
}

void Lowering::declareNewInstances() {
  // (The list grows while it is declared: declareInstance asks the walk for
  // nothing, but the types it lowers may.)
  for (; InstDeclared < Walk.list().size(); ++InstDeclared)
    declareInstance(*Walk.list()[InstDeclared]);
}

void Lowering::lowerPendingInstances() {
  for (; InstLowered < Walk.list().size(); ++InstLowered) {
    declareNewInstances();
    lowerInstanceBody(*Walk.list()[InstLowered]);
  }
  declareNewInstances();
}

bool Lowering::ensureMethod(const S::Func *F, SourceLoc At) {
  if (FuncIds.count(F))
    return true;
  if (!F->RecvInst)
    return false;
  if (!Walk.needMethod(F, At))
    return false;
  declareNewInstances();
  return FuncIds.count(F) != 0;
}

void Lowering::lowerInstanceBody(GenericInst &In) {
  auto It = InstIds.find(&In);
  if (It == InstIds.end())
    return;
  Function &F = M->func(It->second);
  const S::Func *Obj = In.Decl;
  const ast::FuncDecl *FD = Obj->Decl;
  Body B(*this, F, Obj, &In);
  // A function literal in generic code belongs to the instance (decision
  // M6, appendix A.5): its symbol is named by the instance's, so each
  // instance has closures of its own.
  B.ScopeNode = Walk.nodeOf(In);
  if (!B.ScopeNode || sym::mangle(B.ScopeNode) != In.Symbol) {
    internalError(FD->getBegin(), "the instance " + In.Symbol +
                                      " has no symbol node of its own");
    return;
  }
  // What its literals capture and which escape, decided on the instance's
  // types (a variable of type T may hold a func value at one instance only).
  EntityFacts Facts;
  SmallVector<const S::Var *, 8> Outer(Obj->Params.begin(), Obj->Params.end());
  Outer.append(Obj->Results.begin(), Obj->Results.end());
  if (Obj->Recv)
    Outer.push_back(Obj->Recv);
  analyzeEntity({FD->Body}, FD->Body, Outer, Keeps, FuncHolders, I, Facts,
                [&](S::Type *T) { return Walk.inst(&In, T); });
  B.Facts = &Facts;
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
  F->Sig = Ctx.func({}, Ctx.i32(), false, CallConv::C);
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
  // The types declared in function bodies get their symbols first: an
  // instance may have one as a type argument (Instances.h).
  declareLocalTypes(CP, Files, Syms);
  // The instances of generic code the package needs, and what is wrong with
  // any of them, before anything is declared (Instances.h).
  const bool Walked = Walk.run();
  WalkSteps = Walk.steps();
  if (!Walked) {
    Failed = true;
    return nullptr;
  }
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
        // (A unit declaration has no code, UNI-9.)
        if (GD->IsUnit || GD->Keyword == tok::kw_import ||
            GD->Keyword == tok::kw_type || GD->Keyword == tok::kw_var ||
            GD->Keyword == tok::kw_const)
          continue;
        unsupported(GD->getBegin(), "this package-level declaration");
      } else {
        unsupported(D->getBegin(), "this declaration");
      }
    }
  }
  declareGlobals();
  declareNewInstances();
  // A function that was declined has had its error reported: none leaves the
  // module without one (a bug in a step that returned null silently).
  if (!Declined.empty() && !Failed)
    unsupported(Files.empty() ? SourceLoc() : Files[0]->PackageLoc,
                "a function that could not be lowered");
  Keeps.compute(Files, I);
  for (const ast::File *File : Files)
    for (const ast::Decl *D : File->Decls)
      if (auto *FD = dyn_cast<ast::FuncDecl>(D))
        lowerBody(FD);
  lowerPendingInstances();
  FuncId PkgInit = Failed ? NoId : lowerPackageInit();
  lowerPendingInstances(); // (the initializers may need more methods)
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
  uint64_t Steps = 0, Escapes = 0, Walked = 0;
  runOnNewStackOrHere(
      [&] {
        uint64_t Before = LowerSteps, EscBefore = EscapeSteps;
        WalkSteps = 0;
        Lowering L(Ctx, SM, CP, Files, Diags, Opts);
        M = L.run();
        // (The checker's lookups for the method sets count too.)
        Steps = LowerSteps - Before + L.Methods.steps();
        // (The checker's walk over the types that hold a view, which the
        // closures ask, counts with them.)
        Escapes = EscapeSteps - EscBefore + L.Methods.viewSteps();
        Walked = WalkSteps;
      },
      LowerStackBytes);
  CallerSteps += Steps;
  CallerEscape += Escapes;
  CallerInstanceSteps += Walked;
  return M;
}
