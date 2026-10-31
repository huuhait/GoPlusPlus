#!/usr/bin/env python3
"""Writes tests/run/units-generic-table.test and its golden (P04.24, GEN-14).

The table runs the same operation on units twice: in plain code, whose units
the checker knows (every scale change is its constant), and in a body generic
over unit parameters, whose units are the instance's. Each row prints the
plain value, the generic value and whether they are equal. The expected values
are computed here with exact rationals (fractions.Fraction), never by the
compiler, and printed the way println prints a float.

usage: gen-units-table.py --write TEST GOLDEN   write both files
       gen-units-table.py --check TEST GOLDEN   fail if they are not what --write writes
"""
import sys
from fractions import Fraction

# unit name -> (factor over its base unit, base unit, exponent of the base)
UNITS = {"M": (Fraction(1), "M"), "KM": (Fraction(1000), "M"),
         "IM": (Fraction(1), "IM"), "IKM": (Fraction(1, 1000), "IM")}
# A pair is (name, unit of a, unit of b).
RATIO = [("M", "M"), ("KM", "M"), ("M", "KM"), ("KM", "KM")]
PRODUCT = [("M", "IM"), ("KM", "IM"), ("M", "IKM"), ("KM", "IKM")]
# A pair of compound units: (name of the plain unit pair, generic unit
# arguments U and V of `a T[U*KM], b T[V*M]`, factor of each over S*M).
SECS = {"S": Fraction(1), "H": Fraction(3600)}
COMPOUND = [("S", "S"), ("H", "S"), ("S", "H")]
KINDS = [("f64", "float64"), ("f32", "float32"), ("int", "int"), ("i64", "int64")]
INTKINDS = ("int", "i64")


def fmt_float(x):
    s = "%+.6e" % float(x)
    mant, exp = s.split("e")
    sign, digits = exp[0], exp[1:].lstrip("0").rjust(3, "0")
    return "%se%s%s" % (mant, sign, digits)


def trunc(x):
    return int(x) if x >= 0 else -int((-x))


def expect(kind, op, ua, ub, neg=False, fa=None, fb=None):
    fa = UNITS[ua][0] if fa is None else fa
    fb = UNITS[ub][0] if fb is None else fb
    if kind in INTKINDS:
        a, b = (-6 if neg else 6), 4
        if op == "ratio":
            # EXP-33: the coarser operand goes to the finer unit, then divide.
            if fa > fb:
                return trunc(Fraction(a * fa / fb) / b)
            if fa < fb:
                return trunc(Fraction(a) / (b * fb / fa))
            return trunc(Fraction(a, b))
        return a * b * fa * fb  # product: the factor of the result
    a, b = Fraction(-3 if neg else 3), Fraction(2)
    if op == "ratio":
        return a / b * fa / fb
    return a * b * fa * fb


def num(kind, v):
    return fmt_float(v) if kind not in INTKINDS else str(int(v))


def source():
    L = []
    w = L.append
    w("package main\n")
    w("unit M\nunit S\nunit KM = 1000 M\nunit H = 3600 S\nunit IM = 1 / M\nunit IKM = 1 / KM\n")
    for k, t in KINDS:
        K = k.upper()
        w("func gRatio%s[U unit, V unit](a %s[U], b %s[V]) %s { return %s(a / b) }" % (K, t, t, t, t))
        w("func gVia%s[U unit, V unit](a %s[U], b %s[V]) %s { return gRatio%s(a, b) }" % (K, t, t, t, K))
        w("func gProd%s[U unit, V unit](a %s[U], b %s[V]) %s { return %s(a * b) }" % (K, t, t, t, t))
        # Compound units that mention the parameters: U*KM and V*M.
        w("func gRatioC%s[U unit, V unit](a %s[U*KM], b %s[V*M]) %s { return %s(a / b) }" % (K, t, t, t, t))
        # min and max of two values in U*KM and U*M: the coarser goes to the
        # finer unit first, as in plain code (a type that mentions U needs
        # no boxing).
        # (Integers only: min and max of floats are no codegen yet.)
        for fn in ("min", "max") if k in INTKINDS else ():
            w("func g%s%s[U unit](a %s[U*KM], b %s[U*M]) %s[U*M] { return %s(a, b) }"
              % (fn.capitalize(), K, t, t, t, fn))
            w("func p%s%s(a %s[S*KM], b %s[S*M]) %s[S*M] { return %s(a, b) }"
              % (fn.capitalize(), K, t, t, t, fn))
    w("")
    w("func gPlus[U unit](x float64[U]) float64[U] { return x + 2.0[U] }")
    w("func gConv[U unit](x float64[U]) float64[U*KM/M] { return x.In[U*KM/M]() }")
    w("func pPlusM(x float64[M]) float64[M] { return x + 2.0[M] }")
    w("func pPlusKM(x float64[KM]) float64[KM] { return x + 2.0[KM] }")
    w("func pConvM(x float64[M]) float64[KM] { return x.In[KM]() }")
    w("func pConvKM(x float64[KM]) float64[KM*KM/M] { return x.In[KM*KM/M]() }")
    w("")
    # Typed unit constants on both sides of every operator, and in case:
    # the constants are in KM and the variable in M (plain) or in U*M with the
    # constant in U*KM (generic).
    w("const CI int[KM] = 4[KM]")
    w("const CF float64[KM] = 4.0[KM]")
    for op, name in (("+", "Add"), ("-", "Sub")):
        w("func pCL%s(x int[M]) int[M] { return CI %s x }" % (name, op))
        w("func pCR%s(x int[M]) int[M] { return x %s CI }" % (name, op))
        w("func pFL%s(x float64[M]) float64[M] { return CF %s x }" % (name, op))
        w("func pFR%s(x float64[M]) float64[M] { return x %s CF }" % (name, op))
        w("func gCL%s[U unit](x int[U*M]) int[U*M] { return int[U*KM](4) %s x }" % (name, op))
        w("func gCR%s[U unit](x int[U*M]) int[U*M] { return x %s int[U*KM](4) }" % (name, op))
        w("func gFL%s[U unit](x float64[U*M]) float64[U*M] { return float64[U*KM](4) %s x }" % (name, op))
        w("func gFR%s[U unit](x float64[U*M]) float64[U*M] { return x %s float64[U*KM](4) }" % (name, op))
    for op, name in (("<", "Lt"), ("==", "Eq"), (">=", "Ge")):
        w("func pCmpL%s(x int[M]) bool { return CI %s x }" % (name, op))
        w("func pCmpR%s(x int[M]) bool { return x %s CI }" % (name, op))
        w("func gCmpL%s[U unit](x int[U*M]) bool { return int[U*KM](4) %s x }" % (name, op))
        w("func gCmpR%s[U unit](x int[U*M]) bool { return x %s int[U*KM](4) }" % (name, op))
    w("func pCase(x int[M]) int { switch x { case CI: return 1 }; return 0 }")
    w("func gCase[U unit](x int[U*M]) int { switch x { case int[U*KM](4): return 1 }; return 0 }")
    w("")
    # A method of a generic type: in plain code (its unit is known) and one
    # that needs no unit, called from generic code.
    w("type Vec[U unit] struct{ X float64[U] }")
    w("func (v Vec[U]) Over(w float64[M]) float64 { return float64(v.X / w) }")
    w("type Row[U unit] struct{ X float64[U] }")
    w("func (v Row[U]) Twice() Row[U] { return Row[U]{v.X + v.X} }")
    w("func gTwice[U unit](v Row[U]) float64 { return float64(v.Twice().X) }")
    w("func pTwiceM(x float64[M]) float64 { return float64(Row[M]{x}.Twice().X) }")
    w("")
    rows = []  # (label, call plain, call generic, kind, expected)
    for k, t in KINDS:
        K = k.upper()
        isint = k in INTKINDS
        for op, pairs in (("ratio", RATIO), ("product", PRODUCT)):
            for neg in (False, True):
                for ua, ub in pairs:
                    if isint and op == "product" and ub == "IKM" and ua == "M":
                        continue  # the factor 1/1000 is no integer: plain code refuses it
                    a, b = (("-6" if neg else "6"), "4") if isint else (("-3.0" if neg else "3.0"), "2.0")
                    name = "p%s%s_%s_%s%s" % (op.capitalize(), K, ua, ub, "N" if neg else "")
                    body = "a / b" if op == "ratio" else "a * b"
                    w("func %s(a %s[%s], b %s[%s]) %s { return %s(%s) }" % (name, t, ua, t, ub, t, t, body))
                    lit_a = a + "[%s]" % ua
                    lit_b = b + "[%s]" % ub
                    g = ("gRatio" if op == "ratio" else "gProd") + K
                    tag = " neg" if neg else ""
                    rows.append(("%s %s %s %s%s" % (op, k, ua, ub, tag),
                                 "%s(%s, %s)" % (name, lit_a, lit_b),
                                 "%s(%s, %s)" % (g, lit_a, lit_b), k, expect(k, op, ua, ub, neg)))
                    if op == "ratio":
                        rows.append(("via %s %s %s%s" % (k, ua, ub, tag),
                                     "%s(%s, %s)" % (name, lit_a, lit_b),
                                     "gVia%s(%s, %s)" % (K, lit_a, lit_b), k, expect(k, op, ua, ub, neg)))
        # Compound units: a T[S*KM] / b T[S*M] and so on, the generic one
        # with the unit arguments U*KM and V*M (U, V in S, H).
        for neg in (False, True):
            for us, vs in COMPOUND:
                fa, fb = SECS[us] * 1000, SECS[vs]
                if isint:
                    hi, lo = max(fa, fb), min(fa, fb)
                    if (hi / lo).denominator != 1:
                        continue  # a factor that is no integer: plain code refuses it
                a, b = (("-6" if neg else "6"), "4") if isint else (("-3.0" if neg else "3.0"), "2.0")
                name = "pRatioC%s_%s_%s%s" % (K, us, vs, "N" if neg else "")
                w("func %s(a %s[%s*KM], b %s[%s*M]) %s { return %s(a / b) }" % (name, t, us, t, vs, t, t))
                lit_a = a + "[%s*KM]" % us
                lit_b = b + "[%s*M]" % vs
                tag = " neg" if neg else ""
                rows.append(("compound %s %s*KM / %s*M%s" % (k, us, vs, tag),
                             "%s(%s, %s)" % (name, lit_a, lit_b),
                             "gRatioC%s[%s, %s](%s, %s)" % (K, us, vs, lit_a, lit_b), k,
                             expect(k, "ratio", None, None, neg, fa, fb)))
    # min and max: 2 KM against 1999 M is 2000 M and 1999 M.
    for k, t in KINDS:
        K = k.upper()
        if k not in INTKINDS:
            continue
        lit_a = "2[S*KM]"
        lit_b = "1999[S*M]"
        for fn, v in (("min", 1999), ("max", 2000)):
            rows.append(("%s %s" % (fn, k),
                         "p%s%s(%s, %s)" % (fn.capitalize(), K, lit_a, lit_b),
                         "g%s%s[S](%s, %s)" % (fn.capitalize(), K, lit_a, lit_b), k, v))
    w("")
    w("func row(name string, p, g float64, k string) {")
    w("\tprintln(name, p, g, p == g)")
    w("}")
    w("func irow(name string, p, g int) {")
    w("\tprintln(name, p, g, p == g)")
    w("}")
    w("func brow(name string, p, g bool) {")
    w("\tprintln(name, p, g, p == g)")
    w("}")
    w("")
    w("func main() {")
    for label, p, g, k, _ in rows:
        if k in INTKINDS:
            w('\tirow("%s", int(%s), int(%s))' % (label, p, g))
        else:
            w('\trow("%s", float64(%s), float64(%s), "%s")' % (label, p, g, k))
    extra = []
    for u, pp in (("M", "pPlusM"), ("KM", "pPlusKM")):
        w('\trow("const %s", float64(%s(3.0[%s])), float64(gPlus(3.0[%s])), "f64")' % (u, pp, u, u))
        extra.append(("const " + u, Fraction(5)))
    w('\trow("conv M", float64(pConvM(3.0[M])), float64(gConv(3.0[M])), "f64")')
    w('\trow("conv KM", float64(pConvKM(3.0[KM])), float64(gConv(3.0[KM])), "f64")')
    extra.append(("conv M", Fraction(3, 1000)))
    extra.append(("conv KM", Fraction(3, 1000)))
    # Typed constants: x in M (or U*M with U = S), the constant 4 KM = 4000 M.
    for x, (nm, v) in (("7", ("pos", 7)), ("-7", ("neg", -7))):
        for op, name, fn in (("+", "Add", lambda c, y: c + y), ("-", "Sub", lambda c, y: c - y)):
            for side in ("L", "R"):
                exp_i = fn(4000, v) if side == "L" else fn(v, 4000)
                w('\tirow("const int %s%s %s", int(pC%s%s(%s[M])), int(gC%s%s(%s[S*M])), "int")'.replace(', "int")', ")")
                  % (side, name, nm, side, name, x, side, name, x))
                extra.append(("const int %s%s %s" % (side, name, nm), exp_i))
            for side in ("L", "R"):
                xf = x + ".0"
                exp_f = fn(4000, v) if side == "L" else fn(v, 4000)
                w('\trow("const float %s%s %s", float64(pF%s%s(%s[M])), float64(gF%s%s(%s[S*M])), "f64")'
                  % (side, name, nm, side, name, xf, side, name, xf))
                extra.append(("const float %s%s %s" % (side, name, nm), Fraction(exp_f)))
    cmps = {"Lt": lambda c, y: c < y, "Eq": lambda c, y: c == y, "Ge": lambda c, y: c >= y}
    bools = extra  # (one list: the rows are printed in the order they are written)
    for x in (3999, 4000, 4001):
        for name, f in cmps.items():
            for side in ("L", "R"):
                w('\tbrow("cmp %s%s %d", pCmp%s%s(%d[M]), gCmp%s%s(%d[S*M]))' % (side, name, x, side, name, x, side, name, x))
                bools.append(("cmp %s%s %d" % (side, name, x), f(4000, x) if side == "L" else f(x, 4000)))
    for x in (3999, 4000):
        w('\tirow("case %d", pCase(%d[M]), gCase(%d[S*M]))' % (x, x, x))
        extra.append(("case %d" % x, 1 if x == 4000 else 0))
    # Methods of generic types: plain code with a known unit (Vec[KM] over 500 M
    # is 2 KM / 500 M = 4), and a method that needs no unit, from generic code.
    w('\trow("method over", float64(Vec[KM]{2.0[KM]}.Over(500.0[M])), 4.0, "f64")')
    extra.append(("method over", Fraction(4)))
    w('\trow("method twice", float64(pTwiceM(3.0[M])), float64(gTwice(Row[S]{3.0[S]})), "f64")')
    extra.append(("method twice", Fraction(6)))
    w("}")
    out = []
    for label, _, _, k, v in rows:
        out.append("%s %s %s true" % (label, num(k, v), num(k, v)))
    for label, v in extra:
        if label.startswith(("const int", "case")):
            out.append("%s %d %d true" % (label, int(v), int(v)))
        elif label.startswith("cmp"):
            out.append("%s %s %s true" % (label, str(v).lower(), str(v).lower()))
        else:
            out.append("%s %s %s true" % (label, fmt_float(v), fmt_float(v)))
    return "\n".join(L) + "\n", "\n".join(out) + "\n"


HEADER = """// REQUIRES: host-linux-amd64 || host-linux-arm64
// RUN: %python %S/Inputs/gen-units-table.py --check %s %S/golden/units-generic-table.out
// RUN: %python %S/Inputs/run-golden.py --src %s %S/golden/units-generic-table.out 0
// RUN: %if qemu-aarch64 %{ %gopp build -target=linux/arm64 -crt-dir=%aarch64_sysroot/lib %s -o %t.arm64 %}
// RUN: %if qemu-aarch64 %{ %python %S/Inputs/run-golden.py --qemu %aarch64_sysroot %t.arm64 %S/golden/units-generic-table.out 0 %}
// GOLDEN: by-hand
// P04.24 (GEN-14): the parity table at run time. Each row runs one operation
// on units twice, in plain code (its units are known: every scale change is a
// constant) and in a body generic over unit parameters (the run-time descriptors
// of its units decide the collapse sites, Info::UnitSites), for float64,
// float32 and int:
// the ratio of two lengths (M and KM in either order, 1000 or 1/1000), the
// same through a second generic, the product of a length and an inverse
// length (IM = 1/M, IKM = 1/KM: 1, 1000 or 1/1000), a constant of a unit with
// a parameter, and a scale change in the body. A row prints the plain value,
// the generic value and whether they are equal; the expected values are
// computed by Inputs/gen-units-table.py with exact fractions (the compiler is
// not asked), and the same script, run with --check, fails if this file is not
// what it writes. The table also runs negative operands, int64, compound
// units that mention the parameters (`a T[U*KM] / b T[V*M]` with U, V in S, H),
// typed unit constants on both sides of `+`, `-`, `<`, `==`, `>=` and in a
// `case` (a constant in U*KM against a value in U*M), and methods of generic
// types (a unit method with a known unit; one that needs no unit, from generic
// code), and min and max of two values in U*KM and U*M. Integers: 6 U / 4 V takes the coarser operand to the finer
// unit first, in generic code too (EXP-33); the product M * IKM is left out,
// its factor 1/1000 is no integer and plain code refuses it.
"""

if __name__ == "__main__":
    mode, test_path, golden_path = sys.argv[1:4]
    src, golden = source()
    text = HEADER + src
    if mode == "--write":
        open(test_path, "w").write(text)
        open(golden_path, "w").write(golden)
    else:
        if open(test_path).read() != text or open(golden_path).read() != golden:
            sys.exit("%s or %s is not what %s --write writes" % (test_path, golden_path, sys.argv[0]))
