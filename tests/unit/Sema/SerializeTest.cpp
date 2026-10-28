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
func Take(s ref string, move p Point) int   { return 0 }
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
var Callback func(int, ...int) (bool, error)
var Embedded struct {
	Point
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
	Open func() @Required
}

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
  Loaded() {
    std::ofstream(T.Root + "/lib.gopp") << LibSource;
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
           "func Take(ref string, Point) int",
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
func f() {
	u := lib.User{ID: 1}
	t := lib.Team{Lead: u, Size: 2}
	var b lib.Box[int]
	var p lib.Point
	g := lib.Gate{Open: func() {}}
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

} // namespace
