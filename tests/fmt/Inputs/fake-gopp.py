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
import os
import sys

# Few imports: this starts once per run of the real tool (the tests that use it
# start it dozens of times), and every import is part of that.
real, args = sys.argv[1], sys.argv[2:]


def broken(path):
    with open(path, "rb") as f:
        return b"BREAK" in f.read()


data = None
if args[:1] != ["build"] or "-batch" in args:
    data = sys.stdin.buffer.read()
if "-batch" in args:
    if args[0] == "build":
        import json

        files = [i for line in data.decode().splitlines() for i in json.loads(line)["inputs"]]
    else:
        files = [a for a in args if os.path.isfile(a)]
    if any(broken(f) for f in files):
        import signal

        os.kill(os.getpid(), signal.SIGKILL)
    if any(b"HANG" in open(f, "rb").read() for f in files):
        import time

        time.sleep(3600)
elif args[0] == "fmt" and b"BREAK" in data:
    sys.stderr.write("fake gopp: cannot format\n")
    sys.exit(2)
elif args[0] == "build":
    for a in args:
        if a.endswith((".gopp", ".test")) and os.path.isfile(a) and broken(a):
            sys.stderr.write("fake gopp: cannot build %s\n" % os.path.basename(a))
            sys.exit(1)
if data is None:
    os.execv(real, [real] + args)
# The real tool gets the standard input that was read here.
r, w = os.pipe()
pid = os.fork()
if pid == 0:
    os.dup2(r, 0)
    os.close(r)
    os.close(w)
    os.execv(real, [real] + args)
os.close(r)
with os.fdopen(w, "wb") as f:
    try:
        f.write(data)
    except BrokenPipeError:
        pass
_, status = os.waitpid(pid, 0)
sys.exit(os.waitstatus_to_exitcode(status) if os.WIFEXITED(status) else 128 + os.WTERMSIG(status))
