//===- LowerImpl.h - The classes behind gir::lowerPackage -----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Private to lib/GIR. The lowering is split by what it decides:
//
//   Lower.cpp      Lowering: types (lowerType), declarations, package
//                  variables, init order, the entry point.
//   LowerExpr.cpp  Body: places and values (lowerPlace, lowerExpr), operators
//                  and checks, conversions, calls, Option, composite literals.
//   LowerStmt.cpp  Body: statements, control flow, switch, labels.
//   Runtime.cpp    Lowering: the functions the compiler writes in GIR (print
//                  helpers, panic functions, complex division).
//
// Decision points, one per rule (docs/design/gir.md section 15):
//   a checker type to a GIR type      Lowering::lowerType
//   the symbol of an entity           gir::symbolOf / gir::Symbols
//   what a place is                   Body::lowerPlace
//   a check (D4.12)                   Body::arith, Body::indexPlace
//   Option layout and conversion      Lowering::lowerType, Body::wrapSome
//   == and the hash of any type       gir::addEqGlue / gir::addHashGlue
//   a construct outside the subset    Lowering::unsupported (E0439)
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_LIB_GIR_LOWERIMPL_H
#define GOPP_LIB_GIR_LOWERIMPL_H

#include "Symbols.h"

#include "gopp/AST/AST.h"
#include "gopp/Basic/NewStack.h"
#include "gopp/GIR/Builder.h"
#include "gopp/GIR/CType.h"
#include "gopp/GIR/Glue.h"
#include "gopp/GIR/Lower.h"
#include "gopp/Sema/Constant.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"

namespace gopp {
namespace gir {
namespace impl {

namespace S = gopp::sema;
using llvm::ArrayRef;
using llvm::SmallVector;
using llvm::StringRef;

class Body;

/// The steps of the lowering walks that do not make one GIR statement each
/// (the late reads of an assignment's targets, its stores, a range clause,
/// labelPos, exitScopes, the scopes a goto leaves): gir::loweringSteps reads
/// it (test-only, M8).
extern thread_local uint64_t LowerSteps;

/// The runtime functions the compiler writes in GIR for strings, slices and
/// maps (RuntimeStr.cpp, RuntimeMap.cpp). Each is made on first use. The
/// signatures are the contract with the code that calls them (LowerStr.cpp);
/// P05 replaces the bodies, not the calls. gir.md section 15 lists them.
enum class RtFn : unsigned {
  MemResize,   // gopp.mem_resize(p *u8, size u64, zero bool) *u8  (global)
  MemResizeIn, // gopp.mem_resize_in(alloc *u8, p *u8, old u64, size u64,
               // zero bool) *u8   (alloc null: mem_resize; ADR-0029)
  MemFreeIn,   // gopp.mem_free_in(alloc *u8, p *u8, size u64)  (p null: no-op)
  SliceClone,  // gopp.slice_clone(alloc *u8, p *u8, len, esz u64) *u8
  SliceFree,   // gopp.slice_free(alloc *u8, p *u8, cap, esz u64)
  TestAlloc,   // gopp.test_alloc/_resize/_free(self *u8, ...): the counting
  TestResize,  // allocator's table entries (Opts.TestIntrinsics only)
  TestFree,
  PanicText,    // gopp.panic_text(kind i32, text *u8, len u64)  [noreturn]
  StrConcat,    // gopp.str_concat(parts *string, n u64) string
  StrEq,        // gopp.str_eq(ap *u8, an u64, bp *u8, bn u64) bool
  StrCmp,       // gopp.str_cmp(ap, an, bp, bn) i64   (-1, 0, 1)
  StrFromBytes, // gopp.str_from_bytes(p *u8, n u64) string   (copies)
  BytesFromStr, // gopp.bytes_from_str(p *u8, n u64) []u8     (copies)
  RunesFromStr, // gopp.runes_from_str(p *u8, n u64) []i32
  StrFromRunes, // gopp.str_from_runes(p *i32, n u64) string
  StrFromRune,  // gopp.str_from_rune(v i64) string
  DecodeRune,   // gopp.decode_rune(p *u8, n u64, k u64) (i32, u64)
  EncodeRune,   // gopp.encode_rune(buf *u8, r i32) u64
  RoundUpSize,  // gopp.round_up_size(size u64, noscan bool) u64
  GrowSlice,    // gopp.grow_slice(p *u8, len, cap, newlen, elemsize u64, noscan
                // bool) (*u8, u64)
  MakeSlice,   // gopp.make_slice(alloc *u8, elemsize u64, len i64, cap i64) *u8
  MemFree,     // gopp.mem_free(p *u8)  (beside mem_resize)
  MapNew,      // gopp.map_new(info *MapInfo, hint u64, alloc *u8) *Map
  MapFree,     // gopp.map_free(info, m *Map)  (the map's own allocator)
  MapFind,     // gopp.map_find(info, m, key, hash) *u8  (internal)
  MapGrow,     // gopp.map_grow(info, m)  (internal)
  MapGet,      // gopp.map_get(info, m *Map, key *u8) *u8   (null: absent)
  MapAssign,   // gopp.map_assign(info, mp **Map, key *u8) *u8
  MapDelete,   // gopp.map_delete(info, m *Map, key *u8)
  MapClear,    // gopp.map_clear(info, m *Map)
  MapIterInit, // gopp.map_iter_init(it *MapIter, m *Map)   (counts it)
  MapIterNext, // gopp.map_iter_next(info, it *MapIter, k **u8, v **u8) bool
  MapIterDone, // gopp.map_iter_done(it *MapIter)   (ends it; once)
  MapRand,     // gopp.map_rand() u64   (internal: the start of a range)
  MapEntryArgs, // gopp.map_entry_args(recs *MapEntryArg, n u64)  (copy-in and
                // aliasing of a call's map-entry arguments)
  MapEntryBack, // gopp.map_entry_back(recs *MapEntryArg, n u64)  (copy-out)
  MapKeyCheck,  // gopp.map_key_check(info *MapInfo, m *Map, key *u8)  (a key
                // that holds an interface, in a nil or empty map: Go's
                // mapKeyError, P04.06)
  // Interfaces (RuntimeIface.cpp, P04.06):
  GetItab,   // gopp.get_itab(desc *u8, ti *u8) *u8   (null: ti lacks a method)
  IfaceEq,   // gopp.iface_eq(a *iface, b *iface) bool
  IfaceHash, // gopp.iface_hash(p *iface, h u64) u64
  PanicAssert, // gopp.panic_assert(inter string, dyn *u8, asserted string,
               // desc *u8)  [noreturn]
  PanicUncomparable, // gopp.panic_uncomparable(ti *u8, hash bool)  [noreturn]
  Count
};

/// The fields of a type descriptor (`gopp.typeinfo`, gir.md section 15).
namespace TI {
enum : unsigned {
  Self,     // *u8: the descriptor's own address, so it is the itab of (T, any)
  Size,     // u64
  Name,     // string: Go's spelling of the type
  Eq,       // *u8: func(a, b *T) bool, or null (not comparable)
  Hash,     // *u8: func(p *T, seed u64) u64, or null
  Kind,     // u32: reflect.Kind numbers, then Option 27, View 28
  Flags,    // u32: bit 0 = the value is in the data word (no box)
  NMethods, // u64
  Methods,  // *u8: NMethods x {key string, fn *u8}, sorted by key
  Elem,     // *u8: P05 (reflection)
  Fields,   // *u8: P05 (TYP-10 field table)
  NFields,  // u64
  VTable,   // *u8: P04.22 (`extends` type table)
  Count
};
constexpr uint32_t FlagInline = 1;
} // namespace TI

class Lowering {
public:
  Lowering(Context &Ctx, const SourceManager &SM, const S::CheckedPackage &CP,
           ArrayRef<const ast::File *> Files, DiagnosticsEngine &Diags,
           const LowerOptions &Opts)
      : Ctx(Ctx), SM(SM), CP(CP), I(CP.I), Files(Files), Diags(Diags),
        Opts(Opts),
        M(std::make_unique<Module>(Ctx, CP.Pkg ? CP.Pkg->Path.str() : "")),
        Methods(CP, Opts.ScanMethodLookups) {
    for (FuncId &Id : RtIds)
      Id = NoId;
    for (auto &Row : Panics)
      Row[0] = Row[1] = NoId;
  }

  std::unique_ptr<Module> run();

  Context &Ctx;
  const SourceManager &SM;
  const S::CheckedPackage &CP;
  const S::Info &I;
  ArrayRef<const ast::File *> Files;
  DiagnosticsEngine &Diags;
  const LowerOptions &Opts;
  std::unique_ptr<Module> M;
  /// The checker's method sets and interface method lists (P04.06): every
  /// decision of which methods a type or an interface has is the checker's.
  S::MethodSets Methods;
  Symbols Syms;
  bool Failed = false;
  /// Errors reported so far (unsupported, badExtern, ...): a step that
  /// returns null must have raised it (lowerType checks).
  unsigned Reported = 0;
  /// Named types whose lowering is under way (a cycle through no struct).
  /// With the number of structs under way when it began.
  llvm::DenseMap<S::Type *, unsigned> InProgress;
  unsigned StructsActive = 0;
  /// Inside the key or value type of a map (no self-naming type is made for
  /// a type reached again there), and inside the second lowering of a type
  /// without a name that is under way.
  unsigned MapDepth = 0, RebuildDepth = 0;
  llvm::SmallPtrSet<S::Type *, 8> RecursiveReported;
  /// The self-naming GIR type made for a defined type that reached itself
  /// through no struct (its body is set when its lowering is done).
  llvm::DenseMap<S::Type *, Type *> RecShells;
  llvm::DenseMap<const S::Func *, FuncId> FuncIds;
  /// Package variables with storage, by checker object.
  llvm::DenseMap<const S::Var *, uint32_t> Globals;
  /// Functions whose declaration was reported: their calls are not reported
  /// again.
  llvm::DenseSet<const S::Func *> Declined;
  /// Which types C can take by value, memoized (P04.11).
  CTypeRules CRules;

  void unsupported(SourceLoc At, const llvm::Twine &What);
  /// An internal error (E0440): a fact the lowering needs is missing.
  void internalError(SourceLoc At, const llvm::Twine &What);
  void badExtern(SourceLoc At, StringRef Name, const llvm::Twine &Why);
  Span spanOf(SourceLoc L);

  /// The GIR type of a checked type; null after a report.
  Type *lowerType(S::Type *T, SourceLoc At);
  /// The type of Option[Elem] (S::OptionType) in GIR: the pointer itself for
  /// a pointer kind, else Option<T>. Null after a report.
  Type *lowerOption(S::OptionType *O, SourceLoc At);
  /// True for the checker types that are one machine pointer for `Option`:
  /// a raw pointer, unsafe.Pointer or a defined type over them.
  bool isRawPointer(S::Type *T);
  /// A type declared inside a function: the function whose body declares it.
  void declareLocalType(const S::TypeName *TN, const sym::Node *Scope,
                        unsigned Ordinal);
  static bool isComplexType(S::Type *T);

  // The functions the compiler writes (Runtime.cpp).
  FuncId ensureCFunc(StringRef Name, Type *Sig, SourceLoc At, bool NoReturn,
                     StringRef Who, StringRef Needs);
  FuncId ensureWrite(SourceLoc At);
  FuncId ensureExit(SourceLoc At);
  /// print_u64, print_i64, print_hex, print_f64 (fd i32, value).
  FuncId ensurePrintU64(SourceLoc At);
  FuncId ensurePrintI64(SourceLoc At);
  FuncId ensurePrintHex(SourceLoc At);
  FuncId ensurePrintF64(SourceLoc At);
  FuncId ensureComplexDiv(SourceLoc At);
  /// The panic function of an assert kind (Unsigned: the index is a u64).
  FuncId ensurePanic(AssertKind K, bool Unsigned, SourceLoc At);
  /// Go's == and the hash of a checker type (glue); NoId after a report.
  FuncId eqGlue(S::Type *T, SourceLoc At);
  /// Is parameter K of Sig a borrowed map (no `move`)? It passes by a hidden
  /// pointer to the owner's map header (PAR-10), so the callee's first write
  /// to a nil map creates it in the caller's variable (PAR-1, ADR-0018
  /// amendment point 5). The parameter's GIR type is a pointer to the map.
  bool mapByPointer(S::SignatureType *Sig, size_t K) const {
    return !Sig->isMove(K) && K < Sig->Params->Elems.size() &&
           llvm::isa<S::MapType>(S::under(Sig->Params->Elems[K]));
  }
  FuncId hashGlue(S::Type *T, SourceLoc At);

  /// The runtime function (made on first use); NoId after a report.
  FuncId rt(RtFn F, SourceLoc At);
  /// The map functions of RtFn (RuntimeMap.cpp).
  FuncId buildMapRuntime(RtFn F, SourceLoc At);
  /// The interface functions of RtFn (RuntimeIface.cpp).
  FuncId buildIfaceRuntime(RtFn F, SourceLoc At);
  /// The hash and equality of map keys of the checker type K, as functions of
  /// `*u8` (the runtime does not know K); NoId after a report.
  std::pair<FuncId, FuncId> mapKeyFns(S::Type *K, SourceLoc At);
  llvm::DenseMap<Type *, std::pair<FuncId, FuncId>> KeyFns;
  FuncId RtIds[unsigned(RtFn::Count)];
  /// The global that holds gopp.map_rand's state (made with it).
  uint32_t MapSeed = ~0u;
  /// The allocator word (ADR-0029): the table every allocator object starts
  /// with (alloc, resize, free), the counting objects of the test intrinsics
  /// (1 and 2; Opts.TestIntrinsics), and the global allocator's own counters
  /// (the three calls of mem_resize and mem_free).
  Type *allocTableType();
  Type *testAllocType();
  uint32_t testAllocGlobal(unsigned Which);
  uint32_t TestAllocs[2] = {~0u, ~0u};
  uint32_t TestGlobalCalls = ~0u;
  uint32_t testGlobalCalls();
  /// The test intrinsics (`gopp_test_*`) by checker function.
  enum class TestFn {
    MakeSlice,
    MakeMap,
    CloneSlice,
    FreeSlice,
    FreeMap,
    Count,
    Word
  };
  llvm::DenseMap<const S::Func *, TestFn> TestIntrinsics;
  /// Is parameter K of Sig a borrowed `[]T` (no `move`)? It passes as a view,
  /// (ptr, len), without the allocator word (ADR-0029 point 7, D4.4); the
  /// callee makes its own `[]T` local from it with a null word.
  bool sliceBorrowed(S::SignatureType *Sig, size_t K) const {
    return !Sig->isMove(K) && K < Sig->Params->Elems.size() &&
           llvm::isa<S::SliceType>(S::under(Sig->Params->Elems[K]));
  }
  /// Is the receiver of the method F a borrowed []T, a value receiver
  /// without `move` of a slice type (PAR-1)? It passes as (ptr, len), as a
  /// borrowed []T parameter does (P04.06).
  static bool recvBorrowedSlice(const S::Func *F) {
    return F->Recv && !F->HasPtrRecv && !F->MoveRecv &&
           llvm::isa<S::SliceType>(S::under(F->Recv->Ty));
  }
  /// The (ptr, len) a borrowed []T of the GIR slice type T passes as.
  Type *borrowedSliceType(Type *T) {
    return Ctx.tuple({T->fieldType(0, Ctx), Ctx.u64()});
  }
  /// The GIR type parameter K of Sig passes as, T being the type of its
  /// value: a borrowed map is a pointer to the caller's header (PAR-10), a
  /// borrowed []T is (ptr, len) (ADR-0029 point 7), anything else T. The one
  /// rule of declarations, method signatures and itab slots.
  Type *paramType(S::SignatureType *Sig, size_t K, Type *T) {
    if (mapByPointer(Sig, K))
      return Ctx.ptr(T);
    if (sliceBorrowed(Sig, K) && T->getKind() == Type::Kind::Slice)
      return borrowedSliceType(T);
    return T;
  }
  /// A libc function the runtime calls (malloc, memmove, ...).
  FuncId libc(StringRef Name, SourceLoc At);
  llvm::StringMap<FuncId> LibcIds;
  /// The size and alignment of a GIR type (gir::layoutOf, cached).
  Layout layout(Type *T);
  llvm::DenseMap<Type *, Layout> Layouts;
  /// The GIR type of a map (the runtime object), of the map info and of the
  /// iterator the runtime keeps in the caller's frame.
  Type *mapType();
  Type *mapInfoType();
  Type *mapIterType();
  /// One map-entry argument of a call (Body::mapEntryArg): {info, map, key,
  /// key kind, value, slot, wasnil, own}.
  Type *mapEntryArgType();

  //===--------------------------------------------------------------------===//
  // Methods and interfaces (LowerIface.cpp, P04.06; gir.md section 15)
  //===--------------------------------------------------------------------===//
  /// Is T an interface type that has values (not a constraint)?
  static bool isInterface(S::Type *T);
  /// An interface method of the flattened method set of an interface, or a
  /// method of a type's method set, with the key that tells two methods
  /// apart at run time: `Name \x01 [package path of an unexported name]
  /// \x01 mangled signature`. Sets are sorted by Key (bytes), so a type
  /// descriptor and an interface descriptor are walked together.
  struct MethodEntry {
    const S::Func *F = nullptr;
    std::string Key;
  };
  /// The methods of interface I in itab order (sorted by Key), and the slot
  /// of each key, so a call finds its slot in one lookup.
  struct IfaceMethods {
    SmallVector<MethodEntry, 4> List;
    llvm::StringMap<unsigned> Slot;
  };
  /// The methods of interface I: the checker's list (MethodSets, embedded
  /// interfaces flattened), sorted; null after a report. A method that fails
  /// (`!`, P04.10) or is an operator element (P04.23) is reported.
  const IfaceMethods *ifaceMethods(S::Type *I, SourceLoc At);
  /// The itab slot of the interface method F in Ms, or ~0u.
  unsigned ifaceSlot(const IfaceMethods &Ms, const S::Func *F);
  /// Do interfaces with the methods A and B have one itab layout (the same
  /// keys)? Cached per pair: N conversions between interfaces of N methods
  /// compare the lists once.
  bool sameItabs(const IfaceMethods &A, const IfaceMethods &B);
  llvm::DenseMap<std::pair<const IfaceMethods *, const IfaceMethods *>, bool>
      SameItabCache;
  /// The method set of the dynamic type Dyn, as the checker decides it
  /// (MethodSets: a value has the value-receiver methods, a pointer all of
  /// them, a `move` receiver and an operator method neither), sorted by Key.
  /// A method declined where it was declared is in it (it has no thunk; the
  /// users skip it). Null after a report: a method inherited through
  /// `extends` (P04.22).
  const SmallVector<MethodEntry, 4> *methodSet(S::Type *Dyn, SourceLoc At);
  std::string methodKey(const S::Func *F);
  /// The GIR function type of a method signature with the parameter Lead in
  /// front (a receiver, or the data word of an itab entry); null after a
  /// report.
  Type *methodSig(S::SignatureType *Sig, Type *Lead, SourceLoc At);
  /// Go's spelling of a type for panic messages and the descriptor (`main.T`,
  /// `[]int`, `*main.T`, `interface {}`, `map[string]int`).
  std::string rtypeName(S::Type *T);
  /// `(params) results` of a signature in that spelling (`(int, ...string)
  /// (bool, error)`).
  std::string sigText(S::SignatureType *SG);
  /// Can values of T be compared with == (Go's comparable, at run time for an
  /// interface)?
  bool comparable(S::Type *T);
  /// Does a value of the GIR type G live in the data word of an interface
  /// (at most one word: no box)? One decision point (TYP-8).
  bool isInline(Type *G) { return layout(G).Size <= 8; }
  /// The static data (type descriptors, method tables, itabs, interface
  /// descriptors): one global each, `LinkOnceODR`, named by P04.17. ~0u after
  /// a report.
  uint32_t typeInfo(S::Type *T, SourceLoc At);
  /// The == and hash a descriptor holds for a view (G its GIR type): those
  /// of the value it shows (OWN-V), as functions of the value's address; NoId
  /// when that value is not comparable (a slice, a map).
  std::pair<FuncId, FuncId> viewGlue(S::RefType *RT, Type *G, SourceLoc At);
  llvm::DenseMap<S::RefType *, std::pair<FuncId, FuncId>> ViewGlues;
  uint32_t itab(S::Type *Dyn, S::Type *I, SourceLoc At);
  uint32_t ifaceDesc(S::Type *I, SourceLoc At);
  /// The adapter that an itab calls for method F of Dyn: (data word,
  /// arguments) -> results. NoId after a report.
  FuncId ifaceThunk(S::Type *Dyn, const S::Func *F, SourceLoc At);
  Type *tiType();
  Type *mtEntType();
  uint32_t ItabCache = ~0u;
  uint32_t itabCacheGlobal();
  /// A u64 set while gopp.map_key_check hashes a key: the unhashable panic
  /// then says it as Go's map access of a nil or empty map does.
  uint32_t PlainHash = ~0u;
  uint32_t plainHashFlag();
  llvm::StringMap<uint32_t> StaticData;
  llvm::StringMap<FuncId> Thunks;
  /// (Owned by pointer: a reference stays valid while the maps grow.)
  llvm::DenseMap<S::Type *, std::unique_ptr<IfaceMethods>> IfaceMethodCache;
  llvm::DenseMap<S::Type *, std::unique_ptr<SmallVector<MethodEntry, 4>>>
      MethodSetCache;
  llvm::DenseSet<S::Type *> IfaceBad, MethodSetBad;
  llvm::DenseMap<const S::Func *, std::string> MethodKeys;

  /// A method this task lowers: refuses (E0439, naming the task) a generic
  /// receiver, a map receiver, an operator method and an `extends` mark.
  bool methodSupported(const S::Func *F, SourceLoc At);
  void declareFunction(const ast::FuncDecl *FD);
  void declareGlobals();
  void lowerBody(const ast::FuncDecl *FD);
  void addEntry(const ast::FuncDecl *Main, FuncId PkgInit);
  /// The package's init function, made if it has anything to do.
  FuncId lowerPackageInit();

  /// The init functions in the order they run: sorted files, then source
  /// order.
  SmallVector<const ast::FuncDecl *, 4> InitDecls;
  llvm::DenseMap<const ast::FuncDecl *, FuncId> InitIds;
  /// Package variables whose initializer is a constant: static data, not set
  /// by the init function.
  llvm::DenseSet<const S::Var *> StaticInit;
  /// A scalar or string constant of type Ty; false for any other type.
  bool constantOf(const S::ConstValue &V, Type *Ty, Constant &Out);

  /// Names for the structs: the name of each struct type, once.
  llvm::StringSet<> StructNames;
  unsigned AnonStructs = 0;

  FuncId WriteFn = NoId, ExitFn = NoId, PrintU = NoId, PrintI = NoId,
         PrintHex = NoId, PrintF = NoId, ComplexDiv = NoId;
  FuncId Panics[unsigned(AssertKind::SliceConvert) + 1][2] = {};
  /// A C function the compiler needs that the program declares differently.
  llvm::StringSet<> BadC;
  llvm::DenseMap<S::Type *, FuncId> EqGlues;

private:
  friend class Body;
  llvm::DenseMap<S::Type *, Type *> TypeCache;
  /// lowerType recurses once per link of a chain of named types: a new stack
  /// every DeepWalk::Segment links (Basic/NewStack.h).
  DeepWalk TypeWalk;
  bool NoStack = false; // err_no_stack is reported
  llvm::DenseMap<S::StructType *, Type *> StructCache;
  llvm::DenseMap<const S::TypeName *, std::string> LocalNames;
  Type *lowerTypeOnce(S::Type *T, SourceLoc At);
  /// The self-naming type of T, made on the first call (T is under way).
  Type *recShell(S::Type *T);
  /// A unique name for a type of the module (structs and self-naming types).
  std::string uniqueTypeName(S::NamedType *Named);
  Type *lowerStruct(S::Type *Whole, S::StructType *ST, S::NamedType *Named,
                    SourceLoc At);
};

/// Is T (a defined type counts by its underlying type) a string?
inline bool isStringType(S::Type *T) {
  auto *B = llvm::dyn_cast_or_null<S::BasicType>(S::under(T));
  return B && (B->BK == S::BasicKind::String ||
               B->BK == S::BasicKind::UntypedString);
}

/// What a loop or switch does for `break` and `continue`.
struct BranchTarget {
  BlockId Break = NoId, Continue = NoId; // Continue: NoId for a switch
  size_t Depth = 0;
  std::string Label;
};

class Body {
public:
  /// A function with a declaration (Obj may be null for the package init).
  Body(Lowering &L, Function &F, const S::Func *Obj)
      : L(L), M(*L.M), FB(*L.M, F), I(L.I), Obj(Obj) {}

  void lower(const ast::FuncDecl *FD);
  /// The package init function: the variable initializations in order, then
  /// the `init` functions.
  void lowerInit(ArrayRef<S::InitEntry> Order, ArrayRef<FuncId> InitFuncs);

  /// The scope node of a type declared here (the function's symbol node).
  const sym::Node *ScopeNode = nullptr;

private:
  Lowering &L;
  Module &M;
  FuncBuilder FB;
  const S::Info &I;
  const S::Func *Obj;
  llvm::DenseMap<const S::Var *, LocalId> Vars;
  /// A range over a map whose iteration ends with the scope it is in.
  struct MapIteration {
    LocalId It;
    const ast::RangeStmt *R;
  };
  struct Scope {
    SmallVector<LocalId, 4> Locals;
    /// gopp.map_iter_done runs for these on every way out (RNG-4).
    SmallVector<MapIteration, 1> MapIters;
    /// The debug scope that was current before this one began (P04.12).
    ScopeId Outer = 0;
  };
  /// `&` range variables (RNG-1a) and the parameters passed by a hidden
  /// pointer (a borrowed map, PAR-10): the pointer local that stands for the
  /// variable, whose uses are a Deref of it.
  llvm::DenseMap<const S::Var *, LocalId> Aliases;
  /// The function's body, and where each of its labels is (labelPos).
  const ast::BlockStmt *RootBody = nullptr;
  llvm::StringMap<SourceLoc> LabelPositions;
  bool LabelsScanned = false;
  /// The position of the label Name in this function: the statements are
  /// walked once (iteratively), at the first goto that needs it.
  SourceLoc labelPos(StringRef Name);
  SmallVector<Scope, 8> Scopes;
  SmallVector<BranchTarget, 4> Targets;
  llvm::StringMap<BlockId> Labels;
  /// The label of the statement being lowered, for the loop or switch it
  /// names.
  std::string PendingLabel;
  /// The scope depth of the clause being lowered (for `fallthrough`).
  size_t ClauseDepth = 0;
  /// Where `fallthrough` goes (the next case's body), or NoId.
  BlockId Fallthrough = NoId;
  /// Named results, as locals (empty when the results are unnamed).
  SmallVector<LocalId, 2> NamedResults;
  unsigned LocalTypes = 0;

  Context &ctx() { return L.Ctx; }
  void at(const ast::Node *N) {
    Pos = N->getBegin();
    FB.setSpan(L.spanOf(Pos));
  }
  void unsupported(const ast::Node *N, const llvm::Twine &What) {
    L.unsupported(N->getBegin(), What);
  }
  /// The node `at` last named: where errorValue reports what nothing did.
  SourceLoc Pos;
  /// The value of an expression that could not be lowered: the module fails,
  /// so nothing uses it. Something must have been reported (an unsupported
  /// form, a type that cannot be lowered); when nothing was, a fact the
  /// checker should have recorded is missing, and this reports it as an
  /// internal error, so it never becomes a silent wrong value.
  Operand errorValue(Context &C);
  /// V after a step that could not lower N, under the same rule.
  Operand failed(const ast::Node *N, Operand V);
  /// errorValue, said at N.
  Operand errorAt(const ast::Node *N) {
    return failed(N, constInt(ctx().i64(), 0));
  }
  /// The zero of the type of E, as what a refused E gives (after a report):
  /// later uses see a value of the right shape and report nothing more (a
  /// tuple where several values are wanted, an interface where one is); an
  /// i64 zero when E's type has no GIR type.
  Operand zeroOfExpr(const ast::Expr *E);
  static const ast::Expr *unparen(const ast::Expr *E) {
    while (auto *P = llvm::dyn_cast<ast::ParenExpr>(E))
      E = P->X;
    return E;
  }
  Type *tyOf(const ast::Expr *E) {
    return L.lowerType(I.typeOf(E), E->getBegin());
  }
  S::Type *sTyOf(const ast::Expr *E) { return I.typeOf(E); }
  bool isConst(const ast::Expr *E) {
    const S::TypeAndValue *TV = I.lookup(E);
    return TV && TV->Val && TV->M == S::Mode::Constant;
  }
  bool isNilValue(const ast::Expr *E);

  // Values.
  Operand unitValue() { return constBool(ctx(), false); }
  Operand zeroOf(Type *T);
  Operand constOperand(const S::ConstValue &V, Type *Ty, const ast::Node *N);
  Operand lowerExpr(const ast::Expr *E);
  Operand lowerAs(const ast::Expr *E, Type *Want);
  /// E converted to the checker type Want: a constant gets Want's GIR type,
  /// `nil` is Want's nil, a T for an Option[T] is wrapped.
  Operand valueFor(const ast::Expr *E, S::Type *Want);
  /// The argument E for parameter K of Sig: a borrowed `[]T` parameter (one
  /// without `move`) sees the slice's length as its capacity (SLC-12; D4.4
  /// passes it as (ptr, len)).
  Operand argFor(const ast::Expr *E, S::SignatureType *Sig, size_t K);
  /// The value V passed for parameter K of Sig (argFor's capacity rule).
  Operand argAs(Operand V, S::SignatureType *Sig, size_t K, const ast::Node *N);
  Operand lowerBinary(const ast::BinaryExpr *B);
  Operand lowerLogical(const ast::BinaryExpr *B);
  Operand lowerUnary(const ast::UnaryExpr *U);
  Operand lowerCall(const ast::CallExpr *C);
  /// The arguments of C for the signature Sig (GSig: the callee's GIR
  /// signature; Lead are its first parameters, a method's receiver, already
  /// made) and the call of CalleeOp: a function constant, or a pointer of a
  /// function type (an interface method, P04.06).
  Operand callWith(const ast::CallExpr *C, S::SignatureType *Sig, Type *GSig,
                   Operand CalleeOp, ArrayRef<Operand> Lead, bool NoUnwind);
  Operand lowerConversion(const ast::CallExpr *C);
  Operand lowerBuiltin(const ast::CallExpr *C, const S::Builtin *B);
  // Methods and interfaces (LowerIface.cpp, P04.06).
  /// A call of a method: static (the receiver adapted: its address, or its
  /// value) or through the itab of an interface.
  Operand lowerMethodCall(const ast::CallExpr *C, const ast::SelectorExpr *Sel,
                          const S::Func *F, const S::Selection &SL);
  /// The value of E where an interface of type Want is wanted.
  Operand toIface(const ast::Expr *E, S::Type *Want);
  /// X, a value of the checker type From, as a value of the interface type
  /// Want: boxed, converted between interfaces, or an Option's nil as the nil
  /// interface (the one decision point of a conversion to an interface).
  Operand toIfaceValue(Operand X, S::Type *From, S::Type *Want,
                       const ast::Node *N);
  /// V (of the checker type From) where a value of type To is wanted when
  /// it comes out of a call with several results (`return f()`, `a, b =
  /// f()`, `g(f())`): an interface or `Option[I]` target converts, an
  /// `Option[T]` target wraps a T.
  Operand convertResult(Operand V, S::Type *From, S::Type *To,
                        const ast::Node *N);
  /// V (of the concrete type Dyn) in an interface of type Want: inline in the
  /// data word when it fits, else in a new box.
  Operand boxAs(Operand V, S::Type *Dyn, S::Type *Want, const ast::Node *N);
  /// The interface X of type From as one of type To (all methods of To are
  /// in From). A nil one stays nil.
  Operand ifaceConvert(Operand X, S::Type *From, S::Type *To,
                       const ast::Node *N);
  Operand ifaceFrom(Operand Itab, Operand Data);
  /// x.(T): the value (a tuple of it and an ok flag when CommaOk).
  Operand lowerTypeAssert(const ast::TypeAssertExpr *TA, bool CommaOk,
                          S::Type *WantVal = nullptr);
  /// The place of the value an interface holds, as a value of GIR type G.
  Place boxedPlace(Place IfaceP, Type *G);
  /// The descriptor of the dynamic type of the interface in IfaceP, or null
  /// for a nil one.
  Operand dynTypeInfo(Place IfaceP);
  /// Does the dynamic type (Ti, never null here) satisfy `T`? For an
  /// interface T: it has the methods; else it is T.
  Operand typeMatches(Operand Ti, S::Type *T, const ast::Node *N);
  void lowerTypeSwitch(const ast::TypeSwitchStmt *TS, std::string Label);
  /// The address of a global as a *u8.
  Operand globalPtr(uint32_t G);
  Operand lowerOptionMethod(const ast::CallExpr *C,
                            const ast::SelectorExpr *Sel, const S::Func *F);
  Operand lowerCompositeLit(const ast::CompositeLit *CL);
  Operand lowerStructLit(const ast::CompositeLit *CL, S::StructType *ST,
                         Type *Ty);
  Operand lowerArrayLit(const ast::CompositeLit *CL, S::ArrayType *AT,
                        Type *Ty);
  Operand lowerIdent(const ast::Ident *Id);
  Operand lowerNarrowed(const ast::Ident *Id, S::Type *Payload);
  void lowerPrintln(const ast::CallExpr *C, bool Newline);
  bool constText(const ast::Expr *Arg, Type *T, std::string &Out);
  void printValue(Operand V, S::Type *ST, SourceLoc At);
  void writeString(StringRef S, SourceLoc At);
  void callWrite(Operand Ptr, Operand Len, SourceLoc At);

  // Option (D4.5): the layout is decided in Lowering::lowerOption; these
  // build and take apart a value of it.
  Operand nilOf(S::Type *OptionTy, const ast::Node *N);
  Operand wrapSome(Operand V, S::Type *OptionTy, const ast::Node *N);
  Operand optionIsSet(Operand O, const ast::Node *N);
  Operand optionPayload(Operand O);
  S::OptionType *asOption(S::Type *T);

  // Places.
  /// The place of an addressable (or materialized) expression; Ty is null
  /// after a report.
  /// An index check of a destination that waits for the store (the spec's
  /// phase 2): the index is already a local (phase 1), the check is made by
  /// emitIndexChecks just before the store of its target, left to right.
  struct PendingCheck {
    LocalId Idx;
    uint64_t Len;
    const ast::Node *N;
    LocalId LenLocal = NoId; // a length known at run time (a slice, a string)
  };
  using Pending = llvm::SmallVector<PendingCheck, 1>;
  void emitIndexChecks(const Pending &Checks);
  /// Pend: where the index checks of a destination go (null: made now).
  Place lowerPlace(const ast::Expr *E, Pending *Pend = nullptr);
  Place identPlace(const ast::Ident *Id);
  Place selectorPlace(const ast::SelectorExpr *Sel, Pending *Pend = nullptr);
  /// Makes the pending checks now: what follows reads through the element
  /// they guard (a header, a pointer), which must not happen unchecked.
  void flushChecks(Pending *Pend) {
    if (Pend && !Pend->empty()) {
      emitIndexChecks(*Pend);
      Pend->clear();
    }
  }
  Place indexPlace(const ast::IndexExpr *IE, Pending *Pend = nullptr);
  Place starPlace(const ast::StarExpr *St);
  /// The place that holds Op: its own place for a Copy, a temporary else.
  Place placeOf(Operand Op);
  LocalId localOf(Operand Op);
  /// The checked index of an array place; Idx is an integer operand.
  /// With Pend, the check is not made now but recorded (the index is
  /// copied into a local first); see PendingCheck.
  Place checkedIndex(Place Arr, Operand Idx, uint64_t Len, bool IdxConst,
                     const ast::Node *N, Pending *Pend = nullptr);
  /// idx < len, as an Assert (index out of range [idx] with length len).
  void indexCheck(Operand Idx, uint64_t Len, const ast::Node *N);
  Place followFields(Place Base, S::Type *BaseTy, ArrayRef<int> Path,
                     const ast::Node *N, Pending *Pend = nullptr);

  /// Late reads (gir.md "Assignment order"). A place that is written or read
  /// after other code of its statement runs (an assignment's target, the
  /// slice `append` adds to, a map passed by pointer) points into containers
  /// that code may grow or move: a slice's buffer, a map's entries. So it is
  /// made in two steps, in one way for every such place: recordOps evaluates
  /// its operands now (index values, map keys, slice bounds, a value that is
  /// no place, such as a call) into temporaries, in Go's order; the place is
  /// lowered again later under ReplayScope, where lowerExpr of a recorded
  /// operand is its temporary, so the variables, headers and pointers along
  /// the place are read then, and nothing is evaluated twice.
  struct LateOps {
    llvm::SmallDenseMap<const ast::Expr *, LocalId, 4> Saved;
  };
  void recordOps(const ast::Expr *E, LateOps &R);
  const LateOps *Replay = nullptr;
  struct ReplayScope {
    Body &B;
    const LateOps *Prev;
    ReplayScope(Body &B, const LateOps &R) : B(B), Prev(B.Replay) {
      B.Replay = &R;
    }
    ~ReplayScope() { B.Replay = Prev; }
  };

  // Operators and checks.
  /// Assert(Cond) with the panic function of kind K made (Unsigned: the
  /// index shown by a Bounds failure is unsigned).
  void check(Operand Cond, AssertKind K, bool Unsigned, const ast::Node *N,
             ArrayRef<Operand> Args = {});
  /// X op Y with Go's checks (D4.12). T is the checker type of the operands.
  /// The checks that come before X op Y; false after a report.
  bool arithChecks(BinOp Op, const Operand &Y, Type *XT, bool YConst,
                   const ast::Node *N);
  Operand arith(BinOp Op, Operand X, Operand Y, S::Type *T, bool YConst,
                const ast::Node *N);
  Operand complexArith(BinOp Op, Operand X, Operand Y, S::Type *T,
                       const ast::Node *N);
  /// X == Y or X != Y of any comparable type (glue for aggregates).
  Operand equality(bool Eq, Operand X, Operand Y, S::Type *T,
                   const ast::Node *N);
  Operand convert(const ast::CallExpr *C, Operand X, S::Type *From,
                  S::Type *To);

  // Statements.
  void lowerStmt(const ast::Stmt *St);
  void lowerBlock(const ast::BlockStmt *B);
  void lowerStmts(ArrayRef<ast::Stmt *> List);
  void lowerAssign(const ast::AssignStmt *A);
  void lowerOpAssign(const ast::AssignStmt *A);
  void lowerDefineOrAssign(const ast::AssignStmt *A);
  void lowerValueSpec(const ast::ValueSpec *VS);
  void lowerIf(const ast::IfStmt *If);
  void lowerFor(const ast::ForStmt *For, std::string Label);
  void lowerSwitch(const ast::SwitchStmt *Sw, std::string Label);
  // (lowerTypeSwitch: LowerIface.cpp)
  void lowerBranch(const ast::BranchStmt *B);
  void lowerLabeled(const ast::LabeledStmt *LS);
  void lowerReturn(const ast::ReturnStmt *R);
  void lowerIncDec(const ast::IncDecStmt *S);
  void lowerTypeDecl(const ast::GenDecl *GD);
  /// The values of a call used where several are wanted (`a, b := f()`).
  /// Targets: the types the values go to (an interface target converts; the
  /// first one is also the type a comma-ok assertion binds, a view, TYP-9).
  bool multiValues(const ast::Expr *E, size_t Want,
                   SmallVector<Operand, 2> &Out,
                   ArrayRef<S::Type *> Targets = {});
  BlockId labelBlock(StringRef Name);

  struct Dest;
  // Strings, slices, views and maps (LowerStr.cpp, P04.05).
  enum class SeqKind {
    None,
    String,
    Slice,
    ViewString, // View[string]: the data pointer and the length
    ViewSlice,
    Array,
    PtrArray // a pointer to an array, or a view of one
  };
  struct SeqInfo {
    SeqKind K = SeqKind::None;
    S::Type *Elem = nullptr; // slices and arrays
  };
  SeqInfo seqInfo(S::Type *T);
  /// The String or Slice place of a string, slice or view operand.
  Place seqPlace(Operand V, const SeqInfo &SI);
  Operand callFn(FuncId F, ArrayRef<Operand> Args, bool Owned = false);
  Operand callRt(RtFn F, ArrayRef<Operand> Args, const ast::Node *N,
                 bool Owned = false);
  Operand u64Of(Operand V);
  /// An integer operand of the source (any integer type, or a constant) as an
  /// int64.
  Operand intArg(const ast::Expr *E);
  bool hasPointers(Type *T);
  void indexCheckDyn(Operand Idx, Operand Len, const ast::Node *N);
  Place seqIndexPlace(const ast::IndexExpr *IE, const SeqInfo &SI,
                      Pending *Pend);
  /// The move of an in-place shrink `s = s[i:j]` (SLC-6, SLC-7): the kept
  /// elements go down to the start of the block when the header is stored
  /// (phase 2 of the assignment), so another right side of the same
  /// statement still reads them where they were.
  struct ShrinkMove {
    Operand Dst, Src, Bytes;
  };
  /// A slice expression. With Move, the in-place shrink the checker recorded
  /// (Info::Reslices): the result keeps the pointer and the capacity of the
  /// slice, and Move is the move to make before the header is stored.
  Operand lowerSliceExpr(const ast::SliceExpr *SE, ShrinkMove *Move = nullptr);
  /// A string or a slice header for a borrowed string or `[]T` (a parameter
  /// or a re-pointed borrowed variable, PAR-2) made from V, a view of one: a
  /// borrowed slice's capacity is its length (SLC-12), and a borrowed string
  /// owns no buffer, so its capacity is 0 (a literal's too).
  Operand borrowedSeq(Operand V, Type *G, const ast::Node *N);
  Operand lowerStringAdd(const ast::BinaryExpr *B);
  Operand concat(ArrayRef<Operand> Parts, const ast::Node *N);
  Operand stringArith(BinOp Op, Operand X, Operand Y, const ast::Node *N);
  Operand stringEqual(bool Eq, Operand X, Operand Y, const ast::Node *N);
  Operand sliceLiteral(const ast::CompositeLit *CL, S::SliceType *ST, Type *Ty);
  Operand mapLiteral(const ast::CompositeLit *CL, S::MapType *MT);
  Operand lowerMake(const ast::CallExpr *C);
  /// A `gopp_test_*` intrinsic (LowerTest.cpp; Opts.TestIntrinsics only).
  Operand lowerTestIntrinsic(const ast::CallExpr *C, const S::Func *F,
                             Lowering::TestFn Fn);
  Operand lowerAppend(const ast::CallExpr *C);
  Operand lowerCopy(const ast::CallExpr *C);
  Operand lowerLenCap(const ast::CallExpr *C, bool Cap);
  Operand lowerMinMaxString(const ast::CallExpr *C, bool Min);
  void lowerDelete(const ast::CallExpr *C);
  void lowerClear(const ast::CallExpr *C);
  Operand packVariadic(const ast::CallExpr *C, S::SignatureType *Sig,
                       size_t Fixed);
  /// A value of a string, slice, array or other type as a view of it.
  Operand makeView(const ast::Expr *E, S::Type *Want);
  /// A view of the element at P (a value read through a view).
  Operand viewOfPlace(Place P, S::Type *ViewS, const ast::Node *N);
  /// unsafe.Slice, unsafe.String, unsafe.SliceData, unsafe.StringData.
  Operand lowerUnsafeSeq(const ast::CallExpr *C, S::BuiltinId Id);
  Operand convertStrings(const ast::CallExpr *C, Operand X, S::Type *FromS,
                         S::Type *ToS);
  // Maps.
  bool isMapIndex(const ast::IndexExpr *IE);
  /// The map type of a map or a View of one; null if T is neither.
  S::MapType *mapTypeOf(S::Type *T);
  /// The map object of an expression of map type (read through a view).
  Operand mapValue(const ast::Expr *E);
  Operand mapInfoPtr(S::MapType *MT, const ast::Node *N);
  /// Before a lookup or a delete: a key that holds an interface is hashed in
  /// a nil or empty map too (gopp.map_key_check), as Go does.
  void mapKeyCheck(S::MapType *MT, const Operand &Info, const Operand &Mp,
                   const Operand &KeyAddr, const ast::Node *N);
  Operand bytesAddr(Place P);
  Operand mapRead(const ast::IndexExpr *IE, bool CommaOk);
  void lowerMapDest(const ast::IndexExpr *IE, Dest &D);
  /// A map entry m[k] passed to a borrowed map parameter (P04.05 review R11,
  /// R12, R14, R15): copy-in/copy-out, so no pointer into a map's entries is
  /// held across the call (the callee may grow the map, and map_grow frees
  /// the entries). Record Idx of the call's array Recs (gopp.mapentryarg) is
  /// filled with the map's info, the map object (where the copy-out goes,
  /// R13), the key (evaluated once) and its kind; gopp.map_entry_args then
  /// copies every entry in and resolves the aliasing at run time, and
  /// gopp.map_entry_back copies out once per entry after the call. The GIR of
  /// a call is linear in its entry arguments.
  void mapEntryArg(const ast::IndexExpr *IE, const Place &Recs, unsigned Idx);
  // Range (LowerStmt.cpp).
  void lowerRange(const ast::RangeStmt *R, std::string Label);

  LocalId declareVar(const ast::Ident *Id, Type *Ty);
  /// A destination of an assignment, `op=`, `++` or a `range` clause: a
  /// place, or blank. Made in three steps (gir.md "Assignment order"):
  /// recordDest in phase 1 (its operands, recordOps), deriveDest after the
  /// right sides and before the statement's first store (the containers and
  /// pointers along it are read then, as gc does, and an inner index is
  /// checked before anything is read through it), and resolveDest at its own
  /// store (its map entries, outer to inner, and its index checks before).
  struct Dest {
    const ast::Expr *E = nullptr;
    LateOps Ops;
    bool Derived = false;
    Place P;
    Pending Checks; // index checks to make before the store
    bool Blank = false;
    S::Type *STy = nullptr;
    /// `m[k] = v`, `mm[a][b] = v`: each entry is made when the value is
    /// stored, not before: `m[k] = len(m)` reads the map without k, and an
    /// earlier store of the statement may grow the map and move its entries.
    struct MapKey {
      S::MapType *MT;
      Operand KeyAddr;
    };
    SmallVector<MapKey, 1> MapKeys; // outer to inner
    Operand MapAddr; // the address of the variable that holds the outer map
    bool ok() const { return Blank || !MapKeys.empty() || P.Ty; }
  };
  Dest recordDest(const ast::Expr *E);
  void deriveDest(Dest &D);
  /// The map entries of D are made: D.P is the place of the value.
  void resolveDest(Dest &D, const ast::Node *N);
  /// The checks of D, its entries, then the store of V.
  void store(Dest &D, Operand V, const ast::Node *N);
  /// The address of the map at E for a borrowed map parameter (PAR-10).
  Operand mapArgAddr(const ast::Expr *E, LateOps &R);
  void lowerSwitchTests(const ast::SwitchStmt *Sw, ArrayRef<BlockId> Bodies,
                        BlockId Default, BlockId Exit);
  /// A source scope of the lowering. With Debug it is also a lexical block
  /// for the debugger (P04.12); the function's own outermost scopes are not
  /// (the parameters and the body's variables are the function's).
  void pushScope(bool Debug = true) {
    Scopes.emplace_back();
    Scopes.back().Outer = FB.scope();
    if (Debug)
      FB.setScope(FB.newScope());
  }
  void popScope() {
    exitScopes(Scopes.size() - 1);
    FB.setScope(Scopes.back().Outer);
    Scopes.pop_back();
  }
  void exitScopes(size_t Depth) {
    if (FB.terminated())
      return;
    for (size_t S = Scopes.size(); S > Depth; --S) {
      ++LowerSteps;
      for (size_t K = Scopes[S - 1].MapIters.size(); K > 0; --K)
        endMapIteration(Scopes[S - 1].MapIters[K - 1].It);
      for (size_t K = Scopes[S - 1].Locals.size(); K > 0; --K)
        FB.storageDead(Scopes[S - 1].Locals[K - 1]);
    }
  }
  /// gopp.map_iter_done of the iterator in local It.
  void endMapIteration(LocalId It);
  void startDeadBlock() { FB.setBlock(FB.newBlock()); }
  /// Closes every block that has no terminator and drops the unreachable
  /// ones.
  void finish();
};

} // namespace impl
} // namespace gir
} // namespace gopp

#endif // GOPP_LIB_GIR_LOWERIMPL_H
