//===- Foreign.cpp - Foreign scope defaults and the mock ------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Foreign.h"

#include <cctype>

using namespace gopp;
using namespace gopp::sema;
using llvm::ArrayRef;
using llvm::StringRef;

//===----------------------------------------------------------------------===//
// ForeignScope defaults
//===----------------------------------------------------------------------===//

ForeignCall ForeignScope::checkCall(Object *Callee,
                                    ArrayRef<ForeignTemplateArg> Explicit,
                                    const ForeignArg *, ArrayRef<ForeignArg>) {
  ForeignCall R;
  if (!Explicit.empty()) {
    R.Error = "'" + Callee->Name.str() + "' is not a template";
    return R;
  }
  if (auto *F = llvm::dyn_cast<Func>(Callee))
    if (F->Ty)
      R.Sig = llvm::dyn_cast<SignatureType>(F->Ty);
  if (!R.Sig)
    R.Error = "'" + Callee->Name.str() + "' is not a function";
  return R;
}

ForeignInstance ForeignScope::instantiate(Object *Tmpl,
                                          ArrayRef<ForeignTemplateArg>) {
  ForeignInstance R;
  R.Error = "'" + Tmpl->Name.str() + "' cannot be instantiated";
  return R;
}

std::string ForeignScope::describe(const Type *) const { return {}; }

bool ForeignScope::hasDefaultConstructor(const Type *) const { return true; }

Type *ForeignScope::rangeElem(const Type *, Type **) const { return nullptr; }

Type *ForeignScope::cPointer(TypeContext &Ctx, Type *Pointee, bool NonNull) {
  Type *P = Ctx.pointer(Pointee);
  return NonNull ? P : static_cast<Type *>(Ctx.option(P));
}

Type *ForeignScope::cFuncPointer(TypeContext &Ctx, Type *Func, bool NonNull) {
  return NonNull ? Func : static_cast<Type *>(Ctx.option(Func));
}

Type *ForeignScope::cxxReference(TypeContext &Ctx, Type *Pointee) {
  return Ctx.pointer(Pointee);
}

//===----------------------------------------------------------------------===//
// MockForeignScope
//===----------------------------------------------------------------------===//

void MockForeignScope::addOverload(Func *F, Overload O) {
  if (!O.Sig)
    O.Sig = llvm::dyn_cast_or_null<SignatureType>(F->Ty);
  Overloads[F].push_back(O);
  if (!F->Recv)
    Objs[F->Name] = F; // methods are reached through their type
}

void MockForeignScope::addTemplate(Object *Tmpl, TemplateFn Make) {
  Tmpl->IsTemplate = true;
  Templates[Tmpl] = std::move(Make);
  Objs[Tmpl->Name] = Tmpl;
}

void MockForeignScope::addAmbiguous(StringRef Name,
                                    std::vector<ForeignCandidate> Cands) {
  Ambiguous[Name] = std::move(Cands);
}

ForeignLookup MockForeignScope::lookup(StringRef Name) {
  ForeignLookup R;
  auto It = Objs.find(Name);
  if (It != Objs.end()) {
    R.Obj = It->second;
    return R;
  }
  auto A = Ambiguous.find(Name);
  if (A != Ambiguous.end())
    R.Ambiguous = A->second;
  return R;
}

namespace {
/// Can an argument be passed for a parameter of type P? Identity, with two
/// conveniences a C++ call has: a constant takes any basic parameter and nil
/// any pointer-like one.
bool accepts(Type *P, const ForeignArg &A) {
  if (A.Nil) {
    switch (under(P)->getKind()) {
    case Type::Kind::Option: // a C `T*` is Option[*T]: nil is NULL
    case Type::Kind::Slice:
    case Type::Kind::Map:
    case Type::Kind::Chan:
    case Type::Kind::Signature:
    case Type::Kind::Interface:
      return true;
    default:
      return false;
    }
  }
  if (A.Constant && A.Ty) {
    if (auto *B = llvm::dyn_cast<BasicType>(A.Ty))
      if (B->Info & IsUntyped)
        return llvm::isa<BasicType>(under(P));
  }
  // A *T (or a T) passed for an Option parameter converts automatically.
  if (auto *O = llvm::dyn_cast<OptionType>(under(P))) {
    if (A.Constant && A.Ty)
      if (auto *B = llvm::dyn_cast<BasicType>(A.Ty))
        if (B->Info & IsUntyped)
          return llvm::isa<BasicType>(under(O->Elem));
    return A.Ty == P || A.Ty == O->Elem;
  }
  // A `ref T` parameter (a C `const T*` that is only read) takes a T or a
  // ref T: an owned value converts to a ref at zero cost (TYP-2).
  // A borrowed slice (writable) is read as a View of the same slice.
  if (auto *R = llvm::dyn_cast<RefType>(P)) {
    if (auto *AR = llvm::dyn_cast_or_null<RefType>(A.Ty))
      if (AR->Borrowed && !R->Borrowed && AR->Elem == R->Elem)
        return true;
    return A.Ty == P || A.Ty == R->Elem;
  }
  return A.Ty == P;
}
} // namespace

ForeignCall MockForeignScope::checkCall(Object *Callee,
                                        ArrayRef<ForeignTemplateArg> Explicit,
                                        const ForeignArg *Recv,
                                        ArrayRef<ForeignArg> Args) {
  (void)Recv;
  std::vector<Overload> Cands;
  auto It = Overloads.find(Callee);
  if (It != Overloads.end()) {
    Cands = It->second;
  } else {
    // A method of a type, or a function added with add().
    Overload O;
    if (auto *F = llvm::dyn_cast<Func>(Callee))
      O.Sig = llvm::dyn_cast_or_null<SignatureType>(F->Ty);
    if (O.Sig)
      Cands.push_back(O);
  }
  ForeignCall R;
  for (const Overload &O : Cands) {
    // With brackets only templates are candidates (ADR-0010 Q3).
    if (!Explicit.empty() && O.NumTemplateParams < Explicit.size()) {
      R.Notes.push_back("candidate rejected: " + typeString(O.Sig) + " takes " +
                        std::to_string(O.NumTemplateParams) +
                        " template arguments");
      continue;
    }
    auto &Ps = O.Sig->Params->Elems;
    bool Variadic = O.Sig->Variadic;
    if (Args.size() < (Variadic ? Ps.size() - 1 : Ps.size()) ||
        (!Variadic && Args.size() > Ps.size())) {
      R.Notes.push_back("candidate rejected: " + typeString(O.Sig) + " takes " +
                        std::to_string(Ps.size()) + " arguments");
      continue;
    }
    bool Ok = true;
    for (size_t K = 0; K < Args.size() && Ok; ++K) {
      Type *P = K < Ps.size() ? Ps[K] : Ps.back();
      if (Variadic && K + 1 >= Ps.size())
        P = llvm::cast<SliceType>(Ps.back())->Elem;
      Ok = accepts(P, Args[K]);
      if (!Ok)
        R.Notes.push_back(
            "candidate rejected: " + typeString(O.Sig) + ": argument " +
            std::to_string(K + 1) + " does not convert to " + typeString(P) +
            (Args[K].Ty ? " (it is " + typeString(Args[K].Ty) + ")"
                        : std::string()));
    }
    if (!Ok)
      continue;
    R.Sig = O.Sig;
    R.MutableParams = O.MutableParams;
    R.MutableRecv = O.MutableRecv;
    R.Notes.clear();
    return R;
  }
  R.Error = "no matching function for call to '" + Callee->Name.str() + "'";
  return R;
}

ForeignInstance MockForeignScope::instantiate(Object *Tmpl,
                                              ArrayRef<ForeignTemplateArg> A) {
  ForeignInstance R;
  auto It = Templates.find(Tmpl);
  if (It == Templates.end()) {
    R.Error = "'" + Tmpl->Name.str() + "' is not a template";
    return R;
  }
  R.Obj = It->second(A, R.Error);
  return R;
}

std::string MockForeignScope::describe(const Type *T) const {
  auto It = Spellings.find(T);
  return It == Spellings.end() ? std::string() : It->second;
}

//===----------------------------------------------------------------------===//
// MockForeignProvider
//===----------------------------------------------------------------------===//

MockForeignScope &MockForeignProvider::scopeOf(StringRef Path) {
  auto &P = Scopes[Path.str()];
  if (!P)
    P = std::make_unique<MockForeignScope>();
  return *P;
}

MockForeignScope &MockForeignProvider::namespaceIn(MockForeignScope &Parent,
                                                   StringRef Name, Arena &Mem) {
  Nested.push_back(std::make_unique<MockForeignScope>());
  MockForeignScope &S = *Nested.back();
  Package *P = Mem.make<Package>();
  P->Path = Mem.copy(Name);
  P->Name = P->Path;
  P->IsForeign = true;
  P->PkgScope = Mem.newScope(nullptr, {}, "package");
  P->Foreign = &S;
  P->Complete = true;
  PkgName *PN = Mem.make<PkgName>();
  PN->Name = P->Name;
  PN->Imported = P;
  PN->Foreign = true;
  PN->Used = true;
  Parent.add(PN);
  return S;
}

Package *MockForeignProvider::header(StringRef Path, Arena &Mem,
                                     std::string &Error) {
  (void)Error;
  Package *P = Mem.make<Package>();
  P->Path = Mem.copy(Path);
  // FOR-3: the package is named after the header file: "<stdio.h>" is stdio,
  // "github.com/madler/zlib/zlib.h" is zlib, "<vector>" is vector.
  StringRef File = Path.trim("<>");
  File = File.substr(File.find_last_of('/') + 1);
  std::string N;
  for (char C : File) {
    if (std::isalnum(static_cast<unsigned char>(C)) || C == '_')
      N += C;
    else if (C == '.')
      break;
  }
  P->Name = Mem.copy(N);
  P->IsForeign = true;
  P->PkgScope = Mem.newScope(nullptr, {}, "package");
  P->Foreign = &scopeOf(Path);
  P->Complete = true;
  return P;
}
