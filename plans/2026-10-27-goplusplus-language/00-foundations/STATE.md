# P00 Foundations: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P00) · Design: [README.md](README.md)

## Tasks

- [ ] P00.01 Initialize repository
- [ ] P00.02 Nix dev shell
- [ ] P00.03 Pin the LLVM major (ADR-0002)
- [ ] P00.04 CMake skeleton
- [ ] P00.05 `gopp version`
- [ ] P00.06 Test harness
- [ ] P00.07 Code style
- [ ] P00.08 CI
- [ ] P00.09 ADR process
- [ ] P00.10 License
- [ ] P00.11 Naming record
- [ ] P00.12 Host sysroot discovery

## Open questions

- [x] Q1 Code hosting and CI: GitHub + GitHub Actions (D0.7)
- [x] Q2 License: Apache-2.0 WITH LLVM-exception (D0.10)
- [x] Q3 Name: Go++, tool and files `gopp` (D0.8)

## Exit criteria

- [ ] Fresh clone → `nix develop` → configure + `ninja` → `gopp version` works
- [ ] `ninja check` (lit + unit tests) passes locally and in CI
- [ ] Debug+ASan/UBSan build of `gopp` passes CI
- [ ] Assertions-enabled CI job passes

## Decision changes

- None.

## Notes / blockers

- LLVM/Clang development libraries and LLD are not installed on the dev machine (only the clang 21.1.8 wrapper is). P00.02 must provide them, plus the optional assertions-enabled shell (D0.4).
- The dev machine runs NixOS, so M0 needs host sysroot discovery (D0.12, P00.12). The nixpkgs LLVM has RTTI on and no assertions (D0.3, D0.4).
