#!/usr/bin/env python3
"""P03.25 (GEN-14, R10, owner decision 2026-10-30): generic unit code
computes what plain code computes. Random programs of unit-generic functions
(products and quotients of their values and of unit literals, calls of other
generics with unit arguments made of their parameters, concrete instances in
main) are checked by gopp (no error, the same `--dump-types` with 1 and 8
body threads) and then evaluated twice with exact rationals for every call
in main:
  plain:   as plain code with the real units (EXP-33: a product or quotient
           whose dimension is empty is the plain number times its factor);
  generic: as the generic code runs once for all instances, numbers in their
           units, with the factor of each collapse site that gopp recorded
           (`unit-site`, the rule of Info::UnitSite) computed from the real
           units, and a constant of a unit E being its number times c(E).
The two values must be equal. Prints one summary line.

Usage: gen-units-oracle.py --gopp <gopp> --out <dir> [--seeds N] [--deep] [--j1]
"""
import argparse
import os
import random
import re
import subprocess
import sys
from fractions import Fraction

UNITS = {"M": ({"M": 1}, Fraction(1)), "KM": ({"M": 1}, Fraction(1000)),
         "S": ({"S": 1}, Fraction(1)), "H": ({"S": 1}, Fraction(3600))}
HEAD = "package main\n\nunit M\nunit KM = 1000 M\nunit S\nunit H = 3600 S\n\n"
PARAMS = ["U", "V"]


def mul(a, b, sign=1):
    d = dict(a[0])
    for k, e in b[0].items():
        d[k] = d.get(k, 0) + sign * e
        if d[k] == 0:
            del d[k]
    return (d, a[1] * (b[1] ** sign))


ONE = ({}, Fraction(1))
# The unit parameters as units of their own (the generic code's view).
SYMBOLIC = {"U": ({"$U": 1}, Fraction(1)), "V": ({"$V": 1}, Fraction(1))}


def unit_of(terms, env):
    u = ONE
    for name, e in terms:
        base = env[name] if name in env else UNITS[name]
        for _ in range(abs(e)):
            u = mul(u, base, 1 if e > 0 else -1)
    return u


def c(u):
    """The factor a value of unit u carries at run time: u's factor where
    its dimension is empty (the plain number, EXP-33), else 1."""
    return u[1] if not u[0] else Fraction(1)


def text_arg(terms):
    num = [n for n, e in terms for _ in range(e) if e > 0]
    den = [n for n, e in terms for _ in range(-e) if e < 0]
    s = "*".join(num) if num else "1"
    if den:
        s += "/" + ("(" + "*".join(den) + ")" if len(den) > 1 else den[0])
    return s


def rand_arg(rng):
    """A unit argument over U and V (a parameter with exponent 1 or -1, and
    maybe a base unit), never empty as written."""
    while True:
        t = [(p, rng.choice([0, 1, -1])) for p in PARAMS]
        t = [(p, e) for p, e in t if e]
        if rng.random() < 0.3:
            t.append((rng.choice(list(UNITS)), rng.choice([1, -1])))
        if any(p in PARAMS for p, _ in t):
            return t


def rand_expr(rng, depth):
    """A tree: ("a",) ("b",) ("lit", value, unit) (op, l, r)."""
    if depth == 0 or rng.random() < 0.3:
        r = rng.random()
        if r < 0.4:
            return ("a",)
        if r < 0.8:
            return ("b",)
        return ("lit", rng.choice([2, 3, 5]), rng.choice(list(UNITS)))
    return (rng.choice("*/"), rand_expr(rng, depth - 1), rand_expr(rng, depth - 1))


def text_expr(e):
    if e[0] in ("a", "b"):
        return e[0]
    if e[0] == "lit":
        return "%d.0[%s]" % (e[1], e[2])
    return "(%s %s %s)" % (text_expr(e[1]), e[0], text_expr(e[2]))


def columns(e, start, out):
    """The column (1-based) at which each binary node of text_expr(e) starts
    when the text starts at column start: the key of its unit-site record."""
    if e[0] in ("*", "/"):
        out[id(e)] = start + 1  # after the opening parenthesis
        left = text_expr(e[1])
        columns(e[1], start + 1, out)
        columns(e[2], start + 1 + len(left) + 3, out)
    return out


def gen(rng, ngen, fan):
    funcs = []  # (expr, calls[(j, [arg terms, arg terms])])
    for i in range(ngen):
        e = rand_expr(rng, rng.choice([1, 2, 3]))
        calls = []
        if i > 0:
            for _ in range(rng.choice(fan)):
                calls.append((rng.randrange(max(0, i - 3), i),
                              [rand_arg(rng), rand_arg(rng)]))
        funcs.append((e, calls))
    tops = []
    concrete = ["M", "KM", "S", "H", "KM/H", "1/M", "M/S", "KM*S"]
    for _ in range(4):
        tops.append((rng.randrange(ngen), rng.choice(concrete), rng.choice(concrete)))
    return funcs, tops


def concrete_unit(text):
    num, _, den = text.partition("/")
    u = ONE
    for t in num.split("*"):
        if t != "1":
            u = mul(u, UNITS[t])
    if den:
        u = mul(u, UNITS[den], -1)
    return u


def program(funcs, tops):
    lines = HEAD.split("\n")[:-1]
    at = {}
    for i, (e, calls) in enumerate(funcs):
        head = ("func G%d[U unit, V unit](a float64[U], b float64[V]) "
                "float64 { " % i)
        for j, args in calls:
            head += "_ = G%d[%s](float64[%s](1.5), float64[%s](2.5)); " % (
                j, ", ".join(text_arg(x) for x in args),
                text_arg(args[0]), text_arg(args[1]))
        head += "return float64("
        lines.append(head + text_expr(e) + ") }")
        at[i] = (len(lines), columns(e, len(head) + 1, {}))
    lines.append("")
    lines.append("func main() {")
    for j, x, y in tops:
        lines.append("\t_ = G%d(3.0[%s], 7.0[%s])" % (j, x, y))
    lines.append("}")
    return "\n".join(lines) + "\n", at


def parse_unit(t, env):
    dims, f = {}, Fraction(1)
    if t == "plain":
        return dims, f
    m = re.match(r"^\((\d+)/(\d+)\) (.*)$", t) or re.match(r"^(\d+)() (.*)$", t)
    if m:
        f = Fraction(int(m.group(1)), int(m.group(2) or 1))
        t = m.group(3)
    num, _, den = t.partition("/")
    u = ({}, f)
    for part, sign in ((num, 1), (den.strip("()"), -1)):
        for term in part.split("*"):
            if not term or term == "1":
                continue
            name, _, e = term.partition("^")
            base = env[name] if name in env else ({name: 1}, Fraction(1))
            for _ in range(int(e or 1)):
                u = mul(u, base, sign)
    return u


def site_factor(rec, env):
    if " from " in rec:
        res, x = rec.split(" from ")
        return c(parse_unit(res, env)) / c(parse_unit(x, env))
    res, rest = rec.split(" = ")
    op = " * " if " * " in rest else " / "
    x, y = rest.split(op)
    r, cx, cy = (c(parse_unit(t, env)) for t in (res, x, y))
    return r / (cx * cy) if op == " * " else r * cy / cx


def static_unit(e, env):
    """The unit of e as the generic code types it, at the instance env (no
    collapse: the unit math of EXP-33 on the written units)."""
    if e[0] in ("a", "b"):
        return env["U" if e[0] == "a" else "V"]
    if e[0] == "lit":
        return UNITS[e[2]]
    x, y = static_unit(e[1], env), static_unit(e[2], env)
    r = mul(x, y) if e[0] == "*" else mul(x, y, -1)
    if not combine(e, SYMBOLIC)[0]:
        return ONE  # plain whatever the instance (EXP-33 where it is checked)
    return r


def combine(e, env):
    """The unit of the binary node e from the units of its operands."""
    x, y = static_unit(e[1], env), static_unit(e[2], env)
    return mul(x, y) if e[0] == "*" else mul(x, y, -1)


def plain_quantity(e, ua, na, ub, nb):
    """e as plain code computes it with the real units (EXP-33: a product or
    quotient whose dimension is empty is the plain number times its factor):
    the quantity in base units and its dimension."""
    def ev(e):
        if e[0] == "a":
            return na, ua
        if e[0] == "b":
            return nb, ub
        if e[0] == "lit":
            return Fraction(e[1]), UNITS[e[2]]
        (x, u), (y, w) = ev(e[1]), ev(e[2])
        n, unit = (x * y, mul(u, w)) if e[0] == "*" else (x / y, mul(u, w, -1))
        if not unit[0]:
            n, unit = n * unit[1], ONE
        return n, unit
    n, unit = ev(e)
    return n * unit[1], tuple(sorted(unit[0].items()))


def generic_quantity(e, recs, cols, env, ra, rb):
    """e as the generic code computes it once for all instances: each value
    as it is held (the number in its unit, or the plain number where the unit
    is empty at the instance), the recorded collapse sites applied with the
    real units; then the quantity in base units."""
    def ev(e):
        if e[0] == "a":
            return ra
        if e[0] == "b":
            return rb
        if e[0] == "lit":
            return Fraction(e[1])
        x, y = ev(e[1]), ev(e[2])
        v = x * y if e[0] == "*" else x / y
        # a unit that is empty whatever the instance: plain, its factor
        # applied where the code is checked (EXP-33)
        s = combine(e, SYMBOLIC)
        if not s[0]:
            v *= s[1]
        rec = recs.get(cols[id(e)])
        return v * (site_factor(rec, env) if rec else 1)
    v, u = ev(e), static_unit(e, env)
    return (v * u[1] if u[0] else v), tuple(sorted(u[0].items()))


def compare(funcs, sites, at, i, ua, ub, na, nb, seen, out):
    """G_i at the instance (ua, ub) with values na, nb (numbers in ua, ub),
    and every instance it calls: plain and generic quantities must agree."""
    key = (i, tuple(sorted(ua[0].items())), ua[1], tuple(sorted(ub[0].items())),
           ub[1], na, nb)
    if key in seen:
        return
    seen.add(key)
    e, calls = funcs[i]
    line, cols = at[i]
    env = {"U": ua, "V": ub}
    p = plain_quantity(e, ua, na, ub, nb)
    # a value whose unit is empty at the instance is held as the plain number
    g = generic_quantity(e, sites.get(line, {}), cols, env,
                         na * c(ua), nb * c(ub))
    out.append((i, p, g))
    for j, args in calls:
        u1, u2 = unit_of(args[0], env), unit_of(args[1], env)
        compare(funcs, sites, at, j, u1, u2, Fraction(3, 2), Fraction(5, 2),
                seen, out)


def run(gopp, path, j):
    r = subprocess.run([gopp, "check", "-check-j=" + j, "--check-error-limit=0",
                        "--dump-types", path], capture_output=True, text=True,
                       timeout=60)
    return r.stderr + r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seeds", type=int, default=60)
    ap.add_argument("--deep", action="store_true",
                    help="longer chains with more calls")
    ap.add_argument("--j1", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    fails = checked = 0
    for seed in range(a.seeds):
        rng = random.Random(seed)
        if a.deep:
            funcs, tops = gen(rng, rng.choice([10, 12, 14]), [2, 3])
        else:
            funcs, tops = gen(rng, rng.choice([4, 6, 8]), [0, 1, 2])
        text, at = program(funcs, tops)
        path = os.path.join(a.out, "p%d.gopp" % seed)
        open(path, "w").write(text)
        o1 = run(a.gopp, path, "1")
        o8 = o1 if a.j1 else run(a.gopp, path, "8")
        sites = {}
        for m in re.finditer(r"^(\d+):(\d+) unit-site `.*?` (\S.* = .*)$", o1,
                             re.M):  # the products and quotients
            sites.setdefault(int(m.group(1)), {})[int(m.group(2))] = m.group(3)
        problems = []
        if o1 != o8:
            problems.append("j1 != j8")
        if "error[" in o1:
            problems.append("errors")
        seen, out = set(), []
        for j, x, y in tops:
            compare(funcs, sites, at, j, concrete_unit(x), concrete_unit(y),
                    Fraction(3), Fraction(7), seen, out)
        for i, p, g in out:
            checked += 1
            if p != g:
                problems.append("G%d: plain %s generic %s" % (i, p, g))
        if problems:
            fails += 1
            print("seed %d: FAIL %s" % (seed, "; ".join(problems)))
            print(o1[:1500] if "errors" in problems else "")
    print("oracle: %d programs, %d instances, %d mismatches" %
          (a.seeds, checked, fails))
    return 1 if fails else 0


sys.exit(main())
