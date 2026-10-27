# P06 Build driver and compile speed: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P06) · Design: [README.md](README.md)

## Tasks

- [ ] P06.01 Compile benchmark & calibration
- [ ] P06.02 Export data `.gopx`
- [ ] P06.03 Build graph & scheduler
- [ ] P06.04 Content-addressed cache
- [ ] P06.05 Pipelining
- [ ] P06.06 Generic instance homes
- [ ] P06.07 Parallel codegen
- [ ] P06.08 Dev pipeline tuning
- [ ] P06.09 `-trace`
- [ ] P06.10 CLI parity
- [ ] P06.11 Budget gate in CI
- [ ] P06.12 Verification model
- [ ] P06.13 LSP overlays

## Open questions

- [x] Q1 Per-function incremental compilation: decided, not in v1
- [x] Q2 Distributed or remote cache: decided, not in v1
- [ ] Q3 In-process LLD vs a subprocess

## Exit criteria

- [ ] M3: multi-package samples build; no-op rebuild is all cache hits
- [ ] Editing a function body that is neither generic nor exported for inlining doesn't rebuild dependents
- [ ] D6.8 budgets met (or ADR per gap)

## Decision changes

- None.

## Notes / blockers

- Start P06.01 (benchmark generator + `go build` calibration) and P06.09 (`-trace`) as early as the M0 slice. Every later phase is measured against them.
