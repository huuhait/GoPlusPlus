# ADR-0018: Pointers are never nil; `Option[T]` for "maybe"

- **Status:** accepted
- **Date:** 2026-10-28
- **Plan reference:** P01 D1.3, D1.4, D1.7 (decision change to the completed P01); spec chapters 3, 4, 6, 10, 14, 16; P03, P04, P05, P07, P08, P13. Replaces the pointer parts of ADR-0016 (never-nil `@Required` pointer fields) and the `Weak[T]` `IsNil` clause of ADR-0017; replaces the `IsNil` tie-break rules of ADR-0006.

## Context

In Go, and in Go++ until now, every pointer can be `nil`. A function that takes a `*User` can't know that it got a real `User`, and a nil pointer is the most common crash in Go programs. ADR-0016 made `@Required` pointer fields never nil, which covered struct fields but not parameters, results or locals.

In a review of the pointer design (2026-10-28), the owner chose **maximum safety** over Go source compatibility for pointers: pointers can never be nil, and "maybe" is a separate type, as in Rust, Swift, Kotlin and Carbon. The owner also chose:

- the generic name `Option[T]` (not new syntax such as `?*T`), for **any** type, not only pointers;
- the spelling `Unique[*T]`, `Shared[*T]`, `Weak[*T]`, so every pointer type shows a `*`;
- all three ways of checking an `Option`: `!= nil`, `IsNil()`, and the comma-ok `Get()`;
- C pointers imported as `Option[*T]`, checked by the compiler.

## Decision

1. **Pointers are never nil.** `*T`, `Unique[*T]`, `Shared[*T]` and `Weak[*T]` always hold a target. They have **no zero value**, so they join the types of P01 D1.4 rule 9 (`var p *User` is an error, a struct with a pointer field has no zero value unless the field is an `Option`, and so on, exactly as for `@Required` structs). `p == nil` on a pointer is a compile error ("`*User` is never nil"). A `Weak[*T]` always points at its target's control block; whether the target is still alive is asked with `Lock()`.
2. **New spelling.** The owning pointers take the pointer type as their argument: `Unique[*User]`, `Shared[*Config]`, `Weak[*Config]`. `Unique[User]` is an error with the fix-it `Unique[*User]`. `new_unique(v)` returns `Unique[*T]`, `new_shared(v)` returns `Shared[*T]`, `new_weak(s)` returns `Weak[*T]`.
3. **`Option[T]`, for any type `T`, is the only "maybe".**
   - Its zero value is `nil`; `o = nil` empties it. A `T` converts to `Option[T]` automatically (`var o Option[*User] = &bob`, `var n Option[int] = 5`).
   - For the pointer kinds, `Option` costs nothing: `nil` uses the pointer's own empty bit pattern, so `Option[*T]` has the size and layout of a C pointer. For other types it adds a hidden "is set" flag.
   - `Option[T]` has exactly two methods, `IsNil()` and `Get() (T, bool)`. It has no fields and no other methods: the value's own fields and methods are reached only after a check.
   - Other nilable Go types keep `nil` as in Go: interfaces (so `if err != nil` is unchanged), maps, slices, channels and funcs.
4. **Checking.** An `Option` is usable as its value only where the compiler knows it is set:
   - after `o != nil` or `!o.IsNil()` (in the `if` body, after an early `return`/`break`/`continue` on `o == nil`, and in a `for o != nil` loop body);
   - or through `v, ok := o.Get()`, where `v` is usable only where `ok` is true (the comma-ok rule of ADR-0016).
   Narrowing works on local variables and parameters; a field or an element is copied into a local first (`if o := c.Owner; o != nil { … }`), because it could change between the check and the use. Using an unchecked `Option` as its value is a compile error ("`o` may be nil; check `o != nil` first").

   ```go
   type Node struct{ Val int; Next Option[*Node] }

   func sum(head Option[*Node]) int {
       total := 0
       for n := head; n != nil; n = n.Next { total += n.Val }   // n is a *Node in the body
       return total
   }

   func greet(u Option[*User]) {
       if u == nil { return }
       fmt.Println(u.Name)                // u is a *User after the early return
   }
   ```
5. **`IsNil()` exists only on `Option`.** The pointer kinds have no `IsNil` method, so the `IsNil` and `Clone` tie-break rules for pointers (OWN-P9, ADR-0006) are reduced to `Clone` alone; there is no clash, because an `Option` has no methods of `T` before it is checked.
6. **`Weak[*T]`** has no auto-dereference (ADR-0017). `w.Lock()` returns `Option[Shared[*T]]`: `if c := w.Lock(); c != nil { c.Name }`.
7. **`@Required`.** On a field of a pointer kind it is a compile error ("pointers are never nil; `@Required` isn't needed"). The never-nil meaning of ADR-0016 stays for interface, func, map and channel fields, and `mem.Replace` stays the way to take a value out of a field whose type has no zero value.
8. **C and C++.**
   - A C `T*` (including results, parameters and struct fields) imports as `Option[*T]`; a pointer that Clang knows can't be null (`_Nonnull`, `__attribute__((nonnull))`, `returns_nonnull`) imports as `*T`. Passing a `*T` where C takes `Option[*T]` converts automatically. C function pointers follow the same rule (`Option[ffi.Func[…]]`).
   - A C++ `T&` maps to `*T` (it can't be null); a C++ `T*` maps to `Option[*T]`; a returned `T&` is a `*T`.
9. **Moves.** Every pointer kind has no zero value, so the move-out rule of ADR-0016 applies: `move` out of a field, element or `*p` holding a `Unique[*T]` is an error; use an `Option[Unique[*T]]` field (moving out leaves `nil`) or `mem.Replace`.
10. **Failing returns.** In a `!` function, a failing return writes `nil` for a result with no zero value (`return nil, p.fail("x")` for a `*Node` result); that result is left unset, as it is after a failing `x!()`. A caller's `v, err := f()` may use `v` only where `err == nil`; this applies to every call whose type ends in `error` (direct, through a func value or an interface method), because `func!() T` and `func() (T, error)` are the same type. A Go-style `(T, error)` function not declared with `!` can't leave a result unset (`return nil, err` with a `*Node` result is an error; declare it with `!` or return `Option[*Node]`).
11. **More `Option` rules.** `Option[T]` is an error when `T` is already nilable (interfaces, maps, slices, channels, funcs) or is an `Option`. A nil `Option` converts to a nil interface and a set one boxes its `T`, so a type switch never sees an `Option`. Ownership follows `T`: `v, ok := o.Get()` borrows an owning `T`; `move(o).Get()` takes it and leaves `o` nil. In `for n := head; n != nil; n = n.Next` the post statement is narrowed too, unless the body assigns `n`.

## Consequences

- No nil-pointer crash is possible from Go++ pointers in safe code. Dereferences of `*T`, `Unique` and `Shared` need no run-time nil check, which codegen drops (P04).
- Go code that uses `nil` pointers (lists, trees, `if p == nil`, `var p *T`) must be rewritten with `Option`. This breaks Go source compatibility for pointers (README R1, principle 3), on purpose. Go's standard library ports (P13 D13.6) get a larger redesign list.
- The checker (P03) needs flow typing for `Option`: the type of `o` changes after a check. This moves the "known non-nil" analysis of ADR-0016 from P04 into the type checker. The existing P03 code and tests, built on nilable pointers and `Unique[T]`, must be migrated (new task P03.17).
- C APIs need more checks (every nullable result), and `_Nonnull` annotations give the short type.
- `@Required` keeps its meaning for literals, zero values, and the never-nil rule on interface, func, map and channel fields.
- No new grammar: `Option[T]`, `Unique[*T]`, `Shared[*T]` and `Weak[*T]` are generic type syntax, so the parser (P02) is unchanged.

## Alternatives

- **Keep Go's nilable pointers** (with `@Required` for fields). Rejected: parameters and results stay unchecked.
- **A second, never-nil pointer type `&T` next to Go's `*T`.** Considered first, as the Go-compatible option; the owner chose maximum safety instead.
- **`?*T` syntax** (Swift, Kotlin). Rejected by the owner in favor of a plain generic type.
- **`Option` only for pointers.** Rejected: the owner wants one "maybe" for every type.
- **`Get()` only, without `!= nil` narrowing.** Rejected: `if p != nil` is the Go habit; all three forms are allowed.
- **C pointers as `*T` with a run-time panic on NULL.** Rejected: the compiler should force the check.
