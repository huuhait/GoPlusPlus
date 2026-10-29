#!/usr/bin/env python3
"""Checks one program in every order of its top-level type declarations.

  check-decl-orders.py <gopp> <file.go> <outdir> <expected>...

Each `type ...` line of the file is a declaration; every permutation is
written to <outdir> and checked with `gopp check --go`. The sorted list of
"invalid recursive type" errors (with the type named) must be exactly the
expected list in every order (go build reports the same in each order).
"""
import itertools, os, re, subprocess, sys

gopp, src, out = sys.argv[1], sys.argv[2], sys.argv[3]
want = sorted(sys.argv[4:])
lines = open(src).read().split("\n")
types = [l for l in lines if l.startswith("type ")]
head = [l for l in lines if l.startswith("package ")]
os.makedirs(out, exist_ok=True)
bad = 0
for i, perm in enumerate(itertools.permutations(types)):
    f = os.path.join(out, "p%d.go" % i)
    with open(f, "w") as fh:
        fh.write("\n".join(head + list(perm)) + "\n")
    r = subprocess.run([gopp, "check", "--go", "-check-j=1", f],
                       capture_output=True, text=True, timeout=20)
    got = sorted(re.findall(r"invalid recursive type:? ?\w+", r.stderr))
    if got != want:
        bad += 1
        print("order %d: got %s, want %s" % (i, got, want))
print("%d orders, %d wrong" % (i + 1, bad))
sys.exit(1 if bad else 0)
