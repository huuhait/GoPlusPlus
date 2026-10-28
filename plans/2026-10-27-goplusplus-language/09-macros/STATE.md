# P09 Macros: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[ ] not_started`
Last updated: 2026-10-27 (Q2–Q4 decided)
Task definitions: [../TODO.md](../TODO.md) (section P09) · Design: [README.md](README.md)

## Tasks

- [ ] P09.01 Macro design doc
- [ ] P09.02 Declarative macros
- [ ] P09.03 GIR interpreter
- [ ] P09.04 `gopp/macro` API
- [ ] P09.05 Procedural expression/statement macros
- [ ] P09.06 Attached macros
- [ ] P09.07 Built-in macros
- [ ] P09.08 `@Derive` set
- [ ] P09.09 Expansion cache
- [ ] P09.10 Tooling hooks
- [x] P09.11 Decide Q2–Q4 (owner decisions, ADR-0011)
- [ ] P09.12 Overhead budget
- [ ] P09.13 Profile macros and `@If`
- [ ] P09.14 `#Run` compile-time calls

## Open questions

- [x] Q1 `#Try`: not needed (`!` returns errors, P01 D1.8)
- [x] Q2 Quoting API form (decided: constructors only in v0, ADR-0011)
- [x] Q3 Macros in type position (moved from P02 Q2) (decided: not in v0, ADR-0011)
- [x] Q4 Compile-time function evaluation for constants (decided: explicit `#Run`, D9.13, ADR-0011)

## Exit criteria

- [ ] M6: standard macros + `@Derive(JSON, Equal)` work, and a P09 test package uses them (the json output check is in P13.08)
- [ ] Hygiene suite passes
- [ ] ≥ 95% expansion-cache hits on no-op rebuild
- [ ] D9.10 budget met (or ADR)

## Decision changes

- 2026-10-27 (ADR-0006, P01.15 owner decisions): `@Name(args)` on a declaration or struct field and `#Name(args)` in code are both kept; a `block` argument is a func literal (`return`/`break`/`continue` are errors); only `lit` parameters and `@` attribute arguments must be compile-time values (code-fragment parameters may name runtime variables); `stmt`, `type` and `tokens` argument syntax and the procedural-macro API are designed here.
- 2026-10-27 (ADR-0007, build profiles): new D9.12 with `#Profile()`, `#Setting("name")` and the attribute `@If(cond)`; D9.8 lists the two macros, D9.6 adds them to the expansion key, and D9.9 now says macros do not *hide* code from the compiler (the profile forms still type-check everything).
- 2026-10-27 (ADR-0011, Q2–Q4 owner decisions): the `gopp/macro` API builds syntax with constructors only (the format-string sketch is replaced); no macros where a type goes in v0; new D9.13 `#Run(f(args))` and task P09.14.

## Notes / blockers

- Declarative macros (P09.02) and `#Assert`/`#Stringify` only need P03. They can land before the interpreter, which needs P04.
