#!/usr/bin/env python3
"""Differential test of Go's struct embedding: `gopp check --go` against
`go build` (TYP-26: .go files keep Go's embedding). Each program in DIR says
`// want: ok` or `// want: error`; both tools must give that outcome, and for
a program with errors they must report errors on the same lines.

Usage: go-embedding-diff.py GOPP DIR
Each tool runs under a 60 s limit."""
import os
import re
import shutil
import subprocess
import sys
import tempfile

gopp, src = os.path.abspath(sys.argv[1]), sys.argv[2]
LINE = re.compile(r"^(?:\./)?[\w.-]+\.go:(\d+):\d+: ")


def lines_of(out):
    return sorted({int(m.group(1)) for l in out.splitlines() if (m := LINE.match(l))})


def run(argv, cwd):
    try:
        p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    return p.returncode, p.stdout + p.stderr


problems = []
names = sorted(n for n in os.listdir(src) if n.endswith(".go"))
for n in names:
    text = open(os.path.join(src, n)).read()
    want = re.search(r"^// want: (ok|error)$", text, re.M)
    if not want:
        problems.append(f"{n}: no `// want:` line")
        continue
    with tempfile.TemporaryDirectory() as d:
        shutil.copy(os.path.join(src, n), os.path.join(d, n))
        grc, gout = run(["go", "build", "-o", os.devnull, n], d)
        prc, pout = run([gopp, "check", "--go", "--check-error-limit=0", n], d)
    if grc is None or prc is None:
        problems.append(f"{n}: a run took more than 60 s")
        continue
    gok, pok = grc == 0, prc == 0
    if gok != (want.group(1) == "ok"):
        problems.append(f"{n}: go build says {'ok' if gok else 'error'}, the file wants {want.group(1)}:\n{gout}")
    if pok != gok:
        problems.append(f"{n}: gopp says {'ok' if pok else 'error'}, go build {'ok' if gok else 'error'}:\n{pout}\n{gout}")
    elif not gok and lines_of(gout) != lines_of(pout):
        problems.append(f"{n}: error lines differ: go build {lines_of(gout)}, gopp {lines_of(pout)}:\n{gout}\n{pout}")
for p in problems:
    print(p)
print(f"{len(names)} programs, {len(problems)} problems")
sys.exit(1 if problems else 0)
