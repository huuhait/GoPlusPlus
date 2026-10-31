#!/usr/bin/env python3
"""P04.01: the time to build, verify and print GIR grows near-linearly. Runs
`gopp build` on gen-gir-stress.py <shape> N and 4N (best of four, in turn: see
best_of_both), for -emit=gir and -emit=girb, and fails if the larger run takes more than 12
times the smaller one (a quadratic walk takes 16 times). The cost of a
run is its CPU time less that of the program of size 1 (process start and
fixed work: it would flatten the ratio and let a quadratic walk on a small N
pass). A small cost under 0.05 s is clock noise: N doubles until it is not
(the limit has no floor).
--control runs a quadratic program instead of gopp: the verdict must be NO.

Usage: check-gir-linear.py --gopp <gopp> --gen <script> --out <dir> <shape> <N>
"""
import argparse
import os
import subprocess
import sys
import resource


def once(cmd, base=False):
    # The CPU time of the child (user + system), not wall time: other
    # processes on a busy machine do not add to it.
    c0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
    c1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t = (c1.ru_utime - c0.ru_utime) + (c1.ru_stime - c0.ru_stime)
    if r.returncode != 0 and not base:
        print("failed: " + " ".join(cmd) + "\n" + r.stderr[:300])
        sys.exit(1)
    return t


def best_of_both(small, big, base, limit, runs=4):
    """The costs over base of small and big: the least of `runs` runs of
    small, and of big, taken in turn (a machine that gets busier or quieter
    during the test changes both). Big runs only while the pair is over the
    limit, `runs` times at most: a busy machine only adds time, so more runs
    could only lower its cost, and the verdict is the one `runs` runs of
    each give. The small side, which a quadratic walk needs to be too slow
    to pass, always gets all its runs."""
    a = b = 1e9
    nb = 0
    for _ in range(runs):
        a = min(a, once(small) - base)
        if nb == 0 or b > limit * a:
            b = min(b, once(big) - base)
            nb += 1
    while nb < runs and b > limit * a:
        b = min(b, once(big) - base)
        nb += 1
    return a, b


MIN_SMALL = 0.05  # a run shorter than this is clock noise: N doubles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-ratio", type=float, default=12.0)
    ap.add_argument("--emits", default="gir,girb",
                    help="the outputs to time (the text of a chain of array "
                    "types prints the whole type at every use: quadratic text)")
    ap.add_argument("--control", help="a program that is quadratic in N "
                    "(tests/sema/Inputs/quadratic.py): run instead of gopp; "
                    "the verdict must be NO")
    ap.add_argument("shape")
    ap.add_argument("n", type=int)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    ok = True

    def command(n, emit):
        if a.control:
            return [sys.executable, a.control, str(n)]
        p = os.path.join(a.out, "%s-%d.gopp" % (a.shape, n))
        with open(p, "w") as f:
            subprocess.run([sys.executable, a.gen, a.shape, str(n)], stdout=f, check=True)
        cmd = [a.gopp, "build", "-emit=" + emit, p]
        return cmd + ["-o", os.path.join(a.out, "x.girb" if emit == "girb" else "x.txt")]

    for emit in a.emits.split(","):
        # The base: process start and fixed work (the program of 1), the
        # least of three runs, taken off both sides so it does not flatten
        # the ratio.
        base = min(once(command(1, emit), base=True) for _ in range(3))
        n = a.n
        for _ in range(4):
            small, big = best_of_both(command(n, emit), command(4 * n, emit), base,
                                      a.max_ratio)
            if small >= MIN_SMALL:
                break
            n *= 2  # too short to judge: a bigger N
        # No floor on the limit: the small run is at least MIN_SMALL long.
        good = big <= a.max_ratio * small
        ok &= good
        print("%s/%s: near-linear %s (N=%d %.3fs -> %.3fs)" % (a.shape, emit, "yes" if good else "NO", n, small, big))
    return 0 if ok else 1


sys.exit(main())
