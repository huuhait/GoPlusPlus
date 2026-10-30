#!/usr/bin/env python3
"""P03.24: generated programs that stress the operator checks.

  types N      N types, each with every operator form (binary, unary,
               comparison, index, call, increment, @Right, @Convert), and one
               function that uses all of them for each type
  overloads N  one type with N `+` overloads (one per parameter type) and N
               uses, each picking another overload
  chain N      N operands in all, in expressions of at most 1000 operands (the
               parser allows 1024 levels of nesting, for `int` sums too):
               `a0 + a1 + ...`, the same with mixed `+ - *`, and with `<=>`
  generic N    a chain of N generic functions whose bodies use an operator of
               their constraint and call the next one
  nested N     N nested parenthesized operator expressions: ((a + b) + c) ...
  mixed N      N operands in all, in chains of at most 1000 where every operand
               is an operator call, a[i] read or a() call
  inherit N    a chain of N types that extend each other, with the operators
               on the root and a use of each at every level (an inherited
               `+`, `<=>`, `[]`, `()`, and the in-place `+=` through a pointer
               method), so the operator table and the method lookup of a
               deep child are asked N times
  extover N    a chain of N types that extend each other, each with its own `+`
               overload (another parameter type), and 2N uses: one from the
               deepest type and one from each level, so the merged operator
               list of every level is asked
  nestidx N    `a[a[...a...]]` N deep with a user `[]` (every operand is an
               index)
  nestcall N   `a(a(...a...))` N deep with a user `()`
  nestplus N   `a + (a + (... + a))` N deep with a user `+`
  errors N     N uses of an overload set of 50 methods that match none of them
               (N errors)
  parallel N   N bodies that ask at once for the operator sets of the same
               generic instances and of one `extends` chain; some are errors
"""
import sys

kind, n = sys.argv[1], int(sys.argv[2])
out = ["package main", ""]

if kind == "types":
    for i in range(n):
        t = "T%d" % i
        out += [
            "type %s struct{ X, Y float64 }" % t,
            "func (a %s) + (b %s) %s { return %s{a.X + b.X, a.Y + b.Y} }" % (t, t, t, t),
            "func (a %s) - (b %s) %s { return a }" % (t, t, t),
            "func (a %s) * (b %s) %s { return a }" % (t, t, t),
            "@Right",
            "func (a %s) * (s float64) %s { return a }" % (t, t),
            "func (a %s) - () %s { return a }" % (t, t),
            "func (a %s) == (b %s) bool { return a.X == b.X }" % (t, t),
            "func (a %s) <=> (b %s) int { return 0 }" % (t, t),
            "func (a %s) [] (i int) float64 { return a.X }" % t,
            "func (a *%s) []= (i int, v float64) { a.X = v }" % t,
            "func (a %s) () (x int) int { return x }" % t,
            "func (a *%s) ++() { a.X++ }" % t,
            "func (a *%s) += (b %s) { a.X += b.X }" % (t, t),
            "@Convert",
            "func (a %s) Float() float64 { return a.X }" % t,
            "func use%d(a, b %s) float64 {" % (i, t),
            "\tvar m %s" % t,
            "\tvar n %s" % t,
            "\t_ = a + b - a * b",
            "\t_ = 2.0 * a",
            "\t_ = -a",
            "\t_ = a == b",
            "\t_ = a != b",
            "\t_ = a < b",
            "\t_ = a[0] + float64(a(1)) * 1.0",
            "\tm[0] = 1.5",
            "\tm[1] += 2.5",
            "\tm++",
            "\tm += n",
            "\treturn float64(a)",
            "}",
        ]
    out += ["func main() {}"]
elif kind == "overloads":
    out += ["type V struct{ X int }"]
    for i in range(n):
        out += ["type P%d struct{ k int }" % i,
                "func (a V) + (b P%d) V { return a }" % i]
    out += ["func use(a V, ps ...int) {"]
    out += ["\tvar p%d P%d" % (i, i) for i in range(n)]
    for rep in range(max(1, 5000 // n)):
        for i in range(n):
            out += ["\t_ = a + p%d" % i]
    out += ["}", "func main() {}"]
elif kind == "chain":
    out += ["type V struct{ X float64 }",
            "func (a V) + (b V) V { return V{a.X + b.X} }",
            "func (a V) - (b V) V { return a }",
            "func (a V) * (b V) V { return a }",
            "func (a V) <=> (b V) int { return 0 }"]
    ops = ["+", "-", "*"]
    left, f = n, 0
    while left > 0:
        k = min(left, 1000)
        left -= k
        params = ", ".join("a%d" % i for i in range(k)) + " V"
        out += ["func plus%d(%s) V {" % (f, params),
                "\treturn " + " + ".join("a%d" % i for i in range(k)), "}"]
        expr = "a0"
        for i in range(1, k):
            expr += " %s a%d" % (ops[i % 3], i)
        out += ["func mixed%d(%s) V {" % (f, params), "\treturn " + expr, "}"]
        out += ["func cmp%d(%s) bool {" % (f, params),
                "\treturn (" + " + ".join("a%d" % i for i in range(k)) + ") <=> a0 == 0",
                "}"]
        f += 1
    out += ["func main() {}"]
elif kind == "generic":
    out += ["type Adder[T any] interface { + (T) T; Copy }",
            "type V struct{ X float64 }",
            "func (a V) + (b V) V { return V{a.X + b.X} }"]
    for i in range(n):
        if i == 0:
            out += ["func G0[T Adder[T]](a, b T) T { return a + b }"]
        else:
            out += ["func G%d[T Adder[T]](a, b T) T { return G%d[T](a, b) + a }" % (i, i - 1)]
    out += ["func main() { _ = G%d(V{}, V{}) }" % (n - 1)]
elif kind == "nested":
    out += ["type V struct{ X float64 }",
            "func (a V) + (b V) V { return V{a.X + b.X} }",
            "func f(a V, b V) V {"]
    e = "a"
    for i in range(n):
        e = "(%s + b)" % e
    out += ["\treturn " + e, "}", "func main() {}"]
elif kind == "mixed":
    out += ["type V struct{ X float64 }",
            "func (a V) + (b V) V { return V{a.X + b.X} }",
            "func (a V) [] (i int) V { return a }",
            "func (a V) () (x int) V { return a }",
            "@Convert",
            "func (a V) Float() float64 { return a.X }"]
    left, f = n, 0
    while left > 0:
        k = min(left, 1000)
        left -= k
        parts = [["a[%d]" % i, "a(%d)" % i, "a"][i % 3] for i in range(k)]
        out += ["func f%d(a V) V {" % f, "\treturn " + " + ".join(parts), "}"]
        f += 1
    out += ["func main() {}"]
elif kind == "inherit":
    out += ["type T0 struct{ X int }",
            "func (a T0) + (b T0) T0 { return a }",
            "func (a T0) <=> (b T0) int { return 0 }",
            "func (a T0) [] (i int) int { return i }",
            "func (a T0) () (x int) int { return x }",
            "func (a *T0) += (b T0) {}"]
    for i in range(1, n):
        out += ["type T%d extends T%d {}" % (i, i - 1)]
    for i in range(n):
        out += ["func use%d(a T%d, q T0) bool {" % (i, i),
                "	_ = a + q",
                "	a += q",
                "	return a <=> q < 0 && a[0] == a(1)",
                "}"]
    out += ["func main() {}"]
elif kind == "extover":
    out += ["type A0 struct{ x int }"]
    for i in range(1, n + 1):
        out += ["type A%d extends A%d { y%d int }" % (i, i - 1, i),
                "type P%d struct{ v int }" % i,
                "func (a A%d) + (b P%d) int { return 0 }" % (i, i)]
    out += ["func deep(a A%d, p P1) {" % n]
    out += ["	_ = a + p"] * 1
    out += ["}"]
    for i in range(1, n + 1):
        out += ["func use%d(a A%d, p P%d) {" % (i, i, (i * 7) % n + 1 if (i * 7) % n + 1 <= i else i),
                "	_ = a + p",
                "}"]
    out += ["func main() {}"]
elif kind in ("nestidx", "nestcall", "nestplus"):
    out += ["type V struct{ x int }",
            "func (a V) [] (i V) V { return i }",
            "func (a V) () (i V) V { return i }",
            "func (a V) + (b V) V { return b }"]
    if kind == "nestidx":
        e = "a[" * n + "a" + "]" * n
    elif kind == "nestcall":
        e = "a(" * n + "a" + ")" * n
    else:
        e = "a + (" * n + "a" + ")" * n
    out += ["func f(a V) V { return " + e + " }", "func main() {}"]
elif kind == "errors":
    out += ["type V struct{ X int }"]
    for k in range(50):
        out += ["type P%d struct{ Y int }" % k,
                "func (a V) * (b P%d) V { return a }" % k]
    out += ["func use(a V) {"]
    out += ["	_ = a * %d" % i for i in range(n)]
    out += ["}", "func main() {}"]
elif kind == "parallel":
    out += ["type Box[T Copy] struct{ V T }",
            "func (a Box[T]) + (b Box[T]) Box[T] { return a }",
            "func (a Box[T]) == (b Box[T]) bool { return true }",
            "func (a Box[T]) <=> (b Box[T]) int { return 0 }",
            "func (a Box[T]) [] (i int) T { return a.V }",
            "type Root struct{ X int }",
            "func (a Root) + (b Root) Root { return a }",
            "type L0 extends Root {}"]
    for i in range(1, 20):
        out += ["type L%d extends L%d {}" % (i, i - 1)]
    for i in range(n):
        t = ["int", "uint8", "float64", "bool"][i % 4]
        out += ["func f%d(a, b Box[%s], r L%d, q Root) {" % (i, t, i % 20),
                "	_ = a + b",
                "	_ = a < b",
                "	_ = a[0]",
                "	_ = a - b" if i % 3 == 0 else "	_ = a == b",
                "	_ = r + r" if i % 5 == 0 else "	_ = r + q",
                "}"]
    out += ["func main() {}"]
else:
    sys.exit("unknown kind " + kind)
print("\n".join(out))
