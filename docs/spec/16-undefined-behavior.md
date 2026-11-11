# 16. Undefined and implementation-defined behavior

Base: Go spec [Run-time panics](https://go.dev/ref/spec#Run_time_panics), [Integer overflow](https://go.dev/ref/spec#Integer_overflow), [Arithmetic operators](https://go.dev/ref/spec#Arithmetic_operators), [Conversions](https://go.dev/ref/spec#Conversions), [Package unsafe](https://go.dev/ref/spec#Package_unsafe), [Size and alignment guarantees](https://go.dev/ref/spec#Size_and_alignment_guarantees).

Go++ has a borrow checker with no lifetime annotations ([OWN-B1 to B10](04-ownership.md#45-borrow-checking-adr-0021), ADR-0021 and its amendment). It turns most "used after it was thrown away" cases into compile errors, and it allows one writable borrow of a place at a time, so hidden aliasing is a compile error too ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). Data races are compile errors in normal code too ([CON-12 to CON-20](12-concurrency.md), ADR-0022). This chapter is the complete list of what the compiler does **not** protect you from, and of the places where the result depends on the implementation. Where a rule below was once fully undefined, it now says what is an error and what small residue is left.

Terms:
- **Undefined behavior (UB):** the program can do anything. The compiler and the runtime promise nothing.
- **Implementation-defined:** the result is fixed per target or compiler, and documented, but may differ between targets.
- **Defined (Go semantics):** Go defines a result, and Go++ keeps it, even where C would say UB.

Syntax: none.

## 16.1 Defined as in Go (not UB)

**UB-1a.** These operations have Go's results. P04 D4.12 lowers them. Each one is also in the differential corpus.

| Operation | Result |
|---|---|
| integer overflow | wraps |
| `MinInt / -1` | `MinInt` |
| `MinInt % -1` | `0` |
| shift by ≥ bit width | `0`; `-1` for `>>` of a negative signed value |
| integer division by zero | panics |
| out-of-range float to int conversion | saturates deterministically (`MaxInt`, `MinInt`, `0` for `NaN`, [EXP-8](06-expressions.md)), never UB |
| bounds and nil dereference | panic (checks are on); pointers, funcs and interfaces with methods (all but `error`) are never nil, so a nil func call or a nil method call cannot happen in safe code (a method call on a nil `error` panics, as in Go; `any` has no methods) |
| use after move | compile error, not UB |

## 16.2 Undefined behavior

**UB-1.** *Dangling borrows (`*T`, borrow-carrying structs, and views of slices and arrays).* Using a raw pointer `*T`, a borrow-carrying struct, or a view `&s[i:j]` of a slice or array (a `*[]T` whose slice has `backing == nil`) after its owner is dropped, moved, assigned or grown is a **compile error** ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)). Storing any tracked borrow into a global, `NewUnique`/`NewShared`, a heap container, a channel, an escaping closure, a kept `Send` interface or a `go` statement is a compile error ([OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)), as is returning `&local` or `&localArr[i:j]` ([OWN-B3, OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)). (Slicing a heap `string` shares its refcounted `backing` buffer, and `s[i:j]` on a slice or array without `&` is a new slice with its own elements, [OWN-S1, OWN-S2](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032), so sub-strings and sub-slices cannot dangle.)

*Residue (still UB):* a pointer created or kept through `unsafe` or raw memory (UB-7), or through C or C++ code (UB-9, UB-10).

```go
func first(a *[4]int) *int { return &a[0] }     // ok: borrows parameter array a (OWN-B3)

func bad() *int {
    var local [4]int
    return first(&local)    // compile error: result borrows local, which dies at return (OWN-B3, OWN-B5)
}

var keep Option[*int]
func worse() {
    x := 42
    keep = &x               // compile error: borrow stored in a global (OWN-B5)
}
```

**UB-2.** *Pointer aliasing.* In normal code this is a **compile error**: while a writable borrow (`*T` or `&` range alias) of a place is live, no other borrow of that place or of an overlapping place may be live ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). This covers both visible overlap at one call ([OWN-B4](04-ownership.md#45-borrow-checking-adr-0021)) and two pointers that may name the same element. Two pointers into one arena may be the same value; nothing can be borrowed through them from storage the value owns separately ([OWN-B10](04-ownership.md#45-borrow-checking-adr-0021)), so neither can free what the other points into.

```go
func (u *User) SetFrom(src *string) {
    u.Name = *src
    u.Audit = "renamed to " + *src
}
u.SetFrom(&u.Name)            // compile error: &u.Name overlaps writable receiver u (OWN-B4, OWN-B9)

func swapNames(a, b *Node) { old := a.Name; b.Name = "x"; println(old) }
swapNames(&list[i], &list[j]) // compile error: i may equal j (OWN-B9); use list.Swap(i, j) or Split
```

*Residue (still UB):* none in normal code. Unchecked pointer aliasing that can free borrowed storage can only be made in `unsafe` code or in C or C++ code. What a `Shared[T]` holds is read-only through the handle, so no handle can free something another handle points into; a part that changes is in a `Mutex`, `RWMutex` or `Atomic` ([OWN-P8](04-ownership.md#43-pointers-and-heap-allocation-d17), [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021), [ADR-0042](../adr/0042-shared-is-read-only.md)). `*T` pointers omit C `restrict` qualifiers when reachable from `Shared[T]`, and generated C23 compiles with `-fno-strict-aliasing`.

**UB-2a.** *Using a borrowed range variable after its element was written.* In `for _, d := range docs` over elements that cannot be copied, `d` is a read-only borrowed variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) that borrows the element. Writing the current element or entry (`docs[i] = Doc{}`) is defined (the old value is dropped, [RNG-3](08-slices-and-range.md)), and reading `d` **after** that write is a compile error ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)).

```go
for i, d := range docs {    // docs is []Doc and Doc cannot be copied: d borrows docs[i]
    docs[i] = Doc{}         // defined: old Doc is dropped
    fmt.Println(d.Title)    // compile error: d is live after its element was overwritten
}
```

Over elements that can be copied, the plain range variable is a copy of the element ([RNG-1](08-slices-and-range.md), [ADR-0041](../adr/0041-values-are-copied.md) rule 12), so `ss[0] = "z"; fmt.Println(s)` on `[]string` is safe and prints the old string, as in Go. An `&` range variable is an alias of the slot ([RNG-1a](08-slices-and-range.md)), so it sees the new value directly, while writing the same element through both `xs[i]` and a live `&e` is a compile error (OWN-B9). After `delete(m, k)` on the current map entry, the map delays freeing the slot until the end of the iteration ([RNG-3](08-slices-and-range.md)).

**UB-2b.** *Argument changed through an aliased path.* A by-value parameter that the function only reads (`string`, `[]T`, `map[K]V`, `error`, `func`, `Shared[T]`) is passed without a copy (for a counted handle, at 0 retain/release) only when the argument cannot change during the call, as for an unaliased local or a temporary ([PAR-1](05-parameters.md), [ADR-0041](../adr/0041-values-are-copied.md) rule 4). When the argument is read from an aliasable place (`s.fail(s.err)`, `u.SetName(u.Name)`, `run(reg, reg.cb)`, `xs[i]`, `m[k]`, `*p`, a global, a captured cell, or a `Shared[T]` field) that the callee could change, the copy is made: the caller holds a temporary retain of a counted handle (`string`, `func`, `Shared[T]`) across the call, a `[]T` or `map[K]V` argument gets its own buffer, and an interface argument (an `error`) gets its own copy of the value it holds ([PAR-1](05-parameters.md)). So even if the callee overwrites `s.err` or `reg.cb`, the parameter stays valid until the call returns.

**UB-2c.** *`defer` arguments.* By-value `defer` arguments are evaluated and copied at the `defer` statement ([PAN-3](09-errors-and-panics.md), [CLO-7](11-closures.md)): a counted handle (`string`, `Shared[T]`, etc.) is retained, and a `[]T` or `map[K]V` is copied with its elements, so nothing done to the variable afterwards reaches the deferred call. `*T` `defer` borrows are checked by [OWN-B2, OWN-B9](04-ownership.md#45-borrow-checking-adr-0021).

**UB-2d.** *Interface reassigned while an assertion binding is in use.* This case no longer exists: `x := v.(T)` and `switch x := v.(type)` give `x` a copy of the value that `v` holds ([TYP-9](03-types.md), [ADR-0041](../adr/0041-values-are-copied.md) rule 11). An assignment to `v` (or through a pointer to `v`) while `x` is read afterwards is allowed and does not change `x`.

**UB-2e.** *Ranged slice or string reallocated.* `range` takes `(ptr, len)` once ([RNG-3a](08-slices-and-range.md)) and borrows the ranged variable for the whole loop ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)). Appending to, reassigning, or `+=`-growing the ranged variable in the loop body (directly or through an overlapping receiver/pointer) is a compile error ([RNG-3](08-slices-and-range.md), [OWN-B4, OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). *Residue (still UB):* reallocating or freeing the ranged backing buffer through `unsafe` or C code.

**UB-3.** *Data races.* Two goroutines access the same memory, at least one writes, and there is no happens-before edge between them ([CON-7](12-concurrency.md), [Go memory model](https://go.dev/ref/mem)). In normal code this is a **compile error**: `go` may pass or capture only moved, `Send` copied or `Sync` shared values ([CON-12, CON-13](12-concurrency.md)), data is reached through `Mutex[T]` guards ([CON-15](12-concurrency.md)), and package variables written after init must be `Sync` ([CON-18](12-concurrency.md)).

*Residue (still UB):*
- a race in `unsafe` code or through raw memory (UB-7);
- a race with C or C++ threads, or in C and C++ callbacks into Go++ (UB-9, UB-10).

`-race` (TSan) detects the residue. Logic races (check-then-act across two lock sections) and deadlocks the compiler cannot see are not UB, because the memory stays valid, but nothing catches them either ([CON-21](12-concurrency.md)).

```go
x := 0
go func() { x++ }()          // compile error (CON-13): was UB-3
var n sync.Atomic[int]
go func() { n.Add(1) }()     // ok
```

**UB-4.** *`longjmp` across Go++ frames.* A C `longjmp` that jumps over Go++ frames skips destructors and deferred calls. UB ([PAN-9](09-errors-and-panics.md)). Also a C++ exception or a Go++ panic thrown through a frame marked `nounwind` by mistake.

**UB-5.** *Allocator lifetime.* Resetting or freeing a `mem.Arena` or `mem.DropArena` while a value allocated in it is still used later is a compile error: a value from an arena borrows the arena ([OWN-B1, B2](04-ownership.md#45-borrow-checking-adr-0021), [SLC-3](08-slices-and-range.md), P05 D5.3). Fix: finish with the value before `Reset`, or copy it out to the normal heap with an explicit copy-out function (illustrative `mem.CloneTo[User](u, mem.Heap)`; a plain copy does not help, it stays in the arena, SLC-3). *Residue (still UB):* arena memory reached through `unsafe` or from C.

**UB-5a.** *Mixed allocators.* Freeing memory with an allocator other than the one that made it: a Go++ (mimalloc) pointer passed to C `free` or `realloc`, a C-owned pointer that a Go++ drop frees (for example a `ffi.CString` buffer after `.Release()`, which is then C's to free), or a C++ `new` object freed with `free`. UB ([ADR-0027](../adr/0027-mimalloc-global-allocator.md) rule 4a). Go++ never adopts memory that C returns, and `ffi.CString(...).Release()` buffers come from C `malloc` for this reason. Not detected by default; the allocator test hook (P05.03) detects it in non-ASan builds. In ASan builds both sides use the sanitizer heap, so the mismatch is invisible there and the code stays wrong.

**UB-6.** *`@NoBounds`.* An out-of-range index or slice bound in a function with bounds checks turned off.

UB-7 to UB-12 are not listed in D1.17 or D1.4 rule 8. They are collected from P05, P07, P08 and P12 (arena lifetime, `unsafe`, C lifetimes, C++ objects, runtime objects, forced unwinding). The owner confirmed in P01.15 that they stay listed as undefined behavior.

**UB-7.** *`unsafe` and raw memory.* Misuse of `unsafe.Pointer`: wrong type, wrong size, misaligned access, reading uninitialized memory, converting a nil `unsafe.Pointer` to a `*T` (pointers are never nil; convert to `Option[*T]` instead), or mutating immutable `string` bytes through `unsafe.Pointer`.

**UB-8.** *Pinned and moved values.* Using a pointer or borrow into a local after the local has been moved ([OWN-R10](04-ownership.md#42-ownership-rules-d14)) is a compile error, wherever the pointer came from in the same function, and also when it came through a call ([OWN-B2, OWN-B3](04-ownership.md#45-borrow-checking-adr-0021)). *Residue (still UB):* a pointer kept through `unsafe` or C.

**UB-9.** *C interop lifetimes.*
- Passing a Go++ string or slice to a C function that keeps the pointer after the call (`openlog`, `putenv`, `sqlite3_bind_text` with `SQLITE_STATIC`). `gopp vet` has a list of known functions.
- Using the result of `ffi.String(p)` (a non-owning string over C memory) after the C memory is freed.
- Passing a temporary NUL-terminated substring copy to a C callee that keeps the pointer ([FOR-6](14-foreign-surface.md)).
- C or C++ code that writes NULL into a Go++ pointer (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`, all never nil), a func or an interface with methods other than `error` (never nil too), or a never-nil `@Required` field, or zero bytes over a struct with no zero value ([TYP-15](03-types.md)), for example through a pointer passed to C. Go++ code dereferences pointers and calls funcs and interface methods with no nil check (ADR-0018); this, and `unsafe` (UB-7), are the only ways a nil one can reach safe code. A C pointer that may be NULL imports as `Option[*T]`, so this needs a lie in a header (`_Nonnull` on a pointer that can be NULL) or a write through memory.
- C or C++ code that writes a Go++ package variable while Go++ code holds a borrow of it. The checker sees only Go++ writes ([OWN-B3](04-ownership.md#45-borrow-checking-adr-0021), [CON-18](12-concurrency.md)).

**UB-10.** *Misuse of C++ objects.* Using a C++ object after a move except in ways its class allows; calling through a dangling reference returned as `*T` (D8.7).

**UB-11.** *Concurrent use of unsynchronized runtime objects.* A map written while another goroutine reads or ranges over it is a data race. In normal code it is a **compile error**: a map is `Sync` only for reading ([CON-12](12-concurrency.md)): no goroutine may write it while another goroutine can reach it; wrap it in `Mutex[map[K]V]` ([CON-15](12-concurrency.md)). The same holds for the first write to a nil map by two goroutines (a write creates the map, [OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17), and a write needs exclusive access), and for a slice or string variable that two goroutines can reach while one of them writes it. The "map insert during iteration" panic ([RNG-4](08-slices-and-range.md)) is a best-effort check for single-goroutine misuse. *Residue (still UB):* the same access from `unsafe` code or from C and C++ threads (UB-3).

**UB-12.** *Forced unwinding.* `pthread_exit` or thread cancellation through Go++ frames: `recover()` does not stop it, and destructors run only as far as the platform unwinder runs them ([PAN-9](09-errors-and-panics.md)).

## 16.3 Implementation-defined

**UB-13.** These depend on the target or the compiler version. They are documented, not UB.
- Floating-point fusing (FMA): follows Go's compiler on each target, so results can differ between amd64 and arm64, as in Go.
- Size and alignment of C types (follow the target ABI); `int` size on 32-bit targets.
- Struct layout is declaration order with C alignment (fixed by the spec, D1.17).
- Goroutine stack size and the scheduling order.
- Map iteration order (random, as in Go).
- Message text of stack traces.
- The size of `ffi.LongDouble` and the mapping of C `long` on Windows.

**Implementation limit.** The v0 compiler limits the AST height of one declaration to 1024 levels (nesting and operator, selector, index and call chains together; `gopp check` and `gopp vet` do not count a selector on a selector, so `a.b.c...` may be as long as memory allows, and `gopp parse` and `gopp fmt` keep the limit for them). A flat chain of about 1000 terms (`a + b + ...`) or about 1000 `else if` branches is rejected with the error "expression too deeply nested". This is an implementation choice, not part of the language, and it may be raised.

## 16.4 What catches what

| Case | Compiler | `gopp vet` | `-asan` | `-race` |
|---|---|---|---|---|
| use after move | error | | | |
| returning `&local` or `&localArr[i:j]` (OWN-B3, OWN-B5) | error | | | |
| `go f(&local)` | error | | | |
| `defer` borrowing a dying variable | error | | | |
| pointer/borrow used after its owner changed or died (UB-1, OWN-B2) | error | | residue only (`unsafe`, C) | |
| borrow stored in a global, a heap container, a channel, an escaping closure or a `go` (OWN-B5) | error | | | |
| writable argument and a borrow of the same place at one call (UB-2, OWN-B4) | error | | | |
| two borrows of overlapping places, one of them writable, including through two pointers (UB-2, OWN-B9) | error | | | |
| sharing a `Shared[T]` whose `T` is not `Sync` across goroutines (CON-19, OWN-P8) | error | | | |
| changing what a `Shared[T]` holds without a `Mutex`, `RWMutex` or `Atomic` (OWN-P8) | error | | | |
| a borrow into what a value owns separately, through a pointer into an arena (OWN-B10) | error | | | |
| by-value argument read from an aliasable place (`s.fail(s.err)`, UB-2b, PAR-1) | safe (the copy is made: a temp retain, an own buffer for a slice or map, or an own copy for an interface value) | | | |
| by-value `defer` argument reassigned or changed after `defer` (UB-2c, PAN-3) | safe (copied at the `defer`) | | | |
| `v` reassigned while an assertion binding `x` is read afterwards (UB-2d) | safe (`x` is a copy) | | | |
| ranged slice, map or string changed in the loop body (UB-2e) | error | | | |
| borrowed range variable used after its element was written (UB-2a) | error | | | |
| data race in normal code (UB-3, CON-12 to CON-19) | error | | | residue only |
| data race in `unsafe`, C or C++ code (UB-3 residue) | | | | yes |
| opposite lock order, or a lock taken twice with the guard live (CON-20) | error when visible | | | |
| logic race, or a deadlock the compiler cannot see (CON-21) | | | | no |
| `longjmp` (UB-4) | | | maybe | |
| arena reset with live data (UB-5, OWN-B1) | error | | residue only | |
| C keeps a pointer (UB-9) | | known list | maybe | |
| C writes NULL into a Go++ pointer, a func, an interface with methods or a never-nil `@Required` field (UB-9) | | | no (a NULL read crashes like any NULL dereference) | |

## 16.5 Rationale

- The owner chose (2026-10-28, ADR-0021) a borrow check without lifetime annotations: bounds checks, nil checks, zero values, use-after-move and use-after-change errors, and sanitizers one flag away for the residue. It is conservative, so a copy (`y := x`) is the usual fix for a false error.
- The owner chose (2026-10-28, ADR-0021; ADR-0032) that the compiler reads function bodies to see what a `*T` result borrows (no lifetime syntax), that local/unique `*T` borrows are exclusive, and (2026-11-11, [ADR-0042](../adr/0042-shared-is-read-only.md)) that what a `Shared[T]` holds is read-only through the handle, with every change going through a `Mutex`, `RWMutex` or `Atomic`, in one goroutine as across goroutines. What is left undefined is `unsafe`, C and C++, data races there, and `longjmp` / forced unwinding.
- Keeping Go's results for overflow, division and shifts means ported code behaves the same, even where C would allow UB.
- The owner chose (2026-10-28, ADR-0022) that data races are compile errors in normal code, with the automatic labels `Send` and `Sync`, mutexes that hold their data, and scoped goroutines. Defining races would cost every shared access, so the residue (`unsafe`, C and C++ threads) stays UB and `-race` stays one flag away.
