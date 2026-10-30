//===- Vet.cpp - The gopp vet lints --------------------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `gopp vet` v0 (P03.12, D3.12). A pass over the checked files that reads the
// AST and the side table; every finding is a warning, and each lint prefers a
// missed finding to a false alarm. (A view field without @View is a check
// error now, err_view_field_without_attr, OWN-V5.) The lints:
//   - a `drop` method that uses a pointer, interface, func or
//     Unique/Shared/Weak field of its receiver without comparing that field
//     with nil or asking IsNil(): `drop` also runs on the zero value (OWN-R5).
//     This is a heuristic: a guard anywhere in the body counts, whatever it
//     guards;
//   - a `!` that panics because its function cannot return the error, in a
//     package other than main (ERR-6, ERR-10, ERR-11a);
//   - `defer` in a loop body (D3.9, PAN-4);
//   - a @Strict struct with no required field: every field is Option[T],
//     @Option or `_`, so @Strict changes nothing (TYP-15a);
//   - `==` and `<=>` that can disagree (EXP-20): only one of them defined
//     for a right-hand type, different right-hand types, or (best effort)
//     bodies that read different fields; and a type with a user `==` used
//     as a map key or as the argument of a `comparable` parameter, where the
//     built-in equality is used instead of the method.
// The findings come in source order: files in the given order, then by offset.
//
//===----------------------------------------------------------------------===//

#include "gopp/AST/ASTDumper.h"
#include "gopp/Sema/Sema.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"

#include <algorithm>
#include <set>

using namespace gopp;
using namespace gopp::sema;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;
using llvm::StringRef;

namespace {

/// Calls F for every direct child of N.
void forEachChild(const ast::Node *N,
                  llvm::function_ref<void(const ast::Node *)> F) {
  ast::forEachChild(N, [&](const char *, const ast::Node *C) { F(C); });
}

/// A pre-order walk with an explicit stack, so a deep tree (a selector chain
/// of any length) does not use the call stack. Fn(N, State, Push) calls
/// Push(Child, State) for each child it wants walked, in order.
template <class S, class F>
void walkNodes(const ast::Node *Root, S Init, F Fn) {
  llvm::SmallVector<std::pair<const ast::Node *, S>, 64> Stack;
  llvm::SmallVector<std::pair<const ast::Node *, S>, 16> Kids;
  Stack.emplace_back(Root, std::move(Init));
  while (!Stack.empty()) {
    auto [N, St] = std::move(Stack.back());
    Stack.pop_back();
    if (!N)
      continue;
    Kids.clear();
    Fn(N, St,
       [&](const ast::Node *C, S CS) { Kids.emplace_back(C, std::move(CS)); });
    Stack.append(std::make_move_iterator(Kids.rbegin()),
                 std::make_move_iterator(Kids.rend()));
  }
}

const ast::Expr *unparen(const ast::Expr *E) {
  while (auto *P = dyn_cast<ast::ParenExpr>(E))
    E = P->X;
  return E;
}

bool isLoop(const ast::Node *N) {
  return isa<ast::ForStmt>(N) || isa<ast::RangeStmt>(N);
}

bool hasAttr(const ast::GenDecl *GD, StringRef Name) {
  for (const ast::Attribute *A : GD->Attrs)
    if (!A->Pkg && A->Name && A->Name->Name.str() == Name)
      return true;
  return false;
}

class Vet {
public:
  Vet(const CheckedPackage &P, DiagnosticsEngine &D) : P(P), I(P.I), D(D) {}

  void file(const ast::File *F) {
    // Findings of a file are sorted by position before they are reported.
    Pending.clear();
    for (const ast::Decl *Dl : F->Decls) {
      strictWithoutRequired(Dl);
      if (auto *FD = dyn_cast<ast::FuncDecl>(Dl)) {
        dropMethod(FD);
        eqCmpAgree(FD);
      }
    }
    bangsAndDefers(F);
    userEqBuiltin(F);
    std::stable_sort(Pending.begin(), Pending.end(),
                     [](const Finding &A, const Finding &B) {
                       return A.Loc.getRaw() < B.Loc.getRaw();
                     });
    for (Finding &Fd : Pending) {
      auto B = D.report(Fd.Loc, Fd.ID);
      for (const std::string &A : Fd.Args)
        B << A;
    }
  }

private:
  struct Finding {
    SourceLoc Loc;
    diag::ID ID;
    llvm::SmallVector<std::string, 2> Args;
  };

  void add(SourceLoc Loc, diag::ID ID,
           std::initializer_list<StringRef> Args = {}) {
    Finding Fd{Loc, ID, {}};
    for (StringRef A : Args)
      Fd.Args.push_back(A.str());
    Pending.push_back(std::move(Fd));
  }

  // --- @Strict without a required field (TYP-15a) -------------------------

  /// The struct type of a @Strict declaration none of whose fields is
  /// required. It reads the effective bit (Var::Required), as every other
  /// rule does: every field is Option[T], @Option or `_`.
  void strictWithoutRequired(const ast::Decl *Dl) {
    auto *GD = dyn_cast<ast::GenDecl>(Dl);
    if (!GD || GD->Keyword != tok::kw_type || !hasAttr(GD, "Strict"))
      return;
    for (const ast::Spec *Sp : GD->Specs) {
      auto *TS = dyn_cast<ast::TypeSpec>(Sp);
      if (!TS || !TS->Type || TS->AssignLoc.isValid())
        continue;
      auto *St =
          dyn_cast_or_null<sema::StructType>(I.typeOf(unparen(TS->Type)));
      if (!St || !St->Strict ||
          llvm::any_of(St->Fields, [](const Var *F) { return F->Required; }))
        continue;
      add(TS->Name->getBegin(), diag::warn_vet_strict_no_required,
          {TS->Name->Name.str()});
    }
  }

  // --- drop and the zero value (OWN-R5) -----------------------------------

  /// The field of the receiver that E names (`x.f`), or null.
  const Var *receiverField(const ast::Expr *E, const Object *Recv) const {
    auto *Sel = dyn_cast<ast::SelectorExpr>(unparen(E));
    auto *Base = Sel ? dyn_cast<ast::Ident>(unparen(Sel->X)) : nullptr;
    if (!Base)
      return nullptr;
    auto U = I.Uses.find(Base);
    if (U == I.Uses.end() || U->second != Recv)
      return nullptr;
    auto S = I.Selections.find(Sel);
    if (S == I.Selections.end() || S->second.K != Selection::FieldVal)
      return nullptr;
    return dyn_cast_or_null<Var>(S->second.Obj);
  }

  /// A field whose zero value fails when it is used: an `error` (its Error
  /// method). Pointers, funcs and interfaces with methods are never nil
  /// (ADR-0018 and its amendment), an Option cannot be used unchecked, `any`
  /// has no methods to call, a nil map can be read and a nil slice has length
  /// 0, so none of those count; neither does a `@Required` field, which is
  /// never nil (drop glue skips a moved-from value, ADR-0016 point 8).
  static bool mayBeNil(const Var *F) {
    Type *T = F->Ty;
    if (!T || F->Required)
      return false;
    auto *N = dyn_cast<NamedType>(T);
    return N && N->Obj && !N->Obj->Pkg && N->Obj->Name == "error";
  }

  static bool isNilIdent(const ast::Expr *E) {
    auto *Id = dyn_cast<ast::Ident>(unparen(E));
    return Id && Id->Name.str() == "nil";
  }

  void dropMethod(const ast::FuncDecl *FD) {
    if (!FD->Recv || FD->Recv->List.empty() || !FD->Body ||
        FD->Name->Name.str() != "drop")
      return;
    const ast::Field *R = FD->Recv->List[0];
    if (R->Names.empty())
      return;
    auto Def = I.Defs.find(R->Names[0]);
    if (Def == I.Defs.end() || !Def->second)
      return;
    const Object *Recv = Def->second;

    // One pass over the body: guards (`x.f == nil`, `x.f != nil`,
    // `x.f.IsNil()`) and uses that need the field to be set: `x.f.g`,
    // `x.f[i]`, `*x.f`, `x.f()`. A function literal runs later, on its own
    // terms: it is not looked at.
    llvm::SmallPtrSet<const Var *, 4> Guarded;
    llvm::SmallVector<std::pair<const Var *, SourceLoc>, 4> Uses;
    auto Visit = [&](const ast::Node *N, bool, auto &&Push) -> void {
      if (isa<ast::FuncLit>(N))
        return;
      if (auto *B = dyn_cast<ast::BinaryExpr>(N);
          B && (B->Op == tok::eql || B->Op == tok::neq)) {
        const ast::Expr *Other = isNilIdent(B->Y)   ? B->X
                                 : isNilIdent(B->X) ? B->Y
                                                    : nullptr;
        if (Other)
          if (const Var *V = receiverField(Other, Recv))
            Guarded.insert(V);
      }
      const ast::Expr *Through = nullptr;
      if (auto *S = dyn_cast<ast::SelectorExpr>(N)) {
        if (S->Sel->Name.str() == "IsNil") {
          if (const Var *V = receiverField(S->X, Recv))
            Guarded.insert(V);
        } else {
          Through = S->X;
        }
      } else if (auto *X = dyn_cast<ast::IndexExpr>(N)) {
        Through = X->X;
      } else if (auto *X = dyn_cast<ast::StarExpr>(N)) {
        Through = X->X;
      } else if (auto *X = dyn_cast<ast::CallExpr>(N)) {
        Through = X->Fun;
      }
      if (Through)
        if (const Var *V = receiverField(Through, Recv); V && mayBeNil(V))
          Uses.push_back({V, Through->getBegin()});
      forEachChild(N, [&](const ast::Node *C) { Push(C, false); });
    };
    walkNodes(FD->Body, false, Visit);
    llvm::SmallPtrSet<const Var *, 4> Seen;
    for (auto &[V, Loc] : Uses)
      if (!Guarded.count(V) && Seen.insert(V).second)
        add(Loc, diag::warn_vet_drop_zero, {"field " + V->Name.str()});
  }

  // --- `==` and `<=>` that disagree, and a user `==` that is not used ------

  /// The right-hand type of a binary operator method M as a key: the
  /// receiver type itself is one key for every method of a generic type,
  /// whatever its type parameters are called.
  static std::string rhsKey(const Func *M, const Package *Pkg) {
    auto *Sig = dyn_cast_or_null<SignatureType>(M->Ty);
    if (!Sig || Sig->Params->size() != 1 || !M->Recv)
      return {};
    const Type *P0 = Sig->Params->Elems[0], *RT = M->Recv->Ty;
    if (auto *PT = dyn_cast_or_null<PointerType>(RT))
      RT = PT->Elem;
    return P0 == RT ? std::string("<the receiver type>") : typeString(P0, Pkg);
  }

  /// The declared methods of a named type (of its origin, for an instance).
  static const llvm::SmallVector<Func *, 0> *methodsOf(const NamedType *N) {
    return N ? (N->Orig ? N->Orig : N)->Methods : nullptr;
  }

  /// The user operator methods of kind K of a named type (not @Right), as
  /// (right-hand type key, method).
  static llvm::SmallVector<std::pair<std::string, const Func *>, 2>
  opMethodsOf(const NamedType *N, OpKind K, const Package *Pkg) {
    llvm::SmallVector<std::pair<std::string, const Func *>, 2> R;
    const auto *Ms = methodsOf(N);
    if (!Ms)
      return R;
    for (const Func *M : *Ms) {
      if (M->Op != K || M->Right)
        continue;
      std::string Key = rhsKey(M, Pkg);
      if (!Key.empty())
        R.push_back({Key, M});
    }
    return R;
  }

  /// The names of the fields that the body of a method reads or writes.
  std::set<std::string> fieldsRead(const Func *M) const {
    std::set<std::string> R;
    if (!M->Decl || !M->Decl->Body)
      return R;
    // (An explicit stack: a body may hold a chain of any length.)
    walkNodes(M->Decl->Body, false,
              [&](const ast::Node *N, bool, auto &&Push) -> void {
                if (auto *Sel = dyn_cast<ast::SelectorExpr>(N)) {
                  auto It = I.Selections.find(Sel);
                  if (It != I.Selections.end() &&
                      It->second.K == Selection::FieldVal && It->second.Obj)
                    R.insert(It->second.Obj->Name.str());
                }
                forEachChild(N, [&](const ast::Node *C) { Push(C, false); });
              });
    return R;
  }

  static std::string joined(const std::set<std::string> &S) {
    std::string R;
    for (const std::string &X : S)
      R += (R.empty() ? "" : ", ") + X;
    return R;
  }

  void eqCmpAgree(const ast::FuncDecl *FD) {
    if (!FD->Recv || !FD->Name)
      return;
    StringRef Name = FD->Name->Name.str();
    if (Name != "==" && Name != "<=>")
      return;
    auto Def = I.Defs.find(FD->Name);
    auto *F =
        Def == I.Defs.end() ? nullptr : dyn_cast_or_null<Func>(Def->second);
    if (!F || !F->Recv || !F->Ty || F->Right || !F->isOperator())
      return;
    Type *RT = F->Recv->Ty;
    if (auto *PT = dyn_cast_or_null<PointerType>(RT))
      RT = PT->Elem;
    auto *N = dyn_cast_or_null<NamedType>(RT);
    auto *Sig = dyn_cast<SignatureType>(F->Ty);
    if (!N || !Sig || Sig->Params->size() != 1)
      return;
    std::string R = typeString(Sig->Params->Elems[0], P.Pkg);
    const std::string Key = rhsKey(F, P.Pkg);
    bool IsEq = Name == "==";
    auto Other = opMethodsOf(N, IsEq ? OpKind::Cmp : OpKind::Eq, P.Pkg);
    const Func *Twin = nullptr;
    for (auto &[Text, M] : Other)
      if (Text == Key)
        Twin = M;
    if (!Twin) {
      add(FD->Name->getBegin(), diag::warn_vet_op_eq_cmp,
          {typeString(N, P.Pkg), IsEq ? "`==`" : "`<=>`", R,
           IsEq ? "`<=>`" : "`==`"});
      return;
    }
    if (!IsEq)
      return; // the pair is looked at once, from the `==`
    std::set<std::string> A = fieldsRead(F), B = fieldsRead(Twin);
    if (!A.empty() && !B.empty() && A != B)
      add(FD->Name->getBegin(), diag::warn_vet_op_fields,
          {"`==`", joined(A), R, "`" + joined(B) + "`"});
  }

  static bool hasUserEq(const Type *T) {
    const auto *Ms = methodsOf(dyn_cast_or_null<NamedType>(T));
    if (!Ms)
      return false;
    for (const Func *M : *Ms)
      if (M->Op == OpKind::Eq && !M->Right)
        return true;
    return false;
  }

  /// A constraint that is `comparable`, or an interface that embeds it.
  static bool isComparableBound(const Type *B, unsigned Depth = 0) {
    if (!B || Depth > 4)
      return false;
    if (auto *N = dyn_cast<NamedType>(B))
      B = N->Underlying;
    auto *It = dyn_cast_or_null<InterfaceType>(B);
    if (!It)
      return false;
    if (It->Sp == InterfaceType::Comparable)
      return true;
    for (const Type *E : It->Embeddeds)
      if (isComparableBound(E, Depth + 1))
        return true;
    return false;
  }

  void userEqBuiltin(const ast::File *F) {
    // (An explicit stack: a declaration may hold a chain of any length.)
    auto Visit = [&](const ast::Node *N, bool, auto &&Push) -> void {
      if (auto *MT = dyn_cast<ast::MapType>(N); MT && MT->Key) {
        Type *K = I.typeOf(MT->Key);
        if (hasUserEq(K))
          add(MT->Key->getBegin(), diag::warn_vet_op_eq_builtin,
              {typeString(K, P.Pkg), "a map"});
      }
      if (auto *E = dyn_cast<ast::Expr>(N)) {
        auto It = I.Instances.find(E);
        if (It != I.Instances.end()) {
          // The generic function or type the expression names.
          const ast::Expr *Callee = E;
          if (auto *Ix = dyn_cast<ast::IndexExpr>(Callee))
            Callee = Ix->X;
          const ast::Ident *Id = dyn_cast<ast::Ident>(Callee);
          if (auto *Sel = dyn_cast<ast::SelectorExpr>(Callee))
            Id = Sel->Sel;
          auto U = Id ? I.Uses.find(Id) : I.Uses.end();
          llvm::ArrayRef<TypeParamType *> TPs;
          if (U != I.Uses.end() && U->second) {
            if (auto *Fn = dyn_cast<Func>(U->second))
              if (auto *Sig = dyn_cast_or_null<SignatureType>(Fn->Ty))
                TPs = Sig->TParams;
            if (auto *TN = dyn_cast<TypeName>(U->second))
              if (auto *Nm = dyn_cast_or_null<NamedType>(TN->Ty))
                TPs = Nm->TParams;
          }
          const auto &TA = It->second.TArgs;
          for (size_t K = 0; K < TA.size() && K < TPs.size(); ++K)
            if (isComparableBound(TPs[K]->Bound) && hasUserEq(TA[K]))
              add(E->getBegin(), diag::warn_vet_op_eq_builtin,
                  {typeString(TA[K], P.Pkg),
                   "the `comparable` parameter " + TPs[K]->Obj->Name.str()});
        }
      }
      forEachChild(N, [&](const ast::Node *C) { Push(C, false); });
    };
    for (const ast::Decl *Dl : F->Decls)
      walkNodes(Dl, false, Visit);
  }

  // --- ! outside main (ERR-10) and defer in a loop (D3.9) ------------------

  void bangsAndDefers(const ast::File *F) {
    bool InMain = P.Pkg->Name == "main";
    auto Visit = [&](const ast::Node *N, unsigned Loops, auto &&Push) -> void {
      if (auto *C = dyn_cast<ast::CallExpr>(N); C && C->isMust() && !InMain) {
        auto It = I.Bangs.find(C);
        if (It != I.Bangs.end() && It->second == BangKind::Panic)
          add(C->BangLoc, diag::warn_vet_bang_panics, {P.Pkg->Name.str()});
      }
      if (auto *DS = dyn_cast<ast::DeferStmt>(N); DS && Loops > 0)
        add(DS->DeferLoc, diag::warn_vet_defer_in_loop);
      if (auto *L = dyn_cast<ast::FuncLit>(N)) {
        // A function literal starts a new function: its defers run at its
        // end, whatever loop the literal is in.
        if (L->Body)
          Push(L->Body, 0u);
        return;
      }
      unsigned Inner = Loops + (isLoop(N) ? 1 : 0);
      forEachChild(N, [&](const ast::Node *C) { Push(C, Inner); });
    };
    for (const ast::Decl *Dl : F->Decls)
      walkNodes(Dl, 0u, Visit);
  }

  const CheckedPackage &P;
  const Info &I;
  DiagnosticsEngine &D;
  llvm::SmallVector<Finding, 8> Pending;
};

} // namespace

void sema::vetPackage(const SourceManager &, const CheckedPackage &P,
                      llvm::ArrayRef<const ast::File *> Files,
                      DiagnosticsEngine &Diags) {
  Vet V(P, Diags);
  for (const ast::File *F : Files)
    V.file(F);
}
