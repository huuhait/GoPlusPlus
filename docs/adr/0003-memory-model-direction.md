# ADR-0003: Memory model: ownership without a GC or borrow checker

- **Status:** accepted; the "no borrow checker" part is superseded by [ADR-0021](0021-borrow-checking.md). Updated to the final text in P01.15 (the rules are in `docs/spec/`; the owner decisions that changed the plan are in ADR-0006). (changed by ADR-0020: the `ref` type is now `View[…]`, `@Ref` is `@View`, `ref` is no longer a contextual word, and `case T:` no longer matches a stored view)
- **Date:** 2026-10-27
- **Plan reference:** P01 D1.3 to D1.7, D1.21

## Context

Go++ must keep Go's look without a garbage collector, and it must interoperate
with C++ objects (P08), which have constructors, destructors and move
semantics. This is the hardest question in the project, so the direction was
recorded here before the specification (P01) wrote the details. This text now
matches the finished spec.

## Decision

- **Three type categories (D1.3).** *Trivial* types (numbers, raw `*T`, refs,
  plain structs of those) copy bitwise and satisfy the `Copy` constraint.
  *Owning* types (`string`, `[]T`, `map[K]V`, `Unique[T]`, types with a `drop`
  method, C++ classes with non-trivial special members) are move-only and are
  copied only with an explicit `x.Clone()`. *Shared handles* (`Shared[T]`,
  `Weak[T]`, `chan T`, interfaces including `error`, func values) copy by
  atomic retain.
- **One owner (D1.4).** Every value has exactly one owner. When it ends, the
  `drop` method runs, then fields are destroyed in reverse order. Locals die at
  the end of their block, temporaries at the end of the full statement.
  A by-value use of an owned local moves it; using it afterwards is a compile
  error. A moved-from place holds the zero value, so there are no drop flags.
- **Explicit copies (D1.4 rule 6).** Every cloneable type has a method
  `Clone() T`, built-in types too (`string`, `[]T`, `map`, `Unique[T]`,
  numbers). `clone(x)` is a short form that calls it, and `move(x)` stays a
  built-in. A type whose `Clone` has another signature is not cloneable. In
  generic code the constraint is written `[T Clone]`, like `comparable`;
  `[T Copy]` means "copies for free".
- **No borrow checker (D1.4 rule 8).** Refs and raw pointers can dangle, as in
  C++. The compiler catches the common mistakes: returning `&local`, returning
  a `ref` into a local, storing a borrow of a temporary, use after move, and a
  `defer`/`go` that borrows something that dies first. `-asan` catches the rest
  at runtime.
- **Parameters (D1.5).** Owning parameters *borrow* by default (read-only at
  the top level). `move` before the parameter name takes ownership
  (`func (l *List) Push(move s string)`, `func (move b Builder) Finish()`,
  `func Join(move parts ...string)`). `*T` mutates; trivial parameters are
  copies. `&` and `*` keep their Go meanings.
- **Refs (D1.6).** `ref string` and `ref []T` are `(ptr, len)` borrows. `ref` is
  a contextual keyword, special only where a type is expected, so
  `ref := "main"` still works. Slicing makes a ref without allocating; ref to
  owned is an explicit copy. `case string:` in a type switch also matches a
  `ref string`.
- **Pointers and allocation (D1.7).** Every heap allocation is spelled out. Go's
  `new` and `&T{...}` are removed. `*T` is raw and non-owning; `Unique[T]`
  (`new_unique`), `Shared[T]` (`new_shared`, atomic refcount) and `Weak[T]`
  (`new_weak`) own. Every pointer kind has `IsNil()`, and `p == nil` works.
- **Range (D1.21).** `for _, d := range xs` borrows owning elements read-only.
  `for _, &d := range xs` makes `d` the element itself, so writes change the
  collection.

## Consequences

- Deterministic, GC-free memory with C++-compatible object semantics, while
  keeping Go's zero values.
- Call sites look like Go; generic code such as
  `func Index[T comparable](xs []T, v T) int` works for owning `T` with no
  moves or clones.
- Programmers can still write dangling refs in the less obvious cases; this is
  accepted for v1.
- Owning values are never copied implicitly, so some Go code needs `Clone` or
  `move` added. The plain range loop is read-only for owning elements, so Go
  code that writes to a range variable needs the `&d` form.

## Alternatives

- **Tracing GC:** excluded by the requirements.
- **ARC everywhere:** hidden cost and reference cycles.
- **Rust-style borrow checker:** costs compile speed and simplicity; reconsider
  after v1.
- **Rust-style parameter default (plain = take, `ref` or `&` = borrow):**
  discussed and rejected in P01.15. It puts a mark on most functions and
  reuses Go's `&` and `*` signs (ADR-0006).
- **C++-style implicit copies:** hidden allocations.
- **Move-by-default parameters:** hostile to Go-style generic APIs.
- **Copy-by-default owning parameters:** hidden allocations.
- **Read-only trivial parameters:** reject ordinary Go code.
