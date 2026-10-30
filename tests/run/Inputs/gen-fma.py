#!/usr/bin/env python3
"""Generates random float programs for FMA contraction (P04.13).

usage: gen-fma.py SEED FUNCS > program.gopp
       gen-fma.py --write DIR [--go GO]       # Inputs/opt/fma-*.gopp

gc fuses `x*y + z` into one FMA on arm64 and never on amd64 (D4.12). A
product that is kept in a variable, copied, negated, read in another block or
read twice is fused the same way (`t := x*y; r := t + z`), a conversion
(`float64(x*y)`) rounds and stops the fusing and a variable that two
branches write is a phi and is not a product. (A variable whose address is
taken is not here: gc sees through a pointer that does not escape, and fuses
the loads' products, which this compiler keeps apart; core-fma-vars.test
checks the one case that is the same.) These programs mix all of that at random, over values for
which fused and unfused results differ in the last bits, so any difference
from gc shows. The expected output is Go's on each target (gen-opt.py's
goldens): amd64's and the one from Go's arm64 build under QEMU.

Constant factors, as gc's generic rules see them before it fuses: `x*2`
and `x/0.5` are `x+x` (not a product), `x*1` and `x/1` are x, `x*-1` is
-x, `x/2^k` is the product `x*2^-k`, and `x*-2` stays a product; the
constant is a literal or a variable written once with one (`two`, `half`,
`mtwo`, `one`), which gc's SSA sees as the constant.

The generator keeps to what gc decides by the source: an add or subtract has
at most one operand that may hold a product (when both are products, gc fuses
the one that comes later in its SSA order, which the source does not decide).
"""
import argparse
import importlib.util
import os
import random
import sys

# Values with x*y not exactly representable: (1 + 2^-27)(1 - 2^-27) and friends.
SENSITIVE = ["1.0000000074505806", "0.9999999925494194", "-1.0000000149011612",
             "1.0000000298023224", "0.1", "10.0", "-1.0", "3.0", "1.0000001"]


class Gen:
    def __init__(self, seed, nfuncs):
        self.r = random.Random(seed)
        self.n = nfuncs
        self.lines = []
        self.depth = 0
        self.fresh = 0

    def emit(self, s):
        self.lines.append("\t" * (self.depth + 1) + s)

    def plain(self):
        """An operand that never holds a product."""
        r = self.r
        return r.choice(self.nonprod + ["a", "b", "c", "d"] + [r.choice(SENSITIVE) for _ in range(2)])

    def var_(self):
        """A non-constant operand (a constant times a constant would be a
        constant expression, which Go evaluates exactly)."""
        return self.r.choice(self.vars + ["a", "b", "c", "d"])

    def pvar(self):
        return self.r.choice(self.nonprod + ["a", "b", "c", "d"])

    def anyop(self):
        return self.r.choice(self.vars + ["a", "b", "c", "d"] + [self.r.choice(SENSITIVE)])

    # A constant factor: (text, what A*K or A/K is). "plain": x+x; "same": A
    # itself; "neg": -A; "prod": a product.
    MULS = [("2", "plain"), ("two", "plain"), ("1", "same"), ("one", "same"),
            ("-1", "neg"), ("-2", "prod"), ("mtwo", "prod"), ("half", "prod"),
            ("4", "prod")]
    DIVS = [("2", "prod"), ("two", "prod"), ("4", "prod"), ("0.25", "prod"),
            ("half", "plain"), ("1", "same"), ("-1", "neg"), ("3", "plain"),
            ("mtwo", "prod")]

    def const_factor(self, v):
        r = self.r
        a = self.var_()
        if r.random() < 0.5:
            k, kind = r.choice(self.MULS)
            e = "%s * %s" % (a, k) if r.random() < 0.5 else "%s * %s" % (k, a)
        else:
            k, kind = r.choice(self.DIVS)
            e = "%s / %s" % (a, k)
        self.emit("%s = %s" % (v, e))
        if kind == "plain":
            self.mark_plain(v)
        elif kind == "prod":
            self.mark_prod(v)
        elif a in self.prod:
            # A product's copy or negation is a product.
            self.mark_prod(v)
        elif kind == "same" and (a in self.nonprod or a in "abcd"):
            self.mark_plain(v)
        else:
            self.mark_unknown(v)

    def stmt(self, depth):
        r = self.r
        v = r.choice(self.vars)
        k = r.random()
        if k < 0.08:
            self.const_factor(v)
            return
        k = r.random()
        if k < 0.20:
            # v = A * B : v holds a product
            self.emit("%s = %s * %s" % (v, self.var_(), self.anyop()))
            self.mark_prod(v)
        elif k < 0.40:
            # one product operand at most: a product (inline or in a
            # variable of prod) and a plain operand
            op = r.choice(["+", "-"])
            if self.prod and r.random() < 0.5:
                p = r.choice(sorted(self.prod))
                a, b = (p, self.plain()) if r.random() < 0.5 else (self.plain(), p)
            else:
                m = "%s * %s" % (self.var_(), self.anyop())
                a, b = (m, self.plain()) if r.random() < 0.5 else (self.plain(), m)
            self.emit("%s = %s %s %s" % (v, a, op, b))
            self.mark_plain(v)
        elif k < 0.48:
            self.emit("%s = float64(%s * %s)" % (v, self.var_(), self.anyop()))
            self.mark_plain(v)
        elif k < 0.54 and self.prod:
            # a conversion of a product variable
            p = r.choice(sorted(self.prod))
            self.emit("%s = float64(%s) %s %s" % (v, p, r.choice(["+", "-"]), self.plain()))
            self.mark_plain(v)
        elif k < 0.60 and v in self.nonprod:
            self.emit("%s %s= %s * %s" % (v, r.choice(["+", "-"]), self.var_(), self.anyop()))
            # v op= A*B: v is the plain operand, the product is inline.
            self.mark_plain(v)
        elif k < 0.64:
            self.emit("%s = -(%s * %s)" % (v, self.var_(), self.anyop()))
            self.mark_prod(v)
        elif k < 0.67:
            self.emit("%s = -%s" % (v, r.choice(self.vars)))
            self.mark_unknown(v)
        elif k < 0.71 and self.prod:
            p = r.choice(sorted(self.prod))
            self.emit("%s = %s" % (v, p))
            self.mark_unknown(v)
        elif k < 0.79 and depth < 2:
            # a variable written in both branches: a phi, not a product
            self.emit("if %s %s %s {" % (self.var_(), r.choice(["<", ">"]), self.anyop()))
            self.depth += 1
            snapshot = (set(self.prod), set(self.nonprod))
            self.emit("%s = %s * %s" % (v, self.var_(), self.anyop()))
            self.mark_prod(v)  # in this branch v holds a product
            for _ in range(r.randint(0, 2)):
                self.stmt(depth + 1)
            self.depth -= 1
            self.emit("} else {")
            self.depth += 1
            # the other branch starts from the state before the first one
            self.prod, self.nonprod = set(snapshot[0]), [x for x in self.nonprod if x in snapshot[1]]
            self.emit("%s = %s" % (v, self.plain()))
            self.mark_unknown(v)
            for _ in range(r.randint(0, 2)):
                self.stmt(depth + 1)
            self.depth -= 1
            self.emit("}")
            # after the merge nothing is a known product, and everything
            # written in a branch may be one: be careful with it.
            self.prod = set(); self.nonprod = [x for x in self.nonprod if x in snapshot[1] and x != v]
            self.unknown = set(self.vars)
        elif k < 0.84 and depth < 2:
            # a product in a variable read in a later block (a branch, a loop)
            self.emit("%s = %s * %s" % (v, self.var_(), self.anyop()))
            self.mark_prod(v)
            self.emit("if %s > %s {" % (self.var_(), self.anyop()))
            self.depth += 1
            w = r.choice([x for x in self.vars if x != v])
            self.emit("%s = %s %s %s" % (w, v, r.choice(["+", "-"]), self.plain()))
            self.depth -= 1
            self.emit("}")
            self.mark_plain(w)
        elif k < 0.90 and depth < 2:
            # an accumulation: the sum of a loop reads a fresh product
            w = r.choice([x for x in self.vars if x in self.nonprod] or [None])
            if w is None:
                return
            self.emit("for i := 0; i < %d; i++ {" % r.randint(2, 3))
            self.depth += 1
            self.emit("%s = %s %s %s * float64(i + 1)" % (w, w, r.choice(["+", "-"]), self.var_()))
            self.depth -= 1
            self.emit("}")
            self.mark_plain(w)
        else:
            self.emit("%s = %s %s %s" % (v, self.pvar(), r.choice(["+", "-", "/"]), self.plain()))
            self.mark_plain(v)

    # State: prod = variables that hold a product now (assigned from A*B in
    # straight-line code), nonprod = variables known to hold a plain value.
    def mark_prod(self, v):
        self.prod.add(v)
        self.nonprod = [x for x in self.nonprod if x != v]

    def mark_plain(self, v):
        self.prod.discard(v)
        if v not in self.nonprod:
            self.nonprod.append(v)

    def mark_unknown(self, v):
        self.prod.discard(v)
        self.nonprod = [x for x in self.nonprod if x != v]

    def func(self, fi):
        self.lines = []
        self.depth = 0
        self.fresh = 0
        self.vars = ["v1", "v2", "v3", "v4"]
        self.prod = set()
        self.nonprod = ["v1", "v2", "v3", "v4"]
        self.unknown = set()
        self.lines.append("//go:noinline")
        self.lines.append("func f%d(a, b, c, d float64) float64 {" % fi)
        self.emit("v1, v2, v3, v4 := a, b, c, d")
        self.emit("two, half, mtwo, one := 2.0, 0.5, -2.0, 1.0")
        self.emit("_, _, _, _ = two, half, mtwo, one")
        for _ in range(self.r.randint(4, 9)):
            self.stmt(0)
        self.emit("return sum4(v1, v2, v3, v4)")
        self.lines.append("}")
        self.lines.append("")
        return self.lines

    def program(self):
        out = ["package main", "", "// Generated by tests/run/Inputs/gen-fma.py.", "",
               'import "unsafe"', "",
               "// bits shows the last bit (println prints 7 digits); every NaN is one pattern.",
               "// sum4 adds what the function made, after the call: no product of the function is in an add with another.",
               "//go:noinline", "func sum4(a, b, c, d float64) float64 {", "\treturn a + b - c + d", "}", "",
               "func bits(x float64) uint64 {", "\tif x != x {", "\t\treturn 1 << 63", "\t}",
               "\treturn *(*uint64)(unsafe.Pointer(&x))", "}", ""]
        for fi in range(self.n):
            out += self.func(fi)
        out.append("func main() {")
        out.append("\targs := [...]float64{%s}" % ", ".join(SENSITIVE))
        out.append("\tfor i := 0; i < len(args); i++ {")
        out.append("\t\ta, b, c, d := args[i], args[(i+3)%len(args)], args[(i+5)%len(args)], args[(i+7)%len(args)]")
        for fi in range(self.n):
            out.append("\t\tprintln(%d, bits(f%d(a, b, c, d)))" % (fi, fi))
        out.append("\t}")
        out.append("}")
        return "\n".join(out) + "\n"


def fma(seed, nfuncs):
    return Gen(seed, nfuncs).program()


SEEDS = [1, 2, 3, 4]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seed", nargs="?", type=int)
    ap.add_argument("funcs", nargs="?", type=int)
    ap.add_argument("--write", metavar="DIR")
    ap.add_argument("--go", default=None)
    a = ap.parse_args()
    if a.write:
        spec = importlib.util.spec_from_file_location(
            "gen_opt", os.path.join(os.path.dirname(os.path.abspath(__file__)), "gen-opt.py"))
        gen_opt = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(gen_opt)
        for seed in SEEDS:
            path = os.path.join(a.write, "opt", "fma-%d.gopp" % seed)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w").write(fma(seed, 40))
            if a.go:
                gen_opt.goldens(path, a.go)
        return
    if a.seed is None or a.funcs is None:
        ap.error("SEED and FUNCS, or --write")
    sys.stdout.write(fma(a.seed, a.funcs))


if __name__ == "__main__":
    main()
