//===- ExampleMocks.cpp - Mock headers of the spec examples ---------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/ExampleMocks.h"

using namespace gopp;
using namespace gopp::sema;

namespace {
Func *makeFunc(Arena &Mem, TypeContext &Ctx, Package *P, llvm::StringRef Name,
               std::vector<Type *> Params, std::vector<Type *> Results) {
  auto *F = Mem.make<Func>();
  F->Name = Mem.copy(Name);
  F->Pkg = P;
  F->Ty = Ctx.signature(Ctx.tuple(Params), Ctx.tuple(Results), false, false, 0);
  return F;
}

NamedType *makeClass(Arena &Mem, TypeContext &Ctx, Package *P,
                     llvm::StringRef Name) {
  auto *TN = Mem.make<TypeName>();
  TN->Name = Mem.copy(Name);
  TN->Pkg = P;
  NamedType *N = Ctx.newNamed(TN);
  TN->Ty = N;
  N->Underlying = Ctx.structOf({});
  return N;
}

Func *addMethod(Arena &Mem, TypeContext &Ctx, NamedType *N, Package *P,
                llvm::StringRef Name, std::vector<Type *> Params,
                std::vector<Type *> Results) {
  Func *F = makeFunc(Mem, Ctx, P, Name, Params, Results);
  auto *Recv = Mem.make<Var>();
  Recv->Name = "self";
  Recv->Ty = N;
  Recv->Pkg = P;
  F->Recv = Recv;
  if (!N->Methods)
    N->Methods = Ctx.newMethodList();
  Ctx.noteMemberName(F->Name);
  N->Methods->push_back(F);
  return F;
}
} // namespace

ExampleHeaderMocks::ExampleHeaderMocks(TypeContext &Ctx,
                                       MockForeignProvider &Prov)
    : Ctx(Ctx), Prov(Prov) {
  zlib();
  vector();
  algorithm();
}

void ExampleHeaderMocks::zlib() {
  // "github.com/madler/zlib/zlib.h": `uLong crc32(uLong crc, const Bytef
  // *buf, uInt len)`. The `const Bytef*` parameter takes a []byte as its data
  // pointer (ADR-0008 Q2); the mock models that with a `View[[]byte]`
  // parameter, which accepts an owned slice, a borrowed one or a view.
  const char *Path = "github.com/madler/zlib/zlib.h";
  std::string Err;
  Package *P = Prov.header(Path, Mem, Err);
  MockForeignScope &S = Prov.scopeOf(Path);
  auto Alias = [&](llvm::StringRef Name, Type *Ty) {
    auto *TN = Mem.make<TypeName>();
    TN->Name = Mem.copy(Name);
    TN->Pkg = P;
    TN->Ty = Ty;
    TN->IsAlias = true;
    S.add(TN);
    return Ty;
  };
  Type *ULong = Alias("uLong", Ctx.typ(BasicKind::Uint64));
  Type *UInt = Alias("uInt", Ctx.typ(BasicKind::Uint32));
  Alias("Bytef", Ctx.byteType());
  Type *Bytes = Ctx.ref(Ctx.slice(Ctx.byteType()));
  S.add(makeFunc(Mem, Ctx, P, "crc32", {ULong, Bytes, UInt}, {ULong}));
}

NamedType *ExampleHeaderMocks::vectorOf(Type *Elem) {
  NamedType *&Inst = Instances[Elem];
  if (Inst)
    return Inst;
  Type *U64 = Ctx.typ(BasicKind::Uint64);
  NamedType *N = makeClass(Mem, Ctx, VecPkg, "vector");
  NamedType *It = makeClass(Mem, Ctx, VecPkg, "iterator");
  Iterators[Elem] = It;
  addMethod(Mem, Ctx, N, VecPkg, "size", {}, {U64});
  Func *PushBack = addMethod(Mem, Ctx, N, VecPkg, "push_back", {Elem}, {});
  Vec->addOverload(PushBack, {nullptr, 0, true, 0});
  addMethod(Mem, Ctx, N, VecPkg, "begin", {}, {It});
  addMethod(Mem, Ctx, N, VecPkg, "end", {}, {It});
  std::string Spelling = "std::vector<" + typeString(Elem) + ">";
  Vec->setSpelling(N, Spelling);
  Vec->setSpelling(It, Spelling + "::iterator");
  // `for _, x := range v` is lowered to begin()/end() (P08 D8.6).
  Vec->setRangeElem(N, Elem);
  Inst = N;
  return N;
}

void ExampleHeaderMocks::vector() {
  std::string Err;
  VecPkg = Prov.header("<vector>", Mem, Err);
  Vec = &Prov.scopeOf("<vector>");
  auto *TN = Mem.make<TypeName>();
  TN->Name = "vector";
  TN->Pkg = VecPkg;
  TN->Ty = makeClass(Mem, Ctx, VecPkg, "vector"); // the template itself
  // Bodies are checked on several threads, so a scope answers from tables
  // filled before the check (Foreign.h): the instances are made here.
  vectorOf(Ctx.typ(BasicKind::Int32));
  Vec->addTemplate(TN,
                   [this](llvm::ArrayRef<ForeignTemplateArg> A,
                          std::string &Error) -> Object * {
                     auto It = A.size() == 1 && A[0].isType()
                                   ? Instances.find(A[0].Ty)
                                   : Instances.end();
                     if (It == Instances.end()) {
                       Error = "the mock <vector> has vector<int32> only";
                       return nullptr;
                     }
                     return It->second->Obj;
                   });
}

void ExampleHeaderMocks::algorithm() {
  // The std algorithms the example calls, for vector<int32> iterators (the
  // real importer deduces the template arguments from any iterator).
  std::string Err;
  AlgPkg = Prov.header("<algorithm>", Mem, Err);
  Alg = &Prov.scopeOf("<algorithm>");
  Type *I32 = Ctx.typ(BasicKind::Int32);
  NamedType *It = Iterators[I32];
  Type *Bool = Ctx.typ(BasicKind::Bool);
  auto Add = [&](llvm::StringRef Name, std::vector<Type *> Params,
                 std::vector<Type *> Results) {
    Func *F = makeFunc(Mem, Ctx, AlgPkg, Name, Params, Results);
    F->IsTemplate = true;
    Alg->addOverload(F, {llvm::cast<SignatureType>(F->Ty), 0, false, 1});
  };
  Add("sort", {It, It}, {});
  Add("reverse", {It, It}, {});
  Add("binary_search", {It, It, I32}, {Bool});
}
