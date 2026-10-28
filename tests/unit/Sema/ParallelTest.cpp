//===- ParallelTest.cpp - Parallel body checking (P03.10) -----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The diagnostics, and the typed side table, must be the same for any number
// of body threads (D3.4): same order, same text. The input is large and mixes
// what bodies share: generic instances created from several bodies, imports
// marked as used, method sets of one type, and errors of every kind spread
// over the bodies. Each configuration runs many times so that a different
// thread schedule has a chance to show.
//
//===----------------------------------------------------------------------===//

#include "gopp/AST/ASTContext.h"
#include "gopp/Parse/Parser.h"
#include "gopp/Sema/Sema.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace gopp;
using namespace gopp::sema;

namespace {

class UnsafeOnly : public Importer {
public:
  explicit UnsafeOnly(TypeContext &Ctx) : Ctx(Ctx) {}
  Package *import(llvm::StringRef Path, std::string &Error) override {
    if (Path == "unsafe")
      return Ctx.unsafePackage();
    Error = "no such package";
    return nullptr;
  }
  TypeContext &Ctx;
};

/// A package of `Funcs` functions with errors in every seventh function.
std::string makeSource(unsigned Funcs) {
  std::string S = R"(package big

import (
	"unsafe"
	w "unsafe"
	"missing/pkg"
)

type List[T any] struct{ items []T }

func (l *List[T]) Push(move v T) { l.items = append(l.items, v) }
func (l List[T]) Len() int     { return len(l.items) }
func Map[T, U any](xs []T, f func(T) U) []U {
	var out []U
	for _, x := range xs {
		out = append(out, f(x))
	}
	return out
}

type Shape interface{ Area() float64 }
type Named interface {
	Shape
	Name() string
}
type Sq struct{ side float64 }

func (s Sq) Area() float64 { return s.side * s.side }
func (s Sq) Name() string   { return "sq" }

type Base struct{ id int }

func (b *Base) ID() int { return b.id }

type Derived struct {
	Base
	List[int]
	tags map[string][]string
}

type Number interface{ ~int | ~int64 | ~float64 }

func Sum[N Number](xs []N) N {
	var t N
	for _, x := range xs {
		t += x
	}
	return t
}

type Tree[K comparable, V any] struct {
	left, right *Tree[K, V]
	key         K
	val         V
}

func (t *Tree[K, V]) Get(k K) bool {
	if t == nil {
		return false
	}
	if t.key == k {
		return true
	}
	return t.left.Get(k) || t.right.Get(k)
}

)";
  for (unsigned I = 0; I < Funcs; ++I) {
    std::string N = std::to_string(I);
    S += "func f" + N + "(a int, b string) (int, error) {\n";
    S += "\tx := a + " + N + "\n";
    S += "\tvar s []int\n\ts = append(s, x)\n";
    S += "\tfor i := range s {\n\t\t_ = i\n\t}\n";
    S += "\tvar l List[" +
         std::string(I % 3 == 0   ? "int"
                     : I % 3 == 1 ? "string"
                                  : "float64") +
         "]\n";
    S += "\tl.Push(" +
         std::string(I % 3 == 0   ? "1"
                     : I % 3 == 1 ? "\"s\""
                                  : "1.5") +
         ")\n";
    S += "\t_ = l.Len() + int(unsafe.Sizeof(x))\n";
    S += "\tvar sh Shape = Sq{2}\n\t_ = sh.Area()\n";
    S += "\tys := Map(s, func(v int) string { return \"k\" })\n\t_ = ys\n";
    S += "\tvar nm Named = Sq{1}\n\tvar sh2 Shape = nm\n\t_ = sh2\n";
    S += "\td := Derived{}\n\td.Push(a)\n\t_ = d.ID() + d.Len()\n";
    S += "\t_ = Sum([]int{1, 2, a}) + int(Sum([]int64{3}))\n\t_ = "
         "Sum([]float64{1.5, 2})\n";
    S += "\tvar tr *Tree[string, " + std::string(I % 2 ? "int" : "[]string") +
         "]\n";
    S += "\t_ = tr.Get(b)\n";
    S += "\tch := make(chan Shape, 1)\n\tch <- Sq{2}\n";
    S += "\tselect {\n\tcase v := <-ch:\n\t\t_ = v.Area()\n\tdefault:\n\t}\n";
    S +=
        "\tswitch v := sh.(type) {\n\tcase Sq:\n\t\t_ = v.side\n\tcase Named:\n"
        "\t\t_ = v.Name()\n\t}\n";
    S += "\tfn := (*List[int]).Push\n\t_ = fn\n";
    S += "\tdefer func() { _ = recover() }()\n";
    if (I % 7 == 0)
      S += "\tmissing" + N + "()\n";
    if (I % 11 == 0)
      S += "\tvar unused" + N + " int\n";
    if (I % 13 == 0)
      S += "\tvar bad int = \"text\"\n\t_ = bad\n";
    if (I % 17 == 0)
      S += "L" + N + ":\n\tfor {\n\t\tbreak\n\t}\n";
    S += "\treturn x, nil\n}\n\n";
  }
  return S;
}

struct Output {
  std::string Diags;
  std::string Types;
  unsigned NumBodies = 0;
};

Output run(const std::string &Src, unsigned Threads, unsigned ErrorLimit = 0,
           llvm::ThreadPoolInterface *Pool = nullptr) {
  SourceManager SM;
  auto F = SM.addBuffer(llvm::MemoryBuffer::getMemBufferCopy(Src, "big.gopp"));
  EXPECT_TRUE(static_cast<bool>(F));
  CollectingDiagnosticConsumer Quiet;
  DiagnosticsEngine ParseDiags(SM, Quiet, 0);
  ast::ASTContext AC;
  ast::File *AST = parseFile(AC, SM, *F, ParseDiags, ParseOptions{});
  EXPECT_FALSE(ParseDiags.hasErrors());
  const ast::File *Files[] = {AST};

  CollectingDiagnosticConsumer Sink;
  DiagnosticsEngine Diags(SM, Sink, ErrorLimit);
  TypeContext Ctx;
  UnsafeOnly Imp(Ctx);
  CheckOptions Opts;
  Opts.BodyThreads = Threads;
  Opts.MinBodiesPerThread = 1; // use the threads even for small packages
  Opts.Pool = Pool;
  auto CP = checkPackage(Ctx, SM, "big", Files, Imp, Diags, Opts);
  Output O;
  O.NumBodies = CP->NumBodies;
  for (const Diagnostic &D : Sink.Diags) {
    PresumedLoc PL = SM.getPresumedLoc(D.Loc);
    O.Diags += std::to_string(PL.Line) + ":" + std::to_string(PL.Column) +
               ": " + getSeverityName(D.Sev).str() + ": " + D.Message + "\n";
    for (const DiagNote &N : D.Notes)
      O.Diags += "  note: " + N.Message + "\n";
  }
  llvm::raw_string_ostream OS(O.Types);
  dumpTypes(OS, SM, *CP, Files);
  return O;
}

TEST(ParallelCheck, DiagnosticsAndTypesDoNotDependOnTheThreadCount) {
  std::string Src = makeSource(200);
  Output Ref = run(Src, 1);
  ASSERT_GT(Ref.NumBodies, 200u);
  // Errors of every kind are really there.
  EXPECT_NE(Ref.Diags.find("undefined: missing0"), std::string::npos);
  EXPECT_NE(Ref.Diags.find("declared and not used: unused0"),
            std::string::npos);
  EXPECT_NE(Ref.Diags.find("as int value in variable declaration"),
            std::string::npos);
  EXPECT_NE(Ref.Diags.find("imported as w and not used"), std::string::npos);
  EXPECT_EQ(Ref.Diags.find("\"unsafe\" imported and not used"),
            std::string::npos);
  // The output is in declaration order: f0's errors come before f7's.
  EXPECT_LT(Ref.Diags.find("undefined: missing0"),
            Ref.Diags.find("undefined: missing7"));
  for (unsigned Threads : {2u, 3u, 8u, 16u})
    for (int Rep = 0; Rep < 2; ++Rep) {
      Output O = run(Src, Threads);
      ASSERT_EQ(O.Diags, Ref.Diags) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Types, Ref.Types) << Threads << " threads, run " << Rep;
    }
}

TEST(ParallelCheck, TheGeneratedInputHasOnlyThePlantedErrors) {
  // Without this the comparison above could compare the same pile of
  // accidental errors. Functions 0..16 plant: missing (0, 7, 14), unused (0,
  // 11), bad int (0, 13), a label (0), and two import errors.
  Output O = run(makeSource(17), 1);
  unsigned Lines = 0;
  for (char C : O.Diags)
    Lines += C == '\n';
  EXPECT_EQ(Lines, 3u + 2u + 2u + 1u + 2u) << O.Diags;
}

TEST(ParallelCheck, SmallPackagesStayOnTheCallingThread) {
  // MinBodiesPerThread keeps a tiny package serial; the result is the same.
  std::string Src = makeSource(3);
  EXPECT_EQ(run(Src, 1).Diags, run(Src, 8).Diags);
}

TEST(ParallelCheck, ASharedPoolGivesTheSameOutput) {
  // The build driver lends its pool (CheckOptions::Pool) instead of phase 2
  // making threads of its own.
  std::string Src = makeSource(60);
  Output Ref = run(Src, 1);
  llvm::DefaultThreadPool Pool(llvm::hardware_concurrency(4));
  Output O = run(Src, 4, 0, &Pool);
  EXPECT_EQ(O.Diags, Ref.Diags);
  EXPECT_EQ(O.Types, Ref.Types);
}

TEST(ParallelCheck, ErrorLimitCutsAtTheSameDiagnostic) {
  // The limit applies to the diagnostics replayed in declaration order, so
  // the same first N errors are shown whichever body finished first.
  std::string Src = makeSource(80);
  for (unsigned Limit : {1u, 3u, 10u}) {
    Output Ref = run(Src, 1, Limit);
    EXPECT_NE(Ref.Diags.find("too many errors"), std::string::npos);
    for (int Rep = 0; Rep < 2; ++Rep)
      ASSERT_EQ(run(Src, 8, Limit).Diags, Ref.Diags)
          << "limit " << Limit << ", run " << Rep;
  }
}

TEST(ParallelCheck, ParametersStayOwnedByTheirFunction) {
  // Var::FuncOwner of a declared function's parameters is set in phase 1: a
  // closure that captures a parameter of its outer function sees it as the
  // outer function's, whatever the thread count.
  std::string Src = "package p\n\n";
  for (int K = 0; K < 60; ++K) {
    std::string N = std::to_string(K);
    Src += "func f" + N +
           "(a int, b string) (r int) {\n\tg := func() int { return a + "
           "len(b) }\n\tr = g()\n\treturn\n}\n";
    Src += "type T" + N + " struct{}\n\nfunc (x T" + N +
           ") m(a int) int { return a }\n";
  }
  Output Ref = run(Src, 1);
  EXPECT_EQ(Ref.Diags, "");
  for (int Rep = 0; Rep < 3; ++Rep)
    EXPECT_EQ(run(Src, 12).Types, Ref.Types);
}

TEST(ParallelCheck, InstancesMadeByManyBodiesAtOnce) {
  // Many bodies instantiate and expand the same generic types at the same
  // time, and read an instance in a composite literal while another body
  // expands it (a race TSan found): the instance is made and expanded once.
  std::string Src = "package p\n\ntype Box[T Copy] struct{ v T }\n\n"
                    "func (b Box[T]) Get() T { return b.v }\n"
                    "func (b *Box[T]) Set(v T) { b.v = v }\n\n"
                    "type Num interface{ ~int | ~float64 }\n\n"
                    "func Add[T Num](a, b T) T { return a + b }\n";
  for (int K = 0; K < 200; ++K)
    Src += "func u" + std::to_string(K) +
           "() int {\n\tvar a Box[int]\n\ta.Set(1)\n\tvar b Box[Box[int]]\n"
           "\t_ = b.Get().Get()\n\t_ = Add(a.Get(), 2)\n\t_ = Add(1.5, 2)\n"
           "\tc := Box[Box[int]]{}\n\t_ = c.Get().Get()\n"
           "\treturn Box[int]{v: 1}.Get() + Box[int]{2}.Get()\n}\n";
  Output Ref = run(Src, 1);
  EXPECT_EQ(Ref.Diags, "");
  for (int Rep = 0; Rep < 5; ++Rep) {
    Output O = run(Src, 16);
    ASSERT_EQ(O.Diags, Ref.Diags) << "run " << Rep;
    ASSERT_EQ(O.Types, Ref.Types) << "run " << Rep;
  }
}

/// The struct fields of a package as the side table shows them: where each
/// field is declared (the identifier), and what the field's Var itself says
/// (its position and package). Same-shaped structs are one uniqued type, so
/// the Vars are shared by the bodies that wrote them.
std::string fieldReport(const std::string &Src, unsigned Threads,
                        unsigned &SharedVars, bool &AnyPos) {
  SourceManager SM;
  auto F = SM.addBuffer(llvm::MemoryBuffer::getMemBufferCopy(Src, "s.gopp"));
  EXPECT_TRUE(static_cast<bool>(F));
  CollectingDiagnosticConsumer Quiet;
  DiagnosticsEngine ParseDiags(SM, Quiet, 0);
  ast::ASTContext AC;
  ast::File *AST = parseFile(AC, SM, *F, ParseDiags, ParseOptions{});
  const ast::File *Files[] = {AST};
  CollectingDiagnosticConsumer Sink;
  DiagnosticsEngine Diags(SM, Sink, 0);
  TypeContext Ctx;
  UnsafeOnly Imp(Ctx);
  CheckOptions Opts;
  Opts.BodyThreads = Threads;
  Opts.MinBodiesPerThread = 1;
  auto CP = checkPackage(Ctx, SM, "p", Files, Imp, Diags, Opts);
  EXPECT_TRUE(Sink.Diags.empty());
  std::vector<std::pair<uint32_t, std::string>> Lines;
  llvm::DenseSet<const Object *> Vars;
  unsigned Total = 0;
  for (auto &KV : CP->I.Defs) {
    auto *V = llvm::dyn_cast_or_null<Var>(KV.second);
    if (!V || !V->IsField)
      continue;
    ++Total;
    Vars.insert(V);
    AnyPos |= V->Pos.isValid();
    PresumedLoc PL = SM.getPresumedLoc(KV.first->getBegin());
    Lines.push_back({KV.first->getBegin().getRaw(),
                     std::to_string(PL.Line) + ":" + std::to_string(PL.Column) +
                         " " + V->Name.str() +
                         " pos=" + std::to_string(V->Pos.getRaw()) +
                         " pkg=" + (V->Pkg ? V->Pkg->Name.str() : "none")});
  }
  SharedVars = Total - Vars.size();
  llvm::sort(Lines);
  std::string R;
  for (auto &L : Lines)
    R += L.second + "\n";
  return R;
}

TEST(ParallelCheck, UniquedStructsCarryNoBodyData) {
  // The same struct type written in 100 bodies at different places is one
  // uniqued type; its fields must not remember the place of whichever body
  // made it first (that would depend on the thread schedule).
  std::string Src = "package p\n\n";
  for (int K = 0; K < 100; ++K)
    Src += std::string(K % 2 ? "\n\n" : "") + "func f" + std::to_string(K) +
           "() {\n\tvar s struct {\n\t\tA int\n\t\tb string\n\t}\n"
           "\t_ = s\n}\n";
  unsigned Shared1 = 0;
  bool Pos1 = false;
  std::string Ref = fieldReport(Src, 1, Shared1, Pos1);
  EXPECT_FALSE(Ref.empty());
  EXPECT_GT(Shared1, 0u) << "the struct is not uniqued: the test is empty";
  EXPECT_FALSE(Pos1) << Ref;
  for (int Rep = 0; Rep < 5; ++Rep) {
    unsigned SharedN = 0;
    bool PosN = false;
    EXPECT_EQ(fieldReport(Src, 16, SharedN, PosN), Ref) << "run " << Rep;
    EXPECT_FALSE(PosN);
    EXPECT_EQ(SharedN, Shared1);
  }
  // The other side: the identifier of each field is still known (Defs has
  // one entry per written field, with its own position).
  EXPECT_NE(Ref.find(" b pos=0 pkg=p"), std::string::npos) << Ref;
  EXPECT_NE(Ref.find(" A pos=0 pkg=none"), std::string::npos) << Ref;
  EXPECT_EQ(std::count(Ref.begin(), Ref.end(), '\n'), 200);
}

TEST(ParallelCheck, ErrorsOfASharedInterfaceInstanceDoNotMove) {
  // C[int, string] is one instance used by every body. Its type set is
  // computed by whichever body asks first; the duplicate method belongs to the
  // declaration (reported once, in phase 1), not to that body.
  std::string Src = "package p\n\ntype A[T any] interface{ M() T }\n\n"
                    "type C[T, U any] interface {\n\tA[T]\n\tA[U]\n}\n\n"
                    "type D[T, U any] interface {\n\tA[T]\n\tN() U\n}\n\n";
  for (int K = 0; K < 128; ++K) {
    std::string N = std::to_string(K);
    Src += "func f" + N +
           "() {\n\tvar c C[int, string]\n\t_ = c\n"
           "\tvar d D[int, string]\n\t_ = d\n\tundefined" +
           N + "()\n}\n";
  }
  Output Ref = run(Src, 1);
  size_t First = Ref.Diags.find("duplicate method M");
  ASSERT_NE(First, std::string::npos) << Ref.Diags;
  EXPECT_EQ(Ref.Diags.find("duplicate method M", First + 1), std::string::npos)
      << Ref.Diags;
  // Before the bodies' errors: it is the declaration's.
  EXPECT_LT(First, Ref.Diags.find("undefined: undefined0"));
  for (unsigned Threads : {2u, 8u, 16u})
    for (int Rep = 0; Rep < 5; ++Rep)
      ASSERT_EQ(run(Src, Threads).Diags, Ref.Diags)
          << Threads << " threads, run " << Rep;
}

TEST(ParallelCheck, AValidGenericInterfaceInstanceUsedByManyBodiesIsClean) {
  std::string Src = "package p\n\ntype A[T any] interface{ M() T }\n\n"
                    "type D[T, U any] interface {\n\tA[T]\n\tN() U\n}\n\n";
  for (int K = 0; K < 64; ++K)
    Src += "func f" + std::to_string(K) +
           "(d D[int, string]) (int, string) { return d.M(), d.N() }\n";
  for (unsigned Threads : {1u, 8u})
    EXPECT_EQ(run(Src, Threads).Diags, "") << Threads;
}

TEST(ParallelCheck, DeepGenericInterfaceChainsAreNotCutByTheFirstAsker) {
  // I_k[T] embeds I_{k-1}[T]; I0 has M0. Bodies reach I_n[int] for very
  // different n, in any order and on any thread. The type set of an instance
  // is shared by every body, so no body's depth may decide what is published:
  // the program is valid, whatever the schedule.
  for (unsigned Depth : {80u, 300u}) {
    std::string Src = "package p\n\ntype I0[T any] interface{ M0() T }\n";
    for (unsigned K = 1; K < Depth; ++K)
      Src += "type I" + std::to_string(K) + "[T any] interface{ I" +
             std::to_string(K - 1) + "[T] }\n";
    for (int K = 0; K < 64; ++K)
      Src += "func f" + std::to_string(K) + "() {\n\tvar x I" +
             std::to_string(K % 2 == 0 ? Depth - 1 : Depth / 5) +
             "[int]\n\tif x != nil {\n\t\t_ = x.M0()\n\t}\n}\n";
    EXPECT_EQ(run(Src, 1).Diags, "") << Depth;
    // The 300-deep chain is slow under a sanitizer: fewer runs.
    for (unsigned Threads : {2u, 8u, 16u})
      for (int Rep = 0; Rep < (Depth > 100 ? 1 : 5); ++Rep)
        ASSERT_EQ(run(Src, Threads).Diags, "")
            << Depth << " deep, " << Threads << " threads, run " << Rep;
  }
}

TEST(ParallelCheck, AnEndlesslyExpandingInterfaceStopsWithOneStableError) {
  std::string Src =
      "package p\n\ntype I[T any] interface {\n\tM()\n\tI[*T]\n}\n";
  for (int K = 0; K < 32; ++K)
    Src += "func f" + std::to_string(K) + "(x I[int]) { x.M() }\n";
  Output Ref = run(Src, 1);
  EXPECT_NE(Ref.Diags.find("invalid recursive type"), std::string::npos)
      << Ref.Diags;
  for (unsigned Threads : {2u, 8u})
    ASSERT_EQ(run(Src, Threads).Diags, Ref.Diags);
}

TEST(ParallelCheck, BrokenProgramsGiveTheSameDiagnostics) {
  const char *Inputs[] = {
      "package p\n\nfunc f() { undefined1(); var x int = \"s\"; _ = x }\n",
      "package p\n\nfunc f!() { return 1, 2, 3 }\nfunc g() { _ = f!() }\n",
      "package p\n\nfunc (x *T) drop() { x.f.g.h() }\nfunc (T) drop() {}\n",
      "package p\n\nfunc f[T any]() { var x T; x.y(); f[int, int]() }\n",
      "package p\n\ntype A B\ntype B A\nfunc f(a A) { a.x() }\n",
      "package p\n\nconst c = 1 << 100000\nfunc f() { _ = c }\n",
  };
  for (const char *In : Inputs) {
    std::string Ref = run(In, 1).Diags;
    EXPECT_EQ(run(In, 2).Diags, Ref) << In;
    EXPECT_EQ(run(In, 8).Diags, Ref) << In;
  }
}

/// Every file of the ported Go conformance corpus gives the same diagnostics
/// on one thread and on many. The files are small, so MinBodiesPerThread is 1
/// to put the bodies on different threads anyway.
TEST(ParallelCheck, ConformanceCorpusGivesTheSameDiagnostics) {
  std::vector<std::string> Paths;
  std::string Root = std::string(GOPP_CONFORMANCE_DIR) + "/testdata";
  std::error_code EC;
  for (llvm::sys::fs::recursive_directory_iterator It(Root, EC), End;
       It != End && !EC; It.increment(EC))
    if (llvm::sys::path::extension(It->path()) == ".go")
      Paths.push_back(It->path());
  std::sort(Paths.begin(), Paths.end());
  ASSERT_GT(Paths.size(), 200u);
  auto Run = [](const std::string &Path, unsigned Threads) {
    SourceManager SM;
    auto F = SM.addFile(Path);
    if (!F) {
      llvm::consumeError(F.takeError());
      return std::string("unreadable");
    }
    CollectingDiagnosticConsumer Sink;
    DiagnosticsEngine Diags(SM, Sink, 0);
    ast::ASTContext AC;
    ParseOptions PO;
    PO.CheckLanguageRules = false;
    PO.AllowGenericMethods = false;
    PO.AllowNamespaceTypes = false;
    ast::File *AST = parseFile(AC, SM, *F, Diags, PO);
    TypeContext Ctx(true);
    UnsafeOnly Imp(Ctx);
    CheckOptions CO;
    CO.GoCompat = true;
    CO.Ownership = false;
    CO.BodyThreads = Threads;
    CO.MinBodiesPerThread = 1;
    const ast::File *Files[] = {AST};
    checkPackage(Ctx, SM, "p", Files, Imp, Diags, CO);
    std::string Out;
    for (const Diagnostic &D : Sink.Diags)
      Out += std::to_string(D.Loc.isValid() ? SM.getLineNumber(D.Loc) : 0) +
             ": " + D.Message + "\n";
    return Out;
  };
  for (const std::string &P : Paths)
    EXPECT_EQ(Run(P, 1), Run(P, 8)) << P;
}

} // namespace
