#!/usr/bin/env python3
"""Writes the expected output of run tests whose program is also a Go program.

usage: gen-goldens.py --go GO [--dir OUTDIR] PROGRAM...

A PROGRAM is a .test or .gopp file (one package main), or a directory of
.gopp files (several files of one package). The program is valid Go and
valid Go++, so `go run` of the same text gives the expected output: Go's
println writes to standard error, Go++'s bootstrap println to standard
output, so Go's standard error is the expected standard output
(OUTDIR/<name>.out). A program that ends in a run-time panic exits with
status 2 in both: the text before Go's "panic:" line is the expected output
and the "panic:" line itself, the first line of the Go++ program's standard
error, is OUTDIR/<name>.err. (D4.12: the first line equals Go's; the
goroutine trace that follows it is Go's only.)

The tests themselves never run Go; run this script when a program changes.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile


def run_go(go, srcs):
    with tempfile.TemporaryDirectory() as d:
        names = []
        for i, (name, text) in enumerate(srcs):
            n = name if name.endswith(".go") else os.path.splitext(name)[0] + ".go"
            names.append(n)
            with open(os.path.join(d, n), "w", encoding="utf-8", errors="surrogateescape") as f:
                f.write(text)
        r = subprocess.run([go, "run"] + names, cwd=d,
                           capture_output=True, timeout=300,
                           env=dict(os.environ, GO111MODULE="off"))
        return r.returncode, r.stderr.decode("utf-8", "surrogateescape")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--go", required=True)
    ap.add_argument("--dir", default=None)
    ap.add_argument("programs", nargs="+")
    a = ap.parse_args()
    for p in a.programs:
        # DIR@a.gopp,c.gopp: the files of DIR in this order on the command
        # line (the go command keeps the order of explicit files; a
        # directory is sorted by name).
        order = None
        if "@" in p:
            p, o = p.split("@", 1)
            order = o.split(",")
        base = os.path.basename(p.rstrip("/"))
        stem = base.split(".")[0]
        if order:
            stem += "-" + "".join(f[0] for f in order)
        outdir = a.dir or (os.path.dirname(os.path.abspath(p.rstrip("/"))) if os.path.isdir(p)
                           else os.path.join(os.path.dirname(os.path.abspath(p)), "Inputs"))
        if os.path.isdir(p):
            names = order or sorted(f for f in os.listdir(p) if f.endswith(".gopp"))
            srcs = [(f, open(os.path.join(p, f), encoding="utf-8", errors="surrogateescape").read()) for f in names]
        else:
            text = open(p, encoding="utf-8", errors="surrogateescape").read()
            srcs = [(stem + ".go", text)]
        code, err = run_go(a.go, srcs)
        lines = err.split("\n")
        if code == 0:
            out, panic = err, None
        elif code == 1 and "exit status 2" in err:
            i = next(k for k, l in enumerate(lines) if l.startswith("panic:") or l.startswith("fatal error:"))
            out = "".join(l + "\n" for l in lines[:i])
            panic = lines[i] + "\n"
        else:
            sys.exit("%s: go run failed with status %d:\n%s" % (p, code, err[-1500:]))
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, stem + ".out"), "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
            f.write(out)
        if panic is not None:
            with open(os.path.join(outdir, stem + ".err"), "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
                f.write(panic)
        print("%s: %d bytes%s" % (stem, len(out), ", panic: " + panic.strip() if panic else ""))


if __name__ == "__main__":
    main()
