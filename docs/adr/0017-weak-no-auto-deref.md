# ADR-0017: `Weak[T]` has no auto-dereference

- **Status:** accepted
- **Date:** 2026-10-28
- **Plan reference:** spec OWN-P3, OWN-P5, OWN-P9; P01 D1.7; task P03.16.

## Context

OWN-P5 auto-dereferences pointer kinds on field access and method calls. A weak pointer's target can be freed when the last `Shared[T]` reference ends, so `w.Name` would read through a pointer whose target may already be gone, with a hidden lock on every access.

The owner decided in a review of the pointer design (2026-10-28) that a `Weak[T]` must be upgraded with `Lock()` before use.

## Decision

- Auto-dereference applies to `*T`, `Unique[T]` and `Shared[T]` only. On a `Weak[T]`, `w.Name` and `w.Method()` are compile errors with the fix-it `if p := w.Lock(); p != nil { p.Name }`.
- `Weak[T]` (created with `NewWeak(s)` from a `Shared[T]`) has a single method: `Lock() Option[Shared[T]]`, returning `nil` when the target has already been freed ([ADR-0018](0018-never-nil-pointers-and-option.md)). `*w` is not defined, and `Weak[T]` itself is never nil (`Option[Weak[T]]` represents an absent weak back-pointer).

## Consequences

- Every read through a weak pointer is visible at the call site as `w.Lock()` followed by a nil check.
- Selector lookup in `crates/gopp-sema` enforces this rule and emits the `w.Lock()` fix-it.
- Spec chapter 4 starts with an "at a glance" table of the pointer kinds and borrows.

## Alternatives

- **Keep auto-dereference on `Weak[T]`, with a panic on a dead target.** Rejected: a hidden upgrade and a hidden panic on every field access.
