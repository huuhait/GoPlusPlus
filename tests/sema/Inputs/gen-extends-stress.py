#!/usr/bin/env python3
"""Generates the hierarchy stress cases of P03.22: <case> <n>. Every case but
`ring` and `clash` is a valid program (nothing is reported); those two have
one error each, found at the end of a long walk.

  chain    C0 { x int } with a @Virtual M, C1 extends C0 ... Cn extends C(n-1),
           each overriding M with base.M(). Uses: field access x through n
           parents, a call of M, the upcasts *Cn to *C0 and Unique[*Cn] to
           Unique[*C0], the downcast and a type switch.
  rchain   the same chain declared in reverse order (Cn first).
  wide     P { x int } with a @Virtual M and n children W0 .. Wn each extending
           P and overriding M, and one function that upcasts every child.
  mixed    a chain of n/10 where each type also has 9 children (a tree).
  final    a chain of n with no @Virtual or @Abstract method anywhere: the
           inherited final method, base.M() and the root field on the last
           type (no upcast: there is none without a type table).
  generic  G0[T] ... G(i)[T] extends G(i-1)[T] for n types and an instance of
           the last one.
  fresh    a chain where each level adds a field and a @Virtual method of a
           new name (every name is looked up against every ancestor).
  upcast   a tagged chain where each level has a method that upcasts the
           receiver to *C0 (an ancestor test per level).
  abschain a chain of @Abstract types, each adding an @Abstract method; the
           last type implements them all (the open methods of every level).
  genover  a generic chain where each level overrides M and calls base.M().
  wideover one parent with 50 @Virtual methods and n/50 children that
           override all of them.
  local    n functions, each declaring a local type that extends the leaf of
           a tagged chain of 50, a literal of it and an upcast.
  untag    n generic functions g1 .. gn, each passing its pointers on to the
           one before, g0 swaps through them (`*p, *q = move(*q), move(*p)`,
           an implicit "must be untagged" requirement); main calls gn with
           an untagged type (valid).
  untagbad the same, main calls gn with a tagged type (one error).
  switch   a tagged parent with n children and one type switch over all.
  rootsel  a tagged chain where each level's method reads the root field and
           calls the root's method (a lookup that ends at the root, per level).
  clone    a chain where each level adds a slice field and a function that
           clones a value of the level (the clone walk, per level).
  lits     a chain where each level has a function with a literal that sets
           the root's field directly (the literal's key walk, per level).
  iface    a tagged chain where each level converts a pointer to an
           interface whose method is the root's (the method set, per level).
  zero     a chain whose root has a @Required field and a literal of each
           level that sets it (the required-field walk, per level).
  gensel   a generic chain whose level methods read the root field and call
           the root's method (the lookups walk the origin chain, no fresh
           instances).
  undef    a chain where each level's method reads an unknown name of its own
           (n errors, found without walking the chain; with the folded-case
           "did you mean" lookup too).
  ptrmeth  a tagged chain and n generic functions that take a method of T as
           a value, given a pointer to the leaf (n errors).
  deepask  a chain where each level adds a field of a new name, and one body
           on the leaf that reads them all (n names, each declared at its
           own depth: the chain's name index).
  rootmeth a root with n methods (each name also on an unrelated type) and a
           chain where level i calls the root's method i.
  gdeepask deepask on a generic chain (the index of the origins, with the
           type arguments of each ancestor).
  mvpass   n generic functions g1 .. gn, each passing its value on to the one
           before, g0 takes the method Val of T as a value; main calls each
           with a value child whose Val is inherited (n errors: the method use
           travels down n generics and is decided on the real type).
  holdchain n generic types G<k>[T] each holding G<k-1>[T], and n generic
           functions each holding Option[G<k>[T]]; main gives each an
           abstract type (n errors: the instance holds T, decided from the
           declarations without expanding the chain) and a concrete child.
  inferbox a generic chain C<k>[U] over Box[U] and a call Feed(c) per level
           with `Feed[T extends Box[U], U any]`: U is inferred from the
           ancestor instance of Box (the chain index, shared by the bodies).
  ring     a ring of n types (one `extends` cycle error).
  clash    a chain of n whose last type repeats the root's field name.
"""
import sys

case, n = sys.argv[1], int(sys.argv[2])
out = ["package main", ""]


def chain(order):
    d = ["type C0 struct{ x int }", "@Virtual", "func (c *C0) M() int { return c.x }", ""]
    parts = [d]
    for i in range(1, n + 1):
        parts.append([
            "type C%d extends C%d {}" % (i, i - 1),
            "@Override",
            "func (c *C%d) M() int { return base.M() + 1 }" % i,
            "",
        ])
    if order == "rev":
        parts = [parts[0]] + parts[:0:-1]
    for p in parts:
        out.extend(p)
    out.extend([
        "func f(c *C%d, move u Unique[*C%d], p *C0) int {" % (n, n),
        "\tvar b *C0 = c",
        "\tvar v Unique[*C0] = move(u)",
        "\td := p.(*C%d)" % n,
        "\tswitch e := p.(type) {",
        "\tcase *C%d:" % n,
        "\t\t_ = e",
        "\t}",
        "\t_ = v",
        "\t_ = d",
        "\treturn c.x + c.M() + b.x",
        "}",
    ])


if case == "chain":
    chain("fwd")
elif case == "rchain":
    chain("rev")
elif case == "wide":
    out += ["type P struct{ x int }", "@Virtual", "func (p *P) M() int { return p.x }", ""]
    for i in range(n):
        out += [
            "type W%d extends P { y int }" % i,
            "@Override",
            "func (w *W%d) M() int { return base.M() + w.y }" % i,
        ]
    out += ["func f(" + ", ".join("a%d *W%d" % (i, i) for i in range(n)) + ") int {", "\tt := 0"]
    for i in range(n):
        out += ["\tvar p%d *P = a%d" % (i, i), "\tt += p%d.M() + a%d.x" % (i, i)]
    out += ["\treturn t", "}"]
elif case == "mixed":
    k = max(n // 10, 1)
    out += ["type C0 struct{ x int }", "@Virtual", "func (c *C0) M() int { return c.x }"]
    for i in range(1, k + 1):
        out += ["type C%d extends C%d {}" % (i, i - 1), "@Override",
                "func (c *C%d) M() int { return base.M() + 1 }" % i]
        for j in range(9):
            out += ["type D%d_%d extends C%d { y int }" % (i, j, i), "@Override",
                    "func (d *D%d_%d) M() int { return base.M() + d.y }" % (i, j)]
    out += ["func f(c *C%d, d *D%d_8) int { var p *C0 = d; return c.M() + p.M() + d.x }" % (k, k)]
elif case == "final":
    out += ["type C0 struct{ x int }", "func (c *C0) Get() int { return c.x }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d {}" % (i, i - 1)]
    out += ["func (c *C%d) Twice() int { return base.Get() + c.Get() }" % n,
            "func f(c *C%d) int { return c.Twice() + c.x }" % n]
elif case == "generic":
    out += ["type G0[T any] struct{ v T }"]
    for i in range(1, n + 1):
        out += ["type G%d[T any] extends G%d[T] { w%d T }" % (i, i - 1, i)]
    out += ["func f(g *G%d[int]) int { return g.v }" % n]
elif case == "fresh":
    out += ["type C0 struct{ x0 int }", "@Virtual", "func (c *C0) M0() int { return 0 }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d { x%d int }" % (i, i - 1, i), "@Virtual",
                "func (c *C%d) M%d() int { return %d }" % (i, i, i)]
elif case == "upcast":
    out += ["type C0 struct{ x int }", "@Virtual", "func (c *C0) M() int { return 0 }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d {}" % (i, i - 1),
                "func (c *C%d) Up%d() *C0 { return c }" % (i, i)]
elif case == "abschain":
    out += ["type C0 struct{ x int }", "@Abstract", "func (c *C0) A0()"]
    for i in range(1, n + 1):
        out += ["@Abstract", "type C%d extends C%d {}" % (i, i - 1), "@Abstract",
                "func (c *C%d) A%d()" % (i, i)]
    out += ["type Last extends C%d {}" % n]
    for i in range(n + 1):
        out += ["@Override", "func (l *Last) A%d() {}" % i]
    out += ["func (l *Last) Up() *C0 { return l }"]
elif case == "genover":
    out += ["type G0[T any] struct{ v T }", "@Virtual", "func (g *G0[T]) M() int { return 0 }"]
    for i in range(1, n + 1):
        out += ["type G%d[T any] extends G%d[T] {}" % (i, i - 1), "@Override",
                "func (g *G%d[T]) M() int { return base.M() + 1 }" % i]
    out += ["func f(g *G%d[int]) int { var p *G0[int] = g; return p.M() }" % n]
elif case == "wideover":
    out += ["type P struct{ x int }"]
    for j in range(50):
        out += ["@Virtual", "func (p *P) M%d() int { return %d }" % (j, j)]
    for i in range(max(n // 50, 1)):
        out += ["type W%d extends P {}" % i]
        for j in range(50):
            out += ["@Override", "func (w *W%d) M%d() int { return base.M%d() }" % (i, j, j)]
elif case == "local":
    out += ["type C0 struct{ x int }", "@Virtual", "func (c *C0) M() int { return 0 }"]
    for i in range(1, 51):
        out += ["type C%d extends C%d {}" % (i, i - 1)]
    for i in range(n):
        out += ["func f%d() int {" % i,
                "\ttype L extends C50 { y%d int }" % i,
                "\tl := L{y%d: 1}" % i,
                "\tvar p *C0 = &l",
                "\treturn p.M() + l.y%d" % i,
                "}"]
elif case == "switch":
    out += ["type P struct{ x int }", "@Virtual", "func (p *P) M() int { return p.x }"]
    for i in range(n):
        out += ["type K%d extends P {}" % i]
    out += ["func f(p *P) int {", "\tswitch k := p.(type) {"]
    for i in range(n):
        out += ["\tcase *K%d:" % i, "\t\treturn k.x + %d" % i]
    out += ["\t}", "\treturn 0", "}"]
elif case in ("untag", "untagbad"):
    out += ["type A struct{ x int }"]
    if case == "untagbad":
        out += ["@Virtual", "func (a *A) M() int { return a.x }"]
    out += ["func g0[T any](p *T, q *T) { *p, *q = move(*q), move(*p) }"]
    for i in range(1, n + 1):
        out += ["func g%d[U any](p *U, q *U) { g%d(p, q) }" % (i, i - 1)]
    out += ["func main() {", "\ta, b := A{}, A{}", "\tg%d(&a, &b)" % n, "}"]
elif case == "rootsel":
    out += ["type C0 struct{ x int }", "func (c *C0) R() int { return c.x }",
            "@Virtual", "func (c *C0) M() int { return 0 }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d { y%d int }" % (i, i - 1, i),
                "func (c *C%d) F%d() int { return c.x + c.R() }" % (i, i)]
elif case == "clone":
    out += ["type C0 struct{ x []int }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d { y%d []int }" % (i, i - 1, i),
                "func g%d(c C%d) C%d { return c.Clone() }" % (i, i, i)]
elif case == "lits":
    out += ["type C0 struct{ x int }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d { y%d int }" % (i, i - 1, i),
                "func g%d() C%d { return C%d{x: 1, y%d: 2} }" % (i, i, i, i)]
elif case == "iface":
    out += ["type I interface{ R() int }", "type C0 struct{ x int }",
            "func (c *C0) R() int { return c.x }",
            "@Virtual", "func (c *C0) M() int { return 0 }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d {}" % (i, i - 1),
                "func g%d(c *C%d) I { return c }" % (i, i)]
elif case == "zero":
    out += ["type C0 struct{ x int @Required }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d {}" % (i, i - 1),
                "func g%d() C%d { return C%d{x: 1} }" % (i, i, i)]
elif case == "gensel":
    out += ["type C0[T any] struct{ x T }", "func (c *C0[T]) R() int { return 0 }"]
    for i in range(1, n + 1):
        out += ["type C%d[T any] extends C%d[T] { y%d T }" % (i, i - 1, i),
                "func (c *C%d[T]) F%d() int { _ = c.x; return c.R() }" % (i, i)]
    out += ["func use(c *C%d[int]) int { return c.R() }" % n]
elif case == "undef":
    out += ["type C0 struct{ x int }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d {}" % (i, i - 1),
                "func (c *C%d) F%d() int { return c.nope%d }" % (i, i, i)]
elif case == "ptrmeth":
    out += ["type C0 struct{ x int }", "@Virtual", "func (c *C0) M() int { return 0 }",
            "func (c C0) Val() int { return c.x }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d {}" % (i, i - 1),
                "func mv%d[T interface{ Val() int }](x T) int { f := x.Val; return f() }" % i]
    out += ["func f(p *C%d) int {" % n]
    for i in range(1, n + 1):
        out += ["	_ = mv%d(p)" % i]
    out += ["	return 0", "}"]
elif case == "deepask":
    out += ["type C0 struct{ x int }"]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d { y%d int }" % (i, i - 1, i)]
    out += ["func f(c *C%d) int {" % n, "	k := 0"]
    out += ["	k += c.y%d" % i for i in range(1, n + 1)]
    out += ["	return k", "}"]
elif case == "rootmeth":
    out += ["type C0 struct{ x int }"]
    for i in range(1, n + 1):
        out += ["type O%d struct{}" % i, "func (o O%d) M%d() int { return 0 }" % (i, i),
                "func (c *C0) M%d() int { return 0 }" % i]
    for i in range(1, n + 1):
        out += ["type C%d extends C%d {}" % (i, i - 1),
                "func (c *C%d) F%d() int { return c.M%d() }" % (i, i, i)]
elif case == "gdeepask":
    out += ["type C0[T any] struct{ x T }"]
    for i in range(1, n + 1):
        out += ["type C%d[T any] extends C%d[T] { y%d int }" % (i, i - 1, i)]
    out += ["func f(c *C%d[int]) int {" % n, "	k := 0"]
    out += ["	k += c.y%d" % i for i in range(1, n + 1)]
    out += ["	return k", "}"]
elif case == "mvpass":
    out += ["type A struct{ x int }", "@Virtual", "func (a *A) M() int { return a.x }",
            "func (a A) Val() int { return a.x }", "type D extends A {}",
            "type J interface{ Val() int }",
            "func g0[T J](x T) int { f := x.Val; return f() }"]
    for i in range(1, n + 1):
        out += ["func g%d[U J](x U) int { return g%d(x) }" % (i, i - 1)]
    out += ["func main() {", "	var d D"]
    out += ["	_ = g%d(d)" % i for i in range(1, n + 1)]
    out += ["}"]
elif case == "holdchain":
    out += ["type S struct{ N int }", "@Abstract", "func (s *S) Area() int",
            "type Q extends S {}", "@Override", "func (q *Q) Area() int { return 0 }",
            "type G0[T any] struct{ x T }"]
    for i in range(1, n + 1):
        out += ["type G%d[T any] struct{ g G%d[T] }" % (i, i - 1),
                "func h%d[T any]() int { var z Option[G%d[T]]; _ = z; return 0 }" % (i, i)]
    out += ["func main() {"]
    out += ["	_ = h%d[S]() + h%d[Q]()" % (i, i) for i in range(1, n + 1)]
    out += ["}"]
elif case == "inferbox":
    out += ["type Box[U any] struct{ v U }", "type C0[U any] extends Box[U] { f0 int }"]
    for i in range(1, n):
        out += ["type C%d[U any] extends C%d[U] { f%d int }" % (i, i - 1, i)]
    out += ["func Feed[T extends Box[U], U any](a *T) {}"]
    for i in range(n):
        out += ["func f%d(c *C%d[int]) { Feed(c) }" % (i, i)]
elif case == "ring":
    for i in range(n):
        out += ["type R%d extends R%d {}" % (i, (i + 1) % n)]
elif case == "clash":
    out += ["type C0 struct{ x int }"]
    for i in range(1, n):
        out += ["type C%d extends C%d { y%d int }" % (i, i - 1, i)]
    out += ["type C%d extends C%d { x int }" % (n, n - 1)]
else:
    sys.exit("unknown case " + case)
print("\n".join(out))
