#!/usr/bin/env python3
"""Compile time of the codegen pipelines and of the arm64 instruction
selectors on real Go++ programs (P04.13, ADR-0028 addendum).

usage: measure-pipelines.py GOPP TARGET PROGRAM... [--settings 0,s,fastisel]
                            [--isel default,fastisel,globalisel] [--repeat 5]

For every combination of a SETTING (`-opt=N`, or `-pipeline=fastisel`) and an
ISEL (the hidden `-isel=` flag; "default" is the pipeline's own choice), every
PROGRAM is built with `-emit=obj -o /dev/null -pipeline-times` REPEAT times.
The table gives, per combination, the sum over the programs of the best CPU
time of each stage (CPU time of the process, not wall time, so that a loaded
machine disturbs it less; the best of REPEAT runs, as in ADR-0028) and the
size of the object files. Every run is under `timeout 60`.
"""
import os
import re
import subprocess
import sys
import tempfile

args = sys.argv[1:]
gopp, target = args[0], args[1]
settings, isels, repeat = ["0"], ["default"], 5
programs = []
i = 2
while i < len(args):
    if args[i] == "--settings":
        settings = args[i + 1].split(",")
        i += 2
    elif args[i] == "--isel":
        isels = args[i + 1].split(",")
        i += 2
    elif args[i] == "--repeat":
        repeat = int(args[i + 1])
        i += 2
    else:
        programs.append(args[i])
        i += 1

STAGE = re.compile(r"(\S+)=([0-9.]+)/([0-9.]+)")


def build(prog, setting, isel, out):
    cmd = [gopp, "build", "-target=" + target, "-emit=obj", "-pipeline-times",
           "-isel=" + isel, "-o", out]
    cmd.append("-pipeline=fastisel" if setting == "fastisel" else "-opt=" + setting)
    cmd.append(prog)
    r = subprocess.run(["timeout", "60"] + cmd, capture_output=True, text=True)
    if r.returncode:
        return None
    line = [l for l in r.stderr.splitlines() if l.startswith("gopp: pipeline")]
    return {m[0]: float(m[2]) for m in STAGE.findall(line[-1])} if line else {}


print("%-10s %-10s %9s %9s %9s %9s %10s" %
      ("setting", "isel", "gir-inl", "lower", "ir-pass", "codegen", "obj bytes"))
with tempfile.TemporaryDirectory() as tmp:
    for setting in settings:
        for isel in isels:
            tot = {}
            size = 0
            skipped = 0
            for prog in programs:
                out = os.path.join(tmp, "o.o")
                best = None
                for _ in range(repeat):
                    t = build(prog, setting, isel, out)
                    if t is None:
                        best = None
                        break
                    if best is None:
                        best = dict(t)
                    else:
                        for k, v in t.items():
                            best[k] = min(best[k], v)
                if best is None:
                    skipped += 1
                    continue
                for k, v in best.items():
                    tot[k] = tot.get(k, 0) + v
                size += os.path.getsize(out)
            print("%-10s %-10s %9.4f %9.4f %9.4f %9.4f %10d%s" %
                  (setting, isel, tot.get("gir-inline", 0), tot.get("lower", 0),
                   tot.get("ir-passes", 0), tot.get("codegen", 0), size,
                   "  (%d programs did not build)" % skipped if skipped else ""))
