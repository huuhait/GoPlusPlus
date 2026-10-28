//===- GoParserPortTest.cpp - Go's go/parser tests, ported ----------------===//
//
// Part of the Go++ project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Test cases ported from Go's src/go/parser/short_test.go and from the
// generics testdata (src/go/parser/testdata/*.go2):
//   Copyright 2009 The Go Authors. All rights reserved.
//   Use of this source code is governed by a BSD-style license (see NOTICE).
// The "invalid" programs carry Go's /* ERROR "msg" */ markers: the marker is
// placed where Go reports the error. Go++ messages are not Go's (tailored
// messages are P02.07), so a case passes when an error is reported, and the
// position is checked where it matches Go's (PositionAgrees).
//
//===----------------------------------------------------------------------===//

#include "../Support/FrontendTestSupport.h"

#include "gtest/gtest.h"

using namespace gopp;
using namespace gopp::test;

namespace {

const char *const Valids[] = {
    "package p\n",
    R"(package p;)",
    R"(package p; import "fmt"; func f() { fmt.Println("Hello, World!") };)",
    R"(package p; func f() { if f(T{}) {} };)",
    R"(package p; func f() { _ = <-chan int(nil) };)",
    R"(package p; func f() { _ = (<-chan int)(nil) };)",
    R"(package p; func f() { _ = (<-chan <-chan int)(nil) };)",
    R"(package p; func f() { _ = <-chan <-chan <-chan <-chan <-int(nil) };)",
    R"(package p; func f(func() func() func());)",
    R"(package p; func f(...T);)",
    R"(package p; func f(float, ...int);)",
    R"(package p; func f(x int, a ...int) { f(0, a...); f(1, a...,) };)",
    R"(package p; func f(int,) {};)",
    R"(package p; func f(...int,) {};)",
    R"(package p; func f(x ...int,) {};)",
    R"(package p; type T []int; var a []bool; func f() { if a[T{42}[0]] {} };)",
    R"(package p; type T []int; func g(int) bool { return true }; func f() { if g(T{42}[0]) {} };)",
    R"(package p; type T []int; func f() { for _ = range []int{T{42}[0]} {} };)",
    R"(package p; var a = T{{1, 2}, {3, 4}})",
    R"(package p; func f() { select { case <- c: case c <- d: case c <- <- d: case <-c <- d: } };)",
    R"(package p; func f() { select { case x := (<-c): } };)",
    R"(package p; func f() { if ; true {} };)",
    R"(package p; func f() { switch ; {} };)",
    R"(package p; func f() { for _ = range "foo" + "bar" {} };)",
    R"(package p; func f() { var s []int; g(s[:], s[i:], s[:j], s[i:j], s[i:j:k], s[:j:k]) };)",
    R"(package p; var ( _ = (struct {*T}).m; _ = (interface {T}).m ))",
    R"(package p; func ((T),) m() {})",
    R"(package p; func ((*T),) m() {})",
    R"(package p; func (*(T),) m() {})",
    R"(package p; func _(x []int) { for range x {} })",
    R"(package p; func _() { if [T{}.n]int{} {} })",
    R"(package p; func _() { map[int]int{}[0]++; map[int]int{}[0] += 1 })",
    R"(package p; func _(x interface{f()}) { interface{f()}(x).f() })",
    R"(package p; func _(x chan int) { chan int(x) <- 0 })",
    R"(package p; const (x = 0; y; z))", // go.dev/issue/9639
    R"(package p; var _ = map[P]int{P{}:0, {}:1})",
    R"(package p; var _ = map[*P]int{&P{}:0, {}:1})",
    R"(package p; type T = int)",
    R"(package p; type (T = p.T; _ = struct{}; x = *T))",
    R"(package p; type T (*int))",
    R"(package p; type _ struct{ int })",
    R"(package p; type _ struct{ pkg.T })",
    R"(package p; type _ struct{ *pkg.T })",
    R"(package p; var _ = func()T(nil))",
    R"(package p; func _(T (P)))",
    R"(package p; func _(T []E))",
    R"(package p; func _(T [P]E))",
    R"(package p; type _ [A+B]struct{})",
    R"(package p; func (R) _())",
    R"(package p; type _ struct{ f [n]E })",
    R"(package p; type _ struct{ f [a+b+c+d]E })",
    R"(package p; type I1 interface{}; type I2 interface{ I1 })",

    // generic code
    R"(package p; type _ []T[int])",
    R"(package p; type T[P any] struct { P })",
    R"(package p; type T[P comparable] struct { P })",
    R"(package p; type T[P comparable[P]] struct { P })",
    R"(package p; type T[P1, P2 any] struct { P1; f []P2 })",
    R"(package p; func _[T any]()())",
    R"(package p; func _(T (P)))",
    R"(package p; func f[A, B any](); func _() { _ = f[int, int] })",
    R"(package p; func _(x T[P1, P2, P3]))",
    R"(package p; func _(x p.T[Q]))",
    R"(package p; func _(p.T[Q]))",
    R"(package p; type _[A interface{},] struct{})",
    R"(package p; type _[A interface{}] struct{})",
    R"(package p; type _[A,  B any,] struct{})",
    R"(package p; type _[A, B any] struct{})",
    R"(package p; type _[A any,] struct{})",
    R"(package p; type _[A any]struct{})",
    R"(package p; type _[A any] struct{ A })",
    R"(package p; func _[T any]())",
    R"(package p; func _[T any](x T))",
    R"(package p; func _[T1, T2 any](x T))",
    R"(package p; func _[A, B any](a A) B)",
    R"(package p; func _[A, B C](a A) B)",
    R"(package p; func _[A, B C[A, B]](a A) B)",

    R"(package p; type _[A, B any] interface { _(a A) B })",
    R"(package p; type _[A, B C[A, B]] interface { _(a A) B })",
    R"(package p; func _[T1, T2 interface{}](x T1) T2)",
    R"(package p; func _[T1 interface{ m() }, T2, T3 interface{}](x T1, y T3) T2)",
    R"(package p; var _ = []T[int]{})",
    R"(package p; var _ = [10]T[int]{})",
    R"(package p; var _ = func()T[int]{})",
    R"(package p; var _ = map[T[int]]T[int]{})",
    R"(package p; var _ = chan T[int](x))",
    R"(package p; func _(_ T[P], T P) T[P])",
    R"(package p; var _ T[chan int])",

    R"(package p; func (_ R[P]) _(x T))",
    R"(package p; func (_ R[ P, Q]) _(x T))",

    R"(package p; func (R[P]) _())",
    R"(package p; func _(T[P]))",
    R"(package p; func _(T[P1, P2, P3 ]))",
    R"(package p; func _(T[P]) T[P])",
    R"(package p; type _ struct{ T[P]})",
    R"(package p; type _ struct{ T[struct{a, b, c int}] })",
    R"(package p; type _ interface{int|float32; bool; m(); string;})",
    R"(package p; type I1[T any] interface{}; type I2 interface{ I1[int] })",
    R"(package p; type I1[T any] interface{}; type I2[T any] interface{ I1[T] })",
    R"(package p; type _ interface { N[T] })",
    R"(package p; type T[P any] = T0)",

    // from the generics testdata (linalg.go2, map.go2, set.go2, slices.go2,
    // sort.go2, typeset.go2, tparams.go2)
    R"(package p; type Number interface { ~int | ~int8 | ~float32 | ~float64 })",
    R"(package p; type Ordered interface { ~int | ~int8 | ~uint | ~string })",
    R"(package p; func Sum[T Number](s []T) (r T) { for _, v := range s { r += v }; return })",
    R"(package p; func Map[F, T any](s []F, f func(F) T) []T { r := make([]T, len(s)); for i, v := range s { r[i] = f(v) }; return r })",
    R"(package p; type Set[Elem comparable] map[Elem]struct{})",
    R"(package p; func (s Set[Elem]) Add(v Elem) { s[v] = struct{}{} })",
    R"(package p; type Map[K comparable, V any] struct { root *node[K, V]; compare func(K, K) int })",
    R"(package p; func Equal[Elem comparable](s1, s2 []Elem) bool { return true })",
    R"(package p; type T[P *C] struct{})",
    R"(package p; type T[P *C,] struct{})",
    R"(package p; type T[P (C)] struct{})",
    R"(package p; type T[P *C|D] struct{})",
    R"(package p; type T[P ~int|~string] struct{})",
    R"(package p; type _ [P * C]int)",
    R"(package p; type _ [P * C,]int)",
    R"(package p; type T[P any, Q interface{ m() P }] struct{})",
    R"(package p; func f[P interface{ ~[]E }, E any](x P) {})",
    R"(package p; type T[_ any] struct{}; var _ T[int])",
    R"(package p; type T[P []E, E any] struct{})",
    R"(package p; type T[P [10]E, E any] struct{})",
    R"(package p; var _ = f[[]int])",
    R"(package p; var _ = f[map[string]int](x))",
    R"(package p; var _ = (*T[int])(nil))",
    R"(package p; var _ = (func())(nil))",
    R"(package p; func f() { var _ = x.(T[int]) })",
    R"(package p; func f() { switch x := y.(type) { case T[int], *U[string]: } })",
};

struct Invalid {
  const char *Src;
  bool PositionAgrees; // the first error is at Go's marker position
};

// clang-format off
const Invalid Invalids[] = {
    {R"(foo /* ERROR "expected 'package'" */ !)", true},
    {R"(package p; func f() { if { /* ERROR "missing condition" */ } };)", true},
    {R"(package p; func f() { if ; /* ERROR "missing condition" */ {} };)", true},
    {R"(package p; func f() { if f(); /* ERROR "missing condition" */ {} };)", true},
    {R"(package p; func f() { if _ = range /* ERROR "expected operand" */ x; true {} };)", true},
    {R"(package p; func f() { switch _ /* ERROR "expected switch expression" */ = range x; true {} };)", false},
    {R"(package p; func f() { for _ = range x ; /* ERROR "expected '{'" */ ; {} };)", true},
    {R"(package p; func f() { for ; ; _ = range /* ERROR "expected operand" */ x {} };)", true},
    {R"(package p; func f() { for ; _ /* ERROR "expected boolean or range expression" */ = range x ; {} };)", false},
    {R"(package p; func f() { switch t = /* ERROR "expected ':=', found '='" */ t.(type) {} };)", true},
    {R"(package p; func f() { switch t /* ERROR "expected switch expression" */ , t = t.(type) {} };)", true},
    {R"(package p; func f() { switch t /* ERROR "expected switch expression" */ = t.(type), t {} };)", true},
    {R"(package p; func f() { _ = (<-<- /* ERROR "expected 'chan'" */ chan int)(nil) };)", true},
    {R"(package p; func f() { _ = (<-chan<-chan<-chan<-chan<-chan<- /* ERROR "expected channel type" */ int)(nil) };)", true},
    {R"(package p; func f() { if x := g(); x /* ERROR "expected boolean expression" */ = 0 {}};)", true},
    {R"(package p; func f() { _ = x = /* ERROR "expected '=='" */ 0 {}};)", true},
    {R"(package p; func f() { _ = 1 == func()int { var x bool; x = x = /* ERROR "expected '=='" */ true; return x }() };)", true},
    {R"(package p; func f() { var s []int; _ = s[] /* ERROR "expected operand" */ };)", true},
    {R"(package p; func f() { var s []int; _ = s[i:j: /* ERROR "final index required" */ ] };)", true},
    {R"(package p; func f() { var s []int; _ = s[i: /* ERROR "middle index required" */ :k] };)", true},
    {R"(package p; func f() { var s []int; _ = s[i: /* ERROR "middle index required" */ :] };)", true},
    {R"(package p; func f() { var s []int; _ = s[: /* ERROR "middle index required" */ :] };)", true},
    {R"(package p; func f() { var s []int; _ = s[: /* ERROR "middle index required" */ ::] };)", false},
    {R"(package p; func f() { var s []int; _ = s[i:j:k: /* ERROR "expected ']'" */ l] };)", true},
    {R"(package p; func f() { for x /* ERROR "boolean or range expression" */ = []string {} })", false},
    {R"(package p; func f() { for x /* ERROR "boolean or range expression" */ := []string {} })", false},
    {R"(package p; func f() { for i /* ERROR "boolean or range expression" */ , x = []string {} })", false},
    {R"(package p; func f() { for i /* ERROR "boolean or range expression" */ , x := []string {} })", false},
    {R"(package p; func f() { go f /* ERROR HERE "must be function call" */ })", true},
    {R"(package p; func f() { go ( /* ERROR "must not be parenthesized" */ f()) })", true},
    {R"(package p; func f() { defer func() {} /* ERROR HERE "must be function call" */ })", true},
    {R"(package p; func f() { defer ( /* ERROR "must not be parenthesized" */ f()) })", true},
    {R"(package p; func f() { go func() { func() { f(x func /* ERROR "missing ','" */ (){}) } } })", true},
    {R"(package p; func _() (type /* ERROR "found 'type'" */ T)(T))", true},
    {R"(package p; func (type /* ERROR "found 'type'" */ T)(T) _())", true},
    {R"(package p; type _[A+B, /* ERROR "unexpected comma" */ ] int)", true},

    {R"(package p; type _ struct{ [ /* ERROR "expected '}', found '\['" */ ]byte })", true},
    {R"(package p; type _ struct{ ( /* ERROR "cannot parenthesize embedded type" */ int) })", true},
    {R"(package p; type _ struct{ ( /* ERROR "cannot parenthesize embedded type" */ []byte) })", true},
    {R"(package p; type _ struct{ *( /* ERROR "cannot parenthesize embedded type" */ int) })", true},
    {R"(package p; type _ struct{ *( /* ERROR "cannot parenthesize embedded type" */ []byte) })", true},

    // go.dev/issue/8656
    {R"(package p; func f() (a b string /* ERROR "missing ','" */ , ok bool))", true},

    // go.dev/issue/9639
    {R"(package p; var x, y, z; /* ERROR "expected type" */)", true},

    // go.dev/issue/12437
    {R"(package p; var _ = struct { x int, /* ERROR "expected ';', found ','" */ }{};)", true},
    {R"(package p; var _ = struct { x int, /* ERROR "expected ';', found ','" */ y float }{};)", true},

    // go.dev/issue/11611
    {R"(package p; type _ struct { int, } /* ERROR "expected 'IDENT', found '}'" */ ;)", true},
    {R"(package p; type _ struct { int, float } /* ERROR "expected type, found '}'" */ ;)", true},

    // go.dev/issue/13475
    {R"(package p; func f() { if true {} else ; /* ERROR "expected if statement or block" */ })", true},
    {R"(package p; func f() { if true {} else defer /* ERROR "expected if statement or block" */ f() })", true},

    // variadic parameter lists
    {R"(package p; func f(a, b ... /* ERROR "can only use ... with final parameter" */ int))", true},
    {R"(package p; func f(a ... /* ERROR "can only use ... with final parameter" */ int, b int))", true},
    {R"(package p; func f(... /* ERROR "can only use ... with final parameter" */ int, int))", true},
    {R"(package p; func f() (... /* ERROR "invalid use of ..." */ int))", true},
    {R"(package p; func f() (a, b ... /* ERROR "invalid use of ..." */ int))", true},
    {R"(package p; func f[T ... /* ERROR "invalid use of ..." */ C]()() {})", true},

    // generic code
    {R"(package p; type _[_ any] int; var _ = T[] /* ERROR "expected operand" */ {})", true},
    {R"(package p; var _ func[ /* ERROR "must have no type parameters" */ T any](T))", true},
    {R"(package p; func _[]/* ERROR "empty type parameter list" */())", true},

    {R"(package p; type _[A,] /* ERROR "missing type constraint" */ struct{ A })", true},

    {R"(package p; func _[type /* ERROR "found 'type'" */ P, *Q interface{}]())", true},

    {R"(package p; func (T) _[ /* ERROR "must have no type parameters" */ A, B any](a A) B)", true},
    {R"(package p; func (T) _[ /* ERROR "must have no type parameters" */ A, B C](a A) B)", true},
    {R"(package p; func (T) _[ /* ERROR "must have no type parameters" */ A, B C[A, B]](a A) B)", true},

    // go.dev/issue/70957
    {R"(package p; func f() {goto; /* ERROR "expected 'IDENT', found ';'" */ })", true},
    {R"(package p; func f() {goto} /* ERROR "expected 'IDENT', found '}'" */ })", true},
};
// clang-format on

TEST(GoParserPort, Valid) {
  for (const char *Src : Valids) {
    SCOPED_TRACE(Src);
    ParseOptions Go; // valid Go: `&T{}` is only wrong in Go++
    Go.CheckLanguageRules = false;
    Parsed P(Src, Go);
    EXPECT_EQ(P.numErrors(), 0u) << ::testing::PrintToString(P.errors());
  }
}

/// Where Go expects the error: at the token just before the marker comment, or
/// right after it for "ERROR HERE".
unsigned markerOffset(llvm::StringRef Src) {
  size_t M = Src.find("/* ERROR");
  EXPECT_NE(M, llvm::StringRef::npos);
  bool Here = Src.substr(M).starts_with("/* ERROR HERE");
  TestSource S(Src);
  unsigned Result = 0;
  for (const LexedToken &T : lexAll(S)) {
    if (T.Offset + T.Text.size() > M || T.Tok.is(tok::eof))
      break;
    Result = Here ? T.Offset + T.Text.size() : T.Offset;
  }
  return Result;
}

TEST(GoParserPort, Invalid) {
  for (const Invalid &I : Invalids) {
    SCOPED_TRACE(I.Src);
    ParseOptions Go; // Go rejects type parameters on methods (Go++ GEN-6)
    Go.AllowGenericMethods = false;
    Go.AllowNamespaceTypes = false;
    Parsed P(I.Src, Go);
    std::vector<std::string> Errs = P.errors();
    ASSERT_FALSE(Errs.empty()) << "no error reported";
    if (I.PositionAgrees) {
      bool Found = false;
      for (const std::string &E : Errs)
        Found |= std::stoul(E.substr(0, E.find(':'))) == markerOffset(I.Src);
      EXPECT_TRUE(Found) << "no error at the position Go reports ("
                         << markerOffset(I.Src)
                         << "): " << ::testing::PrintToString(Errs);
    }
  }
}

} // namespace
