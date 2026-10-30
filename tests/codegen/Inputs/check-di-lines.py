#!/usr/bin/env python3
"""Checks the line tables of LLVM IR text (-emit=llvm): every DILocation of
a function in file NAME has a line within SRC's line count; with --has, some
DILocation in a function of NAME has exactly that line.

usage: check-di-lines.py IR NAME=SRC [NAME=SRC ...] [--has NAME:LINE ...]
"""
import re
import sys


def main(argv):
    ir = open(argv[1]).read()
    lines, has, mode = {}, [], "src"
    for a in argv[2:]:
        if a == "--has":
            mode = "has"
        elif mode == "has":
            n, l = a.rsplit(":", 1)
            has.append((n, int(l)))
        else:
            n, src = a.split("=", 1)
            lines[n] = len(open(src).read().splitlines())
    files = {m[1]: m[2] for m in re.finditer(
        r'^(!\d+) = !DIFile\(filename: "([^"]*)"', ir, re.M)}
    scopes = {}  # DISubprogram id -> file name
    for m in re.finditer(r"^(!\d+) = distinct !DISubprogram\([^\n]*?file: (!\d+)", ir, re.M):
        scopes[m[1]] = files[m[2]].split("/")[-1]
    seen, bad = set(), []
    for m in re.finditer(r"^(!\d+) = !DILocation\(line: (\d+)(?:, column: \d+)?, scope: (!\d+)\)", ir, re.M):
        line, name = int(m[2]), scopes[m[3]]
        seen.add((name, line))
        if name in lines and line > lines[name]:
            bad.append(f"{name}: line {line}, but the file has {lines[name]}")
    for n, l in has:
        if (n, l) not in seen:
            bad.append(f"{n}: no location at line {l}")
    if not seen:
        bad.append("no DILocation found")
    for b in bad:
        print("FAIL", b)
    return 1 if bad else 0


sys.exit(main(sys.argv))
