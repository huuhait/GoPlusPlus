#!/usr/bin/env python3
"""Every test that was split in parts to stay under the time limit has all its
parts (the same idea as run/Inputs/check-matrix-shards.py, for the tests that
are cut by their RUN lines).

usage: check-split-tests.py TESTS_DIR GROUPS_FILE | --self-test

A part says `// SPLIT: NAME K/N` (K from 1). The parts of one NAME, in one
directory, must be N files, one per K: NAME.test (or NAME.long.test) for 1/N and
NAME-K.test (NAME-K.long.test) for the others, all saying the same N. GROUPS_FILE lists the expected groups
(`DIR/NAME N` a line, DIR relative to TESTS_DIR): a group whose markers were
deleted, or one nobody listed, is refused.
"""
import collections
import os
import re
import sys

SPLIT = re.compile(r"^// SPLIT: (\S+) (\d+)/(\d+)\s*$", re.M)


def file_name(name, k, long):
    return name + ("" if k == 1 else "-%d" % k) + (".long" if long else "") + ".test"


def check(root, expected=None):
    """-> (problems, groups)"""
    groups = collections.defaultdict(list)
    for d, subs, names in os.walk(root):
        subs[:] = sorted(s for s in subs if s not in ("Inputs", "Output"))
        for n in sorted(names):
            if not n.endswith(".test"):
                continue
            with open(os.path.join(d, n), encoding="utf-8", errors="replace") as f:
                text = f.read()
            for name, k, total in SPLIT.findall(text):
                groups[(d, name)].append((int(k), int(total), n))
    problems = []
    if expected is not None:
        found = {"%s/%s" % (os.path.relpath(d, root), name): {t for _, t, _ in v}
                 for (d, name), v in groups.items()}
        for g, n in sorted(expected.items()):
            if found.get(g) != {n}:
                problems.append("%s: expected %d parts with SPLIT markers, found %s"
                                % (g, n, sorted(found.get(g, ()))))
        for g in sorted(set(found) - set(expected)):
            problems.append("%s: has SPLIT markers but is not in the groups file" % g)
    for (d, name), parts in sorted(groups.items()):
        where = os.path.join(d, name)
        totals = {t for _, t, _ in parts}
        if len(totals) != 1:
            problems.append("%s: the parts disagree on how many there are: %s" % (where, sorted(totals)))
            continue
        total = totals.pop()
        ks = sorted(k for k, _, _ in parts)
        if ks != list(range(1, total + 1)):
            problems.append("%s: parts %s, expected 1..%d once each" % (where, ks, total))
            continue
        for k, _, n in parts:
            ok = {file_name(name, k, False), file_name(name, k, True)}
            if n not in ok:
                problems.append("%s: part %d is in %s, expected %s" % (where, k, n, sorted(ok)))
    return problems, groups


def self_test():
    import shutil
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        def put(n, text):
            with open(os.path.join(tmp, n), "w") as f:
                f.write(text)
        put("a.test", "// SPLIT: a 1/2\n")
        put("a-2.test", "// SPLIT: a 2/2\n")
        put("b.long.test", "// SPLIT: b 1/2\n")
        put("b-2.long.test", "// SPLIT: b 2/2\n")
        problems, groups = check(tmp)
        assert not problems and len(groups) == 2, problems
        os.remove(os.path.join(tmp, "a-2.test"))
        assert check(tmp)[0], "a missing part is not refused"
        put("a-2.test", "// SPLIT: a 2/2\n")
        put("a-3.test", "// SPLIT: a 2/2\n")
        assert check(tmp)[0], "a doubled part is not refused"
        os.remove(os.path.join(tmp, "a-3.test"))
        put("a-2.test", "// SPLIT: a 2/3\n")
        assert check(tmp)[0], "disagreeing totals are not refused"
        put("a-2.test", "// SPLIT: a 2/2\n")
        assert not check(tmp, {"./a": 2, "./b": 2})[0], "a listed group is refused"
        assert check(tmp, {"./a": 2, "./b": 2, "./c": 2})[0], "a group without markers passes"
        assert check(tmp, {"./a": 2})[0], "an unlisted group passes"
        shutil.rmtree(tmp, ignore_errors=True)
    print("self-test ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        sys.exit(0)
    expected = {}
    for line in open(sys.argv[2]):
        if line.strip():
            g, n = line.split()
            expected[g] = int(n)
    problems, groups = check(sys.argv[1], expected)
    for p in problems:
        print(p)
    if problems:
        sys.exit(1)
    print("%d split tests: every part once, %d parts" % (len(groups), sum(len(v) for v in groups.values())))
