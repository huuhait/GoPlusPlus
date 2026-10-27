# 9. Errors, `!`, panic, defer and recover

Base: Go spec [Errors](https://go.dev/ref/spec#Errors), [Function declarations](https://go.dev/ref/spec#Function_declarations), [Function literals](https://go.dev/ref/spec#Function_literals), [Function types](https://go.dev/ref/spec#Function_types), [Return statements](https://go.dev/ref/spec#Return_statements), [Defer statements](https://go.dev/ref/spec#Defer_statements), [Go statements](https://go.dev/ref/spec#Go_statements), [Handling panics](https://go.dev/ref/spec#Handling_panics), [Run-time panics](https://go.dev/ref/spec#Run_time_panics), [Comparison operators](https://go.dev/ref/spec#Comparison_operators).

Deltas covered: D1.8 (`!`), D1.9 (panic, defer, recover), D1.19 (errors work like Go).

Contents: [9.1 `!` functions](#91-functions-that-can-fail-d18) · [9.2 `call!()`](#92-unwrapping-calls-d18) · [9.3 errors as values](#93-errors-work-like-go-d119) · [9.4 panic, defer, recover](#94-panic-defer-recover-d19) · [9.5 rationale](#95-rationale)

## 9.1 Functions that can fail (D1.8)

**Go error handling is unchanged.** `val, err := call()` followed by `if err != nil { … }` works exactly as in Go.

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

A function **can fail** if it is declared with `!`, or if its last result has type `error`. This property belongs to the innermost function: a function literal is judged by its own signature, not by the function around it.

> **Open (P01 review):** D1.8 says "the function the call is in" without naming the literal case. This spec takes the innermost function (so `x!()` in a plain closure inside a `!` function panics, and does not return from the outer function).

**ERR-3.** *Returning from a `!` function.*
- `return v` returns `v` with a nil error.
- To fail explicitly, add the error as an extra last operand: `return nil, err`, or `return err` when there are no other results.
- `return g()` is allowed when `g` returns the declared results plus `error`.

> **Open (P01 review):** when the only declared result is itself of type `error` (`func f!() error`), `return e` is ambiguous (value, or failure?). This spec reads: operand count equal to the declared results means "value, nil error"; one more means "failure".

**ERR-4.** *No named error result.* A `!` function cannot name its error result. When a deferred call must see the error, use the Go form `(T, err error)`.

## 9.2 Unwrapping calls (D1.8)

**ERR-5.** `call!(args)` **unwraps** a call whose last result has type `error`. The value of the expression is the other results without the error (zero, one or several). It works for functions, methods and generic functions (`f[T]!(x)`), and it chains: `os.Open!(p).Close!()`.

**ERR-6.** *What happens on error* depends on the function that **contains** the call:
- **Inside a function that can fail**, the error is returned to the caller, with zero values for the other results. The `!` site is added to the error's trace ([ERR-14](#93-errors-work-like-go-d119)). The trace never wraps the error.
- **Anywhere else** (`main`, a goroutine body, any function without an error result), it **panics** with an `errors.MustError` value ([ERR-9](#92-unwrapping-calls-d18)).

**ERR-7.** *Exact failure semantics.* On failure `call!()` does exactly what `return <zero values>, err` does: named results are set, then deferred calls run.

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

**ERR-11b.** *Only for calls.* Map lookups and channel receives keep Go's comma-ok form: `v, ok := m[k]`, `v, ok := <-ch`. There is no `!` for them.

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

**PAN-1.** *Same API as Go.* `panic(v)` unwinds with the platform unwinder (P04 D4.6), running destructors and deferred calls. `recover()` inside a deferred call stops the panic.

**PAN-2.** *Nested panics follow Go exactly.* A panic raised in a deferred call replaces the current panic. A later deferred `recover()` can recover it. An unrecovered chain prints every panic and exits with code 2.

**PAN-3.** *`defer` is function-scoped* (as in Go). The function and the arguments are evaluated at the `defer` statement (the receiver counts as an argument). Each kind of argument:
- **trivial** values are copied;
- **shared handles** (channels, interfaces, funcs, `Shared[T]`) are copied, which retains them, as for `go` ([CON-1](12-concurrency.md));
- **owned temporaries** (call results, conversions, composite literals) are moved into the deferred call, as `go` moves owned arguments (D1.13): `defer os.RemoveAll(filepath.Join(dir, "x"))` is fine;
- **owned places** (an owned local, field or element) must be passed as `move(x)` or `clone(x)`, or be **borrowed**, only if the variable lives until the function returns and is not **written or moved** after the `defer` (assignment, field or element write, `append`, a pointer-receiver call on it or on anything it owns, or a move; [OWN-R3c](04-ownership.md#42-ownership-rules-d14), [CLO-7](11-closures.md)). A **borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) is never passed with `move` (only a slice range variable may move): `for k, v := range m { defer log(v) }` accepts only `clone(v)`, or a borrow only if the variable lives until the function returns, which a loop-body variable never does, since `v` dies at the end of the iteration;
- a **pointer-receiver deferred call** `defer x.M()` evaluates `&x` at the `defer` statement, as in Go. Later writes and pointer calls on `x` are allowed and are seen by the deferred call (`defer w.Flush(); w.WriteString(…)` works). It only needs `x` to live until the function returns (next bullet). **Moving** `x` while such a deferred call is pending is a compile error ([OWN-R3c](04-ownership.md#42-ownership-rules-d14)): the deferred call would read the zero value. Fix-it: `clone(x)`. The "written or moved" rule above is for *borrowed owning arguments*, not for `&x` receivers;
- a **raw pointer or a view** into a local that dies before the function returns (a variable of a nested block or loop body), or into the target of such a local `Unique`, is a **compile error** (D1.4 rule 8; the same rule as [CON-2](12-concurrency.md) for `go`). This includes a pointer receiver on such a local: `defer (&f).Close()`, `defer unlock(&mu)` inside a loop. The same pointer to a function-level local is fine;
- a deferred **function literal** that captures a dying variable is also an error ([CLO-3b](11-closures.md));
- anything else is a compile error.

> **Open (P01 review):** the plan states only the borrowed, `move`/`clone` and trivial cases. The shared-handle, owned-temporary and raw-pointer cases are read from CON-1, D1.13 and D1.4 rule 8.

```go
func read(paths []string) {
    for _, p := range paths {
        f := os.Open!(p)
        defer f.Close() // error: deferred call borrows f, which is destroyed at the end of the loop body
    }                   //        (f's destructor already closes it; the defer is not needed)
}
```

How to write it correctly:

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
    defer log.Println(clone(p))   // ok: owned value cloned at the defer
}
```

Compared with Go: in Go the loop example compiles and closes all files at function exit (a known trap). In Go++ it is rejected, and the destructor closes each file at the end of the iteration.

**PAN-4.** *Function exit order.* Deferred calls run LIFO. Then function-scope locals are destroyed in reverse declaration order. Block-scoped locals are destroyed at the end of their block. `main` is the one exception: its locals are dropped after the other goroutines stop ([INI-2](15-program-init.md)).

**PAN-5.** *Panics in `drop`.* A panic that escapes a `drop` method **aborts** the program with a clear message. Drop glue never unwinds.

**PAN-6.** *`own` arguments.* The callee owns its `own` arguments and drops them on every path, including panics.

**PAN-7.** *`-panic=abort`* removes unwinding: panics print and abort. Binaries are smaller and compiles are faster.
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
- **Function-scoped `defer`** is Go's rule. The loop-defer error turns a famous Go bug into a compile error.
- **Rejected:** postfix `call()!` (needed a special semicolon rule and a `!=` lexing caveat); a `!` that always panics (pushed libraries toward crashing); a `try` keyword or a `#Try(...)` macro (`!` already returns errors in failable functions).
