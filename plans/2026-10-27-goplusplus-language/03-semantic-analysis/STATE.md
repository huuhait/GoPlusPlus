# P03 Semantic analysis: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[~] in_progress`
Last updated: 2026-10-27 (P03.01–P03.07 done)
Task definitions: [../TODO.md](../TODO.md) (section P03) · Design: [README.md](README.md)

## Tasks

- [x] P03.01 Package loader
- [x] P03.02 Constant arithmetic
- [x] P03.03 Scopes & resolution
- [x] P03.04 Types core
- [x] P03.05 Statements & expressions
- [x] P03.06 Generics
- [x] P03.07 Type-level ownership
- [ ] P03.08 `!` typing
- [ ] P03.09 Foreign scope interface
- [ ] P03.10 Parallel body checking
- [ ] P03.11 Type serialization interface
- [ ] P03.12 `gopp check` & `gopp vet` v0
- [ ] P03.13 Check throughput
- [ ] P03.14 Error codes & `gopp explain`

## Open questions

- [x] Q1 Error-code scheme and its stability promise (decided: D3.11, numbered codes, strict promise)
- [x] Q2 `vet` as a separate pass or a mode of `check` (decided: D3.12, built into the checker)

## Exit criteria

- [ ] All P01 samples except `derive` type-check (C/C++ imports mocked)
- [ ] ≥ 90% of the ported types2 subset passes; exclusions listed with reasons
- [ ] Deterministic diagnostics under parallel checking
- [ ] D3.10 budget met (or ADR records the gap)

## Decision changes

- 2026-10-27 (ADR-0009, P03.02 spike): constant floats are exact rationals on `APInt` with a 512-bit binary float past 4096 bits, like `go/constant`; libbf is not vendored (D3.2).
- 2026-10-27 (P03.07 review, manager decisions): a field or element of an owned temporary is itself a temporary and can move out; through a `Shared[T]` temporary it is a place, and a ref into it is a ref into a temporary. A conversion that shares storage (same underlying owning type ignoring tags, to an interface, or through a type parameter) is checked as its operand; slice→array of owning elements needs ownership. Spec: EXP-2.
- 2026-10-27 (ADR-0007, build profiles): `//gopp:build` terms include the main module's active profile name (P06 D6.12).
- 2026-10-27 (ADR-0006, P01.15 owner decisions): constraints `Copy` and `Clone` (replace `trivial` and `clonable`), the `x.Clone()` method and `IsNil()` on every pointer kind, the `&d` range form, the loop-variable redeclaration error, `func f!() error` is an error, and returning a `ref` into a local is an error. Details in P01 D1.4, D1.8, D1.10, D1.21.

## Notes / blockers

- Until P04.09 lands, `gopp check` runs only the type-level checks (D3.5).
- P03.01–P03.07 (team build, ledger `.claude/team/p03-sema-core-ledger.md`, 25 findings fixed): types2 port with uniqued types, a side table and one checker per body; 63 rule-table files in `tests/sema/rules/`. Conformance on Go 1.25's types2 testdata: 262/305 counted files (85.9%), generics 154/167 (92.2%); 34 files excluded by design (`-lang`, `-gotypesalias=0`, cgo), the rest listed with reasons in `tests/sema/conformance/expected-failures.txt`. The overall exit criterion (≥ 90%) is not met yet: the gaps are cycle diagnostics, std packages the harness lacks, the size model and alias spelling.
- Check throughput (Release, 123k lines): 862k lines/s/core against the 250k budget (P03.13 records it formally).
- `tests/sema/spec-examples.test` lists every error the spec examples still give, by the task that removes it (P13 std, P03.08 `!`, P07/P08 imports, P09 macros). Update it when those land.
- For P03.10: bodies write `PkgName::Used` and parameters' `Var::FuncOwner`; make them thread safe first. "Same place" checks compare expression text of side-effect-free places.
- For P04 (GEN-5): a pinned value copied inside a generic body through a type parameter (`struct{state int}(*x)` with `x *T`) is seen only per instance.
