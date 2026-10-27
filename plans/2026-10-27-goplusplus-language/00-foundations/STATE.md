# P00 Foundations: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[~] in_progress`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P00) · Design: [README.md](README.md)

## Tasks

- [x] P00.01 Initialize repository
- [x] P00.02 Nix dev shell
- [x] P00.03 Pin the LLVM major (ADR-0002)
- [x] P00.04 CMake skeleton
- [x] P00.05 `gopp version`
- [x] P00.06 Test harness
- [~] P00.07 Code style (passes locally; "passes in CI" waits for the first CI run)
- [~] P00.08 CI (workflow written and actionlint-clean; "all jobs green on main" waits for a push)
- [x] P00.09 ADR process
- [x] P00.10 License
- [x] P00.11 Naming record
- [x] P00.12 Host sysroot discovery
- [~] P00.13 Fast compiler build (budgets met locally; "header check runs in CI" waits for the first CI run)
- [x] P00.14 Fast test loop

## Open questions

- [x] Q1 Code hosting and CI: GitHub + GitHub Actions (D0.7)
- [x] Q2 License: Apache-2.0 WITH LLVM-exception (D0.10)
- [x] Q3 Name: Go++, tool and files `gopp` (D0.8)

## Exit criteria

- [x] Fresh clone → `nix develop` → configure + `ninja` → `gopp version` works
- [~] `ninja check` (lit + unit tests) passes locally and in CI (local: yes; CI: not run yet)
- [~] Debug+ASan/UBSan build of `gopp` passes CI (passes locally; CI not run yet)
- [~] Assertions-enabled CI job passes (passes locally; CI not run yet)
- [x] D0.13 dev-loop budgets measured and met

## Decision changes

- 2026-10-27 (ADR-0002): the compiler cache is sccache, not ccache (owner request). It is on automatically in both dev shells with a 100 GB cache in `~/.cache/sccache`, shared by all worktrees; CMake falls back to ccache, then none. CI uses sccache with a small cache. D0.3, D0.4 and D0.13 updated.
- 2026-10-27 (ADR-0002): LLVM 23 (23.1.0) is pinned, the newest stable major in the pinned nixpkgs (nixos-26.05, rev 718f670e: 21.1.8, 22.1.8 and 23.1.0 are all official releases).

## Notes / blockers

- Toolchain: LLVM/Clang/LLD 23.1.0 come from the flake. The assertions shell (`nix develop .#assertions`) is a Release LLVM with assertions on, built from source (~10 min the first time, then cached in the Nix store).
- Release builds link LLVM, Clang and LLD fully statically (no `libLLVM.so`); Dev builds link the shared libraries with mold. Configure stops with an error if a build folder's cached LLVM dirs belong to another shell; use a new build folder.
- Musl fallback (D0.12): not bundled in P00. With no host libc, `gopp host-toolchain` prints a clear error and exits 1.
- Suite-time gate (P00.14): CI has no `ci-x86-64` / `ci-arm64` baselines yet, so it only warns there. After the first green CI run, record them with `ninja check-suite-time-update` and `-DGOPP_SUITE_TIME_KEY=<key>`.
- Blocked on a push (not allowed without the owner): CI has never run, so P00.07, P00.08, P00.13 and the CI exit criteria stay `[~]`.
- Team-build ledger for this phase: `.claude/team/p00-foundations-ledger.md` (5 findings, all closed).
