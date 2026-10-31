#!/usr/bin/env python3
"""Exact diagnostics: every error and warning a test's gopp run reports is
matched by one of its CHECK lines, and every CHECK line for an error or a
warning matches one reported diagnostic.

FileCheck's --implicit-check-not=error[E does not see a diagnostic that falls
between two CHECK-DAG matches, so a test written with CHECK-DAG lines could
pass while a "positive" function reports an error nobody expected. This script
closes that gap for every lit test under the given root that pipes a gopp run
into FileCheck with --implicit-check-not=error[E (found by scanning, so a new
test is covered without a new RUN line). It runs each such gopp command again
with no error limit and compares the output with the CHECK lines of the
FileCheck prefix that RUN line uses.

Usage: check-exact-errors.py --gopp PATH --examples DIR --root DIR [--shard K/N] [FILE...]
With FILEs, only those tests are checked. With --shard K/N only the tests at
places K, K+N, K+2N ... of the sorted list (K from 0); the N shards together
are all the tests (exact-errors.test, -2 and -3 are the three shards).
"""

import argparse
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys

# A diagnostic line (with or without a position); not a source snippet line.
DIAG = re.compile(r"\b(?:error|warning)\[E\d+\]:")
SNIPPET = re.compile(r"^\s*(?:\d+\s*)?\|")
RUN = re.compile(r"^\s*//\s*RUN:\s?(.*)$")


def pattern_regex(pat, line_no):
    """A FileCheck pattern (literal text, {{regex}}, [[@LINE+N]]) as a regex."""
    out, i = [], 0
    while i < len(pat):
        if pat.startswith("{{", i):
            j = pat.index("}}", i + 2)
            out.append("(?:" + pat[i + 2:j] + ")")
            i = j + 2
        elif pat.startswith("[[", i):
            j = pat.index("]]", i + 2)
            body = pat[i + 2:j]
            m = re.fullmatch(r"@LINE(?:([+-])(\d+))?", body)
            if m:
                n = line_no
                if m.group(1):
                    n += int(m.group(2)) * (1 if m.group(1) == "+" else -1)
                out.append(str(n))
            else:
                out.append(".*?")  # a [[VAR]] use or [[VAR:re]] definition
            i = j + 2
        else:
            j = len(pat)
            for tok in ("{{", "[["):
                k = pat.find(tok, i)
                if k != -1:
                    j = min(j, k)
            out.append(re.escape(pat[i:j]))
            i = j
    return re.compile("".join(out))


def check_lines(path, prefixes):
    """(line number, regex, text) of the CHECK lines for diagnostics."""
    res = []
    alt = "|".join(re.escape(p) for p in prefixes)
    head = re.compile(r"(?:^|[^A-Za-z0-9_-])(" + alt + r")(?:-DAG|-SAME|-NEXT)?:\s?(.*)$")
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            m = head.search(line)
            if not m:
                continue
            pat = m.group(2).rstrip()
            # Only lines that check an error or warning (`error[`,
            # `error{{\[E...`); notes and the rest are FileCheck's job.
            if not re.search(r"(?:error|warning)(?:\[|\{\{)", pat):
                continue
            res.append((n, pattern_regex(pat, n), pat))
    return res


def subst(cmd, src, args):
    d = os.path.dirname(src)
    return (cmd.replace("%gopp", args.gopp).replace("%examples", args.examples)
            .replace("%S", d).replace("%s", src))


def runs_of(path, args):
    """(gopp argv, check file, prefixes, None) for each RUN that FileChecks
    with --implicit-check-not=error[E, or (None, None, None, why) for one this
    script cannot follow."""
    lines, cur = [], ""
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RUN.match(line)
            if not m:
                continue
            cur += m.group(1)
            if cur.endswith("\\"):
                cur = cur[:-1]
                continue
            lines.append(cur)
            cur = ""
    writers = {}  # %t.x -> producing command
    out = []
    for cmd in lines:
        w = re.match(r"^(.*%gopp\b.*?)\s*>\s*(%t\S*)\s*2>&1\s*$", cmd)
        if w:
            writers[w.group(2)] = w.group(1)
        if "FileCheck" not in cmd or not re.search(r"implicit-check-not[= ]\"?'?error\[", cmd):
            continue
        prod = None
        if "|" in cmd and "%gopp" in cmd.split("|")[0]:
            prod = cmd.split("|")[0]
        else:
            r = re.search(r"<\s*(%t\S*)", cmd)
            if r and r.group(1) in writers:
                prod = writers[r.group(1)]
        if prod is None:
            out.append((None, None, None, "cannot find the gopp command that feeds this FileCheck: " + cmd))
            continue
        fc = shlex.split(subst(cmd.split("|")[-1] if "|" in cmd else cmd, path, args))
        fc = fc[fc.index("FileCheck") + 1:]
        prefixes, files = [], []
        for a in fc:
            if a.startswith("--check-prefix=") or a.startswith("-check-prefix="):
                prefixes.append(a.split("=", 1)[1])
            elif a.startswith("--check-prefixes="):
                prefixes += a.split("=", 1)[1].split(",")
            elif a == "<" or a.startswith("-") or a.startswith("<"):
                continue
            else:
                files.append(a)
        files = [f for f in files if not f.startswith("%t")]
        if not files:
            out.append((None, None, None, "cannot find the check file of: " + cmd))
            continue
        argv = shlex.split(subst(prod.replace("2>&1", ""), path, args))
        if argv and argv[0] == "not":
            argv = argv[1:]
        # Report everything: no error limit.
        if len(argv) > 1 and argv[1] in ("check", "vet"):
            argv = [a for a in argv if not a.startswith("--check-error-limit")]
            argv.insert(2, "--check-error-limit=0")
        elif len(argv) > 1 and argv[1] == "parse":
            argv = [a for a in argv if not a.startswith("-error-limit")]
            argv.insert(2, "-error-limit=0")
        out.append((argv, files[-1], prefixes or ["CHECK"], None))
    return out


def check_one(path, args):
    problems = []
    for argv, cfile, prefixes, bad in runs_of(path, args):
        if bad:
            # Not understood is not checked: say so instead of passing.
            problems.append(f"{path}: {bad}")
            continue
        p = subprocess.run(argv, capture_output=True, text=True)
        diags = [l for l in (p.stdout + p.stderr).splitlines()
                 if DIAG.search(l) and not SNIPPET.match(l)]
        checks = check_lines(cfile, prefixes)
        used = [False] * len(checks)
        for d in diags:
            for k, (n, rx, _) in enumerate(checks):
                if not used[k] and rx.search(d):
                    used[k] = True
                    break
            else:
                problems.append(f"{path}: unexpected diagnostic (no {'/'.join(prefixes)} line matches it): {d}")
        for k, (n, _, pat) in enumerate(checks):
            if not used[k]:
                problems.append(f"{cfile}:{n}: no reported diagnostic matches: {pat}")
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--examples", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--shard", default="0/1")
    ap.add_argument("files", nargs="*")
    args = ap.parse_args()
    shard, shards = (int(x) for x in args.shard.split("/"))
    if not 0 <= shard < shards:
        ap.error("--shard K/N needs 0 <= K < N")
    args.root = os.path.normpath(args.root)
    files = args.files
    if not files:
        for d, subs, names in os.walk(args.root):
            subs[:] = sorted(s for s in subs if s not in ("Inputs", "unit"))
            files += [os.path.join(d, n) for n in sorted(names) if n.endswith(".test")]
    files = files[shard::shards]
    # A few workers: lit runs the other tests at the same time, and one gopp
    # per core here would slow them all down (check-suite-time).
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
        results = list(ex.map(lambda f: check_one(f, args), files))
    problems = [p for r in results for p in r]
    for p in problems:
        print(p)
    if problems:
        sys.exit(1)


if __name__ == "__main__":
    main()
