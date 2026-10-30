#!/usr/bin/env python3
"""P04.01: the time to build, verify and print GIR grows near-linearly. Runs
`gopp build` on gen-gir-stress.py <shape> N and 4N (best of four, in turn), for
-emit=gir and -emit=girb, and fails if the larger run takes more than 12
times the smaller one (a quadratic walk takes 16 times). A small run under
0.05 s is clock noise: N doubles until it is not (the limit has no floor).
--control runs a quadratic program instead of gopp: the verdict must be NO.

Usage: check-gir-linear.py --gopp <gopp> --gen <script> --out <dir> <shape> <N>
"""
import argparse
import os
import subprocess
import sys
import time


def once(cmd):
    s = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    t = time.time() - s
    if r.returncode != 0:
        print("failed: " + " ".join(cmd) + "\n" + r.stderr[:300])
        sys.exit(1)
    return t


def best_of_both(small, big, runs=4):
    """The least of `runs` runs of each, taken in turn: a machine that gets
    busier or quieter during the test changes both."""
    a = b = 1e9
    for _ in range(runs):
        a = min(a, once(small))
        b = min(b, once(big))
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
        n = a.n
        for _ in range(4):
            small, big = best_of_both(command(n, emit), command(4 * n, emit))
            if small >= MIN_SMALL:
                break
            n *= 2  # too short to judge: a bigger N
        # No floor on the limit: the small run is at least MIN_SMALL long.
        good = big <= a.max_ratio * small
        ok &= good
        print("%s/%s: near-linear %s (N=%d %.3fs -> %.3fs)" % (a.shape, emit, "yes" if good else "NO", n, small, big))
    return 0 if ok else 1


sys.exit(main())
