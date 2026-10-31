#!/usr/bin/env python3
"""P04.24 (GEN-14, R19, R20): every route by which a unit of generic code
shared across unit arguments can leave its body, composed, with every
decision that needs the unit at the end of the chain.

usage: gen-shared-units.py --run GOPP [--full] [--jobs N]

A body shared across unit arguments (`G[U unit]`, or a unit-generic layer of
the chain) has its unit parameters as the empty unit, so `W[U]` there is the
type `W[]` of an instance with real units too. Each case is a chain of 0 to 3
layers (routes) from a root to a leaf:

  routes  tparam  F[X C](x X)              X = the value's type
          slice   F[S ~[]X, X C](s S)      a slice literal, S = []X
          map     F[MM ~map[int]X, X C]    a map literal, ranged over
          func    F[FN ~func() X, X C]     a function literal, FN = func() X
          pair    F[X C](p Pair[X, int])   a nested generic type, literal
          fval    F[X C]: g := Fi[X]       a func value of a generic function
          method  B[X C]{v}.Run(m)         a method of a generic type
          mval    r := B[X C]{v}.Run       a method value of one
          unit    F[V unit](x W[V])        unit arguments (a shared body
                                           itself; first layer only)
          ubox    F[V unit](b Box[W[V]])   the same with the unit inside a
                                           generic type's argument
          ustruct F[V unit](s struct{ f W[V] })  inside a struct field
  leaves  A       v.A(m)                   a method that needs the unit
          AV      fa := v.A; fa(m)         a method value of it
          N       v.N()                    a method that needs none
          box     any(v).(W[KM])           a type assertion of a boxed value
          sw      switch any(v).(type)     a type switch
          eq      any(v) == any(W[KM]{..}) interface ==
          map     map[any]int{..}[any(v)]  a map key of interface type
  carriers W[U] (all leaves) and float64[U] (box, sw, eq, map)
  roots   Q  plain code, W[KM] (or float64[KM])
          T  a method of Tw[KM] (real units) with W[U*IKM]: at U = KM the
             empty unit with the factor 1, the twin of the erased W[U]
          G  G[U unit], W[U]: the shared body

Each chain has its own copies of its functions and types, so its instances
are its own. --run checks, at -build-check-j 1 and 16 and in both orders of
the T and G statements in main (the order the walk meets the twin and the
erased instance):

  - every case whose leaf needs no unit (N), or whose roots are not shared
    (Q, T without a unit layer), is built and run, and prints the values of
    plain code (G: the values of Q; T: those of W[]);
  - every case whose leaf needs the unit and that a shared body reaches is
    exactly one E0439, at the call where the shared body passes the unit on
    in a type argument (or at the leaf when it is in that body), in windows
    of 8 cases (the driver stops after 10 errors).

The normal suite runs a sample (depth 0 and 1, leaves A, box, N); --full
(the .long test) runs every chain of depth 0 to 3 with every leaf.
"""
import concurrent.futures
import os
import subprocess
import sys
import tempfile

ROUTES = ["tparam", "slice", "map", "func", "pair", "fval", "method", "mval"]
# (Unit-generic layers, bodies shared across unit arguments themselves: the
# first layer only, where the value still has its unit type.)
UNIT_ROUTES = ["unit", "ubox", "ustruct"]
LEAVES_W = ["A", "AV", "N", "box", "sw", "eq", "map"]
LEAVES_F = ["box", "sw", "eq", "map"]
NEEDY = {"A", "AV", "box", "sw", "eq", "map"}
RES = {"A": "float64", "AV": "float64", "N": "float64", "box": "bool",
       "sw": "int", "eq": "bool", "map": "int"}
ZERO = {"float64": "0.0", "bool": "false", "int": "0"}


def fmt(x):
    s = "%+.6e" % x
    mant, exp = s.split("e")
    return "%se%s%s" % (mant, exp[0], exp[1:].lstrip("0").rjust(3, "0"))


def want(carrier, leaf, twin):
    """What plain code prints: the value 1 KM (a real unit), or the twin 2
    in the empty unit (a W[] or float64 is no W[KM] or float64[KM])."""
    if leaf in ("A", "AV"):
        return fmt(2.0 if twin else 1000.0)
    if leaf == "N":
        return fmt(2.0)
    if leaf in ("box", "eq"):
        return "false" if twin else "true"
    if leaf == "sw":
        return "0" if twin else "1"
    return "0" if twin else "7"


def chains(full):
    """(depth 0..3 or 0..1) sequences of routes, `unit` first only."""
    out = [()]
    top = 3 if full else 1
    level = [()]
    for _ in range(top):
        nxt = []
        for c in level:
            for r in ROUTES + (UNIT_ROUTES if not c else []):
                nxt.append(c + (r,))
        out += nxt
        level = nxt
    return out


class Prog:
    def __init__(self):
        self.L = ["package main", "", "unit M", "unit KM = 1000 M",
                  "unit IKM = 1 / KM", "",
                  "type W[U unit] struct{ X float64[U] }",
                  "func (w W[U]) A(m float64[M]) float64 { return float64(w.X / m) }",
                  "func (w W[U]) N() float64 { return 2.0 }",
                  "type Wish interface {", "\tCopy", "\tA(m float64[M]) float64",
                  "\tN() float64", "}",
                  "type Pair[A any, B any] struct {", "\tL A", "\tR B", "}",
                  "type Box[T any] struct{ v T }", ""]
        self.main = []

    def line(self):
        return len(self.L) + 1  # the line the next append writes

    def w(self, s):
        self.L.append(s)


def leaf_lines(carrier, leaf, v):
    tgt = "W[KM]" if carrier == "W" else "float64[KM]"
    val = "W[KM]{1.0[KM]}" if carrier == "W" else "1.0[KM]"
    if leaf == "A":
        return ["return %s.A(m)" % v]
    if leaf == "AV":
        return ["fa := %s.A" % v, "return fa(m)"]
    if leaf == "N":
        return ["return %s.N()" % v]
    if leaf == "box":
        return ["_, ok := any(%s).(%s)" % (v, tgt), "return ok"]
    if leaf == "sw":
        return ["switch any(%s).(type) {" % v, "case %s:" % tgt, "\treturn 1", "}",
                "return 0"]
    if leaf == "eq":
        return ["return any(%s) == any(%s)" % (v, val)]
    return ["return map[any]int{%s: 7}[any(%s)]" % (val, v)]


def unitOf(vt):
    """The unit of a unit type's text (`W[U*IKM]`: `U*IKM`)."""
    return vt[vt.index("[") + 1:vt.rindex("]")]


def emit_case(P, i, chain, carrier, leaf, which):
    """Writes the functions of case i with the roots in `which` (a plain
    root is walked whether main calls it or not); returns (roots, error
    line): the roots main calls (name, call text, twin), and the line of the
    one E0439 if a shared body reaches a leaf that needs the unit."""
    C = "Wish" if carrier == "W" else "Copy"
    res = RES[leaf]
    unitT = (lambda u: "W[%s]" % u) if carrier == "W" else (lambda u: "float64[%s]" % u)
    # The layers, innermost first, so each knows its callee; then the roots.
    # A layer is written as a function whose body passes `v` (of type `vt`)
    # on: call(k, v, vt) is the text that calls layer k, and the line of the
    # body that holds the call is recorded (the origin of an E0439).
    calls = {}  # layer k -> function (v, vt) -> list of body lines
    body_line = {}  # layer k -> line of its call (or leaf) in its body
    n = len(chain)

    def body(k, v, vt, indent="\t"):
        """The body lines of a function at layer k-1 that passes v to layer
        k (or does the leaf when k == n). The first line is the one that
        names layer k."""
        if k == n:
            return leaf_lines(carrier, leaf, v)
        return calls[k](v, vt)

    def write_fn(header, k, v, vt, pre=(), post=()):
        P.w(header)
        for s in pre:
            P.w("\t" + s)
        # (The line that names the next layer: marked `@`, else the first.)
        lines = body(k, v, vt)
        marked = [j for j, s in enumerate(lines) if s.startswith("@")]
        at = P.line() + (marked[0] if marked else 0)
        ind = "\t" + ("\t" if pre else "")
        for s in lines:
            P.w(ind + s.lstrip("@"))
        for s in post:
            P.w("\t" + s)
        P.w("}")
        return at

    for k in reversed(range(n)):
        r = chain[k]
        name = "L%d_%d" % (i, k)
        if r == "tparam":
            body_line[k] = write_fn("func %s[X %s](x X, m float64[M]) %s {" % (name, C, res), k + 1, "x", "X")
            calls[k] = (lambda name: lambda v, vt: ["return %s(%s, m)" % (name, v)])(name)
        elif r == "slice":
            body_line[k] = write_fn("func %s[S ~[]X, X %s](s S, m float64[M]) %s {" % (name, C, res), k + 1, "s[0]", "X")
            calls[k] = (lambda name: lambda v, vt: ["return %s([]%s{%s}, m)" % (name, vt, v)])(name)
        elif r == "map":
            body_line[k] = write_fn("func %s[MM ~map[int]X, X %s](mm MM, m float64[M]) %s {" % (name, C, res), k + 1, "v", "X",
                                    pre=("for _, v := range mm {",), post=("}", "return " + ZERO[res]))
            calls[k] = (lambda name: lambda v, vt: ["return %s(map[int]%s{0: %s}, m)" % (name, vt, v)])(name)
        elif r == "func":
            body_line[k] = write_fn("func %s[FN ~func() X, X %s](fn FN, m float64[M]) %s {" % (name, C, res), k + 1, "fn()", "X")
            # (A copy in a local: a closure that holds a borrowed variable is
            # CLO-3a's.)
            calls[k] = (lambda name: lambda v, vt: ["y := %s" % v, "@return %s(func() %s { return y }, m)" % (name, vt)])(name)
        elif r == "pair":
            body_line[k] = write_fn("func %s[X %s](p Pair[X, int], m float64[M]) %s {" % (name, C, res), k + 1, "p.L", "X")
            calls[k] = (lambda name: lambda v, vt: ["return %s(Pair[%s, int]{%s, 0}, m)" % (name, vt, v)])(name)
        elif r == "fval":
            inner = name + "i"
            body_line[k] = write_fn("func %s[X %s](x X, m float64[M]) %s {" % (inner, C, res), k + 1, "x", "X")
            P.w("func %s[X %s](x X, m float64[M]) %s {" % (name, C, res))
            P.w("\tg := %s[X]" % inner)
            P.w("\treturn g(x, m)")
            P.w("}")
            calls[k] = (lambda name: lambda v, vt: ["return %s(%s, m)" % (name, v)])(name)
        elif r in ("method", "mval"):
            ty = "B%d_%d" % (i, k)
            P.w("type %s[X %s] struct{ v X }" % (ty, C))
            body_line[k] = write_fn("func (b %s[X]) Run(m float64[M]) %s {" % (ty, res), k + 1, "b.v", "X")
            if r == "method":
                calls[k] = (lambda ty: lambda v, vt: ["return %s[%s]{%s}.Run(m)" % (ty, vt, v)])(ty)
            else:
                calls[k] = (lambda ty: lambda v, vt: ["r := %s[%s]{%s}.Run" % (ty, vt, v), "return r(m)"])(ty)
        elif r == "unit":
            body_line[k] = write_fn("func %s[V unit](x %s, m float64[M]) %s {" % (name, unitT("V"), res), k + 1, "x", unitT("V"))
            calls[k] = (lambda name: lambda v, vt: ["return %s(%s, m)" % (name, v)])(name)
        elif r == "ubox":
            # (The unit argument is written: the checker infers no unit from
            # `Box[float64[V]]`.)
            body_line[k] = write_fn("func %s[V unit](b Box[%s], m float64[M]) %s {" % (name, unitT("V"), res), k + 1, "b.v", unitT("V"))
            calls[k] = (lambda name: lambda v, vt: ["return %s[%s](Box[%s]{%s}, m)" % (name, unitOf(vt), vt, v)])(name)
        elif r == "ustruct":
            body_line[k] = write_fn("func %s[V unit](s struct{ f %s }, m float64[M]) %s {" % (name, unitT("V"), res), k + 1, "s.f", unitT("V"))
            calls[k] = (lambda name: lambda v, vt: ["return %s[%s](struct{ f %s }{%s}, m)" % (name, unitOf(vt), vt, v)])(name)
    # The roots.
    q = "Q%d" % i
    root_line = {}
    if "Q" in which:
        root_line["Q"] = write_fn("func %s(x %s, m float64[M]) %s {" % (q, unitT("KM"), res), 0, "x", unitT("KM"))
    tw = "Tw%d" % i
    if "T" in which:
        P.w("type %s[U unit] struct{}" % tw)
    twv = ("W[U*IKM]{x * 1.0[IKM]}" if carrier == "W" else "x * 1.0[IKM]")
    twt = unitT("U*IKM")
    if "T" not in which:
        pass
    elif n == 0:
        # (The leaf in the method itself.)
        P.w("func (t %s[U]) P(x float64[U], m float64[M]) %s {" % (tw, res))
        P.w("\ty := " + twv)
        root_line["T"] = P.line()
        for s in leaf_lines(carrier, leaf, "y"):
            P.w("\t" + s)
        P.w("}")
    else:
        root_line["T"] = write_fn("func (t %s[U]) P(x float64[U], m float64[M]) %s {" % (tw, res), 0, twv, twt)
    g = "G%d" % i
    if "G" in which:
        root_line["G"] = write_fn("func %s[U unit](x %s, m float64[M]) %s {" % (g, unitT("U"), res), 0, "x", unitT("U"))
    one = "W[KM]{1.0[KM]}" if carrier == "W" else "1.0[KM]"
    roots = [r for r in [("Q", "%s(%s, 1.0[M])" % (q, one), False),
                         ("T", "%s[KM]{}.P(2.0[KM], 1.0[M])" % tw, True),
                         ("G", "%s(%s, 1.0[M])" % (g, one), False)]
             if r[0] in which]
    # Where a shared body passes the unit on: the last unit-generic layer
    # (G, or a `unit` layer, which every root goes through), at its call of
    # the next layer, or at the leaf if it is the last layer.
    err = None
    if leaf in NEEDY and "G" in which:
        if chain and chain[0] in UNIT_ROUTES:
            err = body_line[0]
        else:
            err = root_line["G"]
    return roots, err, bool(chain) and chain[0] in UNIT_ROUTES


def build_program(cases, order, mode):
    """cases: [(chain, carrier, leaf)]. mode 'run': the cases that must
    build; 'refuse': the needy ones. Returns (source, expected stdout or the
    expected E0439 lines)."""
    P = Prog()
    out = []
    errs = []
    for i, (chain, carrier, leaf) in enumerate(cases):
        if mode == "run" and leaf in NEEDY:
            # (Plain roots only; none past a unit-generic layer.)
            which = set() if chain and chain[0] in UNIT_ROUTES else {"Q", "T"}
        else:
            which = {"Q", "T", "G"}
        use, err, _ = emit_case(P, i, chain, carrier, leaf, which)
        if mode != "run":
            errs.append(err)
        if order == "GT":
            use = sorted(use, key=lambda r: {"G": 0, "T": 1, "Q": 2}[r[0]])
        for name, call, twin in use:
            P.main.append("\tprintln(%s)" % call)
            out.append(want(carrier, leaf, twin))
    P.w("func main() {")
    P.L += P.main
    P.w("}")
    src = "\n".join(P.L) + "\n"
    if mode == "run":
        return src, "".join(o + "\n" for o in out)
    return src, sorted(errs)


def all_cases(full):
    leaves_w = LEAVES_W if full else ["A", "box", "N"]
    leaves_f = LEAVES_F if full else ["box"]
    cs = []
    for c in chains(full):
        for l in leaves_w:
            cs.append((c, "W", l))
        for l in leaves_f:
            cs.append((c, "F", l))
    return cs


def check(gopp, d, idx, src, order, j, mode, expect):
    path = os.path.join(d, "p%d.gopp" % idx)
    with open(path, "w") as f:
        f.write(src)
    if mode == "run":
        exe = os.path.join(d, "p%d" % idx)
        r = subprocess.run([gopp, "build", "-build-check-j=%d" % j, path, "-o", exe],
                           capture_output=True, text=True, timeout=300)
        if r.returncode != 0:
            return "FAIL run %d %s j=%d: build\n%s" % (idx, order, j, r.stderr[:2000])
        got = subprocess.run([exe], capture_output=True, text=True, timeout=60).stdout
        if got != expect:
            return "FAIL run %d %s j=%d: wrong output (%s)" % (idx, order, j, path)
        return None
    r = subprocess.run([gopp, "build", "-build-check-j=%d" % j, "-emit=gir", path],
                       capture_output=True, text=True, timeout=300)
    lines = sorted(int(l.split(":")[1]) for l in r.stderr.splitlines()
                   if "error[" in l)
    kinds = [l for l in r.stderr.splitlines() if "error[" in l and "error[E0439]" not in l]
    if r.returncode == 0 or lines != expect or kinds:
        return ("FAIL refuse %d %s j=%d: rc=%d, E0439 at lines %s, want %s (%s)\n%s"
                % (idx, order, j, r.returncode, lines, expect, path, r.stderr[:1500]))
    return None


def run(gopp, full, jobs):
    cases = all_cases(full)
    runs = cases
    needy = [c for c in cases if c[2] in NEEDY]
    progs = []
    step = 120 if full else 1000
    for lo in range(0, len(runs), step):
        progs.append(("run", runs[lo:lo + step]))
    for lo in range(0, len(needy), 8):
        progs.append(("refuse", needy[lo:lo + 8]))
    work = []
    for mode, cs in progs:
        for order in ("TG", "GT"):
            src, expect = build_program(cs, order, mode)
            for j in (1, 16):
                work.append((src, order, j, mode, expect))
    fails = []
    with tempfile.TemporaryDirectory() as d:
        with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
            futs = [ex.submit(check, gopp, d, k, *w) for k, w in enumerate(work)]
            for f in futs:
                r = f.result()
                if r:
                    fails.append(r)
    nrun = sum(len(cs) for m, cs in progs if m == "run")
    print("%d cases (%d refused), %d programs" % (nrun, len(needy), len(work)))
    for f in fails[:10]:
        print(f)
    print("OK" if not fails else "FAILED (%d)" % len(fails))
    return 0 if not fails else 1


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "--run":
        jobs = 1
        if "--jobs" in a:
            jobs = int(a[a.index("--jobs") + 1])
        sys.exit(run(a[1], "--full" in a, jobs))
    # Write one program: CHAIN(comma separated) CARRIER LEAF ORDER MODE
    chain = tuple(x for x in a[0].split(",") if x)
    src, _ = build_program([(chain, a[1], a[2])], a[3], a[4])
    sys.stdout.write(src)
