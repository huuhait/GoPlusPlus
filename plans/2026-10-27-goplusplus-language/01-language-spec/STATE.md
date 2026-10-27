# P01 Language specification v0: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[~] in_progress`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P01) · Design: [README.md](README.md)

## Tasks

- [x] P01.01 Spec skeleton
- [x] P01.02 Lexical deltas
- [x] P01.03 Ownership chapter
- [x] P01.04 Parameter passing
- [x] P01.05 `!` and errors chapter
- [x] P01.06 panic/defer/recover
- [x] P01.07 Generics deltas
- [x] P01.08 Concurrency semantics
- [x] P01.09 Closures chapter
- [x] P01.10 Macro & attribute surface
- [x] P01.11 Foreign import surface
- [x] P01.12 Close open questions Q4, Q5, Q6
- [x] P01.13 Ten sample programs
- [x] P01.14 UB & implementation-defined list
- [ ] P01.15 Spec review (owner review of the `Open (P01 review)` notes; ADR-0003/0004 update)
- [x] P01.16 Slices and range chapter

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

- [x] Every delta D1.2–D1.21 has grammar, semantics, an example and a rationale in `docs/spec/`
- [x] Ten samples written with expected output and compared against Go versions
- [x] Q1–Q8 decided or explicitly deferred
- [x] UB / implementation-defined list written

## Decision changes

- None. Plan-silent points are written as the reading most consistent with the plan and marked `> **Open (P01 review):**` in `docs/spec/`, for the owner to confirm or change in P01.15.

## Notes / blockers

- The spec is in `docs/spec/` (16 chapters + README, 216 numbered rules). The samples are in `docs/spec/examples/`. Each Go counterpart runs and matches `expected_output.txt` byte for byte.
- The `.gopp` samples can't be compiled until P02/P03 exist. They were checked by hand against the rules.
- P01.15 is waiting on the owner. The spec has 47 `Open (P01 review)` notes; the reviewers flagged the variadic `...T` note (`05-parameters.md:20`) as the most important. More owner notes are in the ledger.
- Team-build ledger: `.claude/team/p01-language-spec-ledger.md` (58 findings over 20 rounds, all closed; 5 fresh final reviewers).
