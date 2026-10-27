# P03 — Semantic analysis

**Goal:** resolve names and type-check all of Go++ v0, including generics, the type-level ownership rules and `!`, and produce a typed AST ready for lowering. Ship `gopp check` and a first `gopp vet`.
**Depends on:** P02 · **Unblocks:** P04, P06 (export data), P09, P14 · **Size:** L · **Milestone:** M0–M1

## Decisions

**D3.1 — Follow Go's `go/types` (types2) algorithms wherever the semantics are shared.** That covers untyped constants, assignability, method sets, embedding/promotion, interface satisfaction, type sets, and inference by unification. Port types2's test data as a conformance suite (BSD; attributed).
*Why:* for the shared parts, Go's checker is the de-facto specification. Reinventing it would create subtle incompatibilities.

**D3.2 — Arbitrary-precision constants.** The Go spec requires at least 256-bit integers and 256-bit float mantissas for untyped constants.
- Integers use LLVM `APInt`, widened as needed.
- Floats use a small vendored big-float library. libbf (MIT) is the candidate, to be confirmed by the P03.02 spike.

**D3.3 — Package model = Go's.**
- A directory is a package.
- Build constraints come from `//gopp:build` (its terms include the main module's active build profile, e.g. `//gopp:build debug`, P06 D6.12) and `_os_arch.gopp` file suffixes. `_test.gopp` files and the `internal/` rule work as in Go.
- Capitalized names are exported. Names from imported C/C++ headers are always accessible, whatever their case.
- Unused imports and variables are errors, as in Go, which keeps dependency graphs (and builds) lean.

**D3.4 — Two-stage checking with parallel bodies.**
1. Collect package-level objects and check signatures lazily, with cycle detection, as types2's `objDecl` does.
2. Check function bodies in parallel. Bodies are independent once all signatures are known.

**D3.5 — Ownership checks are split by what they need.**
- **Type-level, in this phase:**
  - type categories (D1.3) and `Copy`/`Clone` constraint satisfaction (`Clone() T` method lookup, `IsNil()` on every pointer kind);
  - `move` parameter mode, `move ...T` for owning variadics, and read-only parameter bindings;
  - range forms: read-only borrowed variables, the `&d` alias form, and the loop-variable redeclaration error (D1.21);
  - `func f!() error` (a `!` function declaring `error` as its last result) is an error (D1.8);
  - returning a `ref` into a local of the same function (D1.4 rule 8, D1.6);
  - owned → ref and `Unique[T]`/`Shared[T]` → `*T` conversions;
  - borrowing a temporary into a longer-lived place;
  - `move`/`clone` typing and the `append` move rule.
- **Flow-sensitive, in P04 on GIR:** use after move, `defer`/`go` borrowing something that dies first, and drop elaboration.

In P03, `gopp check` runs the type-level checks. P04.09 adds the flow-sensitive checks on GIR; after that, `gopp check` runs both.

**D3.6 — `!` typing per D1.8.**
- `func f!(…) (R…)` has the type `func(…) (R…, error)`. Inside it, a `return` lists either the declared results (nil error) or the declared results plus an error.
- The result tuple of `f!(…)` is the call's results minus the trailing `error`. The enclosing function's last result decides whether `!` returns the error or panics.
- `!` on a call without a trailing `error`, or on a conversion or built-in, is an error with a targeted message.

**D3.7 — Foreign scopes behind an interface.** `ForeignScope` covers name lookup, type description, and call and instantiation checking. The Clang importer implements it in P07/P08. P03 ships a mock so the checker can be tested without Clang.

**D3.8 — An export-data-ready type representation.** Types and objects serialize through an interface defined here, so the checker can load dependencies from export data instead of sources. The format itself is P06's job.

**D3.9 — Diagnostics are a product feature.** Every error gets a stable code and, where possible, a fix-it. Go habits that become errors get tailored messages:
- `&T{…}`: "allocation is explicit in Go++: use `new_unique(T{…})`";
- assigning `new_unique(…)` to a `*T` field: "a `*T` field doesn't own: did you mean `Unique[T]`?";
- using a moved variable: points at the move site;

`gopp vet` hints (warnings, not errors):
- with `defer f.Close()` in a loop body the files stay open until the function returns.

**D3.10 — Budget (provisional): type-check ≥ 250 k lines/s per core** on the bench corpus, excluding foreign imports.

## Deliverables

- `compiler/lib/Sema`: scopes, objects, types, the checker, constant evaluation, generic instantiation validation, and the type-level ownership rules.
- `gopp check` (parse + type-check; the GIR checks are added in P04.09).
- `gopp vet` v0 with lints for: ref-typed struct fields without `@Ref`; `drop` methods that may not handle the zero value (heuristic); a `!` that would panic outside package `main`.
- A conformance suite: a ported types2 subset, plus Go++ rule-table tests (one positive and one negative test per ownership rule).

## Exit criteria

- All P01 samples except `derive` (it needs P09 macros) type-check, with C/C++ imports mocked.
- At least 90% of the ported types2 subset passes. Every exclusion is listed with the Go++ rule that makes it differ.
- Diagnostics are deterministic under parallel checking.
- The D3.10 budget is met, or an ADR records the gap.

## Risks

- **Generic inference corner cases** → port types2's algorithm faithfully rather than redesign it.
- **Interactions between ownership rules** (generics × refs × interfaces) → an exhaustive rule table with positive and negative tests, and a feedback loop into the P01 spec.

## Open questions

- **Q1** Error-code scheme and its stability promise (D3.9).
- **Q2** Is `vet` a separate pass (as in Go) or a mode of `check`? Proposed: built into `gopp`, run by `gopp vet` and the LSP.
