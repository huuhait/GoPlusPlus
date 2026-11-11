# ADR-0018: Pointers, funcs and interfaces with methods are never nil; `Option[T]` for "maybe"

- **Status:** accepted (integrated with [ADR-0032](0032-shared-buffers-and-borrow-engine.md))
- **Date:** 2026-10-28
- **Plan reference:** P01 D1.3, D1.4, D1.7; spec chapters 3, 4, 6, 8, 9, 10, 11, 12, 14, 16; P03 (P03.15 to P03.19), P04 (P04.08, P04.09), P05, P07, P08, P13.

## Context

In Go, every pointer, func and interface can be `nil`, making nil dereferences the most common runtime crash in Go programs.

On 2026-10-28 the owner chose **maximum safety**: pointers, `func` types, and interfaces with methods (other than `error`) can never be nil, and "maybe nothing" is a separate predeclared generic type `Option[T]`, with Go-familiar checking (`o != nil`, `o.IsNil()`, and `v, ok := o.Get()`).

## Decision

1. **Pointers, `func` types and non-empty interfaces (except `error`) are never nil.**
   - `*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`, all `func` types, and every interface type with at least one method other than `error` always hold a valid target/value and have **no zero value** (OWN-P9, OWN-R9).
   - `var p *User`, `var f func()`, `var l Logger`, `p == nil`, `f == nil` and `l == nil` are compile errors.
   - `Unique[T]`, `Shared[T]` and `Weak[T]` take the pointee value type `T` directly (`Unique[User]`, `Shared[Config]`, `Weak[Config]`), allocated with `NewUnique(v)`, `NewShared(v)` and `NewWeak(s)` ([ADR-0032](0032-shared-buffers-and-borrow-engine.md)). A `Weak[T]` always points at its control block; whether the target is still alive is checked via `w.Lock() Option[Shared[T]]` ([ADR-0017](0017-weak-no-auto-deref.md)).
2. **`error`, `any` (empty interfaces), `unsafe.Pointer`, slices, maps and channels keep Go's `nil`.**
   - `var err error` and `if err != nil { return err }` work unchanged. (Defined interfaces with methods such as `type MyErr error` or `interface{ error; Code() int }` are never nil.)
   - `any`, `interface{}` and empty-interface aliases keep `nil` (`var x any`, `make([]any, n)`, `recover()`).
   - `Option[T]` is a compile error when `T` is already nilable (`error`, `any`, `unsafe.Pointer`, slices, maps, channels) or is itself an `Option`.
3. **`Option[T]` is the universal "maybe".**
   - Its zero value is `nil`; `o = nil` clears it. Any `T` converts to `Option[T]` automatically (`var o Option[*User] = &bob`, `var n Option[int] = 5`).
   - For pointer kinds, `func`s and interfaces with methods, `Option[T]` uses the underlying zero/null bit pattern at **zero size overhead** (`Option[*T]` has the exact 1-word layout of a nullable C pointer). For value types (`Option[int]`, `Option[User]`) it adds a tag flag.
   - `Option[T]` has exactly two methods: `IsNil() bool` and `Get() (T, bool)`.
   - `Option[T]` is comparable when `T` is comparable (`a == b`, `o == 5`, `map[Option[string]]int`).
   - Converting an `Option[T]` to `any` or `error` produces `nil` when unset and boxes `T` when set; converting to a never-nil interface `I` produces `Option[I]`.
4. **Flow narrowing (P03.18, OWN-P10).**
   - After `o != nil` or `!o.IsNil()` (in the `if` body, after an early `return`/`break`/`continue` on `o == nil`, and in a `for o != nil` loop body and post statement), a local or parameter `o` of type `Option[T]` narrows directly to `T` (so `p := o` gives `p` type `T`).
   - Fields, elements and closure-captured variables are copied into a local first (`if o := c.Owner; o != nil { … }`) so concurrent or aliased writes cannot invalidate narrowing.
   - Flow typing also covers `v, err := f()` (for any call ending in `error` when `v` has no zero value: `v` is usable only where `err == nil` is known) and comma-ok forms (`v, ok := m[k]`, `v, ok := <-ch`, `v, ok := o.Get()`, `v, ok := x.(T)`).

   ```go
   type Node struct{ Val int; Next Option[Unique[Node]] }

   func sum(head Option[*Node]) int {
       total := 0
       for n := head; n != nil; n = n.Next { total += n.Val } // n narrows to *Node
       return total
   }
   ```
5. **`@Required` interaction.** On `Option[T]` fields (and `@Required` type-parameter fields instantiated with `Option`), `@Required` is a compile error (`E0363`) because optional and required conflict. On pointer kinds, `func` fields and interfaces with methods other than `error`, `@Required` is also a compile error because those types are already never nil ([ADR-0016](0016-required-fields.md)). Its never-nil meaning applies to `error`, `any`, `map` and `chan` fields.
6. **C and C++ interop.**
   - Nullable C `T*` imports as `Option[*T]`; non-null C pointers (`_Nonnull`, `__attribute__((nonnull))`, `returns_nonnull`) import as `*T`. C function pointers import as `Option[ffi.Func[…]]`.
   - C++ references `T&` import as `*T`; C++ pointers `T*` import as `Option[*T]`.
7. **Failing returns in `!` functions.** In a `func f!() R` function, `return nil, err` leaves a no-zero-value result `R` (or a generic type-parameter result `T`) unset/zeroed when returning a non-nil error. A plain Go-style `func f() (*Node, error)` without `!` must return a real `*Node` or declare `Option[*Node]`.
8. **Nil slices, nil maps and nil channels (zero nil-map panics).**
   - **Slices:** a `nil` slice behaves as empty (`len == 0`, `append` allocates).
   - **Maps:** a `nil` map is an empty map, and **the first write to a nil map held in an addressable variable, struct field, slice element or map entry creates the map in place** (`var counts map[string]int; counts["go"]++` works without `make`). Map parameters pass by value as 1-word handles (`*[]T` and `*map[K]V` do not exist), so writing to a nil map parameter allocates only in the callee's local parameter unless returned (passing a known-`nil` map to a void/non-returning function emits a warning).
   - **Temporary and boxed maps are compile errors:** writing to a temporary map (`f()["a"] = 1`, `E0353`) or to a map/slice held by value inside an immutable interface box (`v["b"] = 2`, `E0354`) is a compile error, eliminating run-time nil-map panics completely.
   - **Channels:** `nil` channels block on send/receive as in Go (for `select`), while `close(ch)` on a local channel known to be `nil` on all paths is a compile error (`E0355`).

## Consequences

- Dereferences of `*T`, `Unique[T]`, `Shared[T]`, `func` values and interfaces with methods cannot crash with a nil-pointer dereference in safe code and require no run-time nil checks in codegen.
- Run-time nil-map panics are eliminated.
- Go code that uses nullable pointers ports cleanly to `Option[*T]`, `Option[Unique[T]]` or `Option[Shared[T]]`.

## Alternatives

- **Keep Go's nilable pointers.** Rejected: parameters, locals and results stay unchecked.
- **`?*T` punctuation syntax.** Rejected in favor of ordinary generic `Option[T]` usable with every type.
- **`Get()` only without `!= nil` narrowing.** Rejected: `if p != nil` preserves natural Go control flow.
