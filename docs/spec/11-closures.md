# 11. Closures

Base: Go spec [Function literals](https://go.dev/ref/spec#Function_literals), [Method values](https://go.dev/ref/spec#Method_values), [Defer statements](https://go.dev/ref/spec#Defer_statements), [Go statements](https://go.dev/ref/spec#Go_statements), [For clause](https://go.dev/ref/spec#For_clause) (per-iteration loop variables).

Delta D1.11 (decides Q1). The goal: **closures behave exactly like Go**. Go++ only chooses a cheaper storage when it is safe.

Syntax: no change (function literals are `func(…) { … }` or `func!(…) { … }`).

A func value is never nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)): `var f func()` and `f == nil` are compile errors. An optional callback is an `Option[func()]`, checked before the call: `if f := s.OnStop; f != nil { f() }`.

## 11.1 Rules

**CLO-1.** *Non-escaping closures capture by reference.* A closure that is only called locally, or passed to a parameter the callee does not keep, does not escape. Assigning it to a local variable is **not** an escape, as long as that variable does not outlive any variable the closure captures (see CLO-3). Apart from `defer`, a closure is only non-escaping if it cannot outlive its captured variables. A function literal in a `defer` statement is **never** an escape and never gets a cell: it always captures by reference, and [CLO-3b](11-closures.md) decides whether that is legal.

**CLO-2.** *Escaping closures and thread safety.* An escaping closure keeps each captured variable in a reference-counted **cell** that lives until the last reference to the cell goes away:
- **Plain `func(...)` (`Send + Sync` by default, [CON-12](12-concurrency.md)):** Because a plain escaping `func(...)` may be sent on a `chan func()`, passed to `go`, or registered in a concurrent handler (`http.HandleFunc`), its captures must be thread-safe (`Sync` or read-only `Send + Sync`: numbers, `string`, structs of them, `Mutex[T]`, `RWMutex[T]`, `Atomic[T]`, `chan T`, `WaitGroup`, `Once`, `Shared[T]` when `T` is `Sync`). Mutating an unsynchronized captured variable (`n++`) in an escaping plain `func(...)` is a compile error (fix-it: use `sync.Atomic` / `sync.Mutex`, or `Local[func(...)]` for single-goroutine callbacks). (Exception: a literal called directly in `go func(){...}()` runs once on one goroutine and may take over variables that nobody else touches afterwards, per [CON-13](12-concurrency.md).)
- **`Local[func(...)]` (single-goroutine opt-out, [CON-12](12-concurrency.md)):** An escaping closure typed as `Local[func(...)]` uses non-atomic reference-counted cells and allows multiple closures and the enclosing function to read and mutate the same unsynchronized variable (`n++`), just like Go, while preventing the closure from crossing `go` or `chan`.
- **Non-escaping closures (`CLO-1`):** Closures passed to non-keeping parameters (`slices.SortFunc`, `mu.With`), `defer` literals (`CLO-3b`), and local `func` variables that do not outlive their captures never escape and may mutate local variables freely as plain `func(...)` with zero annotations.

**CLO-3.** *What escapes.* A closure escapes when it is:
- returned;
- stored in a field, element, package variable, map or channel;
- passed to a parameter that the callee may keep (store, return, or pass on to a place that keeps it), as in `http.HandleFunc("/", func(…) { … })`;
- used with `go`;
- captured by an escaping closure;
- stored in a local variable declared in an outer scope than some variable it captures, so that the closure could be called after the captured variable has died.

The last bullet keeps the cell rule honest: a closure stored in an outer variable keeps its captures alive. Without it, `var f Option[func()]; { s := "x"; f = func() { use(s) } }; if f != nil { f() }` would call a closure whose captured `s` is already destroyed, and Go prints `x`. Treating it as an escape (a shared cell) keeps the rule that the choice of cell or reference never changes what the program does ([CLO-4](11-closures.md)). The same applies to `last = func() int { return i }` in a loop body, where `last` is declared outside the loop and `i` is per iteration ([CLO-8](11-closures.md)).

**CLO-3a.** *Closures that capture borrowed variables.* A read-only borrowed variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14): e.g. a plain range variable over elements that cannot be copied), a tracked borrow (`*T`, a view `&s[i:j]`, borrow-carrying struct), or an `&` range variable ([RNG-1a](08-slices-and-range.md)) cannot be put in a shared cell, because the cell would outlive the borrow. A closure that runs later (used with `go`, stored, or returned; see CLO-3) cannot capture one: it is a compile error. Fix-its never reuse the loop variable's name ([RNG-8](08-slices-and-range.md)): when the type can be copied, copy it into a differently named local first (`mine := u.Clone()`), or pass a clone (`go fetch(u.Clone())`). Capturing a borrowed variable in a **non-escaping** closure is fine. A closure given to `s.Go` of a `sync.Scope` is also not escaping for this rule: its goroutine ends before the scope returns, so it may capture borrowed variables, subject to [CON-16](12-concurrency.md). Variables that are not borrowed (trivial `OWN-C1` values, `string`, `[]T`, `map[K]V`, `Doc`, `Shared[T]`, `chan T`, etc.) may be captured freely: the closure uses the variable itself, so capturing a slice or a map makes no copy ([ADR-0041](../adr/0041-values-are-copied.md) rule 11a).

**CLO-3b.** *Deferred function literals.* A `defer` is not an escape in the sense of CLO-3, so a deferred closure captures by reference. A deferred literal may capture any variable declared at function level, including named results, and these may be assigned after the `defer` statement. Capture is by reference, so the closure sees the latest value when it runs, exactly like Go (`defer func() { if err != nil { rollback() } }(); err = work()` works).

A deferred literal may also capture an owned local of a nested block or a loop body, as in Go. The compiler extends that variable's lifetime ([PAN-3](09-errors-and-panics.md)): it is not destroyed at the end of the block, but after the deferred call has run, at function exit. Nothing is moved or zeroed, and the literal sees the latest value. A variable that is **borrowed** cannot have its lifetime extended, so a deferred literal that captures one (for example a plain range variable over elements that cannot be copied, which dies at the end of its iteration) is a compile error. Fix-it, when the type can be copied: capture a clone (`mine := r.Clone()`). A map alias can never be captured by a deferred literal ([RNG-1a](08-slices-and-range.md)). Other `&` variables may be captured only if the collection lives until the function returns and is not appended to or reassigned before then (visible: compile error, [OWN-B2](04-ownership.md#45-borrow-checking-adr-0021); through another pointer: compile error too, [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)).

```go
func read(paths []string) {
    for _, p := range paths {
        f := os.Open!(p)
        defer func() { f.Close() }()   // ok, as in Go: f lives until the function returns
    }                                  //   and stays open until read returns
}
func each(res []Resource) {
    for _, r := range res {            // Resource cannot be copied (drop, no Clone)
        defer func() { show(&r) }()    // error: r borrows res[i], which ends with the iteration
    }
}
```

**CLO-4.** *Escape analysis.* Like Go, the compiler infers for every parameter whether the callee may keep it, and records the result in export data with the other inferred attributes (P06 D6.2). A call through a func value or an interface method assumes the parameter is kept. The choice between a cell and a plain reference **never changes what the program does**. It only removes allocations.

**CLO-5.** *Method values.* A method value (`f := s.Handle`) is a closure over its receiver.

**CLO-5a.** *Value receivers.* Evaluating a method value `s.Handle` on a value receiver evaluates `s` at that moment, matching Go:
- for a type that can be copied, `s.Handle` copies `s` into the closure, as passing `s` to a by-value receiver does ([ADR-0041](../adr/0041-values-are-copied.md) rule 1: a `string` is retained, a slice or map field is copied with its elements), so later changes of `s` do not affect `s.Handle`; the copy is skipped when `s` is not used again;
- for a value receiver of a type that cannot be copied (`File`), `s.Handle` takes `s` into the closure if `s` is a temporary or is written `move(s).Handle`, and is a compile error otherwise (a plain copy, [ADR-0041](../adr/0041-values-are-copied.md) rule 3).

For a pointer receiver, the variable is captured by reference like any captured variable (as in Go, it binds `&s`; shared cell if the method value escapes).

**CLO-6.** *Recursion.* A recursive escaping closure creates a reference cycle; `gopp vet` warns. Non-escaping recursive closures are fine.

**CLO-7.** *`defer` arguments.* Deferred calls evaluate their arguments at the `defer` statement, as in Go ([PAN-3](09-errors-and-panics.md)):
- by-value arguments are copied at the `defer` statement ([ADR-0041](../adr/0041-values-are-copied.md) rule 1: `string`, `Shared[T]` and other counted handles are retained; a `[]T` or `map[K]V` is copied with its elements; an interface value is copied with the value it holds), so `s := "a"; defer fmt.Println(s); s = "b"` prints `"a"`, identical to Go. A later write to an element of a slice or map argument is not seen by the deferred call (Go sees it); pass `&xs` to see it;
- `*T` arguments and pointer receivers (`defer x.M()`, `defer f(&x)`) evaluate `&x` at the `defer` statement and observe mutations made to `x` before function exit, identical to Go; moving `x` while `&x` is held by a pending `defer` is a compile error ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)).

**CLO-8.** *Loop variables.* As in Go 1.22 and later, each iteration of a `for` loop has its own copy of the loop variable. A closure that captures it and escapes gets one cell per iteration. Each new copy is made from the previous iteration's variable, so the type must be one that can be copied ([ADR-0041](../adr/0041-values-are-copied.md) rule 3). A loop variable of a type with `drop` and no `Clone()` captured by an escaping (or `defer`red) function literal is **E0452 at the loop**: the value does not move forward to the next iteration, because the closures of the earlier iterations would silently read a zero value (ADR-0006 amendment of 2026-10-31). A non-escaping literal called only within its own iteration captures by reference and needs no `Clone()`.
```go
type F struct{ p unsafe.Pointer }        // has drop, no Clone()
func (f *F) drop() { if f.p != nil { free(f.p) } }

var fs []Local[func() bool]
var i int
for f := (F{malloc(16)}); i < 3; i++ {   // error E0452: f is captured and F has no Clone()
    fs = append(fs, func() bool { return f.p != nil })
}
// fix: give F a Clone() method; then each iteration has its own copy of f
func (f F) Clone() F { return F{malloc(16)} }
```

**CLO-9.** *Captured variables are non-local places.* Inside a closure body, a captured variable lives in its enclosing frame or shared cell rather than as a local of the closure:
- a captured variable of a type that can be copied (numbers, `string`, `[]T`, `map[K]V`, `Shared[T]`, etc.) can be read, assigned, returned or stored freely; returning or storing it is a copy, because the variable stays in use ([ADR-0041](../adr/0041-values-are-copied.md) rule 1);
- taking a captured variable out of its shared cell without a copy (the only way for a type that cannot be copied, `OWN-C3`) uses `mem.Take(&x)` (when the type has a zero value, [OWN-R4](04-ownership.md#42-ownership-rules-d14)) or `mem.Replace(&x, v)`, not `move(x)` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)), because the closure may be called more than once; moving a local in the enclosing function after a closure has captured it is a compile error ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)).
Flow typing does not reach into a closure body: a captured `Option` or checked result (`v` of `v, err := f()`) is not known set inside the closure because the closure can run after the variable changes; copy or unwrap it into a local first ([OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17)).

## 11.2 Examples (each compared with Go)

### Example 1: a returned single-goroutine pair shares a counter

```go
func counter() (inc Local[func() int], get Local[func() int]) {
    n := 0                            // shared unsynchronized cell on 1 goroutine -> Local[func() int]
    inc = func() int { n++; return n }
    get = func() int { return n }
    return
}
inc, get := counter()
inc(); inc()
fmt.Println(get())                    // 2
```
Go: same output, `n` moves to the heap. Go++: `n` lives in a non-atomic refcounted cell, and `Local[...]` ensures `inc` and `get` cannot be sent across goroutines to race on `n` (use `sync.Atomic[int]` if returning a multi-goroutine plain `func() int`).

### Example 2: one variable per loop iteration

```go
var fs []func() int
for i := 0; i < 3; i++ {
    fs = append(fs, func() int { return i })   // escapes (stored in a slice): one cell per iteration
}
for _, f := range fs { fmt.Print(f(), " ") }   // 0 1 2
```
Go 1.22+: prints `0 1 2`. Go++: the same. **Same behavior.**

### Example 3: a non-escaping closure uses no cell

```go
total := 0
slices.SortFunc(&xs, func(a, b int) int {      // SortFunc does not keep its parameter
    total++                                    // captured by reference: no allocation
    return cmp.Compare(a, b)
})
fmt.Println(total > 0)
```
Go: `total` stays on the stack (escape analysis). Go++: same idea, same result. **Same behavior.**

### Example 4: `go` and shared state

```go
var wg sync.WaitGroup
sum := sync.NewMutex(0)                        // the mutex holds the data (CON-15)
for i := 1; i <= 3; i++ {
    wg.Add(1)
    go func() {                                // `go` always escapes: wg and sum live in cells
        defer wg.Done()
        sum.With(func(p *int) { p += i })      // i is a Copy value, copied in (CON-13)
    }()
}
wg.Wait()
fmt.Println(sum.Lock())                        // 6: the guard is used as the value
```
Go: same result, but there `sum` is a plain `int` and `mu.Lock()` is a separate call; Go++ does not allow that (a plain captured `sum` written by the goroutines is an error, [CON-13](12-concurrency.md)). The `sync` types are `@Pinned` ([OWN-R10](04-ownership.md#42-ownership-rules-d14)); a cell never moves them, so this is legal. **Different code, same output.**

### Example 5: a method value is a closure over its receiver

```go
type Greeter struct{ Name string }
func (g *Greeter) Hello() string { return "hi " + g.Name }

g := Greeter{Name: "ann"}
h := g.Hello             // pointer receiver: captures the variable g (not escaping: by reference)
g.Name = "bob"
fmt.Println(h())         // "hi bob" in Go and in Go++
```
Go: `g.Hello` binds `&g`. Go++: the same. If `h` is returned, `g` goes to a cell in Go++ (to the heap in Go). **Same behavior.** For value receivers see CLO-5a.

### Example 6: recursion

```go
var fib Option[func(int) int]                  // a func is never nil: Option until it is set
fib = func(n int) int {                        // not escaping: plain func(int) int is fine
    f, ok := fib.Get()
    if n < 2 || !ok { return n }
    return f(n-1) + f(n-2)
}
if fib != nil { fmt.Println(fib(10)) }         // 55

func makeFib() Local[func(int) int] {
    var fib Option[Local[func(int) int]]
    f := Local[func(int) int](func(n int) int { g, ok := fib.Get(); if n < 2 || !ok { return n }; return g(n-1) + g(n-2) })
    fib = f
    return f                                   // escaping: f captures fib, which holds f -> cycle
}                                              // gopp vet warns: the cell is never freed
```
Go (with plain `var fib func(int) int`): both work (the GC collects the cycle). Go++: the first works with plain `func`. The second mutates `fib` after capture (`Local[...]`) and leaks its cycle; vet warns. **Different only in memory use.**

### Example 7: `defer` arguments are evaluated at the `defer`

```go
func f() {
    x := 1
    defer fmt.Println(x)     // x is copied now (trivial): prints 1
    x = 2
}

func g() {
    s := "a"
    defer fmt.Println(s)     // s ("a") is retained at the defer: prints "a"
    s = "b"
}
```
Go: both compile; `f` prints `1` and `g` prints `a`. Go++: **identical behavior**.

### Example 8: storing a single-goroutine UI callback vs a `Send` callback

```go
type Button struct{ onClick Local[func()] }
func (b *Button) OnClick(f Local[func()]) { b.onClick = f } // single-goroutine UI widget

count := 0
btn.OnClick(func() { count++ })    // ok: Local[func()] shares `count` on 1 goroutine
btn.onClick(); btn.onClick()
fmt.Println(count)                 // 2
```
Go: same. Go++: `Local[func()]` lets UI/event callbacks mutate shared local variables on 1 goroutine while keeping plain `func()` safe for `chan func()` and `go`.

## 11.3 Rationale

- This is Go's behavior (Go moves such variables to the heap), so closures never silently diverge from Go.
- The compiler removes the cell wherever it can prove no closure outlives the variable. That is an optimization only, never a change of meaning.
- **Rejected:** capture by value with a compile error on later mutation (an earlier draft): it rejected common Go code, such as returned closure pairs that share a counter. Explicit capture lists: not Go.
