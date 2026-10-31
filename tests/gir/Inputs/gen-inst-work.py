#!/usr/bin/env python3
"""Prints a package whose instances hold much more code than its source:
gen-inst-work.py K N makes F0 with N statements and F1..FK, each calling the
one before twice with two bigger type arguments, so F0 has 2^(K-1) instances
(the first level's two type arguments are one). K = 16, N = 1000 is a 20 KB
file whose instances hold about 2^15 * 7000 nodes of code (review R4)."""
import sys

K, N = int(sys.argv[1]), int(sys.argv[2])
out = ["package main", "", "type P[A, B any] struct {", "\ta []A", "\tb []B",
       "}", "", "func F0[T any](x []T) int {", "\ts := 0"]
out += [f"\ts += len(x) + {i}" for i in range(N)]
out += ["\treturn s", "}"]
for k in range(1, K + 1):
    out.append(f"func F{k}[T any](x []T) int {{ return F{k-1}([]P[T, int]{{}})"
               f" + F{k-1}([]P[int, T]{{}}) }}")
out.append(f"func main() {{ println(F{K}([]int{{}})) }}")
print("\n".join(out))
