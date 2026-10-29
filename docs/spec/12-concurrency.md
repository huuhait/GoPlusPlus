# 12. Concurrency

Base: Go spec [Go statements](https://go.dev/ref/spec#Go_statements), [Channel types](https://go.dev/ref/spec#Channel_types), [Send statements](https://go.dev/ref/spec#Send_statements), [Receive operator](https://go.dev/ref/spec#Receive_operator), [Select statements](https://go.dev/ref/spec#Select_statements), [Close](https://go.dev/ref/spec#Close), [Program execution](https://go.dev/ref/spec#Program_execution). Memory model: [Go memory model](https://go.dev/ref/mem).

Delta D1.13, extended by ADR-0022. `go`, `chan` and `select` work as in Go. What is new is what happens to **owned values** and to **borrows**, and that **data races are compile errors in normal code** (the labels `Send` and `Sync`, CON-12 to CON-21). `sync.Mutex` and `sync/atomic` functions on plain variables are replaced by `Mutex[T]` and `Atomic[T]`.

Syntax: no change.

## 12.1 Rules

**CON-1.** *`go f(a, b)`.* The function and its arguments (the receiver counts as an argument) are evaluated, then given to the new goroutine. By kind of argument:
- **trivial** values are copied;
- **shared handles** are copied, which retains them;
- **owned temporaries** (call results, conversions, composite literals) are moved in;
- **owned locals** are moved in (`go fetch(u)` with a local `u` moves `u`; later use of `u` is an error);
- **owned places** (a field, element, map entry, package variable or pointer target), **borrowed variables** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) and `&` range variables ([RNG-1a](08-slices-and-range.md)) are a **compile error**: a place read is only a borrow ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)) and a borrow cannot be handed to a goroutine that may outlive it. Fix-its: `x.Clone()`; `move(x)` for owned places and `&` range variables (never for a borrowed variable, [OWN-R3b](04-ownership.md#42-ownership-rules-d14)); for a borrowed parameter also making it `move`; and a closure ([CON-3](12-concurrency.md)) only for owned places of locals, because a closure cannot capture a borrowed variable ([CLO-3a](11-closures.md));
- a **raw pointer, a view or a borrowed slice** into a local (or into the target of a local `Unique`) is a compile error ([CON-2](12-concurrency.md)).

```go
for _, u := range users {   // u borrows the element
    go fetch(u)             // error: borrowed variable. Fix-it: go fetch(u.Clone())
}                           // (a fix-it never reuses the name: not u := u.Clone(), RNG-8)
for _, u := range move(users) {
    go fetch(u)             // ok: u is an owned local, moved into the goroutine
}
go fetch(cfg.Name)          // error: field. Fix-its: cfg.Name.Clone() or move(cfg.Name)
if s, ok := v.(string); ok {
    go log(s)               // error: s is a borrowed variable (assertion binding). Fix-it: go log(s.Clone())
}
```

A value that is allowed by this list must also pass the race rules of [CON-13](#121-rules) (moved in, `Send` and copied, or `Sync` and shared). The case list above applies D1.4 rule 3a and rule 8 to `go`, the same way [PAN-3](09-errors-and-panics.md) does for `defer`. The one difference: a `defer` runs before its function returns, so it may *borrow* a variable that lives that long; a goroutine can outlive the function, so `go` never borrows an owned place or a borrowed variable. `go f(x)` moves owned locals. Fields and borrowed variables need `x.Clone()`, or `move(...)` for owned fields and `&` range variables (never for a borrowed variable).

**CON-2.** *No borrows of locals across `go`.* This is the `go` case of [OWN-B5](04-ownership.md#45-borrow-checking-adr-0021): any borrow in a `go` argument or capture is an error, because the goroutine may outlive the owner. A raw pointer, a view or a borrowed slice into a local, or into the target of a local `Unique`, passed to `go` is a **compile error** outside `unsafe`. Example: `go srv.Run()` with a local `Unique[*Server]` is an error. Fix-it: `go func(){ srv.Run() }()`, whose captured variables live in shared cells ([CON-3](#121-rules)), if the captured variable passes [CON-13](#121-rules); or run the goroutine inside a `sync.Scope`, where borrows are allowed ([CON-16](#121-rules)). `s.Go(...)` of a scope is not a `go` statement, so this rule and OWN-B5 do not apply to it.

**CON-3.** *Closures with `go`.* A closure used with `go` keeps its captured variables in shared cells ([CLO-3](11-closures.md)), so a cell outlives the spawner's block. What the closure may capture is decided by [CON-13](#121-rules): a variable that is moved in, a `Send` value that is copied in, or a `Sync` value that is shared (`Mutex[T]`, `Atomic[T]`, a channel, a `WaitGroup`). Capturing a **writable variable** that the spawner or another goroutine can still touch is a compile error (in Go it compiles and is a data race). A **borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) cannot be captured this way ([CLO-3a](11-closures.md)): `go func(){ fetch(u) }()` or `go func(){ log(s) }()` with such a `u` or `s` is a compile error. Fix-its never reuse the name ([RNG-8](08-slices-and-range.md)): `mine := u.Clone()` first, or `go func(){ fetch(mine) }()`, or `range move(users)` so that `u` is owned.

**CON-4.** *Channels move values.* Sending moves the value in; receiving moves it out ([CON-14](#121-rules): the element type must be `Send`). This includes `for v := range ch`: `v` is an owned local per iteration. A channel, and the values still buffered in it, are freed automatically when nobody holds the channel any more (P12 D12.5). The buffer is owned by the channel, which is a shared handle ([OWN-R1](04-ownership.md#42-ownership-rules-d14), [OWN-C3](04-ownership.md#41-type-categories-d13)); buffered values are dropped oldest first. There is no explicit `ch.Destroy()`: forgetting it would leak, and calling it early would crash. When goroutines must finish before the channel goes away, use `sync.Scope` ([CON-9](#121-rules)).

**CON-5.** *`select`.* A send case moves its value only if that case is **chosen**. A temporary send value is dropped if its case is not chosen. A receive case receives and moves as in CON-4. Random choice among ready cases is as in Go.

**CON-6.** *Receive from a closed channel* gives the zero value. For a type with no zero value (a pointer kind, a func, an interface with methods other than `error`, a C++ type without a default constructor, a struct with a `@Required` field, a `@Strict` struct with a required field), a receive is allowed only in the comma-ok form (`v, ok := <-ch`, or `case v, ok := <-ch:` in a `select`); `v := <-ch`, `case v := <-ch:` and `(<-ch).f` are compile errors. The comma-ok form works, with `v` usable only where `ok` is true ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).

**CON-6a.** *Nil channels.* A nil channel works as in Go: a send or receive on it blocks forever, so a `select` case on a nil channel never fires. `close(ch)` of a channel the compiler knows is nil is a compile error. "Known nil" means a **local** channel variable (not a package variable, not a field, not a parameter) that is nil on every path to the `close`: declared with no value or assigned `nil`, never assigned a non-nil value on any path before the close, and not captured by a function literal or address-taken. So `var ch chan int; close(ch)`, `ch = nil; close(ch)` and `defer close(ch)` of such a variable are errors. A named result counts as a parameter, and a variable that a function literal uses at all counts as captured. A `close` after the channel is checked (`if ch != nil`), in a loop that assigns the variable, or after a `goto` label is not reported. A package-level `var ch chan int` is not checked: `close` panics at run time, as in Go. Every other `close` of a nil channel panics as in Go (ADR-0018 amendment point 5).

**CON-7.** *Memory model.* Go's happens-before model. A channel send happens before the matching receive completes; `sync` types give the usual edges: an unlock of a `Mutex[T]` (guard drop) happens before the next lock; `Atomic[T]` operations are sequentially consistent ([CON-17](#121-rules)); the end of `sync.Scope` happens after every goroutine of the scope ([CON-16](#121-rules)).

**CON-8.** *Data races.* In normal code a data race is a **compile error**: the rules are [CON-12 to CON-20](#121-rules). Data races are undefined behavior only in the cases of [CON-21](#121-rules) ([UB-3](16-undefined-behavior.md)). `-race` (TSan) still detects them there.

**CON-9.** *`sync` types.* `WaitGroup`, `Once` and `Cond` keep Go's API. `Mutex[T]` and `RWMutex[T]` replace `sync.Mutex` and `sync.RWMutex` ([CON-15](#121-rules)). All of them are `@Pinned`: moving or copying one after first use is a compile error ([OWN-R10](04-ownership.md#42-ownership-rules-d14)). `sync.Scope` ([CON-16](#121-rules)) waits for every goroutine it started; closures passed to `s.Go` may borrow without cells.

> **Deferred to P12:** the marker that tells the compiler that a closure given to `s.Go` cannot outlive the scope (for example an attribute such as `@Scoped` on the parameter of `s.Go`). `Cond` waits on a guard ([CON-15](#121-rules)). What is allowed in a scope is fixed by [CON-16](#121-rules).

**CON-10.** *Program exit with goroutines running.* Other goroutines are stopped before `main`'s locals are dropped; see [INI-2](15-program-init.md). A stopped goroutine's stack is not unwound.

**CON-11.** *Panics in goroutines.* An unrecovered panic in any goroutine prints the panic, stops everything and exits with code 2 ([PAN-10](09-errors-and-panics.md)). `go f!()` panics there on error ([ERR-11a](09-errors-and-panics.md)).

**CON-12.** *Two labels on every type: `Send` and `Sync`.* The compiler computes them. They are never written on a type by hand.
- **`Send`:** a value of the type may be moved to another goroutine.
- **`Sync`:** a value of the type may be shared between goroutines at the same time.
- Plain values with no pointers (numbers, `bool`, arrays and structs of them) are both.
- An owning type (`string`, `[]T`, `map[K]V`, `Unique[*T]`, a type with `drop`) is `Send` if its parts are.
- A struct or array has a label only if **all** its fields or elements have it.
- A raw pointer, a borrow and a view are not `Send` across a `go`, unless the target is `Sync` and the use is inside a `sync.Scope` ([CON-16](#121-rules)).
- `Mutex[T]` and `RWMutex[T]` are `Sync` only when `T` is `Send`. `Atomic[T]` is always `Sync` (for its allowed `T`). Channels, `WaitGroup`, `Once` and `context` are `Sync`. `Pool[T]` is `Sync` when `T` is `Send`; `sync.Map[K, V]` is `Sync` when `K` and `V` are `Send` (names illustrative, P12.13).
- `Shared[*T]` is `Send` and `Sync` only when `T` is `Sync`. A non-`Sync` `T` cannot cross goroutines through `Shared` ([CON-19](#121-rules)). Plain data with no interior writes is `Sync`, so read-only sharing of ordinary structs works.
- **Interfaces.** An interface type is `Send` / `Sync` only if it embeds the `Send` / `Sync` constraint, like Rust's `dyn Trait + Send`. The predeclared `error` is declared with `Send` and `Sync`: converting a value to `error` requires its dynamic type to be `Send` and `Sync` (most error types are plain data). `any` has no labels; `interface{ Send }` is a sendable `any`. A conversion into a labeled interface is checked at the conversion.
- **Func values.** The labels of a func value come from its captures, and are checked where the value is created: a function literal, a method value, or a top-level function (no captures: always both). A func-typed variable or parameter whose origin is unknown is not `Send`. `go f()` with a top-level `f` is fine.
- A map, a slice or a plain struct is `Sync` for reading, but no goroutine may write it while it is shared. Writing shared data needs `Mutex[T]` or `Atomic[T]` (for example `Mutex[map[string]int]`), or exclusive access through moving it or `Split`.

`Send` and `Sync` are predeclared constraints, written like `Copy` and `Clone`: `[T Send]` ([GEN-11](10-generics.md)). How to write a `Send` func type is an open point (ADR-0022 open point 1).

**CON-13.** *What `go` may pass or capture.* `go f(args)` and `go func(){…}()` may pass or capture only:
- values that are **moved in** (the caller loses them; later use is an error);
- values whose type is `Send` and that are **copied in**;
- `Sync` values that are **shared**.

Capturing a **writable variable** that the starting goroutine or anyone else can still touch is a compile error. The message suggests `Mutex[T]`, `Atomic[T]` or a channel. A variable that nobody else touches after the `go` counts as moved in. The check is conservative.

```go
x := 0
go func() { x++ }()          // error: x is written by the goroutine and the spawner can touch it.
                             // Fix: sync.Atomic[int], sync.Mutex[int], or send results on a channel
var n sync.Atomic[int]
go func() { n.Add(1) }()     // ok: Atomic is Sync, shared

p := &x
go func() { *p = 1 }()       // error: a raw pointer to a local (CON-2)
ys := []int{1, 2}
go sum(ys)                   // ok: ys is moved in (the caller loses it)
```

**CON-14.** *Channel send moves.* After `ch <- v`, `v` is moved (the use-after-move rules apply, [OWN-R4](04-ownership.md#42-ownership-rules-d14)). Sending a `Copy` value copies it. The element type of a channel must be `Send`: `chan T` with a `T` that is not `Send` (for example `chan View[string]`, or a type holding a guard) is a compile error. This adds the label check to [CON-4](#121-rules) and [CON-5](#121-rules).

**CON-15.** *`Mutex[T]` and `RWMutex[T]` hold the data.* The data lives inside the mutex.
- `m.Lock()` returns a **guard**. The data is reached only through the guard, which is used like a `*T` (the surface is set in P12.13).
- The guard unlocks when it is **dropped**: end of its block, early return, panic. There is no hand-written `Unlock`.
- `m.With(func(p *T) {...})` locks, runs the function, and unlocks.
- `RWMutex[T]` also has `RLock()`, which returns a **read guard**. Reading through it is read-only with the same deep read-only rules as a view ([OWN-V1](04-ownership.md#44-views-d16)), but the guard itself is not a `View[T]` value.
- A panic inside a lock section drops the guard while unwinding, so the mutex is unlocked. There is no poisoning, the same as Go.
- `sync.Cond` waits on a guard: `cond.Wait(&g)` (names illustrative, P12.13).
- A guard is its own type, never a `View`. It is a non-`Copy` borrow that unlocks on drop and cannot be copied or sent. `Lock()` returns a write guard.
- A guard is a **borrow of the mutex** ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)): the mutex may not be moved or dropped while the guard is live. A guard is not `Send`: it cannot be sent, returned through a channel or passed to `go`.
- `Mutex[struct{}]` is the lock-only form.
- Plain `sync.Mutex`, `sync.RWMutex` (no data) and manual `Unlock` are **removed**.
- Mutexes are `@Pinned` ([OWN-R10](04-ownership.md#42-ownership-rules-d14)).

```go
type Stock struct{ N int }
var stock = sync.NewMutex(Stock{N: 3})

func take() bool {
    g := stock.Lock()        // guard
    if g.N == 0 { return false }   // unlocked here
    g.N--
    return true              // and here
}
stock.With(func(s *Stock) { s.N += 10 })   // short form
```

**CON-16.** *`sync.Scope`.* `sync.Scope` is the normal way to run goroutines that use local data.
- Goroutines started with `s.Go(...)` must finish before `sync.Scope` returns. So they may borrow the function's locals (views, borrowed slices, pointers), following the borrow checker ([OWN-B1 to B5](04-ownership.md#45-borrow-checking-adr-0021)) and the labels ([CON-12](#121-rules)): a value shared by reference must be `Sync`, or read-only for every goroutine.
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

**CON-17.** *`Atomic[T]`.* `Atomic[T]` is for integer, `bool` and pointer kinds. Operations are **sequentially consistent only**: no weaker orderings are offered. An `Atomic[T]` is `Sync`. The `atomic.AddInt64(&x, 1)` style functions on plain variables are **removed**.

**CON-18.** *Global variables.* A package-level variable that is written after package initialization (anywhere outside `init` and the initializer, [INI-1](15-program-init.md)) must have a `Sync` type (`Atomic[T]`, `Mutex[T]`, …). Writing any other package variable outside init is a compile error. A package variable that is never written after init is read-only and is fine for any type.

```go
var hits sync.Atomic[int]        // ok: Sync
var cache = sync.NewMutex(map[string]int{})   // ok: Sync
var counter int
func Inc() { counter++ }         // error: counter is written after init and is not Sync
```

**CON-19.** *Shared handles across goroutines.* A `Shared[*T]` is `Send` and `Sync` only when `T` is `Sync` (Rust's `Arc` rule). A non-`Sync` `T` cannot cross goroutines through `Shared` at all. Plain data with no interior writes is `Sync`, so a shared ordinary struct can be read from many goroutines. A `Shared[*T]` is **read-only in every case, with or without goroutines**, except its cell-typed parts ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021) rule 5): a plain write through the handle is a compile error, while a `mem.Cell`, `Mutex`, `RWMutex` or `Atomic` field is used through its own methods (like Rust's `Rc<Node>` with a `RefCell` field). A `View` never changes anything, not even through a cell. For many goroutines the cell type is `Mutex[T]` / `RWMutex[T]` (`cfg := new_shared(sync.NewMutex(User{}))`). For one goroutine it is `mem.Cell[T]`, which checks borrows at run time and is not `Sync` (so it cannot cross goroutines at all). `Weak[*T]` upgrades to a read-only `Shared`. Copying the handle is an atomic retain ([OWN-C3](04-ownership.md#41-type-categories-d13)).

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
// Moves through a channel
type Job struct{ ID int; Text string }
jobs := make(chan Job, 4)
j := Job{ID: 1, Text: "alpha"}
jobs <- j                // j moves into the channel
// fmt.Println(j.Text)   // error: j used after move
got := <-jobs            // moves out: got is the owner

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
