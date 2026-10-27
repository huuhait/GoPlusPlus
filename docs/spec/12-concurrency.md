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
- **owned places** (a field, element, map entry, package variable or pointer target) and **borrowed variables** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) are a **compile error**: a place read is only a borrow ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)) and a borrow cannot be handed to a goroutine that may outlive it. Fix-its: `clone(x)`; `move(x)` for owned places (never for a borrowed variable, except a slice range variable, [OWN-R3b](04-ownership.md#42-ownership-rules-d14)); for a borrowed parameter also making it `own`; and a closure ([CON-3](12-concurrency.md)) only for owned places of locals, because a closure cannot capture a borrowed variable ([CLO-3a](11-closures.md));
- a **raw pointer or a view** into a local (or into the target of a local `Unique`) is a compile error ([CON-2](12-concurrency.md)).

```go
for _, u := range users {   // u borrows the element
    go fetch(u)             // error: borrowed variable. Fix-it: go fetch(clone(u))
}
for _, u := range move(users) {
    go fetch(u)             // ok: u is an owned local, moved into the goroutine
}
go fetch(cfg.Name)          // error: field. Fix-its: clone(cfg.Name) or move(cfg.Name)
if s, ok := v.(string); ok {
    go log(s)               // error: s is a borrowed variable (assertion binding). Fix-it: go log(clone(s))
}
```

> **Open (P01 review):** D1.13 says only "trivial arguments are copied and owned ones are moved". The case list above applies D1.4 rule 3a and rule 8 to `go`, the same way [PAN-3](09-errors-and-panics.md) does for `defer`. The one difference: a `defer` runs before its function returns, so it may *borrow* a variable that lives that long; a goroutine can outlive the function, so `go` never borrows an owned place or a borrowed variable.

**CON-2.** *No borrows of locals across `go`.* A raw pointer or a view into a local, or into the target of a local `Unique`, passed to `go` is a **compile error** outside `unsafe`. Example: `go srv.Run()` with a local `Unique[Server]` is an error. Fix-it: `go func(){ srv.Run() }()`, whose captured variables live in shared cells ([CON-3](#121-rules)).

**CON-3.** *Closures with `go`.* A closure used with `go` keeps its captured variables in shared cells ([CLO-3](11-closures.md)). The goroutine and the spawner see the same variables, as in Go. A **borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) cannot be captured this way ([CLO-3a](11-closures.md)): `go func(){ fetch(u) }()` or `go func(){ log(s) }()` with such a `u` or `s` is a compile error. Fix-it: `u2 := clone(u)` first, or `range move(users)` so that `u` is owned.

**CON-4.** *Channels move values.* Sending moves the value in; receiving moves it out. This includes `for v := range ch`: `v` is an owned local per iteration. A value still buffered when the last handle to the channel goes away is dropped.

> **Open (P01 review):** the buffered-value drop rule comes from P12 D12.5; D1.13 does not state it. It is repeated here as the most consistent reading of rules R1 and C3 (the channel is a shared handle, and its buffer is owned by it). Drop order inside the buffer is not specified (reading: oldest first).

**CON-5.** *`select`.* A send case moves its value only if that case is **chosen**. A temporary send value is dropped if its case is not chosen. A receive case receives and moves as in CON-4. Random choice among ready cases is as in Go.

**CON-6.** *Receive from a closed channel* gives the zero value. For a C++ type without a default constructor this is a compile error ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).

**CON-7.** *Memory model.* Go's happens-before model. A channel send happens before the matching receive completes; `sync` types give the usual edges.

**CON-8.** *Data races are undefined behavior* (as in C++). `-race` (TSan) detects them. See [UB-3](16-undefined-behavior.md).

**CON-9.** *`sync` types.* `sync.Mutex`, `RWMutex`, `WaitGroup`, `Once` and `Cond` keep Go's API and are `@Pinned`: moving or copying one after first use is a compile error ([OWN-R10](04-ownership.md#42-ownership-rules-d14)). `sync.Scope` (P12 D12.7) is an optional helper that waits for every function it spawned; closures passed to it may borrow without cells.

> **Open (P01 review):** the plan says closures given to `sync.Scope` "may borrow without cells" but does not say how the compiler knows the closure cannot outlive the scope. This spec leaves the exact rule (an attribute on the parameter) to P12.

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
