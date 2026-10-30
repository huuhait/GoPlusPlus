#!/usr/bin/env python3
"""Checks the fast runner against what each program says it must do, and
against a real binary (P04.19, D4.14).

usage: runner-check.py --work DIR PROGRAM_DIR (--names a,b,... | --sequence a,b,...)
           [--merge] [--expect-starts N] [-- extra gopp test-run options]

Every NAME.gopp in PROGRAM_DIR has a line `// EXPECT: STATUS` (`exit N`,
`signal N`, `timeout`, `compile-failed`, `load-failed`). A program that must
hang has `// TIMEOUT: MS`, its own short time limit in the manifest; every
other program keeps the default of 60 s, so a loaded machine cannot fail a
program that has to exit (R4). The programs run in
ONE `gopp test-run`; each status must be the expected one. A program with an
exit or signal status and no `// REAL: no` line is also built and run as a
real binary (`gopp build` + exec, in the same working directory), and the
status, standard output and standard error must be the same bytes (with
--merge both streams go to one file, `-merge-output` and `2>&1`, so the order
of the writes counts too).

--sequence runs the programs in the order given (a name may come twice) on
ONE executor, one at a time (`-j 1 -executors 1`): a program that changes its
process is followed by one that looks at that state in the same executor
(manager decision M2). --expect-starts N requires exactly N executor
processes (the summary of `gopp test-run -summary`).
"""
import concurrent.futures
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

argv = sys.argv[1:]
extra = []
if "--" in argv:
    k = argv.index("--")
    argv, extra = argv[:k], argv[k + 1:]
work = prog_dir = None
names = None
sequence = False
merge = False
want_starts = None
i = 0
while i < len(argv):
    a = argv[i]
    if a == "--work":
        work = argv[i + 1]
        i += 2
    elif a in ("--names", "--sequence"):
        names = argv[i + 1].split(",")
        sequence = a == "--sequence"
        i += 2
    elif a == "--merge":
        merge = True
        i += 1
    elif a == "--expect-starts":
        want_starts = int(argv[i + 1])
        i += 2
    else:
        prog_dir = a
        i += 1
if not work or not prog_dir or not names:
    sys.exit(__doc__)
os.makedirs(work, exist_ok=True)

expect, real, limit = {}, {}, {}
for n in set(names):
    text = open(os.path.join(prog_dir, n + ".gopp")).read()
    m = re.search(r"^// EXPECT: (.+)$", text, re.M)
    if not m:
        sys.exit("%s has no EXPECT line" % n)
    expect[n] = m.group(1).strip()
    t = re.search(r"^// TIMEOUT: (\d+)$", text, re.M)
    limit[n] = int(t.group(1)) if t else 0
    if limit[n] and expect[n] != "timeout":
        sys.exit("%s has a TIMEOUT line but is not expected to time out" % n)
    real[n] = "// REAL: no" not in text and expect[n].split()[0] in ("exit", "signal")

ids = ["%d-%s" % (k, n) for k, n in enumerate(names)]
manifest = os.path.join(work, "manifest.jsonl")
with open(manifest, "w") as f:
    for i, n in zip(ids, names):
        f.write(json.dumps({"v": runlib.PROTOCOL_VERSION, "id": i,
                            "inputs": [os.path.join(prog_dir, n + ".gopp")],
                            "merge_output": merge, "timeout_ms": limit[n]}) + "\n")
opts = ["-j", "1", "-executors", "1"] if sequence else []
p = subprocess.run([runlib.gopp(), "test-run", "-summary", "-manifest", manifest]
                   + opts + extra, stdin=subprocess.DEVNULL,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=work,
                   timeout=60)
err = p.stderr.decode(errors="replace")
if p.returncode != 0:
    sys.exit("gopp test-run failed: " + err[:600])
res = runlib.decode_results(p.stdout.decode())


def status(r):
    if r.outcome == "exited":
        return "exit %d" % r.code
    if r.outcome == "signaled":
        return "signal %d" % r.signal
    return r.outcome


fails = []
if want_starts is not None:
    m = re.search(r"(\d+) executor processes started", err)
    if not m or int(m.group(1)) != want_starts:
        fails.append("executor processes started: %s, expected %d (%s)"
                     % (m.group(1) if m else "?", want_starts, err.strip()))
for i, n in zip(ids, names):
    got = status(res[i])
    if got != expect[n]:
        fails.append("%s: status '%s', expected '%s' (%s)"
                     % (i, got, expect[n], res[i].message.strip()[:300]))


def real_run(n):
    return n, runlib._run_real([os.path.join(prog_dir, n + ".gopp")],
                               os.environ.get("GOPP_LINK_FLAGS", "").split(),
                               60, merge)


# The real binaries are built and run side by side: it is the slow part. They
# run in the directory the runner ran in (a program may print it).
cwd = os.getcwd()
os.chdir(work)
with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
    reals = dict(ex.map(real_run, sorted({n for n in names if real[n]})))
os.chdir(cwd)
for i, n in zip(ids, names):
    if not real[n]:
        continue
    f, r = res[i], reals[n]
    if r.failure():
        fails.append("%s: the real binary: %s" % (n, r.failure()))
    elif (f.outcome, f.returncode) != (r.outcome, r.returncode):
        fails.append("%s: binary %s, runner %s" % (i, status(r), status(f)))
    elif f.stdout != r.stdout:
        fails.append("%s: stdout differs from the binary's:\n runner %r\n binary %r"
                     % (i, f.stdout[:200], r.stdout[:200]))
    elif f.stderr != r.stderr:
        fails.append("%s: stderr differs from the binary's:\n runner %r\n binary %r"
                     % (i, f.stderr[:200], r.stderr[:200]))
if fails:
    sys.exit("\n".join(fails))
