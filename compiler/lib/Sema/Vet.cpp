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
// missed finding to a false alarm. The lints:
//   - a struct field of ref type in a struct type that is not part of a type
//     declaration marked @Ref (OWN-V5), in package-level and local
//     declarations and anonymous structs;
//   - a `drop` method that uses a pointer, interface, func or
//     Unique/Shared/Weak field of its receiver without comparing that field
//     with nil or asking IsNil(): `drop` also runs on the zero value (OWN-R5).
//     This is a heuristic: a guard anywhere in the body counts, whatever it
//     guards;
//   - a `!` that panics because its function cannot return the error, in a
//     package other than main (ERR-6, ERR-10, ERR-11a);
//   - `defer` in a loop body (D3.9, PAN-4).
// The findings come in source order: files in the given order, then by offset.
//
//===----------------------------------------------------------------------===//

#include "gopp/AST/ASTDumper.h"
#include "gopp/Sema/Sema.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"

#include <algorithm>

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
      structs(Dl, nullptr);
      if (auto *FD = dyn_cast<ast::FuncDecl>(Dl))
        dropMethod(FD);
    }
    bangsAndDefers(F);
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

  // --- OWN-V5 -------------------------------------------------------------

  /// Every struct type below N. Named is the name of the type declaration
  /// whose type N is (for the message); the type of a declaration marked
  /// @Ref is skipped with all the struct types nested in it.
  void structs(const ast::Node *N, const ast::Ident *Named) {
    if (auto *GD = dyn_cast<ast::GenDecl>(N);
        GD && GD->Keyword == tok::kw_type) {
      bool RefOk = hasAttr(GD, "Ref");
      for (const ast::Spec *S : GD->Specs)
        if (auto *TS = dyn_cast<ast::TypeSpec>(S); TS && TS->Type && !RefOk)
          structs(TS->Type, TS->Name);
      return;
    }
    if (auto *ST = dyn_cast<ast::StructType>(N); ST && ST->Fields)
      refFields(ST, Named);
    forEachChild(N, [&](const ast::Node *C) { structs(C, nullptr); });
  }

  void refFields(const ast::StructType *ST, const ast::Ident *Named) {
    std::string Struct = Named ? "struct " + Named->Name.str().str()
                               : std::string("the anonymous struct");
    for (const ast::Field *Fd : ST->Fields->List) {
      Type *T = I.typeOf(Fd->Type);
      if (!T || !isa<sema::RefType>(under(T)))
        continue;
      if (Fd->Names.empty())
        add(Fd->Type->getBegin(), diag::warn_vet_ref_field,
            {"<embedded>", Struct});
      for (const ast::Ident *Nm : Fd->Names)
        add(Nm->getBegin(), diag::warn_vet_ref_field, {Nm->Name.str(), Struct});
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

  /// A field whose zero value fails when it is used: an interface or func
  /// value. Pointers are never nil (ADR-0018), an Option cannot be used
  /// unchecked, a nil map can be read and a nil slice has length 0, so none of
  /// those count; neither does a `@Required` field, which is never nil (drop
  /// glue skips a moved-from value, ADR-0016 point 8).
  static bool mayBeNil(const Var *F) {
    Type *T = F->Ty;
    if (!T || F->Required)
      return false;
    switch (under(T)->getKind()) {
    case Type::Kind::Signature:
    case Type::Kind::Interface:
      return true;
    default:
      return false;
    }
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
    auto Visit = [&](auto &&Self, const ast::Node *N) -> void {
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
      forEachChild(N, [&](const ast::Node *C) { Self(Self, C); });
    };
    Visit(Visit, FD->Body);
    llvm::SmallPtrSet<const Var *, 4> Seen;
    for (auto &[V, Loc] : Uses)
      if (!Guarded.count(V) && Seen.insert(V).second)
        add(Loc, diag::warn_vet_drop_zero, {"field " + V->Name.str()});
  }

  // --- ! outside main (ERR-10) and defer in a loop (D3.9) ------------------

  void bangsAndDefers(const ast::File *F) {
    bool InMain = P.Pkg->Name == "main";
    auto Visit = [&](auto &&Self, const ast::Node *N, unsigned Loops) -> void {
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
          Self(Self, L->Body, 0);
        return;
      }
      unsigned Inner = Loops + (isLoop(N) ? 1 : 0);
      forEachChild(N, [&](const ast::Node *C) { Self(Self, C, Inner); });
    };
    for (const ast::Decl *Dl : F->Decls)
      Visit(Visit, Dl, 0);
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
