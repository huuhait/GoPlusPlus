# P15 Performance: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
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
- [ ] P15.14 Decide P15 Q1

## Open questions

- [ ] Q1 Small-string optimization for `string` (task P15.14; decide before v1.0)
- [ ] Q2 mimalloc as the default allocator (task P15.10)

## Exit criteria

- [ ] D15.2 targets met (geomean ≤ 1.10× C++, none > 1.5×, ≤ Go on allocation-heavy, size ≤ 1.5×) or gaps documented with ADRs
- [ ] Perf dashboard live; regression alerts working

## Decision changes

- None.

## Notes / blockers

- The compute-bound kernels of P15.01 can be written as soon as M1 lands. Track them from then on, so performance regressions are caught long before this phase formally starts.
