#!/usr/bin/env python3
"""A gopp that breaks on purpose, for the tests of the batches
(tests/gopp_batch.py: tests/fmt/batch.test, tests/run/build-batch.test).

usage: fake-gopp.py REAL_GOPP ARGS...

It is REAL_GOPP, except for an input that contains the word HANG (a
`-batch` run with it never ends) or BREAK:
  * a `-batch` run with such an input dies by SIGKILL before it writes
    anything (as a crash of the compiler would);
  * `fmt` of such a text (on standard input) fails with exit status 2 and
    "fake gopp: cannot format";
  * `build` of such a file fails with exit status 1 and "fake gopp: cannot
    build NAME".
"""
import json
import os
import signal
import subprocess
import sys
import time

real, args = sys.argv[1], sys.argv[2:]


def broken(path):
    with open(path, "rb") as f:
        return b"BREAK" in f.read()


data = None
if args[:1] != ["build"] or "-batch" in args:
    data = sys.stdin.buffer.read()
if "-batch" in args:
    if args[0] == "build":
        files = [i for line in data.decode().splitlines() for i in json.loads(line)["inputs"]]
    else:
        files = [a for a in args if os.path.isfile(a)]
    if any(broken(f) for f in files):
        os.kill(os.getpid(), signal.SIGKILL)
    if any(b"HANG" in open(f, "rb").read() for f in files):
        time.sleep(3600)
elif args[0] == "fmt" and b"BREAK" in data:
    sys.stderr.write("fake gopp: cannot format\n")
    sys.exit(2)
elif args[0] == "build":
    for a in args:
        if a.endswith((".gopp", ".test")) and os.path.isfile(a) and broken(a):
            sys.stderr.write("fake gopp: cannot build %s\n" % os.path.basename(a))
            sys.exit(1)
sys.exit(subprocess.run([real] + args, input=data).returncode)
