#!/usr/bin/env python3
"""Several lit runs at once against ONE build directory (P04.19, R3).

usage: par-lit.py --gopp PATH --source-dir DIR --work DIR [--runs N] [--stale N]

Every lit run removes the batch directories of runs that ended long ago
(tests/lit_gopp_format.py). Two runs that do it together must not crash on a
directory the other one just removed. This makes STALE old directories in a
shared batch directory, starts RUNS nested lit runs of Inputs/batch-suite
together (each with its own suite copy and output, the same batch directory),
and requires that every one passes with no Python traceback.
"""
import argparse
import os
import shutil
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--gopp", required=True)
ap.add_argument("--source-dir", required=True)
ap.add_argument("--work", required=True)
ap.add_argument("--runs", type=int, default=4)
ap.add_argument("--stale", type=int, default=400)
a = ap.parse_args()

shutil.rmtree(a.work, ignore_errors=True)
shared = os.path.join(a.work, "shared")
root = os.path.join(shared, "tests", "fastrun")
os.makedirs(root)
old = 1  # 1970: older than any limit
for i in range(a.stale):
    d = os.path.join(root, "run-stale%d" % i)
    os.makedirs(d)
    with open(os.path.join(d, "f"), "w") as f:
        f.write("x")
    os.utime(d, (old, old))

suite_src = os.path.join(a.source_dir, "tests", "run", "Inputs", "batch-suite")
procs = []
for k in range(a.runs):
    suite = os.path.join(a.work, "suite%d" % k)
    shutil.copytree(suite_src, suite)
    procs.append(subprocess.Popen(
        [sys.executable, "-c", "from lit.main import main; main()", suite, "-a", "-j", "1",
         "--order=lexical", "-Dgopp=" + a.gopp, "-Dgopp_source_dir=" + a.source_dir,
         "-Dexec_root=" + os.path.join(a.work, "out%d" % k), "-Dfastrun_binary_dir=" + shared],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT))
bad = []
for k, p in enumerate(procs):
    out = p.communicate(timeout=300)[0].decode(errors="replace")
    if p.returncode != 0 or "Traceback" in out or "Passed: 3" not in out:
        bad.append("run %d: exit status %d\n%s" % (k, p.returncode, out[-800:]))
if bad:
    sys.exit("\n".join(bad))
print("%d parallel lit runs, %d stale directories: all passed" % (a.runs, a.stale))
