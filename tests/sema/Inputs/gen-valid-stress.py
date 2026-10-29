#!/usr/bin/env python3
"""Generates the stress cases of the valid recursive type check: <case> <n>.

  mixed   G_i[P, Q] { a G_{i+1}[P, int]; b G_{i+1}[int, Q] }: instances with a
          type parameter and a concrete type argument; 2^n walks without a
          cache keyed by the resolved arguments (22 s at n = 26)
  swap    G_i[P, Q] { a G_{i+1}[P, Q]; b G_{i+1}[Q, P] }
  named   G_i[P, Q] { a G_{i+1}[P, N]; b G_{i+1}[N, Q] } with a plain struct N
  wrap    G_i[P] { x G_{i+1}[[1]P] }: the argument grows with the depth
  nested  G_i[P] { a G_{i+1}[W[P]]; b G_{i+1}[W[W[P]]] } with a generic W
  bad     n copies of go.dev/issue/65711: `type A[P any] [1]P`,
          `type B[P any] A[P]`, `type C B[C]`: n errors
  every case ends with a function that copies, clones and zeroes a G_0.
"""
import sys

case, n = sys.argv[1], int(sys.argv[2])
out = ["package main", "", "func main() {}", ""]
use = "func use(a %s) { c := a.Clone(); _ = c; var z %s; _ = z }"
if case == "mixed":
    out += ["type G%d[P, Q any] struct{ a G%d[P, int]; b G%d[int, Q] }" % (i, i + 1, i + 1) for i in range(n)]
    out += ["type G%d[P, Q any] struct{ x P; y Q }" % n]
    out += [use % ("G0[int, string]", "G0[int, string]")]
elif case == "swap":
    out += ["type G%d[P, Q any] struct{ a G%d[P, Q]; b G%d[Q, P] }" % (i, i + 1, i + 1) for i in range(n)]
    out += ["type G%d[P, Q any] struct{ x P; y Q }" % n]
    out += [use % ("G0[int, string]", "G0[int, string]")]
elif case == "named":
    out += ["type N struct{ v int }"]
    out += ["type G%d[P, Q any] struct{ a G%d[P, N]; b G%d[N, Q] }" % (i, i + 1, i + 1) for i in range(n)]
    out += ["type G%d[P, Q any] struct{ x P; y Q }" % n]
    out += [use % ("G0[int, string]", "G0[int, string]")]
elif case == "wrap":
    out += ["type G%d[P any] struct{ x G%d[[1]P] }" % (i, i + 1) for i in range(n)]
    out += ["type G%d[P any] struct{ x P }" % n]
elif case == "nested":
    out += ["type W[P any] struct{ v P }"]
    out += ["type G%d[P any] struct{ a G%d[W[P]]; b G%d[W[W[P]]] }" % (i, i + 1, i + 1) for i in range(n)]
    out += ["type G%d[P any] struct{ x P }" % n]
elif case == "bad":
    for i in range(n):
        out += ["type A%d[P any] [1]P" % i, "type B%d[P any] A%d[P]" % (i, i),
                "type C%d B%d[C%d]" % (i, i, i)]
print("\n".join(out))
