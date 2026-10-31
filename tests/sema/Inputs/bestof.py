"""The ratio of two CPU costs of the near-linear scripts
(check-units-linear.py, ../../gir/Inputs/check-gir-linear.py), as the least of
several runs of each side (the same idea as check-linear.py's rounds)."""

EXTRA_ROUNDS = 8


def best_of_both(once, small, big, base, limit, runs=4):
    """The costs over base of small and big: the least of `runs` runs of
    small, and of big, taken in turn (a machine that gets busier or quieter
    during the test changes both). Big runs only while the pair is over the
    limit, `runs` times at most: a busy machine only adds time, so more runs
    could only lower its cost. The small side, which a quadratic walk needs to
    be too slow to pass, always gets all its runs.

    A pair still over the limit gets up to EXTRA_ROUNDS more rounds of both
    sides: at heavy load every big run can be slowed (memory and cache
    contention) while a small one is not, and four samples were not enough
    (core-linear-2 fanin 0.30 s -> 2.54 s, 8.5x against 8, then passing alone).
    A quadratic shape stays over the limit however many rounds it gets (the
    control tests/sema/linear-control.long.test)."""
    a = b = 1e9
    nb = 0
    for _ in range(runs):
        a = min(a, once(small) - base)
        if nb == 0 or b > limit * a:
            b = min(b, once(big) - base)
            nb += 1
    while nb < runs and b > limit * a:
        b = min(b, once(big) - base)
        nb += 1
    for _ in range(EXTRA_ROUNDS):
        if b <= limit * a:
            break
        a = min(a, once(small) - base)
        b = min(b, once(big) - base)
    return a, b
