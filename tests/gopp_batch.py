"""Many runs of `gopp` in one process: `gopp parse -batch`, `gopp fmt -batch`
and `gopp build -batch` (the test scripts that start a process per file or per
program: tests/fmt/check-corpus.py, compare-gofmt.py,
tests/run/Inputs/run-opt-matrix.py, ...).

Most of the time of those scripts in a sanitizer build is starting and ending
`gopp` processes (tests/procslots.py), not their work; a batch pays that once
for many runs. Each run of a batch is the run the script made before (a file
read as standard input, one build), with its own exit status, standard output
and standard error, in a record (gopp.cpp writeBatchRecord):

  gopp-batch <exit status> <stdout bytes> <stderr bytes> <name>\\n<stdout><stderr>

A batch is trusted only when it is clean: every record there, in order, and
the process exited with status 0 and wrote nothing else to standard error.
Otherwise (a crash, a sanitizer report, a fatal error) every run of the batch
is made again in a process of its own, the way the scripts made it before, so
the result of each, and the name of the one that failed, are exactly what they
were without batches.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import procslots  # noqa: E402

# The runs per batch. On a shard of the standard library corpus in the ASan
# build, 16 and 32 took the same wall time and 8 more; 32 starts the fewest
# processes.
CHUNK = 32
# The fewest runs per batch: a normal test's corpus of 189 small files took 6.7
# s of CPU in batches of one or two (one per worker) and 0.45 s in batches of
# 16; the wall time was lower too.
MIN_CHUNK = 16


class Result:
    """What a run gave: like subprocess.CompletedProcess."""

    def __init__(self, returncode, stdout, stderr):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def chunks(items, jobs, size=CHUNK):
    """items in batches of at most `size`, but small enough that `jobs`
    workers all get one, and never under MIN_CHUNK: a batch of a few small
    files costs little more than one, and every batch is a process start."""
    n = max(min(MIN_CHUNK, size), min(size, -(-len(items) // max(1, jobs))))
    return [items[i:i + n] for i in range(0, len(items), n)]


def parse_records(data):
    """[(name, Result)] of the records in data, in order, and whether data is
    nothing but records."""
    out = []
    at = 0
    while at < len(data):
        nl = data.find(b"\n", at)
        if nl < 0:
            break
        head = data[at:nl].split(b" ", 4)
        if (len(head) != 5 or head[0] != b"gopp-batch"
                or not all(h.isdigit() for h in head[1:4])):
            break
        status, nout, nerr = int(head[1]), int(head[2]), int(head[3])
        body = nl + 1
        if body + nout + nerr > len(data):
            break
        out.append((head[4].decode("utf-8", "surrogateescape"),
                    Result(status, data[body:body + nout],
                           data[body + nout:body + nout + nerr])))
        at = body + nout + nerr
    return out, at == len(data)


def batch_timeout(runs):
    """How long a batch of `runs` may take before it counts as hung: 60 s and
    5 s a run (GOPP_BATCH_TIMEOUT, TEST ONLY, replaces the 60 and scales the
    5), so a hang falls back about as fast as one run's limit did."""
    base = float(os.environ.get("GOPP_BATCH_TIMEOUT") or 60)
    return base + base / 12 * runs


def run(cmd, names, single, stdin=None):
    """Runs the batch command cmd, whose runs are named `names` in order, and
    returns {name: Result}. single(name) -> Result makes one run alone (the
    old way); it is used for every run of a batch that is not clean (also one
    that takes longer than batch_timeout: a hang)."""
    timeout = batch_timeout(len(names))
    try:
        with procslots.slot():
            p = subprocess.run(cmd, input=stdin, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return {n: single(n) for n in names}
    recs, whole = parse_records(p.stdout)
    if (p.returncode == 0 and not p.stderr and whole
            and [n for n, _ in recs] == list(names)):
        return dict(recs)
    return {n: single(n) for n in names}


def main(argv):
    """`gopp_batch.py show [NAME]` < RECORDS: one line per record (name, exit
    status, sizes), or the standard error of the record NAME as it is."""
    recs, whole = parse_records(sys.stdin.buffer.read())
    if len(argv) > 1:
        sys.stdout.buffer.write(dict(recs)[argv[1]].stderr)
        return 0
    for name, r in recs:
        print("%s: exit %d, %d bytes out, %d bytes err" % (name, r.returncode, len(r.stdout), len(r.stderr)))
    return 0 if whole else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
