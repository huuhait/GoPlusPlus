#!/usr/bin/env python3
"""P03.25: the check time of a unit stress shape grows near-linearly. Runs
gen-units-stress.py <shape> N and 4N, takes the best of two runs of each, and
fails if the larger run takes more than 12 times the smaller (a quadratic
analysis takes 16 times). A floor of 0.05 s on the small run keeps timer noise
out of the ratio.

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
import time


def best(gopp, path):
    t = 1e9
    for _ in range(2):
        s = time.time()
        r = subprocess.run([gopp, "check", "-check-j=1", "--check-error-limit=0", path],
                           capture_output=True, text=True, timeout=60)
        t = min(t, time.time() - s)
        if r.returncode not in (0, 1):
            print("crash: " + r.stderr[:200])
            sys.exit(1)
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("shape")
    ap.add_argument("n", type=int)
    ap.add_argument("--max-ratio", type=float, default=12.0)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    times = []
    for n in (a.n, 4 * a.n):
        p = os.path.join(a.out, "%s-%d.gopp" % (a.shape, n))
        with open(p, "w") as f:
            subprocess.run([sys.executable, a.gen, a.shape, str(n)], stdout=f, check=True)
        times.append(best(a.gopp, p))
    small, big = max(times[0], 0.05), times[1]
    ok = big <= a.max_ratio * small
    print("%s: near-linear %s" % (a.shape, "yes" if ok else "NO (%.2fs -> %.2fs)" % (times[0], times[1])))
    return 0 if ok else 1


sys.exit(main())
