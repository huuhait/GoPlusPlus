# P03 Semantic analysis: STATE

Legend: `[ ] not_started` · `[~] in_progress` · `[x] completed`
Phase status: `[~] in_progress`
Last updated: 2026-10-27 (P03.01–P03.14 done; P03.15 `@Required` added, ADR-0016)
Task definitions: [../TODO.md](../TODO.md) (section P03) · Design: [README.md](README.md)

## Tasks

- [x] P03.01 Package loader
- [x] P03.02 Constant arithmetic
- [x] P03.03 Scopes & resolution
- [x] P03.04 Types core
- [x] P03.05 Statements & expressions
- [x] P03.06 Generics
- [x] P03.07 Type-level ownership
- [x] P03.08 `!` typing
- [x] P03.09 Foreign scope interface
- [x] P03.10 Parallel body checking
- [x] P03.11 Type serialization interface
- [x] P03.12 `gopp check` & `gopp vet` v0
- [x] P03.13 Check throughput
- [x] P03.14 Error codes & `gopp explain`
- [ ] P03.15 `@Required` fields (ADR-0016)
- [ ] P03.16 No auto-deref through `Weak` (OWN-P5, ADR-0017)
- [ ] P03.17 Never-nil pointers and `Option[T]` (ADR-0018)

## Open questions

- [x] Q1 Error-code scheme and its stability promise (decided: D3.11, numbered codes, strict promise)
- [x] Q2 `vet` as a separate pass or a mode of `check` (decided: D3.12, built into the checker)

## Exit criteria

- [ ] All P01 samples except `derive` type-check (C/C++ imports mocked)
- [ ] ≥ 90% of the ported types2 subset passes; exclusions listed with reasons
- [x] Deterministic diagnostics under parallel checking
- [x] D3.10 budget met (or ADR records the gap)

## Decision changes

- 2026-10-27 (ADR-0009, P03.02 spike): constant floats are exact rationals on `APInt` with a 512-bit binary float past 4096 bits, like `go/constant`; libbf is not vendored (D3.2).
- 2026-10-27 (P03.07 review, manager decisions): a field or element of an owned temporary is itself a temporary and can move out; through a `Shared[T]` temporary it is a place, and a ref into it is a ref into a temporary. A conversion that shares storage (same underlying owning type ignoring tags, to an interface, or through a type parameter) is checked as its operand; slice→array of owning elements needs ownership. Spec: EXP-2.
- 2026-10-27 (ADR-0007, build profiles): `//gopp:build` terms include the main module's active profile name (P06 D6.12).
- 2026-10-27 (ADR-0006, P01.15 owner decisions): constraints `Copy` and `Clone` (replace `trivial` and `clonable`), the `x.Clone()` method and `IsNil()` on every pointer kind, the `&d` range form, the loop-variable redeclaration error, `func f!() error` is an error, and returning a `ref` into a local is an error. Details in P01 D1.4, D1.8, D1.10, D1.21.

## Notes / blockers

- Until P04.09 lands, `gopp check` runs only the type-level checks (D3.5).
- P03.01–P03.07 (team build, ledger `.claude/team/p03-sema-core-ledger.md`, 25 findings fixed): types2 port with uniqued types, a side table and one checker per body; 63 rule-table files in `tests/sema/rules/`. Conformance on Go 1.25's types2 testdata: 262/305 counted files (85.9%), generics 154/167 (92.2%); 34 files excluded by design (`-lang`, `-gotypesalias=0`, cgo), the rest listed with reasons in `tests/sema/conformance/expected-failures.txt`. The overall exit criterion (≥ 90%) is not met yet: the gaps are cycle diagnostics, std packages the harness lacks, the size model and alias spelling.
- Check throughput (Release, 123k lines, P03.13): 927 to 943k lines/s/core on one thread (865k before P03.10) against the 250k budget: met, no ADR. With 8 body threads the run is 1.95 to 2.03x faster (bodies 5x; serial phase 1 and the table merge are what is left). Numbers and how the time splits: `bench/README.md`.
- `tests/sema/spec-examples.test` lists every error the spec examples still give, by the task that removes it (P13 std, P07/P08 imports, P09 macros); 03-kvparse and 10-minicalc now check except for their std imports. Update it when those tasks land.
- P03.10 (done): one worker per thread with its own arena, side table and set of used imports (one thread writes straight into the package's table); each body's diagnostics are replayed in declaration order; the workers' tables are merged into maps reserved once. `FuncOwner` of parameters is set in phase 1, `PkgName::Used` after the bodies from the workers' sets. The uniquing tables of `TypeContext` are split in 16 shards (lock and allocator each); lazy type completion (`NamedType::Expanded`, `InterfaceType::Set`, a recursive interface's `Pending` set) is done under `TypeContext::lock()` and published with release/acquire; line tables are built once per file. `gopp check` uses at most 8 threads, one per 16 bodies. "Same place" checks compare expression text of side-effect-free places.
- P03.08 (done): `func f!() R` is `func() (R, error)` (identical types); a `return` in a `!` function lists N or N+1 values (`err_bang_return_count` names both lists otherwise); `Info::Bangs` records what each `f!()` does (return the error, panic, set it in a `defer`); the innermost enclosing function decides (a function literal counts, `go f!()` always panics). P03.09: `ForeignScope` (Foreign.h) and a mock; C++ namespaces are `PkgName` objects (`pkg.ns.Name`, also in types: the parser reads `a.b.c` as a type name), templates take explicit bracketed arguments (ADR-0010), const metadata decides the read-only checks (ADR-0008), an ambiguous short name is reported where it is used, with the candidates; foreign call and instantiation failures have their own codes. P03.11: `Serialize.h` writes and reads types and package APIs as records (bitstream-shaped; the on-disk export format is P06); the reader checks every record, and the whole conformance corpus round-trips to identical bytes. P03.12: `gopp vet` = `gopp check` plus `vetPackage` (Vet.cpp) on the package asked for; findings are warnings (`-Werror` makes them fail). P03.14: `DIAG(Name, Code, Sev, Text)`, `docs/errors/codes.lock`, `ninja check-diag-codes` (part of `check`), `gopp explain`; every example in `DiagnosticExplain.def` is compiled by `Explain.*` in the unit tests (27 rows that cannot be compiled from one file are listed there with the reason).
- Error codes: the 117 dynamic messages of the checker (`diag::custom`) now use one named entry per part of the checker (`err_sema_assign`, `err_sema_builtin`, ...), each with its own code. They share a code inside a part, so a code names the kind of mistake, and the message says exactly what is wrong. Splitting a family into one code per message is possible later: add rows, never renumber.
- Before v0.1 (D3.11): the 10 family codes E0314–E0323 (`err_sema_*`, about 115 distinct messages) must be split into specific codes. Splitting after the v0.1 stability promise would change what a released code means (a user who matched E0314 would see it replaced by several codes), and a code never changes. `gopp explain` prints typical messages for them until then (`Families` in `DiagnosticExplain.cpp`); remove an entry there when its code is split.
- For P04 (GEN-5): a pinned value copied inside a generic body through a type parameter (`struct{state int}(*x)` with `x *T`) is seen only per instance.
- P03.08–P03.14 team build: ledger `.claude/team/p03-sema-tools-ledger.md` (8 findings fixed; two were nondeterminism under parallel checking: type-set errors of shared generic interface instances, and a depth cut-off published to every body).
- Known limits (deterministic, valid code only, for later throughput work, P15): a generic interface chain deeper than `MaxTypeSetDepth` (1000) can give a wrong "undefined" error; `substImpl` stops at depth 64, so a type nested more than 64 deep inside a generic (e.g. a field `*`×70 `T`) is not substituted (also before P03.10); a generic interface DAG `I_k[T] interface{ J_k[T]; I_{k-1}[T] }` with `J_k[T] interface{ I_{k-1}[T] }` takes exponential time in phase 1 (0.6 s at 20 levels, over 60 s at 400; the non-generic form is linear).
