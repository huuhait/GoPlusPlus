#!/usr/bin/env python3
"""Requires that the LLVM IR Go++ writes for the run corpus (as lowered,
-pipeline=fastisel -emit=llvm) carries no flag that promises the optimizer
more than Go does (P04.13).

usage: check-ir-flags.py --gopp GOPP --root TESTS/RUN [--jobs N]
                         [--cabi SEED:COUNT ...] [NAME...]
       check-ir-flags.py --self-test

With NAMEs only the programs whose name contains one are scanned. --cabi adds
the C ABI program of gen-cabi.py for SEED (as in run-opt-matrix.py; only its
Go++ half is built here). On arm64 the IR also
must not extend or truncate a function argument directly (the arm64 baseline,
-pipeline=fastisel, would hand LLVM 23.1.0's AArch64 FastISel the pattern
`sext (signext i8 arg) to i64`, which it miscompiles as a zero-extend: ADR-0028
P04.13 addendum): the lowering stores an argument to its slot first.
Forbidden: `sext` or `trunc` of an argument, `zext` of an argument wider than
i1.

Both targets. Forbidden: `nsw`, `nuw`, `exact`, `nneg`, `disjoint`, `samesign`
on integer instructions (Go wraps, shifts past the width and compares
anything; `inbounds nuw` on a getelementptr is allowed: every index is
checked first) and any fast-math flag, `contract` included (FMA is decided by
the code generator, which emits llvm.fma; no pipeline may fuse anything else).
Each program is built with the same unit as the matrix: Inputs/run-opt-matrix.py

Both targets, too: every declaration of a C allocation function (malloc,
calloc, realloc, free, ...) is `nobuiltin`, so no pipeline may delete an
allocation and the runtime's out-of-memory check after it (pipelines.md
section 1).
"""
import argparse
import concurrent.futures
import importlib.util
import os
import re
import subprocess
import sys
import tempfile

INT = re.compile(
    r"\b(add|sub|mul|shl|trunc|zext|uitofp|or|icmp|lshr|ashr|udiv|sdiv)\b[^\n]*"
    r"\b(nsw|nuw|exact|nneg|disjoint|samesign)\b")
FLOAT = re.compile(
    r"\b(fadd|fsub|fmul|fdiv|frem|fneg|fcmp|fpext|fptrunc|call)\b[^\n]*"
    r"\b(fast|nnan|ninf|nsz|arcp|contract|afn|reassoc)\b")


DEFINE = re.compile(r"^define [^\n]*?\(([^\n]*)\)[^\n]*\{$", re.M)
ARG = re.compile(r"%[\w.$-]+(?=[,)]|$)")


def scan_arg_ext(text):
    """The extensions and truncations of a function argument in LLVM IR."""
    out = []
    for m in DEFINE.finditer(text):
        end = text.find("\n}", m.end())
        args = {a for a in re.findall(r"%[\w.$-]+", m.group(1))}
        body = text[m.end():end if end >= 0 else len(text)]
        for u in re.finditer(r"= (sext|trunc|zext) (?:nneg )?i(\d+) (%[\w.$-]+) to ", body):
            kind, width, v = u.group(1), int(u.group(2)), u.group(3)
            if v in args and (kind != "zext" or width > 1):
                out.append(u.group(0))
    return out


ALLOC = {"malloc", "calloc", "realloc", "reallocf", "reallocarray", "free",
         "aligned_alloc", "memalign", "valloc", "pvalloc", "posix_memalign",
         "strdup", "strndup"}
DECLARE = re.compile(r"^declare [^\n]*?@([\w.$]+)\(([^\n]*)$", re.M)
GROUP = re.compile(r"^attributes #(\d+) = \{([^\n]*)\}$", re.M)


def scan_alloc_builtin(text):
    """The C allocation functions declared without `nobuiltin`."""
    groups = {m.group(1): m.group(2) for m in GROUP.finditer(text)}
    out = []
    for m in DECLARE.finditer(text):
        if m.group(1) not in ALLOC:
            continue
        attrs = m.group(2)
        for g in re.findall(r"#(\d+)", attrs):
            attrs += " " + groups.get(g, "")
        if not re.search(r"\bnobuiltin\b", attrs):
            out.append("declare @" + m.group(1))
    return out


def self_test():
    bad = ("define i64 @f(i64 %0, i8 signext %1) {\nentry:\n"
           "  %2 = sext i8 %1 to i64\n  ret i64 %2\n}\n")
    ok = ("define i64 @f(i64 %0, i8 signext %1) {\nentry:\n"
          "  %s = alloca i8\n  store i8 %1, ptr %s\n  %2 = load i8, ptr %s\n"
          "  %3 = sext i8 %2 to i64\n  %4 = zext i1 undef to i64\n  ret i64 %3\n}\n")
    assert len(scan_arg_ext(bad)) == 1, scan_arg_ext(bad)
    assert scan_arg_ext(bad.replace("sext", "trunc").replace("i8 %1 to i64", "i64 %0 to i8")) != []
    assert scan_arg_ext(bad.replace("sext", "zext")) != []
    assert scan_arg_ext(bad.replace("sext i8", "zext i1").replace("i8 signext", "i1")) == []
    assert scan_arg_ext(ok) == [], scan_arg_ext(ok)
    # the allocation declarations
    good = "declare ptr @malloc(i64) #1\nattributes #1 = { nobuiltin nounwind }\n"
    assert scan_alloc_builtin(good) == [], scan_alloc_builtin(good)
    assert scan_alloc_builtin(good.replace("nobuiltin ", "")) == ["declare @malloc"]
    assert scan_alloc_builtin("declare ptr @calloc(i64, i64)\n") == ["declare @calloc"]
    assert scan_alloc_builtin("declare ptr @mallocx(i64)\n") == []
    # the flag scans
    assert INT.search("%3 = add nsw i32 %1, %2") and not INT.search("%3 = add i32 %1, %2")
    print("self-test ok")


def main():
    if "--self-test" in sys.argv:
        return self_test()
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*", metavar="NAME")
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--cabi", action="append", default=[], metavar="SEED:COUNT")
    a = ap.parse_args()
    spec = importlib.util.spec_from_file_location(
        "run_opt_matrix", os.path.join(a.root, "Inputs", "run-opt-matrix.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    progs = m.corpus(a.root)
    if a.files:
        progs = [p for p in progs if any(n in p.name for n in a.files)]
    tmp = tempfile.TemporaryDirectory()
    env = m.Env()
    env.target = "amd64"
    progs += m.cabi_programs(env, a.root, a.cabi, tmp.name, build_c=False)

    def check(prog, target):
        cmd = [a.gopp, "build", "-target=linux/" + target, "-pipeline=fastisel",
               "-emit=llvm"] + prog.files + ["-o", "-"]
        r = subprocess.run(cmd, capture_output=True, timeout=60)
        if r.returncode:
            return ["%s [%s]: build failed: %s" % (prog.name, target, r.stderr.decode()[:200])]
        out = []
        text = r.stdout.decode()
        for rx in (INT, FLOAT):
            mm = rx.search(text)
            if mm:
                out.append("%s [%s]: %s" % (prog.name, target, mm.group(0)[:160]))
        for d in scan_alloc_builtin(text)[:1]:
            out.append("%s [%s]: %s is not nobuiltin" % (prog.name, target, d))
        if target == "arm64":
            for u in scan_arg_ext(text)[:1]:
                out.append("%s [arm64]: %s of an argument" % (prog.name, u))
        return out

    bad = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for res in ex.map(lambda pt: check(*pt), [(p, t) for p in progs for t in ("amd64", "arm64")]):
            bad += res
    print("%d programs x 2 targets: %s" % (len(progs), "%d problems" % len(bad) if bad else "no flags"))
    if bad:
        sys.exit("\n".join(bad[:40]))


if __name__ == "__main__":
    main()
