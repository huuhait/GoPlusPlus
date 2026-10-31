//===- DebugInfo.cpp - DWARF for the code generator -----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/CodeGen/DebugInfo.h"

#include "gopp/Mangle/SymbolName.h"

#include "llvm/ADT/StableHashing.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <cassert>
#include <cctype>

using namespace gopp;
namespace G = gopp::gir;

namespace {

/// An unnamed struct of the source is `struct.N` in GIR (the lowering's
/// uniqueTypeName); the debugger spells it by its fields.
bool isAnonymousStruct(const G::Type *T) {
  if (T->getKind() != G::Type::Kind::Struct || T->isRec())
    return false;
  llvm::StringRef N = T->name();
  return N.starts_with("struct.") && N.size() > 7 &&
         N.drop_front(7).find_if([](char C) { return !std::isdigit(C); }) ==
             llvm::StringRef::npos;
}

/// Is T a struct in DWARF, so that a declaration of its name stands for it?
/// A self-naming pointer or array is one: build wraps it in a struct.
bool isStructShaped(const G::Type *T) {
  using K = G::Type::Kind;
  switch (T->getKind()) {
  case K::Unit:
  case K::String:
  case K::Slice:
  case K::Option:
  case K::Tuple:
  case K::Struct:
    return true;
  case K::Ptr:
  case K::Array:
    return T->isRec();
  default:
    return false;
  }
}

/// A variable the program declared, shown by the debugger: a named parameter
/// or variable (Local::Source) of a function with a position. The helpers
/// the compiler writes (println's, the runtime's) have no source position:
/// their locals are no variables of the program.
bool isVariable(const G::Function &F, const G::Local &Lo) {
  return Lo.Source && !Lo.Name.empty() && Lo.Name != "_" && !Lo.Ty->isUnit() &&
         F.Loc.isValid();
}

/// Is Symbol a wrapper the compiler wrote around another entity (appendix
/// A.5: a C-ABI thunk, a method value or expression, a promoted method, an
/// interface thunk)? It has the position of what it wraps, and is artificial.
bool isWrapper(llvm::StringRef Symbol) {
  for (sym::Kind K :
       {sym::Kind::CABIThunk, sym::Kind::MethodValue, sym::Kind::MethodExpr,
        sym::Kind::Promoted, sym::Kind::IfaceThunk, sym::Kind::IfacePtrThunk})
    if (Symbol.starts_with(sym::Prefix.str() + sym::kindPrefix(K)))
      return true;
  return false;
}

} // namespace

DebugInfoBuilder::DebugInfoBuilder(
    llvm::Module &LM, const G::Module &GM,
    std::function<llvm::Type *(const G::Type *)> MemTy, bool LinkageNames,
    bool Optimized)
    : LM(LM), GM(GM), C(LM.getContext()), DIB(LM), LinkageNames(LinkageNames),
      Optimized(Optimized), MemTy(std::move(MemTy)) {}

void DebugInfoBuilder::start() {
  LM.addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
  LM.addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                   llvm::DEBUG_METADATA_VERSION);
  for (const std::string &F : GM.Files)
    Files.push_back(DIB.createFile(llvm::sys::path::filename(F),
                                   llvm::sys::path::parent_path(F)));
  if (Files.empty())
    Files.push_back(DIB.createFile("<unknown>", ""));
  // DWARF has no code for Go++. C11 is a language both gdb and lldb have a
  // type system and an expression parser for (lldb has none for DW_LANG_Go),
  // and in it both show DW_AT_name (`main.add`) when there is no linkage
  // name. No .debug_names: gdb ignores an index it did not write and warns,
  // and LLVM 23 leaves out of it the base types it makes for a typed
  // expression, which `llvm-dwarfdump --verify` reports.
  CU = DIB.createCompileUnit(llvm::dwarf::DW_LANG_C11, Files[0], "gopp",
                             Optimized, "", 0, "",
                             llvm::DICompileUnit::FullDebug, 0, true, false,
                             llvm::DICompileUnit::DebugNameTableKind::None);
}

void DebugInfoBuilder::finish() {
  // The structs that declarations stand for and nothing made: one at a time,
  // as making one may need more.
  while (!Pending.empty()) {
    const G::Type *T = Pending.back();
    Pending.pop_back();
    Kept.push_back(typeOf(T));
  }
  // In the order of their names (one definition per name, spelling), so the
  // unit does not depend on the order the types were asked for.
  llvm::stable_sort(Kept, [](const llvm::DIType *A, const llvm::DIType *B) {
    return A->getName() < B->getName();
  });
  for (llvm::DIType *T : Kept)
    DIB.retainType(T);
  DIB.finalize();
}

//===----------------------------------------------------------------------===//
// Names
//===----------------------------------------------------------------------===//

std::string DebugInfoBuilder::readableName(llvm::StringRef Symbol) {
  if (!sym::isReservedName(Symbol))
    return Symbol.str();
  sym::DemangleResult R = sym::demangle(Symbol);
  return R.Ok ? R.Text : Symbol.str();
}

/// A name cut at MaxNameLength bytes (not inside a UTF-8 sequence), followed
/// by the hash of T: a type nested N deep would otherwise have a name of N
/// levels (N names of that size are quadratic), and two types cut at the same
/// place must still have two names (a debugger finds a definition by its
/// name), whichever is spelled first.
std::string DebugInfoBuilder::capped(std::string Name, const G::Type *T) {
  if (Name.size() <= MaxNameLength)
    return Name;
  size_t N = MaxNameLength;
  while (N > 0 && (static_cast<unsigned char>(Name[N]) & 0xC0) == 0x80)
    --N;
  Name.resize(N);
  return Name + "...#" + llvm::utohexstr(hashOf(T), /*LowerCase=*/true);
}

/// A hash of the whole spelling of T, past any cut, made from the hashes of
/// its parts (so once per type, and the same in every run: xxh3). Two types
/// of one spelling have one hash; two others have two, but for a collision
/// of 64 bits.
uint64_t DebugInfoBuilder::hashOf(const G::Type *T) {
  if (auto It = Hashes.find(T); It != Hashes.end())
    return It->second;
  ++Work.TypeSteps;
  uint64_t H = static_cast<uint64_t>(T->getKind()) + 1;
  auto Add = [&H](uint64_t V) { H = llvm::stable_hash_combine(H, V); };
  bool Ran = Walk.level([&] {
    if (T->isNominal() && !isAnonymousStruct(T)) {
      Add(llvm::xxh3_64bits(T->name()));
      return;
    }
    Add(T->bits());
    Add(T->isSigned());
    Add(T->length());
    Add(T->fails());
    if (T->elem())
      Add(hashOf(T->elem()));
    for (size_t I = 0; I < T->elems().size(); ++I) {
      ++Work.TypeSteps;
      Add(hashOf(T->elems()[I]));
      Add(llvm::xxh3_64bits(T->fieldName(I)));
    }
    if (T->ret())
      Add(hashOf(T->ret()));
  });
  if (!Ran)
    H = 0; // the lowering reports the depth (failedAt)
  Hashes[T] = H;
  return H;
}

/// The spelling of a type from the spellings of its parts. A long list of
/// parts stops once the name will be cut anyway.
std::string DebugInfoBuilder::spell(const G::Type *T) {
  using K = G::Type::Kind;
  if (T->isNominal() && !isAnonymousStruct(T)) {
    // An instance of a generic type: `main.Pair[int64, string]`, the stem of
    // its name and the arguments as the debugger spells any type.
    if (!T->typeArgs().empty()) {
      llvm::StringRef N = T->name();
      std::string S = N.substr(0, N.find('[')).str() + "[";
      for (size_t I = 0; I < T->typeArgs().size() && S.size() <= MaxNameLength;
           ++I, ++Work.TypeSteps)
        S += (I ? ", " : "") + spelling(T->typeArgs()[I]);
      return S + "]";
    }
    return T->name().str();
  }
  switch (T->getKind()) {
  case K::Unit:
    return "()";
  case K::Bool:
    return "bool";
  case K::Int:
    return (T->isSigned() ? "int" : "uint") + std::to_string(T->bits());
  case K::Float:
    return "float" + std::to_string(T->bits());
  case K::String:
    return "string";
  case K::Ptr:
    return "*" + spelling(T->elem());
  case K::Slice:
    return "[]" + spelling(T->elem());
  case K::Array:
    return "[" + std::to_string(T->length()) + "]" + spelling(T->elem());
  case K::Option:
    return "Option[" + spelling(T->elem()) + "]";
  case K::Tuple:
  case K::Func: {
    // A func value (P04.07) is spelled as Go spells the type: `func(int64)
    // int64`; its code, which also takes the environment, is `func(env,
    // int64) int64`.
    const bool Value = T->isFuncValue();
    const G::Type *Code = Value ? T->elems()[0] : T;
    const bool IsFunc = Value || T->getKind() == K::Func;
    std::string S = IsFunc ? "func(" : "(";
    // (The value's own environment is no parameter of the type.)
    for (size_t I = Value ? 1 : 0;
         I < Code->elems().size() && S.size() <= MaxNameLength;
         ++I, ++Work.TypeSteps) {
      S += I > (Value ? 1u : 0u) ? ", " : "";
      S += !Value && Code->hasEnv() && I == 0 ? std::string("env")
                                              : spelling(Code->elems()[I]);
    }
    S += ")";
    if (IsFunc) {
      if (!Code->ret()->isUnit())
        S += " " + spelling(Code->ret());
      if (Code->fails())
        S += "!";
    }
    return S;
  }
  case K::Struct: {
    // An unnamed struct: its fields.
    std::string S = "struct {";
    for (size_t I = 0; I < T->elems().size() && S.size() <= MaxNameLength;
         ++I, ++Work.TypeSteps) {
      llvm::StringRef F = T->fieldName(I);
      S += (I ? "; " : " ") + (F.empty() ? "_" + std::to_string(I) : F.str()) +
           " " + spelling(T->elems()[I]);
    }
    return S + (T->elems().empty() ? "}" : " }");
  }
  }
  return T->name().str();
}

std::string DebugInfoBuilder::spelling(const G::Type *T) {
  if (auto It = Spelled.find(T); It != Spelled.end())
    return It->second;
  ++Work.TypeSteps;
  // A struct that is among its own type arguments, behind a pointer or
  // deeper (the instances of a generic type whose fields do not use its type
  // parameters share one struct, Lowering::lowerStruct), is its plain name
  // there: each type is spelled once, not without end.
  if (T->isNominal())
    Spelled[T] = T->name().str();
  std::string S;
  if (!Walk.level([&] { S = spell(T); }))
    S = "<too deep>"; // the lowering reports the depth (failedAt)
  // Two unnamed structs can have one spelling (`struct { a int }` and
  // `struct { a int64 }`): then they have one layout, and are one DWARF type.
  if (!T->isNominal() || isAnonymousStruct(T))
    S = capped(std::move(S), T);
  Spelled[T] = S;
  return S;
}

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

llvm::DIType *DebugInfoBuilder::typeOf(const G::Type *T) {
  if (auto It = Types.find(T); It != Types.end() && It->second.Full)
    return It->second.Full;
  makeType(T);
  return Types[T].Full;
}

/// A part that lies inside the type being made: its definition, unless that
/// is MaxTypeHeight high or more (LLVM's DWARF writer recurses once per level
/// of a chain of types); then what a pointer to it refers to (target), a few
/// levels high. The heights are the parts' own, so a type is made once.
llvm::DIType *DebugInfoBuilder::ref(const G::Type *P, unsigned &Height) {
  ++Work.TypeSteps;
  llvm::DIType *Full = typeOf(P);
  unsigned H = Types[P].Height;
  if (H < MaxTypeHeight) {
    Height = std::max(Height, H + 1);
    return Full;
  }
  return target(P, Height);
}

/// What a pointer to P refers to (and a function type's parameters and
/// result, and a part that is too high, ref):
///
///   - a struct-shaped P: the declaration of its name, a leaf, and the unit
///     keeps P's definition (keep), where a debugger finds it by that name.
///     A cycle of types passes through a pointer or a function to a struct
///     or a self-naming type, so the definitions form no cycle;
///   - a pointer, a function or a scalar: its definition (which refers to its
///     own parts this way); an array: the array of what its element is here,
///     so it never holds a struct being made (`S{ p *[2]S }`);
///   - one of those higher than MaxTypeHeight (a chain of pointers, arrays
///     and functions with no struct in it, `type P2 *P1`, ...): the
///     declaration of a struct of its name that holds it as `value` (wrap).
///
/// So a pointer or an array is never a declaration itself: a debugger
/// dereferences and indexes it. Each case depends on P alone, never on which
/// type was asked for first.
llvm::DIType *DebugInfoBuilder::target(const G::Type *P, unsigned &Height) {
  ++Work.TypeSteps;
  if (isStructShaped(P)) {
    keep(P);
    Height = std::max(Height, 2u);
    return declOf(P);
  }
  llvm::DIType *R;
  unsigned H;
  if (P->getKind() == G::Type::Kind::Array) {
    R = arrayTarget(P);
    H = Types[P].TargetHeight;
  } else {
    R = typeOf(P);
    H = Types[P].Height;
  }
  if (H < MaxTypeHeight) {
    Height = std::max(Height, H + 1);
    return R;
  }
  wrap(P, R);
  Height = std::max(Height, 2u);
  return declOf(P);
}

/// What a pointer to array T refers to: the array of its element's target.
/// Once per type, on a DeepWalk level (arrays nest as deep as the chain of
/// names of the source).
llvm::DIType *DebugInfoBuilder::arrayTarget(const G::Type *T) {
  if (auto It = Types.find(T); It != Types.end() && It->second.Target)
    return It->second.Target;
  ++Work.TypeSteps;
  llvm::DIType *R = nullptr;
  unsigned Height = 1;
  if (!Walk.level([&] {
        llvm::DIType *Elem = target(T->elem(), Height);
        R = arrayOf(T, spelling(T), Elem);
      })) {
    R = DIB.createBasicType("<too deep>", 8, llvm::dwarf::DW_ATE_unsigned);
    Height = 1;
  }
  TypeInfo &I = Types[T];
  I.Target = R;
  I.TargetHeight = Height;
  return R;
}

llvm::DIType *DebugInfoBuilder::arrayOf(const G::Type *T,
                                        const std::string &Name,
                                        llvm::DIType *Elem) {
  llvm::Metadata *Sub =
      DIB.getOrCreateSubrange(0, static_cast<int64_t>(T->length()));
  return DIB.createArrayType(nullptr, Name, nullptr, 0, allocBits(T),
                             alignBits(T), Elem, DIB.getOrCreateArray({Sub}));
}

/// The declaration of the struct named T's spelling: T's definition for a
/// struct-shaped T, the struct that holds it otherwise (wrap).
llvm::DIType *DebugInfoBuilder::declOf(const G::Type *T) {
  if (auto It = Types.find(T); It != Types.end() && It->second.Decl)
    return It->second.Decl;
  llvm::DIType *D = DIB.createForwardDecl(llvm::dwarf::DW_TAG_structure_type,
                                          spelling(T), nullptr, nullptr, 0);
  Types[T].Decl = D;
  return D;
}

/// The unit keeps the definition of struct-shaped T (finish makes it if no
/// one has): a declaration of its name stands for it.
void DebugInfoBuilder::keep(const G::Type *T) {
  TypeInfo &I = Types[T];
  if (I.Kept)
    return;
  I.Kept = true;
  Pending.push_back(T);
}

/// The struct named T's spelling that holds Inner (what a pointer to T refers
/// to) as its one member, `value`, kept in the unit.
void DebugInfoBuilder::wrap(const G::Type *T, llvm::DIType *Inner) {
  if (Types[T].Wrapped)
    return;
  Types[T].Wrapped = true;
  uint64_t Size = allocBits(T);
  llvm::Metadata *Member[] = {member(nullptr, "value", Inner, 0, Size)};
  Kept.push_back(DIB.createStructType(nullptr, spelling(T), nullptr, 0, Size,
                                      alignBits(T), llvm::DINode::FlagZero,
                                      nullptr, DIB.getOrCreateArray(Member)));
}

llvm::DIBasicType *DebugInfoBuilder::intType(unsigned Bits, bool Signed) {
  // A byte is a character to the debugger, so a `*uint8` (the data of a
  // string) shows its text.
  unsigned Enc = Bits == 8 && !Signed ? llvm::dwarf::DW_ATE_unsigned_char
                 : Signed             ? llvm::dwarf::DW_ATE_signed
                                      : llvm::dwarf::DW_ATE_unsigned;
  return DIB.createBasicType((Signed ? "int" : "uint") + std::to_string(Bits),
                             Bits, Enc);
}

llvm::DIDerivedType *DebugInfoBuilder::member(llvm::DIScope *Scope,
                                              llvm::StringRef Name,
                                              llvm::DIType *Ty, uint64_t Offset,
                                              uint64_t Size) {
  return DIB.createMemberType(Scope, Name, nullptr, 0, Size, 0, Offset,
                              llvm::DINode::FlagZero, Ty);
}

llvm::DISubroutineType *DebugInfoBuilder::subroutineType(const G::Type *Sig,
                                                         unsigned &Height) {
  llvm::SmallVector<llvm::Metadata *, 8> Ts;
  // The result first (null: none), then the parameters, as DWARF has it. The
  // hidden result pointer of an aggregate result is no parameter here. A
  // function is a pointer to code: its types are referred to as a pointer
  // refers to its target (`type F struct { f func(F) }` is a cycle).
  Ts.push_back(Sig->ret()->isUnit() ? nullptr : target(Sig->ret(), Height));
  for (const G::Type *P : Sig->elems())
    Ts.push_back(target(P, Height));
  // A Go++ function is swiftcc on every target (P04.18, ADR-0030): the
  // debugger must not take its results for the C convention's.
  return DIB.createSubroutineType(
      DIB.getOrCreateTypeArray(Ts), llvm::DINode::FlagPrototyped,
      Sig->callConv() == G::CallConv::Go ? llvm::dwarf::DW_CC_LLVM_Swift : 0);
}

void DebugInfoBuilder::makeType(const G::Type *T) {
  // Only a pointer or a function leads back to a type (GIR types have finite
  // sizes), and those refer to their targets without making them.
  assert(!Types[T].Making && "a type that holds itself");
  Types[T].Making = true;
  llvm::DIType *R = nullptr;
  unsigned Height = 1;
  bool Ran = Walk.level([&] {
    ++Work.TypeSteps;
    // The parts that lie inside the type first: the data layout sizes a type
    // from the sizes of its parts, so asking for the outer one first would
    // recurse once per level of a deep chain, inside LLVM, on this stack.
    // Built bottom up, each size is a lookup. (A pointer is no part: it is
    // reached when the type is built, and its target may be this type.)
    switch (T->getKind()) {
    case G::Type::Kind::Array:
    case G::Type::Kind::Option:
      typeOf(T->elem());
      break;
    case G::Type::Kind::Tuple:
    case G::Type::Kind::Struct:
      for (const G::Type *Part : T->elems())
        typeOf(Part);
      break;
    default:
      break;
    }
    R = build(T, spelling(T), Height);
  });
  if (!Ran) {
    // The lowering reports the depth (failedAt) and makes no module.
    R = DIB.createBasicType("<too deep>", 8, llvm::dwarf::DW_ATE_unsigned);
    Height = 1;
  }
  TypeInfo &I = Types[T];
  I.Making = false;
  I.Full = R;
  I.Height = Height;
}

// The size and alignment of an array come from its element's, kept here: the
// data layout asks an LLVM array for its element's size again at every
// level, and a chain of named arrays is thousands of levels deep.
uint64_t DebugInfoBuilder::allocBits(const G::Type *T) {
  if (T->getKind() == G::Type::Kind::Array) {
    if (auto It = ArraySize.find(T); It != ArraySize.end())
      return It->second;
    const G::Type *E = T->elem();
    uint64_t Elem = allocBits(E), A = alignBits(E);
    uint64_t Bits = T->length() * ((Elem + A - 1) / A * A);
    ArraySize[T] = Bits;
    return Bits;
  }
  return LM.getDataLayout().getTypeAllocSizeInBits(MemTy(T));
}

uint32_t DebugInfoBuilder::alignBits(const G::Type *T) {
  while (T->getKind() == G::Type::Kind::Array)
    T = T->elem();
  return LM.getDataLayout().getABITypeAlign(MemTy(T)).value() * 8;
}

llvm::DIType *DebugInfoBuilder::build(const G::Type *T, const std::string &Name,
                                      unsigned &Height) {
  using K = G::Type::Kind;
  const llvm::DataLayout &DL = LM.getDataLayout();
  llvm::Type *Mem = MemTy(T);
  uint64_t Size = allocBits(T);
  uint32_t Align = alignBits(T);
  unsigned PtrBits = DL.getPointerSizeInBits();
  auto Struct = [&](llvm::ArrayRef<llvm::Metadata *> Fields) {
    return DIB.createStructType(nullptr, Name, nullptr, 0, Size, Align,
                                llvm::DINode::FlagZero, nullptr,
                                DIB.getOrCreateArray(Fields));
  };
  // The fields of a type that lives in memory as an LLVM struct, with the
  // names and types given: offsets come from the data layout.
  auto Fields =
      [&](llvm::ArrayRef<std::pair<std::string, const G::Type *>> Fs) {
        const llvm::StructLayout *SL =
            DL.getStructLayout(llvm::cast<llvm::StructType>(Mem));
        llvm::SmallVector<llvm::Metadata *, 8> Out;
        for (size_t I = 0; I < Fs.size(); ++I) {
          ++Work.TypeSteps;
          const G::Type *FT = Fs[I].second;
          // A member is a level of its own to the DWARF writer.
          unsigned Member = 1;
          llvm::DIType *Ty = ref(FT, Member);
          Height = std::max(Height, Member + 1);
          Out.push_back(member(nullptr, Fs[I].first, Ty,
                               SL->getElementOffsetInBits(I), allocBits(FT)));
        }
        return Out;
      };
  auto Pointer = [&](const std::string &N) {
    return DIB.createPointerType(target(T->elem(), Height), PtrBits, 0,
                                 std::nullopt, N);
  };
  auto Array = [&](const std::string &N) {
    return arrayOf(T, N, ref(T->elem(), Height));
  };
  // A self-naming pointer or array (`type L [1]*L`, `type O Option[*O]` of a
  // pointer kind) is a cycle of pointers and arrays only, and a declaration
  // of its name must name a struct to be found: so the type is a struct of
  // its name with the pointer or array as its one member, `value`. A slice,
  // string, tuple or Option is a struct already.
  if (T->isRec() && (T->getKind() == K::Ptr || T->getKind() == K::Array)) {
    std::string Body =
        T->getKind() == K::Ptr
            ? "*" + spelling(T->elem())
            : "[" + std::to_string(T->length()) + "]" + spelling(T->elem());
    llvm::DIType *Inner = T->getKind() == K::Ptr
                              ? static_cast<llvm::DIType *>(Pointer(Body))
                              : Array(Body);
    Height += 2; // the member and the struct
    llvm::Metadata *Member[] = {member(nullptr, "value", Inner, 0, Size)};
    return Struct(Member);
  }
  G::Context &Ctx = GM.Ctx;
  switch (T->getKind()) {
  case K::Unit:
    return Struct({});
  case K::Bool:
    return DIB.createBasicType("bool", 8, llvm::dwarf::DW_ATE_boolean);
  case K::Int:
    return intType(T->bits(), T->isSigned());
  case K::Float:
    return DIB.createBasicType(Name, T->bits(), llvm::dwarf::DW_ATE_float);
  case K::String:
  case K::Slice: {
    // {ptr, len, cap} (ADR-0026), and for a slice the allocator that owns its
    // buffer (ADR-0029: 32 bytes): the GIR type's own fields, so the DWARF
    // follows its layout.
    static const char *const Words[] = {"ptr", "len", "cap", "alloc"};
    llvm::SmallVector<std::pair<std::string, const G::Type *>, 4> Fs;
    for (unsigned I = 0; I < T->numFields() && I < 4; ++I)
      Fs.push_back({Words[I], T->fieldType(I, Ctx)});
    return Struct(Fields(Fs));
  }
  case K::Ptr:
    return Pointer(Name);
  case K::Array:
    return Array(Name);
  case K::Option:
    // T followed by the one-byte "is set" flag (D4.5).
    return Struct(Fields({{"value", T->elem()}, {"set", Ctx.boolean()}}));
  case K::Tuple: {
    if (T->isFuncValue()) {
      // A func value (P04.07): the code and its environment. The environment
      // is shown as an address (the byte pointer GIR has would print text):
      // for a closure it points at the pointers to the captured variables.
      llvm::DIType *Env = DIB.createPointerType(nullptr, PtrBits);
      Height = std::max(Height, 3u);
      llvm::Metadata *Parts[] = {
          member(nullptr, "code", typeOf(T->elems()[0]), 0, PtrBits),
          member(nullptr, "env", Env, PtrBits, PtrBits)};
      return Struct(Parts);
    }
    llvm::SmallVector<std::pair<std::string, const G::Type *>, 4> Fs;
    for (size_t I = 0; I < T->elems().size(); ++I)
      Fs.push_back({"_" + std::to_string(I), T->elems()[I]});
    return Struct(Fields(Fs));
  }
  case K::Struct: {
    if (T == Ctx.iface()) {
      // An interface value (P04.06): its two words, the itab and the data
      // (a value of up to a word, or the address of one), shown as addresses:
      // as the byte pointers GIR has, a debugger would print text there.
      llvm::DIType *Word = DIB.createPointerType(nullptr, PtrBits);
      Height = std::max(Height, 3u);
      llvm::Metadata *Words[] = {
          member(nullptr, "itab", Word, 0, PtrBits),
          member(nullptr, "data", Word, PtrBits, PtrBits)};
      return Struct(Words);
    }
    llvm::SmallVector<std::pair<std::string, const G::Type *>, 8> Fs;
    for (size_t I = 0; I < T->elems().size(); ++I) {
      llvm::StringRef N = T->fieldName(I);
      Fs.push_back(
          {N.empty() ? "_" + std::to_string(I) : N.str(), T->elems()[I]});
    }
    return Struct(Fields(Fs));
  }
  case K::Func: {
    // A function value is the address of the function.
    llvm::DISubroutineType *Sub = subroutineType(T, Height);
    ++Height;
    return DIB.createPointerType(Sub, PtrBits, 0, std::nullopt, Name);
  }
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Functions, scopes, locations, variables
//===----------------------------------------------------------------------===//

llvm::DIFile *DebugInfoBuilder::fileOf(const G::Span &S) const {
  return S.isValid() && S.File <= Files.size() ? Files[S.File - 1] : Files[0];
}

llvm::DIFile *DebugInfoBuilder::fileOfFunction(const G::Function &F) const {
  return fileOf(F.Loc);
}

llvm::DISubprogram *DebugInfoBuilder::subprogramOf(G::FuncId Id) {
  if (auto It = Funcs.find(Id); It != Funcs.end() && It->second.SP)
    return It->second.SP;
  const G::Function &F = *GM.Funcs[Id];
  llvm::DIFile *File = fileOfFunction(F);
  std::string Name = readableName(F.Name);
  unsigned Height = 1;
  llvm::DISubroutineType *Ty = subroutineType(F.Sig, Height);
  unsigned Flags = llvm::DISubprogram::SPFlagDefinition;
  if (F.Link == G::Linkage::Internal)
    Flags |= llvm::DISubprogram::SPFlagLocalToUnit;
  if (Optimized)
    Flags |= llvm::DISubprogram::SPFlagOptimized;
  // Code the compiler wrote is artificial: the runtime and glue, which have
  // no source position, and the wrappers, which have the position of what
  // they wrap (an interface thunk, the method's).
  llvm::DINode::DIFlags DF = llvm::DINode::FlagPrototyped;
  if (!F.Loc.isValid() || isWrapper(F.Name))
    DF |= llvm::DINode::FlagArtificial;
  llvm::DISubprogram *SP = DIB.createFunction(
      File, Name,
      LinkageNames && Name != F.Name ? llvm::StringRef(F.Name)
                                     : llvm::StringRef(),
      File, F.Loc.Line, Ty, F.Loc.Line, DF,
      static_cast<llvm::DISubprogram::DISPFlags>(Flags));
  Funcs[Id].SP = SP;
  return SP;
}

/// The function's own scopes in DWARF. A block of the source that holds a
/// variable is a lexical block; one without is the nearest enclosing one
/// (the debugger has nothing to show in it). At most MaxBlockDepth blocks
/// nest: LLVM walks the chain of scopes of each location and each variable
/// (its verifier, and its passes at `opt 2`), so the work would grow with the
/// depth times the length (debug-info.md section 1). A deeper block's
/// variables are in the deepest block, which several blocks then share: one
/// of them whose name another variable of the function has is shown with no
/// location (`<optimized out>`), as the debugger could not tell it from the
/// other one, or from an outer one it hides, and would show a wrong value.
DebugInfoBuilder::FuncInfo &DebugInfoBuilder::funcInfo(G::FuncId Id) {
  llvm::DISubprogram *SP = subprogramOf(Id);
  FuncInfo &FI = Funcs[Id];
  if (FI.HasNative)
    return FI;
  const G::Function &F = *GM.Funcs[Id];
  size_t N = F.Scopes.size();
  FI.Native.assign(N + 1, nullptr);
  FI.Native[0] = SP;
  std::vector<uint8_t> HasVars(N + 1, 0), Shared(N + 1, 0);
  // Per scope: the number of lexical blocks around its variables, and the
  // scope whose lexical block holds them (0: the function).
  std::vector<uint32_t> Depth(N + 1, 0), Holder(N + 1, 0);
  auto Own = [&](const G::Local &Lo) {
    return Lo.Origin == G::NoId && Lo.Scope <= N && isVariable(F, Lo);
  };
  for (const G::Local &Lo : F.Locals) {
    ++Work.ScopeSteps;
    if (Own(Lo))
      HasVars[Lo.Scope] = 1;
  }
  bool AnyShared = false;
  for (size_t I = 0; I < N; ++I) {
    ++Work.ScopeSteps;
    const G::DebugScope &D = F.Scopes[I];
    // A block of the function's own lies in its own blocks (V073); the inline
    // sites and what they hold are their callees'.
    if (D.K != G::DebugScope::Block || D.Origin != 0)
      continue;
    llvm::DIScope *Parent = FI.Native[D.Parent];
    Depth[I + 1] = Depth[D.Parent];
    Holder[I + 1] = Holder[D.Parent];
    if (HasVars[I + 1] && Depth[I + 1] < MaxBlockDepth) {
      Parent = DIB.createLexicalBlock(
          Parent, D.Loc.isValid() ? fileOf(D.Loc) : fileOfFunction(F),
          D.Loc.Line, D.Loc.Col);
      ++Depth[I + 1];
      Holder[I + 1] = I + 1;
    } else if (HasVars[I + 1]) {
      Shared[I + 1] = Shared[Holder[I + 1]] = 1;
      AnyShared = true;
    }
    FI.Native[I + 1] = Parent;
  }
  if (AnyShared) {
    llvm::StringMap<unsigned> Names;
    for (const G::Local &Lo : F.Locals) {
      ++Work.ScopeSteps;
      if (Own(Lo))
        ++Names[Lo.Name];
    }
    FI.Hidden.assign(F.Locals.size(), 0);
    for (size_t L = 0; L < F.Locals.size(); ++L) {
      ++Work.ScopeSteps;
      const G::Local &Lo = F.Locals[L];
      if (Own(Lo) && Shared[Lo.Scope] && Names[Lo.Name] > 1)
        FI.Hidden[L] = 1;
    }
  }
  FI.HasNative = true;
  return FI;
}

void DebugInfoBuilder::declareGlobal(const G::Global &Gl,
                                     llvm::GlobalVariable *GV) {
  // Constant data the compiler made is no variable, nor is a variable of the
  // compiler's own (the map seed of the runtime: no source position).
  if (Gl.ReadOnly || !Gl.Loc.isValid())
    return;
  std::string Name = readableName(Gl.Name);
  llvm::DIGlobalVariableExpression *E = DIB.createGlobalVariableExpression(
      CU, Name,
      LinkageNames && Name != Gl.Name ? llvm::StringRef(Gl.Name)
                                      : llvm::StringRef(),
      fileOf(Gl.Loc), Gl.Loc.Line, typeOf(Gl.Ty),
      Gl.Link == G::Linkage::Internal);
  GV->addDebugInfo(E);
}

void DebugInfoBuilder::beginFunction(G::FuncId Id, llvm::Function &LF) {
  CurId = Id;
  CurFn = GM.Funcs[Id].get();
  llvm::DISubprogram *SP = funcInfo(Id).SP;
  LF.setSubprogram(SP);
  // Code with no source position (the runtime, glue) is line 0 throughout.
  // gdb skips a row of line 0 and reads the row before it, the last line of
  // the function before this one: in a section of its own, such code is a
  // line sequence of its own, so it has no line (lldb shows line 0 either
  // way). The linker joins the section to .text (review R8).
  if (!CurFn->Loc.isValid() && LM.getTargetTriple().isOSBinFormatELF())
    LF.setSection(NoSourceSection);
  size_t N = CurFn->Scopes.size();
  Table.assign(N + 1, ScopeInfo());
  Table[0] = {SP, nullptr, Id};
  // Parents come first (V073), so one pass in order fills the table.
  for (size_t I = 0; I < N; ++I) {
    ++Work.ScopeSteps;
    const G::DebugScope &D = CurFn->Scopes[I];
    ScopeInfo Up = Table[D.Parent];
    ScopeInfo &E = Table[I + 1];
    if (D.K == G::DebugScope::Inlined) {
      // The callee's subprogram, inlined at the call: the call's position
      // in the scope that held it (line 0 if it is no position of that
      // scope's function), one distinct location per site, as LLVM's inliner
      // makes them (two inlined calls on one line are two frames).
      const G::Function &Holder = *GM.Funcs[Up.Owner];
      bool Ours = D.Loc.isValid() &&
                  (!Holder.Loc.isValid() || D.Loc.File == Holder.Loc.File);
      E.Owner = D.Callee;
      E.Scope = subprogramOf(D.Callee);
      E.InlinedAt = llvm::DILocation::getDistinct(C, Ours ? D.Loc.Line : 0,
                                                  Ours ? D.Loc.Col : 0,
                                                  Up.Scope, Up.InlinedAt);
    } else {
      // A block of the function itself, or the copy of a block of the
      // callee that holds it, which is that callee's own block (V073).
      E.Owner = Up.Owner;
      E.InlinedAt = Up.InlinedAt;
      E.Scope = D.Origin == 0 ? funcInfo(Id).Native[I + 1]
                              : funcInfo(Up.Owner).Native[D.Origin];
    }
  }
}

llvm::DILocation *DebugInfoBuilder::location(const G::Span &S,
                                             G::ScopeId Scope) {
  ++Work.LocationSteps;
  const ScopeInfo &E = Table[Scope];
  const G::Function &Owner = *GM.Funcs[E.Owner];
  // Code the compiler made has no position: line 0, as in clang. A span of
  // another file than the one of the scope's function would name a line the
  // scope does not have.
  bool Ours = S.isValid() && (!Owner.Loc.isValid() || S.File == Owner.Loc.File);
  assert((!S.isValid() || Ours) && "a span of another file in this scope");
  return llvm::DILocation::get(C, Ours ? S.Line : 0, Ours ? S.Col : 0, E.Scope,
                               E.InlinedAt);
}

/// The function and the local of its own that local L of the current function
/// is: L itself, or for a copy GIR inlining made, the callee's local (V074).
/// What a copy is (a variable, a parameter, in which scope) is the callee's,
/// never the function it was inlined into: a method inlined into its interface
/// thunk, which has no position, keeps its variables.
std::pair<G::FuncId, G::LocalId>
DebugInfoBuilder::originOf(G::LocalId L) const {
  const G::Local &Lo = CurFn->Locals[L];
  if (Lo.Origin == G::NoId)
    return {CurId, L};
  return {Table[Lo.Scope].Owner, Lo.Origin};
}

bool DebugInfoBuilder::isUserVariable(G::LocalId L) const {
  auto [Owner, Origin] = originOf(L);
  const G::Function &F = *GM.Funcs[Owner];
  return isVariable(F, F.Locals[Origin]);
}

/// The variable of local L of function Owner, one of Owner's own (V074: the
/// copy of an inlined callee's local names a local of the callee's own), in
/// the scope it is declared in. A parameter, or a variable that stands for one
/// (`ArgNo`: the slice a borrowed `[]T` parameter's (ptr, len) pair is made
/// into, P04.25), is the function's parameter of that position.
llvm::DILocalVariable *DebugInfoBuilder::variable(G::FuncId Owner,
                                                  G::LocalId L) {
  uint64_t Key = (uint64_t(Owner) << 32) | L;
  if (auto It = Variables.find(Key); It != Variables.end())
    return It->second;
  const G::Function &F = *GM.Funcs[Owner];
  const G::Local &Lo = F.Locals[L];
  llvm::DIScope *Scope = funcInfo(Owner).Native[Lo.Scope];
  llvm::DIFile *File = Lo.Loc.isValid() ? fileOf(Lo.Loc) : fileOfFunction(F);
  // A pointer that stands for its place is shown as that place (Deref).
  llvm::DIType *Ty = typeOf(Lo.Deref && Lo.Ty->isPtr() ? Lo.Ty->elem() : Lo.Ty);
  // A parameter has no position of its own: the function's line.
  unsigned Line = Lo.Loc.isValid() ? Lo.Loc.Line : F.Loc.Line;
  // Kept even when an optimizer drops every use: the debugger then says
  // "optimized out" instead of not knowing the name.
  unsigned Arg = Lo.ArgNo ? Lo.ArgNo : Lo.K == G::Local::Arg ? L : 0;
  llvm::DILocalVariable *V =
      Arg ? DIB.createParameterVariable(Scope, Lo.Name, Arg, File, Line, Ty,
                                        /*AlwaysPreserve=*/true)
          : DIB.createAutoVariable(Scope, Lo.Name, File, Line, Ty,
                                   /*AlwaysPreserve=*/true);
  Variables[Key] = V;
  return V;
}

llvm::Value *DebugInfoBuilder::declareLocal(G::LocalId L, llvm::Value *Addr,
                                            llvm::Instruction *InsertBefore) {
  ++Work.VariableSteps;
  if (!Addr || !isUserVariable(L))
    return nullptr;
  const G::Local &Lo = CurFn->Locals[L];
  // A local the inliner copied is the callee's variable, seen in the
  // callee's scope at the inlined call.
  auto [Owner, Origin] = originOf(L);
  llvm::DILocalVariable *Var = variable(Owner, Origin);
  // A variable of a block past MaxBlockDepth that a debugger could take for
  // another one of its name: no location, so `<optimized out>` (funcInfo).
  if (const std::vector<uint8_t> &Hidden = funcInfo(Owner).Hidden;
      !Hidden.empty() && Hidden[Origin])
    return nullptr;
  llvm::DILocation *Loc = location(Lo.Loc, Lo.Scope);
  llvm::Value *Where = Addr;
  llvm::Value *Keep = nullptr;
  llvm::DIExpression *Expr = DIB.createExpression();
  if (llvm::isa<llvm::Argument>(Addr)) {
    // An aggregate parameter is the address the caller passed, in a
    // register nothing keeps: a slot of its own holds it, and the variable
    // is read through it, as Clang does for an indirect argument.
    llvm::Function *LF = InsertBefore->getFunction();
    llvm::IRBuilder<> AB(&LF->getEntryBlock(), LF->getEntryBlock().begin());
    llvm::AllocaInst *Slot =
        AB.CreateAlloca(Addr->getType(), nullptr, Lo.Name + ".addr");
    llvm::IRBuilder<> B(InsertBefore);
    B.CreateStore(Addr, Slot);
    Where = Keep = Slot;
    uint64_t Ops[] = {llvm::dwarf::DW_OP_deref};
    Expr = DIB.createExpression(Ops);
  } else if (Lo.Deref && Lo.Ty->isPtr()) {
    // The slot holds the pointer: the variable is what it points to (a
    // parameter passed by a hidden pointer, PAR-10; a `&` range variable).
    uint64_t Ops[] = {llvm::dwarf::DW_OP_deref};
    Expr = DIB.createExpression(Ops);
    Keep = Addr;
  } else if (Lo.Origin == G::NoId || allocBits(Lo.Ty) <= MaxKeptBits) {
    Keep = Addr;
  }
  DIB.insertDeclare(Where, Var, Expr, Loc, InsertBefore->getIterator());
  return Keep;
}
