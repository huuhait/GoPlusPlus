#!/usr/bin/env python3
"""Scenarios of the fast runner that need more than "run and compare"
(P04.19, D4.14).

usage: runner-scenarios.py SCENARIO --work DIR --programs DIR [--names a,b,...]

start-fail     (--only missing,ends,hangs: some of the three) an executor that does not start (no such program, one that
               ends at once, one that never says it is ready): every program
               gets the same `internal` result, `gopp test-run` exits 1, and
               the run ends in bounded time
link-error     a program that does not link is a result (the missing symbols,
               sorted); the executor lives on (one process for the run)
compile-error  a program that does not compile is a result; the others run
fds            descriptors above the executor's own (900 and up) are closed after
               every program: three programs that each open as many files as
               they can, in ONE executor (sharing forced), all open the same
               number (4 fewer than a binary: the executor holds 4)
batch-workdir  (--programs is the record file of Inputs/batch-suite/record-args.py,
               --work the lit output directory) the work directory the lit batch
               gave gopp existed under the output directory and is gone now
cleanup        (--signal TERM|INT [--rounds N]) `gopp test-run` stopped by that signal removes its work
               directory and ends with 128 + the signal (SIGKILL cannot)
determinism    (--configs K,...: only the first configuration and these) the results (outcome, status, both streams, message, restart)
               are the same bytes for any -j, any number of executors and any
               order of the manifest
"""
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

scenario = sys.argv[1]
opts = dict(zip(sys.argv[2::2], sys.argv[3::2]))
work, progs = opts["--work"], opts["--programs"]
os.makedirs(work, exist_ok=True)


def run(label, entries, extra, expect_rc=0):
    """entries: [(id, program name)]. -> ({id: Result}, stderr)"""
    d = os.path.join(work, label)
    os.makedirs(d, exist_ok=True)
    manifest = os.path.join(d, "manifest.jsonl")
    with open(manifest, "w") as f:
        for i, n in entries:
            f.write(json.dumps({"v": runlib.PROTOCOL_VERSION, "id": i,
                                "inputs": [os.path.join(progs, n + ".gopp")]}) + "\n")
    r = subprocess.run([runlib.gopp(), "test-run", "-summary", "-manifest", manifest]
                       + extra, stdin=subprocess.DEVNULL, capture_output=True,
                       timeout=60, cwd=work)
    err = r.stderr.decode(errors="replace")
    if r.returncode != expect_rc:
        sys.exit("%s: test-run exit status %d, expected %d: %s"
                 % (label, r.returncode, expect_rc, err[-400:]))
    res = runlib.decode_results(r.stdout.decode())
    if sorted(res) != sorted(i for i, _ in entries):
        sys.exit("%s: results for %r, expected %r" % (label, sorted(res), entries))
    return res, err


def starts(err):
    return int(re.search(r"(\d+) executor processes started", err).group(1))


def check(cond, msg):
    if not cond:
        sys.exit(msg)


def status(r):
    if r.outcome == "exited":
        return "exit %d" % r.code
    if r.outcome == "signaled":
        return "signal %d" % r.signal
    return r.outcome


if scenario == "start-fail":
    sleeper = os.path.join(work, "never-ready.sh")
    with open(sleeper, "w") as f:
        f.write("#!/bin/sh\nexec sleep 30\n")
    os.chmod(sleeper, 0o755)
    entries = [("p%d" % i, "quiet") for i in range(6)]
    # Only the executor that never says it is ready gets a short start-up
    # limit; the others end at once and keep the default (R4: a loaded
    # machine must not turn "ended before it was ready" into "not ready").
    only = opts["--only"].split(",") if "--only" in opts else None
    for label, exe, why, startup in (
            ("missing", os.path.join(work, "no-such-program"), "cannot start", "20000"),
            ("ends", shutil.which("false"), "ended before it was ready", "20000"),
            ("hangs", sleeper, "was not ready after 100 ms", "100")):
        if only and label not in only:
            continue
        t0 = time.time()
        res, err = run(label, entries,
                       ["-executor", exe, "-startup-timeout-ms", startup, "-j", "3"],
                       expect_rc=1)
        took = time.time() - t0
        check(took < 10, "%s: took %.1f s" % (label, took))
        texts = {(r.outcome, r.message) for r in res.values()}
        check(len(texts) == 1, "%s: the programs got different results: %r" % (label, texts))
        outcome, text = texts.pop()
        check(outcome == "internal" and text.startswith("the executor does not start: ")
              and why in text, "%s: unexpected result %r %r" % (label, outcome, text))
        check(text in err, "%s: the error is not on standard error: %r" % (label, err))

elif scenario == "link-error":
    entries = [("a", "counter"), ("b", "nolink"), ("c", "counter"), ("d", "nolink3"),
               ("e", "counter")]
    res, err = run("link", entries, ["-j", "1", "-executors", "1"])
    got = [status(res[i]) for i, _ in entries]
    check(got == ["exit 0", "load-failed", "exit 0", "load-failed", "exit 0"],
          "results %r" % got)
    check(res["b"].message == "undefined symbols: no_such_function_xyz",
          "message %r" % res["b"].message)
    check(res["d"].message == "undefined symbols: aa_missing_a mm_missing_c zz_missing_b",
          "message %r" % res["d"].message)
    check(all(res[i].stdout == b"counter 42\n" for i in "ace"),
          "a program after a link error saw the state of another")
    check(starts(err) == 1, "a link error must not cost an executor: %d starts" % starts(err))

elif scenario == "compile-error":
    entries = [("a", "counter"), ("b", "bad"), ("c", "quiet")]
    res, err = run("cerr", entries, ["-j", "2"])
    got = [status(res[i]) for i, _ in entries]
    check(got == ["exit 0", "compile-failed", "exit 0"], "results %r" % got)
    check("error[" in res["b"].message, "no diagnostic in %r" % res["b"].message)

elif scenario == "fds":
    import resource
    resource.setrlimit(resource.RLIMIT_NOFILE, (1024, 1024))
    entries = [("a", "fd-count"), ("b", "fd-count"), ("c", "fd-count")]
    res, err = run("fds", entries, ["-j", "1", "-executors", "1", "-share-executors"])
    check(starts(err) == 1, "the three programs must share one executor: %d starts" % starts(err))
    counts = {}
    for i, _ in entries:
        m = re.match(rb"opened (\d+) first 3 last (\d+)\n$", res[i].stdout)
        check(m, "%s: output %r" % (i, res[i].stdout))
        counts[i] = int(m.group(1))
    check(len(set(counts.values())) == 1, "descriptors opened differ: %r" % counts)
    real = runlib._run_real([os.path.join(progs, "fd-count.gopp")], [], 60)
    m = re.match(rb"opened (\d+) ", real.stdout)
    check(m and int(m.group(1)) == counts["a"] + 4,
          "binary %r, runner %r: expected the binary to open 4 more" % (real.stdout, counts))
    check(counts["a"] > 900, "the program must open more than 900: %r" % counts)

elif scenario == "batch-workdir":
    lines = [l for l in open(progs).read().splitlines() if l.startswith("work-dir: ")]
    check(len(lines) == 1, "the batch should name one work directory: %r" % lines)
    d = lines[0][len("work-dir: "):]
    check(os.path.abspath(d).startswith(os.path.abspath(work) + os.sep),
          "%s is not inside lit's output %s" % (d, work))
    check(not os.path.exists(d), "%s was not removed" % d)

elif scenario == "cleanup":
    import signal
    sig = getattr(signal, "SIG" + opts["--signal"])
    # --rounds N: N runners at once, all stopped when each has its program
    # running (the stop races with the runner's own threads: P04.19 R6).
    rounds = int(opts.get("--rounds", "1"))
    runners = []
    for k in range(rounds):
        tmp = os.path.join(work, "tmp-%d-%d" % (sig, k))
        os.makedirs(tmp, exist_ok=True)
        env = dict(os.environ, TMPDIR=tmp)
        p = subprocess.Popen([runlib.gopp(), "test-run", "-timeout-ms", "30000",
                              os.path.join(progs, "loop.gopp")], env=env,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        runners.append((tmp, p))
    for tmp, p in runners:
        # Wait for the work directory (made first thing) and for the program
        # to be running (its object is in the directory).
        t0 = time.time()
        while time.time() - t0 < 60:
            made = [os.path.join(tmp, n) for n in os.listdir(tmp)]
            if made and any(n.endswith(".o") for n in os.listdir(made[0])):
                break
            time.sleep(0.05)
        else:
            for _, q in runners:
                q.kill()
            sys.exit("%s: the runner made no object in 60 s" % sig.name)
        p.send_signal(sig)
    for tmp, p in runners:
        try:
            rc = p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            for _, q in runners:
                q.kill()
            sys.exit("%s: the runner did not end" % sig.name)
        check(rc == 128 + sig, "%s: exit status %d" % (sig.name, rc))
        left = [os.path.join(r, n) for r, ds, fs in os.walk(tmp) for n in ds + fs]
        check(not left, "%s left %r" % (sig.name, left))

elif scenario == "determinism":
    names = opts["--names"].split(",")
    entries = [(n, n) for n in names]
    configs = [["-j", "1", "-executors", "1"],
               ["-j", "8", "-executors", "4", "-schedule", "reverse"],
               ["-j", "3", "-executors", "2", "-schedule", "shuffle:7"],
               ["-j", "16", "-executors", "16", "-schedule", "shuffle:99"]]
    # --configs K,...: the first configuration and these (default: all), each
    # compared with the first; the tests that split the scenario take one each.
    picked = [0] + [int(k) for k in opts["--configs"].split(",")] if "--configs" in opts \
        else range(len(configs))
    seen = {}
    for k in picked:
        res, _ = run("det%d" % k, entries, configs[k])
        seen[k] = {i: (r.outcome, r.code, r.signal, r.stdout, r.stderr, r.message,
                       json.dumps(r.restarted))
                   for i, r in res.items()}
    for n in names:
        for k in picked:
            check(seen[k][n] == seen[0][n], "%s differs between %r and %r:\n %r\n %r"
                  % (n, configs[0], configs[k], seen[0][n], seen[k][n]))
else:
    sys.exit("unknown scenario " + scenario)
