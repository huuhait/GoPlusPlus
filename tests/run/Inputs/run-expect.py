#!/usr/bin/env python3
"""Run a built program and check its exact standard output and exit code.

usage: run-expect.py EXE EXPECTED_EXIT [EXPECTED_STDOUT_FILE]

The expected file is compared byte for byte (no text mode, no newline
translation). Without it, the output must be empty.
"""
import subprocess
import sys

exe, code = sys.argv[1], int(sys.argv[2])
want = open(sys.argv[3], "rb").read() if len(sys.argv) > 3 else b""
r = subprocess.run([exe], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                   stderr=subprocess.PIPE, timeout=20)
if r.returncode != code:
    sys.exit("exit code %d, expected %d" % (r.returncode, code))
if r.stdout != want:
    sys.exit("stdout differs:\n got:  %r\n want: %r" % (r.stdout[:300], want[:300]))
if r.stderr:
    sys.exit("unexpected stderr: %r" % r.stderr[:300])
