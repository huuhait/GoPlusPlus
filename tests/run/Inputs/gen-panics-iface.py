#!/usr/bin/env python3
"""Writes the programs of Inputs/panics-iface/: one run-time panic each, for
interfaces (P04.06).

usage: gen-panics-iface.py DIR

Each program prints "before", then fails a type assertion, an == on an
uncomparable dynamic type, a map key hash or a method call on a nil
interface, so it exits with status 2 and Go's first line on standard error:
"panic: interface conversion: interface {} is string, not int", "... is nil,
not int", "interface is nil, not main.Shape", "main.Sq is not main.Namer:
missing method Name", "panic: runtime error: comparing uncomparable type
[]int", "hash of unhashable type ...", "invalid memory address or nil pointer
dereference". The programs are valid Go; gen-goldens.py --dir DIR DIR/*.gopp
writes the expected stdout (.out) and the first line of standard error
(.err) from `go run`.
"""
import os
import sys

TYPES = """type Shape interface{ Area() int }
type Namer interface{ Name() string }
type Sq struct{ S int }
type Big struct{ A, B, C int }
type Rect struct{ W, H int }
type Num int
type Holder struct{ V any }
type Bag struct{ Items []int }

func (s Sq) Area() int      { return s.S }
func (b Big) Area() int     { return b.A }
func (r *Rect) Area() int   { return r.W }
func (n Num) Area() int     { return int(n) }
"""

CASES = {}


def case(name, stmts, decls=TYPES):
    CASES[name] = (decls, stmts)


case("assert-string-int", 'var x any = "s"\n\t_ = x.(int)')
case("assert-int-string", "var x any = 5\n\t_ = x.(string)")
case("assert-nil-int", "var x any\n\t_ = x.(int)")
case("assert-nil-struct", "var x any\n\t_ = x.(Sq)")
case("assert-nil-iface", "var x any\n\t_ = x.(Shape)")
case("assert-missing-method", "var x any = Sq{1}\n\t_ = x.(Namer)")
case("assert-missing-method-ptr", "var r Rect\n\tvar x any = &r\n\t_ = x.(Namer)")
case("assert-iface-struct", "var x Shape = Sq{1}\n\t_ = x.(Big)")
case("assert-iface-ptr", "var r Rect\n\tvar x Shape = &r\n\t_ = x.(Sq)")
case("assert-big-box", "var x any = Big{1, 2, 3}\n\t_ = x.(Sq)")
case("assert-named-int", "var x any = Num(3)\n\t_ = x.(int)")
case("assert-int-named", "var x any = 3\n\t_ = x.(Num)")
case("assert-slice", "var x any = []int{1}\n\t_ = x.([]string)")
case("assert-map", 'var x any = map[string]int{"a": 1}\n\t_ = x.(map[string]string)')
case("assert-ptr-ptr", "var s Sq\n\tvar x any = &s\n\t_ = x.(*Big)")
case("assert-array", "var x any = [2]int{1, 2}\n\t_ = x.([3]int)")
case("assert-error", "var x error\n\t_ = x.(Shape)")
case("assert-after-output", 'println("one")\n\tvar x any = 1\n\tprintln(x.(int))\n\t_ = x.(bool)')
case("eq-slice", "var a, b any = []int{1}, []int{1}\n\tprintln(a == b)")
case("eq-slice-ne", "var a, b any = []int{1}, []int{2}\n\tprintln(a != b)")
case("eq-map", 'var a, b any = map[string]int{}, map[string]int{}\n\tprintln(a == b)')
case("eq-struct-slice", "var a, b any = Bag{}, Bag{}\n\tprintln(a == b)")
case("eq-struct-iface", "var a, b any = Holder{[]int{1}}, Holder{[]int{1}}\n\tprintln(a == b)")
case("eq-iface-array", "var a, b [2]any\n\ta[1] = []int{1}\n\tb[1] = []int{1}\n\tprintln(a == b)")
case("eq-same-error", "var a, b Shape = Sq{1}, Sq{1}\n\tprintln(a == b)\n\tvar c, d any = Holder{1}, Holder{[]int{2}}\n\tprintln(c == d)\n\tvar e, f any = Holder{[]int{1}}, Holder{[]int{1}}\n\tprintln(e == f)")
case("hash-slice-key", "m := map[any]int{}\n\tm[1] = 1\n\tm[[]int{1}] = 2\n\tprintln(len(m))")
case("nil-error-call", "var e error\n\tprintln(e.Error())", "")

# More forms (from the second implementation of P04.06): an interface with
# methods, a value type whose method has a pointer receiver, anonymous struct
# names, the nil `error`, == of different dynamic types (no panic) before one
# that panics, the order of the parts compared, and the hash of an
# unhashable key: in a non-empty map ("runtime error: hash of unhashable type
# T") and in a nil or empty one, where Go says "hash of unhashable type: T"
# (its mapKeyError path: a lookup, a comma-ok lookup and a delete).
MORE = """type I interface{ M() }

type J interface {
	M()
	N()
}

type A struct{}

func (A) M() {}

type B struct{ x int }

func (B) M()  {}
func (*B) N() {}

type E struct{}

func (E) Error() string { return "e" }

type S struct {
	f []int
}

type W struct {
	a any
	n int
}

type K struct{ a any }
"""

case("assert-error-iface", "var x error = E{}\n\t_ = x.(I)", MORE)
case("assert-float", "var x any = 1.5\n\t_ = x.(float32)", MORE)
case("assert-iface-chain", "var x I = A{}\n\tvar y any = x\n\t_ = y.(J)", MORE)
case("assert-iface-value-ptr", "var x I = A{}\n\t_ = x.(*A)", MORE)
case("assert-in-func", 'println(f("s"))',
     MORE + "\nfunc f(x any) int {\n\treturn x.(int)\n}\n")
case("assert-int-ptr", "var x any = 1\n\t_ = x.(*int)", MORE)
case("assert-missing-ptr-recv", "var x any = B{}\n\t_ = x.(J)", MORE)
case("assert-nil-any-any", "var x any\n\t_ = x.(any)", MORE)
case("assert-nil-error", "var x error\n\t_ = x.(E)", MORE)
case("assert-struct", "var x any = struct{ a int }{1}\n\t_ = x.(struct{ b int })", MORE)
case("eq-array-slice", "var x any = [1]any{[]int{}}\n\tvar y any = [1]any{[]int{}}\n\t_ = x == y", MORE)
case("eq-diff-types-ok", "var x any = []int{}\n\tvar y any = 1\n\tprintln(x == y)\n\t_ = x == x", MORE)
case("eq-nested-diff-first", "var x any = W{1, 1}\n\tvar y any = W{2, 1}\n\tprintln(x == y)\n\tvar z any = W{[]int{}, 1}\n\tvar u any = W{[]int{}, 2}\n\tprintln(z == u)\n\t_ = z == z", MORE)
case("hash-error-key", "type L struct{ s []int }\n\tvar x any = L{}\n\tm := map[any]int{1: 1}\n\t_ = m[x]", MORE)
case("hash-map", "var x any = map[string]int{}\n\tm := map[any]int{}\n\tm[x] = 1", MORE)
case("hash-nested", "var x any = W{[]int{}, 1}\n\tm := map[any]int{}\n\tm[x] = 1", MORE)
case("hash-struct", "var x any = S{}\n\tm := map[any]int{}\n\tm[x] = 1", MORE)
case("hash-delete", "var x any = []int{}\n\tm := map[any]int{1: 1}\n\tdelete(m, x)", MORE)
case("hash-lookup-empty", "var x any = []int{}\n\tm := map[any]int{}\n\t_ = m[x]", MORE)
case("hash-commaok-nil", "var x any = []int{}\n\tvar m map[any]int\n\t_, ok := m[x]\n\tprintln(ok)", MORE)
case("hash-delete-empty", "var x any = []int{}\n\tm := map[any]int{}\n\tdelete(m, x)", MORE)
case("hash-struct-key-empty", "m := map[K]int{}\n\t_ = m[K{[]int{}}]", MORE)
# Go's spelling of the types in the message: an interface written out (its
# methods sorted, exported first, an unexported one with its package, the
# embedded ones flattened), a variadic signature, a written-out source type.
case("assert-anon-iface", "var x any = A{}\n\t_ = x.(interface {\n\t\tM()\n\t\tN(int) string\n\t})", MORE)
case("assert-anon-iface-embed", "var x any = A{}\n\t_ = x.(interface {\n\t\terror\n\t\tm()\n\t\tM()\n\t})", MORE)
case("assert-anon-iface-variadic", "var x any = A{}\n\t_ = x.(interface{ V(string, ...int) (bool, error) })", MORE)
case("assert-anon-source", "var y interface{ M() } = A{}\n\t_ = y.(*A)", MORE)
# A struct written out with its tags in Go's quotes (review R3).
case("assert-struct-tag", "var x any = struct {\n\t\tA int `json:\"a\"`\n\t\tb string\n\t}{1, \"x\"}\n\t_ = x.(int)", MORE)
# strconv.Quote's escapes beyond ASCII (review R9): a byte that is not UTF-8
# (\xff), a no-break space, a zero-width space, a soft hyphen and a rune
# outside the BMP that is not printable (U+E0001) are escaped; U+FFFD written
# literally, a printable letter and a printable rune outside the BMP stay.
TAG_R9 = r"\xff\u00a0\u200b\u00ad\U000e0001" + "\ufffd\u00fc\U0001f600"
case("assert-struct-tag-escapes", "var x any = struct {\n\t\tA int \"x\\t\\\\y\"\n\t\tB int \"" + TAG_R9 +
     "\"\n\t}{1, 2}\n\t_ = x.(int)", MORE)


def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    for name, (decls, stmts) in CASES.items():
        src = "package main\n\n" + decls + "\n\nfunc main() {\n\tprintln(\"before\")\n\t" + stmts + \
              "\n\tprintln(\"not reached\")\n}\n"
        with open(os.path.join(d, name + ".gopp"), "w", encoding="utf-8") as f:
            f.write(src)


main()
