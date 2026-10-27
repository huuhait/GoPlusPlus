# P03 Semantic analysis: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P03) · Design: [README.md](README.md)

## Tasks

- [ ] P03.01 Package loader
- [ ] P03.02 Constant arithmetic
- [ ] P03.03 Scopes & resolution
- [ ] P03.04 Types core
- [ ] P03.05 Statements & expressions
- [ ] P03.06 Generics
- [ ] P03.07 Type-level ownership
- [ ] P03.08 `!` typing
- [ ] P03.09 Foreign scope interface
- [ ] P03.10 Parallel body checking
- [ ] P03.11 Type serialization interface
- [ ] P03.12 `gopp check` & `gopp vet` v0
- [ ] P03.13 Check throughput

## Open questions

- [ ] Q1 Error-code scheme and its stability promise
- [ ] Q2 `vet` as a separate pass or a mode of `check`

## Exit criteria

- [ ] All P01 samples except `derive` type-check (C/C++ imports mocked)
- [ ] ≥ 90% of the ported types2 subset passes; exclusions listed with reasons
- [ ] Deterministic diagnostics under parallel checking
- [ ] D3.10 budget met (or ADR records the gap)

## Decision changes

- None.

## Notes / blockers

- Until P04.09 lands, `gopp check` runs only the type-level checks (D3.5).
