#!/usr/bin/env python3
"""Regenerate the golden outputs of the run tests from Go (needs Go; the
tests themselves do not).

usage: regen-goldens.py TEST.test [TEST.test ...]     (from tests/run)

Each test is a program that is valid Go and Go++. Its golden is what
`go run` prints to standard error (Go's println), with the stack trace of a
panic and the `exit status` line cut off, in golden/NAME.out. The same
program built for linux/arm64 and run under qemu-aarch64 gives
golden/NAME.arm64.out when its output differs (FMA contraction, float to
integer conversions). A test whose first line says `// GOLDEN: by-hand` is
skipped: its output is the Go++ rule where Go has none (D4.12). A test of
several files may name other orders of them on the command line with lines
`// ORDER: SUFFIX FILE...`, each giving golden/NAME-SUFFIX.out.
Run it inside `nix develop` (Go and QEMU come from the flake).
"""
import os
import shutil
import subprocess
import sys
import tempfile

here = os.path.dirname(os.path.abspath(__file__))
golden_dir = os.path.join(here, "..", "golden")


def clean(err: bytes) -> bytes:
    out = []
    for line in err.split(b"\n"):
        if line.startswith(b"goroutine ") or line.startswith(b"exit status"):
            break
        out.append(line)
    text = b"\n".join(out)
    # The blank line before "goroutine 1 [running]:" belongs to the trace.
    return text.rstrip(b"\n") + b"\n" if text.strip() else b""


def run(cmd, env=None, cwd=None):
    r = subprocess.run(cmd, env=env, cwd=cwd, stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    return r


for path in sys.argv[1:]:
    name = os.path.basename(path)[: -len(".test")]
    src = open(path, "rb").read()
    srcdir = os.path.join(here, name)  # a test of several files
    if b"// GOLDEN: by-hand" in src[:600]:
        print("skip", name)
        continue
    with tempfile.TemporaryDirectory() as d:
        if os.path.isdir(srcdir):
            for f in sorted(os.listdir(srcdir)):
                if f.endswith(".gopp"):
                    shutil.copy(os.path.join(srcdir, f),
                                os.path.join(d, f[: -len(".gopp")] + ".go"))
            main = "*.go"
            files = sorted(f for f in os.listdir(d) if f.endswith(".go"))
        else:
            main = os.path.join(d, "main.go")
            open(main, "wb").write(src)
            files = [main]
        env = dict(os.environ, GOFLAGS="-mod=mod", GO111MODULE="off", CGO_ENABLED="0")
        r = run(["go", "build", "-o", os.path.join(d, "amd64")] + files, env=env, cwd=d)
        if r.returncode:
            sys.exit("%s: go build failed:\n%s" % (name, r.stderr.decode()))
        r = run([os.path.join(d, "amd64")])
        amd64 = clean(r.stderr) + r.stdout
        open(os.path.join(golden_dir, name + ".out"), "wb").write(amd64)
        arm = None
        if shutil.which("qemu-aarch64"):
            env2 = dict(env, GOARCH="arm64")
            r = run(["go", "build", "-o", os.path.join(d, "arm64")] + files, env=env2, cwd=d)
            if r.returncode:
                sys.exit("%s: go build (arm64) failed:\n%s" % (name, r.stderr.decode()))
            r = run(["qemu-aarch64", os.path.join(d, "arm64")])
            arm = clean(r.stderr) + r.stdout
        alt = os.path.join(golden_dir, name + ".arm64.out")
        if arm is not None and arm != amd64:
            open(alt, "wb").write(arm)
            print(name, "(arm64 differs)")
        else:
            if os.path.exists(alt):
                os.remove(alt)
            print(name)
        # The files of a test of several files in another order on the
        # command line (the go command keeps that order, and the init order
        # follows it): a line `// ORDER: SUFFIX FILE...` of the test gives
        # golden/NAME-SUFFIX.out.
        for line in src.decode().split("\n"):
            if not line.startswith("// ORDER: "):
                continue
            suffix, *order = line[len("// ORDER: "):].split()
            given = [f[: -len(".gopp")] + ".go" for f in order]
            r = run(["go", "build", "-o", os.path.join(d, "ordered")] + given,
                    env=env, cwd=d)
            if r.returncode:
                sys.exit("%s: go build failed:\n%s" % (name, r.stderr.decode()))
            r = run([os.path.join(d, "ordered")])
            open(os.path.join(golden_dir, name + "-" + suffix + ".out"),
                 "wb").write(clean(r.stderr) + r.stdout)
            print(name + "-" + suffix)
