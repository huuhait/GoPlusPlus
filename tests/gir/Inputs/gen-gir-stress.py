#!/usr/bin/env python3
"""P04.01: programs that stress the GIR builder, verifier, printer and
serializer. Usage: gen-gir-stress.py <shape> <N>

  siblings  N statements `x = x + k` in one function
  blocks    N sibling blocks, each with its own variable (scopes)
  calls     N println calls (each is several blocks)
  nested    N nested `if` statements, each with a variable
  chain     one expression with N additions
  funcs     N small functions that call each other
  typearr   a chain of N named array types (`type T1 [1]T0`), compared with ==
  typestruct a chain of N named struct types (`type T1 struct{ f T0 }`),
            compared with == and copied
"""
import sys

shape, n = sys.argv[1], int(sys.argv[2])
out = ["package main", ""]
if shape == "siblings":
    out += ["func main() {", "\tx := 0"]
    out += ["\tx = x + %d" % (i % 7 + 1) for i in range(n)]
    out += ["\tprintln(x)", "}"]
elif shape == "blocks":
    out += ["func main() {", "\tx := 1"]
    for i in range(n):
        out += ["\t{", "\t\ty := x + %d" % i, "\t\tx = y", "\t}"]
    out += ["\tprintln(x)", "}"]
elif shape == "calls":
    out += ["func main() {"]
    out += ["\tprintln(%d, \"s\", true)" % i for i in range(n)]
    out += ["}"]
elif shape == "nested":
    out += ["func main() {", "\tx := 0"]
    for i in range(n):
        out += ["\t" * (i + 1) + "if x == %d {" % i, "\t" * (i + 2) + "v%d := x + 1" % i,
                "\t" * (i + 2) + "x = v%d" % i]
    for i in reversed(range(n)):
        out += ["\t" * (i + 1) + "}"]
    out += ["\tprintln(x)", "}"]
elif shape == "chain":
    out += ["func main() {", "\tx := 1", "\ty := x" + " + x" * n, "\tprintln(y)", "}"]
elif shape == "funcs":
    for i in range(n):
        body = "return %d" % i if i == 0 else "return f%d(x) + 1" % (i - 1)
        out += ["func f%d(x int) int { %s }" % (i, body)]
    out += ["func main() { println(f%d(1)) }" % (n - 1)]
elif shape == "typearr":
    out += ["type T0 [1]int"] + ["type T%d [1]T%d" % (i, i - 1) for i in range(1, n + 1)]
    out += ["func main() {", "\tvar a T%d" % n, "\tb := a", "\tprintln(a == b)", "}"]
elif shape == "typestruct":
    out += ["type T0 struct{ f int }"] + ["type T%d struct{ f T%d }" % (i, i - 1) for i in range(1, n + 1)]
    out += ["func main() {", "\tvar a T%d" % n, "\tb := a", "\tprintln(a == b)", "}"]
else:
    sys.exit("unknown shape " + shape)
print("\n".join(out))
