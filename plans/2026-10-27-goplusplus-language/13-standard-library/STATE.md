# P13 Standard library v1: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27 (Q2–Q3 decided)
Task definitions: [../TODO.md](../TODO.md) (section P13) · Design: [README.md](README.md)

## Tasks

- [ ] P13.01 API conventions doc
- [ ] P13.02 T1 core
- [ ] P13.03 T1 text
- [ ] P13.04 T1 fmt
- [ ] P13.05 T1 collections
- [ ] P13.06 T1 I/O & OS
- [ ] P13.07 T1 time & testing
- [ ] P13.08 T2 encoding, hash, crypto
- [ ] P13.09 T2 context, log, exec, signal, net
- [ ] P13.10 T2 regexp, compress, container
- [ ] P13.11 First `x/` interop packages
- [ ] P13.12 Examples
- [x] P13.13 Decide P13 Q2–Q3 (owner decisions, ADR-0014, ADR-0015)

## Open questions

- [x] Q1 `%v` for any struct: decided, field tables as in Go (D13.4)
- [x] Q2 `sync.Pool` replacement (decided: `sync.Pool[T]` with auto cleanup and optional `Max`, ADR-0014)
- [x] Q3 Time zone database source (decided: OS first, automatic embedded fallback, ADR-0015)

## Exit criteria

- [ ] T1 complete; ported tests pass, including under ASan/LSan
- [ ] T2 complete with tests
- [ ] Every package has a runnable example, checked by `gopp test`
- [ ] `x/zstd` (or equivalent) builds from a locked cdep and passes tests

## Decision changes

- 2026-10-28 (ADR-0022, owner decision): std packages that used a plain `sync.Mutex` or `atomic.Add*` are ported to `Mutex[T]` / `Atomic[T]` and get `Send` / `Sync` labels (D13.6; P13.01 conventions doc, P12.13 library).
- 2026-10-27 (ADR-0006, P01.15 owner decisions): `view` is renamed `ref`; append-style functions take `move dst []T`.

## Notes / blockers

- The bootstrap std (P05 D5.12) covers the early needs; P13 replaces it.
