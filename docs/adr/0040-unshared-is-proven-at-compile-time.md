# ADR-0040: "Unshared" is proven at compile time

- **Status:** superseded by [ADR-0041](0041-values-are-copied.md) on the day it was written: slices and maps are no longer shared, so every one of them is the only handle and the rule has nothing left to decide
- **Date:** 2026-11-11
- **Status note (2026-11-11, [ADR-0045](0045-clone-is-not-called.md)):** where decisions 1 and 2 and Consequences name a fix-it for a copy, a copy is written `b := a`.
- **Plan reference:** P01.16 point 1, P04.19, P04.21; spec OWN-R3d (removed with this ADR), TYP-6, TYP-7, STM-7, CON-1, CON-4, CON-12, CON-14; related [ADR-0022](0022-race-free-concurrency.md), [ADR-0032](0032-shared-buffers-and-borrow-engine.md)

## Context

A slice or a map is a shared handle with a reference count, as in Go: copying the handle shares the elements. Three transfers hand such a value to a holder that must be the only one: `go f(xs)`, `ch <- xs`, and storing `xs` in a kept interface. The spec said the value moves when it is "unshared (`refcount == 1`) on its last use" and that anything else is a compile error.

"Last use" is a fact of the program text. A reference count is a number at run time. The spec named a run-time number as the condition of a compile error and did not say how the compiler knows it.

Two answers were possible: prove at compile time that no other handle exists, or read the count at run time and copy when it is not 1.

## Decision

1. **Whether a value is unshared is decided at compile time.** No reference count is read to decide it. When the compiler cannot show that a local is unshared, the transfer is a compile error, and the fix-it is `.Clone()`.
2. **The rule is OWN-R3d.** In short: a local is unshared when its value is fresh (from `make`, a literal, `.Clone()`, an allocating conversion, a channel receive, or a call with a fresh result) and no second handle to it, or to a container inside it, has been made since.
3. **Across functions the compiler infers two facts, with no annotation,** and stores them in export data: a result is fresh, and a parameter is taken (the body transfers it as the only handle, so each call must pass a temporary or an unshared last use).
4. **A call that cannot be seen through has no facts:** through an interface, through a function value, or to an `@Extern` function. Its results are not fresh and its parameters are not taken. A function with a taken parameter therefore cannot be used as a function value, a method value or the implementation of an interface method.
5. **A debug build asserts that the count is 1** at each such transfer. That check finds mistakes in the compiler's proof; it decides nothing.
6. The in-place fast paths of `append`, `s += x` and `s = s[:k]` keep reading the count. They choose between two ways of doing the same thing, never between accepting and rejecting a program.

## Consequences

- A data race through a shared slice or map, and a change behind a kept interface, stay compile errors, as the spec promises. No copy is made that the source does not show.
- For nested data (`[][]int`, a map of slices) the proof costs nothing at run time. A run-time check would have to walk every element on every transfer.
- **Some safe programs are rejected.** The proof fails for a value read from a field, an element or a package variable, and across a call through an interface or a function value. There the programmer writes `.Clone()`, which copies even when nobody else held the buffer.
- **A change in a function's body can change what its callers may pass.** If a function starts sending a parameter, the parameter becomes taken, and callers that still use their argument stop compiling. Tools should show the two facts next to a function's signature.
- The move checker computes the facts to a fixed point over recursive functions and writes them to export data (P04.19); the concurrency check and the interface conversion use them (P04.21, TYP-6).
- An explicit "move if the only handle, copy otherwise" call may be added later for places where `.Clone()` is too costly. It is not part of this decision.

## Alternatives

- **Read the count at run time and copy when it is not 1** (the model of Swift's copy-on-write): accepts every program and is faster when the value happens to be the only handle, but the copy is invisible in the source, its cost depends on what a distant caller did, and nested data needs a walk of every element on every transfer. The spec promises "never a hidden deep copy".
- **Read the count at run time and panic when it is not 1:** turns a compile error into a run-time failure.
