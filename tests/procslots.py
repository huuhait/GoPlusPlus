"""A bound on the processes the test scripts start side by side, shared by all
the tests of a lit run and by the user's other test runs (tests/lit.cfg.py).

Most of the kernel time of a test run goes into starting and ending processes:
an ASan `gopp` pays its page faults and mappings (sanitizer runtime, shared
LLVM libraries) before main and after exit, also for `gopp version`. Past
about a third of the cores' worth of such processes at once, the kernel's
memory paths contend and the cost per process grows several times (measured on
a 192-thread host, ASan `gopp fmt`: 16 at a time 15 ms of kernel time each, 64
at a time 28 ms, 128 at a time 95 ms, and the wall time grows too). A script
that runs one process per file or per program, or per batch of them
(tests/gopp_batch.py), in many threads (check-corpus.py, run-opt-matrix.py,
...) takes a slot around each process;
with the slots of all the tests together the machine stays below that point
however many such tests run at once.

tests/lit.cfg.py sets GOPP_TEST_SLOTS (the number) and GOPP_TEST_SLOTS_DIR (a
directory of lock files, one per slot). Without them (a script run by hand)
slot() does nothing. A slot is an flock on its file: a test that is killed
gives its slots back.
"""
import contextlib
import fcntl
import os
import random


def _open(d, k):
    return os.open(os.path.join(d, str(k)), os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o644)


@contextlib.contextmanager
def slot():
    """Holds one slot while the body runs (one process, start to end)."""
    d = os.environ.get("GOPP_TEST_SLOTS_DIR")
    n = int(os.environ.get("GOPP_TEST_SLOTS") or 0)
    if not d or n <= 0:
        yield
        return
    os.makedirs(d, exist_ok=True)
    first = random.randrange(n)
    fd = -1
    # A free slot if there is one; else wait for one (picked at random, so the
    # waiters spread over the slots).
    for i in range(n):
        f = _open(d, (first + i) % n)
        try:
            fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            os.close(f)
            continue
        fd = f
        break
    if fd < 0:
        fd = _open(d, first)
        fcntl.flock(fd, fcntl.LOCK_EX)
    try:
        yield
    finally:
        os.close(fd)  # the lock goes with the descriptor
