//===- TypeString.cpp - Go-style text of types ----------------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gopp/Sema/Foreign.h"
#include "gopp/Sema/Types.h"

#include "llvm/Support/raw_ostream.h"

using namespace gopp;
using namespace gopp::sema;

namespace {

class Printer {
public:
  Printer(std::string &Out, const Package *Qual) : OS(Out), Qual(Qual) {}

  void type(const Type *T, unsigned Depth = 0) {
    if (!T) {
      OS << "<nil>";
      return;
    }
    if (Depth > 50) {
      OS << "...";
      return;
    }
    ++Depth;
    switch (T->getKind()) {
    case Type::Kind::Basic: {
      auto *B = llvm::cast<BasicType>(T);
      if (B->BK == BasicKind::UnsafePointer)
        OS << "unsafe.";
      OS << B->Name;
      break;
    }
    case Type::Kind::Pointer:
      OS << '*';
      type(llvm::cast<PointerType>(T)->Elem, Depth);
      break;
    case Type::Kind::Option:
      OS << "Option[";
      type(llvm::cast<OptionType>(T)->Elem, Depth);
      OS << ']';
      break;
    case Type::Kind::Slice:
      OS << "[]";
      type(llvm::cast<SliceType>(T)->Elem, Depth);
      break;
    case Type::Kind::Ref:
      OS << "ref ";
      type(llvm::cast<RefType>(T)->Elem, Depth);
      break;
    case Type::Kind::Array: {
      auto *A = llvm::cast<ArrayType>(T);
      OS << '[' << A->Len << ']';
      type(A->Elem, Depth);
      break;
    }
    case Type::Kind::Map: {
      auto *M = llvm::cast<MapType>(T);
      OS << "map[";
      type(M->Key, Depth);
      OS << ']';
      type(M->Elem, Depth);
      break;
    }
    case Type::Kind::Chan: {
      auto *C = llvm::cast<ChanType>(T);
      bool Paren = false;
      switch (C->D) {
      case ChanType::Send:
        OS << "chan<- ";
        break;
      case ChanType::Recv:
        OS << "<-chan ";
        break;
      case ChanType::Both: {
        OS << "chan ";
        if (auto *E = llvm::dyn_cast<ChanType>(C->Elem))
          Paren = E->D == ChanType::Recv;
        break;
      }
      }
      if (Paren)
        OS << '(';
      type(C->Elem, Depth);
      if (Paren)
        OS << ')';
      break;
    }
    case Type::Kind::Tuple:
      tuple(llvm::cast<TupleType>(T), false, Depth);
      break;
    case Type::Kind::Signature:
      OS << "func";
      signature(llvm::cast<SignatureType>(T), Depth);
      break;
    case Type::Kind::Struct: {
      auto *S = llvm::cast<StructType>(T);
      OS << "struct{";
      for (size_t I = 0; I < S->Fields.size(); ++I) {
        if (I)
          OS << "; ";
        const Var *F = S->Fields[I];
        if (!F->Embedded)
          OS << F->Name << ' ';
        type(F->Ty, Depth);
        if (F->Required) // MAC-12: the attribute comes before the tag
          OS << " @Required";
        if (!F->Tag.empty())
          OS << " \"" << F->Tag << '"';
      }
      OS << '}';
      break;
    }
    case Type::Kind::Interface:
      interface(llvm::cast<InterfaceType>(T), Depth);
      break;
    case Type::Kind::Union: {
      auto *U = llvm::cast<UnionType>(T);
      for (size_t I = 0; I < U->Terms.size(); ++I) {
        if (I)
          OS << " | ";
        if (U->Terms[I].Tilde)
          OS << '~';
        type(U->Terms[I].T, Depth);
      }
      break;
    }
    case Type::Kind::Named: {
      auto *N = llvm::cast<NamedType>(T);
      // A C/C++ type is spelled by its header scope (D3.7).
      if (N->Obj && N->Obj->Pkg && N->Obj->Pkg->Foreign) {
        std::string D = N->Obj->Pkg->Foreign->describe(N);
        if (!D.empty()) {
          OS << D;
          break;
        }
      }
      if (N->Obj) {
        if (N->Obj->Pkg && N->Obj->Pkg != Qual)
          OS << N->Obj->Pkg->Name << '.';
        OS << N->Obj->Name;
      } else {
        OS << "<named>";
      }
      if (!N->TArgs.empty()) {
        OS << '[';
        for (size_t I = 0; I < N->TArgs.size(); ++I) {
          if (I)
            OS << ", ";
          type(N->TArgs[I], Depth);
        }
        OS << ']';
      } else if (!N->TParams.empty()) {
        tparamList(N->TParams, Depth);
      }
      break;
    }
    case Type::Kind::TypeParam: {
      auto *P = llvm::cast<TypeParamType>(T);
      OS << (P->Obj ? P->Obj->Name : llvm::StringRef("$T"));
      break;
    }
    }
  }

private:
  void tuple(const TupleType *T, bool Variadic, unsigned Depth) {
    OS << '(';
    for (size_t I = 0; I < T->Elems.size(); ++I) {
      if (I)
        OS << ", ";
      if (Variadic && I + 1 == T->Elems.size()) {
        OS << "...";
        if (auto *S = llvm::dyn_cast<SliceType>(T->Elems[I]))
          type(S->Elem, Depth);
        else
          type(T->Elems[I], Depth); // string for append([]byte, string...)
        continue;
      }
      type(T->Elems[I], Depth);
    }
    OS << ')';
  }

  /// `[P, Q any, R int]`: consecutive parameters with the same constraint
  /// share it (types2: tParamList).
  void tparamList(llvm::ArrayRef<TypeParamType *> L, unsigned Depth) {
    OS << '[';
    const Type *Prev = nullptr;
    for (size_t I = 0; I < L.size(); ++I) {
      if (I) {
        if (L[I]->Bound != Prev) {
          OS << ' ';
          type(Prev, Depth);
        }
        OS << ", ";
      }
      Prev = L[I]->Bound;
      type(L[I], Depth);
    }
    if (Prev) {
      OS << ' ';
      type(Prev, Depth);
    }
    OS << ']';
  }

  void signature(const SignatureType *S, unsigned Depth) {
    if (!S->TParams.empty())
      tparamList(S->TParams, Depth);
    if (S->Fails)
      OS << '!';
    tuple(S->Params, S->Variadic, Depth);
    size_t NRes = S->Results->Elems.size() - (S->Fails ? 1 : 0);
    if (NRes == 0)
      return;
    OS << ' ';
    if (NRes == 1) {
      type(S->Results->Elems[0], Depth);
      return;
    }
    OS << '(';
    for (size_t I = 0; I < NRes; ++I) {
      if (I)
        OS << ", ";
      type(S->Results->Elems[I], Depth);
    }
    OS << ')';
  }

  void interface(const InterfaceType *I, unsigned Depth) {
    if (I->Implicit && I->Methods.empty() && I->Embeddeds.size() == 1) {
      type(I->Embeddeds[0], Depth);
      return;
    }
    if (I->Sp == InterfaceType::Comparable) {
      OS << "comparable";
      return;
    }
    if (I->IsAny) {
      OS << "any";
      return;
    }
    OS << "interface{";
    bool First = true;
    for (const Func *M : I->Methods) {
      if (!First)
        OS << "; ";
      First = false;
      OS << M->Name;
      signature(llvm::cast<SignatureType>(M->Ty), Depth);
    }
    for (const Type *E : I->Embeddeds) {
      if (!First)
        OS << "; ";
      First = false;
      type(E, Depth);
    }
    OS << '}';
  }

  llvm::raw_string_ostream OS;
  const Package *Qual;
};

} // namespace

std::string sema::typeString(const Type *T, const Package *Qual) {
  std::string S;
  {
    Printer P(S, Qual);
    P.type(T);
  }
  return S;
}
