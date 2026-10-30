#!/usr/bin/env python3
"""P03.25: stress inputs for the unit analysis; each must check in near-linear
time and with no deep recursion.

  chain N     N units, each made from the one before (a chain of N), used at
              the end and at the start
  overflow N  the same with the factor 2 each time: the exact factor outgrows its
              limit (4096 bits) and that is one error, not a crash
  sum N       one expression of N terms, a balanced tree (the parser limits
              the nesting of an expression to 1024)
  mul N       the same with N factors `x * x * ...` of a unit value
  bases N     N base units and one unit that is the product of all of them
              (one unit expression of N factors)
  lits N      N statements with a unit literal
  inst N      N units and N calls of one generic function, one instance each
  nest N      a unit expression with N nested parentheses (an error from the
              parser's depth limit, not a crash)
  prod N      N values of N distinct base units multiplied left to right,
              one statement per factor (each product has one more unit; the
              unit of each step is built once, not part by part)
  prodexpr N  the same as one expression (N at most the parser's 1024)
  genchain N  N plain generic types, each holding the one before
              (`type W_i[U any] struct{ w W_{i-1}[U] }`), in a package with
              a unit (the range of instances is decided per declaration,
              no instance is expanded: R18)
  genplain N  the same with no unit in the package
  unitchain N the same chain over a unit parameter, instantiated with KM
  badchain N  unitchain with `float64[U^412]` at the end: one E0435 (the
              instance at the end of the chain)
  ring N      N units in one cycle (one error that lists them all) and a
              unit that depends on the cycle (no second error)

Usage: gen-units-stress.py MODE N
"""
import sys


def balanced(terms, op):
    """The terms joined by op as a balanced tree: depth log N, not N."""
    while len(terms) > 1:
        nxt = []
        for i in range(0, len(terms) - 1, 2):
            nxt.append("(%s %s %s)" % (terms[i], op, terms[i + 1]))
        if len(terms) % 2:
            nxt.append(terms[-1])
        terms = nxt
    return terms[0]


mode, n = sys.argv[1], int(sys.argv[2])
o = ["package main", ""]
if mode == "chain":
    o.append("unit U0")
    for i in range(1, n):
        # 2 and 1/2 in turn: the exact factor stays small
        o.append("unit U%d = %s U%d" % (i, "2" if i % 2 else "(1 / 2)", i - 1))
    o.append("func main() {")
    o.append("\tvar a float64[U%d] = 1.0[U%d]" % (n - 1, n - 1))
    o.append("\tvar b float64[U0] = a.In[U0]()")
    o.append("\t_ = b")
    o.append("}")
elif mode == "overflow":
    o.append("unit U0")
    for i in range(1, n):
        o.append("unit U%d = 2 U%d" % (i, i - 1))
    o.append("func main() { var a float64[U%d]; _ = a }" % (n - 1))
elif mode == "sum":
    o.append("unit M")
    o.append("func main() {")
    o.append("\tx := " + balanced(["1.0[M]"] * n, "+"))
    o.append("\tvar y float64[M] = x")
    o.append("\t_ = y")
    o.append("}")
elif mode == "mul":
    o.append("unit M")
    o.append("func main() {")
    o.append("\tx := 1.0[M]")
    o.append("\ty := " + balanced(["x"] * n, "*"))
    o.append("\tz := y / y")
    o.append("\t_ = z")
    o.append("}")
elif mode == "bases":
    for i in range(n):
        o.append("unit B%d" % i)
    o.append("unit P = " + " * ".join("B%d" % i for i in range(n)))
    o.append("func main() {")
    o.append("\tvar a float64[P] = 1.0[P]")
    o.append("\tb := a * 2.0")
    o.append("\t_ = b")
    o.append("}")
elif mode == "lits":
    o.append("unit M")
    o.append("func main() {")
    o.append("\tvar s float64[M]")
    for i in range(n):
        o.append("\ts += %d.5[M]" % i)
    o.append("\t_ = s")
    o.append("}")
elif mode == "inst":
    for i in range(n):
        o.append("unit V%d" % i)
    o.append("func Avg[U unit](xs []float64[U]) float64[U] { return xs[0] }")
    o.append("func main() {")
    for i in range(n):
        o.append("\t_ = Avg([]float64[V%d]{1.0[V%d]})" % (i, i))
    o.append("}")
elif mode in ("prod", "prodexpr"):
    o += ["unit B%d" % i for i in range(n)]
    o.append("func G(%s) float64 {" % ", ".join(
        "x%d float64[B%d]" % (i, i) for i in range(n)))
    if mode == "prodexpr":
        o.append("\treturn float64(%s)" % " * ".join("x%d" % i for i in range(n)))
    else:
        o.append("\tv1 := x0 * x1")
        for i in range(2, n):
            o.append("\tv%d := v%d * x%d" % (i, i - 1, i))
        o.append("\treturn float64(v%d)" % (n - 1))
    o.append("}")
    o.append("func main() {}")
elif mode in ("genchain", "genplain", "unitchain", "badchain"):
    if mode != "genplain":
        o += ["unit M", "unit KM = 1000 M", "var unitSeen float64[M]"]
    k = "unit" if mode in ("unitchain", "badchain") else "any"
    f = {"unitchain": "float64[U^2]", "badchain": "float64[U^412]"}.get(mode, "U")
    o.append("type W0[U %s] struct{ f %s }" % (k, f))
    for i in range(1, n):
        o.append("type W%d[U %s] struct{ w W%d[U] }" % (i, k, i - 1))
    if k == "unit":
        o.append("func main() { var w W%d[KM]; _ = w }" % (n - 1))
    else:
        o.append("func main() {}")
elif mode == "ring":
    for i in range(n):
        o.append("unit R%d = 2 R%d" % (i, (i + 1) % n))
    o.append("unit Dep = 3 R0")
    o.append("func main() { var a float64[Dep]; _ = a }")
elif mode == "nest":
    o.append("unit M")
    o.append("unit S")
    o.append("type T float64[" + "(" * n + "M" + ")" * n + "/S]")
    o.append("func main() { var t T; _ = t }")
else:
    sys.exit("unknown mode")
print("\n".join(o))
