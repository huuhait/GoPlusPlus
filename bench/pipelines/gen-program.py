#!/usr/bin/env python3
"""Writes a generated Go++ program (also valid Go) for measuring the codegen
pipelines (P04.13): many functions with the shape of ordinary Go code (integer
and float arithmetic of several widths, structs and arrays, loops, branches,
bounds-checked indexing, divisions, shifts, conversions, calls of earlier
functions). It only has to compile and run without a panic; main prints a
checksum so that the program can also be compared across pipelines.

usage: gen-program.py [--functions N] [--seed S] > prog.gopp
"""
import argparse
import random

ap = argparse.ArgumentParser()
ap.add_argument("--functions", type=int, default=400)
ap.add_argument("--seed", type=int, default=1)
args = ap.parse_args()
R = random.Random(args.seed)

INTS = ["int", "int32", "uint8", "uint32", "int64", "uint64", "int16"]


def lit(t):
    return str(R.randint(1, 100))


out = ["package main", "",
       "type P struct {", "\tx, y int32", "\tf float64", "\tk uint8", "}", "",
       "var g [16]int", "var gf [8]float64", ""]


def func(n):
    ty = R.choice(INTS)
    body = []
    body.append("\tvar a [8]%s" % ty)
    body.append("\tvar p P")
    body.append("\ts := %s(%s)" % (ty, "p0"))
    body.append("\tf := float64(p1)")
    body.append("\t_, _, _ = a, p, f")
    for i in range(R.randint(10, 24)):
        k = R.randint(0, 11)
        c = lit(ty)
        if k == 0:
            body.append("\ts += %s(%s) * %s" % (ty, "p0", c))
        elif k == 1:
            body.append("\ta[%s&7] = s ^ %s(%s)" % ("p0", ty, c))
        elif k == 2:
            body.append("\tif s > %s(%s) {\n\t\ts -= a[%s&7]\n\t} else {\n\t\ts += %s(%s)\n\t}" % (ty, c, "p0", ty, lit(ty)))
        elif k == 3:
            body.append("\tfor i := 0; i < %d; i++ {\n\t\ta[i&7] += s\n\t\ts ^= a[(i+1)&7]\n\t}" % R.randint(2, 9))
        elif k == 4:
            body.append("\tp.x += int32(s)\n\tp.f = p.f*0.5 + f")
        elif k == 5:
            body.append("\tf = f*0.25 + float64(p.x&1023) - float64(a[%s&7]&127)" % "p0")
        elif k == 6:
            body.append("\ts = s / (%s(p0&7) + 1)" % ty)
        elif k == 7:
            body.append("\ts = s << (uint(p0) & 7) >> 1")
        elif k == 8:
            body.append("\tg[int(uint(s)&15)] += int(p.k) + %s" % c)
        elif k == 9 and n > 0:
            body.append("\ts += %s(f%d(int(s)&255, f))" % (ty, R.randint(0, n - 1)))
        elif k == 10:
            body.append("\tp.k = uint8(s) + p.k\n\tgf[int(p.k)&7] = f")
        else:
            body.append("\tf = f*0.5 + float64(g[int(p.k)&15]&255)")
    body.append("\treturn int(s) + int(p.x) + int(f)")
    return "func f%d(p0 int, p1 float64) int {\n%s\n}\n" % (n, "\n".join(body))


for n in range(args.functions):
    out.append(func(n))
out.append("func main() {\n\tt := 0\n\tfor i := 0; i < 3; i++ {\n\t\tt += f%d(i+3, 2.5)\n\t}\n\tprintln(t)\n}" % (args.functions - 1))
print("\n".join(out))
