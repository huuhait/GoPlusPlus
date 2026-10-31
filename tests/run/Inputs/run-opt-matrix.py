#!/usr/bin/env python3
"""Builds and runs the run-test corpus at every `opt` level and with
-pipeline=fastisel, and requires the same result everywhere (P04.13, D4.8:
the pipeline decides speed, never what a program does).

usage: run-opt-matrix.py --gopp GOPP --root TESTS/RUN [options]
                         [--cabi SEED:COUNT ...] [NAME ...]

The corpus (under --root, the tests/run directory):
  * every NAME.test that holds a Go++ program (`package main`);
  * the directories of several files (Inputs/core-init, Inputs/core-init-order);
  * every program of Inputs/diff, Inputs/coll, Inputs/opt, Inputs/allocword
    (the allocator word, P04.25: built with -test-intrinsics), and the panic
    programs of Inputs/panics, Inputs/panics-seq, Inputs/panics-gopp and
    Inputs/oom (an allocation that fails);
  * with --cabi SEED:COUNT (repeatable; needs clang): the C ABI program of
    gen-cabi.py for that seed with COUNT signatures, named cabi/seedSEED. Its
    C half is built once by Clang (for aarch64 with --target arm64) and
    linked into every configuration (-link-obj); its expected output is the
    generator's, and it must exit with status 0 (as in run-cabi.py).
With names, only the programs of the corpus whose name contains one of them
(e.g. `diff/int_`); the --cabi programs are always run. --no-corpus: only
the --cabi programs.

Each program is built once per configuration and run (stdin empty, 60 s limit,
under qemu-aarch64 for --target arm64). The configurations:
  fastisel       -pipeline=fastisel: plain O0, no IR passes (the baseline;
                 FastISel on both targets)
  0 1 2 3 s z    -opt=N (opt 0 selects with FastISel on amd64, GlobalISel on
                 arm64)
The other selector at opt 0 is left out on purpose: arm64 FastISel miscompiles
a sext of a signext argument in LLVM 23.1.0 (ADR-0028, P04.13 addendum;
Pipeline.Arm64SextOfSignextArgument), and x86-64 GlobalISel is used by no
pipeline (ADR-0028 decision 2).
The exit status, standard output and standard error of every configuration
must equal the baseline's. Where Go's output is saved (golden/NAME.out,
Inputs/diff/NAME.out, Inputs/opt/NAME.out, and the .arm64.out beside it for
--target arm64), the baseline's standard output followed by its standard error
must equal it, and a program of Inputs/panics must exit with status 2, print
NAME.out and start its standard error with NAME.err.

The compile times of the configurations: bench/pipelines/measure-opt.py.
"""
import argparse
import concurrent.futures
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import procslots  # noqa: E402

CONFIGS = {
    "fastisel": ["-pipeline=fastisel"],
    "0": ["-opt=0"],
    "1": ["-opt=1"],
    "2": ["-opt=2"],
    "3": ["-opt=3"],
    "s": ["-opt=s"],
    "z": ["-opt=z"],
}
DEFAULT_CONFIGS = list(CONFIGS)

# Tests whose program is not a plain `package main` run (core-ptr-print prints
# stack addresses, which move with the frame size; the others test the
# toolchain: a probe, links with other flags, containers).
SKIP_TESTS = {"core-ptr-print", "debian", "debian-container.long", "link-flags", "deterministic",
              "arm64-qemu-features", "arm64-qemu-hello"}


class Program:
    def __init__(self, name, files, golden=None, panic=None):
        self.name = name
        self.files = files      # the source files, in order
        self.golden = golden    # base path of golden output: X for X.out / X.arm64.out
        self.panic = panic      # base path of X.out / X.err for a panic program
        self.expected = None    # a file with the whole expected output (C ABI)
        self.link_objs = []     # objects linked into the program (C ABI)


def corpus(root):
    progs = []
    for f in sorted(os.listdir(root)):
        if not f.endswith(".test"):
            continue
        stem = f[: -len(".test")]
        if stem in SKIP_TESTS:
            continue
        path = os.path.join(root, f)
        text = open(path, encoding="utf-8", errors="replace").read()
        if not re.search(r"^package main\b", text, re.M):
            continue
        g = os.path.join(root, "golden", stem)
        progs.append(Program(stem, [path], g if os.path.exists(g + ".out") else None))
    for d in ("core-init", "core-init-order"):
        p = os.path.join(root, "Inputs", d)
        if os.path.isdir(p):
            files = sorted(os.path.join(p, f) for f in os.listdir(p) if f.endswith(".gopp"))
            g = os.path.join(root, "golden", d)
            progs.append(Program("Inputs/" + d, files, g if os.path.exists(g + ".out") else None))
    for d, kind in (("diff", "golden"), ("coll", "golden"), ("opt", "golden"),
                    ("allocword", "golden"),
                    ("panics", "panic"), ("panics-seq", "panic"),
                    ("panics-gopp", "panic"), ("panics-allocword", "panic"),
                    ("oom", "panic")):
        p = os.path.join(root, "Inputs", d)
        if not os.path.isdir(p):
            continue
        for f in sorted(os.listdir(p)):
            if not f.endswith(".gopp"):
                continue
            stem = f[: -len(".gopp")]
            base = os.path.join(p, stem)
            if kind == "panic":
                progs.append(Program(d + "/" + stem, [os.path.join(p, f)], panic=base))
            else:
                progs.append(Program(d + "/" + stem, [os.path.join(p, f)],
                                     base if os.path.exists(base + ".out") else None))
    return progs


def cabi_programs(env, root, specs, work, build_c=True):
    """The generated C ABI programs (gen-cabi.py), their C built by Clang
    (build_c; check-ir-flags.py needs only the Go++ half)."""
    clang = shutil.which("clang")
    if build_c and not clang:
        sys.exit("--cabi needs clang on the PATH")
    progs = []
    for spec in specs:
        seed, count = spec.split(":")
        d = os.path.join(work, "cabi-seed" + seed)
        os.makedirs(d, exist_ok=True)
        gen = [sys.executable, os.path.join(root, "Inputs", "gen-cabi.py"),
               "--seed", seed, "--count", count, "--dir", d]
        cc = [clang, "-O1", "-w", "-c", os.path.join(d, "lib.c"), "-o",
              os.path.join(d, "lib.o")]
        if env.target == "arm64":
            cc[1:1] = ["--target=aarch64-linux-gnu"]
        for cmd in (gen, cc) if build_c else (gen,):
            r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True,
                               timeout=60)
            if r.returncode:
                sys.exit("%s failed for C ABI seed %s:\n%s" %
                         (os.path.basename(cmd[1] if cmd is gen else cmd[0]), seed,
                          r.stderr.decode(errors="replace")[:2000]))
        p = Program("cabi/seed" + seed, [os.path.join(d, "main.gopp")])
        p.expected = os.path.join(d, "expected.txt")
        p.link_objs = [os.path.join(d, "lib.o")]
        progs.append(p)
    return progs


class Env:
    pass


def uses_intrinsics(path):
    try:
        with open(path, "rb") as f:
            return b"gopp_test_" in f.read()
    except OSError:
        return False


def build_cmd(env, prog, cfg, out, extra=()):
    cmd = [env.gopp, "build"] + env.build_flags + CONFIGS[cfg] + list(extra)
    if any(uses_intrinsics(f) for f in prog.files):
        cmd.append("-test-intrinsics")  # P04.25: the allocator-word programs
    cmd += ["-link-obj=" + o for o in prog.link_objs]
    return cmd + prog.files + ["-o", out]


def run_exe(env, exe):
    cmd = (["qemu-aarch64", "-L", env.sysroot] if env.target == "arm64" else []) + [exe]
    try:
        with procslots.slot():
            r = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=60)
    except subprocess.TimeoutExpired:
        return (None, b"", b"timeout")
    return (r.returncode, r.stdout, r.stderr)


def one(env, prog, cfg, tmp):
    safe = prog.name.replace("/", "_")
    exe = os.path.join(tmp, "%s.%s" % (safe, cfg))
    with procslots.slot():
        b = subprocess.run(build_cmd(env, prog, cfg, exe), capture_output=True, timeout=60)
    if b.returncode != 0:
        return (prog.name, cfg, "build failed: " + b.stderr.decode(errors="replace")[:300])
    res = run_exe(env, exe)
    os.unlink(exe)
    return (prog.name, cfg, res)


def golden_text(env, base):
    alt = base + ".arm64.out"
    if env.target == "arm64" and os.path.exists(alt):
        return open(alt, "rb").read()
    return open(base + ".out", "rb").read()


def short(b):
    return repr(b[:200])


def check(env, prog, results):
    """results: cfg -> (rc, out, err) or an error string. Returns problems."""
    bad = []
    base = results.get("fastisel")
    for cfg, r in results.items():
        if isinstance(r, str):
            bad.append("%s [%s]: %s" % (prog.name, cfg, r))
    if bad or not isinstance(base, tuple):
        return bad
    if base[0] is None:
        return ["%s: the baseline timed out" % prog.name]
    for cfg, r in results.items():
        if cfg == "fastisel" or not isinstance(r, tuple):
            continue
        if r[0] != base[0]:
            bad.append("%s [%s]: exit status %s, baseline %s" % (prog.name, cfg, r[0], base[0]))
        elif r[1] != base[1]:
            bad.append("%s [%s]: stdout differs from the baseline: %s vs %s" %
                       (prog.name, cfg, short(r[1]), short(base[1])))
        elif r[2] != base[2]:
            bad.append("%s [%s]: stderr differs from the baseline: %s vs %s" %
                       (prog.name, cfg, short(r[2]), short(base[2])))
    if prog.panic:
        want_out = open(prog.panic + ".out", "rb").read()
        want_err = open(prog.panic + ".err", "rb").read()
        first = base[2].split(b"\n")[0] + b"\n"
        if base[0] != 2 or base[1] != want_out or first != want_err:
            bad.append("%s: baseline gave exit %s, stdout %s, stderr %s; Go: %s %s" %
                       (prog.name, base[0], short(base[1]), short(first), short(want_out), short(want_err)))
    elif prog.golden or prog.expected:
        want = (open(prog.expected, "rb").read() if prog.expected
                else golden_text(env, prog.golden))
        if prog.expected and base[0] != 0:
            bad.append("%s: baseline gave exit %s, want 0 (stderr %s)" %
                       (prog.name, base[0], short(base[2])))
        if base[1] + base[2] != want:
            got = base[1] + base[2]
            gl, wl = got.split(b"\n"), want.split(b"\n")
            for i in range(max(len(gl), len(wl))):
                g = gl[i] if i < len(gl) else None
                w = wl[i] if i < len(wl) else None
                if g != w:
                    bad.append("%s: differs from %s at line %d: got %r, want %r" %
                               (prog.name, "the generator's output" if prog.expected
                                else "Go's output", i + 1, g, w))
                    break
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--target", default="amd64", choices=["amd64", "arm64"])
    ap.add_argument("--sysroot", default=None, help="the aarch64 sysroot (arm64)")
    ap.add_argument("--link-flags", default="", help="flags of `gopp build` for the host link")
    ap.add_argument("--configs", default=None,
                    help="comma-separated, default: all (the baseline is "
                         "always added)")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--cabi", action="append", default=[], metavar="SEED:COUNT",
                    help="add the gen-cabi.py program of SEED with COUNT "
                         "signatures (needs clang)")
    ap.add_argument("--no-corpus", action="store_true",
                    help="run only the --cabi programs")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    env = Env()
    env.gopp, env.target, env.sysroot = a.gopp, a.target, a.sysroot
    if a.target == "arm64":
        if not a.sysroot:
            sys.exit("--target arm64 needs --sysroot")
        env.build_flags = ["-target=linux/arm64", "-crt-dir=" + os.path.join(a.sysroot, "lib")]
    else:
        env.build_flags = a.link_flags.split()
    cfgs = a.configs.split(",") if a.configs else list(DEFAULT_CONFIGS)
    unknown = [c for c in cfgs if c not in CONFIGS]
    if unknown:
        sys.exit("unknown configuration %s (known: %s)" %
                 (", ".join(unknown), ", ".join(CONFIGS)))
    if "fastisel" not in cfgs:
        cfgs.insert(0, "fastisel")
    t0 = time.time()
    with tempfile.TemporaryDirectory() as tmp:
        progs = [] if a.no_corpus else corpus(a.root)
        if a.names:
            progs = [p for p in progs if any(n in p.name for n in a.names)]
        progs += cabi_programs(env, a.root, a.cabi, tmp)
        if not progs:
            sys.exit("no program matches")
        results = {p.name: {} for p in progs}
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            futs = [ex.submit(one, env, p, c, tmp) for p in progs for c in cfgs]
            for f in concurrent.futures.as_completed(futs):
                name, cfg, res = f.result()
                results[name][cfg] = res
        problems = []  # the C ABI programs' expected output is in tmp
        for p in progs:
            problems += check(env, p, results[p.name])
    print("%d programs x %d configurations on %s in %.1f s: %s" %
          (len(progs), len(cfgs), a.target, time.time() - t0,
           "%d problems" % len(problems) if problems else "all equal"))
    if problems:
        sys.exit("\n".join(problems[:40]))


if __name__ == "__main__":
    main()
