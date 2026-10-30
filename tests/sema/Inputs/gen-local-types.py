#!/usr/bin/env python3
"""usage: gen-local-types.py <n> [generic]: one function with n local types
(P03.26, R3). With `generic`, the function has a type parameter and every
type uses it."""
import sys

n = int(sys.argv[1])
g = len(sys.argv) > 2
print("package main")
print("func f%s() {" % ("[T any]" if g else ""))
for i in range(n):
    print("\ttype L%d struct{ x %s }" % (i, "T" if g else "int"))
print("}")
print("func main() {")
if g:
    print("\tf[int]()")
print("}")
