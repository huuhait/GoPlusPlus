#!/usr/bin/env python3
"""usage: gen-chain-marks.py <ok|bad|expect> <n>...

P03.26 (R10b): the values a chain form sets for the forms below it, with n
parentheses between the form that sets the value and the one that reads it,
inside chains of every link kind (selector, index, slice, type assertion,
call, `*`, parentheses). Each case comes short and with a long tail or a
long run of outer parentheses, so the chain is checked both by the
recursive forms and by Checker::prepareChain's loop (past 16 links). The
values (Checker::ChainCtx):
- the mark of a selector: `*p` and the parent part `dog.Animal` may start a
  selector (EXP-15);
- the mark of `&`: `&*p`, `&dog.Animal`;
- the receiver of IsNil / Get that is not narrowed (an Option `o` and
  `move(o)`), and `&o`;
- the callee of a call: `o.M()` on a checked Option is a call, not a method
  value, unless it is the call of a `go` / `defer` (OWN-P10);
- the mark of an index or a call on the receiver of an operator method
  (`(*p)[i]`, `(*p)[i] = v`, `(*p)[i] += v`, `(*p)(x)`, ADR-0024).
`ok` prints the accepted uses; `bad` the same shapes without the form that
allows them; `expect` the `<line> <code>` list of errors of `bad`. One
program holds every n given.
"""
import sys

mode, sizes = sys.argv[1], [int(a) for a in sys.argv[2:]]
LONG = 20  # outer parentheses / tail links that make a chain long
n = 0  # the parentheses between the setting form and the reading one


def par(x, k=None):
    k = n if k is None else k
    return "(" * k + x + ")" * k


# Operands of type *Animal (and *Dog) ending in every link kind; each also
# with a long tail of selectors under it.
def belows(root, field, elems, typ, me):
    tail = root + ("." + field) * LONG
    out = []
    for r in (root, tail):
        out += [
            r,  # the name (or the tail)
            r + "." + field,  # selector
            r + "." + elems + "[0]",  # index
            r + "." + elems + "[1:][0]",  # slice, index
            r + ".i.(*" + typ + ")",  # type assertion
            r + "." + me + "()",  # call
            "(*%s).%s" % (r, field),  # `*`, selector
            par(r, 2),  # parentheses
        ]
    return out


# What may come above a selector (the rest of the chain).
ABOVE = ["", ".as[0].N", ".as[1:]", ".i.(*Animal).N", ".Me().N", "", ".Val()"]

hdr = """package main

type Node struct{ V int }
type Big struct{ P *Node }

func (b *Big) M() int { return b.P.V }

type Animal struct {
	N  int
	s  *Animal
	as []*Animal
	i  any
}

@Virtual
func (a *Animal) Speak() int { return 0 }
func (a *Animal) Me() *Animal { return a }

func (a *Animal) [] (k int) int     { return a.N }
func (a *Animal) []= (k int, v int) { a.N = v }
func (a *Animal) () (k int) int     { return a.N }
func (a Animal) Val() int     { return a.N }

type Dog extends Animal {
	d  *Dog
	ds []*Dog
}

func (d *Dog) Me2() *Dog { return d }

func use(x ...any) {}
"""

lines = hdr.split("\n")
expect = []
fn = [0]


def func(params, body, codes=()):
    """Adds a function; codes are the errors of the body's lines (bad)."""
    fn[0] += 1
    lines.append("func f%d(%s) {" % (fn[0], params))
    for i, b in enumerate(body):
        lines.append("\t" + b)
        if i < len(codes) and codes[i]:
            expect.append("%d %s" % (len(lines), codes[i]))
    lines.append("}")


def cases():
    AP = "p *Animal, dog *Dog"
    for b in belows("p", "s", "as", "Animal", "Me"):
        for k, up in enumerate(ABOVE):
            if mode == "ok":
                # The mark of a selector: `*p` starts it (k == 5: a selector
                # inside a long run of outer parentheses).
                sel = par(par("*" + b) + ".s" + up, LONG if k == 5 else 0)
                func(AP, ["use(%s)" % sel])
            elif k == 0:
                func(AP, ["x := %s" % par("*" + b), "use(x)"], ["E0395"])
        if mode == "ok":
            # Operator methods (ADR-0024): `*p` as the receiver of a `[]`,
            # `[]=`, `op=` through them, or `()` method marked by the index
            # or call above it.
            func(AP, ["use(%s[0])" % par("*" + b), "use(%s(1))" % par("*" + b),
                      "%s[0] = 1" % par("*" + b), "%s[0] += 1" % par("*" + b),
                      "use(%s)" % par(par("*" + b) + "[0]", LONG)])
            # The mark of `&`.
            func(AP, ["q := &%s" % par("*" + b), "use(q)"])
            func(AP, ["q := &%s" % par(par("*" + b), LONG), "use(q)"])
    for b in belows("dog", "d", "ds", "Dog", "Me2"):
        for k, up in enumerate(ABOVE):
            if mode == "ok":
                sel = par(par(b + ".Animal") + ".N", LONG if k == 5 else 0)
                func(AP, ["use(%s)" % sel])
                func(AP, ["use(%s)" % (par(b + ".Animal") + ".s" + up)])
            elif k == 0:
                func(AP, ["x := %s" % par(b + ".Animal"), "use(x)"], ["E0395"])
        if mode == "ok":
            func(AP, ["q := &%s" % par(b + ".Animal"), "use(q)"])
            func(AP, ["q := &%s" % par(par(b + ".Animal"), LONG), "use(q)"])

    # The receiver of IsNil / Get that is not narrowed, and `&o`.
    for outer in (0, LONG):
        if mode == "ok":
            func("o Option[*Node]", ["if o != nil {",
                                     "\tuse(%s)" % par(par("o") + ".IsNil()", outer),
                                     "}"])
            func("o Option[*Node]", ["if o != nil {",
                                     "\tuse(%s)" % par(par("move(o)") + ".IsNil()", outer),
                                     "}"])
            func("o Option[*Node]", ["if o != nil {",
                                     "\tvar q *Option[*Node] = &%s" % par(par("o"), outer),
                                     "\tuse(q)",
                                     "}"])
        else:
            func("o *Node", ["use(%s)" % par(par("o") + ".IsNil()", outer)], ["E0178"])
            func("o Option[*Node]", ["if o != nil {",
                                     "\tvar q **Node = &%s" % par(par("o"), outer),
                                     "\tuse(q)",
                                     "}"], [None, "E0314"])

    # The callee of a call.
    for outer in (0, LONG):
        if mode == "ok":
            func("move o Option[Big], r Option[*Big]",
                 ["if o != nil {",
                  "\tuse(%s)" % par(par("o.M") + "()", outer),
                  "\to = nil",
                  "}",
                  "if r != nil {",
                  "\tdefer %s()" % par(par("r.M"), outer),
                  "\tgo %s()" % par(par("r.M"), outer),
                  "}"])
        else:
            func("move o Option[Big], move q Option[Big]",
                 ["if o != nil {",
                  "\tf := %s" % par(par("o.M"), outer),
                  "\to = nil",
                  "\tuse(f)",
                  "}",
                  "if q != nil {",
                  "\tdefer %s()" % par(par("q.M"), outer),
                  "\tgo %s()" % par(par("q.M"), outer),
                  "\tq = nil",
                  "}"],
                 [None, "E0329", None, None, None, None, "E0329", "E0329"])


for n in sizes:
    cases()
lines.append("func main() {}")
if mode == "expect":
    print("\n".join(expect))
else:
    print("\n".join(lines))
