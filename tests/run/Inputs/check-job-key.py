#!/usr/bin/env python3
"""The key of a result of the run tests' batch (runlib.job_key, P04.19,
manager decision M3), both sides of every part of it: the same sources, the
same gopp binary and the same protocol give the same key (so a test finds its
result); a change of any of them gives another key (so a test never takes a
result that is not its program's)."""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runlib  # noqa: E402

fails = []


def same(a, b, what):
    if a != b:
        fails.append("%s: the key changed" % what)


def differ(a, b, what):
    if a == b:
        fails.append("%s: the key did not change" % what)


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


with tempfile.TemporaryDirectory() as d:
    gopp = os.path.join(d, "gopp")
    write(gopp, "a compiler")
    os.environ["GOPP"] = gopp
    a, b = os.path.join(d, "a.gopp"), os.path.join(d, "b.gopp")
    write(a, 'package main\n\nfunc main() { println("a") }\n')
    write(b, "package main\n\nvar x = 1\n")
    key = runlib.job_key([a])

    # Sources.
    same(runlib.job_key([a]), key, "nothing changed")
    write(a, 'package main\n\nfunc main() { println("A") }\n')
    differ(runlib.job_key([a]), key, "one byte of the source changed")
    write(a, 'package main\n\nfunc main() { println("a") }\n')
    same(runlib.job_key([a]), key, "the source has its old bytes again")
    os.makedirs(os.path.join(d, "copy"))
    c = os.path.join(d, "copy", "a.gopp")
    write(c, open(a).read())
    differ(runlib.job_key([c]), key, "the same bytes in another file")
    # The order of the files is the order of package initialization.
    differ(runlib.job_key([a, b]), runlib.job_key([b, a]), "the files in another order")
    same(runlib.job_key([a, b]), runlib.job_key([a, b]), "the files in the same order")
    pkg = os.path.join(d, "pkg")
    os.makedirs(pkg)
    write(os.path.join(pkg, "a.gopp"), open(a).read())
    dkey = runlib.job_key([pkg])
    same(runlib.job_key([pkg]), dkey, "a directory that did not change")
    write(os.path.join(pkg, "b.gopp"), open(b).read())
    differ(runlib.job_key([pkg]), dkey, "a file added to the directory")

    # The gopp binary: a rebuilt one has another time stamp or size.
    st = os.stat(gopp)
    os.utime(gopp, ns=(st.st_atime_ns, st.st_mtime_ns + 1000000000))
    differ(runlib.job_key([a]), key, "gopp has a new time stamp")
    os.utime(gopp, ns=(st.st_atime_ns, st.st_mtime_ns))
    same(runlib.job_key([a]), key, "gopp has its old time stamp again")
    write(gopp, "a compiler, rebuilt")
    os.utime(gopp, ns=(st.st_atime_ns, st.st_mtime_ns))
    differ(runlib.job_key([a]), key, "gopp has another size")
    write(gopp, "a compiler")
    os.utime(gopp, ns=(st.st_atime_ns, st.st_mtime_ns))
    same(runlib.job_key([a]), key, "gopp as it was")

    # The protocol version.
    runlib.PROTOCOL_VERSION += 1
    differ(runlib.job_key([a]), key, "another protocol version")
    runlib.PROTOCOL_VERSION -= 1
    same(runlib.job_key([a]), key, "the protocol version as it was")

if fails:
    sys.exit("\n".join(fails))
print("job keys: all checks passed")
