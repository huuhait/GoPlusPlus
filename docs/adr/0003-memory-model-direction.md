# ADR-0003: Memory model direction: ownership without a GC or borrow checker

- **Status:** accepted (direction only; the rules are specified in P01)
- **Date:** 2026-10-27
- **Plan reference:** P01 D1.3 to D1.7

## Context

Go++ must keep Go's look without a garbage collector, and it must interoperate
with C++ objects (P08), which have constructors, destructors and move
semantics. This is the hardest question in the project, so the direction is
recorded here before the specification (P01) writes the details.

## Decision

- **Three type categories (D1.3).** *Trivial* types (numbers, raw `*T`, views,
  plain structs of those) copy bitwise. *Owning* types (`string`, `[]T`,
  `map[K]V`, `Unique[T]`, types with a `drop` method, C++ classes with
  non-trivial special members) are move-only and are copied only with an
  explicit `clone(x)`. *Shared handles* (`Shared[T]`, `Weak[T]`, `chan T`,
  interfaces including `error`, func values) copy by atomic retain.
- **One owner (D1.4).** Every value has exactly one owner. When it ends, the
  `drop` method runs, then fields are destroyed in reverse order. Locals die at
  the end of their block, temporaries at the end of the full statement.
  A by-value use of an owned local moves it; using it afterwards is a compile
  error. A moved-from place holds the zero value, so there are no drop flags.
- **No borrow checker (D1.4 rule 8).** Views and raw pointers can dangle, as in
  C++. The compiler catches the common mistakes (returning `&local`, use after
  move, a `defer`/`go` that borrows something that dies first). `-asan` catches
  the rest at runtime.
- **Parameters (D1.5).** Owning parameters *borrow* by default (read-only at
  the top level); `own` takes ownership; `*T` mutates; trivial parameters are
  copies.
- **Views (D1.6).** `view string` and `view []T` are `(ptr, len)` borrows.
  Slicing makes a view without allocating; view to owned is an explicit copy.
- **Pointers and allocation (D1.7).** Every heap allocation is spelled out. Go's
  `new` and `&T{...}` are removed. `*T` is raw and non-owning; `Unique[T]`
  (`new_unique`), `Shared[T]` (`new_shared`, atomic refcount) and `Weak[T]`
  (`new_weak`) own.

## Consequences

- Deterministic, GC-free memory with C++-compatible object semantics, while
  keeping Go's zero values.
- Call sites look like Go; generic code such as
  `func Index[T comparable](xs []T, v T) int` works for owning `T` with no
  moves or clones.
- Programmers can still write dangling views; this is accepted for v1.
- Owning values are never copied implicitly, so some Go code needs `clone` or
  `move` added.

## Alternatives

- **Tracing GC:** excluded by the requirements.
- **ARC everywhere:** hidden cost and reference cycles.
- **Rust-style borrow checker:** costs compile speed and simplicity; reconsider
  after v1.
- **C++-style implicit copies:** hidden allocations.
- **Move-by-default parameters:** hostile to Go-style generic APIs.
- **Copy-by-default owning parameters:** hidden allocations.
- **Read-only trivial parameters:** reject ordinary Go code.
