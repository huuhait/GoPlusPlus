#!/usr/bin/env python3
"""Independent plain-vs-generic oracle for GEN-14 (reviewer, P03.25 final).

Random bodies of G[U unit, V unit](a float64[U], b float64[V]) float64 over
`*`, `/`, `+` with a scale change, `.In[...]()`, `float64[...](e)`, literals
with concrete units and literals with parameter units. For each instance
(U, V) -- including units that are EMPTY at the instance, as a generic caller
passing `X/Y` makes them -- two exact evaluations:

  plain:   the quantity in base units (EXP-33: a scale change keeps the
           quantity; a product or quotient multiplies quantities);
  generic: what code compiled once computes from the records gopp dumps
           (unit-site, unit-conv), with c(E) = factor(E) if E is empty at the
           instance else 1, exactly as GEN-14 "Where the factor is applied"
           says; a constant subtree is its folded number times c(E).

Also checks that every parameter whose c(...) the generic evaluation needed
(c != 1) is in `unit-dict`.

The P03.25 final reviewer's oracle (R12 to R16), kept as a test generator.
A node may have two sites (its own value, and its scale change as an
operand), told apart by their form (`= X op Y` and `from X`; `const` lines
are the constants, evaluated as their folded number times c(E)). A product or
quotient whose unit is empty whatever the instance is plain where the code is
checked (EXP-33: the factor is in `unit-conv`, the type has none).

Usage: gen-units-oracle-ops.py GOPP OUTDIR SEEDS [FIRST-SEED]
"""
import os
import random
import re
import subprocess
import sys
from fractions import Fraction as F

BASE = {"M": ({"M": 1}, F(1)), "KM": ({"M": 1}, F(1000)),
        "S": ({"S": 1}, F(1)), "H": ({"S": 1}, F(3600))}
HEAD = "package main\n\nunit M\nunit KM = 1000 M\nunit S\nunit H = 3600 S\n\n"


def mul(a, b, s=1):
    d = dict(a[0])
    for k, e in b[0].items():
        d[k] = d.get(k, 0) + s * e
        if d[k] == 0:
            del d[k]
    return (d, a[1] * b[1] ** s)


ONE = ({}, F(1))


def subst(u, env):
    r = ({}, u[1])
    for k, e in u[0].items():
        base = env[k] if k in env else BASE[k] if k in BASE else ({k: 1}, F(1))
        for _ in range(abs(e)):
            r = mul(r, base, 1 if e > 0 else -1)
    return r


def c(u):
    return u[1] if not u[0] else F(1)


class N:
    pass


def mk(kind, *kids, **kw):
    n = N()
    n.kind, n.kids = kind, list(kids)
    n.__dict__.update(kw)
    return n


SCALES = [("KM/M", F(1000)), ("M/KM", F(1, 1000)), ("H/S", F(3600))]


def static(n):
    """(unit over U, V and bases, written text of the unit or None)."""
    k = n.kind
    if k == "a":
        return ({"U": 1}, F(1)), "U"
    if k == "b":
        return ({"V": 1}, F(1)), "V"
    if k == "lit":
        return n.unit, n.utext
    if k in "*/":
        (x, tx), (y, ty) = static(n.kids[0]), static(n.kids[1])
        u = mul(x, y, 1 if k == "*" else -1)
        if not u[0]:
            # EXP-33 where the code is checked: a plain number, the factor
            # applied to the value (unit-conv), so the type has no factor
            return ONE, None
        if tx is None or ty is None:
            return u, None
        return u, "(%s)%s(%s)" % (tx, k, ty)
    if k in ("in", "conv"):
        (x, tx) = static(n.kids[0])
        return mul(x, ({}, n.scale[1])), "(%s)*%s" % (tx, n.scale[0])
    if k == "add":
        (x, tx), (y, ty) = static(n.kids[0]), static(n.kids[1])
        return (x, tx) if x[1] <= y[1] else (y, ty)


def has_unit(n):
    return bool(static(n)[0][0])


def rand(rng, d):
    if d == 0 or rng.random() < 0.25:
        r = rng.random()
        if r < 0.35:
            return mk("a")
        if r < 0.7:
            return mk("b")
        if r < 0.85:
            nm = rng.choice(list(BASE))
            return mk("lit", val=F(rng.choice([2, 3, 5])), unit=BASE[nm], utext=nm)
        # a literal with a parameter unit
        t = rng.choice([("U*KM", ({"U": 1, "M": 1}, F(1000))),
                        ("U/V", ({"U": 1, "V": -1}, F(1))),
                        ("U*KM/M", ({"U": 1}, F(1000))),
                        ("V/(U*H)", ({"V": 1, "U": -1, "S": -1}, F(1, 3600)))])
        return mk("lit", val=F(rng.choice([2, 3])), unit=t[1], utext=t[0])
    r = rng.random()
    if r < 0.5:
        return mk(rng.choice("*/"), rand(rng, d - 1), rand(rng, d - 1))
    x = rand(rng, d - 1)
    if not has_unit(x) or static(x)[1] is None:
        return mk(rng.choice("*/"), x, rand(rng, d - 1))
    if r < 0.65:
        return mk("in", x, scale=rng.choice(SCALES))
    if r < 0.8:
        return mk("conv", x, scale=rng.choice(SCALES))
    # e + float64[unit(e)*s](e'), with e' a copy of e
    y = mk("conv", clone(x), scale=rng.choice(SCALES))
    return mk("add", x, y) if rng.random() < 0.5 else mk("add", y, x)


def clone(n):
    m = N()
    m.__dict__.update(n.__dict__)
    m.kids = [clone(k) for k in n.kids]
    return m


def render(n, pos, bare=False):
    """Text of n starting at column pos; sets n.col/n.text (the node key) and
    n.okey, the key of the operand expression as its parent sees it (the
    paren around a binary, unless bare)."""
    k = n.kind
    if k in ("a", "b"):
        n.col, n.text = pos, k
        n.okey = (pos, k)
        return k
    if k == "lit":
        n.col = pos
        n.text = "%d.0[%s]" % (n.val, n.utext)
        n.okey = (pos, n.text)
        return n.text
    if k in "*/" or k == "add":
        op = {"*": " * ", "/": " / ", "add": " + "}[k]
        start = pos if bare else pos + 1
        l = render(n.kids[0], start)
        r = render(n.kids[1], start + len(l) + 3)
        n.col, n.text = start, l + op + r
        if bare:
            n.okey = (n.col, n.text)
            return n.text
        n.okey = (pos, "(" + n.text + ")")
        return "(" + n.text + ")"
    if k == "in":
        x = n.kids[0]
        tx = static(x)[1]
        inner = render(x, pos + 1, bare=True)
        n.pkey = (pos, "(" + inner + ")")
        t = "(%s).In[(%s)*%s]()" % (inner, tx, n.scale[0])
        n.col, n.text = pos, t
        n.okey = (pos, t)
        return t
    if k == "conv":
        x = n.kids[0]
        tx = static(x)[1]
        head = "float64[(%s)*%s](" % (tx, n.scale[0])
        inner = render(x, pos + len(head), bare=not os.environ.get("AVOID"))
        t = head + inner + ")"
        n.col, n.text = pos, t
        n.okey = (pos, t)
        return t


def nodes(n):
    yield n
    for k in n.kids:
        yield from nodes(k)


def avoid(e):
    for n in nodes(e):
        u = static(n)[0]
        params = [k for k in u[0] if k in ("U", "V")]
        if len(u[0]) == 1 and params and u[1] != 1:
            return True  # R13: a lone parameter with a factor
        if is_const(n) and params:
            return True  # R12: a constant with a parameter unit
    return False


def is_const(n):
    if n.kind in ("a", "b"):
        return False
    return all(is_const(k) for k in n.kids)


def plain(n, env, na, nb):
    k = n.kind
    if k == "a":
        return na * subst(({"U": 1}, F(1)), env)[1]
    if k == "b":
        return nb * subst(({"V": 1}, F(1)), env)[1]
    if k == "lit":
        return n.val * subst(n.unit, env)[1]
    if k == "*":
        return plain(n.kids[0], env, na, nb) * plain(n.kids[1], env, na, nb)
    if k == "/":
        return plain(n.kids[0], env, na, nb) / plain(n.kids[1], env, na, nb)
    if k in ("in", "conv"):
        return plain(n.kids[0], env, na, nb)
    if k == "add":
        return plain(n.kids[0], env, na, nb) + plain(n.kids[1], env, na, nb)


def parse_unit(t):
    if t == "plain":
        return ONE
    f = F(1)
    m = re.match(r"^\((\d+)/(\d+)\) (.*)$", t) or re.match(r"^(\d+)() (.*)$", t)
    if m:
        f = F(int(m.group(1)), int(m.group(2) or 1))
        t = m.group(3)
    num, _, den = t.partition("/")
    d = {}
    for part, s in ((num, 1), (den.strip("()"), -1)):
        for term in part.split("*"):
            if not term or term == "1":
                continue
            nm, _, e = term.partition("^")
            d[nm] = d.get(nm, 0) + s * int(e or 1)
    return (d, f)


def tk(col, text):
    return (col, text)  # the dump prints the whole text of these records


class Gen:
    def __init__(self, sites, convs, env):
        self.sites, self.convs, self.env, self.need = sites, convs, env, set()

    def cc(self, u):
        v = c(subst(u, self.env))
        if v != 1:
            self.need |= {k for k in u[0] if k in ("U", "V")}
        return v

    def site(self, col, text, role):
        """The site of the node (col, text) with the role: "muldiv" (its own
        value) or "conv" (its scale change as an operand)."""
        recs = [r for r in self.sites.get(tk(col, text), [])
                if not r.endswith(" const")
                and ((" from " in r) == (role == "conv"))]
        if not recs:
            return F(1), None
        rec = recs[0]
        if " from " in rec:
            rec = rec[:-len(" tag")] if rec.endswith(" tag") else rec
            r, x = rec.split(" from ")
            return self.cc(parse_unit(r)) / self.cc(parse_unit(x)), "conv"
        r, rest = rec.split(" = ")
        op = " * " if " * " in rest else " / "
        x, y = rest.split(op)
        R, X, Y = (self.cc(parse_unit(t)) for t in (r, x, y))
        return (R / (X * Y) if op == " * " else R * Y / X), "muldiv"

    def conv(self, col, text):
        return self.convs.get(tk(col, text), F(1))

    def folded(self, n):
        """The folded number of a constant subtree (no c applied)."""
        k = n.kind
        if k == "lit":
            return n.val
        if k in "*/":
            x, y = self.folded(n.kids[0]), self.folded(n.kids[1])
            v = x * y if k == "*" else x / y
            u = mul(static(n.kids[0])[0], static(n.kids[1])[0],
                    1 if k == "*" else -1)
            return v * (u[1] if not u[0] else 1)
        if k in ("in", "conv"):
            return self.folded(n.kids[0]) * static(n.kids[0])[0][1] / static(n)[0][1]
        if k == "add":
            u = static(n)[0]
            return sum(self.folded(x) * static(x)[0][1] / u[1] for x in n.kids)

    def ev(self, n, na, nb):
        """rep of n (before any conversion keyed on n as an operand)."""
        if is_const(n):
            u = static(n)[0]
            return self.folded(n) * (self.cc(u) if u[0] else 1)
        k = n.kind
        if k == "a":
            return na * c(subst(({"U": 1}, F(1)), self.env))
        if k == "b":
            return nb * c(subst(({"V": 1}, F(1)), self.env))
        if k in "*/":
            x, y = self.ev(n.kids[0], na, nb), self.ev(n.kids[1], na, nb)
            v = x * y if k == "*" else x / y
            u = static(n)[0]
            if not u[0]:
                v *= self.conv(n.col, n.text)  # the static factor (EXP-33)
            f, kind = self.site(n.col, n.text, "muldiv")
            return v * (f if kind == "muldiv" else 1)
        if k == "in":
            x = self.ev(n.kids[0], na, nb)
            return self.converted(x, *n.pkey)
        if k == "conv":
            x = n.kids[0]
            return self.converted(self.ev(x, na, nb), *x.okey)
        if k == "add":
            x, y = n.kids
            ux, uy = static(x)[0], static(y)[0]
            vx, vy = self.ev(x, na, nb), self.ev(y, na, nb)
            if ux[1] > uy[1]:
                vx = self.converted(vx, *x.okey)
            elif uy[1] > ux[1]:
                vy = self.converted(vy, *y.okey)
            return vx + vy

    def converted(self, v, col, text):
        f, kind = self.site(col, text, "conv")
        return v * self.conv(col, text) * (f if kind == "conv" else 1)


def quantity(rep, u_inst):
    return rep if not u_inst[0] else rep * u_inst[1]


INST = {"M": BASE["M"], "KM": BASE["KM"], "S": BASE["S"], "H": BASE["H"],
        "KM/H": mul(BASE["KM"], BASE["H"], -1), "1/M": mul(ONE, BASE["M"], -1),
        # empty at the instance: what a generic caller passing X/Y gives
        "KM/M": mul(BASE["KM"], BASE["M"], -1), "H/S": mul(BASE["H"], BASE["S"], -1),
        "M/KM": mul(BASE["M"], BASE["KM"], -1)}


def main():
    gopp, out, seeds = sys.argv[1], sys.argv[2], int(sys.argv[3])
    first_seed = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    import os
    os.makedirs(out, exist_ok=True)
    bad = rejected = checked = 0
    first = {}
    for seed in range(first_seed, first_seed + seeds):
        rng = random.Random(seed)
        e = rand(rng, rng.choice([2, 3, 4]))
        if os.environ.get("AVOID") and avoid(e):
            continue
        head = "func G[U unit, V unit](a float64[U], b float64[V]) float64 { return float64("
        body = render(e, len(head) + 1)
        src = HEAD + head + body + ") }\n\nfunc main() {}\n"
        line = HEAD.count("\n") + 1
        path = "%s/s%d.gopp" % (out, seed)
        open(path, "w").write(src)
        r = subprocess.run([gopp, "check", "--dump-types", "--check-error-limit=0", path],
                           capture_output=True, text=True, timeout=60)
        o = r.stdout + r.stderr
        if "error[" in o:
            rejected += 1
            continue
        sites, convs = {}, {}
        dict_params = set()
        for m in re.finditer(r"^(\d+):(\d+) unit-site `(.*?)` (\S.*)$", o, re.M):
            if int(m.group(1)) == line:
                sites.setdefault((int(m.group(2)), m.group(3)), []).append(
                    m.group(4))
        for m in re.finditer(r"^(\d+):(\d+) unit-conv `(.*?)` \*(\S+)", o, re.M):
            if int(m.group(1)) == line:
                convs[(int(m.group(2)), m.group(3))] = F(m.group(4))
        m = re.search(r"unit-dict G:(.*)$", o, re.M)
        if m:
            dict_params = set(m.group(1).split())
        su = static(e)[0]
        probs = []
        for un, uu in INST.items():
            for vn, vu in INST.items():
                env = {"U": uu, "V": vu}
                na, nb = F(3), F(7)
                try:
                    p = plain(e, env, na, nb)
                    g = Gen(sites, convs, env)
                    rep = g.ev(e, na, nb)
                except ZeroDivisionError:
                    continue
                checked += 1
                q = quantity(rep, subst(su, env))
                if q != p:
                    probs.append("U=%s V=%s plain %s generic %s" % (un, vn, p, q))
                elif not g.need <= dict_params:
                    probs.append("U=%s V=%s needs dict %s, has %s" % (un, vn, sorted(g.need), sorted(dict_params)))
        if probs:
            bad += 1
            if len(first) < 8:
                first[seed] = (body, probs[:2])
    for s, (b, p) in first.items():
        print("seed %d: %s\n   %s" % (s, b, "\n   ".join(p)))
    print("oracle: %d programs (%d rejected by gopp), %d instances, %d programs with mismatches"
          % (seeds, rejected, checked, bad))
    return 1 if bad else 0


sys.exit(main())
