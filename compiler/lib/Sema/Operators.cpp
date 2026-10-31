//===- Operators.cpp - Operator methods (P03.24, ADR-0024) ----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Operator methods: the form of a declaration (EXP-16), overload sets and the
// "second meaning" rule (EXP-17), @Convert (EXP-25), @Right (EXP-26), the
// resolution of every operator expression (EXP-17 to EXP-24), and operator
// elements of constraints (GEN-13).
//
// The design in one paragraph. An operator method is an ordinary method whose
// name is the operator (`+`, `[]=`, `()`, `range`) and whose Func::Op is the
// form that the name and its parameter count give (`-` with no parameter is
// Neg). It sits in the method list of its named type, so declaration order,
// export data and instantiation need nothing new. The operator methods of a
// type and its `extends` ancestors are one table per type (opTable), answered
// from the declarations alone. An operator expression is resolved to ONE
// method (pickOverload) and then checked as the ordinary method call
// `recv.M(args)` (opCall): a call node is made for it, the operands that were
// evaluated for the resolution are handed to it (PreEval), its selector
// names the method itself (OpSelect, as an overload set shares the name), and
// callExpr checks it. So borrow checking, views, never-nil, Send/Sync and
// `go`/`defer` are the rules of method calls, nothing is written twice
// (EXP-18), and Info::Operators keeps the call for the lowering (P04.23).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

#include "llvm/ADT/StringSwitch.h"

#include <map>

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

static const ast::Expr *unparen(const ast::Expr *E) {
  while (auto *P = dyn_cast_or_null<ParenExpr>(E))
    E = P->X;
  return E;
}

//===----------------------------------------------------------------------===//
// Names and kinds
//===----------------------------------------------------------------------===//

OpKind sema::opKindOf(StringRef N, unsigned NP) {
  if (!isOperatorSpelling(N))
    return OpKind::None;
  using K = OpKind;
  return llvm::StringSwitch<OpKind>(N)
      .Case("+", NP == 0 ? K::Pos : K::Add)
      .Case("-", NP == 0 ? K::Neg : K::Sub)
      .Case("*", NP == 0 ? K::Deref : K::Mul)
      .Case("/", K::Quo)
      .Case("%", K::Rem)
      .Case("&", NP == 0 ? K::Bad : K::And) // unary `&` is blocked (EXP-29)
      .Case("|", K::Or)
      .Case("^", NP == 0 ? K::Compl : K::Xor)
      .Case("<<", K::Shl)
      .Case(">>", K::Shr)
      .Case("&^", K::AndNot)
      .Case("!", K::Not)
      .Case("==", K::Eq)
      .Case("<=>", K::Cmp)
      .Case("&&", K::LAnd)
      .Case("||", K::LOr)
      .Case("[]", K::Index)
      .Case("[]=", K::IndexSet)
      .Case("()", K::Call)
      .Case("<-", NP == 0 ? K::Recv : K::Send)
      .Case("++", K::Inc)
      .Case("--", K::Dec)
      .Case("range", K::Range)
      .Case("+=", K::AddAssign)
      .Case("-=", K::SubAssign)
      .Case("*=", K::MulAssign)
      .Case("/=", K::QuoAssign)
      .Case("%=", K::RemAssign)
      .Case("&=", K::AndAssign)
      .Case("|=", K::OrAssign)
      .Case("^=", K::XorAssign)
      .Case("<<=", K::ShlAssign)
      .Case(">>=", K::ShrAssign)
      .Case("&^=", K::AndNotAssign)
      .Default(K::Bad);
}

const char *sema::opSpelling(OpKind K) {
  using O = OpKind;
  switch (K) {
  case O::Add:
  case O::Pos:
    return "+";
  case O::Sub:
  case O::Neg:
    return "-";
  case O::Mul:
  case O::Deref:
    return "*";
  case O::Quo:
    return "/";
  case O::Rem:
    return "%";
  case O::And:
    return "&";
  case O::Or:
    return "|";
  case O::Xor:
  case O::Compl:
    return "^";
  case O::Shl:
    return "<<";
  case O::Shr:
    return ">>";
  case O::AndNot:
    return "&^";
  case O::Not:
    return "!";
  case O::Eq:
    return "==";
  case O::Cmp:
    return "<=>";
  case O::LAnd:
    return "&&";
  case O::LOr:
    return "||";
  case O::Index:
    return "[]";
  case O::IndexSet:
    return "[]=";
  case O::Call:
    return "()";
  case O::Send:
  case O::Recv:
    return "<-";
  case O::Inc:
    return "++";
  case O::Dec:
    return "--";
  case O::Range:
    return "range";
  case O::AddAssign:
    return "+=";
  case O::SubAssign:
    return "-=";
  case O::MulAssign:
    return "*=";
  case O::QuoAssign:
    return "/=";
  case O::RemAssign:
    return "%=";
  case O::AndAssign:
    return "&=";
  case O::OrAssign:
    return "|=";
  case O::XorAssign:
    return "^=";
  case O::ShlAssign:
    return "<<=";
  case O::ShrAssign:
    return ">>=";
  case O::AndNotAssign:
    return "&^=";
  case O::Convert:
    return "@Convert";
  case O::None:
  case O::Bad:
    break;
  }
  return "?";
}

bool sema::isBinaryArith(OpKind K) {
  return K >= OpKind::Add && K <= OpKind::AndNot;
}
bool sema::isCompoundOp(OpKind K) {
  return K >= OpKind::AddAssign && K <= OpKind::AndNotAssign;
}
OpKind sema::compoundToBinary(OpKind K) {
  return static_cast<OpKind>(static_cast<unsigned>(K) -
                             static_cast<unsigned>(OpKind::AddAssign) +
                             static_cast<unsigned>(OpKind::Add));
}
OpKind sema::binaryToCompound(OpKind K) {
  return static_cast<OpKind>(static_cast<unsigned>(K) -
                             static_cast<unsigned>(OpKind::Add) +
                             static_cast<unsigned>(OpKind::AddAssign));
}
bool sema::isCommutativeOp(OpKind K) {
  switch (K) {
  case OpKind::Add:
  case OpKind::Mul:
  case OpKind::And:
  case OpKind::Or:
  case OpKind::Xor:
  case OpKind::Eq:
    return true;
  default:
    return false;
  }
}

namespace {

/// The kind of the binary operator token of an expression: `!=` is Eq, the
/// orderings and `<=>` are Cmp; None for any other token.
OpKind binaryKindOf(tok::Kind T) {
  switch (T) {
  case tok::add:
    return OpKind::Add;
  case tok::sub:
    return OpKind::Sub;
  case tok::mul:
    return OpKind::Mul;
  case tok::quo:
    return OpKind::Quo;
  case tok::rem:
    return OpKind::Rem;
  case tok::and_:
    return OpKind::And;
  case tok::or_:
    return OpKind::Or;
  case tok::xor_:
    return OpKind::Xor;
  case tok::shl:
    return OpKind::Shl;
  case tok::shr:
    return OpKind::Shr;
  case tok::and_not:
    return OpKind::AndNot;
  case tok::land:
    return OpKind::LAnd;
  case tok::lor:
    return OpKind::LOr;
  case tok::eql:
  case tok::neq:
    return OpKind::Eq;
  case tok::lss:
  case tok::leq:
  case tok::gtr:
  case tok::geq:
  case tok::cmp3:
    return OpKind::Cmp;
  default:
    return OpKind::None;
  }
}

/// The token of a binary or unary operator kind, for the built-in test; eof
/// for the kinds that have none.
tok::Kind tokOf(OpKind K) {
  switch (K) {
  case OpKind::Add:
  case OpKind::Pos:
    return tok::add;
  case OpKind::Sub:
  case OpKind::Neg:
    return tok::sub;
  case OpKind::Mul:
    return tok::mul;
  case OpKind::Quo:
    return tok::quo;
  case OpKind::Rem:
    return tok::rem;
  case OpKind::And:
    return tok::and_;
  case OpKind::Or:
    return tok::or_;
  case OpKind::Xor:
  case OpKind::Compl:
    return tok::xor_;
  case OpKind::Shl:
    return tok::shl;
  case OpKind::Shr:
    return tok::shr;
  case OpKind::AndNot:
    return tok::and_not;
  case OpKind::Not:
    return tok::not_;
  case OpKind::LAnd:
    return tok::land;
  case OpKind::LOr:
    return tok::lor;
  default:
    return tok::eof;
  }
}

/// The forms that look through a pointer operand, like the method call
/// `p.M()`: they have no built-in meaning on a pointer to a user type, and
/// their methods often have a pointer receiver (index, call, `<-`, `range`).
/// The others keep the pointer's own meaning (`p == q`, `*p`).
bool throughPtr(OpKind K) {
  switch (K) {
  case OpKind::Index:
  case OpKind::IndexSet:
  case OpKind::Call:
  case OpKind::Send:
  case OpKind::Recv:
  case OpKind::Range:
    return true;
  default:
    return false;
  }
}

/// The binary operator methods that may carry @Right (EXP-26).
bool rightable(OpKind K) {
  return isBinaryArith(K) || K == OpKind::Eq || K == OpKind::Cmp;
}

std::string quoted(StringRef Sp) { return "`" + Sp.str() + "`"; }

unsigned paramCount(const Func *M) {
  return cast<SignatureType>(M->Ty)->Params->size();
}

/// The named type an operator method is declared on (its receiver base).
const NamedType *ownerOf(const Func *M) {
  if (!M->Recv || !M->Recv->Ty)
    return nullptr;
  const Type *RT = M->Recv->Ty;
  if (auto *P = dyn_cast<PointerType>(RT))
    RT = P->Elem;
  return dyn_cast<NamedType>(RT);
}

/// Is M declared on an ancestor of N, not on N itself (EXP-30)?
bool inherited(const Func *M, const Type *T) {
  const NamedType *RN = ownerOf(M);
  auto *N = dyn_cast_or_null<NamedType>(T);
  return RN && N && RN->origin() != N->origin();
}

} // namespace

//===----------------------------------------------------------------------===//
// Declarations
//===----------------------------------------------------------------------===//

void Checker::readOperatorMarks(const FuncDecl *FD, Func *Fn) {
  if (S.Opts.GoCompat)
    return;
  if (FD->Recv && isOperatorSpelling(Fn->Name))
    Fn->Op = opKindOf(Fn->Name,
                      FD->Type->Params ? FD->Type->Params->numFields() : 0);
  for (const Attribute *A : FD->Attrs) {
    if (A->Pkg || !A->Name)
      continue;
    StringRef N = name(A->Name);
    if (N != "Right" && N != "Convert")
      continue;
    bool IsRight = N == "Right";
    if (A->HasArgs)
      err(A->AtLoc, diag::err_attr_no_args, {"@" + N.str()});
    if (!FD->Recv) {
      err(A->AtLoc, IsRight ? diag::err_right_misuse : diag::err_convert_misuse,
          {"the function " + std::string(name(FD->Name)),
           "it is only for methods"});
      continue;
    }
    if (IsRight)
      Fn->Right = true;
    else if (Fn->Op != OpKind::None)
      err(FD->Name->getBegin(), diag::err_convert_misuse,
          {quoted(Fn->Name), "@Convert is for an ordinary method, not for an "
                             "operator method"});
    else
      Fn->Op = OpKind::Convert;
  }
  if (Fn->Op != OpKind::None)
    Ctx.noteOperators();
}

SignatureType *Checker::opNormSig(Func *F) {
  auto *Sig = dyn_cast_or_null<SignatureType>(F->Ty);
  if (!Sig)
    return nullptr;
  if (F->RecvTParams.empty() || !F->Recv)
    return Sig;
  Type *RT = F->Recv->Ty;
  if (auto *P = dyn_cast_or_null<PointerType>(RT))
    RT = P->Elem;
  auto *N = dyn_cast_or_null<NamedType>(RT);
  if (!N)
    return Sig;
  NamedType *O = N->origin();
  if (O->TParams.size() != F->RecvTParams.size())
    return Sig;
  SmallVector<Type *, 4> Args(O->TParams.begin(), O->TParams.end());
  return dyn_cast_or_null<SignatureType>(subst(Sig, F->RecvTParams, Args));
}

/// The reason the type has the operator already (EXP-17 "second meaning"), or
/// empty. RecvT is the receiver base type.
std::string Checker::opSecondMeaning(Func *F, StringRef Sp, Type *RecvT) {
  auto *Sig = dyn_cast_or_null<SignatureType>(F->Ty);
  if (!Sig || !RecvT)
    return {};
  Type *U = under(RecvT);
  if (!U || !isValid(U))
    return {};
  const OpKind K = F->Op;
  Type *P0 = paramCount(F) >= 1 ? Sig->Params->Elems[0] : nullptr;
  std::string Name = str(RecvT);
  auto Has = [&](const std::string &What) {
    return Name + " has " + What + " already (its underlying type is " +
           str(U) + ")";
  };
  switch (K) {
  case OpKind::Index:
  case OpKind::IndexSet: {
    bool Builtin = isa<ArrayType>(U) || isa<SliceType>(U) || isa<MapType>(U) ||
                   isString(U);
    if (auto *P = dyn_cast<PointerType>(U))
      Builtin = Builtin || isa<ArrayType>(under(P->Elem));
    return Builtin ? Has("the built-in index " + quoted("[]")) : std::string();
  }
  case OpKind::Call:
    return isa<SignatureType>(U) ? Has("the built-in call") : std::string();
  case OpKind::Send:
  case OpKind::Recv:
    return isa<sema::ChanType>(U)
               ? Has("the built-in channel operator " + quoted("<-"))
               : std::string();
  case OpKind::Range:
    if (isa<SliceType>(U) || isa<ArrayType>(U) || isa<MapType>(U) ||
        isa<sema::ChanType>(U) || isa<SignatureType>(U) || isString(U) ||
        isInteger(U))
      return Has("`range`");
    if (auto *P = dyn_cast<PointerType>(U); P && isa<ArrayType>(under(P->Elem)))
      return Has("`range`");
    return {};
  case OpKind::Deref:
    return isa<PointerType>(U) ? Has("the built-in dereference")
                               : std::string();
  case OpKind::Inc:
  case OpKind::Dec:
    return isNumeric(U) ? Has("the built-in " + quoted(Sp)) : std::string();
  default:
    break;
  }
  const bool Basic = isa<BasicType>(U);
  if (!Basic && !isa<sema::ChanType>(U))
    return {};
  const OpKind Base = isCompoundOp(K) ? compoundToBinary(K) : K;
  const StringRef Eff = opSpelling(Base);
  switch (Base) {
  case OpKind::Neg:
  case OpKind::Pos:
  case OpKind::Compl:
  case OpKind::Not:
    return Basic && opAllowed(*this, tokOf(Base), RecvT, true)
               ? Has("the unary " + quoted(Sp))
               : std::string();
  default:
    break;
  }
  if (paramCount(F) != 1)
    return {};
  const bool SameP = identical(P0, RecvT);
  switch (Base) {
  case OpKind::Shl:
  case OpKind::Shr:
    return Basic && isInteger(RecvT) && isInteger(P0)
               ? Has("the built-in shift " + quoted(Eff) + " by an integer")
               : std::string();
  case OpKind::Eq:
    return SameP && comparableType(RecvT, false, nullptr)
               ? Has("the built-in " + quoted("==") + " (" + Name + ", " +
                     Name + ") bool")
               : std::string();
  case OpKind::Cmp:
    return SameP && Basic &&
                   (isInteger(RecvT) || isFloat(RecvT) || isString(RecvT))
               ? Has("the built-in " + quoted("<=>") + " (EXP-20)")
               : std::string();
  case OpKind::LAnd:
  case OpKind::LOr:
    if (Basic && allBoolean(RecvT)) {
      Type *Lazy = nullptr;
      if (auto *PS = dyn_cast_or_null<SignatureType>(P0))
        if (PS->Params->size() == 0 && PS->Results->size() == 1)
          Lazy = PS->Results->Elems[0];
      if (SameP || (Lazy && identical(Lazy, RecvT)))
        return Has("the built-in " + quoted(Eff));
    }
    return {};
  default:
    break;
  }
  if (isBinaryArith(Base) && SameP && Basic &&
      opAllowed(*this, tokOf(Base), RecvT, false))
    return Has("the built-in " + quoted(Eff) + " (" + Name + ", " + Name +
               ") " + Name);
  return {};
}

void Checker::checkOperatorShape(Func *F, const FuncDecl *FD,
                                 NamedType *RecvNamed) {
  const bool IsOp = F->isOperator(), IsConvert = F->Op == OpKind::Convert;
  // A blocked name was reported by the parser (EXP-29).
  if (F->Op == OpKind::Bad || (!IsOp && !IsConvert && !F->Right))
    return;
  auto *Sig = dyn_cast_or_null<SignatureType>(F->Ty);
  if (!Sig)
    return;
  StringRef Sp = F->Name;
  const unsigned NP = Sig->Params->size(), NR = Sig->Results->size();
  const bool Ptr = F->HasPtrRecv;
  Type *RecvT = RecvNamed ? static_cast<Type *>(RecvNamed) : nullptr;
  SourceLoc At = FD->Name->getBegin();
  const std::string Disp = quoted(Sp);
  bool Bad = false;
  auto Shape = [&](const std::string &Why) {
    err(At, diag::err_op_shape, {Disp, Why});
    Bad = true;
  };
  auto NeedRecv = [&](const char *Kind, const std::string &Why) {
    err(At, diag::err_op_receiver, {Disp, Kind, Why});
    Bad = true;
  };
  auto Result = [&](unsigned I) -> Type * {
    return I < NR ? Sig->Results->Elems[I] : nullptr;
  };

  if (IsOp) {
    // No own type parameters and no `!` (EXP-30): the rest of the form is
    // not asked then (a `!` function has an error result).
    if (FD->Type->TypeParams && FD->Type->TypeParams->numFields()) {
      err(FD->Type->TypeParams, diag::err_op_tparams, {Disp});
      Bad = true;
    }
    if (Sig->Fails) {
      err(At, diag::err_op_fails, {Disp});
      Bad = true;
    }
    if (Bad)
      return;
    // Only `()` may be variadic (EXP-23): the other forms take plain
    // parameters, as the constraint elements do (GEN-13).
    if (Sig->Variadic && F->Op != OpKind::Call) {
      Shape("the parameters of this operator are plain: a variadic `...` "
            "parameter is only for `()`");
      return;
    }
    auto PureValue = [&](const char *What) {
      if (Ptr)
        NeedRecv("value", std::string(What) +
                              " would need &a, and a may be a temporary; the "
                              "operands are borrowed, so the method cannot "
                              "change them");
    };
    if (Sp == "+" || Sp == "-" || Sp == "^") {
      if (NP > 1)
        Shape("a binary operator takes one parameter (the right operand), a "
              "unary one takes none");
      else if (NR != 1)
        Shape("it returns exactly one result");
      PureValue(NP == 0 ? "a unary operator" : "a binary operator");
    } else if (Sp == "*") {
      if (NP == 1) {
        if (NR != 1)
          Shape("a binary operator returns exactly one result");
        PureValue("a binary operator");
      } else if (NP == 0) {
        Type *R = Result(0);
        if (NR != 1)
          Shape("a user `*` returns exactly one result, a View[T]");
        else if (R && isValid(R) && !isViewType(R))
          Shape("a user `*` must return View[T] (read-only), not " + str(R));
        PureValue("the dereference");
      } else {
        Shape("a binary operator takes one parameter, the dereference none");
      }
    } else if (isBinaryArith(F->Op)) { // `/ % & | &^ << >>`
      if (NP != 1)
        Shape("a binary operator takes one parameter (the right operand)");
      else if (NR != 1)
        Shape("a binary operator returns exactly one result");
      PureValue("a binary operator");
    } else if (F->Op == OpKind::Not) {
      if (NP != 0)
        Shape("`!` is unary only: it takes no parameter");
      else if (NR != 1)
        Shape("it returns exactly one result");
      PureValue("a unary operator");
    } else if (F->Op == OpKind::Eq) {
      if (NP != 1)
        Shape("`==` takes one parameter (the right operand)");
      else if (NR != 1 || !isBoolean(Result(0)))
        Shape("`==` must return bool");
      PureValue("a comparison");
    } else if (F->Op == OpKind::Cmp) {
      if (NP != 1)
        Shape("`<=>` takes one parameter (the right operand)");
      else if (NR != 1 || !Result(0) ||
               !identical(Result(0), typ(BasicKind::Int)))
        Shape("`<=>` must return int (-1, 0 or 1)");
      PureValue("a comparison");
    } else if (F->Op == OpKind::LAnd || F->Op == OpKind::LOr) {
      if (NP != 1)
        Shape("it takes one parameter: the right operand `U` or `func() U` "
              "(EXP-21)");
      else if (NR != 1)
        Shape("it returns exactly one result");
      PureValue("a logical operator");
    } else if (F->Op == OpKind::Index) {
      if (NP < 1)
        Shape("it takes the indexes as parameters (at least one)");
      else if (NR != 1)
        Shape("it returns exactly one result");
    } else if (F->Op == OpKind::IndexSet) {
      if (NP < 2)
        Shape("it takes the indexes and then the value to store (at least two "
              "parameters)");
      else if (NR != 0)
        Shape("it has no result");
      if (!Ptr && !Bad)
        NeedRecv("pointer",
                 "`[]=` changes the receiver; a value receiver would change a "
                 "copy");
    } else if (F->Op == OpKind::Call) {
      // any parameters, any results
    } else if (F->Op == OpKind::Send || F->Op == OpKind::Recv) {
      if (NP == 1 && NR == 0) {
        // send
      } else if (NP == 0 && (NR == 1 || (NR == 2 && isBoolean(Result(1))))) {
        // receive, `() R` or `() (R, bool)`
      } else {
        Shape("a send takes one parameter and has no result; a receive takes "
              "none and returns `R` or `(R, bool)` (CON-22)");
      }
      if (!Ptr && !Bad)
        NeedRecv("pointer",
                 "a send or receive changes the queue; a value receiver "
                 "would change a copy");
    } else if (F->Op == OpKind::Inc || F->Op == OpKind::Dec) {
      if (NP != 0 || NR != 0)
        Shape("it takes no parameter and has no result");
      if (!Ptr && !Bad)
        NeedRecv("pointer", "`" + Sp.str() + "` changes the receiver");
    } else if (isCompoundOp(F->Op)) {
      // The optional in-place compound form (EXP-19).
      if (NP != 1)
        Shape("an in-place compound operator takes one parameter (the right "
              "operand)");
      else if (NR != 0)
        Shape("an in-place compound operator has no result");
      if (!Ptr && !Bad)
        NeedRecv("pointer",
                 "an in-place " + quoted(Sp) +
                     " changes the receiver; a value receiver would change a "
                     "copy");
    } else if (F->Op == OpKind::Range) {
      if (NP != 0)
        Shape("`range` takes no parameter");
      else if (NR != 1)
        Shape("`range` returns exactly one result, a function type that "
              "`range` accepts");
      else {
        Type *R = Result(0);
        auto *RS = R ? dyn_cast<SignatureType>(under(R)) : nullptr;
        bool Ok = false;
        if (RS && RS->Params->size() == 1 && RS->Results->size() == 0)
          if (auto *Y = dyn_cast<SignatureType>(under(RS->Params->Elems[0])))
            Ok = Y->Params->size() <= 2 && Y->Results->size() == 1 &&
                 isBoolean(Y->Results->Elems[0]);
        if (!Ok && R && isValid(R))
          Shape("the result must be a function type that `range` accepts, "
                "like iter.Seq[V]: func(yield func(V) bool); not " +
                str(R));
      }
    }
    // UNI-15: the operators of a unit type are the built-in ones of EXP-33
    // to EXP-36; a method would be a second meaning that skips the unit math
    // (a named type over a unit type too).
    if (RecvT && Ctx.unitsSeen() && unitOf(RecvT)) {
      err(At, diag::err_unit_op_method, {Disp, str(RecvT)});
      F->Op = OpKind::None; // the built-in operator stays the meaning
    } else if (!Bad && RecvT) {
      std::string Why = opSecondMeaning(F, Sp, RecvT);
      if (!Why.empty())
        err(At, diag::err_op_second_meaning, {Disp, str(RecvT), Why});
    }
  }

  if (F->Right) {
    auto Misuse = [&](const std::string &Why) {
      err(At, diag::err_right_misuse, {Disp, Why});
    };
    if (!IsOp || !rightable(F->Op) || NP != 1) {
      if (F->Op == OpKind::LAnd || F->Op == OpKind::LOr)
        Misuse("not allowed on `&&` and `||`: the swap would break the "
               "laziness of the right operand (EXP-21)");
      else if (IsOp && NP == 0)
        Misuse("a unary operator has no left operand to swap");
      else
        Misuse("it is only for a binary operator method (+ - * / % & | ^ << "
               ">> &^ == <=>)");
    } else if (RecvT && identical(Sig->Params->Elems[0], RecvT)) {
      Misuse("the parameter type must differ from the receiver type " +
             str(RecvT) + ": with the same type both orders are the same call");
    }
  }

  if (IsConvert) {
    auto Misuse = [&](const std::string &Why) {
      err(At, diag::err_convert_misuse, {Disp, Why});
    };
    if (NP != 0 || NR != 1) {
      Misuse("it takes no parameters and returns exactly one result");
    } else if (Ptr) {
      Misuse("it needs a value receiver: a conversion does not change its "
             "operand");
    } else if (RecvT) {
      Type *R = Result(0);
      Operand X;
      X.M = Mode::Value;
      X.Ty = RecvT;
      std::string Cause;
      if (R && isValid(R) && convertibleTo(X, R, &Cause))
        Misuse(str(RecvT) + " already converts to " + str(R) +
               " (EXP-9): the conversion would have two meanings");
    }
  }
}

void Checker::checkOperatorSets() {
  if (S.Opts.GoCompat || !Ctx.operatorsSeen())
    return;
  struct Entry {
    Func *F;
    SignatureType *Sig;
  };
  // Keyed by the origin of the receiver type and the set: the kind, with a
  // @Right method of a non-commutative operator in a set of its own (it
  // serves the swapped order, EXP-17); @Convert methods by their target.
  std::map<std::pair<NamedType *, unsigned>, SmallVector<Entry, 2>> Sets;
  for (Object *O : S.PkgObjects) {
    auto *F = dyn_cast<Func>(O);
    if (!F || !F->Decl || !F->Decl->Recv || !F->Recv)
      continue;
    const bool IsOp = F->isOperator();
    if ((!IsOp && F->Op != OpKind::Convert) || F->Op == OpKind::Bad)
      continue;
    // A `!` operator or one with type parameters of its own was reported by
    // its own rule (checkOperatorShape).
    if (IsOp &&
        (!F->OwnTParams.empty() ||
         (F->Decl->Type->TypeParams && F->Decl->Type->TypeParams->numFields())))
      continue;
    SignatureType *Sig = opNormSig(F);
    if (IsOp && Sig && Sig->Fails)
      continue;
    if (!Sig)
      continue;
    // A malformed @Convert was reported by its own rule.
    if (!IsOp && (Sig->Params->size() != 0 || Sig->Results->size() != 1))
      continue;
    auto *N = const_cast<NamedType *>(ownerOf(F));
    if (!N)
      continue;
    unsigned Key = static_cast<unsigned>(F->Op) * 2;
    if (F->Right && !isCommutativeOp(F->Op))
      Key += 1;
    auto &Set = Sets[{N->origin(), Key}];
    for (const Entry &E : Set) {
      bool Same;
      if (IsOp)
        Same = identical(E.Sig->Params, Sig->Params);
      else
        Same = identical(E.Sig->Results->Elems[0], Sig->Results->Elems[0]);
      if (!Same)
        continue;
      unsigned Line = S.SM.getLineNumber(E.F->Pos);
      if (IsOp) {
        std::string Params = "(";
        for (size_t I = 0; I < Sig->Params->Elems.size(); ++I)
          Params += (I ? ", " : "") + str(Sig->Params->Elems[I]);
        Params += ")";
        err(F->Pos, diag::err_op_duplicate,
            {quoted(F->Name), Params + (F->Right ? " with @Right" : ""),
             std::to_string(Line)});
      } else {
        err(F->Pos, diag::err_convert_misuse,
            {"`" + std::string(F->Name) + "`",
             str(N) + " already has a @Convert to " +
                 str(Sig->Results->Elems[0]) + ": " + std::string(E.F->Name) +
                 " at line " + std::to_string(Line) +
                 " (one per target type)"});
      }
      break;
    }
    Set.push_back({F, Sig});
  }
}

//===----------------------------------------------------------------------===//
// Which methods a type has: the operator table
//===----------------------------------------------------------------------===//

const OpTable *Checker::opTable(NamedType *N) {
  if (const OpTable *T = N->OpsState.load(std::memory_order_acquire))
    return T;
  // Up the parent chain to the first type with a stored table, then back
  // down, so a chain of n types costs n steps however many ask.
  SmallVector<NamedType *, 8> Chain;
  llvm::SmallPtrSet<NamedType *, 8> OnChain; // a ring is reported elsewhere
  const OpTable *Base = nullptr;
  for (NamedType *C = N; C && OnChain.insert(C).second; C = parentOf(C)) {
    if (const OpTable *T = C->OpsState.load(std::memory_order_acquire)) {
      Base = T;
      break;
    }
    Chain.push_back(C);
  }
  if (!Base)
    Base = TypeContext::emptyOps();
  const bool Final = typesFinal();
  for (size_t K = Chain.size(); K-- > 0;) {
    NamedType *C = Chain[K];
    NamedType *O = C->origin();
    SmallVector<Func *, 4> Own;
    if (O->Methods)
      for (unsigned I = 0; I < O->Methods->size(); ++I) {
        Func *M = (*O->Methods)[I];
        if (M->Op == OpKind::None || M->Op == OpKind::Bad)
          continue;
        if (!M->Ty)
          objDecl(M, nullptr);
        Own.push_back(C->Orig ? instantiatedMethod(C, I) : M);
      }
    const OpTable *T = Base;
    if (!Own.empty()) {
      std::stable_sort(Own.begin(), Own.end(),
                       [](Func *A, Func *B) { return A->Op < B->Op; });
      T = Ctx.newOpTable(Own, Base);
    }
    // The table is a function of the declarations: store it once they are
    // final. Two threads may build it; the first one stored is kept.
    if (Final && declFinished(C)) {
      const OpTable *First = nullptr;
      if (!C->OpsState.compare_exchange_strong(First, T,
                                               std::memory_order_acq_rel))
        T = First;
    }
    Base = T;
  }
  return Base;
}

Type *Checker::opBase(Type *T, bool ThroughPtr) {
  if (!T || S.Opts.GoCompat || !Ctx.operatorsSeen())
    return nullptr;
  if (auto *R = dyn_cast<RefType>(T); R && !R->Borrowed)
    T = R->Elem; // View[X] (EXP-24, OWN-V1)
  else if (auto *P = dyn_cast<PointerType>(T); P && ThroughPtr)
    T = P->Elem;
  if (isa<TypeParamType>(T))
    return T;
  auto *N = dyn_cast<NamedType>(T);
  if (!N || N->Ptr != PtrKind::None || N->IsOption || N->IsView)
    return nullptr;
  return N;
}

/// Does T have no built-in operators, so that a missing operator method is
/// the whole story: a defined type that is no basic type or interface, or a
/// type parameter whose constraint has no type terms. Plain and generic code
/// then report the same error (EXP-17, GEN-13).
bool Checker::hasNoBuiltinOps(Type *T) {
  if (auto *TP = dyn_cast_or_null<TypeParamType>(T)) {
    TypeSet *TS = typeSetOf(TP);
    return TS && !TS->hasTerms();
  }
  auto *N = dyn_cast_or_null<NamedType>(T);
  return N && !isa<BasicType>(under(N)) && !isa<InterfaceType>(under(N));
}

void Checker::opCandidates(Type *T, OpKind K,
                           llvm::SmallVectorImpl<Func *> &Out) {
  if (!T || S.Opts.GoCompat || !Ctx.operatorsSeen())
    return;
  auto Usable = [](Func *M) {
    return llvm::isa_and_nonnull<SignatureType>(M->Ty);
  };
  if (auto *TP = dyn_cast<TypeParamType>(T)) {
    // GEN-13: the operator elements of the constraint; with `T extends P`
    // also the operator methods of P (GEN-12).
    if (TypeSet *TS = typeSetOf(TP)) {
      for (Func *M : TS->Methods)
        if (M->Op == K && Usable(M))
          Out.push_back(M);
      if (TS->Extends)
        opCandidates(opBase(TS->Extends), K, Out);
    }
    return;
  }
  auto *N = dyn_cast<NamedType>(T);
  if (!N)
    return;
  // The type, then its `extends` ancestors (EXP-30): the list of the table,
  // made once.
  ArrayRef<Func *> L = opMerged(opTable(N), K);
  Out.append(L.begin(), L.end());
}

ArrayRef<Func *> Checker::opMerged(const OpTable *T, OpKind K) {
  const unsigned Slot = static_cast<unsigned>(K);
  // A table holds one run per kind. A nearer method with the same parameter
  // types (the same target for @Convert) hides a farther one: an @Override.
  // Methods of one level never hide each other: they are an overload set.
  // What a table adds to the list of the tables behind it depends on nothing
  // else, so the lists are made from the farthest table that has none.
  SmallVector<const OpTable *, 16> Todo;
  ArrayRef<Func *> Far;
  for (const OpTable *Tab = T; Tab; Tab = Tab->Next) {
    // The shared empty table belongs to no context: nothing is kept in it.
    if (Tab == TypeContext::emptyOps())
      break;
    if (const ArrayRef<Func *> *Done =
            Tab->Merged[Slot].load(std::memory_order_acquire)) {
      Far = *Done;
      break;
    }
    Todo.push_back(Tab);
  }
  for (size_t Ix = Todo.size(); Ix-- > 0;) {
    const OpTable *Tab = Todo[Ix];
    SmallVector<Func *, 8> Mine, Merged;
    auto Lo = std::lower_bound(Tab->Own.begin(), Tab->Own.end(), K,
                               [](Func *F, OpKind Kd) { return F->Op < Kd; });
    for (auto It = Lo; It != Tab->Own.end() && (*It)->Op == K; ++It)
      if (llvm::isa_and_nonnull<SignatureType>((*It)->Ty))
        Mine.push_back(*It);
    Merged = Mine;
    for (Func *G : Far) {
      bool Hidden = false;
      for (size_t J = 0; J < Mine.size() && !Hidden; ++J) {
        Func *F = Mine[J];
        Hidden = K == OpKind::Convert
                     ? identical(F->sig()->Results, G->sig()->Results)
                     : G->Right == F->Right &&
                           identical(F->sig()->Params, G->sig()->Params);
      }
      if (!Hidden)
        Merged.push_back(G);
    }
    const ArrayRef<Func *> *List = Ctx.newOpList(Merged);
    const ArrayRef<Func *> *Expected = nullptr;
    if (!Tab->Merged[Slot].compare_exchange_strong(Expected, List,
                                                   std::memory_order_acq_rel))
      List = Expected;
    Far = *List;
  }
  return Far;
}

bool Checker::hasOpKind(Type *T, OpKind K) {
  SmallVector<Func *, 4> V;
  opCandidates(T, K, V);
  return !V.empty();
}

//===----------------------------------------------------------------------===//
// Picking the overload (EXP-17)
//===----------------------------------------------------------------------===//

std::string Checker::opArgString(const Operand &X) {
  if (X.isNil())
    return "nil";
  if (X.Ty && !isTyped(X.Ty)) {
    std::string K = cast<BasicType>(X.Ty)->Name;
    return K + (X.M == Mode::Constant ? " constant " + X.Val.str() : " value");
  }
  return X.Ty ? str(X.Ty) : std::string("?");
}

std::string Checker::opSetString(ArrayRef<Func *> Cands) {
  std::string R;
  for (Func *M : Cands) {
    if (!R.empty())
      R += "; ";
    R += funcString(M);
    if (M->Right)
      R += " @Right";
  }
  return R;
}

static bool lazyParam(Type *P, Type *&U) {
  auto *Sg = dyn_cast_or_null<SignatureType>(P);
  if (!Sg || Sg->Params->size() != 0 || Sg->Results->size() != 1 ||
      Sg->Variadic)
    return false;
  U = Sg->Results->Elems[0];
  return true;
}

Checker::OpMatch Checker::pickOverload(ArrayRef<Func *> Cands,
                                       ArrayRef<Operand *> Args, bool Lazy,
                                       int MaxPhase) {
  OpMatch R;
  // One argument against one parameter type: Phase 1 is "identical" (an
  // untyped constant: its default type), Phase 2 is the constant rule: the
  // constant can be represented by the parameter type.
  auto ArgMatch = [&](Type *P, Operand &A, int Phase) {
    if (!P || !isValid(P))
      return false;
    if (A.isNil())
      return Phase == 1 && hasNil(P);
    if (!A.Ty || !isValid(A.Ty))
      return false;
    if (isTyped(A.Ty))
      return Phase == 1 && identical(P, A.Ty);
    if (Phase == 1)
      return identical(P, defaultType(A.Ty));
    auto *B = dyn_cast<BasicType>(under(P));
    if (!B)
      return false;
    auto *AB = cast<BasicType>(A.Ty);
    if (AB->BK == BasicKind::UntypedBool)
      return (B->Info & IsBoolean) != 0;
    if (AB->BK == BasicKind::UntypedString)
      return (B->Info & IsString) != 0;
    if (!(B->Info & IsNumeric))
      return false;
    if (A.M != Mode::Constant)
      return false;
    Operand C = A;
    return constConvertibleTo(C, P, nullptr);
  };
  auto Matches = [&](Func *M, int Phase, bool &IsLazy) {
    auto *Sg = cast<SignatureType>(M->Ty);
    ArrayRef<Type *> Ps = Sg->Params->Elems;
    size_t NP = Ps.size(), NA = Args.size();
    // `a(xs...)`: only a variadic method takes it, and xs is its last
    // parameter's slice as it is.
    if (OpSpread && (!Sg->Variadic || NA != NP))
      return false;
    if (Sg->Variadic ? NA + 1 < NP : NA != NP)
      return false;
    IsLazy = false;
    for (size_t I = 0; I < NA; ++I) {
      Type *P = I < NP ? Ps[I] : Ps.back();
      if (Sg->Variadic && I + 1 >= NP && !OpSpread)
        P = cast<SliceType>(Ps.back())->Elem;
      Type *U;
      if (Lazy && I + 1 == NA && lazyParam(P, U)) {
        IsLazy = true;
        P = U;
      }
      if (!ArgMatch(P, *Args[I], Phase))
        return false;
    }
    return true;
  };
  for (int Phase = 1; Phase <= MaxPhase; ++Phase) {
    SmallVector<Func *, 2> Hit;
    SmallVector<bool, 2> HitLazy;
    for (Func *M : Cands) {
      bool IsLazy;
      if (Matches(M, Phase, IsLazy)) {
        Hit.push_back(M);
        HitLazy.push_back(IsLazy);
      }
    }
    // EXP-21: both forms for the same U: the lazy form is used.
    if (Lazy && Hit.size() == 2 && HitLazy[0] != HitLazy[1])
      Hit = {HitLazy[0] ? Hit[0] : Hit[1]};
    if (Hit.size() == 1) {
      R.M = Hit[0];
      return R;
    }
    if (Hit.size() > 1) {
      R.Ambiguous = true;
      R.Tied.assign(Hit.begin(), Hit.end());
      return R;
    }
  }
  return R;
}

//===----------------------------------------------------------------------===//
// The call an operator expression stands for
//===----------------------------------------------------------------------===//

const ast::Expr *Checker::synthExpr(SourceRange R) {
  auto *Id = Mem.make<ast::Ident>();
  Id->Begin = R.getBegin();
  Id->End = R.getEnd();
  Id->Name = Symbol(Mem.copy("_"));
  return Id;
}

const ast::CallExpr *Checker::synthCall(Func *M, const ast::Expr *RecvE,
                                        ArrayRef<const ast::Expr *> ArgEs,
                                        SourceRange R) {
  auto *Id = Mem.make<ast::Ident>();
  Id->Begin = R.getBegin();
  Id->End = R.getEnd();
  Id->Name = Symbol(M->Name);
  auto *Sel = Mem.make<SelectorExpr>();
  Sel->Begin = R.getBegin();
  Sel->End = R.getEnd();
  Sel->X = const_cast<ast::Expr *>(RecvE);
  Sel->Sel = Id;
  SmallVector<ast::Expr *, 4> Args;
  for (const ast::Expr *A : ArgEs)
    Args.push_back(const_cast<ast::Expr *>(A));
  auto *Call = Mem.make<CallExpr>();
  Call->Begin = R.getBegin();
  Call->End = R.getEnd();
  Call->Fun = Sel;
  Call->Lparen = R.getBegin();
  Call->Rparen = R.getEnd();
  Call->Args = Mem.copy(ArrayRef<ast::Expr *>(Args));
  Call->EllipsisLoc = PendingEllipsis; // `a(xs...)`
  PendingEllipsis = SourceLoc();
  return Call;
}

void Checker::recordOperator(const ast::Node *At, const OperatorUse &U) {
  LastOp = U;
  if (At)
    I.Operators[At] = U;
}

bool Checker::opReceiverOk(const ast::Expr *RecvE, Type *RcvTy, Func *F) {
  // A value receiver copies the operand, so on a place that may hold only
  // the parent part of a child (a tagged `*p` or value receiver, the parent
  // part `c.P`, the target of a pointer to a tagged type) the call is a
  // taken receiver: the decision of a method value, receiverWhy, not a
  // second rule. A pointer receiver borrows the place, as every call does.
  // An exact value is no partial place: an inherited operator runs on its
  // parent part, like `Dup()` (ADR-0023 decision 16).
  if (!own() || !RecvE || !RcvTy || !F || F->HasPtrRecv || !isValid(RcvTy))
    return true;
  PartialKind K = partialKindOf(RecvE, RcvTy);
  if (K == PartialKind::None && !pointeeOf(RcvTy))
    return true;
  std::string Core =
      receiverWhy(RcvTy, K, isa<StarExpr>(unparen(RecvE)), F, OwnerStep{},
                  /*Taken=*/true, [&] { return str(RecvE); });
  if (Core.empty())
    return true;
  err(RecvE, diag::err_partial_value,
      {str(RecvE), "as the receiver of the operator " + std::string(F->Name),
       Core + ", and the value receiver of an operator method is a copy, so "
              "it would cover only that part; use a method with a pointer "
              "receiver, or copy the whole value out first"});
  return false;
}

bool Checker::opCall(Operand &Out, const ast::Node *At, SourceRange R, Func *M,
                     const ast::Expr *RecvE, Operand &RecvOp,
                     ArrayRef<const ast::Expr *> ArgEs,
                     ArrayRef<Operand *> ArgOps, OperatorUse U,
                     const ast::CallExpr *Replace) {
  auto Fail = [&] {
    Out.M = Mode::Invalid;
    Out.Ty = invalid();
    return false;
  };
  std::string Sp(M->Name);
  Operand Recv = RecvOp;
  // A View[X] receiver (EXP-18, OWN-V1): a value of a pointer-free Copy X is
  // read out; any other view is no receiver, which is not a view.
  if (auto *RV = dyn_cast_or_null<RefType>(Recv.Ty); RV && !RV->Borrowed) {
    if (!copiesOut(RV->Elem)) {
      err(Recv, diag::err_op_no_method,
          {str(RecvE), str(Recv.Ty), quoted(Sp), "its operands",
           "; " + str(RV->Elem) +
               " is not a pointer-free Copy type, so a view of it cannot be "
               "an operand: the receiver and the parameters are not views "
               "(OWN-V1)"});
      return Fail();
    }
    Recv.Ty = RV->Elem;
    Recv.M = Mode::Value;
  }
  // A pointer receiver needs a place, or a pointer operand that is looked
  // through like `p.M()` (EXP-16).
  if (M->HasPtrRecv && !M->IsInterfaceMethod && Recv.M != Mode::Variable &&
      !(Recv.Ty && isa<PointerType>(under(Recv.Ty)))) {
    err(Recv.pos().isValid() ? Recv.pos() : R.getBegin(),
        diag::err_op_not_addressable, {str(RecvE), quoted(Sp), str(Recv.Ty)});
    return Fail();
  }
  if (!opReceiverOk(RecvE, Recv.Ty, M))
    return Fail();
  const ast::CallExpr *Call = synthCall(M, RecvE, ArgEs, R);
  auto *Sel = cast<SelectorExpr>(Call->Fun);
  OpSelect[Sel] = M;
  PreEval[RecvE] = Recv;
  for (size_t Ix = 0; Ix < ArgEs.size(); ++Ix)
    if (Ix < ArgOps.size() && ArgOps[Ix])
      PreEval[ArgEs[Ix]] = *ArgOps[Ix];
  const CallExpr *SavedGo = GoCall, *SavedDefer = DeferCall;
  if (Replace) {
    if (GoCall == Replace)
      GoCall = Call;
    if (DeferCall == Replace)
      DeferCall = Call;
  }
  Operand Res;
  callExpr(Res, Call);
  GoCall = SavedGo;
  DeferCall = SavedDefer;
  OpSelect.erase(Sel);
  PreEval.erase(RecvE);
  for (const ast::Expr *A : ArgEs)
    PreEval.erase(A);
  if (Res.M == Mode::Invalid)
    return Fail();
  U.Method = M;
  U.Call = Call;
  U.ViaConstraint = M->IsInterfaceMethod;
  U.Inherited = inherited(M, opBase(Recv.Ty, /*ThroughPtr=*/true));
  recordOperator(At, U);
  Out = Res;
  return true;
}

//===----------------------------------------------------------------------===//
// Binary and unary operators (EXP-17 to EXP-21, EXP-26)
//===----------------------------------------------------------------------===//

/// What `x == y` means when no `==` method serves it: true for the built-in
/// comparison (which still checks comparability), false for "no operator"
/// (E0411). `!=` and the cases of a switch take this path too, and so does a
/// constraint element `== (T) bool` met by a type's built-in `==`. The one
/// rule table, for plain and generic code; the first row that fits decides:
///
///  (e) an interface operand (not a type parameter) on either side: the
///      built-in comparison with the dynamic value (EXP-17 rule 6,
///      06-expressions.md:171: "`a == i` ... as `i == a` is").
///  (c) a type with an `==` method, own or inherited, that is no defined
///      number or string type: the method only (EXP-20, 06-expressions.md:201:
///      "A type with an `==` method uses the method in `==`, `!=`, `switch`
///      cases and everywhere else"; EXP-30, 06-expressions.md:276: an
///      inherited `==` "hides the built-in struct equality of the child").
///  (d) a type parameter whose constraint lists `==` (an element, or a method
///      of its `extends` parent): the built-in when its type set is comparable
///      and the other operand is of that type (GEN-13, 10-generics.md:82:
///      "`comparable` still means built-in equality"; EXP-20: "`comparable`
///      instances still use built-in equality, never the method").
///  (b) a defined number or string type with `==` methods for other types:
///      the built-in for two operands of that type (EXP-17 rule 6,
///      06-expressions.md:171).
///  (a) no `==` method on either side: the built-in (EXP-20,
///      06-expressions.md:201: "A type with no `==` method keeps the built-in
///      `==` if it has one").
///
/// (d) against (c) is the one intended difference between generic and plain
/// code: `[T interface{ comparable; == (int) bool }]` has `a == b`, a
/// comparable struct whose only `==` is `== (int)` has no `v == w`.
bool Checker::eqKeepsBuiltin(const Operand &X, const Operand &Y) {
  enum class Row { A, B, C, D, E };
  auto RowOf = [&](Type *T) {
    if (!T)
      return Row::A;
    if (!isTypeParam(T) && isa<InterfaceType>(under(T)))
      return Row::E;
    if (!hasOpKind(opBase(T), OpKind::Eq))
      return Row::A;
    if (isTypeParam(T))
      return Row::D;
    return isNumeric(T) || isString(T) ? Row::B : Row::C;
  };
  const Row RX = RowOf(X.Ty), RY = RowOf(Y.Ty);
  auto Has = [&](Row R) { return RX == R || RY == R; };
  // The other operand is of the type T itself: T, or a constant T holds.
  auto OfOwnType = [&](Row R) {
    const Operand &T = RX == R ? X : Y, &O = RX == R ? Y : X;
    if (identical(T.Ty, O.Ty))
      return true;
    Operand C = O;
    return !isTyped(O.Ty) &&
           implicitTypeAndValue(C, T.Ty, nullptr, nullptr) != nullptr;
  };
  if (Has(Row::E))
    return true;
  if (Has(Row::C))
    return false;
  if (Has(Row::D)) {
    const Operand &T = RX == Row::D ? X : Y;
    return comparableType(T.Ty, false, nullptr) && OfOwnType(Row::D);
  }
  if (Has(Row::B))
    return OfOwnType(Row::B);
  return true;
}

bool Checker::opBinary(Operand &X, Operand &Y, const ast::Expr *Ex,
                       const ast::Expr *L, const ast::Expr *R, tok::Kind Op,
                       SourceLoc OpLoc) {
  (void)OpLoc;
  if (!opCandidateType(X.Ty) && !opCandidateType(Y.Ty))
    return false;
  if (X.isNil() || Y.isNil())
    return false;
  const OpKind K = binaryKindOf(Op);
  if (K == OpKind::None)
    return false;
  const bool IsEq = K == OpKind::Eq;
  const bool IsOrd =
      Op == tok::lss || Op == tok::leq || Op == tok::gtr || Op == tok::geq;
  const bool Logic = K == OpKind::LAnd || K == OpKind::LOr;
  StringRef Shown = tok::getSpelling(Op);
  // A View[X] operand whose X has operator methods: a pointer-free Copy X is
  // read out of the view; any other X cannot be passed (EXP-24, OWN-V1).
  for (Operand *O : {&X, &Y}) {
    auto *RV = dyn_cast_or_null<RefType>(O->Ty);
    if (!RV || RV->Borrowed || !opCandidateType(RV->Elem))
      continue;
    if (!hasOpKind(opBase(RV->Elem), K))
      continue;
    if (copiesOut(RV->Elem)) {
      O->Ty = RV->Elem;
      O->M = Mode::Value;
      continue;
    }
    err(*O, diag::err_op_no_method,
        {Ex ? str(Ex) : str(L) + " " + Shown.str() + " " + str(R), str(X.Ty),
         "`" + Shown.str() + "`", opArgString(Y),
         "; " + str(RV->Elem) +
             " is not a pointer-free Copy type, so a view of it cannot be an "
             "operand: the receiver and the parameters are not views "
             "(OWN-V1)"});
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  const bool LU = opCandidateType(X.Ty), RU = opCandidateType(Y.Ty);
  if (!LU && !RU)
    return false;
  std::string ExprS = Ex ? str(Ex) : str(L) + " " + Shown.str() + " " + str(R);
  SourceRange Range =
      Ex ? Ex->getRange() : SourceRange(L->getBegin(), R->getEnd());
  auto NoMethod = [&](ArrayRef<Func *> Have, const std::string &Hint) {
    Err Er = err(X, diag::err_op_no_method,
                 {ExprS, str(X.Ty), "`" + Shown.str() + "`", opArgString(Y),
                  Hint.empty() ? "" : "; " + Hint});
    if (!Have.empty())
      Er.note(X.pos(), "the operator methods of " + str(X.Ty) + ": " +
                           opSetString(Have));
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  };

  // The methods that can serve `x op y`: those of x's type (a @Right method
  // of a non-commutative operator serves the swapped order only), and the
  // @Right methods of y's type for `x op y` with the order swapped (EXP-26).
  SmallVector<Func *, 4> D, W, AllL, AllR;
  if (LU) {
    opCandidates(opBase(X.Ty), K, AllL);
    for (Func *M : AllL)
      if (paramCount(M) == 1 && (!M->Right || isCommutativeOp(K)))
        D.push_back(M);
  }
  if (RU) {
    opCandidates(opBase(Y.Ty), K, AllR);
    for (Func *M : AllR)
      if (paramCount(M) == 1 && M->Right)
        W.push_back(M);
  }
  auto *XN = dyn_cast<NamedType>(X.Ty);
  const bool UserLeft = hasNoBuiltinOps(X.Ty);

  // A defined type over a number or string next to an untyped constant that
  // it can represent has the built-in operator too (`10 * Km` for
  // `type Meters float64`). A method wins only by an exact match, the
  // constant's default type; the weaker "can represent" match never takes
  // an expression away from the built-in meaning.
  {
    auto BuiltinFits = [&](const Operand &User, const Operand &Other) {
      auto *UN = dyn_cast_or_null<NamedType>(User.Ty);
      if (!UN || !isa<BasicType>(under(UN)) || !Other.Ty || isTyped(Other.Ty) ||
          Other.isNil() || Other.M != Mode::Constant)
        return false;
      Operand C = Other;
      return constConvertibleTo(C, UN, nullptr);
    };
    if ((LU && !RU && BuiltinFits(X, Y)) || (RU && !LU && BuiltinFits(Y, X))) {
      OpMatch D1, W1;
      if (!D.empty())
        D1 = pickOverload(D, {&Y}, Logic, 1);
      if (!W.empty() && !Logic)
        W1 = pickOverload(W, {&X}, false, 1);
      if (!D1.M && !W1.M && !D1.Ambiguous && !W1.Ambiguous)
        return false;
    }
  }

  if (D.empty() && W.empty()) {
    // `==` is commutative, so x has no `==` methods here: if the table says
    // no, they are y's, written for y on the left.
    if (IsEq) {
      if (eqKeepsBuiltin(X, Y))
        return false;
      return NoMethod({}, isa<TypeParamType>(Y.Ty)
                              ? "the `==` elements of the constraint of " +
                                    str(Y.Ty) +
                                    " take it as the left operand (GEN-13)"
                              : "the `==` methods of " + str(Y.Ty) +
                                    " take it as the left operand: for this "
                                    "order mark one with @Right (EXP-26)");
    }
    if (Logic) {
      if (!LU || allBoolean(X.Ty) || !UserLeft)
        return false;
      return NoMethod(AllL, isa<TypeParamType>(X.Ty)
                                ? "list `" + Shown.str() +
                                      " (b U) R` in the constraint (GEN-13)"
                                : "define it as a method: `func (a " +
                                      str(X.Ty) + ") " + Shown.str() +
                                      " (b U) R` (EXP-21)");
    }
    // The other order exists, but written for `a op s` only.
    if (RU) {
      SmallVector<Func *, 4> Plain;
      for (Func *M : AllR)
        if (paramCount(M) == 1 && !M->Right)
          Plain.push_back(M);
      OpMatch PM = pickOverload(Plain, {&X});
      if (PM.M)
        return NoMethod(
            AllR,
            "write the method for the swapped order: mark the method of " +
                str(Y.Ty) + " with @Right (EXP-26)");
    }
    // A constant on the left of an operator that the right operand's type
    // has only for itself on the left (`3 < x`): the built-in conversion
    // error would name the underlying type (EXP-17 rule 5, EXP-26).
    if (RU && !LU && !Logic && !AllR.empty() && !isTyped(X.Ty) && !X.isNil()) {
      auto *YN = dyn_cast<NamedType>(Y.Ty);
      if (YN && !isa<BasicType>(under(YN)) && !isa<InterfaceType>(under(YN))) {
        Err Er = err(Y, diag::err_op_no_method,
                     {ExprS, opArgString(X), "`" + Shown.str() + "`", str(Y.Ty),
                      "; the `" + Shown.str() + "` methods of " + str(Y.Ty) +
                          " take it as the left operand: for a constant on "
                          "the left write a @Right method for the swapped "
                          "order (EXP-26)"});
        Er.note(Y.pos(), "the operator methods of " + str(Y.Ty) + ": " +
                             opSetString(AllR));
        X.M = Mode::Invalid;
        X.Ty = invalid();
        return true;
      }
    }
    // Only @Right methods of a non-commutative operator: each is written
    // for the swapped order and serves `s op a` only (EXP-26).
    if (LU && !AllL.empty() && !isCommutativeOp(K) &&
        llvm::all_of(AllL, [](Func *M) { return M->Right; }))
      return NoMethod(AllL,
                      "the `" + Shown.str() + "` methods of " + str(X.Ty) +
                          " are marked @Right: each is written for the "
                          "swapped order and serves `s " +
                          Shown.str() + " a` only; for `a " + Shown.str() +
                          " s` write a second method without @Right "
                          "(EXP-26)");
    if (K == OpKind::Cmp) {
      if (LU && UserLeft)
        return NoMethod(
            AllL, std::string(isa<TypeParamType>(X.Ty) ? "list `<=> (b "
                                                       : "define `<=> (b ") +
                      str(Y.isNil() ? X.Ty : Y.Ty) +
                      (isa<TypeParamType>(X.Ty) ? ") int` in the "
                                                  "constraint (GEN-13)"
                                                : ") int`") +
                      ": `<`, `<=`, `>` and `>=` are made from it "
                      "(EXP-20)");
      return false;
    }
    if (LU && UserLeft)
      return NoMethod(AllL, isa<TypeParamType>(X.Ty)
                                ? "list `" + Shown.str() +
                                      " (b U) R` in the constraint of " +
                                      str(X.Ty) + " (GEN-13)"
                                : "");
    return false;
  }

  OpMatch DM, WM;
  if (!D.empty())
    DM = pickOverload(D, {&Y}, Logic);
  if (!W.empty() && !Logic)
    WM = pickOverload(W, {&X});
  auto Ambig = [&](const OpMatch &M, const Operand &Arg) {
    err(X, diag::err_op_ambiguous,
        {"`" + Shown.str() + "`", ExprS,
         "the " + opArgString(Arg) + " argument fits " + opSetString(M.Tied) +
             "; write the argument with an explicit type"});
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  };
  if (DM.Ambiguous)
    return Ambig(DM, Y);
  if (WM.Ambiguous)
    return Ambig(WM, X);
  if (DM.M && WM.M && DM.M != WM.M) {
    err(X, diag::err_op_ambiguous,
        {"`" + Shown.str() + "`", ExprS,
         str(X.Ty) + " has " + funcString(DM.M) + " and " + str(Y.Ty) +
             " has the @Right method " + funcString(WM.M) +
             ": the use fits both"});
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  if (!DM.M && !WM.M) {
    // `==`: the rule table (eqKeepsBuiltin). Any other operator of two
    // operands of the same defined type over a basic type is built in, next
    // to the methods for other right-hand types (`Meters * Meters` and
    // `Meters * Seconds`, EXP-17 rule 6).
    if (IsEq ? eqKeepsBuiltin(X, Y)
             : X.Ty && Y.Ty && isTyped(X.Ty) && identical(X.Ty, Y.Ty) &&
                   isa<BasicType>(under(X.Ty)))
      return false;
    std::string Hint;
    // The parent-type hint applies only if the right operand is itself a
    // child of the parameter type of an inherited method.
    auto ChildOfParam = [&] {
      auto *YN = dyn_cast_or_null<NamedType>(Y.Ty);
      if (!YN || !UserLeft)
        return false;
      for (Func *M : D.empty() ? AllL : D) {
        if (!inherited(M, XN) || paramCount(M) != 1)
          continue;
        Type *P = cast<SignatureType>(M->Ty)->Params->Elems[0];
        if (!isa<NamedType>(P))
          continue;
        llvm::SmallPtrSet<NamedType *, 8> Seen;
        for (NamedType *A = YN; A && Seen.insert(A).second; A = parentOf(A))
          if (identical(A, P))
            return true;
      }
      return false;
    };
    if (ChildOfParam())
      Hint =
          "the operator methods are inherited from the parent and take the "
          "parent type, and a " +
          str(X.Ty) +
          " value is not a parent operand (EXP-30): define the operator on " +
          str(X.Ty) + ", or use the parent parts";
    else if (IsEq && identical(X.Ty, Y.Ty))
      Hint = isa<TypeParamType>(X.Ty)
                 ? "the type set of " + str(X.Ty) +
                       " is not comparable, so `==` is the constraint's "
                       "elements only: list `comparable` or `== (" +
                       str(X.Ty) + ") bool` (GEN-13)"
                 : str(X.Ty) +
                       " has `==` methods, so it has no built-in "
                       "`==` (EXP-20): define `== (b " +
                       str(X.Ty) + ") bool`";
    else
      Hint = "the right operand must have exactly the parameter type "
             "(no implicit conversion)";
    SmallVector<Func *, 4> Have(D.begin(), D.end());
    Have.append(W.begin(), W.end());
    return NoMethod(Have, Hint);
  }
  // Resolved.
  const bool Swap = !DM.M;
  Func *M = Swap ? WM.M : DM.M;
  OperatorUse U;
  U.Op = K;
  U.Swapped = Swap;
  U.Negate = Op == tok::neq;
  U.Sign = IsOrd ? Op : tok::illegal;
  Operand Arg = Swap ? X : Y;
  if (Logic) {
    Type *Res;
    U.Lazy = lazyParam(cast<SignatureType>(M->Ty)->Params->Elems[0], Res);
    if (U.Lazy) {
      // The method gets the right operand as a closure (EXP-21); the
      // lowering wraps it. The operand is a value of U here, evaluated in
      // the context of the expression.
      // A constant operand takes the result type of the closure, as the
      // eager form takes the parameter type.
      convertUntyped(Arg, Res);
      if (Arg.M == Mode::Invalid) {
        X.M = Mode::Invalid;
        X.Ty = invalid();
        return true;
      }
      Arg.Ty = cast<SignatureType>(M->Ty)->Params->Elems[0];
      Arg.M = Mode::Value;
    }
  }
  Operand Out;
  const ast::Expr *RecvE = Swap ? R : L;
  const ast::Expr *ArgE = Swap ? L : R;
  Operand &RecvOp = Swap ? Y : X;
  if (!opCall(Out, Ex, Range, M, RecvE, RecvOp, {ArgE}, {&Arg}, U)) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  X.M = Mode::Value;
  X.E = Ex ? Ex : X.E;
  if (IsEq || IsOrd) {
    X.Ty = typ(BasicKind::UntypedBool);
  } else {
    X.Ty = Out.Ty;
    X.M = Out.M == Mode::NoValue ? Mode::NoValue : Mode::Value;
  }
  return true;
}

bool Checker::opUnary(Operand &X, const UnaryExpr *Ex) {
  if (!opCandidateType(X.Ty))
    return false;
  OpKind K = OpKind::None, Bin = OpKind::None;
  switch (Ex->Op) {
  case tok::add:
    K = OpKind::Pos, Bin = OpKind::Add;
    break;
  case tok::sub:
    K = OpKind::Neg, Bin = OpKind::Sub;
    break;
  case tok::xor_:
    K = OpKind::Compl, Bin = OpKind::Xor;
    break;
  case tok::not_:
    K = OpKind::Not;
    break;
  default:
    return false;
  }
  Type *T = opBase(X.Ty);
  SmallVector<Func *, 4> All, C;
  opCandidates(T, K, All);
  for (Func *M : All)
    if (paramCount(M) == 0 && !M->Right)
      C.push_back(M);
  if (C.empty()) {
    if (hasNoBuiltinOps(X.Ty)) {
      StringRef Sp = tok::getSpelling(Ex->Op);
      Err Er = err(X, diag::err_op_no_unary,
                   {str(Ex), str(X.Ty), "`" + Sp.str() + "`",
                    isa<TypeParamType>(X.Ty)
                        ? "; list `" + Sp.str() +
                              " () R` in the constraint "
                              "(GEN-13)"
                        : "; define it as a method: `func (a " + str(X.Ty) +
                              ") " + Sp.str() + " () R` (EXP-16)"});
      if (Bin != OpKind::None)
        opCandidates(T, Bin, All);
      if (!All.empty())
        Er.note(X.pos(), "the operator methods of " + str(X.Ty) + ": " +
                             opSetString(All));
      X.M = Mode::Invalid;
      X.Ty = invalid();
      return true;
    }
    return false;
  }
  OperatorUse U;
  U.Op = K;
  Operand Out;
  if (!opCall(Out, Ex, Ex->getRange(), C[0], Ex->X, X, {}, {}, U)) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  X.M = Mode::Value;
  X.Ty = Out.Ty;
  X.E = Ex;
  return true;
}

void Checker::builtinSpaceship(Operand &X, Operand &Y, const ast::Expr *Ex) {
  // EXP-20: `<=>` is built in for numbers and strings, as cmp.Compare. The
  // operands have the same type; the result is an int that is not constant
  // (NaN is less than every other value and equal to itself).
  if (!isValid(X.Ty) || !isValid(Y.Ty)) {
    X.M = Mode::Invalid;
    return;
  }
  auto Ordered = [&](Type *T) {
    // A type parameter whose every term is a number or a string has the
    // built-in operator as in plain code, as `<` does (GEN-13).
    return isTypeParam(T) ? allOrdered(T)
                          : isInteger(T) || isFloat(T) || isString(T);
  };
  if (!Ordered(X.Ty) || !Ordered(Y.Ty) || !identical(X.Ty, Y.Ty)) {
    if (isTyped(X.Ty) && isTyped(Y.Ty) && !identical(X.Ty, Y.Ty))
      err(X, diag::err_mismatched_types, {str(Ex), str(X.Ty), str(Y.Ty)});
    else
      err(X, diag::err_operator_not_defined, {"<=>", str(X)});
    X.M = Mode::Invalid;
    return;
  }
  if (!isTyped(X.Ty)) {
    Type *D = defaultType(X.Ty);
    convertUntyped(X, D);
    convertUntyped(Y, D);
    if (X.M == Mode::Invalid || Y.M == Mode::Invalid) {
      X.M = Mode::Invalid;
      return;
    }
  }
  if (Ex) {
    OperatorUse U;
    U.Op = OpKind::Cmp;
    U.Builtin = true;
    recordOperator(Ex, U);
  }
  X.M = Mode::Value;
  X.Ty = typ(BasicKind::Int);
  X.E = Ex;
}

bool Checker::opRecv(Operand &X, const UnaryExpr *Ex) {
  Type *T = opBase(X.Ty, /*ThroughPtr=*/true);
  SmallVector<Func *, 4> C;
  opCandidates(T, OpKind::Recv, C);
  if (C.empty()) {
    // A user type with a send but no receive: the receive is not defined.
    if (hasOpKind(T, OpKind::Send)) {
      err(X, diag::err_op_no_unary,
          {str(Ex), str(X.Ty), "`<-`", "; the type has a send only (CON-22)"});
      X.M = Mode::Invalid;
      X.Ty = invalid();
      return true;
    }
    return false;
  }
  if (SelectComm == Ex) {
    err(Ex, diag::err_op_select, {str(Ex), "a receive"});
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  OperatorUse U;
  U.Op = OpKind::Recv;
  Operand Out;
  if (!opCall(Out, Ex, Ex->getRange(), C[0], Ex->X, X, {}, {}, U)) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  E.HasCallOrRecv = true;
  X.E = Ex;
  if (auto *TT = dyn_cast<TupleType>(Out.Ty)) {
    X.M = Mode::CommaOk; // `v, ok := <-q` (CON-22)
    X.Ty = TT->Elems[0];
  } else {
    X.M = Mode::Value;
    X.Ty = Out.Ty;
  }
  return true;
}

bool Checker::opDeref(Operand &X, const StarExpr *Ex) {
  SmallVector<Func *, 4> C;
  opCandidates(opBase(X.Ty), OpKind::Deref, C);
  if (C.empty())
    return false;
  OperatorUse U;
  U.Op = OpKind::Deref;
  Operand Out;
  if (!opCall(Out, Ex, Ex->getRange(), C[0], Ex->X, X, {}, {}, U)) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  X.M = Mode::Value; // read only: not a place (EXP-24)
  X.Ty = Out.Ty;
  X.E = Ex;
  return true;
}

//===----------------------------------------------------------------------===//
// Index, call, conversion (EXP-22, EXP-23, EXP-25)
//===----------------------------------------------------------------------===//

namespace {
/// "int, string" for an argument list, in messages.
std::string argList(Checker &C, ArrayRef<Operand *> Args) {
  std::string R;
  for (Operand *A : Args)
    R += (R.empty() ? "" : ", ") + C.opArgString(*A);
  return R.empty() ? "no arguments" : R;
}
} // namespace

bool Checker::opIndex(Operand &X, const IndexExpr *Ex) {
  SmallVector<Func *, 4> All;
  opCandidates(opBase(X.Ty, /*ThroughPtr=*/true), OpKind::Index, All);
  if (All.empty())
    return false;
  SmallVector<Operand, 4> Idx(Ex->Indices.size());
  SmallVector<Operand *, 4> Ptrs;
  SmallVector<const ast::Expr *, 4> Es;
  bool Bad = false;
  for (size_t K = 0; K < Ex->Indices.size(); ++K) {
    expr(Idx[K], Ex->Indices[K]);
    Bad |= Idx[K].M == Mode::Invalid;
    Ptrs.push_back(&Idx[K]);
    Es.push_back(Ex->Indices[K]);
  }
  if (Bad) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  OpMatch M = pickOverload(All, Ptrs);
  if (M.Ambiguous || !M.M) {
    if (M.Ambiguous)
      err(Ex, diag::err_op_ambiguous,
          {"`[]`", str(Ex),
           "the index fits " + opSetString(M.Tied) +
               "; write the index with an explicit type"});
    else
      err(Ex, diag::err_op_no_method,
          {str(Ex), str(X.Ty), "`[]`", argList(*this, Ptrs),
           "; the operator methods of " + str(X.Ty) + ": " + opSetString(All)});
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  OperatorUse U;
  U.Op = OpKind::Index;
  U.Index = M.M;
  Operand Out;
  if (!opCall(Out, Ex, Ex->getRange(), M.M, Ex->X, X, Es, Ptrs, U)) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  X.M = Mode::Value; // not a place (EXP-22)
  X.Ty = Out.Ty;
  X.E = Ex;
  return true;
}

/// The first part of `a(args)` on an operator type: is there a `()` method,
/// and is it the spread form of one method. NoInline with its locals: the
/// frame of opCallExpr, which stays on the stack while the arguments are
/// checked (nested calls recurse through it), holds only the arguments.
__attribute__((noinline)) int Checker::opCallPrep(Operand &X,
                                                  const CallExpr *Call) {
  SmallVector<Func *, 4> All;
  opCandidates(opBase(X.Ty, /*ThroughPtr=*/true), OpKind::Call, All);
  if (All.empty() || Call->isMust())
    return 0;
  OperatorUse U;
  U.Op = OpKind::Call;
  // `a(xs...)` and `a(f())` (one call giving several values) with one `()`
  // method: the arguments are checked by the call itself, as for any method.
  // The single call argument is evaluated by opCallExpr first (once, through
  // PreEval), so a nest of calls recurses through one small frame and not
  // through the synthesized call.
  const bool Spread = Call->EllipsisLoc.isValid();
  if (All.size() == 1 && !Spread && Call->Args.size() == 1 &&
      isa<CallExpr>(unparen(Call->Args[0])))
    return 3;
  if (All.size() == 1 && Spread) {
    Operand Out;
    SmallVector<const ast::Expr *, 4> Es(Call->Args.begin(), Call->Args.end());
    PendingEllipsis = Call->EllipsisLoc;
    if (!opCall(Out, Call, Call->getRange(), All[0], Call->Fun, X, Es, {}, U,
                Call)) {
      X.M = Mode::Invalid;
      X.Ty = invalid();
      return 2;
    }
    E.HasCallOrRecv = true;
    X = Out;
    X.E = Call;
    return 2;
  }
  return 1;
}

/// `a(f())` with one `()` method: the argument is in PreEval, and may give
/// several values.
__attribute__((noinline)) void Checker::opCallOne(Operand &X,
                                                  const CallExpr *Call) {
  SmallVector<Func *, 4> All;
  opCandidates(opBase(X.Ty, /*ThroughPtr=*/true), OpKind::Call, All);
  OperatorUse U;
  U.Op = OpKind::Call;
  Operand Out;
  SmallVector<const ast::Expr *, 4> Es(Call->Args.begin(), Call->Args.end());
  if (!opCall(Out, Call, Call->getRange(), All[0], Call->Fun, X, Es, {}, U,
              Call)) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return;
  }
  E.HasCallOrRecv = true;
  X = Out;
  X.E = Call;
}

bool Checker::opCallExpr(Operand &X, const CallExpr *Call, bool &Handled) {
  Handled = false;
  int Prep = opCallPrep(X, Call);
  if (Prep == 0)
    return false;
  Handled = true;
  if (Prep == 2)
    return true;
  if (Prep == 3) {
    Operand A;
    const ast::Expr *Arg = Call->Args[0];
    rawExpr(A, Arg, nullptr, true);
    PreEval[Arg] = std::move(A);
    opCallOne(X, Call);
    PreEval.erase(Arg);
    return true;
  }
  SmallVector<Operand, 4> Args;
  genericExprList(Call->Args, Args);
  opCallFinish(X, Call, Args);
  return true;
}

__attribute__((noinline)) void
Checker::opCallFinish(Operand &X, const CallExpr *Call,
                      SmallVector<Operand, 4> &Args) {
  SmallVector<Func *, 4> All;
  opCandidates(opBase(X.Ty, /*ThroughPtr=*/true), OpKind::Call, All);
  OperatorUse U;
  U.Op = OpKind::Call;
  SmallVector<Operand *, 4> Ptrs;
  SmallVector<const ast::Expr *, 4> Es(Call->Args.begin(), Call->Args.end());
  bool Bad = Args.size() != Call->Args.size();
  for (Operand &A : Args) {
    Bad |= A.M == Mode::Invalid;
    Ptrs.push_back(&A);
  }
  if (Bad) {
    if (Args.size() != Call->Args.size() && !Args.empty())
      err(Call, diag::err_op_no_method,
          {str(Call), str(X.Ty), "`()`", "a spread call",
           "; write the arguments one by one"});
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return;
  }
  // One method of the arity: the call rules of any call apply (EXP-23).
  Func *Pick = nullptr;
  if (All.size() == 1) {
    Pick = All[0];
  } else {
    OpSpread = Call->EllipsisLoc.isValid();
    OpMatch M = pickOverload(All, Ptrs);
    OpSpread = false;
    if (M.Ambiguous) {
      err(Call, diag::err_op_ambiguous,
          {"`()`", str(Call), "the arguments fit " + opSetString(M.Tied)});
      X.M = Mode::Invalid;
      X.Ty = invalid();
      return;
    }
    Pick = M.M;
  }
  if (!Pick) {
    err(Call, diag::err_op_no_method,
        {str(Call), str(X.Ty), "`()`", argList(*this, Ptrs),
         "; the operator methods of " + str(X.Ty) + ": " + opSetString(All)});
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return;
  }
  Operand Out;
  PendingEllipsis = Call->EllipsisLoc;
  if (!opCall(Out, Call, Call->getRange(), Pick, Call->Fun, X, Es, Ptrs, U,
              Call)) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return;
  }
  E.HasCallOrRecv = true;
  X = Out;
  X.E = Call;
}

Func *Checker::convertMethod(NamedType *N, Type *T) {
  // The @Convert methods of the type and its ancestors whose result is T.
  if (!N || !T)
    return nullptr;
  SmallVector<Func *, 2> Cs;
  opCandidates(opBase(N), OpKind::Convert, Cs);
  for (Func *M : Cs) {
    auto *Sg = M->sig();
    if (Sg->Params->size() == 0 && Sg->Results->size() == 1 &&
        identical(Sg->Results->Elems[0], T))
      return M;
  }
  return nullptr;
}

bool Checker::opConvert(Operand &X, Type *T, const ast::Expr *At,
                        bool &Handled) {
  Handled = false;
  if (!opCandidateType(X.Ty) || X.M == Mode::TypeExpr)
    return false;
  auto *N = dyn_cast<NamedType>(X.Ty);
  if (!N)
    return false;
  Func *Found = convertMethod(N, T);
  if (!Found)
    return false;
  Handled = true;
  const ast::Expr *Arg = X.E;
  OperatorUse U;
  U.Op = OpKind::Convert;
  Operand Out;
  if (!opCall(Out, At, SourceRange(Arg->getBegin(), Arg->getEnd()), Found, Arg,
              X, {}, {}, U)) {
    X.M = Mode::Invalid;
    X.Ty = invalid();
    return true;
  }
  X.M = Mode::Value;
  X.Ty = Out.Ty;
  return true;
}

//===----------------------------------------------------------------------===//
// Statements: ++/--, send, range, `a[i] = v`, compound forms
//===----------------------------------------------------------------------===//

/// The token of the binary operator of `x op= y`.
static tok::Kind compoundTok(tok::Kind K) {
  switch (K) {
  case tok::add_assign:
    return tok::add;
  case tok::sub_assign:
    return tok::sub;
  case tok::mul_assign:
    return tok::mul;
  case tok::quo_assign:
    return tok::quo;
  case tok::rem_assign:
    return tok::rem;
  case tok::and_assign:
    return tok::and_;
  case tok::or_assign:
    return tok::or_;
  case tok::xor_assign:
    return tok::xor_;
  case tok::shl_assign:
    return tok::shl;
  case tok::shr_assign:
    return tok::shr;
  case tok::and_not_assign:
    return tok::and_not;
  default:
    return tok::eof;
  }
}

bool Checker::opIncDec(const ast::IncDecStmt *ID) {
  Operand X;
  // The index form is no place (EXP-22): `a[i]++` is an error.
  if (auto *Ix = dyn_cast<IndexExpr>(unparen(ID->X))) {
    Operand B;
    exprOrType(B, Ix->X, true);
    PreEval[Ix->X] = B;
    if (B.M != Mode::Invalid && B.M != Mode::TypeExpr &&
        hasOpKind(opBase(B.Ty, /*ThroughPtr=*/true), OpKind::Index)) {
      expr(X, ID->X);
      if (X.M != Mode::Invalid)
        err(ID->X, diag::err_op_index_not_place,
            {str(ID->X), std::string(ID->Op == tok::inc ? "++" : "--"),
             "write `" + str(ID->X) +
                 (ID->Op == tok::inc ? " += 1`" : " -= 1`")});
      return true;
    }
  }
  // The receiver of a `++` method is the place itself (EXP-19).
  markPartialOk(ID->X, /*Addr=*/false);
  expr(X, ID->X);
  if (X.M == Mode::Invalid)
    return true;
  const OpKind K = ID->Op == tok::inc ? OpKind::Inc : OpKind::Dec;
  SmallVector<Func *, 4> All;
  opCandidates(opBase(X.Ty), K, All);
  if (All.empty()) {
    PreEval[ID->X] = X;
    return false;
  }
  OperatorUse U;
  U.Op = K;
  Operand Out;
  opCall(Out, ID, ID->getRange(), All[0], ID->X, X, {}, {}, U);
  return true;
}

bool Checker::opSend(const ast::SendStmt *SS) {
  Operand Ch, Val;
  markPartialOk(SS->Chan, /*Addr=*/false);
  expr(Ch, SS->Chan);
  SmallVector<Func *, 4> C;
  if (Ch.M != Mode::Invalid)
    opCandidates(opBase(Ch.Ty, /*ThroughPtr=*/true), OpKind::Send, C);
  if (C.empty()) {
    PreEval[SS->Chan] = Ch;
    return false;
  }
  if (SelectComm == SS) {
    expr(Val, SS->Value);
    err(SS->ArrowLoc, diag::err_op_select,
        {str(SS->Chan) + " <- " + str(SS->Value), "a send"});
    return true;
  }
  expr(Val, SS->Value);
  if (Val.M == Mode::Invalid)
    return true;
  SmallVector<Operand *, 1> Ptrs = {&Val};
  OpMatch M = pickOverload(C, Ptrs);
  if (M.Ambiguous || !M.M) {
    if (M.Ambiguous)
      err(SS, diag::err_op_ambiguous,
          {"`<-`", str(SS->Chan) + " <- " + str(SS->Value),
           "the value fits " + opSetString(M.Tied)});
    else
      err(SS, diag::err_op_no_method,
          {str(SS->Chan) + " <- " + str(SS->Value), str(Ch.Ty), "`<-` (send)",
           argList(*this, Ptrs),
           "; the operator methods of " + str(Ch.Ty) + ": " + opSetString(C)});
    return true;
  }
  OperatorUse U;
  U.Op = OpKind::Send;
  Operand Out;
  opCall(Out, SS, SS->getRange(), M.M, SS->Chan, Ch, {SS->Value}, Ptrs, U);
  return true;
}

Type *Checker::opRangeType(Operand &X, const ast::RangeStmt *RS,
                           Func **Method) {
  SmallVector<Func *, 4> C;
  opCandidates(opBase(X.Ty, /*ThroughPtr=*/true), OpKind::Range, C);
  if (C.empty())
    return nullptr;
  OperatorUse U;
  U.Op = OpKind::Range;
  Operand Out;
  if (!opCall(Out, RS, RS->X->getRange(), C[0], RS->X, X, {}, {}, U))
    return invalid();
  if (Method)
    *Method = C[0];
  return Out.Ty;
}

bool Checker::opIndexAssign(const ast::Expr *Lhs, const ast::Expr *RhsExpr,
                            Operand *X) {
  auto *Ix = dyn_cast<IndexExpr>(unparen(Lhs));
  if (!Ix)
    return false;
  Operand B;
  markPartialOk(Ix->X, /*Addr=*/false); // the receiver of `[]=`
  exprOrType(B, Ix->X, true);
  SmallVector<Func *, 4> Writes;
  if (B.M != Mode::Invalid && B.M != Mode::TypeExpr)
    opCandidates(opBase(B.Ty, /*ThroughPtr=*/true), OpKind::IndexSet, Writes);
  if (Writes.empty()) {
    PreEval[Ix->X] = B;
    return false;
  }
  SmallVector<Operand, 4> Idx(Ix->Indices.size() + 1);
  SmallVector<Operand *, 4> Ptrs;
  SmallVector<const ast::Expr *, 4> Es;
  bool Bad = false;
  for (size_t K = 0; K < Ix->Indices.size(); ++K) {
    expr(Idx[K], Ix->Indices[K]);
    Bad |= Idx[K].M == Mode::Invalid;
    Ptrs.push_back(&Idx[K]);
    Es.push_back(Ix->Indices[K]);
  }
  Operand &V = Idx.back();
  const ast::Expr *Val = RhsExpr;
  if (X) {
    V = *X;
    if (!Val)
      Val = V.E ? V.E : synthExpr(Lhs->getRange());
  } else {
    expr(V, RhsExpr);
  }
  Bad |= V.M == Mode::Invalid;
  // A value no expression stands for (the variable of a `range` clause): the
  // messages name its type, not the synthesized text.
  const std::string ValS =
      X && !RhsExpr && V.Ty ? "a value of type " + str(V.Ty) : std::string();
  if (Bad) {
    if (X)
      X->M = Mode::Invalid;
    return true;
  }
  Ptrs.push_back(&V);
  Es.push_back(Val);
  OpMatch M = pickOverload(Writes, Ptrs);
  if (M.Ambiguous) {
    err(Ix, diag::err_op_ambiguous,
        {"`[]=`", str(Ix) + " = " + (ValS.empty() ? str(Val) : ValS),
         "the call fits " + opSetString(M.Tied) +
             "; write the value with an explicit type"});
    return true;
  }
  if (!M.M) {
    err(Ix, diag::err_op_no_method,
        {str(Ix) + " = " + (ValS.empty() ? str(Val) : ValS), str(B.Ty), "`[]=`",
         argList(*this, Ptrs),
         "; the operator methods of " + str(B.Ty) + ": " +
             opSetString(Writes)});
    return true;
  }
  OperatorUse U;
  U.Op = OpKind::IndexSet;
  U.IndexSet = M.M;
  Operand Out;
  opCall(Out, Ix, Ix->getRange(), M.M, Ix->X, B, Es, Ptrs, U);
  return true;
}

bool Checker::opCompound(const ast::AssignStmt *AS) {
  Operand X;
  const ast::Expr *Lhs = AS->Lhs[0], *Rhs = AS->Rhs[0];
  tok::Kind Bin = compoundTok(AS->Op);
  if (Bin == tok::eof)
    return false;
  const OpKind BinK = binaryKindOf(Bin);
  // a[i] op= v  =  a[i] = a[i] op v, with a and i evaluated once (EXP-22),
  // if a has `[]` and `[]=`. The element of a built-in slice or array is a
  // place: it takes the in-place path below, as a variable does.
  auto *Ix = dyn_cast<IndexExpr>(unparen(Lhs));
  Operand B;
  Type *BT = nullptr;
  if (Ix) {
    markPartialOk(Ix->X, /*Addr=*/false); // the receiver of `[]` and `[]=`
    exprOrType(B, Ix->X, true);
    BT = B.M == Mode::Invalid || B.M == Mode::TypeExpr
             ? nullptr
             : opBase(B.Ty, /*ThroughPtr=*/true);
    if (!hasOpKind(BT, OpKind::IndexSet)) {
      PreEval[Ix->X] = B;
      Ix = nullptr;
    }
  }
  if (Ix) {
    SmallVector<Operand, 4> Idx(Ix->Indices.size());
    SmallVector<Operand *, 4> IPtr;
    SmallVector<const ast::Expr *, 4> IEs;
    bool Bad = false;
    for (size_t K = 0; K < Ix->Indices.size(); ++K) {
      expr(Idx[K], Ix->Indices[K]);
      Bad |= Idx[K].M == Mode::Invalid;
      IPtr.push_back(&Idx[K]);
      IEs.push_back(Ix->Indices[K]);
    }
    if (Bad) {
      use(Rhs);
      return true;
    }
    // The read.
    SmallVector<Func *, 4> Reads;
    opCandidates(BT, OpKind::Index, Reads);
    OpMatch RM = pickOverload(Reads, IPtr);
    if (!RM.M) {
      err(Ix, diag::err_op_no_method,
          {str(Ix), str(B.Ty), "`[]`", argList(*this, IPtr),
           Reads.empty() ? "; `a[i] op= v` needs `[]` and `[]=` (EXP-22)"
                         : "; the operator methods of " + str(B.Ty) + ": " +
                               opSetString(Reads)});
      use(Rhs);
      return true;
    }
    Operand Elem;
    OperatorUse RU;
    RU.Op = OpKind::Index;
    RU.Index = RM.M;
    if (!opCall(Elem, Ix, Ix->getRange(), RM.M, Ix->X, B, IEs, IPtr, RU)) {
      use(Rhs);
      return true;
    }
    Elem.M = Mode::Value;
    Elem.E = Ix;
    // The element is the receiver or an operand of the middle step, which the
    // lowering reads by this node: its type is recorded (P04.23 review R1).
    recordOperand(Elem);
    // The middle step: elem op rhs, through the binary rule.
    Operand Y;
    expr(Y, Rhs);
    if (Y.M == Mode::Invalid)
      return true;
    PreEval[Ix] = Elem;
    PreEval[Rhs] = Y;
    LastOp = OperatorUse();
    Operand Res;
    binary(Res, nullptr, Ix, Rhs, Bin, AS->OpLoc);
    PreEval.erase(Ix);
    PreEval.erase(Rhs);
    if (Res.M == Mode::Invalid)
      return true;
    OperatorUse Mid = LastOp;
    // The write: a[i] = result.
    SmallVector<Func *, 4> Writes;
    opCandidates(BT, OpKind::IndexSet, Writes);
    const ast::Expr *Val = synthExpr(AS->getRange());
    Operand VOp = Res;
    VOp.M = Mode::Value;
    VOp.E = Val;
    SmallVector<Operand *, 4> WPtr(IPtr.begin(), IPtr.end());
    WPtr.push_back(&VOp);
    SmallVector<const ast::Expr *, 4> WEs(IEs.begin(), IEs.end());
    WEs.push_back(Val);
    OpMatch WM = pickOverload(Writes, WPtr);
    if (!WM.M) {
      err(Ix, diag::err_op_no_method,
          {str(Ix) + " " + tok::getSpelling(AS->Op) + " " + str(Rhs), str(B.Ty),
           "`[]=`", argList(*this, WPtr),
           "; the operator methods of " + str(B.Ty) + ": " +
               opSetString(Writes)});
      return true;
    }
    Operand Out;
    if (!opCall(Out, nullptr, AS->getRange(), WM.M, Ix->X, B, WEs, WPtr,
                OperatorUse()))
      return true;
    // The statement: the middle call, with the read and the write.
    OperatorUse U = Mid;
    U.Op = BinK;
    U.Index = RM.M;
    U.IndexSet = WM.M;
    U.SetCall = LastOp.Call;
    recordOperator(AS, U);
    return true;
  }
  // x op= y with an in-place method (EXP-19): x is a place of that type.
  markPartialOk(Lhs, /*Addr=*/false); // the receiver of the in-place method
  expr(X, Lhs);
  if (X.M == Mode::Invalid) {
    use(Rhs);
    return true;
  }
  auto Back = [&](Operand *Y) {
    // The ordinary path evaluates the operands itself: hand them back.
    PreEval[Lhs] = X;
    if (Y)
      PreEval[Rhs] = *Y;
    return false;
  };
  SmallVector<Func *, 4> All, C;
  opCandidates(opBase(X.Ty), binaryToCompound(BinK), All);
  // A map element is no place: `m[k] += v` is `m[k] = m[k] + v`.
  if (X.M != Mode::MapIndex)
    for (Func *M : All)
      if (paramCount(M) == 1 && (M->HasPtrRecv || M->IsInterfaceMethod))
        C.push_back(M);
  if (C.empty())
    return Back(nullptr);
  Operand Y;
  expr(Y, Rhs);
  if (Y.M == Mode::Invalid)
    return true;
  SmallVector<Operand *, 1> Ptrs = {&Y};
  std::string Sp(tok::getSpelling(AS->Op));
  OpMatch M = pickOverload(C, Ptrs);
  if (M.Ambiguous) {
    err(AS, diag::err_op_ambiguous,
        {"`" + Sp + "`", str(Lhs) + " " + Sp + " " + str(Rhs),
         "the operand fits " + opSetString(M.Tied)});
    return true;
  }
  if (!M.M)
    return Back(&Y); // no in-place method for this operand: x = x op y
  // The in-place method has a writable receiver, so the operands must not
  // overlap it (OWN-B4, OWN-B9): `m += m` is an error with the way out.
  if (own() && overlapsPlace(Rhs, Lhs)) {
    err(Rhs, diag::err_op_compound_overlap,
        {str(Lhs) + " " + Sp + " " + str(Rhs), str(Lhs),
         str(Lhs) + " = " + str(Lhs) + " " + Sp.substr(0, Sp.size() - 1) + " " +
             str(Rhs)});
    return true;
  }
  OperatorUse U;
  U.Op = BinK;
  U.InPlace = true;
  Operand Out;
  opCall(Out, AS, AS->getRange(), M.M, Lhs, X, {Rhs}, Ptrs, U);
  return true;
}

//===----------------------------------------------------------------------===//
// Overlapping places (OWN-B4, EXP-19)
//===----------------------------------------------------------------------===//

/// The variable and the path of fields, elements and derefs of the place E
/// (root first); false if E is not a place.
static bool placePath(Checker &C, const ast::Expr *E, Object *&Root,
                      llvm::SmallVectorImpl<std::string> &Path) {
  SmallVector<std::string, 8> Rev;
  auto Derefs = [&](const ast::Expr *Base) {
    // `p.f` and `p[i]` through a pointer are `(*p).f` and `(*p)[i]`.
    Type *T = C.I.typeOf(Base);
    return T && isa<PointerType>(C.under(T));
  };
  for (;;) {
    E = unparen(E);
    if (auto *Sel = dyn_cast<SelectorExpr>(E)) {
      auto It = C.I.Selections.find(Sel);
      if (It != C.I.Selections.end() && It->second.K != Selection::FieldVal)
        return false;
      Rev.push_back(std::string(Sel->Sel->Name.str()));
      if (Derefs(Sel->X))
        Rev.push_back("*");
      E = Sel->X;
    } else if (auto *Ix = dyn_cast<IndexExpr>(E)) {
      // A constant index names its element; any other index may be any.
      const TypeAndValue *TV =
          Ix->Indices.size() == 1 ? C.I.lookup(Ix->Indices[0]) : nullptr;
      Rev.push_back(TV && TV->Val ? "[" + TV->Val->str() + "]"
                                  : std::string("[]"));
      if (Derefs(Ix->X))
        Rev.push_back("*");
      E = Ix->X;
    } else if (auto *St = dyn_cast<StarExpr>(E)) {
      Rev.push_back("*");
      E = St->X;
    } else if (auto *Un = dyn_cast<UnaryExpr>(E); Un && Un->Op == tok::and_) {
      // `*(&x)` and `(&x).f` name x: the address and the deref cancel; an
      // address that is not dereferenced is no place.
      if (Rev.empty() || Rev.back() != "*")
        return false;
      Rev.pop_back();
      E = Un->X;
    } else {
      break;
    }
  }
  auto *Id = dyn_cast<ast::Ident>(E);
  if (!Id)
    return false;
  auto It = C.I.Uses.find(Id);
  if (It == C.I.Uses.end() || !It->second || !isa<Var>(It->second))
    return false;
  Root = It->second;
  Path.assign(Rev.rbegin(), Rev.rend());
  return true;
}

bool Checker::overlapsPlace(const ast::Expr *A, const ast::Expr *B) {
  Object *RA = nullptr, *RB = nullptr;
  SmallVector<std::string, 8> PA, PB;
  if (!placePath(*this, A, RA, PA) || !placePath(*this, B, RB, PB) || RA != RB)
    return false;
  size_t N = std::min(PA.size(), PB.size());
  for (size_t K = 0; K < N; ++K) {
    if (PA[K] == PB[K])
      continue;
    // `[]` (an unknown index) may be any element.
    if (PA[K][0] == '[' && PB[K][0] == '[' && (PA[K] == "[]" || PB[K] == "[]"))
      continue;
    return false;
  }
  return true; // one is a prefix of the other (or they are equal)
}

//===----------------------------------------------------------------------===//
// Constraints (GEN-13)
//===----------------------------------------------------------------------===//

bool Checker::builtinOpSignature(
    OpKind K, Type *T, SignatureType *Sig,
    const std::function<bool(Type *, Type *)> &Equiv) {
  if (!Sig || Sig->Variadic || !isValid(T) || isa<TypeParamType>(T))
    return false;
  Type *U = under(T);
  if (!U || !isValid(U))
    return false;
  ArrayRef<Type *> P = Sig->Params->Elems, R = Sig->Results->Elems;
  auto Is = [&](Type *A, Type *B) { return A && B && Equiv(A, B); };
  auto OneParam = [&](Type *Want) { return P.size() == 1 && Is(P[0], Want); };
  auto Result = [&](Type *Want) { return R.size() == 1 && Is(R[0], Want); };
  // ADR-0025, UNI-10: a unit type has the built-in operators of EXP-33 to
  // EXP-36 and no others: `+ - %` and the comparisons of two values of the
  // type, `* /` by a plain number of the base type (a product of two units
  // is another type), shifts by a plain count; no bit operators, no `++`.
  const UnitCanon *UnitT = unitOf(T);
  if (UnitT && isa<BasicType>(U)) {
    switch (K) {
    case OpKind::And:
    case OpKind::Or:
    case OpKind::Xor:
    case OpKind::AndNot:
    case OpKind::Compl:
    case OpKind::Inc:
    case OpKind::Dec:
      return false;
    case OpKind::Mul:
    case OpKind::Quo:
      if (!opAllowed(*this, tokOf(K), T, false))
        return false;
      return Result(T) && OneParam(Ctx.unitType(baseKind(T), nullptr));
    default:
      break;
    }
  }
  if (isBinaryArith(K)) {
    if (!isa<BasicType>(U) || !opAllowed(*this, tokOf(K), T, false))
      return false;
    if (K == OpKind::Shl || K == OpKind::Shr)
      return Result(T) && P.size() == 1 && allInteger(P[0]);
    return Result(T) && OneParam(T);
  }
  switch (K) {
  case OpKind::Neg:
  case OpKind::Pos:
  case OpKind::Compl:
    return isa<BasicType>(U) && opAllowed(*this, tokOf(K), T, true) &&
           P.empty() && Result(T);
  case OpKind::Not:
    return isa<BasicType>(U) && allBoolean(T) && P.empty() &&
           Result(typ(BasicKind::Bool));
  case OpKind::LAnd:
  case OpKind::LOr:
    return isa<BasicType>(U) && allBoolean(T) && OneParam(T) && Result(T);
  case OpKind::Eq: {
    // `==` is built in for every comparable type (`comparable` keeps it)
    // whose `==` methods do not replace it (eqKeepsBuiltin, EXP-20).
    Operand A;
    A.M = Mode::Value;
    A.Ty = T;
    return OneParam(T) && Result(typ(BasicKind::Bool)) && !isInterface(T) &&
           comparableType(T, false, nullptr) && eqKeepsBuiltin(A, A);
  }
  case OpKind::Cmp:
    return OneParam(T) && Result(typ(BasicKind::Int)) && isa<BasicType>(U) &&
           (isInteger(U) || isFloat(U) || isString(U));
  case OpKind::Inc:
  case OpKind::Dec:
    return isa<BasicType>(U) && isNumeric(U) && P.empty() && R.empty();
  case OpKind::Index: {
    if (P.size() != 1 || R.size() != 1)
      return false;
    if (auto *Sl = dyn_cast<SliceType>(U))
      return isInteger(P[0]) && Is(R[0], Sl->Elem);
    if (auto *A = dyn_cast<ArrayType>(U))
      return isInteger(P[0]) && Is(R[0], A->Elem);
    if (auto *M = dyn_cast<MapType>(U))
      return Is(P[0], M->Key) && Is(R[0], M->Elem);
    if (isString(U))
      return isInteger(P[0]) && Is(R[0], Ctx.byteType());
    return false;
  }
  case OpKind::Call:
    if (auto *Fn = dyn_cast<SignatureType>(U))
      return !Fn->Variadic && Is(Fn->Params, Sig->Params) &&
             Is(Fn->Results, Sig->Results);
    return false;
  default:
    break;
  }
  // `+= (T)`: the built-in `+` gives the in-place form too (GEN-13).
  if (isCompoundOp(K))
    return P.size() == 1 && R.empty() &&
           builtinOpSignature(
               compoundToBinary(K), T,
               Ctx.signature(Sig->Params, Ctx.tuple({T}), false, false, 0, {}),
               Equiv);
  return false;
}

bool Checker::satisfiesOpElem(Type *V, Func *M,
                              const std::function<bool(Type *, Type *)> &Equiv,
                              std::string *Cause) {
  auto Fail = [&](const std::string &Why) {
    if (Cause)
      *Cause = "(missing operator " + std::string(M->Name) + " " +
               funcString(M).substr(M->Name.size()) + Why + ")";
    return false;
  };
  auto *MSig = dyn_cast_or_null<SignatureType>(M->Ty);
  if (!MSig)
    return true; // reported where the element is declared
  const OpKind K = M->Op;
  Type *U = under(V);
  if (isa<InterfaceType>(U) || isa<TypeParamType>(V)) {
    // A type parameter (or a constraint): the elements of its own constraint
    // meet M, or every term of its type set has the operator.
    if (auto *VI = dyn_cast<InterfaceType>(U)) {
      TypeSet *VTS = typeSet(VI);
      for (Func *F : VTS->Methods)
        if (F->Op == K && F->Ty && Equiv(F->Ty, M->Ty))
          return true;
      if (VTS->hasTerms() && !VTS->Terms.empty()) {
        for (const Term &Tm : VTS->Terms)
          if (!satisfiesOpElem(Tm.T, M, Equiv, nullptr))
            return Fail(": " + str(Tm.T) + " in the type set of " + str(V) +
                        " has none");
        return true;
      }
    }
    return Fail("");
  }
  // The operator methods of V, decided as the plain use is: a form that
  // looks through a pointer finds the methods of the pointee (the method set
  // of *T has both receiver kinds); the others keep the pointer's own.
  Type *PV = V;
  bool IsPtr = false;
  if (auto *P = dyn_cast<PointerType>(V); P && throughPtr(K)) {
    PV = P->Elem;
    IsPtr = true;
  }
  SmallVector<Func *, 4> Cs;
  opCandidates(opBase(PV), K, Cs);
  bool PtrOnly = false;
  for (Func *F : Cs) {
    if (!Equiv(F->Ty, M->Ty))
      continue;
    if (F->Right && !isCommutativeOp(K))
      continue; // written for the swapped order: not `a op s`
    if (F->HasPtrRecv && !IsPtr) {
      PtrOnly = true;
      continue;
    }
    return true;
  }
  if (builtinOpSignature(K, V, MSig, Equiv))
    return true;
  if (PtrOnly && Cause) {
    *Cause = "(method " + std::string(M->Name) + " has pointer receiver)";
    return false;
  }
  return Fail("");
}

Func *Checker::opAtInstance(Type *V, OpKind K, Type *ElemSig) {
  auto *Want = dyn_cast_or_null<SignatureType>(ElemSig);
  if (!Want || !V || !isValid(V))
    return nullptr;
  Type *PV = V;
  bool IsPtr = false;
  if (auto *P = dyn_cast<PointerType>(V); P && throughPtr(K)) {
    PV = P->Elem;
    IsPtr = true;
  }
  SmallVector<Func *, 4> Cs;
  opCandidates(opBase(PV), K, Cs);
  for (Func *F : Cs) {
    if (!identical(F->Ty, Want))
      continue;
    if (F->Right && !isCommutativeOp(K))
      continue;
    if (F->HasPtrRecv && !IsPtr)
      continue;
    return F;
  }
  return nullptr;
}
