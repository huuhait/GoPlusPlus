#!/usr/bin/env python3
"""Generates the declaration stress cases of P03.23: <case> <n> [levels].

  ring    n named types, each holding the next under `levels` slices (a ring)
  fwd     F0 { x F1 } ... Fn { x int }: a forward chain, a copy of F0
  bwd     N0 { x int }, N1 { x N0 } ...: a backward chain, a copy and a
          Clone() of the last
  byval   a ring held by value: invalid recursive type, one error
  direct  `type L0 L1` ... a ring of defined types: invalid recursive type
  gring   a ring of generic types held by value: one error
  gchain  G0[T] { x G1[T]; y []G1[T] } ... Gn[T] { v T }: a forward chain of
          generic types, a zero value and a Clone() of G0[int]
  dchain  F0 { a F1; b F1 } ...: a forward chain naming each type twice
  blocked I { i I } (one error), then a ring E0 { i I; e E1 } ... held by
          value behind I: Go's walk stops at I and reports nothing more, the
          ring must still be broken (a copy and a Clone() of E0 end)
  chains  fwd, bwd and dchain of n types and gchain of n/4 in one file
  valid   chains n, and a ring of 3n types under one slice, 100 under 40
          slices and 300 under 5, in one file
  invalid byval (n types), direct, gring and blocked (n/10 each) in one file:
          one error each
  rings   ring n (one slice), ring 100 (40 slices), ring 300 (5 slices)
"""
import sys

case, n = sys.argv[1], int(sys.argv[2])
levels = int(sys.argv[3]) if len(sys.argv) > 3 else 1
out = ["package main", ""]
if case == "ring":
    sl = "[]" * levels
    out += ["type L%d struct{ next %sL%d }" % (i, sl, (i + 1) % n) for i in range(n)]
elif case == "fwd":
    out += ["type F%d struct{ x F%d }" % (i, i + 1) for i in range(n)]
    out += ["type F%d struct{ x int }" % n, "func f(a F0) { b := a; _ = b }"]
elif case == "bwd":
    out += ["type N0 struct{ x int }"]
    out += ["type N%d struct{ x N%d }" % (i, i - 1) for i in range(1, n + 1)]
    out += ["func f(a N%d) { b := a; _ = b; c := a.Clone(); _ = c }" % n]
elif case == "byval":
    out += ["type L%d struct{ next L%d }" % (i, (i + 1) % n) for i in range(n)]
elif case == "direct":
    out += ["type L%d L%d" % (i, (i + 1) % n) for i in range(n)]
elif case == "gring":
    out += ["type G%d[T any] struct{ x G%d[T] }" % (i, (i + 1) % n) for i in range(n)]
elif case == "gchain":
    out += ["type G%d[T any] struct{ x G%d[T]; y []G%d[T] }" % (i, i + 1, i + 1)
            for i in range(n)]
    out += ["type G%d[T any] struct{ v T }" % n,
            "func f(a G0[int]) { var z G0[int]; _ = z; c := a.Clone(); _ = c }"]
elif case == "dchain":
    out += ["type F%d struct{ a F%d; b F%d }" % (i, i + 1, i + 1) for i in range(n)]
    out += ["type F%d struct{ x int }" % n, "func f(a F0) { b := a; _ = b }"]
elif case == "blocked":
    out += ["type I struct{ i I }"]
    out += ["type E%d struct {\n\ti I\n\te E%d\n}" % (i, (i + 1) % n)
            for i in range(n)]
    out += ["func f(a E0) { b := a; _ = b; c := a.Clone(); _ = c }"]
if case in ("chains", "valid"):
    out += ["type F%d struct{ x F%d }" % (i, i + 1) for i in range(n)]
    out += ["type F%d struct{ x int }" % n, "func f(a F0) { b := a; _ = b }"]
    out += ["type N0 struct{ x int }"]
    out += ["type N%d struct{ x N%d }" % (i, i - 1) for i in range(1, n + 1)]
    out += ["func g(a N%d) { b := a; _ = b; c := a.Clone(); _ = c }" % n]
    out += ["type D%d struct{ a D%d; b D%d }" % (i, i + 1, i + 1) for i in range(n)]
    out += ["type D%d struct{ x int }" % n, "func h(a D0) { b := a; _ = b }"]
    m = n // 4
    out += ["type G%d[T any] struct{ x G%d[T]; y []G%d[T] }" % (i, i + 1, i + 1)
            for i in range(m)]
    out += ["type G%d[T any] struct{ v T }" % m,
            "func k(a G0[int]) { var z G0[int]; _ = z; c := a.Clone(); _ = c }"]
if case == "invalid":
    m = n // 10
    out += ["type L%d struct{ next L%d }" % (i, (i + 1) % n) for i in range(n)]
    out += ["type M%d M%d" % (i, (i + 1) % m) for i in range(m)]
    out += ["type G%d[T any] struct{ x G%d[T] }" % (i, (i + 1) % m) for i in range(m)]
    out += ["type I struct{ i I }"]
    out += ["type E%d struct {\n\ti I\n\te E%d\n}" % (i, (i + 1) % m)
            for i in range(m)]
    out += ["func f(a E0) { b := a; _ = b; c := a.Clone(); _ = c }"]
if case in ("rings", "valid"):
    r = n if case == "rings" else 3 * n
    out += ["type L%d struct{ next []L%d }" % (i, (i + 1) % r) for i in range(r)]
    out += ["type M%d struct{ next %sM%d }" % (i, "[]" * 40, (i + 1) % 100)
            for i in range(100)]
    out += ["type R%d struct{ next %sR%d }" % (i, "[]" * 5, (i + 1) % 300)
            for i in range(300)]
out.append("func main() {}")
print("\n".join(out))
