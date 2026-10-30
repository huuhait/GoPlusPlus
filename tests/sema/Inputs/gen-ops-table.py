#!/usr/bin/env python3
"""P03.24 (GEN-13): generic code and plain code give the same answer for
operators. For every pair (use x type) this script writes the plain form (the
use written for the type itself) and the generic form (the same use for a
type parameter X whose constraint lists the operator, instantiated with the
type), checks both programs and requires: the generic form is accepted
exactly when the plain form is accepted. A built-in operator satisfies the
operator element of the same form (`int` satisfies `+ (X) X` and
`<=> (X) int`; `bool` satisfies neither), and a type with the operator method
satisfies it by that method.

Prints the table: one row per use, one column per type, `.` both accepted,
`x` both rejected. Exits non-zero on a mismatch.

Usage: gen-ops-table.py --gopp <gopp> --out <dir>
"""
import argparse
import os
import re
import subprocess
import sys

DECLS = """type Vec struct{ X float64 }

func (a Vec) + (b Vec) Vec     { return a }
func (a Vec) - (b Vec) Vec     { return a }
func (a Vec) * (b Vec) Vec     { return a }
func (a Vec) % (b Vec) Vec     { return a }
func (a Vec) - () Vec          { return a }
func (a Vec) ! () bool         { return true }
func (a Vec) == (b Vec) bool   { return true }
func (a Vec) <=> (b Vec) int   { return 0 }
func (a Vec) [] (i int) float64 { return a.X }

type Plain struct{ X float64 }

type Acc struct{ X float64 }

func (a *Acc) += (b Acc) {}
func (a *Acc) ++ ()      {}
type Meters float64
type Num int
type Count int

func (a Count) * (b Meters) Meters { return b }

type Flag bool
type Names []string
type Table map[string]int
"""

# A user type with the methods, a struct without, built-in and named basic
# types (Count has a `*` for another right-hand type), and types whose
# operators are all built in: a slice, a map, a pointer.
TYPES = ["Vec", "Plain", "int", "float64", "string", "bool", "Meters", "Num",
         "uint8", "Count", "Flag", "Names", "Table", "*Vec", "[]int",
         "[]float64", "Acc"]

# Where the plain form is accepted and the generic one is not, by design
# (`p` in the table): an in-place method has a pointer receiver, and a value
# type meets no element with one (GEN-13: "has pointer receiver", as `++ ()`);
# a type with only `+ (X) X` has `a += b` in plain code, but meets no `+=`
# element (the element is not listed); Vec has a `<=>` method but is in no
# type set of numbers and strings.
PLAIN_ONLY = {("cmpts", "Vec"), ("ltts", "Vec"), ("addi", "Vec"), ("addi", "Acc"), ("inc", "Acc")}

TSET = ("~int | ~int8 | ~int16 | ~int32 | ~int64 | ~uint | ~uint8 | ~uint16 | "
        "~uint32 | ~uint64 | ~float32 | ~float64 | ~string")

# name, constraint element, statement (a and b are parameters of the type, p
# is a pointer to one: an in-place use needs a place)
USES = [
    ("add", "+ (X) X", "_ = a + b"),
    ("sub", "- (X) X", "_ = a - b"),
    ("mul", "* (X) X", "_ = a * b"),
    ("rem", "% (X) X", "_ = a % b"),
    ("neg", "- () X", "_ = -a"),
    ("not", "! () bool", "_ = !a"),
    ("eq", "== (X) bool", "_ = a == b"),
    ("ne", "== (X) bool", "_ = a != b"),
    ("cmp", "<=> (X) int", "_ = a <=> b"),
    ("lt", "<=> (X) int", "_ = a < b"),
    ("ge", "<=> (X) int", "_ = a >= b"),
    ("idx", "[] (int) float64", "var _ float64 = a[0]"),
    # a type-set constraint gives the built-in operators (GEN-13)
    ("cmpts", TSET, "_ = a <=> b"),
    ("ltts", TSET, "_ = a < b"),
    ("addi", "+= (X)", "*p += b"),
    ("inc", "++ ()", "(*p)++"),
]


def plain_program():
    out = ["package main", "", DECLS]
    for u, (name, _, stmt) in enumerate(USES):
        for t, ty in enumerate(TYPES):
            out.append("func p_%s_%d(a, b %s, p *%s) { %s }" % (name, t, ty, ty, stmt))
    out.append("func main() {}")
    return "\n".join(out) + "\n"


def generic_program():
    # Each type is used twice: as an explicit type argument (i_) and as the
    # inferred one of a call (j_).
    out = ["package main", "", DECLS]
    for u, (name, elem, stmt) in enumerate(USES):
        out.append("type C_%s[X any] interface{ %s }" % (name, elem))
        out.append("func g_%s[X C_%s[X]](a, b X, p *X) { %s }" % (name, name, stmt))
        for t, ty in enumerate(TYPES):
            out.append("func i_%s_%d() { _ = g_%s[%s] }" % (name, t, name, ty))
            out.append("func j_%s_%d(a, b %s, p *%s) { g_%s(a, b, p) }" % (name, t, ty, ty, name))
    out.append("func main() {}")
    return "\n".join(out) + "\n"


def run(gopp, path, jobs):
    r = subprocess.run([gopp, "check", "--check-error-limit=0", "--check-j=%d" % jobs, path],
                       capture_output=True, text=True, timeout=60)
    return r.stdout + r.stderr


def lines_with_errors(text, src):
    bad = set()
    for m in re.finditer(r":(\d+):\d+: error\[", text):
        bad.add(int(m.group(1)))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    pp = os.path.join(a.out, "plain.gopp")
    gp = os.path.join(a.out, "generic.gopp")
    ps, gs = plain_program(), generic_program()
    open(pp, "w").write(ps)
    open(gp, "w").write(gs)
    pout, gout = run(a.gopp, pp, 1), run(a.gopp, gp, 1)
    if pout != run(a.gopp, pp, 8) or gout != run(a.gopp, gp, 8):
        print("MISMATCH: the output depends on the number of threads")
        return 1
    plines, glines = ps.split("\n"), gs.split("\n")
    pbad, gbad = lines_with_errors(pout, pp), lines_with_errors(gout, gp)
    # The generic body itself must be fine for its constraint.
    for u, (name, _, _) in enumerate(USES):
        for i, l in enumerate(glines, 1):
            if l.startswith("func g_%s[" % name) and i in gbad:
                print("MISMATCH: the generic body g_%s is rejected" % name)
                return 1
    print("types: " + " ".join("%d=%s" % (i, t) for i, t in enumerate(TYPES)))
    rc = 0
    for name, _, _ in USES:
        row = ""
        for t, ty in enumerate(TYPES):
            pl = next(i for i, l in enumerate(plines, 1) if l.startswith("func p_%s_%d(" % (name, t)))
            gl = next(i for i, l in enumerate(glines, 1) if l.startswith("func i_%s_%d(" % (name, t)))
            jl = next(i for i, l in enumerate(glines, 1) if l.startswith("func j_%s_%d(" % (name, t)))
            p_ok, g_ok, j_ok = pl not in pbad, gl not in gbad, jl not in gbad
            if p_ok and not g_ok and not j_ok and (name, ty) in PLAIN_ONLY:
                row += "p"
            elif p_ok != g_ok or g_ok != j_ok:
                row += "!"
                rc = 1
            else:
                row += "." if p_ok else "x"
        print("%-6s %s" % (name, row))
    if rc:
        print("MISMATCH: generic and plain code differ")
    return rc


sys.exit(main())
