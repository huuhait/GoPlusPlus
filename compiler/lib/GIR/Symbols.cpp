//===- Symbols.cpp - Naming what the lowering emits -----------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Symbols.h"

#include "gopp/GIR/GIR.h"
#include "gopp/GIR/Lower.h"

using namespace gopp;
using namespace gopp::gir;

std::string Symbols::func(const sema::Func *F) {
  const sym::Node *N = Names.func(F);
  return N ? sym::mangle(N) : std::string();
}

std::string Symbols::funcInstance(const sema::Func *F,
                                  llvm::ArrayRef<sema::Type *> TArgs) {
  const sym::Node *N = Names.funcInstance(F, TArgs);
  return N ? sym::mangle(N) : std::string();
}

std::string Symbols::thunk(const sema::Func *F) {
  const sym::Node *N = Names.func(F);
  return N ? sym::mangle(Ctx.wrapper(sym::Kind::CABIThunk, N)) : std::string();
}

std::string Symbols::var(const sema::Var *V) {
  const sym::Node *N = Names.var(V);
  return N ? sym::mangle(N) : std::string();
}

std::string Symbols::initFunc(const sema::Package *P, uint64_t Ordinal) {
  return sym::mangle(Ctx.initFunc(Names.pathOf(P), Ordinal));
}

std::string Symbols::pkgInit(const sema::Package *P) {
  return sym::mangle(Ctx.pkgInit(Names.pathOf(P)));
}

std::string Symbols::glue(sym::Kind K, const sema::Type *T) {
  const sym::Node *N = Names.type(T);
  return N ? sym::mangle(Ctx.ofType(K, N)) : std::string();
}

std::string gir::symbolOf(SymbolKind K, const sema::Package *Pkg,
                          llvm::StringRef Name, uint64_t Ordinal) {
  sym::Context C;
  switch (K) {
  case SymbolKind::Func:
  case SymbolKind::Var: {
    // A main package is `main` whatever path the command line gave it.
    llvm::StringRef P =
        !Pkg || Pkg->Name == "main" ? llvm::StringRef("main") : Pkg->Path;
    return sym::mangle(K == SymbolKind::Func ? C.func(P, Name)
                                             : C.var(P, Name));
  }
  case SymbolKind::InitFunc: {
    llvm::StringRef P =
        !Pkg || Pkg->Name == "main" ? llvm::StringRef("main") : Pkg->Path;
    return sym::mangle(C.initFunc(P, Ordinal));
  }
  case SymbolKind::ExternC:
    return Name.str();
  case SymbolKind::Entry:
    return "main";
  case SymbolKind::Runtime:
    return runtimeSymbol(Name);
  }
  return Name.str();
}

std::string gir::runtimeSymbol(llvm::StringRef Name) {
  sym::Context C;
  return sym::mangle(C.func("gopp", Name));
}
