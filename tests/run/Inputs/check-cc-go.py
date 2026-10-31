#!/usr/bin/env python3
"""Checks that Go agrees with what gen-cc.py says a program prints (needs Go;
the tests themselves never run it).

usage: check-cc-go.py --go GO SEED COUNT [SEED COUNT]...

For every pair the generator writes main.go and expected.txt; `go run
main.go` (Go's println writes to standard error) must print exactly
expected.txt. Run it when gen-cc.py changes, and before adding a seed to a
test.
"""
import argparse
import os
import subprocess
import sys
import tempfile

ap = argparse.ArgumentParser()
ap.add_argument("--go", required=True)
ap.add_argument("pairs", nargs="+", type=int)
a = ap.parse_args()
here = os.path.dirname(os.path.abspath(__file__))
for seed, count in zip(a.pairs[0::2], a.pairs[1::2]):
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run([sys.executable, os.path.join(here, "gen-cc.py"), "--seed",
                            str(seed), "--count", str(count), "--dir", d])
        if r.returncode:
            sys.exit("gen-cc.py failed")
        r = subprocess.run([a.go, "run", "main.go"], cwd=d, capture_output=True,
                           timeout=300, env=dict(os.environ, GO111MODULE="off"))
        want = open(os.path.join(d, "expected.txt"), "rb").read()
        if r.returncode != 0 or r.stderr != want:
            got, exp = r.stderr.split(b"\n"), want.split(b"\n")
            line = next((i for i in range(max(len(got), len(exp)))
                         if (got[i] if i < len(got) else None) !=
                         (exp[i] if i < len(exp) else None)), 0)
            sys.exit("seed %d: Go prints something else (exit %d), first at line %d:\n"
                     " go:        %r\n expected:  %r"
                     % (seed, r.returncode, line + 1,
                        got[line] if line < len(got) else None,
                        exp[line] if line < len(exp) else None))
        print("seed %d count %d: Go agrees (%d lines)" % (seed, count, want.count(b"\n")))
