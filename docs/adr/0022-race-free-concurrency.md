# ADR-0022: Race-free concurrency in normal code

- **Status:** accepted
- **Status note (2026-10-28, ADR-0023):** a type that uses `extends` is labeled `Send` / `Sync` from all its fields, the parent's included.
- **Date:** 2026-10-28
- **Plan reference:** changes P01 D1.13 ("data races are undefined behavior"), P12 D12.6 and D12.7; supersedes the data-race sentence of ADR-0021 (context and decision 7); spec new rules CON-12 to CON-21, rewrites CON-1, CON-2, CON-3, CON-7, CON-8, CON-9, UB-3, UB-11, 16.4, OWN-B4 note, OWN-B5, OWN-B6, GEN-11, CLO-3 and CLO example 4; new tasks P04.21 and P12.13; P12.06 and P13 notes.

## Context

ADR-0021 made most use-after-free bugs compile errors, but it left data races as "the programmer's job" (a mutex or channels, plus `-race` tests). Races are the same kind of bug: easy to write, hard to see, and the program can do anything (UB-3).

Go code has two common habits that cause races:

```go
x := 0
var wg sync.WaitGroup
for i := 0; i < 100; i++ {
    wg.Add(1)
    go func() { defer wg.Done(); x++ }()    // 1: counter race
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

In case 1 nothing tells the compiler that `x` is shared. In case 2 the lock and the data it protects are two separate things, so the compiler cannot know that `stock` needs `mu`, and a forgotten `Unlock` is a hang.

On 2026-10-28 the owner decided ("B+"): data races are compile errors in normal code, like Rust's `Send` and `Sync`, with some improvements Rust does not have. The owner accepts that Go code which keeps a `sync.Mutex` next to its data must be rewritten.

## Decision

1. **Two automatic labels on every type** (CON-12): `Send` (the value may be moved to another goroutine) and `Sync` (the value may be shared between goroutines at the same time). The compiler computes them. Nobody writes them by hand. They are predeclared constraints, usable in generics (`[T Send]`), like `Copy` and `Clone` (GEN-11).
   - Plain values with no pointers are both.
   - An owning type is `Send` if its parts are.
   - A struct (or array) gets a label only if all its fields have it.
   - Raw pointers, borrows and views are not `Send` across a `go`, unless the target is `Sync` and the borrow is inside a `sync.Scope` (decision 5).
   - `Mutex[T]` and `RWMutex[T]` (when `T` is `Send`), `Atomic[T]`, channels, `WaitGroup`, `Once` and `context` are `Sync`.
   - `Shared[*T]` is `Send` and `Sync` only when `T` is `Sync` (Rust's `Arc` rule). A non-`Sync` `T` cannot cross goroutines through `Shared` at all. Plain data with no interior writes is automatically `Sync`, so read-only sharing of ordinary structs works (decision 8).
   - `Mutex[T]` and `RWMutex[T]` are `Sync` only when `T` is `Send` (Rust's rule). `Atomic[T]` is always `Sync` for its allowed `T`.
   - `sync.Pool[T]` is `Sync` when `T` is `Send`. `sync.Map[K, V]` is `Sync` when `K` and `V` are `Send`.
   - **Interfaces.** An interface type is `Send` / `Sync` only if it embeds the `Send` / `Sync` constraint (like Rust's `dyn Trait + Send`). The predeclared `error` is declared with `Send` and `Sync`, so converting a value to `error` requires its dynamic type to be `Send` and `Sync` (most error types are plain data, so this is cheap). `any` has no labels; use `interface{ Send }` for a sendable any. A conversion into a labeled interface is checked at the conversion.
   - **Func values.** The labels of a func value come from its captures and are checked where the value is created: a function literal, a method value, or a top-level function (no captures, always both). A func-typed variable or parameter whose origin is unknown is not `Send` (conservative). `go f()` with a top-level `f` is fine. A `Send` func type is written `Send[F]` (amendment of 2026-10-31; open point 1 is closed).
2. **`go` statement (CON-13).** `go f(args)` and `go func(){…}()` may pass or capture only:
   - values that are **moved in** (the caller loses them);
   - values whose type is `Send` and that are **copied in**;
   - `Sync` values that are **shared**.

   Capturing a writable variable that the starting goroutine or anyone else can still touch is a compile error. The message suggests `Mutex[T]`, `Atomic[T]` or a channel.
3. **Channel send moves (CON-14).** After `ch <- v`, `v` is moved (use after move is an error). A `Copy` value is copied. The element type of a channel must be `Send`.
4. **`Mutex[T]` and `RWMutex[T]` hold the data (CON-15).**
   - `m.Lock()` returns a guard. The data is reached only through the guard.
   - The guard unlocks when it is dropped (end of block, early return, panic). There is no hand-written `Unlock`.
   - `m.With(func(p *T) {...})` is the short form.
   - A guard is its own type, never a `View`: a non-`Copy` borrow that unlocks on drop and cannot be copied or sent. `Lock()` returns a write guard.
   - `RWMutex[T]` has `RLock()`, which returns a read guard. Reading through it is read-only with the same deep read-only rules as a view (OWN-V1), but the guard itself is not a `View[T]` value.
   - A guard is a borrow of the mutex (OWN-B rules) and is not `Send`.
   - `Mutex[struct{}]` is the lock-only form.
   - Plain `sync.Mutex`, `sync.RWMutex` (no data) and manual `Unlock` are **removed**.
   - Mutexes are `@Pinned`.
   - A panic inside a lock section drops the guard during unwinding and unlocks. There is no poisoning, the same as Go.
   - `sync.Cond` waits on a guard: `cond.Wait(&g)`.
5. **`sync.Scope` (CON-16)** is the normal way to run goroutines that use local data.
   - Goroutines started with `s.Go(...)` must finish before `sync.Scope` returns, so they may borrow the function's locals (views, borrowed slices, pointers), following the borrow checker (ADR-0021) and the labels.
   - Two goroutines may not both get writable access to the same place. Disjoint parts are allowed through `Split`: `left, right := nums.Split(500)` gives two non-overlapping borrowed slices (like Rust's `split_at_mut`).
   - `Split` consumes the receiver's borrow (a moving reborrow), so `head, tail := rest.Split(10); rest = tail` is accepted by OWN-B2, and its two results are known to be disjoint.
   - Plain `go` outside a scope keeps decision 2: no borrows.
6. **`Atomic[T]` (CON-17)** for integer, bool and pointer kinds, sequentially consistent only (no weaker orderings are exposed). `atomic.AddInt64(&x, 1)` style functions on plain variables are **removed**.
7. **Global variables (CON-18).** A package-level variable written after package initialization (outside `init` and its initializer) must have a `Sync` type (`Atomic`, `Mutex`, …). Otherwise writing it outside init is a compile error. Read-only globals after init are fine.
8. **Shared handles (CON-19).** A `Shared[*T]` is `Send` and `Sync` only when `T` is `Sync`, so it can cross goroutines only then. Plain data with no interior writes is `Sync`, and reads through the handle are read-only. To write, put a `Sync` type inside: `new_shared(sync.NewMutex(User{}))`. *(ADR-0021 amendment B: a `Shared[*T]` is read-only everywhere, not only across goroutines, unless a part is a cell type: `Mutex[T]` / `RWMutex[T]` / `Atomic[T]`, or `mem.Cell[T]` on one goroutine.)*
9. **Deadlock check, simple visible cases (CON-20).** Locks are identified by **lock class** (like Linux lockdep): the same field of the same type, or the same package variable, is one class. It is a compile error when:
   - the compiler sees two functions (in the same package, or through export data) that take the same two classes in opposite order;
   - the same mutex is locked twice in one function while its guard is live;
   - one function locks two locks of the **same class** while both guards are live (`a.mu` then `b.mu`, with `a` and `b` of the same type), because `transfer(a, b)` running together with `transfer(b, a)` can deadlock. Fix: `sync.LockAll(&a.mu, &b.mu)`, which takes them in a fixed (address) order.

   Locks behind interfaces or chosen at run time are not checked. Channel deadlocks stay a run-time "all goroutines are asleep" panic, as in Go.
10. **What stays not covered (CON-21):**
    - logic races (check-then-act across two lock sections; no language catches these; the fix is one lock section, for example `With`);
    - deadlocks the compiler cannot see;
    - `unsafe` code and C and C++ threads and callbacks (`-race` / TSan still help there).

    UB-3 is narrowed to these. UB-11 (concurrent use of unsynchronized runtime objects) becomes a compile error in normal code.
11. **Where it runs:** new task P04.21, on the flow graph of P04.09 and P04.20; `gopp check` runs it too. The library side (`Mutex[T]`, `RWMutex[T]`, guards, `With`, `Atomic[T]`, `sync.Scope`, `Split`) is new task P12.13. P13 updates every std package that used a plain mutex.
12. **New error codes** are named at implementation (no numbers here) and get `gopp explain` texts (P03.14).

## Before and after

### 1. The counter race

```go
// Before (compiles in Go, is a data race)
x := 0
for i := 0; i < 100; i++ {
    wg.Add(1)
    go func() { defer wg.Done(); x++ }()   // error: x is written by many goroutines and also read by main
}
println(x)

// After, with Atomic
var x sync.Atomic[int]
for i := 0; i < 100; i++ {
    wg.Add(1)
    go func() { defer wg.Done(); x.Add(1) }()   // ok: Atomic is Sync, shared
}
wg.Wait()
println(x.Load())

// After, with Mutex that holds the data
total := sync.NewMutex(0)
for i := 0; i < 100; i++ {
    wg.Add(1)
    go func() { defer wg.Done(); total.With(func(p *int) { *p++ }) }()
}
```

### 2. Forgot Unlock with an early return

```go
// Before: the early return leaves the lock held. Also nothing says that stock needs mu.
mu.Lock()
if stock[k] == 0 { return 0 }
stock[k]--
mu.Unlock()

// After: the data lives inside the mutex; the guard unlocks at every exit
var stock = sync.NewMutex(map[string]int{})   // package variable: Sync, so it may be used from many goroutines

func take(k string) int {
    g := stock.Lock()           // guard, a borrow of stock
    if g[k] == 0 { return 0 }   // the guard is dropped here: unlocked
    g[k]--
    return 1                    // and here
}
// stock.Lock() is the only way to the map; a write without the lock does not compile.
```

### 3. 100 goroutines fill a local slice with `sync.Scope`

```go
// Before: Go allows it (the GC keeps nums alive); Go++ plain `go` rejects it (CON-2), and nums would need a cell.
nums := make([]int, 1000)
rest := nums[:]                       // a borrowed slice
for i := 0; i < 100; i++ {
    head, tail := rest.Split(10)
    rest = tail
    go func() { for j := range head { head[j] = i } }()   // error: borrowed slice captured by plain `go`
}

// After: the goroutines finish before Scope returns, so they may borrow. Split gives disjoint parts.
nums := make([]int, 1000)
sync.Scope(func(s *sync.Scope) {
    rest := nums[:]                               // a borrowed slice of nums
    for i := 0; i < 100; i++ {
        head, tail := rest.Split(10)              // head and tail do not overlap; Split consumes rest's borrow
        rest = tail
        s.Go(func() { for j := range head { head[j] = i } })   // ok: head is a borrowed slice
    }
})                                                // all 100 are finished here; the borrows end
println(nums[999])                                // ok: nums is not borrowed any more

// Still an error inside a scope: two writable borrows of the same place
sync.Scope(func(s *sync.Scope) {
    s.Go(func() { nums[0] = 1 })
    s.Go(func() { nums[0] = 2 })                  // error: nums is writable in two goroutines. Fix: Split, or a Mutex
})
```

### 4. The opposite-lock-order deadlock

```go
var a = sync.NewMutex(0)
var b = sync.NewMutex(0)

func f() {
    ga := a.Lock()
    gb := b.Lock()          // order: a, b
    *ga += *gb
}
func g() {
    gb := b.Lock()
    ga := a.Lock()          // error: f takes a then b; g takes b then a
    *gb += *ga
}
// Fix: take them in the same order in both functions.
// Also an error: ga := a.Lock(); ga2 := a.Lock()   (the same mutex twice while ga is live)

type Account struct{ mu sync.Mutex[int] }
func transfer(x, y *Account) {
    gx := x.mu.Lock()
    gy := y.mu.Lock()       // error: two locks of the same class (Account.mu); transfer(a, b) with transfer(b, a) can deadlock
}
func transferOK(x, y *Account) {
    gx, gy := sync.LockAll(&x.mu, &y.mu)   // ok: fixed (address) order
}
```

### 5. Not covered: the logic race

```go
var stock = sync.NewMutex(Stock{N: 1})

func take() bool {
    ok := false
    { g := stock.Lock(); ok = g.N > 0 }     // section 1
    if ok {
        g := stock.Lock()                   // section 2: another goroutine may have taken the last item
        g.N--                               // compiles, and N can become -1
    }
    return ok
}
// Fix: one lock section.
func takeFixed() bool {
    g := stock.Lock()
    if g.N == 0 { return false }
    g.N--
    return true
}
```

No data race exists here (every access is locked), so no language rule can tell this is wrong.

## Consequences

- Data races are compile errors in normal code, with a hint for the fix.
- Code that used `sync.Mutex` next to its data, `sync/atomic` functions on plain variables, or shared writable captured variables must be rewritten. The owner accepts this.
- `unlock` mistakes disappear (auto-unlock). Self-deadlock and the simple opposite-order deadlock are found at compile time.
- Some correct programs are rejected (the check is conservative). Fixes: a `Mutex`, an `Atomic`, a channel, or `sync.Scope` with `Split`.
- The labels need export data: a function's signature carries the labels of its types, and the deadlock check needs "which mutexes a function locks, in order" in export data (P06).
- `-race` stays useful for `unsafe` code, C and C++ threads and callbacks, and logic races that become data races inside `unsafe`.
- The semantics of Go's `sync` package change: the Go version of a sample program with a plain `sync.Mutex` no longer compiles unchanged in Go++.

## Alternatives

- **A: keep data races as the programmer's job, plus `-race` (rejected).** Cheap, and this was ADR-0021's position. Rejected by the owner: races show up only when the bad interleaving happens, and tests miss them.
- **C: a `gopp vet` warning first, an error later (rejected).** Softer for existing code. Rejected: the owner accepts the rewrite, and a warning period would mean the rules are built twice.
- **Plain Rust "B": `Send` and `Sync` only, with no auto-unlock, no scope borrowing and no deadlock check (improved upon).** The labels are the same as Rust's. This ADR adds guards that unlock by themselves (Rust has this too through `MutexGuard`, but Go++ also removes the manual form), `sync.Scope` that borrows locals with `Split` (Rust needs `std::thread::scope`, and does not check disjoint parts of an array without `split_at_mut`; Go++ keeps the same idea), and a compile-time check for simple deadlocks (Rust has none).
- **Pony-style reference capabilities (rejected).** Very strong, but every reference carries a capability the programmer must write and understand. Too complex for a Go-like language.
- **Actors only, no shared memory (rejected).** Simplest to prove safe. Too far from Go: channels and shared state with locks would both be gone.

## Open points

1. **A way to write a `Send` func type.** *Closed 2026-10-31 (owner decision, option A): the wrapper `Send[F]`, see the amendment at the end of this ADR.* (Was: a func-typed variable or parameter of unknown origin is not `Send`, so a worker pool that takes `func()` jobs through a channel could not say the funcs are `Send`.)
2. **Writable-capture rule: "can still touch".** A closure that captures `i` (a loop variable, never written afterwards) is fine; a closure that writes a captured local that nobody else uses is also harmless. The exact flow rule is built in P04.21 and is conservative. **Decided 2026-10-31 (owner): option A.** "Can still touch" means a use the compiler can see after the `go`: in the spawner, in another goroutine, or in a later iteration of a loop that holds the `go`. A variable that is only in scope but never used again counts as moved in, so `x := 0; go func(){ x++ }()` with no later use of `x` is accepted, and adding `println(x)` after it is an error. Rejected option B: refuse any writable capture of a variable the spawner could still name, which refuses safe code and contradicts CON-13's "moved in" rule.
3. **Guard access syntax.** The ADR shows `g.N--`, `g[k]` and `*ga += *gb`: the guard is used like a `*T`. P12.13 sets the real surface. All library names in this ADR (`sync.NewMutex`, `sync.Atomic[T]`, `sync.Scope(...)`, `sync.LockAll`, `cond.Wait(&g)`, `sync.Map[K, V]`) are illustrative.
4. **Go mode** (`.go` files): do they get the borrow checker and the concurrency checks? This is one open question, P04 Q3 (it covers ADR-0021 open point 3 and this ADR). This ADR assumed yes. **Deferred by owner 2026-10-28:** Go mode is not a current focus, so P04 Q3 stays open and no new Go-mode work is planned; existing Go-mode rules and tests stay.
5. **Locks in generic code and through function values.** Not checked for deadlocks (decision 9), but the labels are checked per instance, like `Option` (GEN-5, P04.08).

## Amendment (2026-10-31, owner decision): `Send[F]` is the spelling of a `Send` func type

Closes open point 1. The spelling is a wrapper type, `Send[F]` with `F` any func type: `Send[func()]`, `Send[func(int) string]`.

- `Send[F]` is written like `View[T]` and `Option[T]`. It is usable in parameters, fields, results, locals and channel elements (`chan Send[func()]`). Its value is called like the func, and `go job()` on it is accepted.
- A func value converts to `Send[F]` only where the checker can prove the labels from the captures: a function literal, a method value or a top-level function (always fine). A capture that fails the CON-13 rules is a compile error at the conversion. A plain `func()` of unknown origin does not convert: compile error.
- `Send[F]` converts to plain `F` freely.
- The constraint `[T Send]` is unchanged. `Send[F]` is for func types only: `Send[int]` is a compile error. There is no `Sync[F]` for now.
- In generic code `Send[F]` is defined only when `F` is written as a func type (`Send[func(T) U]` with type parameters inside); `func run[F any](f Send[F])` is refused, the conservative choice. The instance is checked again with the real types (P04.08).

```go
func sendEmail(m string) {}

func worker(jobs chan Send[func()]) { for job := range jobs { job() } }

func demo(jobs chan Send[func()]) {
    mail := "hello"                            // a string has a zero value
    sent := 0
    jobs <- func() { sendEmail(move(mail)) }   // ok (CLO-9): the type has a zero value; a second call sees ""
    jobs <- func() { sent++ }                  // error: sent is used below
    println(sent)
}
```
A `Send[func()]` may be called twice, so `move(x)` of a capture needs a type with a zero value (CLO-9); with no zero value it is a compile error.

Rejected alternatives: a `go func()` keyword type (a new type form for one use, and it would not cover a `Send` func that is only sent on a channel); an interface-only spelling (an interface with a `Call()` method makes every job a named type and cannot take a signature such as `func(int) string`). Owner chose the wrapper.
