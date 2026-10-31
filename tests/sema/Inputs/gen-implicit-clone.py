#!/usr/bin/env python3
"""The plain-vs-generic table of the copies the language makes (P04.09 R5).

usage: gen-implicit-clone.py plain|generic|want-plain|want-generic

(want-*: every error the program must report and no other, as
`:LINE:COL: error[EXXXX] TEXT` sorted, which the test compares whole.)

Each site (a map insert by assignment, a map literal's key, the assignment
form of range, a loop variable a function literal captures, CLO-8) is written
once with a concrete type and once in a generic function instantiated with
that type, for a cloneable type (string, a struct of strings) and for one
that is not (a type with drop and no Clone, a struct that holds one). The
plain form of a non-cloneable type is E0452 at the site, the generic one
E0453 at the instantiation (the same rule, Checker::satisfiesClone, decides
both); the cloneable rows are accepted. (Two programs: the instances are
checked only when the checker found no error.)
"""
import sys

TYPES = [("Str", "string", True), ("Pair", "Pair", True),
         ("Res", "Res", False), ("Holder", "Holder", False)]
SITES = {
    "assign": ("func {name}{tp}(m map[{T}]int, k {T}) {{\n\tm[k] = 1\n}}",
               2, 4, "a map insert makes a copy of its key (OWN-V4)"),
    "literal": ("func {name}{tp}(move k {T}) int {{\n\tm := map[{T}]int{{k: 1}}\n"
                "\treturn len(m)\n}}", 2, None,
                "a map insert makes a copy of its key (OWN-V4)"),
    "range": ("func {name}{tp}(xs []{T}, move v {T}) {{\n\tfor _, v = range xs {{\n"
              "\t}}\n\t_ = v\n}}", 2, 9,
              "the assignment form of range makes a copy of each element"),
    "loop": ("func {name}{tp}(move v {T}) int {{\n\tvar fs []func() int\n"
             "\ti := 0\n\tfor x := move(v); i < 2; i++ {{\n"
             "\t\tfs = append(fs, func() int {{ _ = x; return i }})\n\t}}\n"
             "\treturn len(fs)\n}}", 4, 6,
             "this loop (CLO-8: each iteration has its own copy of a variable "
             "a function literal captures) makes a copy of x"),
}
HEAD = """package main

type Pair struct{ a, b string }

type Res struct{ n int }

func (r *Res) drop() { println("drop") }

type Holder struct{ r Res }
"""


def main():
    mode = sys.argv[1]
    plain = mode in ("plain", "want-plain")
    lines = HEAD.split("\n")
    want = []
    calls = []
    for site, (tmpl, dl, col, what) in SITES.items():
        if not plain:
            bound = "comparable" if site in ("assign", "literal") else "any"
            lines += [""] + tmpl.format(name="gen_" + site,
                                        tp="[T " + bound + "]",
                                        T="T").split("\n")
        for tag, T, ok in TYPES:
            if plain:
                start = len(lines) + 2  # the line after the blank one
                lines += [""] + tmpl.format(name="plain_%s_%s" % (site, tag),
                                            tp="", T=T).split("\n")
                # (`m := map[T]int{k: 1}`: k after the tab and the type.)
                c = col if col is not None else 16 + len(T)
                if not ok:
                    want.append(":%d:%d: error[E0452]: %s" %
                                (start + dl - 1, c, what))
            else:
                calls.append((site, T, ok, what))
    if not plain:
        lines += ["", "func main() {"]
        for site, T, ok, what in calls:
            lines.append("\t_ = gen_%s[%s]" % (site, T))
            if not ok:
                want.append(":%d:6: error[E0453]: cannot instantiate "
                            "gen_%s[%s]: %s" % (len(lines), site, T, what))
        lines += ["}", ""]
    else:
        lines += ["", "func main() {}", ""]
    if mode in ("plain", "generic"):
        sys.stdout.write("\n".join(lines))
    else:
        sys.stdout.write("\n".join(sorted(want)) + "\n")


if __name__ == "__main__":
    main()
