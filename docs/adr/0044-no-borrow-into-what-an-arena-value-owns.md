# ADR-0044: Nothing is borrowed from what an arena value owns

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** P01.16 point 16; closes open point 1 of [ADR-0041](0041-values-are-copied.md); new rule [OWN-B10](../spec/04-ownership.md#45-borrow-checking-adr-0021); changes OWN-B4, OWN-B9 rule 5, RNG-3a, UB-2; adds to the rules of [ADR-0021](0021-borrow-checking.md); related [ADR-0042](0042-shared-is-read-only.md)

## Context

Values in a `mem.Arena` point to each other with plain `*T`, and two such pointers may be the same value. The borrow checker does not tell them apart: that is the exception of OWN-B4, and it is what makes trees and graphs in an arena possible.

Since ADR-0041 a slice has one owner and no reference count. So this compiled, and wrote into freed memory:

```go
p := &a.Items[0]
b.Items = append(b.Items, 4)    // a and b are the same value: the elements move
p = 9                           // the old buffer
```

ADR-0042 closed the same hole for `Shared[T]` by making what it holds read-only. An arena value has to stay writable.

## Decision

1. **Through a pointer into an arena, nothing is borrowed from storage that the value owns separately:** the elements of a slice, the entries of a map, the value an interface holds, and what a `Unique[T]` or a `Shared[T]` points to. `p := &a.Items[0]`, a view, an `&` range variable and a pointer-receiver method call on such a part are compile errors.
2. **Reading and writing by index stays:** `a.Items[0] = 9`, `x := a.Items[0]`, `a.Items = append(a.Items, 4)`. The index is applied at the moment of the read or the write.
3. **The compiler knows a pointer into an arena by where it came from:** an arena allocation, a pointer read from memory through another pointer, or a pointer made from one of these.
4. **At a call, a fact inferred from the callee's body decides:** a function that makes such a borrow through a pointer parameter cannot be given a pointer into an arena. A call that cannot see the body counts as making one.
5. **A type that owns nothing separately is not restricted** (`Node{Kind Kind; L, R Option[*Node]}`).

The exact rule is OWN-B10.

## Consequences

- The program above does not compile; the fix-it is the index form, which always reaches the element where it is now. No write is lost without a message.
- No cost at run time, and pointers stay one word.
- **A plain `range` over a slice or map of an arena value makes a copy** when the body could change it. Before, the loop held the buffer.
- **The cost falls on arena values that own slices, maps or interfaces** (a node with `Children []*Node`): their elements are reached by index only, a method with a pointer receiver cannot be called on an element in place, and a pointer to such a value cannot be passed through an interface or a function value, so a visitor that takes a callback does not work for them. Values with only numbers, strings and pointers to other values are free of all of it.
- One more inferred fact per pointer parameter in export data. A change in a function's body can change which callers compile, as for the borrow summaries.
- Function types do not carry the fact. Letting them carry it would allow callbacks again; that is left for later.

## Alternatives

- **Keep the old buffer until the arena is reset** (the first proposal; what Go's collector gives): everything compiles, but the write through `p` goes to a buffer the value no longer uses and is lost without a message, and every part stored in an arena value would have to be allocated in that arena.
- **A pointer that remembers the slice and the index:** always reaches the right element, but every pointer becomes two words and each use a bounds check, for all code.
- **A borrow flag checked at run time** (Rust's `RefCell`): turns the compile error into a panic and adds a word to every arena value.
