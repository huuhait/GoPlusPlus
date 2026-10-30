#!/usr/bin/env python3
"""Generates random Go-shaped programs (P04.13).

usage: gen-shape.py SEED FUNCS > program.gopp
       gen-shape.py --write DIR [--go GO]     # the programs of Inputs/opt/shape-*.gopp

A program has FUNCS functions over integers of several widths, floats, arrays,
structs and pointers, with `if`, `for`, `switch`, bounds-checked indexing,
divisions, shifts, conversions and calls of earlier functions: the shape of
ordinary Go code, for two jobs.

  * The small ones (Inputs/opt/shape-*.gopp, 40 functions) are run-matrix
    programs: Go's output is the expected output, and every `opt` level must
    print it. They are generated from fixed seeds, so the checked-in text is
    reproducible.
  * The big ones (thousands of lines) are the compile-time corpus: the
    arm64 selector measurement of ADR-0028's P04.13 addendum and the pipeline
    timings (bench/pipelines/measure-opt.py).

The program is valid Go and valid Go++, never divides by a value that can be
0, masks every index and shift count, and has no x*y+z for floats (gc fuses
them on arm64, a rule the other tests cover), so Go's answer is the same on
both targets. main prints a checksum of the results of every function.
"""
import argparse
import importlib.util
import os
import random
import sys

INT_TYPES = ["int", "int32", "uint32", "uint8", "int64", "int16"]


class Gen:
    def __init__(self, seed, nfuncs):
        self.r = random.Random(seed)
        self.n = nfuncs
        self.lines = []
        self.depth = 0
        self.nvar = 0

    def emit(self, s):
        self.lines.append("\t" * (self.depth + 1) + s)

    # An integer expression of type int over the function's variables.
    def iexp(self, d=0):
        r = self.r
        if d > 2 or r.random() < 0.25:
            # (A bare constant could make a constant expression: an overflow
            # or a shifted float constant, which Go refuses.)
            return r.choice(["a", "int(b)", "i1", "i2", "i3", "i4", "int(c)", "int(u)",
                             "(i%d + %d)" % (r.randint(1, 4), r.randint(0, 40)),
                             "rec.x", "int(rec.y)", "arr[%s]" % self.idx()])
        op = r.choice(["+", "-", "*", "&", "|", "^", "/", "%", "<<", ">>", "&^", "+", "-"])
        x, y = self.iexp(d + 1), self.iexp(d + 1)
        if op == "/":
            return "(%s / (%s | 1))" % (x, y)
        if op == "%":
            return "(%s %% (%s | 1))" % (x, y)
        if op in ("<<", ">>"):
            return "(%s %s (uint(%s) & 31))" % (x, op, y)
        return "(%s %s %s)" % (x, op, y)

    def idx(self):
        return "uint(%s)&7" % self.iexp(2) if self.r.random() < 0.5 else "i%d&7" % self.r.randint(1, 4)

    def fexp(self, d=0):
        r = self.r
        if d > 1 or r.random() < 0.3:
            return r.choice(["d", "f1", "f2", "float64(int32(i1))", "float64(int32(i2))", "rec.z", "0.5", "1.25", "-3"])
        op = r.choice(["+", "-", "/"])
        x, y = self.fexp(d + 1), self.fexp(d + 1)
        if op == "/":
            return "(%s / float64(%s | 1))" % (x, self.iexp(2))
        return "float64(%s %s %s)" % (x, op, y) if r.random() < 0.3 else "(%s %s %s)" % (x, op, y)

    def cond(self):
        r = self.r
        c = "%s %s %s" % (self.iexp(1), r.choice(["<", "<=", ">", ">=", "==", "!="]), self.iexp(1))
        if r.random() < 0.3:
            c2 = "%s %s %s" % (self.fexp(1), r.choice(["<", ">", "!="]), self.fexp(1))
            return "%s %s %s" % (c, r.choice(["&&", "||"]), c2)
        return c

    def stmt(self, fi, depth):
        r = self.r
        k = r.random()
        v = "i%d" % r.randint(1, 4)
        if k < 0.30:
            self.emit("%s = %s" % (v, self.iexp()))
        elif k < 0.38:
            self.emit("%s %s= %s" % (v, r.choice(["+", "-", "*", "^", "|", "&"]), self.iexp(1)))
        elif k < 0.43:
            self.emit("%s%s" % (v, r.choice(["++", "--"])))
        elif k < 0.49:
            self.emit("arr[%s] = %s" % (self.idx(), self.iexp(1)))
        elif k < 0.53:
            self.emit("arr[%s] += %s" % (self.idx(), self.iexp(1)))
        elif k < 0.59:
            self.emit("f%d = %s" % (r.randint(1, 2), self.fexp()))
        elif k < 0.62:
            self.emit("u = u*%d + uint32(%s)" % (r.choice([3, 5, 33, 1664525]), self.iexp(1)))
        elif k < 0.65:
            self.emit("c = uint8(%s) + c" % self.iexp(1))
        elif k < 0.70:
            what = r.choice(["rec.x = %s" % self.iexp(1), "rec.y = int32(%s)" % self.iexp(1),
                             "rec.z = %s" % self.fexp(1), "rec.w[%s] = %s" % (self.idx().replace("&7", "&3"), self.iexp(1)),
                             "rec.x += int(rec.w[%s])" % self.idx().replace("&7", "&3")])
            self.emit(what)
        elif k < 0.74:
            self.nvar += 1
            self.emit("p%d := &arr[%s]" % (self.nvar, self.idx()))
            self.emit("*p%d += %s" % (self.nvar, self.iexp(1)))
            self.emit("%s += *p%d" % (v, self.nvar))
        elif k < 0.78:
            self.nvar += 1
            self.emit("q%d := &rec" % self.nvar)
            self.emit("q%d.x += %s" % (self.nvar, self.iexp(1)))
            self.emit("%s ^= q%d.x" % (v, self.nvar))
        elif k < 0.84 and fi > 0:
            j = r.randint(max(0, fi - 8), fi - 1)
            self.emit("%s += fn%d(%s, int32(%s), %s, arr)" % (v, j, self.iexp(1), self.iexp(1), self.fexp(1)))
        elif k < 0.90 and depth < 3:
            self.emit("if %s {" % self.cond())
            self.depth += 1
            for _ in range(r.randint(1, 3)):
                self.stmt(fi, depth + 1)
            self.depth -= 1
            if r.random() < 0.5:
                self.emit("} else {")
                self.depth += 1
                for _ in range(r.randint(1, 3)):
                    self.stmt(fi, depth + 1)
                self.depth -= 1
            self.emit("}")
        elif k < 0.95 and depth < 3:
            lv = "k%d" % depth
            self.emit("for %s := 0; %s < %d; %s++ {" % (lv, lv, r.randint(2, 6), lv))
            self.depth += 1
            self.emit("i4 += %s" % lv)
            for _ in range(r.randint(1, 3)):
                self.stmt(fi, depth + 1)
            self.depth -= 1
            self.emit("}")
        elif depth < 3:
            self.emit("switch uint(%s) & 3 {" % self.iexp(1))
            for case in range(r.randint(2, 3)):
                self.emit("case %d:" % case)
                self.depth += 1
                for _ in range(r.randint(1, 2)):
                    self.stmt(fi, depth + 1)
                self.depth -= 1
            self.emit("default:")
            self.depth += 1
            self.stmt(fi, depth + 1)
            self.depth -= 1
            self.emit("}")
        else:
            self.emit("%s = %s" % (v, self.iexp()))

    def func(self, fi):
        self.lines.append("//go:noinline")
        self.lines.append("func fn%d(a int, b int32, d float64, in [8]int) int {" % fi)
        self.depth = 0
        self.emit("arr := in")
        self.emit("var rec Rec")
        self.emit("i1, i2, i3, i4 := a, int(b), a^3, 7")
        self.emit("f1, f2 := d, 1.5")
        self.emit("var u uint32 = uint32(a)")
        self.emit("var c uint8 = uint8(b)")
        for _ in range(self.r.randint(5, 12)):
            self.stmt(fi, 0)
        self.emit("return i1 + i2*3 + i3*5 + i4*7 + arr[1] + arr[6] + rec.x + int(rec.y) + int(u) + int(c) + rec.w[2] + int(f1) + int(f2*8) % 1000 + int(rec.z) % 100")
        self.lines.append("}")
        self.lines.append("")

    def program(self):
        out = ["package main", "", "// Generated by tests/run/Inputs/gen-shape.py.", "",
               "type Rec struct {", "\tx int", "\ty int32", "\tz float64", "\tw [4]int", "}", ""]
        for fi in range(self.n):
            self.lines = []
            self.func(fi)
            out += self.lines
        out.append("func main() {")
        out.append("\tvar h uint64 = 14695981039346656037")
        out.append("\tvar in [8]int")
        out.append("\tfor i := 0; i < 8; i++ {")
        out.append("\t\tin[i] = i*37 - 11")
        out.append("\t}")
        for fi in range(self.n):
            out.append("\th = (h ^ uint64(fn%d(%d, %d, %d.25, in))) * 1099511628211" % (
                fi, self.r.randint(-50, 50), self.r.randint(-9, 9), self.r.randint(-5, 5)))
            if fi % 10 == 9:
                out.append("\tprintln(%d, h)" % fi)
        out.append("\tprintln(h)")
        out.append("}")
        return "\n".join(out) + "\n"


def shape(seed, nfuncs):
    return Gen(seed, nfuncs).program()


SMALL_SEEDS = [1, 2, 3, 4, 5, 6]


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
        for seed in SMALL_SEEDS:
            path = os.path.join(a.write, "opt", "shape-%d.gopp" % seed)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w").write(shape(seed, 40))
            if a.go:
                gen_opt.goldens(path, a.go)
        return
    if a.seed is None or a.funcs is None:
        ap.error("SEED and FUNCS, or --write")
    sys.stdout.write(shape(a.seed, a.funcs))


if __name__ == "__main__":
    main()
