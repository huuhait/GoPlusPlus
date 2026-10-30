# P05 Runtime and memory: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P05) · Design: [README.md](README.md)

## Tasks

- [ ] P05.01 Runtime skeleton
- [ ] P05.02 Startup & exit
- [ ] P05.03 Allocators
- [ ] P05.04 Owned strings
- [ ] P05.05 Slices
- [ ] P05.06 Maps
- [ ] P05.07 Pointer kinds
- [ ] P05.08 Type descriptors & glue
- [ ] P05.09 Dynamic interface conversions
- [ ] P05.10 Panic runtime
- [ ] P05.11 Safety checks
- [ ] P05.12 Sanitizers
- [ ] P05.13 M2 suite
- [ ] P05.14 Errors: shared values and traces
- [ ] P05.15 Bootstrap std
- [ ] P05.16 Spec-revision window
- [ ] P05.17 Drop arena

## Open questions

- [x] Q1 Arenas with destructors (decided: both, `mem.Arena` + `mem.DropArena`, D5.3)
- [x] Q2 mimalloc as the default allocator (decided 2026-10-29, ADR-0027)
- [x] Q3 Package-level destructors at exit: decided, never (P01 D1.18)

## Exit criteria

- [ ] M2: samples without concurrency, C/C++ imports or macros run correctly and leak-free under ASan + LSan
- [ ] panic/defer/recover conformance tests pass, including destructor order on unwind
- [ ] Microbenchmarks recorded: map vs absl::flat_hash_map (≤ 1.2×), append vs std::vector (≤ 1.1×)

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): D5.4: inserting a new key into a map during `range` always panics ("map insert during iteration"), not only when the table must grow; the current entry is dropped at the end of its iteration; `ArenaNew` takes `T Copy`.

## Notes / blockers

- Do P05.15 (bootstrap std) early: the P04.14 differential tests print through it (P04 D4.11).
- P05.16: a spec-revision window after M2. Check the ownership rules against the M2 programs, and feed changes back into P01 (through ADRs) before later phases depend on them.
