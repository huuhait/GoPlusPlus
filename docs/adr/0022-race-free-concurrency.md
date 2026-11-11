# ADR-0022: Race-free concurrency in normal code

- **Status:** accepted (integrated with [ADR-0032](0032-shared-buffers-and-borrow-engine.md))
- **Status note (2026-10-28, [ADR-0023](0023-inheritance.md)):** a type with parents (`@Extends`) is labeled `Send` / `Sync` from all its fields, all parents' fields included.
- **Date:** 2026-10-28
- **Status note (2026-11-11, [ADR-0041](0041-values-are-copied.md)):** the conditions under which a slice, a map or a boxed value may be passed to `go`, sent on a channel or kept in an interface (decisions 1 to 3: only on a last use with a reference count of 1, otherwise `.Clone()`) are replaced: `go f(x)`, `ch <- x` and a conversion to an interface give the other side a copy, or the value itself with `move(x)` or on its last use, with nothing more to write ([ADR-0045](0045-clone-is-not-called.md)), and `*[]T` and `*map[K]V` now exist (decision 4); an interface box has one owner and no reference count (decision 1, rule 11); `With` and `Update` work for every `T` (decision 4) and `Split(k)` returns two views, `(*[]T, *[]T)` (decision 5, open point 6); the `Send` / `Sync` labels, data-carrying mutexes, `sync.Scope`, `Atomic[T]`, the global-variable rule and the lock-order check stand.
- **Status note (2026-11-11, [ADR-0042](0042-shared-is-read-only.md)):** in decision 1, a `Shared[T]` whose `T` has plain fields is no longer writable in a single goroutine: what a `Shared[T]` holds is read-only through the handle, in one goroutine as across goroutines; `Shared[T]` and `Weak[T]` are `Send` and `Sync` when `T` is `Sync`, and the reference count is atomic exactly then.
- **Status note (2026-11-11, spec CON-15; no ADR records it):** the guard of decision 4 and example 1 has no `.Val`: while it is held, `g` is used as the guarded value itself (`g[k]--`, `g = append(g, 4)`).
- **Plan reference:** changes P01 D1.13, P12 D12.6 and D12.7; spec rules CON-12 to CON-21, CON-1 to CON-9, UB-3, UB-11, 16.4, OWN-B8, GEN-11, CLO-2; tasks P04.21 and P12.13.

## Context

Go code has two common habits that cause data races and lock bugs:

```go
x := 0
var wg sync.WaitGroup
for i := 0; i < 100; i++ {
    wg.Add(1)
    go func() { defer wg.Done(); x++ }()    // 1: unsynchronized counter race
}
wg.Wait()

var mu sync.Mutex
var stock = map[string]int{}
func take(k string) int {
    mu.Lock()
    if stock[k] == 0 { return 0 }           // 2: early return, Unlock never runs
    stock[k]--
    mu.Unlock()
    return 1
}
```

On 2026-10-28 the owner decided that **data races are compile errors in normal code**, combining automatic `Send` / `Sync` labels with data-carrying mutex guards, `sync.Scope` for local borrows, and compile-time lock-order checks, while keeping `func(...)` and `interface` `Send` by default so channels and goroutines look like ordinary Go ([ADR-0032](0032-shared-buffers-and-borrow-engine.md)).

## Decision

1. **Two automatic labels on every type (`CON-12`):** `Send` (may move to another goroutine) and `Sync` (may be shared concurrently across goroutines). Computed automatically by the compiler and usable as generic constraints (`[T Send]`, `[T Sync]`, `GEN-11`).
   - Plain scalar values, trivial structs/arrays, and immutable `string`s are both `Send` and `Sync` (heap `string` `backing` headers, `chan T`, plain `func(...)` cells and `Send`/`Sync` interface boxes always use atomic refcounts, with a relaxed `refcount == 1` fast path on final free).
   - `[]T`, `map[K]V`, `Unique[T]` and types with `drop` are `Send` when their parts are.
   - Structs and arrays get a label only if all their fields/elements have it.
   - Raw `*T` and tracked borrows (`arr[i:j]`, borrow-carrying structs, lock guards) are not `Send` across plain `go` or `chan` (inside `sync.Scope`, read-only/`Sync` targets may be borrowed, decision 5).
   - `Mutex[T]` and `RWMutex[T]` (when `T` is `Send`), `Atomic[T]`, channels, `WaitGroup`, `Once`, `context`, `sync.Pool[T]` and `sync.Map[K, V]` (when elements are `Send`) are `Sync`.
   - **`Shared[T]` and `Weak[T]` (`CON-19`):**
     - When `T` has plain unprotected writable fields (`Node{Value int, Children []Shared[Node]}`), `Shared[T]` is writable in a single goroutine (non-atomic reference count) and is **neither `Send` nor `Sync`**.
     - When `T` is synchronized (`Shared[sync.Mutex[State]]`, `RWMutex`, or a struct whose fields are all `Mutex`, `RWMutex`, `Atomic`, `chan`, `WaitGroup`, `Once`), `Shared[T]` and `Weak[T]` are `Send` and `Sync` (atomic reference count).
   - **Interfaces (`Send` by default):** Every ordinary interface (`any`, `io.Reader`, `Handler`, etc.) is **`Send` by default**, so `chan any`, `chan io.Reader`, `Mutex[io.Reader]` and `go serve(r)` (on unshared last use) work directly. Kept plain interfaces check that the boxed dynamic type is `Send` (non-keeping interface parameters like `fmt.Println(x)` borrow any type, `TYP-7`; boxing a mutable `[]T`/`map[K]V` into a kept interface moves it on unshared `refcount == 1` last use and requires explicit `.Clone()` if still used or aliased). An interface that embeds `Sync` (including predeclared `error`) is both `Send` and `Sync` (also accepting `Send` value structs holding mutable slices/maps on unshared last use or `.Clone()`, since the box freezes them as immutable).
   - **Func values (`Send + Sync` by default):** Plain `func(...)` types are **`Send` and `Sync` by default**, so `chan func()`, `go cb()`, `http.HandleFunc` and `Mutex[[]func()]` work with ordinary Go syntax. Escaping closures converted to plain `func(...)` must have `Sync` captures (`CLO-2`), while non-escaping closures (`slices.SortFunc`, `mu.With`, `defer`, `CLO-1`) and direct `go func(){...}()` statements (`CON-13`) allow local mutation with zero annotations.
   - **Single-goroutine opt-out (`Local[T]`):** `Local[func(...)]` and `Local[I]` opt out of `Send`/`Sync` (non-atomic refcounts) for single-goroutine UI/event callbacks that mutate shared local cells (`count++`) or single-goroutine interfaces holding unsynchronized `Shared[T]`. Plain `func(...)` / `I` convert freely to `Local[...]`, never the reverse.

   ```go
   func worker(jobs chan func(), events chan any) {
       mail := "hello"
       jobs <- func() { println(mail) }    // ok: plain chan func() works!
       events <- mail                      // ok: plain chan any works!
   }
   ```
2. **`go` statement (`CON-13`).** `go f(args)` and `go func(){…}()` may pass or capture:
   - values moved in on their last use (the spawner never touches them afterwards);
   - `Send` `Copy` values copied in;
   - `Sync` values shared across goroutines.
   Capturing a writable variable that the spawner, another goroutine, or a later loop iteration can still touch after the `go` is a compile error (suggesting `Mutex[T]`, `Atomic[T]` or `chan`). A variable with no use after the `go` counts as moved in (`y := 0; go func() { y++; println(y) }()` is accepted).
3. **Channel send (`CON-14`).** `chan T` requires `T` to be `Send`. Sending copies `Copy` values, shares `Sync` handles (`string`, `func(...)`, `error`, `chan`, synchronized `Shared[T]`), and moves single-owner or mutable `[]T`/`map[K]V`/plain-interface handles when unshared (`refcount == 1`) on last use (requiring explicit `.Clone()` if still used or aliased by the sender).
4. **`Mutex[T]` and `RWMutex[T]` hold the data (`CON-15`).**
   - `m.Lock()` returns a non-`Copy`, non-`Send` write guard `g` exposing `g.Val` (plus struct field/method auto-deref when `T` is a struct) that unlocks automatically on drop (scope exit, early return, panic). `m.Update(func(v T) T)` updates any `T` (including slices and maps, without `*[]U` or `*map[K]V`), and `m.With(func(p *T) {...})` borrows `*T` for non-slice, non-map `T`.
   - `RWMutex[T].RLock()` returns a deep read-only guard (`OWN-R3b`).
   - `Mutex[struct{}]` is the lock-only form. Plain `sync.Mutex`, `sync.RWMutex` (without data) and manual `Unlock` are removed. Mutexes are `@Pinned`.
5. **`sync.Scope` (`CON-16`).** Goroutines started with `s.Go(...)` finish before `sync.Scope` returns and may borrow locals safely under `OWN-B1`–`OWN-B9`. Disjoint writable slice parts use `left, right := nums.Split(500)`.
6. **`Atomic[T]` (`CON-17`).** For integer types, `bool` and `unsafe.Pointer`; sequentially consistent only; `Sync`. Free `atomic.AddInt64(&x, 1)` functions on plain variables are removed.
7. **Global variables (`CON-18`).** Any package-level variable written outside `init` and its initializer must have a `Sync` type (`Atomic[T]`, `Mutex[T]`, …). Read-only globals after `init` are allowed for any type.
8. **Compile-time deadlock check (`CON-20`).** Locks are grouped by lock class (field of type or package variable). Taking two classes in opposite orders across visible functions, locking the same mutex twice while live, or locking two instances of the same class without `sync.LockAll(&a.mu, &b.mu)` is a compile error.
9. **What stays unchecked (`CON-21`):** multi-section logic races, dynamic/interface lock-order deadlocks, and `unsafe` / C / C++ threads (`-race` / TSan covers the residue, `UB-3`).

## Examples

### 1. Atomic and data-carrying Mutex

```go
var hits sync.Atomic[int]
go func() { hits.Add(1) }()

var stock = sync.NewMutex(map[string]int{})
func take(k string) int {
    g := stock.Lock()
    if g.Val[k] == 0 { return 0 } // guard drops and unlocks automatically
    g.Val[k]--
    return 1
}
```

### 2. Parallel slice fill with `sync.Scope` and `Split`

```go
nums := make([]int, 1000)
sync.Scope(func(s *sync.Scope) {
    left, right := nums.Split(500)
    s.Go(func() { for i := range left  { left[i] = i } })
    s.Go(func() { for i := range right { right[i] = i } })
})
```

## Consequences

- Data races and forgotten `Unlock` calls become compile errors in normal Go++ code.
- `chan func()`, `chan any`, `http.HandleFunc` and goroutine workers keep clean, idiomatic Go signatures, while `Local[func()]` / `Local[any]` covers single-goroutine UI/event state.

## Alternatives

- **Leave data races to `-race` only.** Rejected: schedule-dependent races slip past tests.
- **Require explicit `Send` wrappers on channels and `go`.** Rejected: makes everyday `chan func()` and `chan any` Go code noisy; `Send` by default with `Local[T]` opt-out matches 99% of Go concurrency with zero wrappers.
- **Pony-style reference capabilities or actors-only.** Rejected: too far from Go.
