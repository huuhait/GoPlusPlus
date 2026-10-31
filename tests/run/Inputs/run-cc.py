#!/usr/bin/env python3
"""Build and run generated Go++ to Go++ call programs (P04.18, gen-cc.py).

usage: run-cc.py [--arm64 SYSROOT] [--opts 0,1,2] [--flags "GOPP FLAGS"]
                 [--slow N] GOPP DIR SEED COUNT [SEED COUNT]...

For every SEED/COUNT pair gen-cc.py writes main.gopp and expected.txt under
DIR/seedSEED; gopp builds the program at every `opt` level of --opts (default
0,2: the baseline of the pipelines and a full LLVM pipeline) and the program
runs (under qemu-aarch64 -L SYSROOT for arm64); its standard output must equal
expected.txt, which the generator made and Go agreed with (check-cc-go.py):
the same text at every level. Every step is limited to 60 seconds.

--slow N (lit's %{slow}: 5 in a sanitizer build) makes one in N of the
builds: the K-th seed at the levels whose index I has (I + K) % N == 0 (at
one level when none has), so every seed runs and, over N seeds or more,
every level.
"""
import os
import subprocess
import sys

args = sys.argv[1:]
sysroot = None
flags = []
opts = ["0", "2"]
slow = 1
while args and args[0] in ("--arm64", "--flags", "--opts", "--slow"):
    if args[0] == "--arm64":
        sysroot = args[1]
    elif args[0] == "--opts":
        opts = args[1].split(",")
    elif args[0] == "--slow":
        slow = max(1, int(args[1]))
    else:
        flags = args[1].split()
    args = args[2:]
gopp, workdir = args[0], args[1]
pairs = [(int(args[i]), int(args[i + 1])) for i in range(2, len(args), 2)]
here = os.path.dirname(os.path.abspath(__file__))


def run(argv):
    return subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=60)


for k, (seed, count) in enumerate(pairs):
    d = os.path.join(workdir, "seed%d" % seed)
    os.makedirs(d, exist_ok=True)
    r = run([sys.executable, os.path.join(here, "gen-cc.py"), "--seed", str(seed),
             "--count", str(count), "--dir", d])
    if r.returncode:
        sys.exit("gen-cc.py failed: " + r.stderr.decode())
    want = open(os.path.join(d, "expected.txt"), "rb").read()
    picked = [o for i, o in enumerate(opts) if (i + k) % slow == 0]
    for o in picked or [opts[k % len(opts)]]:
        prog = os.path.join(d, "prog" + o)
        build = [gopp, "build"] + flags + ["-opt=" + o, os.path.join(d, "main.gopp"),
                                           "-o", prog]
        if sysroot:
            build[2:2] = ["-target=linux/arm64", "-crt-dir=" + sysroot + "/lib"]
        r = run(build)
        if r.returncode:
            sys.exit("gopp build failed for seed %d opt %s (%s):\n%s" %
                     (seed, o, d, r.stderr.decode()[:3000]))
        cmd = ["qemu-aarch64", "-L", sysroot, prog] if sysroot else [prog]
        r = run(cmd)
        if r.returncode != 0:
            sys.exit("seed %d opt %s: exit code %d (stderr %r)" %
                     (seed, o, r.returncode, r.stderr[:300]))
        if r.stdout != want:
            gl, wl = r.stdout.split(b"\n"), want.split(b"\n")
            for i in range(max(len(gl), len(wl))):
                g = gl[i] if i < len(gl) else None
                w = wl[i] if i < len(wl) else None
                if g != w:
                    sys.exit("seed %d opt %s (%s): line %d differs\n got:  %r\n want: %r"
                             % (seed, o, d, i + 1, g, w))
