# 16. Undefined and implementation-defined behavior

Base: Go spec [Run-time panics](https://go.dev/ref/spec#Run_time_panics), [Integer overflow](https://go.dev/ref/spec#Integer_overflow), [Arithmetic operators](https://go.dev/ref/spec#Arithmetic_operators), [Conversions](https://go.dev/ref/spec#Conversions), [Package unsafe](https://go.dev/ref/spec#Package_unsafe), [Size and alignment guarantees](https://go.dev/ref/spec#Size_and_alignment_guarantees).

Go++ has a borrow checker with no lifetime annotations ([OWN-B1 to B9](04-ownership.md#45-borrow-checking-adr-0021), ADR-0021 and its amendment). It turns most "used after it was thrown away" cases into compile errors, and it allows one writable borrow of a place at a time, so hidden aliasing is a compile error too ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). Data races are compile errors in normal code too ([CON-12 to CON-20](12-concurrency.md), ADR-0022). This chapter is the complete list of what the compiler does **not** protect you from, and of the places where the result depends on the implementation. Where a rule below was once fully undefined, it now says what is an error and what small residue is left.

Terms:
- **Undefined behavior (UB):** the program can do anything. The compiler and the runtime promise nothing.
- **Implementation-defined:** the result is fixed per target or compiler, and documented, but may differ between targets.
- **Defined (Go semantics):** Go defines a result, and Go++ keeps it, even where LLVM would say UB.

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
| out-of-range float to int conversion | saturates; the exact value is implementation-defined, never UB |
| bounds and nil dereference | panic (checks are on); pointers, funcs and interfaces with methods (all but `error`) are never nil, so a nil func call or a nil method call cannot happen in safe code (a method call on a nil `error` panics, as in Go; `any` has no methods) |
| use after move | compile error, not UB |

## 16.2 Undefined behavior

**UB-1.** *Dangling views and raw pointers.* Using a `View[…]`, a borrowed `[]T` made by slicing, or a `*T` after its owner is destroyed or has been changed so the memory moved is a **compile error** ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)). This includes a view stored past the end of the owner's block, a view into a slice element after `append`, a view into a `string` after it was reassigned, appended to or dropped, a view into a map after it grew, a pointer into an owner that was moved from, and a view boxed into an interface that outlives its owner. Borrows that leave the places the checker can follow (a global, a heap container, a channel, an escaping closure, a `go` statement) are errors too ([OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)), and so is returning `&local` or a view into a local ([OWN-V7](04-ownership.md#44-views-d16)). A view that comes out of a call borrows from the arguments the callee's borrow summary names ([OWN-B3](04-ownership.md#45-borrow-checking-adr-0021)).

*Residue (still UB):* a borrow created or kept through `unsafe` or raw memory (UB-7), or through C or C++ code (UB-9, UB-10).

```go
func trimmed(s string) View[string] { return s[1:] }   // ok: a view into a borrowed parameter

func bad(name string) View[string] {
    s := "abc" + name
    return trimmed(s)       // compile error: the result borrows from s, which dies at return (OWN-B3, OWN-V7)
}

var keep View[string]
func worse(items []string) {
    t := strings.ToUpper(items[0])
    keep = t[:1]            // compile error: a borrow stored in a global (OWN-B5)
}
```

**UB-2.** *Aliasing.* In normal code this is a **compile error**: while a writable borrow of a place is live, no other borrow of that place or of an overlapping place may be live ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). This covers the visible case at one call ([OWN-B4](04-ownership.md#45-borrow-checking-adr-0021)) and the hidden case where two pointers may name one object.

```go
func (u *User) SetName(n string) {
    u.Name = n.Clone()
    u.Audit = "renamed to " + n        // n borrows the old u.Name, which was dropped
}
u.SetName(u.Name)       // compile error: n borrows u.Name, and the receiver can write it

func swapNames(a, b *Node) { old := a.Name; b.Name = "x"; println(old) }
swapNames(list[i], list[j])   // compile error: i may equal j (OWN-B9); use list.Swap(i, j) or Split
```

Fix: `u.SetName(u.Name.Clone())`.

*Residue (still UB):* none in normal code. Two pointers to one object can only be made in `unsafe` code or in C or C++ code. Through a `Shared` handle only cell types can be written, and a `mem.Cell` checks overlapping access at run time: the result is a panic ("cell already borrowed"), not UB.

**UB-2a.** *Using a borrowed range variable after its element was written.* In `for _, s := range ss` over owning elements, `s` is a borrowed variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) that borrows the element. Writing the current element or entry is itself defined (the old value is dropped, [RNG-3](08-slices-and-range.md)). Using `s` **after** the write is a compile error ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)): the write frees the old value, and `s` is still live. A write of a pointer-free Copy value, and a write after the last use of `s`, are fine.

```go
for _, s := range ss {      // ss is []string: s borrows ss[0]
    ss[0] = "z"             // defined: the old string is dropped
    fmt.Println(s)          // compile error: s is live and its element was freed
}
```

Trivial elements are copies ([RNG-1](08-slices-and-range.md)), so the same code on `[]int` is defined and prints the old value, as in Go. An `&` range variable is an alias of the slot ([RNG-1a](08-slices-and-range.md)), so it sees the new value and is not affected. A write to the element through anything other than `e` is an error while `e` is live ([RNG-1a](08-slices-and-range.md), OWN-B9).

The same holds for a range over an array with owning elements (the range variable borrows the live element, [RNG-6](08-slices-and-range.md)), and for `m[k] = x` on the entry the loop is on and `copy(dst, …)` into the ranged slice, followed by a use of the range variable.

Not UB: `delete(m, k)`, `move(m[k])` and `v, ok := move(m[k])` of the entry the loop is on (the key, and for `delete` the value, are dropped at the end of the iteration; after `move(m[k])` a borrowed `v` reads the zero value, not the moved-out value; for a type with no zero value, reading `v` after it is a compile error, [RNG-3](08-slices-and-range.md)). A view or pointer into a removed entry that is used past its iteration or past the removal is a compile error under OWN-B2.

**UB-2b.** *Shared handle released through another path.* A shared-handle parameter is passed without a retain ([PAR-7](05-parameters.md)). If the callee drops the caller's last reference through another path and then uses the parameter, the value is freed. The visible case, where the argument and the writable receiver or argument are the same place (`s.fail(s.err)`), is a compile error ([OWN-B4](04-ownership.md#45-borrow-checking-adr-0021)). There is no UB in normal code: every raw pointer is a tracked borrow ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021) rule 4), and a `Shared` handle is read-only. The one way to change or free a `Shared` target while it is in use is through a `mem.Cell` (or `Mutex`); a value taken from a cell is tied to a run-time cell guard, so a write to that cell while the guard is live **panics** ("cell already borrowed"). The residue is that clear run-time panic, not UB.

```go
func (s *Server) fail(err error) {
    s.err = nil                // drops the last reference to the error
    log.Println(err)           // UB: err was a borrow of s.err, not retained
}
s.fail(s.err)

func run(reg *Registry, cb func()) { reg.cb = noop; cb() }  // same with run(reg, reg.cb): the old func is released
```

Fix: copy the handle first (`e := err` retains), or pass it as a retained copy.

**UB-2c.** *Writes to a borrowed `defer` argument* change what the deferred call reads, where Go would have copied the value ([CLO-7](11-closures.md)). In normal code these are compile errors (CLO-7, and writes through another pointer by [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)).

**UB-2d.** *Interface reassigned while a binding borrows its box.* After `s, ok := v.(string)` or `switch x := v.(type)`, the binding `s`/`x` borrows the box ([TYP-9](03-types.md)). A visible assignment to `v` (or to a prefix of it) while `s` is used afterwards is a compile error ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)). A write through a pointer to `v` is a writable borrow of `v`, so it is checked by [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021) too. Another goroutine is a data race: a compile error in normal code ([CON-13](12-concurrency.md)), UB-3 in the residue.

**UB-2e.** *Ranged slice or string reallocated.* `range` takes `(ptr, len)` once ([RNG-3a](08-slices-and-range.md)) and borrows the collection for the whole loop ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)). A call in the body that appends to, reassigns or re-slices the ranged slice (`for _, v := range w.queue { w.push(10) }`), or grows an owned string, is now a compile error when the call's receiver or argument overlaps the ranged place ([OWN-B4](04-ownership.md#45-borrow-checking-adr-0021)); the old buffer would be freed and the loop would read freed memory, even for a slice of `int`. Go keeps the old array alive, so the same code is safe there. A call that reaches the ranged slice through another pointer is also an error, because that pointer is a writable borrow of the slice ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021); [RNG-3](08-slices-and-range.md) lists the cases).

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

**UB-5.** *Allocator lifetime.* Resetting or freeing a `mem.Arena` or `mem.DropArena` while a value allocated in it is still used later is a compile error: a value from an arena borrows the arena ([OWN-B1, B2](04-ownership.md#45-borrow-checking-adr-0021), [SLC-3](08-slices-and-range.md), P05 D5.3). Fix: finish with the value before `Reset`, or copy it out to the normal heap with an explicit copy-out function (illustrative `mem.CloneTo[User](u, mem.Heap)`; `Clone` does not help, a clone stays in the arena, SLC-3). *Residue (still UB):* arena memory reached through `unsafe` or from C.

**UB-6.** *`@NoBounds`.* An out-of-range index in a function (or package) with bounds checks turned off.

UB-7 to UB-12 are not listed in D1.17 or D1.4 rule 8. They are collected from P05, P07, P08 and P12 (arena lifetime, `unsafe`, C lifetimes, C++ objects, runtime objects, forced unwinding). The owner confirmed in P01.15 that they stay listed as undefined behavior.

**UB-7.** *`unsafe` and raw memory.* Misuse of `unsafe.Pointer`: wrong type, wrong size, misaligned access, reading uninitialized memory, converting a nil `unsafe.Pointer` to a `*T` (pointers are never nil; convert to `Option[*T]` instead), or writing through a pointer converted from an `unsafe.Pointer` that was read out of a view (`*(*int)(v[0]) = 1` with `v View[[]unsafe.Pointer]`; [OWN-V1](04-ownership.md#44-views-d16)).

**UB-8.** *Pinned values.* Using a pointer or view into a local after the local has been moved ([OWN-R10](04-ownership.md#42-ownership-rules-d14)) is a compile error, wherever the pointer came from in the same function, and also when it came through a call ([OWN-B3](04-ownership.md#45-borrow-checking-adr-0021)): `move(x)` is a change that borrows may not survive ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)). *Residue (still UB):* a pointer kept through `unsafe` or C.

**UB-9.** *C interop lifetimes.*
- Passing a Go++ string or slice to a C function that keeps the pointer after the call (`openlog`, `putenv`, `sqlite3_bind_text` with `SQLITE_STATIC`). `gopp vet` has a list of known functions.
- Using the result of `ffi.String(p)` (a view over C memory) after the C memory is freed.
- Passing a `View[string]` temporary copy to a callee that keeps the pointer (FOR-6).
- C or C++ code that writes NULL into a Go++ pointer (`*T`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`, all never nil), a func or an interface with methods other than `error` (never nil too), or a never-nil `@Required` field (or a required field of a `@Strict` struct), or zero bytes over a struct with no zero value ([TYP-15](03-types.md)), for example through a pointer passed to C. Go++ code dereferences pointers and calls funcs and interface methods with no nil check (ADR-0018); this, and `unsafe` (UB-7), are the only ways a nil one can reach safe code. A C pointer that may be NULL imports as `Option[*T]`, so this needs a lie in a header (`_Nonnull` on a pointer that can be NULL) or a write through memory.

**UB-10.** *Misuse of C++ objects.* Using a C++ object after a move except in ways its class allows; calling through a dangling reference returned as `*T` (D8.7).

**UB-11.** *Concurrent use of unsynchronized runtime objects.* A map written while another goroutine reads or ranges over it is a data race. In normal code it is a **compile error**: a map is `Sync` only for reading ([CON-12](12-concurrency.md)): no goroutine may write it while it is shared; wrap it in `Mutex[map[K]V]` ([CON-15](12-concurrency.md)). The same holds for the first write to a nil map by two goroutines (a write creates the map, [OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17), and the map is not `Sync`), and for slices and strings shared with a writer. The "map insert during iteration" panic ([RNG-4](08-slices-and-range.md)) is a best-effort check for single-goroutine misuse. *Residue (still UB):* the same access from `unsafe` code or from C and C++ threads (UB-3).

**UB-12.** *Forced unwinding.* `pthread_exit` or thread cancellation through Go++ frames: `recover()` does not stop it, and destructors run only as far as the platform unwinder runs them ([PAN-9](09-errors-and-panics.md)).

## 16.3 Implementation-defined

**UB-13.** These depend on the target or the compiler version. They are documented, not UB.
- The value of an out-of-range float to int conversion (the conversion saturates; D1.17 calls the exact value implementation-defined).
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
| returning `&local` | error | | | |
| returning a view into a local, or a view that comes out of a call and borrows a local ([OWN-V7](04-ownership.md#44-views-d16), OWN-B3) | error | | | |
| `go f(&local)` | error | | | |
| `defer` borrowing a dying variable | error | | | |
| view/pointer used after its owner changed or died (UB-1, OWN-B2) | error | | residue only (`unsafe`, C) | |
| borrow stored in a global, a heap container, a channel, an escaping closure or a `go` (OWN-B5) | error | | | |
| writable argument and a borrow of the same place at one call (UB-2, OWN-B4) | error | | | |
| two borrows of overlapping places, one of them writable, including through two pointers (UB-2, OWN-B9) | error | | | |
| write through a `Shared` handle whose `T` is not a cell type (OWN-B9) | error | | | |
| shared handle released by callee (`s.fail(s.err)`, UB-2b, OWN-B4, OWN-B9) | error | | | |
| write to a borrowed `defer` argument (UB-2c) | error | | | |
| `v` reassigned while a type-assertion binding borrows its box (UB-2d) | error | | | |
| ranged slice or owned string changed in the loop body (UB-2e) | error | | | |
| borrowed range variable used after its element was written (UB-2a) | error | | | |
| data race in normal code (UB-3, CON-12 to CON-19) | error | | | residue only |
| data race in `unsafe`, C or C++ code (UB-3 residue) | | | | yes |
| opposite lock order, or a lock taken twice with the guard live (CON-20) | error when visible | | | |
| logic race, or a deadlock the compiler cannot see (CON-21) | | | | no |
| `longjmp` (UB-4) | | | maybe | |
| arena reset with live data (UB-5, OWN-B1) | error | | residue only | |
| C keeps a pointer (UB-9) | | known list | maybe | |
| C writes NULL into a Go++ pointer, a func, an interface with methods or a never-nil `@Required` / `@Strict` field (UB-9) | | | no (a NULL read crashes like any NULL dereference) | |

## 16.5 Rationale

- The owner chose (2026-10-28, ADR-0021) a borrow check without lifetime annotations: bounds checks, nil checks, zero values, use-after-move and use-after-change errors, and sanitizers one flag away for the residue. It is conservative, so a `.Clone()` is the usual fix for a false error.
- The owner chose (2026-10-28, ADR-0021 amendment) that the compiler reads function bodies to see what a result borrows (no lifetime syntax), and that only one writable borrow of a place may be live at a time (Rust's `&mut` rule, `Shared` read-only). So hidden aliasing is a compile error, and what is left undefined is `unsafe`, C and C++, data races there, and `longjmp` / forced unwinding.
- Keeping Go's results for overflow, division and shifts means ported code behaves the same, even though LLVM would allow UB there.
- The owner chose (2026-10-28, ADR-0022) that data races are compile errors in normal code, with the automatic labels `Send` and `Sync`, mutexes that hold their data, and scoped goroutines. Defining races would cost every shared access, so the residue (`unsafe`, C and C++ threads) stays UB and `-race` stays one flag away.
