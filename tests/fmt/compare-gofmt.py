#!/usr/bin/env python3
"""Compares `gopp fmt` with real gofmt on Go files (P02.09).

usage: compare-gofmt.py GOPP [--goroot] [--jobs N] [--list] [--allow-other N] [--shard K/N] PATH...

On valid Go the output should be byte-identical. The known differences are
sorted into classes:
  identical     same bytes
  doc-comments  only comment lines differ: go/doc/comment reformatting of doc
                comments and the //go:build fixes, which `gopp fmt` does not do
  fixed-point   gofmt is not idempotent on this file (gofmt of gofmt differs);
                `gopp fmt` prints until the text is stable and gets that result
  go++-syntax   `gopp fmt` rejects the file: it uses `macro`, or `ref` as a name
  skipped       gofmt rejects the file (test data with syntax errors)
  other         anything else: a bug
--goroot adds $(go env GOROOT)/src. Exit status 1 if there are more than
--allow-other (default 0) files in "other".
"""
import concurrent.futures
import functools
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import gopp_batch  # noqa: E402
import procslots  # noqa: E402


def run(args, data=None):
    with procslots.slot():
        return subprocess.run(args, input=data, capture_output=True)


def comment_free(text):
    return [l for l in text.split(b"\n") if l.strip() and not l.lstrip().startswith(b"//")]


def classify(gopp, paths):
    """The class of each file of paths; `gopp fmt` runs in one batch over
    the files gofmt accepts (tests/gopp_batch.py)."""
    # By path, not stdin: gofmt accepts an empty text on stdin but rejects an
    # empty file, like `gopp fmt`.
    gofmt = {p: run(["gofmt", p]) for p in paths}
    ok = [p for p in paths if gofmt[p].returncode == 0]
    fmt = gopp_batch.run([gopp, "fmt", "-batch", *ok], ok,
                         lambda p: run([gopp, "fmt", "-"], open(p, "rb").read())) if ok else {}
    return [classify_one(gofmt[p], fmt.get(p)) for p in paths]


def classify_one(g, o):
    """g: gofmt of the file, o: `gopp fmt -` of its bytes (None if g failed)."""
    if g.returncode != 0:
        return "skipped"
    if o.returncode != 0:
        return "go++-syntax"
    if o.stdout == g.stdout:
        return "identical"
    if comment_free(o.stdout) == comment_free(g.stdout):
        return "doc-comments"
    g2 = run(["gofmt"], g.stdout)
    if g2.returncode == 0 and g2.stdout == o.stdout:
        return "fixed-point"
    g3 = run(["gofmt"], g2.stdout)
    if g3.returncode == 0 and g3.stdout == o.stdout:
        return "fixed-point"
    return "other"


def main():
    args = sys.argv[1:]
    gopp = args.pop(0)
    jobs = os.cpu_count() or 4
    show = False
    allow = 0
    shard = (0, 1)
    paths = []
    while args:
        a = args.pop(0)
        if a == "--goroot":
            root = run(["go", "env", "GOROOT"]).stdout.decode().strip()
            paths.append(os.path.join(root, "src"))
        elif a == "--jobs":
            jobs = int(args.pop(0))
        elif a == "--list":
            show = True
        elif a == "--allow-other":
            allow = int(args.pop(0))
        elif a == "--shard":
            k, n = args.pop(0).split("/")
            shard = (int(k), int(n))
        else:
            paths.append(a)
    files = []
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                files += [os.path.join(root, n) for n in sorted(names) if n.endswith(".go")]
        else:
            files.append(p)
    files.sort()
    # --shard K/N: the files whose place in the sorted list is K modulo N.
    files = files[shard[0] :: shard[1]]
    counts = {}
    # Threads suit a small set; a large one (the standard library, also one
    # shard of it: about 920 files) needs processes, because the Python work
    # per file runs under one interpreter lock.
    pool = concurrent.futures.ProcessPoolExecutor if len(files) > 500 else concurrent.futures.ThreadPoolExecutor
    batches = gopp_batch.chunks(files, jobs)
    with pool(max_workers=jobs) as ex:
        for fs, cs in zip(batches, ex.map(functools.partial(classify, gopp), batches)):
            for f, c in zip(fs, cs):
                counts[c] = counts.get(c, 0) + 1
                if c in ("other", "fixed-point", "go++-syntax") or (show and c != "identical"):
                    print("%s: %s" % (c, f))
    print("compared %d files:" % len(files))
    for c in ("identical", "doc-comments", "fixed-point", "go++-syntax", "skipped", "other"):
        print("  %-13s %d" % (c, counts.get(c, 0)))
    return 1 if counts.get("other", 0) > allow else 0


if __name__ == "__main__":
    sys.exit(main())
