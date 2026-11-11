# Go++ language specification v0

Status: reviewed by the owner (P01.15); decisions D1–D25 applied (see [ADR-0006](../adr/0006-p01-15-owner-decisions.md)). Base: the [Go 1.25 specification](https://go.dev/ref/spec). Go++ is written as a list of **deltas** from Go.

Start with the [overview](01-overview.md).

## Chapters

| # | Chapter | Covers | Plan decisions |
|---|---|---|---|
| 1 | [Overview](01-overview.md) | how to read, rule IDs, delta index | D1.1 |
| 2 | [Lexical elements](02-lexical.md) | keywords, `#`, `@`, `!`, predeclared names, contextual `base` | D1.2, ADR-0023 |
| 3 | [Types](03-types.md) | type syntax, interfaces, layout, required fields, inheritance (`@Extends`) | D1.12, D1.17, ADR-0023, ADR-0034 |
| 4 | [Ownership](04-ownership.md) | categories, copying and `move`, rules 1 to 10, pointers, slicing, borrow engine | D1.3, D1.4, D1.6, D1.7, ADR-0032, ADR-0041, ADR-0042, ADR-0043, ADR-0044, ADR-0045 |
| 5 | [Parameters](05-parameters.md) | by-value copies, pointer parameters, receivers, ABI | D1.5, ADR-0041 |
| 6 | [Expressions](06-expressions.md) | `move`, `mem.Take`, safety defaults, `@Operator` methods, `@Const` & composite `const` | D1.17, ADR-0024, ADR-0032, ADR-0033 |
| 7 | [Statements](07-statements.md) | assignment, blocks, headers | D1.4, D1.6, ADR-0032 |
| 8 | [Slices, maps and range](08-slices-and-range.md) | slicing and views, `:0` reset, allocators, range (also over user types) | D1.20, D1.21, D1.22, ADR-0024, ADR-0041 |
| 9 | [Errors and panics](09-errors-and-panics.md) | `!`, `call!()`, panic, defer, recover | D1.8, D1.9, D1.19 |
| 10 | [Generics](10-generics.md) | `Copy`, `Clone`, generic methods, operator constraints | D1.10, ADR-0024 |
| 11 | [Closures](11-closures.md) | shared cells, method values | D1.11 |
| 12 | [Concurrency](12-concurrency.md) | `go`, channels, `select`, races | D1.13, ADR-0022 |
| 13 | [Macro and attribute surface](13-macros-surface.md) | `macro`, `#`, `@`, built-in attributes | D1.14 |
| 14 | [Foreign import surface](14-foreign-surface.md) | header imports, `gopp.mod` C/C++ settings, `@Build` | D1.15 |
| 15 | [Program init and exit](15-program-init.md) | start and exit order | D1.18 |
| 16 | [Undefined behavior](16-undefined-behavior.md) | UB and implementation-defined list | D1.4 r8, D1.17 |

All `@` attributes on one page: [attributes cheat sheet](../attributes.md).

The memory model (a value is a copy, `move(x)`, pointers and `Shared[T]`) is [ADR-0041](../adr/0041-values-are-copied.md), with [ADR-0042](../adr/0042-shared-is-read-only.md) (what a `Shared[T]` holds is read-only), [ADR-0043](../adr/0043-no-star-to-write-through-a-pointer.md) (a pointer is used like the variable it points to) and [ADR-0044](../adr/0044-no-borrow-into-what-an-arena-value-owns.md) (pointers into an arena). The open points of ADR-0041 are decided; only the numbers of its two new error codes are not assigned yet.

Appendix: [A. Symbol names](appendix-a-symbol-names.md) (how the compiler names functions, methods, generic instances and type tables in object files, and `gopp demangle`; D4.13).

Appendix: [B. Grammar](appendix-b-grammar.md) (the whole syntax in one EBNF: Go's grammar with the Go++ productions written in).

## Samples (D1.16)

Each sample has `main.gopp`, a Go counterpart `main.go`, `expected_output.txt` and a README with the spec rules it uses.

| # | Sample | Shows |
|---|---|---|
| 1 | [hello](examples/01-hello/) | the minimal program |
| 2 | [wc](examples/02-wc/) | files, `!`, shared strings |
| 3 | [kvparse](examples/03-kvparse/) | zero-copy string slices into one buffer |
| 4 | [binarytrees](examples/04-binarytrees/) | `Unique[T]`, `NewUnique`, arenas |
| 5 | [workers](examples/05-workers/) | goroutines, channels, last-use transfers |
| 6 | [zcrc](examples/06-zcrc/) | zlib through a header import |
| 7 | [vecsort](examples/07-vecsort/) | `std::vector` and `std::sort` |
| 8 | [derive](examples/08-derive/) | `@Derive`, `#Assert`, user macro |
| 9 | [orderedmap](examples/09-orderedmap/) | generic container with value and single-owner `T` |
| 10 | [minicalc](examples/10-minicalc/) | arena AST, zero-copy string slicing, `!` |

## Conventions

- Rule IDs such as `OWN-R3a` are stable. Cite them in code review, tests and other phases.
- The P01 review (P01.15) is done: every point that was marked `Open (P01 review)` was decided by the owner, and the notes are removed. A point left to a later phase is marked `> **Deferred to Pxx:**`.
