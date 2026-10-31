#!/usr/bin/env python3
"""Checks the line tables of LLVM IR text (-emit=llvm): every DILocation whose
scope is a function of file NAME has a line within SRC's line count; with
--has, some DILocation in a function of NAME has exactly that line; with
--inlined, some DILocation at line L of file F2 is inlined (directly or
through other inlined code) at line L1 of file F1.

usage: check-di-lines.py IR NAME=SRC [NAME=SRC ...]
           [--has NAME:LINE ...] [--inlined F2:L2@F1:L1 ...]
"""
import re
import sys


def main(argv):
    ir = open(argv[1]).read()
    lines, has, inlined, mode = {}, [], [], "src"
    for a in argv[2:]:
        if a == "--has":
            mode = "has"
        elif a == "--inlined":
            mode = "inlined"
        elif mode == "has":
            n, l = a.rsplit(":", 1)
            has.append((n, int(l)))
        elif mode == "inlined":
            inner, outer = a.split("@")
            (n2, l2), (n1, l1) = inner.rsplit(":", 1), outer.rsplit(":", 1)
            inlined.append((n2, int(l2), n1, int(l1)))
        else:
            n, src = a.split("=", 1)
            lines[n] = len(open(src).read().splitlines())
    files = {m[1]: m[2] for m in re.finditer(
        r'^(!\d+) = !DIFile\(filename: "([^"]*)"', ir, re.M)}
    scopes = {}  # DISubprogram id -> file name
    for m in re.finditer(r"^(!\d+) = distinct !DISubprogram\([^\n]*?file: (!\d+)", ir, re.M):
        scopes[m[1]] = files[m[2]].split("/")[-1]
    locs = {}  # id -> (file name, line, inlinedAt id or None)
    for m in re.finditer(
            r"^(!\d+) = (?:distinct )?!DILocation\(line: (\d+)(?:, column: \d+)?, "
            r"scope: (!\d+)(?:, inlinedAt: (!\d+))?\)", ir, re.M):
        locs[m[1]] = (scopes[m[3]], int(m[2]), m[4])
    seen, seen_inlined, bad = set(), set(), []
    for name, line, at in locs.values():
        seen.add((name, line))
        if name in lines and line > lines[name]:
            bad.append(f"{name}: line {line}, but the file has {lines[name]}")
        while at:  # every call site up the chain
            n1, l1, up = locs[at]
            seen_inlined.add((name, line, n1, l1))
            at = up
    for n, l in has:
        if (n, l) not in seen:
            bad.append(f"{n}: no location at line {l}")
    for t in inlined:
        if t not in seen_inlined:
            bad.append("no location %s:%d inlined at %s:%d" % t)
    if not seen:
        bad.append("no DILocation found")
    for b in bad:
        print("FAIL", b)
    return 1 if bad else 0


sys.exit(main(sys.argv))
