#!/usr/bin/env python3
"""Runs the long-chain cases of gen-chain-stress.py (P03.26) in both modes.

usage: check-chains.py <gopp> <work dir> <suite>

The suites `forms-go` and `forms-gopp` run every form at the depth the parser
allows (about 1000 levels), one function each, in Go mode (one program per
form) and in Go++ mode (all forms in one program): no
error, no crash, under a time limit. The suite `many-<case>,<case>` runs deep forms in 150 functions each,
which a check that costs more than a constant per link (or per link and
chain length) would not finish. The suite `bad` checks the error side: one
error per function with the right text (the parser's own limit for the
forms it counts is pinned by selector-chain-limit.test; a selector on a
selector is not counted by `gopp check`, ParseOptions::UncountedSelectors).
"""
import os
import subprocess
import sys

gopp, work, suite = sys.argv[1], sys.argv[2], sys.argv[3]
here = os.path.dirname(os.path.abspath(__file__))
gen = os.path.join(here, "gen-chain-stress.py")
stubs = os.path.join(here, "chainstub")
os.makedirs(work, exist_ok=True)
failed = []


def run(case, depth, count, mode, expect, limit):
    # case "a:N,b:M,...": every case in one program, one function each.
    many = ":" in case
    name = "forms" if many else "%s-%d-%d" % (case, depth, count)
    path = os.path.join(work, "%s.%s" % (name, "go" if mode == "go" else "gopp"))
    with open(path, "w") as f:
        args = [case] if many else [case, str(depth), str(count)]
        subprocess.run([sys.executable, gen] + args, stdout=f, check=True)
    cmd = ["timeout", str(limit), gopp, "check", "--check-error-limit=0", "-check-j=1",
           "--test-stubs=" + stubs]
    if mode == "go":
        cmd.append("--go")
    r = subprocess.run(cmd + [path], capture_output=True, text=True)
    out = r.stdout + r.stderr
    errs = [l for l in out.splitlines() if "error[" in l]
    tag = "%s %s" % (mode, case) if many else "%s %s depth=%d count=%d" % (mode, case, depth, count)
    if r.returncode in (124, -9) or r.returncode < 0 or "Sanitizer" in out:
        failed.append(tag + ": crashed or timed out (%d)\n%s" % (r.returncode, out[:600]))
    elif expect is None:
        if errs:
            failed.append(tag + ": unexpected errors\n" + "\n".join(errs[:3])[:600])
    else:
        n, text = expect
        if n is None:  # the first error says it, whatever follows
            bad = not errs or text not in errs[0]
        else:
            bad = len(errs) != n or not all(text in l for l in errs)
        if bad:
            failed.append(tag + ": expected %s errors with %r, got %d\n%s" % ("some" if n is None else n, text, len(errs), "\n".join(errs[:3])[:600]))


OK_FORMS = [  # case, depth
    ("sel", 1000), ("call", 1000), ("idx", 1000), ("slice", 1000),
    ("assert", 1000), ("star", 480), ("paren", 480), ("mix", 1000),
    ("mixparen", 1000), ("bin", 1000), ("unary", 480), ("elif", 900),
    ("nestcall", 990), ("nestlit", 490), ("nestif", 320), ("nestfunc", 250),
    ("addr", 1000), ("assign", 1000), ("method", 600), ("defer", 1000),
]
if suite == "forms-gopp":
    # Every form in one program (one gopp run).
    run(",".join("%s:%d" % f for f in OK_FORMS + [("qual", 1000)]), 0, 0, "gopp", None, 20)
elif suite == "forms-go":
    for case, depth in OK_FORMS:
        run(case, depth, 1, "go", None, 20)
elif suite.startswith("many-"):
    for case in suite[5:].split(","):
        run(case, 600 if case == "method" else 1000, 60 if case == "qual" else 150, "gopp", None, 20)
elif suite == "base":
    # A chain over a base that is no name: each prefix is asked about once
    # (qualifierPackage), so the check is linear. 100000 links of the
    # parenthesized base (9 s when quadratic); the other forms are small, and one in Go mode.
    run("selpar", 100000, 1, "gopp", None, 4)
    for case in ("selcall", "selidx", "selassert", "selstar"):
        run(case, 3000, 1, "gopp", None, 5)
    run("selpar", 3000, 1, "go", None, 5)
    run("selparerr", 3000, 1, "gopp", (1, "undefined (type *S has no field or method w)"), 5)
elif suite == "base-all":
    for case in ("selpar", "selcall", "selidx", "selassert", "selstar"):
        run(case, 100000, 1, "gopp", None, 5)
        run(case, 100000, 1, "go", None, 5)
    run("selparerr", 100000, 1, "gopp", (1, "undefined (type *S has no field or method w)"), 5)
elif suite == "bad":
    run("err", 1000, 2, "gopp", (2, "undefined (type *S has no field or method w)"), 20)
    run("badbase", 1000, 2, "gopp", (2, "undefined: nope"), 20)
    # (The parser's own limit, E0031, for the forms it counts:
    # selector-chain-limit.test.)
else:
    sys.exit("unknown suite " + suite)
if failed:
    print("\n".join(failed))
    sys.exit(1)
