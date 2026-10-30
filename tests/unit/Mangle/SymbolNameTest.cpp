//===- SymbolNameTest.cpp - Symbol names (D4.13, P04.17) ------------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The mangler and the demangler (docs/spec/appendix-a-symbol-names.md):
//   - a generated corpus over every kind of entity and type, with the
//     nastiest names, and an independent oracle for the readable text;
//   - round trips (model -> text -> model -> text);
//   - the same names whatever the order they are built or checked in;
//   - no collision with a C name;
//   - a demangler that refuses every malformed text (mutation fuzzing; run it
//     under ASan too);
//   - near-linear time on huge and very deep names (the SymbolNameLong suite,
//     run by tests/driver/symbol-names.long.test).
//
//===----------------------------------------------------------------------===//

#include "gopp/Mangle/SymbolName.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <set>

using namespace gopp::sym;
using llvm::StringRef;

namespace {

//===----------------------------------------------------------------------===//
// The corpus
//===----------------------------------------------------------------------===//

const char *const Idents[] = {
    "",
    "a",
    "_",
    "x1",
    "Foo",
    "日本語",
    "é",
    "Ωmega",
    "𝒳",
    "+",
    "-",
    "[]",
    "[]=",
    "<-",
    "<=>",
    "&^=",
    "==",
    "()",
    "range",
    "!",
    "a b",
    "$",
    "$2b$",
    "9lives",
    "0",
    "__x__",
    "T$",
    "\xff\xfe",
    "\xc3",
    "\xed\xa0\x80",
    "\xc0\x80",
    "\xf4\x90\x80\x80",
    std::string_view("a\0b", 3).data(),
    "tab\there",
    "quote\"s",
    "back\\slash",
    "dot.dot",
    "a/b",
    "ünïcödé_ñame",
};
constexpr size_t NumIdents = sizeof(Idents) / sizeof(Idents[0]);

const char *const Paths[] = {
    "main",
    "std/strings",
    "github.com/x/y.v2/z",
    "例/パッケージ",
    "a//b",
    "",
    "/",
    "a/",
    "../x",
    "example.com/é",
    "0",
    "9/8",
    "_",
    "units",
    "github.com/a-b/c.d/v2",
};
constexpr size_t NumPaths = sizeof(Paths) / sizeof(Paths[0]);

/// An identifier from the pool; NUL is handled: Idents[32] is "a\0b".
std::string ident(size_t I) {
  I %= NumIdents;
  if (I == 32)
    return std::string("a\0b", 3);
  return Idents[I];
}

/// A node with the text the demangler must print for it (written here
/// independently of the printer).
struct G {
  const Node *N;
  std::string T;
};

class Gen {
public:
  Gen(Context &C, uint64_t Seed) : C(C), R(Seed) {}

  size_t pick(size_t N) { return static_cast<size_t>(R() % N); }
  bool coin(unsigned Percent) { return R() % 100 < Percent; }
  std::string name() { return ident(pick(NumIdents)); }
  std::string path() { return Paths[pick(NumPaths)]; }

  static std::string qual(const std::string &P, const std::string &N) {
    return P.empty() ? N : P + "." + N;
  }

  G basic() {
    static const struct {
      Kind K;
      const char *N;
    } B[] = {{Kind::Bool, "bool"},
             {Kind::Int, "int"},
             {Kind::Int8, "int8"},
             {Kind::Int16, "int16"},
             {Kind::Int32, "int32"},
             {Kind::Int64, "int64"},
             {Kind::Uint, "uint"},
             {Kind::Uint8, "uint8"},
             {Kind::Uint16, "uint16"},
             {Kind::Uint32, "uint32"},
             {Kind::Uint64, "uint64"},
             {Kind::Uintptr, "uintptr"},
             {Kind::Float32, "float32"},
             {Kind::Float64, "float64"},
             {Kind::Complex64, "complex64"},
             {Kind::Complex128, "complex128"},
             {Kind::String, "string"},
             {Kind::UnsafePointer, "unsafe.Pointer"}};
    auto &E = B[pick(sizeof(B) / sizeof(B[0]))];
    return {C.basic(E.K), E.N};
  }

  G unitG() {
    unsigned NDims = static_cast<unsigned>(pick(4));
    std::vector<const Node *> Dims;
    struct D {
      std::string P, N;
      int64_t E;
      bool Param;
      uint64_t Idx;
    };
    std::vector<D> Ds;
    std::set<std::pair<std::string, std::string>> Seen;
    std::set<uint64_t> SeenP;
    for (unsigned I = 0; I < NDims; ++I) {
      int64_t E = static_cast<int64_t>(pick(7)) - 3;
      if (E == 0)
        E = 1;
      if (coin(15)) {
        uint64_t Idx = pick(5);
        if (!SeenP.insert(Idx).second)
          continue;
        Ds.push_back({"", "", E, true, Idx});
        Dims.push_back(C.paramDim(Idx, E));
      } else {
        std::string P = path(), N = name();
        if (!Seen.insert({P, N}).second)
          continue;
        Ds.push_back({P, N, E, false, 0});
        Dims.push_back(C.dim(P, N, E));
      }
    }
    static const char *const Factors[][2] = {{"1", "1"},
                                             {"1000", "1"},
                                             {"5", "18"},
                                             {"-3", "7"},
                                             {"1609344", "1000"}};
    auto &F = Factors[pick(5)];
    // The oracle: sort like the model (named by path then name, params last).
    std::stable_sort(Ds.begin(), Ds.end(), [](const D &X, const D &Y) {
      if (X.Param != Y.Param)
        return !X.Param;
      if (X.Param)
        return X.Idx < Y.Idx;
      if (X.P != Y.P)
        return X.P < Y.P;
      return X.N < Y.N;
    });
    auto Term = [](const D &X, int64_t E) {
      std::string T = X.Param ? "$u" + std::to_string(X.Idx)
                              : (X.P.empty() ? X.N : X.P + "." + X.N);
      if (E != 1)
        T += "^" + std::to_string(E);
      return T;
    };
    std::string Pos, Neg;
    int NegCount = 0;
    for (auto &X : Ds)
      if (X.E > 0)
        Pos += (Pos.empty() ? "" : "*") + Term(X, X.E);
    for (auto &X : Ds)
      if (X.E < 0) {
        Neg += (Neg.empty() ? "" : "*") + Term(X, -X.E);
        ++NegCount;
      }
    std::string Dim = Pos;
    if (!Neg.empty())
      Dim = (Pos.empty() ? "1" : Pos) + "/" +
            (NegCount > 1 ? "(" + Neg + ")" : Neg);
    std::string Fac;
    if (std::string(F[0]) != "1" || std::string(F[1]) != "1") {
      Fac = F[0];
      if (std::string(F[1]) != "1")
        Fac = "(" + Fac + "/" + F[1] + ")";
    }
    std::string T = Fac.empty() ? (Dim.empty() ? "1" : Dim)
                                : (Dim.empty() ? Fac : Fac + " " + Dim);
    return {C.unit(F[0], F[1], Dims), T};
  }

  G sig(int Depth) {
    unsigned NP = static_cast<unsigned>(pick(4));
    unsigned NR = static_cast<unsigned>(pick(3));
    bool Variadic = NP > 0 && coin(25);
    // `func!(P) R` is `func(P) (R, error)` (ERR-2): an `error` result, no flag.
    bool Fails = coin(25);
    std::vector<const Node *> Ps, Rs;
    std::string PT, RT;
    for (unsigned I = 0; I < NP; ++I) {
      G P = (Variadic && I + 1 == NP) ? slice(Depth) : type(Depth);
      Ps.push_back(P.N);
      if (I)
        PT += ", ";
      PT += (Variadic && I + 1 == NP) ? "..." + P.T.substr(2) : P.T;
    }
    std::vector<std::string> RTs;
    for (unsigned I = 0; I < NR; ++I) {
      G R2 = type(Depth);
      Rs.push_back(R2.N);
      RTs.push_back(R2.T);
    }
    if (Fails) {
      Rs.push_back(C.universe("error"));
      RTs.push_back("error");
    }
    std::string Res;
    if (RTs.size() == 1)
      Res = " " + RTs[0];
    else if (RTs.size() > 1) {
      Res = " (";
      for (size_t I = 0; I < RTs.size(); ++I)
        Res += (I ? ", " : "") + RTs[I];
      Res += ")";
    }
    uint64_t Flags = Variadic ? SigVariadic : 0;
    uint64_t Mask = coin(30) ? R() : 0;
    const Node *S = C.signature(C.tuple(Ps), C.tuple(Rs), Flags, Mask);
    return {S, "(" + PT + ")" + Res};
  }

  G slice(int Depth) {
    G E = type(Depth - 1);
    return {C.slice(E.N), "[]" + E.T};
  }

  /// A type; Depth bounds the nesting. Types made earlier are reused now and
  /// then, so the corpus is a DAG with repeated subtrees.
  G type(int Depth) {
    if (!Pool.empty() && coin(20))
      return Pool[pick(Pool.size())];
    G Result = typeNew(Depth);
    if (Pool.size() < 400)
      Pool.push_back(Result);
    return Result;
  }

  G typeNew(int Depth) {
    if (Depth <= 0)
      return coin(50) ? basic() : namedLeaf();
    switch (pick(22)) {
    case 0:
      return basic();
    case 1: {
      G E = type(Depth - 1);
      return {C.pointer(E.N), "*" + E.T};
    }
    case 2:
      return slice(Depth);
    case 3: {
      G E = type(Depth - 1);
      uint64_t N = coin(10) ? ~uint64_t(0) : pick(1000);
      return {C.array(E.N, N), "[" + std::to_string(N) + "]" + E.T};
    }
    case 4: {
      G K = type(Depth - 1), V = type(Depth - 1);
      return {C.map(K.N, V.N), "map[" + K.T + "]" + V.T};
    }
    case 5: {
      G E = type(Depth - 1);
      return {C.option(E.N), "Option[" + E.T + "]"};
    }
    case 6: {
      G E = type(Depth - 1);
      return {C.view(E.N), "View[" + E.T + "]"};
    }
    case 7: {
      G E = slice(Depth);
      return {C.borrowed(E.N), "borrowed " + E.T};
    }
    case 8: {
      G E = type(Depth - 1);
      char D = "bsr"[pick(3)];
      std::string P = D == 's' ? "chan<- " : D == 'r' ? "<-chan " : "chan ";
      bool Paren = D == 'b' && E.N->K == Kind::Chan && E.N->Sub == 'r';
      return {C.chan(D, E.N), P + (Paren ? "(" + E.T + ")" : E.T)};
    }
    case 9: {
      unsigned N = static_cast<unsigned>(pick(4));
      std::vector<const Node *> Es;
      std::string T = "(";
      for (unsigned I = 0; I < N; ++I) {
        G E = type(Depth - 1);
        Es.push_back(E.N);
        T += (I ? ", " : "") + E.T;
      }
      return {C.tuple(Es), T + ")"};
    }
    case 10: {
      G S = sig(Depth - 1);
      return {S.N, "func" + S.T};
    }
    case 11:
      return structG(Depth);
    case 12:
      return interfaceG(Depth);
    case 13:
    case 14:
      return namedLeaf();
    case 15:
      return instanceG(Depth);
    case 16: {
      uint64_t I = pick(8);
      return {C.typeParam(I), "$" + std::to_string(I)};
    }
    case 17: {
      G U = unitG();
      return {U.N, U.T};
    }
    case 18: {
      G U = unitG();
      static const struct {
        Kind K;
        const char *N;
      } B[] = {{Kind::Int, "int"},         {Kind::Int8, "int8"},
               {Kind::Uint64, "uint64"},   {Kind::Float32, "float32"},
               {Kind::Float64, "float64"}, {Kind::Complex128, "complex128"}};
      auto &E = B[pick(6)];
      return {C.unitType(E.K, U.N), std::string(E.N) + "[" + U.T + "]"};
    }
    case 19:
      return localNamed(Depth);
    case 20: {
      std::string N = name();
      return {C.universe(N), N};
    }
    default:
      return namedLeaf();
    }
  }

  G namedLeaf() {
    std::string P = path(), N = name();
    return {C.named(P, N), qual(P, N)};
  }

  G localNamed(int Depth) {
    G Scope = entityBase(Depth - 1);
    uint64_t Ord = pick(4);
    std::string N = name();
    return {C.localNamed(Scope.N, Ord, N),
            Scope.T + "." + N + "#" + std::to_string(Ord)};
  }

  G instanceG(int Depth) {
    G Base = coin(25) ? localNamed(Depth) : namedLeaf();
    unsigned N = 1 + static_cast<unsigned>(pick(3));
    std::vector<const Node *> Args;
    std::string T = Base.T + "[";
    for (unsigned I = 0; I < N; ++I) {
      G A = type(Depth - 1);
      Args.push_back(A.N);
      T += (I ? ", " : "") + A.T;
    }
    return {C.instance(Base.N, Args), T + "]"};
  }

  G structG(int Depth) {
    unsigned N = static_cast<unsigned>(pick(4));
    std::vector<const Node *> Fs;
    std::string T;
    bool Strict = coin(25);
    for (unsigned I = 0; I < N; ++I) {
      G E = type(Depth - 1);
      std::string FN = name();
      std::string Pkg = coin(30) ? path() : "";
      std::string Tag = coin(30) ? "json:\"" + name() + "\"" : "";
      uint64_t Fl =
          (coin(20) ? FieldEmbedded : 0) | (coin(20) ? FieldRequired : 0) |
          (coin(20) ? FieldOption : 0) | (coin(10) ? FieldParent : 0) |
          (coin(15) ? FieldMarkedRequired : 0) | (coin(10) ? FieldSetOnly : 0);
      Fs.push_back(C.field(FN, Pkg, E.N, Fl, Tag));
      std::string F;
      if (Fl & FieldParent)
        F = "extends ";
      else if (!(Fl & FieldEmbedded))
        F = FN + " ";
      F += E.T;
      // As TypeString: a @Strict struct shows the written @Required only.
      if (Fl & (Strict ? FieldMarkedRequired : FieldRequired))
        F += " @Required";
      if (Fl & FieldOption)
        F += " @Option";
      if (!Tag.empty())
        F += " \"" + Tag + "\"";
      T += (I ? "; " : "") + F;
    }
    return {C.structOf(Fs, Strict),
            std::string(Strict ? "@Strict " : "") + "struct{" + T + "}"};
  }

  G interfaceG(int Depth) {
    unsigned N = static_cast<unsigned>(pick(4));
    struct M {
      std::string Pkg, Name, Sig;
      const Node *Node;
    };
    std::vector<M> Ms;
    for (unsigned I = 0; I < N; ++I) {
      G S = sig(Depth - 1);
      std::string Pkg = coin(30) ? path() : "";
      std::string Name = name();
      Ms.push_back({Pkg, Name, S.T, C.imethod(Name, Pkg, S.N)});
    }
    std::stable_sort(Ms.begin(), Ms.end(), [](const M &X, const M &Y) {
      if (X.Pkg != Y.Pkg)
        return X.Pkg < Y.Pkg;
      return X.Name < Y.Name;
    });
    std::vector<const Node *> Nodes;
    std::string T = "interface{";
    for (size_t I = 0; I < Ms.size(); ++I) {
      Nodes.push_back(Ms[I].Node);
      T += (I ? "; " : "") + Ms[I].Name + Ms[I].Sig;
    }
    // The empty method set is `any` (and `interface{}`: one type).
    return {C.interfaceOf(Nodes), Ms.empty() ? "any" : T + "}"};
  }

  /// A function, method, variable or closure: something a closure or a
  /// local type can belong to.
  G entityBase(int Depth) {
    switch (pick(Depth > 0 ? 6 : 2)) {
    case 0: {
      std::string P = path(), N = name();
      return {C.func(P, N), qual(P, N)};
    }
    case 1: {
      std::string P = path(), N = name();
      return {C.var(P, N), qual(P, N)};
    }
    case 2: {
      G Parent = entityBase(Depth - 1);
      uint64_t Ord = 1 + pick(5);
      return {C.closure(Parent.N, Ord),
              Parent.T + ".func" + std::to_string(Ord)};
    }
    case 3:
      return methodG(Depth);
    case 4: {
      // A generic method of a generic type, instantiated.
      G M = methodG(Depth);
      unsigned N = 1 + static_cast<unsigned>(pick(2));
      std::vector<const Node *> Args;
      std::string T = M.T + "[";
      for (unsigned I = 0; I < N; ++I) {
        G A = type(Depth - 1);
        Args.push_back(A.N);
        T += (I ? ", " : "") + A.T;
      }
      return {C.instance(M.N, Args), T + "]"};
    }
    default: {
      std::string P = path();
      uint64_t Ord = pick(3);
      return {C.initFunc(P, Ord), P + ".init." + std::to_string(Ord)};
    }
    }
  }

  G methodG(int Depth) {
    G Recv = coin(50) ? namedLeaf() : instanceG(Depth - 1);
    std::string N = name();
    return {C.method(Recv.N, N), Recv.T + "." + N};
  }

  G opMethodG(int Depth) {
    G Recv = coin(50) ? namedLeaf() : instanceG(Depth - 1);
    static const char *const Ops[] = {"+",   "-",  "*",   "[]", "[]=",   "<-",
                                      "<=>", "==", "&^=", "()", "range", "!"};
    std::string N = Ops[pick(12)];
    unsigned NP = static_cast<unsigned>(pick(3));
    std::vector<const Node *> Ps;
    std::string T = "(";
    for (unsigned I = 0; I < NP; ++I) {
      G P = type(Depth - 1);
      Ps.push_back(P.N);
      T += (I ? ", " : "") + P.T;
    }
    T += ")";
    bool Right = coin(30);
    return {C.method(Recv.N, N, C.tuple(Ps), Right),
            Recv.T + "." + N + T + (Right ? " @Right" : "")};
  }

  /// Any entity.
  G entity(int Depth) {
    switch (pick(20)) {
    case 0:
    case 1:
    case 2:
      return entityBase(Depth);
    case 3: {
      std::string P = path();
      return {C.pkgInit(P), P + ".init"};
    }
    case 4:
    case 5:
      return opMethodG(Depth);
    case 6: {
      G T = type(Depth);
      return {C.ofType(Kind::TypeInfo, T.N), "typeinfo for " + T.T};
    }
    case 7: {
      G T = type(Depth);
      return {C.ofType(Kind::VTable, T.N), "vtable for " + T.T};
    }
    case 8: {
      G T = type(Depth), I = type(Depth);
      return {C.itab(T.N, I.N), "itab for " + T.T + " as " + I.T};
    }
    case 9: {
      G T = type(Depth);
      return {C.ofType(Kind::EqGlue, T.N), "eq glue for " + T.T};
    }
    case 10: {
      G T = type(Depth);
      return {C.ofType(Kind::HashGlue, T.N), "hash glue for " + T.T};
    }
    case 11: {
      G T = type(Depth);
      return {C.ofType(Kind::DropGlue, T.N), "drop glue for " + T.T};
    }
    case 12: {
      G T = type(Depth);
      return {C.ofType(Kind::CloneGlue, T.N), "clone glue for " + T.T};
    }
    case 13: {
      std::string K = name();
      return {C.data(K), "data " + K};
    }
    case 14: {
      G E = entity(Depth - 1 < 0 ? 0 : Depth - 1);
      return {C.wrapper(Kind::CABIThunk, E.N), "C-ABI thunk for " + E.T};
    }
    case 15: {
      G E = entityBase(Depth);
      return {C.wrapper(Kind::MethodValue, E.N), "method value " + E.T};
    }
    case 16: {
      G E = entityBase(Depth);
      return {C.wrapper(Kind::MethodExpr, E.N), "method expression " + E.T};
    }
    case 17: {
      G E = entityBase(Depth);
      return {C.wrapper(Kind::Promoted, E.N), "promoted " + E.T};
    }
    case 18: {
      G E = entityBase(Depth);
      return {C.wrapper(Kind::IfaceThunk, E.N), "interface thunk for " + E.T};
    }
    default: {
      // A generic function instance.
      std::string P = path(), N = name();
      unsigned NA = 1 + static_cast<unsigned>(pick(3));
      std::vector<const Node *> Args;
      std::string T = qual(P, N) + "[";
      for (unsigned I = 0; I < NA; ++I) {
        G A = type(Depth);
        Args.push_back(A.N);
        T += (I ? ", " : "") + A.T;
      }
      return {C.instance(C.func(P, N), Args), T + "]"};
    }
    }
  }

  Context &C;
  std::mt19937_64 R;
  std::vector<G> Pool;
};

bool onlySymbolChars(StringRef S) {
  for (unsigned char Ch : S)
    if (!((Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') ||
          (Ch >= '0' && Ch <= '9') || Ch == '_' || Ch == '$'))
      return false;
  return true;
}

constexpr size_t BigOut = size_t(1) << 28;

/// The whole chain on one entity. Returns the mangled name.
std::string roundTrip(Context &C, const G &E) {
  std::string M = mangle(E.N);
  EXPECT_TRUE(StringRef(M).starts_with("_GP"));
  EXPECT_TRUE(onlySymbolChars(M)) << M;
  ParseError Err;
  // Same context: interning gives back the very node.
  const Node *Same = parseSymbol(C, M, Err);
  EXPECT_EQ(Same, E.N) << M << ": " << Err.Message << " at " << Err.Pos;
  // Another context: same text again, same readable text as the oracle.
  Context C2;
  const Node *N2 = parseSymbol(C2, M, Err);
  if (!N2) {
    ADD_FAILURE() << M << ": " << Err.Message << " at " << Err.Pos;
    return M;
  }
  EXPECT_EQ(mangle(N2), M);
  Printed P = print(N2, BigOut);
  EXPECT_TRUE(P.Ok);
  EXPECT_EQ(P.Text, E.T) << M;
  DemangleResult D = demangle(M, BigOut);
  EXPECT_TRUE(D.Ok) << D.Error;
  EXPECT_EQ(D.Text, E.T);
  return M;
}

TEST(SymbolName, TagsArePrefixFreeAndInTheAlphabet) {
  std::set<std::string> Seen;
  for (unsigned K = 0; K < static_cast<unsigned>(Kind::NumKinds); ++K) {
    std::string P = kindPrefix(static_cast<Kind>(K));
    ASSERT_FALSE(P.empty());
    EXPECT_TRUE(onlySymbolChars(P)) << P;
    EXPECT_TRUE(Seen.insert(P).second) << "tag used twice: " << P;
    for (char Reserved : std::string("E_$0123456789"))
      EXPECT_NE(P[0], Reserved);
  }
  for (const std::string &A : Seen)
    for (const std::string &B : Seen)
      if (A != B)
        EXPECT_FALSE(StringRef(B).starts_with(A))
            << A << " is a prefix of " << B;
  // `I` introduces a list of type arguments, `E` ends one: no other kind uses
  // `E`.
  EXPECT_FALSE(Seen.count("E"));
}

TEST(SymbolName, Goldens) {
  // The examples of the appendix (docs/spec/appendix-a-symbol-names.md): the
  // text is checked both ways, so the appendix cannot drift.
  Context C;
  const Node *Int = C.basic(Kind::Int), *Str = C.basic(Kind::String);
  const Node *Vec2 = C.named("main", "Vec2");
  const Node *Speed = C.unitType(
      Kind::Float64,
      C.unit("5", "18", {C.dim("units", "M", 1), C.dim("units", "S", -1)}));
  struct {
    const Node *N;
    const char *Mangled;
    const char *Text;
  } Cases[] = {
      {C.func("main", "Foo"), "_GPF4mainE3Foo", "main.Foo"},
      {C.func("std/strings", "Index"), "_GPF3std7stringsE5Index",
       "std/strings.Index"},
      {C.func("github.com/x/y", "Run"), "_GPF13github$2e$com1x1yE3Run",
       "github.com/x/y.Run"},
      {C.func("main", "日本語"), "_GPF4mainE18$65e5$$672c$$8a9e$",
       "main.日本語"},
      {C.func("p", "0x"), "_GPF1pE5$30$x", "p.0x"},
      {C.func("", ""), "_GPF0_E0_", ""},
      {C.method(Vec2, "Len"), "_GPM3LenN4mainE4Vec2", "main.Vec2.Len"},
      // An operator method: its name is the spelling, and its parameters tell
      // the overloads apart. The second `main.Vec2` is a back reference to
      // the offset of the first.
      {C.method(Vec2, "+", C.tuple({Vec2})), "_GPTo6op_addN4mainE4Vec2LB12_E",
       "main.Vec2.+(main.Vec2)"},
      {C.method(Vec2, "-", C.tuple({})), "_GPTo6op_negN4mainE4Vec2LE",
       "main.Vec2.-()"},
      // `@Right` (EXP-26): a second `/` with the same parameters.
      {C.method(Vec2, "/", C.tuple({C.basic(Kind::Float64)})),
       "_GPTo6op_quoN4mainE4Vec2LdE", "main.Vec2./(float64)"},
      {C.method(Vec2, "/", C.tuple({C.basic(Kind::Float64)}), true),
       "_GPTo7opr_quoN4mainE4Vec2LdE", "main.Vec2./(float64) @Right"},
      // An operator method of an instance is named by its declaration: the
      // receiver's type parameter is `$0`.
      {C.method(C.instance(C.named("main", "W"), {C.basic(Kind::Float64)}), "+",
                C.tuple({C.typeParam(0)})),
       "_GPTo6op_addIN4mainE1WdELQ0_E", "main.W[float64].+($0)"},
      {C.instance(C.func("std/slices", "Map"), {Int, Str}),
       "_GPIF3std6slicesE3MapirE", "std/slices.Map[int, string]"},
      {C.closure(C.method(C.instance(C.named("main", "Box"), {Int}), "Get"), 2),
       "_GPC2_M3GetIN4mainE3BoxiE", "main.Box[int].Get.func2"},
      {C.ofType(Kind::TypeInfo, C.map(Str, C.slice(Int))), "_GPTiHrSi",
       "typeinfo for map[string][]int"},
      {C.ofType(Kind::VTable, C.named("main", "Dog")), "_GPTvN4mainE3Dog",
       "vtable for main.Dog"},
      {C.pkgInit("main"), "_GPJ4mainE", "main.init"},
      {C.initFunc("main", 1), "_GPK4mainE1_", "main.init.1"},
      {C.var("main", "counter"), "_GPR4mainE7counter", "main.counter"},
      {C.wrapper(Kind::CABIThunk, C.func("main", "Callback")),
       "_GPTCF4mainE8Callback", "C-ABI thunk for main.Callback"},
      {C.ofType(Kind::TypeInfo, Speed),
       "_GPTiUdk5_18_o5unitsE1M1_o5unitsE1Sn1_E",
       "typeinfo for float64[(5/18) units.M/units.S]"},
      {C.ofType(Kind::TypeInfo,
                C.instance(C.named("main", "Pair"), {Speed, Speed})),
       "_GPTiIN4mainE4PairUdk5_18_o5unitsE1M1_o5unitsE1Sn1_EB18_E",
       "typeinfo for main.Pair[float64[(5/18) units.M/units.S], "
       "float64[(5/18) units.M/units.S]]"},
  };
  for (auto &T : Cases) {
    EXPECT_EQ(mangle(T.N), T.Mangled) << T.Text;
    DemangleResult D = demangle(T.Mangled);
    EXPECT_TRUE(D.Ok) << T.Mangled << ": " << D.Error;
    EXPECT_EQ(D.Text, T.Text) << T.Mangled;
  }
}

//===----------------------------------------------------------------------===//
// Round trip over the generated corpus
//===----------------------------------------------------------------------===//

TEST(SymbolName, CorpusRoundTrips) {
  Context C;
  Gen Gn(C, 20261030);
  size_t Compressed = 0, Kinds[static_cast<size_t>(Kind::NumKinds)] = {};
  std::map<std::string, const Node *> Names;
  for (int I = 0; I < 6000; ++I) {
    G E = Gn.entity(4);
    std::string M = roundTrip(C, E);
    // Injective: two different entities never share a name.
    auto [It, New] = Names.insert({M, E.N});
    EXPECT_TRUE(New || It->second == E.N) << M;
    if (M.size() < E.N->Size + 3)
      ++Compressed;
    // Count every kind that was met.
    llvm::SmallVector<const Node *, 32> Work{E.N};
    std::set<const Node *> Seen;
    while (!Work.empty()) {
      const Node *N = Work.pop_back_val();
      if (!Seen.insert(N).second)
        continue;
      ++Kinds[static_cast<size_t>(N->K)];
      Work.append(N->Kids.begin(), N->Kids.end());
    }
  }
  // The corpus reaches every kind (a back reference exists only in text).
  for (size_t K = 0; K < static_cast<size_t>(Kind::NumKinds); ++K)
    if (static_cast<Kind>(K) != Kind::BackRef)
      EXPECT_GT(Kinds[K], 0u) << "kind " << kindPrefix(static_cast<Kind>(K));
  EXPECT_GT(Compressed, 50u) << "the corpus should repeat subtrees";
}

TEST(SymbolName, EveryIdentifierRoundTrips) {
  Context C;
  for (size_t I = 0; I < NumIdents; ++I)
    for (size_t P = 0; P < NumPaths; ++P) {
      std::string N = ident(I);
      G E{C.func(Paths[P], N),
          std::string(Paths[P]).empty() ? N : std::string(Paths[P]) + "." + N};
      roundTrip(C, E);
    }
  // Every single byte, as a name of its own and inside a name.
  for (unsigned B = 0; B < 256; ++B) {
    std::string One(1, static_cast<char>(B));
    for (const std::string &N : {One, "a" + One + "b", One + "a", "é" + One}) {
      const Node *F = C.func("p", N);
      std::string M = mangle(F);
      EXPECT_TRUE(onlySymbolChars(M));
      ParseError Err;
      EXPECT_EQ(parseSymbol(C, M, Err), F) << Err.Message;
      DemangleResult D = demangle(M);
      EXPECT_TRUE(D.Ok);
      EXPECT_EQ(D.Text, "p." + N);
    }
  }
}

TEST(SymbolName, AllCodePointsRoundTrip) {
  Context C;
  std::string All;
  for (uint32_t CP = 0; CP < 0x110000; CP += 1) {
    if (CP >= 0xD800 && CP <= 0xDFFF)
      continue;
    // Pack code points in names of 64.
    std::string One;
    if (CP < 0x80)
      One.push_back(static_cast<char>(CP));
    else if (CP < 0x800) {
      One.push_back(static_cast<char>(0xC0 | (CP >> 6)));
      One.push_back(static_cast<char>(0x80 | (CP & 0x3F)));
    } else if (CP < 0x10000) {
      One.push_back(static_cast<char>(0xE0 | (CP >> 12)));
      One.push_back(static_cast<char>(0x80 | ((CP >> 6) & 0x3F)));
      One.push_back(static_cast<char>(0x80 | (CP & 0x3F)));
    } else {
      One.push_back(static_cast<char>(0xF0 | (CP >> 18)));
      One.push_back(static_cast<char>(0x80 | ((CP >> 12) & 0x3F)));
      One.push_back(static_cast<char>(0x80 | ((CP >> 6) & 0x3F)));
      One.push_back(static_cast<char>(0x80 | (CP & 0x3F)));
    }
    All += One;
    if (All.size() >= 4096) {
      const Node *F = C.func("p", All);
      std::string M = mangle(F);
      ParseError Err;
      ASSERT_EQ(parseSymbol(C, M, Err), F) << Err.Message;
      All.clear();
    }
  }
}

TEST(SymbolName, ReadableTextIsPlainAndStable) {
  Context C;
  const Node *Box = C.instance(
      C.named("main", "Box"),
      {C.unitType(Kind::Float64,
                  C.unit("5", "18",
                         {C.dim("units", "M", 1), C.dim("units", "S", -1)}))});
  EXPECT_EQ(print(Box).Text, "main.Box[float64[(5/18) units.M/units.S]]");
  EXPECT_EQ(print(C.ofType(Kind::TypeInfo,
                           C.instance(C.named("main", "Vec2"),
                                      {C.unit("1000", "1",
                                              {C.dim("units", "M", 1)})})))
                .Text,
            "typeinfo for main.Vec2[1000 units.M]");
}

TEST(SymbolName, BoundariesBetweenPathsAndNamesNeverBlur) {
  // The parts of a name never run into each other: a path segment
  // and a name, a name and a type argument, a name and grammar letters.
  Context C;
  std::set<std::string> S{
      mangle(C.var("a/b", "c")),  mangle(C.var("a", "b/c")),
      mangle(C.var("ab", "c")),   mangle(C.var("a", "bc")),
      mangle(C.var("a/b/c", "")), mangle(C.var("", "a/b/c")),
      mangle(C.var("a/", "bc")),  mangle(C.var("", "abc"))};
  EXPECT_EQ(S.size(), 8u);
  // Names that look like grammar stay names.
  EXPECT_NE(mangle(C.func("p", "E")), mangle(C.func("p", "I")));
  EXPECT_NE(mangle(C.func("p", "E")), mangle(C.func("pE", "")));
  EXPECT_NE(mangle(C.method(C.named("p", "T"), "+", C.tuple({}))),
            mangle(C.method(C.named("p", "T"), "op_pos")));
  EXPECT_NE(mangle(C.instance(C.func("", "a"), {C.basic(Kind::Bool)})),
            mangle(C.func("", "ab")));
  for (const std::string &M : S) {
    DemangleResult D = demangle(M);
    EXPECT_TRUE(D.Ok) << M << ": " << D.Error;
  }
}

//===----------------------------------------------------------------------===//
// Determinism: content, never creation order
//===----------------------------------------------------------------------===//

TEST(SymbolName, SameNameWhateverTheBuildOrder) {
  // 300 recipes; build them in many shuffled orders, each in a fresh Context
  // that already holds junk (so node ids differ), and compare the names.
  constexpr int N = 300;
  std::vector<std::string> Expected(N);
  auto Recipe = [](Context &C, int I) {
    Gen Gn(C, 777 + I); // the recipe is the seed: same content every time
    return mangle(Gn.entity(3).N);
  };
  {
    Context C;
    for (int I = 0; I < N; ++I)
      Expected[I] = Recipe(C, I);
  }
  std::mt19937_64 R(5);
  for (int Round = 0; Round < 12; ++Round) {
    std::vector<int> Order(N);
    for (int I = 0; I < N; ++I)
      Order[I] = I;
    std::shuffle(Order.begin(), Order.end(), R);
    Context C;
    Gen Junk(C, 99 + Round);
    for (int J = 0; J < Round * 50; ++J)
      Junk.entity(3);
    for (int I : Order)
      EXPECT_EQ(Recipe(C, I), Expected[I])
          << "recipe " << I << " round " << Round;
  }
}

TEST(SymbolName, OperatorMethodsUseTheFixedTable) {
  Context C;
  const Node *V = C.named("main", "V");
  const Node *F = C.basic(Kind::Float64);
  auto Name = [&](const char *Op, unsigned NParams) {
    std::vector<const Node *> Ps(NParams, F);
    return mangle(C.method(V, Op, C.tuple(Ps)));
  };
  struct Row {
    const char *Op;
    unsigned N;
    const char *Ident; // as written in the name: the length and the text
  } Rows[] = {
      {"+", 1, "6op_add"},
      {"-", 1, "6op_sub"},
      {"*", 1, "6op_mul"},
      {"/", 1, "6op_quo"},
      {"%", 1, "6op_rem"},
      {"&", 1, "6op_and"},
      {"|", 1, "5op_or"},
      {"^", 1, "6op_xor"},
      {"<<", 1, "6op_shl"},
      {">>", 1, "6op_shr"},
      {"&^", 1, "9op_andnot"},
      {"+=", 1, "12op_addassign"},
      {"&^=", 1, "15op_andnotassign"},
      {"-", 0, "6op_neg"},
      {"+", 0, "6op_pos"},
      {"^", 0, "8op_compl"},
      {"!", 0, "6op_not"},
      {"*", 0, "8op_deref"},
      {"++", 0, "6op_inc"},
      {"--", 0, "6op_dec"},
      {"==", 1, "5op_eq"},
      {"<=>", 1, "6op_cmp"},
      {"&&", 1, "7op_land"},
      {"||", 1, "6op_lor"},
      {"[]", 1, "8op_index"},
      {"[]", 3, "8op_index"},
      {"[]=", 2, "11op_setindex"},
      {"[]=", 4, "11op_setindex"},
      {"()", 0, "7op_call"},
      {"()", 5, "7op_call"},
      {"<-", 1, "7op_send"},
      {"<-", 0, "7op_recv"},
      {"range", 0, "8op_range"},
      // No row at that count: `opx_` and the spelling, escaped.
      {"+", 0 + 2, "8opx_$2b$"},
      {"-", 3, "8opx_$2d$"},
      {"[]=", 1, "16opx_$5b$$5d$$3d$"},
      {"~", 1, "8opx_$7e$"},
      {"", 1, "4opx_"},
  };
  for (const Row &R : Rows) {
    std::string M = Name(R.Op, R.N);
    EXPECT_EQ(M.rfind(std::string("_GPTo") + R.Ident + "N4mainE1VL", 0), 0u)
        << R.Op << "/" << R.N << ": " << M;
    // The readable name is the spelling and the parameter types.
    std::string Params = "(";
    for (unsigned I = 0; I < R.N; ++I)
      Params += std::string(I ? ", " : "") + "float64";
    DemangleResult D = demangle(M);
    EXPECT_TRUE(D.Ok) << M << ": " << D.Error;
    EXPECT_EQ(D.Text, std::string("main.V.") + R.Op + Params + ")") << M;
  }
  // `@Right` (EXP-26) is `opr_` (`oprx_` for a spelling without a row): a
  // non-commutative operator may have a `@Right` method and a plain one with
  // the same parameters, so the two need two names.
  for (const Row &R : Rows) {
    std::vector<const Node *> Ps(R.N, F);
    std::string M = mangle(C.method(V, R.Op, C.tuple(Ps), true));
    StringRef Ident = StringRef(R.Ident).drop_while(
        [](char Ch) { return Ch >= '0' && Ch <= '9'; });
    std::string Want =
        std::to_string(Ident.size() + 1) + "opr" + Ident.drop_front(2).str();
    EXPECT_EQ(M.rfind(std::string("_GPTo") + Want + "N4mainE1VL", 0), 0u)
        << R.Op << "/" << R.N << ": " << M;
    DemangleResult D = demangle(M);
    EXPECT_TRUE(D.Ok) << M << ": " << D.Error;
    EXPECT_TRUE(StringRef(D.Text).ends_with(") @Right")) << D.Text;
    EXPECT_NE(M, mangle(C.method(V, R.Op, C.tuple(Ps))));
  }
  // The overloads of one operator are told apart by their parameters, and the
  // unary and the binary `-` too.
  EXPECT_NE(mangle(C.method(V, "*", C.tuple({F}))),
            mangle(C.method(V, "*", C.tuple({V}))));
  EXPECT_NE(Name("-", 0), Name("-", 1));
  // An ordinary method called op_add is another thing than `+`.
  EXPECT_NE(mangle(C.method(V, "op_add")),
            mangle(C.method(V, "+", C.tuple({F}))));
  EXPECT_EQ(mangle(C.method(V, "op_add")), "_GPM6op_addN4mainE1V");
  // Names that are not in the table are refused.
  Context C2;
  ParseError Err;
  for (const char *Bad :
       {"_GPTo6op_bogN4mainE1VLE", "_GPTo3addN4mainE1VLE",
        "_GPTo2opN4mainE1VLE", "_GPTo3oprN4mainE1VLE",
        "_GPTo7oprangeN4mainE1VLE", "_GPTo7opr_bogN4mainE1VLE",
        "_GPTo7opy_addN4mainE1VLE",
        // `+` with one parameter has a row: opx_ / oprx_ are not canonical
        "_GPTo10opx_$2b$N4mainE1VLB16_E", "_GPTo11oprx_$2b$N4mainE1VLB17_E"})
    EXPECT_FALSE(parseSymbol(C2, Bad, Err)) << Bad;
}

TEST(SymbolName, ReorderedChildrenOfSetsGiveOneName) {
  // Interface methods and unit dimensions are sets: the order they are
  // given in does not matter.
  Context C;
  const Node *Sig = C.signature(C.tuple({}), C.tuple({}));
  const Node *I1 =
      C.interfaceOf({C.imethod("B", "", Sig), C.imethod("A", "", Sig),
                     C.imethod("c", "p", Sig)});
  const Node *I2 =
      C.interfaceOf({C.imethod("c", "p", Sig), C.imethod("A", "", Sig),
                     C.imethod("B", "", Sig)});
  EXPECT_EQ(I1, I2);
  const Node *U1 = C.unit("1", "1",
                          {C.dim("u", "S", -1), C.dim("u", "M", 1),
                           C.paramDim(1, 2), C.paramDim(0, 1)});
  const Node *U2 = C.unit("1", "1",
                          {C.paramDim(0, 1), C.paramDim(1, 2),
                           C.dim("u", "M", 1), C.dim("u", "S", -1)});
  EXPECT_EQ(U1, U2);
}

//===----------------------------------------------------------------------===//
// No collision with C
//===----------------------------------------------------------------------===//

TEST(SymbolName, NeverCollidesWithACName) {
  // Every name has the reserved prefix; the prefix is not a name a C
  // program (or any other mangling) can have: C reserves `_` + capital for
  // the implementation, and C++ uses `_Z`, Rust `_R`, Swift `$s`.
  EXPECT_TRUE(isReservedName("_GP"));
  EXPECT_TRUE(isReservedName("_GPF4mainE3Foo"));
  EXPECT_FALSE(isReservedName("main"));
  EXPECT_FALSE(isReservedName("_G"));
  EXPECT_FALSE(isReservedName("_Z3foov"));
  EXPECT_FALSE(isReservedName("_R"));
  EXPECT_FALSE(isReservedName("GP"));
  Context C;
  Gen Gn(C, 4242);
  for (int I = 0; I < 2000; ++I) {
    std::string M = mangle(Gn.entity(3).N);
    EXPECT_TRUE(isReservedName(M));
    EXPECT_FALSE(StringRef(M).starts_with("_Z"));
    EXPECT_FALSE(StringRef(M).starts_with("_R"));
    EXPECT_FALSE(StringRef(M).starts_with("$s"));
  }
  // The names C code and well-known runtimes use: none is a Go++ symbol, and
  // the demangler leaves them alone.
  for (const char *CName :
       {"main", "printf", "_start", "__libc_start_main", "_Z3foov",
        "_ZN3foo3barEv", "_RNvCs1234_7mycrate3foo", "$s4main3fooyyF",
        "_GLOBAL_OFFSET_TABLE_", "_G", "GP", "gopp_main", "_gp",
        "_Gp4mainE3Foo", "runtime.main", "_IO_stdin_used", "__GP_x"}) {
    EXPECT_FALSE(isReservedName(CName)) << CName;
    EXPECT_FALSE(demangle(CName).Ok) << CName;
  }
  // `_GP` itself and anything that is not a whole symbol is not accepted.
  EXPECT_FALSE(demangle("_GP").Ok);
  EXPECT_FALSE(demangle("").Ok);
  // A C name is made of the symbol characters of a name only by accident;
  // random C identifiers never start with the reserved prefix unless they are
  // reserved: check the checker's rule on a set of generated identifiers.
  std::mt19937_64 R(77);
  for (int I = 0; I < 5000; ++I) {
    std::string Name;
    size_t Len = 1 + R() % 12;
    for (size_t K = 0; K < Len; ++K)
      Name.push_back(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789"
              [R() % 63]);
    if (isdigit(static_cast<unsigned char>(Name[0])))
      Name[0] = '_';
    if (isReservedName(Name)) {
      EXPECT_TRUE(StringRef(Name).starts_with("_GP"));
      // such a C name is refused by the front end: the demangler too unless
      // it happens to be a valid symbol (then it is one of ours).
      DemangleResult D = demangle(Name);
      if (D.Ok) {
        Context C2;
        ParseError E;
        const Node *N = parseSymbol(C2, Name, E);
        ASSERT_TRUE(N);
        EXPECT_EQ(mangle(N), Name);
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Malformed input
//===----------------------------------------------------------------------===//

/// Whatever the text, the demangler returns; if it accepts, the text is
/// canonical (mangle(parse(S)) == S) and the readable form exists.
void expectCleanVerdict(StringRef S) {
  Context C;
  ParseError Err;
  const Node *N = parseSymbol(C, S, Err);
  if (N) {
    EXPECT_EQ(mangle(N), S.str());
    DemangleResult D = demangle(S, 1 << 20);
    // The only way to fail after a good parse is the output limit.
    EXPECT_TRUE(D.Ok || D.Error.find("limit") != std::string::npos);
  } else {
    EXPECT_FALSE(Err.Message.empty());
    EXPECT_LE(Err.Pos, S.size());
    EXPECT_FALSE(demangle(S).Ok);
  }
}

TEST(SymbolName, EveryPrefixOfAValidNameIsRejected) {
  Context C;
  Gen Gn(C, 31337);
  for (int I = 0; I < 300; ++I) {
    G E = Gn.entity(3);
    std::string M = mangle(E.N);
    if (M.size() > 600)
      continue;
    // The grammar is prefix free: no proper prefix is a name.
    for (size_t Len = 0; Len < M.size(); ++Len) {
      Context C2;
      ParseError Err;
      EXPECT_FALSE(parseSymbol(C2, StringRef(M).substr(0, Len), Err))
          << M.substr(0, Len);
    }
    // Trailing text is refused too.
    Context C2;
    ParseError Err;
    EXPECT_FALSE(parseSymbol(C2, M + "i", Err));
    EXPECT_FALSE(parseSymbol(C2, M + "E", Err));
  }
}

TEST(SymbolName, MutatedNamesNeverCrash) {
  Context C;
  Gen Gn(C, 8675309);
  std::mt19937_64 R(309);
  const std::string Alphabet =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$\xff";
  std::vector<std::string> Seeds;
  for (int I = 0; I < 150; ++I)
    Seeds.push_back(mangle(Gn.entity(3).N));
  size_t Accepted = 0, Total = 0;
  for (const std::string &Seed : Seeds) {
    for (int K = 0; K < 60; ++K) {
      std::string S = Seed;
      int Edits = 1 + static_cast<int>(R() % 3);
      for (int E = 0; E < Edits && !S.empty(); ++E) {
        size_t At = R() % S.size();
        switch (R() % 5) {
        case 0:
          S[At] = Alphabet[R() % Alphabet.size()];
          break;
        case 1:
          S.insert(S.begin() + static_cast<long>(At),
                   Alphabet[R() % Alphabet.size()]);
          break;
        case 2:
          S.erase(At, 1 + R() % 3);
          break;
        case 3: {
          size_t To = R() % S.size();
          S.insert(At, S.substr(To, 1 + R() % 8));
          break;
        }
        default:
          S.resize(At);
          break;
        }
      }
      ++Total;
      Context C2;
      ParseError Err;
      if (parseSymbol(C2, S, Err))
        ++Accepted;
      expectCleanVerdict(S);
    }
  }
  EXPECT_GT(Total, 5000u);
  (void)Accepted;
}

TEST(SymbolName, RandomTextNeverCrashes) {
  std::mt19937_64 R(2718);
  const std::string Alphabet =
      "bialsxjhtmyzfdcerpPSAHOVWXLGZqYnNguIQkUowBFMTRCJKvecD_$E0123456789";
  for (int I = 0; I < 20000; ++I) {
    std::string S = "_GP";
    size_t Len = R() % 40;
    for (size_t K = 0; K < Len; ++K)
      S.push_back(Alphabet[R() % Alphabet.size()]);
    expectCleanVerdict(S);
  }
}

TEST(SymbolName, HostileShapes) {
  auto Rejected = [](const std::string &S) {
    Context C;
    ParseError Err;
    EXPECT_FALSE(parseSymbol(C, S, Err)) << S.substr(0, 80);
    EXPECT_FALSE(Err.Message.empty());
    EXPECT_FALSE(demangle(S).Ok);
  };
  // Lengths and numbers that overflow.
  Rejected("_GPF99999999999999999999999E1a");
  Rejected("_GPF4294967296E1a");
  Rejected("_GPF18446744073709551616_E1a");
  Rejected("_GPF1000000000000000000000000000000000E1a");
  Rejected("_GPC18446744073709551616_F1pE1a");
  Rejected("_GPTiA99999999999999999999_i");
  // Leading zeros, missing separators, bad escapes.
  Rejected("_GPF01pE1a");
  Rejected("_GPF1pE02_ab");
  Rejected("_GPF1pE3$$");
  Rejected("_GPF1pE3$zz$");
  Rejected("_GPF1pE5$x1$a");
  Rejected("_GPF1pE4$d800$");     // surrogate
  Rejected("_GPF1pE9$110000$");   // beyond Unicode
  Rejected("_GPF1pE3a-b");        // character that must be escaped
  Rejected("_GPF1pE4$2B$");       // uppercase hex is not canonical
  Rejected("_GPF1pE5$0061$");     // escape of a plain letter is not canonical
  Rejected("_GPF1pE4$xc3$$xa9$"); // two raw bytes that make one letter
  Rejected("_GPF0E1a");           // path without segments
  Rejected("_GPF1pE");            // no name
  Rejected("_GPF1pE0");           // name `0` needs the `_`
  // Bad structure.
  Rejected("_GPE");
  Rejected("_GPI");
  Rejected("_GPIF1pE1aE");         // instance without arguments
  Rejected("_GPIN1pE1aiE");        // instance of a type is not an entity
  Rejected("_GPIIF1pE1aiEiE");     // instance of an instance
  Rejected("_GPTiF1pE1a");         // a type table of a function
  Rejected("_GPTCi");              // a thunk of a type
  Rejected("_GPMi1a");             // a method of a basic type
  Rejected("_GPM1aPN1pE1b");       // a method of a pointer type
  Rejected("_GPTo1+N1pE1aN1pE1a"); // operator method without a tuple
  Rejected("_GPUF1pE1a");
  Rejected("_GPTiUzk1_1_E");       // a unit on uintptr
  Rejected("_GPTiUbk1_1_E");       // a unit on bool
  Rejected("_GPTiUik1_1_N1pE1aE"); // a type inside a unit
  Rejected("_GPTik1_1_o1pE1aE");   // a dimension without exponent
  Rejected("_GPTiG9_0_LELE");      // bad signature flag
  Rejected("_GPTiG2_0_LELE");   // no `!` flag: func!(P) R is func(P) (R, error)
  Rejected("_GPTiG3_0_LSiELE"); // the same with variadic
  Rejected("_GPTiZ0_q256_1aiE"); // bad field flag
  Rejected("_GPTiG0_0_iLE");     // signature of a basic type and a tuple
  Rejected("_GPTiZ2_E");         // bad strict flag
  Rejected("_GPTiZq0_1ai E");
  Rejected("_GPTiXxi"); // bad channel direction
  // Unsorted sets.
  Rejected("_GPTiYn01bG0_0_LELEn01aG0_0_LELEE");
  Rejected("_GPTik1_1_o1pE1b1_o1pE1a1_E");
  // A unit as the checker never writes it (UNI-5, UNI-6): exponent 0, a
  // dimension twice, a bad exponent. (R1, R2.)
  Rejected("_GPTiUdk1_1_o1pE1x0_E");
  Rejected("_GPTiUdk1_1_o1pE1xn0_E");
  Rejected("_GPTiUdk1_1_w0_0_E");
  Rejected("_GPTiUdk1_1_o1pE1x1_o1pE1x1_E");
  Rejected("_GPTiUdk1_1_o1pE1x1_o1pE1x2_E");
  Rejected("_GPTiUdk1_1_w0_1_w0_1_E");
  Rejected("_GPTiUdk1_1_o1pE1xn9223372036854775808_E");
  Rejected("_GPTiUdk1_1_o1pE1x9223372036854775807_E");
  Rejected("_GPTiUdk1_1_o1pE1x1048577_E");
  Rejected("_GPTiUdk1_1_o1pE1xn1048577_E");
  Rejected("_GPTiUdk1_1_w0_1048577_E");
  // Back references to nowhere, to themselves, forward, to an open node.
  Rejected("_GPTiB0_");
  Rejected("_GPTiB3_");
  Rejected("_GPTiPB3_");
  Rejected("_GPTiHiB4_");
  Rejected("_GPTiB99999999999999999999_");
  Rejected("_GPB3_");
  // A back reference where the mangler would write the subtree again, and
  // the subtree written again where the mangler would use a reference.
  {
    Context C;
    const Node *T = C.named("main", "LongTypeName");
    const Node *M = C.map(T, T);
    std::string Good = mangle(C.ofType(Kind::TypeInfo, M));
    ParseError Err;
    Context C2;
    EXPECT_TRUE(parseSymbol(C2, Good, Err)) << Err.Message;
    EXPECT_NE(Good.find("B"), std::string::npos);
    std::string Expanded = "_GPTiHN4mainE12LongTypeNameN4mainE12LongTypeName";
    Rejected(Expanded);
    // The same with a reference to an offset that is not the start of a type.
    Rejected("_GPTiHN4mainE12LongTypeNameB7_");
  }
}

TEST(SymbolName, UnitExponentsAtTheLimit) {
  // Both sides (R2): the largest exponents the checker keeps round-trip and
  // print with the right sign; one more is refused.
  auto Check = [](const std::string &S, const char *Readable) {
    Context C;
    ParseError Err;
    const Node *N = parseSymbol(C, S, Err);
    ASSERT_TRUE(N) << S << ": " << Err.Message;
    EXPECT_EQ(mangle(N), S);
    EXPECT_EQ(demangle(S).Text, Readable);
  };
  Check("_GPTiUdk1_1_o1pE1x1048576_E", "typeinfo for float64[p.x^1048576]");
  Check("_GPTiUdk1_1_o1pE1xn1048576_E", "typeinfo for float64[1/p.x^1048576]");
  Check("_GPTiUdk1_1_o1pE1xn1_o1pE1y2_E", "typeinfo for float64[p.y^2/p.x]");
  Check("_GPTiUdk1_1_w0_n1048576_E", "typeinfo for float64[1/$u0^1048576]");
  Check("_GPTiUdk1_1_o1pE1x1_o1pE1y1_E", "typeinfo for float64[p.x*p.y]");
}

/// The cells of a Markdown table row, trimmed and without backticks.
std::vector<std::string> cells(StringRef Line) {
  std::vector<std::string> Out;
  Line = Line.trim();
  if (!Line.starts_with("|"))
    return Out;
  Line = Line.drop_front();
  while (!Line.empty()) {
    auto [Cell, Rest] = Line.split('|');
    std::string C = Cell.trim().str();
    if (C.size() >= 2 && C.front() == '`' && C.back() == '`')
      C = C.substr(1, C.size() - 2);
    Out.push_back(C);
    Line = Rest;
  }
  return Out;
}

TEST(SymbolName, TheAppendixIsTheCode) {
  // docs/spec/appendix-a-symbol-names.md: the tag table lists every kind
  // with the tag the code writes, and every example row is what the mangler
  // and the demangler do.
  auto Buf =
      llvm::MemoryBuffer::getFile(GOPP_SPEC_DIR "/appendix-a-symbol-names.md");
  ASSERT_TRUE(static_cast<bool>(Buf));
  std::set<std::string> Kinds;
  size_t Examples = 0;
  bool InTags = false;
  StringRef Text = (*Buf)->getBuffer();
  while (!Text.empty()) {
    auto [Line, Rest] = Text.split('\n');
    Text = Rest;
    if (Line.starts_with("| Tag |")) {
      InTags = true;
      continue;
    }
    if (!Line.starts_with("|")) {
      InTags = false;
      continue;
    }
    std::vector<std::string> C = cells(Line);
    if (C.size() < 2)
      continue;
    if (InTags && C[0] != "---") {
      bool Found = false;
      for (unsigned K = 0; K < static_cast<unsigned>(Kind::NumKinds); ++K)
        if (C[1] == kindName(static_cast<Kind>(K))) {
          Found = true;
          EXPECT_EQ(C[0], kindPrefix(static_cast<Kind>(K))) << C[1];
          Kinds.insert(C[1]);
        }
      EXPECT_TRUE(Found) << "the appendix has a kind the code does not: "
                         << C[1];
    } else if (StringRef(C[0]).starts_with("_GP")) {
      DemangleResult D = demangle(C[0]);
      EXPECT_TRUE(D.Ok) << C[0] << ": " << D.Error;
      EXPECT_EQ(D.Text, C[1]) << C[0];
      ++Examples;
    }
  }
  // Every kind except the back reference (a tag of its own, listed too).
  EXPECT_EQ(Kinds.size(), static_cast<size_t>(Kind::NumKinds));
  EXPECT_GE(Examples, 21u);
}

TEST(SymbolName, TheFuzzSeedsAreValidNames) {
  // tests/fuzz/symbol-corpus holds the seeds of the libFuzzer target
  // (gopp-fuzz-symbol): all valid names, every one of the kinds the grammar
  // has; the property the fuzzer checks holds for them and for every proper
  // prefix.
  std::error_code EC;
  size_t Seen = 0;
  for (llvm::sys::fs::directory_iterator It(GOPP_FUZZ_SYMBOL_DIR, EC), End;
       It != End && !EC; It.increment(EC)) {
    auto Buf = llvm::MemoryBuffer::getFile(It->path());
    ASSERT_TRUE(static_cast<bool>(Buf)) << It->path();
    StringRef Text = (*Buf)->getBuffer();
    DemangleResult D = demangle(Text);
    EXPECT_TRUE(D.Ok) << It->path() << ": " << D.Error;
    expectCleanVerdict(Text);
    for (size_t Len = 0; Len < Text.size(); ++Len)
      EXPECT_FALSE(demangle(Text.substr(0, Len)).Ok)
          << It->path() << " " << Len;
    ++Seen;
  }
  EXPECT_GE(Seen, 30u);
}

TEST(SymbolName, ExponentialSharingStaysSmall) {
  // T(n+1) = Pair[T(n), T(n)]: written out it has 2^n leaves; written with
  // back references it is linear, and the readable form is cut at the limit.
  Context C;
  const Node *T = C.basic(Kind::Int);
  for (int I = 0; I < 60; ++I)
    T = C.instance(C.named("main", "Pair"), {T, T});
  const Node *E = C.ofType(Kind::TypeInfo, T);
  std::string M = mangle(E);
  EXPECT_LT(M.size(), 60u * 40);
  ParseError Err;
  Context C2;
  const Node *Back = parseSymbol(C2, M, Err);
  ASSERT_TRUE(Back) << Err.Message << " at " << Err.Pos;
  EXPECT_EQ(mangle(Back), M);
  DemangleResult D = demangle(M, 1 << 16);
  EXPECT_FALSE(D.Ok);
  EXPECT_NE(D.Error.find("limit"), std::string::npos);
  EXPECT_LE(D.Text.size(), (1u << 16) + 8);
  // A shallow one prints in full and is checked against the closed form.
  const Node *S = C.basic(Kind::Int);
  std::string Expect = "int";
  for (int I = 0; I < 6; ++I) {
    S = C.instance(C.named("main", "Pair"), {S, S});
    Expect = "main.Pair[" + Expect + ", " + Expect + "]";
  }
  EXPECT_EQ(demangle(mangle(C.ofType(Kind::TypeInfo, S))).Text,
            "typeinfo for " + Expect);
}

//===----------------------------------------------------------------------===//
// Speed: a name 100000 levels deep, 100000 arguments, a 1 MB identifier
//===----------------------------------------------------------------------===//

template <class F> double seconds(F Fn) {
  auto T0 = std::chrono::steady_clock::now();
  Fn();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - T0)
      .count();
}

/// The least of three timings: a busy machine only ever slows a run down.
template <class F> double bestOf3(F Fn) {
  return std::min({seconds(Fn), seconds(Fn), seconds(Fn)});
}

/// Time of mangle + parse + print of a name built by Make(N).
template <class Make>
double fullCycle(Make Mk, size_t N, bool Prints = true, size_t *Len = nullptr) {
  Context C;
  const Node *E = Mk(C, N);
  std::string M;
  double T = bestOf3([&] {
    M = mangle(E);
    Context C2;
    ParseError Err;
    const Node *Back = parseSymbol(C2, M, Err);
    EXPECT_TRUE(Back) << Err.Message;
    if (Prints) {
      Printed P = print(Back, size_t(1) << 30);
      EXPECT_TRUE(P.Ok);
    }
  });
  if (Len)
    *Len = M.size();
  return T;
}

TEST(SymbolName, DeepAndWideNamesWork) {
  // Depth 200000: nothing recurses.
  Context C;
  const Node *T = C.basic(Kind::Int);
  for (int I = 0; I < 200000; ++I)
    T = (I % 3 == 0)   ? C.pointer(T)
        : (I % 3 == 1) ? C.slice(T)
                       : C.instance(C.named("p", "Box"), {T});
  const Node *E = C.ofType(Kind::TypeInfo, T);
  std::string M = mangle(E);
  Context C2;
  ParseError Err;
  const Node *Back = parseSymbol(C2, M, Err);
  ASSERT_TRUE(Back) << Err.Message;
  EXPECT_EQ(mangle(Back), M);
  Printed P = print(Back, size_t(1) << 30);
  EXPECT_TRUE(P.Ok);
  // The same deep text with its end cut off is refused, not run into.
  Context C3;
  EXPECT_FALSE(parseSymbol(C3, StringRef(M).drop_back(1), Err));
  // 200000 opening tags and nothing else.
  std::string Open = "_GPTi" + std::string(200000, 'P');
  EXPECT_FALSE(parseSymbol(C3, Open, Err));
  EXPECT_EQ(Err.Message, "the text ends before the name is complete");
}

/// R1: text made to cost more than it is long. Each shape at n and 4n: the
/// time may grow less than 8 times (linear is 4, quadratic is 16). The
/// readable form is capped at 1 MB, so a shape that prints a lot is cut at
/// the same size for both and its time stays flat.
/// Timing check, so it runs in the long suite (`symbol-names.long.test`):
/// at the normal suite's size the runs took a millisecond and failed on noise
/// when 192 tests ran at once. At 40000 the small run takes milliseconds to
/// tens of milliseconds.
TEST(SymbolNameLong, DISABLED_HostileTextCostsLinearTime) {
  struct Shape {
    const char *Name;
    bool Valid; // the parser accepts it (else it must refuse at once)
    std::function<std::string(size_t)> Make;
  };
  auto Dim = [](size_t I, const char *Exp) {
    char Buf[32];
    std::snprintf(Buf, sizeof Buf, "o1pE7d%06zu%s_", I, Exp);
    return std::string(Buf);
  };
  auto Repeat = [](size_t N, const std::string &Piece) {
    std::string S;
    for (size_t I = 0; I < N; ++I)
      S += Piece;
    return S;
  };
  const Shape Shapes[] = {
      // The shape of R1 (exponent 0): refused at once.
      {"dimensions to the power 0, repeated", false,
       [&](size_t N) {
         std::string S = "_GPTiLUdk1_1_";
         for (size_t I = 0; I < N; ++I)
           S += "o1pE1x0_";
         return S + "E" + Repeat(N, "B6_") + "E";
       }},
      // The same with the dimension twice.
      {"the same dimension many times", false,
       [&](size_t N) {
         return "_GPTiLUdk1_1_" + Repeat(N, "o1pE1x1_") + "E" +
                Repeat(N, "B6_") + "E";
       }},
      // Valid: many dimensions, the unit repeated by back references.
      {"many dimensions, repeated", true,
       [&](size_t N) {
         std::string S = "_GPTiLUdk1_1_";
         for (size_t I = 0; I < N; ++I)
           S += Dim(I, "1");
         return S + "E" + Repeat(N, "B6_") + "E";
       }},
      // Valid: a type that prints little, repeated.
      {"an empty struct in an array, repeated", true,
       [&](size_t N) {
         std::string S = "_GPTiLA9_A9_Z0_E";
         return S + Repeat(N, "B6_") + "E";
       }},
      // Valid: a long tuple of short types.
      {"a wide tuple of empty structs", true,
       [&](size_t N) { return "_GPTiL" + Repeat(N, "Z0_E") + "E"; }},
  };
  for (const Shape &S : Shapes) {
    size_t N = 40000;
    auto Time = [&](size_t Size) {
      std::string Text = S.Make(Size);
      Context C;
      ParseError Err;
      EXPECT_EQ(parseSymbol(C, Text, Err) != nullptr, S.Valid) << S.Name;
      return bestOf3([&] { (void)demangle(Text, size_t(1) << 20); });
    };
    double Small = Time(N), Big = Time(N * 4);
    double Ratio = Big / std::max(Small, 1e-3);
    std::fprintf(stderr, "  %-40s %8.4fs -> %8.4fs  (x%.1f for x4)\n", S.Name,
                 Small, Big, Ratio);
    EXPECT_LT(Ratio, 8) << S.Name << ": " << Small << "s -> " << Big << "s";
  }
}

/// A sanitizer build is about five times slower: the long suite does a fifth
/// of the work there (the full count runs in the plain build).
#if defined(__SANITIZE_ADDRESS__)
constexpr int Slow = 5;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int Slow = 5;
#else
constexpr int Slow = 1;
#endif
#else
constexpr int Slow = 1;
#endif

TEST(SymbolNameLong, DISABLED_TheTimingCheckSeesAQuadraticWalk) {
  // The control of the next test: a quadratic loop must fail its limit.
  auto Quadratic = [](size_t N) {
    return seconds([&] {
      std::string S;
      for (size_t I = 0; I < N; ++I)
        S.insert(0, "xy");
      EXPECT_EQ(S.size(), 2 * N);
    });
  };
  double Ratio = Quadratic(64000 * 4) / std::max(Quadratic(4000 * 4), 1e-4);
  EXPECT_GT(Ratio, 16 * 6);
}

TEST(SymbolNameLong, DISABLED_NearLinearOnLongNames) {
  struct Shape {
    const char *Name;
    const Node *(*Make)(Context &, size_t);
    bool Prints; // false: the readable form is astronomically long
  } Shapes[] = {
      {"deep pointers",
       [](Context &C, size_t N) {
         const Node *T = C.basic(Kind::Int);
         for (size_t I = 0; I < N; ++I)
           T = C.pointer(T);
         return C.ofType(Kind::TypeInfo, T);
       },
       true},
      {"nested instances",
       [](Context &C, size_t N) {
         const Node *T = C.basic(Kind::Int);
         for (size_t I = 0; I < N; ++I)
           T = C.instance(C.named("main", "Box"), {T});
         return C.ofType(Kind::TypeInfo, T);
       },
       true},
      {"many arguments",
       [](Context &C, size_t N) {
         std::vector<const Node *> Args;
         for (size_t I = 0; I < N; ++I)
           Args.push_back(C.named("main", "T" + std::to_string(I)));
         return C.instance(C.func("main", "F"), Args);
       },
       true},
      {"long identifier",
       [](Context &C, size_t N) {
         std::string S;
         for (size_t I = 0; I < N; ++I)
           S += (I % 7 == 0) ? "é" : "x";
         return C.func("main", S);
       },
       true},
      {"closure chain",
       [](Context &C, size_t N) {
         const Node *E = C.func("main", "f");
         for (size_t I = 0; I < N; ++I)
           E = C.closure(E, 1 + I % 3);
         return E;
       },
       true},
      {"long package path",
       [](Context &C, size_t N) {
         std::string S = "a";
         for (size_t I = 0; I < N; ++I)
           S += "/seg";
         return C.func(S, "f");
       },
       true},
      {"many unit dimensions",
       [](Context &C, size_t N) {
         std::vector<const Node *> Dims;
         for (size_t I = 0; I < N; ++I)
           Dims.push_back(C.dim("u", "D" + std::to_string(I), 1 + I % 3));
         return C.ofType(Kind::TypeInfo,
                         C.unitType(Kind::Float64, C.unit("1", "1", Dims)));
       },
       true},
      {"shared halves",
       [](Context &C, size_t N) {
         const Node *T = C.basic(Kind::Int);
         for (size_t I = 0; I < N; ++I)
           T = C.map(T, T);
         return C.ofType(Kind::TypeInfo, T);
       },
       false},
  };
  for (const Shape &S : Shapes) {
    // 16000, not 4000: a run of a millisecond is mostly noise.
    size_t N = 16000;
    double Small = fullCycle(S.Make, N, S.Prints);
    double Big = fullCycle(S.Make, N * 16, S.Prints);
    // 16 times the size: a linear walk takes ~16 times as long. A quadratic
    // one 256 times. Leave room for noise.
    double Ratio = Big / std::max(Small, 1e-4);
    std::fprintf(stderr, "  %-22s %8.4fs -> %8.4fs  (x%.1f for x16)\n", S.Name,
                 Small, Big, Ratio);
    EXPECT_LT(Ratio, 16 * 6)
        << S.Name << ": " << Small << "s -> " << Big << "s";
    EXPECT_LT(Big, 5.0) << S.Name;
  }
}

//===----------------------------------------------------------------------===//
// The long suite (tests/driver/symbol-names.long.test)
//===----------------------------------------------------------------------===//

TEST(SymbolNameLong, DISABLED_AMillionLevelsDeep) {
  Context C;
  const Node *T = C.basic(Kind::Int);
  for (int I = 0; I < 1000000 / Slow; ++I)
    T = (I % 2) ? C.pointer(T) : C.instance(C.named("p", "Box"), {T});
  const Node *E = C.ofType(Kind::TypeInfo, T);
  std::string M = mangle(E);
  Context C2;
  ParseError Err;
  const Node *Back = parseSymbol(C2, M, Err);
  ASSERT_TRUE(Back) << Err.Message;
  EXPECT_EQ(mangle(Back), M);
  EXPECT_TRUE(print(Back, size_t(1) << 30).Ok);
  EXPECT_FALSE(parseSymbol(C2, StringRef(M).drop_back(2), Err));
}

TEST(SymbolNameLong, DISABLED_MutationFuzz) {
  Context C;
  Gen Gn(C, 424242);
  std::mt19937_64 R(4242);
  const std::string Tags =
      "bialsxjhtmyzfdcerpPSAHOVWXLGZqYnNguIQkUowBFMTRCJKvD_$E0123456789abcdefx";
  std::vector<std::string> Seeds;
  for (int I = 0; I < 400; ++I)
    Seeds.push_back(mangle(Gn.entity(3).N));
  size_t Accepted = 0;
  for (int Iter = 0; Iter < 1500000 / Slow; ++Iter) {
    std::string S = Seeds[R() % Seeds.size()];
    int Edits = 1 + static_cast<int>(R() % 4);
    for (int E = 0; E < Edits && S.size() > 3; ++E) {
      size_t At = 3 + R() % (S.size() - 3);
      switch (R() % 7) {
      case 0:
        S[At] = Tags[R() % Tags.size()];
        break;
      case 1:
        S.insert(S.begin() + static_cast<long>(At), Tags[R() % Tags.size()]);
        break;
      case 2:
        S.erase(At, 1 + R() % 4);
        break;
      case 3:
        S.insert(At, S.substr(3 + R() % (S.size() - 3), 1 + R() % 12));
        break;
      case 4: { // splice with another name
        const std::string &O = Seeds[R() % Seeds.size()];
        S = S.substr(0, At) + O.substr(3 + R() % (O.size() - 3));
        break;
      }
      case 5: // change a digit (a length or a number)
        for (size_t K = At; K < S.size(); ++K)
          if (S[K] >= '0' && S[K] <= '9') {
            S[K] = static_cast<char>('0' + R() % 10);
            break;
          }
        break;
      default:
        S.resize(At);
        break;
      }
    }
    Context C2;
    ParseError Err;
    if (parseSymbol(C2, S, Err))
      ++Accepted;
    expectCleanVerdict(S);
  }
  // Some mutants stay valid names (a swapped letter, say): the accepting
  // path is fuzzed as well as the rejecting one.
  EXPECT_GT(Accepted, 100u / Slow);
}

TEST(SymbolNameLong, DISABLED_BigCorpusRoundTrips) {
  Context C;
  Gen Gn(C, 1234567);
  for (int I = 0; I < 40000 / Slow; ++I)
    roundTrip(C, Gn.entity(5));
}

} // namespace
