#!/usr/bin/env python3
"""The fast runner's cost grows linearly with the number of programs (P04.19).

Runs every 4th program of the run corpus through one `gopp test-run` with the
selection 1, 4 and 16 times over and compares the CPU time (children of this process) per extra
program on the two steps: linear growth gives the same number twice, quadratic
growth gives four times more on the second step. Each size is measured twice,
the sizes in turn, and the smaller time counts (load from other work only
adds time). Timing, so a long test.

The control goes through the same measurement and the same check first: a
child process whose work is quadratic in its size (measured, not made up) must
be refused, so a pass of the runner means something.
"""
import os
import resource
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util  # noqa: E402

spec = importlib.util.spec_from_file_location(
    "compare_runner", os.path.join(os.path.dirname(os.path.abspath(__file__)), "compare-runner.py"))
cr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cr)

LIMIT = 2.0  # noise margin: linear gives 1, quadratic gives 4
SIZES = (1, 4, 16)


def growth(points):
    """points: [(size, cpu seconds)] x3. The marginal cost per unit of the
    second step divided by the first's."""
    (n1, c1), (n2, c2), (n3, c3) = points
    return ((c3 - c2) / (n3 - n2)) / ((c2 - c1) / (n2 - n1))


def measure(run_one):
    """[(size, best CPU seconds of two)] for SIZES, the sizes in turn."""
    best = {}
    for _ in range(2):
        for n in SIZES:
            cpu = run_one(n)
            best[n] = min(best.get(n, cpu), cpu)
    return [(n, best[n]) for n in SIZES]


def children_cpu():
    r = resource.getrusage(resource.RUSAGE_CHILDREN)
    return r.ru_utime + r.ru_stime


QUADRATIC = """
import sys
n = int(sys.argv[1]) * 250
s = 0
for i in range(n):
    for j in range(n):
        s += i ^ j
"""


def quadratic_child(n):
    c0 = children_cpu()
    subprocess.run([sys.executable, "-c", QUADRATIC, str(n)], check=True, timeout=60)
    return children_cpu() - c0


# The control: a quadratic workload, measured the same way, is refused.
g = growth(measure(quadratic_child))
print("control: a quadratic child grows by %.2f (must be over %.1f)" % (g, LIMIT))
if g <= LIMIT:
    sys.exit("the check does not see a quadratic workload: it proves nothing")

# Every STRIDE-th program of the corpus: the check is about how the cost grows
# with the NUMBER of programs, not about which ones (compare-runner.py runs
# them all), and the 21 corpus-sizes of the two rounds took 800 s of CPU in the
# ASan build with the whole corpus.
STRIDE = 4
jobs = cr.corpus()[::STRIDE]


def corpus_copies(copies):
    _, wall, cpu = cr.run_fast(jobs, copies)
    print("%d programs: %.2f s cpu, %.2f s wall" % (copies * len(jobs), cpu, wall))
    return cpu


points = [(n * len(jobs), cpu) for n, cpu in measure(corpus_copies)]
g = growth(points)
print("cost per program grows by %.2f from the first step to the second (limit %.1f)" % (g, LIMIT))
if g > LIMIT:
    sys.exit("the runner is not linear in the number of programs")
