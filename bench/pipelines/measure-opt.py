#!/usr/bin/env python3
"""Compile time of the codegen pipelines (P04.13; ADR-0028's P04.13 addendum).

usage: measure-opt.py --gopp GOPP [--target amd64|arm64] [--runs 5]
                      [--configs a,b,...] [--corpus TESTS/RUN] [FILE.gopp ...]

For each configuration and each input, the CPU time (user + system, from
wait4) of `gopp build -emit=obj` in a process of its own, best of --runs
rounds. The rounds go round-robin over all (input, configuration) pairs, so a
burst of load from other jobs hits every pair alike. With --corpus the inputs
are also summed over the run-test corpus of that directory (tests/run). The
extra configuration `gir` stops after the front end (`-emit=gir`): what the
pipelines add is the difference.

Configurations: fastisel (-pipeline=fastisel), 0 1 2 3 s z (-opt=N), and
isel:fastisel / isel:globalisel (opt 0 with that selector, the hidden -isel).
`uptime` is printed first: report it with the numbers. measure-pipelines.py
beside it times the stages of one build (-pipeline-times) instead of the whole
process.

Example (the arm64 selector decision of the P04.13 addendum):
  python3 bench/pipelines/measure-opt.py --gopp build/bin/gopp --target arm64 \\
      --configs gir,fastisel,0,isel:fastisel,isel:globalisel --runs 7 \\
      --corpus tests/run big101.gopp big102.gopp big103.gopp
(big*.gopp: `python3 tests/run/Inputs/gen-shape.py SEED 250 > bigSEED.gopp`.)
"""
import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile


def config_args(cfg):
    if cfg == "gir":
        return ["-emit=gir", "-gir-no-locs"]
    if cfg == "fastisel":
        return ["-emit=obj", "-pipeline=fastisel"]
    if cfg.startswith("isel:"):
        return ["-emit=obj", "-opt=0", "-isel=" + cfg[5:]]
    return ["-emit=obj", "-opt=" + cfg]


def cpu_of(cmd):
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    _, status, ru = os.wait4(p.pid, 0)
    p.returncode = os.waitstatus_to_exitcode(status)
    err = p.stderr.read()
    p.stderr.close()
    if p.returncode != 0:
        sys.exit("%s failed:\n%s" % (" ".join(cmd), err.decode(errors="replace")[:500]))
    return ru.ru_utime + ru.ru_stime


def load_corpus(root, runner):
    spec = importlib.util.spec_from_file_location(
        "run_opt_matrix", os.path.join(root, "Inputs", "run-opt-matrix.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return [p.files for p in mod.corpus(root, [])]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--target", default="amd64", choices=["amd64", "arm64"])
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--configs", default="gir,fastisel,0,1,2,3,s,z")
    ap.add_argument("--corpus", default=None, help="the tests/run directory")
    ap.add_argument("files", nargs="*")
    a = ap.parse_args()
    os.system("uptime")
    cfgs = a.configs.split(",")
    inputs = {}  # name -> list of source files (one program)
    if a.corpus:
        for i, files in enumerate(load_corpus(a.corpus, None)):
            inputs["corpus/%03d" % i] = files
    for f in a.files:
        inputs[os.path.basename(f)] = [f]
    target = ["-target=linux/arm64"] if a.target == "arm64" else []
    best = {}
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "x.o")
        for _ in range(a.runs):
            for name, files in inputs.items():
                for cfg in cfgs:
                    cmd = [a.gopp, "build"] + target + config_args(cfg) + files + ["-o", out]
                    t = cpu_of(cmd)
                    k = (name, cfg)
                    best[k] = min(best.get(k, t), t)
    print("target %s, CPU seconds of `gopp build -emit=obj` (best of %d rounds)" % (a.target, a.runs))
    print()
    print("| input | " + " | ".join(cfgs) + " |")
    print("|---|" + "---|" * len(cfgs))
    groups = {"corpus (%d programs)" % sum(1 for n in inputs if n.startswith("corpus/")):
              [n for n in inputs if n.startswith("corpus/")]}
    for n in inputs:
        if not n.startswith("corpus/"):
            groups[n] = [n]
    for g, names in groups.items():
        if not names:
            continue
        row = [sum(best[(n, c)] for n in names) for c in cfgs]
        base = row[cfgs.index("fastisel")] if "fastisel" in cfgs else None
        cells = ["%.3f%s" % (v, " (%.2fx)" % (v / base) if base and c != "fastisel" else "")
                 for c, v in zip(cfgs, row)]
        print("| %s | %s |" % (g, " | ".join(cells)))


if __name__ == "__main__":
    main()
