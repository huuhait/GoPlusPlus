//===- SemaSymbolsTest.cpp - Symbol names of checked code (P04.17) -------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The names of what the checker sees (functions, methods, variables, types,
// generic instances, unit types):
//   - the same whatever the order the packages were checked in and whatever
//     the number of body threads: four packages are loaded in many shuffled
//     orders, each into a fresh TypeContext, with -check-j 1 and 16, and every
//     name is compared byte for byte; every name round-trips through the
//     demangler;
//   - two types have one name exactly when the checker says they are
//     identical (a generated table of type pairs, the checker as the oracle);
//   - every method of a type has its own name (operator overloads, `@Right`,
//     instances of generic types);
//   - the readable name of a type is the type printer's text (TypeString), so
//     the two cannot drift apart, except where one name stands for several
//     spellings of one type (listed and tested both ways).
//
//===----------------------------------------------------------------------===//

#include "gopp/Mangle/SemaSymbols.h"
#include "gopp/Sema/Loader.h"
// The checker's own limit; Units.h is private to lib/Sema.
#include "../../../compiler/lib/Sema/Units.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <random>
#include <set>

using namespace gopp;
using namespace gopp::sema;
using namespace gopp::sym;

namespace {

class Tree {
public:
  Tree() {
    llvm::SmallString<128> P;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-symbols", P));
    Root = std::string(P);
  }
  ~Tree() { llvm::sys::fs::remove_directories(Root); }
  void write(const std::string &Rel, const std::string &Text) {
    std::string Path = Root + "/" + Rel;
    llvm::sys::fs::create_directories(llvm::sys::path::parent_path(Path));
    std::ofstream Out(Path);
    Out << Text;
  }
  std::string Root;
};

const char *const Units = R"(package units

@Symbol("m")
unit M
unit S
unit KG
unit KM = 1000 M
unit H = 3600 S
)";

const char *const Geo = R"(package geo

import "m/units"

type Box[T any] struct{ V T }

func (b Box[T]) Size() int      { return 1 }
func (b *Box[T]) Set(move v T)  { b.V = v }
func (b Box[T]) Each() int {
	g := func() int { return 1 }
	return g()
}

type Pair[K comparable, V any] struct {
	Key K
	Val V
}

func Map[T, U any](xs []T, f func(T) U) []U {
	var out []U
	for _, x := range xs {
		out = append(out, f(x))
	}
	return out
}

func Apply[T any](move x T, f func(T) T) T { return f(x) }

type Vec2 struct{ X, Y float64 }

func (a Vec2) + (b Vec2) Vec2     { return Vec2{a.X + b.X, a.Y + b.Y} }
func (a Vec2) * (s float64) Vec2  { return Vec2{a.X * s, a.Y * s} }
func (a Vec2) * (b Vec2) float64  { return a.X*b.X + a.Y*b.Y }
func (a Vec2) - () Vec2           { return Vec2{-a.X, -a.Y} }

type Speed = float64[units.KM/units.H]

type VecU[U unit] struct{ X, Y float64[U] }

func Scale[U unit](v VecU[U], k float64) VecU[U] { return v }

type Animal struct{ N int }

type Dog extends Animal { Extra int }

type Shape interface{ Area() float64 }

type Named interface {
	Shape
	Name() string
}

type Tagged struct {
	A int `json:"a"`
	b string
}

var Counter int
var Table map[string]Box[Pair[int, []string]]
)";

const char *const Extra = R"(package extra

import (
	"m/geo"
	"m/units"
)

type Holder struct {
	Items []geo.Box[geo.Pair[string, int]]
	Fast  float64[units.KM/units.H]
	Ptr   Option[*geo.Box[geo.Vec2]]
}

func (h Holder) Len() int { return len(h.Items) }

func Use() geo.VecU[units.KM] {
	var v geo.VecU[units.KM]
	return geo.Scale(v, 2)
}

func UseShape(s geo.Shape, n geo.Named) int { return 0 }
)";

const char *const App = R"(package main

import (
	"m/extra"
	"m/geo"
	"m/units"
)

type Local struct{ b geo.Box[geo.Pair[int, string]] }

var S geo.Box[geo.Pair[string, []int]]
var D geo.VecU[units.KM]
var M map[string]geo.Box[geo.Vec2]

func main() {
	x := geo.Map[int, string]([]int{1}, func(i int) string { return "" })
	y := geo.Apply(geo.Box[int]{}, func(b geo.Box[int]) geo.Box[int] { return b })
	v := geo.Scale(geo.VecU[units.KM/units.H]{}, 2)
	var sp geo.Speed
	h := extra.Holder{}
	_, _, _, _, _ = x, y, v, sp, h
	_ = extra.Use()
	_ = geo.Counter
	var a, b geo.Vec2
	_ = a + b
	_ = a * 2.0
	_ = -a
	var d geo.Dog
	_ = d
}
)";

void writeAll(Tree &T) {
  T.write("units/u.gopp", Units);
  T.write("geo/g.gopp", Geo);
  T.write("extra/e.gopp", Extra);
  T.write("app/a.gopp", App);
}

struct Names {
  std::set<std::string> Mangled;
  std::set<std::string> Readable;
  std::string Errors;
  size_t Skipped = 0;
};

/// The names of everything the packages loaded in this order contain.
Names collect(Tree &T, const std::vector<std::string> &Order,
              unsigned Threads) {
  Names R;
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  TypeContext Ctx;
  LoadOptions LO;
  LO.Root = T.Root;
  LO.ModulePath = "m";
  CheckOptions CO;
  CO.BodyThreads = Threads;
  CO.MinBodiesPerThread = 1;
  Loader L(SM, Diags, Ctx, LO, CO);
  for (const std::string &P : Order) {
    std::string E;
    L.import("m/" + P, E);
  }
  for (const Diagnostic &D : Collect.Diags)
    if (D.Sev == Severity::Error)
      R.Errors += D.Message + "\n";
  if (!R.Errors.empty())
    return R;

  Context C;
  SemaNames SN(C);
  auto add = [&](const Node *N) {
    if (!N) {
      ++R.Skipped;
      return;
    }
    if (!N->isEntity())
      return;
    R.Mangled.insert(mangle(N));
  };
  auto addType = [&](const sema::Type *Ty) {
    if (!Ty)
      return;
    const Node *N = SN.type(Ty);
    add(N ? C.ofType(Kind::TypeInfo, N) : nullptr);
    // The methods of an instance, by the declarations of its origin.
    if (auto *NT = llvm::dyn_cast<NamedType>(Ty))
      if (NT->Orig && NT->Orig->Methods)
        for (Func *M : *NT->Orig->Methods)
          add(SN.method(NT, M));
  };
  for (CheckedPackage *CP : L.packages()) {
    std::vector<Object *> Objs;
    for (auto &E : CP->Pkg->PkgScope->Elems)
      Objs.push_back(E.second);
    std::sort(Objs.begin(), Objs.end(),
              [](Object *A, Object *B) { return A->Name < B->Name; });
    for (Object *O : Objs) {
      if (auto *F = llvm::dyn_cast<Func>(O)) {
        add(SN.func(F));
      } else if (auto *V = llvm::dyn_cast<Var>(O)) {
        add(SN.var(V));
        addType(V->Ty);
      } else if (auto *TN = llvm::dyn_cast<TypeName>(O)) {
        addType(TN->Ty);
        if (auto *N = llvm::dyn_cast_or_null<NamedType>(TN->Ty))
          if (N->Methods)
            for (Func *M : *N->Methods)
              add(SN.func(M));
      }
    }
    // Every type the checker typed, and every generic instance.
    for (auto &E : CP->I.Types)
      addType(E.second.Ty);
    for (auto &E : CP->I.Instances) {
      const Instance &In = E.second;
      addType(In.Ty);
      const ast::Ident *Id = llvm::dyn_cast<ast::Ident>(E.first);
      if (auto *Sel = llvm::dyn_cast<ast::SelectorExpr>(E.first))
        Id = Sel->Sel;
      if (!Id)
        continue;
      auto U = CP->I.Uses.find(Id);
      if (U == CP->I.Uses.end())
        continue;
      if (auto *F = llvm::dyn_cast<Func>(U->second))
        add(SN.funcInstance(F, In.TArgs));
    }
  }
  for (const std::string &M : R.Mangled) {
    DemangleResult D = demangle(M);
    EXPECT_TRUE(D.Ok) << M << ": " << D.Error;
    R.Readable.insert(D.Text);
    // Round trip of the text.
    Context C2;
    ParseError Err;
    const Node *N = parseSymbol(C2, M, Err);
    EXPECT_TRUE(N) << M;
    if (N)
      EXPECT_EQ(mangle(N), M);
  }
  return R;
}

TEST(SemaSymbols, NamesDoNotDependOnCheckingOrderOrThreads) {
  Tree T;
  writeAll(T);
  Names Base = collect(T, {"app"}, 1);
  ASSERT_EQ(Base.Errors, "") << Base.Errors;
  // Not vacuous: the interesting names are there.
  for (const char *Want : {
           "m/geo.Map[int, string]",
           "m/geo.Apply[m/geo.Box[int]]",
           "m/geo.Scale[1000 m/units.M]",
           "m/geo.Scale[(5/18) m/units.M/m/units.S]",
           "m/geo.Box[int].Size",
           "m/geo.Box[m/geo.Pair[int, string]].Size",
           "m/geo.Vec2.+(m/geo.Vec2)",
           "m/geo.Vec2.*(float64)",
           "m/geo.Vec2.*(m/geo.Vec2)",
           "m/geo.Vec2.-()",
           "m/geo.Counter",
           "typeinfo for m/geo.Box[m/geo.Pair[string, []int]]",
           "typeinfo for m/geo.VecU[1000 m/units.M]",
           "typeinfo for float64[(5/18) m/units.M/m/units.S]",
           "typeinfo for m/geo.Dog",
           "typeinfo for map[string]m/geo.Box[m/geo.Vec2]",
       })
    EXPECT_TRUE(Base.Readable.count(Want)) << "missing: " << Want;
  EXPECT_GT(Base.Mangled.size(), 60u);

  std::vector<std::string> Order = {"units", "geo", "extra", "app"};
  std::mt19937_64 R(20261030);
  for (unsigned Round = 0; Round < 8; ++Round) {
    std::shuffle(Order.begin(), Order.end(), R);
    unsigned Threads = (Round % 2) ? 16 : 1;
    Names N = collect(T, Order, Threads);
    ASSERT_EQ(N.Errors, "") << N.Errors;
    // The packages that app does not import (all four are loaded here, so
    // `extra` and `units` add names): the names of the app run are a subset.
    for (const std::string &M : Base.Mangled)
      EXPECT_TRUE(N.Mangled.count(M))
          << M << " [order " << Order[0] << "," << Order[1] << "," << Order[2]
          << "," << Order[3] << " j=" << Threads << "]";
    // Same order, other thread count: byte for byte the same set.
    Names Other = collect(T, Order, Threads == 1 ? 16 : 1);
    EXPECT_EQ(N.Mangled, Other.Mangled);
    EXPECT_EQ(N.Skipped, Other.Skipped);
  }
  // Only app: j=1 against j=16.
  Names Wide = collect(T, {"app"}, 16);
  EXPECT_EQ(Base.Mangled, Wide.Mangled);
}

TEST(SemaSymbols, WhatHasNoSymbolSaysWhy) {
  Tree T;
  T.write("p/p.gopp", R"(package p

func Id[T any](move x T) T { return x }

type G[T any] struct{ v T }

func (g G[T]) Size() int { return 1 }

func Local() {
	type inner struct{ a int }
	var _ inner
}

func Sum[T interface{ + (int) int }](a T) {}

func init() {}
func init() {}
func _()    {}
func _()    {}

var _ = 1
var _ = "two"
var Kept = 3
)");
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags(SM, Collect, 0);
  TypeContext Ctx;
  LoadOptions LO;
  LO.Root = T.Root;
  LO.ModulePath = "m";
  Loader L(SM, Diags, Ctx, LO, {});
  CheckedPackage *CP = L.loadDir(T.Root + "/p", "m/p");
  ASSERT_TRUE(CP);
  ASSERT_TRUE(Collect.Diags.empty()) << Collect.Diags[0].Message;
  // One SemaNames for every call: why() is the reason of the last call,
  // also for a type whose failure was remembered from an earlier call.
  Context C;
  SemaNames SN(C);
  auto *Id = llvm::cast<Func>(CP->Pkg->PkgScope->lookupLocal("Id"));
  EXPECT_EQ(SN.func(Id), nullptr);
  EXPECT_NE(SN.why().find("generic"), std::string::npos) << SN.why();
  auto *G = llvm::cast<TypeName>(CP->Pkg->PkgScope->lookupLocal("G"));
  EXPECT_EQ(SN.type(G->Ty), nullptr);
  EXPECT_NE(SN.why().find("generic type"), std::string::npos) << SN.why();
  // A type declared inside a function has no symbol yet (the lowering knows
  // its function: P04.04, P04.07).
  const TypeName *Inner = nullptr;
  for (auto &E : CP->I.Defs)
    if (E.first->Name.str() == "inner")
      Inner = llvm::dyn_cast<TypeName>(E.second);
  ASSERT_TRUE(Inner);
  EXPECT_EQ(SN.type(Inner->Ty), nullptr);
  EXPECT_NE(SN.why().find("inside a function"), std::string::npos) << SN.why();
  // A slice of it fails for the same reason, from the remembered failure.
  EXPECT_EQ(SN.type(Ctx.slice(const_cast<Type *>(Inner->Ty))), nullptr);
  EXPECT_NE(SN.why().find("inside a function"), std::string::npos) << SN.why();
  EXPECT_TRUE(SN.type(Ctx.typ(BasicKind::Int)));
  EXPECT_EQ(SN.why(), "");
  EXPECT_EQ(SN.type(G->Ty), nullptr);
  EXPECT_NE(SN.why().find("generic type"), std::string::npos) << SN.why();
  // An interface with an operator element is a constraint, never a value.
  auto *Sum = llvm::cast<Func>(CP->Pkg->PkgScope->lookupLocal("Sum"));
  EXPECT_EQ(SN.type(Sum->sig()->TParams[0]->Bound), nullptr);
  EXPECT_NE(SN.why().find("operator"), std::string::npos) << SN.why();
  // A package may declare many `init` and `_` functions (they are not in the
  // package scope): one name for all of them would collide. An init function
  // is named by its ordinal (Context::initFunc), which the lowering counts; a
  // blank one is never emitted.
  unsigned Inits = 0, Blanks = 0;
  for (auto &E : CP->I.Defs) {
    auto *F = llvm::dyn_cast_or_null<Func>(E.second);
    if (!F || (F->Name != "init" && F->Name != "_"))
      continue;
    ++(F->Name == "init" ? Inits : Blanks);
    EXPECT_EQ(SN.func(F), nullptr) << F->Name.str();
    EXPECT_NE(SN.why().find(F->Name == "init" ? "initFunc" : "blank"),
              std::string::npos)
        << SN.why();
  }
  EXPECT_EQ(Inits, 2u);
  EXPECT_EQ(Blanks, 2u);
  // A blank package variable has no storage, and there may be many.
  unsigned BlankVars = 0;
  for (auto &E : CP->I.Defs) {
    auto *V = llvm::dyn_cast_or_null<Var>(E.second);
    if (!V || V->Name != "_" || !V->IsPackageLevel)
      continue;
    ++BlankVars;
    EXPECT_EQ(SN.var(V), nullptr);
    EXPECT_NE(SN.why().find("blank"), std::string::npos) << SN.why();
  }
  EXPECT_EQ(BlankVars, 2u);
  auto *Kept = llvm::cast<Var>(CP->Pkg->PkgScope->lookupLocal("Kept"));
  ASSERT_TRUE(SN.var(Kept)) << SN.why();
  EXPECT_EQ(print(SN.var(Kept)).Text, "m/p.Kept");
  auto *LocalFn = llvm::cast<Func>(CP->Pkg->PkgScope->lookupLocal("Local"));
  ASSERT_TRUE(SN.func(LocalFn)) << SN.why();
  EXPECT_EQ(print(SN.func(LocalFn)).Text, "m/p.Local");
  // The generic function with an explicit instance is named; without its
  // type arguments (or with too many) it is not the name of the generic code.
  const Node *I = SN.funcInstance(Id, {Ctx.typ(BasicKind::Int)});
  ASSERT_TRUE(I) << SN.why();
  EXPECT_EQ(print(I).Text, "m/p.Id[int]");
  EXPECT_EQ(SN.funcInstance(Id, {}), nullptr);
  EXPECT_NE(SN.why().find("type argument"), std::string::npos) << SN.why();
  EXPECT_EQ(
      SN.funcInstance(Id, {Ctx.typ(BasicKind::Int), Ctx.typ(BasicKind::Int)}),
      nullptr);
}

/// One package, checked from its source in a fresh context.
class Checked {
public:
  Checked(const std::string &Path, const std::string &Src) {
    T.write(Path + "/x.gopp", Src);
    LoadOptions LO;
    LO.Root = T.Root;
    L = std::make_unique<Loader>(SM, Diags, Ctx, LO, CheckOptions());
    CP = L->loadDir(T.Root + "/" + Path, Path);
  }
  Object *get(llvm::StringRef Name) const {
    return CP->Pkg->PkgScope->lookupLocal(Name);
  }
  NamedType *named(llvm::StringRef Name) const {
    return llvm::cast<NamedType>(get(Name)->Ty);
  }
  Func *func(llvm::StringRef Name) const { return llvm::cast<Func>(get(Name)); }
  /// The errors of the check, and a note if the package did not load.
  std::string errors() const {
    std::string E = CP ? "" : "the package did not load\n";
    for (const Diagnostic &D : Collect.Diags)
      if (D.Sev == Severity::Error)
        E += std::to_string(SM.getLineNumber(D.Loc)) + ": " + D.Message + "\n";
    return E;
  }

  Tree T;
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  DiagnosticsEngine Diags{SM, Collect, 0};
  TypeContext Ctx;
  std::unique_ptr<Loader> L;
  CheckedPackage *CP = nullptr;
};

TEST(SemaSymbols, OneNameExactlyWhenTheCheckerSaysIdentical) {
  // The checker is the oracle: `func tI_J(x *A) *B { return x }` type-checks
  // exactly when A and B are identical. Every pair of these spellings is
  // tried; several of them are one type written another way: byte and
  // uint8, `func!(int) string` and `func(int) (string, error)` (ERR-2), `any`
  // and `interface{}`, an interface with its methods in another order or
  // embedded, a unit written two ways (UNI-5), an instance of those. The
  // names agree: one node exactly for the identical pairs.
  static const char *const Spellings[] = {
      "int",
      "uint8",
      "byte",
      "int32",
      "rune",
      "float64",
      "string",
      "error",
      "any",
      "interface{}",
      "[]int",
      "[3]int",
      "[4]int",
      "map[string]int",
      "chan int",
      "<-chan int",
      "chan<- int",
      "chan (<-chan int)",
      "Option[*int]",
      "Option[int]",
      "*int",
      "func!(int) string",
      "func(int) (string, error)",
      "func(int) string",
      "func(...int)",
      "func([]int)",
      "func(move []int)",
      "func(move a []int, b []int)",
      "struct{ X int }",
      "struct{ X int @Required }",
      "struct{ X int `t` }",
      "struct{ X int; Y int }",
      "struct{ Y int; X int }",
      "struct{ x int }",
      "struct{ X Option[int] }",
      "interface{ A(); B() }",
      "interface{ B(); A() }",
      "interface{ A() }",
      "interface{ I1; B() }",
      "interface{ b() }",
      "interface{ Load!(p string) []byte }",
      "interface{ Load(p string) ([]byte, error) }",
      "float64[KM/H]",
      "float64[KM*H^-1]",
      "float64[M/S]",
      "float32[KM/H]",
      "int32[KM]",
      "float64[KM]",
      "float64[M]",
      "Unique[*int]",
      "Shared[*int]",
      "Box[int]",
      "Box[float64[KM/H]]",
      "Box[float64[KM*H^-1]]",
      "Box[func!(int) string]",
      "Box[func(int) (string, error)]",
  };
  constexpr size_t N = sizeof(Spellings) / sizeof(Spellings[0]);
  std::string Src = "package p\n\nunit M\nunit S\nunit KM = 1000 M\n"
                    "unit H = 3600 S\n\ntype I1 interface{ A() }\n\n"
                    "type Box[T any] struct{ V T }\n\n";
  unsigned Line =
      static_cast<unsigned>(std::count(Src.begin(), Src.end(), '\n'));
  std::map<unsigned, std::pair<size_t, size_t>> ByLine;
  for (size_t I = 0; I < N; ++I)
    for (size_t J = 0; J < N; ++J) {
      Src += "func t" + std::to_string(I) + "_" + std::to_string(J) + "(x *" +
             Spellings[I] + ") *" + Spellings[J] + " { return x }\n";
      ByLine[++Line] = {I, J};
    }
  Checked P("p", Src);
  ASSERT_TRUE(P.CP);
  std::set<std::pair<size_t, size_t>> Different;
  for (const Diagnostic &D : P.Collect.Diags) {
    if (D.Sev != Severity::Error)
      continue;
    auto It = ByLine.find(P.SM.getLineNumber(D.Loc));
    ASSERT_NE(It, ByLine.end()) << D.Message;
    Different.insert(It->second);
  }
  Context C;
  SemaNames SN(C);
  size_t Same = 0;
  for (size_t I = 0; I < N; ++I) {
    ASSERT_FALSE(Different.count({I, I})) << "not a type: " << Spellings[I];
    for (size_t J = 0; J < N; ++J) {
      Func *F = P.func("t" + std::to_string(I) + "_" + std::to_string(J));
      const Type *A = llvm::cast<PointerType>(F->sig()->Params->Elems[0])->Elem;
      const Type *B =
          llvm::cast<PointerType>(F->sig()->Results->Elems[0])->Elem;
      const Node *NA = SN.type(A);
      ASSERT_TRUE(NA) << Spellings[I] << ": " << SN.why();
      const Node *NB = SN.type(B);
      ASSERT_TRUE(NB) << Spellings[J] << ": " << SN.why();
      bool Identical = !Different.count({I, J});
      EXPECT_EQ(NA == NB, Identical)
          << Spellings[I] << " and " << Spellings[J] << ": " << mangleType(NA)
          << " " << mangleType(NB);
      if (Identical && I != J)
        ++Same;
    }
  }
  // Not vacuous: the list has one type written several ways.
  EXPECT_GE(Same, 2 * 10u);
}

TEST(SemaSymbols, StructIdentityHasEveryFieldMark) {
  // `@Strict` is written on a declaration only, so these structs are the
  // underlying types of declared types and of instances. The checker uniques
  // struct types with every mark of their identity (Types.h, TYP-15a): two
  // are identical exactly when they are one pointer. A required field of an
  // instance declared with a type parameter only means "must be set"
  // (RequiredSetOnly, ADR-0016 point 8) where that changes never-nil-ness, so
  // Box[error] has another struct than PE, while Box[int] has PB's.
  Checked P("p", R"(package p

@Strict
type SA struct{ X int }

@Strict
type SC struct{ X int }

@Strict
type SO struct{ X int @Option }

type SD struct{ X int @Required }

type SE struct{ X int }

type SF struct{ X *int }

@Strict
type SG struct{ X *int }

type Box[T any] struct{ Val T @Required }

type PB struct{ Val int @Required }

type PE struct{ Val error @Required }

func UseBox(a Box[int], b Box[error]) {}
)");
  ASSERT_EQ(P.errors(), "");
  std::vector<std::pair<std::string, const Type *>> Ts;
  for (const char *Name :
       {"SA", "SC", "SO", "SD", "SE", "SF", "SG", "PB", "PE"})
    Ts.push_back({Name, P.named(Name)->Underlying});
  for (const Type *T : P.func("UseBox")->sig()->Params->Elems) {
    auto *Inst = llvm::cast<NamedType>(T);
    ASSERT_TRUE(Inst->Underlying) << typeString(Inst);
    Ts.push_back({typeString(Inst), Inst->Underlying});
  }
  // The oracle itself: one struct written twice is one pointer; the
  // "must be set" mark splits Box[error] from PE but not Box[int] from PB.
  EXPECT_EQ(Ts[0].second, Ts[1].second);
  EXPECT_EQ(Ts[7].second, Ts[9].second);
  EXPECT_NE(Ts[8].second, Ts[10].second);
  Context C;
  SemaNames SN(C);
  for (auto &X : Ts)
    for (auto &Y : Ts) {
      const Node *NX = SN.type(X.second), *NY = SN.type(Y.second);
      ASSERT_TRUE(NX && NY) << SN.why();
      EXPECT_EQ(NX == NY, X.second == Y.second)
          << X.first << " and " << Y.first << ": " << mangleType(NX) << " "
          << mangleType(NY);
    }
}

TEST(SemaSymbols, EveryMethodHasItsOwnName) {
  // Operator methods form overload sets (ADR-0024): the parameters tell
  // them apart, and so does `@Right` (EXP-26: a non-commutative operator
  // may have a `@Right` method and a plain one with the same parameters).
  // The methods of an instance of a generic type are named by their
  // declaration, so `+ (T)` and `+ (float64)` stay two names in W[float64],
  // where both take a float64.
  Checked P("p", R"(package p

type V struct{ X float64 }

func (a V) / (s float64) V { return a }

@Right
func (a V) / (s float64) V { return a }

func (a V) - () V          { return a }
func (a V) - (b V) V       { return a }
func (a V) * (s float64) V { return a }
func (a V) * (b V) float64 { return 0 }
func (a V) Len() float64   { return 0 }

type W[T any] struct{ x T }

func (a W[T]) + (b T) int       { return 0 }
func (a W[T]) + (b float64) int { return 1 }

@Right
func (a W[T]) - (b float64) int { return 2 }

func (a W[T]) - (b float64) int { return 3 }
func (a W[T]) Get() int         { return 4 }

func Use(a W[float64], b W[int]) int { return a.Get() + (a - 2.0) + (2.0 - a) }
)");
  ASSERT_EQ(P.errors(), "");
  Context C;
  SemaNames SN(C);
  std::map<std::string, std::string> ByName; // mangled -> readable
  auto Add = [&](const Node *N, const std::string &What) {
    ASSERT_TRUE(N) << What << ": " << SN.why();
    std::string M = mangle(N);
    auto [It, New] = ByName.insert({M, print(N).Text});
    EXPECT_TRUE(New) << What << " has the name of " << It->second << ": " << M;
    ParseError Err;
    Context C2;
    EXPECT_TRUE(parseSymbol(C2, M, Err)) << M << ": " << Err.Message;
  };
  for (Func *M : *P.named("V")->Methods)
    Add(SN.func(M), "V." + M->Name.str());
  NamedType *W = P.named("W");
  size_t Instantiated = 0;
  for (const Type *T : P.func("Use")->sig()->Params->Elems) {
    auto *Inst = llvm::cast<NamedType>(T);
    for (size_t I = 0; I < W->Methods->size(); ++I) {
      Func *Decl = (*W->Methods)[I];
      const Node *N = SN.method(Inst, Decl);
      Add(N, typeString(Inst) + "." + Decl->Name.str());
      // The method the checker instantiated has the same name.
      if (Inst->InstMethods && I < Inst->InstMethods->size())
        if (Func *IM = (*Inst->InstMethods)[I]) {
          EXPECT_EQ(SN.func(IM), N) << print(N).Text << ": " << SN.why();
          ++Instantiated;
        }
    }
  }
  EXPECT_GE(Instantiated, 3u);
  std::set<std::string> Readable;
  for (auto &E : ByName)
    Readable.insert(E.second);
  for (const char *Want :
       {"p.V./(float64)", "p.V./(float64) @Right", "p.V.-()", "p.V.-(p.V)",
        "p.V.*(float64)", "p.V.*(p.V)", "p.V.Len", "p.W[float64].+($0)",
        "p.W[float64].+(float64)", "p.W[float64].-(float64)",
        "p.W[float64].-(float64) @Right", "p.W[float64].Get", "p.W[int].+($0)"})
    EXPECT_TRUE(Readable.count(Want)) << "missing: " << Want;
  EXPECT_EQ(ByName.size(), 7u + 2 * 5u);
}

//===----------------------------------------------------------------------===//
// The readable name is the type printer's text
//===----------------------------------------------------------------------===//

// The package path is the package name here: TypeString qualifies a type
// with its package name, the symbol with its path.
const char *const LibSource = R"(package lib

unit M
unit S
unit KM = 1000 M
unit H = 3600 S

type Speed = float64[KM/H]

type Point struct {
	X, Y int
	tag  string `json:"t"`
	Next Option[*Point]
}

func (p Point) Dist() float64    { return 0 }
func (p *Point) Move(dx int)     {}
func (p Point) + (q Point) Point { return Point{} }
func (p Point) + (q int) Point   { return Point{} }

type Shape interface {
	Area() float64
	name() string
}
type Number interface{ ~int | ~float64 }

type node[T any] struct {
	val  T
	next Option[*node[T]]
}
type List[T any] struct{ head Option[*node[T]] }

func (l *List[T]) Push(v T) {}

type Pair[K comparable, V any] struct {
	Key K
	Val V
}
type Vec2[U unit] struct{ X, Y float64[U] }

@Strict
type Config struct {
	Name string
	Port int
	Note string @Option
	Opt  Option[int]
}

type Plain struct {
	ID   int @Required
	Name string
	Ptr  *Point
}

type Animal struct{ Legs int }

type Dog extends Animal { Name string }

func Map[T, U any](xs []T, f func(T) U) []U { return nil }
func Parse!(s string) int                   { return 0 }
func Take(s View[string], move p Point) int { return 0 }
func Sum[N Number](xs []N) N                { var z N; return z }
func C1[T ~int | ~float64](x T)             {}
func C2[T ~float64 | ~int](x T)             {}

var V1 Pair[string, List[int]]
var V2 Pair[Pair[int, int], Pair[int, int]]
var V3 List[Speed]
var V4 Vec2[KM]
var V5 map[string][]Point
var V6 chan<- <-chan int
func Sig(a func(int, ...int) (bool, error), b Shape, c Shared[*List[Point]],
	d struct {
		P Point
		N int @Required
	}, e func(move x []int, y View[[]int]) Option[*Point], f any, g error) {
}
var V10 [4]Option[Unique[*Point]]
var V11 float64[KM/H]
var V12 float64[KM*H^-1]
var V13 float64[M/S]
var V14 Vec2[KM/H]
var V15 int32[KM]
var V16 []Shape
var V17 map[Pair[string, int]]chan float32[KM]

func init() {}

func Use() {
	var lst List[int]
	lst.Push(1)
	type local struct{ a int }
	var x local
	_ = x
	_ = Map[int, string]
}
)";

/// Every package-level object of P, by name.
std::vector<Object *> objects(const Package &P) {
  std::vector<Object *> Os;
  for (auto &KV : P.PkgScope->Elems)
    Os.push_back(KV.second);
  std::sort(Os.begin(), Os.end(),
            [](Object *A, Object *B) { return A->Name < B->Name; });
  return Os;
}

/// Every type the package mentions at its top level.
std::vector<const Type *> topLevelTypes(const Package &P) {
  std::vector<const Type *> Out;
  std::set<const Type *> Seen;
  auto Add = [&](const Type *T) {
    if (T && Seen.insert(T).second)
      Out.push_back(T);
  };
  for (Object *O : objects(P)) {
    if (O->getKind() == Object::Kind::Unit || !O->Ty)
      continue;
    Add(O->Ty);
    if (auto *S = llvm::dyn_cast<SignatureType>(O->Ty)) {
      for (const Type *E : S->Params->Elems)
        Add(E);
      for (const Type *E : S->Results->Elems)
        Add(E);
    }
    if (auto *N = llvm::dyn_cast<NamedType>(O->Ty)) {
      Add(N->Underlying);
      if (auto *S = llvm::dyn_cast_or_null<StructType>(N->Underlying))
        for (const Var *F : S->Fields)
          Add(F->Ty);
      if (N->Methods)
        for (const Func *M : *N->Methods)
          Add(M->Ty);
    }
  }
  return Out;
}

/// Does the type printer write T the one way its symbol is read back? Not
/// where one symbol stands for several spellings of one type: `byte` and
/// `rune` (read back as uint8 and int32), a fallible func type (read back
/// with its `error` result, ERR-2), an interface literal that embeds, lists
/// its methods out of order, or is written `interface{}` (read back as the
/// sorted method set, or `any`). SpellingDiffersOnlyForOneTypeWrittenTwoWays
/// tests each of these.
bool spelledOneWay(const Type *Root) {
  std::vector<const Type *> Work{Root};
  std::set<const Type *> Seen;
  while (!Work.empty()) {
    const Type *T = Work.back();
    Work.pop_back();
    if (!T || !Seen.insert(T).second)
      continue;
    switch (T->getKind()) {
    case Type::Kind::Basic: {
      llvm::StringRef Name = llvm::cast<BasicType>(T)->Name;
      if (Name == "byte" || Name == "rune")
        return false;
      break;
    }
    case Type::Kind::Pointer:
      Work.push_back(llvm::cast<PointerType>(T)->Elem);
      break;
    case Type::Kind::Slice:
      Work.push_back(llvm::cast<SliceType>(T)->Elem);
      break;
    case Type::Kind::Array:
      Work.push_back(llvm::cast<ArrayType>(T)->Elem);
      break;
    case Type::Kind::Ref:
      Work.push_back(llvm::cast<RefType>(T)->Elem);
      break;
    case Type::Kind::Option:
      Work.push_back(llvm::cast<OptionType>(T)->Elem);
      break;
    case Type::Kind::Chan:
      Work.push_back(llvm::cast<ChanType>(T)->Elem);
      break;
    case Type::Kind::Map:
      Work.push_back(llvm::cast<MapType>(T)->Key);
      Work.push_back(llvm::cast<MapType>(T)->Elem);
      break;
    case Type::Kind::Tuple:
      for (const Type *E : llvm::cast<TupleType>(T)->Elems)
        Work.push_back(E);
      break;
    case Type::Kind::Signature: {
      auto *S = llvm::cast<SignatureType>(T);
      if (S->Fails)
        return false;
      Work.push_back(S->Params);
      Work.push_back(S->Results);
      break;
    }
    case Type::Kind::Struct:
      for (const Var *F : llvm::cast<StructType>(T)->Fields)
        Work.push_back(F->Ty);
      break;
    case Type::Kind::Interface: {
      auto *I = llvm::cast<InterfaceType>(T);
      if (!I->Embeddeds.empty() || (I->Methods.empty() && !I->IsAny))
        return false;
      for (size_t K = 1; K < I->Methods.size(); ++K) {
        const Func *X = I->Methods[K - 1], *Y = I->Methods[K];
        auto Key = [](const Func *F) {
          return std::make_pair(isExportedName(F->Name) ? std::string()
                                                        : F->Pkg->Path.str(),
                                F->Name.str());
        };
        if (Key(Y) < Key(X))
          return false;
      }
      for (const Func *M : I->Methods)
        Work.push_back(M->Ty);
      break;
    }
    case Type::Kind::Named:
      for (const Type *A : llvm::cast<NamedType>(T)->TArgs)
        Work.push_back(A);
      break;
    case Type::Kind::Union:
    case Type::Kind::TypeParam:
      break;
    }
  }
  return true;
}

TEST(SemaSymbols, ReadableNameIsTheTypePrintersText) {
  // demangle(mangle(T)) == typeString(T) for every type of a checked package
  // that has a symbol, so the readable names cannot drift from the type
  // printer of the diagnostics.
  Checked P("lib", LibSource);
  ASSERT_EQ(P.errors(), "");
  Context C;
  SemaNames SN(C);
  unsigned Compared = 0, OneOfMany = 0, NoSymbol = 0;
  for (const Type *T : topLevelTypes(*P.CP->Pkg)) {
    const Node *N = SN.type(T);
    if (!N) {
      EXPECT_FALSE(SN.why().empty());
      ++NoSymbol;
      continue;
    }
    if (!spelledOneWay(T)) {
      ++OneOfMany;
      continue;
    }
    std::string M = mangle(C.ofType(Kind::TypeInfo, N));
    DemangleResult D = demangle(M);
    ASSERT_TRUE(D.Ok) << M << ": " << D.Error;
    EXPECT_EQ(D.Text, "typeinfo for " + typeString(T)) << M;
    ++Compared;
  }
  EXPECT_GE(Compared, 40u);
  EXPECT_GE(NoSymbol, 5u);  // generic declarations and constraints
  EXPECT_GE(OneOfMany, 1u); // Parse's `func!(string) int`
}

TEST(SemaSymbols, SpellingDiffersOnlyForOneTypeWrittenTwoWays) {
  Checked P("q", R"(package q

type I1 interface{ A() }

func Types(b byte, u uint8, r rune, i int32, f1 func!(int) string,
	f2 func(int) (string, error), e1 interface{ B(); A() },
	e2 interface{ I1; B() }, e3 interface{}, e4 any,
	e5 interface{ A(); B() }) {
}
)");
  ASSERT_EQ(P.errors(), "");
  llvm::ArrayRef<Type *> Ps = P.func("Types")->sig()->Params->Elems;
  Context C;
  SemaNames SN(C);
  struct Case {
    size_t Param, SameAs;
    const char *Printer, *Readable;
  } Cases[] = {
      {0, 1, "byte", "uint8"},
      {2, 3, "rune", "int32"},
      {4, 5, "func!(int) string", "func(int) (string, error)"},
      {6, 10, "interface{B(); A()}", "interface{A(); B()}"},
      {7, 10, "interface{B(); q.I1}", "interface{A(); B()}"},
      {8, 9, "interface{}", "any"},
  };
  for (const Case &K : Cases) {
    const Node *N = SN.type(Ps[K.Param]);
    ASSERT_TRUE(N) << SN.why();
    EXPECT_EQ(N, SN.type(Ps[K.SameAs])) << K.Printer;
    EXPECT_EQ(typeString(Ps[K.Param]), K.Printer);
    EXPECT_EQ(print(N).Text, K.Readable);
    // The other spelling is the printer's text, and is read back as itself.
    EXPECT_EQ(typeString(Ps[K.SameAs]), K.Readable);
    EXPECT_FALSE(spelledOneWay(Ps[K.Param])) << K.Printer;
    EXPECT_TRUE(spelledOneWay(Ps[K.SameAs])) << K.Readable;
  }
}

// The demangler refuses a unit exponent past MaxUnitExp: that must be the
// largest exponent the checker keeps, or legal names would be refused.
static_assert(gopp::sym::MaxUnitExp == gopp::sema::units::MaxExp,
              "the demangler's unit exponent limit differs from the checker's");
TEST(SemaSymbols, UnitExponentLimitIsTheCheckers) {
  EXPECT_EQ(gopp::sym::MaxUnitExp, gopp::sema::units::MaxExp);
}

TEST(SemaSymbols, UnitsAreNamedByTheirCanonicalForm) {
  // UNI-5: KM/H, KM*H^-1 and the alias Speed are one type and one name.
  Checked P("lib", LibSource);
  ASSERT_EQ(P.errors(), "");
  Context C;
  SemaNames SN(C);
  auto N = [&](const char *V) {
    const Node *Nd = SN.type(P.get(V)->Ty);
    EXPECT_TRUE(Nd) << V << ": " << SN.why();
    return Nd;
  };
  EXPECT_EQ(N("V11"), N("V12"));
  EXPECT_EQ(N("V11"), N("Speed"));
  EXPECT_EQ(print(N("V11")).Text, "float64[(5/18) lib.M/lib.S]");
  // Another factor, or another base type, is another name.
  EXPECT_NE(N("V11"), N("V13"));
  EXPECT_EQ(print(N("V13")).Text, "float64[lib.M/lib.S]");
  EXPECT_EQ(print(N("V15")).Text, "int32[1000 lib.M]");
  // A generic instance with a unit argument carries the canonical unit.
  EXPECT_EQ(print(N("V4")).Text, "lib.Vec2[1000 lib.M]");
  EXPECT_EQ(print(N("V14")).Text, "lib.Vec2[(5/18) lib.M/lib.S]");
  EXPECT_NE(N("V4"), N("V14"));
}

TEST(SemaSymbols, PackagePathsWithDotsSlashesAndUnicode) {
  const char *Src = "package pkg\n\ntype T struct{ N int }\nfunc F() {}\n"
                    "func (t T) M() {}\nvar G int\n";
  const char *Path = "example.com/ünï/pkg.v2";
  Checked P(Path, Src);
  ASSERT_EQ(P.errors(), "");
  Context C;
  SemaNames SN(C);
  const Node *T = SN.type(P.get("T")->Ty);
  const Node *F = SN.func(P.func("F"));
  const Node *M = SN.func((*P.named("T")->Methods)[0]);
  const Node *G = SN.var(llvm::cast<Var>(P.get("G")));
  ASSERT_TRUE(T && F && M && G) << SN.why();
  EXPECT_EQ(print(T).Text, std::string(Path) + ".T");
  EXPECT_EQ(print(F).Text, std::string(Path) + ".F");
  EXPECT_EQ(print(M).Text, std::string(Path) + ".T.M");
  EXPECT_EQ(print(G).Text, std::string(Path) + ".G");
  // Only the symbol alphabet reaches the object file.
  for (const Node *E : {F, M, G, C.ofType(Kind::TypeInfo, T)})
    for (unsigned char Ch : mangle(E))
      EXPECT_TRUE(isalnum(Ch) || Ch == '_' || Ch == '$') << mangle(E);
  // A package with the same last element is another package.
  Checked Other("other.com/pkg.v2", Src);
  ASSERT_EQ(Other.errors(), "");
  Context C2;
  SemaNames SN2(C2);
  const Node *F2 = SN2.func(Other.func("F"));
  const Node *T2 = SN2.type(Other.get("T")->Ty);
  ASSERT_TRUE(F2 && T2) << SN2.why();
  EXPECT_NE(mangle(F2), mangle(F));
  EXPECT_NE(mangleType(T2), mangleType(T));
}

//===----------------------------------------------------------------------===//
// Speed: the long suite (tests/driver/symbol-names.long.test)
//===----------------------------------------------------------------------===//

/// The least of three timings: a busy machine only ever slows a run down.
template <class F> double bestOf3(F Fn) {
  double Best = 1e9;
  for (int I = 0; I < 3; ++I) {
    auto T0 = std::chrono::steady_clock::now();
    Fn();
    Best = std::min(Best, std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - T0)
                              .count());
  }
  return Best;
}

TEST(SemaSymbolsLong, DISABLED_NearLinearOnDeepAndWideTypes) {
  // SemaNames walks the checker's types with an explicit stack and a memo,
  // so its time is linear in the number of distinct types: deep chains of
  // every constructor, func types that repeat their parameter, a wide struct
  // and tuple, and a type whose two halves are the type below (2^n paths).
  using Make =
      const Type *(*)(TypeContext &, std::deque<std::string> &, size_t);
  struct Shape {
    const char *Name;
    Make Fn;
  } Shapes[] = {
      {"deep mix",
       [](TypeContext &Ctx, std::deque<std::string> &,
          size_t N) -> const Type * {
         Type *T = Ctx.typ(BasicKind::Int);
         for (size_t I = 0; I < N; ++I) {
           switch (I % 5) {
           case 0:
             T = Ctx.pointer(T);
             break;
           case 1:
             T = Ctx.slice(T);
             break;
           case 2:
             T = Ctx.chan(ChanType::Both, T);
             break;
           case 3:
             T = Ctx.map(Ctx.typ(BasicKind::String), T);
             break;
           default:
             T = Ctx.array(T, 3);
             break;
           }
         }
         return T;
       }},
      {"deep funcs",
       [](TypeContext &Ctx, std::deque<std::string> &,
          size_t N) -> const Type * {
         Type *T = Ctx.typ(BasicKind::Int);
         for (size_t I = 0; I < N; ++I)
           T = Ctx.signature(Ctx.tuple({T}), Ctx.tuple({T, Ctx.errorType()}),
                             false, I % 2 == 0, 1);
         return T;
       }},
      {"wide struct",
       [](TypeContext &Ctx, std::deque<std::string> &Names,
          size_t N) -> const Type * {
         std::vector<TypeContext::FieldSpec> Fs;
         for (size_t I = 0; I < N; ++I) {
           Names.push_back("F" + std::to_string(I));
           Fs.push_back({Names.back(), nullptr,
                         Ctx.typ(I % 2 ? BasicKind::Int : BasicKind::String),
                         false, ""});
         }
         return Ctx.structOf(Fs);
       }},
      {"wide tuple",
       [](TypeContext &Ctx, std::deque<std::string> &,
          size_t N) -> const Type * {
         std::vector<Type *> Es;
         for (size_t I = 0; I < N; ++I)
           Es.push_back(
               Ctx.array(Ctx.typ(BasicKind::Int), static_cast<int64_t>(I)));
         return Ctx.tuple(Es);
       }},
      {"shared halves",
       [](TypeContext &Ctx, std::deque<std::string> &,
          size_t N) -> const Type * {
         Type *T = Ctx.typ(BasicKind::Int);
         for (size_t I = 0; I < N; ++I)
           T = Ctx.map(T, T);
         return T;
       }},
  };
  for (const Shape &S : Shapes) {
    auto Run = [&](size_t Size) {
      TypeContext Ctx;
      std::deque<std::string> Names;
      const Type *T = S.Fn(Ctx, Names, Size);
      return bestOf3([&] {
        Context C;
        SemaNames SN(C);
        const Node *N = SN.type(T);
        ASSERT_TRUE(N) << SN.why();
        EXPECT_FALSE(mangleType(N).empty());
      });
    };
    constexpr size_t N = 10000;
    double Small = Run(N), Big = Run(N * 16);
    // 16 times the size: a linear walk takes ~16 times as long, a quadratic
    // one 256 times. Leave room for noise.
    double Ratio = Big / std::max(Small, 1e-4);
    std::fprintf(stderr, "  %-14s %8.4fs -> %8.4fs  (x%.1f for x16)\n", S.Name,
                 Small, Big, Ratio);
    EXPECT_LT(Ratio, 16 * 6) << S.Name;
    EXPECT_LT(Big, 10.0) << S.Name;
  }
}

} // namespace
