#!/usr/bin/env python3
"""P03.26 stress for the uses that a View[T] (ViewArg) or an instance held
behind a pointer or a view (`*Box[T]`, `View[Box[T]]`: a Make use) gives a
type parameter, decided on every type argument (GEN-12, untaggedCheck).
Each program has N sibling or nested cases and exactly one bad
instantiation (or none with `ok`):

  sigs N [ok]     N generics with View[T] in the signature, each instantiated
                  with int; the last one also with *int
  chain N [ok]    N generics, each passing T on to the one before; the first
                  writes View[T] in its body; the last instantiated with int
                  and with *int (the use travels N levels)
  types N [ok]    N generic types with a View[T] in a func field, each used
                  with int; the first also with *int
  behind N [ok]   N generic types, each holding the one before behind a
                  pointer, the first holding *Box[T]; the last used with int
                  and with an abstract type
  views N [ok]    N generics with View[Box[T]] in the signature, each
                  instantiated with int; the last one also with an abstract
                  type
"""
import sys

shape, n = sys.argv[1], int(sys.argv[2])
ok = "ok" in sys.argv[3:]
out = ["package main", "",
       "type Shape struct{ N int }", "", "@Abstract",
       "func (s *Shape) Area() int", "",
       "type Box[T any] struct{ x T }", ""]
if shape == "sigs":
    for k in range(n):
        out.append("func f%d[T any](v View[T]) {}" % k)
    out.append("func use() {")
    out += ["\t_ = f%d[int]" % k for k in range(n)]
    if not ok:
        out.append("\t_ = f%d[*int]" % (n - 1))
    out.append("}")
elif shape == "chain":
    out.append("func g0[T any](p *T) { var v View[T] = *p; _ = v }")
    for k in range(1, n):
        out.append("func g%d[T any](p *T) { g%d[T](p) }" % (k, k - 1))
    out.append("func use() {")
    out.append("\t_ = g%d[int]" % (n - 1))
    if not ok:
        out.append("\t_ = g%d[*int]" % (n - 1))
    out.append("}")
elif shape == "types":
    for k in range(n):
        out.append("type H%d[T any] struct{ f func(View[T]) }" % k)
    for k in range(n):
        out.append("func u%d(h H%d[int]) {}" % (k, k))
    if not ok:
        out.append("func bad(h H0[*int]) {}")
elif shape == "behind":
    out.append("type B0[T any] struct{ p *Box[T] }")
    for k in range(1, n):
        out.append("type B%d[T any] struct{ p *B%d[T] }" % (k, k - 1))
    out.append("func good(b *B%d[int]) {}" % (n - 1))
    if not ok:
        out.append("func bad(b *B%d[Shape]) {}" % (n - 1))
elif shape == "views":
    for k in range(n):
        out.append("func h%d[T any](v View[Box[T]]) {}" % k)
    out.append("func use() {")
    out += ["\t_ = h%d[int]" % k for k in range(n)]
    if not ok:
        out.append("\t_ = h%d[Shape]" % (n - 1))
    out.append("}")
else:
    sys.exit("unknown shape " + shape)
out += ["", "func main() {}"]
print("\n".join(out))
