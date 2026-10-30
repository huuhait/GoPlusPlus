#!/usr/bin/env python3
"""same-output.py GOPP FILE...: the diagnostics of each file are the same text
in the same order with -check-j=1 and -check-j=8."""
import subprocess
import sys

gopp = sys.argv[1]
bad = False
for f in sys.argv[2:]:
    outs = []
    for j in ("1", "8"):
        r = subprocess.run([gopp, "check", "-check-j=" + j,
                            "--check-error-limit=0", f],
                           capture_output=True, text=True, timeout=60)
        outs.append(r.stderr + r.stdout)
    if outs[0] != outs[1] or "error[" not in outs[0]:
        print("DIFFERENT or no errors: " + f)
        bad = True
sys.exit(1 if bad else 0)
