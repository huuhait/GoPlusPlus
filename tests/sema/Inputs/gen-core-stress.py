#!/usr/bin/env python3
"""P04.04 (manager decision M3): programs that stressed the checker with a
quadratic walk or a recursion per link. Usage: gen-core-stress.py <shape> <N>

  initchain   N package variables, each initialized from the next one
              declared (`var v1 = v0 + 1` above `var v0 = 1`)
  constchain  the same with constants
  cycle       N package variables in a ring (`var v0 = v1 + 1` ... `var vN = v0`)
  constcycle  the same with constants
  mixcycle    N variables and N functions in a ring (`var v0 = f0()`,
              `func f0() int { return v1 }`, ...)
  cyctail     a chain of N variables whose last one depends on a ring of 3,
              the chain declared first (a walk for a cycle per link would be
              quadratic)
  fanin       N variables declared before the N they all depend on
  switch      a switch with N constant cases (integers and strings), and a
              tagless one with N conditions
  fields      a struct of N fields, each read and written by name
  keyed       a struct of N fields and keyed literals of it, keys in reverse
"""
import sys

shape, n = sys.argv[1], int(sys.argv[2])
out = ["package main", ""]
if shape == "initchain":
    out += ["var v%d = v%d + 1" % (i, i - 1) for i in range(n - 1, 0, -1)]
    out += ["var v0 = 1", "", "func main() { println(v%d) }" % (n - 1)]
elif shape == "constchain":
    out += ["const c%d = c%d + 1" % (i, i - 1) for i in range(n - 1, 0, -1)]
    out += ["const c0 = 1", "", "func main() { println(c%d) }" % (n - 1)]
elif shape == "cycle":
    out += ["var v%d = v%d + 1" % (i, (i + 1) % n) for i in range(n)]
    out += ["", "func main() { println(v0) }"]
elif shape == "constcycle":
    out += ["const c%d = c%d + 1" % (i, (i + 1) % n) for i in range(n)]
    out += ["", "func main() { println(c0) }"]
elif shape == "mixcycle":
    for i in range(n):
        out += ["var v%d = f%d()" % (i, i),
                "func f%d() int { return v%d }" % (i, (i + 1) % n)]
    out += ["", "func main() { println(v0) }"]
elif shape == "cyctail":
    out += ["var t%d = t%d + 1" % (i, i + 1) for i in range(n - 1)]
    out += ["var t%d = r0" % (n - 1)]
    out += ["var r0 = r1 + 1", "var r1 = r2 + 1", "var r2 = r0 + 1"]
    out += ["", "func main() { println(t0) }"]
elif shape == "fanin":
    out += ["var w%d = u%d + u%d" % (i, i, (i * 7) % n) for i in range(n)]
    out += ["var u%d = %d" % (i, i) for i in range(n)]
    out += ["", "func main() { println(w0, w%d) }" % (n - 1)]
elif shape == "switch":
    out += ["func f(x int, s string) int {", "\tswitch x {"]
    for i in range(n):
        out += ["\tcase %d:" % i, "\t\treturn %d" % (i * 3)]
    out += ["\t}", "\tswitch s {"]
    for i in range(n):
        out += ["\tcase \"s%d\":" % i, "\t\treturn %d" % i]
    out += ["\t}", "\tswitch {"]
    for i in range(n):
        out += ["\tcase x == %d:" % (i + n), "\t\treturn 1"]
    out += ["\t}", "\treturn 0", "}", "", "func main() { println(f(5, \"s7\")) }"]
elif shape == "fields":
    out += ["type T struct {"] + ["\tf%d int32" % i for i in range(n)] + ["}", ""]
    out += ["func main() {", "\tvar t T", "\ts := int32(0)"]
    out += ["\tt.f%d = %d" % (i, i % 100) for i in range(n)]
    out += ["\ts += t.f%d" % i for i in range(n)]
    out += ["\tprintln(s)", "}"]
elif shape == "keyed":
    out += ["type T struct {"] + ["\tf%d int32" % i for i in range(n)] + ["}", ""]
    keys = ", ".join("f%d: %d" % (i, i % 100) for i in range(n - 1, -1, -1))
    out += ["func main() {", "\ta := T{%s}" % keys, "\tb := T{f%d: 1}" % (n - 1),
            "\tprintln(a.f0, b.f%d, a == b)" % (n - 1), "}"]
else:
    sys.exit("unknown shape " + shape)
print("\n".join(out))
