# P02 Front-end: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P02) · Design: [README.md](README.md)

## Tasks

- [ ] P02.01 Source manager
- [ ] P02.02 Diagnostics engine
- [ ] P02.03 Lexer
- [ ] P02.04 AST & arenas
- [ ] P02.05 Parser core
- [ ] P02.06 Parser: macros & attributes
- [ ] P02.07 Error recovery
- [ ] P02.08 `gopp parse`
- [ ] P02.09 `gopp fmt`
- [ ] P02.10 Parser fuzzing
- [ ] P02.11 Parse throughput

## Open questions

- [x] Q1 Full CST for the LSP: decided, no (D2.3)
- [x] Q2 Macros in type position: deferred to P09 (P09 Q3)

## Exit criteria

- [ ] Every P01 grammar production has a test; all ten samples parse
- [ ] `gopp fmt` round-trip invariant holds on the corpus; gofmt-derived golden tests pass
- [ ] 24 CPU-hours of fuzzing without a crash
- [ ] D2.9 budget met (or ADR records the gap)

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): `macro` is the only new reserved keyword; `ref` and `move` are contextual (`move` goes before the parameter name); `&` marks the value variable in a range clause; struct fields take attributes after the type; a `block` macro argument is a func literal. Details in P01 D1.2, D1.14, D1.21.

## Notes / blockers

- Starts once P01.02 (lexical) and the grammar parts of P01.03–P01.11 are drafted. The lexer can start as soon as P01.02 is done.
