#!/usr/bin/env python3
"""Groups the crash files of a fuzz run by cause (P02.10).

usage: tests/fuzz/triage.py [--minimize] [--save NAME_PREFIX] [CRASH_DIR]

CRASH_DIR defaults to build-fuzz/fuzz-work. Every crash-*, timeout-*, oom-*
and leak-* file is run through gopp-fuzz-parse (symbolized, so run it inside
`nix develop -c`). The signature of a crash is the failed check or the
sanitizer error plus its top three frames in our code; files with the same
signature are one bug.

  --minimize        shrink one file per signature with libFuzzer's
                    -minimize_crash (up to 60 s each)
  --save PREFIX     copy the (minimized) file of each signature to
                    tests/fuzz/regressions/PREFIX-N.gopp; rename them to say
                    what they test, fix the bug, and the unit test
                    FuzzCorpus.RegressionsPassTheChecks keeps it fixed
"""
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.environ.get("BUILD_DIR", os.path.join(ROOT, "build-fuzz"))
FUZZER = os.path.join(BUILD, "bin", "gopp-fuzz-parse")


def signature(path):
    p = subprocess.run([FUZZER, "-timeout=10", "-rss_limit_mb=2048", path],
                       capture_output=True, text=True)
    err = p.stderr
    m = re.search(r"fuzz check failed: (.*)", err)
    if m:
        return "check: " + m.group(1).strip()
    m = re.search(r"ERROR: (\w+Sanitizer): ([^\n]*?) on", err)
    kind = m.group(2) if m else None
    m2 = re.search(r"runtime error: ([^\n]*)", err)
    if m2:
        kind = "UBSan: " + re.sub(r"0x[0-9a-f]+", "ADDR", m2.group(1))
    frames = re.findall(r"#\d+ 0x[0-9a-f]+ in (?:\S+ )*?(gopp::[^\s(]+|\S+)\(?[^\n]* /[^\n]*?/(?:compiler|tests)/([^\s:]+:\d+)", err)
    top = ["%s %s" % (f, loc) for f, loc in frames[:3]]
    if "ERROR: libFuzzer: timeout" in err:
        kind = "timeout"
    if "out-of-memory" in err or "oom" in err.lower() and "rss" in err.lower():
        kind = kind or "out of memory"
    if kind is None:
        kind = "unknown (exit %d)" % p.returncode
    return kind + " @ " + " <- ".join(top)


def main():
    args = sys.argv[1:]
    minimize = False
    prefix = None
    while args and args[0].startswith("--"):
        a = args.pop(0)
        if a == "--minimize":
            minimize = True
        elif a == "--save":
            prefix = args.pop(0)
        else:
            sys.exit(__doc__)
    work = args[0] if args else os.path.join(BUILD, "fuzz-work")
    files = []
    for pat in ("crash-*", "timeout-*", "oom-*", "leak-*"):
        files += glob.glob(os.path.join(work, pat))
    if not files:
        print("no crash files in", work)
        return 0
    groups = {}
    for f in sorted(files):
        groups.setdefault(signature(f), []).append(f)
    for n, (sig, fs) in enumerate(sorted(groups.items()), 1):
        best = min(fs, key=os.path.getsize)
        if minimize:
            out = best + ".min"
            subprocess.run([FUZZER, "-minimize_crash=1", "-runs=20000",
                            "-max_total_time=60", "-exact_artifact_path=" + out, best],
                           capture_output=True)
            if os.path.exists(out):
                best = out
        print("[%d] %s" % (n, sig))
        print("    %d file(s); smallest: %s (%d bytes)" % (len(fs), best, os.path.getsize(best)))
        if prefix:
            dest = os.path.join(ROOT, "tests", "fuzz", "regressions", "%s-%d.gopp" % (prefix, n))
            shutil.copy(best, dest)
            print("    saved as", os.path.relpath(dest, ROOT))
    print("%d file(s), %d distinct cause(s)" % (len(files), len(groups)))
    return 1


sys.exit(main())
