#!/usr/bin/env python3
"""P04.01: the time to build, verify and print GIR grows near-linearly. Runs
`gopp build` on gen-gir-stress.py <shape> N and 4N (best of two), for
-emit=gir and -emit=girb, and fails if the larger run takes more than 12
times the smaller one (a quadratic walk takes 16 times). A floor of 0.05 s on
the small run keeps timer noise out of the ratio.

Usage: check-gir-linear.py --gopp <gopp> --gen <script> --out <dir> <shape> <N>
"""
import argparse
import os
import subprocess
import sys
import time


def best(cmd):
    t = 1e9
    for _ in range(2):
        s = time.time()
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        t = min(t, time.time() - s)
        if r.returncode != 0:
            print("failed: " + " ".join(cmd) + "\n" + r.stderr[:300])
            sys.exit(1)
    return t


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--gen", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-ratio", type=float, default=12.0)
    ap.add_argument("shape")
    ap.add_argument("n", type=int)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    ok = True
    for emit in ("gir", "girb"):
        times = []
        for n in (a.n, 4 * a.n):
            p = os.path.join(a.out, "%s-%d.gopp" % (a.shape, n))
            with open(p, "w") as f:
                subprocess.run([sys.executable, a.gen, a.shape, str(n)], stdout=f, check=True)
            cmd = [a.gopp, "build", "-emit=" + emit, p]
            if emit == "girb":
                cmd += ["-o", os.path.join(a.out, "x.girb")]
            else:
                cmd += ["-o", os.path.join(a.out, "x.txt")]
            times.append(best(cmd))
        small, big = max(times[0], 0.05), times[1]
        good = big <= a.max_ratio * small
        ok &= good
        print("%s/%s: near-linear %s" % (a.shape, emit, "yes" if good else "NO (%.2fs -> %.2fs)" % tuple(times)))
    return 0 if ok else 1


sys.exit(main())
