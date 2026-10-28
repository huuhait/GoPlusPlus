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

#include "gopp/Sema/Foreign.h"
#include "gopp/Sema/Sema.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"

#include <deque>
#include <functional>
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
  const FuncDecl *FDecl = nullptr;
  unsigned IotaVal = 0;
  bool Pinned = false; // `@Pinned` type declaration (OWN-R10)
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
  llvm::DenseMap<StringRef, Package *> ImportedByPath;
  /// (file scope, name) of a dot-imported object -> its import.
  llvm::DenseMap<std::pair<const Scope *, StringRef>, PkgName *> DotImportMap;
  PackageState(TypeContext &Ctx, const SourceManager &SM, Importer &Imp,
               const CheckOptions &Opts)
      : Ctx(Ctx), SM(SM), Imp(Imp), Opts(Opts) {}
};

/// Type category of D1.3.
enum class Category : uint8_t { Trivial, Owning, SharedHandle };

class Checker;

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
  std::string str(const Type *T) const { return typeString(T, S.Pkg); }
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
  bool Bodies = false; // body checker (phase 2)

  /// Per function environment (types2's `environment`).
  struct Env {
    Func *Decl = nullptr;
    bool GenericOk = false;  // a generic type or alias may be named here
    DeclInfo *Cur = nullptr; // declaration whose dependencies are recorded
    Scope *Sc = nullptr;
    SourceLoc Pos;
    const ConstValue *Iota = nullptr;
    SignatureType *Sig = nullptr;
    ArrayRef<Var *> Results;
    bool HasLabel = false;
    bool HasCallOrRecv = false;
    bool InTParamList = false;
    const void *FuncId = nullptr;
    /// The implicit `error` result of a `!` function (a `return` can fail
    /// with it, ERR-3).
    Var *FailsErr = nullptr;
  } E;

  SmallVector<std::function<void()>, 16> Delayed;
  void later(std::function<void()> F) { Delayed.push_back(std::move(F)); }
  llvm::DenseMap<const ast::Expr *, ExprInfo> Untyped;
  SmallVector<Object *, 8> ObjPath;
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
  unsigned long long sizeofType(Type *T);

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
  Func *lookupMethodInSet(TypeSet *TS, Package *P, StringRef N, bool Fold,
                          int *Idx = nullptr);
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
    bool Indirect = false;
    bool Ambiguous = false; // index set, no object
  };
  LookupResult lookupFieldOrMethod(Type *T, bool Addressable, Package *Pkg,
                                   StringRef Name, bool FoldCase = false);
  LookupResult lookupImpl(Type *T, bool Addressable, Package *Pkg,
                          StringRef Name, bool FoldCase);
  Func *lookupMethod(NamedType *N, Package *Pkg, StringRef Name, bool Fold,
                     int *Idx);
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
  /// A value stored in a composite literal (OWN-R3a, OWN-V6).
  void storeLiteralElem(Operand &X, Type *T);
  int64_t indexedElts(ArrayRef<ast::Expr *> Elts, Type *Elem, int64_t Length);

  // --- calls (Call.cpp)
  ExprKind callExpr(Operand &X, const CallExpr *E);
  void selector(Operand &X, const SelectorExpr *E, bool WantType,
                NamedType *Def = nullptr);
  /// The package an expression like `pkg` or `pkg.ns` denotes (marking the
  /// import used), or null.
  Package *qualifierPackage(const ast::Expr *X);
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
  std::string lookupError(Type *T, StringRef Sel, Object *Alt);
  Func *syntheticMethod(StringRef Name, Type *Result, SourceLoc Pos);
  void renameTParams(ArrayRef<TypeParamType *> TPs, TupleType *Params,
                     SmallVector<TypeParamType *, 4> &NewTPs,
                     TupleType **NewParams);

  // --- type expressions (Typexpr.cpp)
  Type *typ(const ast::Expr *E);
  Type *varType(const ast::Expr *E);
  Type *definedType(const ast::Expr *E, NamedType *Def);
  Type *genericType(const ast::Expr *E, std::string *Cause,
                    TypeName **Alias = nullptr);
  Type *typInternal(const ast::Expr *E, NamedType *Def);
  void ident(Operand &X, const ast::Ident *Id, NamedType *Def, bool WantType);
  int64_t arrayLength(const ast::Expr *E);
  Type *structTypeExpr(const ast::StructType *E);
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
  bool typeList(ArrayRef<ast::Expr *> Es, SmallVector<Type *, 4> &Out);
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
                bool Pinned = false);
  void funcDecl(Func *O, DeclInfo *D);
  void collectMethods(TypeName *O);
  void declStmt(const DeclStmt *S);
  void importPackage(const ImportSpec *Spec, FileState &F);
  bool validCycle(Object *Start, bool &Nval, bool &Ndef);
  void cycleError(ArrayRef<Object *> Cycle, int Start = -1);
  void validType(NamedType *T);
  bool validTypeRec(Type *T, SmallVector<NamedType *, 8> &Nest,
                    SmallVector<NamedType *, 8> &Path);
  void checkInitOrder();
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
                  SmallVector<SourceLoc, 8> &SeenPos);

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
  int verify(SourceLoc Pos, ArrayRef<TypeParamType *> TParams,
             ArrayRef<Type *> TArgs, std::string *Cause);
  bool satisfies(SourceLoc Pos, Type *TArg, TypeParamType *TPar,
                 ArrayRef<TypeParamType *> TParams, ArrayRef<Type *> TArgs,
                 std::string *Cause);
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
  bool isCopyType(Type *T);
  bool satisfiesClone(Type *T, std::string *Why);
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
  /// Lhs is written; Repoint: a borrowed string / []T is re-pointed (PAR-2).
  void checkWrite(const ast::Expr *Lhs, bool Repoint = false);
  void checkAddressOf(const UnaryExpr *E, Operand &X);
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
  /// The ranges being checked (RNG-3): the range expression and the key
  /// variable, and whether the body wrote the key already.
  struct RangeFrame {
    std::string X;
    Type *XTy = nullptr;
    Var *Key = nullptr;
    bool KeyWritten = false;
  };
  SmallVector<RangeFrame, 4> RangeStack;
  /// RNG-3: What is "assign to", "append to", ...; Lhs the place written.
  void checkRangeMutation(const ast::Expr *Lhs, const char *What);

  // --- scopes
  Object *lookup(StringRef Name) { return E.Sc->lookup(Name, E.Pos); }

  // --- misc state
  bool SwitchCaseCmp = false;
  /// A borrowed string / []T parameter takes a ref at zero cost (PAR-8), and
  /// so does a map key used for a lookup (OWN-V4).
  bool RefToOwnedOk = false;
  bool NoOwnCheck = false;
  /// Checking the init statement of an if / for / switch header.
  bool InHeaderInit = false;
  void headerInit(const ast::Stmt *Init);
  /// The selector being evaluated as the callee of a call.
  const ast::Expr *CalleeSel = nullptr;
  /// Call -> the place its result is assigned to (OWN-R4 implicit moves).
  llvm::DenseMap<const ast::Expr *, const ast::Expr *> AppendTargets;
  llvm::DenseSet<const NamedType *> ValidTypes, ReportedCycle;
};

/// Scope position helper: variables declared in a statement become visible at
/// the end of that statement.
inline SourceLoc scopePosAfter(const ast::Node *N) { return N->getEnd(); }

} // namespace sema
} // namespace gopp

#endif // GOPP_SEMA_CHECKER_H
