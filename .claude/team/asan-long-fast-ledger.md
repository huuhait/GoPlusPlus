# Findings ledger: asan-long-fast

Task: make ASan `check-long` fast (owner rule: no command over 60 s) by fixing the cost itself: no skipped tests, no lost coverage, no raised limits (owner, 2026-10-30). One Opus coder, reviewer loop.

Result: ASan check-long 83.5 s with 4 timing failures → 55.9 s quiet, 0 failures (78 s on a loaded host, still no failures); normal check-long 28.6 → 22.9 s; fmt normal tests no longer time out under load. Batch records byte-identical to single runs (337 programs × 7 settings, both targets, with and without -no-debug; 7,354 GOROOT files).

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | major | old wall limits became `timeout 240` hang guards (limits effectively raised) | none | FIXED: `tests/cpu-limit.py` (CPU quotient against `gopp version`, ceiling 3× alone; `driver/cpu-limit.test` both sides) | CONFIRMED-FIXED | 2 |
| R2 | minor | leftovers not recorded; wall timeout in normal-suite `check-chains.py`; SemaSymbolsTest 5 s ceiling | none | FIXED: wait4 CPU limits, thread CPU, TODO P03.27 (overloads quadratic), P03.28 (idle checker threads burn CPU) | CONFIRMED-FIXED | 2 |
| R3 | minor | a hang inside a batch waited up to 32 min | none | FIXED: 60 s + 5 s per run, `fmt/batch-hang.test` | CONFIRMED-FIXED | 2 |
| R4 | minor | `-batch` not in a design doc | none | FIXED: docs/design/test-runner.md | CONFIRMED-FIXED | 2 |
| R5 | minor | check-ir-flags scanned only indented lines | none | FIXED: every non-metadata line, self-test both sides | CONFIRMED-FIXED | 2 |

Also in this task (manager requests): fmt normal tests batched (no TIMEOUT at 40 busy loops); assertion crash in `(a + b).x` with a refused operator fixed (`gir/operator-refused-field.test`); opt matrices sharded 3 ways per target (`run/core-opt-matrix-shards.test` proves the union); CMake Dev flags now apply with an explicit `-DCMAKE_BUILD_TYPE=Dev`. Measured and rejected: an ASan build at -O1 (same wall time, 7% less CPU).
Round 2 (reviewer, Mode 2): CLEAN.
