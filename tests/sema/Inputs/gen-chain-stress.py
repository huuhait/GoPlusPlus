#!/usr/bin/env python3
"""Generates the long-chain cases of P03.26: <case> <depth> [count], or several
cases in one program: <case>:<depth>,<case>:<depth>,...

Each case is a Go program (also valid Go++) with `count` functions, each one
holding a chain of `depth` links of one form or a mix of forms:

  sel      a.s.s.s...v                (field selectors)
  call     f()()()...                 (calls of a function returning itself)
  idx      len(a[0][0]...)            (index expressions)
  slice    len(a[:][:]...)            (slice expressions)
  assert   a.(any).(any)...           (type assertions)
  star     *p...                      (pointer indirections)
  paren    ((((a))))                  (parentheses)
  mix      t.s.a[0].me().f().s...     (selectors, indexes and calls)
  mixparen (*(*t).s).s...    (selectors, stars and parentheses)
  selpar, selcall, selidx, selassert, selstar
           a selector chain over a base that is no name: (a).s.s...v,
           sg(a).s..., a[0].s..., a.(*S).s..., (*a).s...
  selparerr  selpar with a wrong name at the bottom
  err      sel with a wrong name at the bottom: one error per function
  addr     &a.s.s...v                 (address of a field at the end)
  assign   a.s.s...v = 1              (assignment to a field at the end)
  method   a.s.s...Me().Me()...       (method calls)
  defer    defer a.s.s...Me()         (a deferred method call)
  badbase  a long chain on an undefined name: one error per function
  nestcall g(g(g(...)))               (calls nested in arguments)
  nestlit  T{T{T{...}}}               (composite literals nested)
  nestif   if a > 0 { if a > 0 { ... (nested statements)
  nestfunc func() { func() { ... (nested function literals)
  qual     q.V.L[0].L[0]...V          (chains that start at a package name;
           needs the stub package Inputs/chainstub/q)
  bin      a + a + a ...              (a long binary chain)
  unary    !!!!a and - - -a
  elif     a long `else if` chain
"""
import sys

# One case: <case> <depth> [count]. Several in one program (one function
# each, named after its case): <case>:<depth>,<case>:<depth>,...
if ":" in sys.argv[1]:
    cases = [(c, int(d), c) for c, d in (x.split(":") for x in sys.argv[1].split(","))]
    count = 1
else:
    cases = [(sys.argv[1], int(sys.argv[2]), "f")]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 1
out = ["package main", ""]
if any(c == "qual" for c, _, _ in cases):
    out += ['import "q"', ""]
out += [
    "type S struct {",
    "\ts *S",
    "\tv int",
    "}",
    "type F func() F",
    "type A []A",
    "type P *P",
    "type T struct {",
    "\ts *T",
    "\ta []*T",
    "\tf func() *T",
    "\tm map[int]*T",
    "}",
    "",
    "func (t *T) me() *T { return t }",
    "func (p *S) Me() *S { return p }",
    "type L []any",
    "func g(x any) any { return x }",
    "func sg(p *S) *S { return p }",
    "",
]
for case, depth, name in cases:
    for k in range(count):
        n = "%s%d" % (name, k)
        if case == "sel":
            out.append("func %s(a *S) int { return a%s.v }" % (n, ".s" * depth))
        elif case in ("selpar", "selparerr"):
            out.append("func %s(a *S) int { return (a)%s.%s }" % (n, ".s" * depth, "w" if case == "selparerr" else "v"))
        elif case == "selcall":
            out.append("func %s(a *S) int { return sg(a)%s.v }" % (n, ".s" * depth))
        elif case == "selidx":
            out.append("func %s(a []*S) int { return a[0]%s.v }" % (n, ".s" * depth))
        elif case == "selassert":
            out.append("func %s(a any) int { return a.(*S)%s.v }" % (n, ".s" * depth))
        elif case == "selstar":
            out.append("func %s(a **S) int { return (*a)%s.v }" % (n, ".s" * depth))
        elif case == "err":
            out.append("func %s(a *S) int { return a%s.w }" % (n, ".s" * depth))
        elif case == "addr":
            out.append("func %s(a *S) *int { return &a%s.v }" % (n, ".s" * depth))
        elif case == "assign":
            out.append("func %s(a *S) { a%s.v = 1 }" % (n, ".s" * depth))
        elif case == "method":
            out.append("func %s(a *S) *S { return a%s }" % (n, ".s.Me()" * (depth // 2)))
        elif case == "defer":
            out.append("func %s(a *S) { defer a%s.Me() }" % (n, ".s" * depth))
        elif case == "badbase":
            out.append("func %s() int { return nope%s.v }" % (n, ".s" * depth))
        elif case == "qual":
            out.append("func %s() int { return q.V%s.V }" % (n, ".L[0]" * (depth // 2)))
            out.append("func %sc() int { return q.New()%s.V }" % (n, ".L[0]" * (depth // 2)))
        elif case == "nestcall":
            out.append("func %s() any { return %s1%s }" % (n, "g(" * depth, ")" * depth))
        elif case == "nestlit":
            out.append("func %s() any { return %sL{}%s }" % (n, "L{" * depth, "}" * depth))
        elif case == "nestif":
            out.append("func %s(a int) { %s%s }" % (n, "if a > 0 { " * depth, "}" * depth))
        elif case == "nestfunc":
            out.append("func %s() { %s%s }" % (n, "func() { " * depth, "}() " * depth))
        elif case == "call":
            out.append("func %s(a F) F { return a%s }" % (n, "()" * depth))
        elif case == "idx":
            out.append("func %s(a A) int { return len(a%s) }" % (n, "[0]" * depth))
        elif case == "slice":
            out.append("func %s(a []int) int { return len(a%s) }" % (n, "[:]" * depth))
        elif case == "assert":
            out.append("func %s(a any) any { return a%s }" % (n, ".(any)" * depth))
        elif case == "star":
            out.append("func %s(a P) P { return %sa }" % (n, "*" * depth))
        elif case == "paren":
            out.append("func %s(a int) int { return %sa%s }" % (n, "(" * depth, ")" * depth))
        elif case == "mix":
            out.append("func %s(t *T) *T { return t%s }" % (n, ".s.a[0].me().f().s" * (depth // 8)))
        elif case == "mixparen":
            e = "t"
            for _ in range(depth // 4):
                e = "(*%s).s" % e
            out.append("func %s(t *T) *T { return %s }" % (n, e))
        elif case == "bin":
            out.append("func %s(a int) int { return a%s }" % (n, " + a" * depth))
        elif case == "unary":
            out.append("func %s(a bool) bool { return %sa }" % (n, "!" * depth))
        elif case == "elif":
            out.append("func %s(a int) int {" % n)
            out.append("\tif a == 0 {\n\t\treturn 0\n\t}" + "".join(" else if a == %d {\n\t\treturn %d\n\t}" % (i + 1, i) for i in range(depth)))
            out.append("\treturn 1\n}")
        else:
            sys.exit("unknown case " + case)
print("\n".join(out))
