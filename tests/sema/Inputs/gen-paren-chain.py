#!/usr/bin/env python3
"""usage: gen-paren-chain.py <n> <ok|bad>: forms with n nested parentheses
(P03.26, R10). `ok`: the accepted uses (IsNil on a narrowed Option, `&` of a
part, in a chain of n parentheses). `bad`: the same shapes that must still be
errors: IsNil on a plain pointer (E0178), a part of a Dog used as a value
(E0395)."""
import sys

n, mode = int(sys.argv[1]), sys.argv[2]


def par(x):
    return "(" * n + x + ")" * n


print("package main")
print("type Node struct{ v int }")
print("type Animal struct{ N int; Tags []string }")
print("@Virtual\nfunc (a *Animal) Speak() int { return 0 }")
print("type Dog extends Animal { Extra [8]int }")
if mode == "ok":
    print("func a(o Option[*Node]) bool {\n\tif o != nil {\n\t\treturn %s.IsNil()\n\t}\n\treturn false\n}" % par("o"))
    print("func b(o Option[*Node]) bool {\n\tif o != nil {\n\t\treturn %s\n\t}\n\treturn false\n}" % par("move(o).IsNil()"))
    print("func c(dog *Dog, p *Animal) {\n\tq := &%s\n\t_ = q\n\tr := &%s\n\t_ = r\n}" % (par("dog.Animal"), par("*p")))
else:
    print("func a(o *Node) bool { return %s.IsNil() }" % par("o"))
    print("func c(dog *Dog, p *Animal) {\n\tq := %s\n\t_ = q\n\tr := %s\n\t_ = r\n}" % (par("dog.Animal"), par("*p")))
print("func main() {}")
