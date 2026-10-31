#!/usr/bin/env python3
"""Rejects a raw read of a checker type in the body lowering (P04.08).

usage: check-raw-types.py FILE...

The body of an instance of generic code is lowered by the plain `Body` with
the type arguments applied (gir.md section 10): every checker type a `Body`
reads must go through its instance accessors (sTyOf, tyOf, tyOfObj, sigOf,
selOf, narrowed, modeOf), which substitute in generic code and are the
identity in plain code. A raw read (`->Ty`, `->sig()`, `I.typeOf(`,
`I.narrowedType(`, `I.Selections`, a variable's `->Mode`) inside a `Body::`
member function of a .cpp file, or inside `class Body` of a header, is a
type parameter left for an instance (review R1: a type switch binding read
`V->Ty`; decision M6: a method value made in generic code read its selection
from `I.Selections`; review R5: a mode the checker decided on a type
parameter). The accessors themselves say `// raw: accessor`.
Prints each such line and exits 1 if there is one.
"""
import re
import sys

RAW = re.compile(r"->Ty\b|->sig\(\)|\bI\.typeOf\(|\bI\.narrowedType\(|"
                 r"\bI\.Selections\b|->Mode\b")
START_FN = re.compile(r"^\S.*\bBody::[A-Za-z_]\w*\(")
START_CLASS = re.compile(r"^class Body\b")


def code(line):
    """The line without a // comment (none of the patterns is in a string)."""
    i = line.find("//")
    return line if i < 0 else line[:i]


bad = 0
for path in sys.argv[1:]:
    inside = False
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        if not inside:
            inside = bool(START_FN.match(line) or START_CLASS.match(line))
        if inside and RAW.search(code(line)) and "raw: accessor" not in line:
            print("%s:%d: a raw checker type in Body code: %s" %
                  (path, n, line.strip()))
            bad += 1
        # A member function, or the class, ends at a closing brace in
        # column 0.
        if inside and (line.startswith("}") or line.startswith("};")):
            inside = False
print("ok" if not bad else "%d raw reads" % bad)
sys.exit(1 if bad else 0)
