# 12. Concurrency

Base: Go spec [Go statements](https://go.dev/ref/spec#Go_statements), [Channel types](https://go.dev/ref/spec#Channel_types), [Send statements](https://go.dev/ref/spec#Send_statements), [Receive operator](https://go.dev/ref/spec#Receive_operator), [Select statements](https://go.dev/ref/spec#Select_statements), [Close](https://go.dev/ref/spec#Close), [Program execution](https://go.dev/ref/spec#Program_execution). Memory model: [Go memory model](https://go.dev/ref/mem).

Delta D1.13, extended by ADR-0022. `go`, `chan` and `select` work as in Go. What is new is what happens to **owned values** and to **borrows**, and that **data races are compile errors in normal code** (the labels `Send` and `Sync`, CON-12 to CON-21). `sync.Mutex` and `sync/atomic` functions on plain variables are replaced by `Mutex[T]` and `Atomic[T]`.

Syntax: no change.

## 12.1 Rules

**CON-1.** *`go f(a, b)`.* The function and its arguments (the receiver counts as an argument) are evaluated, then given to the new goroutine. Every argument must satisfy the race-safety rules of [CON-12, CON-13](#121-rules):
- **trivial (`OWN-C1`)** values (without raw pointers) are copied;
- **immutable / `Sync` shared handles (`OWN-C2`)** (`string`, `chan T`, `func(...)`, `error` and `Sync` interfaces, `Shared[T]`/`Weak[T]` when `T` is synchronized, and structs of them) are shared (using atomic retain/release when shared across goroutines, or transferred at 0 retain/release on unshared last use);
- **mutable-container or `Send`-only `OWN-C2` values** (`[]T`, `map[K]V`, plain non-`Sync` interfaces such as `any` / `io.Reader`, or structs holding them, with `Send` elements) **move in $O(1)$** when passed from a temporary or an unshared local on its last use (`ys := []int{1, 2}; go sum(ys)`, `go serve(conn)`), and are a **compile error** if the spawner still uses the variable afterwards or holds another alias (fix-it: `ys.Clone()`, `Mutex[T]`, `sync.Scope` + `Split`, or `Sync` on the interface), preventing concurrent unsynchronized mutation ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md));
- **single-owner (`OWN-C3`) temporaries and locals** (whose type is `Send`) move in (`go run(u)` moves `u`; later use of `u` is `E0451`);
- **non-local `OWN-C3` places** (`p.file`), **read-only borrowed variables** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14): e.g. a plain range variable over struct or single-owner elements), and `&` range variables ([RNG-1a](08-slices-and-range.md)) are a **compile error**: a borrow cannot outlive its owner across `go` (fix-it: `u.Clone()`, or `mem.Take(&p.file)` for a zeroable slot);
- a **raw pointer `*T` or stack-array slice `arr[i:j]`** into a local (or into the target of a local `Unique`) is a compile error ([CON-2](12-concurrency.md)).

```go
for _, r := range resources { // r borrows a single-owner Resource (OWN-C3)
    go fetch(r)               // error: borrowed range variable. Fix-it: go fetch(r.Clone())
}
go log(cfg.Name)              // ok: string is immutable (Send + Sync), retained atomically!
ys := []int{1, 2}
go sum(ys)                    // ok: ys is unshared on its last use, moved in O(1)
```

**CON-2.** *No borrows of locals across `go`.* This is the `go` case of [OWN-B5](04-ownership.md#45-borrow-checking-adr-0021): any tracked borrow in a `go` argument or capture is an error, because the goroutine may outlive the owner. A raw pointer `*T` or a stack-array slice `arr[i:j]` into a local, or into the target of a local `Unique`, passed to `go` is a **compile error** outside `unsafe`. Example: `go srv.Run()` with a pointer receiver on a local `srv` or `Unique[Server]` is an error. Fix-it: `go func(){ srv.Run() }()`, whose captured variables live in shared cells ([CON-3](#121-rules)), if the captured variable passes [CON-13](#121-rules); or run the goroutine inside a `sync.Scope`, where borrows are allowed ([CON-16](#121-rules)). `s.Go(...)` of a scope is not a `go` statement, so this rule and OWN-B5 do not apply to it.

**CON-3.** *Closures with `go`.* A closure used with `go` keeps its captured variables in shared cells ([CLO-3](11-closures.md)), so a cell outlives the spawner's block. What the closure may capture is decided by [CON-13](#121-rules): a variable that is moved in (nobody else touches it after the `go`), a `Send` value that is copied in, or a `Sync` value that is shared (`string`, `Mutex[T]`, `Atomic[T]`, a channel, a `WaitGroup`). Capturing a **writable variable** (or mutable `[]T`/`map[K]V`) that the spawner or another goroutine can still touch is a compile error (in Go it compiles and is a data race). A **borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) cannot be captured this way ([CLO-3a](11-closures.md)): `go func(){ fetch(u) }()` with a borrowed range variable `u` is a compile error (fix-it: `mine := u.Clone(); go func(){ fetch(mine) }()`).

**CON-4.** *Channels transfer values safely.* Sending transfers the value into the channel ([CON-14](#121-rules): the element type must be `Send`, which includes plain `func(...)`, `any` and ordinary interfaces): `OWN-C1` copies, immutable `string` and `Sync` `OWN-C2` handles (`func(...)`, `error`, `chan T`, synchronized `Shared[T]`) share (atomic refcount) or move on last use, mutable/non-`Sync` `OWN-C2` handles (`[]T`, `map[K]V`, plain `any` / `io.Reader`) move in $O(1)$ on unshared last use (requiring `.Clone()` or `Sync` if still used or aliased by the sender), and `OWN-C3` moves in. Receiving transfers the value out, including `for v := range ch`. A channel, and the values still buffered in it, are freed automatically when nobody holds the channel any more (P12 D12.5). The buffer is owned by the channel, which is a shared handle ([OWN-R1](04-ownership.md#42-ownership-rules-d14), [OWN-C2](04-ownership.md#41-type-categories-d13)); buffered values are dropped oldest first. There is no explicit `ch.Destroy()`: forgetting it would leak, and calling it early would crash. When goroutines must finish before the channel goes away, use `sync.Scope` ([CON-9](#121-rules)).

**CON-5.** *`select`.* A send case transfers its value only if that case is **chosen**. A temporary send value is dropped if its case is not chosen. A receive case receives as in CON-4. Random choice among ready cases is as in Go.

**CON-6.** *Receive from a closed channel* gives the zero value. For a type with no zero value (a pointer kind, a func, an interface with methods other than `error`, a C++ type without a default constructor, a struct with a `@Required` field), a receive is allowed only in the comma-ok form (`v, ok := <-ch`, or `case v, ok := <-ch:` in a `select`); `v := <-ch`, `case v := <-ch:` and `(<-ch).f` are compile errors. The comma-ok form works, with `v` usable only where `ok` is true ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).

**CON-6a.** *Nil channels.* A nil channel works as in Go: a send or receive on it blocks forever, so a `select` case on a nil channel never fires. `close(ch)` of a channel the compiler knows is nil is a compile error. "Known nil" means a **local** channel variable (not a package variable, not a field, not a parameter) that is nil on every path to the `close`: declared with no value or assigned `nil`, never assigned a non-nil value on any path before the close, and not captured by a function literal or address-taken. So `var ch chan int; close(ch)`, `ch = nil; close(ch)` and `defer close(ch)` of such a variable are errors. A named result counts as a parameter, and a variable that a function literal uses at all counts as captured. A `close` after the channel is checked (`if ch != nil`), in a loop that assigns the variable, or after a `goto` label is not reported. A package-level `var ch chan int` is not checked: `close` panics at run time, as in Go. Every other `close` of a nil channel panics as in Go (ADR-0018 amendment point 5).

**CON-7.** *Memory model.* Go's happens-before model. A channel send happens before the matching receive completes; `sync` types give the usual edges: an unlock of a `Mutex[T]` (guard drop) happens before the next lock; `Atomic[T]` operations are sequentially consistent ([CON-17](#121-rules)); the end of `sync.Scope` happens after every goroutine of the scope ([CON-16](#121-rules)).

**CON-8.** *Data races.* In normal code a data race is a **compile error**: the rules are [CON-12 to CON-20](#121-rules). Data races are undefined behavior only in the cases of [CON-21](#121-rules) ([UB-3](16-undefined-behavior.md)). `-race` (TSan) still detects them there.

**CON-9.** *`sync` types.* `WaitGroup`, `Once` and `Cond` keep Go's API. `Mutex[T]` and `RWMutex[T]` replace `sync.Mutex` and `sync.RWMutex` ([CON-15](#121-rules)). All of them are `@Pinned`: moving or copying one after first use is a compile error ([OWN-R10](04-ownership.md#42-ownership-rules-d14)). `sync.Scope` ([CON-16](#121-rules)) waits for every goroutine it started; closures passed to `s.Go` may borrow without cells.

> **Deferred to P12:** the marker that tells the compiler that a closure given to `s.Go` cannot outlive the scope (for example an attribute such as `@Scoped` on the parameter of `s.Go`). `Cond` waits on a guard ([CON-15](#121-rules)). What is allowed in a scope is fixed by [CON-16](#121-rules).

**CON-10.** *Program exit with goroutines running.* When `main.main` returns, `main`'s deferred calls and `main`'s local `drop()`s run first (releasing any `Mutex` guards or channels held by `main`), and then remaining background goroutines are stopped before `exit()`; see [INI-2](15-program-init.md). A stopped goroutine's stack is not unwound.

**CON-11.** *Panics in goroutines.* An unrecovered panic in any goroutine prints the panic, stops everything and exits with code 2 ([PAN-10](09-errors-and-panics.md)). `go f!()` panics there on error ([ERR-11a](09-errors-and-panics.md)).

**CON-12.** *Two labels on every type: `Send` and `Sync`.* The compiler computes them. They are never written on a type by hand. A type that uses `extends` is labeled from all its fields, all parents' fields included ([TYP-24](03-types.md#35-inheritance-adr-0023), ADR-0023). An upcast pointer has the labels of its pointer kind and of the parent type.
- **`Send`:** a value of the type may be moved to another goroutine.
- **`Sync`:** a value of the type may be shared between goroutines at the same time.
- Plain values with no pointers (numbers, `bool`, arrays and structs of them) and immutable `string`s are both `Send` and `Sync`. Heap `string` `backing` headers (`backing != nil`), `chan T`, plain `func(...)` cells, and `Send`/`Sync` interface boxes always use atomic reference-count increments and decrements (with a relaxed `refcount == 1` fast path on final free; static literals, lends, and last-use moves perform zero atomic operations), so a `string` aliased locally and later sent to another goroutine is race-free.
- `[]T`, `map[K]V`, `Unique[T]` and a type with `drop` are `Send` if their parts are.
- A struct or array has a label only if **all** its fields or elements have it.
- A raw pointer `*T` and a tracked borrow (`arr[i:j]`, borrow-carrying struct, lock guard) are not `Send` across `go` or `chan`, unless the target is `Sync` and the use is inside a `sync.Scope` ([CON-16](#121-rules)). A raw pointer is never `Send` on conversion to `error` (`error` is `Send + Sync`; a raw pointer is a borrow as [CON-2](#121-rules) and [OWN-B5](04-ownership.md#45-borrow-checking-adr-0021) define it): `return 0, &errBad` with a pointer-receiver `Error` method as an `error` is a compile error; an error type is converted by value (or synchronized `Shared[T]`), with value-receiver methods.
- `Mutex[T]` and `RWMutex[T]` are `Sync` only when `T` is `Send`. `Atomic[T]` is always `Sync` (for its allowed `T`). Channels, `WaitGroup`, `Once` and `context` are `Sync`. `Pool[T]` is `Sync` when `T` is `Send`; `sync.Map[K, V]` is `Sync` when `K` and `V` are `Send` (names illustrative, P12.13).
- `Shared[T]` and `Weak[T]` are `Send` and `Sync` only when `T` is **synchronized** (every field of `T` is thread-safe synchronized: `Mutex[U]`, `RWMutex[U]`, `Atomic[U]`, `chan U`, `WaitGroup`, `Once`, or nested synchronized structs; [CON-19](#121-rules)). When `T` has plain unprotected writable fields (`Node{Value int, Children []Shared[Node]}`), `Shared[T]` is writable in a single goroutine with a non-atomic reference count and is **neither `Send` nor `Sync`**, so unsynchronized shared structs cannot cross goroutines ([OWN-P8](04-ownership.md#43-pointers-and-heap-allocation-d17), [CON-19](#121-rules)).
- **Interfaces (`Send` by default).** Every ordinary interface type (`any`, `io.Reader`, `Handler`, etc.) is **`Send` by default** ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)): `chan any`, `chan io.Reader`, `Mutex[io.Reader]` and `go serve(r)` (on unshared last use of `r`) all work directly. Converting a value into a kept plain interface checks that its dynamic type is `Send` (non-keeping interface parameters such as `fmt.Println(v)` only borrow `v` and accept any type, [TYP-7](03-types.md)). An interface that embeds `Sync` (including predeclared `error`) is both `Send` and `Sync` so it may be shared across goroutines concurrently: converting into it accepts `Send + Sync` values directly, and also accepts `Send` value types whose only non-`Sync` parts are mutable value containers (`[]U`, `map[K]V`, e.g. `ExitError{Stderr []byte}`) when moved in unshared (`refcount == 1`) on last use or via `.Clone()`, because the interface box freezes them as deeply immutable ([TYP-5, TYP-6](03-types.md)).
- **Func values (`Send + Sync` by default).** Plain `func(...)` types are **`Send` and `Sync` by default** ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)): `chan func()`, `go cb()`, `http.HandleFunc` and `Mutex[[]func()]` all work with ordinary `func(...)` syntax. An escaping closure or method value converted to a plain `func(...)` must have `Sync` captures (numbers, `string`, structs of them, `Mutex[T]`, `RWMutex[T]`, `Atomic[T]`, `chan T`, `WaitGroup`, `Once`, synchronized `Shared[T]`; [CLO-2](11-closures.md)). Non-escaping closures (`slices.SortFunc`, `mu.With`, `defer`, [CLO-1](11-closures.md)) and direct `go func(){...}()` statements ([CON-13](#121-rules)) still allow local variable mutation with zero annotations.
- **Single-goroutine opt-out (`Local[T]`).** `Local[T]` (where `T` is a `func` or `interface` type: `Local[func()]`, `Local[any]`, `Local[Widget]`) opts out of `Send` and `Sync` using non-atomic reference counts: `Local[func(...)]` allows escaping single-goroutine callbacks to share unsynchronized mutable cells (`count++`, [CLO-2](11-closures.md)), and `Local[I]` allows boxing single-goroutine non-`Send` values (such as unsynchronized `Shared[Node]`). Plain `func(...)` converts freely to `Local[func(...)]`, and plain `I` converts freely to `Local[I]`, never the reverse; `Local[T]` cannot cross `go`, `chan` or `Mutex`. (`Local[int]` on a non-func/non-interface type is a compile error.)
- A map, a slice, or a struct containing a slice or map is `Sync` for read-only sharing inside `sync.Scope`, never for concurrent unsynchronized writes ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)). Writing shared data across goroutines needs `Mutex[T]` or `Atomic[T]` (for example `NewShared(sync.NewMutex(map[string]int{}))`), or exclusive access through moving an unshared handle on last use, `.Clone()`, or `Split`.

`Send` and `Sync` are predeclared constraints, written like `Copy` and `Clone`: `[T Send]`, `[T Sync]` ([GEN-11](10-generics.md), [LEX-7](02-lexical.md)).
```go
func sendEmail(m string) {}
func pick() func() { return func() {} }
func pickLocal() Local[func()] { n := 0; return func() { n++ } }

func worker(jobs chan func()) { for job := range jobs { job() } }

func demo(jobs chan func(), events chan any) {
    mail := "hello"                            // string is immutable (Send + Sync)
    sent := 0
    jobs <- func() { sendEmail(mail) }         // ok: plain chan func() works!
    jobs <- pick()                             // ok: plain func() is Send + Sync!
    events <- mail                             // ok: plain chan any works!
    jobs <- func() { sent++ }                  // error: sent is unsynchronized; use Atomic[int] or Local[func()]
    println(sent)
    jobs <- pickLocal()                        // error: Local[func()] cannot cross goroutines
}
```

**CON-13.** *What `go` may pass or capture.* `go f(args)` and `go func(){…}()` may pass or capture only:
- values that are **moved in** (the caller loses them; later use is an error);
- values whose type is `Send` and that are **copied in**;
- `Sync` values that are **shared**.

Capturing a **writable variable** that the starting goroutine or anyone else can still touch is a compile error. The message suggests `Mutex[T]`, `Atomic[T]` or a channel. A variable that nobody else touches after the `go` counts as moved in: "can still touch" means a use that the compiler can see after the `go` (in the spawner, in another goroutine, or in a later iteration of a loop that holds the `go`), not merely that the variable is still in scope (owner decision 2026-10-31, ADR-0022 open point 2). The check is conservative.

```go
x := 0
go func() { x++ }()          // error: x is written by the goroutine and the spawner uses it below.
println(x)                   // Fix: sync.Atomic[int], sync.Mutex[int], or send results on a channel

y := 0
go func() { y++; println(y) }()   // ok: nobody touches y after the `go`, so it is moved in
var n sync.Atomic[int]
go func() { n.Add(1) }()     // ok: Atomic is Sync, shared

p := &x
go func() { *p = 1 }()       // error: a raw pointer to a local (CON-2)
ys := []int{1, 2}
go sum(ys)                   // ok: ys is moved in (the caller loses it)
```

**CON-14.** *Channel send transfers.* Sending a `Copy` (`OWN-C1`) value copies it; sending an immutable `string` or `Sync` `OWN-C2` handle (`func(...)`, `error`, `chan T`, synchronized `Shared[T]`) shares or moves it on last use; sending a mutable `[]T`, `map[K]V` or non-`Sync` interface (`any`, `io.Reader`) moves it in $O(1)$ when unshared on last use (or shares when known `Sync`) and requires `.Clone()` if still used or aliased by the sender; sending a single-owner (`OWN-C3`) value moves it ([OWN-R3, OWN-R5](04-ownership.md#42-ownership-rules-d14)). The element type of a channel must be `Send`: `chan T` with a `T` that is not `Send` (for example `chan *int`, `chan Local[func()]`, or a type holding a lock guard) is a compile error. This adds the label check to [CON-4](#121-rules) and [CON-5](#121-rules).

**CON-15.** *`Mutex[T]` and `RWMutex[T]` hold the data.* The data lives inside the mutex.
- `m.Lock()` returns a **write guard** `g`. `g.Val` accesses the guarded `T` directly for every `T` (`g.Val = append(g.Val, 10)`, `g.Val["a"] = 1`, `g.Val++`), and when `T` is a struct, field and method selectors on `g` auto-dereference to `g.Val` (`g.N--`, without needing `*[]U` or `*map[K]V`, which do not exist in Go++).
- The guard unlocks when it is **dropped**: end of its block, early return, panic. There is no hand-written `Unlock`.
- `m.Update(func(v T) T)` locks, passes the guarded `T` by value (`0` retain on last-use update) and stores the returned `T` back before unlocking (works for every `T`, including `Mutex[[]int]` and `Mutex[map[string]int]`); for non-slice, non-map `T`, `m.With(func(p *T) {...})` locks, passes `*T`, and unlocks.
- `RWMutex[T]` also has `RLock()`, which returns a **read guard** providing deep read-only access to `g.Val` ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)).
- A panic inside a lock section drops the guard while unwinding, so the mutex is unlocked. There is no poisoning, the same as Go.
- `sync.Cond` waits on a guard: `cond.Wait(&g)` (names illustrative, P12.13).
- A guard is its own non-`Copy` borrow type that unlocks on drop and cannot be copied or sent. `Lock()` returns a write guard.
- A guard is a **borrow of the mutex** ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)): the mutex may not be moved or dropped while the guard is live. A guard is not `Send`: it cannot be sent, returned through a channel or passed to `go`.
- `Mutex[struct{}]` is the lock-only form.
- Plain `sync.Mutex`, `sync.RWMutex` (no data) and manual `Unlock` are **removed**.
- Mutexes are `@Pinned` ([OWN-R10](04-ownership.md#42-ownership-rules-d14)).

```go
type Stock struct{ N int }
var stock = sync.NewMutex(Stock{N: 3})
var logMu = sync.NewMutex([]string{})

func take() bool {
    g := stock.Lock()              // guard: struct fields auto-deref on g
    if g.N == 0 { return false }   // unlocked here
    g.N--
    return true                    // and here
}
stock.With(func(s *Stock) { s.N += 10 })
logMu.Update(func(xs []string) []string { return append(xs, "ok") })
```

**CON-16.** *`sync.Scope`.* `sync.Scope` is the normal way to run goroutines that use local data.
- Goroutines started with `s.Go(...)` must finish before `sync.Scope` returns. So they may borrow the function's locals (pointers, slices, read-only bindings), following the borrow checker ([OWN-B1 to B9](04-ownership.md#45-borrow-checking-adr-0021)) and the labels ([CON-12](#121-rules)): a value shared by reference must be `Sync`, or read-only for every goroutine.
- Two goroutines of a scope may **not both get writable access to the same place**. This is a compile error, the same rule as for one call ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). Disjoint parts are allowed through `Split`: `left, right := nums.Split(500)` gives two non-overlapping borrowed slices, like Rust's `split_at_mut`. `Split` consumes the receiver's borrow (a moving reborrow, [OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)), so `head, tail := rest.Split(10); rest = tail` is accepted, and the two results are known to the compiler to be disjoint.
- While a scope is running, the spawner may not change what a goroutine borrows ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)).
- Plain `go` outside a scope keeps the strict rule: [CON-1](#121-rules) to [CON-3](#121-rules) and [CON-13](#121-rules), no borrows.

```go
nums := make([]int, 1000)
sync.Scope(func(s *sync.Scope) {
    left, right := nums.Split(500)
    s.Go(func() { for i := range left  { left[i] = i } })    // ok: disjoint
    s.Go(func() { for i := range right { right[i] = i } })
})                                   // both goroutines are finished here
```

**CON-17.** *`Atomic[T]`.* `Atomic[T]` is for integer types, `bool` and `unsafe.Pointer`. Operations are **sequentially consistent only**: no weaker orderings are offered. An `Atomic[T]` is `Sync`. The `atomic.AddInt64(&x, 1)` style functions on plain variables are **removed**.

**CON-18.** *Global variables.* A package-level variable that is written after package initialization (anywhere outside `init` and the initializer, [INI-1](15-program-init.md)) must have a `Sync` type (`Atomic[T]`, `Mutex[T]`, …). Writing any other package variable outside init is a compile error. A package variable that is never written after init is read-only and is fine for any type.

```go
var hits sync.Atomic[int]        // ok: Sync
var cache = sync.NewMutex(map[string]int{})   // ok: Sync
var counter int
func Inc() { counter++ }         // error: counter is written after init and is not Sync
```

**CON-19.** *`Shared[T]` in 1 goroutine vs across goroutines* ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)).
- **Single-goroutine `Shared[T]`:** When `T` has plain writable fields (`Node{Value int, Children []Shared[Node]}`), `Shared[T]` works like `[]T` and `map[K]V` — fields and elements are directly readable and writable (`p.Value = 99`, `p.Children = append(p.Children, c)`), using a fast non-atomic reference count ([OWN-P8](04-ownership.md#43-pointers-and-heap-allocation-d17)). Because any function holding `Shared[T]` can mutate `T` without a lock, an unsynchronized `Shared[T]` (and `Weak[T]`) is neither `Send` nor `Sync` and cannot be passed to `go` or sent on `chan`.
- **Multi-goroutine `Shared[T]` (`Send + Sync`):** When `T` is synchronized — either `Shared[sync.Mutex[State]]` / `Shared[sync.RWMutex[State]]`, or a struct whose fields are all synchronized (`Mutex[U]`, `RWMutex[U]`, `Atomic[U]`, `chan U`, `WaitGroup`, `Once`, or nested synchronized structs) — `Shared[T]` and `Weak[T]` are `Send` and `Sync`, use atomic reference counting, and may be freely shared across goroutines (`cache := NewShared(sync.NewMutex(map[string]User{})); go worker(cache)`). `Weak[T].Lock()` upgrades to `Option[Shared[T]]` with the same `Send`/`Sync` status.

**CON-20.** *Deadlock check, visible cases only.* Locks are identified by **lock class**, like Linux lockdep: the same field of the same type, or the same package variable, is one class. It is a compile error when:
- the compiler sees two functions (in the same package, or through export data) that take the same two classes in opposite order;
- the same mutex is locked twice in one function while its guard is live;
- one function locks two locks of the **same class** while both guards are live (`a.mu` then `b.mu`, with `a` and `b` of the same type), because `transfer(a, b)` running together with `transfer(b, a)` can deadlock. Fix: `sync.LockAll(&a.mu, &b.mu)`, which takes them in a fixed (address) order.

Locks behind interfaces or chosen at run time are not checked. Channel deadlocks stay a run-time panic "all goroutines are asleep", as in Go.

**CON-21.** *What is not covered.*
- **Logic races:** check-then-act across two lock sections. Nothing is wrong at the memory level, and no language rule can see it. Fix: one lock section (for example `With`).
- **Deadlocks the compiler cannot see** (CON-20).
- **`unsafe` code, and C and C++ threads and callbacks.** `-race` (TSan) helps there ([UB-3](16-undefined-behavior.md)).

```go
ok := false
{ g := stock.Lock(); ok = g.N > 0 }        // section 1
if ok { g := stock.Lock(); g.N-- }         // section 2: compiles, but another goroutine may have taken the last item
```

## 12.2 Examples

```go
// Transfers through a channel
type Job struct{ ID int; Payload Unique[Buffer] }
jobs := make(chan Job, 4)
j := Job{ID: 1, Payload: NewUnique(Buffer{})}
jobs <- j                // single-owner j moves into the channel
// fmt.Println(j.ID)     // error (E0451): j used after move
got := <-jobs            // moves out: got is the owner

// Immutable strings share freely across channels (atomic refcount when kept in caller)
msgs := make(chan string, 2)
s := "alpha"
msgs <- s
fmt.Println(s)           // ok: string is immutable (Send + Sync)!

// go with a closure: wg is captured in a shared cell
var wg sync.WaitGroup
wg.Add(1)
go func() { defer wg.Done(); process(got) }()   // `got` is moved in (CON-13); wg is Sync, shared
wg.Wait()

// go with a writable variable: rejected (CON-13)
n := 0
go func() { n++ }()      // error. Fix: var n sync.Atomic[int]; n.Add(1)

// go with a raw pointer to a local: rejected
var wg2 sync.WaitGroup
go worker(&wg2)          // error: raw pointer to a local passed to go. Fix-it: use a closure

// select moves only the chosen send
select {
case out <- msg:         // msg moves only if this case is chosen
case <-done:             // msg is still ours here
}
```

Compared with Go: channel programs look the same. Go++ rejects `go worker(&wg)` on a local because the local may be gone when the goroutine runs; Go allows it because `wg` escapes to the GC heap. Go++ also rejects `n++` in a goroutine when `n` is a plain captured variable (a data race in Go), and has no `sync.Mutex` next to its data: use `Mutex[T]` ([CON-15](#121-rules)).

## 12.3 Rationale

- Moving through channels makes "who owns this value now?" a compile-time answer and avoids copies.
- Banning borrows of locals in `go` removes the most common dangling bug in a language without lifetime annotations. The closure fix-it keeps the code short.
- Races are compile errors because a race is a bug of the program, not a feature: the compiler can see who shares what once types carry `Send` and `Sync` (ADR-0022). Making races defined would cost every memory access.
- A mutex that owns its data removes the most common Go bug, "the lock and the data are not connected", and the guard removes the forgotten `Unlock`.
- `sync.Scope` with `Split` gives the speed of shared local memory without a cell or a copy, and without a race.
- The cost: Go code with `sync.Mutex` next to its data must be rewritten, and some correct sharing is rejected (the check is conservative). The owner accepted this.
- TSan (`-race`) stays for what the compiler cannot see: `unsafe`, C and C++.
