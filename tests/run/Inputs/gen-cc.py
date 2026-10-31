#!/usr/bin/env python3
"""Generate a program of Go++ to Go++ calls (P04.18, the internal calling
convention), with the output it must print.

usage: gen-cc.py --seed N --count K --dir DIR

Writes DIR/main.gopp (the Go++ program), DIR/main.go (the same program for
`go run`: the `move` marks of parameters left out) and DIR/expected.txt.

The program is valid Go and valid Go++ and prints only integers, bools and
string constants with println, so Go's standard error (Go's println writes
there) and Go++'s standard output (D4.11 e) are the same text: Go is the
oracle (tests/run/Inputs/check-cc-go.py compares them; the tests themselves
never need Go). A float is printed as int64(x*1024): every float the generator
makes is a multiple of 1/1024 below 2**23, which float32 and float64 hold
exactly.

K signatures are made, each one a function that prints every scalar of every
parameter and returns values the generator chose; its caller builds the
arguments from literals and prints every scalar of the result. So what a
callee prints says what arrived, and what the caller prints says what came
back, and the generator knows both. Which functions:

  plain      up to 24 parameters of scalars and of structs (nested, with
             arrays, empty ones, small ones that travel in registers and
             large ones that go through memory), results of nothing, one
             value, a struct, or two to five values (the shape of `(T, error)`)
  forward    a function that passes its parameters on to a plain one and
             returns its result (the values go through registers and the stack
             twice)
  recursive  a function that calls itself with a struct and returns a struct,
             printing on the way down and up
  trivial    parameters of a Copy type that the callee changes: the caller's
             values stay as they were (a trivial parameter is a copy)
  borrowed   parameters that own memory (a struct with a slice, a string, a
             slice, a string): read only in the callee; elements written
             through a slice are seen by the caller
  moved      `move` parameters, passed temporaries (Go++ only: main.go has no
             `move`)
"""
import argparse
import os
import random

SCALARS = {
    "i8": ("int8", -(1 << 7), (1 << 7) - 1),
    "u8": ("uint8", 0, (1 << 8) - 1),
    "i16": ("int16", -(1 << 15), (1 << 15) - 1),
    "u16": ("uint16", 0, (1 << 16) - 1),
    "i32": ("int32", -(1 << 31), (1 << 31) - 1),
    "u32": ("uint32", 0, (1 << 32) - 1),
    "i64": ("int64", -(1 << 63), (1 << 63) - 1),
    "u64": ("uint64", 0, (1 << 64) - 1),
    "int": ("int", -(1 << 63), (1 << 63) - 1),
}
FLOATS = {"f32": "float32", "f64": "float64"}
NGLOBALS = 4


class Scalar:
    def __init__(self, key):
        self.key = key
        if key in SCALARS:
            self.go = SCALARS[key][0]
            self.kind = "int"
        elif key in FLOATS:
            self.go = FLOATS[key]
            self.kind = "float"
        elif key == "bool":
            self.go = "bool"
            self.kind = "bool"
        else:
            assert key == "ptr"
            self.go = "*int32"
            self.kind = "ptr"

    def leaves(self, path, val):
        return [(path, self, val)]

    def values(self, rng):
        k = self.key
        if self.kind == "int":
            _, lo, hi = SCALARS[k]
            c = rng.random()
            if c < 0.2:
                return rng.choice([lo, hi, 0, 1, hi - 1, lo + 1, -1 if lo else 2])
            return rng.randint(lo, hi)
        if self.kind == "float":
            m = rng.randint(-(1 << 22) + 1, (1 << 22) - 1)
            return m / 1024.0 if rng.random() < 0.7 else float(rng.randint(-1000, 1000))
        if self.kind == "bool":
            return rng.randint(0, 1)
        return rng.choice(list(range(NGLOBALS)))

    def lit(self, v):
        if self.kind == "int":
            return "%s(%d)" % (self.go, v)
        if self.kind == "float":
            return "%s(%r)" % (self.go, v)
        if self.kind == "bool":
            return "true" if v else "false"
        return "&gv%d" % v

    def show(self, path):
        """The Go expression printed for the scalar at path."""
        if self.kind == "float":
            return "int64(%s*1024)" % path
        if self.kind == "ptr":
            return "pid(%s)" % path
        return path

    def text(self, v):
        """What println prints for the value."""
        if self.kind == "int":
            return str(v)
        if self.kind == "float":
            return str(int(v * 1024))
        if self.kind == "bool":
            return "true" if v else "false"
        return str(GLOBALS[v])

    def zero_size(self):
        return False


class ArrayTy:
    def __init__(self, elem, n):
        self.elem, self.n = elem, n
        self.go = "[%d]%s" % (n, elem.go)

    def leaves(self, path, val):
        out = []
        for i in range(self.n):
            out += self.elem.leaves("%s[%d]" % (path, i), val[i])
        return out

    def values(self, rng):
        return [self.elem.values(rng) for _ in range(self.n)]

    def lit(self, v):
        return "%s{%s}" % (self.go, ", ".join(self.elem.lit(x) for x in v))

    def zero_size(self):
        return self.n == 0 or self.elem.zero_size()


class StructTy:
    def __init__(self, name, fields):
        self.name, self.fields = name, fields
        self.go = name

    def leaves(self, path, val):
        out = []
        for i, f in enumerate(self.fields):
            out += f.leaves("%s.f%d" % (path, i), val[i])
        return out

    def values(self, rng):
        return [f.values(rng) for f in self.fields]

    def lit(self, v):
        return "%s{%s}" % (self.name, ", ".join(
            "f%d: %s" % (i, f.lit(v[i])) for i, f in enumerate(self.fields)))

    def zero_size(self):
        return all(f.zero_size() for f in self.fields)


GLOBALS = []


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.structs = []
        self.n = 0

    def new_struct(self, fields):
        self.n += 1
        s = StructTy("S%d" % self.n, fields)
        self.structs.append(s)
        return s

    def scalar(self, floats=0.25):
        r = self.rng
        if r.random() < floats:
            return Scalar(r.choice(["f32", "f64"]))
        if r.random() < 0.08:
            return Scalar("bool")
        if r.random() < 0.05:
            return Scalar("ptr")
        return Scalar(r.choice(list(SCALARS)))

    def struct(self, depth=0, force=None):
        r = self.rng
        if self.structs and r.random() < 0.2:
            return r.choice(self.structs)
        kind = force or r.choice(
            ["ints", "mixed", "mixed", "floats", "tiny", "nested", "array",
             "empty", "pair", "one", "big", "chars"])
        if kind == "ints":
            f = [Scalar(r.choice(["i8", "i16", "i32", "i64", "u32", "u64"]))
                 for _ in range(r.randint(1, 4))]
        elif kind == "mixed":
            f = [self.scalar() for _ in range(r.randint(2, 7))]
        elif kind == "floats":
            t = Scalar(r.choice(["f32", "f64"]))
            f = [t] * r.randint(1, 5)
        elif kind == "tiny":
            f = [Scalar(r.choice(["i8", "u8", "bool", "i16"]))
                 for _ in range(r.randint(1, 6))]
        elif kind == "nested" and depth < 2:
            f = [self.struct(depth + 1) for _ in range(r.randint(1, 3))]
            f += [self.scalar() for _ in range(r.randint(0, 2))]
            r.shuffle(f)
        elif kind == "array":
            el = Scalar(r.choice(["i8", "i16", "i32", "i64", "f32", "f64", "bool"]))
            f = [ArrayTy(el, r.randint(0, 6))]
            if r.random() < 0.5:
                f.append(self.scalar())
        elif kind == "empty":
            f = [] if r.random() < 0.5 else [self.new_struct([]), Scalar("i32")]
        elif kind == "pair":
            f = [self.scalar(0.3), self.scalar(0.3)]
        elif kind == "one":
            f = [self.scalar(0.3)]
        elif kind == "big":
            f = [ArrayTy(Scalar(r.choice(["i64", "f64", "i32", "u8"])),
                         r.randint(8, 40))]
            if r.random() < 0.5:
                f.append(self.scalar())
        elif kind == "chars":
            f = [Scalar("u8")] * r.randint(1, 9)
        else:
            f = [self.scalar()]
        return self.new_struct(f)

    def ptype(self):
        return self.scalar(0.35) if self.rng.random() < 0.45 else self.struct()

    def signature(self):
        r = self.rng
        c = r.random()
        if c < 0.1:
            n = r.randint(12, 24)
        elif c < 0.45:
            n = r.randint(0, 3)
        else:
            n = r.randint(1, 7)
        params = [self.ptype() for _ in range(n)]
        x = r.random()
        if x < 0.1:
            rets = []
        elif x < 0.3:
            rets = [self.scalar(0.3)]
        elif x < 0.5:
            rets = [self.struct()]
        else:
            # several results; (T, bool) and (T, *int32) are the shapes of
            # (T, error)
            k = r.randint(2, 5)
            rets = [self.ptype() for _ in range(k)]
            if r.random() < 0.4:
                rets[-1] = Scalar(r.choice(["bool", "ptr"]))
        return params, rets


def show_lines(g, name, tys, vals, indent="\t"):
    """println statements for the values (named by Go expressions) and the
    lines they print. Returns (code lines, expected lines)."""
    code, exp = [], []
    for nm, ty, v in zip(name, tys, vals):
        ls = ty.leaves(nm, v)
        if not ls:
            code.append('%sprintln("%s")' % (indent, nm))
            exp.append(nm)
            continue
        # at most 8 values per println
        for k in range(0, len(ls), 8):
            chunk = ls[k:k + 8]
            code.append('%sprintln("%s", %s)' % (
                indent, nm, ", ".join(sc.show(p) for (p, sc, _) in chunk)))
            exp.append(" ".join([nm] + [sc.text(val) for (_, sc, val) in chunk]))
    return code, exp


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--dir", required=True)
    args = ap.parse_args()
    g = Gen(args.seed)
    r = g.rng
    GLOBALS.clear()
    GLOBALS.extend(r.randint(-1000, 1000) for _ in range(NGLOBALS))

    funcs = []   # Go text (may contain "move ")
    calls = []   # Go text of the call_N functions
    expected = []
    mains = []

    def call_and_show(i, fname, params, rets, pvals, rvals, call_args=None):
        """The body of call_<i>: build args, call, print the results."""
        body, exp = [], []
        names = []
        for j, (t, v) in enumerate(zip(params, pvals)):
            body.append("\tx%d := %s" % (j, t.lit(v)))
            names.append("x%d" % j)
        args_txt = ", ".join(call_args or names)
        if not rets:
            body.append("\t%s(%s)" % (fname, args_txt))
        else:
            rn = ["r%d" % k for k in range(len(rets))]
            body.append("\t%s := %s(%s)" % (", ".join(rn), fname, args_txt))
            body += ["\t_ = " + n for n in rn]
            c, e = show_lines(g, rn, rets, rvals)
            body += c
            exp += e
        return body, exp

    def plain(i, tag="f", forwards=None):
        params, rets = g.signature()
        pvals = [t.values(r) for t in params]
        rvals = [t.values(r) for t in rets]
        name = "%s%d" % (tag, i)
        pn = ["a%d" % j for j in range(len(params))]
        # callee
        body = ['\tprintln("%s")' % name]
        c, e_callee = show_lines(g, pn, params, pvals)
        body += c
        if rets:
            body.append("\treturn " + ", ".join(t.lit(v) for t, v in zip(rets, rvals)))
        sig = ", ".join("%s %s" % (n, t.go) for n, t in zip(pn, params))
        if len(rets) == 0:
            rt = ""
        elif len(rets) == 1:
            rt = " " + rets[0].go
        else:
            rt = " (" + ", ".join(t.go for t in rets) + ")"
        funcs.append("func %s(%s)%s {\n%s\n}\n" % (name, sig, rt, "\n".join(body)))
        return name, params, rets, pvals, rvals, e_callee, sig, rt

    n_plain = 0
    for i in range(args.count):
        kind = i % 6
        callee_exp = []
        if kind in (0, 1, 2):
            name, params, rets, pvals, rvals, e_callee, sig, rt = plain(i)
            target = name
            if kind == 1:
                # forward: pass the parameters on and return the result
                pn = ["a%d" % j for j in range(len(params))]
                wname = "w%d" % i
                funcs.append("func %s(%s)%s {\n\tprintln(\"%s\")\n\treturn %s(%s)\n}\n" % (
                    wname, sig, rt, wname, name, ", ".join(pn)) if rets else
                    "func %s(%s) {\n\tprintln(\"%s\")\n\t%s(%s)\n}\n" % (
                        wname, sig, wname, name, ", ".join(pn)))
                target = wname
                callee_exp = [wname] + [name]
            else:
                callee_exp = [name]
            body, exp = call_and_show(i, target, params, rets, pvals, rvals)
            expected += callee_exp + e_callee + exp
            calls.append("func call%d() {\n%s\n}\n" % (i, "\n".join(body)))
        elif kind == 3:
            # recursive: a struct goes down and up
            st = g.struct(force="mixed")
            ints = [k for k, f in enumerate(st.fields)
                    if isinstance(f, Scalar) and f.kind == "int" and f.key in ("i32", "i64", "int")]
            if not ints:
                st = g.new_struct([Scalar("i32"), Scalar("f64"), Scalar("u8"),
                                   ArrayTy(Scalar("i16"), 3)])
                ints = [0]
            k0 = ints[0]
            depth = r.randint(1, 6)
            v0 = st.values(r)
            v0[k0] = r.randint(-1000, 1000)
            b0 = r.randint(-50, 50)
            name = "r%d" % i
            sc = st.fields[k0]
            ls = st.leaves("a", v0)
            body = ['\tprintln("%s", n, %s, b)' % (name, ", ".join(s.show(p) for (p, s, _) in ls[:6]))]
            body += ["\tif n == 0 {", "\t\treturn a, b", "\t}",
                     "\ta.f%d = a.f%d + 1" % (k0, k0),
                     "\ta2, b2 := %s(n-1, a, b+1)" % name,
                     '\tprintln("%s up", n, b2, a2.f%d)' % (name, k0),
                     "\treturn a2, b2 + int64(n)"]
            funcs.append("func %s(n int, a %s, b int64) (%s, int64) {\n%s\n}\n" % (
                name, st.go, st.go, "\n".join(body)))
            # model
            def sim(n, a, b, out):
                a = list(a)
                ls = st.leaves("a", a)
                out.append(" ".join([name, str(n)] + [s.text(v) for (p, s, v) in ls[:6]] + [str(b)]))
                if n == 0:
                    return a, b
                a[k0] = a[k0] + 1
                if sc.key in SCALARS:
                    lo, hi = SCALARS[sc.key][1], SCALARS[sc.key][2]
                    if a[k0] > hi:
                        a[k0] = lo
                a2, b2 = sim(n - 1, a, b + 1, out)
                out.append("%s up %d %d %s" % (name, n, b2, sc.text(a2[k0])))
                bb = b2 + n
                bb = (bb + (1 << 63)) % (1 << 64) - (1 << 63)
                return a2, bb
            out = []
            ra, rb = sim(depth, v0, b0, out)
            expected += out
            body = ["\tx0 := %s" % st.lit(v0), "\tr0, r1 := %s(%d, x0, int64(%d))" % (name, depth, b0), "\t_, _ = r0, r1"]
            c, e = show_lines(g, ["r0", "r1"], [st, Scalar("i64")], [ra, rb])
            body += c
            expected += e
            c, e = show_lines(g, ["x0"], [st], [v0])
            body += c
            expected += e
            calls.append("func call%d() {\n%s\n}\n" % (i, "\n".join(body)))
        elif kind == 4:
            # trivial: the callee changes its copies; the caller's stay
            st = g.struct(force="mixed")
            arr = ArrayTy(Scalar("i32"), r.randint(2, 6))
            ints = [k for k, f in enumerate(st.fields) if isinstance(f, Scalar) and f.kind == "int"]
            if not ints:
                st = g.new_struct([Scalar("i64"), Scalar("f32")])
                ints = [0]
            k0 = ints[0]
            name = "t%d" % i
            pv, av, nv = st.values(r), arr.values(r), r.randint(-100, 100)
            body = ["\tp.f%d = p.f%d + 1" % (k0, k0), "\tq[1] = q[1] + 5", "\tn += 7"]
            sc = st.fields[k0]
            p2 = list(pv); p2[k0] = pv[k0] + 1
            if sc.key in SCALARS and p2[k0] > SCALARS[sc.key][2]:
                p2[k0] = SCALARS[sc.key][1]
            q2 = list(av); q2[1] = (av[1] + 5 + (1 << 31)) % (1 << 32) - (1 << 31)
            n2 = nv + 7
            c, e1 = show_lines(g, ["p", "q"], [st, arr], [p2, q2])
            body += c
            body += ['\tprintln("%s n", n)' % name, "\treturn p, q, n"]
            funcs.append("func %s(p %s, q %s, n int) (%s, %s, int) {\n%s\n}\n" % (
                name, st.go, arr.go, st.go, arr.go, "\n".join(body)))
            cb = ["\tx0 := %s" % st.lit(pv), "\tx1 := %s" % arr.lit(av),
                  "\tr0, r1, r2 := %s(x0, x1, %d)" % (name, nv), "\t_, _, _ = r0, r1, r2"]
            c, e2 = show_lines(g, ["r0", "r1", "r2"], [st, arr, Scalar("int")], [p2, q2, n2])
            cb += c
            c, e3 = show_lines(g, ["x0", "x1"], [st, arr], [pv, av])
            cb += c
            expected += e1 + ["%s n %d" % (name, n2)] + e2 + e3
            calls.append("func call%d() {\n%s\n}\n" % (i, "\n".join(cb)))
        else:
            # borrowed and moved parameters that own memory
            xs = [r.randint(-1000, 1000) for _ in range(r.randint(0, 6))]
            ys = [r.randint(-1000, 1000) for _ in range(r.randint(1, 6))]
            s1 = "".join(r.choice("abcdefgh ") for _ in range(r.randint(0, 9)))
            nn = r.randint(-30000, 30000)
            moved = r.random() < 0.5
            mv = "move " if moved else ""
            name = "b%d" % i
            sumx, sumy = sum(xs), sum(ys)
            body = [
                "\ts := 0",
                "\tfor i := 0; i < len(o.Xs); i++ {",
                "\t\ts += int(o.Xs[i])",
                "\t}",
                '\tprintln("%s", len(o.Xs), s, o.N, o.Name, len(o.Name), len(ys), len(str))' % name,
                "\tys[0] = ys[0] + 1",
                "\treturn s + int(ys[0]) + k",
            ]
            funcs.append("type Own%d struct {\n\tXs []int32\n\tN int16\n\tName string\n}\n" % i)
            funcs.append("func %s(%so Own%d, %sys []int32, str string, k int) int {\n%s\n}\n" % (
                name, mv, i, mv, "\n".join(body)))
            xlit = "[]int32{%s}" % ", ".join(str(x) for x in xs) if xs else "[]int32{}"
            ylit = "[]int32{%s}" % ", ".join(str(y) for y in ys)
            cb = [] if moved else ['\tys := %s' % ylit]
            if moved:
                call = '%s(Own%d{%s, %d, "%s"}, %s, "%s", 3)' % (
                    name, i, xlit, nn, s1, ylit, s1)
                cb.append("\tr := " + call)
                ys_after = None
            else:
                cb.append('\to := Own%d{%s, %d, "%s"}' % (i, xlit, nn, s1))
                cb.append('\tr := %s(o, ys, "%s", 3)' % (name, s1))
            res = sumx + (ys[0] + 1) + 3
            cb.append('\tprintln("r", r)')
            expected.append("%s %d %d %d %s %d %d %d" % (name, len(xs), sumx, nn, s1, len(s1), len(ys), len(s1)))
            expected.append("r %d" % res)
            if not moved:
                # the write through the borrowed slice is seen; the struct is
                # as it was
                cb.append('\tprintln("after", ys[0], len(o.Xs), o.N, o.Name)')
                expected.append("after %d %d %d %s" % (ys[0] + 1, len(xs), nn, s1))
            calls.append("func call%d() {\n%s\n}\n" % (i, "\n".join(cb)))
        mains.append("\tcall%d()" % i)

    head = ["// generated by gen-cc.py --seed %d --count %d" % (args.seed, args.count),
            "package main\n"]
    for k in range(NGLOBALS):
        head.append("var gv%d int32 = %d\n" % (k, GLOBALS[k]))
    head.append("func pid(p *int32) int32 {\n\treturn *p\n}\n")
    for s in g.structs:
        fields = "\n".join("\tf%d %s" % (i, f.go) for i, f in enumerate(s.fields))
        head.append("type %s struct {\n%s\n}\n" % (s.name, fields))
    text = "\n".join(head + funcs + calls + ["func main() {\n" + "\n".join(mains) + "\n}\n"])
    os.makedirs(args.dir, exist_ok=True)
    open(os.path.join(args.dir, "main.gopp"), "w").write(text)
    open(os.path.join(args.dir, "main.go"), "w").write(text.replace("move ", ""))
    open(os.path.join(args.dir, "expected.txt"), "w").write(
        "".join(l + "\n" for l in expected))


if __name__ == "__main__":
    main()
