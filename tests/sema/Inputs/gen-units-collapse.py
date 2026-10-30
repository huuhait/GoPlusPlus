#!/usr/bin/env python3
"""P03.25 (GEN-14, UNI-5, R2/R7-R9, owner decision 2026-10-30): generic code
whose units cancel at an instance computes what plain code does, so no
instance is an error; the checker records the collapse sites and which unit
parameters need their units at run time (Use::UnitDict, passed on through
any number of generics, in any order, and across packages by the use
fixpoint, a boolean per parameter). Each shape is generated with units that
cancel with the factor 1000 (`KM`, `M`) and with units that agree (`M`, `M`):
no error, the expected `unit-dict` and `unit-site` records of `--dump-types`,
and the same dump with 1 and 8 body threads.

Usage: gen-units-collapse.py --gopp <gopp> --out <dir>
"""
import argparse
import os
import subprocess
import sys

HEAD = "package main\n\nunit M\nunit KM = 1000 M\n\n"
SIG = "[U unit, V unit](a float64[U], b float64[V])"


def chain(n, x, y):
    # callers first: each function calls the next, the leaf cancels U/V
    s = HEAD + "func main() {\n\t_ = F%d(1.0[%s], 1.0[%s])\n}\n" % (n, x, y)
    for i in range(n, 1, -1):
        s += "func F%d%s float64 { return F%d(a, b) }\n" % (i, SIG, i - 1)
    s += "func F1%s float64 { r := a / b; return float64(r) }\n" % SIG
    return s


def uses(n, x, y):
    # a leaf with n distinct unit types that mention U and V (no collapse
    # site) and one quotient that is, behind a caller
    s = HEAD + "func main() {\n\tOuter(1.0[%s], 1.0[%s])\n}\n" % (x, y)
    s += "func Outer%s { Leaf(a, b) }\n" % SIG
    s += "func Leaf%s {\n" % SIG
    k = 0
    for p in range(1, 1000):
        for q in range(1, 1000):
            if k >= n:
                break
            s += "\tvar x%d float64[U^%d * V^-%d]; _ = x%d\n" % (k, p, q, k)
            k += 1
        if k >= n:
            break
    s += "\tr := a / b; _ = r\n}\n"
    return s


def run(gopp, path, j):
    r = subprocess.run([gopp, "check", "-check-j=" + j, "--check-error-limit=0",
                        "--dump-types", path], capture_output=True, text=True,
                       timeout=60)
    return r.stderr + r.stdout


def report(name, gopp, path, dicts, sites):
    a, b = run(gopp, path, "1"), run(gopp, path, "8")
    errs = a.count("error[")
    nd, ns = a.count(" unit-dict "), a.count(" unit-site ")
    ok = a == b and errs == 0 and nd == dicts and ns == sites
    print("%-34s errors=%d dicts=%d sites=%d j1==j8:%s %s" %
          (name, errs, nd, ns, "yes" if a == b else "NO", "ok" if ok else "FAIL"))
    return not ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    bad = False
    # (name, generator, size, generics that need the units, collapse sites)
    cases = [("chain40", chain, 40, 40, 1), ("chain200", chain, 200, 200, 1),
             ("uses4500", uses, 4500, 2, 1)]
    for name, gen, n, dicts, sites in cases:
        for tag, x, y in (("cancel", "KM", "M"), ("agree", "M", "M")):
            path = os.path.join(a.out, "%s_%s.gopp" % (name, tag))
            open(path, "w").write(gen(n, x, y))
            bad |= report("%s %s" % (name, tag), a.gopp, path, dicts, sites)
    # a forwarder in a second package: main.Fwd -> h.Mid2 -> h.Mid -> g.Rt
    for tag, x, y in (("cancel", "KM", "M"), ("agree", "M", "M")):
        d = os.path.join(a.out, "pkg_" + tag)
        for sub in ("g", "h"):
            os.makedirs(os.path.join(d, sub), exist_ok=True)
        open(os.path.join(d, "g", "g.gopp"), "w").write(
            "package g\n\nunit M\nunit KM = 1000 M\n\n"
            "func Rt%s float64 { r := a / b; return float64(r) }\n" % SIG)
        open(os.path.join(d, "h", "h.gopp"), "w").write(
            'package h\n\nimport "g"\n\n'
            "func Mid%s float64 { return g.Rt[U, V](a, b) }\n"
            "func Mid2%s float64 { return Mid(a, b) }\n" % (SIG, SIG))
        open(os.path.join(d, "main.gopp"), "w").write(
            'package main\n\nimport ("g"; "h")\n\n'
            "func Fwd%s float64 { return h.Mid2(a, b) }\n\n"
            "func main() {\n"
            "\t_ = h.Mid2(1.0[g.%s], 1.0[g.%s])\n"
            "\t_ = Fwd(1.0[g.%s], 1.0[g.%s])\n}\n" % (SIG, x, y, x, y))
        bad |= report("forwarder-package %s" % tag, a.gopp, d, 1, 0)
    if bad:
        print("FAIL")
        sys.exit(1)


main()
