#!/usr/bin/env python3
"""usage: gen-inline-chain.py <n>: n functions, each calling the one before
(all inlined at opt 2: n*n/2 inlined frames, each with a parameter and a
variable)."""
import sys

n = int(sys.argv[1])
print("package main")
print("func f0(a int) int {\n\tb := a + 1\n\treturn b\n}")
for i in range(1, n):
    print("func f%d(a int) int {\n\tb := f%d(a)\n\treturn b\n}" % (i, i - 1))
print("func main() {\n\tprintln(f%d(0))\n}" % (n - 1))
