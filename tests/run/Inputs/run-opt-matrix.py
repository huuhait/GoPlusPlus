#!/usr/bin/env python3
"""Builds and runs the run-test corpus at every `opt` level and with
-pipeline=fastisel, and requires the same result everywhere (P04.13, D4.8:
the pipeline decides speed, never what a program does).

usage: run-opt-matrix.py --gopp GOPP --root TESTS/RUN [options]
                         [--cabi [reverse:]SEED:COUNT ...] [NAME ...]

The corpus (under --root, the tests/run directory):
  * every NAME.test that holds a Go++ program (`package main`);
  * the directories of several files (Inputs/core-init, Inputs/core-init-order);
  * the programs of ../debug/Inputs (the debugger sessions of P04.12: locals
    and types, a call chain, scopes, inlining across files, a deep chain of
    types, the parameter shapes of P04.25);
  * every program of Inputs/diff, Inputs/coll, Inputs/opt, Inputs/allocword
    (the allocator word, P04.25: built with -test-intrinsics), Inputs/iface
    and Inputs/iface-gopp, Inputs/cc (the calling convention, P04.18), and
    the panic programs of Inputs/panics, Inputs/panics-seq, Inputs/panics-gopp,
    Inputs/panics-allocword, Inputs/panics-iface and Inputs/oom (an
    allocation that fails);
  * with --cabi SEED:COUNT (repeatable; needs clang): the C ABI program of
    gen-cabi.py for that seed with COUNT signatures, named cabi/seedSEED
    (--cabi reverse:SEED:COUNT: C calling Go++ through @Export and callbacks,
    gen-cabi.py --direction reverse, P04.18; named cabi/reverseSEED). Its
    C half is built once by Clang (for aarch64 with --target arm64) and
    linked into every configuration (-link-obj); its expected output is the
    generator's, and it must exit with status 0 (as in run-cabi.py).
With names, only the programs of the corpus whose name contains one of them
(e.g. `diff/int_`); the --cabi programs are always run. --no-corpus: only
the --cabi programs. --shard K/N: only the corpus programs whose place in the
sorted corpus is K modulo N, every configuration of each (the long tests run
the corpus in shards side by side). --list prints the builds and stops.

Each program is built once per configuration (one `gopp build -batch` builds
a batch of programs: tests/gopp_batch.py) and run (stdin empty, 60 s limit,
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

With --verify-dwarf every program built must also pass
`llvm-dwarfdump --verify` (P04.12: the DWARF is well formed at every `opt`, on
both targets). --verify-dwarf-self-test shows that this check refuses a broken
object and accepts a good one (both sides), and runs nothing else. With
--no-debug every build has no DWARF (`gopp build -no-debug`): the results must
be the same without debug information as with it (P04.12 keeps the variables
of `opt 0` in memory only with DWARF).

The compile times of the configurations: bench/pipelines/measure-opt.py.
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import gopp_batch  # noqa: E402
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

# Tests whose program is not a plain `package main` run (core-ptr-print and
# iface-print print addresses, which move with the frame size and the
# optimizer's layout of the data; the others test the toolchain: a probe,
# links with other flags, containers).
SKIP_TESTS = {"core-ptr-print", "iface-print", "debian", "debian-container.long", "link-flags",
              "deterministic", "arm64-qemu-features", "arm64-qemu-hello"}


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
                    ("allocword", "golden"), ("iface", "golden"),
                    ("iface-gopp", "golden"), ("cc", "golden"),
                    ("panics", "panic"), ("panics-seq", "panic"),
                    ("panics-gopp", "panic"), ("panics-allocword", "panic"),
                    ("panics-iface", "panic"),
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
    # The programs of the debugger tests (P04.12: tests/debug/Inputs): the
    # DWARF of every type and variable kind, a call chain, shadowed names and
    # scopes, inlined code across files, and a chain of types that is cut. Their expected output is saved
    # beside them (by hand: they are Go++ programs).
    dbg = os.path.join(root, "..", "debug", "Inputs")
    for stem, files in (("locals", ["locals.gopp"]), ("chain", ["chain.gopp"]),
                        ("types", ["types.gopp"]), ("scopes", ["scopes.gopp"]),
                        ("inl", ["inl/a.gopp", "inl/b.gopp"]),
                        ("deep", ["deep.gopp"]), ("params", ["params.gopp"])):
        if os.path.isfile(os.path.join(dbg, files[0])):
            base = os.path.join(dbg, stem)
            progs.append(Program("debug/" + stem, [os.path.join(dbg, f) for f in files],
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
        # SEED:COUNT, or reverse:SEED:COUNT for C calling Go++ (P04.18)
        parts = spec.split(":")
        reverse = parts[0] == "reverse"
        seed, count = parts[-2:]
        name = ("reverse" if reverse else "seed") + seed
        d = os.path.join(work, "cabi-" + name)
        os.makedirs(d, exist_ok=True)
        gen = [sys.executable, os.path.join(root, "Inputs", "gen-cabi.py"),
               "--seed", seed, "--count", count, "--dir", d]
        if reverse:
            gen += ["--direction", "reverse"]
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
        p = Program("cabi/" + name, [os.path.join(d, "main.gopp")])
        p.expected = os.path.join(d, "expected.txt")
        p.link_objs = [os.path.join(d, "lib.o")]
        progs.append(p)
    return progs


class Env:
    verify_dwarf = False


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


def dwarf_problems(path):
    """What `llvm-dwarfdump --verify` finds in an object or program ("": it
    accepts it). Any error fails: nothing is excused."""
    with procslots.slot():
        v = subprocess.run(["llvm-dwarfdump", "--verify", path], capture_output=True,
                           timeout=60)
    text = (v.stdout + v.stderr).decode(errors="replace")
    if v.returncode == 0 and "No errors." in text:
        return ""
    return text[:600] or "exit status %d" % v.returncode


def verify_self_test(gopp):
    """Both sides of --verify-dwarf: the object of a program passes; the same
    object with the length of its first .debug_info unit broken does not."""
    with tempfile.TemporaryDirectory() as d:
        src, obj = os.path.join(d, "t.gopp"), os.path.join(d, "t.o")
        open(src, "w").write("package main\n\nfunc main() {\n\tx := 1\n\tprintln(x)\n}\n")
        subprocess.run([gopp, "build", "-emit=obj", src, "-o", obj], check=True,
                       timeout=60)
        why = dwarf_problems(obj)
        if why:
            sys.exit("self-test: a good object is refused: " + why)
        info, bad = os.path.join(d, "info.bin"), os.path.join(d, "bad.o")
        subprocess.run(["llvm-objcopy", "--dump-section", ".debug_info=" + info, obj],
                       check=True, timeout=60)
        data = bytearray(open(info, "rb").read())
        data[0:4] = b"\x7f\x00\x00\x00"
        open(info, "wb").write(data)
        subprocess.run(["llvm-objcopy", "--update-section", ".debug_info=" + info,
                        obj, bad], check=True, timeout=60)
        if not dwarf_problems(bad):
            sys.exit("self-test: a broken object passes")
    print("verify-dwarf self-test: a good object passes, a broken one fails")


def exe_path(prog, cfg, tmp):
    return os.path.join(tmp, "%s.%s" % (prog.name.replace("/", "_"), cfg))


def build_batch(env, progs, cfg, tmp):
    """{name: CompletedProcess-like} of building each program of progs in
    configuration cfg: one `gopp build -batch` for all of them
    (tests/gopp_batch.py), each build exactly the build_cmd one."""
    jobs = "".join(json.dumps({
        "id": p.name, "inputs": p.files, "o": exe_path(p, cfg, tmp),
        "link_obj": p.link_objs,
        "test_intrinsics": any(uses_intrinsics(f) for f in p.files)}) + "\n"
        for p in progs)
    by_name = {p.name: p for p in progs}

    def single(name):
        p = by_name[name]
        with procslots.slot():
            return subprocess.run(build_cmd(env, p, cfg, exe_path(p, cfg, tmp)),
                                  capture_output=True, timeout=60)

    cmd = [env.gopp, "build"] + env.build_flags + CONFIGS[cfg] + ["-batch", "-"]
    return gopp_batch.run(cmd, [p.name for p in progs], single, stdin=jobs.encode())


def some(env, progs, cfg, tmp):
    """[(name, cfg, result)] of progs in configuration cfg: built in one
    batch, then each run."""
    built = build_batch(env, progs, cfg, tmp)
    out = []
    for prog in progs:
        b = built[prog.name]
        if b.returncode != 0:
            out.append((prog.name, cfg, "build failed: " + b.stderr.decode(errors="replace")[:300]))
            continue
        exe = exe_path(prog, cfg, tmp)
        if env.verify_dwarf:
            why = dwarf_problems(exe)
            if why:
                os.unlink(exe)
                out.append((prog.name, cfg, "llvm-dwarfdump --verify failed: " + why))
                continue
        res = run_exe(env, exe)
        os.unlink(exe)
        out.append((prog.name, cfg, res))
    return out


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
    ap.add_argument("--verify-dwarf", action="store_true",
                    help="also require `llvm-dwarfdump --verify` to pass on "
                         "every program built (P04.12; DWARF is on by default)")
    ap.add_argument("--no-debug", action="store_true",
                    help="build every program without DWARF (-no-debug)")
    ap.add_argument("--verify-dwarf-self-test", action="store_true",
                    help="only show that the --verify-dwarf check refuses a "
                         "broken object and accepts a good one")
    ap.add_argument("--no-corpus", action="store_true",
                    help="run only the --cabi programs")
    ap.add_argument("--shard", default=None, metavar="K/N",
                    help="only the corpus programs whose place in the sorted "
                         "corpus is K modulo N (every configuration of each)")
    ap.add_argument("--list", action="store_true",
                    help="print the builds this run would make, one `TARGET "
                         "CONFIGURATION PROGRAM` line each, and stop")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    if a.verify_dwarf_self_test:
        return verify_self_test(a.gopp)
    env = Env()
    env.gopp, env.target, env.sysroot = a.gopp, a.target, a.sysroot
    env.verify_dwarf = a.verify_dwarf
    if a.target == "arm64":
        if not a.sysroot:
            sys.exit("--target arm64 needs --sysroot")
        env.build_flags = ["-target=linux/arm64", "-crt-dir=" + os.path.join(a.sysroot, "lib")]
    else:
        env.build_flags = a.link_flags.split()
    if a.no_debug:
        if a.verify_dwarf:
            sys.exit("--no-debug builds have no DWARF to verify")
        env.build_flags.append("-no-debug")
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
        if a.shard:
            # The corpus is sorted (corpus()), so the shards of one N split it
            # into disjoint parts that together are all of it
            # (core-opt-matrix-shards.test checks the long tests' shards).
            k, n = map(int, a.shard.split("/"))
            progs = progs[k::n]
        if a.list:
            names = [p.name for p in progs] + ["cabi/seed" + c.split(":")[0] for c in a.cabi]
            for name in names:
                for c in cfgs:
                    print("%s %s %s" % (a.target, c, name))
            return
        progs += cabi_programs(env, a.root, a.cabi, tmp)
        if not progs:
            sys.exit("no program matches")
        results = {p.name: {} for p in progs}
        # Each configuration builds its programs in batches (one `gopp
        # build` per batch), and each batch runs its programs.
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            futs = [ex.submit(some, env, ps, c, tmp)
                    for c in cfgs for ps in gopp_batch.chunks(progs, a.jobs)]
            for f in concurrent.futures.as_completed(futs):
                for name, cfg, res in f.result():
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
