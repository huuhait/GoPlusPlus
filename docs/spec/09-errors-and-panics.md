# 9. Errors, `!`, panic, defer and recover

Base: Go spec [Errors](https://go.dev/ref/spec#Errors), [Function declarations](https://go.dev/ref/spec#Function_declarations), [Function literals](https://go.dev/ref/spec#Function_literals), [Function types](https://go.dev/ref/spec#Function_types), [Return statements](https://go.dev/ref/spec#Return_statements), [Defer statements](https://go.dev/ref/spec#Defer_statements), [Go statements](https://go.dev/ref/spec#Go_statements), [Handling panics](https://go.dev/ref/spec#Handling_panics), [Run-time panics](https://go.dev/ref/spec#Run_time_panics), [Comparison operators](https://go.dev/ref/spec#Comparison_operators).

Deltas covered: D1.8 (`!`), D1.9 (panic, defer, recover), D1.19 (errors work like Go).

Contents: [9.1 `!` functions](#91-functions-that-can-fail-d18) · [9.2 `call!()`](#92-unwrapping-calls-d18) · [9.3 errors as values](#93-errors-work-like-go-d119) · [9.4 panic, defer, recover](#94-panic-defer-recover-d19) · [9.5 rationale](#95-rationale)

## 9.1 Functions that can fail (D1.8)

**Go error handling is unchanged.** `val, err := call()` followed by `if err != nil { … }` works exactly as in Go. `error` keeps Go's `nil`, like the empty interface `any` (every interface with methods, and every func, is never nil, [OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)), so `var err error` and `return nil` for an `error` result are unchanged.

### Syntax

```
FunctionDecl = "func" FunctionName [ TypeParameters ] [ "!" ] Signature [ FunctionBody ] .
MethodDecl   = "func" Receiver MethodName [ TypeParameters ] [ "!" ] Signature [ FunctionBody ] .
FunctionLit  = "func" [ "!" ] Signature FunctionBody .
FunctionType = "func" [ "!" ] Signature .
MethodElem   = MethodName [ "!" ] Signature .
PrimaryExpr  = /* Go alternatives */ | PrimaryExpr "!" Arguments .
```

The `!` is always right before the parameter list. After type parameters: `func parse[T any]!(s string) T`, called as `parse[int]!(s)`.

**ERR-1.** *Declaration.* `func name!(…) R` declares a function that **can fail**. It is the same function as `func name(…) (R, error)`, and callers see `(R, error)`.
- Zero results: `func run!()` is `func run() error`.
- Several results: `func f!() (int, string)` is `func f() (int, string, error)`.
- The two forms are interchangeable: they have identical types.

**ERR-2.** *All function forms.* Methods, interface methods, function literals and function types use the same mark: `func (r *R) Read!(p []byte) int`, `interface{ Load!(path string) []byte }`, `func!(x int) T { … }`, `func!(int) T` (identical to `func(int) (T, error)`).

A function **can fail** if it is declared with `!`, or if its last result has type `error`. This property belongs to the innermost function: a function literal is judged by its own signature, not by the function around it. So `x!()` follows the innermost function: if that function can return an error, the error is returned; otherwise it panics. A `x!()` in a plain closure inside a `!` function panics, and does not return from the outer function.

**ERR-3.** *Returning from a `!` function.*
- `return v` returns `v` with a nil error.
- To fail explicitly, add the error as an extra last operand: `return nil, err`, or `return err` when there are no other results.
- `return g()` is allowed when `g` returns the declared results plus `error`.
- In a failing return, a result whose type has no zero value (a pointer, a func, an interface with methods other than `error`, a `@Required` or `@Strict` struct, …) is written `nil`: `return nil, p.fail("x")` for a `func p.parse!() *Node`. That result is left unset, and no caller can read it. A failing `x!()` inside such a function does the same. A Go-style function whose last result is `error` but that is not declared with `!` can't leave a result unset: there `return nil, err` with a `*Node` result is a compile error with the fix-its "declare it with `!`" or "return `Option[*Node]`" (ADR-0018).
- A caller that uses the Go form `v, err := f()` may use `v` only where `err == nil` is known whenever `v`'s type has no zero value, for example after `if err != nil { return err }` or inside `if err == nil { … }`: the same flow typing as for an `Option` and for comma-ok ([OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17)). Reading such a `v` where `err` may be set is a compile error. A `v` whose type has a zero value stays usable, as in Go: `n, err := r.Read(buf)` uses `n` even when `err != nil`. This holds for **every** call whose function type ends in `error`, whether the callee was declared with `!` or not, and whether it is called directly, through a func value or through an interface method, because `func!() T` and `func() (T, error)` are the same type (ERR-2) and the caller can't tell how a failure was written.
- The operand count decides: as many operands as declared results means "value, nil error"; one more means "failure". This is never ambiguous, because a `!` function cannot declare `error` as its last result (ERR-15).

**ERR-4.** *No named error result.* A `!` function cannot name its error result. When a deferred call must see the error, use the Go form `(T, err error)`.

**ERR-15.** *No `error` as the last result of a `!` function.* A `!` function already adds the `error` result, so it cannot also declare `error` as its last result. `func check!() error` is a compile error: "`!` already adds the error; remove it". `func check!()` returns only an error. This applies to every `!` form (functions, methods, interface methods, literals and function types). To return an error as a normal value, use the Go form `func check() (error, error)`: the first `error` is the value, the second is the failure. This removes the `return e` ambiguity. For a generic `!` function instantiated with `T = error`, `return e` is success with the value `e`; failure is always `return zero, err` (a result with no zero value is written `nil` and left unset, ERR-3), so there is no ambiguity.

## 9.2 Unwrapping calls (D1.8)

**ERR-5.** `call!(args)` **unwraps** a call whose last result has type `error`. The value of the expression is the other results without the error (zero, one or several). It works for functions, methods and generic functions (`f[T]!(x)`), and it chains: `os.Open!(p).Close!()`.

**ERR-6.** *What happens on error* depends on the function that **contains** the call:
- **Inside a function that can fail**, the error is returned to the caller, with zero values for the other results (results with no zero value are left unset, ERR-3). The `!` site is added to the error's trace ([ERR-14](#93-errors-work-like-go-d119)). The trace never wraps the error.
- **Anywhere else** (`main`, a goroutine body, any function without an error result), it **panics** with an `errors.MustError` value ([ERR-9](#92-unwrapping-calls-d18)).

**ERR-7.** *Exact failure semantics.* On failure `call!()` does exactly what `return <zero values>, err` does (results with no zero value are left unset, ERR-3): named results are set, then deferred calls run.

**ERR-8.** *Cost.* One compare and a branch to a cold block: the same code as a hand-written `if err != nil { return …, err }`. The trace entry is written only on the error path.

**ERR-9.** *The panic value.* `errors.MustError` has the fields `Err`, `Expr`, `Pos` and the method `Unwrap()`. The message reads:

```
panic: must: os.Open(path) at main.gopp:12:9: open /nope: no such file or directory
```

followed by the error's trace. `MustError` is not predeclared ([LEX-9](02-lexical.md)).

**ERR-10.** *Libraries do not panic on ordinary errors.* A library function that can fail is declared with `!`, so the `!` calls inside it return errors and the caller decides. `gopp vet` flags a `!` that would panic outside package `main`.

**ERR-11.** *`(n, err)` calls.* `gopp vet` warns when `!` is used on `(n, err)` calls such as `Read` and `Write`, where `n` matters even when there is an error.

**ERR-11a.** *`defer` and `go`.*
- `go f!()` panics in the new goroutine, which has no caller to return to.
- `defer f!()` inside a function that can fail keeps the other results and sets the error only if it is still nil. While the function is panicking, the deferred error is ignored.
- Anywhere else, `defer f!()` panics on error.

**ERR-11b.** *Only for calls.* Map lookups and channel receives keep Go's comma-ok form: `v, ok := m[k]`, `v, ok := <-ch`. There is no `!` for them. For a value type with no zero value, `v` exists only where `ok` is true ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).

### Examples

```go
func load!(path string) []byte {    // can fail: callers get ([]byte, error)
    f := os.Open!(path)             // on error, load returns it
    return io.ReadAll!(f)
}

func count!(path string) int {
    data := load!(path)             // on error, count returns it too; the trace now has both sites
    return len(data)
}

func main() {
    a := load!("a.txt")             // main cannot return an error, so `!` panics here
    fmt.Println(len(a))

    b, err := load("b.txt")         // or handle the error like Go
    if err != nil {
        log.Fatal(err)
    }
    fmt.Println(len(b))
}
```

More forms:

```go
func (c *Conn) Send!(msg string) int { … }          // method
type Loader interface{ Load!(path string) []byte }   // interface method
f := func!(n int) string { return strconv.Itoa(n) } // literal: type func(int) (string, error)
g := parse[int]!("42")                               // generic call
os.Open!(p).Close!()                                 // chaining

func run!() {
    defer cleanup!()         // sets run's error only if it is still nil
    step!()
    return                   // nil error
}

func opens() {
    go worker!()             // panics inside the goroutine on error
}
```

Compared with Go:

```go
// Go                                   // Go++
func load(p string) ([]byte, error) {   func load!(p string) []byte {
    f, err := os.Open(p)                    f := os.Open!(p)
    if err != nil { return nil, err }       return io.ReadAll!(f)
    return io.ReadAll(f)                }
}
```

## 9.3 Errors work like Go (D1.19)

**ERR-12.** *Errors are shared values.* `error` is an interface, so it is a shared handle ([OWN-C3](04-ownership.md#41-type-categories-d13), [TYP-5](03-types.md)). `return ErrNotFound` shares the same error. `err == io.EOF`, `errors.Is`, `%w` and type switches work as in Go.

**ERR-13.** *Static errors.* Package-level errors from `errors.New` are static: never freed, never counted. Other error values are reference-counted.

**ERR-14.** *The `!` trace* is stored per goroutine, keyed by the error's identity, **outside** the error. It never changes `==`, type switches or `Error()`. It is printed by a `!` panic and by `%+v`. After an error crosses goroutines, its trace is best effort.

```go
var ErrNotFound = errors.New("not found")          // static

func find!(k string) int {
    if k == "" { return 0, ErrNotFound }           // explicit failure (ERR-3): same error value every time
    return 1
}

_, err := find("")
fmt.Println(err == ErrNotFound)                    // true, as in Go
```

## 9.4 panic, defer, recover (D1.9)

Base: [Handling panics](https://go.dev/ref/spec#Handling_panics), [Defer statements](https://go.dev/ref/spec#Defer_statements).

**PAN-1.** *Same API as Go.* `panic(v)` unwinds with the platform unwinder (P04 D4.6), running destructors and deferred calls. `recover()` inside a deferred call stops the panic. `recover()` returns an `any` (which keeps Go's `nil`), so `if r := recover(); r != nil { … }` works as in Go, and `panic(nil)` panics with a `*runtime.PanicNilError`, as in Go 1.21 and later.

**PAN-2.** *Nested panics follow Go exactly.* A panic raised in a deferred call replaces the current panic. A later deferred `recover()` can recover it. An unrecovered chain prints every panic and exits with code 2.

**PAN-3.** *`defer` is function-scoped* (as in Go). The function and the arguments are evaluated at the `defer` statement (the receiver counts as an argument). Each kind of argument:
- **trivial** values are copied;
- **shared handles** (channels, interfaces, funcs, `Shared[*T]`) are copied, which retains them, as for `go` ([CON-1](12-concurrency.md));
- **owned temporaries** (call results, conversions, composite literals) are moved into the deferred call, as `go` moves owned arguments (D1.13): `defer os.RemoveAll(filepath.Join(dir, "x"))` is fine;
- **owned places** (an owned local, field or element) must be passed as `move(x)` or `x.Clone()`, or be **borrowed**, only if the variable lives until the function returns and is not **written or moved** after the `defer` (assignment, field or element write, `append`, a pointer-receiver call on it or on anything it owns, or a move; [OWN-R3c](04-ownership.md#42-ownership-rules-d14), [CLO-7](11-closures.md)). A **borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) is never passed with `move` (use the `&` range form, or `Clone`). It may be *borrowed* by a `defer` if it lives until the function returns: parameters and function-level borrowed variables, so `func handle(name string) { defer fmt.Println("done", name) }` is legal. It is a compile error for a borrowed variable that dies earlier (a borrowed loop range variable, an assertion binding in a nested block): `for k, v := range m { defer log(v) }` accepts only `v.Clone()`, because `v` is borrowed from the map and dies at the end of the iteration. An owned local of a loop body is different: see the next bullet;
- **owned locals declared in a nested block or loop body** (they would die before the function returns) may be used by a pending deferred call: as the receiver of `defer x.M()`, as a borrowed argument, or as a capture of a deferred function literal. The compiler uses **lifetime extension**, in three parts. (1) *Storage:* the compiler sees statically which block-scoped locals a `defer` in their scope may use, and gives each one its own storage per execution of the block (like Go's per-iteration loop variables), so `&x` taken before or after the `defer` stays valid. (2) *The `defer` ran:* its defer record keeps the local alive, so the local is NOT destroyed at the end of its block. It is destroyed at function exit, after all deferred calls, in the order of [PAN-4](#94-panic-defer-recover-d19). (3) *The `defer` did not run on this path* (for example it sits in an `if` that was not taken): the local is destroyed at the end of its block as usual. The defer record is the only flag; there are no extra drop flags ([OWN-R5](04-ownership.md#42-ownership-rules-d14)). Nothing is moved or zeroed at the `defer` statement: later reads, writes, pointer calls and `&x` refer to that same variable, and the deferred call sees its latest value, as in Go (`f := os.Open!(n); defer f.Close(); io.Copy(dst, f)` reads a valid file). A borrowed argument still follows the "not written or moved after the `defer`" rule above, because it is evaluated at the `defer` statement. The deferred calls therefore run at function exit exactly as in Go, also in a loop. Borrowed variables that die earlier keep their errors (see the bullets above). A `@Pinned` local can be extended too, because nothing is moved. `gopp vet` hints that with `defer f.Close()` in a loop body the files stay open until the function returns;
- an **`&` range variable** ([RNG-1a](08-slices-and-range.md)) is an alias of an element, so it counts as a **place** (`xs[i]`), not as a loop-body local. The collection owns the element and the `defer` never destroys it. `defer f.Close()` evaluates `&xs[i]`, which is fine (except for map aliases, see [RNG-1a](08-slices-and-range.md): later inserts can rehash a map, so the map-alias ban covers every deferred use (method calls, arguments and captures): `for k, &v := range m { defer v.Flush() }` and `{ defer show(v) }` are errors) if the collection lives until the function returns and is not appended to or reassigned before then (visible: compile error, [OWN-B2](04-ownership.md#45-borrow-checking-adr-0021); through an alias the compiler cannot see: UB, [UB-2](16-undefined-behavior.md)), and a compile error if the collection is itself a loop-body local. `defer show(f)` follows the borrowed-argument rule for places (the collection lives until return and `f` is not written after). A deferred literal that captures `f` follows [CLO-3b](11-closures.md);
- a **pointer-receiver deferred call** `defer x.M()` evaluates `&x` at the `defer` statement, as in Go. Later writes and pointer calls on `x` are allowed and are seen by the deferred call (`defer w.Flush(); w.WriteString(…)` works). It only needs `x` to live until the function returns (next bullet). **Moving** `x` while such a deferred call is pending is a compile error ([OWN-R3c](04-ownership.md#42-ownership-rules-d14)): the deferred call would read the zero value. Fix-it: `x.Clone()`. The "written or moved" rule above is for *borrowed owning arguments*, not for `&x` receivers;
- **A view or address written directly in the `defer` statement** (`&x`, `&x.f`, `buf[:2]`) is a use of that local, so it is extended like any other use. `defer unlock(&mu)` and `defer write(buf[:2])` inside a loop body are legal, as in Go: each iteration's `mu` lives until function exit. A pointer or view taken **earlier** and stored in another variable does NOT extend its target: `p := &mu; defer unlock(p)` in a loop body does not keep `mu` alive, and if `mu` dies first (at the end of its block) the later use is a compile error when the borrow checker can follow the pointer ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021), [OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)), and UB only through an alias it cannot see ([UB-2](16-undefined-behavior.md)). The deferred call does not extend `mu`. A pointer into a function-level local is fine in both forms;
- anything else is a compile error.

```go
func read(paths []string) {
    for _, p := range paths {
        f := os.Open!(p)
        defer f.Close() // ok, as in Go: f lives until read returns, and is closed then
    }
}
```

If you want each file closed at the end of its iteration, do not defer. Let the destructor run:

```go
func read(paths []string) {
    for _, p := range paths {
        f := os.Open!(p)          // closed by f's destructor at the end of each iteration
        process(f)
    }
}

func one(p string, done chan int) {
    f := os.Open!(p)
    defer f.Close()               // ok: pointer receiver, so &f is evaluated now; f lives until one returns
    defer close(done)             // ok: a channel is a shared handle: copied (retained)
    defer os.Remove(p + ".tmp")   // ok: the temporary string moves into the deferred call
    var mu sync.Mutex
    mu.Lock()
    defer mu.Unlock()             // ok: &mu is a pointer to a function-level local
    defer log.Println(p)          // ok: p is a borrowed parameter that lives until one returns
}
```

Compared with Go: the first loop example behaves exactly as in Go. All files stay open until the function returns (a known Go trap). Go++ keeps that behavior on purpose, and the second form is the way to close each file early.

**PAN-4.** *Function exit order.* Deferred calls run LIFO. Then function-scope locals are destroyed in reverse declaration order. Block-scoped locals are destroyed at the end of their block, except locals extended by an executed `defer` ([PAN-3](09-errors-and-panics.md)). **Exit order:** (1) deferred calls run LIFO; (2) then the extended locals and the function-scope locals are destroyed **together, in reverse order of creation** (the latest iteration's variable first). A local used by several deferred calls (`defer f.A(); defer f.B()`) therefore lives until all of them have run. `main` is the one exception: its locals, extended ones included, are dropped after the other goroutines stop ([INI-2](15-program-init.md), D1.18).

**PAN-5.** *Panics in `drop`.* A panic that escapes a `drop` method **aborts** the program with a clear message. Drop glue never unwinds.

**PAN-6.** *`move` arguments.* The callee owns its `move` arguments and drops them on every path, including panics.

**PAN-7.** *`-panic=abort`* (the `panic abort` build profile setting, P06 D6.12) removes unwinding: panics print and abort. Binaries are smaller and compiles are faster.
- `recover()` always returns nil in this mode, and `gopp vet` warns about it.
- A C++ exception that reaches a Go++ frame aborts too.
- Unwind tables are kept, so stack traces still work.

**PAN-8.** *C++ exceptions* entering Go++ frames unwind like panics, and `recover()` returns them as `ffi.Exception` (P08).

**PAN-9.** *Forced unwinding and `longjmp`.* `recover()` and `ffi.Try` never stop forced unwinding (`pthread_exit`, thread cancellation). A C `longjmp` across Go++ frames is **undefined behavior**: it skips destructors and defers ([UB-4](16-undefined-behavior.md)).

**PAN-10.** *Unrecovered panic.* The trampoline prints the panic value and a stack trace, stops the other goroutines and exits with code 2 ([INI-4](15-program-init.md)).

```go
func safe() (err error) {
    defer func() {
        if r := recover(); r != nil {
            err = fmt.Errorf("recovered: %v", r)    // named result set from a deferred closure
        }
    }()
    var xs []int
    _ = xs[3]                                       // index out of range: panic
    return nil
}
```

## 9.5 Rationale

- **`!` returns errors inside failable functions.** A `!` call is one mark that means "propagate, or crash if there is nobody to propagate to". It is the same code as a hand-written `if err != nil`.
- **Libraries return errors.** This is why `!` does not always panic.
- **Errors keep Go semantics** by being shared handles, so existing error idioms keep working.
- **Traces outside the error** keep `==` and type switches exact.
- **Function-scoped `defer`** is Go's rule, also in a loop. The loop variable's lifetime is extended until the deferred call has run, so code that is valid Go keeps its meaning.
- **Rejected:** postfix `call()!` (needed a special semicolon rule and a `!=` lexing caveat); a `!` that always panics (pushed libraries toward crashing); a `try` keyword or a `#Try(...)` macro (`!` already returns errors in failable functions).
