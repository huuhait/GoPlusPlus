#!/usr/bin/env python3
"""The shards of the long opt matrices are the whole matrix, each build once.

usage: check-matrix-shards.py ROOT        (ROOT: tests/run)
       check-matrix-shards.py --self-test

1. The RUN lines of ROOT/*.long.test that call run-opt-matrix.py with
   --shard K/N, grouped by the rest of the line, have every K of 0..N-1
   exactly once, with one N.
2. For both targets, the `--list` builds of the N shards of the corpus put
   together equal the `--list` of the whole corpus, none twice.
"""
import collections
import glob
import os
import re
import subprocess
import sys

SHARD = re.compile(r" --shard (\d+)/(\d+)")


def groups_problems(lines):
    """[problem] for RUN lines (text) of run-opt-matrix.py with --shard."""
    groups = collections.defaultdict(list)
    for l in lines:
        m = SHARD.search(l)
        if m:
            groups[SHARD.sub("", l)].append((int(m.group(1)), int(m.group(2))))
    bad = []
    for rest, ks in groups.items():
        ns = {n for _, n in ks}
        got = sorted(k for k, _ in ks)
        if len(ns) != 1 or got != list(range(ns.pop())):
            bad.append("shards %s of: %s" % (sorted(ks), rest.strip()))
    return bad, len(groups)


def listing(script, root, target, shard=None):
    cmd = [sys.executable, script, "--gopp", "unused", "--root", root, "--list",
           "--target", target, "--sysroot", "unused"]
    if shard:
        cmd += ["--shard", shard]
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.splitlines()


def main():
    if sys.argv[1:] == ["--self-test"]:
        good = ["x --shard 0/2 y", "x --shard 1/2 y"]
        assert groups_problems(good) == ([], 1)
        assert groups_problems(good[:1])[0], "a missing shard passes"
        assert groups_problems(good + ["x --shard 1/2 y"])[0], "a shard twice passes"
        print("self-test ok")
        return 0
    root = sys.argv[1]
    script = os.path.join(root, "Inputs", "run-opt-matrix.py")
    lines = [l for f in sorted(glob.glob(os.path.join(root, "*.long.test")))
             for l in open(f) if "run-opt-matrix.py" in l and "RUN:" in l]
    bad, ngroups = groups_problems(lines)
    ns = {int(m.group(2)) for l in lines for m in [SHARD.search(l)] if m}
    for target in ("amd64", "arm64"):
        whole = listing(script, root, target)
        for n in ns:
            parts = [b for k in range(n) for b in listing(script, root, target, "%d/%d" % (k, n))]
            twice = [b for b, c in collections.Counter(parts).items() if c > 1]
            if twice or sorted(parts) != sorted(whole):
                bad.append("%s, %d shards: %d builds twice, %d missing" % (
                    target, n, len(twice), len(set(whole) - set(parts))))
    if bad:
        sys.exit("\n".join(bad))
    print("%d sharded matrices: every shard once; the shards of %s are the whole "
          "corpus on both targets (%d builds a target)" % (
              ngroups, ", ".join("%d" % n for n in sorted(ns)), len(whole)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
