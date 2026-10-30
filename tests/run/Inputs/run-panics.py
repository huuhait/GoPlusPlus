#!/usr/bin/env python3
"""Builds and runs the panic programs of Inputs/panics/ and checks each one.

usage: run-panics.py DIR [--names a,b,...] [--qemu SYSROOT] -- BUILD_COMMAND...

For every NAME.gopp in DIR (or only the --names): BUILD_COMMAND FILE -o EXE
builds it; running EXE must give exit status 2, the standard output of
NAME.out, and a standard error whose first line is NAME.err (Go's first line,
D4.12: "panic: runtime error: ..."). With --qemu the executable is for
linux/arm64 and runs under qemu-aarch64.
"""
import os
import subprocess
import sys
import tempfile

argv = sys.argv[1:]
dash = argv.index("--")
build = argv[dash + 1:]
opts = argv[:dash]
d = opts[0]
names = None
qemu = []
i = 1
while i < len(opts):
    if opts[i] == "--names":
        names = opts[i + 1].split(",")
        i += 2
    elif opts[i] == "--qemu":
        qemu = ["qemu-aarch64", "-L", opts[i + 1]]
        i += 2
    else:
        sys.exit("bad option " + opts[i])
if names is None:
    names = sorted(f[:-5] for f in os.listdir(d) if f.endswith(".gopp"))
fails = []
with tempfile.TemporaryDirectory() as tmp:
    for n in names:
        exe = os.path.join(tmp, n)
        b = subprocess.run(build + [os.path.join(d, n + ".gopp"), "-o", exe],
                           capture_output=True, timeout=60)
        if b.returncode != 0:
            fails.append("%s: build failed: %s" % (n, b.stderr[:300]))
            continue
        r = subprocess.run(qemu + [exe], stdin=subprocess.DEVNULL,
                           capture_output=True, timeout=60)
        want_out = open(os.path.join(d, n + ".out"), "rb").read()
        want_err = open(os.path.join(d, n + ".err"), "rb").read()
        first = r.stderr.split(b"\n")[0] + b"\n"
        if r.returncode != 2:
            fails.append("%s: exit status %d, expected 2" % (n, r.returncode))
        elif r.stdout != want_out:
            fails.append("%s: stdout %r, expected %r" % (n, r.stdout[:100], want_out[:100]))
        elif first != want_err:
            fails.append("%s: stderr starts with %r, expected %r" % (n, first, want_err))
if fails:
    sys.exit("\n".join(fails))
