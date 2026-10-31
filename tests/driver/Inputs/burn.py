#!/usr/bin/env python3
"""For tests/driver/cpu-limit.test: `burn.py SECONDS [STATUS]` burns that much
CPU time and exits with STATUS (default 0); `burn.py version` only starts."""
import itertools
import sys
import time

if sys.argv[1] != "version":
    t, s = time.process_time, time.process_time()
    next(i for i in itertools.count() if t() - s >= float(sys.argv[1]))
    sys.exit(int(sys.argv[2]) if len(sys.argv) > 2 else 0)
