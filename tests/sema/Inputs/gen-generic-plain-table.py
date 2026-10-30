#!/usr/bin/env python3
"""P03.22 (R18): generic code and plain code decide the partial places the
same way. For every pair (use kind x type argument kind) this script writes
the plain form (the use written for the argument type itself) and the generic
form (the same code for a type parameter T, instantiated with the argument
type), checks both programs, and compares the results pair by pair. Each pair
is one line in the plain program and two in the generic one (the generic
function and its instantiation), so an error's line names its pair.

A pair whose generic form the constraint itself excludes (the type does not
satisfy it: `int` has no method Val, `Shared[*Animal]` is not Copy) is marked
`c`: there is no such instantiation, so nothing to compare. For every other
pair:

  - the generic form is rejected exactly when the plain form is rejected;
  - when the plain form is E0395 (a partial place), the generic form is
    rejected with E0316 (the instantiation) or E0395 (a body that is partial
    for every type of its constraint);
  - an "implicit requirement" error only comes with a rejected plain form;
  - when the plain form is E0371 (a View of a pointer kind or of a view,
    OWN-V1), the generic form is rejected with E0371 too, at the
    instantiation (P03.26).

KNOWN lists the pairs where the two differ by a rule that is not about
partial places, with the reason; the script fails if one of them stops
differing, so the list stays true.

Both programs are checked with one body thread and with eight, and the two
outputs must be the same.

Usage: gen-generic-plain-table.py --gopp <gopp> --out <dir>
Prints the table (one row per use kind, one column per argument kind: `.`
both accepted, `R` both rejected by the partial-place or abstract rule, `x`
both rejected by another rule, `c` excluded by the constraint, `k` a KNOWN
difference, `-` not applicable: `extends` needs a struct type) and exits
non-zero on a mismatch.
"""
import argparse
import os
import re
import subprocess
import sys

DECLS = """type Animal struct{ N int }

@Virtual
func (a *Animal) Speak() int { return 0 }

func (a Animal) Val() int      { return a.N }
func (move a Animal) Eat() int { return a.N }

type Dog extends Animal { Extra int }

type Pet struct{ N int }

@Virtual
func (p *Pet) Speak() int { return 0 }

type Cat extends Pet { Extra int }

func (c Cat) Val() int      { return c.N }
func (move c Cat) Eat() int { return c.N }

type Config struct{ Port int }

func (c Config) Val() int      { return c.Port }
func (move c Config) Eat() int { return c.Port }

type MyConfig extends Config { Debug bool }

type Plain struct{ N int }

func (p Plain) Val() int      { return p.N }
func (move p Plain) Eat() int { return p.N }

type Shape struct{ N int }

@Abstract
func (s *Shape) Area() int

type J interface{ Val() int }

type Box[T any] struct{ x T }

type PBox[T any] struct{ p *T }

type MkF[T any] func() T

type VAlias[T any] = View[T]

@View
type VBox[T any] struct{ v View[T] }
"""

# (name, type, method-expression spelling, is a struct type (extends works))
ARGS = [
    ("child-inherited", "Dog", "Dog", True),
    ("child-own", "Cat", "Cat", True),
    ("tagged", "Animal", "Animal", True),
    ("ptr-tagged", "*Animal", "(*Animal)", False),
    ("ptr-child", "*Dog", "(*Dog)", False),
    ("untagged-child", "MyConfig", "MyConfig", True),
    ("untagged-root", "Config", "Config", True),
    ("unique", "Unique[*Animal]", "Unique[*Animal]", False),
    ("shared", "Shared[*Animal]", "Shared[*Animal]", False),
    ("view", "View[Animal]", "View[Animal]", False),
    ("plain", "Plain", "Plain", True),
    ("int", "int", "int", False),
    ("abstract", "Shape", "Shape", True),
    ("func-tagged", "func() Animal", "(func() Animal)", False),
    ("func-abstract", "func() Shape", "(func() Shape)", False),
]

# (name, constraint, signature, body). {T} is the type (T or the argument),
# {ME} its method-expression spelling. The constraint `ext` is `extends A`
# and `root` is `extends Animal` (only for struct types), `exact` is
# `interface{ A }`.
USES = [
    ("method-value", "J", "(x {T}) int", "f := x.Val; return f()"),
    ("go", "J", "(x {T})", "go x.Val()"),
    ("defer", "J", "(x {T})", "defer x.Val()"),
    ("method-value-deref", "J", "(p *{T}) int", "f := (*p).Val; return f()"),
    ("method-expr", "J", "(x {T}) int", "f := {ME}.Val; return f(x)"),
    ("method-value-ext", "ext", "(x {T}) int", "f := x.Val; return f()"),
    ("go-ext", "ext", "(x {T})", "go x.Val()"),
    ("method-value-deref-ext", "ext", "(p *{T}) int",
     "f := (*p).Val; return f()"),
    ("method-value-ptr-ext", "ext", "(p *{T}) int", "f := p.Val; return f()"),
    ("method-expr-ext", "ext", "(x {T}) int", "f := {ME}.Val; return f(x)"),
    ("move-recv-ext", "ext", "(move x {T}) int", "return x.Eat()"),
    ("call-ext", "ext", "(x {T}) int", "return x.Val()"),
    ("method-value-root", "root", "(x {T}) int", "f := x.Val; return f()"),
    ("go-root", "root", "(x {T})", "go x.Val()"),
    ("move-recv-root", "root", "(move x {T}) int", "return x.Eat()"),
    ("copy", "Copy", "(p *{T})", "x := *p; _ = x"),
    ("swap", "any", "(p *{T}, q *{T})", "*p, *q = move(*q), move(*p)"),
    ("assign", "Copy", "(p *{T}, x {T})", "*p = x"),
    ("clone", "Clone", "(p *{T})", "_ = (*p).Clone()"),
    ("clone-builtin", "Clone", "(p *{T})", "_ = clone(*p)"),
    ("view-clone", "exact", "(v View[{T}])", "var c {T} = v.Clone(); _ = c"),
    ("view-sig", "any", "(v View[{T}])", "_ = v"),
    ("view-result", "any", "(v View[{T}]) View[{T}]", "return v"),
    ("view-body", "any", "(p *{T})", "var v View[{T}] = *p; _ = v"),
    ("view-closure", "any", "()", "f := func(v View[{T}]) {{}}; _ = f"),
    ("view-alias", "any", "(v VAlias[{T}])", "_ = v"),
    ("view-type", "any", "(v VBox[{T}])", "_ = v"),
    ("view-instance", "any", "(v View[Box[{T}]])", "_ = v"),
    ("ptr-instance", "any", "(p *Box[{T}])", "_ = p"),
    ("make", "any", "() int", "s := make([]{T}, 0); return len(s)"),
    ("hold", "any", "() int", "var s []{T}; return len(s)"),
    ("hold-option", "any", "()", "var z Option[{T}]; _ = z"),
    ("hold-instance", "any", "()", "var z Option[*Box[{T}]]; _ = z"),
    ("hold-pointer", "any", "()", "var z Option[*{T}]; _ = z"),
    ("hold-ptr-instance", "any", "()", "var z Option[*PBox[{T}]]; _ = z"),
    ("hold-func-result", "any", "()", "var f Option[func() {T}]; _ = f"),
    ("hold-func-param", "any", "()", "var f Option[func({T})]; _ = f"),
    ("hold-func-instance", "any", "()", "var f Option[MkF[{T}]]; _ = f"),
    ("zero", "exact", "()", "var z {T}; _ = z"),
    ("literal", "exact", "()", "z := {T}{{}}; _ = z"),
]

# The method route (P03.26, R1): the signature of a method of a generic type,
# `func (r G[T]) m<sig>`, against the plain `func (r P) m<sig>`; the body is
# None. Each row is also written through a generic function that holds
# `G[U]` (the "-via" rows), whose instantiation is the pair's second line.
USES += [
    ("method-param", "any", "(x {T})", None),
    ("method-slice", "any", "(x []{T})", None),
    ("method-result", "any", "() {T}", None),
    ("method-box", "any", "(b Box[{T}])", None),
    ("method-ptr-box", "any", "(b *Box[{T}])", None),
    ("method-view-box", "any", "(b View[Box[{T}]])", None),
    ("method-option", "any", "(o Option[{T}])", None),
    ("method-func", "any", "(f func({T}))", None),
    ("method-ptr-func", "any", "(f *func({T}))", None),
    ("method-via", "any", "(x []{T})", "via"),
    ("method-ptr-recv", "any", "(x []{T})", "ptr"),
    ("method-ptr-pointer", "any", "(x *{T})", None),
]

# A value of T written only inside a type argument that the generic holds
# behind a pointer (P03.26, R12): `PBox[[]T]` is refused for an abstract T as
# the plain `PBox[[]Shape]` is (E0389 at the written `[]Shape`), on every
# route: a signature (function rows), a field of a generic type ("field";
# "fieldrec" a recursive one, instantiated behind a pointer), a method
# signature (body None, "via"), a generic alias ("alias"). The controls:
# `Box[[]T]` (held by value), `PBox[*T]` and `PBox[T]` (no value of T).
USES += [
    ("arg-slice", "any", "(x PBox[[]{T}])", "_ = x"),
    ("arg-array", "any", "(x PBox[[1]{T}])", "_ = x"),
    ("arg-box", "any", "(x PBox[Box[{T}]])", "_ = x"),
    ("arg-func", "any", "(x PBox[func({T})])", "_ = x"),
    ("arg-func-result", "any", "(x PBox[func() {T}])", "_ = x"),
    ("arg-result", "any", "() PBox[[]{T}]", "panic(1)"),
    ("arg-nested", "any", "(x Box[PBox[[]{T}]])", "_ = x"),
    ("arg-held", "any", "(x Box[[]{T}])", "_ = x"),
    ("arg-pointer", "any", "(x PBox[*{T}])", "_ = x"),
    ("arg-bare", "any", "(x PBox[{T}])", "_ = x"),
    ("field-arg-slice", "any", "PBox[[]{T}]", "field"),
    ("field-arg-box", "any", "PBox[Box[{T}]]", "field"),
    ("field-arg-func", "any", "PBox[func({T})]", "field"),
    ("field-arg-pointer", "any", "PBox[*{T}]", "field"),
    ("fieldrec-arg-array", "any", "PBox[[1]{T}]", "fieldrec"),
    ("method-arg-slice", "any", "(x PBox[[]{T}])", None),
    ("method-arg-func", "any", "(x PBox[func({T})])", None),
    ("method-arg-box", "any", "(x PBox[Box[{T}]])", "via"),
    ("alias-arg-slice", "any", "PBox[[]{T}]", "alias"),
    ("alias-arg-array", "any", "PBox[[1]{T}]", "alias"),
]

# Differences that are not about partial places (use, argument): reason.
KNOWN = {}

ERR = re.compile(r"^[^:]+:(\d+):\d+: error\[(E\d+)\]: (.*)$")


def build():
    plain = ["package main", ""] + DECLS.splitlines()
    generic = list(plain)
    lines = {}  # (use, arg) -> (plain line, {generic lines})
    for ui, (use, cons, sig, body) in enumerate(USES):
        for ai, (arg, ty, me, struct) in enumerate(ARGS):
            if cons in ("ext", "root") and not struct:
                continue
            name = "u%d_a%d" % (ui, ai)
            if body in ("field", "fieldrec", "alias"):
                # A type (or alias) declaration holding the written type,
                # instantiated behind a pointer.
                rec = "next *{}; " if body == "fieldrec" else ""
                if body == "alias":
                    plain.append("type P%s = %s" % (name, sig.format(T=ty)))
                else:
                    plain.append("type P%s struct{ %sf %s }" % (
                        name, rec.format("P" + name), sig.format(T=ty)))
                g1 = len(generic) + 1
                if body == "alias":
                    generic.append("type G%s[T %s] = %s" % (
                        name, cons, sig.format(T="T")))
                else:
                    generic.append("type G%s[T %s] struct{ %sf %s }" % (
                        name, cons, rec.format("G%s[T]" % name),
                        sig.format(T="T")))
                generic.append("func i%s() { var z Option[*G%s[%s]]; _ = z }" % (
                    name, name, ty))
                lines[(use, arg)] = (len(plain), {g1, g1 + 1})
                continue
            if body is None or body in ("via", "ptr"):
                rc = "*" if body == "ptr" else ""
                res = "panic(1)" if sig.startswith("() ") else ""
                plain.append("type P%s struct{}; func (r %sP%s) m%s { %s }" % (
                    name, rc, name, sig.format(T=ty), res))
                g1 = len(generic) + 1
                generic.append(
                    "type G%s[T %s] struct{}; func (r %sG%s[T]) m%s { %s }" % (
                        name, cons, rc, name, sig.format(T="T"), res))
                if body == "via":
                    generic.append(
                        "func v%s[U any]() { var z G%s[U]; _ = z }" % (
                            name, name))
                    generic.append("func i%s() { v%s[%s]() }" % (
                        name, name, ty))
                    lines[(use, arg)] = (len(plain), {g1, g1 + 1, g1 + 2})
                    continue
                generic.append("func i%s() { var z G%s[%s]; _ = z }" % (
                    name, name, ty))
                lines[(use, arg)] = (len(plain), {g1, g1 + 1})
                continue
            plain.append("func p%s%s { %s }" % (
                name, sig.format(T=ty), body.format(T=ty, ME=me)))
            c = {"ext": "extends " + ty, "root": "extends Animal",
                 "exact": "interface{ %s }" % ty}.get(cons, cons)
            g1 = len(generic) + 1
            generic.append("func g%s[T %s]%s { %s }" % (
                name, c, sig.format(T="T"), body.format(T="T", ME="T")))
            generic.append("func i%s() { _ = g%s[%s] }" % (name, name, ty))
            lines[(use, arg)] = (len(plain), {g1, g1 + 1})
    plain += ["", "func main() {}", ""]
    generic += ["", "func main() {}", ""]
    return "\n".join(plain), "\n".join(generic), lines


def check(gopp, path):
    """The errors by line. The program is checked with one body thread and
    with eight: the uses are sets merged after the bodies, so the output
    must be the same (None if it is not)."""
    outs = [subprocess.run([gopp, "check", "--check-error-limit=0",
                            "-check-j=%d" % j, path],
                           capture_output=True, text=True, timeout=60).stderr
            for j in (1, 8)]
    if outs[0] != outs[1]:
        return None
    errs = {}
    for l in outs[0].splitlines():
        m = ERR.match(l)
        if m:
            errs.setdefault(int(m.group(1)), []).append(
                (m.group(2), m.group(3)))
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    ptext, gtext, lines = build()
    pp, gp = os.path.join(a.out, "plain.gopp"), os.path.join(a.out,
                                                             "generic.gopp")
    open(pp, "w").write(ptext)
    open(gp, "w").write(gtext)
    pe, ge = check(a.gopp, pp), check(a.gopp, gp)
    if pe is None or ge is None:
        print("MISMATCH one body thread and eight differ")
        return 1
    used = set()
    bad, rows = [], []
    counts = {k: 0 for k in ".Rxck"}
    for use, *_ in USES:
        row = ""
        for arg, *_ in ARGS:
            if (use, arg) not in lines:
                row += "-"
                continue
            pl, gls = lines[(use, arg)]
            used.add(pl)
            p = pe.get(pl, [])
            g = [e for gl in sorted(gls) for e in ge.get(gl, [])]
            pcodes = {c for c, _ in p}
            gcodes = {c for c, _ in g}
            implicit = any("implicit requirement" in m for _, m in g)
            excluded = bool(g) and all(
                "does not satisfy" in m and "implicit requirement" not in m
                for _, m in g)
            why = ""
            if excluded:
                mark = "c"
            elif bool(p) != bool(g):
                why = "plain %s, generic %s" % (
                    "rejected" if p else "accepted",
                    "rejected" if g else "accepted")
            elif "E0395" in pcodes and not gcodes & {"E0316", "E0395"}:
                why = "plain E0395, generic not E0316/E0395"
            elif "E0371" in pcodes and "E0371" not in gcodes:
                why = "plain E0371, generic not E0371"
            elif implicit and not p:
                why = "implicit requirement with an accepted plain form"
            if (use, arg) in KNOWN:
                if not why:
                    why = "listed in KNOWN but no longer different"
                else:
                    why, mark = "", "k"
            elif not why and not excluded:
                rule = "E0395" in pcodes or "E0389" in pcodes
                mark = "." if not p else ("R" if rule else "x")
            if why:
                bad.append("%s x %s: %s\n  plain: %s\n  generic: %s" % (
                    use, arg, why, p, g))
                row += "!"
                continue
            counts[mark] += 1
            row += mark
        rows.append("%-24s %s" % (use, row))
    stray = [l for l in pe if l not in used]
    stray_g = [l for l in ge
               if not any(l in g for _, g in lines.values())]
    print("args: " + " ".join("%d=%s" % (i, n) for i, (n, *_)
                              in enumerate(ARGS)))
    print("\n".join(rows))
    print("pairs: %d (accepted %d, rule %d, other %d, constraint %d, "
          "known %d), mismatches: %d" % (
              len(lines), counts["."], counts["R"], counts["x"], counts["c"],
              counts["k"], len(bad)))
    for (u, g), r in sorted(KNOWN.items()):
        print("KNOWN %s x %s: %s" % (u, g, r))
    for b in bad:
        print("MISMATCH " + b)
    for l in stray + stray_g:
        print("STRAY error at line %d" % l)
    return 1 if bad or stray or stray_g else 0


if __name__ == "__main__":
    sys.exit(main())
