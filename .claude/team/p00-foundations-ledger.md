# Findings ledger: p00-foundations

Task: P00 Foundations (P00.01–P00.14) + owner sccache (100G, auto). 3 coders → PM merge (base c3).
Branch: `task/p00-foundations-merged`

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | blocker | flake.nix:68-79 + GoppLLVM.cmake:44-52: Dev build in `.#assertions` shell → gopp RUNPATH picks non-assertions libclang-cpp.so → `symbol lookup error`, 4/7 driver lit tests fail; CLAUDE.md documents that workflow | .scratch/assert-dev.sh | FIXED | CONFIRMED-FIXED | 1→2 |
| R2 | minor | GoppBuildOptions.cmake:36-42 launcher set as normal var so CMakeCache shows env `ccache` while build.ninja uses sccache; CLAUDE.md:17 "share one ccache" should say sccache | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R3 | minor (report only) | GoppBuildOptions.cmake launcher CACHE FORCE also overrides an explicit -DCMAKE_CXX_COMPILER_LAUNCHER=; no opt-out (fits owner "automatic" requirement) | none | — | WITHDRAWN (reviewer: not blocking) | 2 |
| R4 | major | CLAUDE.md:49 `tools/check-suite-time.py --update` fails (requires --baseline, lit path, test dirs); no ninja target; CI warning text same gap | .scratch/r4.sh | FIXED | CONFIRMED-FIXED | 3→4 |
| R5 | major | GoppLLVM.cmake:4-6 stale LLVM_DIR/Clang_DIR/LLD_DIR in CMakeCache silently wins over the shell's env after a shell switch or flake bump (assertions shell builds against non-assertions LLVM) | .scratch/r5.sh | FIXED | CONFIRMED-FIXED | 3→4 |

Notes (below the merge bar):
- `check-suite-time-update` writes the baseline even if a normal test exceeds 2 s (reviewer round 4).
- `.scratch/r5.sh` is stale (expects "assertions: ON"; behaviour is now a FATAL_ERROR).
