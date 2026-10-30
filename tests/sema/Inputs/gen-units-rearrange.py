#!/usr/bin/env python3
"""P03.25 (GEN-14, R10, owner decision 2026-10-30): chains of unit-generic
functions that pass their unit parameters on, rearranged at each level
(`F_i[U, V]` calls `F_{i-1}[U*V, U]` and `F_{i-1}[V, U]`, ...) or straight
(`F_{i-1}[U, V]`). The units the code reaches double at each level of a
rearranging chain, but nothing is followed per instance: each `a*b` is a
collapse site of its own function, and which unit parameters need their units
at run time passes on through the use fixpoint, a boolean per parameter. So
every chain is accepted (the units that cancel are computed at run time as in
plain code) with the expected `unit-dict` and `unit-site` records, the same
dump with 1 and 8 body threads, in time linear in its length. Two more shapes
that a search over instances could not finish (a chain that encodes subset
sum; a chain that squares its units past the exponent range) are accepted at
once.

Usage: gen-units-rearrange.py --gopp <gopp> --out <dir> [--sizes 10,40]
       [--shapes two,three] [--leaves cancel] [--j1]   (a subset; --j1: no
                       second run with 8 body threads)
       [--others]      (the subset-sum and squaring chains)
       [--linear 50]   (each shape: the time of 4x the size is at most 12x)
"""
import argparse
import os
import subprocess
import sys
import time

HEAD = "package main\n\nunit M\nunit KM = 1000 M\nunit S\n\n"
SIG = "[U unit, V unit](a float64[U], b float64[V])"
CALLS = {
    "two": ["F%d[U*V, U](a*b, a)", "F%d[V, U](b, a)"],
    "twogrow": ["F%d[U*V, U](a*b, a)", "F%d[V, U*V](b, a*b)"],
    "three": ["F%d[U*V, U](a*b, a)", "F%d[V, U](b, a)", "F%d[U, U*V](a, a*b)"],
    "straight": ["F%d(a, b)"],
}
J8 = True


def chain(shape, n, leaf, x, y):
    s = HEAD + "func main() { F%d(1.0[%s], 1.0[%s]) }\n" % (n, x, y)
    for i in range(n, 0, -1):
        s += "func F%d%s { %s }\n" % (i, SIG, "; ".join(c % (i - 1) for c in CALLS[shape]))
    s += "func F0%s { %s }\n" % (SIG, "r := a / b; _ = r" if leaf == "cancel" else "")
    return s


def expect(shape, n, leaf):
    """(generics that need the units, collapse sites): every `a*b` is a site
    of its function; the leaf `a / b` is one more, and with it the straight
    chain needs the units at every level."""
    per = sum(c.count("a*b") for c in CALLS[shape])
    sites = n * per + (leaf == "cancel")
    dicts = (n if per else 0) + (leaf == "cancel")
    if not per and leaf == "cancel":
        dicts = n + 1
    return dicts, sites


def subset(n):
    s = "package main\n\nunit X\nunit Y\nunit Z\nunit YB\nunit KY = 1000 YB\n"
    for i in range(n):
        s += "unit W%d = %s^%d\n" % (i, "XYZ"[i % 3], 2 << ((n - 1 - i) // 3))
    s += "\nfunc main() { F%d[1/(YB*X)](1.0) }\n" % n
    for i in range(n, 0, -1):
        s += ("func F%d[U unit](v float64) { F%d[U*W%d](v); F%d[U/W%d](v) }\n"
              % (i, i - 1, i - 1, i - 1, i - 1))
    s += ("func F0[U unit](v float64) { x := float64[U](v) * 1.0[KY]; "
          "_ = float64(x) }\n")
    return s


def squares(n, y):
    s = HEAD + "func main() { F%d(1.0[KM], 1.0[%s]) }\n" % (n, y)
    for i in range(n, 0, -1):
        s += "func F%d%s { F%d[U*U, V*V](a*a, b*b) }\n" % (i, SIG, i - 1)
    s += "func F0%s { r := a / b; _ = r }\n" % SIG
    return s


def run(gopp, path, j):
    s = time.time()
    r = subprocess.run([gopp, "check", "-check-j=" + j, "--check-error-limit=0",
                        "--dump-types", path], capture_output=True, text=True,
                       timeout=60)
    if r.returncode not in (0, 1):
        print("crash: " + r.stderr[:300])
        sys.exit(1)
    return r.stderr + r.stdout, time.time() - s


def report(name, gopp, path, dicts, sites):
    a, _ = run(gopp, path, "1")
    b = run(gopp, path, "8")[0] if J8 else a
    errs = a.count("error[")
    nd, ns = a.count(" unit-dict "), a.count(" unit-site ")
    ok = a == b and errs == 0 and nd == dicts and ns == sites
    print("%-28s errors=%d dicts=%d sites=%d j1==j8:%s %s" %
          (name, errs, nd, ns, ("yes" if a == b else "NO") if J8 else "-",
           "ok" if ok else "FAIL"))
    return not ok


def best(gopp, path, j):
    return min(run(gopp, path, j)[1] for _ in range(2))


def main():
    global J8
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--sizes", default="")
    ap.add_argument("--others", action="store_true")
    ap.add_argument("--shapes", default=",".join(CALLS))
    ap.add_argument("--leaves", default="plain,cancel")
    ap.add_argument("--j1", action="store_true")
    ap.add_argument("--linear", type=int, default=0)
    a = ap.parse_args()
    J8 = not a.j1
    os.makedirs(a.out, exist_ok=True)
    bad = False
    for n in [int(x) for x in a.sizes.split(",") if x]:
        for shape in a.shapes.split(","):
            for leaf in a.leaves.split(","):
                p = os.path.join(a.out, "%s-%s-%d.gopp" % (shape, leaf, n))
                open(p, "w").write(chain(shape, n, leaf, "KM", "M"))
                bad |= report("%s %s n=%d" % (shape, leaf, n), a.gopp, p,
                              *expect(shape, n, leaf))
    if a.others:
        p = os.path.join(a.out, "subset.gopp")
        open(p, "w").write(subset(24))
        bad |= report("subset n=24", a.gopp, p, 25, 2)
        for y in ("M", "S"):
            p = os.path.join(a.out, "squares-%s.gopp" % y)
            open(p, "w").write(squares(22, y))
            bad |= report("squares KM,%s n=22" % y, a.gopp, p, 23, 1)
    if a.linear:
        for shape in a.shapes.split(","):
            for j in ("1", "8"):
                t = []
                for n in (a.linear, 4 * a.linear):
                    p = os.path.join(a.out, "lin-%s-%d.gopp" % (shape, n))
                    open(p, "w").write(chain(shape, n, "cancel", "KM", "M"))
                    t.append(best(a.gopp, p, j))
                ok = t[1] <= 12 * max(t[0], 0.05)
                bad |= not ok
                print("%s j%s: near-linear %s" % (shape, j, "yes" if ok else
                      "NO (%.2fs -> %.2fs)" % (t[0], t[1])))
    return 1 if bad else 0


sys.exit(main())
