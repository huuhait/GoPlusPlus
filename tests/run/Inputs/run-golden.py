#!/usr/bin/env python3
"""Run a built program and compare its output with a golden file made by
`go run` of the same program (tests/run/Inputs/regen-goldens.py).

usage: run-golden.py [--qemu SYSROOT] EXE GOLDEN [EXPECTED_EXIT]

The output compared is the program's standard output followed by its standard
error (Go's println writes to standard error; ours writes to standard output,
D4.11 e; a panic's first line goes to standard error in both). With --qemu the
program is a linux/arm64 executable run under qemu-aarch64, and a golden named
`NAME.arm64.out` next to `NAME.out` is used when it exists (Go's FMA fusing
and float conversions differ on arm64). The test itself never needs Go.
"""
import os
import subprocess
import sys

args = sys.argv[1:]
argv_prefix = []
arm64 = False
if args and args[0] == "--qemu":
    argv_prefix = ["qemu-aarch64", "-L", args[1]]
    arm64 = True
    args = args[2:]
exe, golden = args[0], args[1]
code = int(args[2]) if len(args) > 2 else 0
if arm64:
    alt = golden[:-4] + ".arm64.out" if golden.endswith(".out") else golden
    if os.path.exists(alt):
        golden = alt
want = open(golden, "rb").read()
r = subprocess.run(argv_prefix + [exe], stdin=subprocess.DEVNULL,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
if r.returncode != code:
    sys.exit("exit code %d, expected %d (stderr: %r)" %
             (r.returncode, code, r.stderr[:300]))
got = r.stdout + r.stderr
if got != want:
    gl, wl = got.split(b"\n"), want.split(b"\n")
    for i in range(max(len(gl), len(wl))):
        g = gl[i] if i < len(gl) else None
        w = wl[i] if i < len(wl) else None
        if g != w:
            sys.exit("output differs from %s at line %d:\n got:  %r\n want: %r"
                     % (golden, i + 1, g, w))
