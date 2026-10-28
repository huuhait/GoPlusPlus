//===- Builtins.cpp - Built-in function calls -----------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Ported from types2's builtins.go (BSD-style license, Copyright The Go
// Authors), plus the Go++ built-ins move, clone, new_unique, new_shared and
// new_weak (EXP-1).
//
//===----------------------------------------------------------------------===//

#include "Checker.h"

using namespace gopp;
using namespace gopp::sema;
using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace {
struct BuiltinInfo {
  const char *Name;
  unsigned NArgs;
  bool Variadic;
};
} // namespace

static BuiltinInfo builtinInfo(BuiltinId Id) {
  switch (Id) {
  case BuiltinId::Append:
    return {"append", 1, true};
  case BuiltinId::Cap:
    return {"cap", 1, false};
  case BuiltinId::Clear:
    return {"clear", 1, false};
  case BuiltinId::Close:
    return {"close", 1, false};
  case BuiltinId::Complex:
    return {"complex", 2, false};
  case BuiltinId::Copy:
    return {"copy", 2, false};
  case BuiltinId::Delete:
    return {"delete", 2, false};
  case BuiltinId::Imag:
    return {"imag", 1, false};
  case BuiltinId::Len:
    return {"len", 1, false};
  case BuiltinId::Make:
    return {"make", 1, true};
  case BuiltinId::Max:
    return {"max", 1, true};
  case BuiltinId::Min:
    return {"min", 1, true};
  case BuiltinId::New:
    return {"new", 1, false};
  case BuiltinId::Panic:
    return {"panic", 1, false};
  case BuiltinId::Print:
    return {"print", 0, true};
  case BuiltinId::Println:
    return {"println", 0, true};
  case BuiltinId::Real:
    return {"real", 1, false};
  case BuiltinId::Recover:
    return {"recover", 0, false};
  case BuiltinId::Assert:
    return {"assert", 1, false};
  case BuiltinId::Trace:
    return {"trace", 0, true};
  case BuiltinId::Move:
    return {"move", 1, false};
  case BuiltinId::Clone:
    return {"clone", 1, false};
  case BuiltinId::NewUnique:
    return {"new_unique", 1, false};
  case BuiltinId::NewShared:
    return {"new_shared", 1, false};
  case BuiltinId::NewWeak:
    return {"new_weak", 1, false};
  case BuiltinId::Alignof:
    return {"unsafe.Alignof", 1, false};
  case BuiltinId::Offsetof:
    return {"unsafe.Offsetof", 1, false};
  case BuiltinId::Sizeof:
    return {"unsafe.Sizeof", 1, false};
  case BuiltinId::Add:
    return {"unsafe.Add", 2, false};
  case BuiltinId::Slice:
    return {"unsafe.Slice", 2, false};
  case BuiltinId::SliceData:
    return {"unsafe.SliceData", 1, false};
  case BuiltinId::String:
    return {"unsafe.String", 2, false};
  case BuiltinId::StringData:
    return {"unsafe.StringData", 1, false};
  }
  return {"?", 0, false};
}

static const ast::Expr *unparen(const ast::Expr *E) {
  while (auto *P = dyn_cast<ParenExpr>(E))
    E = P->X;
  return E;
}

bool Checker::builtin(Operand &X, const CallExpr *Call, BuiltinId Id) {
  BuiltinInfo Bin = builtinInfo(Id);
  auto Use = [&]() {
    for (const ast::Expr *A : Call->Args)
      use(A);
  };
  if (Call->EllipsisLoc.isValid() && Id != BuiltinId::Append) {
    err(Call->EllipsisLoc, diag::err_cannot_use_ellipsis_builtin, {Bin.Name});
    Use();
    return false;
  }
  bool SavedRecv = E.HasCallOrRecv;
  E.HasCallOrRecv = false;
  SmallVector<Operand, 4> Args;
  size_t NArgs = Call->Args.size();
  switch (Id) {
  case BuiltinId::Make:
  case BuiltinId::New:
  case BuiltinId::Offsetof:
    break;
  default:
    exprList(Call->Args, Args);
    NArgs = Args.size();
    for (Operand &A : Args)
      if (A.M == Mode::Invalid) {
        E.HasCallOrRecv |= SavedRecv;
        return false;
      }
    if (NArgs > 0)
      X = Args[0];
  }
  struct Restore {
    Checker &C;
    bool Saved;
    ~Restore() { C.E.HasCallOrRecv = C.E.HasCallOrRecv || Saved; }
  } R{*this, SavedRecv};
  bool HadCallOrRecv = E.HasCallOrRecv;

  // Argument count.
  if (NArgs < Bin.NArgs) {
    err(Call->Rparen.isValid() ? Call->Rparen : Call->getBegin(),
        diag::err_builtin_not_enough_args,
        {str(Call), num(Bin.NArgs), num(NArgs)});
    return false;
  }
  if (!Bin.Variadic && NArgs > Bin.NArgs) {
    err(Args.size() > Bin.NArgs ? Args[Bin.NArgs].E : Call->Args.back(),
        diag::err_builtin_too_many_args,
        {str(Call), num(Bin.NArgs), num(NArgs)});
    return false;
  }

  auto Sig1 = [&](Type *Res, ArrayRef<Type *> Ps) {
    return Ctx.signature(
        Ctx.tuple(Ps),
        Ctx.tuple(Res ? ArrayRef<Type *>(&Res, 1) : ArrayRef<Type *>()), false,
        false, 0);
  };
  auto ElemOfSlice = [&](Type *T) -> Type * {
    Type *U = coreType(T);
    if (auto *Rf = dyn_cast_or_null<RefType>(U))
      U = under(Rf->Elem);
    if (auto *Sl = dyn_cast_or_null<SliceType>(U))
      return Sl->Elem;
    return nullptr;
  };

  switch (Id) {
  case BuiltinId::Append: {
    Type *S0 = X.Ty;
    SignatureType *Sig = nullptr;
    bool Dots = Call->EllipsisLoc.isValid();
    if (NArgs == 2 && Dots) {
      Operand Tmp = X;
      if (assignableTo(Tmp, Ctx.slice(Ctx.byteType()), nullptr)) {
        Operand &Y = Args[1];
        bool HasString = false;
        bool Ok = underIs(Y.Ty, [&](Type *U) {
          if (!U)
            return false;
          if (auto *Sl = dyn_cast<SliceType>(U);
              Sl && identical(Sl->Elem, Ctx.byteType()))
            return true;
          if (isString(U)) {
            HasString = true;
            return true;
          }
          return false;
        });
        if (Ok && HasString) {
          Type *Ps[] = {S0, Y.Ty};
          Sig =
              Ctx.signature(Ctx.tuple(Ps), Ctx.tuple(ArrayRef<Type *>(&S0, 1)),
                            true, false, 0);
        }
      }
    }
    if (!Sig) {
      std::string Why;
      Type *Elem = sliceElem(X, Why);
      if (!Elem) {
        if (isValid(under(S0)))
          err(X, diag::err_sema_builtin, {"invalid append: " + Why});
        return false;
      }
      Type *Ps[] = {S0, Ctx.slice(Elem)};
      Sig = Ctx.signature(Ctx.tuple(Ps), Ctx.tuple(ArrayRef<Type *>(&S0, 1)),
                          true, false, 0);
    }
    SignatureType *Out = nullptr;
    arguments(Call, Sig, {}, {}, Args, Dots, Out);
    // Go++ (OWN-R4, PAR-1, OWN-V1): appending takes ownership of the slice,
    // unless the same place receives the result, and of the new elements.
    if (own() && X.E) {
      if (llvm::isa_and_nonnull<RefType>(coreType(S0))) {
        err(X, diag::err_own_append_ref, {str(X.E)});
      } else if (category(S0) == Category::Owning) {
        PlaceKind PK = placeKind(X.E);
        if (PK == PlaceKind::BorrowedVar) {
          err(X, diag::err_own_borrowed_append, {str(X.E)});
        } else if (PK == PlaceKind::OtherPlace && !isImplicitMove(X.E, Call)) {
          std::string S1 = str(X.E);
          err(X, diag::err_own_append_borrowed, {S1, S1});
        }
      }
      if (Dots && NArgs == 2) {
        if (Args[1].E && placeKind(Args[1].E) != PlaceKind::Temp) {
          Type *E2 = ElemOfSlice(Args[1].Ty);
          if (E2 && category(E2) != Category::Trivial)
            err(Args[1], diag::err_own_append_spread);
        }
      } else {
        Type *AElem = ElemOfSlice(S0);
        for (size_t K = 1; K < Args.size(); ++K) {
          if (AElem && isa<RefType>(AElem))
            checkRefStore(Args[K].E, AElem, Args[K].Ty);
          else
            needOwnership(Args[K], "append", AElem);
        }
      }
    }
    X.M = Mode::Value;
    X.Ty = S0;
    break;
  }
  case BuiltinId::Cap:
  case BuiltinId::Len: {
    Mode M = Mode::Invalid;
    ConstValue Val;
    Type *U = under(X.Ty);
    if (auto *Rf = dyn_cast<RefType>(U))
      U = under(Rf->Elem);
    if (auto *P = dyn_cast<PointerType>(U))
      if (auto *A = dyn_cast<sema::ArrayType>(under(P->Elem)))
        U = A;
    auto Check = [&](Type *Ux) {
      switch (Ux->getKind()) {
      case Type::Kind::Basic:
        return isString(Ux) && Id == BuiltinId::Len;
      case Type::Kind::Array:
      case Type::Kind::Slice:
      case Type::Kind::Chan:
        return true;
      case Type::Kind::Map:
        return Id == BuiltinId::Len;
      default:
        return false;
      }
    };
    switch (U->getKind()) {
    case Type::Kind::Basic:
      if (isString(U) && Id == BuiltinId::Len) {
        if (X.M == Mode::Constant) {
          M = Mode::Constant;
          Val = ConstValue::makeInt64(
              static_cast<int64_t>(X.Val.stringVal().size()));
        } else {
          M = Mode::Value;
        }
      }
      break;
    case Type::Kind::Array:
      M = Mode::Value;
      if (!HadCallOrRecv) {
        M = Mode::Constant;
        int64_t L = cast<sema::ArrayType>(U)->Len;
        Val = L >= 0 ? ConstValue::makeInt64(L) : ConstValue();
      }
      break;
    case Type::Kind::Slice:
    case Type::Kind::Chan:
      M = Mode::Value;
      break;
    case Type::Kind::Map:
      if (Id == BuiltinId::Len)
        M = Mode::Value;
      break;
    case Type::Kind::Interface:
      if (isTypeParam(X.Ty) && underIs(X.Ty, [&](Type *Ux) {
            if (!Ux)
              return false;
            if (auto *P = dyn_cast<PointerType>(Ux))
              if (auto *A = dyn_cast<sema::ArrayType>(under(P->Elem)))
                Ux = A;
            return Check(Ux);
          }))
        M = Mode::Value;
      break;
    default:
      break;
    }
    if (M == Mode::Invalid) {
      if (isValid(under(X.Ty)))
        err(X, diag::err_builtin_invalid_arg_type, {str(X), Bin.Name});
      return false;
    }
    X.M = M;
    X.Ty = typ(BasicKind::Int);
    X.Val = Val;
    break;
  }
  case BuiltinId::Clear: {
    bool Ok = underIs(X.Ty, [&](Type *U) {
      if (!U)
        return false;
      if (isa<sema::MapType>(U) || isa<SliceType>(U))
        return true;
      err(X, diag::err_sema_builtin,
          {"invalid argument: cannot clear " + str(X) +
           ": argument must be (or constrained by) map or slice"});
      return false;
    });
    if (!Ok)
      return false;
    if (X.E)
      checkRangeMutation(X.E, "clear");
    X.M = Mode::NoValue;
    break;
  }
  case BuiltinId::Close: {
    bool Ok = underIs(X.Ty, [&](Type *U) {
      auto *Ch = dyn_cast_or_null<sema::ChanType>(U);
      if (!Ch) {
        err(X, diag::err_sema_builtin,
            {"invalid operation: cannot close non-channel " + str(X)});
        return false;
      }
      if (Ch->D == sema::ChanType::Recv) {
        err(X, diag::err_sema_builtin,
            {"invalid operation: cannot close receive-only channel " + str(X)});
        return false;
      }
      return true;
    });
    if (!Ok)
      return false;
    X.M = Mode::NoValue;
    break;
  }
  case BuiltinId::Complex: {
    Operand &Y = Args[1];
    int D = 0;
    if (isUntyped(X.Ty))
      D |= 1;
    if (isUntyped(Y.Ty))
      D |= 2;
    switch (D) {
    case 1:
      convertUntyped(X, Y.Ty);
      break;
    case 2:
      convertUntyped(Y, X.Ty);
      break;
    case 3:
      if (X.M == Mode::Constant && Y.M == Mode::Constant) {
        auto ToFloat = [&](Operand &O) {
          if (isNumeric(O.Ty) && O.Val.imag().sign() == 0)
            O.Ty = typ(BasicKind::UntypedFloat);
        };
        ToFloat(X);
        ToFloat(Y);
      } else {
        convertUntyped(X, typ(BasicKind::Float64));
        convertUntyped(Y, typ(BasicKind::Float64));
      }
      break;
    default:
      break;
    }
    if (X.M == Mode::Invalid || Y.M == Mode::Invalid)
      return false;
    if (!identical(X.Ty, Y.Ty)) {
      err(X, diag::err_mismatched_types, {str(Call), str(X.Ty), str(Y.Ty)});
      return false;
    }
    Type *Res = nullptr;
    if (isa<TypeParamType>(X.Ty)) {
      bool Ok = underIs(X.Ty, [&](Type *U) {
        auto *B = dyn_cast_or_null<BasicType>(U);
        return B &&
               (B->BK == BasicKind::Float32 || B->BK == BasicKind::Float64);
      });
      if (!Ok) {
        err(X, diag::err_sema_builtin,
            {"invalid argument: arguments have type " + str(X.Ty) +
             ", expected floating-point"});
        return false;
      }
      err(X, diag::err_sema_builtin,
          {str(X) + " not supported as argument to built-in complex for go1.18 "
                    "(see go.dev/issue/50937)"});
      X.M = Mode::Value;
      X.Ty = invalid();
      break;
    }
    if (auto *B = dyn_cast<BasicType>(under(X.Ty))) {
      switch (B->BK) {
      case BasicKind::Float32:
        Res = typ(BasicKind::Complex64);
        break;
      case BasicKind::Float64:
        Res = typ(BasicKind::Complex128);
        break;
      case BasicKind::UntypedFloat:
        Res = typ(BasicKind::UntypedComplex);
        break;
      default:
        break;
      }
    }
    if (!Res) {
      err(X, diag::err_sema_builtin,
          {"invalid argument: arguments have type " + str(X.Ty) +
           ", expected floating-point"});
      return false;
    }
    if (X.M == Mode::Constant && Y.M == Mode::Constant) {
      ConstValue Re = X.Val.toFloat(), Im = Y.Val.toFloat();
      X.Val = ConstValue::makeComplex(Re.floatVal(), Im.floatVal());
    } else {
      X.M = Mode::Value;
    }
    X.Ty = Res;
    break;
  }
  case BuiltinId::Real:
  case BuiltinId::Imag: {
    if (isUntyped(X.Ty)) {
      if (X.M == Mode::Constant) {
        if (isNumeric(X.Ty))
          X.Ty = typ(BasicKind::UntypedComplex);
      } else {
        convertUntyped(X, typ(BasicKind::Complex128));
        if (X.M == Mode::Invalid)
          return false;
      }
    }
    Type *Res = nullptr;
    if (isa<TypeParamType>(X.Ty)) {
      bool Ok = underIs(X.Ty, [&](Type *U) {
        auto *B = dyn_cast_or_null<BasicType>(U);
        return B && (B->BK == BasicKind::Complex64 ||
                     B->BK == BasicKind::Complex128);
      });
      if (!Ok) {
        err(X, diag::err_sema_builtin,
            {"invalid argument: argument has type " + str(X.Ty) +
             ", expected complex type"});
        return false;
      }
      err(X, diag::err_sema_builtin,
          {str(X) + " not supported as argument to built-in " +
           (Id == BuiltinId::Real ? "real" : "imag") +
           " for go1.18 (see go.dev/issue/50937)"});
      X.M = Mode::Value;
      X.Ty = invalid();
      break;
    }
    if (auto *B = dyn_cast<BasicType>(under(X.Ty))) {
      switch (B->BK) {
      case BasicKind::Complex64:
        Res = typ(BasicKind::Float32);
        break;
      case BasicKind::Complex128:
        Res = typ(BasicKind::Float64);
        break;
      case BasicKind::UntypedComplex:
        Res = typ(BasicKind::UntypedFloat);
        break;
      default:
        break;
      }
    }
    if (!Res) {
      err(X, diag::err_sema_builtin,
          {"invalid argument: argument has type " + str(X.Ty) +
           ", expected complex type"});
      return false;
    }
    if (X.M == Mode::Constant)
      X.Val = Id == BuiltinId::Real ? X.Val.real() : X.Val.imag();
    else
      X.M = Mode::Value;
    X.Ty = Res;
    break;
  }
  case BuiltinId::Copy: {
    Operand &Y = Args[1];
    Type *Src = nullptr;
    // The special case: a []byte destination with a string source.
    bool Special = false;
    {
      Operand Tmp = X;
      if (assignableTo(Tmp, Ctx.slice(Ctx.byteType()), nullptr)) {
        Special = underIs(Y.Ty, [&](Type *U) {
          if (!U)
            return false;
          if (auto *Sl = dyn_cast<SliceType>(U);
              Sl && identical(Sl->Elem, Ctx.byteType()))
            return true;
          return isString(U);
        });
        if (Special)
          Src = Ctx.byteType();
      }
    }
    if (!Special) {
      std::string Why;
      Type *Dst = sliceElem(X, Why);
      if (!Dst) {
        err(X, diag::err_sema_builtin, {"invalid copy: " + Why});
        return false;
      }
      Src = sliceElem(Y, Why);
      if (!Src) {
        if (!allString(Y.Ty)) {
          err(Y, diag::err_sema_builtin, {"invalid copy: " + Why});
          return false;
        }
        Src = Ctx.byteType();
      }
      if (!identical(Dst, Src)) {
        err(X, diag::err_sema_builtin,
            {"invalid copy: arguments " + str(X) + " and " + str(Y) +
             " have different element types " + str(Dst) + " and " + str(Src)});
        return false;
      }
    }
    if (own() && Y.E && placeKind(Y.E) != PlaceKind::Temp &&
        category(Src) != Category::Trivial)
      err(Y, diag::err_own_copy_source);
    X.M = Mode::Value;
    X.Ty = typ(BasicKind::Int);
    break;
  }
  case BuiltinId::Delete: {
    Type *Key = nullptr;
    Type *MapT = X.Ty;
    bool Ok = underIs(MapT, [&](Type *U) {
      auto *M = dyn_cast_or_null<sema::MapType>(U);
      if (!M) {
        err(X, diag::err_sema_builtin,
            {"invalid argument: " + str(X) + " is not a map"});
        return false;
      }
      if (Key && !identical(M->Key, Key)) {
        err(X, diag::err_sema_builtin,
            {"invalid argument: maps of " + str(X) +
             " must have identical key types"});
        return false;
      }
      Key = M->Key;
      return true;
    });
    if (!Ok)
      return false;
    X = Args[1];
    assignment(X, Key, "argument to delete");
    if (X.M == Mode::Invalid)
      return false;
    X.M = Mode::NoValue;
    break;
  }
  case BuiltinId::Make: {
    const ast::Expr *Arg0 = Call->Args[0];
    Type *T = varType(Arg0);
    if (!isValid(T))
      return false;
    unsigned Min;
    std::string Why;
    Type *C = commonUnder(
        T,
        [&](Type *, Type *U) -> std::string {
          if (!U)
            return "no specific type";
          if (isa<SliceType>(U) || isa<sema::MapType>(U) ||
              isa<sema::ChanType>(U))
            return "";
          return "type must be slice, map, or channel";
        },
        &Why);
    if (!C) {
      err(Arg0, diag::err_sema_builtin,
          {"invalid argument: cannot make " + str(Arg0) + ": " + Why});
      return false;
    }
    Min = isa<SliceType>(C) ? 2 : 1;
    if (NArgs < Min || Min + 1 < NArgs) {
      err(Call, diag::err_sema_builtin,
          {str(Call) + " expects " + num(Min) + " or " + num(Min + 1) +
           " arguments; found " + num(NArgs)});
      return false;
    }
    SmallVector<int64_t, 2> Sizes;
    for (size_t K = 1; K < NArgs; ++K) {
      int64_t Sz = index(Call->Args[K], -1);
      if (Sz >= 0)
        Sizes.push_back(Sz);
    }
    if (Sizes.size() == 2 && Sizes[0] > Sizes[1])
      err(Call->Args[1], diag::err_sema_builtin,
          {"invalid argument: length and capacity swapped"});
    X.M = Mode::Value;
    X.Ty = T;
    break;
  }
  case BuiltinId::Max:
  case BuiltinId::Min: {
    tok::Kind Op = Id == BuiltinId::Max ? tok::gtr : tok::lss;
    for (size_t K = 0; K < Args.size(); ++K) {
      Operand &A = Args[K];
      if (A.M == Mode::Invalid)
        return false;
      if (!allOrdered(A.Ty)) {
        err(A, diag::err_sema_builtin,
            {"invalid argument: " + str(A) + " cannot be ordered"});
        return false;
      }
      if (K > 0) {
        matchTypes(X, A);
        if (X.M == Mode::Invalid)
          return false;
        if (!identical(X.Ty, A.Ty)) {
          err(A, diag::err_sema_builtin,
              {"invalid argument: mismatched types " + str(X.Ty) +
               " (previous argument) and " + str(A.Ty) + " (type of " +
               str(A.E) + ")"});
          return false;
        }
        if (X.M == Mode::Constant && A.M == Mode::Constant) {
          if (ConstValue::compare(A.Val, Op, X.Val))
            X = A;
        } else {
          X.M = Mode::Value;
        }
      }
    }
    if (X.M != Mode::Constant) {
      X.M = Mode::Value;
      assignment(X, Ctx.anyType(),
                 (std::string("argument to built-in ") + Bin.Name));
      if (X.M == Mode::Invalid)
        return false;
    }
    for (Operand &A : Args)
      updateExprType(A.E, X.Ty, true);
    // EXP-5: min/max of refs return a ref; of owned strings a copy.
    break;
  }
  case BuiltinId::New: {
    Type *T = varType(Call->Args[0]);
    if (!isValid(T))
      return false;
    X.M = Mode::Value;
    X.Ty = Ctx.pointer(T);
    break;
  }
  case BuiltinId::Panic:
    assignment(X, Ctx.anyType(), "argument to panic");
    if (X.M == Mode::Invalid)
      return false;
    X.M = Mode::NoValue;
    break;
  case BuiltinId::Print:
  case BuiltinId::Println:
    for (Operand &A : Args) {
      assignment(A, nullptr, std::string("argument to built-in ") + Bin.Name);
      if (A.M == Mode::Invalid)
        return false;
    }
    X.M = Mode::NoValue;
    break;
  case BuiltinId::Recover:
    X.M = Mode::Value;
    X.Ty = Ctx.anyType();
    break;
  case BuiltinId::Assert:
    // types2 self-test builtin: the result is the (constant) predicate.
    if (X.M != Mode::Constant || !isBoolean(X.Ty)) {
      err(X.E, diag::err_sema_builtin,
          {"invalid argument: " + str(X.E) + " is not a boolean constant"});
      return false;
    }
    if (!X.Val.boolVal())
      err(Call, diag::err_sema_builtin, {str(Call) + " failed"});
    break;
  case BuiltinId::Trace:
    // The result is the value of the first argument.
    break;
  // --- Go++ built-ins (EXP-1)
  case BuiltinId::Move: {
    const ast::Expr *Arg = Call->Args[0];
    if (X.M != Mode::Variable && X.M != Mode::MapIndex) {
      err(Arg, diag::err_own_move_not_place, {str(Arg)});
      return false;
    }
    if (own()) {
      if (Var *V = borrowedRoot(Arg)) {
        if (V->LoopVar)
          err(Arg, diag::err_own_move_range_borrowed, {str(Arg)});
        else
          err(Arg, diag::err_own_borrowed_move, {str(Arg)});
        return false;
      }
      if (isPinned(X.Ty)) {
        err(Arg, diag::err_own_pinned_move, {str(Arg), str(X.Ty)});
        return false;
      }
    }
    bool Comma = X.M == Mode::MapIndex;
    X.M = Comma ? Mode::CommaOk : Mode::Value;
    break;
  }
  case BuiltinId::Clone: {
    Type *T = X.Ty;
    if (own()) {
      if (auto *P = dyn_cast<PointerType>(T)) {
        LookupResult LR = lookupFieldOrMethod(T, false, S.Pkg, "Clone");
        if (auto *M = dyn_cast_or_null<Func>(LR.Obj)) {
          if (!M->HasPtrRecv) {
            err(Call, diag::err_own_clone_ambiguous,
                {str(Call), str(T), str(P->Elem), "Clone"});
            return false;
          }
        }
      }
      std::string Why;
      if (!satisfiesClone(T, &Why)) {
        err(Call->Args[0], diag::err_own_clone_not_cloneable,
            {str(Call->Args[0]), str(T), Why.empty() ? "" : ": " + Why});
        return false;
      }
    }
    if (isUntyped(T)) {
      assignment(X, nullptr, "argument to clone");
      if (X.M == Mode::Invalid)
        return false;
    }
    X.M = X.M == Mode::MapIndex ? Mode::CommaOk : Mode::Value;
    break;
  }
  case BuiltinId::NewUnique:
  case BuiltinId::NewShared: {
    assignment(X, nullptr, std::string("argument to ") + Bin.Name);
    if (X.M == Mode::Invalid)
      return false;
    Object *O = Ctx.universe()->lookupLocal(
        Id == BuiltinId::NewUnique ? "Unique" : "Shared");
    Type *El = X.Ty;
    needOwnership(X, "allocate from");
    X.M = Mode::Value;
    X.Ty = instantiateNamed(cast<NamedType>(O->Ty), ArrayRef<Type *>(&El, 1));
    break;
  }
  case BuiltinId::NewWeak: {
    PtrKind K;
    Type *El;
    if (!hasPtrKind(X.Ty, &K, &El) || K != PtrKind::Shared) {
      err(X, diag::err_own_weak_arg, {str(X.Ty)});
      return false;
    }
    Object *O = Ctx.universe()->lookupLocal("Weak");
    X.M = Mode::Value;
    X.Ty = instantiateNamed(cast<NamedType>(O->Ty), ArrayRef<Type *>(&El, 1));
    break;
  }
  // --- unsafe
  case BuiltinId::Add: {
    assignment(X, typ(BasicKind::UnsafePointer), "argument to unsafe.Add");
    if (X.M == Mode::Invalid)
      return false;
    if (!isValidIndex(Args[1], "length", true))
      return false;
    X.M = Mode::Value;
    X.Ty = typ(BasicKind::UnsafePointer);
    break;
  }
  case BuiltinId::Alignof:
  case BuiltinId::Sizeof: {
    assignment(X, nullptr, std::string("argument to ") + Bin.Name);
    if (X.M == Mode::Invalid)
      return false;
    unsigned long long Sz = sizeofType(X.Ty);
    X.M = Mode::Constant;
    if (Id == BuiltinId::Alignof)
      Sz = Sz >= 8 ? 8 : (Sz == 0 ? 1 : Sz);
    X.Val = ConstValue::makeUint64(Sz);
    X.Ty = typ(BasicKind::Uintptr);
    break;
  }
  case BuiltinId::Offsetof: {
    const ast::Expr *Arg0 = Call->Args[0];
    auto *Selx = dyn_cast<SelectorExpr>(unparen(Arg0));
    if (!Selx) {
      err(Arg0, diag::err_sema_builtin,
          {"invalid argument: " + str(Arg0) + " is not a selector expression"});
      use(Arg0);
      return false;
    }
    expr(X, Selx->X);
    if (X.M == Mode::Invalid)
      return false;
    Type *Base = derefStructPtr(X.Ty);
    LookupResult LR = lookupFieldOrMethod(Base, false, S.Pkg, name(Selx->Sel));
    if (!LR.Obj || isa<Func>(LR.Obj)) {
      err(Arg0, diag::err_sema_builtin,
          {"invalid argument: " + str(Arg0) + " has no single field " +
           std::string(name(Selx->Sel))});
      return false;
    }
    X.M = Mode::Constant;
    X.Val = ConstValue::makeUint64(0);
    X.Ty = typ(BasicKind::Uintptr);
    break;
  }
  case BuiltinId::Slice: {
    auto *P = dyn_cast_or_null<PointerType>(coreType(X.Ty));
    if (!P) {
      err(X, diag::err_sema_builtin,
          {"invalid argument: " + str(X) + " is not a pointer"});
      return false;
    }
    if (!isValidIndex(Args[1], "length", false))
      return false;
    X.M = Mode::Value;
    X.Ty = Ctx.slice(P->Elem);
    break;
  }
  case BuiltinId::SliceData: {
    auto *Sl = dyn_cast_or_null<SliceType>(coreType(X.Ty));
    if (!Sl) {
      err(X, diag::err_sema_builtin,
          {"invalid argument: " + str(X) + " is not a slice"});
      return false;
    }
    X.M = Mode::Value;
    X.Ty = Ctx.pointer(Sl->Elem);
    break;
  }
  case BuiltinId::String: {
    assignment(X, Ctx.pointer(Ctx.byteType()), "argument to unsafe.String");
    if (X.M == Mode::Invalid)
      return false;
    if (!isValidIndex(Args[1], "length", false))
      return false;
    X.M = Mode::Value;
    X.Ty = typ(BasicKind::String);
    break;
  }
  case BuiltinId::StringData: {
    assignment(X, typ(BasicKind::String), "argument to unsafe.StringData");
    if (X.M == Mode::Invalid)
      return false;
    X.M = Mode::Value;
    X.Ty = Ctx.pointer(Ctx.byteType());
    break;
  }
  }
  (void)Sig1;
  return true;
}
