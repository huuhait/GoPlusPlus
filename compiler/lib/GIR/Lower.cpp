//===- Lower.cpp - The checked AST to GIR ---------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Decision points, one per rule:
//   - a checker type to a GIR type:        Lowering::lowerType
//   - the symbol of a function:            gir::symbolOf
//   - what @Extern("C") accepts:            Lowering::declareFunction
//   - what println / print do:              Body::lowerPrintln (and the
//     helpers it calls, written here in GIR on top of the C `write`)
//   - a construct outside the M0 subset:    Lowering::unsupported (E0439)
//
// The builder recurses once per level of the checked AST; the parser bounds
// that (ParseOptions::MaxHeight), and lowerPackage runs the builder on a
// thread with a stack sized for the deepest legal tree (runOnBigStack).
//
//===----------------------------------------------------------------------===//

#include "gopp/GIR/Lower.h"

#include "gopp/AST/AST.h"
#include "gopp/GIR/Builder.h"
#include "gopp/GIR/Verify.h"
#include "gopp/Sema/Constant.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/raw_ostream.h"

#include <pthread.h>

using namespace gopp;
using namespace gopp::gir;
using llvm::ArrayRef;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::SmallVector;
using llvm::StringRef;

namespace {

namespace S = gopp::sema;

class Lowering {
public:
  Lowering(Context &Ctx, const SourceManager &SM, const S::CheckedPackage &CP,
           ArrayRef<const ast::File *> Files, DiagnosticsEngine &Diags,
           const LowerOptions &Opts)
      : Ctx(Ctx), SM(SM), CP(CP), I(CP.I), Files(Files), Diags(Diags),
        Opts(Opts),
        M(std::make_unique<Module>(Ctx, CP.Pkg ? CP.Pkg->Path.str() : "")) {}

  std::unique_ptr<Module> run();

  // Shared with the body lowering.
  Context &Ctx;
  const SourceManager &SM;
  const S::CheckedPackage &CP;
  const S::Info &I;
  ArrayRef<const ast::File *> Files;
  DiagnosticsEngine &Diags;
  const LowerOptions &Opts;
  std::unique_ptr<Module> M;
  bool Failed = false;
  llvm::DenseMap<const S::Func *, FuncId> FuncIds;
  FuncId WriteFn = NoId, PrintU = NoId, PrintI = NoId;
  bool WriteBad = false; // the program's `write` is not the one println needs
  /// Functions whose declaration was reported: their calls are not reported
  /// again.
  llvm::DenseSet<const S::Func *> Declined;

  void unsupported(SourceLoc At, const llvm::Twine &What) {
    Failed = true;
    Diags.report(At, diag::err_gir_unsupported) << What.str();
  }

  void badExtern(SourceLoc At, StringRef Name, const llvm::Twine &Why) {
    Failed = true;
    Diags.report(At, diag::err_gir_extern_bad) << Name.str() << Why.str();
  }

  Span spanOf(SourceLoc L) {
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

  /// The GIR type of a checked type; null after a report.
  Type *lowerType(S::Type *T, SourceLoc At);
  Type *lowerTypeOnce(S::Type *T, SourceLoc At);
  /// The C `write` that println calls (the program's own declaration if it
  /// has one). At is the println that needs it.
  FuncId ensureWrite(SourceLoc At);
  FuncId ensurePrintU64(SourceLoc At);
  FuncId ensurePrintI64(SourceLoc At);
  void declareFunction(const ast::FuncDecl *FD);
  void lowerBody(const ast::FuncDecl *FD);
  void addEntry(const ast::FuncDecl *Main);

private:
  llvm::DenseMap<S::Type *, Type *> TypeCache;
};

//===----------------------------------------------------------------------===//
// Types
//===----------------------------------------------------------------------===//

const char *kindName(S::Type::Kind K) {
  switch (K) {
  case S::Type::Kind::Basic:
    return "this basic type";
  case S::Type::Kind::Pointer:
    return "pointer";
  case S::Type::Kind::Slice:
    return "slice";
  case S::Type::Kind::Array:
    return "array";
  case S::Type::Kind::Map:
    return "map";
  case S::Type::Kind::Chan:
    return "channel";
  case S::Type::Kind::Ref:
    return "view";
  case S::Type::Kind::Tuple:
    return "tuple";
  case S::Type::Kind::Signature:
    return "function value";
  case S::Type::Kind::Struct:
    return "struct";
  case S::Type::Kind::Interface:
    return "interface";
  case S::Type::Kind::Union:
    return "union";
  case S::Type::Kind::Named:
    return "named type";
  case S::Type::Kind::TypeParam:
    return "type parameter";
  case S::Type::Kind::Option:
    return "Option";
  }
  return "type";
}

Type *Lowering::lowerType(S::Type *T, SourceLoc At) {
  if (!T) {
    unsupported(At, "an expression without a type");
    return nullptr;
  }
  // A type that cannot be lowered is reported once, at its first use in
  // source order; later uses get null without a second report.
  auto [It, New] = TypeCache.try_emplace(T, nullptr);
  if (!New)
    return It->second;
  Type *R = lowerTypeOnce(T, At);
  TypeCache[T] = R;
  return R;
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
    case S::BasicKind::UntypedInt:
      R = Ctx.intTy(PB, true);
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
    if (N->Orig || !N->TParams.empty() || !N->Underlying ||
        !isa<S::BasicType>(N->Underlying)) {
      unsupported(At, "the defined type " + N->Obj->Name.str());
      return nullptr;
    }
    R = lowerType(N->Underlying, At);
    break;
  }
  default:
    unsupported(At, std::string("a ") + kindName(T->getKind()));
    return nullptr;
  }
  return R;
}

//===----------------------------------------------------------------------===//
// Bodies
//===----------------------------------------------------------------------===//

class Body {
public:
  Body(Lowering &L, Function &F, const S::Func *Obj)
      : L(L), M(*L.M), FB(*L.M, F), I(L.I), Obj(Obj) {}

  void lower(const ast::FuncDecl *FD);

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
  struct Loop {
    BlockId Break, Continue;
    size_t Depth;
  };
  SmallVector<Loop, 4> Loops;

  Context &ctx() { return L.Ctx; }
  void at(const ast::Node *N) { FB.setSpan(L.spanOf(N->getBegin())); }
  void unsupported(const ast::Node *N, const llvm::Twine &What) {
    L.unsupported(N->getBegin(), What);
  }

  static const ast::Expr *unparen(const ast::Expr *E) {
    while (auto *P = dyn_cast<ast::ParenExpr>(E))
      E = P->X;
    return E;
  }

  Type *tyOf(const ast::Expr *E) {
    return L.lowerType(I.typeOf(E), E->getBegin());
  }
  bool isConst(const ast::Expr *E) {
    const S::TypeAndValue *TV = I.lookup(E);
    return TV && TV->Val && TV->M == S::Mode::Constant;
  }

  Operand constOperand(const S::ConstValue &V, Type *Ty, const ast::Node *N);
  Operand lowerExpr(const ast::Expr *E);
  Operand lowerAs(const ast::Expr *E, Type *Want);
  Operand lowerBinary(const ast::BinaryExpr *B);
  Operand lowerLogical(const ast::BinaryExpr *B);
  Operand lowerUnary(const ast::UnaryExpr *U);
  Operand lowerCall(const ast::CallExpr *C);
  Operand lowerConversion(const ast::CallExpr *C);
  void lowerPrintln(const ast::CallExpr *C, bool Newline);
  bool constText(const ast::Expr *Arg, Type *T, std::string &Out);
  void printValue(Operand V, SourceLoc At);
  void writeString(StringRef S, SourceLoc At);
  void callWrite(Operand Ptr, Operand Len, SourceLoc At);

  void lowerStmt(const ast::Stmt *St);
  void lowerBlock(const ast::BlockStmt *B);
  void lowerAssign(const ast::AssignStmt *A);
  void lowerValueSpec(const ast::ValueSpec *VS);
  void lowerIf(const ast::IfStmt *If);
  void lowerFor(const ast::ForStmt *For);
  void lowerBranch(const ast::BranchStmt *B);
  void lowerReturn(const ast::ReturnStmt *R);
  void lowerIncDec(const ast::IncDecStmt *S);

  LocalId declareVar(const ast::Ident *Id, Type *Ty);
  bool lhsLocal(const ast::Expr *E, LocalId &Out, bool &Blank);
  void pushScope() { Scopes.emplace_back(); }
  void popScope() {
    exitScopes(Scopes.size() - 1);
    Scopes.pop_back();
  }
  /// StorageDead for every local of the scopes above Depth (the scopes stay).
  void exitScopes(size_t Depth) {
    if (FB.terminated())
      return;
    for (size_t S = Scopes.size(); S > Depth; --S)
      for (size_t K = Scopes[S - 1].Locals.size(); K > 0; --K)
        FB.storageDead(Scopes[S - 1].Locals[K - 1]);
  }
  /// After a terminator the code that follows (if any) goes to a block no
  /// edge reaches; removeDeadBlocks drops it.
  void startDeadBlock() { FB.setBlock(FB.newBlock()); }
};

Operand Body::constOperand(const S::ConstValue &V, Type *Ty,
                           const ast::Node *N) {
  if (!Ty)
    return constBool(ctx(), false);
  switch (Ty->getKind()) {
  case Type::Kind::Bool:
    return constBool(ctx(), V.boolVal());
  case Type::Kind::String:
    return constStr(M, V.stringVal());
  case Type::Kind::Int: {
    S::ConstValue IV = V.toInt();
    if (IV.isUnknown()) {
      unsupported(N, "this constant");
      return constInt(Ty, 0);
    }
    return constInt(Ty, IV.intVal().sextOrTrunc(64).getZExtValue());
  }
  case Type::Kind::Float: {
    S::ConstValue FV = V.toFloat();
    return constFloat(Ty, FV.float64());
  }
  default:
    unsupported(N, "a constant of type " + Ty->str());
    return constInt(ctx().i64(), 0);
  }
}

Operand Body::lowerAs(const ast::Expr *E, Type *Want) {
  if (isConst(E))
    return constOperand(*I.lookup(E)->Val, Want, E);
  return lowerExpr(E);
}

Operand Body::lowerExpr(const ast::Expr *E) {
  if (isConst(E))
    return constOperand(*I.lookup(E)->Val, tyOf(E), E);
  if (I.UnitConvs.count(E)) {
    unsupported(E, "a unit conversion");
    return constInt(ctx().i64(), 0);
  }
  switch (E->getKind()) {
  case ast::Node::Kind::ParenExpr:
    return lowerExpr(cast<ast::ParenExpr>(E)->X);
  case ast::Node::Kind::Ident: {
    auto *Id = cast<ast::Ident>(E);
    auto U = I.Uses.find(Id);
    if (U != I.Uses.end())
      if (auto *V = dyn_cast<S::Var>(U->second)) {
        auto It = Vars.find(V);
        if (It != Vars.end())
          return copyOf(FB.local(It->second));
      }
    unsupported(E, "the name " + Id->Name.str());
    break;
  }
  case ast::Node::Kind::BinaryExpr:
    return lowerBinary(cast<ast::BinaryExpr>(E));
  case ast::Node::Kind::UnaryExpr:
    return lowerUnary(cast<ast::UnaryExpr>(E));
  case ast::Node::Kind::CallExpr:
    return lowerCall(cast<ast::CallExpr>(E));
  default:
    unsupported(E, std::string("the expression ") + E->getKindName());
    break;
  }
  return constInt(ctx().i64(), 0);
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

Operand Body::lowerBinary(const ast::BinaryExpr *B) {
  if (I.Operators.count(B)) {
    unsupported(B, "an operator method");
    return constInt(ctx().i64(), 0);
  }
  if (B->Op == tok::land || B->Op == tok::lor)
    return lowerLogical(B);
  BinOp Op;
  if (!binOpFor(B->Op, Op)) {
    unsupported(B, "this operator");
    return constInt(ctx().i64(), 0);
  }
  Type *ResTy = tyOf(B);
  if (!ResTy)
    return constInt(ctx().i64(), 0);
  Operand X, Y;
  if (isShift(Op)) {
    X = lowerAs(B->X, ResTy);
    Y = lowerExpr(B->Y);
  } else if (isComparison(Op)) {
    // The operands have the type of the one that is not a constant.
    Type *OpTy = isConst(B->X) ? tyOf(B->Y) : tyOf(B->X);
    if (isConst(B->X) && isConst(B->Y))
      OpTy = tyOf(B->X);
    X = lowerAs(B->X, OpTy);
    Y = lowerAs(B->Y, OpTy);
  } else {
    X = lowerAs(B->X, ResTy);
    Y = lowerAs(B->Y, ResTy);
  }
  Type *XT = X.type();
  if (!XT->isInt() && !XT->isFloat() && !(XT->isBool() && isComparison(Op))) {
    unsupported(B, std::string("an operator on ") + XT->str());
    return constInt(ctx().i64(), 0);
  }
  at(B);
  // Go's checks (D4.12): a zero divisor and a negative shift count panic.
  if ((Op == BinOp::Div || Op == BinOp::Rem) && XT->isInt()) {
    if (!isConst(B->Y)) {
      Place NZ = FB.temp(FB.binary(BinOp::Ne, Y, constInt(XT, 0)));
      FB.assertThat(copyOf(NZ), true, AssertKind::DivByZero);
    }
  }
  if (isShift(Op) && Y.type()->isSigned() && !isConst(B->Y)) {
    Place NN = FB.temp(FB.binary(BinOp::Ge, Y, constInt(Y.type(), 0)));
    FB.assertThat(copyOf(NN), true, AssertKind::NegativeShift);
  }
  Place R = FB.temp(FB.binary(Op, X, Y));
  return copyOf(R);
}

Operand Body::lowerUnary(const ast::UnaryExpr *U) {
  if (I.Operators.count(U)) {
    unsupported(U, "an operator method");
    return constInt(ctx().i64(), 0);
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
    return constInt(ctx().i64(), 0);
  }
  Operand X = lowerAs(U->X, tyOf(U));
  Type *T = X.type();
  if (!(T->isInt() || (T->isFloat() && Op == UnOp::Neg) ||
        (T->isBool() && Op == UnOp::Not))) {
    unsupported(U, std::string("a unary operator on ") + T->str());
    return constInt(ctx().i64(), 0);
  }
  at(U);
  return copyOf(FB.temp(FB.unary(Op, X)));
}

Operand Body::lowerConversion(const ast::CallExpr *C) {
  if (C->Args.size() != 1) {
    unsupported(C, "this conversion");
    return constInt(ctx().i64(), 0);
  }
  Type *To = tyOf(C);
  Operand X = lowerExpr(C->Args[0]);
  if (!To)
    return X;
  Type *From = X.type();
  if (From == To)
    return X;
  CastKind K;
  if (From->isInt() && To->isInt())
    K = CastKind::IntToInt;
  else if (From->isInt() && To->isFloat())
    K = CastKind::IntToFloat;
  else if (From->isFloat() && To->isInt())
    K = CastKind::FloatToInt;
  else if (From->isFloat() && To->isFloat())
    K = CastKind::FloatToFloat;
  else {
    unsupported(C, "a conversion from " + From->str() + " to " + To->str());
    return X;
  }
  at(C);
  return copyOf(FB.temp(FB.cast(K, X, To)));
}

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
    Out = IV.exactStr();
    return true;
  }
  unsupported(Arg, "printing this constant");
  return false;
}

/// Prints one value that is not a constant (M0: bool, integers, strings).
void Body::printValue(Operand V, SourceLoc At) {
  Type *T = V.type();
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
    FuncId H = Signed ? L.ensurePrintI64(At) : L.ensurePrintU64(At);
    if (H == NoId)
      return;
    FB.call(constFunc(M, H), {V}, FB.local(FB.newTemp(ctx().unit())),
            {UnwindAction::Continue, NoId});
    return;
  }
  default: // lowerPrintln reported it
    return;
  }
}

/// println and print (D4.11 e: the bootstrap writes to standard output; Go's
/// writes to standard error). Go evaluates every operand before the call, so
/// the first phase evaluates, left to right, the operands that are not
/// constants, each into its own temporary; the second phase writes. A
/// constant is formatted here, and the text from one value to the next
/// (println's spaces and newline included) is one `write`.
void Body::lowerPrintln(const ast::CallExpr *C, bool Newline) {
  SourceLoc At = C->getBegin();
  struct Item {
    bool IsValue = false;
    std::string Text; // a constant
    Operand Val;      // a value
  };
  SmallVector<Item, 4> Items;
  for (const ast::Expr *Arg : C->Args) {
    Type *T = tyOf(Arg);
    if (!T)
      return;
    if (!T->isInt() && !T->isBool() && T->getKind() != Type::Kind::String) {
      unsupported(Arg, "printing a value of type " + T->str());
      return;
    }
    Item It;
    if (isConst(Arg)) {
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
    printValue(V, At);
  }
  if (Newline)
    Text += '\n';
  if (!Text.empty())
    writeString(Text, At);
}

Operand Body::lowerCall(const ast::CallExpr *C) {
  const ast::Expr *Fun = unparen(C->Fun);
  const S::TypeAndValue *FT = I.lookup(Fun);
  Operand Unit = constBool(ctx(), false);
  if (C->isMust() || C->EllipsisLoc.isValid()) {
    unsupported(C, "this call form");
    return Unit;
  }
  if (FT && FT->M == S::Mode::TypeExpr)
    return lowerConversion(C);
  auto *Id = dyn_cast<ast::Ident>(Fun);
  auto U = Id ? I.Uses.find(Id) : I.Uses.end();
  if (U != I.Uses.end()) {
    if (auto *B = dyn_cast<S::Builtin>(U->second)) {
      if (B->Id == S::BuiltinId::Println || B->Id == S::BuiltinId::Print) {
        lowerPrintln(C, B->Id == S::BuiltinId::Println);
        return Unit;
      }
      unsupported(C, "the built-in " + B->Name.str());
      return Unit;
    }
    if (auto *F = dyn_cast<S::Func>(U->second)) {
      auto FI = L.FuncIds.find(F);
      if (FI == L.FuncIds.end() && L.Declined.count(F))
        return Unit; // its declaration was reported
      if (FI == L.FuncIds.end() || F->Recv || F->IsInterfaceMethod) {
        unsupported(C, "a call of " + F->Name.str());
        return Unit;
      }
      const Function &Callee = M.func(FI->second);
      if (Callee.Sig->elems().size() != C->Args.size() || F->sig()->Variadic) {
        unsupported(C, "this call form");
        return Unit;
      }
      SmallVector<Operand, 4> Args;
      for (size_t K = 0; K < C->Args.size(); ++K)
        Args.push_back(lowerAs(C->Args[K], Callee.Sig->elems()[K]));
      at(C);
      LocalId Res = FB.newTemp(Callee.Sig->ret());
      FB.call(
          constFunc(M, FI->second), Args, FB.local(Res),
          {Callee.NoUnwind ? UnwindAction::Unreachable : UnwindAction::Continue,
           NoId});
      return copyOf(FB.local(Res));
    }
  }
  // A call of a value (a func value, a method value): its type says what is
  // missing, reported once (lowerType).
  if (!tyOf(Fun))
    return Unit;
  unsupported(C, "this call");
  return Unit;
}

//===----------------------------------------------------------------------===//
// Statements
//===----------------------------------------------------------------------===//

LocalId Body::declareVar(const ast::Ident *Id, Type *Ty) {
  auto D = I.Defs.find(Id);
  LocalId Loc = FB.newLocal(Ty, Local::Var, Id->Name.str());
  if (D != I.Defs.end())
    if (auto *V = dyn_cast_or_null<S::Var>(D->second))
      Vars[V] = Loc;
  Scopes.back().Locals.push_back(Loc);
  FB.storageLive(Loc);
  return Loc;
}

/// The local an assignment target names. Blank is set for `_`.
bool Body::lhsLocal(const ast::Expr *E, LocalId &Out, bool &Blank) {
  Blank = false;
  E = unparen(E);
  auto *Id = dyn_cast<ast::Ident>(E);
  if (!Id) {
    unsupported(E, "assignment to this kind of place");
    return false;
  }
  if (Id->Name.str() == "_") {
    Blank = true;
    return true;
  }
  auto U = I.Uses.find(Id);
  if (U != I.Uses.end())
    if (auto *V = dyn_cast<S::Var>(U->second)) {
      auto It = Vars.find(V);
      if (It != Vars.end()) {
        Out = It->second;
        return true;
      }
    }
  unsupported(E, "assignment to " + Id->Name.str());
  return false;
}

void Body::lowerAssign(const ast::AssignStmt *A) {
  at(A);
  if (A->Op == tok::define || A->Op == tok::assign) {
    if (A->Lhs.size() != A->Rhs.size()) {
      unsupported(A, "assignment from a multi-value call");
      return;
    }
    bool Define = A->Op == tok::define;
    // Evaluate every right side first (parallel assignment).
    SmallVector<Operand, 2> Vals;
    SmallVector<Type *, 2> Tys;
    for (size_t K = 0; K < A->Rhs.size(); ++K) {
      const ast::Expr *Lhs = unparen(A->Lhs[K]);
      Type *Want = nullptr;
      auto *Id = dyn_cast<ast::Ident>(Lhs);
      if (Id && Id->Name.str() == "_") {
        Want = tyOf(A->Rhs[K]);
      } else if (Define && Id && I.Defs.count(Id) && I.Defs.find(Id)->second) {
        Want = L.lowerType(I.Defs.find(Id)->second->Ty, Id->getBegin());
      } else {
        Want = tyOf(Lhs);
      }
      if (!Want)
        return;
      Operand V = lowerAs(A->Rhs[K], Want);
      // A later right side must not see an earlier left side change.
      if (A->Rhs.size() > 1 && V.K != Operand::Const) {
        at(A);
        V = copyOf(FB.temp(FB.use(V)));
      }
      Vals.push_back(std::move(V));
      Tys.push_back(Want);
    }
    for (size_t K = 0; K < A->Lhs.size(); ++K) {
      const ast::Expr *Lhs = unparen(A->Lhs[K]);
      auto *Id = dyn_cast<ast::Ident>(Lhs);
      at(A);
      if (Define && Id && Id->Name.str() != "_" && I.Defs.count(Id) &&
          I.Defs.find(Id)->second) {
        LocalId Loc = declareVar(Id, Tys[K]);
        FB.assign(FB.local(Loc), FB.use(Vals[K]));
        continue;
      }
      LocalId Loc;
      bool Blank;
      if (!lhsLocal(Lhs, Loc, Blank))
        return;
      if (!Blank)
        FB.assign(FB.local(Loc), FB.use(Vals[K]));
    }
    return;
  }
  // x op= y
  BinOp Op;
  tok::Kind Base;
  switch (A->Op) {
  case tok::add_assign:
    Base = tok::add;
    break;
  case tok::sub_assign:
    Base = tok::sub;
    break;
  case tok::mul_assign:
    Base = tok::mul;
    break;
  case tok::quo_assign:
    Base = tok::quo;
    break;
  case tok::rem_assign:
    Base = tok::rem;
    break;
  case tok::and_assign:
    Base = tok::and_;
    break;
  case tok::or_assign:
    Base = tok::or_;
    break;
  case tok::xor_assign:
    Base = tok::xor_;
    break;
  case tok::shl_assign:
    Base = tok::shl;
    break;
  case tok::shr_assign:
    Base = tok::shr;
    break;
  case tok::and_not_assign:
    Base = tok::and_not;
    break;
  default:
    unsupported(A, "this assignment operator");
    return;
  }
  binOpFor(Base, Op);
  if (A->Lhs.size() != 1 || A->Rhs.size() != 1 || I.Operators.count(A)) {
    unsupported(A, "this assignment");
    return;
  }
  LocalId Loc;
  bool Blank;
  if (!lhsLocal(A->Lhs[0], Loc, Blank) || Blank)
    return;
  Type *T = FB.function().Locals[Loc].Ty;
  Operand X = copyOf(FB.local(Loc));
  Operand Y = isShift(Op) ? lowerExpr(A->Rhs[0]) : lowerAs(A->Rhs[0], T);
  if (!T->isInt() && !T->isFloat()) {
    unsupported(A, "an operator on " + T->str());
    return;
  }
  at(A);
  if ((Op == BinOp::Div || Op == BinOp::Rem) && T->isInt() &&
      !isConst(A->Rhs[0])) {
    Place NZ = FB.temp(FB.binary(BinOp::Ne, Y, constInt(T, 0)));
    FB.assertThat(copyOf(NZ), true, AssertKind::DivByZero);
  }
  if (isShift(Op) && Y.type()->isSigned() && !isConst(A->Rhs[0])) {
    Place NN = FB.temp(FB.binary(BinOp::Ge, Y, constInt(Y.type(), 0)));
    FB.assertThat(copyOf(NN), true, AssertKind::NegativeShift);
  }
  FB.assign(FB.local(Loc), FB.binary(Op, X, Y));
}

void Body::lowerIncDec(const ast::IncDecStmt *S) {
  at(S);
  LocalId Loc;
  bool Blank;
  if (!lhsLocal(S->X, Loc, Blank) || Blank)
    return;
  Type *T = FB.function().Locals[Loc].Ty;
  if (!T->isInt() && !T->isFloat()) {
    unsupported(S, "++ on " + T->str());
    return;
  }
  Operand One = T->isInt() ? constInt(T, 1) : constFloat(T, 1.0);
  FB.assign(FB.local(Loc),
            FB.binary(S->Op == tok::inc ? BinOp::Add : BinOp::Sub,
                      copyOf(FB.local(Loc)), One));
}

void Body::lowerValueSpec(const ast::ValueSpec *VS) {
  at(VS);
  size_t N = VS->Names.size();
  if (!VS->Values.empty() && VS->Values.size() != N) {
    unsupported(VS, "a declaration from a multi-value call");
    return;
  }
  SmallVector<Operand, 2> Vals;
  SmallVector<Type *, 2> Tys;
  for (size_t K = 0; K < N; ++K) {
    auto D = I.Defs.find(VS->Names[K]);
    S::Type *ST = D != I.Defs.end() && D->second ? D->second->Ty : nullptr;
    Type *T = L.lowerType(ST, VS->Names[K]->getBegin());
    if (!T)
      return;
    Tys.push_back(T);
    if (VS->Values.empty()) {
      Operand Z;
      if (T->isInt())
        Z = constInt(T, 0);
      else if (T->isBool())
        Z = constBool(ctx(), false);
      else if (T->isFloat())
        Z = constFloat(T, 0.0);
      else {
        Z.K = Operand::Const;
        Z.C.K = Constant::Zero;
        Z.C.Ty = T;
      }
      Vals.push_back(Z);
    } else {
      Vals.push_back(lowerAs(VS->Values[K], T));
    }
  }
  for (size_t K = 0; K < N; ++K) {
    if (VS->Names[K]->Name.str() == "_")
      continue;
    at(VS);
    LocalId Loc = declareVar(VS->Names[K], Tys[K]);
    FB.assign(FB.local(Loc), FB.use(Vals[K]));
  }
}

void Body::lowerIf(const ast::IfStmt *If) {
  pushScope();
  if (If->Init)
    lowerStmt(If->Init);
  Operand Cond = lowerAs(If->Cond, ctx().boolean());
  at(If);
  BlockId Then = FB.newBlock(), Else = If->Else ? FB.newBlock() : NoId,
          Join = FB.newBlock();
  FB.branch(Cond, Then, If->Else ? Else : Join);
  FB.setBlock(Then);
  lowerBlock(If->Then);
  if (!FB.terminated())
    FB.gotoBlock(Join);
  if (If->Else) {
    FB.setBlock(Else);
    lowerStmt(If->Else);
    if (!FB.terminated())
      FB.gotoBlock(Join);
  }
  FB.setBlock(Join);
  popScope();
}

void Body::lowerFor(const ast::ForStmt *For) {
  pushScope();
  if (For->Init)
    lowerStmt(For->Init);
  BlockId Head = FB.newBlock(), BodyB = FB.newBlock(),
          Post = For->Post ? FB.newBlock() : NoId, Exit = FB.newBlock();
  FB.gotoBlock(Head);
  FB.setBlock(Head);
  if (For->Cond) {
    Operand Cond = lowerAs(For->Cond, ctx().boolean());
    at(For);
    FB.branch(Cond, BodyB, Exit);
  } else {
    FB.gotoBlock(BodyB);
  }
  FB.setBlock(BodyB);
  Loops.push_back({Exit, For->Post ? Post : Head, Scopes.size()});
  lowerBlock(For->Body);
  Loops.pop_back();
  if (!FB.terminated())
    FB.gotoBlock(For->Post ? Post : Head);
  if (For->Post) {
    FB.setBlock(Post);
    lowerStmt(For->Post);
    if (!FB.terminated())
      FB.gotoBlock(Head);
  }
  FB.setBlock(Exit);
  popScope();
}

void Body::lowerBranch(const ast::BranchStmt *B) {
  at(B);
  if (B->Label || Loops.empty() ||
      (B->Keyword != tok::kw_break && B->Keyword != tok::kw_continue)) {
    unsupported(B, "this branch statement");
    return;
  }
  const Loop &Lp = Loops.back();
  exitScopes(Lp.Depth);
  FB.gotoBlock(B->Keyword == tok::kw_break ? Lp.Break : Lp.Continue);
  startDeadBlock();
}

void Body::lowerReturn(const ast::ReturnStmt *R) {
  at(R);
  Type *Ret = FB.function().Sig->ret();
  if (R->Results.size() > 1 || (!R->Results.empty() && Ret->isUnit()) ||
      (R->Results.empty() && !Ret->isUnit())) {
    unsupported(R, "this return");
    return;
  }
  if (!R->Results.empty()) {
    Operand V = lowerAs(R->Results[0], Ret);
    at(R);
    FB.assign(FB.local(FB.returnPlace()), FB.use(V));
  }
  exitScopes(0);
  FB.ret();
  startDeadBlock();
}

void Body::lowerBlock(const ast::BlockStmt *B) {
  pushScope();
  for (const ast::Stmt *St : B->List)
    lowerStmt(St);
  popScope();
}

void Body::lowerStmt(const ast::Stmt *St) {
  using K = ast::Node::Kind;
  switch (St->getKind()) {
  case K::EmptyStmt:
    return;
  case K::BlockStmt:
    lowerBlock(cast<ast::BlockStmt>(St));
    return;
  case K::ExprStmt: {
    const ast::Expr *E = cast<ast::ExprStmt>(St)->X;
    if (isa<ast::CallExpr>(unparen(E))) {
      at(St);
      lowerCall(cast<ast::CallExpr>(unparen(E)));
      return;
    }
    unsupported(St, "this expression statement");
    return;
  }
  case K::AssignStmt:
    lowerAssign(cast<ast::AssignStmt>(St));
    return;
  case K::IncDecStmt:
    lowerIncDec(cast<ast::IncDecStmt>(St));
    return;
  case K::IfStmt:
    lowerIf(cast<ast::IfStmt>(St));
    return;
  case K::ForStmt:
    lowerFor(cast<ast::ForStmt>(St));
    return;
  case K::BranchStmt:
    lowerBranch(cast<ast::BranchStmt>(St));
    return;
  case K::ReturnStmt:
    lowerReturn(cast<ast::ReturnStmt>(St));
    return;
  case K::DeclStmt: {
    auto *GD = dyn_cast_or_null<ast::GenDecl>(cast<ast::DeclStmt>(St)->D);
    // Constants are folded by the checker; types are lowered where they are
    // used.
    if (GD && (GD->Keyword == tok::kw_type ||
               (GD->Keyword == tok::kw_const && !GD->IsUnit)))
      return;
    if (GD && GD->Keyword == tok::kw_var && !GD->IsUnit) {
      for (const ast::Spec *Sp : GD->Specs)
        lowerValueSpec(cast<ast::ValueSpec>(Sp));
      return;
    }
    unsupported(St, "this declaration");
    return;
  }
  default:
    unsupported(St, std::string("the statement ") + St->getKindName());
    return;
  }
}

void Body::lower(const ast::FuncDecl *FD) {
  Function &F = FB.function();
  at(FD);
  // The parameters are the locals 1..N.
  auto Params = Obj->Params;
  if (Params.size() != F.numParams()) {
    unsupported(FD, "this parameter list");
    return;
  }
  for (size_t K = 0; K < Params.size(); ++K) {
    Vars[Params[K]] = FB.param(K);
    F.Locals[FB.param(K)].Name = Params[K]->Name.str();
  }
  for (const S::Var *R : Obj->Results)
    if (!R->Name.empty() && R->Name != "_") {
      unsupported(FD, "named results");
      return;
    }
  pushScope();
  lowerBlock(FD->Body);
  if (!FB.terminated()) {
    FB.setSpan(L.spanOf(FD->Body->Rbrace));
    if (F.Sig->ret()->isUnit()) {
      exitScopes(0);
      FB.ret();
    } else {
      FB.unreachable();
    }
  }
  Scopes.pop_back();
  // Dead blocks after return/break/continue: close, then drop.
  for (BasicBlock &BB : F.Blocks)
    if (!BB.Term.Set) {
      BB.Term.K = Terminator::Unreachable;
      BB.Term.Set = true;
    }
  removeDeadBlocks(F);
}

//===----------------------------------------------------------------------===//
// Functions of the module
//===----------------------------------------------------------------------===//

/// The @Extern attribute of FD, or null.
static const ast::Attribute *externAttr(const ast::FuncDecl *FD) {
  for (const ast::Attribute *A : FD->Attrs)
    if (!A->Pkg && A->Name && A->Name->Name.str() == "Extern")
      return A;
  return nullptr;
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

/// A type a C function can take or return in M0: a bool, an integer, a float
/// or a pointer (P04.11 adds aggregates through Clang's ABI classification).
static bool isCScalar(const Type *T) {
  return T->isBool() || T->isInt() || T->isFloat() || T->isPtr();
}

void Lowering::declareFunction(const ast::FuncDecl *FD) {
  auto D = I.Defs.find(FD->Name);
  auto *Obj =
      D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::Func>(D->second);
  if (!Obj || FD->Recv || FD->Name->Name.str() == "_") {
    if (FD->Recv)
      unsupported(FD->getBegin(), "a method");
    return;
  }
  // Every return below that is not the last one has reported an error.
  Declined.insert(Obj);
  StringRef Name = FD->Name->Name.str();
  if (Name == "init") {
    // Run before main.main, in the checker's InitOrder: P04.04.
    unsupported(FD->getBegin(), "an init function");
    return;
  }
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
    unsupported(FD->getBegin(), "a generic function");
    return;
  }
  S::SignatureType *Sig = Obj->sig();
  if (Sig->Fails || Sig->Variadic || Sig->Results->size() > 1) {
    unsupported(FD->getBegin(), "this function signature");
    return;
  }
  SmallVector<Type *, 4> Params;
  for (S::Type *P : Sig->Params->Elems) {
    Type *T = lowerType(P, FD->getBegin());
    if (!T)
      return;
    Params.push_back(T);
  }
  Type *Ret = Sig->Results->size()
                  ? lowerType(Sig->Results->Elems[0], FD->getBegin())
                  : Ctx.unit();
  if (!Ret)
    return;
  if (Ext) {
    for (Type *T : Params)
      if (!isCScalar(T)) {
        badExtern(FD->getBegin(), Name,
                  "parameter type " + T->str() +
                      " is not a C scalar or pointer");
        return;
      }
    if (!Ret->isUnit() && !isCScalar(Ret)) {
      badExtern(FD->getBegin(), Name,
                "result type " + Ret->str() + " is not a C scalar or pointer");
      return;
    }
  }
  auto F = std::make_unique<Function>();
  F->Name =
      symbolOf(Ext ? SymbolKind::ExternC : SymbolKind::Func, CP.Pkg, Name);
  if (Ext && F->Name == symbolOf(SymbolKind::Entry, CP.Pkg, "")) {
    badExtern(Ext->getBegin(), Name,
              "the C symbol " + F->Name + " is the program's entry point");
    return;
  }
  F->Sig = Ctx.func(Params, Ret);
  F->CC = Ext ? CallConv::C : CallConv::Go;
  F->NoUnwind = Ext;
  F->Loc = spanOf(FD->getBegin());
  FuncIds[Obj] = M->addFunction(std::move(F));
  Declined.erase(Obj);
}

FuncId Lowering::ensureWrite(SourceLoc At) {
  if (WriteFn != NoId || WriteBad)
    return WriteFn;
  Type *Sig = Ctx.func({Ctx.i32(), Ctx.ptr(Ctx.u8()), Ctx.u64()}, Ctx.i64());
  std::string Sym = symbolOf(SymbolKind::ExternC, CP.Pkg, "write");
  FuncId Id = M->lookupFunction(Sym);
  if (Id != NoId) {
    // The program declares `write` itself: println shares it if it is the
    // function println needs.
    if (M->func(Id).Sig != Sig || !M->func(Id).isDeclaration()) {
      WriteBad = true;
      badExtern(At, "write",
                "println calls it, and needs func write(fd int32, p *byte, "
                "n uint) int");
      return NoId;
    }
    return WriteFn = Id;
  }
  auto F = std::make_unique<Function>();
  F->Name = Sym;
  F->Sig = Sig;
  F->CC = CallConv::C;
  F->NoUnwind = true;
  return WriteFn = M->addFunction(std::move(F));
}

FuncId Lowering::ensurePrintU64(SourceLoc At) {
  if (PrintU != NoId)
    return PrintU;
  FuncId Write = ensureWrite(At);
  if (Write == NoId)
    return NoId;
  Function *F;
  {
    auto Fn = std::make_unique<Function>();
    Fn->Name = symbolOf(SymbolKind::Runtime, CP.Pkg, "print_u64");
    Fn->Sig = Ctx.func({Ctx.u64()}, Ctx.unit());
    Fn->Link = Linkage::Internal;
    Fn->CC = CallConv::Go;
    F = Fn.get();
    PrintU = M->addFunction(std::move(Fn));
  }
  // var buf [20]byte; i := 20
  // for { i--; buf[i] = byte(v%10 + '0'); v /= 10; if v == 0 { break } }
  // write(1, &buf[i], 20-i)
  FuncBuilder FB(*M, *F);
  Type *U64 = Ctx.u64(), *U8 = Ctx.u8();
  LocalId V = FB.param(0);
  F->Locals[V].Name = "v";
  LocalId Buf = FB.newLocal(Ctx.array(U8, 20), Local::Var, "buf");
  LocalId Idx = FB.newLocal(U64, Local::Var, "i");
  FB.assign(FB.local(Idx), FB.use(constInt(U64, 20)));
  BlockId Loop = FB.newBlock(), Done = FB.newBlock();
  FB.gotoBlock(Loop);
  FB.setBlock(Loop);
  FB.assign(FB.local(Idx),
            FB.binary(BinOp::Sub, copyOf(FB.local(Idx)), constInt(U64, 1)));
  Place Digit =
      FB.temp(FB.binary(BinOp::Rem, copyOf(FB.local(V)), constInt(U64, 10)));
  Place Narrow = FB.temp(FB.cast(CastKind::IntToInt, copyOf(Digit), U8));
  Place Ascii =
      FB.temp(FB.binary(BinOp::Add, copyOf(Narrow), constInt(U8, '0')));
  FB.assign(FB.index(FB.local(Buf), Idx), FB.use(copyOf(Ascii)));
  FB.assign(FB.local(V),
            FB.binary(BinOp::Div, copyOf(FB.local(V)), constInt(U64, 10)));
  Place More =
      FB.temp(FB.binary(BinOp::Ne, copyOf(FB.local(V)), constInt(U64, 0)));
  FB.branch(copyOf(More), Loop, Done);
  FB.setBlock(Done);
  Place Ptr = FB.temp(FB.addrOf(FB.index(FB.local(Buf), Idx)));
  Place Count =
      FB.temp(FB.binary(BinOp::Sub, constInt(U64, 20), copyOf(FB.local(Idx))));
  FB.call(constFunc(*M, Write),
          {constInt(Ctx.i32(), 1), copyOf(Ptr), copyOf(Count)},
          FB.local(FB.newTemp(Ctx.i64())), {UnwindAction::Unreachable, NoId});
  FB.ret();
  return PrintU;
}

FuncId Lowering::ensurePrintI64(SourceLoc At) {
  if (PrintI != NoId)
    return PrintI;
  FuncId U = ensurePrintU64(At);
  if (U == NoId)
    return NoId;
  FuncId Write = ensureWrite(At);
  Function *F;
  {
    auto Fn = std::make_unique<Function>();
    Fn->Name = symbolOf(SymbolKind::Runtime, CP.Pkg, "print_i64");
    Fn->Sig = Ctx.func({Ctx.i64()}, Ctx.unit());
    Fn->Link = Linkage::Internal;
    Fn->CC = CallConv::Go;
    F = Fn.get();
    PrintI = M->addFunction(std::move(Fn));
  }
  // u := uint64(v); if v < 0 { write("-"); u = uint64(-v) }; print_u64(u)
  FuncBuilder FB(*M, *F);
  Type *I64 = Ctx.i64(), *U64 = Ctx.u64();
  LocalId V = FB.param(0);
  F->Locals[V].Name = "v";
  LocalId Uv = FB.newLocal(U64, Local::Var, "u");
  FB.assign(FB.local(Uv),
            FB.cast(CastKind::IntToInt, copyOf(FB.local(V)), U64));
  Place Neg =
      FB.temp(FB.binary(BinOp::Lt, copyOf(FB.local(V)), constInt(I64, 0)));
  BlockId Minus = FB.newBlock(), Digits = FB.newBlock();
  FB.branch(copyOf(Neg), Minus, Digits);
  FB.setBlock(Minus);
  Place S = FB.temp(FB.use(constStr(*M, "-")));
  FB.call(
      constFunc(*M, Write),
      {constInt(Ctx.i32(), 1), copyOf(FB.field(S, 0)), copyOf(FB.field(S, 1))},
      FB.local(FB.newTemp(I64)), {UnwindAction::Unreachable, NoId});
  Place Flipped = FB.temp(FB.unary(UnOp::Neg, copyOf(FB.local(V))));
  FB.assign(FB.local(Uv), FB.cast(CastKind::IntToInt, copyOf(Flipped), U64));
  FB.gotoBlock(Digits);
  FB.setBlock(Digits);
  FB.call(constFunc(*M, U), {copyOf(FB.local(Uv))},
          FB.local(FB.newTemp(Ctx.unit())), {UnwindAction::Continue, NoId});
  FB.ret();
  return PrintI;
}

void Lowering::lowerBody(const ast::FuncDecl *FD) {
  auto D = I.Defs.find(FD->Name);
  auto *Obj =
      D == I.Defs.end() ? nullptr : dyn_cast_or_null<S::Func>(D->second);
  if (!Obj || !FD->Body)
    return;
  auto It = FuncIds.find(Obj);
  if (It == FuncIds.end())
    return;
  Function &F = M->func(It->second);
  Body B(*this, F, Obj);
  B.lower(FD);
}

void Lowering::addEntry(const ast::FuncDecl *Main) {
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
  FB.call(constFunc(*M, It->second), {}, FB.local(FB.newTemp(Ctx.unit())),
          {UnwindAction::Continue, NoId});
  FB.assign(FB.local(FB.returnPlace()), FB.use(constInt(Ctx.i32(), 0)));
  FB.ret();
}

std::unique_ptr<Module> Lowering::run() {
  const ast::FuncDecl *Main = nullptr;
  for (const ast::File *File : Files) {
    for (const ast::Decl *D : File->Decls) {
      if (auto *FD = dyn_cast<ast::FuncDecl>(D)) {
        declareFunction(FD);
        if (!FD->Recv && FD->Name->Name.str() == "main")
          Main = FD;
      } else if (auto *GD = dyn_cast<ast::GenDecl>(D)) {
        // Constants are folded by the checker; types are lowered where they
        // are used (lowerType).
        if (GD->Keyword == tok::kw_import || GD->Keyword == tok::kw_type ||
            (GD->Keyword == tok::kw_const && !GD->IsUnit))
          continue;
        unsupported(GD->getBegin(), "this package-level declaration");
      } else {
        unsupported(D->getBegin(), "this declaration");
      }
    }
  }
  for (const ast::File *File : Files)
    for (const ast::Decl *D : File->Decls)
      if (auto *FD = dyn_cast<ast::FuncDecl>(D))
        lowerBody(FD);
  if (Opts.Entry && CP.Pkg && CP.Pkg->Name == "main") {
    if (Main) {
      addEntry(Main);
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

struct StackJob {
  llvm::function_ref<void()> Fn;
  static void *entry(void *Self) {
    static_cast<StackJob *>(Self)->Fn();
    return nullptr;
  }
};

/// Runs Fn on a thread with a LowerStackBytes stack and waits for it. If the
/// thread cannot be made (an address-space limit) Fn runs on this thread:
/// never an abort. The result does not depend on the thread.
void runOnBigStack(llvm::function_ref<void()> Fn) {
  StackJob Job{Fn};
  pthread_attr_t Attr;
  pthread_t Id;
  bool Made = pthread_attr_init(&Attr) == 0;
  if (Made) {
    Made = pthread_attr_setstacksize(&Attr, LowerStackBytes) == 0 &&
           pthread_create(&Id, &Attr, &StackJob::entry, &Job) == 0;
    pthread_attr_destroy(&Attr);
  }
  if (Made)
    pthread_join(Id, nullptr);
  else
    Fn();
}

} // namespace

std::string gir::symbolOf(SymbolKind K, const sema::Package *Pkg,
                          StringRef Name) {
  switch (K) {
  case SymbolKind::Func: {
    // A main package is `main` whatever path the command line gave it.
    StringRef P = !Pkg || Pkg->Name == "main" ? StringRef("main") : Pkg->Path;
    return (P + "." + Name).str();
  }
  case SymbolKind::ExternC:
    return Name.str();
  case SymbolKind::Entry:
    return "main";
  case SymbolKind::Runtime:
    return ("gopp." + Name).str();
  }
  return Name.str();
}

std::unique_ptr<Module> gir::lowerPackage(Context &Ctx, const SourceManager &SM,
                                          const sema::CheckedPackage &CP,
                                          ArrayRef<const ast::File *> Files,
                                          DiagnosticsEngine &Diags,
                                          const LowerOptions &Opts) {
  std::unique_ptr<Module> M;
  runOnBigStack([&] { M = Lowering(Ctx, SM, CP, Files, Diags, Opts).run(); });
  return M;
}
