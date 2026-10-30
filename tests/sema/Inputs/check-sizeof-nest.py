#!/usr/bin/env python3
"""P03.26: unsafe.Sizeof / Alignof / Offsetof of types nested by value.

usage: check-sizeof-nest.py <gopp> <work dir> <suite> [go]

For each shape and depth of gen-sizeof-nest.py the numbers come from a model
of the gc layout (written here, not shared with the compiler), and `gopp
check --go` must accept the generated checks `[1]byte{}[Sizeof(T) - want]`
(an index out of range for any other value). With `go` the model is first
compared with `go run` (the real gc answers) at the depths Go can still
compile: the suite `go` below. Suites:

  go      70, 200 and 1000 levels of every shape, model against `go run`
  gopp    gopp against the model: the struct shape at 70 and 1000 levels
          (`all`: every shape at every depth)
  deep-<shape>,..  10000 levels (30 for dag, which doubles at every level)
  rev-<shape>,..   10000 levels, declared from the top down
  huge-<shape>,..  100000 levels
  wrong    a wrong number is an error (so the checks above are real)
"""
import os
import subprocess
import sys

gopp, work, suite = sys.argv[1], sys.argv[2], sys.argv[3]
use_go = len(sys.argv) > 4 and sys.argv[4] == "go"
here = os.path.dirname(os.path.abspath(__file__))
gen = os.path.join(here, "gen-sizeof-nest.py")
os.makedirs(work, exist_ok=True)
failed = []


def align(x, a):
    return (x + a - 1) // a * a


def struct(fields):
    """fields: list of (size, align); returns size, align, offsets."""
    off, offs, al = 0, [], 1
    for sz, a in fields:
        off = align(off, a)
        offs.append(off)
        off += sz
        al = max(al, a)
    last = fields[-1][0] if fields else 0
    # the last field of a non-zero-sized struct is not allowed to have size 0
    if offs and offs[-1] > 0 and last == 0:
        off += 1
    return align(off, al), al, offs


def model(shape, depth):
    """The numbers gen-sizeof-nest.py prints: Sizeof(top), Alignof(top),
    [Offsetof(top.f)], Sizeof(the type 3 levels below the top)."""
    low = max(depth - 3, 0)
    sizes = {}
    offs_top = None
    if shape == "struct":
        cur = (1, 1)  # S0 { a int8 }
        sizes[0] = cur
        for i in range(1, depth + 1):
            f = [(1, 1), cur, (2, 2)] + ([(8, 8)] if i % 7 == 0 else [])
            sz, al, offs = struct(f)
            cur = (sz, al)
            sizes[i] = cur
            offs_top = offs[1]
    elif shape == "array":
        b = (1, 1)  # B0 { x int8 }
        sizes[0] = b
        for i in range(1, depth + 1):
            a = b  # A_i [1]B_{i-1}
            sz, al, offs = struct([(1, 1), a, (4, 4)])
            b = (sz, al)
            sizes[i] = b
            offs_top = offs[1]
    elif shape == "generic":
        cur = (4, 4)  # E0 int32
        sizes[0] = cur
        for i in range(1, depth + 1):
            sz, al, _ = struct([(1, 1), cur, (2, 2)])
            cur = (sz, al)
            sizes[i] = cur
        offs_top = None
    elif shape == "dag":
        cur = (1, 1)
        sizes[0] = cur
        for i in range(1, depth + 1):
            sz, al, offs = struct([cur, cur])
            cur = (sz, al)
            sizes[i] = cur
            offs_top = offs[1]
    elif shape == "wide":
        cur = (1, 1)
        sizes[0] = cur
        for i in range(1, depth + 1):
            sz, al, offs = struct([cur, (8, 8)])
            cur = (sz, al)
            sizes[i] = cur
            offs_top = offs[1]
    top = sizes[depth]
    out = [top[0], top[1]]
    if shape in ("struct", "array", "dag", "wide"):
        out.append(offs_top)
    out.append(sizes[low][0])
    return out


def gen_program(shape, depth, mode, want=None, rev=False):
    cmd = [sys.executable, gen, shape, str(depth), mode]
    if want:
        cmd.append(want)
    if rev:
        cmd.append("rev")
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def case(shape, depth, rev=False, wrong=False):
    nums = model(shape, depth)
    if wrong:
        nums[0] += 1  # Sizeof: the program must not be accepted
    tag = "%s depth=%d%s%s" % (shape, depth, " rev" if rev else "",
                               " wrong" if wrong else "")
    d = os.path.join(work, "%s-%d%s%s" % (shape, depth, "-rev" if rev else "",
                                         "-wrong" if wrong else ""))
    os.makedirs(d, exist_ok=True)
    if use_go:
        if shape == "dag":
            # gc does not keep the size of a type it has worked out: 2^depth
            # steps. 14 levels are the most it does in a blink.
            depth = min(depth, 14)
            nums = model(shape, depth)
            tag = "%s depth=%d" % (shape, depth)
        with open(os.path.join(d, "main.go"), "w") as f:
            f.write(gen_program(shape, depth, "go"))
        if not os.path.exists(os.path.join(d, "go.mod")):
            with open(os.path.join(d, "go.mod"), "w") as f:
                f.write("module sz\n\ngo 1.25\n")
        r = subprocess.run(["timeout", "60", "go", "run", "."], cwd=d, capture_output=True, text=True)
        got = [int(x) for x in r.stderr.split()]
        if r.returncode != 0 or got != nums:
            failed.append("%s: go says %s, the model %s (%s)" % (tag, got, nums, r.stderr[:200]))
        return
    wantf = os.path.join(d, "want")
    with open(wantf, "w") as f:
        f.write("\n".join(str(n) for n in nums))
    path = os.path.join(d, "main.go")
    with open(path, "w") as f:
        f.write(gen_program(shape, depth, "gopp", wantf, rev))
    r = subprocess.run(["timeout", "60", gopp, "check", "--go", "--check-error-limit=0", "-check-j=1", path],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    if wrong:
        if r.returncode in (0, 124) or r.returncode < 0 or "error[" not in out or "Sanitizer" in out:
            failed.append("%s: a wrong size was not refused: %d\n%s" % (tag, r.returncode, out[:500]))
    elif r.returncode != 0 or "error[" in out or "Sanitizer" in out:
        failed.append("%s (wants %s): %d\n%s" % (tag, nums, r.returncode, out[:500]))


SHAPES = ["struct", "array", "generic", "dag", "wide"]
if suite == "gopp":
    for shape, depth in (("struct", 70), ("struct", 1000)):
        case(shape, depth)
elif suite in ("go", "all"):
    for shape in SHAPES:
        for depth in (70, 200, 1000):
            if shape == "dag":
                depth = min(depth, 30)
            case(shape, depth)
elif suite.startswith("deep-"):
    for shape in suite[5:].split(","):
        case(shape, 30 if shape == "dag" else 10000)
elif suite.startswith("rev-"):
    for shape in suite[4:].split(","):
        case(shape, 10000, rev=True)
elif suite == "wrong":
    case("generic", 70, wrong=True)
elif suite.startswith("huge-"):
    for shape in suite[5:].split(","):
        case(shape, 100000)
else:
    sys.exit("unknown suite " + suite)
if failed:
    print("\n".join(failed))
    sys.exit(1)
