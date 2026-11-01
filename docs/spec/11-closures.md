# 11. Closures

Base: Go spec [Function literals](https://go.dev/ref/spec#Function_literals), [Method values](https://go.dev/ref/spec#Method_values), [Defer statements](https://go.dev/ref/spec#Defer_statements), [Go statements](https://go.dev/ref/spec#Go_statements), [For clause](https://go.dev/ref/spec#For_clause) (per-iteration loop variables).

Delta D1.11 (decides Q1). The goal: **closures behave exactly like Go**. Go++ only chooses a cheaper storage when it is safe.

Syntax: no change (function literals are `func(…) { … }` or `func!(…) { … }`).

A func value is never nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)): `var f func()` and `f == nil` are compile errors. An optional callback is an `Option[func()]`, checked before the call: `if f := s.OnStop; f != nil { f() }`.

## 11.1 Rules

**CLO-1.** *Non-escaping closures capture by reference.* A closure that is only called locally, or passed to a parameter the callee does not keep, does not escape. Assigning it to a local variable is **not** an escape, as long as that variable does not outlive any variable the closure captures (see CLO-3). Apart from `defer`, a closure is only non-escaping if it cannot outlive its captured variables. A function literal in a `defer` statement is **never** an escape and never gets a cell: it always captures by reference, and [CLO-3b](11-closures.md) decides whether that is legal.

**CLO-2.** *Escaping closures share their variables.* An escaping closure keeps each captured variable in a shared, reference-counted **cell**. All closures and the enclosing function see the same variable, exactly like Go. A captured variable lives until the last reference to its cell goes away, and is destroyed then.

A captured owned variable is destroyed when its cell is released for the last time, not at the end of the block. Otherwise the closure would see a destroyed value. So a captured variable lives as long as the closure that holds the cell.

**CLO-3.** *What escapes.* A closure escapes when it is:
- returned;
- stored in a field, element, package variable, map or channel;
- passed to a `move` parameter;
- passed to a parameter that the callee may keep (store, return, or pass on to a place that keeps it), as in `http.HandleFunc("/", func(…) { … })`;
- used with `go`;
- captured by an escaping closure;
- stored in a local variable declared in an outer scope than some variable it captures, so that the closure could be called after the captured variable has died.

The last bullet keeps the cell rule honest: a closure stored in an outer variable keeps its captures alive. Without it, `var f Option[func()]; { s := "x"; f = func() { use(s) } }; if f != nil { f() }` would call a closure whose captured `s` is already destroyed, and Go prints `x`. Treating it as an escape (a shared cell) keeps the rule that the choice of cell or reference never changes what the program does ([CLO-4](11-closures.md)). The same applies to `last = func() int { return i }` in a loop body, where `last` is declared outside the loop and `i` is per iteration ([CLO-8](11-closures.md)).

**CLO-3a.** *Closures that capture borrowed variables.* A borrowed variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)), or an `&` range variable ([RNG-1a](08-slices-and-range.md)), cannot be put in a shared cell, because the cell would outlive the borrow. A closure that runs later (used with `go`, stored, or returned; see CLO-3) cannot capture one: it is a compile error. Fix-its never reuse the loop variable's name ([RNG-8](08-slices-and-range.md)): copy it into a differently named local first (`mine := u.Clone()`), pass a clone (`go fetch(u.Clone())`), make a borrowed parameter `move`, or use `range move(xs)` so that the range variable is owned. Capturing a borrowed variable in a **non-escaping** closure is fine. A closure given to `s.Go` of a `sync.Scope` is also not escaping for this rule: its goroutine ends before the scope returns, so it may capture borrowed variables, subject to [CON-16](12-concurrency.md). Variables that are not borrowed variables (trivial or shared-handle copies) may be captured freely.

**CLO-3b.** *Deferred function literals.* A `defer` is not an escape in the sense of CLO-3, so a deferred closure captures by reference. A deferred literal may capture any variable declared at function level, including named results, and these may be assigned after the `defer` statement. Capture is by reference, so the closure sees the latest value when it runs, exactly like Go (`defer func() { if err != nil { rollback() } }(); err = work()` works).

A deferred literal may also capture an owned local of a nested block or a loop body, as in Go. The compiler extends that variable's lifetime ([PAN-3](09-errors-and-panics.md)): it is not destroyed at the end of the block, but after the deferred call has run, at function exit. Nothing is moved or zeroed, and the literal sees the latest value. A variable that is **borrowed** cannot have its lifetime extended, so a deferred literal that captures one (for example a range variable over owning elements, which dies at the end of its iteration) is a compile error. Fix-it: capture a clone (`mine := d.Clone()`). A map alias can never be captured by a deferred literal ([RNG-1a](08-slices-and-range.md)). Other `&` variables may be captured only if the collection lives until the function returns and is not appended to or reassigned before then (visible: compile error, [OWN-B2](04-ownership.md#45-borrow-checking-adr-0021); through another pointer: compile error too, [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). The rule "not written after the `defer`" (CLO-7) applies only to *borrowed arguments* of a deferred call ([CLO-7](11-closures.md), [PAN-3](09-errors-and-panics.md)), because an argument is evaluated at the `defer` statement and a literal's captures are not.

```go
func read(paths []string) {
    for _, p := range paths {
        f := os.Open!(p)
        defer func() { f.Close() }()   // ok, as in Go: f lives until the function returns
    }                                  //   and stays open until read returns
}
func each(docs []Doc) {
    for _, d := range docs {
        defer func() { show(d) }()     // error: d is a borrowed variable; capture mine := d.Clone()
    }
}
```

**CLO-4.** *Escape analysis.* Like Go, the compiler infers for every parameter whether the callee may keep it, and records the result in export data with the other inferred attributes (P06 D6.2). A call through a func value or an interface method assumes the parameter is kept. The choice between a cell and a plain reference **never changes what the program does**. It only removes allocations.

**CLO-5.** *Method values.* A method value (`f := s.Handle`) is a closure over its receiver.

**CLO-5a.** *Value receivers of owning types.* In Go, `s.Handle` with a value receiver copies `s` when the method value is evaluated, so later changes to `s` are not seen. A Go++ closure that captured `s` would either see those changes (a silent difference from Go) or need a copy that is not written (a hidden allocation). So for a value-receiver method of an **owning** type, `s.Handle` as a method value is a compile error. Fix-its: `s.Clone().Handle` (snapshot, the closure owns the clone) or `move(s).Handle` (the closure takes `s`). There are no silent copies.

For a pointer receiver, the variable is captured like any captured variable (as in Go, it binds `&s`; shared cell if the method value escapes). For a value receiver of a trivial type, the receiver is copied, as in Go.

**CLO-6.** *Recursion.* A recursive escaping closure creates a reference cycle; `gopp vet` warns. Non-escaping recursive closures are fine.

**CLO-7.** *`defer` arguments.* Deferred calls evaluate their arguments at the `defer` statement, as in Go. A borrowed owning *argument* whose variable is **written** after the `defer` (not a pointer receiver `&x`, which is evaluated as a pointer at the `defer`, [PAN-3](09-errors-and-panics.md)) is a compile error (fix-it: `x.Clone()`). "Written" means any write the compiler can see to the borrowed variable or to anything it owns: assignment, a field or element write, `append`, a call of a pointer-receiver method on it, and **moving it** ([OWN-R3c](04-ownership.md#42-ownership-rules-d14)). This way Go++ never prints the new value where Go prints the old one. Writes through another pointer are compile errors too ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021), [UB-2c](16-undefined-behavior.md)). A closure that captures a borrow and escapes is an error ([OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)).

The wide definition of "written" makes the borrow behave like Go's copy (Go prints `{old 1}` for a struct argument changed after the `defer`). A borrowed `defer` argument written after the `defer` is an error, and `defer show(d.Clone())` gives Go's snapshot.

**CLO-8.** *Loop variables.* As in Go 1.22 and later, each iteration of a `for` loop has its own copy of the loop variable. A closure that captures it and escapes gets one cell per iteration. Each new copy is made from the previous iteration's variable, so the type needs a `Clone()` (or must be trivial or a shared handle). A loop variable of a type with `drop` and no `Clone()` that a function literal captures is **E0452 at the loop**: the value does not move forward to the next iteration, because the closures of the earlier iterations would silently read a zero value (ADR-0006 amendment of 2026-10-31). Until the escape facts are in the checker, E0452 is reported for any capture, so a literal that only runs inside its iteration will be accepted then.
```go
type F struct{ p unsafe.Pointer }        // has drop, no Clone()
func (f F) drop() { free(f.p) }

var fs []func() bool
var i int
for f := (F{malloc(16)}); i < 3; i++ {   // error E0452: f is captured and F has no Clone()
    fs = append(fs, func() bool { return f.p != nil })
}
// fix: give F a Clone() method; then each iteration has its own copy of f
func (f F) Clone() F { return F{malloc(16)} }
```

**CLO-9.** *Captured variables are "other places".* Inside a closure body, a captured owned variable is not a local of the closure, so [OWN-R3a](04-ownership.md#42-ownership-rules-d14) applies: using it as an operand or borrowed argument is fine; taking ownership of it needs `x.Clone()` or `move(x)`. After `move(x)` the variable holds the zero value ([OWN-R5](04-ownership.md#42-ownership-rules-d14)), so calling the closure again is safe. For a Go++ type with no zero value (a `@Required` or `@Strict` struct), `move(x)` of a captured variable is a compile error, because the next call would read zero bytes ([OWN-R4](04-ownership.md#42-ownership-rules-d14)). The opposite direction, moving a variable in the enclosing function while a closure that captured it may still run, is a compile error: [OWN-R3c](04-ownership.md#42-ownership-rules-d14). Flow typing does not reach into the body either: a captured `Option` or checked result (`v` of `v, err := f()`) is never known set inside the closure, even after a check outside or inside it, because the closure can run after the variable changes; copy it into a local first ([OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17)).

So a closure that takes a captured outer variable needs `move(x)` or `x.Clone()`.

## 11.2 Examples (each compared with Go)

### Example 1: a returned pair shares a counter

```go
func counter() (inc func() int, get func() int) {
    n := 0                            // captured by two escaping closures -> one shared cell
    inc = func() int { n++; return n }
    get = func() int { return n }
    return
}
inc, get := counter()
inc(); inc()
fmt.Println(get())                    // 2
```
Go: same output, `n` moves to the heap. Go++: `n` lives in a refcounted cell. **Same behavior.** (An earlier draft that captured by value rejected this common code.)

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
slices.SortFunc(xs, func(a, b int) int {       // SortFunc does not keep its parameter
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
        sum.With(func(p *int) { *p += i })     // i is a Copy value, copied in (CON-13)
    }()
}
wg.Wait()
fmt.Println(*sum.Lock())                       // 6
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
Go: `g.Hello` binds `&g`. Go++: the same. If `h` is returned, `g` goes to a cell in Go++ (to the heap in Go). **Same behavior.** For value receivers of owning types see CLO-5a (compile error with fix-its).

### Example 6: recursion

```go
var fib Option[func(int) int]                  // a func is never nil: Option until it is set
fib = func(n int) int {                        // not escaping: fine
    f, ok := fib.Get()
    if n < 2 || !ok { return n }
    return f(n-1) + f(n-2)
}
if fib != nil { fmt.Println(fib(10)) }         // 55

func makeFib() func(int) int {
    var fib Option[func(int) int]
    f := func(n int) int { g, ok := fib.Get(); if n < 2 || !ok { return n }; return g(n-1) + g(n-2) }
    fib = f
    return f                                   // escaping: f captures fib, which holds f -> cycle
}                                              // gopp vet warns: the cell is never freed
```
Go (with plain `var fib func(int) int`): both work (the GC collects the cycle). Go++: the first works. The second works too but leaks; vet warns. **Different only in memory use.**

### Example 7: `defer` arguments are evaluated at the `defer`

```go
func f() {
    x := 1
    defer fmt.Println(x)     // x is copied now (trivial): prints 1
    x = 2
}

func g() {
    s := "a"
    defer fmt.Println(s)     // error: borrows s, and s is assigned after the defer
    s = "b"                  // fix-it: defer fmt.Println(s.Clone())
}
```
Go: both compile; `g` prints `a`. Go++: `f` is the same. `g` is a compile error with a fix-it, because borrowing `s` would print `b`. **Different on purpose.**

### Example 8: storing or passing a closure to a keeper

```go
type Button struct{ onClick func() }
func (b *Button) OnClick(f func()) { b.onClick = f }     // keeps f: the parameter "may be kept"

count := 0
btn.OnClick(func() { count++ })    // escapes (the callee keeps it): count is in a cell
btn.onClick(); btn.onClick()
fmt.Println(count)                 // 2
```
Go: same. Go++: same; the compiler found by escape analysis that `OnClick` keeps `f`. **Same behavior.**

## 11.3 Rationale

- This is Go's behavior (Go moves such variables to the heap), so closures never silently diverge from Go.
- The compiler removes the cell wherever it can prove no closure outlives the variable. That is an optimization only, never a change of meaning.
- **Rejected:** capture by value with a compile error on later mutation (an earlier draft): it rejected common Go code, such as returned closure pairs that share a counter. Explicit capture lists: not Go.
