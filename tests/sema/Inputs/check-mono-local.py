#!/usr/bin/env python3
"""P03.26: instantiation cycles through types declared in generic functions.

usage: check-mono-local.py <gopp> <work dir> [go|stress] [gopp]

Each case is one small program. `want` is what `go build` (1.25) says: the
cycle (position of the error, then each note as "line:col text") or nothing.
gopp check must say the same, both in Go mode and in Go++ mode (the same
source in a .gopp file). With `go` the cases are also run through `go build`,
so the table cannot drift from the real compiler.
"""
import os
import re
import subprocess
import sys

gopp, work = sys.argv[1], sys.argv[2]
use_go = len(sys.argv) > 3 and sys.argv[3] == "go"
stress = len(sys.argv) > 3 and sys.argv[3] == "stress"
# `gopp`: Go++ mode only (the default suite); `go` and the default run both.
modes = ("gopp",) if "gopp" in sys.argv[3:] else ("go", "gopp")
os.makedirs(work, exist_ok=True)

CYC = "instantiation cycle:"
# name, source (after `package main`), want
CASES = [
    ("spec", "func f[P any]() { type L struct{ x []P }; f[L]() }",
     ["1:8 " + CYC, "1:45 P instantiated as L", "1:24 L implicitly parameterized by P"]),
    ("no-call", "func f[P any]() { type L struct{ x []P }; _ = L{} }", []),
    ("pointer", "func f[P any]() { type L struct{ x []P }; f[*L]() }",
     ["1:8 " + CYC, "1:45 P instantiated as *L", "1:24 L implicitly parameterized by P"]),
    ("unused-param", "func f[P any]() { type L struct{ x int }; f[L]() }",
     ["1:8 " + CYC, "1:45 P instantiated as L", "1:24 L implicitly parameterized by P"]),
    ("slice-of", "func f[P any]() { type L int; f[[]L]() }",
     ["1:8 " + CYC, "1:33 P instantiated as []L", "1:24 L implicitly parameterized by P"]),
    ("same-param-first", "func f[P any]() { type L int; f[P](); f[L]() }",
     ["1:8 " + CYC, "1:41 P instantiated as L", "1:24 L implicitly parameterized by P"]),
    ("local-generic", "func f[P any]() { type G[Q any] struct{ x Q }; f[G[int]]() }",
     ["1:8 " + CYC, "1:50 P instantiated as G[int]", "1:24 G implicitly parameterized by P"]),
    ("method", "type T[P any] struct{ x P }\nfunc (t T[P]) m() { type L struct{ y []P }; var u T[L]; _ = u; u.m() }",
     ["1:8 " + CYC, "2:53 P instantiated as L", "2:26 L implicitly parameterized by P"]),
    ("nested-local", "func f[P any]() { type L struct{ x []P }; type M struct{ y L }; f[M]() }",
     ["1:8 " + CYC, "1:67 P instantiated as M", "1:48 M implicitly parameterized by P"]),
    ("through-generic", "func f[P any]() { type L struct{ x []P }; g[L]() }\nfunc g[Q any]() { f[Q]() }",
     ["1:8 " + CYC, "2:21 P instantiated as Q", "1:45 Q instantiated as L", "1:24 L implicitly parameterized by P"]),
    ("closure", "func f[P any]() { func() { type L struct{ x []P }; f[L]() }() }",
     ["1:8 " + CYC, "1:54 P instantiated as L", "1:33 L implicitly parameterized by P"]),
    ("block", "func f[P any]() { { type L struct{ x []P }; f[L]() } }",
     ["1:8 " + CYC, "1:47 P instantiated as L", "1:26 L implicitly parameterized by P"]),
    # Accepted: a local type of a function that is not generic has no
    # ambient parameters; one that only goes to a finite generic is fine.
    ("plain-func", "func f[P any]() {}\nfunc h() { type L int; f[L]() }", []),
    ("finite", "func g[Q any]() {}\nfunc f[P any]() { type L struct{ x []P }; g[L](); g[[]L]() }", []),
    ("local-in-plain-recursion", "func f[P any]() { f[P]() }\nfunc h() { type L int; f[L]() }", []),
    ("unrelated", "func f[P any]() { type L int; var x L; _ = x; f[P]() }", []),
]

failed = []


def run_stress():
    """Generated programs with thousands of local types: the time must stay
    near-linear (a timeout of 10 s each; they take a fraction of a second)."""
    n = 3000
    progs = []
    # one generic function with n local types, each given to a finite generic
    body = "\n".join("\ttype L%d struct{ x []P }\n\tg[L%d]()" % (i, i) for i in range(n))
    progs.append(("wide", "func g[Q any]() {}\nfunc f[P any]() {\n%s\n}" % body, 0))
    # n generic functions, each with a local type
    fs = "\n".join("func f%d[P any]() { type L struct{ x []P }; g[L]() }" % i for i in range(n))
    progs.append(("many", "func g[Q any]() {}\n" + fs, 0))
    # a chain of calls through local types: no cycle
    fs = "\n".join("func f%d[P any]() { type L struct{ x []P }; f%d[L]() }" % (i, i + 1) for i in range(n))
    progs.append(("chain", fs + "\nfunc f%d[P any]() {}" % n, 0))
    # the chain closed to a ring: one cycle
    fs = "\n".join("func f%d[P any]() { type L struct{ x []P }; f%d[L]() }" % (i, (i + 1) % n) for i in range(n))
    progs.append(("ring", fs, 1))
    # n rings of 3, each one cycle
    fs = "\n".join("func f%d_%d[P any]() { type L struct{ x []P }; f%d_%d[L]() }" % (j, i, j, (i + 1) % 3)
                  for j in range(n // 3) for i in range(3))
    progs.append(("rings", fs, n // 3))
    for name, src, nerr in progs:
        for mode in modes:
            path = os.path.join(work, "stress-%s.%s" % (name, "go" if mode == "go" else "gopp"))
            with open(path, "w") as f:
                f.write("package main\n" + src + "\nfunc main() {}\n")
            cmd = ["timeout", "10", gopp, "check", "--check-error-limit=0", "-check-j=1"]
            if mode == "go":
                cmd.append("--go")
            r = subprocess.run(cmd + [path], capture_output=True, text=True)
            out = r.stdout + r.stderr
            got = out.count("instantiation cycle")
            if r.returncode in (124, -9) or got != nerr or (nerr == 0 and "error[" in out):
                failed.append("stress %s [%s]: want %d cycles, got %d (exit %d)\n%s" % (name, mode, nerr, got, r.returncode, out[:300]))


if stress:
    run_stress()
    CASES.clear()
for name, src, want in CASES:
    text = "package main\n" + src + "\nfunc main() {}\n"
    # lines of the source are 1-based after `package main`: the table counts
    # from the first line of src.
    for mode in modes:
        d = os.path.join(work, name + "-" + mode)
        os.makedirs(d, exist_ok=True)
        ext = "go" if mode == "go" else "gopp"
        path = os.path.join(d, "main." + ext)
        with open(path, "w") as f:
            f.write(text)
        cmd = ["timeout", "20", gopp, "check", "--check-error-limit=0", "-check-j=1"]
        if mode == "go":
            cmd.append("--go")
        r = subprocess.run(cmd + [path], capture_output=True, text=True)
        out = r.stdout + r.stderr

        def norm(lines):
            res = []
            for l in lines:
                m = re.match(r"^\s*(?:.*?main\.\w+|\./main\.go):(\d+):(\d+):? (?:error\[E\d+\]: |note: )?(.*)$", l)
                if m:
                    line = int(m.group(1)) - 1  # `package main` is line 1
                    res.append("%d:%s %s" % (line, m.group(2), m.group(3).rstrip(":") + (":" if "cycle" in m.group(3) and "instantiation" in m.group(3) else "")))
            return res

        got = [l for l in norm(out.splitlines()) if "instantiation cycle" in l or "instantiated as" in l or "implicitly parameterized" in l]
        got = [re.sub(r"^(\d+:\d+) instantiation cycle:?$", r"\1 " + CYC, l) for l in got]
        if mode == "gopp" and not use_go:
            pass
        if got != want:
            failed.append("%s [%s]: want %s, got %s\n%s" % (name, mode, want, got, out[:400]))
    if use_go:
        d = os.path.join(work, name + "-gobuild")
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "main.go"), "w") as f:
            f.write(text)
        with open(os.path.join(d, "go.mod"), "w") as f:
            f.write("module m\n\ngo 1.25\n")
        r = subprocess.run(["timeout", "50", "go", "build", "-o", os.devnull, "."], cwd=d, capture_output=True, text=True)
        lines = [l for l in (r.stdout + r.stderr).splitlines() if not l.startswith("#")]
        gw = []
        for l in lines:
            m = re.match(r"^\s*\./main\.go:(\d+):(\d+): (.*)$", l)
            if m:
                gw.append("%d:%s %s" % (int(m.group(1)) - 1, m.group(2), m.group(3)))
        if gw != want:
            failed.append("%s: go build says %s, the table %s" % (name, gw, want))
if failed:
    print("\n".join(failed))
    sys.exit(1)
