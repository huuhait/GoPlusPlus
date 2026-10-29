#!/usr/bin/env python3
"""Generates the copy-out stress cases of the P03.21 review (R9, R11): <n>.

  A chain of n Copy structs S_i { a S_{i+1}; n int } ending in one that holds
  a []*int, read through a View once per level (n reads, each a view: the
  pointer is n-i levels down) and cloned once per level (n E0373 errors);
  a ring of n structs R_i { s []R_{i+1}; p Unique[*int] } (a cycle through
  slices, no raw pointer) cloned once per level (none); and a chain of n
  pointer-free Copy structs P_i { a P_{i+1}; n int } read once per level
  (plain copies).
"""
import sys

n = int(sys.argv[1])
out = ["package main", ""]
for i in range(n):
    nxt = "S%d" % (i + 1)
    if i + 1 == n:
        out.append("type S%d struct{ n int; p []*int }" % i)
    else:
        out.append("type S%d struct{ a %s; n int }" % (i, nxt))
    out.append("type R%d struct{ s []R%d; p Unique[*int] }" % (i, (i + 1) % n))
    if i + 1 == n:
        out.append("type P%d struct{ n int }" % i)
    else:
        out.append("type P%d struct{ a P%d; n int }" % (i, i + 1))
for i in range(n):
    if i + 1 < n:
        out.append("func f%d(v View[S%d]) int { x := v.a; return x.n }" % (i, i))
        out.append("func h%d(v View[P%d]) int { x := v.a; x.n++; return x.n }"
                   % (i, i))
    out.append("func g%d(v View[S%d]) { _ = v.Clone() }" % (i, i))
    out.append("func r%d(v View[R%d]) R%d { return v.Clone() }" % (i, i, i))
out.append("func main() {}")
print("\n".join(out))
