# P03 — Semantic analysis

**Goal:** resolve names and type-check all of Go++ v0, including generics, the type-level ownership rules and `!`, and produce a typed AST ready for lowering. Ship `gopp check` and a first `gopp vet`.
**Depends on:** P02 · **Unblocks:** P04, P06 (export data), P09, P14 · **Size:** L · **Milestone:** M0–M1

## Decisions

**D3.1 — Follow Go's `go/types` (types2) algorithms wherever the semantics are shared.** That covers untyped constants, assignability, method sets, embedding/promotion (Go mode only: struct embedding is an error in `.gopp` files, D3.13), interface satisfaction, type sets, and inference by unification. Port types2's test data as a conformance suite (BSD; attributed).
*Why:* for the shared parts, Go's checker is the de-facto specification. Reinventing it would create subtle incompatibilities.

**D3.2 — Arbitrary-precision constants.** The Go spec requires at least 256-bit integers and 256-bit float mantissas for untyped constants.
- Integers use LLVM `APInt`, widened as needed.
- Floats are exact rationals on `APInt`, with a 512-bit binary float when they grow past 4096 bits, as in Go's `go/constant`. No vendored library: the P03.02 spike rejected libbf ([ADR-0009](../../../docs/adr/0009-big-float.md)).

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
  - type categories (D1.3) and `Copy`/`Clone` constraint satisfaction (`Clone() T` method lookup; `IsNil()` and `Get()` on `Option`);
  - never-nil pointers, interfaces and funcs, and `Option[T]` (D1.7, ADR-0018 and its amendment points 3 and 4): the pointer kinds, `func` types and interface types with at least one method other than `error` have no zero value and can't be compared with `nil` (`error` and its aliases, the empty interface `any` (any interface with an empty method set), `unsafe.Pointer`, maps, slices and channels keep Go's `nil` (a nil map is empty and a write creates it; `close` of a known-nil channel, a write into a temporary map and a write into a map or slice inside an interface are errors, P03.19); `type MyErr error` is an interface with methods, so never nil; `Option[Logger]` / `Option[func()]` are valid, `Option[any]`, `Option[error]` and `Option[unsafe.Pointer]` are not; `@Required` on a func field or a field of an interface with methods other than `error` is an error, like on a pointer field); `nil` is assignable only to `Option`, `error`, `any`, `unsafe.Pointer`, slices, maps and channels (spec TYP-2); `T` converts to `Option[T]`; `Option[T]` is comparable when `T` is (equality, `comparable`, struct fields, map keys; with a non-comparable `T` only `== nil`; `o == 5` means `o == Option[int](5)`); `Unique[T]` without `*` is an error; **flow typing** for `Option`: after `o != nil`, `!o.IsNil()`, an early exit on `o == nil`, or in a `for o != nil` body, a local or parameter `o` has type `T` (this is in the type checker, because `o.Name` type-checks only after the check); a copy made where `o` is narrowed has type `T` (`p := a` is a `*Node`), an explicit `var p Option[*Node] = a` stays an `Option`; `move(o)` ends the narrowing of `o`. Task P03.17. The same flow typing covers `v, err := f()` when `v`'s type has no zero value (`v` only where `err == nil`) and the comma-ok forms of types with no zero value (`v` only where `ok` is true); the interface and func part and these checks are task P03.18;
  - `move` parameter mode, `move ...T` for owning variadics, and read-only parameter bindings;
  - range forms: read-only borrowed variables, the `&d` alias form, and the loop-variable redeclaration error (D1.21);
  - `func f!() error` (a `!` function declaring `error` as its last result) is an error (D1.8);
  - returning a view into a local of the same function (D1.4 rule 8, D1.6);
  - owned → view and `Unique[*T]`/`Shared[*T]` → `*T` conversions;
  - borrowing a temporary into a longer-lived place;
  - `move`/`clone` typing and the `append` move rule;
  - `@Strict` structs and `@Option` fields (D1.4 rule 11, ADR-0019, spec TYP-15a): the effective required bit of each field, the attribute placement and combination errors, the `@Strict`-without-required-field vet warning, and the required bits in struct conversions and in serialization; task P03.20;
  - `extends`, method marks, abstract types, `base`, up- and downcasts, and the removal of struct embedding in `.gopp` files (ADR-0023, spec TYP-16 to TYP-26, D3.13);
  - operator methods: forms, overload sets, resolution, compound forms, `@Right`, `range`, user `<-` and operator constraints (ADR-0024, spec TYP-27, EXP-16 to EXP-30, D3.14);
  - units of measure: `unit` declarations, cycles, canonical form and identity, `T[U]`, unit literals, unit math, scale conversion, exact-unit assignment, integer exactness, `[U unit]` generics and inference, `.In[U]()` and `@Symbol` (ADR-0025, spec UNI-1 to UNI-16, EXP-31 to EXP-37, TYP-29, GEN-14);
  - types with no zero value (D1.4 rules 9 and 11): C++ types without a default constructor and structs with a `@Required` field or a `@Strict` struct with a required field, spread to the structs and arrays that contain them; every composite literal must set each `@Required` field (and each field required by `@Strict`) (by name or by position); every place where Go makes a zero value is an error (`var x T`, `make([]T, n)` unless `n` is the constant `0`, `var a [N]T`, array literals with missing elements, keyed slice literals with gaps, named results, `var zero T` in generic code, `m[k]` and `<-ch` outside comma-ok forms, a write or `delete`, `clear(s)` on a slice); for generic types, per instance. The comma-ok lookups stay legal; that their value is read only where `ok` is true is checked by the flow typing of task P03.18. `move` out of a non-local place of a Go++ type with no zero value is an error. A nilable `@Required` field is never nil: storing `nil`, `move` out of it and `&` of it (except in `mem.Replace`) are errors here, `@Required` on `Weak[*T]` is an error, and every other stored value must be known non-nil (spec TYP-15's list), checked by the flow typing of task P03.18. Task P03.15, ADR-0016. The `@Required` flags and "no zero value" are part of a type's serialized description (D3.8), so they hold across packages.
- **Flow-sensitive, in P04 on GIR:** use after move, `defer`/`go` borrowing something that dies first, and drop elaboration. The comma-ok, `err == nil` and known-non-nil checks are done in P03 (task P03.18, ADR-0018 amendment point 4); P04.09 keeps them only as GIR regression cases.

In P03, `gopp check` runs the type-level checks. P04.09 adds the flow-sensitive checks on GIR; after that, `gopp check` runs both.

**D3.6 — `!` typing per D1.8.**
- `func f!(…) (R…)` has the type `func(…) (R…, error)`. Inside it, a `return` lists either the declared results (nil error) or the declared results plus an error.
- The result tuple of `f!(…)` is the call's results minus the trailing `error`. The enclosing function's last result decides whether `!` returns the error or panics.
- `!` on a call without a trailing `error`, or on a conversion or built-in, is an error with a targeted message.

**D3.7 — Foreign scopes behind an interface.** `ForeignScope` covers name lookup, type description, and call and instantiation checking. The Clang importer implements it in P07/P08. P03 ships a mock so the checker can be tested without Clang.

**D3.8 — An export-data-ready type representation.** Types and objects serialize through an interface defined here, so the checker can load dependencies from export data instead of sources. The format itself is P06's job.

**D3.9 — Diagnostics are a product feature.** Every error gets a stable code and, where possible, a fix-it. Go habits that become errors get tailored messages:
- `&T{…}`: "allocation is explicit in Go++: use `new_unique(T{…})`";
- assigning `new_unique(…)` to a `*T` field: "a `*T` field doesn't own: did you mean `Unique[*T]`?";
- using a moved variable: points at the move site;
- a missing `@Required` field: "missing required fields ID, Email in User literal", naming every missing field (no fix-it that fills in a zero, which would defeat the rule);
- `make([]User, n)` for a type with no zero value: "`User` has no zero value: use `make([]User, 0, n)` and `append`"; `clear(users)`: "use `users = users[:0]`";

`gopp vet` hints (warnings, not errors):
- with `defer f.Close()` in a loop body the files stay open until the function returns.

**D3.10 — Budget (provisional): type-check ≥ 250 k lines/s per core** on the bench corpus, excluding foreign imports.

## Deliverables

- `compiler/lib/Sema`: scopes, objects, types, the checker, constant evaluation, generic instantiation validation, and the type-level ownership rules.
- `gopp check` (parse + type-check; the GIR checks are added in P04.09).
- `gopp vet` v0 with lints for: `drop` methods that may not handle the zero value (heuristic); a `!` that would panic outside package `main`.
- A conformance suite: a ported types2 subset, plus Go++ rule-table tests (one positive and one negative test per ownership rule).

## Exit criteria

- All P01 samples except `derive` (it needs P09 macros) type-check, with C/C++ imports mocked.
- At least 90% of the ported types2 subset passes. Every exclusion is listed with the Go++ rule that makes it differ.
- Diagnostics are deterministic under parallel checking.
- The D3.10 budget is met, or an ADR records the gap.

## Risks

- **Generic inference corner cases** → port types2's algorithm faithfully rather than redesign it.
- **Interactions between ownership rules** (generics × views × interfaces) → an exhaustive rule table with positive and negative tests, and a feedback loop into the P01 spec.

**D3.11 — Numbered error codes with a strict promise** (owner decision; decides Q1).
- Every compiler error shows a short numbered code, Rust style: `error[E0042]: name used after move`.
- `gopp explain E0042` prints a longer explanation with a small example.
- Inside the compiler each code keeps its readable name (the `DiagnosticIDs.def` names from P02); the JSON output carries both the number and the name.
- **Promise, from v0.1:** a released code never changes meaning and is never reused. The message text may still be improved.
- Codes cover errors from every phase (lexer and parser from P02 included), so one table holds them all.

```
main.gopp:12:5: error[E0042]: name used after move
    fmt.Println(name)
                ^~~~
note: moved here (main.gopp:11:14)
help: use name.Clone() before the move to keep a copy
```

**D3.12 — `vet` is built into the checker** (owner decision; decides Q2).
- Vet checks run on the already type-checked code inside `gopp`; there is no separate tool that re-parses and re-checks.
- `gopp vet` turns the hints on, and the LSP shows them live in the editor.
- Like Go, `gopp test` also runs a small set of high-confidence vet checks. `gopp build` doesn't run them.

**D3.13 — Single inheritance and no struct embedding** (ADR-0023; task P03.22).
- `extends` parent links are part of the type graph and of export data (P03.11). Method marks (`@Virtual`, `@Abstract`, `@Override`), the abstract bit and the parent chain of the field table are serialized.
- The override check runs when the child's method set is built: each child method is matched against the ancestors' methods by name; the signature must be identical; a missing or surplus `@Override` is an error.
- Struct embedding is an error in `.gopp` files with a fix-it (use `extends`, or a named field). In `.go` files the types2 embedding code stays, so Go mode is unchanged.
- Dispatch, type tables, drop order and `base` calls are lowered in P04.22 (D4.18).

**D3.14 — Operator overloading** (ADR-0024; task P03.24).
- An operator method is an ordinary method whose name is an operator (`OperatorName`, TYP-27). It lives in the method set, in export data and in `gopp doc` like any method. The new things are the overload set (one operator name, several parameter lists), the shape table (EXP-16) and the resolution (EXP-17).
- Resolution is one function: given the operator and the static types of the operands, return the method, the built-in operation, or an error. Every operator expression, compound form, `!=`, `<`-family, index, call, conversion and `range` goes through it, so the rules cannot drift apart. Generic bodies call it with the constraint's method set (GEN-13), so a body is checked once.
- The call is then checked like the method call `a.op(b)` by the existing passes (borrow summaries, views, never-nil, `Send`/`Sync`). No new analysis is added, and the stress test covers resolution cost: overload sets are looked up by a hash on the operator and the parameter types, so a set of 50 methods and an expression of 10 000 operands stay near-linear.
- The blocked operators (`=`, `.`, unary `&`, the comma, implicit conversions) are rejected in the parser or at the declaration with the reason from EXP-29. `select` rejects user channel types (CON-22); this is decided for v1 (ADR-0024 decision 10).
- The lowering is P04.23 (D4.19).

**D3.15 — Units of measure** (ADR-0025; task P03.25).
- A unit is a package-level object with a dimension (a sparse vector of base-unit exponents) and an exact rational factor (P03.02 constants). Every unit expression is simplified to a canonical form and interned, so identity (UNI-5) is a pointer compare and the result cannot depend on thread order. The display form is kept beside it for printing; the printed one is chosen by a fixed order.
- A unit type `T[U]` is a type with the base kind `T` and the unit `U`. The checker tells a unit from a type argument by the base name (UNI-7): predeclared numeric type, or a generic whose parameter has the constraint `unit`. The parser switches to the unit grammar after a numeric type name, a number literal and `.In`.
- The rules of arithmetic are one function over (operator, left type, right type): same dimension and both operands converted to the finer unit for `+`, `-`, `%`, `min`, `max` and comparisons (the untyped constant `0` is accepted for any unit); unit math for `*` and `/`; exact types for assignment. It is used by every expression and by constant folding, so the rules cannot drift apart. Integer scale conversions are checked for an integer ratio.
- Unit declarations are resolved by an iterative walk (P03.23 lesson). Inference of a unit parameter solves one linear equation on the canonical form.
- The work is near-linear; the stress test of P03.25 covers declaration chains, long unit expressions and many instances. The lowering is P04.24 (D4.20).

## Open questions

- **Q1** (decided 2026-10-27) Error codes: numbered `E0042`-style codes with `gopp explain`, internal names kept, strict stability promise from v0.1 (D3.11).
- **Q2** (decided 2026-10-27) `vet` is built into the checker, run by `gopp vet`, the LSP, and a small subset by `gopp test` (D3.12).
