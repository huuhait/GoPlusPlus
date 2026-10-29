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
#include "gopp/Sema/Serialize.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ThreadPool.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

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

type Derived extends Base {
	items List[int]
	tags  map[string][]string
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
	left, right Option[*Tree[K, V]]
	key         K
	val         V
}

func (t *Tree[K, V]) Get(k K) bool {
	if t.key == k {
		return true
	}
	if l := t.left; l != nil && l.Get(k) {
		return true
	}
	if r := t.right; r != nil {
		return r.Get(k)
	}
	return false
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
    S += "\td := Derived{}\n\td.items.Push(a)\n\t_ = d.ID() + d.items.Len()\n";
    S += "\t_ = Sum([]int{1, 2, a}) + int(Sum([]int64{3}))\n\t_ = "
         "Sum([]float64{1.5, 2})\n";
    S += "\ttr := new_unique(Tree[string, " +
         std::string(I % 2 ? "int" : "[]string") + "]{})\n";
    S += "\t_ = tr.Get(b)\n";
    S += "\tch := make(chan Shape, 1)\n\tch <- Sq{2}\n";
    S += "\tselect {\n\tcase v, ok := <-ch:\n\t\tif ok {\n\t\t\t_ = "
         "v.Area()\n\t\t}\n\tdefault:\n\t}\n";
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
  /// The package written by the type serializer (P03.11): it records "has
  /// no zero value" per named type, which bodies fill lazily.
  std::string Export;
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
  MemoryRecordStream Stream;
  TypeWriter(Stream).writePackage(*CP->Pkg);
  O.Export = Stream.toBytes();
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

/// Many bodies that use types with no zero value (P03.15), Option flow typing
/// (P03.17) and the errors of both. Function I plants an error for each of
/// the divisors 3, 4, 5, 6, 7, 8, 9, 10, 11 that divides I, and two for 13;
/// every body also contains the matching good code, which must stay clean.
/// C0 .. C59 is a chain of structs, and W[W[...[*Node]]] a chain of generic
/// instances, deeper than any depth limit of the zero-value query used to be:
/// whichever body asks first, every link has no zero value. The instances in
/// the signature of Keep are asked about by bodies only; their "no zero
/// value" is in the serialized package, which must not depend on the threads
/// either.
constexpr unsigned ChainLen = 60, InstDepth = 45;
std::string nestedW(unsigned Depth) {
  std::string T = "*Node";
  for (unsigned K = 0; K < Depth; ++K)
    T = "W[" + T + "]";
  return T;
}
std::string makeNoZeroSource(unsigned Funcs) {
  std::string Chain;
  for (unsigned K = 0; K + 1 < ChainLen; ++K)
    Chain += "type C" + std::to_string(K) + " struct{ c C" +
             std::to_string(K + 1) + " }\n";
  Chain += "type C" + std::to_string(ChainLen - 1) + " struct{ p *Node }\n";
  Chain += "type W[T any] struct{ v T }\n\n";
  std::string S = Chain + R"(

type User struct {
	ID   int @Required
	Name string
}
type Team struct {
	Lead User
	N    int
}
type Box[T any] struct{ V T }
type Node struct {
	Val  int
	Next Option[*Node]
}

func mk(id int) User { return User{ID: id} }

func Keep(b Box[Team], w W[W[Team]]) {}

)";
  for (unsigned I = 0; I < Funcs; ++I) {
    std::string N = std::to_string(I);
    S += "func g" + N + "(head Option[*Node], move u User, n int) int {\n";
    S += "\tt := Team{Lead: mk(" + N + "), N: " + N + "}\n";
    S += "\tb := Box[User]{V: u}\n\tvar bi Box[int]\n\t_, _ = b, bi\n";
    S += "\txs := make([]User, 0, 4)\n\txs = append(xs, mk(" + N + "))\n";
    S += "\ttotal := t.N + len(xs)\n";
    S += "\tfor p := head; p != nil; p = p.Next {\n\t\ttotal += p.Val\n\t}\n";
    S += "\tif head == nil {\n\t\treturn total\n\t}\n";
    S += "\ttotal += head.Val\n";
    S += "\tif q := head.Next; q != nil && q.Val > 0 {\n\t\ttotal += "
         "q.Val\n\t}\n";
    S += "\tm := map[string]User{}\n\tif w, ok := move(m[\"a\"]); ok {\n\t\t_ "
         "= w\n\t}\n";
    if (I % 3 == 0)
      S += "\tvar bu User\n\t_ = bu\n";
    if (I % 4 == 0)
      S += "\t_ = User{Name: \"x\"}\n";
    if (I % 5 == 0)
      S += "\to := head.Next\n\t_ = o.Val\n";
    if (I % 6 == 0)
      S += "\tys := make([]User, n)\n\t_ = ys\n";
    if (I % 7 == 0)
      S += "\tvar pp *Node\n\t_ = pp\n";
    if (I % 8 == 0)
      S += "\tv := m[\"a\"]\n\t_ = v\n";
    if (I % 9 == 0)
      S += "\tvar bb Box[User]\n\t_ = bb\n";
    if (I % 10 == 0)
      S += "\tvar cc C" + std::to_string(I % ChainLen) + "\n\t_ = cc\n";
    if (I % 11 == 0)
      S += "\tvar ww " + nestedW(InstDepth - I % 7) + "\n\t_ = ww\n";
    if (I % 13 == 0)
      S += "\tvar bt Box[Team]\n\tvar wt W[W[Team]]\n\t_, _ = bt, wt\n";
    S += "\treturn total\n}\n\n";
  }
  return "package nz\n\n" + S;
}

TEST(ParallelCheck, NoZeroValueTypesAndOptionFlowDoNotDependOnTheThreadCount) {
  const unsigned Funcs = 120;
  std::string Src = makeNoZeroSource(Funcs);
  Output Ref = run(Src, 1);
  ASSERT_GT(Ref.NumBodies, Funcs);
  // The planted errors are all there, and only they (both sides: the good
  // code in every body stays clean).
  unsigned Want = 0;
  for (unsigned I = 0; I < Funcs; ++I)
    for (unsigned D : {3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 13u, 13u})
      Want += I % D == 0;
  unsigned Errors = 0;
  for (size_t Pos = 0;
       (Pos = Ref.Diags.find(": error: ", Pos)) != std::string::npos; ++Pos)
    ++Errors;
  EXPECT_EQ(Errors, Want) << Ref.Diags.substr(0, 3000);
  std::string Deep = nestedW(InstDepth) + " has no zero value";
  for (const char *Msg :
       {"User has no zero value", "missing required field ID in User literal",
        "o may be nil", "*Node has no zero value",
        "Box[User] has no zero value", "may not exist", "C0 has no zero value",
        "C50 has no zero value", Deep.c_str()})
    EXPECT_NE(Ref.Diags.find(Msg), std::string::npos)
        << Msg << "\n"
        << Ref.Diags.substr(0, 3000);
  for (unsigned Threads : {2u, 8u, 16u})
    for (int Rep = 0; Rep < 2; ++Rep) {
      Output O = run(Src, Threads);
      ASSERT_EQ(O.Diags, Ref.Diags) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Types, Ref.Types) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Export, Ref.Export) << Threads << " threads, run " << Rep;
    }
}

TEST(ParallelCheck,
     NeverNilInterfacesAndCheckedResultsDoNotDependOnTheThreadCount) {
  // P03.18: funcs and interfaces with methods have no zero value (the answer
  // is cached per named type and shared by every body), and the err / ok
  // flow typing is per body (loops, the uses of function literals reported
  // at the end of the body). Many bodies ask about the same named types, in
  // any order, with good and bad code side by side.
  std::string Src = "package p\n\ntype Logger interface{ Log(s string) }\n"
                    "type MyErr error\n"
                    "type Holder struct{ L Logger }\n"
                    "type Wrap struct{ H Holder }\n"
                    "type Opt struct{ L Option[Logger]; F Option[func()] }\n"
                    "type U struct{ N string @Required }\n"
                    "func load() (U, error) { return U{N: \"a\"}, nil }\n"
                    "func use(u U) {}\n";
  const unsigned Funcs = 120;
  for (unsigned I = 0; I < Funcs; ++I) {
    std::string N = std::to_string(I);
    Src += "func good" + N +
           "(m map[string]U, ch chan U, e error, o Opt) error {\n"
           "\tu, err := load()\n\tif err != nil {\n\t\treturn err\n\t}\n"
           "\tuse(u)\n\tif v, ok := <-ch; ok {\n\t\tuse(v)\n\t}\n"
           "\tfor w, err := load(); err == nil; w, err = load() {\n"
           "\t\tuse(w)\n\t}\n"
           "\tc := u\n\tdefer func() { use(c) }()\n"
           "\tif f := o.F; f != nil {\n\t\tf()\n\t}\n"
           "\tvar x any\n\t_ = x == nil\n\treturn e\n}\n";
    if (I % 3 == 0)
      Src += "func bad" + N +
             "(l Logger, e error) {\n\tvar w Wrap\n\t_ = w\n"
             "\tu, err := load()\n\tuse(u)\n\t_ = err\n"
             "\tif err != nil {\n\t\treturn\n\t}\n"
             "\tgo func() { use(u) }()\n"
             "\tfor i := 0; i < 2; i++ {\n\t\tuse(u)\n"
             "\t\tu, err = load()\n\t}\n"
             "\t_ = l == nil\n\tvar m MyErr = e\n\t_ = m\n}\n";
  }
  Output Ref = run(Src, 1);
  for (const char *Msg :
       {"Wrap has no zero value", "u may be unset here",
        "a function literal can run at any time", "Logger is never nil",
        "e may be nil; MyErr is never nil"})
    EXPECT_NE(Ref.Diags.find(Msg), std::string::npos)
        << Msg << "\n"
        << Ref.Diags.substr(0, 3000);
  // Only the planted errors: six in each bad function, none in good ones.
  unsigned Errors = 0;
  for (size_t Pos = 0;
       (Pos = Ref.Diags.find(": error: ", Pos)) != std::string::npos; ++Pos)
    ++Errors;
  EXPECT_EQ(Errors, 6u * ((Funcs + 2) / 3)) << Ref.Diags.substr(0, 3000);
  for (unsigned Threads : {2u, 8u, 16u})
    for (int Rep = 0; Rep < 2; ++Rep) {
      Output O = run(Src, Threads);
      ASSERT_EQ(O.Diags, Ref.Diags) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Types, Ref.Types) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Export, Ref.Export) << Threads << " threads, run " << Rep;
    }
}

TEST(ParallelCheck, NilSafetyChecksDoNotDependOnTheThreadCount) {
  // P03.19: the loop fixpoint (assumptions decided at the end of each body,
  // uses made the Option again where it fits, which rewrites their recorded
  // types), known-nil channels, temporary maps and writes into an interface
  // box are per body. Good and bad bodies side by side.
  std::string Src = "package p\n\ntype Node struct{ V int }\n"
                    "func firstO() Option[*Node]\n"
                    "func nextO(n *Node) Option[*Node]\n"
                    "func anyO() Option[*Node]\n"
                    "func first() (*Node, error)\n"
                    "func next(n *Node) (*Node, error)\n"
                    "func use(n *Node) {}\n"
                    "func keep(o Option[*Node]) {}\n"
                    "func f() map[string]int\n"
                    "func pick() bool { return true }\n";
  const unsigned Funcs = 60;
  for (unsigned I = 0; I < Funcs; ++I) {
    std::string N = std::to_string(I);
    Src += "func good" + N +
           "(x any, xs []int) error {\n"
           "\tm := f()\n\tm[\"a\"] = 1\n"
           "\tif d, ok := x.(map[string]int); ok {\n"
           "\t\tc := d.Clone()\n\t\tc[\"b\"] = 2\n\t}\n"
           "\tch := make(chan int)\n\tclose(ch)\n"
           "\to := firstO()\n\tif o == nil {\n\t\treturn nil\n\t}\n"
           "\tfor range xs {\n\t\tuse(o)\n\t\tkeep(o)\n\t\tc := o\n"
           "\t\tkeep(c)\n\t\to = nextO(o)\n\t\tif o == nil {\n"
           "\t\t\treturn nil\n\t\t}\n\t}\n"
           "\tn, err := first()\n\tif err != nil {\n\t\treturn err\n\t}\n"
           "\tfor {\n\t\tuse(n)\n\t\tn, err = next(n)\n"
           "\t\tif err != nil {\n\t\t\treturn err\n\t\t}\n\t}\n}\n";
    if (I % 3 == 0)
      Src += "func bad" + N +
             "(x any, xs []int) {\n"
             "\to := firstO()\n\tif o == nil {\n\t\treturn\n\t}\n"
             "\tfor range xs {\n\t\tuse(o)\n\t\tkeep(o)\n\t\to = anyO()\n\t}\n"
             "\tf()[\"a\"] = 1\n\tx.(map[string]int)[\"b\"] = 2\n"
             "\tvar ch chan int\n\tclose(ch)\n"
             "\tfor {\n\tL:\n\t\tif pick() {\n\t\t\treturn\n\t\t}\n"
             "\t\tif pick() {\n\t\t\tgoto L\n\t\t}\n\t}\n}\n";
  }
  Output Ref = run(Src, 1);
  for (const char *Msg :
       {"o may be nil; check o != nil first (the loop assigns o",
        "this map is a temporary", "inside an interface value",
        "ch is nil on every path to this close"})
    EXPECT_NE(Ref.Diags.find(Msg), std::string::npos)
        << Msg << "\n"
        << Ref.Diags.substr(0, 3000);
  // Only the planted errors: four in each bad function, none in good ones.
  unsigned Errors = 0;
  for (size_t Pos = 0;
       (Pos = Ref.Diags.find(": error: ", Pos)) != std::string::npos; ++Pos)
    ++Errors;
  EXPECT_EQ(Errors, 4u * ((Funcs + 2) / 3)) << Ref.Diags.substr(0, 3000);
  for (unsigned Threads : {2u, 8u, 16u})
    for (int Rep = 0; Rep < 2; ++Rep) {
      Output O = run(Src, Threads);
      ASSERT_EQ(O.Diags, Ref.Diags) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Types, Ref.Types) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Export, Ref.Export) << Threads << " threads, run " << Rep;
    }
}

TEST(ParallelCheck, StrictStructsAndCloneWitnessesDoNotDependOnTheThreadCount) {
  // P03.20: @Strict structs, @Option fields, the conversion rule and the
  // clone witness path, many of each: a chain of nested @Strict types (no
  // zero value spreads down the chain), siblings that use them, and
  // `Clone()` on a nest of structs ending in a type with a drop method. Also
  // `V T @Option` judged per instance in bodies, and conversions that ask
  // whether a field type has a zero value (a pointer field of a @Strict
  // struct and of a plain one). Check time must stay near-linear (a generous
  // bound catches a quadratic walk).
  const unsigned Types = 400, Funcs = 200;
  std::string Src = "package st\n\ntype Mu struct{ b bool }\n"
                    "func (m *Mu) drop() {}\n"
                    "@Strict\ntype BoxOpt[T any] struct {\n\tN int\n"
                    "\tV T @Option\n}\n"
                    "@Strict\ntype SK struct {\n\tID int\n\tP *int\n}\n"
                    "type MK struct {\n\tID int @Required\n\tP *int\n}\n";
  for (unsigned I = 0; I < Types; ++I) {
    std::string N = std::to_string(I), M = std::to_string(I + 1);
    Src += "@Strict\ntype S" + N +
           " struct {\n\tID int\n\tNote Option[string]\n"
           "\tR int @Option\n\t_ int\n";
    if (I + 1 < Types)
      Src += "\tNext S" + M + "\n";
    Src += "}\n";
    Src += "type P" + N +
           " struct {\n\tID int @Required\n\tNote Option[string]\n"
           "\tR int\n\t_ int\n}\n";
    Src += "type C" + N + " struct {\n\tmu Mu\n";
    if (I > 0)
      Src += "\tIn C" + std::to_string(I - 1) + "\n";
    Src += "}\n";
  }
  for (unsigned I = 0; I < Funcs; ++I) {
    std::string N = std::to_string(I % Types);
    Src += "func good" + std::to_string(I) + "(x S" + N + ", k SK) P" + N +
           " {\n"
           "\ty := S" +
           N +
           "{ID: 1, Next: x.Next.Clone()}\n"
           "\t_ = y\n\t_ = MK(k)\n\t_ = BoxOpt[int]{N: 1}\n\treturn P" +
           N + "{ID: 2}\n}\n";
    if (I % 5 == 0)
      Src += "func bad" + std::to_string(I) + "(p P" + N + ", s S" + N +
             ") {\n"
             "\t_ = S" +
             N +
             "(p)\n"
             "\tvar z S" +
             N +
             "\n\t_ = z\n"
             "\t_ = S" +
             N +
             "{}\n"
             "\t_ = BoxOpt[S" +
             N +
             "]{N: 1, V: s.Clone()}\n}\n"
             "func cl" +
             std::to_string(I) + "(c C" + N +
             ") {\n"
             "\t_ = c.Clone()\n}\n";
  }
  Output Ref = run(Src, 1);
  for (const char *Msg :
       {"has no zero value (field ID is required (@Strict))",
        "missing required fields ID, Next in S0 literal",
        "can't be cloned: field mu (Mu)", "cannot convert",
        "BoxOpt[S0]: field V is @Option, but S0 has no zero value"})
    EXPECT_NE(Ref.Diags.find(Msg), std::string::npos)
        << Msg << "\n"
        << Ref.Diags.substr(0, 3000);
  // Bad functions: one conversion error, one zero value, one literal error,
  // one BoxOpt instance error, one clone error each (the literal error lists
  // the missing fields).
  unsigned Errors = 0;
  for (size_t Pos = 0;
       (Pos = Ref.Diags.find(": error: ", Pos)) != std::string::npos; ++Pos)
    ++Errors;
  EXPECT_EQ(Errors, 5u * ((Funcs + 4) / 5)) << Ref.Diags.substr(0, 3000);
  auto Start = std::chrono::steady_clock::now();
  for (unsigned Threads : {2u, 8u, 16u})
    for (int Rep = 0; Rep < 2; ++Rep) {
      Output O = run(Src, Threads);
      ASSERT_EQ(O.Diags, Ref.Diags) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Types, Ref.Types) << Threads << " threads, run " << Rep;
      ASSERT_EQ(O.Export, Ref.Export) << Threads << " threads, run " << Rep;
    }
  auto Secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - Start)
          .count();
  EXPECT_LT(Secs, 30.0) << "six checks of " << Types << " nested types";
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
      Src += "func f" + std::to_string(K) + "(x I" +
             std::to_string(K % 2 == 0 ? Depth - 1 : Depth / 5) +
             "[int]) {\n\t_ = x.M0()\n}\n";
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
    PO.AllowExtends = false;
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
