# ADR-0017: `Weak[T]` has no auto-dereference

- **Status:** accepted; parts changed by ADR-0018 (`IsNil` on `Weak` removed; `Lock()` returns `Option[Shared[*T]]`; spelled `Weak[*T]`)
- **Date:** 2026-10-28
- **Plan reference:** spec OWN-P5, OWN-P9; P01 D1.7; new task P03.16. Changes one clause of ADR-0006 (the `IsNil` tie-break on `Weak[T]`).

## Context

OWN-P5 said field access and method calls auto-dereference every pointer kind, which included `Weak[T]`. A weak pointer's target can be freed at any moment, so `w.Name` would read through a pointer that may already be gone, with a hidden lock on every access. The checker (P03) currently allows it. ADR-0006 also said that on a `Weak[T]` whose `T` has its own `IsNil`, `T`'s method is reached as `(*p).IsNil()`.

The owner decided in a review of the pointer design (2026-10-28) that a `Weak` must be locked before use.

## Decision

- Auto-dereference applies to `*T`, `Unique[T]` and `Shared[T]` only. On a `Weak[T]`, `w.Name` and `w.Method()` are compile errors with the fix-it `if p := w.Lock(); p != nil { p.Name }`.
- A `Weak[T]` has exactly two methods: `Lock()` (returns a `Shared[T]`, or `nil` when the value is gone) and `IsNil()` (true when there is no target). `*w` is not defined. (Changed by ADR-0018: the type is spelled `Weak[*T]`, it has only `Lock()`, which returns an `Option[Shared[*T]]`, and there is no `IsNil()`, because a `Weak` always points at its target's control block.)
- This replaces the `Weak[T]` part of ADR-0006's `IsNil` tie-break. `T`'s own `IsNil` is reached through `Lock`: `if p := w.Lock(); p != nil { p.IsNil() }`, where the `Shared[T]` rule applies (`(*p).IsNil()`). The rules for `Unique[T]`, `Shared[T]` and raw `*T` don't change.
- `IsNil()` and the name `new_weak` stay as they are (owner decision in the same review). (Changed by ADR-0018: `IsNil()` is removed from `Weak`; `new_weak` stays.)

## Consequences

- A read through a weak pointer is always visible as a `Lock()` and a nil check.
- Task P03.16 changes the selector lookup in `compiler/lib/Sema/Call.cpp` and adds tests.
- Spec chapter 4 starts with an "at a glance" table of the pointer kinds and borrows (documentation only).

## Alternatives

- **Keep auto-dereference on `Weak`, with a panic on a dead target.** Rejected: a hidden lock and a hidden panic on every field access.
- **Remove `IsNil()` everywhere and use `p == nil`; rename `new_weak` to `weak`.** Considered in the same review; the owner kept both as they are.
