#!/usr/bin/env python3
"""P03.25 (GEN-14, UNI-2, R15 to R20): the range of the unit types of the
instances of generic types, on random graphs of generic declarations: cycles
of 1 to k declarations and mutual recursion (through pointers), parameters
permuted or raised around a cycle, methods that return nested instances,
generic aliases, and concrete instances in main near the limits (an exponent
of 2^20, a factor of 4096 bits). Each program is checked by gopp and by a
brute-force expansion that makes every instance main reaches (fields and
method results) and decides each unit exactly; a cycle whose arguments grow
is the instantiation cycle error (E0366; a method's result counts, GEN-14,
but not an argument made with a generic method's own parameter W, while a
body that calls the method with a grown W does, R23).
The two must agree on accept and
reject, with 1 and 8 body threads (the same text). --timing: k-cycles of
k = 10, 100 and 1000 declarations and a chain of 8000 check in near-linear
time.

Usage: gen-units-typegraph.py --gopp <gopp> --out <dir> [--seeds N]
       [--first F] [--timing] [--j1]
"""
import argparse
import os
import random
import subprocess
import sys
import time
from fractions import Fraction

MAXEXP = 1 << 20
MAXBITS = 4096
BASES = {"M": ({"M": 1}, Fraction(1)), "KM": ({"M": 1}, Fraction(1000)),
         "S": ({"S": 1}, Fraction(1)), "H": ({"S": 1}, Fraction(3600))}
HEAD = "package main\n\nunit M\nunit KM = 1000 M\nunit S\nunit H = 3600 S\n\n"
P = ["U", "V"]


def mul(a, b, e=1):
    d = dict(a[0])
    for k, x in b[0].items():
        d[k] = d.get(k, 0) + e * x
        if d[k] == 0:
            del d[k]
    return (d, a[1] * b[1] ** e)


ONE = ({}, Fraction(1))


def power(u, e):
    r = ONE
    for _ in range(e):
        r = mul(r, u)
    return r


def ok(u):
    if any(abs(x) > MAXEXP for x in u[0].values()):
        return False
    f = u[1]
    return max(f.numerator.bit_length(), f.denominator.bit_length()) <= MAXBITS


def gen(rng, k):
    """k declarations: (nparams, fields [(param exps)], refs [(j, args,
    method?)]); an arg is a list of param exponents (U^a V^b)."""
    decls = []
    for i in range(k):
        np_ = rng.choice([1, 2])
        fields = []
        for _ in range(rng.choice([0, 1, 1, 2])):
            fields.append([rng.choice([1, 2, 3, 5]) if q == 0 or rng.random() < 0.5 else 0
                           for q in range(np_)])
        decls.append([np_, [f for f in fields if any(f)], []])
    for i in range(k):
        np_ = decls[i][0]
        for _ in range(rng.choice([1, 1, 2])):
            j = rng.randrange(k)
            args = []
            for q in range(decls[j][0]):
                r = rng.random()
                if r < 0.8:  # a parameter (permuted)
                    a = [0] * np_
                    a[rng.randrange(np_)] = 1
                elif r < 0.9:  # a power: grows around a cycle
                    a = [0] * np_
                    a[rng.randrange(np_)] = 2
                else:  # a product
                    a = [1] * np_
                args.append(a)
            meth = rng.random() < 0.3
            ownw = [0] * len(args)
            body = None
            if meth and rng.random() < 0.5:
                # a generic method: its own unit parameter W in an argument
                # (`Get[W unit]() Option[*T[U*W]]`), given at each call; its
                # body may call itself with W (no growth) or W*M (growth: an
                # instantiation cycle, R23)
                ownw[rng.randrange(len(args))] = rng.choice([1, -1])
                body = rng.choice([None, "same", "grow"])
            decls[i][2].append((j, args, meth, ownw, body))
    return decls


def unit_text(exps, w=0):
    t = ["%s^%d" % (P[q], e) if e != 1 else P[q] for q, e in enumerate(exps) if e]
    if w:
        t.append("W" if w == 1 else "W^%d" % w)
    return "*".join(t) if t else None


def arg_text(exps):  # in a type: `^` is allowed
    return unit_text(exps)


def program(decls, tops, alias):
    o = [HEAD]
    for i, (np_, fields, refs) in enumerate(decls):
        ps = ", ".join("%s unit" % P[q] for q in range(np_))
        fs = ["f%d float64[%s]" % (n, unit_text(f)) for n, f in enumerate(fields)]
        for n, (j, args, meth, ownw, body) in enumerate(refs):
            if not meth:
                fs.append("r%d Option[*T%d[%s]]" % (n, j, ", ".join(arg_text(a) for a in args)))
        o.append("type T%d[%s] struct{ %s }" % (i, ps, "; ".join(fs)))
        o.append("type A%d[%s] = T%d[%s]" % (i, ps, i, ", ".join(P[:np_])))
        for n, (j, args, meth, ownw, body) in enumerate(refs):
            if meth:
                inst = "T%d[%s]" % (j, ", ".join(unit_text(a, w)
                                                 for a, w in zip(args, ownw)))
                own = "[W unit]" if any(ownw) else ""
                call = ""
                if body:
                    call = "_ = x.Get%d[%s](); " % (n, "W" if body == "same" else "W*M")
                o.append("func (x *T%d[%s]) Get%d%s() Option[*%s] { %sreturn nil }" %
                         (i, ", ".join(P[:np_]), n, own, inst, call))
    o.append("")
    o.append("func main() {")
    for n, (i, units) in enumerate(tops):
        name = "A" if alias[n] else "T"
        o.append("\tvar x%d %s%d[%s]" % (n, name, i, ", ".join(units)))
        o.append("\t_ = x%d" % n)
    o.append("}")
    return "\n".join(o) + "\n"


def growing_cycle(decls):
    """The instantiation cycle (E0366): an argument other than a bare
    parameter on a cycle of the declarations (parameters as vertices). In a
    .gopp file the instances in method signatures count too (GEN-14, owner
    decision 2026-10-30), as those in fields do."""
    edges = {}
    for i, (np_, fields, refs) in enumerate(decls):
        for n, (j, args, meth, ownw, body) in enumerate(refs):
            if body:  # the body calls the method with W or W*M
                edges.setdefault((i, "W%d" % n), []).append(
                    ((i, "W%d" % n), 1 if body == "grow" else 0))
            for q, a in enumerate(args):
                if ownw[q]:
                    continue  # a method's own parameter: no edge (GEN-14)
                for p, e in enumerate(a):
                    if e:
                        w = 0 if (sum(a) == 1 and e == 1) else 1
                        edges.setdefault((i, p), []).append(((j, q), w))
    # a cycle with weight > 0: Bellman-Ford style on the small graph
    nodes = set(edges) | {t for v in edges.values() for t, _ in v}
    for s in nodes:
        best = {s: 0}
        work = [s]
        steps = 0
        while work and steps < 10000:
            steps += 1
            x = work.pop()
            for t, w in edges.get(x, []):
                d = best[x] + w
                if t == s and d > 0:
                    return True
                if t not in best or d > best[t]:
                    if d > len(nodes) + 1:
                        return True
                    best[t] = d
                    work.append(t)
    return False


def expand(decls, i, units):
    """Every instance reached from T_i[units] (fields and method results):
    False if a unit of one leaves the range."""
    seen = set()
    work = [(i, units)]
    while work:
        i, units = work.pop()
        key = (i, tuple((tuple(sorted(u[0].items())), u[1]) for u in units))
        if key in seen:
            continue
        seen.add(key)
        np_, fields, refs = decls[i]
        env = units
        for f in fields:
            u = ONE
            for q, e in enumerate(f):
                u = mul(u, power(env[q], e))
            if not ok(u):
                return False
        for j, args, _, ownw, _ in refs:
            if any(ownw):
                # the instance depends on what each call gives: only the
                # arguments without W are decided here (they are units of
                # the signature), and it is not followed
                for a, w in zip(args, ownw):
                    if not w:
                        u = ONE
                        for q, e in enumerate(a):
                            u = mul(u, power(env[q], e))
                        if not ok(u):
                            return False
                continue
            sub = []
            for a in args:
                u = ONE
                for q, e in enumerate(a):
                    u = mul(u, power(env[q], e))
                if not ok(u):
                    return False
                sub.append(u)
            work.append((j, sub))
        if len(seen) > 5000:
            return None  # (no growing cycle, so this does not happen)
    return True


def concrete(rng):
    b = rng.choice(list(BASES))
    c = rng.choice([1, 2, 3, 40, 100, 137, 205, 411, 412, 500, 1000])
    return ("%s^%d" % (b, c) if c != 1 else b), power(BASES[b], c)


def run(gopp, path, j):
    r = subprocess.run(["timeout", "60", gopp, "check", "-check-j=" + j,
                        "--check-error-limit=0", path],
                       capture_output=True, text=True)
    return r.returncode, r.stderr + r.stdout


def timing(gopp, out):
    bad = False
    def cyc(k):
        o = [HEAD]
        for i in range(k):
            o.append("type C%d[U unit] struct{ v float64[U^2]; n Option[*C%d[U]] }" % (i, (i + 1) % k))
        o.append("func main() { var x C0[KM^100]; _ = x }")
        return "\n".join(o) + "\n"
    def chain(k):
        o = [HEAD, "type W0[U unit] struct{ v float64[U^3] }"]
        for i in range(1, k):
            o.append("type W%d[U unit] struct{ n Option[*W%d[U]]; m Option[*W%d[U*U]] }" % (i, i - 1, max(i - 2, 0)) if i % 2 else
                     "type W%d[U unit] struct{ n Option[*W%d[U]] }" % (i, i - 1))
        o.append("func main() { var x W%d[KM]; _ = x }" % (k - 1))
        return "\n".join(o) + "\n"
    for name, make, sizes in (("cycle", cyc, (10, 100, 1000, 4000)),
                              ("chain", chain, (2000, 8000))):
        t = []
        for k in sizes:
            p = os.path.join(out, "%s%d.gopp" % (name, k))
            open(p, "w").write(make(k))
            s = time.time()
            rc, o = run(gopp, p, "1")
            t.append(time.time() - s)
            if rc not in (0, 1) or "error[E" in o and "E0435" not in o:
                print("%s %d: rc %d %s" % (name, k, rc, o[:300]))
                bad = True
        ok_ = t[-1] <= 12 * max(t[-2], 0.05)
        bad |= not ok_
        print("%s %s: near-linear %s" % (name, "/".join(map(str, sizes)),
                                          "yes" if ok_ else "NO %s" % t))
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seeds", type=int, default=40)
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--timing", action="store_true")
    ap.add_argument("--j1", action="store_true",
                    help="no second run with 8 body threads")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    fails = rejected = cycles = ranges = 0
    for seed in range(a.first, a.first + a.seeds):
        rng = random.Random(seed)
        decls = gen(rng, rng.choice([1, 2, 3, 4, 6]))
        tops, units_v, alias = [], [], []
        for _ in range(rng.choice([1, 2, 3])):
            i = rng.randrange(len(decls))
            us = [concrete(rng) for _ in range(decls[i][0])]
            tops.append((i, [t for t, _ in us]))
            units_v.append((i, [u for _, u in us]))
            alias.append(rng.random() < 0.3)
        text = program(decls, tops, alias)
        path = os.path.join(a.out, "g%d.gopp" % seed)
        open(path, "w").write(text)
        if growing_cycle(decls):
            want = False  # Go's instantiation cycle
            cycles += 1
        else:
            res = [all(ok(u) for u in us) and expand(decls, i, us)
                   for i, us in units_v]
            if None in res and False not in res:
                continue  # (an expansion too large to decide here)
            want = all(res)
            ranges += not want
        rc1, o1 = run(a.gopp, path, "1")
        rc8, o8 = (rc1, o1) if a.j1 else run(a.gopp, path, "8")
        got = rc1 == 0
        rejected += not got
        if rc1 not in (0, 1) or o1 != o8 or got != want:
            fails += 1
            print("seed %d: FAIL want %s got %s (rc %d, j1==j8 %s)\n%s" %
                  (seed, "accept" if want else "reject",
                   "accept" if got else "reject", rc1, o1 == o8, o1[:600]))
    print("typegraph: %d programs, %d rejected (%d instantiation cycles, %d "
          "out of range), %d mismatches" % (a.seeds, rejected, cycles, ranges,
                                             fails))
    bad = fails > 0
    if a.timing:
        bad |= timing(a.gopp, a.out)
    return 1 if bad else 0


sys.exit(main())
