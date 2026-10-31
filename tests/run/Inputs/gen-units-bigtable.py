#!/usr/bin/env python3
"""Writes tests/run/units-bigfactor-table.test and its golden (P04.24 item 1).

A unit factor at run time is an exact fraction of up to 4096 bits each way
(ADR-0025, UNI-2, GEN-14), so generic code computes what plain code computes
for any factor of science. Each row is a pair of units U and V of one
dimension, made of the SI and CODATA factors (the electronvolt, the dalton,
the electron and proton masses, the hartree, the light year, the astronomical
unit, the mile, the inch, the pound, the calorie, the hour...) with random
exponents, so their ratio F = f(U)/f(V) is a random rational with a numerator
and a denominator of up to some hundreds of bits; and a fixed row for each
float32 and float64 boundary (the largest finite value, the ties above it,
the overflow, the smallest subnormal and half of it, and the ties 1 + 2^-24
and 1 + 2^-53 plus a part far below, 2^-1000, that a rounding through 512
bits would lose, made of units whose factors are each at most 512 bits).

Each row runs a / b and a * b' (b' in 1/V) in float64, float32 and int64,
in plain code (float), in a method of an instance of a generic type whose
units the compiler knows (int64: plain code refuses a factor that is no
integer, EXP-35), and in a function generic over the units, whose factors are
made at run time from the descriptors. The random rows also run a / b and
a^2 / b^2 (float64) and a / b (int64) in a method of an instance of a generic
type that calls the generic function, also from a function literal in it:
the units are known there, so it passes their constant descriptors (the
final review of item 1, R1). The program prints the bits of each;
the golden is computed here with exact rationals (fractions.Fraction) and
IEEE rounding (float64 by Python's own arithmetic, float32 by hand, to
nearest even), never by the compiler, so a difference between plain and
generic code, or between either and the exact value, fails the test.

usage: gen-units-bigtable.py --write TEST GOLDEN [--rows N]   write both files
       gen-units-bigtable.py --check TEST GOLDEN [--rows N]   fail if they are not what --write writes

The normal test has 16 random rows (units-bigfactor-table.test, under the 2 s
of a normal test), the long one 96 (units-bigfactor-table.long.test); both
have every boundary row.
"""
import math
import os
import random
import struct
import sys
from fractions import Fraction as Fr

ROWS = 16  # (--rows)
SEED = 20261031

# name -> (declaration, factor in the base units, dimension atom)
DECLS = [
    ("M", "unit M", Fr(1), "L"),
    ("KG", "unit KG", Fr(1), "Ma"),
    ("S", "unit S", Fr(1), "T"),
    ("KM", "unit KM = 1000 M", Fr(1000), "L"),
    ("MI", "unit MI = (1609344 / 1000) M", Fr(1609344, 1000), "L"),
    ("IN", "unit IN = (254 / 10000) M", Fr(254, 10000), "L"),
    ("AU", "unit AU = 149597870700 M", Fr(149597870700), "L"),
    ("LY", "unit LY = 9460730472580800 M", Fr(9460730472580800), "L"),
    ("ANG", "unit ANG = (1 / 10000000000) M", Fr(1, 10 ** 10), "L"),
    ("G", "unit G = (1 / 1000) KG", Fr(1, 1000), "Ma"),
    ("LB", "unit LB = (45359237 / 100000000) KG", Fr(45359237, 10 ** 8), "Ma"),
    ("DA", "unit DA = (166053906660 / 100000000000000000000000000000000000000) KG",
     Fr(166053906660, 10 ** 38), "Ma"),
    ("ME", "unit ME = (91093837015 / 100000000000000000000000000000000000000000) KG",
     Fr(91093837015, 10 ** 41), "Ma"),
    ("MP", "unit MP = (167262192369 / 100000000000000000000000000000000000000) KG",
     Fr(167262192369, 10 ** 38), "Ma"),
    ("MIN", "unit MIN = 60 S", Fr(60), "T"),
    ("H", "unit H = 3600 S", Fr(3600), "T"),
    ("YR", "unit YR = 31557600 S", Fr(31557600), "T"),
    ("NS", "unit NS = (1 / 1000000000) S", Fr(1, 10 ** 9), "T"),
    ("J", "unit J = KG * M^2 / S^2", Fr(1), "E"),
    ("EV", "unit EV = (1602176634 / 10000000000000000000000000000) J",
     Fr(1602176634, 10 ** 28), "E"),
    ("CAL", "unit CAL = (4184 / 1000) J", Fr(4184, 1000), "E"),
    ("KWH", "unit KWH = 3600000 J", Fr(3600000), "E"),
    ("EH", "unit EH = (43597447222071 / 10000000000000000000000000000000) J",
     Fr(43597447222071, 10 ** 31), "E"),
]
FACTOR = {n: f for n, _, f, _ in DECLS}
ATOMS = {}
for n, _, _, a in DECLS:
    ATOMS.setdefault(a, []).append(n)

# The boundary units, each a factor of at most 512 bits over M (float rows only).
B = 2 ** 511 - 1
BOUND_DECLS = [
    ("FMAX", "unit FMAX = 340282346638528859811704183484516925440 M", Fr(340282346638528859811704183484516925440)),
    ("F128", "unit F128 = 340282366920938463463374607431768211456 M", Fr(2 ** 128)),
    ("FTIE", "unit FTIE = 340282356779733661637539395458142568448 M", Fr(2 ** 128 - 2 ** 103)),
    ("TWO400", "unit TWO400 = %d M" % 2 ** 400, Fr(2 ** 400)),
    ("TWO500", "unit TWO500 = %d M" % 2 ** 500, Fr(2 ** 500)),
    ("D53", "unit D53 = %d M" % (2 ** 53 - 1), Fr(2 ** 53 - 1)),
    ("D54", "unit D54 = %d M" % (2 ** 54 - 1), Fr(2 ** 54 - 1)),
    ("TWO71", "unit TWO71 = %d M" % 2 ** 71, Fr(2 ** 71)),
    ("TWO70", "unit TWO70 = %d M" % 2 ** 70, Fr(2 ** 70)),
    ("TWO24", "unit TWO24 = %d M" % 2 ** 24, Fr(2 ** 24)),
    ("TWO74", "unit TWO74 = %d M" % 2 ** 74, Fr(2 ** 74)),
    ("TWO75", "unit TWO75 = %d M" % 2 ** 75, Fr(2 ** 75)),
    ("TWO149", "unit TWO149 = %d M" % 2 ** 149, Fr(2 ** 149)),
    ("TWO150", "unit TWO150 = %d M" % 2 ** 150, Fr(2 ** 150)),
    ("TWO151", "unit TWO151 = %d M" % 2 ** 151, Fr(2 ** 151)),
    ("THREE", "unit THREE = 3 M", Fr(3)),
    ("X24", "unit X24 = (%d / %d) M" % (2 ** 24 + 1, 2 ** 24), Fr(2 ** 24 + 1, 2 ** 24)),
    ("X53", "unit X53 = (%d / %d) M" % (2 ** 53 + 1, 2 ** 53), Fr(2 ** 53 + 1, 2 ** 53)),
    ("Y1", "unit Y1 = (%d / %d) M" % (B + 1, B), Fr(B + 1, B)),
    ("Y2", "unit Y2 = (%d / %d) M" % (B + 2, B + 1), Fr(B + 2, B + 1)),
]
for n, _, f in BOUND_DECLS:
    FACTOR[n] = f
# name, U (products of boundary units, dimension M^k), V (M^k)
BOUNDS = [
    ("f32 max", ["FMAX"], ["M"]),
    ("f32 max tie", ["FTIE"], ["M"]),
    ("2^128", ["F128"], ["M"]),
    ("2^-149", ["M"], ["TWO149"]),
    ("2^-150", ["M"], ["TWO150"]),
    ("3*2^-151", ["THREE"], ["TWO151"]),
    ("f64 max", ["D53", "TWO400", "TWO500", "TWO71"], ["M", "M", "M", "M"]),
    ("f64 max tie", ["D54", "TWO400", "TWO500", "TWO70"], ["M", "M", "M", "M"]),
    ("2^1024", ["TWO500", "TWO500", "TWO24"], ["M", "M", "M"]),
    ("2^-1074", ["M", "M", "M"], ["TWO500", "TWO500", "TWO74"]),
    ("2^-1075", ["M", "M", "M"], ["TWO500", "TWO500", "TWO75"]),
    ("3*2^-1075 tie", ["THREE", "M", "M", "M"], ["TWO500", "TWO500", "TWO75", "M"]),
    ("1+2^-24+far", ["X24", "Y1"], ["M", "Y2"]),
    ("1+2^-53+far", ["X53", "Y1"], ["M", "Y2"]),
    ("1+2^-24", ["X24"], ["M"]),
    ("1+2^-53", ["X53"], ["M"]),
]


def rnd(x, p):
    """The Fraction x rounded once to p significant bits (24: float32, 53:
    float64), to nearest even, with the format's range: a Python float
    (exact), +-inf or +-0."""
    if x == 0:
        return 0.0
    neg = x < 0
    x = abs(x)
    emin, emax = (-126, 127) if p == 24 else (-1022, 1023)
    e = x.numerator.bit_length() - x.denominator.bit_length()
    if Fr(2) ** e > x:
        e -= 1
    if e > emax:
        v = math.inf
    else:
        keep = p if e >= emin else p - (emin - e)
        if keep < 0:
            v = 0.0
        else:
            q2 = x * Fr(2) ** (keep - e)
            q, r = divmod(q2.numerator, q2.denominator)
            hq, half = q >> 1, q & 1
            if half and (r != 0 or hq & 1):
                hq += 1
            vf = hq * Fr(2) ** (e - keep + 1)
            top = Fr(2) ** (emax + 1)
            v = math.inf if vf >= top else float(vf)
    return -v if neg else v


def plain_ok(f, p):
    """Is f a number of the float type (Checker::unitFactorFits: it rounds to
    a finite number that is not 0)? Else plain code refuses it and generic
    code panics."""
    v = rnd(f, p)
    return not math.isinf(v) and v != 0


def f32_mul(a, b):
    """a * b in float32 (a and b float32 values held in floats)."""
    if math.isinf(a) or math.isinf(b) or a == 0 or b == 0:
        return a * b  # (IEEE: the sign, inf and zero; no NaN here)
    return rnd(Fr(a) * Fr(b), 24)


def b64(v):
    return struct.unpack("<Q", struct.pack("<d", v))[0]


def b32(v):
    return struct.unpack("<I", struct.pack("<f", v))[0]


def wrap(v):
    v %= 2 ** 64
    return v - 2 ** 64 if v >= 2 ** 63 else v


def idiv(a, b):
    return int(Fr(a, b))


def ratio_int(a, b, f):
    # EXP-33: the coarser operand to the finer unit (truncating, wrapping in
    # 64 bits), then the quotient, truncating. (A divisor that wrapped to 0
    # would be a division by zero: the seed makes none.)
    if f > 1:
        return idiv(wrap(int(a * f)), b)
    if f < 1:
        d = wrap(int(b / f))
        assert d != 0, "a divisor wrapped to 0"
        return idiv(a, d)
    return idiv(a, b)


def product_int(a, b, f):
    return wrap(int(wrap(a * b) * f))


def spell(names, exps):
    """A unit expression of names with exponents (`EV^2*MI/H^3`, `1/S`)."""
    pos = [(n, e) for n, e in zip(names, exps) if e > 0]
    neg = [(n, -e) for n, e in zip(names, exps) if e < 0]

    def part(n, e):
        return n if e == 1 else "%s^%d" % (n, e)

    s = "*".join(part(n, e) for n, e in pos) or "1"
    for n, e in neg:
        s += "/" + part(n, e)
    return s


def factor(names, exps):
    f = Fr(1)
    for n, e in zip(names, exps):
        f *= FACTOR[n] ** e
    return f


def rows():
    r = random.Random(SEED)
    out = []
    atoms = sorted(ATOMS)
    for i in range(ROWS):
        k = r.randint(1, 3)
        chosen = r.sample(atoms, k)
        exps = [r.choice([-3, -2, -1, 1, 2, 3]) for _ in chosen]
        u = [r.choice(ATOMS[a]) for a in chosen]
        v = [r.choice(ATOMS[a]) for a in chosen]
        out.append(("r%d" % i, spell(u, exps), spell(v, exps),
                    factor(u, exps) / factor(v, exps)))
    for name, us, vs in BOUNDS:
        f = Fr(1)
        for n in us:
            f *= FACTOR[n]
        for n in vs:
            f /= FACTOR[n]
        out.append((name, "*".join(us), "*".join(vs), f))
    return out


# Operand values: a / b and a * b'. (A factor that is no number of the float
# type, past its range or rounded to 0, is plain code's E0435 and generic
# code's "unit out of range" panic (ADR-0025 Amendment point 6): such a line
# is left out; the panics are Inputs/panics-gopp/unit-range-float*.gopp.)
FA, FB = (3, 7)
IA, IB = (123456789012345, 7)


def expected(name, f, ints):
    lines = []
    p64, p32 = plain_ok(f, 53), plain_ok(f, 24)

    for a, b in ((1, 1), (FA, FB), (-FA, FB)):
        f64, f32 = rnd(f, 53), rnd(f, 24)
        if p64:
            v = b64((a / b) * f64)
            lines.append("%s f64 %d/%d %d %d" % (name, a, b, v, v))
            v = b64(float(a * b) * f64)
            lines.append("%s f64 %d*%d %d %d" % (name, a, b, v, v))
            if ints:
                # A method of an instance (units known) calling the generic,
                # directly and from a function literal, with U and U*U.
                v = b64((a / b) * f64)
                lines.append("%s method %d/%d %d %d" % (name, a, b, v, v))
                lines.append("%s literal %d/%d %d %d" % (name, a, b, v, v))
                if plain_ok(f * f, 53):
                    v = b64(float(a * a) / float(b * b) * rnd(f * f, 53))
                    lines.append("%s method %d^2/%d^2 %d %d" % (name, a, b, v, v))
        if p32:
            v = b32(f32_mul(rnd(Fr(a, b), 24), f32))
            lines.append("%s f32 %d/%d %d %d" % (name, a, b, v, v))
            v = b32(f32_mul(rnd(Fr(a * b), 24), f32))
            lines.append("%s f32 %d*%d %d %d" % (name, a, b, v, v))
    for a, b in ((IA, IB), (-IA, IB), (IB, IA)) if ints else ():
        q = ratio_int(a, b, f)
        p = product_int(a, b, f)
        lines.append("%s i64 %d/%d %d %d" % (name, a, b, q, q))
        lines.append("%s i64 %d*%d %d %d" % (name, a, b, p, p))
        lines.append("%s i64-method %d/%d %d %d" % (name, a, b, q, q))
    return lines


def program(rs, gold):
    out = []
    w = out.append
    rows_arg = "" if ROWS == 16 else " --rows %d" % ROWS
    w("// REQUIRES: %shost-linux-amd64 || host-linux-arm64" % ("long, " if ROWS != 16 else ""))
    w("// RUN: %%python %%S/Inputs/gen-units-bigtable.py --check %%s %%S/golden/%s%s" % (gold, rows_arg))
    w("// RUN: %%python %%S/Inputs/run-golden.py --src %%s %%S/golden/%s 0" % gold)
    w("// RUN: %if qemu-aarch64 %{ %gopp build -target=linux/arm64 -crt-dir=%aarch64_sysroot/lib %s -o %t.arm64 %}")
    w("// RUN: %%if qemu-aarch64 %%{ %%python %%S/Inputs/run-golden.py --qemu %%aarch64_sysroot %%t.arm64 %%S/golden/%s 0 %%}" % gold)
    w("// GOLDEN: by-hand")
    w("// P04.24 (TODO item 1): written by Inputs/gen-units-bigtable.py (see there); do")
    w("// not edit. Plain code against generic code for random rationals made of the SI")
    w("// and CODATA factors and for the float32 and float64 boundaries; the golden is")
    w("// computed by the script with exact fractions.")
    w("package main")
    w("")
    w('import "unsafe"')
    w("")
    for _, d, _, _ in DECLS:
        w(d)
    for _, d, _ in BOUND_DECLS:
        w(d)
    w("")
    w("func bits(x float64) uint64   { return *(*uint64)(unsafe.Pointer(&x)) }")
    w("func bits32(x float32) uint32 { return *(*uint32)(unsafe.Pointer(&x)) }")
    w("")
    w("func GQ64[U unit, V unit](a float64[U], b float64[V]) float64 { return float64(a / b) }")
    w("func GP64[U unit, V unit](a float64[U], b float64[1/V]) float64 { return float64(a * b) }")
    w("func GQ32[U unit, V unit](a float32[U], b float32[V]) float32 { return float32(a / b) }")
    w("func GP32[U unit, V unit](a float32[U], b float32[1/V]) float32 { return float32(a * b) }")
    w("func GQI[U unit, V unit](a int64[U], b int64[V]) int64 { return int64(a / b) }")
    w("func GPI[U unit, V unit](a int64[U], b int64[1/V]) int64 { return int64(a * b) }")
    w("")
    w("// The units are known in a method of an instance (the reference for int64).")
    w("type Pair[U unit, V unit] struct {")
    w("\ta int64[U]")
    w("\tb int64[V]")
    w("\tc int64[1/V]")
    w("}")
    w("")
    w("func (p Pair[U, V]) Q() int64 { return int64(p.a / p.b) }")
    w("func (p Pair[U, V]) P() int64 { return int64(p.a * p.c) }")
    w("")
    w("// A method of an instance of a generic type (its units known) calling the")
    w("// generic that takes descriptors: the constant descriptors of its units.")
    w("func (p Pair[U, V]) QG() int64 { return GQI(p.a, p.b) }")
    w("")
    w("type PairF[U unit, V unit] struct {")
    w("\ta float64[U]")
    w("\tb float64[V]")
    w("}")
    w("")
    w("func (p PairF[U, V]) Q() float64  { return GQ64(p.a, p.b) }")
    w("func (p PairF[U, V]) QQ() float64 { return GQ64(p.a*p.a, p.b*p.b) }")
    w("func (p PairF[U, V]) QL() float64 {")
    w("\tf := func() float64 { return GQ64(p.a, p.b) }")
    w("\treturn f()")
    w("}")
    w("")
    for i, (name, u, v, _) in enumerate(rs):
        w("type T%d = Pair[%s, %s]" % (i, u, v))
        if i < ROWS:
            w("type TF%d = PairF[%s, %s]" % (i, u, v))
    w("")
    w("func main() {")
    for i, (name, u, v, _) in enumerate(rs):
        f = rs[i][3]
        p64, p32 = plain_ok(f, 53), plain_ok(f, 24)
        for a, b in ((1, 1), (FA, FB), (-FA, FB)):
            w("\t{")
            if p64:
                w("\t\ta, b, c := %d.0[%s], %d.0[%s], %d.0[1/(%s)]" % (a, u, b, v, b, v))
                w('\t\tprintln("%s", "f64", "%d/%d", bits(float64(a/b)), bits(GQ64(a, b)))' % (name, a, b))
                w('\t\tprintln("%s", "f64", "%d*%d", bits(float64(a*c)), bits(GP64(a, c)))' % (name, a, b))
                if i < ROWS:
                    w("\t\tq := TF%d{a, b}" % i)
                    w('\t\tprintln("%s", "method", "%d/%d", bits(float64(a/b)), bits(q.Q()))' % (name, a, b))
                    w('\t\tprintln("%s", "literal", "%d/%d", bits(float64(a/b)), bits(q.QL()))' % (name, a, b))
                    if plain_ok(f * f, 53):
                        w('\t\tprintln("%s", "method", "%d^2/%d^2", bits(float64((a*a)/(b*b))), bits(q.QQ()))' % (name, a, b))
            if p32:
                w("\t\ta32, b32, c32 := float32[%s](%d), float32[%s](%d), float32[1/(%s)](%d)" % (u, a, v, b, v, b))
                w('\t\tprintln("%s", "f32", "%d/%d", bits32(float32(a32/b32)), bits32(GQ32(a32, b32)))' % (name, a, b))
                w('\t\tprintln("%s", "f32", "%d*%d", bits32(float32(a32*c32)), bits32(GP32(a32, c32)))' % (name, a, b))
            w("\t}")
        for a, b in ((IA, IB), (-IA, IB), (IB, IA)) if i < ROWS else ():
            w("\t{")
            w("\t\tp := T%d{int64[%s](%d), int64[%s](%d), int64[1/(%s)](%d)}" % (i, u, a, v, b, v, b))
            w('\t\tprintln("%s", "i64", "%d/%d", p.Q(), GQI(p.a, p.b))' % (name, a, b))
            w('\t\tprintln("%s", "i64", "%d*%d", p.P(), GPI(p.a, p.c))' % (name, a, b))
            w('\t\tprintln("%s", "i64-method", "%d/%d", p.Q(), p.QG())' % (name, a, b))
            w("\t}")
    w("}")
    w("")
    return "\n".join(out) + "\n"


def golden(rs):
    lines = []
    for i, (name, _, _, f) in enumerate(rs):
        lines += expected(name, f, i < ROWS)
    return "\n".join(lines) + "\n"


def main():
    global ROWS
    args = sys.argv[1:]
    if len(args) == 5 and args[3] == "--rows":
        ROWS = int(args[4])
        args = args[:3]
    if len(args) != 3 or args[0] not in ("--write", "--check"):
        sys.exit(__doc__)
    sys.argv[1:] = args
    rs = rows()
    test, gold = program(rs, os.path.basename(sys.argv[3])), golden(rs)
    if sys.argv[1] == "--write":
        with open(sys.argv[2], "w") as f:
            f.write(test)
        with open(sys.argv[3], "w") as f:
            f.write(gold)
        return
    bad = []
    for path, want in ((sys.argv[2], test), (sys.argv[3], gold)):
        with open(path) as f:
            if f.read() != want:
                bad.append(path)
    if bad:
        sys.exit("not what gen-units-bigtable.py --write writes: " + ", ".join(bad))


if __name__ == "__main__":
    main()
