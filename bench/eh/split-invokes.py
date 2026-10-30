#!/usr/bin/env python3
"""Move every `invoke` that is not alone in its block into a new block of its
own (P04.15, ADR-0028).

This emulates a tiny IR pass run just before instruction selection, after the
`opt 0` passes (simplifycfg would merge the blocks back if it ran later).
x86-64 FastISel cannot select an invoke and gives the whole block that ends in
it to SelectionDAG; with the invoke alone, SelectionDAG sees one instruction.
The invoke's successors get their PHI entries renamed to the new block.

  python3 bench/eh/split-invokes.py < in.ll > out.ll
"""
import re
import sys

LABEL = re.compile(r"^([\w.$-]+):")
INVOKE = re.compile(r"^  (?:%[\w.$-]+ = )?invoke ")
DESTS = re.compile(r"to label %([\w.$-]+) unwind label %([\w.$-]+)")


def split_function(lines):
    out, renames = [], {}  # renames[successor] = [(old block, new block)]
    block, nonphi, n = None, 0, 0
    i = 0
    while i < len(lines):
        line = lines[i]
        m = LABEL.match(line)
        if m:
            block, nonphi = m.group(1), 0
        elif line.startswith("define "):
            block, nonphi = "entry", 0
        elif INVOKE.match(line):
            # the destinations may be on the next line
            text = line + " " + (lines[i + 1] if i + 1 < len(lines) else "")
            d = DESTS.search(text)
            if nonphi and d:
                n += 1
                new = "invoke.alone%d" % n
                out += ["  br label %%%s" % new, "%s:" % new]
                for succ in d.groups():
                    renames.setdefault(succ, []).append((block, new))
                block = new
            nonphi += 1
        elif line.startswith("  ") and not line.lstrip().startswith(";") and " = phi " not in line \
                and line.strip():
            nonphi += 1
        out.append(line)
        i += 1
    # rename the PHI entries of the successors
    block = None
    for k, line in enumerate(out):
        m = LABEL.match(line)
        if m:
            block = m.group(1)
        elif block in renames and " = phi " in line:
            for old, new in renames[block]:
                line = re.sub(r"(\[ [^\[\]]+, )%%%s( \])" % re.escape(old), r"\g<1>%%%s\g<2>" % new, line)
            out[k] = line
    return out


def main():
    lines = sys.stdin.read().split("\n")
    out, fn = [], None
    for line in lines:
        if line.startswith("define "):
            fn = [line]
        elif fn is not None:
            fn.append(line)
            if line == "}":
                out += split_function(fn)
                fn = None
        else:
            out.append(line)
    sys.stdout.write("\n".join(out))


if __name__ == "__main__":
    main()
