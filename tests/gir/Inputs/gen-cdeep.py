#!/usr/bin/env python3
"""A Go++ program whose @Extern("C") function takes a type at a limit of C
types (P04.11, gir::CTypeRules; tests/gir/extern-deep.test).

usage: gen-cdeep.py N [chain|diamond|wide|gib]

chain    L0 { x int32 }, L1 { x L0 }, ... LN: N + 1 levels of structs.
diamond  L0 { a, b int32 }, L1 { a, b L0 }, ... LN: each level holds the one
         below twice, 2^(N+2) - 1 fields when written out, N + 1 types.
wide     W, a struct of N int32 fields: N + 1 fields written out.
gib      G { a [N]int64 }: 8 * N bytes.
"""
import sys

n = int(sys.argv[1])
mode = sys.argv[2] if len(sys.argv) > 2 else "chain"
print("package main\n")
if mode == "diamond":
    print("type L0 struct{ a, b int32 }")
    for i in range(1, n + 1):
        print("type L%d struct{ a, b L%d }" % (i, i - 1))
    param = "L%d" % n
elif mode == "chain":
    print("type L0 struct{ x int32 }")
    for i in range(1, n + 1):
        print("type L%d struct{ x L%d }" % (i, i - 1))
    param = "L%d" % n
elif mode == "wide":
    print("type W struct {")
    for i in range(n):
        print("\tf%d int32" % i)
    print("}")
    param = "W"
elif mode == "gib":
    print("type G struct{ a [%d]int64 }" % n)
    param = "G"
else:
    sys.exit("unknown mode " + mode)
print('\n@Extern("C")\nfunc deep(d %s)\n\nfunc main() {}' % param)
