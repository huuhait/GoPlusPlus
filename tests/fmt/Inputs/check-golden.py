#!/usr/bin/env python3
"""`gopp fmt INPUT` prints GOLDEN, byte for byte, for every INPUT GOLDEN pair.

usage: check-golden.py GOPP INPUT GOLDEN [INPUT GOLDEN]...

The same check as one `gopp fmt INPUT | diff - GOLDEN` a pair, with the runs in
one process (`gopp fmt -batch`, tests/gopp_batch.py; a batch that is not clean
is run again a file at a time). A run that exits with a status other than 0 fails its pair (as the pipe did
under pipefail). Prints one line when all agree, else the first differing
lines of each pair that does not (exit status 1).
"""
import difflib
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import gopp_batch  # noqa: E402
import procslots  # noqa: E402

gopp, rest = sys.argv[1], sys.argv[2:]
if not rest or len(rest) % 2:
    sys.exit("usage: check-golden.py GOPP INPUT GOLDEN [INPUT GOLDEN]...")
pairs = list(zip(rest[0::2], rest[1::2]))


def alone(path):
    with procslots.slot():
        p = subprocess.run([gopp, "fmt", path], capture_output=True)
    return gopp_batch.Result(p.returncode, p.stdout, p.stderr)


inputs = list(dict.fromkeys(i for i, _ in pairs))
got = gopp_batch.run([gopp, "fmt", "-batch"] + inputs, inputs, alone)
bad = 0
for inp, golden in pairs:
    want = open(golden, "rb").read()
    have = got[inp].stdout
    if got[inp].returncode != 0:
        bad += 1
        print("DIFFERS: gopp fmt %s exits %d (golden %s): %s" % (
            inp, got[inp].returncode, golden,
            got[inp].stderr.decode("utf-8", "replace").strip()[:300]))
    elif have != want:
        bad += 1
        diff = difflib.unified_diff(want.decode("utf-8", "replace").splitlines(),
                                    have.decode("utf-8", "replace").splitlines(),
                                    golden, "gopp fmt " + inp, lineterm="", n=1)
        print("\n".join(list(diff)[:20]))
        print("DIFFERS: gopp fmt %s and %s" % (inp, golden))
if bad:
    sys.exit(1)
print("%d goldens ok" % len(pairs))
