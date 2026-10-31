#!/usr/bin/env python3
"""The size checks of the long suite: the program of a generator at N checks
(or builds) without a crash, and its cost grows near-linearly up to N.

usage: check-linear.py --out DIR --n N --gen "GEN ARGS" [--scale S] [--power P]
                       [--status K] [--output FILE] [--timeout T]
                       [--label TEXT] -- CMD...
       check-linear.py --control PROG --n N [--scale S] [--power P] [--label TEXT]

GEN ARGS is a generator and its arguments with {n} for the size (run with
this Python, its output is the program); CMD is the command, with {file} for
the program. The command runs on the program of N/S (small) and of N (big),
and on the program of 1 (the base: process start and the fixed work). The
cost of a run is its CPU time (user + system of the child; other processes
on a busy machine do not add to it), less the least of three base runs. The
check fails if big costs more than 3 * S**P times small, P the power the
shape's cost grows with by nature (--power, default 1). Linear is S,
quadratic S * S: with S = 16 (the default), 16 and 256, limit 48. With
--power 2, for a shape whose output itself grows quadratically, S = 8: 64,
cubic 512, limit 192. A round runs small twice and big once and keeps the
least cost of each side (a busy machine only adds time; a quadratic shape
passes only if both small runs cost S / 3 times too much); a pair over the
limit gets a round more, twice at most. Big runs side by side with the
small and base runs. A small cost under 0.02 s is too short to judge (a fast
build): both sizes double, four times at most (then an error, not a pass),
and N gets a run of its own. Each run must exit with status K (default 0);
--output keeps what the run at N printed (stdout, then stderr) for
FileCheck. --timeout is only a guard against a hang (default 240 s a
run), never the verdict.

--control PROG runs PROG N (a program quadratic in N, Inputs/quadratic.py,
or cubic with --power 2, Inputs/cubic.py) the same way, in one round (a
second would only look for a pass): the verdict must be NO
(tests/sema/linear-control-2.long.test).

Exit status 0 for "near-linear yes", 1 for NO or a failed run.
"""
import argparse
import os
import shlex
import subprocess
import sys
import tempfile
import threading

MIN_SMALL = 0.02  # seconds of CPU over the base: below this, noise


def main():
    argv = sys.argv[1:]
    cmd = []
    if "--" in argv:
        i = argv.index("--")
        argv, cmd = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--gen")
    ap.add_argument("--scale", type=int, default=16)
    ap.add_argument("--power", type=int, default=1)
    ap.add_argument("--status", type=int, default=0)
    ap.add_argument("--output")
    ap.add_argument("--timeout", type=float, default=240)
    ap.add_argument("--label", default="")
    ap.add_argument("--control")
    a = ap.parse_args(argv)
    if not a.control and not (a.out and a.gen and cmd):
        ap.error("--out, --gen and the command after -- are needed")
    sizes = {"base": 1, "small": max(1, a.n // a.scale), "big": a.n, "size": a.n}
    keep = ["big"]  # the run whose status and output are the size check's
    label = a.label or "check"

    made = set()

    def command(which):
        n = sizes[which]
        if a.control:
            return [sys.executable, a.control, str(n)]
        os.makedirs(a.out, exist_ok=True)
        path = os.path.join(a.out, "%s.gopp" % which)
        if (which, n) not in made:  # made anew by every run of the script
            made.add((which, n))
            with open(path, "w") as f:
                subprocess.run([sys.executable] + shlex.split(a.gen.replace("{n}", str(n))),
                               stdout=f, check=True)
        return [c.replace("{file}", path) for c in cmd]

    def run(which):
        """The CPU seconds of one run (user + system of this child only:
        wait4), its exit status checked, its output kept if asked."""
        c = command(which)
        with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
            p = subprocess.Popen(c, stdin=subprocess.DEVNULL, stdout=out, stderr=err)
            hung = []
            guard = threading.Timer(a.timeout, lambda: (hung.append(1), p.kill()))
            guard.start()
            _, status, ru = os.wait4(p.pid, 0)
            guard.cancel()
            p.returncode = code = os.waitstatus_to_exitcode(status)
            if hung:
                fail("%s: the %s run (N=%d) did not end in %d s (hang guard): %s" %
                     (label, which, sizes[which], a.timeout, " ".join(c)))
            want = 0 if a.control else a.status
            if code != want and which != "base":
                err.seek(0)
                fail("%s%s: the %s run (N=%d) exited with %d, expected %d: %s" %
                     (err.read().decode(errors="replace")[:2000], label, which,
                      sizes[which], code, want, " ".join(c)))
            if which == keep[0] and a.output:
                out.seek(0)
                err.seek(0)
                with open(a.output, "wb") as f:
                    f.write(out.read() + err.read())
        return ru.ru_utime + ru.ru_stime

    failed = []

    def fail(why):
        failed.append(why)
        raise SystemExit(why)

    def in_background(which):
        """Runs `which` on a thread of its own: () -> its CPU seconds."""
        box = {}

        def body():
            try:
                box["t"] = run(which)
            except SystemExit:
                pass

        t = threading.Thread(target=body)
        t.start()

        def result():
            t.join()
            if failed:
                sys.exit(failed[0])
            return box["t"]
        return result

    limit = 3 * a.scale ** a.power
    # One round: small twice and big once, each side its least cost; big runs
    # side by side with the small and base runs (each cost is its own
    # process's CPU time). A busy machine only adds time: a quadratic shape
    # could pass only with a small cost S / 3 times too high, on both small
    # runs. A pair over the limit gets a round more (twice at most) for a
    # check; a control has one round (another would only look for a pass).
    def measure():
        small = big = 1e9
        for _ in range(1 if a.control else 3):
            big_run = in_background("big")
            small = min(small, run("small") - base, run("small") - base)
            big = min(big, big_run() - base)
            if small < MIN_SMALL or big <= limit * small:
                break
        return small, big

    base = min(run("base") for _ in range(3))
    for _ in range(5):
        command("big")  # made before any run starts
        small, big = measure()
        if small >= MIN_SMALL:
            break
        # Too short to judge (a fast build): both sides double. The size N is
        # still checked on its own run below, with its status and output.
        sizes["small"] *= 2
        sizes["big"] *= 2
        keep[0] = "size"
    else:
        sys.exit("%s: the small run (N=%d) costs %.3f s over the base: too short "
                 "to judge" % (label, sizes["small"], small))
    if keep[0] == "size":
        run("size")
    ok = big <= limit * small
    print("%s: near-linear %s (N=%d %.3fs -> N=%d %.3fs, %.1fx, limit %dx)" %
          (label, "yes" if ok else "NO", sizes["small"], small, sizes["big"], big,
           big / small, limit))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
