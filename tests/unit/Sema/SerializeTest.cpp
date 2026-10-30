//===- SerializeTest.cpp - Type serialization round trips (P03.11) --------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A checked package is written through TypeWriter into a MemoryRecordStream
// (also as bytes), read back into a fresh TypeContext, and compared by its
// canonical API text; writing the copy again gives the same bytes. Reading
// into the context the types came from must give the same uniqued pointers.
// This runs on a hand-written package, on every file of the conformance
// suite, the spec examples and the fuzz corpus. Corrupt input (every
// truncation, random bytes, every operand of every record, too deep, a
// redefinition of a universe type) is rejected without a crash (also under
// ASan) and without changing the context.
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gopp/Sema/Loader.h"
#include "gopp/Sema/Serialize.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <map>
#include <set>

#include <algorithm>
#include <fstream>

using namespace gopp;
using namespace gopp::sema;

namespace {

const char *LibSource = R"(package lib

const Pi = 3.14159265358979323846264338327950288419716939937510582097494459
const Big = 1 << 100
const Neg int64 = -9223372036854775808
const Name = "go++\x00\n"
const Yes = true
const Cx = 1 + 2i
const Third = 1.0 / 3
const (
	A = iota
	B
)

type Point struct {
	X, Y int
	tag  string `json:"t"`
	Next Option[*Point]
}

func (p Point) Dist() float64 { return 0 }
func (p *Point) Move(dx int)  {}

type Shape interface {
	Area() float64
	name() string
}
type Number interface{ ~int | ~float64 }
type Opt interface{ Rec | int }
type Rec struct{ next Option[*Rec] }

type List[T any] struct{ head Option[*node[T]] }
type node[T any] struct {
	val  T
	next Option[*node[T]]
}

func (l *List[T]) Push(v T) {}
func (l List[T]) Len() int  { return 0 }

type Pair[K comparable, V any] struct {
	Key K
	Val V
}

func Map[T, U any](xs []T, f func(T) U) []U { return nil }
func Sum[N Number](xs []N) N                { var z N; return z }
func Take(s View[string], move p Point) int   { return 0 }
func Parse!(s string) int                   { return 0 }
func Pairs(m map[string][]Point) []Pair[string, int] { return nil }

var Counter int
var Handles map[string][]Point
var Ch chan<- int
var Arr [4]byte
var Runes []rune
var Owner Option[Unique[*Point]]
var Shared1 Option[Shared[*List[int]]]
var Weak1 Option[Weak[*Point]]
var Anyval any
var Err error
var Callback = func(int, ...int) (bool, error) { return false, nil }
var Embedded struct {
	P     Point
	Extra []Shape
}

// P03.15: a @Required field, and the types with no zero value that follow.
type User struct {
	ID   int @Required
	Name string
}
type Team struct {
	Lead User
	Size int
}
type Box[T any] struct{ V T }
type Gate struct {
	Open error @Required
}

// P03.20: a @Strict struct. The effective required bits and the attributes
// (`@Strict`, `@Required`, `@Option`) are stored.
@Strict
type Payment struct {
	ID      int
	Amount  int64
	Note    Option[string]
	Retries int @Option
	_       int
}
@Strict
type Memo struct {
	Note Option[string] @Required
	N    int
}
@Strict
type GenOpt[T any] struct {
	N int
	V T @Option
}
type Ledger struct {
	Pay  Payment
	Size int
}
type RawPayment struct {
	ID      int @Required
	Amount  int64 @Required
	Note    Option[string]
	Retries int
	_       int
}
type LoosePayment struct {
	ID      int
	Amount  int64
	Note    Option[string]
	Retries int
	_       int
}
@Strict
type Gen[T any] struct{ V T }

type Alias = Point
type Fn func(int) (string, error)
type Celsius float64
type Table map[string]List[Celsius]
)";

class Tree {
public:
  Tree() {
    llvm::SmallString<128> P;
    EXPECT_FALSE(llvm::sys::fs::createUniqueDirectory("gopp-serialize", P));
    Root = std::string(P);
  }
  ~Tree() { llvm::sys::fs::remove_directories(Root); }
  std::string Root;
};

struct Resolver : PackageResolver {
  Package *resolve(llvm::StringRef) override { return nullptr; }
};

struct Loaded {
  explicit Loaded(const char *Src = LibSource) {
    std::ofstream(T.Root + "/lib.gopp") << Src;
    LoadOptions LO;
    LO.Root = T.Root;
    Diags = std::make_unique<DiagnosticsEngine>(SM, Collect, 0);
    L = std::make_unique<Loader>(SM, *Diags, Ctx, LO, CheckOptions{});
    CP = L->loadDir(T.Root, "lib");
  }
  Tree T;
  SourceManager SM;
  CollectingDiagnosticConsumer Collect;
  std::unique_ptr<DiagnosticsEngine> Diags;
  TypeContext Ctx;
  std::unique_ptr<Loader> L;
  CheckedPackage *CP = nullptr;
};

std::string dump(const Package &P) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  dumpPackageAPI(OS, P);
  return S;
}

TEST(Serialize, SourcePackageChecksCleanly) {
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  std::string Msgs;
  for (const Diagnostic &D : L.Collect.Diags)
    Msgs += D.Message + "\n";
  EXPECT_TRUE(L.Collect.Diags.empty()) << Msgs;
}

TEST(Serialize, RoundTripIntoAFreshContextKeepsTheAPI) {
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);

  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(Stream, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  EXPECT_EQ(P2->Path, "lib");
  EXPECT_EQ(dump(*L.CP->Pkg), dump(*P2));
  // Constants keep their exact value, not only the printed one.
  for (const char *Name : {"Pi", "Big", "Neg", "Name", "Yes", "Cx", "Third"}) {
    auto *A = llvm::cast<Const>(L.CP->Pkg->PkgScope->lookupLocal(Name));
    auto *B = llvm::cast<Const>(P2->PkgScope->lookupLocal(Name));
    ASSERT_TRUE(A->Val && B->Val) << Name;
    EXPECT_EQ(A->Val->kind(), B->Val->kind()) << Name;
    EXPECT_TRUE(ConstValue::compare(*A->Val, tok::eql, *B->Val)) << Name;
  }
  // Parameter names survive; the receiver of a method is the decoded type.
  auto *Take = llvm::cast<Func>(P2->PkgScope->lookupLocal("Take"));
  ASSERT_EQ(Take->Params.size(), 2u);
  EXPECT_EQ(Take->Params[0]->Name, "s");
  EXPECT_TRUE(Take->sig()->isMove(1));
  auto *Pt = llvm::cast<NamedType>(P2->PkgScope->lookupLocal("Point")->Ty);
  ASSERT_TRUE(Pt->Methods);
  EXPECT_EQ(Pt->Methods->size(), 2u);
  EXPECT_EQ((*Pt->Methods)[1]->Recv->Ty, Ctx2.pointer(Pt));
  // The text covers every kind of type, constant and method.
  std::string D = dump(*P2);
  for (const char *Want : {
           "const Big untyped int = 1267650600228229401496703205376",
           "const Neg int64 = -9223372036854775808",
           "const Yes untyped bool = true",
           "type Point struct{X int; Y int; tag string \"json:\"t\"\"; "
           "Next Option[*Point]}",
           "func (*Point) Move(int)",
           "func (Point) Dist() float64",
           "type List[T any] struct{head Option[*node[T]]}",
           "func (*List[T any]) Push(T)",
           "type Number interface{~int | ~float64}",
           "func Map[T, U any]([]T, func(T) U) []U",
           "func Parse!(string) int",
           "var Owner Option[Unique[*Point]]",
           "var Shared1 Option[Shared[*List[int]]]",
           "var Ch chan<- int",
           "func Take(View[string], Point) int",
           "type Alias = Point",
       })
    EXPECT_NE(D.find(Want), std::string::npos) << Want << "\n" << D;
}

TEST(Serialize, RoundTripThroughBytes) {
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();

  MemoryRecordStream Decoded;
  ASSERT_TRUE(Decoded.fromBytes(Bytes));
  EXPECT_EQ(Decoded.toBytes(), Bytes);
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(Decoded, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  EXPECT_EQ(dump(*L.CP->Pkg), dump(*P2));
}

TEST(Serialize, EncodingIsDeterministic) {
  std::string First;
  for (int I = 0; I < 3; ++I) {
    Loaded L;
    ASSERT_NE(L.CP, nullptr);
    MemoryRecordStream Stream;
    TypeWriter(Stream).writePackage(*L.CP->Pkg);
    if (First.empty())
      First = Stream.toBytes();
    EXPECT_EQ(First, Stream.toBytes());
  }
}

TEST(Serialize, ReadingIntoTheSameContextGivesTheSameUniquedTypes) {
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(Stream, L.Ctx, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  // Types made of basic and universe types are the very same objects.
  for (const char *Name : {"Runes", "Arr", "Ch", "Err", "Anyval", "Callback"}) {
    Object *A = L.CP->Pkg->PkgScope->lookupLocal(Name);
    Object *B = P2->PkgScope->lookupLocal(Name);
    ASSERT_TRUE(A && B) << Name;
    EXPECT_EQ(A->Ty, B->Ty) << Name;
  }
  // A named type is decoded to a new object, and so is every type that
  // contains it, even `Option[Unique[*Point]]` (its origin is the universe's).
  Object *Own = P2->PkgScope->lookupLocal("Owner");
  ASSERT_NE(Own, nullptr);
  EXPECT_NE(Own->Ty, L.CP->Pkg->PkgScope->lookupLocal("Owner")->Ty);
}

/// An importer that hands out one package.
struct OnePackage : Importer {
  OnePackage(llvm::StringRef Path, Package *P) : Path(Path.str()), P(P) {}
  Package *import(llvm::StringRef Want, std::string &) override {
    return Want == Path ? P : nullptr;
  }
  std::string Path;
  Package *P;
};

/// The error messages of main, a package that imports Lib.
std::vector<std::string> checkMain(TypeContext &Ctx, Package *Lib,
                                   llvm::StringRef Text) {
  ParseOptions PO;
  test::Parsed Pr(Text, PO);
  OnePackage Imp("lib", Lib);
  CheckOptions CO;
  CO.Ownership = true;
  const ast::File *Files[] = {Pr.File};
  checkPackage(Ctx, Pr.Src.SM, "main", Files, Imp, Pr.Src.Diags, CO);
  return Pr.errors();
}

bool anyHas(const std::vector<std::string> &Errs, llvm::StringRef Sub) {
  return std::any_of(Errs.begin(), Errs.end(), [&](const std::string &E) {
    return E.find(Sub) != std::string::npos;
  });
}

// P03.15: an importing package gets the same errors from a package read back
// from bytes as from the one that was just checked. The writer records
// @Required per field and "no zero value" per type.
TEST(Serialize, RequiredFieldsAndNoZeroValueSurviveTheRoundTripAcrossPackages) {
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  // The text of a struct shows the attribute, so the dump compares it.
  EXPECT_NE(dump(*P2).find("type User struct{ID int @Required; Name string}"),
            std::string::npos)
      << dump(*P2);

  // "No zero value" is recorded per type (1 = has one, 2 = has none) before
  // anything asks the copy.
  auto State = [&](const char *Name) {
    auto *N = llvm::cast<NamedType>(P2->PkgScope->lookupLocal(Name)->Ty);
    return N->ZeroState.load();
  };
  EXPECT_EQ(State("User"), 2);
  EXPECT_EQ(State("Team"), 2);
  EXPECT_EQ(State("Gate"), 2);
  EXPECT_EQ(State("Point"), 1);

  const char *Bad = R"(package main
import "lib"
func f() {
	_ = lib.User{Name: "x"}
	var u lib.User
	var t lib.Team
	var b lib.Box[lib.User]
	_ = lib.Gate{}
	_, _, _ = u, t, b
}
)";
  const char *Good = R"(package main
import "lib"
type nope struct{}
func (nope) Error() string { return "" }
func f() {
	u := lib.User{ID: 1}
	t := lib.Team{Lead: u, Size: 2}
	var b lib.Box[int]
	var p lib.Point
	g := lib.Gate{Open: nope{}}
	_, _, _, _, _ = u, t, b, p, g
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "missing required field ID in lib.User literal"))
      << Decoded.size();
  EXPECT_TRUE(anyHas(Decoded, "lib.User has no zero value"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Team has no zero value"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Box[lib.User] has no zero value"));
  EXPECT_TRUE(
      anyHas(Decoded, "missing required field Open in lib.Gate literal"));
  EXPECT_EQ(Decoded.size(), 5u);
  // Both sides: the good code is clean in both.
  EXPECT_TRUE(checkMain(L.Ctx, L.CP->Pkg, Good).empty());
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
  // And the bytes of the copy are the bytes of the original.
  MemoryRecordStream Again;
  TypeWriter(Again).writePackage(*P2);
  EXPECT_EQ(Again.toBytes(), Bytes);
}

// P03.21: a View, a @View struct and the borrowed []T of slicing survive the
// round trip (REC_REF keeps the borrowed flag): an importing package gets the
// same errors from the copy as from the checked package (OWN-V1, OWN-V2).
const char *ViewLibSource = R"(package lib

type User struct {
	Name string
	Tags []string
	Age  int
}

@View
type Token struct {
	Text View[string]
	Line int
}

var Arr [4]int
var Part = Arr[1:]

func Name(u View[User]) View[string]   { return u.Name }
func Tags(u View[User]) View[[]string] { return u.Tags }
)";

TEST(Serialize, ViewsAndBorrowedSlicesSurviveTheRoundTripAcrossPackages) {
  Loaded L(ViewLibSource);
  ASSERT_NE(L.CP, nullptr);
  ASSERT_TRUE(L.Collect.Diags.empty());
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  std::string D = dump(*P2);
  EXPECT_NE(D.find("func Tags(View[User]) View[[]string]"), std::string::npos)
      << D;
  EXPECT_NE(D.find("type Token struct{Text View[string]; Line int}"),
            std::string::npos)
      << D;
  // The borrowed flag: Part is a borrowed []int, not a View[[]int].
  auto *Part = P2->PkgScope->lookupLocal("Part");
  ASSERT_NE(Part, nullptr);
  auto *R = llvm::dyn_cast<RefType>(Part->Ty);
  ASSERT_NE(R, nullptr);
  EXPECT_TRUE(R->Borrowed);

  const char *Bad = R"(package main
import "lib"
func f(u lib.User) {
	v := lib.Tags(u)
	v[0] = "x"
	var s string = lib.Name(u)
	p := lib.Part
	p = append(p, 1)
	var w View[[]int] = lib.Part
	lib.Part = w
	_ = s
}
)";
  const char *Good = R"(package main
import "lib"
func g(u lib.User) int {
	v := lib.Tags(u)
	p := lib.Part
	p[0] = 1
	var w View[[]int] = lib.Part
	t := lib.Token{Text: lib.Name(u), Line: 1}
	return len(v) + len(lib.Name(u)) + p[0] + len(w) + t.Line
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "cannot write v[0]: it is read-only"));
  EXPECT_TRUE(anyHas(Decoded, "as string value in variable declaration"));
  EXPECT_TRUE(anyHas(Decoded, "cannot append to p"));
  EXPECT_TRUE(anyHas(Decoded, "as borrowed []int value in assignment"));
  EXPECT_EQ(Decoded.size(), 4u);
  // Both sides: the good code is clean in both.
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good), std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
  MemoryRecordStream Again;
  TypeWriter(Again).writePackage(*P2);
  EXPECT_EQ(Again.toBytes(), Bytes);
}

// P03.21 review (R3): which generic structs may hold views (@View) survives
// the round trip, so the OWN-V5 check of an instance made in another package
// gives the same answer from the copy read back from bytes.
// P03 review (R24): a type parameter that is Copy only because of a ~T term
// requires every type argument to be Copy, also in an importing package
// (the type set travels in the export data, the requirement is derived from
// it): a defined int type with a drop method is rejected there.
TEST(Serialize, ImplicitCopyOfATildeTypeSetIsRequiredAcrossPackages) {
  Loaded L(R"(package lib

func Dup[T ~int](x T) (T, T) { return x, x }

func Max[T ~int | ~float64](a, b T) T {
	if a > b {
		return a
	}
	return b
}
)");
  ASSERT_NE(L.CP, nullptr);
  ASSERT_TRUE(L.Collect.Diags.empty());
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  const char *Bad = R"(package main
import "lib"
type N int
func (n *N) drop() {}
func outer[U ~int](u U) (U, U) { return lib.Dup(u) }
func f(n N) {
	a, b := lib.Dup(n)
	c, d := outer(n)
	_, _, _, _ = a, b, c, d
}
)";
  const char *Good = R"(package main
import "lib"
type Count int
func outer[U ~int](u U) (U, U) { return lib.Dup(u) }
func f(c Count) Count {
	a, b := lib.Dup(c)
	d, _ := outer(c)
	return lib.Max(a, b) + d
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "N does not satisfy Copy, which the type set of "
                              "T (~int) implies: N has a drop method"));
  EXPECT_TRUE(anyHas(Decoded, "N does not satisfy Copy, which the type set of "
                              "U (~int) implies: N has a drop method"));
  EXPECT_EQ(Decoded.size(), 2u);
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good), std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
}

// P03.22 review (R1): a type parameter whose generic code uses `*p` as a value
// must be untagged at every instantiation, also in an importing package (the
// requirement is carried by the type parameter in the export data), and also
// through a generic of the importing package that passes its parameter on.
TEST(Serialize, ImplicitUntaggedRequirementIsRequiredAcrossPackages) {
  Loaded L(R"(package lib

func Swap[T any](p *T, q *T) { *p, *q = move(*q), move(*p) }

func Same[T any](p *T, q *T) bool { return p == q }

func Zero[T ~struct{ N int }](p *T) int { var z T; _ = z; return 0 }

type Cell[T any] struct{ V *T }

func (c *Cell[T]) Put(move x T) { *c.V = move(x) }
)");
  ASSERT_NE(L.CP, nullptr);
  ASSERT_TRUE(L.Collect.Diags.empty());
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  const char *Bad = R"(package main
import "lib"
type Animal struct{ Name string }
@Virtual
func (a *Animal) Speak() string { return "" }
type Shape struct{ N int }
@Abstract
func (s *Shape) Area() int
func outer[U any](p *U, q *U) { lib.Swap(p, q) }
func f(a *Animal, b *Animal, sh *Shape) {
	lib.Swap(a, b)
	outer(a, b)
	lib.Zero(sh)
	c := lib.Cell[Animal]{V: a}
	_ = c
}
)";
  const char *Good = R"(package main
import "lib"
type Animal struct{ Name string }
@Virtual
func (a *Animal) Speak() string { return "" }
type Plain struct{ N int }
func outer[U any](p *U, q *U) { lib.Swap(p, q) }
func g(a *Animal, b *Animal, x *Plain, y *Plain) bool {
	lib.Swap(x, y)
	outer(x, y)
	lib.Zero(x)
	c := lib.Cell[Plain]{V: x}
	_ = c
	return lib.Same(a, b)
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "Animal does not satisfy the implicit "
                              "requirement of T: the generic code copies, "
                              "moves or assigns a value of T through a "
                              "pointer"));
  EXPECT_TRUE(anyHas(Decoded, "Animal does not satisfy the implicit "
                              "requirement of U: the generic code passes U "
                              "on to a generic"));
  EXPECT_TRUE(anyHas(Decoded, "Shape does not satisfy the implicit "
                              "requirement of T: the generic code makes or "
                              "holds a value of T"));
  EXPECT_EQ(Decoded.size(), 4u);
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good), std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
}

TEST(Serialize, ViewGenericStructsSurviveTheRoundTripAcrossPackages) {
  Loaded L(R"(package lib

type W[T any] struct{ X T }

@View
type GV[T any] struct {
	V View[string]
	X T
}
)");
  ASSERT_NE(L.CP, nullptr);
  ASSERT_TRUE(L.Collect.Diags.empty());
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  auto *W = llvm::cast<NamedType>(P2->PkgScope->lookupLocal("W")->Ty);
  auto *GV = llvm::cast<NamedType>(P2->PkgScope->lookupLocal("GV")->Ty);
  EXPECT_FALSE(W->ViewAttr);
  EXPECT_TRUE(GV->ViewAttr);

  // A @View struct holds the views of its fields (OWN-V5), also an
  // instance whose type argument is no view: only the decoded ViewAttr says
  // that lib.GV[int] is worth looking into.
  const char *Bad = R"(package main
import "lib"
type S struct{ w lib.W[View[string]] }
type T struct{ g lib.GV[int] }
)";
  const char *Good = R"(package main
import "lib"
type A struct{ w lib.W[int] }
@View
type B struct{ g lib.GV[View[string]] }
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "field w of struct S holds a view"));
  EXPECT_TRUE(anyHas(Decoded, "field g of struct T holds a view"));
  EXPECT_EQ(Decoded.size(), 2u);
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good), std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
  MemoryRecordStream Again;
  TypeWriter(Again).writePackage(*P2);
  EXPECT_EQ(Again.toBytes(), Bytes);
}

// The struct of a named type of P as text: `S:` if it is @Strict (`-:` if
// not), then per field its effective required bit (R or -) and the
// attributes written on it (r: @Required, o: @Option).
std::string strictBits(Package *P, const char *Type) {
  auto *N = llvm::cast<NamedType>(P->PkgScope->lookupLocal(Type)->Ty);
  auto *St = llvm::cast<StructType>(N->Underlying);
  std::string S = St->Strict ? "S:" : "-:";
  for (const Var *F : St->Fields) {
    S += ' ';
    S += F->Required ? 'R' : '-';
    if (F->MarkedRequired)
      S += 'r';
    if (F->MarkedOption)
      S += 'o';
  }
  return S;
}

// P03.20: the effective required bits of a @Strict struct and the attributes
// (`@Strict`, `@Required`, `@Option`; P09 and P14 read them) survive the
// round trip: a package importing Payment gets the same errors from the copy
// read back from bytes as from the original, for the rules of literals and
// zero values, the bit compared in a conversion, the messages that say why a
// field is required, and `V T @Option` judged per instance.
TEST(Serialize, StrictStructBitsSurviveTheRoundTripAcrossPackages) {
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  // The dump shows the attributes as written: type identity includes them, so
  // the strict struct and the one marked by hand print differently.
  std::string D = dump(*P2);
  EXPECT_NE(D.find("type Payment @Strict struct{ID int; Amount int64; "
                   "Note Option[string]; Retries int @Option; _ int}"),
            std::string::npos)
      << D;
  EXPECT_NE(D.find("type RawPayment struct{ID int @Required; Amount int64 "
                   "@Required; Note Option[string]; Retries int; _ int}"),
            std::string::npos)
      << D;
  auto State = [&](const char *Name) {
    auto *N = llvm::cast<NamedType>(P2->PkgScope->lookupLocal(Name)->Ty);
    return N->ZeroState.load();
  };
  EXPECT_EQ(State("Payment"), 2);
  EXPECT_EQ(State("Ledger"), 2);
  EXPECT_EQ(State("RawPayment"), 2);
  EXPECT_EQ(State("LoosePayment"), 1);
  // The bits and the attributes, exactly, and the same as the original's.
  EXPECT_EQ(strictBits(P2, "Payment"), "S: R R - -o -");
  EXPECT_EQ(strictBits(P2, "RawPayment"), "-: Rr Rr - - -");
  EXPECT_EQ(strictBits(P2, "LoosePayment"), "-: - - - - -");
  EXPECT_EQ(strictBits(P2, "Memo"), "S: Rr R");
  EXPECT_EQ(strictBits(P2, "Gen"), "S: R");
  EXPECT_EQ(strictBits(P2, "GenOpt"), "S: R -o");
  for (const char *T :
       {"Payment", "RawPayment", "LoosePayment", "Memo", "Gen", "GenOpt"})
    EXPECT_EQ(strictBits(P2, T), strictBits(L.CP->Pkg, T)) << T;

  const char *Bad = R"(package main
import "lib"
func f(loose lib.LoosePayment, ch chan lib.Payment) {
	_ = lib.Payment{ID: 1}
	var p lib.Payment
	var l lib.Ledger
	var g lib.Gen[int]
	_ = make([]lib.Payment, 3)
	_ = lib.Ledger{Size: 1}
	_ = lib.LoosePayment(lib.Payment{ID: 1, Amount: 2})
	_ = lib.Payment(loose)
	_, _, _ = p, l, g
	_ = ch
	var m lib.Memo
	_ = m
}
func g(b lib.GenOpt[lib.Payment]) {}
)";
  const char *Good = R"(package main
import "lib"
func f() {
	raw := lib.RawPayment{ID: 1, Amount: 2}
	p := lib.Payment{ID: 1, Amount: 2, Retries: 3}
	q := lib.Payment{ID: 1, Amount: 2, Note: nil}
	l := lib.Ledger{Pay: p, Size: 1}
	g := lib.Gen[int]{V: 1}
	r := lib.RawPayment(p)
	s := lib.Payment(raw)
	ps := make([]lib.Payment, 0, 4)
	_, _, _, _, _, _, _ = q, l, g, r, s, ps, p
	m := lib.Memo{Note: nil, N: 1}
	o := lib.GenOpt[int]{N: 1}
	_, _ = m, o
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(
      anyHas(Decoded, "missing required field Amount in lib.Payment literal"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Payment has no zero value"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Ledger has no zero value"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Gen[int] has no zero value"));
  EXPECT_TRUE(anyHas(Decoded, "make([]lib.Payment, 3) creates zero values"));
  EXPECT_TRUE(
      anyHas(Decoded, "missing required field Pay in lib.Ledger literal"));
  EXPECT_TRUE(anyHas(Decoded, "to type lib.LoosePayment"));
  EXPECT_TRUE(anyHas(Decoded, "to type lib.Payment"));
  // The messages say why a field is required: @Strict or the mark.
  EXPECT_TRUE(anyHas(Decoded, "lib.Payment has no zero value (field ID is "
                              "required (@Strict))"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Memo has no zero value (field Note is "
                              "@Required)"));
  EXPECT_TRUE(anyHas(Decoded, "lib.GenOpt[lib.Payment]: field V is @Option, "
                              "but lib.Payment has no zero value"));
  EXPECT_EQ(Decoded.size(), 10u);
  EXPECT_TRUE(checkMain(L.Ctx, L.CP->Pkg, Good).empty());
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
  MemoryRecordStream Again;
  TypeWriter(Again).writePackage(*P2);
  EXPECT_EQ(Again.toBytes(), Bytes);
}

// P03.22: the hierarchy is exported: the parent of each type (the hidden
// first field), the mark of each method, the abstract bit and the `@Abstract`
// attribute of a type, and the `extends` constraint. A package that imports
// Payment gets the same errors from the decoded package as from the original.
const char *HierarchySource = R"(package lib

type Payment struct{ Amount int64 }

@Abstract
func (p *Payment) Pay() error

@Virtual
func (p *Payment) Fee() int64 { return 0 }

func (p *Payment) Total() int64 { return p.Amount }

type Card extends Payment { Number string }

@Override
func (c *Card) Pay() error { return nil }

@Override
func (c *Card) Fee() int64 { return 1 }

type Premium extends Card {}

@Abstract
type Half extends Payment {}

type Animal struct{ Name string }

func (a *Animal) Eat() {}

type Dog extends Animal { Age int }

type Box[T any] struct{ V T }
type Crate[T any] extends Box[T] { Extra T }

func Feed[T extends Animal](a *T) { a.Eat() }

type Both interface {
	extends Payment
	Clone
}
)";

// P03.22 review (R12): a type that is abstract by its `@Abstract` attribute
// only (nothing left open, abstract state 3) survives the round trip, and is
// still abstract in the importing package.
TEST(Serialize, AbstractByAttributeOnlySurvivesTheRoundTrip) {
  Loaded L(R"(package lib

type Animal struct{ Name string }

@Abstract
type Bird extends Animal {}

type Sparrow extends Bird {}
)");
  ASSERT_NE(L.CP, nullptr);
  ASSERT_TRUE(L.Collect.Diags.empty());
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  const char *Bad = R"(package main
import "lib"
func f() {
	_ = lib.Bird{}
}
)";
  const char *Good = R"(package main
import "lib"
func g() {
	_ = lib.Sparrow{}
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "cannot use the abstract type lib.Bird"));
  EXPECT_EQ(Decoded.size(), 1u);
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good), std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
}

TEST(Serialize, HierarchySurvivesTheRoundTripAcrossPackages) {
  Loaded L(HierarchySource);
  ASSERT_NE(L.CP, nullptr);
  std::string Msgs;
  for (const Diagnostic &D : L.Collect.Diags)
    Msgs += D.Message + "\n";
  ASSERT_TRUE(L.Collect.Diags.empty()) << Msgs;
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;

  // The decoded package has the same hierarchy: dump and bytes.
  EXPECT_EQ(dump(*P2), dump(*L.CP->Pkg));
  std::string D = dump(*P2);
  EXPECT_NE(D.find("extends Payment"), std::string::npos) << D;
  EXPECT_NE(D.find("extends Animal"), std::string::npos) << D;
  auto Named = [&](Package *P, const char *Name) {
    return llvm::cast<NamedType>(P->PkgScope->lookupLocal(Name)->Ty);
  };
  for (const char *T :
       {"Payment", "Card", "Premium", "Half", "Animal", "Dog"}) {
    EXPECT_EQ(Named(P2, T)->AbstractAttr, Named(L.CP->Pkg, T)->AbstractAttr)
        << T;
    EXPECT_EQ(Named(P2, T)->AbstractState.load(),
              Named(L.CP->Pkg, T)->AbstractState.load())
        << T;
  }
  EXPECT_TRUE(Named(P2, "Half")->AbstractAttr);
  EXPECT_FALSE(Named(P2, "Card")->AbstractAttr);
  // The marks of the methods.
  auto Mark = [&](const char *Ty, const char *M) {
    for (Func *F : *Named(P2, Ty)->Methods)
      if (F->Name == M)
        return F->MethodMark;
    return Func::Mark::None;
  };
  EXPECT_EQ(Mark("Payment", "Pay"), Func::Mark::Abstract);
  EXPECT_EQ(Mark("Payment", "Fee"), Func::Mark::Virtual);
  EXPECT_EQ(Mark("Payment", "Total"), Func::Mark::None);
  EXPECT_EQ(Mark("Card", "Fee"), Func::Mark::Override);
  // The field table of a child starts with the parent.
  auto *CardSt = llvm::cast<StructType>(Named(P2, "Card")->Underlying);
  ASSERT_EQ(CardSt->Fields.size(), 2u);
  EXPECT_TRUE(CardSt->Fields[0]->IsParent);
  EXPECT_TRUE(CardSt->Fields[0]->Embedded);
  EXPECT_EQ(CardSt->Fields[0]->Name, "Payment");
  EXPECT_FALSE(CardSt->Fields[1]->IsParent);
  // TYP-10: the field table lists the parent chain, the root's fields first.
  llvm::SmallVector<const Var *, 4> Table;
  fieldTable(Named(P2, "Premium"), Table);
  ASSERT_EQ(Table.size(), 2u);
  EXPECT_EQ(Table[0]->Name, "Amount");
  EXPECT_EQ(Table[1]->Name, "Number");

  const char *Bad = R"(package main
import "lib"
type Cat struct{ N int }
type Own extends lib.Payment {}
func f(c *lib.Card, h lib.Half, d lib.Dog, move pu Unique[*lib.Dog], a *lib.Animal, cat *Cat, dp *lib.Dog) {
	_ = lib.Payment{Amount: 1}
	var p lib.Payment
	_ = p
	_ = make([]lib.Payment, 3)
	_ = lib.Half{Amount: 1}
	var q lib.Payment = *c
	_ = q
	lib.Feed[Cat](cat)
	_ = a.(*lib.Dog)
	var ua Unique[*lib.Animal] = move(pu)
	var ap *lib.Animal = dp
	var bx lib.Box[lib.Payment]
	k := lib.Card{Number: "x"}
	_ = k.Payment
	_, _, _ = ua, ap, bx
}
type NoOverride extends lib.Card {}
func (n *NoOverride) Fee() int64 { return 2 }
func (n *NoOverride) Total() int64 { return 2 }
)";
  const char *Good = R"(package main
import "lib"
type Own extends lib.Card {}
@Override
func (o *Own) Fee() int64 { return base.Fee() + base.Total() }
func f(c *lib.Card, h *lib.Half, d *lib.Dog) {
	var p *lib.Payment = c
	d.Eat()
	var cp Unique[*lib.Payment] = p.Clone()
	switch x := p.(type) {
	case *lib.Premium:
		_ = x
	}
	_ = lib.Card{Amount: 1, Number: "x"}
	_ = lib.Card{Payment: lib.Payment{Amount: 1}}
	_ = lib.Premium{Amount: 2}
	_ = lib.Crate[int]{V: 1, Extra: 2}
	lib.Feed[lib.Dog](d)
	lib.Feed(d)
	_, _, _ = p, cp, h
	if k, ok := p.(*lib.Card); ok {
		_ = k
	}
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "lib.Payment for a composite literal"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Payment for the type of a variable"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Payment for an element type"));
  EXPECT_TRUE(anyHas(Decoded, "a type argument of lib.Box[lib.Payment]"));
  EXPECT_TRUE(anyHas(Decoded, "so a *lib.Animal points at a child only in "
                              "the parent's own methods and what they "
                              "return"));
  EXPECT_TRUE(anyHas(Decoded, "cannot use k.Payment as a value"));
  EXPECT_TRUE(anyHas(Decoded, "cannot use *c as a value"));
  EXPECT_TRUE(anyHas(Decoded, "its @Abstract method Pay is not implemented"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Half for a composite literal"));
  EXPECT_TRUE(anyHas(Decoded, "value slicing"));
  EXPECT_TRUE(anyHas(Decoded, "does not satisfy extends lib.Animal"));
  EXPECT_TRUE(anyHas(Decoded, "Animal has no type table"));
  EXPECT_TRUE(anyHas(Decoded, "does not implement the @Abstract method"));
  EXPECT_TRUE(anyHas(Decoded, "write @Override to replace it"));
  EXPECT_TRUE(anyHas(Decoded, "is final"));
  EXPECT_FALSE(Decoded.empty());
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good), std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
  MemoryRecordStream Again;
  TypeWriter(Again).writePackage(*P2);
  EXPECT_EQ(Again.toBytes(), Bytes);
}

// P03.24 (ADR-0024): a package that imports operator methods gets the same
// overloads, @Right and @Convert marks and the same errors as the package
// that declares them, from the decoded export data and from the source.
const char *OperatorSource = R"(package lib

type Vec2 struct{ X, Y float64 }

func (a Vec2) + (b Vec2) Vec2   { return a }
func (a Vec2) - () Vec2         { return a }
func (a Vec2) == (b Vec2) bool  { return true }
func (a Vec2) <=> (b Vec2) int  { return 0 }
@Right
func (a Vec2) * (s float64) Vec2 { return a }
func (a Vec2) * (s int) Vec2     { return a }
func (a Vec2) [] (i int) float64 { return a.X }
func (a *Vec2) []= (i int, v float64) {}
func (a *Vec2) ++() {}
func (a Vec2) () (x int) int { return x }
func (a Vec2) ! () bool { return true }

type Big struct{ n int }

@Convert
func (b Big) Float() float64 { return 1 }

type Ordered[T any] interface {
	<=> (T) int
}

func Max[T Ordered[T]](a, b T) bool { return a > b }

type Vec[T any] struct{ a, b T }

func (v Vec[T]) + (w Vec[T]) Vec[T] { panic(1) }
)";

TEST(Serialize, OperatorMethodsSurviveTheRoundTripAcrossPackages) {
  Loaded L(OperatorSource);
  ASSERT_NE(L.CP, nullptr);
  std::string Msgs;
  for (const Diagnostic &D : L.Collect.Diags)
    Msgs += D.Message + "\n";
  ASSERT_TRUE(L.Collect.Diags.empty()) << Msgs;
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  EXPECT_TRUE(Ctx2.operatorsSeen());
  EXPECT_EQ(dump(*P2), dump(*L.CP->Pkg));
  // The kind of each operator method is derived from its name and parameter
  // count on reading, as at its declaration; the marks are read back.
  for (const char *TN : {"Vec2", "Big", "Vec"}) {
    auto *Src = llvm::cast<NamedType>(L.CP->Pkg->PkgScope->lookupLocal(TN)->Ty);
    auto *Dec = llvm::cast<NamedType>(P2->PkgScope->lookupLocal(TN)->Ty);
    ASSERT_EQ(Src->Methods->size(), Dec->Methods->size()) << TN;
    for (size_t K = 0; K < Src->Methods->size(); ++K) {
      const Func *A = (*Src->Methods)[K], *B = (*Dec->Methods)[K];
      EXPECT_EQ(A->Name, B->Name);
      EXPECT_EQ(A->Op, B->Op) << TN << "." << std::string(A->Name);
      EXPECT_EQ(A->Right, B->Right) << TN << "." << std::string(A->Name);
    }
  }
  auto *Vec2T = llvm::cast<NamedType>(P2->PkgScope->lookupLocal("Vec2")->Ty);
  unsigned Rights = 0, Negs = 0, Subs = 0, Calls = 0;
  for (const Func *F : *Vec2T->Methods) {
    Rights += F->Right;
    Negs += F->Op == OpKind::Neg;
    Subs += F->Op == OpKind::Sub;
    Calls += F->Op == OpKind::Call;
  }
  EXPECT_EQ(Rights, 1u);
  EXPECT_EQ(Negs, 1u);
  EXPECT_EQ(Subs, 0u);
  EXPECT_EQ(Calls, 1u);
  auto *BigT = llvm::cast<NamedType>(P2->PkgScope->lookupLocal("Big")->Ty);
  ASSERT_EQ(BigT->Methods->size(), 1u);
  EXPECT_EQ((*BigT->Methods)[0]->Op, OpKind::Convert);
  std::string D = dump(*P2);
  EXPECT_NE(D.find("@Right func (Vec2) *(float64) Vec2"), std::string::npos)
      << D;
  EXPECT_NE(D.find("@Convert func (Big) Float() float64"), std::string::npos)
      << D;
  EXPECT_NE(D.find("func (*Vec2) []=(int, float64)"), std::string::npos) << D;

  const char *Bad = R"(package main
import "lib"
func f(a lib.Vec2, b lib.Big, n int) {
	_ = a + 1
	_ = a * "x"
	_ = a - a
	_ = a + a + b
	_ = float32(b)
	var g float64 = b
	_ = g
	_ = n + a
	_ = lib.Max(true, false)
	_ = a["k"]
	_ = a(true)
	a[0]++
}
)";
  const char *Good = R"(package main
import "lib"
type Mine struct{ n int }
func (m Mine) <=> (o Mine) int { return 0 }
func f(a lib.Vec2, b lib.Big) {
	_ = a + a
	_ = -a
	_ = !a
	_ = a == a
	_ = a != a
	_ = a < a
	_ = a * 2
	_ = a * 2.5
	_ = 2.5 * a
	_ = a[0]
	_ = a(1)
	_ = float64(b)
	_ = b.Float()
	_ = lib.Max(1, 2)
	_ = lib.Max(a, a)
	_ = lib.Max("a", "b")
	_ = lib.Max(Mine{}, Mine{})
	v := lib.Vec[int]{}
	_ = v + v
}
func g() {
	var a lib.Vec2
	a[0] = 1.5
	a++
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(
      anyHas(Decoded, "has no operator `+` for untyped int constant 1"));
  EXPECT_TRUE(anyHas(Decoded, "lib.Vec2 has no operator `*`"));
  EXPECT_TRUE(anyHas(Decoded, "has no operator `-`"));
  EXPECT_TRUE(anyHas(Decoded, "cannot convert b"));
  EXPECT_TRUE(anyHas(Decoded, "does not satisfy"));
  EXPECT_TRUE(anyHas(Decoded, "is not a place"));
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good), std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
  MemoryRecordStream Again;
  TypeWriter(Again).writePackage(*P2);
  EXPECT_EQ(Again.toBytes(), Bytes);
}

// P03.24 (EXP-19, EXP-22): `a[f()] += v` is checked as a read through `[]`,
// the binary method and a write through `[]=`, and the record the lowering
// reads (Info::Operators) gives the three calls one index expression, so
// `f()` runs once. The record is a function of the checked package, so it is
// tested next to the export data of the same package.
TEST(Serialize, OperatorCompoundIndexSharesTheIndex) {
  Loaded L(R"(package lib

type V struct{ X int }

func (a V) + (b V) V              { return a }
func (g Grid) [] (i int) V        { return V{} }
func (g *Grid) []= (i int, v V)   {}

type Grid struct{ n int }

func f() int { return 0 }

func Use(g Grid, v V) {
	g[f()] += v
}
)");
  ASSERT_NE(L.CP, nullptr);
  std::string Msgs;
  for (const Diagnostic &D : L.Collect.Diags)
    Msgs += D.Message + "\n";
  ASSERT_TRUE(L.Collect.Diags.empty()) << Msgs;
  const Info &I = L.CP->I;
  const ast::AssignStmt *AS = nullptr;
  const OperatorUse *Stmt = nullptr;
  for (auto &KV : I.Operators)
    if (auto *A = llvm::dyn_cast<ast::AssignStmt>(KV.first)) {
      AS = A;
      Stmt = &KV.second;
    }
  ASSERT_NE(AS, nullptr);
  EXPECT_EQ(Stmt->Op, OpKind::Add);
  ASSERT_NE(Stmt->Method, nullptr);
  ASSERT_NE(Stmt->Index, nullptr);
  ASSERT_NE(Stmt->IndexSet, nullptr);
  EXPECT_EQ(Stmt->Index->Op, OpKind::Index);
  EXPECT_EQ(Stmt->IndexSet->Op, OpKind::IndexSet);
  ASSERT_NE(Stmt->Call, nullptr);
  ASSERT_NE(Stmt->SetCall, nullptr);
  auto *Ix = llvm::dyn_cast<ast::IndexExpr>(AS->Lhs[0]);
  ASSERT_NE(Ix, nullptr);
  auto Read = I.Operators.find(Ix);
  ASSERT_NE(Read, I.Operators.end());
  ASSERT_NE(Read->second.Call, nullptr);
  EXPECT_EQ(Read->second.Op, OpKind::Index);
  // One index expression for the read and the write; the middle call takes
  // the element (the IndexExpr) and the right operand.
  ASSERT_EQ(Read->second.Call->Args.size(), 1u);
  ASSERT_EQ(Stmt->SetCall->Args.size(), 2u);
  EXPECT_EQ(Read->second.Call->Args[0], Ix->Indices[0]);
  EXPECT_EQ(Stmt->SetCall->Args[0], Ix->Indices[0]);
  ASSERT_EQ(Stmt->Call->Args.size(), 1u);
  EXPECT_EQ(Stmt->Call->Args[0], AS->Rhs[0]);
  EXPECT_EQ(llvm::cast<ast::SelectorExpr>(Stmt->Call->Fun)->X, Ix);
}

//===----------------------------------------------------------------------===//
// Round trips of whole corpora
//===----------------------------------------------------------------------===//

class NoImports : public Importer {
public:
  Package *import(llvm::StringRef, std::string &) override { return nullptr; }
};

/// Every package-level object, with the canonical text of the dump.
std::string dumpAll(const Package &P) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  dumpPackageAPI(OS, P, /*OnlyExported=*/false);
  return S;
}

std::string bytesOf(const Package &P) {
  MemoryRecordStream S;
  TypeWriter(S).writePackage(P, /*OnlyExported=*/false);
  return S.toBytes();
}

/// Checks Text as one package, writes all its objects, reads them into a new
/// context and compares. Returns "" on success, else what differs. Objects
/// counts the objects that went through.
std::string roundTrip(llvm::StringRef Text, bool GoMode, unsigned &Objects) {
  ParseOptions PO;
  PO.CheckLanguageRules = !GoMode;
  PO.AllowGenericMethods = !GoMode;
  PO.AllowNamespaceTypes = !GoMode;
  PO.AllowExtends = !GoMode;
  PO.AllowOperators = !GoMode;
  test::Parsed Pr(Text, PO);
  TypeContext Ctx(GoMode);
  NoImports Imp;
  CheckOptions CO;
  CO.GoCompat = GoMode;
  CO.Ownership = !GoMode;
  const ast::File *Files[] = {Pr.File};
  auto CP = checkPackage(Ctx, Pr.Src.SM, "p", Files, Imp, Pr.Src.Diags, CO);
  if (!CP || !CP->Pkg->PkgScope)
    return "";
  std::string Bytes = bytesOf(*CP->Pkg);
  MemoryRecordStream In;
  if (!In.fromBytes(Bytes))
    return "the bytes do not decode";
  TypeContext Ctx2(GoMode);
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  if (!P2)
    return "read failed: " + Err;
  if (dumpAll(*CP->Pkg) != dumpAll(*P2))
    return "the API differs:\n" + dumpAll(*CP->Pkg) + "---\n" + dumpAll(*P2);
  if (bytesOf(*P2) != Bytes)
    return "writing the copy gives other bytes";
  Objects += P2->PkgScope->Elems.size();
  return "";
}

void forEachFile(const char *Dir, bool GoMode, unsigned &Files,
                 unsigned &Objects) {
  std::vector<std::string> Paths;
  std::error_code EC;
  for (llvm::sys::fs::recursive_directory_iterator It(Dir, EC), End;
       It != End && !EC; It.increment(EC)) {
    llvm::StringRef Ext = llvm::sys::path::extension(It->path());
    if (GoMode ? Ext == ".go" : (Ext == ".gopp" || Ext == ".src"))
      Paths.push_back(It->path());
  }
  std::sort(Paths.begin(), Paths.end());
  for (const std::string &Path : Paths) {
    auto Buf = llvm::MemoryBuffer::getFile(Path);
    ASSERT_TRUE(static_cast<bool>(Buf)) << Path;
    EXPECT_EQ(roundTrip((*Buf)->getBuffer(), GoMode, Objects), "") << Path;
    ++Files;
  }
}

TEST(Serialize, ConformanceCorpusRoundTrips) {
  unsigned Files = 0, Objects = 0;
  forEachFile(GOPP_CONFORMANCE_DIR, /*GoMode=*/true, Files, Objects);
  EXPECT_GT(Files, 200u);
  EXPECT_GT(Objects, 2500u);
}

TEST(Serialize, SpecExamplesAndFuzzCorpusRoundTrip) {
  unsigned Files = 0, Objects = 0;
  for (const char *Dir :
       {GOPP_EXAMPLES_DIR, GOPP_FUZZ_CORPUS_DIR, GOPP_FUZZ_REGRESSIONS_DIR})
    forEachFile(Dir, /*GoMode=*/false, Files, Objects);
  EXPECT_GT(Files, 20u);
  EXPECT_GT(Objects, 100u);
}

//===----------------------------------------------------------------------===//
// Corrupt input
//===----------------------------------------------------------------------===//

/// Reads Bytes into a new context: no crash, and what it accepts can be
/// printed. True if the stream was accepted.
bool tryRead(llvm::StringRef Bytes) {
  MemoryRecordStream In;
  if (!In.fromBytes(Bytes))
    return false;
  TypeContext Ctx;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P = TypeReader(In, Ctx, Mem, PR).readPackage(Err);
  if (P)
    (void)dumpAll(*P);
  return P != nullptr;
}

TEST(Serialize, EveryTruncationIsRejected) {
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  std::string Good = bytesOf(*L.CP->Pkg);
  ASSERT_TRUE(tryRead(Good));
  // The package end record is last: no prefix is a whole package.
  for (size_t N = 0; N < Good.size(); ++N)
    EXPECT_FALSE(tryRead(llvm::StringRef(Good).take_front(N))) << N;
}

TEST(Serialize, RandomByteChangesNeverCrash) {
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  std::string Good = bytesOf(*L.CP->Pkg);
  uint32_t Seed = 12345;
  auto Rand = [&] {
    Seed = Seed * 1664525u + 1013904223u;
    return Seed >> 8;
  };
  for (unsigned Round = 0; Round < 3000; ++Round) {
    std::string Bad = Good;
    for (unsigned K = 0, N = 1 + Rand() % 3; K < N; ++K)
      Bad[Rand() % Bad.size()] = static_cast<char>(Rand());
    (void)tryRead(Bad);
  }
}

TEST(Serialize, EveryOperandOfEveryRecordCanBeWrong) {
  // Record by record and operand by operand: small, out of range and huge
  // values in every place. Nothing crashes, and the universe stays as it was.
  Loaded L;
  ASSERT_NE(L.CP, nullptr);
  MemoryRecordStream Good;
  TypeWriter(Good).writePackage(*L.CP->Pkg, /*OnlyExported=*/false);
  const std::vector<Record> &Recs = Good.records();
  TypeContext Ctx;
  Type *ErrorUnder = under(Ctx.errorType());
  unsigned Accepted = 0, Tried = 0;
  for (size_t R = 0; R < Recs.size(); ++R)
    for (size_t O = 0; O < Recs[R].Ops.size(); ++O)
      for (uint64_t V :
           {uint64_t(0), uint64_t(1), Recs[R].Ops[O] + 1, ~uint64_t(0)}) {
        MemoryRecordStream S;
        for (size_t K = 0; K < Recs.size(); ++K) {
          std::vector<uint64_t> Ops = Recs[K].Ops;
          if (K == R)
            Ops[O] = V;
          S.write(Recs[K].Code, Ops, Recs[K].Blob);
        }
        Arena Mem;
        Resolver PR;
        std::string Err;
        if (Package *P = TypeReader(S, Ctx, Mem, PR).readPackage(Err)) {
          (void)dumpAll(*P);
          ++Accepted;
        }
        ++Tried;
      }
  EXPECT_GT(Tried, 1000u);
  EXPECT_LT(Accepted, Tried);
  EXPECT_EQ(under(Ctx.errorType()), ErrorUnder);
}

TEST(Serialize, DeepTypesAreRejected) {
  // A chain of pointer types far deeper than any program writes.
  MemoryRecordStream S;
  S.write(REC_PKG_BEGIN, {0, 0});
  S.write(REC_BASIC, {static_cast<uint64_t>(BasicKind::Int), 0});
  for (uint64_t Id = 1; Id <= 100000; ++Id)
    S.write(REC_POINTER, {Id});
  S.write(REC_PKG_END, {});
  TypeContext Ctx;
  Arena Mem;
  Resolver PR;
  std::string Err;
  EXPECT_EQ(TypeReader(S, Ctx, Mem, PR).readPackage(Err), nullptr);
  EXPECT_EQ(Err, "type nested too deeply");
}

TEST(Serialize, UniverseTypesCannotBeRedefined) {
  // A definition record may only complete a type the stream declared: here
  // it names the universe's `error`.
  TypeContext Ctx;
  Type *ErrorUnder = under(Ctx.errorType());
  MemoryRecordStream S;
  S.write(REC_STRING, {}, "error");
  S.write(REC_PKG_BEGIN, {0, 0});
  S.write(REC_UNIVERSE, {1});
  S.write(REC_BASIC, {static_cast<uint64_t>(BasicKind::Int), 0});
  S.write(REC_NAMED_DEF, {1, 2, 0, 0});
  S.write(REC_PKG_END, {});
  Arena Mem;
  Resolver PR;
  std::string Err;
  EXPECT_EQ(TypeReader(S, Ctx, Mem, PR).readPackage(Err), nullptr);
  EXPECT_EQ(Err, "malformed named type definition");
  EXPECT_EQ(under(Ctx.errorType()), ErrorUnder);
}

TEST(Serialize, MalformedStreamsAreRejected) {
  TypeContext Ctx;
  Arena Mem;
  Resolver PR;
  std::string Err;
  {
    MemoryRecordStream S;
    S.write(REC_POINTER, {7}); // unknown element id
    EXPECT_EQ(TypeReader(S, Ctx, Mem, PR).readPackage(Err), nullptr);
    EXPECT_FALSE(Err.empty());
  }
  {
    MemoryRecordStream S; // no package header
    S.write(REC_PKG_END, {});
    EXPECT_EQ(TypeReader(S, Ctx, Mem, PR).readPackage(Err), nullptr);
  }
  {
    MemoryRecordStream S;
    EXPECT_FALSE(S.fromBytes(std::string("\x05\xff\xff", 3)));
  }
  {
    // A tuple that claims more elements than the record has.
    MemoryRecordStream S;
    S.write(REC_PKG_BEGIN, {0, 0});
    S.write(REC_TUPLE, {~uint64_t(0), 0});
    S.write(REC_PKG_END, {});
    EXPECT_EQ(TypeReader(S, Ctx, Mem, PR).readPackage(Err), nullptr);
  }
  {
    // A variadic signature whose last parameter is not a slice.
    MemoryRecordStream S;
    S.write(REC_PKG_BEGIN, {0, 0});
    S.write(REC_BASIC, {static_cast<uint64_t>(BasicKind::Int), 0});
    S.write(REC_TUPLE, {1, 1});
    S.write(REC_SIGNATURE, {2, 2, 1, 0, 0, 0});
    S.write(REC_PKG_END, {});
    EXPECT_EQ(TypeReader(S, Ctx, Mem, PR).readPackage(Err), nullptr);
    EXPECT_EQ(Err, "malformed signature");
  }
  {
    // Two objects of one name, and a named type without a definition.
    MemoryRecordStream S;
    S.write(REC_STRING, {}, "x");
    S.write(REC_PKG_BEGIN, {0, 0});
    S.write(REC_BASIC, {static_cast<uint64_t>(BasicKind::Int), 0});
    S.write(REC_OBJ_VAR, {1, 1});
    S.write(REC_OBJ_VAR, {1, 1});
    S.write(REC_PKG_END, {});
    EXPECT_EQ(TypeReader(S, Ctx, Mem, PR).readPackage(Err), nullptr);
    EXPECT_EQ(Err, "object declared twice");
    MemoryRecordStream S2;
    S2.write(REC_PKG_BEGIN, {0, 0});
    S2.write(REC_NAMED_DECL, {0, 0, 0, 0});
    S2.write(REC_PKG_END, {});
    EXPECT_EQ(TypeReader(S2, Ctx, Mem, PR).readPackage(Err), nullptr);
  }
}

// A parent checked in Go mode (a Go package, TYP-26) whose first field is an
// embedded one: the members it promotes stay reachable on a .gopp child
// (the chain lookup must leave a non-parent embedded field to the general
// walk, wherever it is in the chain), and an unknown name is still an error.
TEST(Serialize, GoModeParentWithEmbedding) {
  TypeContext Ctx;
  ParseOptions LPO;
  LPO.AllowExtends = false;
  LPO.AllowOperators = false;
  test::Parsed Lib(R"(package lib

type Inner struct{ N int }

func (i *Inner) Get() int { return i.N }

type Outer struct {
	Inner
	X int
}
)",
                   LPO);
  CheckOptions LCO;
  LCO.GoCompat = true;
  LCO.Ownership = false;
  const ast::File *LF[] = {Lib.File};
  OnePackage None("none", nullptr);
  auto LP = checkPackage(Ctx, Lib.Src.SM, "lib", LF, None, Lib.Src.Diags, LCO);
  ASSERT_TRUE(LP && Lib.errors().empty());
  auto Errs = checkMain(Ctx, LP->Pkg, R"(package main
import "lib"
type K extends lib.Outer { Y int }
type K2 extends K { Z int }
func use(o *lib.Outer, k *K, k2 *K2) int {
	return o.Get() + o.N + k.Get() + k.N + k.X + k2.Get() + k2.N + k2.X + k2.Y
}
func bad(k2 *K2) int { return k2.Nope }
)");
  ASSERT_EQ(Errs.size(), 1u) << (Errs.empty() ? "" : Errs[0]);
  EXPECT_NE(Errs[0].find("Nope"), std::string::npos) << Errs[0];
}

// R17: a type parameter that takes a method of T as a value (a method value,
// `go x.M()`, `T.M`) must not be a pointer to a tagged type when the method
// has a value receiver. The method names travel in the export data; a call
// `x.M()`, a pointer receiver and an untagged pointee stay valid, also
// through another generic of the importing package.
TEST(Serialize, PointerToTaggedAsTypeArgument) {
  Loaded L(R"(package lib

func MV[T interface{ Val() int }](x T) int { f := x.Val; return f() }

func Go[T interface{ Val() int }](x T) { go x.Val() }

func Call[T interface{ Val() int }](x T) int { return x.Val() }

func PMV[T interface{ PVal() int }](x T) int { f := x.PVal; return f() }

type Box[T interface{ Val() int }] struct{ X T }

func (b *Box[T]) Get() int { f := b.X.Val; return f() }
)");
  ASSERT_NE(L.CP, nullptr);
  ASSERT_TRUE(L.Collect.Diags.empty());
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  const char *Bad = R"(package main
import "lib"
type Animal struct{ N int }
@Virtual
func (a *Animal) Speak() int { return 0 }
func (a Animal) Val() int { return a.N }
func (a *Animal) PVal() int { return a.N }
func outer[U interface{ Val() int }](x U) int { return lib.MV(x) }
func f(a *Animal, b *lib.Box[*Animal]) int {
	lib.Go(a)
	return lib.MV(a) + outer(a)
}
)";
  const char *Good = R"(package main
import "lib"
type Animal struct{ N int }
@Virtual
func (a *Animal) Speak() int { return 0 }
func (a Animal) Val() int { return a.N }
func (a *Animal) PVal() int { return a.N }
type Plain struct{ N int }
func (p Plain) Val() int { return p.N }
func outer[U interface{ Val() int }](x U) int { return lib.Call(x) }
func g(a *Animal, p *Plain, b *lib.Box[*Plain]) int {
	return lib.Call(a) + lib.PMV(a) + lib.MV(p) + outer(a) + b.Get()
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad);
  auto Decoded = checkMain(Ctx2, P2, Bad);
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "*Animal does not satisfy the implicit "
                              "requirement of T: the generic code takes the "
                              "method Val of T as a value"));
  EXPECT_TRUE(anyHas(Decoded, "requirement of U: the generic code passes U "
                              "on to a generic that takes the method Val of "
                              "it as a value"));
  EXPECT_EQ(Decoded.size(), 4u) << (Decoded.empty() ? "" : Decoded[0]);
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good), std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good), std::vector<std::string>{});
}

TEST(Serialize, GoEmbeddingPromotedValueMethodIsNoParentPart) {
  // R23: a Go struct with an embedded field (Go's embedding, not an
  // `extends` parent): its promoted value method taken as a method value or
  // deferred copies a field of a whole value, not the parent part of a
  // child, in plain and in generic code. The receiver is the parent part
  // only when the last step to the method's owner is a parent part (`k.Show`
  // with Show on the parent lib.Outer).
  TypeContext Ctx;
  ParseOptions LPO;
  LPO.AllowExtends = false;
  LPO.AllowOperators = false;
  test::Parsed Lib(R"(package lib

type Inner struct{ N int }

func (i Inner) Get() int { return i.N }

type Outer struct {
	Inner
	X int
}

func (o Outer) Show() int { return o.X }
)",
                   LPO);
  CheckOptions LCO;
  LCO.GoCompat = true;
  LCO.Ownership = false;
  const ast::File *LF[] = {Lib.File};
  OnePackage None("none", nullptr);
  auto LP = checkPackage(Ctx, Lib.Src.SM, "lib", LF, None, Lib.Src.Diags, LCO);
  ASSERT_TRUE(LP && Lib.errors().empty());
  auto Errs = checkMain(Ctx, LP->Pkg, R"(package main
import "lib"
type K extends lib.Outer { Y int }
func use(o lib.Outer, k K) int {
	f := o.Get
	g := o.Inner.Get
	h := k.Get
	defer o.Get()
	return f() + g() + h()
}
type J interface{ Get() int }
func MV[T J](x T) int { f := x.Get; return f() }
func use2(o lib.Outer, k K) int { return MV(o) + MV(k) }
func bad(k K) int { s := k.Show; return s() }
)");
  ASSERT_EQ(Errs.size(), 1u) << (Errs.empty() ? "" : Errs[0]);
  EXPECT_NE(Errs[0].find("cannot use k.Show as a method value"),
            std::string::npos)
      << Errs[0];
}

TEST(Serialize, GoEmbeddingInsideChain) {
  // R27: a .gopp chain over a Go-mode parent that embeds Inner. A method of
  // Inner reached through the parent part has the field Inner as its
  // receiver (a whole value), whatever place holds it (`k.Outer.Get`,
  // `pt.Get` on a tagged child); a method declared on Outer itself is
  // inherited (its receiver is the parent part).
  TypeContext Ctx;
  ParseOptions LPO;
  LPO.AllowExtends = false;
  LPO.AllowOperators = false;
  test::Parsed Lib(R"(package lib

type Inner struct{ N int }

func (i Inner) Get() int { return i.N }

type Outer struct {
	Inner
	X int
}

func (o Outer) Own() int { return o.X }
)",
                   LPO);
  CheckOptions LCO;
  LCO.GoCompat = true;
  LCO.Ownership = false;
  const ast::File *LF[] = {Lib.File};
  OnePackage None("none", nullptr);
  auto LP = checkPackage(Ctx, Lib.Src.SM, "lib", LF, None, Lib.Src.Diags, LCO);
  ASSERT_TRUE(LP && Lib.errors().empty());
  auto Good = checkMain(Ctx, LP->Pkg, R"(package main
import "lib"
type K extends lib.Outer { Y int }
type T extends lib.Outer { Z int }
@Virtual
func (t *T) V() int { return 0 }
func good(k K, pk *K, pt *T) int {
	a := k.Get
	b := k.Outer.Inner.Get
	c := k.Outer.Get
	d := pt.Get
	e := (*pt).Get
	f := pk.Get
	return a() + b() + c() + d() + e() + f()
}
)");
  EXPECT_TRUE(Good.empty()) << (Good.empty() ? "" : Good[0]);
  auto Bad = checkMain(Ctx, LP->Pkg, R"(package main
import "lib"
type K extends lib.Outer { Y int }
func bad(k K, pk *K) int {
	a := k.Own
	b := pk.Own
	return a() + b()
}
)");
  EXPECT_EQ(Bad.size(), 2u) << (Bad.empty() ? "" : Bad[0]);
}

struct ByPath : Importer {
  std::map<std::string, Package *> M;
  Package *import(llvm::StringRef Want, std::string &) override {
    auto It = M.find(Want.str());
    return It == M.end() ? nullptr : It->second;
  }
};

/// The errors (offset-prefixed, as test::Parsed::errors gives them) by the
/// line of Text (1-based) they point at.
std::map<unsigned, std::string>
errorsByLine(llvm::StringRef Text, const std::vector<std::string> &Errs) {
  std::map<unsigned, std::string> Lines;
  for (const std::string &E : Errs) {
    size_t Off = std::strtoul(E.c_str(), nullptr, 10);
    Lines[1 + static_cast<unsigned>(Text.take_front(Off).count('\n'))] += E;
  }
  return Lines;
}

TEST(Serialize, GoEmbeddedPointerToTagged) {
  // R28: a Go struct embeds *a.Animal, a tagged Go++ type. Its promoted value
  // method taken as a method value copies *w.Animal, which may be the Animal
  // part of a Dog: the same copy as `p.Val` with p *a.Animal (E0395), and as
  // the written-out `w.Animal.Val`; in generic code too (`MV(w)`).
  TypeContext Ctx;
  test::Parsed A(R"(package a

type Animal struct{ N int }

@Virtual
func (x *Animal) Speak() int { return 0 }

func (x Animal) Val() int { return x.N }

type Dog extends Animal { X int }
)");
  CheckOptions ACO;
  ACO.Ownership = true;
  const ast::File *AF[] = {A.File};
  OnePackage None("none", nullptr);
  auto AP = checkPackage(Ctx, A.Src.SM, "a", AF, None, A.Src.Diags, ACO);
  ASSERT_TRUE(AP && A.errors().empty());
  ParseOptions LPO;
  LPO.AllowExtends = false;
  LPO.AllowOperators = false;
  test::Parsed Lib(R"(package lib

import "a"

type W struct {
	*a.Animal
}

type V struct {
	a.Dog
}
)",
                   LPO);
  CheckOptions LCO;
  LCO.GoCompat = true;
  LCO.Ownership = false;
  const ast::File *LF[] = {Lib.File};
  OnePackage OnlyA("a", AP->Pkg);
  auto LP = checkPackage(Ctx, Lib.Src.SM, "lib", LF, OnlyA, Lib.Src.Diags, LCO);
  ASSERT_TRUE(LP && Lib.errors().empty());
  test::Parsed Main(R"(package main
import (
	"a"
	"lib"
)
func plain(p *a.Animal) int {
	f := p.Val
	return f()
}
func viaGo(w lib.W) int {
	f := w.Val
	defer w.Val()
	return f()
}
func viaGo2(v lib.V) int {
	f := v.Val
	return f()
}
type J interface{ Val() int }
func MV[T J](x T) int { f := x.Val; return f() }
func viaGen(w lib.W) int { return MV(w) }
)");
  ByPath Imp;
  Imp.M["a"] = AP->Pkg;
  Imp.M["lib"] = LP->Pkg;
  CheckOptions CO;
  CO.Ownership = true;
  const ast::File *MF[] = {Main.File};
  checkPackage(Ctx, Main.Src.SM, "main", MF, Imp, Main.Src.Diags, CO);
  // plain p.Val, w.Val (twice), v.Val (inherited Val of a.Dog) and MV(w).
  auto Errs = Main.errors();
  ASSERT_EQ(Errs.size(), 5u) << (Errs.empty() ? "" : Errs[0]);
  EXPECT_TRUE(anyHas(Errs, "lib.W does not satisfy the implicit requirement "
                           "of T"));
}

TEST(Serialize, GoEmbeddedFieldKindsMatchTheWrittenOutSelector) {
  // R28: `w.Val` promoted through a Go embedded field and the written-out
  // `w.F.Val` give the same partial-place answer (E0395 or none) for every
  // kind of embedded field (a value, *T, Unique[*T], Shared[*T], View[T])
  // over a tagged type, an untagged one, an abstract one and an `extends`
  // child: one decision (receiverWhy). A method is not promoted through an
  // embedded Unique, Shared or View (the lookup does not look through them),
  // so there `w.Val` is undefined: nothing to compare, pinned below.
  TypeContext Ctx;
  const char *Types[] = {"Animal", "Config", "Shape", "Dog"};
  std::string AText = R"(package a

type Animal struct{ N int }

@Virtual
func (x *Animal) Speak() int { return 0 }

func (x Animal) Val() int { return x.N }

type Dog extends Animal { X int }

type Config struct{ N int }

func (c Config) Val() int { return c.N }

type Shape struct{ N int }

@Abstract
func (s *Shape) Area() int

func (s *Shape) Val() int { return s.N }
)";
  // The owning pointers and views are named through aliases (Go-mode code
  // embeds a type name).
  for (const char *T : Types)
    AText += std::string("type U") + T + " = Unique[*" + T + "]\ntype S" + T +
             " = Shared[*" + T + "]\ntype V" + T + " = View[" + T + "]\n";
  test::Parsed A(AText);
  CheckOptions ACO;
  ACO.Ownership = true;
  const ast::File *AF[] = {A.File};
  OnePackage None("none", nullptr);
  auto AP = checkPackage(Ctx, A.Src.SM, "a", AF, None, A.Src.Diags, ACO);
  ASSERT_TRUE(AP && A.errors().empty())
      << (A.errors().empty() ? "" : A.errors()[0]);
  // Kind: how the field is embedded, and its name.
  struct Kind {
    const char *Pre, *Field;
  };
  const Kind Kinds[] = {
      {"a.", ""}, {"*a.", ""}, {"a.U", "U"}, {"a.S", "S"}, {"a.V", "V"}};
  std::string LText = "package lib\n\nimport \"a\"\n\n";
  std::string MText = "package main\n\nimport \"lib\"\n\n";
  struct Pair {
    std::string What;
    unsigned Promoted, Written;
    bool Owning; // embedded as Unique, Shared or View
  };
  std::vector<Pair> Pairs;
  unsigned Line = 5;
  for (unsigned K = 0; K < 5; ++K)
    for (const char *T : Types) {
      std::string W = "W" + std::to_string(K) + T;
      std::string F = std::string(Kinds[K].Field) + T;
      LText += "type " + W + " struct {\n\t" + Kinds[K].Pre + T + "\n}\n";
      MText +=
          "func p" + W + "(w lib." + W + ") int { f := w.Val; return f() }\n";
      MText += "func q" + W + "(w lib." + W + ") int { f := w." + F +
               ".Val; return f() }\n";
      Pairs.push_back({std::string(Kinds[K].Pre) + T, Line, Line + 1, K >= 2});
      Line += 2;
    }
  ParseOptions LPO;
  LPO.AllowExtends = false;
  LPO.AllowOperators = false;
  test::Parsed Lib(LText, LPO);
  CheckOptions LCO;
  LCO.GoCompat = true;
  LCO.Ownership = false;
  const ast::File *LF[] = {Lib.File};
  OnePackage OnlyA("a", AP->Pkg);
  auto LP = checkPackage(Ctx, Lib.Src.SM, "lib", LF, OnlyA, Lib.Src.Diags, LCO);
  ASSERT_TRUE(LP && Lib.errors().empty())
      << (Lib.errors().empty() ? "" : Lib.errors()[0]);
  test::Parsed Main(MText);
  ByPath Imp;
  Imp.M["a"] = AP->Pkg;
  Imp.M["lib"] = LP->Pkg;
  CheckOptions CO;
  CO.Ownership = true;
  const ast::File *MF[] = {Main.File};
  checkPackage(Ctx, Main.Src.SM, "main", MF, Imp, Main.Src.Diags, CO);
  std::map<unsigned, std::string> Errs = errorsByLine(MText, Main.errors());
  auto Partial = [&](unsigned L) {
    return Errs[L].find("as a method value or deferred call") !=
           std::string::npos;
  };
  unsigned Rejected = 0, Accepted = 0;
  for (const Pair &P : Pairs) {
    if (Errs[P.Promoted].find("undefined") != std::string::npos) {
      EXPECT_TRUE(P.Owning)
          << "embedded " << P.What << ": " << Errs[P.Promoted];
      continue;
    }
    EXPECT_FALSE(P.Owning) << "embedded " << P.What << " promotes Val";
    EXPECT_EQ(Partial(P.Promoted), Partial(P.Written))
        << "embedded " << P.What << ": w.Val and the written-out selector "
        << "differ: " << Errs[P.Promoted] << " / " << Errs[P.Written];
    (Partial(P.Written) ? Rejected : Accepted) += 1;
  }
  // Both sides are there: a tagged pointee or an inherited method is
  // rejected (*Animal, Dog, *Dog), a value of Animal, Config or Shape and a
  // pointer to an untagged type are not.
  EXPECT_EQ(Rejected, 3u);
  EXPECT_EQ(Accepted, 5u);
}

TEST(Serialize, InheritedMethodOfAValueTypeArgument) {
  // R18: the uses of a type parameter travel in the export data as data
  // (kind, place, method name) and are decided in the importing package on
  // the real type argument, with the rule of the plain code: Val of a Dog
  // value is Animal.Val, so its method value copies the parent part.
  Loaded L(R"(package lib

func MV[T interface{ Val() int }](x T) int { f := x.Val; return f() }

func Go[T interface{ Val() int }](x T) { go x.Val() }

func Call[T interface{ Val() int }](x T) int { return x.Val() }
)");
  ASSERT_NE(L.CP, nullptr);
  ASSERT_TRUE(L.Collect.Diags.empty());
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*L.CP->Pkg);
  std::string Bytes = Stream.toBytes();
  MemoryRecordStream In;
  ASSERT_TRUE(In.fromBytes(Bytes));
  TypeContext Ctx2;
  Arena Mem;
  Resolver PR;
  std::string Err;
  Package *P2 = TypeReader(In, Ctx2, Mem, PR).readPackage(Err);
  ASSERT_NE(P2, nullptr) << Err;
  const char *Decls = R"(package main
import "lib"
type Animal struct{ N int }
@Virtual
func (a *Animal) Speak() int { return 0 }
func (a Animal) Val() int { return a.N }
type Dog extends Animal { Extra int }
type Pet struct{ N int }
@Virtual
func (p *Pet) Speak() int { return 0 }
type Cat extends Pet { Extra int }
func (c Cat) Val() int { return c.N }
)";
  std::string Bad = std::string(Decls) + R"(
func outer[U interface{ Val() int }](x U) int { return lib.MV(x) }
func f(d Dog) int {
	lib.Go(d)
	return lib.MV(d) + outer(d)
}
)";
  std::string Good = std::string(Decls) + R"(
func outer[U interface{ Val() int }](x U) int { return lib.Call(x) }
func g(d Dog, c Cat, a Animal) int {
	lib.Go(c)
	return lib.MV(c) + lib.MV(a) + lib.Call(d) + outer(d)
}
)";
  auto Direct = checkMain(L.Ctx, L.CP->Pkg, Bad.c_str());
  auto Decoded = checkMain(Ctx2, P2, Bad.c_str());
  EXPECT_EQ(Direct, Decoded);
  EXPECT_TRUE(anyHas(Decoded, "Dog does not satisfy the implicit requirement "
                              "of T: the generic code takes the method Val "
                              "of T as a value"));
  EXPECT_TRUE(anyHas(Decoded, "the method Val is inherited, so its receiver "
                              "is the parent part of Dog"));
  EXPECT_TRUE(anyHas(Decoded, "requirement of U: the generic code passes U "
                              "on to a generic that takes the method Val of "
                              "it as a value"));
  EXPECT_EQ(Decoded.size(), 3u) << (Decoded.empty() ? "" : Decoded[0]);
  EXPECT_EQ(checkMain(L.Ctx, L.CP->Pkg, Good.c_str()),
            std::vector<std::string>{});
  EXPECT_EQ(checkMain(Ctx2, P2, Good.c_str()), std::vector<std::string>{});
}

} // namespace
