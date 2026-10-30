#!/usr/bin/env python3
"""usage: gen-abstract-instances.py <n> <ptr|value>: a generic type W with n
methods and n abstract types, each used in an instance W[S<i>] (P03.26, R11).
`ptr`: every method takes *T, so no instance holds its argument by value and
the program is accepted. `value`: one method takes T by value, so each of the
n instances is an error (E0389)."""
import sys

n, mode = int(sys.argv[1]), sys.argv[2]
print("package main")
print("type W[T any] struct{ n int }")
for i in range(n):
    print("func (w W[T]) m%d(p *T) int { return %d }" % (i, i))
if mode == "value":
    print("func (w W[T]) held(p []T) {}")
for i in range(n):
    print("type S%d struct{ N int }" % i)
    print("@Abstract\nfunc (s *S%d) Area() int" % i)
    print("func u%d(p *W[S%d]) {}" % (i, i))
    if mode == "value":
        print("func v%d(p W[S%d]) {}" % (i, i))
print("func main() {}")
