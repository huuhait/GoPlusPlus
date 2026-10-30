#!/usr/bin/env python3
"""P03.25 (GEN-14, UNI-10): generic code gives the same answer as plain code.

Table 1 (bodies): each use is written once with `K[M]` and once in a body
generic over a unit parameter U (`K[U]`), for the number kinds K of KINDS,
checked with one body thread and with eight (the same errors); a cell is `.`
(accepted) or `E` (an error). The two forms differ only where the spec says
so (GEN-14: "A conversion to a concrete unit (`float64[M](x)`) is an error in
the body, because the dimension of `U` is not known"): the conversion to a
concrete unit, `.In[KM]()` (the same conversion, EXP-32) and a literal with
a concrete unit added to a value in U; such a row is marked `intended` and
prints `plain/generic`. Any other difference prints MISMATCH and exits
non-zero.

Table 2 (instances): for each type T, the operator element
`interface{ OP (T) T }` is met by T exactly when the plain code can write
`a OP b` with the result of type T (UNI-10), so the instantiation and the
plain use are decided by the same predicate. `.` accepted, `E` error.

Table 3 (instances of cancelling units, R2, GEN-14): a unit-generic function
whose code cancels two unit parameters (`float64(a / b)` with U, V; `a * b`
with U and an inverse unit V) is instantiated with each pair of units, and the
same expression is written as plain code with those units. A cell is
`plain/generic`: plain is `.` (no scale change) or the factors that
`--dump-types` records (`unit-conv`, EXP-33); generic is the factor that the
generic code applies at run time at that instance, computed from the collapse
sites it records (`unit-site`, the rule of Info::UnitSite) with the real units:
`.` when every site gives 1. The two must be equal. The one cell that cannot
be equal is an integer kind whose remaining factor is no integer: plain code
sees the units and reports EXP-35's error (`E`); generic code truncates at run
time like an explicit conversion (`t`, GEN-14); that cell is `E/t` (owner
decision 2026-10-30, ADR-0025 Amendment point 7).

Usage: gen-units-parity.py --gopp <gopp> --out <dir>
"""
import argparse
import os
import re
import subprocess
import sys
from fractions import Fraction

HEAD = """package main

unit M
unit S
unit A
unit KM = 1000 M

type Dist float64[M]
"""

# (name, expression, intended-difference?)  a, b: float64[X]; k: float64;
# p: float64; n: int; z: uint; s: float64[Y] (another unit)
USES = [
    ("add", "a + b", False),
    ("sub", "a - b", False),
    ("cmp-lt", "a < b", False),
    ("cmp-eq", "a == b", False),
    ("cmp-ne", "a != b", False),
    ("spaceship", "a <=> b", False),
    ("neg", "-a", False),
    ("pos", "+a", False),
    ("mul-scalar", "a * k", False),
    ("scalar-mul", "k * a", False),
    ("div-scalar", "a / k", False),
    ("mul-lit", "a * 2", False),
    ("div-lit", "a / 2.5", False),
    ("mul-unit", "a * b", False),
    ("div-unit", "a / b", False),
    ("inv", "1.0 / a", False),
    ("add-plain", "a + p", False),
    ("add-lit", "a + 1.0", False),
    ("add-zero", "a + 0", False),
    ("sub-zero", "a - 0", False),
    ("cmp-zero", "a > 0", False),
    ("zero-cmp", "0 == a", False),
    ("add-other", "a + s", False),
    ("cmp-other", "a < s", False),
    ("rem", "a % b", False),
    ("and", "a & b", False),
    ("xor", "a ^ b", False),
    ("not", "^a", False),
    ("shl", "a << z", False),
    ("shl-unit", "z << a", False),
    ("min", "min(a, b)", False),
    ("max", "max(a, 0)", False),
    ("min-plain", "min(a, p)", False),
    ("in-self", "a.In[X]()", False),
    ("in-other", "a.In[Y]()", False),
    ("plain-conv", "float64(a)", False),
    ("unit-conv", "float64[X](p)", False),
    ("conv-concrete", "float64[KM](a)", True),
    ("in-concrete", "a.In[KM]()", True),
    ("lit-concrete", "a + 1.0[KM]", True),
    ("assign-plain", "VARP = a", False),
    ("assign-unit", "VARA = p", False),
    ("assign-zero", "VARA = 0", False),
    ("any", "any(a)", False),
    ("send", "SEND <- a", False),
    ("slice-elem", "SL[0] = a", False),
    ("map-key", "MP[a] = 1", False),
    ("incr", "VARA++", False),
    ("addassign", "VARA += b", False),
    ("addassign-other", "VARA += s", False),
    ("mulassign", "VARA *= k", False),
    ("mulassign-unit", "VARA *= b", False),
]

# Table 2: types and operator elements. The plain use is `a OP b` (binary),
# `OP a` (unary) with the result checked against T.
TYPES = [
    ("float64[M]", "float64"),
    ("int[M]", "int"),
    ("uint8[M]", "uint8"),
    ("complex128[A]", "complex128"),
    ("float32[KM]", "float32"),
    ("Dist", "float64"),
]
OPS = [
    ("+", "+ (T) T", "a + b"),
    ("-", "- (T) T", "a - b"),
    ("*", "* (T) T", "a * b"),
    ("/", "/ (T) T", "a / b"),
    ("%", "% (T) T", "a % b"),
    ("&", "& (T) T", "a & b"),
    ("|", "| (T) T", "a | b"),
    ("^", "^ (T) T", "a ^ b"),
    ("&^", "&^ (T) T", "a &^ b"),
    ("*s", "* (S) T", "a * k"),
    ("neg", "- () T", "-a"),
    ("compl", "^ () T", "^a"),
    ("==", "== (T) bool", "a == b"),
    ("<=>", "<=> (T) int", "a <=> b"),
    ("++", "++ ()", "VAR++"),
]


def run(gopp, path, threads="1"):
    r = subprocess.run([gopp, "check", "-check-j=" + threads,
                        "--check-error-limit=0", path],
                       capture_output=True, text=True, timeout=60)
    errs = set()
    for m in re.finditer(r":(\d+):\d+: error\[E\d+\]", r.stderr + r.stdout):
        errs.add(int(m.group(1)))
    return errs


KINDS = ["int", "uint8", "float32", "float64", "complex128"]


def table1(gopp, out):
    lines = HEAD.split("\n")
    # (plain line, generic line) per use and kind
    plain_at, gen_at = {}, {}
    src = list(lines)
    for name, expr, _ in USES:
        for kind in KINDS:
            for form in ("plain", "generic"):
                x, y = ("M", "S") if form == "plain" else ("U", "V")
                e = expr.replace("X", x).replace("Y", y).replace("float64", kind)
                e = e.replace("VARP", "p").replace("VARA", "a").replace(
                    "SEND", "ch").replace("SL", "sl").replace("MP", "mp")
                fn = "%s_%s_%s" % (form, name.replace("-", "_"), kind)
                tps = "" if form == "plain" else "[U unit, V unit]"
                src.append("func %s%s(a, b %s[%s], s %s[%s], k, p %s, n int, "
                           "z uint, ch chan %s[%s], sl []%s[%s], "
                           "mp map[%s[%s]]int) {" %
                           (fn, tps, kind, x, kind, y, kind, kind, x, kind, x,
                            kind, x))
                src.append("\t_ = n; _ = z; _ = k; _ = p; _ = s; _ = b; "
                           "_ = ch; _ = sl; _ = mp")
                if name.startswith(("assign", "incr", "addassign", "mulassign",
                                    "send", "map-key", "slice-elem")):
                    stmt = e
                else:
                    stmt = "_ = " + e
                src.append("\t" + stmt)
                (plain_at if form == "plain" else gen_at)[(name, kind)] = len(src)
                src.append("\t_ = a")
                src.append("}")
    path = os.path.join(out, "t1.gopp")
    open(path, "w").write("\n".join(src) + "\n")
    errs = run(gopp, path)
    errs8 = run(gopp, path, "8")
    bad = errs != errs8
    if bad:
        print("DIFFERENT with 8 body threads")
    print("uses (plain/generic body), columns: " + " ".join(KINDS))
    for name, expr, intended in USES:
        cells = []
        for kind in KINDS:
            p = "E" if plain_at[(name, kind)] in errs else "."
            g = "E" if gen_at[(name, kind)] in errs else "."
            if p == g:
                cells.append(p)
            else:
                cells.append(p + "/" + g)
                if not intended:
                    cells[-1] += "!MISMATCH"
                    bad = True
        print("%-16s %s" % (name, " ".join(cells)))
    return bad


def table2(gopp, out):
    src = HEAD.split("\n")
    where = {}
    for tname, SC in TYPES:
        T = tname
        for oname, elem, use in OPS:
            fn = "%s_%s" % (re.sub(r"\W", "_", tname), re.sub(
                r"\W", "x", oname) + str(len(src)))
            unary = oname in ("neg", "compl")
            if oname == "++":
                plain = "func plain_%s(a %s) { a++ }" % (fn, T)
                inst = "func inst_%s() { var v %s; inc_%s(&v) }" % (fn, T, fn)
                helper = ("func inc_%s[T interface{ ++ () }](p *T) "
                          "{ (*p)++ }" % fn)
            elif oname == "*s":
                sc = SC
                plain = ("func plain_%s(a %s, k %s) %s { return a * k }" %
                         (fn, T, sc, T))
                helper = ("func h_%s[T interface{ * (%s) T }](a T, k %s) T "
                          "{ return a * k }" % (fn, sc, sc))
                inst = ("func inst_%s(a %s, k %s) { _ = h_%s(a, k) }" %
                        (fn, T, sc, fn))
            elif oname in ("==",):
                plain = "func plain_%s(a, b %s) bool { return a == b }" % (fn, T)
                helper = ("func h_%s[T interface{ == (T) bool }](a, b T) bool "
                          "{ return a == b }" % fn)
                inst = "func inst_%s(a, b %s) { _ = h_%s(a, b) }" % (fn, T, fn)
            elif oname == "<=>":
                plain = "func plain_%s(a, b %s) int { return a <=> b }" % (fn, T)
                helper = ("func h_%s[T interface{ <=> (T) int }](a, b T) int "
                          "{ return a <=> b }" % fn)
                inst = "func inst_%s(a, b %s) { _ = h_%s(a, b) }" % (fn, T, fn)
            elif unary:
                op = "-" if oname == "neg" else "^"
                plain = "func plain_%s(a %s) %s { return %sa }" % (fn, T, T, op)
                helper = ("func h_%s[T interface{ %s }](a T) T { return %sa }" %
                          (fn, elem, op))
                inst = "func inst_%s(a %s) { _ = h_%s(a) }" % (fn, T, fn)
            else:
                plain = "func plain_%s(a, b %s) %s { return %s }" % (fn, T, T, use)
                helper = ("func h_%s[T interface{ %s }](a, b T) T { return %s }" %
                          (fn, elem, use))
                inst = "func inst_%s(a, b %s) { _ = h_%s(a, b) }" % (fn, T, fn)
            if oname == "++":
                src.append(helper)
                src.append(plain)
                where[(tname, oname, "plain")] = len(src)
                src.append(inst)
                where[(tname, oname, "inst")] = len(src)
                continue
            src.append(helper)
            src.append(plain)
            where[(tname, oname, "plain")] = len(src)
            src.append(inst)
            where[(tname, oname, "inst")] = len(src)
    path = os.path.join(out, "t2.gopp")
    open(path, "w").write("\n".join(src) + "\n")
    errs = run(gopp, path)
    bad = False
    print("instances (plain use / instantiation), columns: " +
          " ".join(t for t, _ in TYPES))
    for oname, elem, use in OPS:
        row = []
        for tname, _ in TYPES:
            p = "E" if where[(tname, oname, "plain")] in errs else "."
            # an instantiation line also carries the error of the call
            i = "E" if where[(tname, oname, "inst")] in errs else "."
            if p != i:
                row.append(p + "/" + i + "!")
                bad = True
            else:
                row.append(p)
        print("%-8s %s" % (oname, " ".join(row)))
    return bad


PAIRS_RATIO = [("M", "M"), ("KM", "M"), ("M", "KM"), ("KM", "KM")]
PAIRS_PROD = [("M", "IM"), ("KM", "IM"), ("M", "IKM"), ("KM", "IKM")]
KINDS3 = ["float64", "float32", "int"]
# (name, pairs, body with K the kind, A and B the operands; helper-based use)
USES3 = [
    ("ratio", PAIRS_RATIO, "return K(a / b)"),
    ("ratio-var", PAIRS_RATIO, "r := a / b\n\treturn K(r)"),
    ("ratio-sum", PAIRS_RATIO, "return K(a / b) + K(a / b)"),
    ("product", PAIRS_PROD, "return K(a * b)"),
    ("via-helper", PAIRS_RATIO, "return HELP(a, b)"),
]
HEAD3 = """package main

unit M
unit KM = 1000 M
unit IM = 1 / M
unit IKM = 1 / KM
"""


def table3(gopp, out):
    src = HEAD3.split("\n")

    def emit(text):
        src.extend(text.split("\n"))
    plain_range, call_at = {}, {}
    for kind in KINDS3:
        # the helpers: one plain per pair, one generic
        for x, y in PAIRS_RATIO:
            emit("func help_%s_%s_%s(a %s[%s], b %s[%s]) %s { return %s(a / b) }"
                       % (kind, x, y, kind, x, kind, y, kind, kind))
        emit("func help_g_%s[U unit, V unit](a %s[U], b %s[V]) %s "
                   "{ return %s(a / b) }" % (kind, kind, kind, kind, kind))
    for name, pairs, body in USES3:
        for kind in KINDS3:
            gname = "g_%s_%s" % (name.replace("-", "_"), kind)
            gb = body.replace("K(", kind + "(").replace(
                "HELP", "help_g_" + kind)
            emit("func %s[U unit, V unit](a %s[U], b %s[V]) %s {\n\t%s\n}"
                       % (gname, kind, kind, kind, gb))
            emit("func main_%s() {" % gname)
            calls = []
            for x, y in pairs:
                one = "1" if kind == "int" else "1.0"
                emit("\t_ = %s[%s, %s](%s[%s], %s[%s])" %
                     (gname, x, y, one, x, one, y))
                calls.append((x, y, len(src)))
            emit("}")
            for x, y, ln in calls:
                call_at[(name, kind, x, y)] = ln
            for x, y in pairs:
                pname = "p_%s_%s_%s_%s" % (name.replace("-", "_"), kind, x, y)
                pb = body.replace("K(", kind + "(").replace(
                    "HELP", "help_%s_%s_%s" % (kind, x, y))
                start = len(src) + 1
                emit("func %s(a %s[%s], b %s[%s]) %s {" %
                           (pname, kind, x, kind, y, kind))
                for ln in pb.split("\n"):
                    emit(ln if ln.startswith("\t") else "\t" + ln)
                emit("}")
                plain_range[(name, kind, x, y)] = (start, len(src))
    emit("func main() {}")
    path = os.path.join(out, "t3.gopp")
    open(path, "w").write("\n".join(src) + "\n")
    errs = run(gopp, path)
    bad0 = errs != run(gopp, path, "8")
    if bad0:
        print("DIFFERENT with 8 body threads")
    r = subprocess.run([gopp, "check", "--dump-types", "--check-error-limit=0",
                        path], capture_output=True, text=True, timeout=60)
    convs, sites = {}, {}
    for m in re.finditer(r"^(\d+):\d+ unit-conv `[^`]*` (\S+)", r.stdout,
                         re.M):
        convs.setdefault(int(m.group(1)), []).append(m.group(2))
    for m in re.finditer(r"^(\d+):\d+ unit-site `[^`]*` (.*)$", r.stdout, re.M):
        sites.setdefault(int(m.group(1)), []).append(m.group(2))
    bad = bad0 or bool(errs - {ln for rg in plain_range.values()
                               for ln in range(rg[0], rg[1] + 1)})
    print("instances of cancelling units (plain factor / generic), columns: " +
          " ".join("%s/%s" % pr for pr in PAIRS_RATIO) + " | " +
          " ".join("%s/%s" % pr for pr in PAIRS_PROD))
    for name, pairs, _ in USES3:
        for kind in KINDS3:
            gname = "g_%s_%s" % (name.replace("-", "_"), kind)
            glines = [ln for ln in range(1, len(src) + 1)
                      if src[ln - 1].startswith("func " + gname + "[")]
            lo_g = glines[0]
            hi_g = next(ln for ln in range(lo_g, len(src) + 1)
                        if src[ln - 1] == "}")
            gsites = [x for ln in range(lo_g, hi_g + 1) for x in sites.get(ln, [])]
            if name == "via-helper":
                gsites = [x for ln, v in sites.items()
                          if src[ln - 1].startswith("func help_g_%s[" % kind)
                          for x in v]
            cells = []
            for x, y in pairs:
                lo, hi = plain_range[(name, kind, x, y)]
                fs = [f for ln in range(lo, hi + 1) for f in convs.get(ln, [])]
                # the helper's own line is part of a plain "via-helper" use
                if name == "via-helper":
                    fs = [f for ln, v in convs.items()
                          if src[ln - 1].startswith("func help_%s_%s_%s(" %
                                                    (kind, x, y)) for f in v]
                plain = ",".join(fs) if fs else "."
                if any(ln in errs for ln in range(lo, hi + 1)):
                    plain = "E"  # no exact factor for an integer kind
                gs = [site_factor(t, {"U": UNITS3[x], "V": UNITS3[y]},
                                  kind == "int") for t in gsites]
                gs = [g for g in gs if g != "1"]
                gen = ",".join(g if g == "t" else "*" + g for g in gs) or "."
                ok = plain == gen or (plain == "E" and gen == "t")
                cells.append("%s/%s%s" % (plain, gen, "" if ok else "!MISMATCH"))
                bad |= not ok
            print("%-10s %-7s %s" % (name, kind, " ".join(cells)))
    return bad


# The real units of table 3: dimension (base unit -> exponent) and factor.
UNITS3 = {"M": ({"M": 1}, Fraction(1)), "KM": ({"M": 1}, Fraction(1000)),
          "IM": ({"M": -1}, Fraction(1)), "IKM": ({"M": -1}, Fraction(1, 1000))}


def parse_unit(t, env):
    """A unit as the dump prints it (`U/V`, `(5/18) M*U/S`, `1000 M`,
    `1/(M*U)`, `plain`) with the parameters replaced by env."""
    dims, f = {}, Fraction(1)
    if t == "plain":
        return dims, f
    m = re.match(r"^\((\d+)/(\d+)\) (.*)$", t) or re.match(r"^(\d+)() (.*)$", t)
    if m:
        f = Fraction(int(m.group(1)), int(m.group(2) or 1))
        t = m.group(3)
    num, _, den = t.partition("/")
    for part, sign in ((num, 1), (den.strip("()"), -1)):
        for term in part.split("*"):
            if not term or term == "1":
                continue
            name, _, e = term.partition("^")
            e = sign * int(e or 1)
            if name in env:
                d, g = env[name]
            else:
                d, g = {name: 1}, Fraction(1)
            for k, v in d.items():
                dims[k] = dims.get(k, 0) + v * e
            f *= g ** e
    return {k: v for k, v in dims.items() if v}, f


def site_factor(text, env, integer):
    """The factor a collapse site applies at the instance env (Info::UnitSite):
    c(E) is the factor of E where its dimension is empty, else 1. An integer
    `/` whose result is empty converts the coarser operand to the finer unit
    first (EXP-33); a factor that is no integer truncates (`t`)."""
    def c(t):
        d, f = parse_unit(t, env)
        return f if not d else Fraction(1)
    if " from " in text:
        res, x = text.split(" from ")
        r = c(res) / c(x)
    else:
        res, rest = text.split(" = ")
        op = " * " if " * " in rest else " / "
        x, y = rest.split(op)
        if op == " * ":
            r = c(res) / (c(x) * c(y))
        else:
            r = c(res) * c(y) / c(x)
            if integer and not parse_unit(res, env)[0]:
                fx, fy = parse_unit(x, env)[1], parse_unit(y, env)[1]
                r = fx / fy if fx >= fy else fy / fx  # the coarser operand
    if r == 1:
        return "1"
    if integer and r.denominator != 1:
        return "t"
    return str(r.numerator) if r.denominator == 1 else "%d/%d" % (
        r.numerator, r.denominator)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    b1 = table1(a.gopp, a.out)
    b2 = table2(a.gopp, a.out)
    b3 = table3(a.gopp, a.out)
    if b1 or b2 or b3:
        print("MISMATCH")
        sys.exit(1)


main()
