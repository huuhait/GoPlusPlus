# Findings ledger: test-speed-2

Task: normal tests near the 2.0 s limit made `check-suite-time` fail at random (lit -j 192 on 192 threads). One coder, reviewer loop.
Result: lit's default jobs is half the cores (D0.13 updated; -DGOPP_LIT_JOBS overrides): at -j 96 the suite is faster (1.7 s wall vs 2.1-2.4 s) and the slowest test ~1 s; ~40 tests split (SPLIT markers, tests/driver/split-tests.test + split-groups.txt prove completeness); fmt goldens batched; the runner-cleanup-int race fixed; baseline 150.9 s at -j 96.

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | minor | batched fmt golden check ignored the exit status | .scratch/golden-exit.sh | FIXED (tests/fmt/golden-exit.test) | CONFIRMED-FIXED | 2 |
| R2 | minor | TODO P00.14 stale; dead CHECK copies in split parts; split markers deletable | none | FIXED | CONFIRMED-FIXED | 2 |
| R3 | minor | one dead CORPUS-GOOD line left in fmt/batch-fallback.test | grep | FIXED (manager) | | 3 |

Observation O1 (not fixed here): fmt/roundtrip-sema and fmt/batch-fallback-2 timed out at exactly ~2.02 s in 2 of 13 full `check` runs under outside load from other agents, 0 of 65 alone. Suspect: a machine-wide wait (e.g. the shared process-slot pool in $XDG_RUNTIME_DIR used by every worktree) rather than test cost; to watch.
Round 3 (manager): R3 removed, CLEAN.
