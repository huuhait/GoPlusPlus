#!/usr/bin/env python3
"""Run a program built for linux/amd64 and the same program built for
linux/arm64 (under qemu-aarch64 -L SYSROOT) and require equal results.

usage: run-compare-arm64.py AMD64_EXE ARM64_EXE SYSROOT [EXPECTED_EXIT]

The standard output (byte for byte), the standard error and the exit code of
the two runs must be equal; if EXPECTED_EXIT is given, the exit code must be it.
Each run is limited to 60 seconds (P04.03, owner decision M6).
"""
import subprocess
import sys

amd64, arm64, sysroot = sys.argv[1:4]
want = int(sys.argv[4]) if len(sys.argv) > 4 else None


def run(argv):
    r = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=60)
    return r.returncode, r.stdout, r.stderr


a = run([amd64])
b = run(["qemu-aarch64", "-L", sysroot, arm64])
if a[0] != b[0]:
    sys.exit("exit code: amd64 %d, arm64 %d" % (a[0], b[0]))
if want is not None and a[0] != want:
    sys.exit("exit code %d, expected %d" % (a[0], want))
if a[1] != b[1]:
    sys.exit("stdout differs:\n amd64: %r\n arm64: %r" % (a[1][:300], b[1][:300]))
if a[2] != b[2]:
    sys.exit("stderr differs:\n amd64: %r\n arm64: %r" % (a[2][:300], b[2][:300]))
if not a[1]:
    sys.exit("the program printed nothing, so the comparison proves little")
