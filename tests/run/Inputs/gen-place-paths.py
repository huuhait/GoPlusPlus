#!/usr/bin/env python3
"""The table of where a borrowed Place receiver or argument lives (P04.18,
reviews R9 to R11; Body::placeHow and docs/design/calling-convention.md).

usage: gen-place-paths.py (program | want | refused | check-refused ERRORS)

Every path shape is used as a receiver (`E.Put()`) and as an argument
(`put(E)`) of a value that holds a nil map. The method or function makes the
map (PAR-1); the program then prints, for each shape and mode, the length of
the original's map:

  write    the callee got the address of the original: 1 (PAR-1)
  copy     the callee got a copy: the original, or the interface's box, is
           unchanged (TYP-5, TYP-9; or no place at all): 0
  refused  a value that holds a map, in a map entry that is not copied in
           and out: one E0439 naming P04.09 (`refused` makes that program;
           `check-refused ERRORS` checks that the diagnostics `gopp build`
           wrote for it are exactly those, one per call, and nothing else)

`program` makes the program of the write and copy rows, `want` its output.
(A type-switch or assertion binding of an owning type is a View, TYP-9: no
Place receiver or argument at all.)
"""
import re
import sys

PRELUDE = """package main

type Doc struct {
	N     int
	Index map[string]int
}

func (d Doc) Put() int {
	d.Index["x"] = 1
	return len(d.Index)
}

func put(d Doc) int {
	d.Index["x"] = 1
	return len(d.Index)
}

type W struct{ D Doc }

type WS struct{ Ds []Doc }

type Tab map[string]int

func (t Tab) Add() int {
	t["x"] = 1
	return len(t)
}

func add(t Tab) int {
	t["x"] = 1
	return len(t)
}

type HT struct{ T Tab }

type Named struct{ S string }

func (n Named) Len() int { return len(n.S) }

func nlen(n Named) int { return len(n.S) }

var gd Doc

func mk() Doc { return Doc{} }

func optLen(o Option[Doc]) int {
	if o != nil {
		return len(o.Index)
	}
	return -1
}

func recvChecked(o Option[Doc]) {
	if o != nil {
		o.Put()
	}
}

func putChecked(o Option[Doc]) {
	if o != nil {
		put(o)
	}
}

func toOpt(o Option[Doc]) {
	if o != nil {
		o.Index["x"] = 1
	}
}

type Pair [2]Doc

func pairPut(p Pair) int {
	p[1].Index["x"] = 1
	return len(p[1].Index)
}

func putUnnamed(d struct {
	N     int
	Index map[string]int
}) int {
	d.Index["x"] = 1
	return len(d.Index)
}

type DocAlias = Doc

type OptDoc Option[Doc]

func putOptDoc(o OptDoc) {
	if o != nil {
		o.Index["x"] = 1
	}
}
"""

# (name, setup, receiver call, argument call, check, kind[, want]): the check
# is the length of the original's map after the call (kind: write 1, copy 0,
# or the given want).
SHAPES = [
    ("local", "var d Doc", "d.Put()", "put(d)", "len(d.Index)", "write"),
    ("field", "var w W", "w.D.Put()", "put(w.D)", "len(w.D.Index)", "write"),
    ("array-elem", "var a [2]Doc", "a[1].Put()", "put(a[1])",
     "len(a[1].Index)", "write"),
    ("deref", "var d Doc\n\tp := &d", "(*p).Put()", "put(*p)",
     "len(d.Index)", "write"),
    ("ptr-field", "var w W\n\tp := &w", "p.D.Put()", "put(p.D)",
     "len(w.D.Index)", "write"),
    ("slice-elem", "s := make([]Doc, 2)", "s[1].Put()", "put(s[1])",
     "len(s[1].Index)", "write"),
    ("slice-of-map-entry", 'ms := map[string][]Doc{"k": make([]Doc, 2)}',
     'ms["k"][1].Put()', 'put(ms["k"][1])', 'len(ms["k"][1].Index)', "write"),
    ("slice-in-map-entry", 'mw := map[string]WS{"k": {Ds: make([]Doc, 1)}}',
     'mw["k"].Ds[0].Put()', 'put(mw["k"].Ds[0])', 'len(mw["k"].Ds[0].Index)',
     "write"),
    ("slice-of-slice", "ss := [][]Doc{make([]Doc, 1)}", "ss[0][0].Put()",
     "put(ss[0][0])", "len(ss[0][0].Index)", "write"),
    ("package-var", "gd = Doc{}", "gd.Put()", "put(gd)", "len(gd.Index)",
     "write"),
    ("assert-pointer", "var d Doc\n\tvar x any = &d", "x.(*Doc).Put()",
     "put(*x.(*Doc))", "len(d.Index)", "write"),
    ("assert-value", "var x any = Doc{}", "x.(Doc).Put()", "put(x.(Doc))",
     "len(x.(Doc).Index)", "copy"),
    ("assert-array-elem", "var q any = [2]Doc{}", "q.([2]Doc)[1].Put()",
     "put(q.([2]Doc)[1])", "len(q.([2]Doc)[1].Index)", "copy"),
    ("assert-slice-elem", "var b any = make([]Doc, 1)", "b.([]Doc)[0].Put()",
     "put(b.([]Doc)[0])", "len(b.([]Doc)[0].Index)", "copy"),
    ("assert-field-slice-elem", "var y any = WS{Ds: make([]Doc, 1)}",
     "y.(WS).Ds[0].Put()", "put(y.(WS).Ds[0])", "len(y.(WS).Ds[0].Index)",
     "copy"),
    ("call-result", "", "mk().Put()", "put(mk())", "0", "copy"),
    # A checked Option "simply is a T" (OWN-P10, review R12): its payload is
    # a part of the variable, also through a parameter (the caller's); one
    # that a conversion makes (`Doc` to `Option[Doc]`) is a new value.
    ("checked-option", "var o Option[Doc] = Doc{}",
     "if o != nil {\n\t\to.Put()\n\t}", "if o != nil {\n\t\tput(o)\n\t}",
     "optLen(o)", "write"),
    ("checked-option-param", "var o Option[Doc] = Doc{}", "recvChecked(o)",
     "putChecked(o)", "optLen(o)", "write"),
    ("converted-to-option", "var d Doc", None, "toOpt(d)", "len(d.Index)",
     "copy"),
    # Assigned as it is with another type (the checker's assignableAsIs:
    # identical underlying types, one of them unnamed; an alias is the type
    # itself): no new value, the original (review R14).
    ("unnamed-struct-to-named",
     "var u struct {\n\t\tN     int\n\t\tIndex map[string]int\n\t}", None,
     "put(u)", "len(u.Index)", "write"),
    ("named-struct-to-unnamed", "var d Doc", None, "putUnnamed(d)",
     "len(d.Index)", "write"),
    ("unnamed-array-to-named", "var a [2]Doc", None, "pairPut(a)",
     "len(a[1].Index)", "write"),
    ("alias", "var d DocAlias", "d.Put()", "put(d)", "len(d.Index)", "write"),
    ("unnamed-option-to-named", "var o Option[Doc] = Doc{}", None,
     "putOptDoc(o)", "optLen(o)", "write"),
    ("unnamed-map-to-named", "var m map[string]int", None, "add(m)", "len(m)",
     "write"),
    ("map-local", "var t Tab", "t.Add()", "add(t)", "len(t)", "write"),
    ("map-entry-of-map", 'mt := map[string]Tab{"k": nil}', 'mt["k"].Add()',
     'add(mt["k"])', 'len(mt["k"])', "write"),
    ("map-in-box", "var y any = Tab(nil)", "y.(Tab).Add()", "add(y.(Tab))",
     "len(y.(Tab))", "copy"),
    ("no-map-in-map-entry", 'mn := map[string]Named{"k": {"ab"}}',
     'mn["k"].Len()', 'nlen(mn["k"])', 'len(mn["k"].S)', "copy", "2"),
    ("map-entry", 'md := map[string]Doc{"k": {}}', 'md["k"].Put()',
     'put(md["k"])', "", "refused"),
    ("field-of-map-entry", 'mh := map[string]W{"k": {}}', 'mh["k"].D.Put()',
     'put(mh["k"].D)', "", "refused"),
    ("array-elem-of-map-entry", 'ma := map[string][2]Doc{"k": {}}',
     'ma["k"][1].Put()', 'put(ma["k"][1])', "", "refused"),
    ("map-field-of-map-entry", 'mh := map[string]HT{"k": {}}',
     'mh["k"].T.Add()', 'add(mh["k"].T)', "", "refused"),
]

MODES = (("receiver", 2), ("argument", 3))


def want(shape):
    if len(shape) > 6:
        return shape[6]
    return "1" if shape[5] == "write" else "0"


def ident(name, mode):
    return (name + "_" + mode).replace("-", "_")


def program():
    out = [PRELUDE]
    calls = []
    for shape in SHAPES:
        if shape[5] == "refused":
            continue
        for mode, col in MODES:
            if shape[col] is None:
                continue
            fn = ident(shape[0], mode)
            body = "\t" + shape[1] + "\n" if shape[1] else ""
            body += "\t" + shape[col] + "\n"
            body += '\tprintln("%s %s %s", %s)\n' % (shape[0], mode, shape[5],
                                                   shape[4])
            out.append("func %s() {\n%s}\n" % (fn, body))
            calls.append("\t%s()\n" % fn)
    out.append("func main() {\n" + "".join(calls) + "}\n")
    return "\n".join(out)


def want_text():
    lines = []
    for shape in SHAPES:
        if shape[5] == "refused":
            continue
        for mode, col in MODES:
            if shape[col] is None:
                continue
            lines.append("%s %s %s %s" % (shape[0], mode, shape[5], want(shape)))
    return "\n".join(lines) + "\n"


REFUSAL = ("error[E0439]: a value that holds a map passed from a map entry "
           "(P04.09) is not supported by code generation yet (P04)")


def refused():
    """The program of the refused rows and the line of each call."""
    text = PRELUDE
    lines = []
    for shape in SHAPES:
        if shape[5] != "refused":
            continue
        for mode, col in MODES:
            text += "\nfunc %s() {\n\t%s\n" % (ident(shape[0], mode), shape[1])
            lines.append(text.count("\n") + 1)
            text += "\t_ = %s\n}\n" % shape[col]
    return text + "\nfunc main() {}\n", lines


def check_refused(path):
    got = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"^\S+?:(\d+):\d+: (error\[E\d+\]: .*)$", line.rstrip())
        if m:
            got.append((int(m.group(1)), m.group(2)))
        elif "error" in line and not line.startswith(" "):
            got.append((0, line.rstrip()))
    want = [(n, REFUSAL) for n in refused()[1]]
    if sorted(got) != sorted(want):
        sys.exit("refused diagnostics differ:\n  got  %s\n  want %s" %
                 (sorted(got), sorted(want)))
    print("%d refusals" % len(want))


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if what == "program":
        sys.stdout.write(program())
    elif what == "want":
        sys.stdout.write(want_text())
    elif what == "refused":
        sys.stdout.write(refused()[0])
    elif what == "check-refused" and len(sys.argv) > 2:
        check_refused(sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
