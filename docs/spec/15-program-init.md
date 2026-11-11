# 15. Program initialization and exit

Base: Go spec [Program initialization and execution](https://go.dev/ref/spec#Program_initialization_and_execution), [Package initialization](https://go.dev/ref/spec#Package_initialization), [Program initialization](https://go.dev/ref/spec#Program_initialization), [Program execution](https://go.dev/ref/spec#Program_execution), [The zero value](https://go.dev/ref/spec#The_zero_value).

Delta D1.18. Syntax: no change.

## 15.1 Order at startup

**INI-1.** *Startup order.*
1. C and C++ static initializers from imported code run first, as part of C runtime startup.
2. Then Go++ package variable initialization and `init` functions run in import-dependency order (as in [Package initialization](https://go.dev/ref/spec#Package_initialization)).
3. Then `main.main`.

`init` functions cannot be called, as in Go. `drop` methods cannot be called either ([OWN-R7](04-ownership.md#42-ownership-rules-d14)).

## 15.2 Exit

**INI-2.** *Normal exit.* When `main.main` returns:
1. `main`'s deferred calls run (LIFO, [PAN-4](09-errors-and-panics.md));
2. `main`'s locals (including locals kept alive by a `defer`, [PAN-3](09-errors-and-panics.md)) are dropped in reverse order of creation;
3. the runtime stops all remaining background goroutines (P12 D12.12);
4. the runtime calls C `exit()`, which runs atexit handlers, flushes stdio and runs C++ static destructors.

A stopped goroutine parks at its next yield point and never resumes; its stack is not unwound and its locals are not dropped.

**INI-3.** *`os.Exit(code)`* skips defers and drops, stops the other goroutines, and calls `exit(code)`.

**INI-4.** *Unrecovered panic.* If a panic unwinds `main.main`, `main`'s deferred calls and local drops run while unwinding `main`, then the trampoline prints the panic, stops the other goroutines, and calls `exit(2)`. An unrecovered panic in another goroutine prints the panic, stops all goroutines without unwinding them, and calls `exit(2)` ([CON-11](12-concurrency.md)).

**INI-5.** *Package-level variables are never destroyed* (as in Go). That avoids C++'s static-destruction-order problems. Buffered writers must be flushed explicitly, as in Go.

**INI-6.** *Order of destruction in `main`.* Deferred calls first (LIFO), then `main`'s locals in reverse creation order, then background goroutines stop, then `exit()`.

## 15.3 Example

```go
var logger = newLogger()            // package variable: initialized before main, never dropped

func main() {
    buf := NewBuffer()              // owned; has a drop method
    defer fmt.Println("deferred")   // runs 1st
    go background()                 // still running when main returns; reads package variable logger
    fmt.Println("done")
}                                   // 1. "deferred"  2. buf dropped  3. goroutines stop  4. exit()
```

Compared with Go: Go runs no cleanup for `main`'s locals and just exits. Go++ drops `main`'s locals before stopping background goroutines so any `Mutex` guard or channel owned by `main` is released cleanly without deadlocking background goroutines, and background goroutines can never borrow `main`'s locals ([CON-2](12-concurrency.md)).

## 15.4 Rationale

- Dropping `main`'s locals (after `main`'s defers) before stopping background goroutines ensures `Mutex` guards and channels held in `main` unlock/close cleanly instead of deadlocking shutdown, while [CON-2](12-concurrency.md) guarantees no background goroutine borrows `main`'s stack locals.
- Calling C `exit()` last gives C and C++ code their normal shutdown (atexit, stdio flush, static destructors).
- Never destroying package variables avoids the "static destruction order fiasco" and matches Go.
