#!/usr/bin/env python3
"""usage: gen-many-bodies.py <n>: a package with n small function bodies."""
import sys

print("package main")
for i in range(int(sys.argv[1])):
    print("func f%d(a int) int { b := a + %d; return b }" % (i, i))
print("func main() {}")
