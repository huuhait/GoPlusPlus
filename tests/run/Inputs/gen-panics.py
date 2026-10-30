#!/usr/bin/env python3
"""Writes the programs of Inputs/panics/: one run-time panic each.

usage: gen-panics.py DIR

Each program prints "before", then fails a check of D4.12, so it exits with
status 2 and Go's first line on standard error. The programs are valid Go;
gen-goldens.py --dir DIR DIR/*.gopp writes the expected stdout (.out) and the
first line of standard error (.err) from `go run`.
"""
import os
import sys

# name: (declarations, statements)
CASES = {
    "div-int": ("var z int", "println(10 / z)"),
    "div-int8": ("var z int8", "var x int8 = -128\n\tprintln(x / z)"),
    "div-uint64": ("var z uint64", "println(uint64(10) / z)"),
    "rem-int32": ("var z int32", "println(int32(7) % z)"),
    "rem-uint8": ("var z uint8", "var x uint8 = 200\n\tprintln(x % z)"),
    "div-assign": ("var z int16", "x := int16(5)\n\tx /= z\n\tprintln(x)"),
    "rem-assign": ("var z int", "x := 5\n\tx %= z\n\tprintln(x)"),
    "div-callee": ("var z int\n\nfunc f(a, b int) int { return a / b }\n\nfunc g(n int) int {\n\tif n == 0 {\n\t\treturn f(1, z)\n\t}\n\treturn g(n-1) + 1\n}",
                   "println(g(50))"),
    "div-init": ("var z int\nvar x = 100 / z", "println(x)"),
    "shift-left": ("var n = -1", "println(1 << n)"),
    "shift-right": ("var n int8 = -5", "var x int64 = 1 << 40\n\tprintln(x >> n)"),
    "shift-assign": ("var n = -64", "x := uint32(1)\n\tx <<= n\n\tprintln(x)"),
    "index-array": ("var a [5]int\nvar i = 5", "println(a[i])"),
    "index-negative": ("var a [5]int\nvar i = -1", "println(a[i])"),
    "index-int8": ("var a [10]int16\nvar i int8 = -100", "println(a[i])"),
    "index-uint8": ("var a [10]int16\nvar i uint8 = 200", "println(a[i])"),
    "index-uint64": ("var a [3]int\nvar i uint64 = 1<<64 - 1", "println(a[i])"),
    "index-int64-min": ("var a [3]int\nvar i int64 = -1 << 63", "println(a[i])"),
    "index-store": ("var a [4]byte\nvar i = 4", "a[i] = 1\n\tprintln(a[0])"),
    "index-compound": ("var a [4]int\nvar i = 7", "a[i] += 2\n\tprintln(a[0])"),
    "index-incdec": ("var a [4]int\nvar i = 4", "a[i]++\n\tprintln(a[0])"),
    "index-nested": ("var g [3][4]int\nvar i, j = 2, 4", "println(g[i][j])"),
    "index-struct": ("type S struct {\n\tn   int\n\tarr [6]int32\n}\n\nvar s S\nvar i = 6", "println(s.arr[i])"),
    "index-pointer": ("var a [5]int\nvar i = 9", "p := &a\n\tprintln(p[i])"),
    "index-local": ("var i = 3", "var a [3]float64\n\tprintln(a[i])"),
    "index-zero-length": ("var a [0]int\nvar i = 0", "println(a[i])"),
    "index-after-output": ("var a [2]int\nvar i = 2", "for k := 0; k < 5; k++ {\n\t\tprintln(\"k\", k)\n\t}\n\tprintln(a[i])"),
    # Go's order (spec, "Assignment statements"): the operands of index
    # expressions and the right sides are evaluated first, the index check of
    # a target is part of its assignment, after every right side (R16).
    "assign-order-call": ("var a [5]int\n\nfunc n(x int) int { return x }\n\nfunc f() int {\n\tprintln(\"f called\")\n\treturn 1\n}",
                          "a[n(5)] = f()"),
    "assign-order-rhs": ("var a [5]int\n\nfunc n(x int) int { return x }", "a[n(5)] = a[n(6)]"),
    "assign-order-tuple": ("var a [5]int\n\nfunc n(x int) int { println(\"n\", x); return x }\n\nfunc f() int {\n\tprintln(\"f called\")\n\treturn 1\n}",
                           "a[1], a[n(5)], a[n(6)] = f(), f(), f()"),
    "assign-order-multi": ("var a [5]int\n\nfunc n(x int) int { return x }\n\nfunc two() (int, int) {\n\tprintln(\"two called\")\n\treturn 1, 2\n}",
                           "a[n(5)], a[n(0)] = two()"),
    "assign-order-op": ("var a [5]int\n\nfunc n(x int) int { return x }\n\nfunc f() int {\n\tprintln(\"f called\")\n\treturn 1\n}",
                        "a[n(5)] += f()"),
    "assign-order-nested": ("var m [2][3]int\n\nfunc n(x int) int { return x }\n\nfunc f() int {\n\tprintln(\"f called\")\n\treturn 1\n}",
                            "m[n(4)][n(7)] = f()"),
    # P04.13: checks an optimizer might move or drop (run/core-opt-matrix*
    # builds each of these at every `opt`). The output before the panic must be
    # complete, and a result nobody reads does not remove the check. The other
    # opt-* programs are written by gen-opt.py.
    "opt-loop-index": ("var a [5]int\nvar n = 6", "s := 0\n\tfor i := 0; i < n; i++ {\n\t\tprintln(\"i\", i)\n\t\ts += a[i]\n\t}\n\tprintln(s)"),
    "opt-shift-loop": ("var n = -3", "for i := 2; i >= 0; i-- {\n\t\tprintln(1 << (n + i))\n\t}"),
    "opt-index-hoist": ("var a [4]int32\nvar j = 4", "for i := 0; i < 4; i++ {\n\t\ta[i] = int32(i)\n\t\tprintln(a[i])\n\t}\n\tprintln(a[j])"),
    "opt-div-min": ("var z int\nvar m = -1 << 63", "println(m / -1, m % -1)\n\tprintln(m / z)"),
    "opt-div-inlined": ("func f(a, b int) int { return a / b }", "println(f(1, 0))"),
    "opt-index-local": ("", "var a [5]int\n\ti := 7\n\tprintln(a[i])"),
}


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    for name, (decls, stmts) in CASES.items():
        src = "package main\n\n" + decls + "\n\nfunc main() {\n\tprintln(\"before\")\n\t" + stmts + \
              "\n\tprintln(\"not reached\")\n}\n"
        with open(os.path.join(d, name + ".gopp"), "w") as f:
            f.write(src)


main()
