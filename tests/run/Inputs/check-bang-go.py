#!/usr/bin/env python3
"""Checks that the goldens of the `!` return-path tests are what Go prints.

usage: check-bang-go.py [--go GO]

Go has no `!`, so each program has a mechanical Go form in Inputs/go-bang/:
`x := f!(a)` is `x, err := f(a); if err != nil { return zero values, err }`,
hoisted in the order Go++ evaluates it. `go run` of NAME.go must print (stdout
then stderr, as run-golden.py compares them) what golden/NAME.out holds. Needs
Go; the tests themselves never run it (the panic path's goldens have no Go
form: they are written from ERR-9).
"""
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
go = sys.argv[sys.argv.index("--go") + 1] if "--go" in sys.argv else "go"
bad = 0
for n in ("bang-return", "bang-positions", "bang-shapes", "bang-funcvalue"):
    r = subprocess.run([go, "run", os.path.join(here, "go-bang", n + ".go")],
                       capture_output=True, timeout=120)
    got = r.stdout + r.stderr
    want = open(os.path.join(here, "..", "golden", n + ".out"), "rb").read()
    if r.returncode != 0 or got != want:
        bad += 1
        print("%s: differs from golden (go exit %d)" % (n, r.returncode))
print("ok" if not bad else "FAILED")
sys.exit(1 if bad else 0)
