#!/usr/bin/env python3
"""Round-trip check of `gopp fmt` on a corpus (P02.09).

usage: check-corpus.py GOPP [--go] [--goroot] [--ext .test] [--skip NAME] [--jobs N] [--min N] [--shard K/N] PATH...

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
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gopp_batch  # noqa: E402
import procslots  # noqa: E402

POS = re.compile(r"<\d+:\d+, \d+:\d+>")
SPACE = re.compile(r"[\x00-\x20]+")
ESC_SPACE = re.compile(r"(\\[ntr]|\\x[01][0-9a-f]|\\0[0-3][0-7]|[\x00-\x20])+")  # the dump shows a newline as backslash-n


def run(args, data=None):
    with procslots.slot():
        return subprocess.run(args, input=data, capture_output=True)


def batch(gopp, args, paths):
    """{path: result} of `gopp ARGS -` on the bytes of each file, from
    `gopp ARGS -batch PATHS` (tests/gopp_batch.py)."""
    if not paths:
        return {}
    return gopp_batch.run([gopp, *args, "-batch", *paths], paths,
                          lambda p: run([gopp, *args, "-"], open(p, "rb").read()))


def dump(r):
    """The normalized syntax tree and the comments of one source, from one
    `gopp parse` run (the token dump comes first, the tree from its `File`
    line on); None if it does not parse."""
    if r.returncode != 0:
        return None
    text = r.stdout.decode("utf-8", "replace")
    at = text.find("\nFile ") + 1 if not text.startswith("File ") else 0
    return normalize(POS.sub("", text[at:])), comments(text[:at])


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


COMMENT = re.compile(r"\d+:\d+ comment (.*)$")


def comments(tokens):
    out = []
    for line in tokens.splitlines():
        m = COMMENT.match(line) if " comment " in line else None
        if m:
            text = m.group(1)
            if len(text) >= 2 and text[0] == "'" and text[-1] == "'":
                text = text[1:-1]
            out.append(ESC_SPACE.sub(" ", text).strip())
    return out


PARSE = ["parse", "-language-rules=false", "-dump-tokens", "-dump-comments", "-dump-ast"]


def check(gopp, paths):
    """[(status, why)] of each file of paths. Each step is one batch over the
    files that need it; the checks and their order are those of one file."""
    d0 = {p: dump(r) for p, r in batch(gopp, PARSE, paths).items()}
    live = [p for p in paths if d0[p] is not None]
    f1 = batch(gopp, ["fmt", "-no-sort-imports"], live)
    # Idempotence, with import sorting on.
    s1 = batch(gopp, ["fmt"], live)
    with tempfile.TemporaryDirectory() as tmp:
        again = {}  # source path -> (output of f1, output of s1) as files
        for i, p in enumerate(live):
            again[p] = (os.path.join(tmp, "%d.f1" % i), os.path.join(tmp, "%d.s1" % i))
            for f, r in zip(again[p], (f1[p], s1[p])):
                if r.returncode == 0:
                    with open(f, "wb") as out:
                        out.write(r.stdout)
        d1 = batch(gopp, PARSE, [again[p][0] for p in live if f1[p].returncode == 0])
        s2 = batch(gopp, ["fmt"], [again[p][1] for p in live if s1[p].returncode == 0])
    out = []
    for p in paths:
        f1p, s1p = again.get(p, (None, None))
        out.append(verdict(d0[p], f1.get(p), d1.get(f1p), s1.get(p), s2.get(s1p)))
    return out


def verdict(d0, f1, d1, s1, s2):
    """The checks of one file, in order: d0 the dump of the source, f1 `fmt
    -no-sort-imports` of it and d1 that output's parse; s1 `fmt` of the
    source and s2 `fmt` of s1's output. A step after one that failed is
    None."""
    if d0 is None:
        return "skip", ""
    if f1.returncode != 0:
        return "fail", "fmt failed: " + f1.stderr.decode("utf-8", "replace")[:300]
    d1 = dump(d1)
    if d1 is None:
        return "fail", "output does not parse"
    if d0[0] != d1[0]:
        return "fail", "syntax tree changed"
    if s1.returncode != 0 or s2.returncode != 0 or s2.stdout != s1.stdout:
        return "fail", "not idempotent"
    if d0[1] != d1[1]:
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
    shard = (0, 1)
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
        elif a == "--shard":
            k, n = args.pop(0).split("/")
            shard = (int(k), int(n))
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
    # --shard K/N: the files whose place in the sorted list is K modulo N.
    files = files[shard[0] :: shard[1]]
    counts = {"ok": 0, "skip": 0, "fail": 0}
    # Threads suit a small set; a large one (the standard library, also one
    # shard of it: about 920 files) needs processes, because the Python work
    # per file runs under one interpreter lock.
    pool = concurrent.futures.ProcessPoolExecutor if len(files) > 500 else concurrent.futures.ThreadPoolExecutor
    # The files go to gopp in batches (tests/gopp_batch.py).
    batches = gopp_batch.chunks(files, jobs)
    with pool(max_workers=jobs) as ex:
        for fs, res in zip(batches, ex.map(functools.partial(check, gopp), batches)):
            for f, (status, why) in zip(fs, res):
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
