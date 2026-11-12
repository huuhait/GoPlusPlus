# ADR-0043: A pointer is used like the variable it points to; `*p = v` is not written

- **Status:** accepted (owner decision)
- **Date:** 2026-11-11
- **Plan reference:** P01.16; extends rule 7 of [ADR-0041](0041-values-are-copied.md); changes OWN-P5, OWN-P1, OWN-S2, TYP-2, PAR-1, SLC-8, EXP-17 point 7 and decision 4 of [ADR-0024](0024-operator-overloading.md)

## Context

ADR-0041 made a pointer to a slice, a map or an array usable without `*` for indexing, slicing, `range`, `len`, `cap`, `delete` and `clear`, as field access and method calls already were. The whole value was still written with the star: `*xs = append(*xs, 4)`, `*n = 9`.

So the same function needed the star on some lines and not on others, and a function changed from a by-value parameter to a pointer parameter had to be rewritten line by line. The guard of a `Mutex[T]` already worked without it (`g = append(g, 4)`).

The owner decided: the star is not written to change what a pointer points to.

## Decision

1. **Assigning a value to a pointer writes the variable it points to.** For `p` of type `*T` and `v` of type `T`: `p = v`, `p += v`, `p++`.
   ```go
   func add(xs *[]int, n *int) {
       xs = append(xs, 4)   // grows the caller's slice
       n = 9                // sets the caller's int
       n++
   }
   ```
2. **Reading needs no star either.** A `*T` used where only a `T` fits is read as the value it points to: `n + 1`, `append(xs, 4)`, `var m int = n`, `f(n)` for a parameter of type `int`, `return n` for a result of type `int`. This is rule 7 of ADR-0041 for every type and every use.
3. **Where a pointer fits, it stays a pointer,** as in Go: `q := p` is a second pointer to the same variable, `g(p)` passes the pointer to a `*T` parameter, and `p == q` on two pointers compares the pointers.
4. **Assigning a pointer to a pointer points it at the other variable,** as in Go: `n = n.next` walks a list. The type of the right side decides: a `*T` re-points, a `T` writes through. When the right side fits both (a `*any` assigned to a `*any`), it re-points.
5. **`*p` is still an expression** and reads the value: `m := *p` gives `m` a copy, and `a, b = *b, *a` swaps two values through their pointers.
6. **`*p` on the left of an assignment is a compile error (`E0469`) in a `.gopp` file:** `*p = v`, `*p += v`, `*p++`. The fix-it removes the star. A `.go` file keeps Go's spelling.
7. The same holds through `Unique[T]`. What a `Shared[T]` holds stays read-only ([ADR-0042](0042-shared-is-read-only.md)).

## Consequences

- A function body reads the same with a by-value parameter and with a pointer parameter. The signature and the `&` at the call say which variable is changed.
- Code moved from Go changes `*p = v` to `p = v`; the compiler says where. No Go statement changes its meaning: `p = v` with a `T` on the right and `p + 1` on a pointer are type errors in Go.
- `a, b = b, a` on two pointer parameters swaps the two local pointers, as in Go, and changes nothing for the caller. Swapping the values is `a, b = *b, *a`.
- An operator on a pointer to a type with `@Operator` methods now reaches the method: `p + q` and `p += v` work for a `*Matrix`. Before, they were errors (EXP-17 point 7).
- The checker resolves an assignment to a pointer by the type of the right side, before it decides copies and moves.

## Alternatives

- **Keep the star for the whole value** (ADR-0041 rule 7 as it was): two spellings in one function body.
- **A pointer that can never be re-pointed** (a reference, as in C++): no rule 4 and no ambiguity, but a loop cannot walk a list with `n = n.next`, and a struct could not change which node a field points to.
- **No star for writing, a star for reading:** `n = 9` and `n++` would work while `n = n + 1` would not.
