#!/usr/bin/env python3
"""The plain-vs-generic table of what a closure may hold (P04.08, review R5).

usage: gen-borrow-table.py --gopp GOPP --out DIR [--go-source FILE]

For each form that declares a variable a closure captures (a parameter, a
value receiver, a range value or key, a map value, a type switch binding, an
assertion, a comma-ok assertion and map read, an `&` range alias, an
Option's Get) and each type argument (int, a struct, an array, string), it
writes the plain code (the type written in) and the generic code (a type
parameter instantiated with it), each closure escaping (it is returned), and
asks the compiler for both:

  1. `gopp check` of every row (one file per side): the checker's errors;
  2. `gopp build -emit=gir` of the rows the checker accepts: the lowering's
     refusals (E0439: CLO-3a, CLO-5a);
  3. the rows both sides accept, built and run (one program per side): each
     prints its row and `true`.

It prints the table, each cell plain/generic: `.` accepted (and run), `3`
refused by CLO-3a and `5` by CLO-5a (the lowering's E0439), `c` refused by
the checker, `K` a known difference (the checker's false E0350 of TODO
P03.31); then whether the two sides differ anywhere else, and the run lines
of the generic program, which must equal the plain program's (checked here)
and the golden (checked by the test).
With --go-source it also writes the plain program of the Go-valid accepted
rows as Go source, for regenerating the golden with `go run`.
"""
import argparse
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

TYPES = [("int", "1"), ("P", "P{1}"), ("A", "A{1, 2}"), ("string", '"s"')]
CMP = "comparable"
COPY = "interface{ comparable; Copy }"

# name: (constraint of the generic form, Go-valid, body). The body declares
# the captured variable v from `y X` (and `xs []X`) and appends one closure
# that captures it; X is the type (plain) or the type parameter (generic).
FORMS = [
    ("param", CMP, True,
     "fs = append(fs, func() bool { return y == y })"),
    ("rangeval", CMP, True,
     "for _, v := range xs {\n\t\tfs = append(fs, func() bool { return v == y })\n\t}"),
    ("rangekey", COPY, True,
     "m := map[X]int{y: 1}\n\tfor k := range m {\n\t\tfs = append(fs, func() bool { return k == y })\n\t}"),
    ("mapval", COPY, True,
     "m := map[int]X{1: y}\n\tfor _, v := range m {\n\t\tfs = append(fs, func() bool { return v == y })\n\t}"),
    ("tswitch", COPY, True,
     "var a any = y\n\tswitch v := a.(type) {\n\tcase X:\n\t\tfs = append(fs, func() bool { return v == y })\n\t}"),
    ("assert", COPY, True,
     "var a any = y\n\tv := a.(X)\n\tfs = append(fs, func() bool { return v == y })"),
    ("assertok", COPY, True,
     "var a any = y\n\tif v, ok := a.(X); ok {\n\t\tfs = append(fs, func() bool { return v == y })\n\t}"),
    ("mapok", COPY, True,
     "m := map[int]X{1: y}\n\tif v, ok := m[1]; ok {\n\t\tfs = append(fs, func() bool { return v == y })\n\t}"),
    ("alias", COPY, False,
     "for _, &v := range xs {\n\t\tfs = append(fs, func() bool { return v == y })\n\t}"),
    ("optget", COPY, False,
     "var o Option[X] = y\n\tif v, ok := o.Get(); ok {\n\t\tfs = append(fs, func() bool { return v == y })\n\t}"),
]
# The value receiver form is a method of a type of its own.
RECV = ("recv", CMP, True)
# Known differences (the checker's, TODO P03.31): a comma-ok binding of a
# Copy type parameter (`v, ok := a.(X)`, `m[k]`, `o.Get()`) that a literal
# captures is a false E0350 in the generic code.
KNOWN = {(f, t) for f in ("assertok", "mapok", "optget") for t, _ in TYPES}

HEADER = "package main\n\ntype P struct{ n int }\n\ntype A [2]int\n\n"


def row_plain(form, body, tn):
    b = body.replace("X", tn)
    return (f"func r_{form}_{tn}(xs []{tn}, y {tn}) []func() bool {{\n"
            f"\tvar fs []func() bool\n\t{b}\n\treturn fs\n}}\n\n")


def gen_generic(form, cons, body):
    return (f"func g_{form}[X {cons}](xs []X, y X) []func() bool {{\n"
            f"\tvar fs []func() bool\n\t{body}\n\treturn fs\n}}\n\n")


def recv_plain(tn):
    return (f"type R_{tn} struct{{ v {tn} }}\n\n"
            f"func (b R_{tn}) run(y {tn}) []func() bool {{\n"
            f"\treturn []func() bool{{func() bool {{ return b.v == y }}}}\n}}\n\n")


RECV_GENERIC = ("type R[X comparable] struct{ v X }\n\n"
                "func (b R[X]) run(y X) []func() bool {\n"
                "\treturn []func() bool{func() bool { return b.v == y }}\n}\n\n")


def call(form, tn, lit, generic):
    if form == "recv":
        t = f"R[{tn}]" if generic else f"R_{tn}"
        return f"{t}{{{lit}}}.run({lit})"
    f = f"g_{form}[{tn}]" if generic else f"r_{form}_{tn}"
    return f"{f}([]{tn}{{{lit}}}, {lit})"


def program(rows, generic, run):
    """The source of the rows (form, tn, lit): each in a function of its own
    (callers too), and a main that runs them when run is set."""
    src = HEADER
    forms = {f: (c, b) for f, c, _, b in FORMS}
    done = set()
    for form, tn, lit in rows:
        if form == "recv":
            if generic and "recv" not in done:
                src += RECV_GENERIC
                done.add("recv")
            if not generic:
                src += recv_plain(tn)
            continue
        cons, body = forms[form]
        if generic:
            if form not in done:
                src += gen_generic(form, cons, body)
                done.add(form)
        else:
            src += row_plain(form, body, tn)
    for form, tn, lit in rows:
        src += (f"func c_{form}_{tn}() bool {{\n\tfs := "
                f"{call(form, tn, lit, generic)}\n"
                f"\treturn len(fs) > 0 && fs[0]()\n}}\n\n")
    src += "func main() {\n"
    if run:
        for form, tn, _ in rows:
            src += f'\tprintln("{form} {tn}", c_{form}_{tn}())\n'
    src += "}\n"
    return src


def errors_by_row(path, text, rows, generic, out):
    """Maps each error line of `out` to the rows it belongs to."""
    lines = text.split("\n")
    owner = {}  # line number -> row key, or "form" for a generic body
    cur = None
    for n, l in enumerate(lines, 1):
        m = re.match(r"func (?:\([^)]*\) )?(?:r_|c_)(\w+?)_(\w+)\(", l)
        if m:
            cur = (m.group(1), m.group(2))
        m = re.match(r"func \(b R_(\w+)\) run", l)
        if m:
            cur = ("recv", m.group(1))
        m = re.match(r"func g_(\w+)\[", l)
        if m:
            cur = ("form", m.group(1))
        if l.startswith("func (b R[X])"):
            cur = ("form", "recv")
        if l.startswith("type ") or l.startswith("func main"):
            cur = None
        owner[n] = cur
    bad = {}
    for l in out.splitlines():
        m = re.match(re.escape(path) + r":(\d+):\d+: error\[(E\d+)\]: (.*)", l)
        if not m:
            continue
        o = owner.get(int(m.group(1)))
        code = m.group(2)
        what = "CLO3a" if "CLO-3a" in m.group(3) else (
            "CLO5a" if "CLO-5a" in m.group(3) else code)
        if o is None:
            continue
        if o[0] == "form":
            inst = re.search(r"in the instance \w+\[(\w+)\]", m.group(3))
            for form, tn, _ in rows:
                if form == o[1] and (not inst or inst.group(1) == tn):
                    bad.setdefault((form, tn), what)
        else:
            bad.setdefault(o, what)
    return bad


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)


def decide(gopp, out_dir, rows, generic):
    side = "generic" if generic else "plain"
    path = os.path.join(out_dir, f"check-{side}.gopp")
    text = program(rows, generic, run=False)
    open(path, "w").write(text)
    r = run([gopp, "check", "--check-error-limit=0", path])
    bad = errors_by_row(path, text, rows, generic, r.stdout + r.stderr)
    left = [x for x in rows if (x[0], x[1]) not in bad]
    # The lowering's refusals: all rows in one build, or (past the build's
    # limit of ten errors, E0001) each row in a build of its own.
    def lower(part, tag):
        path = os.path.join(out_dir, f"lower-{side}{tag}.gopp")
        text = program(part, generic, run=False)
        open(path, "w").write(text)
        r = run([gopp, "build", "-emit=gir", path, "-o", os.devnull])
        out = r.stdout + r.stderr
        return out, errors_by_row(path, text, part, generic, out)
    out, found = lower(left, "")
    if "error[E0001]" in out:
        found = {}
        for k, x in enumerate(left):
            found.update(lower([x], f"-{k}")[1])
    bad.update(found)
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gopp", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--go-source")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    names = [f for f, _, _, _ in FORMS] + ["recv"]
    rows = [(f, tn, lit) for f in names for tn, lit in TYPES]
    with ThreadPoolExecutor(2) as ex:
        plain_f = ex.submit(decide, a.gopp, a.out, rows, False)
        gen_f = ex.submit(decide, a.gopp, a.out, rows, True)
        plain, gen = plain_f.result(), gen_f.result()
    ok = [x for x in rows if (x[0], x[1]) not in plain and
          (x[0], x[1]) not in gen]
    print("types: " + " ".join(tn for tn, _ in TYPES))
    differ = 0
    # A refusal by its kind: 3 CLO-3a, 5 CLO-5a (the lowering's E0439),
    # c the checker's error.
    def mark(bad, key):
        w = bad.get(key)
        return "." if w is None else {"CLO3a": "3", "CLO5a": "5"}.get(w, "c")
    for f in names:
        cells = []
        for tn, _ in TYPES:
            p, g = mark(plain, (f, tn)), mark(gen, (f, tn))
            if (p == ".") != (g == ".") and (f, tn) in KNOWN:
                g = "K"
            elif (p == ".") != (g == "."):
                differ += 1
            cells.append(p + "/" + g)
        print(f"{f:9} " + " ".join(cells))
    # Run what both sides accept: the same lines from both programs.
    outs = []
    for generic in (False, True):
        side = "generic" if generic else "plain"
        path = os.path.join(a.out, f"run-{side}.gopp")
        open(path, "w").write(program(ok, generic, run=True))
        exe = os.path.join(a.out, f"run-{side}")
        r = run([a.gopp, "build", path, "-o", exe])
        if r.returncode:
            print(f"{side}: the accepted rows do not build:\n{r.stderr}")
            return 1
        r = run([exe])
        outs.append((r.stdout + r.stderr).strip())
    if a.go_source:
        go_rows = [x for x in ok if dict((f, g) for f, _, g, _ in FORMS).get(
            x[0], True)]
        open(a.go_source, "w").write(program(go_rows, False, run=True))
    print(f"differ: {differ}")
    print("runs: " + ("same" if outs[0] == outs[1] else "DIFFER"))
    print(outs[1])
    return 0


sys.exit(main())
