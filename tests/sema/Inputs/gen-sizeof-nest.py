#!/usr/bin/env python3
"""Generates the by-value nesting cases of P03.26:
<shape> <depth> [go|gopp] [want] [rev].

Shapes (a type declared per level, each holding the previous one by value):
  struct   S_i { a int8; b S_{i-1}; c int16 } (an int64 field every 7th level)
  array    A_i [1]B_{i-1}; B_i { x int8; y A_i; z int32 }
  generic  E_i G[E_{i-1}] with G[T] { a int8; b T; c int16 }
  dag      D_i { a, b D_{i-1} }: 2^depth leaves, a tree if nothing is kept
  wide     W_i { a [1]W_{i-1}; p *W_i }: an array by value, a pointer back
           (the pointer is a cycle that must not count)

With `go` the program prints Sizeof, Alignof and Offsetof of the top type and
of a type 3 levels below it; with `gopp` the program holds the same as
checks: every value is compared with the number found in the `want` file
(one number per line, in the order of printing), a wrong one is an error.
With `rev` the types are declared from the top down (each before the one it
holds), so a size is asked of a type whose parts are declared later.
"""
import sys

shape, depth = sys.argv[1], int(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else "go"
want = sys.argv[4] if len(sys.argv) > 4 and sys.argv[4] != "rev" else None
rev = "rev" in sys.argv[4:]
out = ["package main", "", 'import "unsafe"', ""]
head = len(out)
exprs = []


def top(name, low, off=None):
    exprs.append("unsafe.Sizeof(%s{})" % name)
    exprs.append("unsafe.Alignof(%s{})" % name)
    if off:
        exprs.append("unsafe.Offsetof(%s{}.%s)" % (name, off))
    exprs.append("unsafe.Sizeof(%s{})" % low)


low = max(depth - 3, 0)
if shape == "struct":
    out.append("type S0 struct{ a int8 }")
    for i in range(1, depth + 1):
        extra = "; d int64" if i % 7 == 0 else ""
        out.append("type S%d struct{ a int8; b S%d; c int16%s }" % (i, i - 1, extra))
    top("S%d" % depth, "S%d" % low, "b")
elif shape == "array":
    out.append("type B0 struct{ x int8 }")
    for i in range(1, depth + 1):
        out.append("type A%d [1]B%d" % (i, i - 1))
        out.append("type B%d struct{ x int8; y A%d; z int32 }" % (i, i))
    top("B%d" % depth, "B%d" % low, "y")
elif shape == "generic":
    out.append("type G[T any] struct{ a int8; b T; c int16 }")
    out.append("type E0 int32")
    for i in range(1, depth + 1):
        out.append("type E%d G[E%d]" % (i, i - 1))
    top("E%d" % depth, "E%d" % low)
elif shape == "dag":
    out.append("type D0 struct{ a int8 }")
    for i in range(1, depth + 1):
        out.append("type D%d struct{ a, b D%d }" % (i, i - 1))
    top("D%d" % depth, "D%d" % low, "b")
elif shape == "wide":
    out.append("type W0 struct{ a int8 }")
    for i in range(1, depth + 1):
        out.append("type W%d struct{ a [1]W%d; p *W%d }" % (i, i - 1, i))
    top("W%d" % depth, "W%d" % low, "p")
else:
    sys.exit("unknown shape " + shape)
if rev:
    out[head:] = reversed(out[head:])

if mode == "go":
    out += ["", "func main() {"]
    out += ["\tprintln(%s)" % e for e in exprs]
    out += ["}"]
else:
    wants = open(want).read().split()
    out += [""]
    for e, w in zip(exprs, wants):
        # an index out of range unless the value is the wanted one
        out.append("var _ = [1]byte{}[%s-%s]" % (e, w))
print("\n".join(out))
