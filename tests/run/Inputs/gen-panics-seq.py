#!/usr/bin/env python3
"""Writes the programs of Inputs/panics-seq/: one run-time panic each, for
strings, slices and make (P04.05).

usage: gen-panics-seq.py DIR

Each program prints "before", then fails a check, so it exits with status 2
and Go's first line on standard error (Go's boundsErrorFmts and
boundsNegErrorFmts: "index out of range [5] with length 3", "slice bounds out
of range [:7] with capacity 4", "[3:2]", "[::9] with length 5", "[-1::]",
"makeslice: len out of range", ...). The programs are valid Go;
gen-goldens.py --dir DIR DIR/*.gopp writes the expected stdout (.out) and the
first line of standard error (.err) from `go run`.
"""
import os
import sys

S = "var s = []int{1, 2, 3}\n"
C = "var c = make([]int, 3, 5)\n"
A = "var a [5]int\n"
T = 'var t = "héllo"\n'

# name: (declarations, statements)
CASES = {}


def case(name, decls, stmts):
    CASES[name] = (decls, stmts)


# Index of a slice and of a string, by index type.
case("idx-slice", S + "var i = 3", "println(s[i])")
case("idx-slice-neg", S + "var i = -1", "println(s[i])")
case("idx-slice-nil", "var s []int\nvar i = 0", "println(s[i])")
case("idx-slice-empty", "var s = []int{}\nvar i = 0", "println(s[i])")
case("idx-slice-int8", S + "var i int8 = -100", "println(s[i])")
case("idx-slice-uint8", S + "var i uint8 = 200", "println(s[i])")
case("idx-slice-uint64", S + "var i uint64 = 1<<64 - 1", "println(s[i])")
case("idx-slice-int64-min", S + "var i int64 = -1 << 63", "println(s[i])")
case("idx-slice-store", S + "var i = 3", "s[i] = 1\n\tprintln(s[0])")
case("idx-slice-compound", S + "var i = 7", "s[i] += 2\n\tprintln(s[0])")
case("idx-slice-incdec", S + "var i = 3", "s[i]++\n\tprintln(s[0])")
case("idx-slice-2d", "var g = [][]int{{1}, {2, 3}}\nvar i, j = 1, 2", "println(g[i][j])")
case("idx-slice-2d-outer", "var g = [][]int{{1}, {2, 3}}\nvar i, j = 2, 0", "println(g[i][j])")
case("idx-slice-of-array", "var g = [][3]int{{1, 2, 3}}\nvar i, j = 0, 3", "println(g[i][j])")
case("idx-string", T + "var i = 6", "println(t[i])")
case("idx-string-neg", T + "var i = -3", "println(t[i])")
case("idx-string-empty", 'var t = ""\nvar i = 0', "println(t[i])")
case("idx-string-uint8", T + "var i uint8 = 255", "println(t[i])")
case("idx-store-order", S + "var i = 5\n\nfunc f() int {\n\tprintln(\"f called\")\n\treturn 1\n}",
     "s[i] = f()")
case("idx-store-tuple", S + "var i = 5\n\nfunc f() int {\n\tprintln(\"f called\")\n\treturn 1\n}",
     "s[1], s[i], s[7] = f(), f(), f()")
case("idx-after-output", S + "var i = 3", "for k := 0; k < 3; k++ {\n\t\tprintln(\"k\", k)\n\t}\n\tprintln(s[i])")

# Slice expressions: [:hi] against the capacity (a slice) or the length (a
# string, an array), then [lo:hi].
case("sl-hi-cap", C + "var i = 6", "println(len(c[:i]))")
case("sl-hi-cap-nil", "var s []int\nvar i = 1", "println(len(s[:i]))")
case("sl-hi-len-string", T + "var i = 7", "println(t[:i])")
case("sl-hi-len-array", A + "var i = 6", "println(len(a[:i]))")
case("sl-hi-len-ptr-array", A + "var i = 6", "p := &a\n\tprintln(len(p[:i]))")
case("sl-lo-hi", C + "var i, j = 3, 2", "println(len(c[i:j]))")
case("sl-lo-hi-string", T + "var i, j = 3, 2", "println(t[i:j])")
case("sl-lo-end", C + "var i = 4", "println(len(c[i:]))")
case("sl-lo-end-string", T + "var i = 7", "println(t[i:])")
case("sl-lo-end-array", A + "var i = 6", "println(len(a[i:]))")
case("sl-neg-hi", C + "var i = -1", "println(len(c[:i]))")
case("sl-neg-lo", C + "var i = -1", "println(len(c[i:]))")
case("sl-neg-lo-hi", C + "var i = -1", "println(len(c[i:2]))")
case("sl-int8", C + "var i int8 = -5", "println(len(c[:i]))")
case("sl-uint8", C + "var i uint8 = 200", "println(len(c[:i]))")
case("sl-uint64", C + "var i uint64 = 1<<64 - 1", "println(len(c[:i]))")
case("sl-string-neg", T + "var i = -2", "println(t[:i])")
case("sl-hi-then-lo", C + "var i, j = 9, 7", "println(len(c[j:i]))")
# The three-index form is for arrays only in Go++ (SLC-13): an array and a
# pointer to one.
case("sl3-max-len-ptr-array", A + "var i = 6", "p := &a\n\tprintln(len(p[0:1:i]))")
case("sl3-max-len-array", A + "var i = 6", "println(len(a[0:1:i]))")
case("sl3-hi-max", A + "var i = 3", "println(len(a[0:i:2]))")
case("sl3-lo-hi", A + "var i = 2", "println(len(a[i:1:3]))")
case("sl3-neg-max", A + "var i = -1", "println(len(a[0:1:i]))")
case("sl3-neg-hi", A + "var i = -1", "println(len(a[0:i:2]))")
case("sl3-neg-lo", A + "var i = -1", "println(len(a[i:1:2]))")
case("sl3-uint8", A + "var i uint8 = 99", "println(len(a[0:1:i]))")
# From the other P04.05 implementations: an index through a borrowed slice of
# an array, a bound one past a full slice, unsigned bounds.
case("idx-borrowed-of-array", A + "var i = 4", "t := a[1:3]\n\tprintln(t[i])")
case("sl-hi-cap-full", "var c = make([]int, 3)\nvar i = 4", "println(len(c[:i]))")
case("sl-lo-hi-uint64", C + "var i, j uint64 = 3, 1", "println(len(c[i:j]))")

# make
case("make-len-neg", "var n = -1", "println(len(make([]int, n)))")
case("make-len-huge", "var n = 1 << 62", "println(len(make([]int, n)))")
case("make-cap-small", "var n, c = 5, 2", "println(len(make([]int, n, c)))")
case("make-cap-neg", "var n, c = 0, -1", "println(len(make([]int, n, c)))")
case("make-cap-huge", "var n, c = 0, 1 << 62", "println(len(make([]int, n, c)))")
case("make-len-uint64", "var n uint64 = 1<<64 - 1", "println(len(make([]int, n)))")
case("make-len-int8", "var n int8 = -3", "println(len(make([]byte, n)))")
case("make-len-uint64-byte", "var n uint64 = 1<<64 - 1", "println(len(make([]byte, n)))")
case("make-len-big-elem", "var n = 1 << 40", "println(len(make([][1 << 20]byte, n)))")

# The conversion of a slice to an array or a pointer to one (Go's
# panicSliceConvert).
case("conv-array", S, "a := [4]int(s)\n\tprintln(a[0])")
case("conv-ptr-array", S, "p := (*[5]int)(s)\n\tprintln(p[0])")

# unsafe.Slice and unsafe.String (runtime/unsafe.go).
U = 'import "unsafe"\n\nvar a [5]int\n'
case("unsafe-slice-neg", U + "var n = -1", "println(len(unsafe.Slice(&a[0], n)))")
case("unsafe-slice-huge", U + "var n = 1 << 62", "println(len(unsafe.Slice(&a[0], n)))")
case("unsafe-string-neg", U + "var n = -2\nvar b = [3]byte{1, 2, 3}", "println(len(unsafe.String(&b[0], n)))")

# An index target of a range clause: its check is part of the store.
case("range-index-target", S + "var a [2]int\nvar i = 2", "for a[i] = range s {\n\t}")


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    for name, (decls, stmts) in CASES.items():
        src = "package main\n\n" + decls + "\n\nfunc main() {\n\tprintln(\"before\")\n\t" + stmts + \
              "\n\tprintln(\"not reached\")\n}\n"
        with open(os.path.join(d, name + ".gopp"), "w") as f:
            f.write(src)


main()
