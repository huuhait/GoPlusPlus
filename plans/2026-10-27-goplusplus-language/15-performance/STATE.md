# P15 Performance: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-29 (Q1 decided, ADR-0026)
Task definitions: [../TODO.md](../TODO.md) (section P15) · Design: [README.md](README.md)

## Tasks

- [ ] P15.01 Benchmark suite
- [ ] P15.02 Perf CI & dashboard
- [ ] P15.03 Allocation elision
- [ ] P15.04 Ownership-derived attributes
- [ ] P15.05 Check elimination
- [ ] P15.06 Devirtualization
- [ ] P15.07 Cross-package inlining
- [ ] P15.08 ThinLTO including cdeps
- [ ] P15.09 PGO
- [ ] P15.10 Allocator evaluation
- [ ] P15.11 Binary size
- [ ] P15.12 Targets review
- [ ] P15.13 Padding hint
- [x] P15.14 Decide P15 Q1

## Open questions

- [x] Q1 Small-string optimization for `string` (decided: no, ADR-0026)
- [ ] Q2 mimalloc as the default allocator (task P15.10)
- [ ] Q3 SIMD: deferred by owner 2026-10-28 (no task; options A, B, C in the README; recommendation: B now, C later)

## Exit criteria

- [ ] D15.2 targets met (geomean ≤ 1.10× C++, none > 1.5×, ≤ Go on allocation-heavy, size ≤ 1.5×) or gaps documented with ADRs
- [ ] Perf dashboard live; regression alerts working

## Decision changes

- 2026-10-28 (owner decision): SIMD is deferred but recorded as open question Q3 in the README (options A, B, C and the comparison). Not designed; no task. Operator overloading (ADR-0024) makes the `simd.Vec[T, N]` operators of option B possible.
- 2026-10-27 (ADR-0006, P01.15 owner decisions): `own` parameters are now `move` parameters (the `noalias` row), and `view` is renamed `ref`.

## Notes / blockers

- The compute-bound kernels of P15.01 can be written as soon as M1 lands. Track them from then on, so performance regressions are caught long before this phase formally starts.
