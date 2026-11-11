# ADR-0045: A copy is written `=`; there is no `.Clone()` on a value

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** P01.16; replaces rule 5 of the "Rules the documents follow" of [ADR-0041](0041-values-are-copied.md) ("`.Clone()` and `clone(x)` remain"); changes OWN-R6, EXP-14, LEX-7, GEN-1

## Context

Since ADR-0041 `b := a` gives `b` its own copy of everything in `a`, for every type. Every value also had a method `Clone()` and there was a built-in `clone(x)`; both did exactly what `=` does and were kept as a way to write "this copy is meant".

Two spellings for one thing invite a wrong reading: in Go and in Rust the plain form shares or moves and the `Clone` form copies. A reader who knows those languages would take `b := a` for the cheap one.

## Decision

1. **A copy is written as an assignment, an argument or a result:** `b := a`, `f(a)`, `return a`. The copy of what a pointer points to is `*p`. There is no other spelling.
2. **A value has no `Clone` method of its own, and there is no built-in `clone`.** The documents do not show either form and have no rule about them: a type has a method named `Clone` only when its author wrote one.
3. **`Clone` is the name of three things:**
   - **The copy method of a type with cleanup.** A type with a `drop` method can be copied only when its author defines `func (h T) Clone() T`. `=` calls it.
   - **The constraint.** `Clone` in a type parameter list means "can be copied"; `Copy` means "is copied by copying its bytes". Every type that can be copied meets `Clone`.
   - **The copy of a whole object through a parent pointer.** `p.Clone()` on a `*P`, a `Unique[P]` or a `Shared[P]`, where `P` has a type table, copies the complete object of the dynamic type and returns a `Unique[P]`. `*p` would give only the parent part.
4. A `.go` file is not changed: `Clone` is an ordinary name there.

## Consequences

- One way to copy. Nothing in the source can be mistaken for "the form that shares".
- A large copy is not marked in the source. `b := a` reads the same for a number and for a struct with a thousand elements; tools can show the cost of a copy next to the statement.
- Nothing special is reported for `a.Clone()` on a type without such a method: it is an unknown method like any other.
- A copy may be skipped when the program cannot notice (ADR-0041, "Copies that are not made"), so a copy method must not have effects a program relies on.

## Alternatives

- **Keep both spellings** (ADR-0041 as it was): two forms that do the same, one of them misleading.
- **A special compile error with a fix-it for `a.Clone()`:** more text in the documents and in the compiler for a form that does not exist.
- **Remove the name `Clone` entirely:** a type with cleanup would have no way to say how it is copied, generic code no way to ask for a type that can be copied, and a child could not be copied through a parent pointer.
