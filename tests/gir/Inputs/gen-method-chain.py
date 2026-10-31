#!/usr/bin/env python3
"""P04.24 (GEN-14, R15): methods of a generic type that reach the unit need
of another method, in every way one method can name another, in a generated
chain, and the generic code that calls them.

usage: gen-method-chain.py N ORDER MODE [G...]  write the program
       gen-method-chain.py --run GOPP N      build and check every case

Vec[U] has methods M0..M(N-1). M0 has a collapse site (`v.X / w`, so it needs
the unit of Vec). Each M(i), i >= 1, reaches M(i-1) in one of eight ways:
a call on the receiver, through a pointer, on a composite literal of another
generic type (W[U]{v.X}.A, whose A has the site), a method value, a call on a copy, a call from a function literal, from a branch not taken, and in a loop. F(i) needs no unit (`return 2.0`) and FP(i) calls F(i). ORDER
asc declares M0 first, desc last (the checker sees the callers before the
callee). MODE generic adds, for the callers listed, a generic function G(i)[U]
calling v.M(i) and GF(i) calling v.F(i)/v.FP(i); MODE plain only calls them with known
units.

--run checks, for ORDER in asc, desc and -build-check-j 1 and 16: MODE plain
builds and prints 1000 for each M(i) and 2 for each F(i), FP(i); MODE generic
is refused with exactly one E0439 for each G(i) (not for GF(i)), never a
program (each window of 8 callers at a time: the driver stops after 10 errors). The two kinds of methods are in one program, so a fix that refuses
too much (R12) or too little (R15) fails one side.
"""
import os
import subprocess
import sys
import tempfile

WAYS = 8


def source(n, order, mode, gs=()):
    L = ["package main", "", "unit M", "unit KM = 1000 M", ""]
    w = L.append
    w("type W[U unit] struct{ X float64[U] }")
    w("func (w W[U]) A(m float64[M]) float64 { return float64(w.X / m) }")
    w("type Vec[U unit] struct{ X float64[U] }")
    meth = []
    for i in range(n):
        if i == 0:
            body = "return float64(v.X / w)"
        else:
            j = i - 1
            body = [
                "return v.M%d(w)" % j,
                "return (&v).M%d(w)" % j,
                "return W[U]{v.X}.A(w)",
                "f := v.M%d\n\treturn f(w)" % j,
                "u := v\n\treturn u.M%d(w)" % j,
                "f := func() float64 { return v.M%d(w) }\n\treturn f()" % j,
                "if w > 1.0e300[M] {\n\t\treturn v.M%d(w)\n\t}\n\treturn v.M%d(w)" % (j, j),
                "var r float64\n\tfor k := 0; k < 1; k++ {\n\t\tr = v.M%d(w)\n\t}\n\treturn r" % j,
            ][i % WAYS]
        meth.append("func (v Vec[U]) M%d(w float64[M]) float64 {\n\t%s\n}" % (i, body))
        meth.append("func (v Vec[U]) F%d(w float64[M]) float64 { return 2.0 }" % i)
        meth.append("func (v Vec[U]) FP%d(w float64[M]) float64 { return v.F%d(w) }" % (i, i))
    if order == "desc":
        meth.reverse()
    L += meth
    if mode == "generic":
        for i in gs:
            w("func G%d[U unit](v Vec[U], w float64[M]) float64 { return v.M%d(w) }" % (i, i))
            w("func GF%d[U unit](v Vec[U], w float64[M]) float64 { return v.F%d(w) + v.FP%d(w) }" % (i, i, i))
    w("func main() {")
    w("\tv := Vec[KM]{1.0[KM]}")
    w("\tm := 1.0[M]")
    for i in range(n):
        w("\tprintln(v.M%d(m), v.F%d(m), v.FP%d(m))" % (i, i, i))
    for i in gs:
        w("\tprintln(GF%d(v, m), G%d(v, m))" % (i, i))
    w("}")
    return "\n".join(L) + "\n"


def fmt(x):
    s = "%+.6e" % x
    mant, exp = s.split("e")
    return "%se%s%s" % (mant, exp[0], exp[1:].lstrip("0").rjust(3, "0"))


def expected(n):
    a, b = fmt(1000.0), fmt(2.0)
    return "".join("%s %s %s\n" % (a, b, b) for _ in range(n))


def run(gopp, n):
    ok = True
    with tempfile.TemporaryDirectory() as d:
        for order in ("asc", "desc"):
            for j in (1, 16):
                src = os.path.join(d, "p.gopp")
                open(src, "w").write(source(n, order, "plain"))
                exe = os.path.join(d, "p")
                r = subprocess.run([gopp, "build", "-build-check-j=%d" % j, src, "-o", exe],
                                   capture_output=True, text=True, timeout=120)
                if r.returncode != 0:
                    print("FAIL plain %s j=%d: build\n%s" % (order, j, r.stderr[:500]))
                    ok = False
                    continue
                out = subprocess.run([exe], capture_output=True, text=True, timeout=60).stdout
                if out != expected(n):
                    print("FAIL plain %s j=%d: wrong output" % (order, j))
                    ok = False
                # (The driver stops after 10 errors: a window of 8 callers.)
                for lo in range(0, n, 8):
                    gs = list(range(lo, min(lo + 8, n)))
                    open(src, "w").write(source(n, order, "generic", gs))
                    r = subprocess.run([gopp, "build", "-build-check-j=%d" % j, "-emit=gir", src],
                                       capture_output=True, text=True, timeout=120)
                    got = r.stderr.count("error[E0439]")
                    if r.returncode == 0 or got != len(gs):
                        print("FAIL generic %s j=%d callers %d..: rc=%d, %d E0439, want %d"
                              % (order, j, lo, r.returncode, got, len(gs)))
                        ok = False
    print("OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    if sys.argv[1] == "--run":
        sys.exit(run(sys.argv[2], int(sys.argv[3])))
    sys.stdout.write(source(int(sys.argv[1]), sys.argv[2], sys.argv[3], [int(x) for x in sys.argv[4:]]))
