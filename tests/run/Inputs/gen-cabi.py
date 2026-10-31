#!/usr/bin/env python3
"""Generate a C ABI test (P04.11): random signatures called from Go++ into C.

usage: gen-cabi.py --seed N --count K --dir DIR [--mode random|edges]
                  [--start S] [--stride T] [--direction forward|reverse]

Writes DIR/main.gopp (the Go++ program), DIR/lib.c (the C functions, built by
Clang) and DIR/expected.txt (what the program must print).

--mode random (the default): every signature has up to 24 parameters and a
result: scalars (bool, signed and unsigned integers of every width, floats,
pointers, Option[*byte]) and structs of them, nested, with
arrays, empty structs and homogeneous float structs (HFA on arm64), small (in
registers) and large (in memory). Many parameters, and signatures made of
floats and HFAs only, make the register classes run out.

--mode edges: the enumerated signatures at the edges of the register classes
(edge_sigs, about 2150), every STRIDE-th from START, at most COUNT; the seed
picks only the values.

--direction reverse (P04.18): the other way. The same signatures, but C calls
Go++: for even N, the Go++ function `gopp_fN` is exported to C
(`@Export("C")`) and C calls it by name; for odd N, `gopp_fN` is a plain
Go++ function that Go++ passes to the C function `cabi_cbN`, which calls it
through the pointer (a callback, wrapped in a C-ABI thunk). The Go++ function
prints the bytes of every scalar field of the arguments it received and
returns the values the generator chose; C prints the bytes of the result it
got. The output is the same text as in the forward direction.

The C function `cabi_fN` prints, with write(2), the bytes of every scalar
field of every argument as it received them; Go++ prints, after the call, the
bytes of every scalar field of the result. The program's output is compared
with expected.txt, which this script computes from the values it chose, so
neither Go++ nor C is the oracle for the other: a wrong register, a lost
field, a wrong extension or a mis-sized copy shows up as different bytes.
The Python here knows the byte order (little endian on both targets) and the
sizes of the scalars; it does not model struct layout, because it dumps field
by field.
"""
import argparse
import itertools
import os
import random
import struct

# name -> (Go++ type, C type, size, kind)
SCALARS = {
    "i8": ("int8", "signed char", 1, "int"),
    "u8": ("uint8", "unsigned char", 1, "int"),
    "i16": ("int16", "short", 2, "int"),
    "u16": ("uint16", "unsigned short", 2, "int"),
    "i32": ("int32", "int", 4, "int"),
    "u32": ("uint32", "unsigned int", 4, "int"),
    "i64": ("int64", "long", 8, "int"),
    "u64": ("uint64", "unsigned long", 8, "int"),
    "int": ("int", "long", 8, "int"),
    "f32": ("float32", "float", 4, "float"),
    "f64": ("float64", "double", 8, "float"),
    "bool": ("bool", "_Bool", 1, "bool"),
    "ptr": ("*byte", "void *", 8, "ptr"),
    "optptr": ("Option[*byte]", "void *", 8, "optptr"),
}
# Option[T] by value (T not a pointer) is refused in C signatures until P07
# (decision M6): only Option[*byte], a pointer, is generated.

SIGNED = {"i8", "i16", "i32", "i64", "int"}


class Scalar:
    def __init__(self, key):
        self.key = key
        self.gopp, self.c, self.size, self.kind = SCALARS[key]

    def decl_c(self, name):
        return "%s %s" % (self.c, name)

    def leaves(self, gpath, cpath):
        return [(gpath, cpath, self)]


class ArrayTy:
    def __init__(self, elem, n):
        self.elem, self.n = elem, n
        self.gopp = "[%d]%s" % (n, elem.gopp)

    def decl_c(self, name):
        # an array of arrays is not generated
        return "%s %s[%d]" % (self.elem.c, name, self.n)

    def leaves(self, gpath, cpath):
        out = []
        for i in range(self.n):
            out += self.elem.leaves("%s[%d]" % (gpath, i), "%s[%d]" % (cpath, i))
        return out


class StructTy:
    def __init__(self, name, fields):
        self.name = name
        self.fields = fields  # list of types
        self.gopp = name
        self.c = "struct " + name

    def decl_c(self, name):
        return "%s %s" % (self.c, name)

    def leaves(self, gpath, cpath):
        out = []
        for i, f in enumerate(self.fields):
            out += f.leaves("%s.f%d" % (gpath, i), "%s.f%d" % (cpath, i))
        return out


def zero_size(t):
    """A type of no bytes: an empty struct, structs and arrays of them, an
    array of length 0."""
    if isinstance(t, StructTy):
        return all(zero_size(f) for f in t.fields)
    if isinstance(t, ArrayTy):
        return t.n == 0 or zero_size(t.elem)
    return False


def leaf_parts(leaf):
    """(go path, c path, scalar type, role)"""
    if len(leaf) == 3:
        return leaf + ("plain",)
    return leaf


class Gen:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.structs = []  # in creation order: dependencies first
        self.counter = 0

    # --- values ---------------------------------------------------------
    def value(self, sc):
        r = self.rng
        k = sc.key
        if sc.kind == "int":
            bits = sc.size * 8
            lo = -(1 << (bits - 1)) if k in SIGNED else 0
            hi = (1 << (bits - 1)) - 1 if k in SIGNED else (1 << bits) - 1
            c = r.random()
            if c < 0.15:
                return r.choice([lo, hi, 0, 1, hi - 1, lo + 1])
            if c < 0.25 and k in SIGNED:
                return -r.randint(1, 100)
            return r.randint(lo, hi)
        if sc.kind == "float":
            m = r.randint(1, (1 << 20) - 1)
            e = r.randint(-30, 30)
            v = m * (2.0 ** e)
            if r.random() < 0.3:
                v = float(r.randint(1, 1000))
            if r.random() < 0.5:
                v = -v
            if sc.size == 4:
                v = struct.unpack("<f", struct.pack("<f", v))[0]
            return v
        if sc.kind == "bool":
            return r.randint(0, 1)
        if sc.kind == "optptr" and r.random() < 0.25:
            return 0
        if sc.kind in ("ptr", "optptr"):
            return 0x1000 + r.randint(0, 1 << 30) * 8
        raise AssertionError(k)

    def lit_go(self, sc, v):
        if sc.kind == "int":
            return str(v)
        if sc.kind == "float":
            return v.hex().replace("+", "+") if v >= 0 else "-" + (-v).hex()
        if sc.kind == "bool":
            return "true" if v else "false"
        return "nil" if v == 0 else "p(0x%x)" % v

    def lit_c(self, sc, v):
        if sc.kind == "int":
            if sc.key == "i64" or sc.key == "int":
                return "%dL" % v if v != -(1 << 63) else "(-9223372036854775807L-1)"
            if sc.key == "u64":
                return "%dUL" % v
            if sc.key == "u32":
                return "%dU" % v
            return str(v)
        if sc.kind == "float":
            h = v.hex() if v >= 0 else "-" + (-v).hex()
            return h + ("f" if sc.size == 4 else "")
        if sc.kind == "bool":
            return str(v)
        return "(void *)0x%xUL" % v

    def encode(self, sc, v):
        if sc.kind == "int":
            return v.to_bytes(sc.size, "little", signed=(sc.key in SIGNED)).hex()
        if sc.kind == "float":
            return struct.pack("<f" if sc.size == 4 else "<d", v).hex()
        if sc.kind == "bool":
            return "%02x" % v
        return v.to_bytes(8, "little").hex()

    def literal(self, ty, it, vals):
        """The Go++ expression for a value of ty, from the next leaf values."""
        if isinstance(ty, Scalar):
            (leaf, v) = next(it)
            return self.lit_go(ty, v)
        if isinstance(ty, ArrayTy):
            return "%s{%s}" % (ty.gopp, ", ".join(
                self.literal(ty.elem, it, vals) for _ in range(ty.n)))
        return "%s{%s}" % (ty.name, ", ".join(
            "f%d: %s" % (i, self.literal(f, it, vals))
            for i, f in enumerate(ty.fields)))

    # --- types ----------------------------------------------------------
    def scalar(self, floats=0.3):
        r = self.rng
        if r.random() < floats:
            return Scalar(r.choice(["f32", "f64"]))
        return Scalar(r.choice(["i8", "u8", "i16", "u16", "i32", "u32", "i64",
                                "u64", "int", "bool", "ptr", "optptr"]))

    def new_struct(self, fields):
        self.counter += 1
        s = StructTy("S%d" % self.counter, fields)
        self.structs.append(s)
        return s

    def struct(self, depth=0):
        r = self.rng
        # reuse a pool entry sometimes: the classification cache sees it again
        if self.structs and r.random() < 0.25:
            return r.choice(self.structs)
        kind = r.choice(["ints", "mixed", "hfa", "hfa", "big", "tiny", "nested",
                         "array", "array", "empty", "floatsint", "chars"])
        if kind == "ints":
            f = [Scalar(r.choice(["i8", "i16", "i32", "i64", "u32", "u64", "ptr"]))
                 for _ in range(r.randint(1, 4))]
        elif kind == "mixed":
            f = [self.scalar() for _ in range(r.randint(2, 6))]
        elif kind == "hfa":
            t = Scalar(r.choice(["f32", "f64"]))
            if r.random() < 0.3:
                f = [ArrayTy(t, r.randint(1, 4))]
            else:
                f = [t for _ in range(r.randint(1, 5))]
            if depth < 2 and r.random() < 0.3:
                # a nested HFA
                f = [self.new_struct(f[:2]), t]
        elif kind == "big":
            if r.random() < 0.5:
                f = [ArrayTy(Scalar(r.choice(["i64", "f64", "i32", "u8", "f32"])),
                             r.randint(3, 24))]
            else:
                f = [self.scalar() for _ in range(r.randint(6, 14))]
        elif kind == "tiny":
            f = [Scalar(r.choice(["i8", "u8", "bool", "i16"]))
                 for _ in range(r.randint(1, 6))]
        elif kind == "nested" and depth < 2:
            f = [self.struct(depth + 1) for _ in range(r.randint(1, 3))]
            f += [self.scalar() for _ in range(r.randint(0, 2))]
            r.shuffle(f)
        elif kind == "array":
            el = Scalar(r.choice(["i8", "i16", "i32", "i64", "f32", "f64", "bool"]))
            f = [ArrayTy(el, r.randint(1, 5))]
            if r.random() < 0.5:
                f.append(self.scalar())
            if r.random() < 0.3 and depth < 2:
                el = self.struct(depth + 1)
                if not zero_size(el):
                    # (an array of zero-size elements is not a C type for
                    # Go++: Clang's x86-64 classification divides by the
                    # element size; tests/gir/extern-bad-aggregates.test)
                    f = [ArrayTy(el, r.randint(1, 3))] + f
        elif kind == "empty":
            if r.random() < 0.5:
                f = []
            else:
                f = [self.new_struct([]), Scalar("i32")]
        elif kind == "floatsint":
            f = r.choice([[Scalar("f64"), Scalar("i64")],
                          [Scalar("i64"), Scalar("f64")],
                          [Scalar("f32"), Scalar("i32"), Scalar("f32")]])
        elif kind == "chars":
            f = [Scalar("u8")] * r.randint(1, 9)
        else:
            f = [self.scalar()]
        return self.new_struct(f)

    def param(self):
        r = self.rng
        x = r.random()
        if x < 0.42:
            return self.scalar(0.4)
        return self.struct()

    def float_param(self):
        """A float, or a struct of one float type: an HFA of 1 to 4 members
        on arm64 (5 is not one), also through nesting and arrays."""
        r = self.rng
        x = r.random()
        t = Scalar(r.choice(["f32", "f64"]))
        if x < 0.35:
            return t
        if x < 0.55:
            return self.new_struct([ArrayTy(t, r.randint(1, 5))])
        if x < 0.7:
            inner = self.new_struct([t, t])
            return self.new_struct([inner, t] if r.random() < 0.5
                                   else [inner, inner])
        return self.new_struct([t] * r.randint(1, 5))

    def signature(self):
        r = self.rng
        if r.random() < 0.15:
            # floats and HFAs only, so the vector registers run out
            params = [self.float_param() for _ in range(r.randint(6, 24))]
            return params, self.float_param()
        c = r.random()
        if c < 0.12:
            n = r.randint(8, 16)  # the register classes run out
        elif c < 0.2:
            n = r.randint(16, 24)
        elif c < 0.5:
            n = r.randint(0, 3)
        else:
            n = r.randint(1, 7)
        params = [self.param() for _ in range(n)]
        x = r.random()
        if x < 0.1:
            ret = None
        elif x < 0.4:
            ret = self.scalar(0.4)
        else:
            ret = self.struct()
        return params, ret


def edge_sigs(g):
    """The signatures at the edges of the register classes (c2's
    enumeration): every struct of one and two scalars, of three and four from
    smaller sets, arrays of every length around 8, 16 and 32 bytes, nested
    structs and arrays of structs (HFAs through nesting), empty structs and
    zero-length arrays, a struct after enough scalars to use the integer or
    vector registers up (both, in both orders), two structs in a row, narrow
    integers and bools among wide ones; each as an argument, a result and
    both. """
    sigs = []
    S = Scalar
    I8, I16, I32, I64 = S("i8"), S("i16"), S("i32"), S("i64")
    F32, F64, BOOL, PTR = S("f32"), S("f64"), S("bool"), S("ptr")

    def st(*fs):
        return g.new_struct(list(fs))

    def alone(t):
        sigs.append(([t], None))
        sigs.append(([], t))
        sigs.append(([t], t))

    two = [I8, I16, I32, I64, F32, F64, BOOL, PTR]
    for a in two:
        alone(st(a))
    for a, b in itertools.product(two, two):
        alone(st(a, b))
    three = [I8, I32, I64, F32, F64]
    for a, b, c in itertools.product(three, three, three):
        alone(st(a, b, c))
    for a, b, c, d in itertools.product([I8, I32, F32, F64], repeat=4):
        t = st(a, b, c, d)
        sigs.append(([t], t))
    # sizes around 8, 16 and 32 bytes
    for el, nmax in ((I8, 40), (I16, 18), (I32, 10), (I64, 6), (F32, 10),
                     (F64, 6), (BOOL, 20)):
        for n in range(1, nmax + 1):
            alone(st(ArrayTy(el, n)))
    for n in range(1, 6):
        alone(st(ArrayTy(F32, n), I32))
        alone(st(I8, ArrayTy(F32, n)))
        alone(st(ArrayTy(F64, n), I8))
        alone(st(ArrayTy(I8, 3 * n), F64))
    # nested structs and arrays of structs (HFA through nesting)
    pf, pd = st(F32, F32), st(F64, F64)
    for t in (st(pf, pf), st(pf, pf, F32), st(pd, F64), st(pd, pd),
              st(ArrayTy(pf, 2)), st(ArrayTy(pf, 3)), st(pf, I32, I32),
              st(st(st(F32)), st(F32), F32, F32), st(st(I8), st(I8), st(I16), I32),
              st(ArrayTy(st(I8, I8), 4))):
        alone(t)
    # empty structs and zero-length arrays, alone and among others
    e = st()
    for t in (e, st(ArrayTy(I32, 0)), st(e, e), st(e, I32), st(I32, e),
              st(F32, e, F32), st(ArrayTy(I64, 0), I8)):
        alone(t)
        sigs.append(([I32, t, I32], I32))
        sigs.append(([t, t], t))
    # register exhaustion: n scalars, then a struct that needs registers
    shapes = [st(I64), st(I64, I64), st(I32, I32, I32), st(I64, I64, I64),
              st(F64), st(F64, F64), st(F32, F32, F32), st(F32, F32, F32, F32),
              st(F64, F64, F64, F64), st(I64, F64), st(F64, I64), st(*[I8] * 9),
              st(F32, I32), st(PTR, PTR), st(BOOL, I8, S("i16"))]
    for sh in shapes:
        for kind in (I64, F64, I8, F32):
            for n in (4, 5, 6, 7, 8, 9):
                sigs.append(([kind] * n + [sh, kind], None))
                sigs.append(([kind] * n + [sh], sh))
        sigs.append(([I64] * 6 + [F64] * 8 + [sh, I64, F64], sh))
        sigs.append(([F64] * 8 + [I64] * 6 + [sh, F64, I64], I64))
        sigs.append(([I64] * 8 + [F64] * 8 + [sh, sh], None))
    # two structs in a row, then scalars (a struct that does not fit takes no
    # register; a scalar after it still does)
    for a, b in ((st(I64, I64), st(I64, I64)), (st(I64, I64), I64),
                 (st(F64, F64), st(F64, F64)), (st(I64, I64, I64), I64),
                 (st(F32, F32, F32, F32), st(F32, F32, F32, F32))):
        for n in (0, 3, 4, 5, 6, 7):
            sigs.append(([I64] * n + [a, b, I64, F64], None))
    # narrow integers and bools (extension) among wide ones, as results too
    for k in ("i8", "u8", "i16", "u16", "i32", "u32", "bool", "ptr", "optptr",
              "f32", "f64", "i64"):
        t = S(k)
        sigs.append(([t, t, t], t))
        sigs.append(([I64, t, I8, t, BOOL, t, F32, t], t))
        sigs.append(([t] * 12, t))
    return sigs


def used_structs(sigs):
    """The structs the signatures use, in creation (dependency) order."""
    seen = {}

    def walk(t):
        if isinstance(t, StructTy):
            if id(t) not in seen:
                for f in t.fields:
                    walk(f)
                seen[id(t)] = t
        elif isinstance(t, ArrayTy):
            walk(t.elem)

    for params, ret in sigs:
        for p in params:
            walk(p)
        if ret is not None:
            walk(ret)
    return seen


def c_helpers(c):
    """The C helpers both directions share (output through write(2))."""
    c.append("extern long write(int, const void *, unsigned long);")
    c.append("static void out_str(const char *s) {")
    c.append("  unsigned long n = 0;")
    c.append("  while (s[n]) n++;")
    c.append("  write(1, s, n);")
    c.append("}")
    c.append("static void out_hex(const void *p, unsigned long n) {")
    c.append("  char buf[40];")
    c.append("  const unsigned char *b = (const unsigned char *)p;")
    c.append("  for (unsigned long i = 0; i < n; i++) {")
    c.append("    buf[2 * i] = \"0123456789abcdef\"[b[i] >> 4];")
    c.append("    buf[2 * i + 1] = \"0123456789abcdef\"[b[i] & 15];")
    c.append("  }")
    c.append("  buf[2 * n] = '.';")
    c.append("  write(1, buf, 2 * n + 1);")
    c.append("}")
    c.append("static void out_num(unsigned long v) {")
    c.append("  char buf[24]; int i = 24;")
    c.append("  do { buf[--i] = '0' + v % 10; v /= 10; } while (v);")
    c.append("  write(1, buf + i, 24 - i);")
    c.append("}")
    c.append("void cabi_dump(const void *p, unsigned long n) { out_hex(p, n); }")
    c.append("void cabi_fline(int id) { out_str(\"f\"); out_num(id); out_str(\" \"); }")
    c.append("void cabi_aline(int id) { out_str(\"a\"); out_num(id); out_str(\"=\"); }")
    c.append("void cabi_rline(int id) { out_str(\"r\"); out_num(id); out_str(\"=\"); }")
    c.append("void cabi_nl(void) { out_str(\"\\n\"); }")


def reverse(args, g, sigs):
    """C calls Go++ (P04.18): see the module docstring."""
    c = ["/* generated by gen-cabi.py --direction reverse --seed %d --count %d */"
         % (args.seed, args.count)]
    c_helpers(c)
    for s in g.structs:
        body = "".join("  %s;\n" % f.decl_c("f%d" % i)
                       for i, f in enumerate(s.fields))
        c.append("struct %s {\n%s};" % (s.name, body))
    go = ["// generated by gen-cabi.py --direction reverse --seed %d --count %d"
          % (args.seed, args.count),
          "package main\n\nimport \"unsafe\"\n",
          "func p(v uintptr) *byte { return (*byte)(unsafe.Pointer(v)) }\n",
          "@Extern(\"C\")\nfunc cabi_dump(p *byte, n uint)\n",
          "func dump(p *byte, n uint) { cabi_dump(p, n) }\n",
          "@Extern(\"C\")\nfunc cabi_fline(id int32)\n",
          "@Extern(\"C\")\nfunc cabi_aline(id int32)\n",
          "@Extern(\"C\")\nfunc cabi_nl()\n"]
    for s in g.structs:
        fields = "\n".join("\tf%d %s" % (i, f.gopp)
                           for i, f in enumerate(s.fields))
        go.append("type %s struct {\n%s\n}\n" % (s.name, fields))
    calls = []
    expected = []
    for i, (params, ret) in enumerate(sigs):
        arg_vals = []
        for pr in params:
            arg_vals.append([(leaf_parts(l), g.value(leaf_parts(l)[2]))
                             for l in pr.leaves("x", "x")])
        ret_vals = []
        if ret is not None:
            ret_vals = [(leaf_parts(l), g.value(leaf_parts(l)[2]))
                        for l in ret.leaves("x", "x")]
        rty = ret.c if ret is not None else "void"
        ptypes = ", ".join(pr.c for pr in params) or "void"
        # The C caller: builds the arguments leaf by leaf, calls, prints.
        body = []
        for j, pr in enumerate(params):
            body.append("  %s a%d;" % (pr.c, j))
            body.append("  __builtin_memset(&a%d, 0, sizeof a%d);" % (j, j))
            for ((gp, cp, sc, role), v) in arg_vals[j]:
                body.append("  %s = %s;" % (cp.replace("x", "a%d" % j, 1),
                                            g.lit_c(sc, v)))
        argl = ", ".join("a%d" % j for j in range(len(params)))
        callee = "gopp_f%d" % i if i % 2 == 0 else "f"
        call = "%s(%s)" % (callee, argl)
        if ret is not None:
            body.append("  %s r = %s;" % (ret.c, call))
        else:
            body.append("  " + call + ";")
        body.append("  cabi_rline(%d);" % i)
        for ((gp, cp, sc, role), v) in ret_vals:
            path = cp.replace("x", "r", 1)
            body.append("  out_hex(&%s, sizeof(%s));" % (path, path))
        body.append("  cabi_nl();")
        if i % 2 == 0:
            c.append("extern %s gopp_f%d(%s);" % (rty, i, ptypes))
            c.append("void cabi_call%d(void) {\n%s\n}" % (i, "\n".join(body)))
        else:
            c.append("void cabi_cb%d(%s (*f)(%s)) {\n%s\n}" %
                     (i, rty, ptypes, "\n".join(body)))
        # The Go++ function: prints what it received, returns the values.
        pdecl = ", ".join("a%d %s" % (j, pr.gopp) for j, pr in enumerate(params))
        gbody = ["\tcabi_fline(%d)" % i]
        for j, pr in enumerate(params):
            gbody.append("\tcabi_aline(%d)" % j)
            for (gp, cp, sc, role) in map(leaf_parts,
                                          pr.leaves("a%d" % j, "a%d" % j)):
                gbody.append("\tdump((*byte)(unsafe.Pointer(&%s)), %d)" %
                             (gp, sc.size))
        gbody.append("\tcabi_nl()")
        if ret is not None:
            gbody.append("\treturn " + g.literal(ret, iter(ret_vals), ret_vals))
        export = "@Export(\"C\")\n" if i % 2 == 0 else ""
        go.append("%sfunc gopp_f%d(%s)%s {\n%s\n}\n" %
                  (export, i, pdecl, " " + ret.gopp if ret is not None else "",
                   "\n".join(gbody)))
        if i % 2 == 0:
            go.append("@Extern(\"C\")\nfunc cabi_call%d()\n" % i)
            calls.append("\tcabi_call%d()" % i)
        else:
            go.append("@Extern(\"C\")\nfunc cabi_cb%d(f func(%s)%s)\n" %
                      (i, pdecl, " " + ret.gopp if ret is not None else ""))
            calls.append("\tcabi_cb%d(gopp_f%d)" % (i, i))
        expected.append("f%d " % i)
        for j, vals in enumerate(arg_vals):
            expected.append("a%d=" % j)
            for ((gp, cp, sc, role), v) in vals:
                expected.append(g.encode(sc, v) + ".")
        expected.append("\n")
        expected.append("r%d=" % i)
        for ((gp, cp, sc, role), v) in ret_vals:
            expected.append(g.encode(sc, v) + ".")
        expected.append("\n")
    go.append("func main() {\n" + "\n".join(calls) + "\n}\n")
    open(os.path.join(args.dir, "lib.c"), "w").write("\n".join(c) + "\n")
    open(os.path.join(args.dir, "main.gopp"), "w").write("\n".join(go))
    open(os.path.join(args.dir, "expected.txt"), "w").write("".join(expected))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, required=True)
    ap.add_argument("--count", type=int, required=True)
    ap.add_argument("--dir", required=True)
    ap.add_argument("--mode", default="random", choices=["random", "edges"])
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--stride", type=int, default=1)
    ap.add_argument("--direction", default="forward",
                    choices=["forward", "reverse"])
    args = ap.parse_args()
    g = Gen(args.seed)
    if args.mode == "edges":
        sigs = edge_sigs(g)[args.start::args.stride][:args.count]
    else:
        sigs = [g.signature() for _ in range(args.count)]
    used = used_structs(sigs)
    g.structs = [t for t in g.structs if id(t) in used]
    os.makedirs(args.dir, exist_ok=True)
    if args.direction == "reverse":
        reverse(args, g, sigs)
        return

    # ---- C ----------------------------------------------------------------
    c = []
    c.append("/* generated by gen-cabi.py --seed %d --count %d */" %
             (args.seed, args.count))
    c.append("extern long write(int, const void *, unsigned long);")
    c.append("static void out_str(const char *s) {")
    c.append("  unsigned long n = 0;")
    c.append("  while (s[n]) n++;")
    c.append("  write(1, s, n);")
    c.append("}")
    c.append("static void out_hex(const void *p, unsigned long n) {")
    c.append("  char buf[40];")
    c.append("  const unsigned char *b = (const unsigned char *)p;")
    c.append("  for (unsigned long i = 0; i < n; i++) {")
    c.append("    buf[2 * i] = \"0123456789abcdef\"[b[i] >> 4];")
    c.append("    buf[2 * i + 1] = \"0123456789abcdef\"[b[i] & 15];")
    c.append("  }")
    c.append("  buf[2 * n] = '.';")
    c.append("  write(1, buf, 2 * n + 1);")
    c.append("}")
    c.append("static void out_num(unsigned long v) {")
    c.append("  char buf[24]; int i = 24;")
    c.append("  do { buf[--i] = '0' + v % 10; v /= 10; } while (v);")
    c.append("  write(1, buf + i, 24 - i);")
    c.append("}")
    c.append("void cabi_dump(const void *p, unsigned long n) { out_hex(p, n); }")
    c.append("void cabi_rline(int id) { out_str(\"r\"); out_num(id); out_str(\"=\"); }")
    c.append("void cabi_nl(void) { out_str(\"\\n\"); }")
    for s in g.structs:
        body = "".join("  %s;\n" % f.decl_c("f%d" % i)
                       for i, f in enumerate(s.fields))
        c.append("struct %s {\n%s};" % (s.name, body))

    go_calls = []
    expected = []
    go_decls = []
    for i, (params, ret) in enumerate(sigs):
        # arguments: values for every leaf
        arg_vals = []
        for p in params:
            arg_vals.append([(leaf_parts(l), g.value(leaf_parts(l)[2]))
                             for l in p.leaves("x", "x")])
        ret_vals = []
        if ret is not None:
            ret_vals = [(leaf_parts(l), g.value(leaf_parts(l)[2]))
                        for l in ret.leaves("x", "x")]

        # C function
        plist = ", ".join(p.decl_c("a%d" % j) for j, p in enumerate(params)) \
            or "void"
        rty = ret.c if ret is not None else "void"
        if isinstance(ret, ArrayTy):
            raise AssertionError
        c.append("%s cabi_f%d(%s) {" % (rty, i, plist))
        c.append("  out_str(\"f%d \");" % i)
        for j, p in enumerate(params):
            c.append("  out_str(\"a%d=\");" % j)
            for (gp, cp, sc, role) in map(leaf_parts, p.leaves("a%d" % j, "a%d" % j)):
                c.append("  out_hex(&%s, sizeof(%s));" % (cp, cp))
        c.append("  out_str(\"\\n\");")
        if ret is not None:
            c.append("  %s r;" % ret.c)
            c.append("  __builtin_memset(&r, 0, sizeof r);")
            for ((gp, cp, sc, role), v) in ret_vals:
                path = cp.replace("x", "r", 1)
                c.append("  %s = %s;" % (path, g.lit_c(sc, v)))
            c.append("  return r;")
        c.append("}")

        # Go++
        pdecl = ", ".join("a%d %s" % (j, p.gopp) for j, p in enumerate(params))
        go_decls.append("@Extern(\"C\")\nfunc cabi_f%d(%s)%s\n" %
                        (i, pdecl, " " + ret.gopp if ret is not None else ""))
        body = []
        for j, p in enumerate(params):
            body.append("\tvar a%d %s = %s" % (
                j, p.gopp, g.literal(p, iter(arg_vals[j]), arg_vals[j])))
        call = "cabi_f%d(%s)" % (i, ", ".join("a%d" % j for j in range(len(params))))
        if ret is not None:
            body.append("\tr := " + call)
            body.append("\t_ = r")
        else:
            body.append("\t" + call)
        body.append("\tcabi_rline(%d)" % i)
        if ret is not None:
            for ((gp, cp, sc, role), v) in ret_vals:
                path = gp.replace("x", "r", 1)
                body.append("\tdump((*byte)(unsafe.Pointer(&%s)), %d)" % (path, sc.size))
        body.append("\tcabi_nl()")
        go_calls.append("func call%d() {\n%s\n}\n" % (i, "\n".join(body)))

        # expected
        expected.append("f%d " % i)
        for j, vals in enumerate(arg_vals):
            expected.append("a%d=" % j)
            for ((gp, cp, sc, role), v) in vals:
                expected.append(g.encode(sc, v) + ".")
        expected.append("\n")
        expected.append("r%d=" % i)
        for ((gp, cp, sc, role), v) in ret_vals:
            expected.append(g.encode(sc, v) + ".")
        expected.append("\n")

    # ---- Go++ -------------------------------------------------------------
    go = []
    go.append("// generated by gen-cabi.py --seed %d --count %d" %
              (args.seed, args.count))
    go.append("package main\n\nimport \"unsafe\"\n")
    go.append("func p(v uintptr) *byte { return (*byte)(unsafe.Pointer(v)) }\n")
    go.append("@Extern(\"C\")\nfunc cabi_dump(p *byte, n uint)\n")
    go.append("func dump(p *byte, n uint) { cabi_dump(p, n) }\n")
    go.append("@Extern(\"C\")\nfunc cabi_rline(id int32)\n")
    go.append("@Extern(\"C\")\nfunc cabi_nl()\n")
    for s in g.structs:
        fields = "\n".join("\tf%d %s" % (i, f.gopp) for i, f in enumerate(s.fields))
        go.append("type %s struct {\n%s\n}\n" % (s.name, fields))
    go += go_decls
    go += go_calls
    go.append("func main() {\n" + "\n".join(
        "\tcall%d()" % i for i in range(len(sigs))) + "\n}\n")

    open(os.path.join(args.dir, "lib.c"), "w").write("\n".join(c) + "\n")
    open(os.path.join(args.dir, "main.gopp"), "w").write("\n".join(go))
    open(os.path.join(args.dir, "expected.txt"), "w").write("".join(expected))


if __name__ == "__main__":
    main()
