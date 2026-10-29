#!/usr/bin/env python3
"""Round-trip check of `gopp fmt` on a corpus (P02.09).

usage: check-corpus.py GOPP [--go] [--goroot] [--ext .test] [--skip NAME] [--jobs N] [--min N] PATH...

For every file (a directory is searched for *.gopp, with --go also *.go, with
--ext also files with that suffix; --skip leaves a file name out) that
parses without errors:
  1. `gopp fmt` succeeds,
  2. parse(print(parse(src))) == parse(src): the syntax tree dumps are equal
     once source positions, empty statements, redundant parentheses and the
     the spelling of numbers and raw import paths are removed (imports are not
     sorted for this check),
  3. fmt is idempotent: fmt(fmt(src)) == fmt(src),
  4. no comment is lost: the comment tokens are the same, up to white space.
Files with syntax errors are counted and skipped. Prints a summary; exit status
1 if any check failed or fewer than --min files were checked.
"""
import concurrent.futures
import functools
import os
import re
import subprocess
import sys

POS = re.compile(r"<\d+:\d+, \d+:\d+>")
SPACE = re.compile(r"[\x00-\x20]+")
ESC_SPACE = re.compile(r"(\\[ntr]|\\x[01][0-9a-f]|\\0[0-3][0-7]|[\x00-\x20])+")  # the dump shows a newline as backslash-n


def run(args, data=None):
    return subprocess.run(args, input=data, capture_output=True)


def tree(gopp, extra, data):
    r = run([gopp, "parse", *extra, "-dump-ast", "-"], data)
    if r.returncode != 0:
        return None
    text = POS.sub("", r.stdout.decode("utf-8", "replace"))
    return normalize(text)


def normalize_lit(rest, is_import_path):
    """The literal rewrites of gofmt: 0X1 -> 0x1, 1E3 -> 1e3, 0765i -> 765i,
    and a raw string import path becomes a double-quoted one."""
    m = re.match(r"(BasicLit +)(int_lit|float_lit|imag_lit) (.*)$", rest)
    if m:
        value = m.group(3).lower()
        if m.group(2) == "imag_lit" and not re.search(r"[.e]|^0[xbo]", value):
            value = value.lstrip("0_") or "0i"
            if value == "i":
                value = "0i"
        return m.group(1) + m.group(2) + " " + value
    if not is_import_path:
        return rest
    m = re.match(r"(BasicLit +string_lit )`([^`\\\"]*)`$", rest)
    if m:
        return m.group(1) + '"' + m.group(2) + '"'
    return rest


def normalize(text):
    """Drops what gofmt drops: empty statements `;` in a list and redundant
    parentheses (a ParenExpr line goes, its children move up and take its role)."""
    out = []
    # (indent of a dropped ParenExpr, its role) for the open ones
    dropped = []
    # The scanner drops carriage returns of raw strings, the printer too.
    lines = [l for l in text.replace("\r", "").split("\n") if l.strip()]
    for n, line in enumerate(lines):
        indent = len(line) - len(line.lstrip(" "))
        body = line.lstrip(" ")
        while dropped and indent <= dropped[-1][0]:
            dropped.pop()
        role, _, rest = body.partition(": ")
        if rest.startswith("EmptyStmt"):
            continue
        if role == "Results" and rest.startswith("FieldList"):
            nxt = lines[n + 1] if n + 1 < len(lines) else ""
            if len(nxt) - len(nxt.lstrip(" ")) <= indent:
                continue  # `func f() ()`: the empty result list is dropped
        shift = 2 * len(dropped)
        if rest.startswith("ParenExpr"):
            # a parenthesized parenthesized expression: only the role of the
            # outermost one is kept
            direct = dropped and indent == dropped[-1][0] + 2
            dropped.append((indent, dropped[-1][1] if direct else role))
            continue
        if dropped and indent == dropped[-1][0] + 2:
            role = dropped[-1][1]
        if rest.startswith("BasicLit"):
            rest = normalize_lit(rest, role == "Path")
        out.append(" " * (indent - shift) + role + ": " + rest)
    return "\n".join(out)


def comments(gopp, extra, data):
    r = run([gopp, "parse", *extra, "-dump-tokens", "-dump-comments", "-"], data)
    out = []
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        m = re.match(r"\d+:\d+ comment (.*)$", line)
        if m:
            text = m.group(1)
            if len(text) >= 2 and text[0] == "'" and text[-1] == "'":
                text = text[1:-1]
            out.append(ESC_SPACE.sub(" ", text).strip())
    return out


def check(gopp, path):
    extra = ["-language-rules=false"]
    src = open(path, "rb").read()
    t0 = tree(gopp, extra, src)
    if t0 is None:
        return "skip", ""
    f1 = run([gopp, "fmt", "-no-sort-imports", "-"], src)
    if f1.returncode != 0:
        return "fail", "fmt failed: " + f1.stderr.decode("utf-8", "replace")[:300]
    t1 = tree(gopp, extra, f1.stdout)
    if t1 is None:
        return "fail", "output does not parse"
    if t0 != t1:
        return "fail", "syntax tree changed"
    # Idempotence, with import sorting on.
    s1 = run([gopp, "fmt", "-"], src)
    s2 = run([gopp, "fmt", "-"], s1.stdout)
    if s1.returncode != 0 or s2.returncode != 0 or s2.stdout != s1.stdout:
        return "fail", "not idempotent"
    if comments(gopp, extra, src) != comments(gopp, extra, f1.stdout):
        return "fail", "comments changed"
    return "ok", ""


def main():
    args = sys.argv[1:]
    gopp = args.pop(0)
    go = False
    exts = [".gopp"]
    skip = []
    jobs = os.cpu_count() or 4
    minimum = 1
    paths = []
    while args:
        a = args.pop(0)
        if a == "--go":
            go = True
        elif a == "--goroot":
            go = True
            root = run(["go", "env", "GOROOT"]).stdout.decode().strip()
            paths.append(os.path.join(root, "src"))
        elif a == "--ext":
            exts.append(args.pop(0))
        elif a == "--skip":
            skip.append(args.pop(0))
        elif a == "--jobs":
            jobs = int(args.pop(0))
        elif a == "--min":
            minimum = int(args.pop(0))
        else:
            paths.append(a)
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in sorted(names):
                    if n in skip:
                        continue
                    if n.endswith(tuple(exts)) or (go and n.endswith(".go")):
                        files.append(os.path.join(root, n))
        else:
            files.append(p)
    files.sort()
    counts = {"ok": 0, "skip": 0, "fail": 0}
    # Threads suit a small set; a large one (the standard library) needs processes,
    # because the Python work per file runs under one interpreter lock.
    pool = concurrent.futures.ProcessPoolExecutor if len(files) > 1000 else concurrent.futures.ThreadPoolExecutor
    with pool(max_workers=jobs) as ex:
        for f, (status, why) in zip(files, ex.map(functools.partial(check, gopp), files, chunksize=16)):
            counts[status] += 1
            if status == "fail":
                print("FAIL %s: %s" % (f, why))
    print(
        "checked %d files: %d ok, %d skipped (syntax errors), %d failed"
        % (len(files), counts["ok"], counts["skip"], counts["fail"])
    )
    bad = counts["fail"] > 0 or counts["ok"] < minimum
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
