# ADR-0018: Pointers are never nil; `Option[T]` for "maybe"

- **Status:** accepted
- **Date:** 2026-10-28
- **Plan reference:** P01 D1.3, D1.4, D1.7 (decision change to the completed P01); spec chapters 3, 4, 6, 8, 9, 10, 11, 12, 14, 16; P03 (P03.15 to P03.18), P04 (P04.08, P04.09), P05, P07, P08, P13. Replaces the pointer parts of ADR-0016 (never-nil `@Required` pointer fields) and the `Weak[T]` `IsNil` clause of ADR-0017; replaces the `IsNil` tie-break rules of ADR-0006.

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
   - Other nilable Go types keep `nil` as in Go: interfaces (so `if err != nil` is unchanged), maps, slices, channels and funcs. (Changed by amendment point 3: funcs and interfaces with methods are never nil too; `error`, `any`, `unsafe.Pointer`, maps, slices and channels keep `nil`.)
4. **Checking.** An `Option` is usable as its value only where the compiler knows it is set:
   - after `o != nil` or `!o.IsNil()` (in the `if` body, after an early `return`/`break`/`continue` on `o == nil`, and in a `for o != nil` loop body);
   - or through `v, ok := o.Get()`; when `T` has no zero value, `v` is usable only where `ok` is true (the comma-ok rule of ADR-0016).
   Narrowing works on local variables and parameters; a field or an element is copied into a local first (`if o := c.Owner; o != nil { … }`), because it could change between the check and the use. A variable captured by a function literal is not narrowed inside the literal (spec CLO-9, OWN-P10), neither by a check outside nor inside it; it is copied into a local first. Using an unchecked `Option` as its value is a compile error ("`o` may be nil; check `o != nil` first").

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
7. **`@Required`.** On a field of a pointer kind it is a compile error ("pointers are never nil; `@Required` isn't needed"). The never-nil meaning of ADR-0016 stays for interface, func, map and channel fields, and `mem.Replace` stays the way to take a value out of a field whose type has no zero value. (Changed by amendment point 3: the never-nil meaning stays only for `error`, `any`, map and channel fields; on a func field or a field of an interface with methods `@Required` is an error, as on a pointer field.)
8. **C and C++.**
   - A C `T*` (including results, parameters and struct fields) imports as `Option[*T]`; a pointer that Clang knows can't be null (`_Nonnull`, `__attribute__((nonnull))`, `returns_nonnull`) imports as `*T`. Passing a `*T` where C takes `Option[*T]` converts automatically. C function pointers follow the same rule (`Option[ffi.Func[…]]`).
   - A C++ `T&` maps to `*T` (it can't be null); a C++ `T*` maps to `Option[*T]`; a returned `T&` is a `*T`.
9. **Moves.** Every pointer kind has no zero value, so the move-out rule of ADR-0016 applies: `move` out of a field, element or `*p` holding a `Unique[*T]` is an error; use an `Option[Unique[*T]]` field (moving out leaves `nil`) or `mem.Replace`.
10. **Failing returns.** In a `!` function, a failing return writes `nil` for a result with no zero value (`return nil, p.fail("x")` for a `*Node` result); that result is left unset, as it is after a failing `x!()`. A caller's `v, err := f()` may use `v` only where `err == nil` when `v`'s type has no zero value; this applies to every call whose type ends in `error` (direct, through a func value or an interface method), because `func!() T` and `func() (T, error)` are the same type. A Go-style `(T, error)` function not declared with `!` can't leave a result unset (`return nil, err` with a `*Node` result is an error; declare it with `!` or return `Option[*Node]`).
11. **More `Option` rules.** `Option[T]` is an error when `T` is already nilable (interfaces, maps, slices, channels, funcs) or is an `Option`. A nil `Option` converts to a nil interface and a set one boxes its `T`, so a type switch never sees an `Option`. Ownership follows `T`: `v, ok := o.Get()` borrows an owning `T`; `move(o).Get()` takes it and leaves `o` nil. In `for n := head; n != nil; n = n.Next` the post statement is narrowed too, unless the body assigns `n`. (Narrowed by amendment point 3: funcs and interfaces with methods are no longer "already nilable"; the conversion to a nil interface holds for `any` and `error`.)

## Consequences

- No nil-pointer crash is possible from Go++ pointers in safe code. Dereferences of `*T`, `Unique` and `Shared` need no run-time nil check, which codegen drops (P04).
- Go code that uses `nil` pointers (lists, trees, `if p == nil`, `var p *T`) must be rewritten with `Option`. This breaks Go source compatibility for pointers (README R1, principle 3), on purpose. Go's standard library ports (P13 D13.6) get a larger redesign list.
- The checker (P03) needs flow typing for `Option`: the type of `o` changes after a check. This moves the "known non-nil" analysis of ADR-0016 from P04 into the type checker. The existing P03 code and tests, built on nilable pointers and `Unique[T]`, must be migrated (new task P03.17).
- C APIs need more checks (every nullable result), and `_Nonnull` annotations give the short type.
- `@Required` keeps its meaning for literals, zero values, and the never-nil rule on interface, func, map and channel fields. (Changed by amendment point 3: the never-nil rule stays only on `error`, `any`, map and channel fields.)
- `Option[T]` is comparable when `T` is, so it works as a map key and as a field of a comparable struct; the runtime must hash and compare it (P05) and codegen must emit the equality (P04). See the amendment below.
- No new grammar: `Option[T]`, `Unique[*T]`, `Shared[*T]` and `Weak[*T]` are generic type syntax, so the parser (P02) is unchanged.

## Alternatives

- **Keep Go's nilable pointers** (with `@Required` for fields). Rejected: parameters and results stay unchecked.
- **A second, never-nil pointer type `&T` next to Go's `*T`.** Considered first, as the Go-compatible option; the owner chose maximum safety instead.
- **`?*T` syntax** (Swift, Kotlin). Rejected by the owner in favor of a plain generic type.
- **`Option` only for pointers.** Rejected: the owner wants one "maybe" for every type.
- **`Get()` only, without `!= nil` narrowing.** Rejected: `if p != nil` is the Go habit; all three forms are allowed.
- **C pointers as `*T` with a run-time panic on NULL.** Rejected: the compiler should force the check.
- **Option comparable only with `nil`** (the earlier wording of spec TYP-3: "an `Option[T]` is comparable only with `nil`"). Rejected in the amendment below: a struct with an `Option` field would stop being comparable, and `Option[string]` could not be a map key.
- **A narrowed copy stays an `Option`** (`p := a` after `a != nil` gives `Option[*Node]`). Rejected in the amendment below: the check would be forgotten at the copy, and the copy would need another check.
- **Interfaces and funcs keep Go's `nil`** (only pointers never nil). Rejected in the amendment below: a nil interface or func crashes on use just like a nil pointer.
- **`error` never nil too, with results written `Option[error]`.** Considered in the amendment; rejected by the owner: `var err error` and `if err != nil` are the most common Go lines, and `!` functions already hide the error type. `error` and `any` are the exceptions (amendment point 3).
- **Checking `v, err` and comma-ok values only in P04 on GIR.** Rejected in the amendment below: the P03 flow typing already answers "is it known here?", and users get the error from `gopp check` before P04 exists.

## Amendment (2026-10-28, owner decisions)

1. **`Option[T]` is comparable when `T` is comparable.** Two `Option[T]` values are equal when both are nil, or both are set and their values are equal as `T` compares them (`var a, b Option[int]; a == b`). One nil and one set are not equal. `Option[*T]` compares by address, like `*T` in Go. So `Option[T]` satisfies `comparable` when `T` does, a struct or array with `Option` fields is comparable when all its fields are, and both can be map keys (`map[Option[string]]int`). An `Option[T]` whose `T` is not comparable (`Option[struct{ s []int }]`) compares only with `nil` and is rejected as a map key or as a `comparable` type argument. Comparing an `Option[T]` with a plain `T` is allowed: `o == 5` means `o == Option[int](5)`. This follows the implicit `T` to `Option[T]` conversion (point 3), it is never ambiguous (a nil `o` is simply not equal), and it adds no new rule. Not changed: `p == nil` on a pointer is still an error.
2. **A copy of a narrowed `Option` takes the value type.** Where `o` is checked it is a `T`, so after `if a != nil { p := a }` the variable `p` is a `*Node` (also after an early exit on `a == nil`). An explicit type keeps the `Option`: `var p Option[*Node] = a`.
3. **Interfaces and funcs are never nil too; `error` and `any` are the exceptions.** A nil interface or a nil func crashes on use exactly like a nil pointer (`l.Log("x")`, `f()`), so they follow point 1:
   - Every interface type with at least one method, other than `error`, and all `func` types, always hold a value and have **no zero value**. `var l Logger`, `var f func()`, `l == nil` and `f == nil` are compile errors, and a struct with such a field has no zero value unless the field is an `Option`.
   - "Maybe" is written `Option[Logger]` and `Option[func()]`, with the same checks and narrowing as for pointers (point 4). They use the nil bit pattern of the interface or func, so they cost nothing (layout note for P04 D4.5 and P05).
   - **`error` keeps Go's `nil`.** `var err error` is legal and `if err != nil { return err }` is unchanged; `Option[error]` stays an error (point 11). This covers `error` and its aliases (`type E = error`) only: a defined `type MyErr error` and an interface that embeds `error` and adds methods (`interface{ error; Code() int }`) are interfaces with methods, so they are never nil.
   - **`any` keeps Go's `nil` too** (owner decision 2026-10-28, second round). The rule is "the empty interface": `any`, `interface{}` and every interface type whose method set is empty, named or an alias (`type Thing interface{}`), keep `nil`, because a nil one has no method to call. `var x any`, `make([]any, n)`, `m[k]` on a `map[string]any`, `x == nil` and `recover()` (which returns `any`; `panic(nil)` panics with a `*runtime.PanicNilError`, as in Go 1.21) work as in Go, and `Option[any]` is an error, like `Option[error]`. An interface with at least one method is never nil.
   - Maps, slices and channels keep Go's `nil` (a nil slice is empty, a nil map can be read, a nil channel blocks; none of them reads bad memory). Point 5 makes a nil map fully empty (a write creates it).
   - `unsafe.Pointer` keeps Go's `nil` too: it is for unsafe code and stands for a C `void*`, which may be NULL. `Option[unsafe.Pointer]` is an error (already nilable). `@Required` on an `unsafe.Pointer` field only means "must be set".
   - C function pointers already import as `Option[ffi.Func[…]]` (point 8).
   - `@Required` on a func field, or on a field of an interface with methods other than `error`, is a compile error ("never nil; `@Required` isn't needed"), as point 7 says for pointers. Its never-nil meaning stays only for `error`, `any` (empty-interface), map and channel fields.
   - This narrows point 11: funcs and interfaces with methods (other than `error`) are no longer "already nilable", so `Option[Logger]` and `Option[func()]` are valid. Point 11's conversion still holds for `any` and `error`: a nil `Option[T]` converts to a nil interface and a set one boxes its `T`, so `fmt.Println(o)` prints `<nil>` or the value. Converting to a never-nil interface `I` (one with methods) gives an `Option[I]` instead: nil when `o` is nil, the boxed `T` when it is set.
4. **Checked results and comma-ok values are part of flow typing (P03), not P04.** The narrowing of point 4 also covers:
   - `v, err := f()` for every call whose type ends in `error` (point 10), when `v`'s type has no zero value: `v` is usable only where `err == nil` is known, for example after `if err != nil { return err }` or inside `if err == nil { … }`;
   - `v, ok := m[k]`, `v, ok := <-ch`, `v, ok := o.Get()` and `v, ok := x.(T)` when the value type has no zero value (ADR-0016 point 9): `v` is usable only where `ok` is known to be true, for example after `if !ok { return }`;
   - a value stored in a never-nil `@Required` `error`, `any`, map or channel field must be known non-nil (the list is in spec TYP-15: a literal, `make(…)`, a local checked with `!= nil`, any value of a never-nil type converted to `error` / `any`, or a read of another such field); a func field or a field of an interface with methods needs no extra check: every value of that type is already non-nil.

   ```go
   type Server struct {
       Log    Logger                // never nil: must be set in every literal
       OnStop Option[func()]        // optional
   }
   u, err := load()
   if err != nil { return err }
   fmt.Println(u.Name)              // OK: err is nil here
   if f := s.OnStop; f != nil { f() }
   ```

5. **A nil slice and a nil map are empty; a nil channel stays as in Go** (owner decision, 2026-10-28). Slices, maps and channels keep `nil` (point 3), and none of them reads bad memory:
   - **Slices** already behave as empty when nil: `len(s)` is 0, `range` runs zero times, `append` works. Nothing changes.
   - **Maps**: a nil map is an empty map. Reading, `len`, `range` and `delete` work as in Go, and **the first write creates the map** instead of panicking: `var m map[string]int; m["a"] = 1` is fine. This is possible because a Go++ map has one owner (OWN-C): the write goes to that owner's header.
     - **In a place, the map is created in place.** A place is: a local or package variable; a field (of an owned value, or reached through a pointer); an element or map entry; an `&` range alias; a borrowed parameter or a borrowed range variable. Creating a nil map anywhere inside a borrowed variable (the variable itself, or a field or array element of it held by value) is the one header change a borrowed variable may make: `func f(d Doc) { d.Index["x"] = 1 }` with a nil `Index` creates the map in the caller's `Doc`. It works because borrowed owning parameters pass by hidden pointer (PAR-10) and range variables refer to the element in place. Creating an empty map doesn't change what any reader sees; the entry the write adds is an ordinary map-entry write, so the read-only rule keeps its purpose. A map reached through an interface is read-only and not a place (see the next bullet).
     - **Map inside a map:** `mm[k1][k2] = v` on a writable outer map. If `k1` is absent, the write inserts `k1` with a new inner map, then writes `k2`. If `k1` is present with a nil inner map, the inner map is created in the entry. Inserting `k1` counts as inserting a new key into `mm`, so the active-iterator rule (RNG-4) applies during a `range mm`.
     - **Not in a place: compile error, never a panic** (owner decision, 2026-10-28). Two cases:
       - *A map held in a temporary* (the result of a call that returns a value, an assertion, or a field or element of one). `f()["a"] = 1`, `f()["a"]++`, `delete(f(), "a")`, `f()["k1"]["k2"] = v`, and `f().Index["a"] = 1` where `f` returns a struct by value are compile errors, nil or not. In Go++, `f()` returns a new owned value that is dropped at the end of the statement, so the write is always lost. `f().Index["a"] = 1` where `f` returns `*Doc` is allowed (a pointer target is a place). Message: "this map is a temporary; the write is lost: store it first (`m := f(); m["a"] = 1`)".
       - *A map inside an interface.* The box of an interface value is shared and immutable (TYP-5), so a map reached through a type-switch binding or an assertion is **read-only**. Reading, `len` and `range` are allowed. `v["b"] = 2`, `v["b"]++`, `delete(v, "a")`, `clear(v)` and `x.(map[string]int)["b"] = 2` are compile errors, nil or not. The elements of a **slice** in the box are read-only for the same reason (`v[0] = 1` on a `[]int` binding is an error): the box is immutable, so what it owns is too. Inside an interface means everything the box holds **by value**, all the way down until a pointer: the map or slice itself, and the maps and slices in struct fields and array elements of the boxed value. So `v.(Doc).Index["x"] = 1`, `case Doc:` with `d.Index["x"] = 1`, and `d.Tags[0] = 1` are errors. A `*T` field in the boxed struct still gives a writable pointee. Fixes: copy it out (`m := v.Clone(); m["b"] = 2`), or store a pointer in the interface (`var x any = &myMap`, then `(*p)["b"] = 2` through `case *map[string]int`; a pointer is never nil, and the write creates a nil map in place).
       - *Rejected alternative:* "panic at run time as in Go" (rejected by the owner: a human mistake must be a compile error). So there is **no run-time nil-map panic** in Go++.
       - Task P03.19 builds these checks (E0353 a temporary map, E0354 a map or slice inside an interface). They cover every direct write form: `m[k] = v`, `m[k] op= v`, `m[k]++`, `delete`, `clear`, and for a part of a box also `copy`, `append` through a conversion that shares the box's slice, `move` of an element, and making a pointer into a part (`&v[i]`, a pointer-receiver call or method value on an element, an `&` range variable, `(*[N]T)(v)`, `unsafe.SliceData(v)`), on the binding or assertion itself, also on an Option `Get` binding of an Option in the box (spec TYP-9). A `Shared[*T]` result is a place like a `*T` one; a `Unique[*T]` result owns its target, so a write into a map there is lost too. A write through a variable or call result that stands for a part (a view made from one, a range variable over one, a helper's result), and a call that hands a part on as a borrowed argument, are closed by P03.21, where these bindings become views ([ADR-0020](0020-view-type.md): a view never converts to a non-view parameter; P04.09's "writes this parameter" bit relaxes that for callees that only read).
     - **Concurrency:** two goroutines doing the first write to the same nil map is a data race on the map header, like any unsynchronized map write (UB-11). "Creates it" does not mean thread-safe.
     - The new map uses the allocator in the map's header (SLC-2).
   - **Channels** keep Go's nil behaviour: a send or receive on a nil channel blocks forever, which `select` uses to switch a case off. An automatically created channel would let two goroutines silently create two different channels, so this is not changed. `close(ch)` of a channel the compiler **knows** is nil is a compile error (flow typing in P03, task P03.19, E0355). "Known nil" means a **local** channel variable (not a package variable, not a field, not a parameter) that is nil on every path to the `close`: declared with no value or assigned `nil`, never assigned a non-nil value on any path before the close, and not captured by a function literal or address-taken. So `var ch chan int; close(ch)` and `ch = nil; close(ch)` are errors. A package-level `var ch chan int` is not checked: `close` panics at run time, as in Go. Every other `close` of a nil channel panics as in Go.

   ```go
   var counts map[string]int        // nil: an empty map
   counts["go"]++                   // OK: creates the map
   var ch chan int
   close(ch)                        // compile error: ch is always nil here
   ```

The documents are updated in this change: spec chapters 3, 4, 6, 8, 9, 10, 11, 12 and 16, P01 D1.4, D1.7 and D1.22, and the P03, P04 and P05 plans. Task P03.18 builds the code for points 3 and 4 and migrates the existing tests, examples and explain examples.

## Amendment (2026-10-31, owner decision): a map entry passed to a borrowed map parameter is a place, also when the key is absent

**Decision.** In a call, a map entry that is passed to a borrowed map parameter (or a borrowed parameter that holds a map) is a *place*, even when its key is absent. An absent entry behaves like an entry that holds a nil map: the first write through the parameter adds the key with the new map, so the write belongs to the owner (point 5, "in a place, the map is created in place").

Owner's words: "nil map mean empty map mean accessable like Go so B".

```go
func addScore(scores map[string]int, game string, points int) { scores[game] = points }

players := map[string]map[string]int{}
addScore(players["ann"], "chess", 10)
println(len(players))             // 1
println(players["ann"]["chess"])  // 10
```

Consequences:
- **Reading never inserts.** A callee that does not write adds no key: passing `players["ann"]` to a function that only reads leaves `len(players)` at 0. The key is added at the callee's first write, not at the call.
- **A nil outer map.** `var nm map[string]map[string]int; put(nm["k"], 1)`: if `nm` is held in a place, the first write creates `nm` in place and adds the key. An outer map held in a temporary is a compile error (`f()["a"] = 1` and its family, point 5), and **passing an entry of a temporary map to a borrowed map parameter that the callee writes becomes E0353 too** (`put(f()["k"], 1)`: the write would be lost). This last case is a new rule, not an existing one: it is not an error today. It is decided by the "writes this parameter" bit (TODO P04.29 (e)); an unknown callee (a func value, an interface method, a body-less declaration) counts as writing, and a read-only callee stays accepted.
- **A nil map inside an aggregate entry** follows the same rule: with `docs` a `map[string]Doc` and an absent `"k"`, `put(docs["k"])` where the callee writes `d.Index["x"] = 1` adds `"k"` with a `Doc` whose `Index` holds the new map. A callee that creates no map adds no key. (OWN-R3b: creating a nil map anywhere inside a borrowed variable.)
- **The write is visible from the callee's first write.** A callee that writes and then panics, with the panic recovered by the caller, leaves the key and the value in place.
- Adding a key counts as an insert into the outer map for the active-iterator rule (RNG-4), as `mm[k1][k2] = v` does.

**Supersedes.** The earlier behaviour of the compiler, "an argument never inserts: the callee gets a nil map of its own and its writes are dropped with it" (TODO P04.29 (a), option A). Point 5's text is unchanged: it already lists a map entry as a place; this amendment says that an absent entry is one too. ADR-0020 has no text on this and needs no change.
