#!/usr/bin/env python3
"""Writes a manifest (JSON lines) for `gopp test-run -manifest` with one job
per program given; the id is the path. A program with a `// TIMEOUT: MS` line
gets that time limit (it must hang); the others get none, so the default of
60 s applies and a loaded machine cannot fail a program that has to exit.

usage: make-manifest.py PROGRAM.gopp... > manifest.jsonl"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

for path in sys.argv[1:]:
    m = re.search(r"^// TIMEOUT: (\d+)$", open(path).read(), re.M)
    print(json.dumps({"v": runlib.PROTOCOL_VERSION, "id": path, "inputs": [path],
                      "timeout_ms": int(m.group(1)) if m else 0}))
