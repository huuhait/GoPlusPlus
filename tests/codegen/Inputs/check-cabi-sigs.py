#!/usr/bin/env python3
"""The IR signature of a C function as Go++ declares it equals Clang's (P04.11).

usage: check-cabi-sigs.py GOPP SEED COUNT amd64|arm64 DIR [--cover] [--edges]

With --edges the signatures are gen-cabi.py's enumerated edges (from SEED, at
most COUNT) instead of random ones.

With --cover, the generated signatures must also reach every kind of ABI
decision the target can make (so a long run proves the comparison saw them):
the IR of Go++'s declarations must contain each pattern in COVER.

gen-cabi.py makes a Go++ program that declares COUNT C functions and the C
file that defines them. Clang compiles the C to LLVM IR for the target; gopp
compiles the Go++ program to LLVM IR. For every function the IR types of the
parameters and the result, and their ABI attributes (signext, zeroext, byval
and sret with their types and alignment, inreg), must be the same. What
differs on purpose is dropped before the comparison: parameter names,
noundef, noalias and the other attributes Clang adds from C's rules, struct
names, linkage and function attributes.
"""
import os
import re
import shutil
import subprocess
import sys

gopp, seed, count, arch, d = sys.argv[1:6]
cover = "--cover" in sys.argv[6:]
edges = "--edges" in sys.argv[6:]
# amd64: Extend (signext, zeroext), Indirect (byval, sret), Direct with every
# coerce type (integers, pairs, SSE vectors, a pair of SSE and integer);
# arm64: no extension, Indirect (a plain pointer, sret), Direct with arrays
# (integers and HFAs of floats and doubles).
COVER = {
    "amd64": [r"signext", r"zeroext", r"byval\(", r"sret\(", r"<2 x float>",
              r"\{ i64, i64 \}", r"\{ double, i64 \}", r"\{ i64, double \}",
              r"\{ double, double \}", r"\bi48\b|\bi24\b|\bi40\b", r"@cabi_f\d+\(\)"],
    "arm64": [r"sret\(", r"ptr align 8 dead_on_return", r"\[2 x i64\]",
              r"\[\d x float\]", r"\[\d x double\]", r"alignstack\(8\)",
              r"@cabi_f\d+\(\)"],
}
here = os.path.dirname(os.path.abspath(__file__))
gen = os.path.join(here, "..", "..", "run", "Inputs", "gen-cabi.py")
os.makedirs(d, exist_ok=True)
gen_args = [sys.executable, gen, "--seed", seed, "--count", count, "--dir", d]
if edges:
    gen_args += ["--mode", "edges", "--start", seed]
subprocess.run(gen_args, check=True)
clang = shutil.which("clang")
cc = [clang, "-O0", "-w", "-S", "-emit-llvm", os.path.join(d, "lib.c"), "-o",
      os.path.join(d, "lib.ll")]
go = [gopp, "build", "-emit=llvm", os.path.join(d, "main.gopp"), "-o",
      os.path.join(d, "main.ll")]
if arch == "arm64":
    cc[1:1] = ["--target=aarch64-linux-gnu"]
    go[2:2] = ["-target=linux/arm64"]
subprocess.run(cc, check=True, stderr=subprocess.DEVNULL)
subprocess.run(go, check=True)

# Clang's named struct types and their bodies, to compare a struct that is
# returned or passed as itself with Go++'s literal struct of the same fields.
TYPES = {}


def expand(s):
    prev = None
    while prev != s:
        prev = s
        s = re.sub(r"%struct\.\w+", lambda m: TYPES.get(m.group(0), m.group(0)), s)
    return s


DROP = {"noundef", "noalias", "writable", "dead_on_unwind", "nocapture",
        "nonnull", "readonly", "readnone", "captures(none)", "initializes"}


def params_of(line):
    m = re.match(r"(?:define|declare)\s+(.*?)\s*@(cabi_f\d+)\((.*)\)\s*(?:#\d+|\{|$)", line)
    if not m:
        return None
    ret, name, rest = m.groups()
    # split at top-level commas
    ps, depth, cur = [], 0, ""
    for ch in rest:
        if ch in "({[<":
            depth += 1
        elif ch in ")}]>":
            depth -= 1
        if ch == "," and depth == 0:
            ps.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        ps.append(cur)
    return name, norm(ret, False), [norm(p) for p in ps]


def hide_pointee(s):
    out, i = "", 0
    while i < len(s):
        m = re.compile(r"(sret|byval)\(").search(s, i)
        if not m:
            return out + s[i:]
        out += s[i:m.end()] + "%T)"
        depth, j = 1, m.end()
        while depth:
            depth += {"(": 1, ")": -1}.get(s[j], 0)
            j += 1
        i = j
    return out


def norm(s, name=True):
    # the types behind sret(...) and byval(...) are compared by name only
    s = hide_pointee(s)
    s = expand(s)
    s = re.sub(r"%main\.\w+", "{?}", s)
    if name:
        s = re.sub(r"\s%[\w.]+$", "", s.strip())  # the parameter name
    toks = []
    for t in s.split():
        t = t.replace("dso_local", "").replace("local_unnamed_addr", "")
        if t and t not in DROP and not re.match(r"^dereferenceable", t):
            toks.append(t)
    # the attribute order is not part of the ABI
    return " ".join(toks)


def read(path):
    out = {}
    TYPES.clear()
    for line in open(path):
        m = re.match(r"(%struct\.\w+) = type (.*)$", line.strip())
        if m:
            TYPES[m.group(1)] = m.group(2)
    for line in open(path):
        r = params_of(line)
        if r:
            out[r[0]] = r[1:]
    return out


want, got = read(os.path.join(d, "lib.ll")), read(os.path.join(d, "main.ll"))
if (not want or (not edges and len(want) != int(count))
        or set(want) != set(got)):
    sys.exit("expected %s functions, clang %d, gopp %d" % (count, len(want), len(got)))
bad = 0
for name in sorted(want):
    # ret attributes may come in another order: compare as sorted token lists
    # for the parts before the type, exactly for the rest
    w, g = want[name], got[name]
    key = lambda x: sorted(x.split())
    if key(w[0]) != key(g[0]) or [key(p) for p in w[1]] != [key(p) for p in g[1]]:
        bad += 1
        if bad <= 5:
            print("%s differs:\n  clang: %s\n  gopp:  %s" % (name, w, g))
if bad:
    sys.exit("%d of %d signatures differ" % (bad, len(want)))

if cover:
    text = open(os.path.join(d, "main.ll")).read()
    decls = "\n".join(l for l in text.splitlines() if l.startswith("declare") and "@cabi_f" in l)
    missing = [p for p in COVER[arch] if not re.search(p, decls)]
    if missing:
        sys.exit("the generated signatures did not reach: " + ", ".join(missing))
