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

**INI-2.** *Normal exit.* After `main.main` returns and its deferred calls have run, the runtime:
1. stops all other goroutines (P12 D12.12);
2. drops `main`'s locals, including locals kept alive by a `defer` ([PAN-3](09-errors-and-panics.md));
3. calls C `exit()`, which runs atexit handlers, flushes stdio and runs C++ static destructors.

This is safe because no Go++ code is running any more. A stopped goroutine parks at its next yield point and never resumes; its stack is not unwound and its locals are not dropped.

**INI-3.** *`os.Exit(code)`* skips defers and drops, stops the other goroutines, and calls `exit(code)`.

**INI-4.** *Unrecovered panic.* The trampoline prints the panic, stops the other goroutines, and calls `exit(2)`. If the panic leaves `main.main`, the other goroutines are stopped before `main`'s locals are dropped, as at a normal exit.

**INI-5.** *Package-level variables are never destroyed* (as in Go). That avoids C++'s static-destruction-order problems. Buffered writers must be flushed explicitly, as in Go.

**INI-6.** *Order of destruction in `main`.* Deferred calls first (LIFO), then goroutines stop, then `main`'s locals are dropped in reverse order, then `exit()`.

## 15.3 Example

```go
var logger = newLogger()            // package variable: initialized before main, never dropped

func main() {
    buf := NewBuffer()              // owned; has a drop method
    defer fmt.Println("deferred")   // runs 1st
    go background()                 // still running when main returns; it reads the package variable logger itself
    fmt.Println("done")
}                                   // 1. "deferred"  2. goroutines stop  3. buf dropped  4. exit()
```

Compared with Go: Go runs no cleanup for `main`'s locals and just exits. Go++ stops other goroutines first so that dropping `buf` cannot race with them.

## 15.4 Rationale

- Stopping goroutines before drops means a destructor in `main` never races with another goroutine.
- Calling C `exit()` last gives C and C++ code their normal shutdown (atexit, stdio flush, static destructors).
- Never destroying package variables avoids the "static destruction order fiasco" and matches Go.
