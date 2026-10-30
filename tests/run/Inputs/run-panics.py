#!/usr/bin/env python3
"""Runs the panic programs of Inputs/panics/ and checks each one.

usage: run-panics.py DIR [--names a,b,...] [--real]
       run-panics.py DIR [--names a,b,...] --qemu SYSROOT -- BUILD_COMMAND...

For every NAME.gopp in DIR (or only the --names): running it must give exit
status 2, the standard output of NAME.out, and a standard error whose first
line is NAME.err (Go's first line, D4.12: "panic: runtime error: ...").

The first form compiles and runs the programs with the fast runner
(runlib.py), or as real binaries with --real. The second builds real
binaries with BUILD_COMMAND FILE -o EXE; with --qemu the executables are for
linux/arm64 and run under qemu-aarch64.
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

argv = sys.argv[1:]
build = None
if "--" in argv:
    dash = argv.index("--")
    build = argv[dash + 1:]
    argv = argv[:dash]
opts, pos = runlib.split_args(argv)
d = pos[0]
programs = runlib.panic_programs(d, opts["names"])
prefix = ["qemu-aarch64", "-L", opts["qemu"]] if opts["qemu"] else []
fails = []
with tempfile.TemporaryDirectory() as tmp:
    for n, src in programs:
        if build is None:
            r = runlib.run_sources([src], real=opts["real"])
        else:
            exe = os.path.join(tmp, n)
            b = subprocess.run(build + [src, "-o", exe], capture_output=True,
                               timeout=60)
            if b.returncode != 0:
                fails.append("%s: build failed: %s" % (n, b.stderr[:300]))
                continue
            r = runlib.run_exe(exe, prefix)
        if r.failure():
            fails.append("%s: %s" % (n, r.failure()))
            continue
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
