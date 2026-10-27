# 16. Undefined and implementation-defined behavior

Base: Go spec [Run-time panics](https://go.dev/ref/spec#Run_time_panics), [Integer overflow](https://go.dev/ref/spec#Integer_overflow), [Arithmetic operators](https://go.dev/ref/spec#Arithmetic_operators), [Conversions](https://go.dev/ref/spec#Conversions), [Package unsafe](https://go.dev/ref/spec#Package_unsafe), [Size and alignment guarantees](https://go.dev/ref/spec#Size_and_alignment_guarantees).

Go++ has no borrow checker (D1.4 rule 8). This chapter is the complete list of what the compiler does **not** protect you from, and of the places where the result depends on the implementation.

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
| bounds and nil dereference | panic (checks are on) |
| use after move | compile error, not UB |

## 16.2 Undefined behavior

**UB-1.** *Dangling refs and raw pointers.* Using a `ref string`, `ref []T` or `*T` after its owner is destroyed or has been changed so the memory moved. This includes:
- a ref or pointer stored past the end of the owner's block;
- a ref into a slice element after `append` reallocated the slice;
- a ref into a `string` after the string was reassigned, appended to or dropped;
- a ref into a map key after the map grew;
- a pointer to a `Unique[T]` target after the `Unique` was moved from, reassigned or dropped;
- the compiler catches only the closed list of D1.4 rule 8: returning `&local`, returning a `ref` into a local ([OWN-V7](04-ownership.md#44-refs-d16)), storing a borrow of a temporary, a `go` that borrows something that dies first, a `defer` that borrows something that dies before the function returns, and use after move. A ref or address written directly in a `defer` (`&x`) extends `x` ([PAN-3](09-errors-and-panics.md)), so it is not an error. A pointer stored earlier does not extend its target, and that case is UB-1. Refs may be stored and returned at the programmer's risk ([OWN-V5](04-ownership.md#44-refs-d16)); less obvious cases, such as a ref that comes out of a call, are **not** caught.

```go
func trimmed(s string) ref string { return s[1:] }   // ok: a ref into a borrowed parameter

func bad(name string) ref string {
    s := "abc" + name
    return trimmed(s)       // NOT caught: the ref outlives s. UB when the result is used
}
// func direct(name string) ref string { s := "abc" + name; return s[:2] }   // compile error (OWN-V7)

var keep ref string
func worse(items []string) {
    t := strings.ToUpper(items[0])
    keep = t[:1]            // NOT caught: keep outlives t. UB when used after worse returns
}
```

**UB-2.** *Borrowed parameter whose owner the callee frees through another path.* A callee gets a borrowed parameter and then destroys or changes the owner through a different route. Not caught.

```go
func (u *User) SetName(n string) {
    u.Name = n.Clone()                  // right side first; then the old u.Name is dropped (STM-1)
    u.Audit = "renamed to " + n        // n still points into the dropped old u.Name: UB
}
u.SetName(u.Name)       // n is a borrow of u.Name, which the callee frees through u
```

Fix: `u.SetName(u.Name.Clone())`.

**UB-2a.** *Using a borrowed range variable after its element was written.* In `for _, s := range ss` over owning elements, `s` is a borrowed variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) that borrows the element. Writing the current element or entry is itself defined (the old value is dropped, [RNG-3](08-slices-and-range.md)). Using `s` **after** the write reads the dropped value. When the compiler can see both the write and the later use in the body, it is a compile error. When the write happens where the compiler cannot see it (inside a call, through an alias), it is UB. Not caught.

```go
for _, s := range ss {      // ss is []string: s borrows ss[0]
    ss[0] = "z"             // defined: the old string is dropped
    fmt.Println(s)          // visible write then use: compile error
}
for _, s := range ss {
    reset(ss)               // writes ss[0] inside the call: not visible
    fmt.Println(s)          // UB: s dangles
}
```

Trivial elements are copies ([RNG-1](08-slices-and-range.md)), so the same code on `[]int` is defined and prints the old value, as in Go. An `&` range variable is an alias of the slot ([RNG-1a](08-slices-and-range.md)), so it sees the new value and is not affected.

The same holds for a range over an array with owning elements (the range variable borrows the live element, [RNG-6](08-slices-and-range.md)).

Also UB-2a, when not visible to the compiler: `m[k] = x` on the entry the loop is on (the old value that `v` borrows is dropped) and `copy(dst, …)` into the ranged slice, followed by a use of the range variable.

Not UB: `delete(m, k)`, `move(m[k])` and `v, ok := move(m[k])` of the entry the loop is on (the key, and for `delete` the value, are dropped at the end of the iteration; after `move(m[k])` a borrowed `v` reads the zero value, not the moved-out value, [RNG-3](08-slices-and-range.md)). UB: keeping a ref or pointer into any removed entry past its iteration or past the removal.

**UB-2b.** *Shared handle released through another path.* A shared-handle parameter is passed without a retain ([PAR-7](05-parameters.md)). If the callee drops the caller's last reference through another path and then uses the parameter, the value is freed. Not caught.

```go
func (s *Server) fail(err error) {
    s.err = nil                // drops the last reference to the error
    log.Println(err)           // UB: err was a borrow of s.err, not retained
}
s.fail(s.err)

func run(reg *Registry, cb func()) { reg.cb = nil; cb() }   // same with run(reg, reg.cb)
```

Fix: copy the handle first (`e := err` retains), or pass it as a retained copy.

**UB-2c.** *Writes to a borrowed `defer` argument through an alias the compiler cannot see* change what the deferred call reads, where Go would have copied the value ([CLO-7](11-closures.md)).

**UB-2d.** *Interface reassigned while a binding borrows its box.* After `s, ok := v.(string)` or `switch x := v.(type)`, the binding `s`/`x` borrows the box ([TYP-9](03-types.md)). If `v` is reassigned or cleared through a path the compiler cannot see (another goroutine is a race, UB-3; an alias `*p = nil` where `p` points to `v`; a callee that sets the field `v` lives in) while `s` is still used, the box may be freed and `s` dangles. A visible assignment to `v` while `s` is used afterwards is a compile error.

**UB-2e.** *Ranged slice or string reallocated where the compiler cannot see it.* `range` takes `(ptr, len)` once ([RNG-3a](08-slices-and-range.md)). If a call in the body appends to, reassigns or re-slices the ranged slice (`for _, v := range w.queue { w.push(10) }`), the old buffer is freed and the loop reads freed memory, even for a slice of `int`. The same holds for an owned string that a call in the body grows (`+=`) or reassigns. Go keeps the old array alive, so the same code is safe there. Not caught (visible cases are compile errors, [RNG-3](08-slices-and-range.md)).

**UB-3.** *Data races.* Two goroutines access the same memory, at least one writes, and there is no happens-before edge between them ([CON-7](12-concurrency.md), [Go memory model](https://go.dev/ref/mem)). Unlike Go, this is UB. `-race` (TSan) detects them.

**UB-4.** *`longjmp` across Go++ frames.* A C `longjmp` that jumps over Go++ frames skips destructors and deferred calls. UB ([PAN-9](09-errors-and-panics.md)). Also a C++ exception or a Go++ panic thrown through a frame marked `nounwind` by mistake.

**UB-5.** *Allocator lifetime.* Resetting or freeing a `mem.Arena` while slices, maps or raw pointers allocated in it are alive leaves them dangling ([SLC-3](08-slices-and-range.md), P05 D5.3).

**UB-6.** *`@NoBounds`.* An out-of-range index in a function (or package) with bounds checks turned off.

UB-7 to UB-12 are not listed in D1.17 or D1.4 rule 8. They are collected from P05, P07, P08 and P12 (arena lifetime, `unsafe`, C lifetimes, C++ objects, runtime objects, forced unwinding). The owner confirmed in P01.15 that they stay listed as undefined behavior.

**UB-7.** *`unsafe` and raw memory.* Misuse of `unsafe.Pointer`: wrong type, wrong size, misaligned access, reading uninitialized memory.

**UB-8.** *Pinned values.* Using a pointer or ref into a local after the local has been moved ([OWN-R10](04-ownership.md#42-ownership-rules-d14)); `gopp vet` reports the case where the address was stored, but not every case.

**UB-9.** *C interop lifetimes.*
- Passing a Go++ string or slice to a C function that keeps the pointer after the call (`openlog`, `putenv`, `sqlite3_bind_text` with `SQLITE_STATIC`). `gopp vet` has a list of known functions.
- Using the result of `ffi.String(p)` (a ref over C memory) after the C memory is freed.
- Passing a `ref string` temporary copy to a callee that keeps the pointer (FOR-6).

**UB-10.** *Misuse of C++ objects.* Using a C++ object after a move except in ways its class allows; calling through a dangling reference returned as `*T` (D8.7).

**UB-11.** *Concurrent use of unsynchronized runtime objects.* A map written while another goroutine reads or ranges over it is a data race (UB-3). The "map insert during iteration" panic ([RNG-4](08-slices-and-range.md)) is a best-effort check for single-goroutine misuse, not for races.

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

## 16.4 What catches what

| Case | Compiler | `gopp vet` | `-asan` | `-race` |
|---|---|---|---|---|
| use after move | error | | | |
| returning `&local` | error | | | |
| returning a `ref` into a local of the same function ([OWN-V7](04-ownership.md#44-refs-d16)); a ref that comes out of a call is not caught (UB-1) | error | | yes, for the uncaught cases | |
| `go f(&local)` | error | | | |
| `defer` borrowing a dying variable | error | | | |
| ref/pointer kept past owner (UB-1) | not caught (only the D1.4 rule 8 list) | some | yes | |
| borrowed parameter freed by callee (UB-2) | | | yes | |
| shared handle released by callee through another path (UB-2b) | | | yes | |
| hidden-alias write to a borrowed `defer` argument (UB-2c) | | | only when the write frees memory | |
| `v` reassigned while a type-assertion binding still borrows its box, write not visible (UB-2d; a visible write is a compile error) | | | yes | |
| ranged slice or owned string reallocated by a call in the loop body (UB-2e; visible cases are compile errors) | | | yes | |
| borrowed range variable (slice, array or map) used after its element was written (UB-2a; a visible write then use is a compile error) | write and use both visible | | yes | |
| ref/pointer into a removed map entry, or `m[k] = x` / `copy` over a borrowed element (UB-2a; removing the current entry with `delete` or `move(m[k])` is defined; a borrowed `v` then reads the old value or the zero value respectively) | | | yes | |
| data race (UB-3) | | | | yes |
| `longjmp` (UB-4) | | | maybe | |
| arena reset with live data (UB-5) | | | yes | |
| C keeps a pointer (UB-9) | | known list | maybe | |

## 16.5 Rationale

- v1 chose no extra safety layer beyond Go's defaults: bounds checks, nil checks, zero values, use-after-move errors, and sanitizers one flag away. A borrow checker costs compile speed and simplicity, and can be reconsidered after v1.
- Keeping Go's results for overflow, division and shifts means ported code behaves the same, even though LLVM would allow UB there.
- Races are UB because defining them would cost every shared access.
