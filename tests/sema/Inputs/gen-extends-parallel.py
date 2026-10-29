#!/usr/bin/env python3
"""An error-heavy program for extends-parallel.test: n bodies, each with a
local type that extends a tagged chain (a field clash and an open @Abstract
method), a partial-place copy, an upcast to an untagged parent and a clone
through a pointer, so the hierarchy answers are asked from many threads."""
import sys

n = int(sys.argv[1])
o = ["package main", "",
     "type A struct{ x int }", "@Abstract", "func (a *A) M() int",
     "type B extends A {}",
     "type U struct{ n int }", "type V extends U {}"]
for i in range(n):
    o += ["func f%d(p *A, v *V) {" % i,
          "\ttype L%d extends B { x int }" % i,
          "\tc := *p",
          "\t_ = c",
          "\tvar u *U = v",
          "\t_ = u",
          "\t_ = p.Clone()",
          "}"]
print("\n".join(o))
