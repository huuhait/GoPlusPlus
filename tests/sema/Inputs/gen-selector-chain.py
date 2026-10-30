#!/usr/bin/env python3
"""Generated selector chains for P03.26.

  chain N [K]       N selectors in one expression, used in K of the ways a
                    chain is used (write, call, address, closure, increment,
                    read, value-receiver call, address of the whole chain,
                    defer, go, if, composite literal, func-valued field;
                    default all)
  chain-errors N    three errors at the end of a chain of N selectors (a
                    missing field, a wrong value, a wrong type)
  callchain N       N method calls in a chain (`s.m().m()...`): the parser
                    still counts them
"""
import sys


def main():
    case, n = sys.argv[1], int(sys.argv[2])
    K = int(sys.argv[3]) if case == "chain" and len(sys.argv) > 3 else 99
    out = ["package main", ""]
    if case == "chain":
        c = "s" + ".a" * n
        out += ["type S struct{ a *S; v int; f func() int }", "",
                "func (s *S) M() int { return s.v }",
                "func (s S) V() int { return s.v }",
                "func use(x int) {}", "",
                "func f(s S) int {"]
        for stmt in ["%s.v = 1", "use(%s.M())", "p := &%s.v; _ = p",
                     "func() { use(%s.v) }()", "%s.v++", "use(%s.v)",
                     "use(%s.V())", "q := &%s; _ = q", "defer use(%s.v)",
                     "go use(%s.v)", "if %s.v > 0 { use(1) }",
                     "x := []int{%s.v}; _ = x", "use(%s.f())"][:K]:
            out.append("\t" + stmt % c)
        out += ["\treturn %s.v" % c, "}", "", "func main() {}"]
    elif case == "chain-errors":
        c = "s" + ".a" * n
        out += ["type S struct{ a *S; v int }", "", "func f(s S) {",
                "\t_ = %s.zzz" % c, "\t%s.v = \"x\"" % c,
                "\tvar i int = %s" % c, "\t_ = i", "}", "", "func main() {}"]
    elif case == "callchain":
        out += ["type S struct{}", "", "func (s S) m() S { return s }", "",
                "func f(s S) { _ = s" + ".m()" * n + " }", "",
                "func main() {}"]
    print("\n".join(out))


main()
