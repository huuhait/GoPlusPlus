# P02 Front-end: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[~] in_progress`
Last updated: 2026-10-27
Task definitions: [../TODO.md](../TODO.md) (section P02) · Design: [README.md](README.md)

## Tasks

- [x] P02.01 Source manager
- [x] P02.02 Diagnostics engine
- [x] P02.03 Lexer
- [x] P02.04 AST & arenas
- [x] P02.05 Parser core
- [x] P02.06 Parser: macros & attributes
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
- 2026-10-27: P02.01–P02.06 done through the team workflow (3 coders, PM merge on c2 = go/scanner + go/parser ports, review until a fresh reviewer returned CLEAN; ledger `.claude/team/p02-frontend-core-ledger.md`). All 10 spec samples and all 6240 Go stdlib files parse. Robustness: MaxNesting 1000 and a total AST height limit of 1024 per declaration (documented in spec ch. 16). Text diagnostics clip long lines to 120 columns. Follow-ups for P02.07 are in the ledger notes.
