#!/usr/bin/env python3
# Generated plain-vs-generic table: for each operator form and each type,
# the generic body's result is printed next to the plain expression's.
# The program prints "MISMATCH <row>" when they differ. Spec oracle for the
# ordering forms under a `<=>` constraint is `(a <=> b) OP 0` (GEN-13).
# Regenerate: python3 gen-ops-table.py OUT.gopp, then paste OUT.gopp under the
# header of ../ops-generic-table.test (keep the row count in its golden).
# P04.23 review R2, R6: the overload sets (U has `+`/`*` for U and for W,
# `-` binary and unary; Meters has methods only for Seconds, so its
# `+ - *` between Meters are the built-in ones) are the point of the rows.
import sys
types = {
  "int8": ["-10", "11", "-1"],
  "uint": ["0", "1", "18446744073709551615"],
  "float32": ["fz", "1.5", "fnan", "-finf"],
  "float64": ["dz", "2.5", "dnan", "dinf"],
  "string": ['""', '"a"', '"ab"'],
  "U": ["U{1}", "U{-2}", "U{3}"],
  "Meters": ["1.5", "-2", "0"],
}
bin_ops = ["+", "-", "*"]
out = []
P = out.append
P("package main\n")
P("type U struct{ n int }")
P("type W struct{ n int }")
P("type Seconds float64")
P("type Meters float64\n")
P("func (a U) + (b W) U { return U{a.n + b.n*1000} }")
P("func (a U) * (b W) U { return U{a.n * b.n * 1000} }")
P("func (a Meters) + (b Seconds) Seconds { return Seconds(float64(a) + float64(b) + 1000) }")
P("func (a Meters) * (b Seconds) Seconds { return Seconds(float64(a) * float64(b) * 1000) }")
P("func (a U) + (b U) U { return U{a.n + b.n} }")
P("func (a U) - (b U) U { return U{a.n - b.n} }")
P("func (a U) * (b U) U { return U{a.n * b.n} }")
P("func (a U) - () U { return U{-a.n} }")
P("func (a U) == (b U) bool { return a.n == b.n }")
P("func (a U) <=> (b U) int { if a.n < b.n { return -1 }; if a.n > b.n { return 1 }; return 0 }")
P("func (a *U) ++ () { a.n += 10 }")
for op, name in [("+", "Add"), ("-", "Sub"), ("*", "Mul")]:
    P(f"func G{name}[T interface{{ Copy; {op} (T) T }}](a, b T) T {{ return a {op} b }}")
    P(f"func G{name}Eq[T interface{{ Copy; {op} (T) T }}](a, b T) T {{ x := a; x {op}= b; return x }}")
P("func GNeg[T interface{ Copy; - () T }](a T) T { return -a }")
P("func GEq[T interface{ == (T) bool }](a, b T) bool { return a == b }")
P("func GNe[T interface{ == (T) bool }](a, b T) bool { return a != b }")
P("func GCmp[T interface{ <=> (T) int }](a, b T) int { return a <=> b }")
for op, name in [("<", "Lt"), ("<=", "Le"), (">", "Gt"), (">=", "Ge")]:
    P(f"func G{name}[T interface{{ <=> (T) int }}](a, b T) bool {{ return a {op} b }}")
P("func GInc[T interface{ Copy; ++ () }](a T) T { x := a; x++; return x }")
#P("func GIdx[S interface{ [] (int) E }, E Copy](s S, i int) E { return s[i] }")
P("func GAddW[T interface{ Copy; + (W) T }](a T, b W) T { return a + b }")
P("func GMulW[T interface{ Copy; * (W) T }](a T, b W) T { return a * b }")
P("func GMulS[T interface{ * (Seconds) Seconds }](a T, b Seconds) Seconds { return a * b }")
P("var bad int\n")
for t in types:
    if t != "U":
        P(f"func id_{t}(x {t}) {t} {{ return x }}" if t != "string" else "func id_string(x string) string { return x + \"\" }")
P("func chk(row string, ok bool) { if !ok { bad++; println(\"MISMATCH\", row) } }\n")
P("func main() {")
P("\tfz32 := float32(0)\n\tfz := fz32\n\tfnan := fz / fz\n\tfinf := 1 / fz\n\t_ = finf")
P("\tdz := 0.0\n\tdnan := dz / dz\n\tdinf := 1 / dz\n\t_ = dinf")
def same(t, x, y):
    # equal results, treating NaN == NaN for floats (bit equality is not needed)
    if t.startswith("float"):
        return f"(({x}) == ({y}) || (({x}) != ({x}) && ({y}) != ({y})))"
    if t == "U":
        return f"(({x}).n == ({y}).n)"
    return f"(({x}) == ({y}))"
row = 0
for t, vals in types.items():
    for a in vals:
        for b in vals:
            A = f"id_{t}({t}({a}))" if t != "U" else a
            B = f"id_{t}({t}({b}))" if t != "U" else b
            for op, name in [("+", "Add"), ("-", "Sub"), ("*", "Mul")]:
                if t == "string":
                    continue
                row += 1
                P(f"\tchk(\"{row}\", {same(t, f'G{name}({A}, {B})', f'{A} {op} {B}')})")
                row += 1
                P(f"\tchk(\"{row}\", {same(t, f'G{name}Eq({A}, {B})', f'{A} {op} {B}')})")
            if t != "string":
                pass
            row += 1
            P(f"\tchk(\"{row}\", GEq({A}, {B}) == ({A} == {B}))")
            row += 1
            P(f"\tchk(\"{row}\", GNe({A}, {B}) == ({A} != {B}))")
            row += 1
            P(f"\tchk(\"{row}\", GCmp({A}, {B}) == ({A} <=> {B}))")
            for op, name in [("<", "Lt"), ("<=", "Le"), (">", "Gt"), (">=", "Ge")]:
                row += 1
                P(f"\tchk(\"{row}\", G{name}({A}, {B}) == (({A} <=> {B}) {op} 0))")
        if t not in ("string",):
            row += 1
            A = f"id_{t}({t}({a}))" if t != "U" else a
            P(f"\tchk(\"{row}\", {same(t, f'GNeg({A})', f'-{A}')})")
            row += 1
            if t != "U": P(f"\t{{ x := {A}; x++; chk(\"{row}\", {same(t, f'GInc({A})', 'x')}) }}")
#P("\tchk(\"idx slice\", GIdx[[]int, int]([]int{4, 5}, 1) == 5)")
#P("\tchk(\"idx string\", GIdx[string, byte](\"hey\", 1) == 'e')")
for u in ["U{1}", "U{-2}", "U{3}"]:
    for w in ["W{0}", "W{5}"]:
        row += 1
        P(f"\tchk(\"{row}\", GAddW({u}, {w}).n == ({u} + {w}).n)")
        row += 1
        P(f"\tchk(\"{row}\", GMulW({u}, {w}).n == ({u} * {w}).n)")
for m in ["1.5", "-2", "0"]:
    row += 1
    P(f"\tchk(\"{row}\", GMulS(id_Meters(Meters({m})), Seconds(2)) == id_Meters(Meters({m})) * Seconds(2))")
P("\tprintln(\"rows\", " + str(row) + ", \"bad\", bad)")
P("}")
open(sys.argv[1], "w").write("\n".join(out) + "\n")
