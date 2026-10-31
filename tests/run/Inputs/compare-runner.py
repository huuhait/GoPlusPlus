#!/usr/bin/env python3
"""Runs the whole run corpus twice, through the fast runner (ONE `gopp
test-run`) and as real binaries (`gopp build` in batches, then each exec), and
requires equal results byte for byte: outcome (exit status or signal), standard
output and standard error. Prints both times (P04.19, D4.14).

usage: compare-runner.py [--scale N,N,...] [--workers N] [--list]

--scale: the fast runner's time with the corpus N times over (different
job ids, same programs: every copy is compiled and run again), to show how it
grows with the number of programs. --list prints the corpus and stops.
"""
import argparse
import concurrent.futures
import json
import os
import re
import resource
import subprocess
import sys
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
sys.path.insert(0, os.path.dirname(os.path.dirname(here)))
import gopp_batch  # noqa: E402
import procslots  # noqa: E402
import runlib  # noqa: E402

RUN = os.path.dirname(here)


def corpus():
    """[(id, [sources])]: every program of tests/run that runs on the host
    and ends of its own (no infinite loop, no link error)."""
    jobs = []
    for name in sorted(os.listdir(RUN)):
        p = os.path.join(RUN, name)
        if name.endswith(".test") and re.search(r"^package main$", open(p).read(), re.M):
            jobs.append((name, [p]))
    for sub in ("panics", "diff"):
        d = os.path.join(here, sub)
        for name in sorted(os.listdir(d)):
            if name.endswith(".gopp"):
                jobs.append((sub + "/" + name, [os.path.join(d, name)]))
    for d in ("core-init", "core-init-order"):
        jobs.append((d, [os.path.join(here, d)]))
    jobs.append(("core-init-order/cba", [os.path.join(here, "core-init-order", n)
                                         for n in ("c.gopp", "b.gopp", "a.gopp")]))
    # The runner's own programs that end by themselves with a status a real
    # binary has too (runner-check.py's rule: no hang, flood or link error).
    r = os.path.join(here, "runner")
    for name in sorted(os.listdir(r)):
        if not name.endswith(".gopp"):
            continue
        text = open(os.path.join(r, name)).read()
        if re.search(r"^// EXPECT: (exit|signal) ", text, re.M) and "// REAL: no" not in text:
            jobs.append(("runner/" + name, [os.path.join(r, name)]))
    return jobs


def cpu_children():
    r = resource.getrusage(resource.RUSAGE_CHILDREN)
    return r.ru_utime + r.ru_stime


def run_fast(jobs, copies=1):
    with tempfile.TemporaryDirectory() as tmp:
        man = os.path.join(tmp, "m.jsonl")
        with open(man, "w") as f:
            for c in range(copies):
                for i, srcs in jobs:
                    f.write(json.dumps({"v": 1, "id": "%d/%s" % (c, i), "inputs": srcs}) + "\n")
        t0, c0 = time.time(), cpu_children()
        p = subprocess.run([runlib.gopp(), "test-run", "-manifest", man],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        wall, cpu = time.time() - t0, cpu_children() - c0
        if p.returncode != 0:
            sys.exit("gopp test-run failed: " + p.stderr.decode(errors="replace")[:500])
        return runlib.decode_results(p.stdout.decode()), wall, cpu


def run_real_batch(part):
    """{id: Result} of the programs of part as real binaries: built by one
    `gopp build -batch` (tests/gopp_batch.py; each build the one
    runlib._run_real makes), then each run."""
    flags = os.environ.get("GOPP_LINK_FLAGS", "").split()
    with tempfile.TemporaryDirectory() as tmp:
        exe = {i: os.path.join(tmp, str(n), "prog") for n, (i, _) in enumerate(part)}
        for e in exe.values():
            os.mkdir(os.path.dirname(e))
        srcs = dict(part)

        def single(i):
            with procslots.slot():
                return subprocess.run([runlib.gopp(), "build"] + flags + list(srcs[i]) + ["-o", exe[i]],
                                      capture_output=True, timeout=60)

        man = "".join(json.dumps({"id": i, "inputs": s, "o": exe[i]}) + "\n" for i, s in part)
        built = gopp_batch.run([runlib.gopp(), "build"] + flags + ["-batch", "-"], [i for i, _ in part],
                               single, stdin=man.encode())
        out = {}
        for i, _ in part:
            if built[i].returncode != 0:
                out[i] = runlib.Result("compile-failed", message=built[i].stderr.decode(errors="replace"))
            else:
                out[i] = runlib.run_exe(exe[i], [], 60)
        return out


def run_real(jobs, workers):
    t0, c0 = time.time(), cpu_children()
    out = {}
    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        for res in ex.map(run_real_batch, gopp_batch.chunks(jobs, 16)):
            out.update(res)
    return out, time.time() - t0, cpu_children() - c0


ADDR = re.compile(rb"0x[0-9a-f]{6,}")


def describe(r):
    # An address printed by a program (core-ptr-print) is where the stack is
    # in that process: the one thing that may differ.
    return (r.outcome, r.returncode, ADDR.sub(b"0xADDR", r.stdout),
            ADDR.sub(b"0xADDR", r.stderr))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", default="")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    jobs = corpus()
    if a.list:
        for i, s in jobs:
            print(i)
        return 0
    fast, fw, fc = run_fast(jobs)
    real, rw, rc = run_real(jobs, a.workers)
    bad = []
    for i, _ in jobs:
        f, r = fast["0/" + i], real[i]
        if describe(f) != describe(r):
            bad.append("%s: fast %r, real %r" % (
                i, (f.outcome, f.returncode, f.stdout[:80], f.stderr[:80]),
                (r.outcome, r.returncode, r.stdout[:80], r.stderr[:80])))
        elif f.outcome not in ("exited", "signaled"):
            bad.append("%s: did not run: %s" % (i, f.failure()))
    print("corpus: %d programs" % len(jobs))
    print("fast runner: %.2f s wall, %.2f s cpu (one gopp test-run)" % (fw, fc))
    print("real binaries: %.2f s wall, %.2f s cpu (%d at a time, gopp build -batch + exec)" % (rw, rc, a.workers))
    for n in [int(x) for x in a.scale.split(",") if x]:
        _, w, c = run_fast(jobs, n)
        print("fast runner x%d (%d programs): %.2f s wall, %.2f s cpu" % (n, n * len(jobs), w, c))
    if bad:
        sys.exit("\n".join(bad))
    print("all %d results equal, byte for byte" % len(jobs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
