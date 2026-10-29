#!/usr/bin/env python3
"""Generates the view-analysis stress cases of P03.21: <case> <n>.

  wide  n struct types and n functions taking a View of one: reads (copy-out,
        views of parts, a pointer target, range, len) and two writes each
        (a field through the view, an element through a range variable)
  deep  n nested struct types; one read and one write n fields deep through
        a View: one error
  box   n type switches over `any`: per case a read, a direct write into the
        box (E0354) and a write behind a pointer the box holds (E0368)
  generic  a chain of n generic structs G_i[T] { x G_{i+1}[T] } ending in
        G_n[T] { v T }, and n structs S_i { g G_i[View[string]] } (each an
        OWN-V5 error: E0367) and n structs K_i { g G_i[int] } (none)
  all   wide n, deep n/10 and box n in one file
"""
import sys

case, n = sys.argv[1], int(sys.argv[2])
out = ["package main", "", "type Inner struct{ N int }"]
if case in ("wide", "all"):
    for i in range(n):
        out += [
            "type S%d struct {" % i,
            "\tName  string",
            "\tTags  []string",
            "\tIn    Inner",
            "\tOwner *Inner",
            "\tCount int",
            "\tItems []map[string]int",
            "}",
            "func f%d(v View[S%d]) int {" % (i, i),
            "\tn := v.Count + v.In.N + v.Owner.N + len(v.Name) + len(v.Tags)",
            "\tfor _, it := range v.Items {",
            "\t\tn += len(it)",
            "\t\tit[\"k\"] = 1",
            "\t}",
            "\tv.Count = n",
            "\treturn n",
            "}",
        ]
if case == "generic":
    out += ["type G%d[T any] struct{ x G%d[T] }" % (i, i + 1) for i in range(n)]
    out += ["type G%d[T any] struct{ v T }" % n]
    out += ["type S%d struct{ g G%d[View[string]] }" % (i, i) for i in range(n)]
    out += ["type K%d struct{ g G%d[int] }" % (i, i) for i in range(n)]
if case in ("deep", "all"):
    d = n if case == "deep" else n // 10
    out += ["type D%d struct{ n D%d; m map[string]int }" % (i, i + 1) for i in range(d)]
    out += ["type D%d struct{ x int }" % d]
    path = ".".join(["v"] + ["n"] * d)
    out += ["func deep(v View[D0]) int {", "\t%s.x = 1" % path, "\treturn %s.x" % path, "}"]
if case in ("box", "all"):
    out += ["type Doc struct {", "\tTags []int", "\tIn   *Inner", "}"]
    for i in range(n):
        out += [
            "func b%d(x any) int {" % i,
            "\tswitch d := x.(type) {",
            "\tcase Doc:",
            "\t\td.Tags[0] = 1",
            "\t\td.In.N = 1",
            "\t\treturn len(d.Tags) + d.In.N",
            "\t}",
            "\treturn 0",
            "}",
        ]
out.append("func main() {}")
print("\n".join(out))
