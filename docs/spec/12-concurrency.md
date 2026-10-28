# 12. Concurrency

Base: Go spec [Go statements](https://go.dev/ref/spec#Go_statements), [Channel types](https://go.dev/ref/spec#Channel_types), [Send statements](https://go.dev/ref/spec#Send_statements), [Receive operator](https://go.dev/ref/spec#Receive_operator), [Select statements](https://go.dev/ref/spec#Select_statements), [Close](https://go.dev/ref/spec#Close), [Program execution](https://go.dev/ref/spec#Program_execution). Memory model: [Go memory model](https://go.dev/ref/mem).

Delta D1.13. `go`, `chan` and `select` work as in Go. What is new is what happens to **owned values** and to **borrows**.

Syntax: no change.

## 12.1 Rules

**CON-1.** *`go f(a, b)`.* The function and its arguments (the receiver counts as an argument) are evaluated, then given to the new goroutine. By kind of argument:
- **trivial** values are copied;
- **shared handles** are copied, which retains them;
- **owned temporaries** (call results, conversions, composite literals) are moved in;
- **owned locals** are moved in (`go fetch(u)` with a local `u` moves `u`; later use of `u` is an error);
- **owned places** (a field, element, map entry, package variable or pointer target), **borrowed variables** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) and `&` range variables ([RNG-1a](08-slices-and-range.md)) are a **compile error**: a place read is only a borrow ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)) and a borrow cannot be handed to a goroutine that may outlive it. Fix-its: `x.Clone()`; `move(x)` for owned places and `&` range variables (never for a borrowed variable, [OWN-R3b](04-ownership.md#42-ownership-rules-d14)); for a borrowed parameter also making it `move`; and a closure ([CON-3](12-concurrency.md)) only for owned places of locals, because a closure cannot capture a borrowed variable ([CLO-3a](11-closures.md));
- a **raw pointer or a ref** into a local (or into the target of a local `Unique`) is a compile error ([CON-2](12-concurrency.md)).

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

The case list above applies D1.4 rule 3a and rule 8 to `go`, the same way [PAN-3](09-errors-and-panics.md) does for `defer`. The one difference: a `defer` runs before its function returns, so it may *borrow* a variable that lives that long; a goroutine can outlive the function, so `go` never borrows an owned place or a borrowed variable. `go f(x)` moves owned locals. Fields and borrowed variables need `x.Clone()`, or `move(...)` for owned fields and `&` range variables (never for a borrowed variable).

**CON-2.** *No borrows of locals across `go`.* A raw pointer or a ref into a local, or into the target of a local `Unique`, passed to `go` is a **compile error** outside `unsafe`. Example: `go srv.Run()` with a local `Unique[*Server]` is an error. Fix-it: `go func(){ srv.Run() }()`, whose captured variables live in shared cells ([CON-3](#121-rules)).

**CON-3.** *Closures with `go`.* A closure used with `go` keeps its captured variables in shared cells ([CLO-3](11-closures.md)). The goroutine and the spawner see the same variables, as in Go. A **borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) cannot be captured this way ([CLO-3a](11-closures.md)): `go func(){ fetch(u) }()` or `go func(){ log(s) }()` with such a `u` or `s` is a compile error. Fix-its never reuse the name ([RNG-8](08-slices-and-range.md)): `mine := u.Clone()` first, or `go func(){ fetch(mine) }()`, or `range move(users)` so that `u` is owned.

**CON-4.** *Channels move values.* Sending moves the value in; receiving moves it out. This includes `for v := range ch`: `v` is an owned local per iteration. A channel, and the values still buffered in it, are freed automatically when nobody holds the channel any more (P12 D12.5). The buffer is owned by the channel, which is a shared handle ([OWN-R1](04-ownership.md#42-ownership-rules-d14), [OWN-C3](04-ownership.md#41-type-categories-d13)); buffered values are dropped oldest first. There is no explicit `ch.Destroy()`: forgetting it would leak, and calling it early would crash. When goroutines must finish before the channel goes away, use `sync.Scope` ([CON-9](#121-rules)).

**CON-5.** *`select`.* A send case moves its value only if that case is **chosen**. A temporary send value is dropped if its case is not chosen. A receive case receives and moves as in CON-4. Random choice among ready cases is as in Go.

**CON-6.** *Receive from a closed channel* gives the zero value. For a type with no zero value (a pointer kind, a func, an interface with methods other than `error`, a C++ type without a default constructor, a struct with a `@Required` field), a receive is allowed only in the comma-ok form (`v, ok := <-ch`, or `case v, ok := <-ch:` in a `select`); `v := <-ch`, `case v := <-ch:` and `(<-ch).f` are compile errors. The comma-ok form works, with `v` usable only where `ok` is true ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).

**CON-6a.** *Nil channels.* A nil channel works as in Go: a send or receive on it blocks forever, so a `select` case on a nil channel never fires. `close(ch)` of a channel the compiler knows is nil is a compile error. "Known nil" means a **local** channel variable (not a package variable, not a field, not a parameter) that is nil on every path to the `close`: declared with no value or assigned `nil`, never assigned a non-nil value on any path before the close, and not captured by a function literal or address-taken. So `var ch chan int; close(ch)` and `ch = nil; close(ch)` are errors. A package-level `var ch chan int` is not checked: `close` panics at run time, as in Go. Every other `close` of a nil channel panics as in Go (ADR-0018 amendment point 5).

**CON-7.** *Memory model.* Go's happens-before model. A channel send happens before the matching receive completes; `sync` types give the usual edges.

**CON-8.** *Data races are undefined behavior* (as in C++). `-race` (TSan) detects them. See [UB-3](16-undefined-behavior.md).

**CON-9.** *`sync` types.* `sync.Mutex`, `RWMutex`, `WaitGroup`, `Once` and `Cond` keep Go's API and are `@Pinned`: moving or copying one after first use is a compile error ([OWN-R10](04-ownership.md#42-ownership-rules-d14)). `sync.Scope` (P12 D12.7) is an optional helper that waits for every function it spawned; closures passed to it may borrow without cells.

> **Deferred to P12:** how the compiler knows that a closure given to `sync.Scope` cannot outlive the scope. The marker is designed in P12 (for example an attribute such as `@Scoped` on the parameter of `s.Go`).

**CON-10.** *Program exit with goroutines running.* Other goroutines are stopped before `main`'s locals are dropped; see [INI-2](15-program-init.md). A stopped goroutine's stack is not unwound.

**CON-11.** *Panics in goroutines.* An unrecovered panic in any goroutine prints the panic, stops everything and exits with code 2 ([PAN-10](09-errors-and-panics.md)). `go f!()` panics there on error ([ERR-11a](09-errors-and-panics.md)).

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
go func() { defer wg.Done(); process(got) }()   // `got` is captured too, in a cell
wg.Wait()

// go with a raw pointer to a local: rejected
var wg2 sync.WaitGroup
go worker(&wg2)          // error: raw pointer to a local passed to go. Fix-it: use a closure

// select moves only the chosen send
select {
case out <- msg:         // msg moves only if this case is chosen
case <-done:             // msg is still ours here
}
```

Compared with Go: the programs look the same. Go++ rejects `go worker(&wg)` on a local because the local may be gone when the goroutine runs; Go allows it because `wg` escapes to the GC heap.

## 12.3 Rationale

- Moving through channels makes "who owns this value now?" a compile-time answer and avoids copies.
- Banning borrows of locals in `go` removes the most common dangling bug in a language without a borrow checker. The closure fix-it keeps the code short.
- Races are UB because making them defined would cost every memory access. TSan finds them in tests.
