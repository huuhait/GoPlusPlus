#!/usr/bin/env python3
"""Runs `GOPP ARGS -batch FILES` and, for each file, `GOPP ARGS -` with the
file on standard input, and prints one line per file: its name, the exit
status of its record, and whether the record (status, standard output,
standard error) is byte for byte the run alone. Then the first line of
standard error of each record that has one (tests/fmt/batch.test).

usage: batch-vs-alone.py GOPP ARGS... -- FILES...
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import gopp_batch  # noqa: E402

dash = sys.argv.index("--")
cmd, files = sys.argv[1:dash], sys.argv[dash + 1:]
p = subprocess.run(cmd + ["-batch"] + files, capture_output=True)
recs, whole = gopp_batch.parse_records(p.stdout)
print("batch: exit %d, %d records%s" % (p.returncode, len(recs), "" if whole else ", trailing bytes"))
for (name, r), f in zip(recs, files):
    alone = subprocess.run(cmd + ["-"], input=open(f, "rb").read(), capture_output=True)
    same = (alone.returncode, alone.stdout, alone.stderr) == (r.returncode, r.stdout, r.stderr)
    print("%s: exit %d, %s" % (os.path.basename(name), r.returncode,
                               "same as alone" if same else "DIFFERS from alone"))
for name, r in recs:
    if r.stderr:
        print("%s: stderr: %s" % (os.path.basename(name), r.stderr.decode().splitlines()[0]))
