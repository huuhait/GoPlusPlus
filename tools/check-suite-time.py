#!/usr/bin/env python3
"""D0.13 gate: fail if the test suite got slower than the committed baseline.

Runs lit on the given test directories several times. The measure is the SUM of
the per-test times (total test work) plus the CPU time of the run tests' batch
runner (tests/lit.cfg.py), not the wall clock: tests run in
parallel, so a new test would hide behind the slowest one in the wall time. The
minimum over the runs filters machine noise. Fails when
  * that time is more than --tolerance (default 10%) above the baseline, or
  * a normal (non-long) test takes longer than --max-test-seconds (default 2).

Baselines are stored per machine class ("key") in a JSON file, because a CI
runner and a dev machine have different speeds:
    {"dev": {"seconds": 0.52}, "ci-x86-64": {"seconds": 1.9}}
Use --update to (re)write the baseline for --key. --measured skips running
lit and uses the given number instead (for the gate's own test).
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile


def run_lit(lit, jobs, dirs):
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "results.json")
        stats = os.path.join(tmp, "fastrun.json")
        # tests/lit.cfg.py runs the programs of the run tests in one batch
        # before the tests start (P04.19) and writes the CPU time of that
        # batch here; the tests themselves then only compare results.
        proc = subprocess.run(
            [lit, "-j", str(jobs), "-q", "-o", out] + dirs,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            env=dict(os.environ, GOPP_FASTRUN_STATS=stats),
        )
        if proc.returncode != 0:
            sys.stdout.write(proc.stdout)
            sys.exit("error: the test suite failed; timing is meaningless")
        with open(out) as f:
            data = json.load(f)
        data["batch_cpu"] = 0.0
        if os.path.exists(stats):
            with open(stats) as f:
                data["batch_cpu"] = json.load(f)["cpu_seconds"]
        return data


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dirs", nargs="*")
    ap.add_argument("--lit", default="lit")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--key", default="dev")
    ap.add_argument("--runs", type=int, default=10)
    ap.add_argument("--tolerance", type=float, default=0.10)
    ap.add_argument("--max-test-seconds", type=float, default=2.0)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--allow-missing", action="store_true",
                    help="warn instead of failing when --key has no baseline")
    ap.add_argument("--measured", type=float,
                    help="use this suite time instead of running lit")
    args = ap.parse_args()

    if args.measured is not None:
        best = {"work": args.measured, "elapsed": args.measured, "tests": []}
    elif args.dirs:
        best = None
        for _ in range(args.runs):
            data = run_lit(args.lit, args.jobs, args.dirs)
            data["work"] = (sum(t["elapsed"] for t in data["tests"])
                            + data["batch_cpu"])
            if best is None or data["work"] < best["work"]:
                best = data
    else:
        ap.error("give test directories or --measured")
    seconds = best["work"]
    print(f"suite time: {seconds:.3f}s of test work, {best['elapsed']:.3f}s wall "
          f"(best of {args.runs} runs, {len(best['tests'])} tests)")

    failed = False
    for t in best["tests"]:
        if ".long." in t["name"]:
            continue
        if t["elapsed"] > args.max_test_seconds:
            print(f"FAIL: {t['name']} took {t['elapsed']:.2f}s "
                  f"(limit {args.max_test_seconds}s; mark it long)")
            failed = True

    baselines = {}
    if os.path.exists(args.baseline):
        with open(args.baseline) as f:
            baselines = json.load(f)

    if args.update:
        baselines[args.key] = {"seconds": round(seconds, 3)}
        with open(args.baseline, "w") as f:
            json.dump(baselines, f, indent=2, sort_keys=True)
            f.write("\n")
        print(f"baseline '{args.key}' updated to {seconds:.3f}s")
    elif args.key in baselines:
        base = baselines[args.key]["seconds"]
        limit = base * (1 + args.tolerance)
        pct = (seconds / base - 1) * 100
        print(f"baseline '{args.key}': {base:.3f}s, limit {limit:.3f}s, now {pct:+.1f}%")
        if seconds > limit:
            print(f"FAIL: the suite is {pct:.1f}% slower than the baseline "
                  f"(allowed {args.tolerance * 100:.0f}%)")
            failed = True
    elif args.allow_missing:
        print(f"warning: no baseline for key '{args.key}'. To create it, run "
              f"`ninja check-suite-time-update` in a build configured with "
              f"-DGOPP_SUITE_TIME_KEY={args.key}, then commit {args.baseline}")
    else:
        print(f"FAIL: no baseline for key '{args.key}' in {args.baseline}")
        failed = True

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
