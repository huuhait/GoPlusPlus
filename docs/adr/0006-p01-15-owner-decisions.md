# ADR-0006: P01.15 owner decisions that shaped the spec

- **Status:** accepted (updated with [ADR-0018](0018-never-nil-pointers-and-option.md), [ADR-0032](0032-shared-buffers-and-borrow-engine.md) and [ADR-0034](0034-extends-attribute.md))
- **Date:** 2026-10-27
- **Status note (2026-11-11, [ADR-0041](0041-values-are-copied.md), [ADR-0045](0045-clone-is-not-called.md)):** in D4 and D19, a deep copy is written `b := a`, `f(a)` or `return a`: assignment, passing and storing copy every type. `Clone` stays as the copy method of a type with `drop`, which `=` calls, as `p.Clone()` on a `*P` or `Unique[P]` whose `P` has a type table, and as the constraint, which means "can be copied"; the other decisions stand.
- **Plan reference:** P01 D1.2, D1.4, D1.5, D1.6, D1.7, D1.8, D1.9, D1.10, D1.11, D1.12, D1.14, D1.15, D1.21. Also P05 D5.4, P07 D7.3 and D7.9, P08 D8.1, D8.3 and D8.6, P10 D10.2, P04 D4.6 (D17), P09 (D22, D23) and P12 D12.5 and D12.7 (D22).

## Context

In P01.15 the owner reviewed all `Open (P01 review)` notes of the language
specification and settled each one. This ADR records the enduring language rules
established in that review.

## Decision

### D2: a map insert during `range` always panics
Inserting a **new key** into a map while any `range` over that map runs always
panics, whether or not the table has room ("map insert during iteration").
Writing an existing key is not an insert. Inserts the compiler can see are
compile errors (RNG-3, RNG-4).

### D4: `x.Clone()` is the explicit deep-copy method; `clone(x)` is the short form
Every cloneable type has a method `Clone() T`, built-in types included
(`string`, `[]T`, `map`, `Unique[T]`, numbers). `clone(x)` stays as a short form
that calls it. A type whose `Clone` method has another signature is legal, but
does not satisfy `Clone`. Examples and fix-its use `x.Clone()`. A hand-written
`Clone` has a value receiver (borrowed read-only so cloning never consumes a
single-owner source). `Unique[T]` and `Shared[T]` keep their own `Clone` when
`T` also has one: `u.Clone()` deep-clones, `s.Clone()` retains. On a raw `*T`
where `T` has a value-receiver `Clone() T`, `p.Clone()` is a compile error for
ambiguity (`(*p).Clone()` clones `T`, `q := p` copies/reborrows the pointer).

### D7: the `&d` range form
`for _, &d := range xs` makes `d` an alias of the element itself, so writes
change the collection. It works for slices, arrays, map values
(`for k, &v := range m`), C++ containers and iterators that yield `*T`. Map keys
cannot use `&`. Plain `for _, d := range xs` prevents accidental writes to a
discarded struct copy (`d.Title = "x"` is a compile error with fix-it `&d`),
while growing the collection in the loop is a compile error (RNG-1, RNG-1a,
RNG-3).

### D15: a `!` function cannot declare `error` as its last result
`func check!() error` is a compile error ("`!` already adds the error; remove
it"). `func check!()` returns only an error. To return an error as a normal
value alongside an error, use the Go form `func check() (error, error)`
(ERR-15).

### D17: `defer` in a loop works like Go
`defer f.Close()` on a loop-body variable is legal: the variable's lifetime is
extended until its deferred call has run at function exit, matching Go (PAN-3).
`@Pinned` locals can be extended because nothing moves in memory.

### D19: `Clone` and `Copy` are built-in constraints
`[T Clone]` means `T` has `Clone() T` (every cloneable type, numbers included).
`[T Copy]` means `T` copies bitwise for free (`OWN-C1`). They are written like
Go's `comparable` and can be embedded in constraint interfaces (GEN-1, GEN-2).

### D20: no redeclaring a loop variable inside its loop
Declaring a variable with the same name as a loop variable inside that loop's
body (`u := u`, `u := u.Clone()`) is a compile error (RNG-8).

### D22: channel lifetime, `sync.Scope`, field attributes, block arguments
A channel and its buffered values are freed automatically when the last handle
to the channel is released. `sync.Scope` lets scoped goroutines borrow local
data safely. Attributes are allowed on struct fields, written after the field's
type and before any Go tag (``Name string @JSON("name") `db:"name"` ``). A
`block` macro argument is written like a func literal, `#Repeat(3, func() { … })`;
`return`, `break` and `continue` inside it are compile errors because the code
is pasted inline (CON-4, CON-9, MAC-12, MAC-13).

### D23: macro arguments
`@Name(args)` on top of a declaration receives its arguments plus the
declaration and returns new code. `#Name(args)` expands in place. Arguments that
a macro reads as values must be compile-time constants; code-fragment parameters
(`expr`, `ident`, `block`, `stmt`) are pasted into the program and may name
runtime variables (`#Assert(x > 0, "bad")`, MAC-14).

### D24: C/C++ settings live in `gopp.mod`
Module-wide directives `cxxstd c++20`, `cflags …` and `ldflags …` live in
`gopp.mod`, set once per project like Go's `go.mod`. The build condition stays
in source files: the attribute `@Build(...)` (FOR-10, FOR-11,
[ADR-0046](0046-build-attribute.md)).

### D25: returning a pointer into a local is a compile error
Returning `&local` or any borrow into storage that dies at function return is a
compile error ([ADR-0021](0021-borrow-checking.md), OWN-B2, OWN-B3).

## Consequences

- Only `macro` is a reserved new keyword; `base` is contextual. (Inheritance is the attribute `@Extends`, [ADR-0034](0034-extends-attribute.md).)
- Map iteration panics deterministically on any unseen new-key insert or `clear` while an iterator is active.
- `gopp.mod` owns `cxxstd`, `cflags` and `ldflags`.
- Deferring in a loop preserves Go's exact function-exit lifetime semantics.

## Amendment (2026-10-31, owner decision): a captured loop variable of a type with no `Clone` is E0452

Each iteration of a three-clause `for` has its own copy of a loop variable that
a function literal captures (CLO-8), made from the previous iteration's
variable. A single-owner variable of a type with `drop` and no `Clone()` cannot
be copied, so a function literal that captures it is **E0452 at the loop** (with
`Clone()`, each iteration gets its own copy).
