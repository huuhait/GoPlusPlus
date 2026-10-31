#!/usr/bin/env python3
"""A ceiling on the cost of one gopp run, for size tests whose input cannot
grow enough to be timed by a ratio of sizes (tests/sema/Inputs/check-linear.py):
a depth capped by the parser, a factor limit, an object file LLVM makes in
quadratic time.

usage: cpu-limit.py --sanitizer=0|1 DEFAULT SANITIZER [--status K] -- GOPP ARGS...

The cost of the run is measured against `GOPP version` on the same machine at
the same moment: the run's CPU time (user + system of that process) divided
by the least CPU time of two `GOPP version` runs taken around it. A busy
machine slows both (the CPU time of a process grows on a loaded machine too:
shared cores, page faults), so the quotient holds where a fixed number of
seconds does not. The run fails if the quotient is over the ceiling: DEFAULT
in the default build, SANITIZER in a sanitizer build (tests/lit.cfg.py gives
`%{cpu_limit}` with the right --sanitizer). A ceiling is three times the
quotient of the run alone, so a run ten times slower fails it. A quotient
over the ceiling is measured again (twice at most; the least counts). Exit
status 0 if the run exits with K (default 0) within the ceiling, 1 otherwise;
its standard input, output and error are the test's (the first run's). 240 s
of wall time is a guard against a hang, never the verdict.
"""
import os
import subprocess
import sys
import threading


def cpu_of(cmd, quiet):
    """(exit status, CPU seconds of that process, hung)."""
    out = subprocess.DEVNULL if quiet else None
    p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL if quiet else None,
                         stdout=out, stderr=out)
    hung = []
    guard = threading.Timer(240, lambda: (hung.append(1), p.kill()))
    guard.start()
    _, status, ru = os.wait4(p.pid, 0)
    guard.cancel()
    p.returncode = os.waitstatus_to_exitcode(status)
    return p.returncode, ru.ru_utime + ru.ru_stime, bool(hung)


def main():
    argv = sys.argv[1:]
    dash = argv.index("--")
    opts, cmd = argv[:dash], argv[dash + 1:]
    sanitizer = opts.pop(0) == "--sanitizer=1"
    ceiling = float(opts[1] if sanitizer else opts[0])
    want = int(opts[opts.index("--status") + 1]) if "--status" in opts else 0
    base_cmd = [cmd[0], "version"]
    best = None
    for round in range(3):
        b0 = cpu_of(base_cmd, True)[1]
        code, cpu, hung = cpu_of(cmd, round > 0)
        b1 = cpu_of(base_cmd, True)[1]
        if hung:
            sys.stderr.write("cpu-limit: no end in 240 s (hang guard): %s\n" % " ".join(cmd))
            return 1
        if code != want:
            sys.stderr.write("cpu-limit: exit status %d, expected %d: %s\n" %
                             (code, want, " ".join(cmd)))
            return 1
        q = cpu / max(min(b0, b1), 1e-3)
        best = q if best is None else min(best, q)
        if best <= ceiling:
            return 0
    sys.stderr.write("cpu-limit: %.1f times the CPU time of `gopp version`, over the "
                     "ceiling of %.1f: %s\n" % (best, ceiling, " ".join(cmd)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
