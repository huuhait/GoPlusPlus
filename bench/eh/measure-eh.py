#!/usr/bin/env python3
"""Time `llc -O0` on generated IR and count instruction-selection fallbacks
(P04.15, ADR-0028).

For each IR file and each selector, the best of --runs runs (each under
`timeout 60`) of: the process wall time, the process CPU time (user + system,
less sensitive to a loaded machine), and from -time-passes the CPU time of all
codegen passes and of the instruction-selection passes. The runs go
round-robin over all (file, selector) pairs, so a burst of load from other
jobs hits every pair alike. The fallback counts come from a separate run with
remarks on:

  * FastISel: `-pass-remarks-missed=sdagisel` gives one remark per
    instruction FastISel gives to SelectionDAG; works in a release LLVM, so
    no assertions build is needed for `-stats`;
  * GlobalISel: `-global-isel-abort=2` falls back to SelectionDAG per
    function and warns once per function.

  python3 bench/eh/measure-eh.py --runs 3 eh.ll noeh.ll

`--perf PATH` adds the user-space instructions of one run (`perf stat`), a
number that does not move with the load of a shared machine.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

SELECTORS = {
    # x86-64 at -O0 uses FastISel by default.
    "fast": ["-O0"],
    "dag": ["-O0", "-fast-isel=false"],
    "gisel": ["-O0", "-global-isel", "-global-isel-abort=2"],
    # AArch64 at -O0 uses GlobalISel by default; `-global-isel=false` gives
    # FastISel.
    "a64-gisel": ["-O0", "-mtriple=aarch64-unknown-linux-gnu", "-global-isel-abort=2"],
    "a64-fast": ["-O0", "-mtriple=aarch64-unknown-linux-gnu", "-global-isel=false"],
}

ISEL_PASSES = re.compile(r"(Instruction Selection|IRTranslator|Legalizer|"
                         r"RegBankSelect|InstructionSelect|Analysis for ComputingKnownBits|"
                         r"Analysis containing CSE Info)")


def llc(llc_path, args, ir, extra=()):
    """Runs llc; returns (wall s, CPU s, stderr)."""
    cmd = ["timeout", "60", llc_path] + args + list(extra) + ["-filetype=obj", "-o", "/dev/null", ir]
    with tempfile.TemporaryFile(mode="w+") as err:
        t0 = time.perf_counter()
        p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err, text=True)
        _, status, ru = os.wait4(p.pid, 0)
        wall = time.perf_counter() - t0
        p.returncode = os.waitstatus_to_exitcode(status)
        err.seek(0)
        text = err.read()
    if p.returncode != 0:
        sys.exit("error: %s failed (%d):\n%s" % (" ".join(cmd), p.returncode, text[-2000:]))
    return wall, ru.ru_utime + ru.ru_stime, text


def pass_seconds(report):
    """(all passes, instruction-selection passes) CPU time, from -time-passes.
    Only the pass report counts: SelectionDAG prints its own timer group
    ("Instruction Selection and Scheduling") with lines of the same names."""
    start = report.find("Pass execution timing report")
    head_end = report.find("===-", start)
    end = report.find("===-", report.find("\n", head_end))
    report = report[start:end if end > 0 else len(report)]
    m = re.search(r"Total Execution Time: ([0-9.]+) seconds", report)
    total = 0.0
    for line in report.splitlines():
        if ISEL_PASSES.search(line):
            # columns: user, system, user+system, wall (each "secs (pct)")
            nums = re.findall(r"([0-9.]+) \(\s*[0-9.]+%\)", line)
            if nums:
                total += float(nums[2])  # user + system
    return float(m.group(1)) if m else 0.0, total


def instructions(perf, llc_path, args, ir):
    """User-space instructions retired by one llc run (`perf stat`): unlike
    time, this does not change with the load of a shared machine."""
    with tempfile.NamedTemporaryFile(mode="r") as out:
        cmd = [perf, "stat", "-x", ",", "-e", "instructions:u", "-o", out.name, "--",
               "timeout", "60", llc_path] + args + ["-filetype=obj", "-o", "/dev/null", ir]
        p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        if p.returncode != 0:
            sys.exit("error: %s failed (%d):\n%s" % (" ".join(cmd), p.returncode, p.stderr[-2000:]))
        for line in out.read().splitlines():
            f = line.split(",")
            if len(f) > 2 and f[2].startswith("instructions"):
                return int(f[0])
    sys.exit("error: perf printed no instruction count")


def fallbacks(llc_path, args, ir):
    if "-global-isel-abort=2" in args:
        _, _, err = llc(llc_path, args, ir, ["-pass-remarks-missed=gisel.*"])
        funcs = err.count("Instruction selection used fallback path")
        why = {}
        for m in re.finditer(r"unable to (?:translate|legalize|select)[^:]*: (\w+)", err):
            why[m.group(1)] = why.get(m.group(1), 0) + 1
        return {"functions": funcs, **why}
    if "-fast-isel=false" in args:
        return {}
    _, _, err = llc(llc_path, args, ir, ["-pass-remarks-missed=sdagisel"])
    kinds = {}
    for m in re.finditer(r"FastISel (missed terminator|missed call|missed|didn't lower all arguments)"
                         r":\s+(?:%\S+ = )?(\w+)", err):
        k = m.group(1).replace("missed terminator", "term").replace("missed call", "call") \
            .replace("didn't lower all arguments", "args").replace("missed", "inst")
        key = k if k == "args" else "%s %s" % (k, m.group(2))
        kinds[key] = kinds.get(key, 0) + 1
    kinds["total"] = sum(kinds.values())
    return kinds


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("ir", nargs="+")
    ap.add_argument("--llc", default="llc")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--selectors", default="fast,dag,gisel")
    ap.add_argument("--no-fallbacks", action="store_true")
    ap.add_argument("--perf", help="path of `perf`: adds the instructions of one run")
    args = ap.parse_args()
    sels = args.selectors.split(",")
    pairs = [(ir, sel) for ir in args.ir for sel in sels]
    best = {p: [float("inf")] * 4 for p in pairs}
    cpu_runs = {p: [] for p in pairs}
    for _ in range(args.runs):
        for ir, sel in pairs:
            wall, cpu, err = llc(args.llc, SELECTORS[sel], ir, ["-time-passes"])
            got = (wall, cpu) + pass_seconds(err)
            best[(ir, sel)] = [min(a, b) for a, b in zip(best[(ir, sel)], got)]
            cpu_runs[(ir, sel)].append(cpu)
    print("| IR | selector | wall (s) | CPU (s) | CPU runs (s) | passes CPU (s) | ISel CPU (s) |%s fallbacks |"
          % (" instructions (G) |" if args.perf else ""))
    print("|---|---|---|---|---|---|---|%s---|" % ("---|" if args.perf else ""))
    for ir, sel in pairs:
        fb = "" if args.no_fallbacks else fallbacks(args.llc, SELECTORS[sel], ir)
        fbs = ", ".join("%s %d" % kv for kv in sorted(fb.items(), key=lambda kv: -kv[1])) if fb else "-"
        ins = " %.3f |" % (instructions(args.perf, args.llc, SELECTORS[sel], ir) / 1e9) if args.perf else ""
        print("| %s | %s | %.3f | %.3f | %s | %.3f | %.3f |%s %s |" % (
            (ir.rsplit("/", 1)[-1], sel) + tuple(best[(ir, sel)][:2]) +
            (" ".join("%.3f" % r for r in cpu_runs[(ir, sel)]),) + tuple(best[(ir, sel)][2:]) + (ins, fbs)))


if __name__ == "__main__":
    main()
