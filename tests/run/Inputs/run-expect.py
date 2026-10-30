#!/usr/bin/env python3
"""Run a program and check its exact standard output and exit code.

usage: run-expect.py [--real] (--src PATH... | EXE) EXPECTED_EXIT [EXPECTED_STDOUT_FILE]

--src: the fast runner compiles and runs the sources (runlib.py); --real: a
real binary. EXE is a program that is already built. The expected file is
compared byte for byte (no text mode, no newline translation). Without it,
the output must be empty. Standard error must be empty.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

opts, pos = runlib.split_args(sys.argv[1:])
if opts["src"]:
    res = runlib.run_sources(opts["src"], real=opts["real"], timeout=20)
else:
    res = runlib.run_exe(pos[0], [], 20)
    pos = pos[1:]
code = int(pos[0])
want = open(pos[1], "rb").read() if len(pos) > 1 else b""
if res.failure():
    sys.exit(res.failure())
if res.returncode != code:
    sys.exit("exit code %d, expected %d" % (res.returncode, code))
if res.stdout != want:
    sys.exit("stdout differs:\n got:  %r\n want: %r" % (res.stdout[:300], want[:300]))
if res.stderr:
    sys.exit("unexpected stderr: %r" % res.stderr[:300])
