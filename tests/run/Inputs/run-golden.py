#!/usr/bin/env python3
"""Run a program and compare its output with a golden file made by `go run`
of the same program (tests/run/Inputs/regen-goldens.py).

usage: run-golden.py [--qemu SYSROOT] [--real] (--src PATH... | EXE) GOLDEN [EXPECTED_EXIT]

With --src (one or more source files, or one directory) the program is
compiled and run by the fast runner (runlib.py); --real builds a real binary
instead. With EXE it is a program that is already built.

The output compared is the program's standard output followed by its standard
error (Go's println writes to standard error; ours writes to standard output,
D4.11 e; a panic's first line goes to standard error in both). With --qemu the
program is a linux/arm64 executable run under qemu-aarch64, and a golden named
`NAME.arm64.out` next to `NAME.out` is used when it exists (Go's FMA fusing
and float conversions differ on arm64). The test itself never needs Go.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

opts, pos = runlib.split_args(sys.argv[1:])
arm64 = opts["qemu"] is not None
if opts["src"]:
    golden = pos[0]
    code = int(pos[1]) if len(pos) > 1 else 0
    res = runlib.run_sources(opts["src"], real=opts["real"])
else:
    exe, golden = pos[0], pos[1]
    code = int(pos[2]) if len(pos) > 2 else 0
    prefix = ["qemu-aarch64", "-L", opts["qemu"]] if arm64 else []
    res = runlib.run_exe(exe, prefix)
if arm64:
    alt = golden[:-4] + ".arm64.out" if golden.endswith(".out") else golden
    if os.path.exists(alt):
        golden = alt
want = open(golden, "rb").read()
if res.failure():
    sys.exit(res.failure())
if res.returncode != code:
    sys.exit("exit code %d, expected %d (stderr: %r)" %
             (res.returncode, code, res.stderr[:300]))
got = res.stdout + res.stderr
if got != want:
    gl, wl = got.split(b"\n"), want.split(b"\n")
    for i in range(max(len(gl), len(wl))):
        g = gl[i] if i < len(gl) else None
        w = wl[i] if i < len(wl) else None
        if g != w:
            sys.exit("output differs from %s at line %d:\n got:  %r\n want: %r"
                     % (golden, i + 1, g, w))
