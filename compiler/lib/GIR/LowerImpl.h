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

class Lowering {
public:
  Lowering(Context &Ctx, const SourceManager &SM, const S::CheckedPackage &CP,
           ArrayRef<const ast::File *> Files, DiagnosticsEngine &Diags,
           const LowerOptions &Opts)
      : Ctx(Ctx), SM(SM), CP(CP), I(CP.I), Files(Files), Diags(Diags),
        Opts(Opts),
        M(std::make_unique<Module>(Ctx, CP.Pkg ? CP.Pkg->Path.str() : "")) {}

  std::unique_ptr<Module> run();

  Context &Ctx;
  const SourceManager &SM;
  const S::CheckedPackage &CP;
  const S::Info &I;
  ArrayRef<const ast::File *> Files;
  DiagnosticsEngine &Diags;
  const LowerOptions &Opts;
  std::unique_ptr<Module> M;
  Symbols Syms;
  bool Failed = false;
  /// Errors reported so far (unsupported, badExtern, ...): a step that
  /// returns null must have raised it (lowerType checks).
  unsigned Reported = 0;
  /// Named types whose lowering is under way (a cycle through no struct).
  /// With the number of structs under way when it began.
  llvm::DenseMap<S::Type *, unsigned> InProgress;
  unsigned StructsActive = 0;
  llvm::SmallPtrSet<S::Type *, 8> RecursiveReported;
  llvm::DenseMap<const S::Func *, FuncId> FuncIds;
  /// Package variables with storage, by checker object.
  llvm::DenseMap<const S::Var *, uint32_t> Globals;
  /// Functions whose declaration was reported: their calls are not reported
  /// again.
  llvm::DenseSet<const S::Func *> Declined;
  /// Which types C can take by value, memoized (P04.11).
  CTypeRules CRules;

  void unsupported(SourceLoc At, const llvm::Twine &What);
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
  FuncId Panics[5][2] = {
      {NoId, NoId}, {NoId, NoId}, {NoId, NoId}, {NoId, NoId}, {NoId, NoId}};
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
  Type *lowerStruct(S::Type *Whole, S::StructType *ST, S::NamedType *Named,
                    SourceLoc At);
};

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
  struct Scope {
    SmallVector<LocalId, 4> Locals;
  };
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
  void at(const ast::Node *N) { FB.setSpan(L.spanOf(N->getBegin())); }
  void unsupported(const ast::Node *N, const llvm::Twine &What) {
    L.unsupported(N->getBegin(), What);
  }
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
  Operand lowerBinary(const ast::BinaryExpr *B);
  Operand lowerLogical(const ast::BinaryExpr *B);
  Operand lowerUnary(const ast::UnaryExpr *U);
  Operand lowerCall(const ast::CallExpr *C);
  Operand lowerConversion(const ast::CallExpr *C);
  Operand lowerBuiltin(const ast::CallExpr *C, const S::Builtin *B);
  Operand lowerOptionMethod(const ast::CallExpr *C,
                            const ast::SelectorExpr *Sel, const S::Func *F);
  Operand lowerCompositeLit(const ast::CompositeLit *CL);
  bool ownedOk(const ast::Expr *E, Type *Dest);
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
  };
  using Pending = llvm::SmallVector<PendingCheck, 1>;
  void emitIndexChecks(const Pending &Checks);
  /// Pend: where the index checks of a destination go (null: made now).
  Place lowerPlace(const ast::Expr *E, Pending *Pend = nullptr);
  Place identPlace(const ast::Ident *Id);
  Place selectorPlace(const ast::SelectorExpr *Sel, Pending *Pend = nullptr);
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
                     const ast::Node *N);

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
  void lowerBranch(const ast::BranchStmt *B);
  void lowerLabeled(const ast::LabeledStmt *LS);
  void lowerReturn(const ast::ReturnStmt *R);
  void lowerIncDec(const ast::IncDecStmt *S);
  void lowerTypeDecl(const ast::GenDecl *GD);
  /// The values of a call used where several are wanted (`a, b := f()`).
  bool multiValues(const ast::Expr *E, size_t Want,
                   SmallVector<Operand, 2> &Out);
  BlockId labelBlock(StringRef Name);

  LocalId declareVar(const ast::Ident *Id, Type *Ty);
  /// A destination of an assignment: a place, or blank.
  struct Dest {
    Place P;
    Pending Checks; // index checks to make before the store
    bool Blank = false;
    S::Type *STy = nullptr;
  };
  Dest lowerDest(const ast::Expr *E);
  void store(const Dest &D, Operand V, const ast::Node *N);
  void lowerSwitchTests(const ast::SwitchStmt *Sw, ArrayRef<BlockId> Bodies,
                        BlockId Default, BlockId Exit);
  void pushScope() { Scopes.emplace_back(); }
  void popScope() {
    exitScopes(Scopes.size() - 1);
    Scopes.pop_back();
  }
  void exitScopes(size_t Depth) {
    if (FB.terminated())
      return;
    for (size_t S = Scopes.size(); S > Depth; --S)
      for (size_t K = Scopes[S - 1].Locals.size(); K > 0; --K)
        FB.storageDead(Scopes[S - 1].Locals[K - 1]);
  }
  void startDeadBlock() { FB.setBlock(FB.newBlock()); }
  /// Closes every block that has no terminator and drops the unreachable
  /// ones.
  void finish();
};

} // namespace impl
} // namespace gir
} // namespace gopp

#endif // GOPP_LIB_GIR_LOWERIMPL_H
