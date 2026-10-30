#!/usr/bin/env python3
"""P03.25: the check time of a unit stress shape grows near-linearly. Runs
gen-units-stress.py <shape> N and 4N, takes the best of four runs of each, small and big in turn, and
fails if the larger run takes more than 12 times the smaller (a quadratic
analysis takes 16 times). A small run under 0.05 s is
clock noise: N doubles until it is not (the limit itself has no floor).
--control PROG runs a program that is quadratic in N instead of gopp; it must
be reported as NOT near-linear (tests/sema/linear-control.long.test).

--max-ratio R replaces the 12 (a shape whose output itself grows
quadratically, as the units of a long product do, is bounded by 20: cubic
takes 64 times).

Usage: check-units-linear.py --gopp <gopp> --gen <script> --out <dir>
       [--max-ratio R] <shape> <N>
"""
import argparse
import os
import subprocess
import sys
import resource


def once(cmd):
    # The CPU time of the child (user + system), not wall time: other
    # processes on a busy machine do not add to it.
    c0 = resource.getrusage(resource.RUSAGE_CHILDREN)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    c1 = resource.getrusage(resource.RUSAGE_CHILDREN)
    t = (c1.ru_utime - c0.ru_utime) + (c1.ru_stime - c0.ru_stime)
    if r.returncode not in (0, 1):
        print("crash: " + r.stderr[:200])
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
    ap.add_argument("shape")
    ap.add_argument("n", type=int)
    ap.add_argument("--max-ratio", type=float, default=12.0)
    ap.add_argument("--control", help="a program that is quadratic in N "
                    "(Inputs/quadratic.py): run instead of gopp; the verdict "
                    "must be NO")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    def command(n):
        if a.control:
            return [sys.executable, a.control, str(n)]
        p = os.path.join(a.out, "%s-%d.gopp" % (a.shape, n))
        with open(p, "w") as f:
            subprocess.run([sys.executable, a.gen, a.shape, str(n)], stdout=f, check=True)
        return [a.gopp, "check", "-check-j=1", "--check-error-limit=0", p]

    n = a.n
    for _ in range(4):
        small, big = best_of_both(command(n), command(4 * n))
        if small >= MIN_SMALL:
            break
        n *= 2  # too short to judge: a bigger N
    # No floor on the limit: the small run is at least MIN_SMALL long.
    ok = big <= a.max_ratio * small
    print("%s: near-linear %s (N=%d %.3fs -> %.3fs)" % (a.shape, "yes" if ok else "NO", n, small, big))
    return 0 if ok else 1


sys.exit(main())
