#!/usr/bin/env python3
"""usage: gen-import-chain.py <dir> <depth>: a main package that imports a
chain of <depth> declaration-only packages (<dir>/stubs/p0 ... p<depth-1>,
each importing the one before) and a function body each. The main file is
<dir>/main/m.gopp."""
import os
import sys

d, n = sys.argv[1], int(sys.argv[2])
for i in range(n):
    os.makedirs("%s/stubs/p%d" % (d, i), exist_ok=True)
    imp = 'import _ "p%d"\n' % (i - 1) if i else ""
    open("%s/stubs/p%d/a.gopp" % (d, i), "w").write(
        "package p%d\n%sfunc F() int { a := %d; return a }\n" % (i, imp, i))
os.makedirs(d + "/main", exist_ok=True)
open(d + "/main/m.gopp", "w").write(
    'package main\nimport _ "p%d"\nfunc main() {}\n' % (n - 1))
