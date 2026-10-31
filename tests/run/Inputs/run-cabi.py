#!/usr/bin/env python3
"""Build and run one generated C ABI program (P04.11, gen-cabi.py).

usage: run-cabi.py [--arm64 SYSROOT] [--flags "GOPP FLAGS"] [--edges STRIDE]
                  [--reverse] [--slow N] GOPP DIR SEED COUNT [SEED COUNT]...

For every SEED/COUNT pair: gen-cabi.py writes main.gopp, lib.c and
expected.txt under DIR/seedSEED; Clang compiles lib.c for the target (the
flake's clang, for aarch64 against no headers at all: the C code declares
what it uses); gopp builds main.gopp and links lib.o (-link-obj); the program
runs (under qemu-aarch64 -L SYSROOT for arm64) and its standard output must
equal expected.txt byte for byte. Every step is limited to 60 seconds.

With --reverse the programs are the other direction (P04.18, gen-cabi.py
--direction reverse): C calls Go++, through @Export and through callbacks.

With --edges STRIDE the signatures are the enumerated edges (gen-cabi.py
--mode edges): every STRIDE-th from SEED (the start), at most COUNT.

--slow N (lit's %{slow}: 5 in a sanitizer build) runs one in N of the
signatures: COUNT/N of each random seed, every (STRIDE*N)-th edge.
"""
import os
import shutil
import subprocess
import sys

args = sys.argv[1:]
sysroot = None
flags = []
edges = None
reverse = False
slow = 1
while args and args[0] in ("--arm64", "--flags", "--edges", "--reverse",
                           "--slow"):
    if args[0] == "--reverse":
        reverse = True
        args = args[1:]
        continue
    if args[0] == "--arm64":
        sysroot = args[1]
    elif args[0] == "--edges":
        edges = args[1]
    elif args[0] == "--slow":
        slow = max(1, int(args[1]))
    else:
        flags = args[1].split()
    args = args[2:]
gopp, workdir = args[0], args[1]
pairs = [(int(args[i]), int(args[i + 1])) for i in range(2, len(args), 2)]
if edges:
    edges = str(int(edges) * slow)
else:
    pairs = [(seed, max(1, count // slow)) for seed, count in pairs]
here = os.path.dirname(os.path.abspath(__file__))
clang = shutil.which("clang")
if not clang:
    sys.exit("clang is not on the PATH")


def run(argv, **kw):
    r = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=60, **kw)
    return r


for seed, count in pairs:
    d = os.path.join(workdir, "seed%d" % seed)
    os.makedirs(d, exist_ok=True)
    gen = [sys.executable, os.path.join(here, "gen-cabi.py"), "--seed",
           str(seed), "--count", str(count), "--dir", d]
    if reverse:
        gen += ["--direction", "reverse"]
    if edges:
        gen += ["--mode", "edges", "--start", str(seed), "--stride", edges]
    r = run(gen)
    if r.returncode:
        sys.exit("gen-cabi.py failed: " + r.stderr.decode())
    cc = [clang, "-O1", "-w", "-c", os.path.join(d, "lib.c"), "-o",
          os.path.join(d, "lib.o")]
    build = [gopp, "build"] + flags + [os.path.join(d, "main.gopp"),
             "-link-obj=" + os.path.join(d, "lib.o"), "-o",
             os.path.join(d, "prog")]
    if sysroot:
        cc[1:1] = ["--target=aarch64-linux-gnu"]
        build[2:2] = ["-target=linux/arm64", "-crt-dir=" + sysroot + "/lib"]
    r = run(cc)
    if r.returncode:
        sys.exit("clang failed for seed %d:\n%s" % (seed, r.stderr.decode()))
    r = run(build)
    if r.returncode:
        sys.exit("gopp build failed for seed %d (%s):\n%s" %
                 (seed, d, r.stderr.decode()[:3000]))
    prog = [os.path.join(d, "prog")]
    if sysroot:
        prog = ["qemu-aarch64", "-L", sysroot] + prog
    r = run(prog)
    want = open(os.path.join(d, "expected.txt"), "rb").read()
    if r.returncode != 0:
        sys.exit("seed %d: exit code %d (stderr %r)" %
                 (seed, r.returncode, r.stderr[:300]))
    if r.stdout != want:
        gl, wl = r.stdout.split(b"\n"), want.split(b"\n")
        for i in range(max(len(gl), len(wl))):
            g = gl[i] if i < len(gl) else None
            w = wl[i] if i < len(wl) else None
            if g != w:
                # the C function of this line says which signature it is
                sys.exit("seed %d (%s): line %d differs\n got:  %r\n want: %r"
                         % (seed, d, i + 1, g, w))
