#!/usr/bin/env python3
"""Writes the generated interface tests of P04.06 (programs that are valid Go
and valid Go++; the expected output is `go run`'s).

usage: gen-iface-tables.py TESTS/RUN

  iface-table-methodsets.test  every dynamic type form against every
                               interface: does `any(x).(I)` hold (Go's method
                               sets: a value has the value-receiver methods, a
                               pointer both), and the result of each method
                               called through the interface
  iface-table-switch.test      a type switch of every case form (nil, one
                               type, a list of types, an interface, default, a
                               clause with no variable) over every kind of
                               value
  iface-table-eq.test          == and != of interface values: same and
                               different dynamic types, an interface and a
                               concrete value, NaN and +0/-0 inside, structs
                               and arrays that hold interfaces, pointers, map
                               keys

The failing forms (assertions, == of uncomparable types, unhashable keys) are
programs of Inputs/panics-iface (gen-panics-iface.py). Run
Inputs/regen-goldens.py on the three .test files afterwards (it needs Go; the
tests never do).
"""
import os
import sys

HEADER = """// REQUIRES: host-linux-amd64 || host-linux-arm64
// RUN: %python %S/Inputs/run-golden.py --src %s %S/golden/{name}.out
// {doc}
// The expected output is what `go run` prints for this very file
// (Inputs/regen-goldens.py); Inputs/gen-iface.py wrote it.
"""

# The dynamic types: a name, its declaration and the Go expressions of values
# of it (the second is made addressable first, see PTRS).
TYPES = [
    # (name, declarations, value expression)
    ("TV", "type TV struct{ n int }\nfunc (t TV) V() int { return t.n }", "TV{1}"),
    ("TP", "type TP struct{ n int }\nfunc (t *TP) P() int { t.n++; return t.n }", "TP{2}"),
    ("TB", "type TB struct{ n int }\nfunc (t TB) V() int { return t.n + 10 }\nfunc (t *TB) P() int { t.n++; return t.n + 20 }", "TB{3}"),
    ("NI", "type NI int\nfunc (n NI) V() int { return int(n) }\nfunc (n *NI) P() int { *n += 5; return int(*n) }", "NI(4)"),
    ("NF", "type NF float64\nfunc (f NF) V() int { return int(f * 2) }", "NF(1.5)"),
    ("NS", "type NS string\nfunc (s NS) V() int { return len(s) }", 'NS("abc")'),
    ("NB", "type NB bool\nfunc (b NB) V() int {\n\tif b {\n\t\treturn 1\n\t}\n\treturn 0\n}", "NB(true)"),
    ("SL", "type SL []int\nfunc (s SL) V() int { return len(s) }", "SL{1, 2, 3}"),
    ("AR", "type AR [3]int\nfunc (a AR) V() int { return a[0] + len(a) }\nfunc (a *AR) P() int { a[0]++; return a[0] }", "AR{7, 8, 9}"),
    ("BIG", "type BIG struct {\n\ta, b, c, d int\n\ts string\n}\nfunc (g BIG) V() int { return g.a + g.b + g.c + g.d + len(g.s) }", 'BIG{1, 2, 3, 4, "five"}'),
    ("EM", "type EM struct{}\nfunc (EM) V() int { return 42 }", "EM{}"),
    ("PT", "type PT struct{ x, y int32 }\nfunc (p PT) V() int { return int(p.x + p.y) }", "PT{3, 4}"),
]

IFACES = """type VI interface{ V() int }
type PI interface{ P() int }
type BI interface {
	V() int
	P() int
}
type AI interface {
	VI
	PI
}
"""


def methodsets():
    s = HEADER.format(
        name="iface-table-methodsets",
        doc="P04.06: method sets and dynamic dispatch, a table. For every type form (a "
        "struct with value, pointer or both kinds of methods, named scalars, a slice, an array, an empty "
        "struct, a big struct) the value and a pointer to it are put in an `any`, "
        "asserted to four interfaces (one of them embeds the other two) and the methods "
        "are called through the interface that holds them. Go's rule: a value has the "
        "methods of value receivers, a pointer has all of them.")
    s += "package main\n\n" + IFACES + "\n"
    for _, decl, _ in TYPES:
        s += decl + "\n\n"
    s += """func check(name string, x any) {
	_, v := x.(VI)
	_, p := x.(PI)
	_, b := x.(BI)
	_, a := x.(AI)
	println(name, v, p, b, a)
	if vi, ok := x.(VI); ok {
		println("  V", vi.V(), vi.V())
	}
	if pi, ok := x.(PI); ok {
		println("  P", pi.P(), pi.P())
	}
	if bi, ok := x.(BI); ok {
		println("  B", bi.V(), bi.P(), bi.V())
	}
	if ai, ok := x.(AI); ok {
		println("  A", ai.P(), ai.V())
	}
}

func direct(name string, vi VI) {
	println("direct", name, vi.V())
}

func main() {
"""
    for name, _, val in TYPES:
        s += "\t{\n\t\tv := %s\n\t\tcheck(%r, v)\n\t\tp := &v\n\t\tcheck(%r, p)\n" % (
            val, name, "*" + name)
        if name in ("TV", "TB", "NI", "NF", "NS", "NB", "SL", "AR", "BIG", "EM", "PT"):
            s += "\t\tdirect(%r, v)\n\t\tdirect(%r, p)\n" % (name, "*" + name)
        s += "\t}\n"
    s += "}\n"
    return s.replace("'", '"')


# Values for the switch and equality tables: (name, Go expression).
VALUES = [
    ("nil", "nil"),
    ("int", "1"),
    ("int64", "int64(2)"),
    ("uint8", "uint8(3)"),
    ("float64", "1.5"),
    ("string", '"s"'),
    ("bool", "true"),
    ("rune", "'x'"),
    ("TV", "TV{4}"),
    ("*TV", "&tv"),
    ("TB", "TB{5}"),
    ("*TB", "&tb"),
    ("SL", "SL{1}"),
    ("[]int", "[]int{1}"),
    ("[2]int", "[2]int{1, 2}"),
    ("struct", "struct{ a int }{6}"),
    ("map", "map[string]int{}"),
    ("E", "E{}"),
    ("error", "error(Err{})"),
    ("*int", "&iv"),
]

SWITCH_DECLS = """type TV struct{ n int }

func (t TV) V() int { return t.n }

type TB struct{ n int }

func (t TB) V() int   { return t.n }
func (t *TB) P() int  { return t.n }

type SL []int

func (s SL) V() int { return len(s) }

type E struct{}

type Err struct{}

func (Err) Error() string { return "err" }

type VI interface{ V() int }
type PI interface{ P() int }
"""


def switches():
    s = HEADER.format(
        name="iface-table-switch",
        doc="P04.06: type switches of every case form over every kind of value: nil, one "
        "type, a list of types (the variable keeps the interface type), an interface "
        "type, a pointer type, a clause that binds nothing, default in the middle and "
        "no default. The first clause that matches wins.")
    s += "package main\n\n" + SWITCH_DECLS + "\n"
    s += """func a(x any) int {
	switch v := x.(type) {
	case nil:
		return 1
	case int:
		return 10 + v
	case int64, uint8:
		return 20
	case float64:
		if v > 1 {
			return 30
		}
		return 31
	case string:
		return 40 + len(v)
	case bool, rune:
		return 50
	case VI:
		return 60 + v.V()
	case error:
		return 70 + len(v.Error())
	case *int:
		return 80
	case []int, [2]int:
		return 90
	case struct{ a int }:
		return 100 + v.a
	default:
		return -1
	}
}

func b(x any) int {
	switch x.(type) {
	case TV:
		return 1
	case *TB:
		return 2
	case PI:
		return 3
	case VI:
		return 4
	case nil:
		return 5
	}
	return 0
}

func c(x any) int {
	switch v := x.(type) {
	default:
		_ = v
		return -2
	case SL:
		return len(v)
	case E:
		return 7
	case map[string]int:
		return 8 + len(v)
	}
}

func d(x error) int {
	switch v := x.(type) {
	case nil:
		return 0
	case Err:
		return len(v.Error())
	default:
		return -3
	}
}

func e(x any) string {
	switch v := x.(type) {
	case int, string:
		_, isStr := v.(string)
		if isStr {
			return "str"
		}
		return "int"
	case any:
		_ = v
		return "any"
	}
	return "unreachable"
}

func main() {
	var iv int
	tv := TV{9}
	tb := TB{9}
	_, _, _ = iv, tv, tb
"""
    for name, expr in VALUES:
        s += "\tprintln(%s, a(%s), b(%s), c(%s), e(%s))\n" % (
            '"' + name + '"', expr, expr, expr, expr)
    s += "\tvar nilerr error\n\tprintln(d(nilerr), d(Err{}), d(nil))\n"
    s += "}\n"
    return s


def equalities():
    s = HEADER.format(
        name="iface-table-eq",
        doc="P04.06: == and != of interfaces, a table. The same dynamic type compares "
        "by value, different dynamic types are never equal (int and int64 with the "
        "same number are different), an interface and a concrete value compare as "
        "interfaces, NaN is never equal to itself inside an interface, +0 equals "
        "-0, interfaces nested in structs and arrays compare as their parts, "
        "pointers by address, and an interface is a map key. (Comparing "
        "uncomparable dynamic types panics: Inputs/panics-iface.)")
    s += """package main

type TV struct{ n int }

func (t TV) V() int { return t.n }

type VI interface{ V() int }

type Box struct {
	a any
	n int
}

type Pair struct {
	x, y any
}

type Err struct{ code int }

func (e Err) Error() string { return "e" }

func eq(name string, a, b any) {
	println(name, a == b, a != b)
}

func main() {
	var iv, iw int
	nan := 0.0
	nan = nan / nan
	var negz float64 = 0
	negz = -negz
	tv := TV{1}
	pa, pb := &iv, &iw
	eq("int=int", 1, 1)
	eq("int<>int", 1, 2)
	eq("int<>int64", 1, int64(1))
	eq("int<>uint", 1, uint(1))
	eq("int8<>uint8", int8(1), uint8(1))
	eq("str=str", "ab", "a"+"b")
	eq("str<>str", "ab", "ba")
	eq("str<>int", "1", 1)
	eq("float=float", 1.5, 1.5)
	eq("nan", nan, nan)
	eq("0=-0", 0.0, negz)
	eq("f32<>f64", float32(1), 1.0)
	eq("bool", true, true)
	eq("bool<>", true, false)
	eq("nil=nil", nil, nil)
	eq("nil<>int", nil, 0)
	eq("int<>nil", 0, nil)
	eq("struct=", TV{1}, TV{1})
	eq("struct<>", TV{1}, TV{2})
	eq("box=", Box{1, 2}, Box{1, 2})
	eq("box<>type", Box{1, 2}, Box{int64(1), 2})
	eq("box<>val", Box{"x", 2}, Box{"y", 2})
	eq("box nil", Box{nil, 2}, Box{nil, 2})
	eq("pair nested", Pair{Box{1, 1}, 2}, Pair{Box{1, 1}, 2})
	eq("pair nested <>", Pair{Box{1, 1}, 2}, Pair{Box{1, 2}, 2})
	eq("pair nan", Pair{nan, 1}, Pair{nan, 1})
	eq("array=", [2]any{1, "a"}, [2]any{1, "a"})
	eq("array<>", [2]any{1, "a"}, [2]any{1, "b"})
	eq("ptr=", pa, pa)
	eq("ptr<>", pa, pb)
	eq("ptr<>val", pa, iv)
	eq("tv ptr", &tv, &tv)
	eq("err=", Err{1}, Err{1})
	eq("err<>", Err{1}, Err{2})
	var e1, e2 error = Err{1}, Err{1}
	println("error", e1 == e2, e1 != e2, e1 == Err{1}, Err{2} == e1)
	var v1 VI = tv
	var v2 VI = TV{1}
	var x any = tv
	println("iface", v1 == v2, v1 == x, x == v1, x == any(v2), v1 == TV{1}, TV{1} == v1, v1 == TV{2})
	var n1, n2 error
	println("nil errors", n1 == n2, n1 == nil, nil == n1, n1 != nil)
	var a1, a2 any
	println("nil any", a1 == a2, a1 == nil, a1 != nil)
	a1 = 1
	println("set any", a1 == nil, a1 != nil, a1 == 1, 1 == a1, a1 == 2, a1 == "1")
	m := map[any]int{}
	m[1] = 1
	m[int64(1)] = 2
	m["a"] = 3
	m[1.5] = 4
	m[TV{1}] = 5
	m[Box{1, 2}] = 6
	m[nil] = 7
	m[true] = 8
	m[pa] = 9
	m[[2]int{1, 2}] = 10
	m[nan] = 11
	m[nan] = 12
	println(len(m), m[1], m[int64(1)], m["a"], m[1.5], m[TV{1}], m[Box{1, 2}], m[nil], m[true], m[pa], m[pb], m[[2]int{1, 2}], m[nan], m[2])
	delete(m, 1)
	delete(m, nil)
	println(len(m), m[1], m[nil], m[int64(1)])
	var key error = Err{3}
	me := map[error]string{}
	me[key] = "three"
	me[Err{4}] = "four"
	println(len(me), me[Err{3}], me[Err{4}], me[Err{5}] == "")
}
"""
    return s


def main():
    root = sys.argv[1]
    for fn, name in ((methodsets, "iface-table-methodsets"),
                     (switches, "iface-table-switch"),
                     (equalities, "iface-table-eq")):
        with open(os.path.join(root, name + ".test"), "w") as f:
            f.write(fn())


if __name__ == "__main__":
    main()
