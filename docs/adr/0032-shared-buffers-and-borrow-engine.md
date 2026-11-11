# ADR-0032: Shared reference-counted buffers and compile-time borrow engine

- **Status:** accepted
- **Date:** 2026-11-11
- **Status note (2026-11-11, [ADR-0041](0041-values-are-copied.md)):** the shared, reference-counted buffers of `[]T` and `map[K]V` described here are replaced: a slice or a map is a value with one owner, and `t := s` is a copy. Also replaced: `*[]T` and `*map[K]V` are ordinary pointer types (decision 2); `s[i:j]` on a slice or an array is a copy, and the borrowed part is the view `&s[i:j]` (decisions 2 and 5); an interface value owns its box, with no reference count (decision 1); a by-value parameter is the function's own copy, made by the caller and skipped when the function only reads it, in place of lending and the callee's retain (decision 3); a value that cannot be copied is handed over with `move(x)` and a `Unique[T]` is copied, in place of the automatic move (decisions 1 and 4); `go f(x)` and `ch <- x` give the other side a copy (decision 6); and `f(s []int)` changes its own copy, which reverses the reason given under Alternatives. The borrow engine and the string rules stand.
- **Status note (2026-11-11, [ADR-0042](0042-shared-is-read-only.md)):** a `Shared[T]` is no longer writable directly in a single goroutine: what it holds is read-only through the handle, and a part that changes is in a `Mutex`, `RWMutex` or `Atomic`, in one goroutine as across goroutines.
- **Plan reference:** integrates with [ADR-0003](0003-memory-model-direction.md), [ADR-0016](0016-required-fields.md), [ADR-0018](0018-never-nil-pointers-and-option.md), [ADR-0021](0021-borrow-checking.md), [ADR-0022](0022-race-free-concurrency.md), [ADR-0026](0026-no-small-string-optimization.md) and [ADR-0029](0029-slice-and-map-allocator-word.md); `docs/spec/` chapters 01 to 16, `appendix-a-symbol-names.md`, `docs/attributes.md`, `docs/design/` (`gir.md`, `calling-convention.md`, `debug-info.md`) and `docs/spec/examples/`.

## Context

Go++ targets deterministic, pause-free memory management without a garbage collector while keeping everyday code as clean and composable as Go. To achieve both without forcing lifetime annotations, ownership keywords on parameters, or manual `.Clone()` calls onto ordinary string, slice and struct code, the ownership and concurrency model is built around reference-counted shared buffers paired with compile-time borrow lending, last-use move elision, and flow-based `*T` borrow checking.

## Decision

1. **Pure Go parameter and expression syntax across three ownership categories (`OWN-C1`, `OWN-C2`, `OWN-C3`).**
   - **`OWN-C1` (Trivial / `Copy`):** scalars, raw `*T`, `unsafe.Pointer`, and structs/arrays of trivial parts copy bitwise.
   - **`OWN-C2` (Shared / value handles):** `string`, `[]T` and `map[K]V` (when elements are not single-owner), `Shared[T]`, `Weak[T]`, `chan T`, `interface` values, `func` values, `Local[T]`, and structs/arrays/`Option`s of them share reference-counted backing storage.
   - **`OWN-C3` (Single-owner):** `Unique[T]`, types with a `drop()` method (`File`, `Conn`), `@Pinned` types, non-copyable C++ classes, and composites containing them have a single owner and move automatically on by-value transfer.
   - Explicit extraction from an addressable non-local slot uses library functions in package `mem`: `mem.Take(&place)` (writes the zero value) and `mem.Replace(&place, v)` (writes `v` and works for every movable type).

2. **`string`, `[]T` and `map[K]V` use Go-style shared headers (`OWN-C2`).**
   - **`string` (3 words: `{ptr, len, backing}`):** immutable bytes backed by an atomic reference-counted heap allocation `{refcount, byte_cap}` (plus a trailing NUL byte), or `backing == nil` for static literals and empty strings (where retain and release are no-ops). Substring slicing `s[i:j]` is $O(1)$ (`{s.ptr + i, j - i, s.backing}`) with zero byte copies.
   - **`[]T` (4 words: `{ptr, len, cap, backing}`):** Go-style slice header sharing a reference-counted backing array whose heap header holds `{refcount, alloc_cap, allocator}`. Assigning `b := a`, passing `f(s)` and slicing `s[i:j]` share the backing array in $O(1)$ (including `s = s[1:]`), and element writes (`s[0] = x`) mutate the shared backing array just as in Go. Because `backing` carries the allocator ([ADR-0029](0029-slice-and-map-allocator-word.md)), passing or slicing a slice preserves its arena or custom allocator. Slicing a value array `arr[i:j]` produces `{ptr: &arr[i], len: j - i, cap: j - i, backing: nil}` tracked by the borrow checker as a borrow of `arr` (allowed for non-keeping parameters, rejected on keeping or indirect calls; `cap == len` ensures `append` allocates a fresh heap buffer).
   - **`map[K]V` (1 word: pointer to heap map header):** Go-style shared map header whose heap header holds `{refcount, allocator, ...}`. Assigning `b := a` and passing `f(m)` pass the 1-word handle by value and share the map, and `m[k] = v` mutates entries directly as in Go. Writing to a nil map held in an addressable variable/field/element initializes it in place (`*[]T` and `*map[K]V` are compile errors everywhere because `[]T` and `map[K]V` are already reference handles).
   - **`x.Clone()` (`clone(x)`)** creates an explicit deep/independent copy when a caller wants a separate slice, map, struct or `Unique[T]`.

3. **Compile-time borrow lending and last-use move elision keep `OWN-C2` fast.**
   - **Caller lends, saver retains (`PAR-1`):** Passing an `OWN-C2` argument to a parameter or receiver lends it with **zero reference-count operations** whenever the argument comes from an unaliased local binding. Only if the callee stores, returns or sends the value does the callee retain it. If an argument is read from a mutable/aliasable place (a slice element, map entry, dereferenced pointer, or a local also borrowed via `&` in the same call), the caller holds a temporary retain across the call so callee mutations (`u.SetName(u.Name)`, `s.fail(s.err)`) can never free the lent buffer mid-call.
   - **Last-use auto-move (`OWN-R3`):** When a local `OWN-C2` variable has no later read after a transfer (`b := a`, `return a`, `s = append(s, a)`, `ch <- a`, `T{F: a}`, or passing to a consuming parameter), the compiler transfers ownership directly without retaining and releasing.
   - **API stability:** Editing a function body to start or stop keeping an `OWN-C2` parameter never breaks callers at compile time.

4. **Explicit pointer kinds (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`) with `NewUnique` / `NewShared` / `NewWeak`.**
   - **`*T` (`&x`)** is always a temporary tracked borrow (1 word, never nil, zero heap allocation, zero reference count).
   - **`Unique[T]` (`NewUnique(v)`)** is a single-owner heap pointer (`OWN-C3`, 1 word, zero reference count). Passing it to `*T` borrows the target (`OWN-P6`); passing or assigning `Unique[T]` by value moves it automatically (`E0451` on use after move).
   - **`Shared[T]` (`NewShared(v)`)** is a multi-owner reference-counted heap pointer (`OWN-C2`). Like `[]T` and `map[K]V`, `Shared[T]` is **directly writable in a single goroutine** (`p.Value = 99`, `p.Children = append(p.Children, c)`) using fast non-atomic reference counts (`T` holds `OWN-C1`/`OWN-C2` fields or `Mutex`-guarded `OWN-C3` state, and interior borrows `&s.Slice[i]` retain temporary backing references). Across goroutines (`go` / `chan`), `Shared[T]` is `Send` and `Sync` (atomic reference counts) when `T`'s fields are synchronized (`Mutex`, `RWMutex`, `Atomic`, `chan`, `WaitGroup`, `Once`; `CON-19`).
   - **`Weak[T]` (`NewWeak(s)`)** is a non-owning back-reference to `Shared[T]` (`w.Lock()` returns `Option[Shared[T]]`) used to break `Parent <-> Child` reference cycles.

5. **The borrow checker focuses on `&` (`*T`), stack-array slices, `range`, arenas, guards and `sync.Scope`.**
   - Plain value code without `&` needs no borrow checking at call sites (`w := s[0:5]; s = "bye"; println(w)` and `v := nums[1:3]; nums = append(nums, 4); println(v[0])` work as in Go because `w` and `v` retain the old backing buffer).
   - When code takes `&` (`p := &nums[0]`), slices a stack/value array (`arr[:]`), ranges over a collection, allocates in a `mem.Arena`, or holds a `Mutex` guard, `OWN-B1` to `OWN-B9` enforce non-dangling lifetimes and exclusive writable `*T` access.

6. **Race-free concurrency (`ADR-0022`) with `func` and `interface` `Send` by default and `Local[T]` opt-out.**
   - `string` has immutable bytes and is `Send` and `Sync` (atomic reference count when shared across goroutines).
   - Mutable shared-header containers (`[]T`, `map[K]V`) are not shared across goroutines for concurrent unsynchronized mutation: passing or capturing one in `go` or sending on `chan` moves it when unshared on last use ($O(1)$), and requires `.Clone()` (or `Mutex[T]` / `sync.Scope` + `Split`) if the sender still uses it or holds another alias.
   - **Plain `func(...)` (`Send + Sync` by default) and plain `interface` (`Send` by default):** `chan func()`, `chan any`, `chan io.Reader`, `go cb()` and `go serve(r)` work with ordinary Go syntax. Escaping plain `func(...)` values require `Sync` captures (`CLO-2`), while non-escaping closures (`CLO-1`) and direct `go func(){...}()` (`CON-13`) allow local variable mutation with zero annotations. Kept plain interfaces (`TYP-6`) require a `Send` dynamic type (`error` and `Sync`-embedding interfaces require `Send + Sync`, or `Send` value containers moved in unshared on last use / `.Clone()`), moving mutable `[]T`/`map[K]V` on unshared `refcount == 1` last use and requiring explicit `.Clone()` if still used or aliased, while non-keeping interface parameters (`TYP-7`: `fmt.Println`) borrow any value at zero copy.
   - **`Local[T]` (`Local[func()]`, `Local[any]`, `Local[Widget]`):** opts out of `Send`/`Sync` for single-goroutine callbacks that mutate shared local cells or single-goroutine interfaces boxing unsynchronized `Shared[T]`.

7. **Struct fields use Go defaults plus `@Required` and `Option[T]`.**
   - Plain struct fields default to their Go zero value when omitted in composite literals, `@Required` marks fields that must be explicitly set ([ADR-0016](0016-required-fields.md)), and `Option[T]` marks nullable/optional fields ([ADR-0018](0018-never-nil-pointers-and-option.md)). Combining `Option[T]` and `@Required` on the same field (`Name Option[T] @Required`) is a compile error (`E0363`).

## Consequences

- **Go-simple surface:** Samples in `docs/spec/examples/` (`02-wc`, `03-kvparse`, `05-workers`, `08-derive`, `09-orderedmap`, `10-minicalc`) read like ordinary Go, while `04-binarytrees` uses `Unique[Node]`, `NewUnique` and `mem.Arena`.
- **Stable APIs:** Functions taking `string`, `[]T`, `map[K]V` or ordinary structs have stable call-site validity regardless of internal implementation changes.
- **Fast non-GC execution:** Zero GC pauses; immediate deterministic destruction when reference counts reach zero; zero refcount traffic on lent parameters and last-use moves; $O(1)$ slicing on strings and slices; preserved custom allocators across slice calls and sub-slices.

## Alternatives

- **Copy-on-Write value slices and maps (`f(s []int)` cannot mutate elements in place):** Rejected so `f(s []int)` and `f(m map[K]V)` mutate elements directly without `&`, matching Go.
- **Collapse `Unique[T]` / `Shared[T]` / `Weak[T]` into implicit `*T`:** Rejected so single-owner vs shared vs weak heap ownership stays explicit in data structures.
