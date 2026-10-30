"""Runs Go++ programs for the run tests: through the fast runner (P04.19,
D4.14) or as real binaries. Shared by run-golden.py, run-expect.py and
run-panics.py, and by tests/lit.cfg.py (which batches the programs of all the
tests of a directory before the tests start).

The fast way: `gopp test-run` compiles the program in one process and runs it
in an ORC JIT in an executor process; its result is one JSON line (the
protocol in compiler/include/gopp/TestRun/Protocol.h and
docs/design/test-runner.md). lit.cfg.py runs all the programs of a directory
in one `gopp test-run` and leaves the results as <key>.json files in
$GOPP_FASTRUN_DIR; a test finds its result there by the key (a hash of the
program's sources, the gopp binary and the target). With no result there (one
test run alone) it runs `gopp test-run` for its one program.

The real way: `gopp build` + the executable, as before. It is used when
  * the script gets --real (the test is about linking, or it needs the exit
    path of a real process), or
  * the environment says GOPP_TEST_REAL=1 (lit -Dreal_binaries=1), or
  * the program is for another target (--qemu).
"""
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile

PROTOCOL_VERSION = 1
TIMEOUT = 60


class Result:
    """What a program did, the same for both ways."""

    def __init__(self, outcome, code=0, signal=0, stdout=b"", stderr=b"",
                 message="", restarted=False):
        self.outcome = outcome  # exited signaled timeout compile-failed ...
        self.code = code
        self.signal = signal
        self.stdout = stdout
        self.stderr = stderr
        self.message = message
        self.restarted = restarted  # the executor process ended with it

    @property
    def returncode(self):
        """Like subprocess: the status, or minus the signal."""
        return -self.signal if self.outcome == "signaled" else self.code

    def failure(self):
        """A text if the program did not run to an end of its own."""
        if self.outcome in ("exited", "signaled"):
            return None
        return "%s: %s" % (self.outcome, self.message.strip()[:600])


def gopp():
    """The compiler: $GOPP (set by tests/lit.cfg.py), else gopp on PATH."""
    exe = os.environ.get("GOPP") or shutil.which("gopp")
    if not exe:
        sys.exit("gopp is not on PATH")
    return exe


def trace(mode):
    """GOPP_RUNLIB_TRACE=1: say on standard error how a program was run."""
    if os.environ.get("GOPP_RUNLIB_TRACE") == "1":
        sys.stderr.write("runlib: %s\n" % mode)


def want_real(flag=False):
    return flag or os.environ.get("GOPP_TEST_REAL") == "1"


def _file_digest(h, path):
    h.update(path.encode() + b"\0")
    with open(path, "rb") as f:
        h.update(hashlib.sha256(f.read()).digest())


def job_key(srcs):
    """The identity of a job: what is compiled and by which compiler."""
    h = hashlib.sha256()
    h.update(b"gopp-fastrun-%d\0" % PROTOCOL_VERSION)
    st = os.stat(gopp())
    h.update(b"%d:%d\0" % (st.st_size, st.st_mtime_ns))
    for s in srcs:
        if os.path.isdir(s):
            for name in sorted(os.listdir(s)):
                p = os.path.join(s, name)
                if os.path.isfile(p):
                    _file_digest(h, p)
        else:
            _file_digest(h, s)
    return h.hexdigest()


def _decode(line):
    d = json.loads(line)
    if d.get("v") != PROTOCOL_VERSION:
        raise ValueError("unknown protocol version in " + line[:80])
    return d["id"], Result(
        d["outcome"], d.get("exit", 0), d.get("signal", 0),
        base64.b64decode(d.get("stdout", "")),
        base64.b64decode(d.get("stderr", "")), d.get("message", ""),
        d.get("restarted", False))


def decode_results(text):
    """{id: Result} for the JSON lines `gopp test-run` wrote."""
    out = {}
    for line in text.splitlines():
        if line.strip():
            i, r = _decode(line)
            out[i] = r
    return out


def _run_fast_single(srcs, timeout):
    job = json.dumps({"v": PROTOCOL_VERSION, "id": "job", "inputs": list(srcs),
                      "timeout_ms": timeout * 1000})
    p = subprocess.run([gopp(), "test-run", "-manifest", "-"], input=job.encode(),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout + 60)
    if p.returncode != 0:
        sys.exit("gopp test-run failed: %s" % p.stderr.decode(errors="replace")[:600])
    return decode_results(p.stdout.decode())["job"]


def _run_real(srcs, link_flags, timeout, merge=False):
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "prog")
        b = subprocess.run([gopp(), "build"] + link_flags + list(srcs) + ["-o", exe],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout)
        if b.returncode != 0:
            return Result("compile-failed", message=b.stderr.decode(errors="replace"))
        return run_exe(exe, [], timeout, merge)


def run_exe(exe, prefix, timeout=TIMEOUT, merge=False):
    """Runs a built program (prefix: e.g. qemu-aarch64 -L SYSROOT). merge:
    standard error goes where standard output goes (2>&1), as with the
    runner's merge_output."""
    try:
        r = subprocess.run(prefix + [exe], stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT if merge else subprocess.PIPE,
                           timeout=timeout)
    except subprocess.TimeoutExpired:
        return Result("timeout", message="no end in %d s" % timeout)
    err = r.stderr or b""
    if r.returncode < 0:
        return Result("signaled", signal=-r.returncode, stdout=r.stdout, stderr=err)
    return Result("exited", code=r.returncode, stdout=r.stdout, stderr=err)


def run_sources(srcs, real=False, timeout=TIMEOUT):
    """Compiles and runs the program made of srcs (files, or one directory)."""
    if want_real(real):
        trace("real binary")
        flags = os.environ.get("GOPP_LINK_FLAGS", "").split()
        return _run_real(srcs, flags, timeout)
    d = os.environ.get("GOPP_FASTRUN_DIR")
    if d:
        path = os.path.join(d, job_key(srcs) + ".json")
        try:
            with open(path) as f:
                res = _decode(f.readline())[1]
            trace("fast, from the batch")
            return res
        except FileNotFoundError:
            pass
    trace("fast, alone")
    return _run_fast_single(srcs, timeout)


def split_args(argv):
    """Options shared by the run scripts: --src P (repeated), --real,
    --qemu SYSROOT. Returns (opts, positional)."""
    opts = {"src": [], "real": False, "qemu": None, "names": None}
    pos = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--src":
            opts["src"].append(argv[i + 1])
            i += 2
        elif a == "--real":
            opts["real"] = True
            i += 1
        elif a == "--qemu":
            opts["qemu"] = argv[i + 1]
            i += 2
        elif a == "--names":
            opts["names"] = argv[i + 1].split(",")
            i += 2
        else:
            pos.append(a)
            i += 1
    return opts, pos


def panic_programs(d, names):
    """The programs of a panics directory (all, or the named ones)."""
    if names is None:
        names = sorted(f[:-5] for f in os.listdir(d) if f.endswith(".gopp"))
    return [(n, os.path.join(d, n + ".gopp")) for n in names]


def plan(script, argv):
    """The source lists a run script would run, for the batch of lit.cfg.py.
    Scripts that are not run scripts, runs that must be real, and runs for
    another target give none."""
    base = os.path.basename(script)
    if base not in ("run-golden.py", "run-expect.py", "run-panics.py",
                    "run-output.py"):
        return []
    if "--" in argv:  # the old form: a build command, a real binary
        return []
    opts, pos = split_args(argv)
    if opts["real"] or opts["qemu"]:
        return []
    if base == "run-panics.py":
        return [[p] for _, p in panic_programs(pos[0], opts["names"])]
    return [opts["src"]] if opts["src"] else []
