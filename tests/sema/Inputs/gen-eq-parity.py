#!/usr/bin/env python3
"""P03.24 (EXP-17 rule 6, EXP-20, EXP-30, GEN-13): the rule table of `==`
when no `==` method serves it (Checker::eqKeepsBuiltin), pinned for plain and
generic code side by side.

Each row is a constraint and its plain twin: a type with the same built-in
comparability and the same `==` methods. Every use is written once with the
plain type and once in a generic body. A cell is `.` (accepted) or the error
code; when plain and generic differ it is `plain/generic`. A row whose
difference the spec intends is marked `intended`: only (c) against (d) in
`a == b`, `a != b` and `case b`, plain E0411 and generic accepted (see the
test); any other difference prints MISMATCH and exits non-zero.

The second table instantiates `[T interface{ == (T) bool }]` and
`[T comparable]` with each plain type: a type meets the element `== (T) bool`
with its built-in `==` exactly when the table keeps that built-in (GEN-13).

Usage: gen-eq-parity.py --gopp <gopp> --out <dir>
"""
import argparse
import os
import re
import subprocess
import sys

DECLS = """type PC struct{ x int }

type PA struct{ s []int }

type PE struct{ s []int }

func (a PE) == (b int) bool { return true }

type PCE struct{ x int }

func (a PCE) == (b int) bool { return true }

type G[T any] struct{ v T }

func (a G[T]) == (b int) bool { return true }

type PN []int

func (a PN) == (b int) bool { return true }

type PM float64

func (a PM) == (b string) bool { return true }

type PB bool

func (a PB) == (b int) bool { return true }

type PS struct{ x int }

func (a PS) == (b PS) bool { return true }

type PP struct{ x int }

func (a PP) == (b PP) bool { return true }

type CC extends PP { y int }

type GC extends CC { z int }

type PQ struct{ x int }

func (a PQ) == (b int) bool { return true }

type CQ extends PQ { y int }

type GK[T any] extends PP { v T }

type CN extends PP { s []int }

type GN extends CC { s []int }

func eqT[T interface{ == (T) bool }](a, b T) bool { return a == b }

func cmpT[T comparable](a, b T) bool { return a == b }
"""

# name, table row (plain/generic), constraint, plain type, intended difference
ROWS = [
    ("comparable", "a/a", "comparable", "PC", False),
    ("typeset", "a/a", "~int | ~int64", "int", False),
    ("any", "a/a", "any", "PA", False),
    ("element", "c/d", "interface{ == (int) bool }", "PE", False),
    ("both", "c/d", "interface{ comparable; == (int) bool }", "PCE", True),
    ("gstruct", "c/d", "interface{ comparable; == (int) bool }", "G[int]", True),
    ("noncomp", "c/d", "interface{ ~[]int; == (int) bool }", "PN", False),
    ("num", "b/d", "interface{ ~float64; == (string) bool }", "PM", False),
    ("bool", "c/d", "interface{ ~bool; == (int) bool }", "PB", True),
    ("self", "c/d", "interface{ comparable; == (T) bool }", "PS", False),
    ("child", "c/d", "interface{ comparable; extends PP }", "CC", True),
    ("grandchild", "c/d", "interface{ comparable; extends CC }", "GC", True),
    ("gchild", "c/d", "interface{ comparable; extends PP }", "GK[int]", True),
    ("ichild", "c/d", "interface{ comparable; extends PQ }", "CQ", True),
    ("ncchild", "c/d", "extends PP", "CN", False),
    ("ncgrand", "c/d", "extends CC", "GN", False),
]
# The uses where a row marked intended may differ, and how: plain code has
# no built-in `==` next to an `==` method (row (c)), a comparable type set
# keeps it (row (d)).
INTENDED = {"eq", "ne", "case"}
USES = [
    ("eq", "_ = a == b"),
    ("ne", "_ = a != b"),
    ("case", "switch a {\n\tcase b:\n\t}"),
    ("one", "_ = a == 1"),
    ("str", '_ = a == "x"'),
    ("lone", "_ = 1 == a"),
    ("iface", "_ = a == i"),
]
INSTANCES = ["eqT", "cmpT"]


def program(plain):
    out = ["package main", "", DECLS]
    for name, _, cons, pt, _ in ROWS:
        for u, stmt in USES:
            if plain:
                head = "func f_%s_%s(a, b %s, i any) {" % (name, u, pt)
            else:
                head = "func f_%s_%s[T %s](a, b T, i any) {" % (name, u, cons)
            out.append("%s\n\t%s\n}" % (head, stmt))
        if plain:
            for fn in INSTANCES:
                out.append("func f_%s_%s(a %s) { _ = %s(a, a) }" % (name, fn, pt, fn))
    out.append("func main() {}")
    return "\n".join(out) + "\n"


def codes(gopp, path, text):
    r = subprocess.run([gopp, "check", "--check-error-limit=0", path],
                       capture_output=True, text=True, timeout=60)
    # The f_<row>_<use> function each line belongs to.
    owner, cur = [], None
    for l in text.split("\n"):
        k = re.match(r"func f_(\w+?)_(\w+?)[\[(]", l)
        if k:
            cur = (k.group(1), k.group(2))
        elif not l.startswith(("\t", "}")):
            cur = None
        owner.append(cur)
    res = {}
    for m in re.finditer(r":(\d+):\d+: error\[(E\d+)\]", r.stdout + r.stderr):
        o = owner[int(m.group(1)) - 1]
        if o:
            res.setdefault(o, m.group(2))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    got = {}
    for plain in (True, False):
        p = os.path.join(a.out, "plain.gopp" if plain else "generic.gopp")
        t = program(plain)
        open(p, "w").write(t)
        got[plain] = codes(a.gopp, p, t)
    rc = 0
    print("uses: " + " | ".join(u for u, _ in USES))
    for name, row, _, _, intended in ROWS:
        cells = []
        mark = ""
        for u, _ in USES:
            pc = got[True].get((name, u), ".")
            gc = got[False].get((name, u), ".")
            if pc == gc:
                cells.append(pc)
                continue
            cells.append("%s/%s" % (pc, gc))
            if intended and u in INTENDED and (pc, gc) == ("E0411", "."):
                mark = mark or " intended"
            else:
                mark = " MISMATCH"
                rc = 1
        print("%-10s %s %s%s" % (name, row, " ".join(cells), mark))
    print("instances: " + " | ".join(INSTANCES))
    for name, _, _, pt, _ in ROWS:
        cells = [got[True].get((name, u), ".") for u in INSTANCES]
        print("%-10s %s" % (pt, " ".join(cells)))
    if rc:
        print("MISMATCH: generic and plain code differ")
    return rc


sys.exit(main())
