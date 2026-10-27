# P01 Language specification v0: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[~] in_progress`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P01) · Design: [README.md](README.md)

## Tasks

- [ ] P01.01 Spec skeleton
- [ ] P01.02 Lexical deltas
- [ ] P01.03 Ownership chapter
- [ ] P01.04 Parameter passing
- [ ] P01.05 `!` and errors chapter
- [ ] P01.06 panic/defer/recover
- [ ] P01.07 Generics deltas
- [ ] P01.08 Concurrency semantics
- [ ] P01.09 Closures chapter
- [ ] P01.10 Macro & attribute surface
- [ ] P01.11 Foreign import surface
- [x] P01.12 Close open questions Q4, Q5, Q6
- [ ] P01.13 Ten sample programs
- [ ] P01.14 UB & implementation-defined list
- [ ] P01.15 Spec review
- [ ] P01.16 Slices and range chapter

## Open questions

- [x] Q1 Closure capture rule (decided: D1.11)
- [x] Q2 Map element access with owning values (decided: D1.4 rule 3a)
- [x] Q3 Owning pointers: `Unique[T]`/`Shared[T]`/`Weak[T]` (decided: D1.7)
- [x] Q4 Allocators: every owned slice and map stores its allocator (decided: D1.22)
- [x] Q5 Generic methods: allowed (decided: D1.10)
- [x] Q6 Interface inline storage: one word (decided: D1.12)
- [x] Q7 Range over owning elements / consuming range (decided: D1.21)
- [x] Q8 Small-string optimization (deferred to P15, P15 Q1)

## Exit criteria

- [ ] Every delta D1.2–D1.21 has grammar, semantics, an example and a rationale in `docs/spec/`
- [ ] Ten samples written with expected output and compared against Go versions
- [x] Q1–Q8 decided or explicitly deferred
- [ ] UB / implementation-defined list written

## Decision changes

- None.

## Notes / blockers

- None.
