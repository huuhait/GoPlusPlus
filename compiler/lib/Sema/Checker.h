//===- Checker.h - The type checker (private) -------------------*- C++ -*-===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The checker is a port of the algorithms of types2 (D3.1) into the
// LLVM-style architecture of Types.h. One `Checker` object checks either the
// package-level declarations (phase 1) or one function body (phase 2); both
// share a read-mostly `PackageState`.
//
//===----------------------------------------------------------------------===//

#ifndef GOPP_SEMA_CHECKER_H
#define GOPP_SEMA_CHECKER_H

#include "Units.h"

#include "gopp/Sema/Foreign.h"
#include "gopp/Sema/Sema.h"

#include "gopp/Basic/NewStack.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

#include <deque>
#include <functional>
#include <map>
#include <set>

namespace gopp {
namespace sema {

// The AST classes by name, except those a sema type has the same name as
// (ArrayType, SliceType, StructType, InterfaceType, MapType, ChanType, RefType,
// FuncType): those are written ast::X.
using ast::AssignStmt;
using ast::Attribute;
using ast::BadDecl;
using ast::BadExpr;
using ast::BadStmt;
using ast::BasicLit;
using ast::BinaryExpr;
using ast::BlockStmt;
using ast::BranchStmt;
using ast::CallExpr;
using ast::CaseClause;
using ast::CommClause;
using ast::CompositeLit;
using ast::Decl;
using ast::DeclStmt;
using ast::DeferStmt;
using ast::Ellipsis;
using ast::EmptyStmt;
using ast::Expr;
using ast::ExprStmt;
using ast::Field;
using ast::FieldList;
using ast::File;
using ast::ForStmt;
using ast::FuncDecl;
using ast::FuncLit;
using ast::GenDecl;
using ast::GoStmt;
using ast::Ident;
using ast::IfStmt;
using ast::ImportSpec;
using ast::IncDecStmt;
using ast::IndexExpr;
using ast::KeyValueExpr;
using ast::LabeledStmt;
using ast::MacroCallDecl;
using ast::MacroDecl;
using ast::MacroInvocation;
using ast::MacroParam;
using ast::Node;
using ast::ParenExpr;
using ast::ProcMacroDecl;
using ast::RangeStmt;
using ast::ReturnStmt;
using ast::SelectorExpr;
using ast::SelectStmt;
using ast::SendStmt;
using ast::SliceExpr;
using ast::Spec;
using ast::StarExpr;
using ast::Stmt;
using ast::SwitchStmt;
using ast::TypeAssertExpr;
using ast::TypeSpec;
using ast::TypeSwitchStmt;
using ast::UnaryExpr;
using ast::ValueSpec;
using llvm::ArrayRef;
using llvm::SmallVector;
using llvm::StringRef;

//===----------------------------------------------------------------------===//
// Operands
//===----------------------------------------------------------------------===//

struct Operand {
  Mode M = Mode::Invalid;
  Type *Ty = nullptr;
  ConstValue Val;
  const ast::Expr *E = nullptr;
  BuiltinId BId = BuiltinId::Len;
  /// Explicit type arguments of a partially instantiated generic function.
  ArrayRef<Type *> PartialTArgs;

  bool isNil() const { return M == Mode::NilValue; }
  SourceLoc pos() const { return E ? E->getBegin() : SourceLoc(); }
  bool isInvalid() const { return M == Mode::Invalid; }
};

/// How an expression was evaluated (types2's exprKind).
enum class ExprKind { Conversion, Expression, Statement };

/// Information about a package-level object (types2's declInfo).
struct DeclInfo {
  Scope *FileScope = nullptr;
  ArrayRef<Var *> Lhs; // n:1 var declaration `var a, b = f()`
  const ast::Expr *VType = nullptr;
  const ast::Expr *Init = nullptr;
  bool Inherited = false;
  const TypeSpec *TDecl = nullptr;
  const ast::UnitSpec *UDecl = nullptr; // a unit declaration (UNI-1)
  const FuncDecl *FDecl = nullptr;
  unsigned IotaVal = 0;
  bool Pinned = false;   // `@Pinned` type declaration (OWN-R10)
  bool Strict = false;   // `@Strict` struct type declaration (TYP-15a)
  bool ViewOk = false;   // `@View` struct type declaration (OWN-V5)
  bool Abstract = false; // `@Abstract` type declaration (TYP-19)
  /// Package-level objects this declaration refers to (initorder.go).
  llvm::SmallSetVector<Object *, 4> Deps;
};

/// A method declaration waiting to be associated with its receiver base type.
struct MethodInfo {
  Func *F;
  DeclInfo *D;
};

struct FileState {
  const ast::File *AST = nullptr;
  Scope *FileScope = nullptr;
  SmallVector<PkgName *, 8> Imports; // for the unused check
  bool HasCgo = false;
};

struct BodyJob {
  Func *F;
  const FuncDecl *Decl;
  DeclInfo *D;
  SignatureType *Sig;
  Scope *FileScope;
  Scope *Sc;
  ArrayRef<Var *> Params, Results;
  Var *Recv;
};

/// State shared by the checkers of one package. Written in phase 1 only
/// (the bodies of phase 2 read it), except where noted.
struct PackageState {
  TypeContext &Ctx;
  const SourceManager &SM;
  Package *Pkg = nullptr;
  Importer &Imp;
  CheckOptions Opts;
  SmallVector<FileState, 4> Files;
  llvm::DenseMap<Object *, DeclInfo *> ObjMap;
  llvm::DenseMap<TypeName *, SmallVector<MethodInfo, 2>> Methods;
  SmallVector<Object *, 32> PkgObjects; // in source order
  llvm::DenseMap<const ast::Ident *, Object *> *DefsPtr = nullptr;
  SmallVector<BodyJob, 16> Bodies;
  std::deque<DeclInfo> Decls; // stable addresses
  DeclInfo *newDecl() { return &Decls.emplace_back(); }
  SmallVector<MethodInfo, 4> OrphanMethods;
  /// The field and method names of every package-level type and of its
  /// ancestors (name -> the declaring origins), for the checks of the local
  /// types of the bodies (Checker::checkLocalHierarchy, ADR-0023).
  llvm::StringMap<SmallVector<std::pair<NamedType *, Object *>, 2>> HierNames;
  llvm::DenseSet<NamedType *> HierIndexed;
  llvm::DenseMap<StringRef, Package *> ImportedByPath;
  /// The declared units that messages name a unit by (UNI-5), added in
  /// phase 1 only: the units of the package as they are resolved and those of
  /// its imports as they are imported.
  units::Namer UNamer;
  /// The display forms of the package-level variables initialized from a
  /// unit expression (phase 1 only writes it).
  llvm::DenseMap<const Var *, const UnitDisplay *> UnitVarDisplays;
  /// (file scope, name) of a dot-imported object -> its import.
  llvm::DenseMap<std::pair<const Scope *, StringRef>, PkgName *> DotImportMap;
  PackageState(TypeContext &Ctx, const SourceManager &SM, Importer &Imp,
               const CheckOptions &Opts)
      : Ctx(Ctx), SM(SM), Imp(Imp), Opts(Opts) {}
};

/// Type category of D1.3.
enum class Category : uint8_t { Trivial, Owning, SharedHandle };

/// Calls Fn(Q, Exact) for each type parameter Q that Arg mentions: Exact when
/// Arg is Q itself. Iterative, each type once (Mono.cpp).
void forEachTypeParam(Type *Arg,
                      llvm::function_ref<void(TypeParamType *, bool)> Fn);
/// The same for the vertices of the instantiation cycle check: the type
/// parameters Arg mentions and the generic origins of the types declared
/// inside generic functions it mentions (NamedType::ImplicitTParams).
void forEachMonoSource(Type *Arg, llvm::function_ref<void(Type *, bool)> Fn);

class Checker;

/// Is the unary or binary operator Op defined on the (core) type T by the
/// language itself (Expr.cpp).
bool opAllowed(Checker &C, tok::Kind Op, Type *T, bool Unary);

/// The method as its type declares it: the origin of an instantiated method
/// or of a substituted interface method (Lookup.cpp).
const Func *declaredMethod(const Func *F);

/// Runs Fn on a thread with a stack of Bytes bytes and waits for it; false,
/// with Fn not run, if no thread could be made (Check.cpp).
bool runOnNewStack(llvm::function_ref<void()> Fn, size_t Bytes);

/// A diagnostic under construction; emitted by the destructor.
class Err {
public:
  Err(Checker &C, SourceLoc Loc, diag::ID ID, std::vector<std::string> Args);
  Err(Err &&O) noexcept : C(O.C), D(std::move(O.D)), Live(O.Live) {
    O.Live = false;
  }
  Err(const Err &) = delete;
  ~Err();
  Err &note(SourceLoc Loc, diag::ID ID, std::vector<std::string> Args);
  Err &note(SourceLoc Loc, std::string Msg);
  Err &fixit(SourceRange R, StringRef Text);
  Err &range(SourceRange R) {
    D.Primary = R;
    return *this;
  }

private:
  Checker &C;
  Diagnostic D;
  bool Live = true;
};

struct ExprInfo {
  bool IsLhs = false;
  Mode M = Mode::Invalid;
  Type *Ty = nullptr;
  ConstValue Val;
};

struct TermList {
  bool All = true;
  SmallVector<Term, 4> Terms;
};

class Checker {
public:
  Checker(PackageState &S, Info &I, Arena &Mem, DiagnosticsEngine &Diags);

  // --- driver (Check.cpp)
  void checkFiles(ArrayRef<const ast::File *> Files);
  void checkBody(const BodyJob &J);
  void processDelayed(size_t From = 0);
  void unusedImports();
  void recordUntyped();
  /// After a body: report unused variables of all scopes created.
  bool hadErrors() const { return NumErrors != 0; }

  // --- errors (Check.cpp)
  Err err(SourceLoc Loc, diag::ID ID, std::vector<std::string> Args = {});
  Err err(const ast::Node *N, diag::ID ID, std::vector<std::string> Args = {}) {
    return err(N ? N->getBegin() : SourceLoc(), ID, std::move(Args));
  }
  Err err(const Operand &X, diag::ID ID, std::vector<std::string> Args = {}) {
    return err(X.pos(), ID, std::move(Args));
  }
  void emit(Diagnostic D);
  std::string str(const Type *T) const {
    return typeString(T, S.Pkg, &unitNamer());
  }
  std::string str(const ast::Expr *E) const;
  std::string str(const Operand &X) const;
  std::string str(const ConstValue &V) const { return V.str(); }
  static std::string num(uint64_t N) { return std::to_string(N); }

  // --- state
  PackageState &S;
  TypeContext &Ctx;
  Info &I;
  Arena &Mem;
  DiagnosticsEngine &Diags;
  unsigned NumErrors = 0;
  bool FirstErr = false;
  /// types2's errpos: while an inherited constant initializer is checked, its
  /// errors are reported at the constant's name.
  SourceLoc ErrPos;
  /// types2's arity check of a constant declaration.
  void constArity(const ast::ValueSpec *VS, const ast::ValueSpec *Src,
                  bool Inherited);
  bool Bodies = false; // body checker (phase 2)
  /// Every package-level type is declared, with its method list (set in
  /// packageObjects once the type declarations are done). From then on a
  /// type-level answer that met only finished declarations is final, so
  /// NamedType::CloneState and CategoryState may be stored (typesFinal()).
  bool TypesDeclared = false;
  bool typesFinal() const { return Bodies || TypesDeclared; }
  /// Is N's declaration finished (its underlying type and method list are
  /// final)? A type of another package always is.
  bool declFinished(NamedType *N) const {
    NamedType *O = N->origin();
    return O->Methods || !O->Obj || O->Obj->Pkg != S.Pkg;
  }

  /// Per function environment (types2's `environment`).
  struct Env {
    Func *Decl = nullptr;
    bool GenericOk = false;  // a generic type or alias may be named here
    DeclInfo *Cur = nullptr; // declaration whose dependencies are recorded
    Scope *Sc = nullptr;
    /// How many pointers, views, pointer kinds and interfaces (a method, an
    /// embedded type, a constraint) the type expression being evaluated is
    /// behind. An instance there is no value of the declaration, so the
    /// check on the instance (holdsByValue, which stops at those) does not
    /// see it, and noteHeldTParams notes it as a use instead (P03.26).
    unsigned Behind = 0;
    SourceLoc Pos;
    const ConstValue *Iota = nullptr;
    SignatureType *Sig = nullptr;
    ArrayRef<Var *> Results;
    bool HasLabel = false;
    bool HasCallOrRecv = false;
    bool InTParamList = false;
    /// The factor of a unit is being evaluated: `/` of two integer
    /// constants is the exact quotient, not Go's integer division (UNI-2).
    /// (In the environment, so a constant declared elsewhere that the factor
    /// names is checked without it.)
    bool ExactQuo = false;
    const void *FuncId = nullptr;
    /// The implicit `error` result of a `!` function (a `return` can fail
    /// with it, ERR-3).
    Var *FailsErr = nullptr;
  } E;

  SmallVector<std::function<void()>, 16> Delayed;
  void later(std::function<void()> F) { Delayed.push_back(std::move(F)); }
  llvm::DenseMap<const ast::Expr *, ExprInfo> Untyped;
  SmallVector<Object *, 8> ObjPath;
  /// One instantiation whose type arguments mention type parameters, for the
  /// instantiation-cycle check (Mono.cpp).
  struct MonoInst {
    SourceLoc Pos;
    NamedType *Orig = nullptr; // the generic type, null for a function
    SmallVector<TypeParamType *, 2> TParams;
    SmallVector<Type *, 2> TArgs;
    SmallVector<SourceLoc, 2> ArgPos; // of the written arguments, for notes
    // GEN-14 (owner decisions 2026-10-30), outside Go mode:
    // SkipOwn: in a method's signature: an argument made with the method's
    //   own type parameter is no edge (the caller gives it at each call).
    // SrcRaw: in the body of a generic method, which is instantiated per
    //   call, not per receiver instance: its receiver parameters are
    //   vertices of their own (as a generic function's are), not the type's.
    // DstRaw: the receiver of a generic method call: the receiver
    //   instance's arguments bind those vertices.
    bool SkipOwn = false, SrcRaw = false, DstRaw = false;
  };
  /// A type declared in the current scope inside a generic function or method
  /// is implicitly parameterized by the type parameters around it (types2
  /// localNamedVertex).
  void noteImplicitTParams(TypeName *TN);
  /// The type parameters declared in a type-parameter or function scope,
  /// collected on the first local type below it (noteImplicitTParams).
  llvm::DenseMap<const Scope *, SmallVector<TypeParamType *, 4>> ScopeTParams;
  std::vector<MonoInst> MonoInsts;
  unsigned NoMono = 0; // > 0: instantiations are not recorded (Mono.cpp)
  std::vector<std::pair<TypeParamType *, TypeParamType *>> MonoCanon;
  /// The own type parameters of the generic methods of this package: an
  /// argument that mentions one is no edge of the cycle check (the caller
  /// gives it at each call; owner decision 2026-10-30, GEN-14).
  llvm::DenseSet<const TypeParamType *> MonoMethodOwn;
  unsigned MonoInSig = 0;       // > 0: recording a method's signature
  const Func *MonoFn = nullptr; // the function whose body is checked
  void recordMono(SourceLoc Pos, NamedType *Orig,
                  ArrayRef<TypeParamType *> TParams, ArrayRef<Type *> TArgs,
                  ArrayRef<SourceLoc> ArgPos = {}, bool BindRecv = false);
  /// Ex names a generic method of an instance (`l.Deep[*U]`): its receiver
  /// parameters (vertices of their own, SrcRaw) are bound to the instance's
  /// arguments, as a call's type arguments are.
  void monoBindRecv(SourceLoc Pos, const ast::Expr *Ex);
  /// Finds the instantiation cycles among Insts (true if there is one) and
  /// marks the generic types on them (InstCycle). Report: also report each
  /// cycle (E0366); the caller asks only for a package without other errors, as
  /// Go does.
  bool monoCheck(ArrayRef<MonoInst> Insts, bool Report);
  /// An instance of a generic type on an instantiation cycle: the ONE test
  /// that the walks which follow every instance a type reaches (clone,
  /// pinned, zero values, views held, pointers a copy shares, bad Option
  /// arguments, the by-value cycle sweep) make, so
  /// they end (the instances grow without end). Nothing else treats the type
  /// differently: it stays valid, so every other error is reported as Go
  /// does, and the cycle itself only when there is no other error.
  static bool onInstCycle(const Type *T) {
    auto *N = llvm::dyn_cast_or_null<NamedType>(T);
    return N && N->Orig && N->Orig->InstCycle;
  }
  /// Nesting of declarations and type expressions being resolved; see objDecl.
  static constexpr unsigned MaxDeclNest = 96;
  unsigned DeclNest = 0;
  /// objDecl starts a thread with a stack of its own when this thread has
  /// less than DeclReserveBytes of stack left (Decl.cpp): one declaration is
  /// a step that does not hop, and its initializer may nest to the parser's
  /// limit. The worst measured step (990 nested calls, a literal, a
  /// conversion; Dev and ASan) is 16.3 MB under ASan, so the reserve is 24
  /// MB; a segment is 48 MB (virtual: only the pages touched cost), so it
  /// holds 24 MB of links, thousands of cheap ones, and a 20000-link chain
  /// needs a few threads.
  static constexpr size_t DeclReserveBytes = size_t(24) << 20;
  static constexpr size_t DeclSegmentBytes = size_t(48) << 20;
  /// Set once the checker could not get a stack for a deep chain, and
  /// err_no_stack is reported (only the first time).
  bool NoStack = false;
  unsigned SegmentDepth = ~0u;
  /// Resolving the right-hand side of an alias: a defined type declared from
  /// there waits until the alias is done (objDecl).
  unsigned InAliasRhs = 0;
  bool DrainingDecls = false;
  llvm::DenseMap<TypeName *, NamedType *> PendingDecl;
  /// A deferred generic type: the scope of its type parameters, declared
  /// when it was deferred.
  llvm::DenseMap<TypeName *, Scope *> PendingTParamScope;
  /// The pending type declarePending asks objDecl to declare.
  TypeName *ForcePending = nullptr;
  /// Declares a type objDecl deferred (P03.23), if it still is.
  void declarePending(TypeName *TN);
  SmallVector<TypeName *, 16> DeferredDecls;
  void drainDeferredDecls();
  SmallVector<Var *, 16> LocalVars;

  // --- universe helpers
  BasicType *typ(BasicKind K) { return Ctx.typ(K); }
  Type *invalid() { return Ctx.invalid(); }
  bool isValid(const Type *T) const {
    return T != static_cast<const Type *>(Ctx.typ(BasicKind::Invalid));
  }

  // --- object creation (all in Mem)
  Var *newVar(StringRef Name, Type *T, SourceLoc Pos);
  Var *newParam(StringRef Name, Type *T, SourceLoc Pos);
  Const *newConst(StringRef Name, Type *T, SourceLoc Pos);
  TypeName *newTypeName(StringRef Name, Type *T, SourceLoc Pos);
  Func *newFunc(StringRef Name, Type *T, SourceLoc Pos);
  Scope *openScope(SourceRange Extent, const char *Comment);
  void closeScope();
  /// Declares Obj in Sc (reports redeclaration); ScopePos orders visibility.
  void declare(Scope *Sc, const ast::Ident *Id, Object *Obj,
               SourceLoc ScopePos);
  void recordDef(const ast::Ident *Id, Object *O);
  void recordUse(const ast::Ident *Id, Object *O);
  void recordImplicit(const ast::Node *N, Object *O);
  void recordTypeAndValue(const ast::Expr *E, Mode M, Type *T,
                          const ConstValue *V);
  void recordBuiltinType(const ast::Expr *F, Type *Sig);
  void recordInstance(const ast::Expr *E, ArrayRef<Type *> TArgs, Type *T);
  void recordSelection(const SelectorExpr *E, const Selection &Sel);

  // --- identifiers
  StringRef name(const ast::Ident *Id) const { return Id->Name.str(); }

  // =========================================================================
  // Predicates (Predicates.cpp)
  // =========================================================================
  bool identical(const Type *X, const Type *Y);
  bool identicalIgnoreTags(const Type *X, const Type *Y);
  bool identicalImpl(const Type *X, const Type *Y, bool IgnoreTags, void *Seen);
  bool comparableType(Type *T, bool Dynamic, SmallVector<const Type *, 4> *Seen,
                      std::string *Cause = nullptr);
  bool hasNil(Type *T);
  /// Why T is never nil in Go++ mode (ADR-0018 points 1 and 3: a pointer kind,
  /// a func, an interface with methods other than `error` itself), or null.
  const char *neverNilKind(Type *T);
  /// `*T`, a type with underlying `*T`, or Unique/Shared/Weak (ADR-0018).
  bool isPointerKind(Type *T);
  Type *under(Type *T);    // expands instances
  Type *coreType(Type *T); // nil if none
  bool isBasic(Type *T, unsigned Info);
  bool allBasic(Type *T, unsigned Info);
  bool isBoolean(Type *T) { return isBasic(T, IsBoolean); }
  bool isInteger(Type *T) { return isBasic(T, IsInteger); }
  bool isUnsigned(Type *T) { return isBasic(T, IsUnsigned); }
  bool isFloat(Type *T) { return isBasic(T, IsFloat); }
  bool isComplex(Type *T) { return isBasic(T, IsComplex); }
  bool isNumeric(Type *T) { return isBasic(T, IsNumeric); }
  bool isString(Type *T) { return isBasic(T, IsString); }
  bool isIntegerOrFloat(Type *T) { return isBasic(T, IsInteger | IsFloat); }
  bool isConstType(Type *T) { return isBasic(T, IsConstType); }
  bool allBoolean(Type *T) { return allBasic(T, IsBoolean); }
  bool allInteger(Type *T) { return allBasic(T, IsInteger); }
  bool allUnsigned(Type *T) { return allBasic(T, IsUnsigned); }
  bool allNumeric(Type *T) { return allBasic(T, IsNumeric); }
  bool allString(Type *T) { return allBasic(T, IsString); }
  bool allOrdered(Type *T) { return allBasic(T, IsOrdered); }
  bool allNumericOrString(Type *T) { return allBasic(T, IsNumeric | IsString); }
  bool isTyped(Type *T);
  bool isUntyped(Type *T) { return !isTyped(T); }
  bool isUntypedNumeric(Type *T);
  bool isTypeParam(const Type *T) const {
    return T && T->getKind() == Type::Kind::TypeParam;
  }
  bool isNamed(const Type *T) const;
  bool hasName(const Type *T) const; // basic, named or type param
  bool isInterface(Type *T);
  bool isNonTypeParamInterface(Type *T);
  bool isInterfacePtr(Type *T);
  bool isPointer(Type *T);
  bool isBytesOrRunes(Type *T);
  bool isUnsafePointer(Type *T);
  bool isUintptr(Type *T);
  bool isGeneric(Type *T);
  bool isEmptyInterface(Type *T);
  Type *defaultType(Type *T);
  Type *maxType(Type *X, Type *Y);
  Type *derefStructPtr(Type *T);
  Type *deref(Type *T, bool &IsPtr);
  NamedType *asNamed(Type *T);
  /// Calls F for each term's underlying type of the type parameter's type set
  /// (or F(under(T)) for other types). F(nullptr) if there are no specific
  /// types. Stops at the first false.
  bool underIs(Type *T, const std::function<bool(Type *)> &F);
  /// Calls F for each term of a type parameter (nullptr if none).
  /// types2 commonUnder: the common underlying type of all types in T's type
  /// set (channels may differ in direction); null with Err set otherwise.
  /// Cond, if given, is applied to every (type, underlying) pair first and
  /// returns an error text, or "" when the pair is fine.
  Type *commonUnder(Type *T,
                    const std::function<std::string(Type *, Type *)> &Cond,
                    std::string *Err);
  /// The element type of the channel (type set) operand X for a receive
  /// (Recv) or a send; reports the error at Pos and returns null otherwise.
  Type *chanElem(SourceLoc Pos, const Operand &X, bool Recv);
  /// The element type of the slice (type set) operand X, or null with Err.
  Type *sliceElem(const Operand &X, std::string &Err);
  bool typeParamIs(TypeParamType *P,
                   const std::function<bool(const Term *)> &F);
  /// The size of T in bytes for the layout code and the width of unsigned
  /// constants; 8 when T is too large.
  unsigned long long sizeofType(Type *T);
  /// The gc sizes for amd64 (a port of types2's gcsizes.go); a negative size
  /// or offset means the type is too large.
  int64_t gcSizeof(Type *T) { return gcLayout(T).Size; }
  int64_t gcAlignof(Type *T) { return gcLayout(T).Align; }
  struct Layout {
    int64_t Size = 0, Align = 1;
  };
  /// The size and alignment of T, with an explicit stack (a struct may hold
  /// another 10000 levels deep by value) and a memo per named type and
  /// instance; a by-value cycle or an instance that holds itself (which
  /// grows without end) is cut at one word (Sizes.cpp).
  Layout gcLayout(Type *T);
  void gcOffsetsof(const StructType *S, llvm::SmallVectorImpl<int64_t> &Offs);
  int64_t gcOffsetof(Type *T, llvm::ArrayRef<int> Index);
  /// Whether the size of T depends on a type parameter (builtins.go).
  bool hasVarSize(Type *T, llvm::DenseMap<NamedType *, bool> *Seen = nullptr);
  uint64_t varParams(NamedType *O);

  /// Recursion guard for checks that can chase infinitely expanding generic
  /// types (invalid programs); past the limit they give up quietly.
  struct DepthGuard {
    unsigned &D;
    explicit DepthGuard(unsigned &D) : D(D) { ++D; }
    ~DepthGuard() { --D; }
  };
  static constexpr unsigned MaxRecDepth = 64;
  unsigned RecDepth = 0;
  /// Nesting of computeTypeSet on this thread (interface embedding chains,
  /// which are bounded by the declarations unless the program is an invalid
  /// cycle). A set cut at the limit is never published; see computeTypeSet.
  static constexpr unsigned MaxTypeSetDepth = 1000;
  unsigned TypeSetDepth = 0;
  bool TypeSetCut = false;
  /// Stable storage for the iota values that delayed function literals use.
  std::deque<ConstValue> IotaStore;
  /// What the target of reverse type inference is called in error messages.
  std::string ReverseDesc;

  // --- type sets (TypeSet.cpp)
  TypeSet *typeSet(InterfaceType *I);
  TypeSet *typeSetOf(Type *T); // of an interface or type parameter
  bool termIncludes(const Term &T, Type *V);
  bool termDisjoint(const Term &X, const Term &Y);
  bool typeSetSubsetOf(TypeSet *A, TypeSet *B);
  bool termSubsetOf(const Term &X, const Term &Y);
  /// Want (ADR-0024): the operator method a synthesized selector names; an
  /// overload set shares its name, so the lookup finds that method itself
  /// (the element of the set, or the method a type declares) and ignores the
  /// name's package. Null for every lookup by name.
  Func *lookupMethodInSet(TypeSet *TS, Package *P, StringRef N, bool Fold,
                          int *Idx = nullptr, const Func *Want = nullptr);
  std::string termsString(ArrayRef<Term> Ts);
  bool typeSetIsComparable(TypeSet *TS);
  TypeSet *computeTypeSet(InterfaceType *I, SourceLoc Pos);
  TermList intersectTerms(const TermList &A, const TermList &B);
  TermList unionTerms(const TermList &A, const TermList &B);
  bool termListIncludes(const TermList &L, Type *V);

  // --- lookup (Lookup.cpp)
  struct LookupResult {
    Object *Obj = nullptr;
    SmallVector<int, 4> Index;
    /// A member found up an `extends` chain: the path is ParentSteps parent
    /// parts (field 0 at each level), then Index (the member's own index).
    /// The zeros are not spelled out: on a chain of n types they would cost
    /// up to n for each lookup (Selection::ParentSteps).
    uint32_t ParentSteps = 0;
    bool Indirect = false;
    bool Ambiguous = false; // index set, no object
  };
  bool chainLookup(NamedType *N, bool IsPtr, bool Addressable, Package *Pkg,
                   StringRef Name, LookupResult &R, const Func *Want = nullptr);
  /// The entry of the origin O in the chain name index (TypeContext::
  /// chainEntry), entering O and its ancestors not entered yet. False before
  /// the types are final or on a ring.
  bool chainIndex(NamedType *O, TypeContext::ChainEntry &E);
  /// The ancestor origin of O (E its entry) at depth D <= E.Depth, and, if
  /// Args, its type arguments in terms of O's own type parameters.
  NamedType *chainAncestor(NamedType *O, TypeContext::ChainEntry E, uint32_t D,
                           llvm::SmallVectorImpl<Type *> *Args);
  LookupResult lookupFieldOrMethod(Type *T, bool Addressable, Package *Pkg,
                                   StringRef Name, bool FoldCase = false,
                                   const Func *Want = nullptr);
  LookupResult lookupImpl(Type *T, bool Addressable, Package *Pkg,
                          StringRef Name, bool FoldCase,
                          const Func *Want = nullptr);
  Func *lookupMethod(NamedType *N, Package *Pkg, StringRef Name, bool Fold,
                     int *Idx, const Func *Want = nullptr);
  /// The name index of the methods of the origin O (NamedType::MethodIndex),
  /// made on first use once the types are final.
  const llvm::ArrayRef<uint32_t> *methodIndex(NamedType *O);
  /// A type with more methods than this is looked up by its index.
  static constexpr size_t IndexedMethods = 8;
  /// Test-only work count: the methods lookupMethod compared (a stress test
  /// counts a lookup per name instead of timing it).
  uint64_t LookupSteps = 0;
  /// Test-only: lookupMethod scans the methods, the walk the index replaces
  /// (the control of the stress test).
  bool ScanMethods = false;
  Func *namedMethod(NamedType *N, unsigned I); // instantiates lazily
  unsigned numMethods(NamedType *N);
  bool sameId(const Object *O, Package *Pkg, StringRef Name, bool Fold);
  /// Returns the missing method of V for interface T, or null if V implements
  /// T.
  Func *missingMethod(Type *V, Type *T, bool Static,
                      const std::function<bool(Type *, Type *)> &Equiv,
                      std::string *Cause, bool *WrongType = nullptr);
  bool implements(SourceLoc Pos, Type *V, Type *T, bool Constraint,
                  std::string *Cause);
  bool hasAllMethods(Type *V, Type *T, bool Static, std::string *Cause);
  bool assertableTo(Type *V, Type *T, std::string *Cause);
  bool mentions(Type *T, Type *Want);
  std::string funcString(const Func *F, bool PkgInfo = false);

  // --- operands & conversion (Operand.cpp)
  std::string operandString(const Operand &X);
  bool assignableTo(Operand &X, Type *T, std::string *Cause);
  /// Is a value of type V a T as it is, with no new value made: identical
  /// types (an alias included), or identical underlying types where one of
  /// them is unnamed (neither a type parameter)? The part of assignableTo
  /// that needs no conversion (P04.18, review R14: Info::AsIsArgs).
  bool assignableAsIs(Type *V, Type *T);
  bool convertibleTo(Operand &X, Type *T, std::string *Cause);
  void conversion(Operand &X, Type *T);
  bool representableConst(const ConstValue &X, BasicType *T,
                          ConstValue *Rounded);
  enum class ReprCode { Ok, Truncated, Overflow, Invalid };
  ReprCode representation(Operand &X, BasicType *T, ConstValue &Out);
  void representable(Operand &X, BasicType *T);
  void invalidConversion(ReprCode C, Operand &X, Type *Target);
  Type *implicitTypeAndValue(Operand &X, Type *Target, ConstValue *NewVal,
                             ReprCode *Code);
  bool constConvertibleTo(Operand &X, Type *T, ConstValue *Val);
  void setConst(Operand &X, ConstValue V, Type *T);

  // --- expressions (Expr.cpp)
  ExprKind rawExpr(Operand &X, const ast::Expr *E, Type *Hint,
                   bool AllowGeneric);
  /// Records what the operand X of the expression X.E is (its type and
  /// value; an untyped one for later): the one record of an evaluated
  /// expression, made by rawExpr and by every path that evaluates an
  /// expression without it (a plain index argument, genericExprList).
  void recordOperand(const Operand &X);
  ExprKind exprInternal(Operand &X, const ast::Expr *E, Type *Hint);
  void expr(Operand &X, const ast::Expr *E);
  void exprWithHint(Operand &X, const ast::Expr *E, Type *Hint);
  void genericExpr(Operand &X, const ast::Expr *E);
  void exprOrType(Operand &X, const ast::Expr *E, bool AllowGeneric);
  void multiExpr(SmallVector<Operand, 4> &Out, const ast::Expr *E,
                 bool AllowCommaOk, bool *CommaOk);
  void singleValue(Operand &X);
  void exclude(Operand &X, unsigned ModeSet);
  void unary(Operand &X, const UnaryExpr *E);
  void binary(Operand &X, const ast::Expr *E, const ast::Expr *L,
              const ast::Expr *R, tok::Kind Op, SourceLoc OpLoc);
  void binaryRest(Operand &X, Operand &Y, const ast::Expr *E,
                  const ast::Expr *L, const ast::Expr *R, tok::Kind Op,
                  SourceLoc OpLoc);
  void shift(Operand &X, Operand &Y, const ast::Expr *E, tok::Kind Op);
  void comparison(Operand &X, Operand &Y, tok::Kind Op);
  void matchTypes(Operand &X, Operand &Y);
  bool mayConvert(Operand &X, Operand &Y);
  void convertUntyped(Operand &X, Type *Target);
  void updateExprType(const ast::Expr *E, Type *T, bool Final);
  void updateExprVal(const ast::Expr *E, const ConstValue &V);
  void rememberUntyped(const ast::Expr *E, bool Lhs, Mode M, Type *T,
                       const ConstValue &V);
  void overflow(Operand &X, SourceLoc OpLoc);
  void opPos(Operand &X, SourceLoc P);
  bool isComparison(tok::Kind Op);
  void basicLit(Operand &X, const BasicLit *E);
  void funcLit(Operand &X, const FuncLit *E);
  void typeAssertion(const ast::Expr *E, Operand &X, Type *T, bool TypeSwitch);
  void useExprs(ArrayRef<const ast::Expr *> Es);
  void useLHS(ArrayRef<const ast::Expr *> Es);
  void use(const ast::Expr *E);

  // --- index & literals (Index.cpp)
  bool indexExpr(Operand &X, const IndexExpr *E);
  void sliceExpr(Operand &X, const SliceExpr *E);
  bool isValidIndex(Operand &X, const char *What, bool AllowNegative);
  int64_t index(const ast::Expr *Idx, int64_t Max);
  void compositeLit(Operand &X, const CompositeLit *E, Type *Hint);
  /// The composite literal that is the parent part of a child's literal
  /// (`Card{Payment: Payment{...}}`): no standalone value, so allowed even
  /// when the parent type is abstract (TYP-19).
  const CompositeLit *ParentPartLit = nullptr;
  /// A value stored in a composite literal (OWN-R3a, OWN-V6).
  void storeLiteralElem(Operand &X, Type *T);
  int64_t indexedElts(ArrayRef<ast::Expr *> Elts, Type *Elem, int64_t Length,
                      int64_t *NumSet = nullptr);

  // --- calls (Call.cpp)
  ExprKind callExpr(Operand &X, const CallExpr *E);
  /// The setup of a selector that comes before its operand is evaluated: a
  /// receiver that is an Option and meets IsNil / Get is not narrowed.
  void noNarrowReceiver(const SelectorExpr *Ex);
  void selector(Operand &X, const SelectorExpr *E, bool WantType,
                NamedType *Def = nullptr);
  /// The package an expression like `pkg` or `pkg.ns` denotes (marking the
  /// import used), or null.
  Package *qualifierPackage(const ast::Expr *X);
  /// qualifierPackage of the prefixes of the long paths met so far.
  llvm::DenseMap<const SelectorExpr *, Package *> QualMemo;
  /// Long chains (`a.b.c.d…`, `f()()()…`, `a[0][0]…`, `((((a))))`, mixes of
  /// them): the checker would recurse once per link. The operand part of a
  /// selector, call, index, slice, type assertion, `*` or parentheses is
  /// evaluated by chainOperand. When a chain below is long, prepareChain
  /// first evaluates its links bottom-up in a loop and parks their operands
  /// in ChainOperands, so the evaluation above takes the parked operand
  /// instead of recursing (P03.26).
  ///
  /// The forms read a few values that the forms above them set on the way
  /// down (ChainCtx). prepareChain first walks the chain top-down and does
  /// what each form does before its operand (enterChainForm, chainSetup),
  /// keeping the values each operand starts with; the bottom-up loop then
  /// evaluates each operand with exactly those values, and keeps the values
  /// it ends with, which the form above gets back when it takes the operand.
  struct ChainCtx {
    const ast::Expr *PartialOk = nullptr;
    bool PartialOkAddr = false;
    const ast::Expr *CalleeSel = nullptr;
    bool CalleeSuspended = false;
    const ast::Expr *NoNarrowExpr = nullptr;
  };
  ChainCtx chainCtx() const;
  void setChainCtx(const ChainCtx &C);
  struct ChainEntry {
    Operand Op;
    ExprKind Kind = ExprKind::Expression;
    ChainCtx After; // the values when the operand was done
  };
  llvm::DenseMap<const ast::Expr *, ChainEntry> ChainOperands;
  ExprKind chainOperand(Operand &X, const ast::Expr *Parent);
  /// What the form Eval sets before it evaluates its operand Child, and puts
  /// back after (the values it changes are returned).
  ChainCtx chainSetup(const ast::Expr *Eval, const ast::Expr *Child);
  void chainRestore(const ast::Expr *Eval, const ChainCtx &Saved);
  ExprKind chainEval(Operand &X, const ast::Expr *Eval, const ast::Expr *Child);
  void prepareChain(const ast::Expr *Parent);
  // --- foreign (C/C++ header) packages (Call.cpp, D3.7)
  Object *foreignTemplateOf(const IndexExpr *IE);
  Object *foreignCallee(const ast::Expr *Fun, const ast::Expr *&Recv);
  bool foreignTemplateArgs(ArrayRef<ast::Expr *> Idx,
                           SmallVector<ForeignTemplateArg, 4> &Out);
  /// `pkg.vec[int32]` as a type; null if IE is not a foreign template.
  Type *foreignInstantiate(const IndexExpr *IE);
  /// A read-only Go++ value (ADR-0008 Q2): a borrowed variable or part of
  /// one, a `ref` value.
  bool isReadOnlyValue(const ast::Expr *Ex, Type *Ty);
  /// Marks an import used. Body checkers collect the imports in UsedImports
  /// (the set of their worker) instead of writing PkgName::Used.
  void markUsed(PkgName *PN);
  llvm::SmallPtrSetImpl<PkgName *> *UsedImports = nullptr;
  bool builtin(Operand &X, const CallExpr *E, BuiltinId Id);
  /// The checks of a built-in after its arguments are evaluated, and those
  /// of make (which evaluates its own): out of builtin's frame (R13).
  bool builtinChecks(Operand &X, const CallExpr *Call, BuiltinId Id,
                     SmallVector<Operand, 4> &Args, size_t NArgs,
                     bool HadCallOrRecv);
  bool builtinMake(Operand &X, const CallExpr *Call, size_t NArgs);
  void arguments(const CallExpr *Call, SignatureType *Sig,
                 ArrayRef<Type *> TArgs, ArrayRef<SourceLoc> ArgPos,
                 SmallVector<Operand, 4> &Args, bool HasDots,
                 SignatureType *&Out);
  void funcInst(Operand &X, SourceLoc Pos, const IndexExpr *Inst,
                bool InferRest, Type *Hint = nullptr);
  void genericExprList(ArrayRef<ast::Expr *> Es, SmallVector<Operand, 4> &Out);
  void exprList(ArrayRef<ast::Expr *> Es, SmallVector<Operand, 4> &Out);
  std::string tupleString(TupleType *T, bool Variadic);
  std::string typesSummary(ArrayRef<Type *> List, bool Variadic, bool HasDots);
  std::string lookupError(Type *T, StringRef Sel, Object *Alt,
                          bool StructLit = false);
  Func *syntheticMethod(StringRef Name, ArrayRef<Type *> Results,
                        SourceLoc Pos);
  /// Whether a value of type T (the value of a checked Option) has a field
  /// or method Sel, through the auto-dereference of Unique / Shared (OWN-P5);
  /// a Weak has Lock only.
  bool valueHasMember(Type *T, StringRef Sel);
  void renameTParams(ArrayRef<TypeParamType *> TPs, TupleType *Params,
                     SmallVector<TypeParamType *, 4> &NewTPs,
                     TupleType **NewParams);

  // --- extends hierarchy (Hierarchy.cpp, ADR-0023)
  /// While a method of an `extends` type is checked: the name of its type.
  /// `base` may not be declared then (TYP-20).
  std::string BaseMethodOf;
  /// The hidden `base` variable of a method whose receiver type extends a
  /// type (TYP-20), owned by Body; null otherwise. Made in phase 1.
  Var *makeBaseVar(Var *Recv, const void *Body);
  /// The name of the receiver's type if it extends a type, else empty.
  std::string baseOwnerName(Var *Recv);
  /// The marks of a method declaration (TYP-18); reported once, at the
  /// declaration.
  void readMethodMarks(const FuncDecl *FD, Func *Fn);
  /// @Virtual, @Abstract and @Override are for methods; @Abstract is also a
  /// type attribute of an `extends` type (TYP-19).
  void checkDeclMarks(const ast::Decl *D);
  /// The parent of N: the type its hidden first field names (TYP-16).
  NamedType *parentOf(NamedType *N);
  /// The first ancestor of From (itself included) that is an instance of the
  /// generic type Orig, or null. Memoized for every type on the walk, so the
  /// calls of a hierarchy cost one step per type (inference, GEN-12).
  NamedType *ancestorInstanceOf(NamedType *From, NamedType *Orig);
  llvm::DenseMap<std::pair<const NamedType *, const NamedType *>, NamedType *>
      AncestorInst;
  /// The depth of N in its hierarchy (1 for a root), with the jump pointers
  /// on the way; 0 before the types are final.
  uint32_t hierDepth(NamedType *N);
  /// The ancestor of X (itself included) at depth D, by the jump pointers
  /// (O(log depth)); hierDepth(X) must be known and at least D.
  NamedType *levelAncestor(NamedType *X, uint32_t D);
  /// The first type from N up its parents (N included) with a field that must
  /// be set in a literal (TYP-15), or null; memoized for every type on the
  /// walk. Only once the types are final.
  NamedType *requiredAncestor(NamedType *N);
  /// Is Anc a strict ancestor of Child (O(log depth))?
  bool extendsType(NamedType *Child, NamedType *Anc);
  /// GEN-12: V is P or extends it (V may be a type parameter bounded by
  /// `extends Q`, with Q extending P).
  bool extendsOrIs(Type *V, Type *P);
  /// Ch is a defined type or a type parameter that extends P (not P itself).
  bool strictlyExtends(Type *Ch, Type *P);
  static Func::Mark markOf(const Func *F);
  /// TYP-22: N or an ancestor has a @Virtual or @Abstract method.
  bool hasTypeTable(NamedType *N);
  /// A struct type with a type table, or a type parameter bounded by
  /// `extends P` with such a P ("tagged", ADR-0023 decision 16).
  bool isTagged(Type *T);
  /// TYP-19 on the origin of N: 1 concrete, 2 an @Abstract method is open on
  /// its path, 3 abstract only by the @Abstract attribute.
  uint8_t abstractState(NamedType *N);
  bool isAbstractType(Type *T);
  /// Why N is abstract, for messages.
  std::string abstractWhy(NamedType *N);
  /// Reports (once the declarations are done) that a value of type T cannot
  /// be made here because T is abstract. What names the construct.
  void noAbstract(const ast::Expr *At, Type *T, const char *What);
  /// TYP-19 through type arguments: an abstract argument that the instance
  /// (a named type, an Option, or the signature of a generic function) holds
  /// by value, decided on the substituted types. Called once the instance
  /// is verified (a delayed check). Behind: the type expression of the
  /// instance was behind a pointer, a view or an interface (Env::Behind).
  void noAbstractArgs(SourceLoc At, Type *Holder, ArrayRef<Type *> Args,
                      bool Behind = false);
  /// TYP-22: reports the `extends` rings among Types (their origins) and
  /// makes their types invalid.
  void checkExtendsCycles(ArrayRef<NamedType *> Types);
  /// The positions of the declared fields of an `extends` struct
  /// (structTypeExpr), and the batch of a body's local types.
  void noteHierarchy(NamedType *N, const ast::StructType *Decl, bool Local);
  /// The walk of every package-level type, once (phase 1).
  void ensureHierarchy();
  bool HierarchyDone = false;
  /// TYP-17 to TYP-19: one depth-first walk of the `extends` forest of Seeds
  /// and their ancestors, with the names of the path in a hash map and an
  /// undo log (linear); reports for the seeds of this package if Report,
  /// and stores the abstract state of every type it meets.
  void hierarchyBatch(ArrayRef<NamedType *> Seeds, bool Report);
  llvm::DenseMap<const ast::StructType *,
                 SmallVector<std::pair<StringRef, SourceLoc>, 4>>
      FieldPosOf;
  llvm::DenseMap<NamedType *, SmallVector<std::pair<StringRef, SourceLoc>, 4>>
      FieldPos;
  SmallVector<NamedType *, 8> LocalBatch;
  /// The rules of the local types of a body (no methods of their own): the
  /// names of their fields against the ancestors', through NameIndex (every
  /// declaring type of a name, filled once per type), and the open @Abstract
  /// methods they inherit.
  void checkLocalHierarchy(ArrayRef<NamedType *> Batch);
  llvm::StringMap<SmallVector<std::pair<NamedType *, Object *>, 2>> NameIndex;
  llvm::DenseSet<NamedType *> NameIndexed;
  /// Adds the own field and method names of From and its ancestors to Index,
  /// up to a type in Indexed or Also.
  void indexNames(
      NamedType *From,
      llvm::StringMap<SmallVector<std::pair<NamedType *, Object *>, 2>> &Index,
      llvm::DenseSet<NamedType *> &Indexed,
      const llvm::DenseSet<NamedType *> *Also);
  /// extendsType on origins: is AncO the origin of an ancestor of Child?
  bool extendsOrigin(NamedType *Child, NamedType *AncO);
  /// EXP-13: V converts to T by an upcast (`*C` to `*P`, `Unique[*C]` to
  /// `Unique[*P]`, `View[C]` to `View[P]`), which needs a type table in the
  /// target parent. Handled: the pair is such an upcast, so the answer is
  /// final (false with a Cause when it is not allowed).
  bool hierarchyUpcast(Type *V, Type *T, std::string *Cause, bool &Handled);
  /// The Cause of an upcast to a parent P without a type table.
  std::string untaggedCause(NamedType *P, Type *Child, Type *To);
  /// The parent struct behind a pointer or an owning pointer kind (a downcast
  /// or a type switch on it), or null.
  NamedType *downcastParent(Type *From);
  /// EXP-15: T is a pointer to a strict descendant of P and P has a type
  /// table; reports at At otherwise.
  bool downcastTarget(NamedType *P, Type *T, const ast::Expr *At,
                      const std::string &What);
  /// EXP-15: `p.(*Card)` on a parent pointer. True if the assertion is a
  /// downcast (X is invalid after an error).
  bool downcastOk(Operand &X, Type *T, const ast::Expr *Ex);
  /// The places that may hold only the parent part of a bigger value
  /// (EXP-15, ADR-0023 decision 16): the parent part `c.P`, `*p` of a tagged
  /// type, and a value receiver of a tagged type. They are allowed as the
  /// start of a selector (field access, method call) and under `&`; the
  /// selector and `&` mark the operand they allow (PartialOk), and the place
  /// takes the mark when it is checked.
  enum class PartialKind : uint8_t { None, ParentPart, Deref, Receiver };
  struct PartialMark {
    bool Ok = false;   // the start of a selector, or the operand of `&`
    bool Addr = false; // the operand of `&`
  };
  const ast::Expr *PartialOk = nullptr;
  bool PartialOkAddr = false;
  void markPartialOk(const ast::Expr *E, bool Addr);
  PartialMark takePartialMark(const ast::Expr *E);
  /// What a selector or `*p` does on entry, before its operand: it takes its
  /// mark, and a selector keeps an Option receiver from narrowing. The one
  /// place for the forms and for prepareChain's top-down walk; the other
  /// chain forms do nothing on entry.
  PartialMark enterChainForm(const ast::Expr *E);
  /// The one decision for every partial place: true if this use is allowed,
  /// else reports. PartTy is the type of the place, Whole the child (for
  /// messages).
  bool partialUse(const ast::Expr *E, PartialKind K, Type *PartTy, Type *Whole,
                  PartialMark M);
  /// The kind of partial place E (already checked, of type T) is.
  PartialKind partialKindOf(const ast::Expr *E, Type *T);
  /// The kind of the place `*p` for a p pointing to T: Deref if T is tagged.
  PartialKind derefKind(Type *T);
  /// The target of a pointer, an owning pointer (not Weak) or a view, or null.
  Type *pointeeOf(Type *T, bool IncludeWeak = false);
  /// A method value, a `go` / `defer` call (Taken) or a `move` receiver
  /// takes the receiver of Ex (a COPY for a value receiver, the object
  /// itself for a `move` one). When that place may hold only the parent part
  /// of a child (a tagged `*p`, a tagged value receiver, a view, the parent
  /// part, a method found on a parent), it is the same event as `x := *p`:
  /// an error (R6, R7). False if reported.
  /// How the member of a lookup is reached from the receiver: its own
  /// member, or the last step to its owner is the parent part (inherited) or
  /// a Go embedded field (the receiver is that field, Via).
  enum class OwnerKind : uint8_t { Own, Parent, Field };
  struct OwnerStep {
    OwnerKind K = OwnerKind::Own;
    Var *Via = nullptr; // Field: the embedded field of the last step
  };
  bool receiverTakenOk(const SelectorExpr *Ex, Func *F, OwnerStep Step,
                       Type *RcvTy, bool Taken);
  /// The one decision behind receiverTakenOk, also made at instantiation on
  /// the real type argument (useWhy): why taking the receiver of F on a
  /// receiver of type RcvTy whose place is of kind K (Star: it is `*p`;
  /// ParentPart for a pointer: `base`) would cut a child down, or empty if it
  /// does not. Step: how F was found (ownerStep). A receiver
  /// that is (or points to) a type parameter is decided by its
  /// instantiations: the use is noted (noteUse). Text gives the receiver's
  /// text, asked only for a message.
  std::string receiverWhy(Type *RcvTy, PartialKind K, bool Star, Func *F,
                          OwnerStep Step, bool Taken,
                          llvm::function_ref<std::string()> Text);
  /// The last step of lookup R on RcvTy to the member's owner: an `extends`
  /// parent part, or a Go embedded field (decided as the written-out
  /// selector through that field is).
  OwnerStep ownerStep(Type *RcvTy, const LookupResult &R);
  /// `v.Clone()` / `clone(v)` on a view of a type parameter: a value clone of
  /// a place that may be a parent part (R9).
  void noteViewClone(Type *RcvTy);
  /// ADR-0023 decision 16: the tagged P behind a *P or View[P] whose Clone()
  /// is a Unique[*P] with a full copy of the dynamic type, or null.
  Type *cloneThroughTable(Type *T);
  Type *uniqueOf(Type *Elem);
  /// `(*p).Clone()` and the like on a partial place (reported at At).
  bool rejectPartialClone(const ast::Expr *Recv, Type *T, const ast::Expr *At);

  // --- type expressions (Typexpr.cpp)
  Type *typ(const ast::Expr *E);
  /// The type of a variable, field, element, parameter or result. A value
  /// use (not the pointee of `*`, a type argument, the argument of View or a
  /// parent) may not be abstract (TYP-19); What names it for the message.
  Type *varType(const ast::Expr *E, bool ValueUse = true,
                const char *What = nullptr);
  Type *definedType(const ast::Expr *E, NamedType *Def);
  Type *genericType(const ast::Expr *E, std::string *Cause,
                    TypeName **Alias = nullptr);
  Type *typInternal(const ast::Expr *E, NamedType *Def);
  void ident(Operand &X, const ast::Ident *Id, NamedType *Def, bool WantType);
  int64_t arrayLength(const ast::Expr *E);
  Type *structTypeExpr(const ast::StructType *E);
  /// TYP-16: resolves the parent type of an `extends` declaration, checks
  /// that it can be one, and gives the hidden parent field to Add. DeclName
  /// is the type being declared (Hierarchy.cpp).
  void addParentField(const ast::Expr *PE, StringRef DeclName,
                      llvm::function_ref<void(Type *, StringRef)> Add);
  Type *interfaceTypeExpr(const ast::InterfaceType *E, NamedType *Def);
  struct FuncInfo {
    SignatureType *Sig = nullptr;
    Scope *Sc = nullptr;
    ArrayRef<Var *> Params, Results;
  };
  /// Sc: a prepared scope (receiver and its type parameters) or null.
  FuncInfo funcSignature(const ast::FuncType *E, Scope *Sc, Func *ForFunc);
  Type *unionTypeExpr(const ast::Expr *E);
  Type *instantiatedType(const IndexExpr *E, NamedType *Def);
  bool typeList(ArrayRef<ast::Expr *> Es, SmallVector<Type *, 4> &Out,
                ArrayRef<TypeParamType *> Want = {});
  void validVarType(const ast::Expr *E, Type *T);
  void collectParams(Scope *Sc, const FieldList *L, bool IsResult,
                     SmallVector<Var *, 8> &Vars, SmallVector<Type *, 8> &Types,
                     bool &Variadic, uint64_t &MoveMask, SourceLoc ScopePos);
  /// Bind, if given, runs after all parameters are declared and before their
  /// constraints are resolved (a constraint may name the generic type).
  SmallVector<TypeParamType *, 4> collectTypeParams(
      const FieldList *L, Scope *Sc,
      const std::function<void(ArrayRef<TypeParamType *>)> &Bind = nullptr);
  Type *boundType(const ast::Expr *E, bool IsImplicit = false);
  /// GEN-12: the parent type of an `extends P` constraint element; null
  /// after an error.
  Type *extendsConstraintType(const ast::Expr *PE);
  /// `T extends P`: the implicit interface `interface{ extends P }`.
  Type *extendsBound(const ast::Expr *PE, SourceLoc ExtendsLoc);
  Type *boundTypeNoWrap(const ast::Expr *E);
  SourceLoc exprLoc(const ast::Expr *E) const { return E->getBegin(); }

  // --- declarations (Decl.cpp)
  void collectObjects();
  void packageObjects();
  void objDecl(Object *O, NamedType *Def);
  void constDecl(Const *O, const ast::Expr *Typ, const ast::Expr *Init,
                 bool Inherited, const ConstValue *Iota);
  void varDecl(Var *O, ArrayRef<Var *> Lhs, const ast::Expr *Typ,
               const ast::Expr *Init);
  void typeDecl(TypeName *O, const TypeSpec *TD, NamedType *Def,
                bool Pinned = false, bool Strict = false, bool ViewOk = false);
  /// The struct type expression of the `@Strict` type declaration being
  /// resolved (TYP-15a); null otherwise. Only that very node is strict: a
  /// struct nested in a field type of it is not.
  const ast::StructType *StrictStructExpr = nullptr;
  /// The struct type of the `@View` type declaration being resolved (OWN-V5):
  /// only its fields may hold views.
  const ast::StructType *ViewStructExpr = nullptr;
  bool containsView(Type *T);
  /// The one question "is this read-only" (OWN-V1): a View, not the borrowed
  /// slice made by slicing a writable one.
  bool isViewType(Type *T);
  bool pointerFree(Type *T);
  /// The ONE question "is a value of type T read through a view or a box a
  /// plain copy" (OWN-V1, TYP-9): T is Copy and definitely pointer-free
  /// (pointerFree; a type parameter: every type in its type set). viewRead,
  /// range, box bindings and inference ask it.
  bool copiesOut(Type *T);
  bool viewCopyHasPointer(Type *T);
  bool reachesBlocker(
      Type *Root, llvm::function_ref<std::atomic<uint8_t> &(NamedType *)> Slot,
      llvm::function_ref<bool(Type *, llvm::SmallVectorImpl<Type *> &)> Visit);
  Type *boxBindingType(Type *T);
  /// The type of a part of type T read through a view: a plain copy if
  /// copiesOut(T), else View[T] (a pointer gives a view of its target).
  Type *viewRead(Type *T);
  /// The view of the target of a pointer read through a view.
  Type *viewTarget(Type *T);
  /// Is the place Ex a part of a view (reached through a View-typed
  /// operand, at any depth)?
  bool viewPlace(const ast::Expr *Ex);
  /// viewPlace of the operand of each slice expression checked (sliceExpr):
  /// a slice of a slice asks it, so a long chain is walked once, not once per
  /// link.
  llvm::DenseMap<const SliceExpr *, bool> SliceInView;
  /// Is Place (of type T) a view variable that is no part of a view or an
  /// interface box? It may be re-pointed, also through a pointer to it, and
  /// moved (a copy of the view): what it looks at stays read-only by its
  /// type (OWN-V1, "re-pointing"). The one test of this rule.
  bool viewVariable(const ast::Expr *Place, Type *T);
  bool rejectViewWrite(const ast::Expr *At, const ast::Expr *Place,
                       const std::string &What, bool Self = false);
  llvm::DenseSet<const ast::Expr *> ViewWriteReported;
  /// The struct type expression of the type declaration being resolved and
  /// the name of that type (for messages).
  const ast::StructType *DeclStructExpr = nullptr;
  /// The parent type expression of the `extends` declaration being resolved
  /// (TYP-16): structTypeExpr makes the hidden parent field of DeclStructExpr
  /// from it.
  const ast::Expr *DeclExtendsExpr = nullptr;
  StringRef DeclStructName;
  /// `@Ref` on a type declaration: an unknown attribute (ADR-0020).
  void rejectRefAttr(const GenDecl *GD);
  llvm::DenseSet<const Attribute *> RefAttrReported;
  void funcDecl(Func *O, DeclInfo *D);
  void collectMethods(TypeName *O);
  void declStmt(const DeclStmt *S);
  void importPackage(const ImportSpec *Spec, FileState &F);
  bool validCycle(Object *Start, bool &Nval, bool &Ndef);
  void cycleError(ArrayRef<Object *> Cycle, int Start = -1,
                  ArrayRef<SourceLoc> At = {});
  void validType(NamedType *T);
  bool validTypeWalk(Type *Root);
  void resolvePendingUnder();
  void invalidateByValueCycles(ArrayRef<NamedType *> Roots);
  void checkInitOrder(std::vector<InitEntry> &Out);
  void addDeclDep(Object *To) {
    if (E.Cur && S.ObjMap.count(To))
      E.Cur->Deps.insert(To);
  }
  std::string genericAliasString(TypeName *TN);
  const ast::Expr *instantiatedAliasTarget(TypeName *TN);
  void collectRecv(Func *F, const FieldList *Recv, Scope *Sc, DeclInfo *D,
                   const FuncDecl *FD, NamedType *&RecvNamed);
  /// The defined type of this package a receiver type expression names,
  /// following aliases; Ptr: a pointer was seen on the way. Null if none.
  TypeName *resolveRecvBase(const ast::Expr *Recv, bool &Ptr);
  Package *importedPackage(StringRef Path, SourceLoc Pos);

  // --- statements (Stmt.cpp)
  void funcBody(DeclInfo *D, StringRef Name, SignatureType *Sig,
                const BlockStmt *Body, const ConstValue *Iota, Scope *Sc,
                ArrayRef<Var *> Params, ArrayRef<Var *> Results, Var *Recv,
                const ast::FuncType *FT);
  enum StmtCtx : unsigned {
    BreakOk = 1,
    ContinueOk = 2,
    FallthroughOk = 4,
    FinalSwitchCase = 8,
    InTypeSwitch = 16,
  };
  void stmt(unsigned Ctx, const Stmt *S);
  void stmtList(unsigned Ctx, ArrayRef<Stmt *> L);
  void simpleStmt(const Stmt *S);
  void openBlock(const BlockStmt *B, const char *Comment);
  void blockStmt(unsigned Ctx, const BlockStmt *B);
  void assignStmt(const AssignStmt *S);
  void assignOp(Operand &X, const ast::Expr *Lhs, const ast::Expr *Rhs,
                tok::Kind Op, SourceLoc OpLoc, const ast::Expr *Whole);
  void switchStmt(unsigned Ctx, const SwitchStmt *S);
  void typeSwitchStmt(unsigned Ctx, const TypeSwitchStmt *S);
  void selectStmt(unsigned Ctx, const SelectStmt *S);
  void rangeStmt(unsigned Ctx, const RangeStmt *S);
  void ifStmt(unsigned Ctx, const IfStmt *S);
  void forStmt(unsigned Ctx, const ForStmt *S);
  void returnStmt(const ReturnStmt *S);
  void failsReturn(const ReturnStmt *S);
  void goDeferStmt(const ast::Expr *Call, const char *What);
  /// True if the function being checked returns errors: it is declared with
  /// `!` or its last result is `error` (ERR-2). A package-level initializer
  /// is in no function.
  bool enclosingCanFail() const;
  /// The `go` / `defer` call being checked: its `!` panics / sets the error.
  const CallExpr *GoCall = nullptr, *DeferCall = nullptr;
  void suspendedCall(const char *What, const ast::Expr *Call);
  void labels(const BlockStmt *Body);
  bool isTerminating(const Stmt *S, StringRef Label);
  bool isTerminatingList(ArrayRef<Stmt *> L, StringRef Label);
  bool isTerminatingSwitch(const BlockStmt *Body, StringRef Label);
  bool hasBreak(const Stmt *S, StringRef Label, bool Implicit);
  bool hasBreakList(ArrayRef<Stmt *> L, StringRef Label, bool Implicit);
  void caseValues(Operand &X, ArrayRef<ast::Expr *> Values,
                  SmallVector<std::pair<ConstValue, Type *>, 8> &Seen,
                  SmallVector<SourceLoc, 8> &SeenPos,
                  llvm::StringMap<SmallVector<uint32_t, 1>> &SeenIdx);

  // --- assignment (Assign.cpp)
  void assignment(Operand &X, Type *T, StringRef Context);
  Type *lhsVar(const ast::Expr *Lhs);
  Type *assignVar(const ast::Expr *Lhs, const ast::Expr *RhsExpr, Operand *X,
                  StringRef Context);
  void initVar(Var *Lhs, Operand &X, StringRef Context);
  void initVars(ArrayRef<Var *> Lhs, ArrayRef<ast::Expr *> Rhs,
                const Stmt *ReturnStmtOrNull);
  void assignVars(ArrayRef<ast::Expr *> Lhs, ArrayRef<ast::Expr *> Rhs);
  void shortVarDecl(SourceLoc Pos, ArrayRef<ast::Expr *> Lhs,
                    ArrayRef<ast::Expr *> Rhs);
  void assignError(ArrayRef<ast::Expr *> Rhs, unsigned L, unsigned R);
  void returnError(const Stmt *At, ArrayRef<Var *> Lhs, ArrayRef<Operand> Rhs);
  void initConst(Const *Lhs, Operand &X);
  bool recordInitVars(ArrayRef<Var *> Lhs, ArrayRef<Operand> Rhs,
                      const Stmt *ReturnStmt);

  // --- generics (Infer.cpp)
  Type *subst(Type *T, ArrayRef<TypeParamType *> From, ArrayRef<Type *> To);
  Type *substImpl(Type *T, const llvm::DenseMap<const Type *, Type *> &M,
                  unsigned Depth);
  NamedType *instantiateNamed(NamedType *Orig, ArrayRef<Type *> TArgs);
  void expandNamed(NamedType *N);
  Type *instantiateSignature(SourceLoc Pos, SignatureType *Sig,
                             ArrayRef<Type *> TArgs, ArrayRef<SourceLoc> ArgPos,
                             const ast::Expr *Ex = nullptr);
  /// ViewWhy: set (and Cause too) when the argument fails because generic
  /// code writes View[T] and the argument is a pointer kind or a view; the
  /// caller then reports it as the written View[X] is (E0371).
  int verify(SourceLoc Pos, ArrayRef<TypeParamType *> TParams,
             ArrayRef<Type *> TArgs, std::string *Cause, bool ForType = false,
             std::string *ViewWhy = nullptr);
  bool satisfies(SourceLoc Pos, Type *TArg, TypeParamType *TPar,
                 ArrayRef<TypeParamType *> TParams, ArrayRef<Type *> TArgs,
                 std::string *Cause, bool ForType = false,
                 std::string *ViewWhy = nullptr);
  /// An instantiation of a generic of this package whose type argument may
  /// break a use of the generic code (GEN-12, EXP-15, TYP-19; mayBreakUses).
  /// The generic's bodies are not all checked yet, so the answer waits for
  /// untaggedCheck, after them.
  struct UntaggedPending {
    SourceLoc Pos;
    Type *TArg;
    TypeParamType *TPar;
    bool ForType; // a type instantiation (E0323), else a call (E0316)
    const Func *Owner = nullptr; // the method whose body made it (GEN-14)
    bool NamesType = false;      // a written `Vec[U]`: Owner needs no unit
  };
  std::vector<UntaggedPending> UntaggedPend;
  /// The uses the generic code makes of its type parameters (TypeParamType::
  /// Use), recorded by each checker and merged after the bodies, so their
  /// set does not depend on which thread checked which body.
  struct UseRec {
    TypeParamType *TP;
    TypeParamType::Use U;
  };
  std::vector<UseRec> TPUses;
  /// Set while a check that converts to an interface only to test validity
  /// runs (`min`, `max`): noteIfaceConversion records nothing.
  bool NoIfaceNote = false;
  /// GEN-14: a method of a generic type that names another method (a call, a
  /// method value, a method expression, through a pointer, a promoted method)
  /// whose receiver instance is Recv. The caller needs the unit of its own
  /// receiver parameters that the callee's needs come from (untaggedCheck).
  struct MethodEdge {
    const Func *Caller;
    const Func *Callee; // the declared method
    const NamedType *Recv;
  };
  std::vector<MethodEdge> MethodEdges;
  /// Set while the unit arguments of a written type (`Vec[U]`) pass their
  /// uses on: naming a type needs no unit of the method that names it.
  bool NoMethodNote = false;
  void noteMethodUnitDict(const Func *F, const TypeParamType *TP);
  /// GEN-14: the instance of a generic whose unit parameter TPar gets the
  /// unit TArg: a unit argument made of this code's unit parameters is
  /// decided like the other uses (Use::UnitDict passes on to them).
  void unitDictUses(SourceLoc Pos, Type *TArg, TypeParamType *TPar,
                    bool ForType);
  /// GEN-14, UNI-2: an instance in which a unit type of the generic's
  /// signature or fields (Generic) leaves the range of units (an exponent
  /// past MaxExp, a factor past Real::MaxRatBits) is E0435, as the same type
  /// written in plain code is. True if it reported.
  bool unitInstanceRange(SourceLoc Pos, Type *Generic,
                         ArrayRef<TypeParamType *> TPs, ArrayRef<Type *> TAs,
                         StringRef Name);
  /// The message of unitInstanceRange, or empty.
  std::string unitRangeWhy(Type *Generic, ArrayRef<TypeParamType *> TPs,
                           ArrayRef<Type *> TAs, StringRef Name);
  /// After the bodies: every instance of a generic type that the package's
  /// code reaches (any route) whose fields or methods leave the range of
  /// units is E0435, once, at its first place in the source.
  void unitRangeScan();
  /// The UnitRangeSum of the generic declaration O (memoized, iterative).
  const UnitRangeSum *unitRangeSum(NamedType *O);
  /// Do the bounds of O's declaration prove that the instance with the
  /// arguments TArgs keeps every unit in range?
  bool unitRangeSafe(NamedType *O, ArrayRef<Type *> TArgs);
  /// A generic type held by the declaration D (in its fields or the
  /// signatures of its methods), as written there over Ps.
  struct UnitHeld {
    NamedType *To;
    NamedType *Inst;
    ArrayRef<TypeParamType *> Ps;
  };
  void unitHeldInstances(NamedType *D, llvm::SmallVectorImpl<UnitHeld> &Out);
  /// The first instance out of range among O[TArgs] and the instances its
  /// fields and method signatures hold, followed as (declaration, units)
  /// pairs, each once (no instance is made but the one reported).
  std::pair<NamedType *, std::string> unitRangeExact(NamedType *O,
                                                     ArrayRef<Type *> TArgs);
  /// GEN-14: an operation of generic code whose value depends on whether a
  /// unit becomes a plain number at the instance (Info::UnitSites): recorded
  /// for the lowering, and its unit parameters need their run-time units.
  /// Replace: a Const that recordTypeAndValue notes keeps the one a scale
  /// change made (its number is in the new unit).
  void recordUnitSite(const ast::Expr *Ex, UnitSite::Kind K,
                      const UnitCanon *Res, const UnitCanon *X,
                      const UnitCanon *Y = nullptr, bool Tag = false,
                      bool Replace = true);
  std::vector<UseRec> *UseSink = &TPUses;
  bool NotingPassed = false; // the uses noted now are given on (Use::Passed)
  /// Notes U for T if T is a type parameter (its original, RenamedFrom).
  void noteUse(Type *T, TypeParamType::Use U);
  /// Notes a Make use for every type parameter that the instance Holder of
  /// generic code holds by value (TYP-19, M3: `Option[T]`, `Box[T]`): in a
  /// body, and in a signature, a generic type or an alias where the instance
  /// is Behind a pointer, a view or an interface (`*Box[T]`, `View[Box[T]]`).
  void noteHeldTParams(Type *Holder, ArrayRef<Type *> Args,
                       bool Behind = false);
  /// NamedType::HeldParams of the generic declaration O, computed
  /// iteratively over the declarations it holds (no instance is expanded).
  uint64_t heldParams(NamedType *O);
  /// Does a value of type T hold the type parameter TP by value (the parts
  /// holdsByValue walks; an instance through heldParams)?
  bool holdsTParam(Type *T, TypeParamType *TP);
  /// After all bodies: the uses travel to the type parameters they are
  /// passed to (a fixpoint: deciding a use on a type argument that is, or
  /// points to, a type parameter notes it there), then each pending
  /// instantiation is decided. Order independent: the uses are sets, the
  /// errors follow Pend and the sorted uses.
  void untaggedCheck(ArrayRef<UntaggedPending> Pend);
  /// The uses of a generic whose body does not show them: `mem.Replace(p,
  /// v)` is `old := *p; *p = v`, a Copy use of its T (R13, R22), decided at
  /// every instantiation (a call, a function value, through a generic).
  void noteIntrinsicUses();
  /// Decides the use U of TPar on the real type argument TArg with the
  /// predicate of the plain code: why the instantiation is an error, or
  /// empty. A use that TArg gives on to a type parameter is noted
  /// (NotingPassed).
  std::string useWhy(Type *TArg, TypeParamType *TPar,
                     const TypeParamType::Use &U);
  /// A type argument whose instantiation useWhy may reject or pass a use on
  /// from: the others are decided at once (nothing to note, nothing to
  /// reject).
  bool mayBreakUses(Type *TArg);
  struct InferError {
    SourceLoc Pos;
    std::string Msg;
  };
  /// Infers the missing type arguments (types2 infer); an empty result means
  /// failure, with Err set unless an argument was invalid.
  SmallVector<Type *, 4> infer(SourceLoc Pos, ArrayRef<TypeParamType *> TParams,
                               ArrayRef<Type *> TArgs, TupleType *Params,
                               ArrayRef<Operand> Args, bool Reverse,
                               InferError *Err);
  Func *instantiatedMethod(NamedType *Inst, unsigned I);
  SignatureType *instantiateFuncSig(SignatureType *Sig, ArrayRef<Type *> TArgs);

  // --- ownership (Ownership.cpp)
  bool own() const { return S.Opts.Ownership && !S.Opts.GoCompat; }
  Category category(Type *T);
  bool isOwning(Type *T) { return category(T) == Category::Owning; }
  /// A type switch or assertion binding of this type borrows the box (TYP-9)
  /// and is a view of it (OWN-V1): every type that is not a plain copy
  /// (copiesOut), except an interface, a view and a handle the box holds
  /// itself (boxHandle: its target is not in the box; an owning Unique
  /// binding still borrows it).
  bool bindsBox(Type *T) {
    if (!isValid(T))
      return false;
    if (llvm::isa<TypeParamType>(T))
      return !copiesOut(T);
    if (isInterface(T) || isViewType(T)) // a view: the box holds no target
      return false;
    if (boxHandle(T))
      return isOwning(T);
    return !copiesOut(T);
  }
  /// A handle the box of an interface holds itself (TYP-9): a pointer (also
  /// a named pointer type), a pointer kind, unsafe.Pointer, a func or a chan.
  /// Bound out of the box, it is that handle: what it reaches is not in the
  /// box (as `case *Doc:` keeps a writable target).
  bool boxHandle(Type *T) {
    Type *U = under(T);
    return llvm::isa<PointerType>(U) || llvm::isa<SignatureType>(U) ||
           llvm::isa<ChanType>(U) || isUnsafePointer(U) ||
           hasPtrKind(T, nullptr, nullptr);
  }
  bool isCopyType(Type *T);
  /// Is type parameter P Copy (Clone)? Implicit (if given): only because of
  /// a type set with a ~T term, so every instantiation must be too.
  bool paramIs(TypeParamType *P, bool Clone, bool *Implicit = nullptr);
  std::string notCopyWhy(Type *T);
  /// ByValue: T is cloned as a value (an abstract type cannot be); false
  /// for the target of a clone through the type table (ADR-0023 decision 16).
  bool satisfiesClone(Type *T, std::string *Why, bool ByValue = true);
  bool cloneWalk(Type *T, std::string *Why); // satisfiesClone's walk
  bool hasPtrKind(Type *T, PtrKind *K, Type **Elem);
  bool isPlaceExpr(const ast::Expr *E);
  /// What an expression is for the rules of OWN-R3a.
  enum class PlaceKind { Temp, Local, BorrowedVar, OtherPlace };
  PlaceKind placeKind(const ast::Expr *E);
  /// OwnedParts: indexing a slice or map that the root owns is not an
  /// indirection (it dies with the root).
  Var *rootVar(const ast::Expr *E, bool &ThroughIndirection,
               bool OwnedParts = false);
  /// The borrowed variable that E is, or is a part of by value (fields, array
  /// elements; not through a slice, map or pointer), or null (OWN-R3b).
  Var *borrowedRoot(const ast::Expr *E);
  Object *identObj(const ast::Expr *E);
  bool isBuiltinCall(const ast::Expr *E, BuiltinId Id);
  /// True if Arg is the place that also receives the result of Call in the
  /// same assignment (`p = append(p, ...)`, `b = f(b)`): the implicit move of
  /// OWN-R4.
  /// True if A and B name the same place: equal text and no side effects in
  /// any index or selector operand (`xs[f()]` is never the same as `xs[f()]`).
  bool samePlace(const ast::Expr *A, const ast::Expr *B);
  bool isImplicitMove(const ast::Expr *Arg, const ast::Expr *Call);
  /// Context words: "initialize from", "return", "send", ...
  void checkMoveRecvUse(const SelectorExpr *Sel, const char *What);
  bool conversionShares(Type *From, Type *To);
  const ast::Expr *sharedConversionArg(const ast::Expr *E);
  void needOwnership(const Operand &X, const char *What, Type *Dst = nullptr);
  bool isTempOrSharedTempPart(const ast::Expr *Ex);
  bool isOwnedTempPart(const ast::Expr *Base);
  /// Lhs is written; Repoint: a borrowed string / []T is re-pointed (PAR-2);
  /// Redeclare: by a `:=` that reuses the variable (the fix is a new name).
  void checkWrite(const ast::Expr *Lhs, bool Repoint = false,
                  bool Redeclare = false);
  void checkAddressOf(const UnaryExpr *E, Operand &X);
  /// OWN-R3b: the one rule for a writable reference into a borrowed
  /// variable's own storage (Ownership.cpp); true after a report.
  enum class BorrowedRef { Address, PtrMethod };
  bool rejectBorrowedRef(const ast::Node *At, const ast::Expr *Inner,
                         BorrowedRef How, StringRef Method = "");
  /// The same rule for slicing an array: is the array held by value in a
  /// borrowed variable (its slice is then a view)?
  bool borrowedStorageSlice(const SliceExpr *Ex);
  // --- writes into maps and slices (P03.19, Ownership.cpp)
  /// How the storage an expression denotes relates to the shared box of an
  /// interface value: not in one; the value the box holds (or a part of it by
  /// value that is no map or slice element); or inside a map or slice of it.
  enum class BoxKind { None, Direct, Via };
  /// The Option Get bindings of the outermost body being checked whose
  /// Option is a part of the box (`d, ok := w.Get()` on a binding or
  /// assertion of an `Option[T]`): box bindings too. Kept per checker.
  llvm::DenseSet<const Var *> BoxGetBindings;
  bool declaredPointerPart(const ast::Expr *Ex);
  /// OWN-V6: the locals of the outermost body being checked that hold a view
  /// into a temporary kept alive to the end of their block (`w :=
  /// f()[0:3]`, `var w = f()[0:3]`), and the locals such a view was copied
  /// into. Kept per checker, like BoxGetBindings.
  llvm::DenseSet<const Var *> TempViews;
  /// The TempViews variable whose view R is (`w`, `w[1:]`), or null.
  const Var *tempViewVar(const ast::Expr *R);
  /// Is the view R a view into a temporary (checkRefStore's question)?
  bool refIntoTemp(const ast::Expr *R, Type *Dst, Type *SrcTy);
  /// A type switch, assertion or box Option Get binding.
  bool boxBinding(const Var *V);
  /// Where Ex lies, for a direct write (P03.19): rooted at a box binding or
  /// an assertion through fields, elements, slicing and sharing conversions.
  BoxKind boxPlace(const ast::Expr *Ex);
  /// V is the value binding of `V, ok := Opt.Get()`.
  void noteBoxGet(const Var *V, const ast::Expr *Opt);
  /// Report a write through Place (the container itself with AnyDepth, as
  /// `delete(m, k)`, `clear(s)`, `copy(s, t)`; else a place inside a
  /// container) when it lies in the box of an interface value. At is where,
  /// What says what is written. Returns true if reported.
  bool rejectBoxWrite(const ast::Expr *At, const ast::Expr *Place,
                      bool AnyDepth, const std::string &What);
  /// Report a write into the map MapEx when it is a temporary.
  bool rejectTempMapWrite(const ast::Expr *At, const ast::Expr *MapEx);
  /// The checks of an assignment target, `x op= y`, `x++`: Lhs is written.
  void checkContainerStore(const ast::Expr *Lhs);
  void checkPtrRecvCall(const SelectorExpr *Sel, Func *M);
  void checkSigOwnership(const ast::FuncType *FT, SignatureType *Sig,
                         ArrayRef<Var *> Params, ArrayRef<Var *> Results);
  void checkMoveRecvSig(Func *F, const FuncDecl *FD);
  /// `Clone() T` needs a value receiver (OWN-R6); `drop` has the shape
  /// `func (x *T) drop()` (OWN-R7).
  void checkSpecialMethod(Func *F, const FuncDecl *FD);
  void checkReturnRef(const ast::Expr *R, Type *Want);
  /// OWN-V6: a ref into a temporary stored in a variable, field or element.
  void checkRefStore(const ast::Expr *X, Type *Dst, Type *SrcTy = nullptr);
  /// D3.9: a new Unique/Shared stored in a `*T` field. Reports, invalidates X
  /// and returns true.
  bool checkPointerField(Operand &X, Type *FieldTy);
  bool isPinned(Type *T);
  /// T with its type parameters replaced by shared stand-ins
  /// (TypeContext::canonicalParam), for the type-level queries whose answer
  /// only depends on a type parameter's constraint; T itself when it has
  /// none, or when that is not safe (see Ownership.cpp).
  Type *canonicalParams(Type *T);
  bool isPinnedRec(Type *T, bool &Exact);
  /// Zero values (TYP-15, OWN-R9): false for a pointer kind, a struct with a
  /// `@Required` field or a C++ type without a default constructor, and for
  /// anything that contains one; for a type parameter, false unless every
  /// term of its constraint has one (GEN-5). Why gets the reason. True when
  /// not in Go++ mode.
  bool hasZeroValue(Type *T, std::string *Why = nullptr);
  /// True when T has no zero value in every instance: unlike !hasZeroValue,
  /// false for a type whose answer depends on a type parameter (GEN-5).
  bool lacksZeroValueForSure(Type *T);
  /// Like !hasZeroValue, but a foreign (C++) type counts as having one: the
  /// types the move rule of ADR-0016 point 5 is about.
  bool lacksGoZeroValue(Type *T);
  /// Reports "What: T has no zero value (why[; Hint])" at Pos unless T has
  /// one.
  void needZeroValue(SourceLoc Pos, Type *T, std::string What,
                     std::string Hint = "");
  /// The ranges being checked (RNG-3): the range expression and the key
  /// variable, and whether the body wrote the key already.
  struct RangeFrame {
    std::string X;
    const ast::Expr *XE = nullptr; // the range expression, a stable place
    Type *XTy = nullptr;
    Var *Key = nullptr;
    bool KeyWritten = false;
    bool User = false; // a `range` method: X stays borrowed (RNG-6b)
  };
  SmallVector<RangeFrame, 4> RangeStack;
  /// RNG-6b: a write to, or a pointer-method call on, the place that a
  /// `range` method is looping over.
  void checkUserRangeWrite(const ast::Expr *Place, const std::string &What);
  /// RNG-3: What is "assign to", "append to", ...; Lhs the place written.
  void checkRangeMutation(const ast::Expr *Lhs, const char *What);

  /// A struct field marked `@Required` whose type can be nil (interface,
  /// func, map, channel): never nil, checked at compile time (ADR-0016
  /// point 8). Slices and type parameters only mean "must be set".
  bool isNeverNilField(const Var *F);
  /// Does `@Required` on a field of type Ty mean "never nil" (as opposed to
  /// only "must be set")? The one list isNeverNilField reads.
  bool requiredMeansNeverNil(Type *Ty);
  /// How a message says why the required field F is required: "@Required"
  /// where it is marked, "required (@Strict)" where only its struct's
  /// `@Strict` makes it so (TYP-15a).
  static const char *requiredWhy(const Var *F) {
    return F->MarkedRequired ? "@Required" : "required (@Strict)";
  }
  /// The selected field of a selector expression Ex, or null.
  const Var *selectedField(const ast::Expr *Ex);
  /// Lookups that can fail (ADR-0016 point 9): `m[k]` and `<-ch` of a type
  /// with no zero value are allowed only where the caller marked them before
  /// checking them (a comma-ok form, the target of `m[k] = v`, the argument
  /// of clone / move in a comma-ok form, a bare `move(m[k])` or `<-ch`
  /// statement); everything else is an error, reported where the lookup is
  /// checked.
  llvm::SmallPtrSet<const ast::Expr *, 8> ZeroLookupOk;
  /// MoveOnly: only a `move(m[k])` call is marked (a bare statement).
  void markLookupOk(const ast::Expr *Ex, bool MoveOnly = false);
  /// False (after the error) if the lookup Ex of a value of type Elem is
  /// not allowed here.
  bool checkLookupZero(const ast::Expr *Ex, Type *Elem);
  /// The places written by the assignment being checked: a `move` out of one
  /// of them is refilled in the same statement (a swap, ADR-0016 point 5).
  SmallVector<const ast::Expr *, 4> RefillLhs;
  /// The places moved so far by the statement (each at most once).
  SmallVector<const ast::Expr *, 4> RefillMoved;
  /// The first argument of the mem.Replace call being checked (the `&place`
  /// that may name a never-nil field), and its second argument when it is the
  /// literal nil.
  const ast::Expr *ReplaceArg = nullptr, *ReplaceNil = nullptr;
  /// The never-nil field whose address that call takes (when its second
  /// argument is not the literal nil), and how to name it: the new value
  /// must be known non-nil (TYP-15).
  const Var *ReplaceField = nullptr;
  std::string ReplacePlace;
  /// True if Fun is `mem.Replace` (package path "mem"; the package is P05's,
  /// a module may provide it until then).
  bool isMemReplace(const ast::Expr *Fun);
  /// Why Arg cannot be the value type of an Option (it has a nil already, or
  /// is an Option), or null (OWN-P9).
  const char *badOptionArg(Type *Arg);
  const char *badViewArg(Type *Arg);
  /// OWN-P9 per instance: an instance Inst of a generic type or function with
  /// the type arguments TArgs must not hold an Option of one of them that
  /// cannot be (Option[T] with T = error). Reported at Pos.
  void checkOptionArgs(SourceLoc Pos, Type *Inst, ArrayRef<Type *> TArgs);
  /// TYP-15a per instance: an instance Inst (from a type expression) of a
  /// generic `@Strict` struct whose `V T @Option` field gets a T with no zero
  /// value. Reported at Pos.
  void checkOptionFields(SourceLoc Pos, Type *Inst);

  // --- flow typing of Option (Flow.cpp, ADR-0018 point 4)
  // The state is the set of Option locals and parameters known to be set at
  // the point being checked, per checker (so per body and thread). A use of
  // one is recorded with the type T instead of Option[T] (Info::narrowedType).
  // An assignment ends it (PendingKills, applied at the end of the
  // statement); where paths meet, only what every path knows is kept; a loop
  // top is the meet of the state before it and at its back edges; a function
  // literal starts empty.
  struct FlowState {
    llvm::SmallPtrSet<const Var *, 8> N;
    /// Checked results and comma-ok values (P03.18, ADR-0018 point 4): the
    /// locals whose value may be invalid right now (their type has no zero
    /// value and the `err` / `ok` that came with them is not known good),
    /// each with the guard variable whose good state makes them valid
    /// (null: nothing can, until the variable is assigned again).
    llvm::DenseMap<const Var *, const Var *> Pend;
    /// The tentative entries of Pend: put there at a loop head or a goto
    /// label for a variable that a multi-value assignment of the loop or body
    /// binds again, before that assignment is checked. The regions (indices
    /// into FlowRegions) decide it at the end of the body: a loop region by
    /// the states at its back edges (flowLoopBack), a label region by whether
    /// one of its assignments is a checked form binding the variable
    /// (RealForms); a use of it waits for that (TentUses).
    llvm::DenseMap<const Var *, SmallVector<unsigned, 2>> Tent;
    /// The entries of N that hold only if a loop's assumption does: a local
    /// that the loop assigns is assumed set at the loop head (the entry state
    /// says so), and the back edges of the loop decide it at its end
    /// (loop fixpoint, FlowRegion::NEdge). The regions it depends on; read
    /// only for members of N.
    llvm::DenseMap<const Var *, SmallVector<unsigned, 2>> TentN;
    /// Local channel variables that are nil on every path to here (CON-6a):
    /// declared with no value or with nil, or assigned nil, and not assigned
    /// since.
    llvm::SmallPtrSet<const Var *, 4> Nil;
    bool Dead = false; // unreachable: everything counts as known
  };
  struct FlowFacts {
    llvm::SmallPtrSet<const Var *, 4> True, False;
  };
  /// A for, switch or select statement that `break` can leave: the state
  /// after it joins the states at its breaks.
  struct FlowTarget {
    std::string Label;
    FlowState Break;
    bool HasBreak = false;
    /// A loop: the states at its `continue`s (back edges to the head).
    bool IsLoop = false;
    FlowState Cont;
    bool HasCont = false;
  };
  FlowState Cur;
  /// Checked results (P03.18, Flow.cpp): the locals of the outermost body
  /// being checked that a checked-result or comma-ok form bound, and how to
  /// say what makes each valid.
  llvm::SmallPtrSet<const Var *, 8> GuardedVars;
  llvm::DenseMap<const Var *, std::string> GuardText;
  /// Variables given a guard by the statement being checked (applied at its
  /// end, after the kills of the variables it assigns).
  SmallVector<std::pair<const Var *, const Var *>, 4> PendingGuards;
  /// Uses of captured variables with no zero value in function literals of
  /// the outermost body: errors if a form binds the variable anywhere.
  SmallVector<std::pair<const ast::Ident *, const Var *>, 4> CapturedUses;
  /// The locals of the outermost body that a reference into their storage
  /// was taken of (`&v`, `&v.F`, a pointer-receiver method value, ...), where
  /// first; and the forms that bind an existing local again. A form that can
  /// fail must not bind a variable a reference can still see.
  llvm::DenseMap<const Var *, SourceLoc> RefTaken;
  struct Rebinding {
    const ast::Expr *At;
    const Var *V;
    std::string Rhs, What;
  };
  SmallVector<Rebinding, 2> Rebindings;
  /// A reference into the storage of the place Ex is made (it can outlive
  /// the statement).
  void noteReference(const ast::Expr *Ex);
  /// The value names of the multi-value assignments of the body being
  /// checked, when it has a goto (a label may be reached again after any of
  /// them), and the region of those assignments (-1: none).
  SmallVector<std::string, 4> LabelForms;
  int LabelRegion = -1;
  /// The loop and label regions of the outermost body (see FlowState::Tent
  /// and TentN). A label region holds the right sides of the body's
  /// multi-value assignments and is decided by RealForms; a loop region by
  /// the states at its back edges (flowLoopBack): for each variable the loop
  /// assumed, the regions its state at the back edges depends on (a pending
  /// value: real at once when PendReal; a narrowed one: failed at once when
  /// NBad, it is not known set on some back edge).
  struct FlowRegion {
    llvm::SmallPtrSet<const ast::Expr *, 4> Forms; // label regions
    bool IsLoop = false;
    bool Closed = false; // the loop is checked: no back edge comes any more
    llvm::DenseSet<const Var *> PendReal, NBad;
    llvm::DenseMap<const Var *, SmallVector<unsigned, 2>> PendEdge, NEdge;
  };
  SmallVector<FlowRegion, 4> FlowRegions;
  /// V is pending for real at the head of region R (Flow.cpp).
  bool regionPendReal(unsigned R, const Var *V);
  /// The assumption of loop region R that V stays known set fails.
  bool regionNBad(unsigned R, const Var *V);
  /// Walk from the regions Rs for V (Pend: pending, else known set): true if
  /// a region that decides it is reached (pending for real, or failing);
  /// else Out gets the regions still open on the way (sorted).
  bool regionFrontier(ArrayRef<unsigned> Rs, const Var *V, bool Pend,
                      llvm::SmallVectorImpl<unsigned> &Out);
  /// A use of a local whose narrowing holds only if a loop's assumption does.
  /// Regions is empty for a use that was made the Option again
  /// (unnarrowTentative): it needs no assumption.
  struct TentNUse {
    const ast::Ident *Id;
    const Var *V;
    SmallVector<unsigned, 2> Regions;
  };
  SmallVector<TentNUse, 4> TentNUses;
  /// The index in TentNUses of the use of each identifier.
  llvm::DenseMap<const ast::Ident *, unsigned> TentNIndex;
  /// The declared Option type of X when X (a name, maybe in parentheses) is
  /// a use that only a loop's assumption narrows; else null.
  Type *tentOption(const Operand &X);
  /// Such a use where the Option itself fits as well (an argument, store,
  /// result or comparison that takes the Option, a variable whose type is
  /// inferred from it, a type parameter, a conversion, a switch tag): it is
  /// the Option again (its recorded type too), so whether the assumption
  /// holds does not matter there and no type depends on it (P03.19, no
  /// regression against the rule before the loop fixpoint). Returns true if
  /// X changed.
  bool unnarrowTentative(Operand &X);
  /// The end of a loop body: the back edge states decide the loop's region.
  void flowLoopBack(int Region, const FlowState &Back);
  /// The state S after the loop of Region: a fact that rests on a region that
  /// failed (this loop's or one inside it) is not known there.
  void flowLoopClean(int Region, FlowState &S);
  /// (right side, local) for every checked form that bound a local: the
  /// checker's own classification (noteGuardedForm).
  llvm::DenseSet<std::pair<const ast::Expr *, const Var *>> RealForms;
  /// A read of a tentatively pending value, with the regions that decide it.
  struct TentUse {
    const ast::Ident *Id;
    const Var *V;
    std::string Why;
    SmallVector<unsigned, 2> Regions;
  };
  SmallVector<TentUse, 4> TentUses;
  /// Report the TentUses from Top whose value is pending for real at one of
  /// their regions, and the TentNUses from TopN that rest on an assumption
  /// that failed (the end of a body, when every form and loop is checked).
  void flowDecideTentative(size_t Top, size_t TopN);
  /// Selector names used on each plain name in the body (`x.M`): a
  /// pointer-receiver method among them takes the address of x implicitly.
  llvm::StringMap<llvm::StringSet<>> MethodSels;
  bool takesImplicitAddress(const Var *V);
  /// `return f()` / `h(f())`: the values Ops of a checked form Call handed on
  /// as they are, into Into. LastInto is the type of the receiving slot of
  /// the form's guard (null for the arguments of a call).
  void checkForwarded(const ast::Expr *Call, ArrayRef<Operand> Ops,
                      Type *LastInto, const char *Into);
  /// The identifier written (not read) by the assignment being checked.
  const ast::Expr *NoGuardExpr = nullptr;
  const ast::Ident *LastGuardErr = nullptr;
  bool AssignReadsLhs = false; // `x op= y` reads x
  enum class GuardKind { None, Err, Ok };
  void checkSpreadCall(const ast::Expr *Ex, ArrayRef<Operand> Ops);
  /// An `error` (maybe nil) converted to an interface with methods (never
  /// nil), where it is not known set: report it, return true.
  bool checkNilableToNeverNil(Operand &X, Type *T);
  /// Records Info::IfaceConversions when a value of type From (no interface,
  /// or a type parameter) at E becomes a value of the interface type To, or
  /// of an Option of one (P04.08).
  void noteIfaceConversion(const ast::Expr *E, Type *From, Type *To);
  /// What kind of checked form `v, x := Rhs` (operands Ops) is.
  GuardKind guardKindOf(const ast::Expr *Rhs, ArrayRef<Operand> Ops,
                        bool CommaOk);
  /// Bind the value targets of a checked form to their guard (end of the
  /// statement), or report a value stored where nothing can check it. Vars[k]
  /// is the local target k (null for a blank or a place); Places[k] its
  /// expression (null when unknown).
  void noteGuardedForm(ArrayRef<Var *> Vars, ArrayRef<const ast::Expr *> Places,
                       const ast::Expr *Rhs, ArrayRef<Operand> Ops,
                       bool CommaOk);
  /// "it is valid only where G == nil: check ... first" for the guard G.
  std::string guardHint(const Var *G);
  /// Report a read of a local whose value may be invalid here.
  void guardedUse(const ast::Ident *Id, Var *V);
  /// The checks that need every form of the outermost body (at its end): the
  /// CapturedUses of checked results and the Rebindings of referenced ones.
  void flowEndBody();
  /// The narrowable local of type error or bool Ex names (a guard).
  Var *guardVarOf(const ast::Expr *Ex);
  /// The local of this function that Name denotes at Pos if its type has no
  /// zero value (it can hold a checked result), or null.
  Var *checkableLocal(StringRef Name, SourceLoc Pos);
  /// The narrowable local of a nilable type (error, any, map, channel) or
  /// Option Ex names: `x != nil` makes it known non-nil.
  Var *nilCheckVarOf(const ast::Expr *Ex);
  /// Ex is known to be non-nil as the value of a never-nil @Required error,
  /// any, map or channel field (TYP-15's list). Target is the field's type.
  bool knownNonNil(const ast::Expr *Ex, Type *Target, Type *SrcTy = nullptr);
  /// A store of X into the never-nil @Required field Fld (Place names it).
  void checkNeverNilStore(const Operand &X, const Var *Fld,
                          const std::string &Place);
  /// The values that a condition on the guard G is a fact about.
  void addGuarded(const Var *G, llvm::SmallPtrSetImpl<const Var *> &Into);
  void flowAddTo(FlowState &S, const llvm::SmallPtrSetImpl<const Var *> &Vars);
  /// The state after a label that a goto names (at Pos): nothing is known.
  void flowReset(SourceLoc Pos);
  /// The loop head (the loop fixpoint, Flow.cpp): the state before the loop
  /// with the assumptions that the back edges of the loop decide. Returns the
  /// loop's region (-1: no flow typing here).
  int flowLoopHead(const ast::Node *Loop, SourceLoc BodyPos);
  /// The values that the statements Ns rebind with a multi-value assignment
  /// are pending (tentatively, until it is known which are checked forms)
  /// when control comes back to Pos from their end (a loop head, a post
  /// statement).
  void flowRebound(ArrayRef<const ast::Node *> Ns, SourceLoc Pos, int Region);
  /// GuardText of V when its guard G stops guarding it ("G <What>, so ...").
  void untieText(const Var *V, const Var *G, StringRef What);
  SmallVector<FlowTarget, 4> Targets;
  /// The label of the statement about to be checked (for `break L`).
  std::string PendingLabel;
  /// The state at a `fallthrough` into the next case.
  FlowState FallState;
  bool HasFall = false;
  SmallVector<const Var *, 4> PendingKills;
  /// Channels the statement being checked leaves nil (applied after the
  /// kills at its end).
  SmallVector<const Var *, 2> PendingNil;
  /// Names used inside a function literal anywhere in the body: a channel
  /// captured by one is never known nil (a literal may assign it).
  llvm::StringSet<> ClosureNames;
  /// Names assigned inside a function literal, or whose address is taken,
  /// anywhere in the function: never narrowed, a call may change them.
  llvm::StringSet<> NoNarrowNames;
  /// Labels that a goto names: nothing is known where they are.
  llvm::StringSet<> GotoLabels;
  /// The identifier that must be evaluated as the Option itself (the operand
  /// of `== nil`, the receiver of IsNil / Get, an assignment target, the
  /// operand of `&`).
  const ast::Expr *NoNarrowExpr = nullptr;
  static FlowState flowJoin(const FlowState &A, const FlowState &B);
  void flowAdd(const llvm::SmallPtrSetImpl<const Var *> &Vars);
  /// What Cond says about the Option variables when it is true / false.
  void flowFacts(const ast::Expr *Cond, FlowFacts &Out);
  void flowFactsRec(const ast::Expr *Cond, FlowFacts &Out);
  /// Names passed to `move(name)` in a subtree.
  void collectMoved(const ast::Node *N, llvm::StringSet<> &Names);
  /// `move(o)` of an Option local: o is nil again, so narrowing ends right
  /// after the move expression (ADR-0018 point 11).
  void flowMoved(const ast::Expr *Arg);
  /// The Option locals moved so far in this body, in order (an operand of
  /// `&&` / `||` restores the state; the moves in it are applied again).
  SmallVector<const Var *, 4> MoveLog;
  /// The narrowable Option local or parameter Ex names (after it is checked).
  Var *optionVarOf(const ast::Expr *Ex);
  /// The checked Option local o when the place Ex is stored inside o's
  /// payload: o itself, or a part reached by fields and array elements held
  /// by value (not through a pointer, slice, map or string: that is other
  /// storage), also through a variable that borrows such a part (see
  /// PayloadBorrows; then o need not be narrowed any more). Else null.
  Var *payloadOption(const ast::Expr *Ex);
  /// Borrowed variables (a plain range variable over an array held by value,
  /// a `v, ok := o.F.Get()` binding) -> the checked Option whose payload
  /// they borrow from, so a reference made from them is one into it.
  /// Recorded only while o is narrowed; cleared at each outermost body.
  llvm::DenseMap<const Var *, Var *> PayloadBorrows;
  /// Borrower borrows the place Ex: record it if Ex is in a payload.
  void notePayloadBorrow(Var *Borrower, const ast::Expr *Ex);
  /// A construct makes a reference to the storage of the place Ex that can
  /// outlive the statement (OWN-P10; the audit is at the top of Flow.cpp):
  /// `&Ex`, a pointer-receiver method value or go / defer call on Ex, `Ex[i:j]`
  /// of an array, an `&` range variable over the array Ex. If Ex is in the
  /// payload of a checked Option o, `o = nil` would leave zero bytes under the
  /// reference: report E0329 and return true.
  bool rejectPayloadAddress(const ast::Expr *Ex, const char *What);
  /// Whether V (an Option variable) may be narrowed here: a local or
  /// parameter of the function being checked (not a captured variable,
  /// CLO-9) that no function literal assigns and whose address is not taken.
  bool narrowable(const Var *V);
  /// The type of a use Id of V (type T): the value type where V is known to
  /// be set, T otherwise.
  Type *narrowedType(const ast::Ident *Id, Var *V, Type *T);
  /// How to check Ex (an unchecked Option) before using its value: `check o
  /// != nil first` for a local that narrows, else a local copy to check.
  std::string nilCheckHint(const ast::Expr *Ex);
  /// V is known to be set at this point.
  bool isNarrowed(const Var *V) {
    return narrowable(V) && (Cur.Dead || Cur.N.count(V));
  }
  bool isNilLit(const ast::Expr *Ex);
  /// `nil`, or a conversion of nil to a type (`(chan int)(nil)`).
  bool isNilValue(const ast::Expr *Ex);
  /// Known nil channels (CON-6a): `ch` is a local channel variable that is
  /// nil on every path to here.
  bool knownNilChan(const ast::Expr *Ex);
  void collectAssigned(const ast::Node *N, llvm::StringSet<> &Names);
  void flowKillNames(const llvm::StringSet<> &Names);
  void flowFlushKills();
  void flowDead() { Cur.Dead = true; }
  void flowBranch(const ast::BranchStmt *B);
  void flowPushTarget(bool IsLoop = false);
  FlowTarget flowPopTarget();
  void flowBeginBody(const BlockStmt *Body);
  void markNoNarrow(const ast::Expr *Ex);

  // =========================================================================
  // Units of measure (UnitCheck.cpp, ADR-0025)
  // =========================================================================
  /// The unit of a type: a unit type, or a named type over one (UNI-8); null
  /// for every other type. The one question every unit rule asks.
  const UnitCanon *unitOf(Type *T);
  BasicKind baseKind(Type *T);
  Type *withUnit(BasicKind BK, const UnitCanon *U);
  bool isUnitBound(Type *Bound);
  bool isUnitParam(Type *T);
  const UnitCanon *unitArgCanon(Type *T);
  // --- declarations (UNI-1 to UNI-3)
  SmallVector<UnitObj *, 8> UnitPath;
  void resolveUnit(UnitObj *U);
  void unitDecl(UnitObj *U, DeclInfo *D);
  void reportUnitCycle(UnitObj *At);
  bool unitFactorValue(UnitObj *U, const ast::Expr *Factor, Real &Out);
  void readUnitSymbol(const GenDecl *GD, UnitObj *U);
  void addImportedUnits(Package *P);
  // --- unit expressions (UNI-4, UNI-5, UNI-7)
  Object *unitLookup(const ast::Expr *N);
  bool unitNamePart(const ast::Expr *N, UnitDim &Part, const UnitCanon *&Canon);
  /// The canonical form of a unit expression, and its display form in Disp
  /// when asked (UNI-4, UNI-5); null after an error. Iterative.
  const UnitCanon *unitExpr(const ast::Expr *X,
                            const UnitDisplay **Disp = nullptr);
  Type *unitArgType(const ast::Expr *X);
  void unitStatusError(const ast::Node *At, UnitStatus St, StringRef What);
  Type *unitTypeIndex(const IndexExpr *Ix);
  bool unitZeroFits(const Operand &X, const BasicType *Target);
  std::string unitStr(const UnitCanon *U) const {
    return units::str(U, S.Pkg, &unitNamer());
  }
  /// The display forms this checker prints (UNI-5): the package's, and in a
  /// body checker the forms of the body on top of them.
  units::Namer BodyUnitNames;
  units::Namer &unitNamer() { return Bodies ? BodyUnitNames : S.UNamer; }
  const units::Namer &unitNamer() const {
    return Bodies ? BodyUnitNames : S.UNamer;
  }
  // --- display forms (UNI-5)
  /// The forms of the local variables initialized from a unit expression
  /// (those of package variables are in PackageState).
  llvm::DenseMap<const Var *, const UnitDisplay *> UnitVarDisplays;
  /// The form of the result of the last `x *= y` / `x /= y` (no expression
  /// of its own: the left operand `x` is the operand of the message).
  const ast::Expr *CompoundE = nullptr;
  const UnitDisplay *CompoundD = nullptr;
  const UnitDisplay *unitDisplayOf(const ast::Expr *Ex, const UnitCanon *U);
  std::string unitStrOf(const Operand &O);
  std::string unitTypeStrOf(const Operand &O);
  void noteUnitDisplay(const ast::Expr *Ex, const UnitCanon *U,
                       const UnitDisplay *D);
  void noteUnitVar(Var *V, const Operand &X);
  void noteUnitVarType(Var *V, const ast::Expr *TypeExpr);
  // --- values (EXP-31 to EXP-37)
  Type *unitRetype(Type *T, const UnitCanon *U);
  bool unitConversion(Operand &X, Type *T);
  void unitNoteConversion(Operand &X, const CallExpr *Call);
  /// The receiver of each `x.In` selector, and the unit (with its form) of
  /// each `x.In[U]`.
  llvm::DenseMap<const SelectorExpr *, Operand> UnitRecv;
  llvm::DenseMap<const IndexExpr *,
                 std::pair<const UnitCanon *, const UnitDisplay *>>
      UnitInArg;
  bool unitInSelector(Operand &X, const SelectorExpr *Ex);
  void unitInIndex(Operand &X, const IndexExpr *Ix);
  void unitInCall(Operand &X, const CallExpr *Call);
  void unitLit(Operand &X, const ast::UnitLit *Lit);
  void recordUnitConv(const ast::Expr *Ex, const Real &Factor, bool Truncate,
                      bool Tag);
  bool unitScale(Operand &O, const UnitCanon *To, Type *NewTy, bool Explicit,
                 const ast::Node *At, const ast::Expr *Key = nullptr,
                 bool Tag = false);
  bool unitMatch(Operand &X, Operand &Y, tok::Kind Op, bool Compound,
                 const ast::Expr *Ex, const ast::Expr *CaseKey = nullptr);
  bool unitMinMax(llvm::SmallVectorImpl<Operand> &Args, const CallExpr *Call);
  bool unitBinary(Operand &X, Operand &Y, const ast::Expr *Ex,
                  const ast::Expr *L, const ast::Expr *R, tok::Kind Op,
                  SourceLoc OpLoc);
  /// EXP-33 (R5): `x / y` of two integer values of one dimension: both go to
  /// the finer unit first, then the numbers divide, as `+` does. False after
  /// an error (X invalid).
  /// `real`, `imag` and `complex` keep the unit of the argument: Res, the
  /// plain result type, with the unit of From (EXP-35). Out of line: the
  /// caller recurses.
  Type *unitPartType(Type *From, Type *Res);
  /// GEN-14 (R1): a term of a type set (`interface{ float64[M] | float64[S]
  /// }`, `~float64[M]`) that has a unit is an error: the body would take the
  /// term for a plain number type. Decided later, when the type is complete.
  void unitTermCheck(const ast::Expr *Term, Type *T);
  /// R3: the factor R that a scale change applies to a value of type Ty must
  /// be a number of that kind: not 0 or infinite in a float kind, a whole
  /// number (or, truncating, a ratio of two numbers) that fits an integer
  /// kind. One error at At if not; false then.
  bool unitFactorFits(const ast::Node *At, const Real &R, Type *Ty);
  bool unitIntQuo(Operand &X, Operand &Y, const ast::Expr *Ex);
  bool unitMulDiv(Operand &X, Operand &Y, const ast::Expr *Ex,
                  const ast::Expr *L, const ast::Expr *R, tok::Kind Op,
                  SourceLoc OpLoc);
  // --- out of line for the frames that recurse per nesting level (M4)
  Type *unitParamAsType(const ast::Expr *Ex);
  Type *unitAsType(const ast::Expr *Ex);
  void unitNotExpr(const ast::Expr *Ex);
  void unitExprAsValue(const ast::Expr *Ex);
  bool unitBitNot(Operand &X, const ast::UnaryExpr *Ex);
  void unitParamCall(const CallExpr *Call);
  bool unitIncDec(const ast::IncDecStmt *ID, const Operand &X);
  bool unitInAfterError(const IndexExpr *Ex);
  void unitCombinedConstraint(SourceLoc At);
  /// How typeList reads the type argument Ex for the parameter Want[I]: 0
  /// as a type, 1 as a unit (GEN-14), 2 not at all (a unit for a type
  /// parameter: reported).
  int unitArgAt(const ast::Expr *Ex, ArrayRef<TypeParamType *> Want, size_t I);
  /// Is Ex written as a unit: a unit expression, a unit name, or `*`, `/`
  /// and parentheses of such names and `1`? (No errors, no uses recorded.)
  bool unitLooksLike(const ast::Expr *Ex);
  Type *unitSubstBasic(Type *T, const llvm::DenseMap<const Type *, Type *> &M);
  bool unitIndexLiteral(Operand &X, const IndexExpr *Ex);

  // --- scopes
  Object *lookup(StringRef Name) { return E.Sc->lookup(Name, E.Pos); }

  // --- misc state
  bool SwitchCaseCmp = false;
  /// A borrowed string / []T parameter takes a ref at zero cost (PAR-8), and
  /// so does a map key used for a lookup (OWN-V4).
  bool RefToOwnedOk = false;
  bool ViewKeyLookup = false; // a map key lookup takes a View key (OWN-V4)
  bool NoOwnCheck = false;
  /// Checking the init statement of a three-clause for: its temporaries are
  /// not kept (STM-4, OWN-V6).
  bool InForInit = false;
  void headerInit(const ast::Stmt *Init);
  /// The selector being evaluated as the callee of a call.
  const ast::Expr *CalleeSel = nullptr;
  /// The callee is the call of a `go` or `defer` statement: it runs later.
  bool CalleeSuspended = false;
  /// Call -> the place its result is assigned to (OWN-R4 implicit moves).
  llvm::DenseMap<const ast::Expr *, const ast::Expr *> AppendTargets;
  llvm::DenseSet<const NamedType *> ReportedCycle;
  /// Rings of right-hand sides found at a declaration, reported by the walk
  /// that first meets them (noteDeclRing, reportDeclRing): the type each
  /// ring member's right-hand side names, the ring of each member, and the
  /// rings not reported yet.
  llvm::DenseMap<const NamedType *, NamedType *> RingNext;
  llvm::DenseMap<const NamedType *, unsigned> RingOf;
  std::vector<std::vector<NamedType *>> DeclRings;
  std::set<unsigned> PendingRings;
  void noteDeclRing(ArrayRef<std::pair<NamedType *, NamedType *>> Links);
  void reportDeclRing(NamedType *N);
  void flushDeclRings();
  /// validTypeWalk: the numbers for types and type arguments (equal numbers,
  /// equal types), and the keys of the finished walks: valid, or failed with
  /// whether a by-value cycle was met below the key (a walk that reports
  /// may skip a failed key only without one).
  std::map<std::vector<uint64_t>, unsigned> WalkIds;
  llvm::DenseSet<unsigned> ValidKeys;
  llvm::DenseMap<unsigned, bool> FailedKeys;
  /// Types waiting for the underlying type of an instance (typeDecl).
  std::vector<std::pair<NamedType *, NamedType *>> PendingUnder;
  /// Struct types found to hold no view (containsView).
  llvm::DenseSet<const NamedType *> ViewFree;
  /// Struct types found to hold a view (containsView).
  llvm::DenseSet<const NamedType *> ViewHolders;
  /// Test-only work count of containsView's loops (the lowering asks it
  /// through MethodSets::holdsBorrow, P04.07 review R7).
  uint64_t ViewSteps = 0;
  /// Package-level types whose by-value recursion is not checked yet.
  std::vector<NamedType *> WalkQueue;
  /// validTypeWalk met a type inside its own definition.
  bool ByValueCycle = false;
  /// The types on the cycles validTypeWalk found: where the search for what
  /// is left of them starts besides the declared types.
  std::vector<NamedType *> CycleSeeds;
  /// The instances expanded while the package's types are declared (so
  /// invalidateByValueCycles can start from them); null when not recording.
  std::vector<NamedType *> *ExpandedInsts = nullptr;

  // =========================================================================
  // Operator methods (Operators.cpp, ADR-0024, EXP-16 to EXP-30)
  //
  // An operator method is a Func named by its operator, with its form in
  // Func::Op. The rules, each decided in ONE place:
  //   the form of a declaration          checkOperatorShape (+ opSecondMeaning)
  //   overload sets, duplicates          checkOperatorSets
  //   which methods a type has           opCandidates, from the per-type
  //                                      operator table (opTable: the type,
  //                                      its `extends` ancestors) or the
  //                                      constraint of a type parameter
  //   which overload an operand list     pickOverload (exact types; the
  //   picks                              untyped-constant rule)
  //   how the call is checked            opCall: the operator expression is
  //                                      checked as the method call
  //                                      Recv.M(Args) (EXP-18), once, with the
  //                                      operands evaluated before (PreEval)
  //   a partial place as the receiver    opReceiverOk (receiverWhy)
  //   a constraint element               satisfiesOpElem (GEN-13)
  // =========================================================================
  /// Operands already evaluated by the operator code: rawExpr hands them back
  /// instead of evaluating the expression again, so a left-deep chain of ten
  /// thousand `a + b + ...` is checked once, not twice at every level.
  llvm::DenseMap<const ast::Expr *, Operand> PreEval;
  /// The operator method that each synthesized selector `a.op` names: an
  /// overload set shares its name, so the selector finds the method itself
  /// (lookupFieldOrMethod with Want).
  llvm::DenseMap<const ast::SelectorExpr *, const Func *> OpSelect;
  /// The `...` of the operator call being synthesized (`a(xs...)`).
  SourceLoc PendingEllipsis;
  /// Func::Op and Func::Right of a function declaration, and the misuse of
  /// `@Right` and `@Convert` on a function (EXP-25, EXP-26).
  void readOperatorMarks(const FuncDecl *FD, Func *Fn);
  /// The form of the operator method F at its declaration (EXP-16, EXP-17
  /// second meaning, EXP-25, EXP-26, EXP-30).
  void checkOperatorShape(Func *F, const FuncDecl *FD, NamedType *RecvNamed);
  std::string opSecondMeaning(Func *F, StringRef Sp, Type *RecvT);
  /// Overload sets of the package: duplicates (EXP-17), a second @Convert
  /// to one target (EXP-25).
  void checkOperatorSets();
  /// The signature of the operator method F as the declaration of its
  /// generic receiver type writes it (the type parameters of the receiver are
  /// those of the type), so two methods can be compared.
  SignatureType *opNormSig(Func *F);
  /// The operator methods of N and its `extends` ancestors (EXP-30), from
  /// the origin's declarations (an instance has the instantiated methods).
  /// Stored on N once the types are final, the first stored table staying,
  /// so no answer depends on which thread asks first.
  const OpTable *opTable(NamedType *N);
  /// The operator type of a value of type T: a defined type or a type
  /// parameter; X for a View[X] of one (OWN-V1: the value is read out, or
  /// the use is an error); with ThroughPtr also N for a *N, for the forms
  /// that look through a pointer like the method call `p.M()` does (index,
  /// call, `<-`, `range`). Null for every other type.
  Type *opBase(Type *T, bool ThroughPtr = false);
  bool opCandidateType(Type *T, bool ThroughPtr = false) {
    return opBase(T, ThroughPtr) != nullptr;
  }
  /// The methods of kind K that a value of the operator type T (opBase) has:
  /// its own and its ancestors' (a nearer method with the same parameter
  /// types hides a farther one, an @Override), or the operator elements of
  /// the constraint of a type parameter (GEN-13).
  void opCandidates(Type *T, OpKind K, llvm::SmallVectorImpl<Func *> &Out);
  /// The visible methods of kind K of the table T and the tables behind it,
  /// memoized in the tables (OpTable::Merged).
  llvm::ArrayRef<Func *> opMerged(const OpTable *T, OpKind K);
  bool hasOpKind(Type *T, OpKind K);
  bool hasNoBuiltinOps(Type *T);
  struct OpMatch {
    Func *M = nullptr;
    bool Ambiguous = false;
    SmallVector<Func *, 2> Tied;
  };
  /// The overload among Cands that the arguments Args pick (EXP-17): the
  /// parameter types must be identical; an untyped constant picks the
  /// method whose parameter type is its default type, else the only method
  /// that can represent it. Lazy: a candidate whose parameter is `func() U`
  /// matches an argument of type U (EXP-21).
  OpMatch pickOverload(ArrayRef<Func *> Cands, ArrayRef<Operand *> Args,
                       bool Lazy = false, int MaxPhase = 2);
  std::string opArgString(const Operand &X);
  std::string opSetString(ArrayRef<Func *> Cands);
  /// The partial-place rule for the receiver of an operator method (EXP-30,
  /// ADR-0023 decision 16): a value receiver on a place that may hold only
  /// the parent part of a child is a taken receiver, decided by receiverWhy.
  /// False if reported.
  bool opReceiverOk(const ast::Expr *RecvE, Type *RcvTy, Func *F);
  /// Checks the call RecvE.M(ArgEs...) as the method call it stands for.
  /// RecvOp and ArgOps are the operands, evaluated already. Out is the
  /// result (Mode::NoValue for a method without result). U is the record of
  /// the use (Op and the flags); opCall adds the method and the call and
  /// stores it under At. Returns false after an error.
  bool opCall(Operand &Out, const ast::Node *At, SourceRange Range, Func *M,
              const ast::Expr *RecvE, Operand &RecvOp,
              ArrayRef<const ast::Expr *> ArgEs, ArrayRef<Operand *> ArgOps,
              OperatorUse U, const ast::CallExpr *Replace = nullptr);
  /// Mem-allocated nodes standing for a method call.
  const ast::CallExpr *synthCall(Func *M, const ast::Expr *RecvE,
                                 ArrayRef<const ast::Expr *> ArgEs,
                                 SourceRange R);
  const ast::Expr *synthExpr(SourceRange R);
  void recordOperator(const ast::Node *At, const OperatorUse &U);
  /// The use the last opCall recorded (a compound statement keeps the
  /// middle step of its expression).
  OperatorUse LastOp;
  /// Binary expression `X op Y` with a user type on a side; true if it was
  /// handled (X is the result or invalid), false to use the built-in rules.
  bool opBinary(Operand &X, Operand &Y, const ast::Expr *Ex, const ast::Expr *L,
                const ast::Expr *R, tok::Kind Op, SourceLoc OpLoc);
  /// The rule table of `x == y` when no `==` method serves it (EXP-17 rule
  /// 6, EXP-20, EXP-30, GEN-13): true for the built-in comparison, false for
  /// "no operator".
  bool eqKeepsBuiltin(const Operand &X, const Operand &Y);
  bool opUnary(Operand &X, const UnaryExpr *Ex);
  /// The built-in `<=>` of numbers and strings (EXP-20).
  void builtinSpaceship(Operand &X, Operand &Y, const ast::Expr *Ex);
  bool opIndex(Operand &X, const IndexExpr *Ex);
  bool opDeref(Operand &X, const StarExpr *Ex);
  bool opRecv(Operand &X, const UnaryExpr *Ex);
  /// `a(x)` where a has `()` methods; X is the evaluated callee.
  bool opCallExpr(Operand &X, const CallExpr *Call, bool &Handled);
  /// pickOverload for `a(xs...)`: variadic methods only, xs as the slice.
  bool OpSpread = false;
  void opCallOne(Operand &X, const CallExpr *Call);
  int opCallPrep(Operand &X, const CallExpr *Call);
  void opCallFinish(Operand &X, const CallExpr *Call,
                    SmallVector<Operand, 4> &Args);
  /// The @Convert method of N (or of an ancestor) whose result is T, or null.
  Func *convertMethod(NamedType *N, Type *T);
  /// `R(a)` through an @Convert method (EXP-25).
  bool opConvert(Operand &X, Type *T, const ast::Expr *At, bool &Handled);
  /// Statements: `x op= y`, `x++`, `a[i] = v`, `q <- v`.
  bool opCompound(const ast::AssignStmt *AS);
  /// Is A the same place as B, a part of it, or a prefix of it?
  bool overlapsPlace(const ast::Expr *A, const ast::Expr *B);
  /// The communication of the `select` case being checked, the send
  /// statement or the receive expression itself (CON-22); null elsewhere.
  /// Its operands and function literals are no select operation.
  const ast::Node *SelectComm = nullptr;
  bool opIncDec(const ast::IncDecStmt *ID);
  bool opIndexAssign(const ast::Expr *Lhs, const ast::Expr *RhsExpr,
                     Operand *X);
  bool opSend(const ast::SendStmt *SS);
  /// `for x := range a` through a `range` method: the function type that
  /// the method returns, or null if the type has none.
  Type *opRangeType(Operand &X, const ast::RangeStmt *RS, Func **Method);
  /// The built-in operator K of the type T has the signature Sig, the one a
  /// constraint element asks for: `+ (T) T` for numbers and strings,
  /// `== (T) bool` for comparable types, `<=> (T) int` for numbers and
  /// strings, `[] (int) E` for slices, ... (GEN-13).
  bool builtinOpSignature(OpKind K, Type *T, SignatureType *Sig,
                          const std::function<bool(Type *, Type *)> &Equiv);
  /// Does V meet the operator element M of a constraint (same operator,
  /// a signature Equiv to M's; a value type needs a value receiver)? Built-in
  /// types meet it with their built-in operators, a type parameter with the
  /// elements of its own constraint or the terms of its type set (GEN-13).
  bool satisfiesOpElem(Type *V, Func *M,
                       const std::function<bool(Type *, Type *)> &Equiv,
                       std::string *Cause);
  /// The operator method that stands for the constraint element of operator K
  /// with the signature ElemSig (the element's, written over the instance's
  /// types) on the concrete type V: the method of V's overload set whose
  /// signature is identical (the rule satisfiesOpElem applies to meet the
  /// element), or null when V meets the element with its built-in operator
  /// (the built-in is then the meaning). The one place that decides what an
  /// operator element means at an instance (GEN-13, EXP-17).
  Func *opAtInstance(Type *V, OpKind K, Type *ElemSig);
};

/// Scope position helper: variables declared in a statement become visible at
/// the end of that statement.
inline SourceLoc scopePosAfter(const ast::Node *N) { return N->getEnd(); }

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_CHECKER_H
