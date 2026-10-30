#!/usr/bin/env python3
"""Reads the JSON lines of `gopp test-run` on standard input and prints one
line per program, sorted by id, for FileCheck:

  NAME: outcome exit=N signal=N restarted=B stdout=<repr> stderr=<repr of the first line>

(the diagnostics' first line for a program that did not compile or load).
Ids are shown by their last path part.
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

rows = []
for line in sys.stdin:
    if not line.strip():
        continue
    d = json.loads(line)
    _, r = runlib._decode(line)
    first = (r.stderr.split(b"\n")[0] if r.stderr else
             r.message.split("\n")[0].encode())
    rows.append((d["id"], "%s: %s exit=%d signal=%d restarted=%s stdout=%r first-line=%r" % (
        os.path.basename(d["id"]), d["outcome"], d["exit"], d["signal"],
        str(d["restarted"]).lower(), r.stdout, first)))
for _, text in sorted(rows):
    print(text)
