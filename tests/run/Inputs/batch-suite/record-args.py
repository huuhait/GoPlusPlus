#!/usr/bin/env python3
"""TEST ONLY (fastrun_batch_gopp): records the arguments the lit batch gives
`gopp` and the work directory it names, then runs the real gopp.

usage: record-args.py RECORD_FILE GOPP ARGS..."""
import os
import sys

record, gopp, args = sys.argv[1], sys.argv[2], sys.argv[3:]
with open(record, "a") as f:
    f.write(" ".join(args) + "\n")
    if "-work-dir" in args:
        # The directory exists while the batch runs (gopp makes it).
        f.write("work-dir: %s\n" % args[args.index("-work-dir") + 1])
os.execv(gopp, [gopp] + args)
