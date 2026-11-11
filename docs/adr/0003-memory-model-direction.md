# ADR-0003: Memory model: deterministic ownership without a GC

- **Status:** accepted (refined by [ADR-0021](0021-borrow-checking.md) and [ADR-0032](0032-shared-buffers-and-borrow-engine.md))
- **Date:** 2026-10-27
- **Status note (2026-11-11, [ADR-0041](0041-values-are-copied.md)):** partly reversed. Slices and maps are no longer shared handles: `t := s` copies for every type, moving is `move(x)`, and a parameter is the function's own copy. An interface value owns its box, with no reference count, and is copied with the value it holds. `s[i:j]` on a slice or an array is a copy, and the borrowed part is the view `&s[i:j]` (D1.6). "Every heap allocation is spelled out" (D1.7) no longer holds for copies. The alternative rejected below as "C++-style implicit deep copies" is now the rule, by owner decision. The absence of a garbage collector, deterministic destruction, single-owner types and borrow checking stand.
- **Status note (2026-11-11, [ADR-0045](0045-clone-is-not-called.md)):** in "Explicit deep copies", a copy is written `b := a`, `f(a)` or `return a` and in no other way. `Clone` is the name of the copy method that a type with `drop` defines, which `=` calls, and of the constraint `[T Clone]`.
- **Plan reference:** P01 D1.3 to D1.7, D1.21

## Context

Go++ must keep Go's look without a garbage collector, and it must interoperate
with C++ objects (P08), which have constructors, destructors and move
semantics. This ADR records the core three-category memory model (see
[ADR-0021](0021-borrow-checking.md) and [ADR-0032](0032-shared-buffers-and-borrow-engine.md)
for the borrow engine and shared-buffer details).

## Decision

- **Three type categories (D1.3).**
  - *Trivial (`Copy`, `OWN-C1`):* numbers, `bool`, raw `*T`, `unsafe.Pointer`,
    and arrays/structs of trivial types copy bitwise and satisfy `Copy`.
  - *Shared / value handles (`OWN-C2`):* `string`, `[]T` and `map[K]V` (with
    non-single-owner elements), `Shared[T]`, `Weak[T]`, `chan T`, interfaces
    (including `error`), `func` values, `Local[T]`, and structs/arrays/`Option`s
    of them share reference-counted backing storage, lend across calls at zero
    retain/release cost, and auto-move on last use.
  - *Single-owner (`OWN-C3`):* `Unique[T]`, types with a `drop` method,
    `@Pinned` types, non-copyable C++ classes, and composites containing them
    have one owner at a time and move automatically on by-value transfer.
- **Deterministic destruction (D1.4).** When a single-owner value's life ends,
  its `drop` method runs, then its non-trivial fields are destroyed in reverse
  declaration order. Shared handles release their reference count and free
  immediately when the strong count reaches zero. Locals die at the end of their
  block, temporaries at the end of the full statement. A moved-from place holds
  all-zero bytes, so normal paths need no drop flags.
- **Explicit deep copies (D1.4 rule 6).** Every cloneable type has a method
  `Clone() T`, built-in types too (`string`, `[]T`, `map`, `Unique[T]`,
  numbers). `clone(x)` is a short form that calls it. In generic code the
  constraint is written `[T Clone]`, like `comparable`; `[T Copy]` means "copies
  for free".
- **Borrow checking without lifetime annotations ([ADR-0021](0021-borrow-checking.md)).**
  Raw `*T` (`&x`), stack-array slices (`arr[i:j]`), active `range` loops, arenas
  and lock guards are checked by flow and liveness with no lifetime syntax.
- **Parameters (D1.5).** Parameter lists use pure Go syntax: trivial types copy,
  `OWN-C2` handles lend without retaining, single-owner `OWN-C3` values move,
  and `*T` borrows and mutates in place.
- **Slicing (D1.6).** Slicing a `string` or heap `[]T` produces a `string` or
  `[]T` sharing the reference-counted backing buffer in $O(1)$ without copying
  bytes; slicing a value array `arr[i:j]` produces a tracked borrow of `arr`.
- **Pointers and allocation (D1.7).** Every heap allocation is spelled out. Go's
  `new` and `&T{...}` are removed. `*T` is raw, non-owning and never nil;
  `Unique[T]` (`NewUnique`), `Shared[T]` (`NewShared`) and `Weak[T]` (`NewWeak`)
  manage heap lifetimes, and `Option[T]` represents nullable values
  ([ADR-0018](0018-never-nil-pointers-and-option.md)).
- **Range (D1.21).** `for _, d := range xs` gives read-only element bindings for
  structs and single-owner elements; `for _, &d := range xs` aliases each
  element in place so writes mutate the collection.

## Consequences

- Deterministic, GC-free memory with C++-compatible object semantics, while
  keeping Go's surface syntax and zero values.
- Call sites look identical to Go; generic code such as
  `func Index[T comparable](xs []T, v T) int` works for every type category.

## Alternatives

- **Tracing GC:** excluded by the requirements.
- **Unoptimized ARC everywhere:** avoided via parameter borrow lending and
  last-use auto-move elision ([ADR-0032](0032-shared-buffers-and-borrow-engine.md)).
- **Rust-style lifetime annotations:** avoided via flow-based borrow checking
  ([ADR-0021](0021-borrow-checking.md)).
- **C++-style implicit deep copies:** hidden allocations.
