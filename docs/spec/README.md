# Go++ language specification v0

Status: reviewed by the owner (P01.15); decisions D1–D25 applied (see [ADR-0006](../adr/0006-p01-15-owner-decisions.md)). Base: the [Go 1.25 specification](https://go.dev/ref/spec). Go++ is written as a list of **deltas** from Go.

Start with the [overview](01-overview.md).

## Chapters

| # | Chapter | Covers | Plan decisions |
|---|---|---|---|
| 1 | [Overview](01-overview.md) | how to read, rule IDs, delta index | D1.1 |
| 2 | [Lexical elements](02-lexical.md) | keywords, `#`, `@`, `!`, `<=>`, predeclared names, contextual `unit` | D1.2, ADR-0024, ADR-0025 |
| 3 | [Types](03-types.md) | type syntax, interfaces, layout, required fields, single inheritance (`extends`), unit types | D1.12, D1.17, ADR-0023, ADR-0025 |
| 4 | [Ownership](04-ownership.md) | categories, rules 1 to 10, pointers, views | D1.3, D1.4, D1.6, D1.7 |
| 5 | [Parameters](05-parameters.md) | borrow, `move`, receivers, ABI | D1.5 |
| 6 | [Expressions](06-expressions.md) | `move`, `clone`, safety defaults, operator methods, unit expressions | D1.17, ADR-0024, ADR-0025 |
| 7 | [Statements](07-statements.md) | assignment, blocks, headers | D1.4, D1.6 |
| 8 | [Slices, maps and range](08-slices-and-range.md) | in-place re-slicing, allocators, range (also over user types) | D1.20, D1.21, D1.22, ADR-0024 |
| 9 | [Errors and panics](09-errors-and-panics.md) | `!`, `call!()`, panic, defer, recover | D1.8, D1.9, D1.19 |
| 10 | [Generics](10-generics.md) | `Copy`, `Clone`, generic methods, operator constraints, unit parameters | D1.10, ADR-0024, ADR-0025 |
| 11 | [Closures](11-closures.md) | shared cells, method values | D1.11 |
| 12 | [Concurrency](12-concurrency.md) | `go`, channels, `select`, races, user `<-` types | D1.13, ADR-0024 |
| 13 | [Macro and attribute surface](13-macros-surface.md) | `macro`, `#`, `@`, built-in attributes (`@Symbol`) | D1.14, ADR-0025 |
| 14 | [Foreign import surface](14-foreign-surface.md) | header imports, `gopp.mod` C/C++ settings, `//gopp:build` | D1.15 |
| 15 | [Program init and exit](15-program-init.md) | start and exit order | D1.18 |
| 16 | [Undefined behavior](16-undefined-behavior.md) | UB and implementation-defined list | D1.4 r8, D1.17 |
| 17 | [Units of measure](17-units.md) | `unit` declarations, `float64[KM]`, unit literals, unit math, `[U unit]`, printing | ADR-0025 |

All `@` attributes on one page: [attributes cheat sheet](../attributes.md).

## Samples (D1.16)

Each sample has `main.gopp`, a Go counterpart `main.go`, `expected_output.txt` and a README with the spec rules it uses.

| # | Sample | Shows |
|---|---|---|
| 1 | [hello](examples/01-hello/) | the minimal program |
| 2 | [wc](examples/02-wc/) | files, `!`, owned strings, views |
| 3 | [kvparse](examples/03-kvparse/) | views into one buffer, `@View` |
| 4 | [binarytrees](examples/04-binarytrees/) | `Unique[*T]`, `new_unique`, arenas |
| 5 | [workers](examples/05-workers/) | goroutines, channels, moves |
| 6 | [zcrc](examples/06-zcrc/) | zlib through a header import |
| 7 | [vecsort](examples/07-vecsort/) | `std::vector` and `std::sort` |
| 8 | [derive](examples/08-derive/) | `@Derive`, `#Assert`, user macro |
| 9 | [orderedmap](examples/09-orderedmap/) | generic container with owning `T` |
| 10 | [minicalc](examples/10-minicalc/) | arena AST, views, `!` |

## Conventions

- Rule IDs such as `OWN-R3a` are stable. Cite them in code review, tests and other phases.
- The P01 review (P01.15) is done: every point that was marked `Open (P01 review)` was decided by the owner, and the notes are removed. A point left to a later phase is marked `> **Deferred to Pxx:**`.
