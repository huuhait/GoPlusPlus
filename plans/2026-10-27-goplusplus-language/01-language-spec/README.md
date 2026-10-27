# P01 — Language specification v0

**Goal:** a written specification (`docs/spec/`) precise enough to implement P02–P05. It is defined as *the Go specification plus a numbered list of deltas*, and validated by ten sample programs written before any implementation.
**Depends on:** P00 · **Unblocks:** P02–P05, P07–P09, P12 · **Size:** L · **Milestone:** M0 (grammar), M2 (memory model)

This is the most important phase: it fixes how Go++ code feels. The memory model (D1.3–D1.7) answers the hardest question in the project: how to keep Go's look without a GC.

## Decisions

**D1.1 — The base is the Go spec; Go++ is written as deltas.**
- The base is the **Go 1.25** specification (installed on the dev machine).
- The P03 types2 port and the P04 differential tests use the same Go version.
- Moving to a newer Go needs an ADR.

Everything not mentioned stays as in Go:
- packages, imports, visibility by capitalization;
- declarations, `:=`, constants and `iota`;
- control flow, `switch`/`select`, labels/`goto`;
- methods, embedding, structural interfaces;
- generics syntax, `init`, build constraints, testing conventions.

Each delta has EBNF, semantics, an example and a rationale.
*Why:* maximum transfer of Go knowledge, and a small spec to maintain.

**D1.2 — Lexical deltas.**
- Source files are `.gopp`, UTF-8.
- New keywords: `own`, `view`, `macro`.
- New predeclared identifiers:
  - the functions `move`, `clone`, `new_unique`, `new_shared`, `new_weak`;
  - the generic types `Unique[T]`, `Shared[T]`, `Weak[T]`;
  - the constraints `trivial` and `clonable` (D1.10);
  - the std macros, such as `#Assert` and `@Derive` (D1.14).
- Go's `new` is removed.
- `MustError` is not predeclared: it lives in package `errors` (`errors.MustError`, D1.8).
- New tokens: `#` (macro invocation) and `@` (attribute or attached macro). The existing `!` token also marks functions that can fail (`func load!(…)`) and unwrapping calls (`call!()`), D1.8.
- Semicolon insertion: unchanged from Go. A `call!()` always ends in `)`, so no new rule is needed.

**D1.3 — Type categories.**

| Category | Types | Copy | End of life |
|---|---|---|---|
| Trivial | bool, numbers, raw `*T`, `unsafe.Pointer`, `view string`, `view []T`, arrays/structs of trivial types | implicit, bitwise | nothing |
| Owning | `string`, `[]T`, `map[K]V`, `Unique[T]`, types with a `drop` method, arrays/structs containing an owning type, C++ classes with a non-trivial copy/move/destructor | move-only; `clone(x)` copies | drop glue runs |
| Shared handle | `Shared[T]`, `Weak[T]`, `chan T`, interface values (including `error`), func values, arrays/structs whose non-trivial fields are all shared handles | implicit; copying retains (atomic) | release; the last strong reference frees |

**D1.4 — Ownership rules.**
1. **One owner.** Every value has exactly one owner: a variable, field, element or temporary. When the owner's lifetime ends, the value is destroyed: its `drop` method runs (if any), then its owning fields are destroyed in reverse declaration order.
2. **Destruction order.** Locals are destroyed at the end of their block, in reverse declaration order. Temporaries are destroyed at the end of the full statement; for temporaries in a `for … range`, `if` or `switch` header, see D1.6.
3. **By-value use moves.** A by-value use of an owned local moves it: `b := a`, `x.f = a`, passing `a` to an `own` parameter, `return a`, `ch <- a`, `T{F: a}`. Any later use of `a` is a compile error until `a` is assigned again.

   **3a. Reading from other places** (decides Q2).
   - Reading an owned value from any other place (a field, element, map entry, package variable, pointer target or borrowed parameter) borrows it where a borrow is enough: operands, borrowed arguments (including interface parameters), receivers, the `range` source.
   - Where ownership is needed (`:=`, `=`, an `own` argument, `return`, a channel send, a composite literal), it is a compile error with two fix-its: `clone(p)` (copy it) and `move(p)` (take it, leaving the zero value). For a borrowed parameter, the fix-its are `clone(p)` and making the parameter `own`, because a borrowed parameter can't be moved out of (D1.5).
   - Maps:
     - `move(m[k])` removes the entry and returns it.
     - `v, ok := clone(m[k])` and `v, ok := move(m[k])` keep the comma-ok form.
     - `m[k].f = x` stays illegal (as in Go).
   - A swap that reads and writes the same places in one statement (`a[i], a[j] = a[j], a[i]`) is allowed and moves.
   - Built-in `min`/`max` on owned strings return a copy.
4. **Explicit and implicit moves.**
   - `move(place)` takes a value out of any place and leaves the zero value.
   - The move is implicit when the same place is passed to an `own` parameter and also receives the result, in one statement: `p = append(p, …)`, `buf = strconv.AppendInt(buf, n, 10)`. So `x.items = append(x.items, v)` works exactly as in Go.
   - Anywhere else, write `append(move(p), …)` or `append(clone(p), …)`.
   - Spread (`append(a, b...)`) and `copy(dst, src)` with non-`trivial` elements need `move` or `clone` on the source: `move(b)` or `clone(b)`, `move(src)` or `clone(src)`.
   - Moves out of places happen after all other arguments are evaluated.
5. **Zero values.** Every type has a zero value (Go), and a moved-from place holds it (for C++ class types, see rule 9). A `drop` method must therefore accept the zero value; `gopp vet` checks this. Drop glue skips a type's `drop` when the value is all zero bytes, so dropping a zero value behaves the same in every build mode.
6. **Copies are explicit: `clone(x)`.**
   - Clone is structural when all fields are clonable.
   - Shared handles (D1.3) copy by retaining.
   - A type with a `drop` method is clonable only if it defines `Clone() T`, which prevents silently duplicating unique resources such as file descriptors. There is no `Clone` derive: a type with `drop` writes `Clone()` by hand on purpose.
   - A type that mixes owning fields with raw `*T` fields needs a hand-written `Clone()`, because an automatic clone would copy the raw pointers (for example, tree nodes with parent pointers, like `Node` in D1.7).
   - Imported C++ types clone through their copy constructor.
7. **Destructors are a method named `drop`** with a pointer receiver: `func (f *File) drop()`. It can't be called directly (like `init`).
8. **No borrow checker.** Views and raw pointers can dangle, as in C++.
   - The compiler does catch: returning `&local`; storing a borrow of a temporary; a `defer` or `go` that borrows something that dies first (D1.9, D1.13); use after move.
   - The cases it doesn't catch are listed in the UB chapter (P01.14). They include views or raw pointers kept past their owner, and a borrowed parameter whose owner the callee frees through another path (`u.SetName(u.Name)`).
   - `-asan` catches the rest at runtime. The owner chose no extra safety layer for v1.
9. **C++ class types.**
   - A C++ type has a zero value only if it has a default constructor.
   - Where Go needs a zero value and there is none, it is a compile error: `make([]T, n)`, a missing map key, a receive from a closed channel, `var zero T` in generic code, named results, a failing `!`.
   - Every move of a C++ value, implicit or `move(x)`, runs its move constructor and leaves the C++ "moved-from" state, not zero. The source is destroyed at the end of its scope.
   - Only containers relocate values, with the type's relocate operation (P05 D5.5): `memcpy` only for Go++ types and trivially relocatable C++ types.
10. **Pinned values.**
    - `gopp vet` reports moving a local after its address, or a view into it, has been stored.
    - Types marked `@Pinned` (the `sync` types) can't be moved or copied after first use: a compile error, like Go vet's copylocks.
    - `return local` builds the value in the caller's result slot, so it never moves.

*Why:* deterministic, GC-free memory with C++-compatible object semantics, which C++ interop (P08) needs anyway, while keeping Go's zero values. Zero-on-move means no drop flags and no partially dead variables; moved-from state is always defined.
*Rejected:* tracing GC (excluded by the requirements); ARC everywhere (hidden cost, cycles); a Rust-style borrow checker (cost to compile speed and simplicity; reconsider after v1); C++-style implicit copies (hidden allocations).

**D1.5 — Parameters: owning types borrow, trivial types copy.**
- **Borrowing.** Parameters and receivers of **owning** types *borrow* their argument. They are read-only at the top level: you can't assign them (or their fields), move out of them, or append to them. Data reached through slices, maps and pointers stays mutable, as in Go: elements of `[]T`, map entries, pointer targets.
- **Re-pointing views.** One exception: a borrowed `string`/`[]T` parameter is a local view, so it can be re-pointed (`p = p[n:]`). It still can't be appended to.
- **No `&param`.** `&param` on a borrowed owning parameter is a compile error (fix-it: take a `*T` parameter).
- **Trivial parameters are copies.** Trivial parameters (`int`, raw pointers, trivial structs) are ordinary copies, as in Go; the callee may change them. The compiler may skip the copy only when nothing can write through an alias.
- **`own` takes ownership.** `own` parameters and receivers take ownership: `func (l *List[T]) Push(v own T)`, `func (b own Builder) Finish() string`. The caller's argument is moved, or the caller writes `clone(x)`. Inside the callee, an `own` parameter is an ordinary owned local (mutable).
- **`*T` mutates.** `*T` parameters and receivers mutate, as in Go.
- **Shared handles work like Go.** Parameters of shared-handle types (`Shared[T]`, `Weak[T]`, `chan`, interfaces including `error`, funcs) can be copied, stored and returned by the callee, and each copy retains (`return err` works as in Go). The call itself passes them without a retain.
- **ABI.**
  - Trivial parameters follow the C ABI (copies).
  - Borrowed owning parameters pass by hidden pointer.
  - `string` and `[]T` pass as `(ptr, len)`, so callers can pass owned values, views and literals at zero cost.
  - Go++ → Go++ calls may use the internal convention (P04 D4.4).

*Why:* generic code such as `func Index[T comparable](xs []T, v T) int` works for owning `T` with no moves or clones, and call sites look exactly like Go. This is the C++ `const T&` / Swift / Mojo convention. Trivial parameters stay real copies, so Go code such as `for b != 0 { a, b = b, a%b }` works unchanged.
*Rejected:* move-by-default parameters (Rust without `&`, hostile to Go-style generic APIs); copy-by-default owning parameters (hidden allocations); read-only trivial parameters (they reject ordinary Go code, and passing large ones by pointer gives wrong results when an argument aliases memory the callee writes).

**D1.6 — Views.**
- `view string` (immutable bytes) and `view []T` (mutable elements, fixed length) are `(ptr, len)` borrows.
- Slicing produces a view without allocating: `s[i:j]` gives a `view string`, `xs[i:j]` a `view []T`, and `:=` infers that. The exception is re-slicing an owned slice into itself (`s = s[i:j]`), which shrinks it in place (D1.20).
- Owned → view conversion is implicit wherever a view is expected. View → owned is explicit and copies: `string(v)`, `[]T(v)` (`[]T(v)` needs `clonable` elements). Unlike Go, where slice conversions never copy, the copy is how you get ownership.
- Maps accept a `view` key for lookups, so `counts[w]` with `w view string` doesn't allocate. Inserting with a view key clones the key.
- Temporaries live until the end of the full statement. In the header of a `for … range`, `if` or `switch`, they live until the end of the whole statement, body included.
- Views may be stored and returned at the programmer's risk, like C++ `string_view`/`span`. `gopp vet` warns about view-typed struct fields unless the struct is marked `@View` (for example, tokens pointing into a source buffer).

**D1.7 — Pointers and heap allocation.** Every heap allocation is spelled out; Go's `new` is removed.

| Type | Created with | Meaning |
|---|---|---|
| `*T` | `&x` (address of a variable) | raw, nullable, non-owning (Go syntax, no GC) |
| `Unique[T]` | `new_unique(v)` | single owner; the value is dropped and freed when the pointer is destroyed; nullable (zero value `nil`) |
| `Shared[T]` | `new_shared(v)` | shared ownership, atomically refcounted; one allocation holds the counts and the value (like `make_shared`) |
| `Weak[T]` | `new_weak(s)`, from a `Shared[T]` | doesn't own and doesn't allocate; `w.Lock()` returns a `Shared[T]`, or `nil` if the value is gone |

- `new_unique` and `new_shared` take the initial value and move it in: `new_unique(Node{Name: "a"})`, or `new_unique(Node{})` for a zero value.
- `new_weak` takes a `Shared[T]`. A weak pointer can't own a new object, because it would be freed at once.
- `&T{…}` is a compile error with a fix-it ("allocation is explicit in Go++: use `new_unique(T{…})`"). In Go it allocated silently.
- Field access and method calls auto-dereference every pointer kind.
- `Unique[T]` and `Shared[T]` have the methods of both `T` and `*T`.
- `Unique[T]` and `Shared[T]` convert implicitly to `*T` when borrowed from a place. They never convert from a temporary, because the target would be freed immediately.
- `make([]T, n)`, `make(map[K]V)` and `make(chan T, n)` work as in Go.

Ownership on one screen:

```go
type Node struct {
    Name     string          // owned
    Children []Unique[Node]  // owned slice of owning pointers
    Parent   *Node           // raw back-pointer: does not own
}

func NewNode(name string, parent *Node) Unique[Node] {
    return new_unique(Node{Name: clone(name), Parent: parent}) // name is borrowed: clone it to own it
}

func (n *Node) Add(child own Unique[Node]) {        // `own`: Add takes ownership
    child.Parent = n
    n.Children = append(n.Children, child)         // child moves into the slice
}

func main() {
    root := NewNode("root", nil)
    root.Add(NewNode("a", root))   // the temporary moves into Add
    b := NewNode("b", root)
    root.Add(b)                    // b moves
    // fmt.Println(b.Name)         // compile error: b used after move
    head := root.Name[:2]          // view string: a borrow, no allocation
    fmt.Println(head, len(root.Children))
}                                  // root is dropped → the whole tree is freed
```

**D1.8 — Errors and `!`.**
- **Go error handling is unchanged.** `val, err := call()` followed by `if err != nil { … }` works exactly as in Go.
- **`func name!(…)` declares a function that can fail.** `func load!(path string) []byte` is the same function as `func load(path string) ([]byte, error)`, and callers see `([]byte, error)`. Methods and interface methods use the same form. The `!` sits right before the parameter list, as in calls: `func parse[T any]!(s string) T` is called as `parse[int]!(s)`.
- **`call!(args)` unwraps a call whose last result is `error`.** The value is the other results, without the error (zero, one or several). What happens on error depends on the function the call is in:
  - **inside a function that can fail** (declared with `!`, or whose last result is `error`), the error is returned to the caller, with zero values for the other results. The `!` site is added to the error's trace. The trace never wraps the error (D1.19);
  - **anywhere else** (`main`, a goroutine body, any function without an error result), it panics with an `errors.MustError` value (fields `Err`, `Expr`, `Pos`; method `Unwrap()`). The message reads `panic: must: os.Open(path) at main.gopp:12:9: open /nope: no such file or directory`, followed by the error's trace.
- **Failure semantics.** On failure, `call!()` does exactly what `return <zero values>, err` does: named results are set, then deferred calls run. A `!` function can't name its error result; use the Go form `(T, err error)` when a deferred call must see the error.
- **Returning from a `!` function.** `return v` returns `v` with a nil error. To fail explicitly, add the error: `return nil, err`, or `return err` when there are no other results.
- **Libraries don't panic on ordinary errors.** A library function that can fail is declared with `!`, so the `!` calls inside it return errors and the caller decides. `gopp vet` flags a `!` that would panic outside package `main`.
- **`(n, err)` calls.** `gopp vet` warns when `!` is used on `(n, err)` calls such as `Read` and `Write`, where `n` matters even when there is an error.
- **Syntax.** Grammar: `PrimaryExpr = … | PrimaryExpr "!" Arguments`. It works for functions, methods and generic functions (`f[T]!(x)`), and it chains: `os.Open!(p).Close!()`. Function literals are written `func!(x int) T { … }`, and function types `func!(int) T` (the same as `func(int) (T, error)`).
- **Cost.** One compare and a branch to a cold block, the same code as a hand-written `if err != nil { return …, err }`. The trace entry is written only on the error path.
- **`defer` and `go`.**
  - `go f!()` panics in the new goroutine, which has no caller to return to.
  - `defer f!()` inside a function that can fail keeps the other results and sets the error only if it is still nil. While the function is panicking, the deferred error is ignored. Anywhere else, `defer f!()` panics on error.
- **Only for calls.** Map lookups and channel receives keep Go's comma-ok form: `v, ok := m[k]`, `v, ok := <-ch`.

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
    a := load!("a.txt")             // main can't return an error, so `!` panics here
    fmt.Println(len(a))

    b, err := load("b.txt")         // or handle the error like Go
    if err != nil {
        log.Fatal(err)
    }
    fmt.Println(len(b))
}
```

*Rejected:*
- Postfix `call()!` (an earlier draft): it needed a special semicolon-insertion rule and a `!=` lexing caveat (`f()!==y`). With `call!()`, the `!` sits next to the function name, where the reader looks first.
- `!` that always panics (an earlier draft): it pushed library code toward crashing.
- A `try` keyword or a `#Try(...)` macro: `!` already returns errors inside functions that can fail.

**D1.9 — panic, defer, recover.**
- **Same API as Go.** A panic unwinds with the platform unwinder (P04 D4.6), running destructors and deferred calls. `recover()` inside a deferred call stops it. An unrecovered panic prints the value and a stack trace and exits with code 2.
- **Panics in deferred calls** follow Go exactly. A panic raised in a deferred call replaces the current panic, and a later deferred `recover()` can recover it. An unrecovered chain prints every panic and exits with code 2.
- **`defer` stays function-scoped** (Go). Arguments are evaluated at the `defer` statement:
  - trivial values are copied;
  - owned values must be passed as `move(x)` or `clone(x)`, or borrowed only if the borrowed variable lives until the function returns and isn't assigned after the `defer` (D1.11). Anything else is a compile error:

  ```go
  for _, p := range paths {
      f := os.Open!(p)
      defer f.Close() // error: deferred call borrows f, which is destroyed at the end of the loop body
  }                   //        (f's destructor already closes it; the defer isn't needed)
  ```
- **Function exit order:** deferred calls run LIFO, then function-scope locals are destroyed in reverse order. Block-scoped locals are destroyed at the end of their block. `main` is the one exception: its locals are dropped after the other goroutines stop (D1.18).
- **Panics in `drop`.** A panic that escapes a `drop` method aborts the program with a clear message. Drop glue never unwinds.
- **`own` arguments.** The callee owns its `own` arguments and drops them on every path, including panics.
- **`-panic=abort`** removes unwinding: panics print and abort. Binaries are smaller and compiles faster. `recover` always returns nil in this mode, and `gopp vet` warns about it. A C++ exception that reaches a Go++ frame aborts too. Unwind tables are kept, so stack traces still work.
- **C++ exceptions** entering Go++ frames unwind like panics, and `recover()` returns them as `ffi.Exception` (P08).
- **Forced unwinding and `longjmp`.** `recover()` and `ffi.Try` never stop forced unwinding (`pthread_exit`, thread cancellation). A C `longjmp` across Go++ frames is undefined behavior: it skips destructors and defers.

**D1.10 — Generics.** Go's design: type parameters, constraint interfaces with type sets, inference. Additions:
- predeclared constraints `trivial` (bitwise-copyable types) and `clonable` (types `clone` accepts);
- a generic body may copy a `T` only if `T` is `trivial`; otherwise it borrows, moves (`own` parameters) or calls `clone` (which requires `clonable`);
- in a generic body, `return a` for a borrowed parameter of non-`trivial` `T` needs `clone(a)`, which requires `clonable` (D1.4 rule 3a);
- the implementation is full monomorphization (P04);
- **generic methods are allowed** (owner decision; decides Q5): a method may have its own type parameters, as in `func (s Set) Map[U any](f func(int) U) []U`. Each use is monomorphized like a generic function. Interfaces can't declare generic methods, so a generic method never satisfies an interface method.

**D1.11 — Closures** (decides Q1).
- **Non-escaping closures capture by reference.** A closure that is only called locally, or passed to a parameter that the callee doesn't keep, doesn't escape. Assigning it to a local variable is not an escape.
- **Escaping closures share their variables.** An escaping closure keeps each captured variable in a shared, reference-counted cell. So all closures and the enclosing function see the same variable, exactly like Go. A closure escapes when it is:
  - returned;
  - stored in a field, element, package variable, map or channel;
  - passed to an `own` parameter;
  - passed to a parameter that the callee may keep (store, return, or pass on to a place that keeps it), as in `http.HandleFunc("/", func(…) { … })`;
  - used with `go`;
  - captured by an escaping closure.
- **Escape analysis.** Like Go, the compiler infers for every parameter whether the callee may keep it, and records the result in export data with the other inferred attributes (P06 D6.2). A call through a func value or an interface method assumes the parameter is kept. The choice between a cell and a plain reference never changes what the program does; it only removes allocations.
- **Method values.** A method value (`f := s.Handle`) is a closure over its receiver.
- **Recursion.** A recursive escaping closure creates a reference cycle; `gopp vet` warns. Non-escaping recursive closures are fine.
- **`defer` arguments.** Deferred calls evaluate their arguments at the `defer` statement, as in Go. A borrowed argument whose variable is assigned after the `defer` is a compile error (fix-it: `clone`).

*Why:* this is Go's behavior (Go moves such variables to the heap), so closures never silently diverge from Go.
*Rejected:* capture by value with a compile error on later mutation (an earlier draft): it rejected common Go code, such as returned closure pairs that share a counter. Explicit capture lists: not Go.

**D1.12 — Interfaces and runtime type information.**
- Go's structural interfaces.
- **Shared handles.** Interface values are shared handles (D1.3). Copying one shares the same dynamic value: an immutable, reference-counted box, or the pointer it holds. So `==`, type switches and copies behave as in Go.
- **Boxing.** Converting an owned value into an interface variable, field or element moves it into a box. Storing a `*T` in an interface stores a borrow.
- **Interface parameters.** An interface parameter (including `...any`) is a shared handle like any interface value (D1.5), so the callee may keep it: store it, return it or send it.
  - Passing an interface value (such as `err`) passes the same box.
  - Passing a non-interface owned value doesn't consume it: the callee gets a borrowed `(itab, &value)`, with no box and no allocation. Only if the callee keeps it is the value cloned into a box, at that moment. So `fmt.Println(name)` doesn't consume `name`.
  - A kept slice or map is then a copy that no longer shares elements with the caller's value. This is a listed difference from Go (P04 D4.10).
  - A value that isn't `clonable` can be passed as it is only when the callee doesn't keep the parameter (D1.11). Otherwise it must be passed with `move(x)`.
- **Inline values** (owner decision; decides Q6). Trivial values up to one word (8 bytes: `int`, `float64`, raw pointers) are stored inline, with no box, so interface values stay 2 words.
- **Type switches** and assertions use compile-time type descriptors. `case string:` doesn't match a `view string`; use `case view string:`.
- For every type converted to an interface, the descriptor also carries a compact field table (field names, offsets, types, struct tags), emitted once per type (`linkonce_odr`). `fmt` and `encoding/json` read it, so printing and JSON work on any type as in Go.
- There is no user-facing `reflect` package in v1.

**D1.13 — Concurrency semantics.** `go`, `chan` and `select` work as in Go.
- `go f(a, b)`: trivial arguments are copied and owned ones are moved into the new goroutine. A raw pointer or view into a local, or into the target of a local `Unique`, is a compile error outside `unsafe`. For example, `go srv.Run()` with a local `Unique[Server]` is an error. Fix-it: `go func(){ srv.Run() }()`, whose captured variables live in shared cells (D1.11).
- Sending on a channel moves the value in; receiving moves it out.
- A send case in `select` moves its value only if that case is chosen. A temporary send value is dropped if its case is not chosen.
- The memory model is Go's happens-before model. **Data races are undefined behavior** (as in C++). `-race` (TSan) detects them.
- Program exit while goroutines are running: see D1.18.

**D1.14 — Macro and attribute surface** (semantics in P09).
- Definitions: `macro Name(p kind, …) { … }` (declarative) and `macro func Name(…) …` (procedural).
- Invocation: `#Name(args)` in expression, statement or declaration position.
- Attachment: `@Name` or `@Name(args)` before a declaration.
- Names: every macro and attribute name starts with a capital letter (`#Assert`, `@Derive`, `@Extern`), whether built-in or user-defined.
- Across packages: a macro from another package is qualified like a function: `#pkg.Name(…)`, `@pkg.Name`.
- Predeclared: the std macros `#Assert`, `#Stringify`, `#File`, `#Line`, `#Embed`, `#Todo`, `#Unreachable` and `@Derive` need no import.
- `@Derive(X)` finds `X` in std package `derive`, unless qualified (`@Derive(sql.Table)`).
- Built-in attributes use the same syntax: `@Extern("C")`, `@Export("C")`, `@Inline`, `@NoInline`, `@Packed`, `@Align(N)`, `@View`, `@NoBounds`, `@Pinned`, `@Unwind`. `@Derive(…)` is a std macro (P09).
- Errors: a macro's error or panic becomes a compile error at the use site (P09 D9.11).
- `!` never means "macro", which keeps it unambiguous as Must.

**D1.15 — Foreign import surface** (semantics in P07/P08).
- **Imports.** Headers from C/C++ packages use the package path plus the header's path inside the package: `import "github.com/madler/zlib/zlib.h"`. System and standard-library headers use angle brackets inside the string, as in C: `import "<stdio.h>"`, `import "<vector>"`.
- **Classification.** The longest prefix over both `require` and `cdep` paths decides whether an import is a Go++ package or a header. The same path in both is an error.
- **Each header is a package named after the file**, used like any Go package. There is no `C.` or `cpp.` prefix:
  - `zlib.crc32(…)`, `stdio.printf(…)`, `vector.vector[int32]`, `algorithm.sort(…)`;
  - C/C++ names keep their spelling and are always accessible, whatever their case;
  - C++ namespaces are flattened into the package: `std::sort` from `<algorithm>` is `algorithm.sort` (P08 D8.1);
  - if the file name is a Go++ keyword or predeclared name (`<map>`, `<string>`, `<any>`), the import must be named: `import stdmap "<map>"`;
  - a file name that isn't an identifier (`foo-bar.h`) must be imported with a name too.
- **Order.** Header imports keep their source order. `gopp fmt` never reorders them, because C header order can matter. `import _ "<…>"` is allowed for order-only or side-effect imports.
- **No conversions in human code.**
  - C types are plain Go++ types (`int32`, `uint64`, `byte`, …; P07 D7.4).
  - String literals and owned strings pass to `const char*` parameters as they are: owned strings are kept NUL-terminated, so there is no copy. A `view string`, a substring or a borrowed `string` parameter (which may be a substring) passes as a temporary NUL-terminated copy. Slices pass to pointer parameters (P07 D7.7).
- **Header extensions** are `.h`, `.hh`, `.hpp` and `.hxx`. This is the one list every phase uses.
- **Language.** For package headers, C or C++ comes from the package's CMake target (P11). For `<…>` headers, `.h` is C and anything else (`<vector>`, `.hpp`) is C++.
- **Headers inside a Go++ module** (yours or a dependency's) are header imports too: an import path inside a Go++ module that ends in a header extension names a header file, not a package (P10 D10.1). `.h` is C, the other extensions are C++. Flags come from the package's `//gopp:cflags`/`//gopp:cxxstd`.
- File-level build metadata uses Go-style directives: `//gopp:build`, `//gopp:cflags`, `//gopp:ldflags`, `//gopp:cxxstd`.

**D1.16 — Ten sample programs drive the spec.** Each is written in Go++ before implementation, alongside a Go counterpart and its expected output:

| # | Program | What it exercises |
|---|---|---|
| 1 | `hello` | the minimal program |
| 2 | `wc` | files, `!`, owned strings, views |
| 3 | `kvparse` | parser that stores views into one buffer; a `@View` token struct |
| 4 | `binarytrees` | allocation-heavy code; `Unique[T]`, `new_unique`, arenas |
| 5 | `workers` | goroutine pool, channels, moves |
| 6 | `zcrc` | zlib through `import "github.com/madler/zlib/zlib.h"` |
| 7 | `vecsort` | `std::vector` + `std::sort` through `import "<vector>"` and `import "<algorithm>"` |
| 8 | `derive` | `@Derive`, `#Assert`, a user-defined `macro` |
| 9 | `orderedmap` | generic container with an owning `T` |
| 10 | `minicalc` | arena-allocated AST, views into the source, `!` error handling |

Sample 8 (`derive`) needs P09 macros. P03 and M2 check the other samples.

**D1.17 — Safety defaults and layout.** These follow Go:
- integer overflow wraps;
- `int` is 64-bit on 64-bit targets;
- bounds and nil checks are on;
- integer division by zero panics.

Some operations are defined in Go but undefined in LLVM. Go++ gives them Go's results (P04 D4.12 lowers them):
- `MinInt / -1` gives `MinInt`, and `MinInt % -1` gives 0, with no trap.
- Shifts by ≥ the bit width give 0 (or -1 for `>>` of a negative signed value).
- Out-of-range float → int conversion saturates. The value is implementation-defined, never UB.
- Floating-point fusing (FMA) follows Go's compiler on each target, so results match Go.

Struct layout is declaration order with C alignment. That is C-compatible, which P07 requires, so fields are never reordered automatically.

**D1.18 — Program initialization and exit.**
- C/C++ static initializers from imported code run first, as part of C runtime startup.
- Then Go++ package `init`s run in import-dependency order, then `main.main`.
- **Exit.** After `main.main` returns and its deferred calls have run, the runtime:
  1. stops all other goroutines (P12 D12.12);
  2. drops `main`'s locals;
  3. calls C `exit()`, which runs atexit handlers, flushes stdio and runs C++ static destructors.

  This is safe because no Go++ code is running any more.
- **`os.Exit(code)`** skips defers and drops, stops the goroutines, and calls `exit(code)`.
- **Unrecovered panic.** The trampoline prints the panic, stops the other goroutines, and calls `exit(2)` (P04 D4.6). If the panic leaves `main.main`, the other goroutines are stopped before `main`'s locals are dropped, as at a normal exit.
- **Package-level variables are never destroyed** (as in Go). That avoids C++'s static-destruction-order problems. Buffered writers must be flushed explicitly, as in Go.

**D1.19 — Errors work like Go.**
- `error` is an interface, so it is a shared handle (D1.3, D1.12). `return ErrNotFound` shares the same error, and `err == io.EOF`, `errors.Is`, `%w` and type switches work as in Go.
- Package-level errors from `errors.New` are static: never freed, never counted. Other error values are reference-counted.
- **The `!` trace** is stored per goroutine, keyed by the error's identity, outside the error. It never changes `==`, type switches or `Error()`. It is printed by a `!` panic and by `%+v`. After an error crosses goroutines, its trace is best effort.

**D1.20 — Owned slices: re-slicing in place** (owner decision).
- `s = s[i:j]` on the same owned slice shrinks it in place: it drops the elements outside `[i, j)` and keeps the capacity.
- For `i > 0`, the remaining elements move down (O(n)), so a slice needs no extra offset field. Its layout is in D1.22.
- For fast FIFO queues, std has `container/deque`.
- Reset a buffer with `buf = buf[:0]`.
- Append-style functions take `dst own []T`: `buf = strconv.AppendInt(buf, n, 10)` moves implicitly (D1.4 rule 4).
- `cap()` of a borrowed slice equals its length.

**D1.21 — Range loops** (decides Q7).
- `for i, x := range xs` borrows each element.
- `for _, x := range move(xs)` moves the elements out one by one.
- Inside the loop body, a statement that can reallocate the ranged slice or map is a compile error when the compiler can see it: appending to it, assigning it, inserting into the map, or calling `clear` on it. `delete` is allowed.
- Maps also count their active iterators, and panic with "map grew during iteration" in the cases the compiler can't see.
- Go allows inserting into a map during `range`, so this is a listed difference from Go (P04 D4.10).

**D1.22 — Slices and maps carry their allocator** (owner decision; decides Q4).
- An owned `[]T` is 4 words: pointer, length, capacity and allocator. A map header also stores its allocator.
- A nil allocator means the global allocator, so zero values stay valid. `make`, `append`, literals and conversions use the global allocator unless told otherwise.
- `mem` functions create a slice or map in another allocator, such as a `mem.Arena` (P05 D5.3).
- Growth (`append`), `clone` and drop use the value's own allocator; a clone stays in the same allocator.
- Views and borrowed `[]T` parameters stay `(ptr, len)`: reading and writing elements needs no allocator. An `own []T` parameter passes all 4 words.
- `string` always uses the global allocator, so strings stay 3 words.
- The allocator word points to an allocator object whose first word is its function table, so one pointer is enough.

*Why:* arena- and pool-backed collections keep the same `[]T` and `map` types everywhere.
*Cost:* one more word in every owned slice and map header.

## Deliverables

- `docs/spec/`: overview, lexical, types, ownership, parameters, expressions, statements, slices-and-range, errors-and-panics, generics, concurrency, macros-surface, foreign-surface, program-init, undefined-behavior.
- `docs/spec/examples/`: the ten samples, each with a Go counterpart and its expected output.
- ADR-0003 (memory model) and ADR-0004 (`!`) updated to the final text.

## Exit criteria

- Every delta D1.2–D1.21 has grammar, semantics, an example and a rationale in `docs/spec/`.
- All ten samples are written, annotated with expected output, and compared against their Go versions for readability.
- Q1–Q8 are decided, or explicitly deferred to a named phase.
- The list of undefined and implementation-defined behavior is written.

## Risks

- **The ownership model reads poorly in real code** → write the samples first. After M2 there is a spec-revision window (P05) before v1 freezes anything.
- **Spec ambiguities found during implementation** → the spec is versioned (`gopp 0.1` in gopp.mod), and fixes go through ADRs.
- **Too many new keywords** → only `own`, `view` and `macro` are new; owning pointers are predeclared generic types (D1.7).

## Open questions (close in P01.12 unless noted)

- **Q1** (decided 2026-10-27) Closure capture rule: non-escaping closures capture by reference; escaping closures keep captured variables in shared cells, like Go (D1.11).
- **Q2** (decided 2026-10-27) Map element access with owning values: `m[k]` borrows where a borrow is enough; taking ownership needs `clone(m[k])` or `move(m[k])`, both with a comma-ok form; `m[k].f = x` stays illegal, as in Go (D1.4 rule 3a).
- **Q3** (decided 2026-10-27) Owning pointers are predeclared generic types `Unique[T]`, `Shared[T]`, `Weak[T]`, created with `new_unique`, `new_shared`, `new_weak` (D1.7).
- **Q4** (decided 2026-10-27) Allocators for `[]T`/`map`: every owned slice and map stores its allocator; nil means the global one (D1.22).
- **Q5** (decided 2026-10-27) Generic methods on concrete types: allowed; they can't satisfy interface methods (D1.10).
- **Q6** (decided 2026-10-27) Interface inline storage: one word (8 bytes), trivial values only (D1.12).
- **Q7** (decided 2026-10-27) Ranging over owning elements: `for _, x := range xs` borrows each element, and `for _, x := range move(xs)` moves them out (D1.21).
- **Q8** (deferred to P15, P15 Q1) Small-string optimization for `string`? Proposed: no in v0. The layout is ABI, so P15 decides before v1.0.
