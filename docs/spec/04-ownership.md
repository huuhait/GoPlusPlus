# 4. Ownership

> **Being rewritten.** [ADR-0041](../adr/0041-values-are-copied.md) (2026-11-11) replaces the shared-handle model of slices and maps: a value is a copy, moving is `move(x)`, sharing is a pointer or `Shared[T]`. Rules below that speak of shared slices or maps, reference counts on their buffers, or "unshared" describe the earlier model.

Base: Go spec [Variables](https://go.dev/ref/spec#Variables), [The zero value](https://go.dev/ref/spec#The_zero_value), [Assignability](https://go.dev/ref/spec#Assignability), [Assignment statements](https://go.dev/ref/spec#Assignment_statements), [Pointer types](https://go.dev/ref/spec#Pointer_types), [Address operators](https://go.dev/ref/spec#Address_operators), [Allocation](https://go.dev/ref/spec#Allocation), [Making slices, maps and channels](https://go.dev/ref/spec#Making_slices_maps_and_channels).

This is the core of Go++ ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)). Go++ has **no garbage collector**:
- Everyday values (`string`, `[]T`, `map[K]V`, channels, interfaces, funcs and ordinary structs) use **reference-counted backing buffers** with **compile-time borrow lending and last-use auto-move**, so ordinary code reads and behaves like Go while freeing memory deterministically with zero GC pauses.
- Heap pointer ownership is spelled explicitly (`Unique[T]`, `Shared[T]`, `Weak[T]`), and raw pointers `*T` (`&x`) are checked by the **borrow engine** so they never dangle or alias unsafely.

Contents: [at a glance](#at-a-glance-pointers-and-borrows) · [4.1 categories](#41-type-categories-d13) · [4.2 rules 1 to 10](#42-ownership-rules-d14) · [4.3 pointers and heap](#43-pointers-and-heap-allocation-d17) · [4.4 slicing and shared buffers](#44-slicing-and-shared-buffers-d16-adr-0032) · [4.5 borrow checking](#45-borrow-checking-adr-0021) · [4.6 rationale](#46-rationale)

## At a glance: pointers and borrows

Two questions decide which pointer you use: **do I own it on the heap?** and **one owner or many?** A pointer, a func and an interface with methods are never nil (`error` and `any` keep Go's `nil`); "maybe nothing" is written `Option[T]` ([ADR-0018](../adr/0018-never-nil-pointers-and-option.md)).

| I want to… | Write | Owns? | Rule |
|---|---|---|---|
| read or pass an ordinary value | a normal parameter `func show(u User)`; lent without retaining, or copied if trivial (`int`) | lent | [PAR-1](05-parameters.md) |
| change the caller's variable in place | `func rename(u *User)`, called as `rename(&u)` | no (borrow) | [OWN-P1](#43-pointers-and-heap-allocation-d17) |
| slice part of a string or slice | `s[i:j]` gives a `string` or `[]T` sharing the backing buffer in $O(1)$, no byte copy | shares buffer | [OWN-S1](#44-slicing-and-shared-buffers-d16-adr-0032) |
| change slice elements or map entries in a function | `func fill(xs []int, m map[string]int)` — shared header, as in Go | shares buffer | [PAR-1](05-parameters.md), [SLC-1](08-slices-and-range.md) |
| change every element in a loop | `for _, &u := range users { u.Age++ }` | no (borrow) | [RNG-1a](08-slices-and-range.md) |
| own a heap value, alone | `Unique[User]`, made with `NewUnique(User{…})`; auto-moves on transfer, freed when its owner ends | yes (1 owner) | [OWN-P2](#43-pointers-and-heap-allocation-d17) |
| share a heap value | `Shared[User]`, made with `NewShared(User{…})`; writable in 1 goroutine (`u.Name = "x"`), freed when the last reference ends; across goroutines wrap mutable state in `Mutex` / `Atomic` ([CON-19](12-concurrency.md)) | yes, together | [OWN-P8](#43-pointers-and-heap-allocation-d17) |
| point at a shared value without keeping it alive | `Weak[User]`, made with `NewWeak(s)`; read it with `if p := w.Lock(); p != nil { … }` | no | [OWN-P3, P5](#43-pointers-and-heap-allocation-d17) |
| say "maybe nothing" | `Option[*User]`, `Option[int]`, `Option[Logger]`, `Option[func()]`, …; check with `o != nil`, `!o.IsNil()` or `v, ok := o.Get()` | — | [OWN-P9, P10](#43-pointers-and-heap-allocation-d17) |
| make an independent deep copy | `u.Clone()` (short form `clone(u)`) | yes (new copy) | [OWN-R6](#42-ownership-rules-d14) |
| take a value out of a place and leave zero / replacement | `mem.Take(&place)` or `mem.Replace(&place, next)` | yes | [OWN-R4](#42-ownership-rules-d14) |

```go
func show(u User)            { fmt.Println(u.Name) }   // lent: 0 copies, 0 refcount ops
func rename(u *User)         { u.Name = "amy" }        // *T: changes the caller's User
first := name[0:3]                                     // string: shares backing buffer, no byte copy
for _, &u := range users     { u.Age++ }               // & range: changes each element in place
owner := NewUnique(User{Name: "bob"})                  // Unique: single owner, freed when owner ends
cfg   := NewShared(Config{})                           // Shared: freed when the last reference ends
var who Option[*User]                                  // Option: nil until set
if who != nil { rename(who) }                          // checked: who is a *User here
back  := NewWeak(cfg)                                  // Weak: doesn't keep cfg alive
if c := back.Lock(); c != nil { fmt.Println(c.Name) }  // the only way to read through a Weak
```

## 4.1 Type categories (D1.3)

Every type is in exactly one of three categories ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)).

| Category | Types | Assignment & passing | End of life |
|---|---|---|---|
| Trivial (`Copy`) | `bool`, numbers, raw `*T`, `unsafe.Pointer`, arrays/structs of trivial types | implicit bitwise copy | nothing |
| Shared / value handle | `string`, `[]T` and `map[K]V` (when `T`, `K`, `V` are not single-owner), `Shared[T]`, `Weak[T]`, `chan T`, interface values (including `error`), `func` values, arrays/structs/`Option`s whose non-trivial parts are all shared/value handles | implicit (Go semantics): parameters lend (`0` retain); last-use local transfers auto-move (`0` retain); other copies retain | release; last strong reference frees immediately |
| Single-owner | `Unique[T]`, types with a `drop` method, `@Pinned` types, non-copyable C++ classes, and arrays/structs/`[]T`/`map[K]V`/`Option`s containing a single-owner part | auto-moves on by-value transfer; `*T` borrows; `x.Clone()` makes an explicit copy when supported | `drop` / destructor and field drop glue run |

**OWN-C1.** *Trivial (`Copy`).* A trivial value is copied bitwise with `=`, as in Go, and has no destructor. A trivial type satisfies the `Copy` constraint ([GEN-1](10-generics.md)). Examples: `int`, `float64`, `*Node`, `struct{ X, Y int }`. Copying a *writable* `*T` (or an interface or struct that holds one) is a reborrow, not a free alias: the source may not be used while the copy is live ([OWN-B9](#45-borrow-checking-adr-0021) rule 3).

**OWN-C2.** *Shared / value handle.* Everyday heap-backed Go types (`string`, `[]T`, `map[K]V` with non-single-owner elements, `Shared[T]`, `Weak[T]`, `chan T`, interfaces, `func` values, and structs/arrays of them) are reference-counted handles:
- **`string`** holds immutable bytes in a reference-counted backing buffer (or static storage for string literals, where retain/release is a no-op).
- **`[]T`** and **`map[K]V`** use Go-style shared headers: copying the header or passing it to a function shares the underlying elements or entries, so `xs[0] = v` and `m[k] = v` are visible through every copy of that header, as in Go. Use `x.Clone()` when an independent copy is wanted ([OWN-R6](#42-ownership-rules-d14)).
- **Borrow lending and last-use move elision.** Passing an `OWN-C2` value to a parameter lends it without retaining ([PAR-1](05-parameters.md)), and transferring a local on its last read moves ownership without retaining or releasing ([OWN-R3](#42-ownership-rules-d14)). Only values kept in two live places at once increment and decrement the reference count. When the strong count reaches zero, the backing storage and its elements are destroyed and freed immediately.

**OWN-C3.** *Single-owner.* Types that represent unique ownership (`Unique[T]`, types with a `drop` method such as `os.File`, `@Pinned` types, C++ classes with non-trivial move/destroy, and composites containing them) have **one owner at a time**. By-value assignment, return, composite literal construction, channel send, or passing to a by-value parameter moves the value automatically; any later read of the moved-from variable is a compile error (`E0451`, use after move) until reassigned. Borrowing uses `*T` (`&x`, or implicit conversion of `Unique[T]` to `*T` per [OWN-P6](#43-pointers-and-heap-allocation-d17)).

**OWN-C4.** *Structural.* The category of an array, struct, slice, map or `Option[T]` is computed from its parts:
- any single-owner (`OWN-C3`) field or element makes the composite single-owner (`OWN-C3`);
- otherwise, any shared/value handle (`OWN-C2`) field or element (or being a `string`, `[]T`, `map[K]V`, `chan`, `interface`, `func`, `Shared` or `Weak` itself) makes it `OWN-C2`;
- otherwise it is trivial (`OWN-C1`).
A `drop` method or `@Pinned` attribute always makes a type single-owner (`OWN-C3`). `Option[*T]` is trivial, `Option[string]` and `Option[User]` (with `User` holding strings/slices) are `OWN-C2`, and `Option[Unique[T]]` is `OWN-C3`.

**OWN-C5.** *Type parameters.* Inside a generic body, an unconstrained type parameter `T` may be instantiated with any category (`OWN-C1`, `OWN-C2` or `OWN-C3`), so the body is checked conservatively as potentially `OWN-C3` unless constrained by `Copy` or `Clone` ([GEN-1, GEN-2, GEN-3](10-generics.md)). After monomorphization each instance uses the real category of its type argument.

```go
type P struct{ X, Y int }                    // trivial (OWN-C1)
type Doc struct{ Title string }              // shared/value handle (OWN-C2): works like Go
type Tree struct{ Sub Option[Unique[Tree]] } // single-owner (OWN-C3): 1 owner, auto-moves
```

*Why (C1 to C5):* putting `string`, `[]T`, `map[K]V` and ordinary structs into `OWN-C2` makes everyday Go++ code look and behave like Go, while `OWN-C3` preserves strict single ownership where the programmer explicitly asked for `Unique[T]` or a custom `drop()` resource.

## 4.2 Ownership rules (D1.4)

Syntax: no new keywords. `clone(x)` is a predeclared short form for `x.Clone()` ([EXP-1](06-expressions.md), [OWN-R6](#42-ownership-rules-d14)); `NewUnique`, `NewShared` and `NewWeak` allocate heap pointers ([4.3](#43-pointers-and-heap-allocation-d17)); `mem.Take` and `mem.Replace` extract values from addressable places.

**OWN-R1.** *Deterministic destruction.* Every non-trivial value is destroyed deterministically without a tracing GC:
- A single-owner (`OWN-C3`) value is destroyed when its owner's life ends:
  1. its `drop` method runs, if any;
  2. then its non-trivial fields are destroyed, in **reverse declaration order**.
- A shared/value handle (`OWN-C2`) releases its reference when its holder's life ends; when the last strong reference is released, its elements/fields are destroyed and its heap block is freed immediately.

```go
type Conn struct {
    fdPlusOne int
    buf       []byte          // declared second
    log       Logger          // declared third
}
func (c *Conn) drop() {
    if c.fdPlusOne != 0 { println("closing", c.fdPlusOne-1) } // runs first
}
// then c.log is released, then c.buf (reverse declaration order)
```

**OWN-R2.** *Destruction order.*
- Locals are destroyed (or released) at the end of their block, in reverse declaration order. One exception: a block-scoped local that an executed `defer` uses lives until function exit ([PAN-3, PAN-4](09-errors-and-panics.md)).
- Temporaries are destroyed at the end of the full statement.
- In the header of a `for … range`, `if` or `switch`, temporaries live until the end of the whole statement, body included.
- A value of a type with parents ([TYP-16](03-types.md#35-inheritance-adr-0023)) is destroyed child first: the child's `drop` method (if any), then the child's own fields in reverse declaration order, then the parent subobjects in reverse `@Extends` order (`Pn` down to `P1`, each running its `drop`, its fields in reverse, and its own parents in reverse) ([TYP-24](03-types.md#35-inheritance-adr-0023), [ADR-0023](../adr/0023-inheritance.md)). Destroying a `Unique[Parent]` that holds a `Child` destroys the whole `Child`.

**OWN-R3.** *Last-use auto-move and sharing.*
- **For `OWN-C2` values (`string`, `[]T`, `map[K]V`, `Shared[T]`, ordinary structs):**
  - If a local variable has **no later read** on any path after an assignment, return, composite literal, `append`, channel send or consuming call, the compiler **moves** the handle directly with **zero reference-count changes**.
  - If the variable **is** read again later (or read from a struct field, slice element or map entry that stays alive), the destination **retains** a reference automatically. Both bindings stay valid, as in Go.
- **For `OWN-C3` single-owner values (`Unique[T]`, `drop` types):**
  - A by-value transfer of a local (`b := a`, `x.f = a`, passing `a` to a `T` parameter, `return a`, `ch <- a`, `T{F: a}`, `xs = append(xs, a)`) **moves** `a`.
  - Any later read of `a` before `a` is reassigned is a compile error (`E0451`, use after move). Fix-its: `a.Clone()` (if cloneable), pass `&a` to a `*T` parameter to borrow, or use `Shared[T]`.
- *Types with parents.* A child and its parent parts are one value. Taking only a parent part out of a child (`var p Payment = card`) is value slicing and is a compile error ([TYP-21](03-types.md#35-inheritance-adr-0023)). Moving a `Unique[Child]` into a `Unique[Parent]` place is an upcast and moves the pointer ([EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023)).

```go
// OWN-C2 (string, slice, ordinary struct): works like Go
name := readLine()
users.Add(name)          // if `name` is unused below -> auto-moved (0 refcount ops)
fmt.Println(name)        // if `name` IS used here -> retained automatically; no .Clone() needed

// OWN-C3 (Unique[T], File): strict single owner, auto-moves on transfer
n := NewUnique(Node{})
root := Node{Left: n}    // n auto-moves into root.Left
// check(n)              // compile error E0451: n used after move
n = NewUnique(Node{})    // reassigned: usable again
```

**OWN-R3a.** *Reading from non-local places.* "Non-local places" are struct fields, slice/array elements, map entries, package variables, pointer targets, and variables captured by closures ([CLO-9](11-closures.md)).
- For `OWN-C1` (trivial) and `OWN-C2` (shared/value handle) types, reading from any place works just like Go: operand uses lend where safe, and storing into a new owner retains the handle (`d.Title = other.Title`, `return items[0]`, `counts[w]++` all work with no `.Clone()`).
- For `OWN-C3` (single-owner) types:
  - Borrowing a place (`check(node.Left)` where `check` takes `Option[*Node]`, or calling a `*T` method `f.Read(buf)`) borrows in place without moving.
  - Taking ownership out of a non-local place with `b := node.Left` is a compile error (`E0253`), because moving silently out of a struct field or slice element would leave an unexpected zero/empty slot behind. Fix-its: `node.Left.Clone()` (to copy), `mem.Take(&node.Left)` (to take and leave the zero value, when the type has one), or `mem.Replace(&node.Left, next)` (for any type, including `@Required` / never-nil fields).
  - Swapping places in one parallel assignment (`xs[i], xs[j] = xs[j], xs[i]`, `c.A, c.B = c.B, c.A`) is allowed for every movable type, including `OWN-C3` types with no zero value: all index and pointer operands and their bounds checks are evaluated before any bytes move.

**OWN-R3b.** *Read-only bindings (interface assertions and plain range variables).*
Two bindings view an existing value without copying a struct slot:
1. **Plain `range` variables over structs/arrays (`for _, d := range docs`):** for `OWN-C1` and `OWN-C2` elements, `d` behaves like a read-only local value of the element (top-level assignments `d.Title = "x"` and `&d` are compile errors so `d.Title = "x"` never silently mutates a discarded copy as in Go; fix-it `for _, &d := range docs { d.Title = "x" }`, [RNG-1, RNG-1a](08-slices-and-range.md)). Passing `d` or `d.Title` to functions, storing `out = append(out, d)`, or writing through a shared slice/map field (`d.Tags[0] = "x"`) works as in Go. For `OWN-C3` elements, `d` borrows the element read-only (`&d` in `for _, &d := range xs` gives writable access).
2. **Type-assertion and type-switch bindings (`s, ok := v.(T)`, `switch x := v.(type)`):** an interface box is shared and immutable ([TYP-5, TYP-9](03-types.md)), so data held **by value** inside the box (including slices and maps inside the boxed value) is read-only through the binding; write `m := x.Clone()` to mutate a separate copy, or box a pointer (`*T`, `Unique[T]`).

**OWN-R3c.** *Single-owner locals captured by closures or `defer`.* For an `OWN-C3` local (`File`, `Unique[T]`), moving the local while a closure or pending `defer` still refers to it is a compile error ([CLO-7, CLO-9](11-closures.md), [PAN-3](09-errors-and-panics.md)). For `OWN-C2` locals (`string`, `[]T`, `map[K]V`, ordinary structs), closures share variables via cells ([CLO-2](11-closures.md)) and `defer` arguments retain/snapshot the handle evaluated at the `defer` statement, matching Go.

<a id="own-r3d"></a>
**OWN-R3d.** *Unshared: the only handle.* Three transfers hand a mutable container to a holder that must be the only one: storing it in a kept interface ([TYP-6, TYP-7](03-types.md#32-interfaces-and-runtime-type-information-d112)), sending it on a channel ([STM-7](07-statements.md), [CON-14](12-concurrency.md)) and passing it to `go` ([CON-1](12-concurrency.md)). A mutable container is a `[]T`, a `map[K]V`, a plain interface that is not `Sync`, or a struct, array or `Option` that holds one. The operand of such a transfer must be a temporary, or a local on its **last use** ([OWN-R3](#42-ownership-rules-d14)) that is **unshared**. Both are decided at compile time. No reference count is read to decide them: when the compiler cannot show that a local is unshared, the transfer is a compile error whose fix-it is `.Clone()` ([ADR-0040](../adr/0040-unshared-is-proven-at-compile-time.md)).
- *Unshared.* A local or parameter is unshared at a point when both of these hold on every path that reaches the point:
  1. **Its value is fresh.** It was produced by `make`; by a composite literal; by `.Clone()` or `clone`; by a conversion that allocates (`[]byte(s)`); by `append(x, …)` on the last use of an unshared `x`; by a receive from a channel; by a call whose result is fresh (below); or the variable is a parameter that the function takes (below). A zero value is fresh.
  2. **No second handle was made since.** Since then the variable was not copied into another variable, field, element, map entry, package variable, result or closure while it is read again later (a retaining copy, [OWN-R3](#42-ownership-rules-d14)); no slice expression of it was stored or kept; it was not passed to a parameter that keeps it; and no handle to a container inside it was copied out and kept.
- *All the way down.* A value that holds mutable containers is fresh only if each of them was a temporary or was moved in on the unshared last use of its own variable. Storing a container that is not unshared into the variable (as an element, a field, a map entry or through `append`) makes the variable shared.
- *What does not share.* Lending the variable to a parameter that does not keep it ([PAR-1](05-parameters.md)); reading and writing elements, fields and entries of trivial or immutable type; `len`, `cap` and `range`; a slice expression that is only a lent argument.
- *Once shared,* a variable is unshared again only after it is assigned a fresh value.
- *Across functions.* The compiler infers two facts for every function, with no annotation, and keeps them in the package's export data like the borrow summaries of [OWN-B3](#45-borrow-checking-adr-0021):
  - **a fresh result:** every `return` gives that result a temporary or an unshared local on its last use;
  - **a taken parameter:** the body transfers the parameter where an unshared value is needed, without sharing it first. Inside the function a taken parameter is fresh. A call that passes an argument to a taken parameter is itself such a transfer: the argument must be a temporary or an unshared local on its last use.
- *Calls that cannot be seen through.* A call through an interface or a function value, and a call of an `@Extern` function, has no facts: its results are not fresh and its parameters are not taken. So a function with a taken parameter cannot be used as a function value, as a method value or as the implementation of an interface method. That use is a compile error; the fix-it is to clone inside the function.
- *Run time.* The reference count of an unshared value is 1 at the transfer. A debug build asserts it; no build decides anything by it. The in-place paths of `append`, `s += x` and `s = s[:k]` ([SLC-9](08-slices-and-range.md), [STM-8](07-statements.md)) still read the count: they choose between two ways of doing the same thing, never between accepting and rejecting a program.

```go
ys := []int{1, 2}
go sum(ys)                       // ok: fresh, no other handle, last use

func send(xs []int, ch chan []int) {
    ch <- xs                     // ok: xs becomes a taken parameter of send
}
zs := make([]int, 8)
send(zs, ch)                     // ok: zs is unshared on its last use
keep := make([]int, 8)
send(keep, ch)                   // error: keep is read below; fix-it: send(keep.Clone(), ch)
fmt.Println(keep)

a := cfg.items                   // a handle read from a field: cfg.items holds the buffer too
go work(a)                       // error: a is not unshared; fix-it: go work(a.Clone())

type Sink interface{ Put(xs []int) }
func (q *Queue) Put(xs []int) { q.ch <- xs }   // xs is taken, so *Queue cannot be used as a Sink;
                                               // fix-it: q.ch <- xs.Clone()
```

**OWN-R4.** *In-place updates, `mem.Take` and `mem.Replace`.*
- `p = append(p, …)`, `buf = strconv.AppendInt(buf, n, 10)` and `m[k] = append(m[k], v)` update the destination in place. When the slice's backing buffer has `refcount == 1` and spare capacity, `append` writes directly in place with zero allocation.
- `mem.Take(&place)` returns the current value of `place` and writes the zero value of `T` into `place` (compile error if `T` has no zero value, [OWN-R9](#42-ownership-rules-d14)).
- `mem.Replace(&place, v)` stores `v` into `place` and returns the previous value; it works for every movable type, including never-nil pointers and `@Required` structs.

**OWN-R5.** *Zero values and moved-from state.*
- Every type has Go's zero value, except the types listed in [OWN-R9](#42-ownership-rules-d14) (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`, `func` types, interfaces with methods other than `error`, structs with required fields, C++ classes without a default constructor, and arrays/structs containing them).
- When a Go++ local or `mem.Take` slot is moved from, its storage is set to all-zero bytes, and drop glue is a no-op on all-zero bytes (so no extra boolean drop flags are needed on normal paths). A custom `drop()` method must treat the zero value as a no-op ([OWN-R7](#42-ownership-rules-d14)). Imported C++ classes without zero-state destruction use their C++ move constructor and destruction state tracking ([OWN-R9](#42-ownership-rules-d14)).

**OWN-R6.** *Explicit deep copies: `x.Clone()`.*
- Every cloneable type has `Clone() T` (and short form `clone(x)`): built-in scalars, `string` (copies bytes into a fresh buffer, or returns empty), `[]T` (allocates a fresh backing array in the slice's allocator and clones each element), `map[K]V` (allocates a fresh map in the map's allocator and clones keys and values), `Unique[T]` (allocates a new `Unique[T]` holding `(*u).Clone()`), `Shared[T]` / `Weak[T]` / `chan` / `interface` / `func` (retains the handle), and structs/arrays whose fields are all cloneable.
- Because `b := a` on `[]T` and `map[K]V` shares elements like Go, `b := a.Clone()` is how you ask for a separate, independent backing collection.
- A type with a `drop` method is **not** cloneable unless it defines `Clone() T` by hand (with a value receiver `func (h Handle) Clone() Handle`, whose receiver is borrowed read-only without consuming `h`). This prevents accidental duplication of OS handles.
- *Types with parents.* Auto-`Clone` of a child clones all parent and child fields. For any ancestor `P` with a type table ([TYP-22](03-types.md#35-inheritance-adr-0023)), `p.Clone()` on `*P` or `Unique[P]` returns a `Unique[P]` holding a full copy of the dynamic type ([ADR-0023](../adr/0023-inheritance.md) decision 16).
- On a raw `*T` where `T` is an untagged type with a value-receiver `Clone() T`, `p.Clone()` is a compile error for ambiguity (for a tagged parent type `P`, `p.Clone()` dynamically clones into `Unique[P]` per the rule above): write `(*p).Clone()` to clone `T`, or `q := p` to copy/reborrow the pointer. `gopp vet` warns on redundant `.Clone()` calls on `Shared[T]` and `Weak[T]`.

**OWN-R7.** *Destructors are a method named `drop`* with a pointer receiver: `func (f *File) drop()`. It cannot be called directly (like `init`), and must accept the zero value safely.

```go
// Store fd + 1 so valid POSIX fd 0 (stdin) is non-zero and 0 means "closed / moved-from".
type File struct{ fdPlusOne int }

func (f *File) drop() {
    if f.fdPlusOne != 0 { sys_close(f.fdPlusOne - 1) }
}
```

**OWN-R8.** *Borrow checking without lifetime annotations.* Raw pointers `*T` (`&x`), stack-array slices (`arr[:]`), active `range` loops, arena allocations and mutex guards are checked by flow and liveness ([OWN-B1 to OWN-B9](#45-borrow-checking-adr-0021)). Returning `&local`, resizing a slice while `&xs[i]` is live, passing overlapping `*T` arguments, or sending `&local` to `go` are compile errors. What remains unchecked (`unsafe`, C/C++ raw pointers, `longjmp`) is listed in [chapter 16](16-undefined-behavior.md).

**OWN-R9.** *Types with no zero value and C++ classes.*
- Pointer kinds (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`), `func` types, interface types with methods other than `error`, structs with `@Required` fields, C++ classes without a default constructor, and arrays/structs containing them have **no zero value** ([OWN-P9](#43-pointers-and-heap-allocation-d17)). `Option[T]` always has the zero value `nil`.
- Operations that materialize a zero value (`var x T`, `make([]T, n)` for non-zero `n`, `clear(slice)`, single-value `m[k]` or `<-ch`) are compile errors when `T` has no zero value. Use `make([]T, 0, cap)`, `s = s[:0]`, and comma-ok lookups `v, ok := m[k]` / `v, ok := <-ch` (usable where `ok` is known true, [OWN-P10](#43-pointers-and-heap-allocation-d17)).
- Moving an imported C++ class runs its C++ move constructor and destroys the moved-from source at scope end.

**OWN-R10.** *Pinned values and RVO.*
- Types marked `@Pinned` (the `sync` types, including `Mutex[T]`) cannot be moved or copied after first use: a compile error.
- `return local` constructs the local directly in the caller's result slot when possible so it never moves in memory.

### Example: the whole model on one screen

```go
type Node struct {
    Name     string                 // shared/value handle (OWN-C2)
    Children []Shared[Node]         // writable slice of shared child nodes
    Parent   Option[Weak[Node]]     // back-pointer: does not keep Parent alive
}

func NewNode(name string, parent Option[Weak[Node]]) Shared[Node] {
    return NewShared(Node{Name: name, Parent: parent})
}

func Add(parent Shared[Node], child Shared[Node]) {
    parent.Children = append(parent.Children, child)    // mutates Shared directly in 1 goroutine
}

func main() {
    root := NewNode("root", nil)
    Add(root, NewNode("a", NewWeak(root)))
    b := NewNode("b", NewWeak(root))
    Add(root, b)                                        // b is retained for Children and usable below
    b.Name = "b-renamed"                                // direct field write on Shared[Node]
    if w, ok := b.Parent.Get(); ok {
        if p := w.Lock(); p != nil { fmt.Println(p.Name) }
    }
    head := root.Name[:2]                               // plain string: shares buffer, 0 byte copy
    fmt.Println(head)
}                                                       // counts hit 0: whole tree freed immediately
```

## 4.3 Pointers and heap allocation (D1.7)

Every heap pointer allocation is spelled out. Go's `new` is removed.

| Type | Created with | Meaning |
|---|---|---|
| `*T` | `&x` (address of a variable, field or element) | raw, non-owning tracked borrow, **never nil** (0 malloc, 0 counter) |
| `Unique[T]` | `NewUnique(v)` | single heap owner (`OWN-C3`); dropped and freed when its owner ends; never nil |
| `Shared[T]` | `NewShared(v)` | shared heap ownership (`OWN-C2`); writable in 1 goroutine (non-atomic refcount), `Send + Sync` (atomic refcount) when `T` is synchronized (`Mutex`, `Atomic`, `chan`, OWN-P8); never nil |
| `Weak[T]` | `NewWeak(s)`, from a `Shared[T]` | non-owning back-pointer (`OWN-C2`); `w.Lock()` returns `Option[Shared[T]]`, `nil` if freed |
| `Option[T]` | `nil`, or any `T` (converts automatically) | "maybe nothing", zero value `nil`; the nilable form of pointers, funcs and interfaces with methods |

**OWN-P1.** `*T` is a raw borrowed pointer. `&x` takes the address of an addressable operand (a variable, struct field or slice/array element), as in Go, except that pointers to slice or map types (`*[]T`, `*map[K]V`, including pointers to defined slice or map types) are compile errors everywhere because `[]T` and `map[K]V` are already reference handles. `*T` never allocates and never frees anything.

**OWN-P2.** `NewUnique(v)` and `NewShared(v)` move `v` into a heap allocation and return `Unique[T]` or `Shared[T]`: `NewUnique(Node{Name: "a"})`.

**OWN-P3.** `NewWeak(s)` takes a `Shared[T]` and returns `Weak[T]` without keeping the target alive.

**OWN-P4.** `&T{…}` is a compile error with fix-it: "allocation is explicit in Go++: use `NewUnique(T{…})` or `NewShared(T{…})`".

**OWN-P5.** Field access and method calls **auto-dereference** `*T`, `Unique[T]` and `Shared[T]`. `Weak[T]` does **not** ([ADR-0017](../adr/0017-weak-no-auto-deref.md)): only `w.Lock()` is available on `Weak[T]`. `Unique[T]` and `Shared[T]` get the methods of `T` and `*T` (`Clone` calls the pointer kind's own `Clone`, [OWN-R6](#42-ownership-rules-d14)).

**OWN-P6.** `Unique[T]` and `Shared[T]` stored in a place convert **implicitly** to `*T` (and `Option[*T]`); `Option[Unique[T]]` and `Option[Shared[T]]` places convert to `Option[*T]`. Converting from a temporary is an error because the temporary would die at the end of the statement.

**OWN-P7.** `make([]T, n)`, `make(map[K]V)` and `make(chan T, n)` work as in Go (with the allocator rules of [SLC-2](08-slices-and-range.md)).

**OWN-P8.** `Unique[T]` is single-owner (`OWN-C3`) and grants exclusive writable access. `Shared[T]` and `Weak[T]` are reference-counted handles (`OWN-C2`):
- **Single-goroutine `Shared[T]`:** When `T` has plain writable `OWN-C1` / `OWN-C2` fields (`Node{Name string, Children []Shared[Node]}`), fields and elements are directly readable and writable (`p.Name = "x"`, `p.Children = append(p.Children, c)`) and `Shared[T]` uses a fast non-atomic reference count. Unprotected single-owner (`OWN-C3`) fields (`Unique[U]`, `File`) are not allowed directly inside `Shared[T]` (wrap them in `Mutex[U]`), so overwriting a field through one `Shared[T]` alias can never drop a unique resource while another alias borrows it ([OWN-B9](#45-borrow-checking-adr-0021) rule 5).
- **Multi-goroutine `Shared[T]` (`Send + Sync`):** `Shared[T]` (and `Weak[T]`) can cross goroutines (`go` / `chan`) only when all of `T`'s state is thread-safe synchronized (`Mutex[U]`, `RWMutex[U]`, `Atomic[U]`, `chan U`, or nested structs of them, [CON-19](12-concurrency.md)), in which case `Shared[T]` uses an atomic reference count.

**OWN-P9.** *Never nil, and `Option[T]`* ([ADR-0018](../adr/0018-never-nil-pointers-and-option.md)).
- `*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`, `func` types, and interface types with at least one method (except `error`) are **never nil** and have no zero value.
- `error`, `any` (empty interfaces), `unsafe.Pointer`, slices, maps and channels keep Go's `nil`. Writing to a nil map held in an addressable local variable, struct field, slice element or map entry (`var m map[string]int; m["a"] = 1`) creates the map in place; because `map[K]V` parameters pass by value as 1-word handles ([PAR-1](05-parameters.md)), writing to a nil map parameter allocates only in the callee's local parameter unless returned (passing a known-`nil` map to a void/non-returning function emits a warning). Writing to a temporary map (`f()["a"] = 1`) or a map inside an interface box is a compile error (`E0353` / `E0354`), so Go++ has no run-time nil-map panic.
- `Option[T]` provides `nil` for any type `T` that is not already nilable (`Option[*User]` has the exact 1-word layout of a nullable C pointer; writing `Option[T]` when `T` is already nilable—`[]U`, `map[K]V`, `chan U`, `any`, `error`, `unsafe.Pointer`, or `Option[U]`—is a compile error `E0446`). `Option[T]` has two methods: `IsNil() bool` and `Get() (T, bool)`.

**OWN-P10.** *Flow narrowing on `Option[T]`, `(T, error)` and comma-ok.*
- After `if o != nil` (or early return on `o == nil` / `o.IsNil()`), a local or parameter `o` of type `Option[T]` narrows directly to `T`. Assignment to `o` or `mem.Take(&o)` ends the narrowing.
- When `T` has no zero value, `v, err := f()` makes `v` usable only where `err == nil` is known, and `v, ok := m[k]` / `v, ok := <-ch` / `v, ok := o.Get()` / `v, ok := x.(T)` makes `v` usable only where `ok` is known true.

```go
type Tree struct{ L, R Option[Unique[Tree]]; V int }

func sum(t Option[*Tree]) int {
    if t == nil { return 0 }
    return t.V + sum(t.L) + sum(t.R)   // t is *Tree; t.L borrows as Option[*Tree] (OWN-P6)
}

func build() Unique[Tree] {
    return NewUnique(Tree{V: 1, L: NewUnique(Tree{V: 2})})
}
```

## 4.4 Slicing and shared buffers (D1.6, ADR-0032)

Slicing produces ordinary `string` and `[]T` values backed by reference-counted headers:

**OWN-S1.** *String slicing.* `s[i:j]` on a `string` returns a `string` `{s.ptr + i, j - i, s.backing}` in $O(1)$ with no byte allocation. If `s.backing != nil`, retaining `s.backing` keeps the heap bytes alive for as long as the substring lives; if `s` is a static literal (`backing == nil`), no reference count is touched.

**OWN-S2.** *Slice and array slicing.*
- On a heap-backed `[]T` (`OWN-C2`), `xs[i:j]` returns a `[]T` `{xs.ptr + i*sizeof(T), j - i, xs.cap - i, xs.backing}` in $O(1)$ sharing the same reference-counted backing array and its allocator ([SLC-1](08-slices-and-range.md)). Element writes (`sub[0] = v`) mutate the shared backing array, as in Go. Reassigning or `append`-reallocating `xs` does **not** invalidate `sub`, because `sub` holds its own strong reference to the old backing array.
- On a value array `arr[i:j]` (where `arr` is on the stack or inside a struct), the result has `backing == nil` and `cap == len`, and is tracked by the borrow checker as a **borrow of `arr`** ([OWN-B1](#45-borrow-checking-adr-0021)): it may be passed to **non-keeping** slice parameters (`sort.Ints(arr[:])`, `w.Write(hash[:])`) and mutated in place with `0` heap allocation, is a compile error when returned, stored, or passed to a **keeping** parameter or indirect `func([]T)` (`use arr[:].Clone()`), and `append(arr[:], x)` allocates a fresh heap backing array because `len == cap`.
- On a single-owner slice (`[]Unique[T]`, `OWN-C3`), slicing `xs[i:j]` produces a non-escaping borrowed slice tracked under [OWN-B1](#45-borrow-checking-adr-0021) (allowed for non-keeping slice parameters, rejected on keeping or indirect calls) so unique elements cannot have two owners.

**OWN-S3.** *Map keys.* Looking up or updating `m[k]` borrows `k` for the lookup (`0` retains if `k` already exists in `m`), and retains `k` only when inserting a new key.

<a id="45-borrow-checking-adr-0021"></a>
## 4.5 Borrow checking (ADR-0021, ADR-0032)

Go++ has no lifetime annotations. Plain value code (`T`, `string`, `[]T`, `map[K]V`) needs no borrow checking at the call site because heap buffers are reference-counted. The borrow checker runs on **tracked borrows** (`&x` raw pointers, stack-array slices, borrow-carrying structs/arrays, active `range` loops, arenas, mutex guards and `sync.Scope`) using flow and liveness.

**OWN-B1.** *What is a tracked borrow.* A tracked borrow is any of:
- a raw pointer `*T` made by `&x` (of a local, parameter, struct field or slice/array element), an `&` range alias (`for _, &e := range xs`), or borrowed from a `Unique[T]` / `Shared[T]` place ([OWN-P6](#43-pointers-and-heap-allocation-d17));
- a slice made by slicing a value array (`arr[i:j]`) or a single-owner `OWN-C3` slice ([OWN-S2](#44-slicing-and-shared-buffers-d16-adr-0032));
- a **borrow-carrying struct, array or `Option`** whose fields/elements contain `*T` or a borrowed slice (`Parser{arena *mem.Arena}`, `Node{Left, Right Option[*Node]}`);
- an active `range` loop over a slice, array or map ([RNG-3](08-slices-and-range.md));
- a value allocated from a `mem.Arena` / `mem.DropArena` (it borrows the arena);
- a `Mutex[T]` / `RWMutex[T]` guard (`Lock` / `RLock`, [CON-15](12-concurrency.md)).

An upcast (`*Child` to `*Parent`) is the same borrow of the same place ([EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023)).

**OWN-B2.** *No invalidation while a borrow is live.* While a tracked borrow is **live** (used on some path afterwards), the borrowed storage may not be modified in a way that can free, reallocate or relocate the memory the borrow points into:
- reassigning the owner or a prefix of it while `&x` or `&x.f` is live;
- `append`, `clear` or in-place re-assignment on the same slice variable/path `xs` while `&xs[i]` (or an active `range` over a value array) is live — because when `xs` has `refcount == 1`, `append` may free or reallocate the backing array that `&xs[i]` points into;
- inserting into or clearing a map while an `&v` range alias or `range` loop on that map is live;
- `arena.Reset()`, `arena.Free()`, or ending `arena`'s scope while an arena-allocated pointer, slice or borrow-carrying struct is live (bump-allocating further values via `mem.ArenaNew` or `mem.MakeIn` adds chunk blocks without moving existing chunks, so it does **not** invalidate existing arena borrows; `mem.Arena` and `mem.DropArena` free all chunks automatically when their owning scope ends);
- ending the owner's scope while the borrow escapes (`return &local`).

Note the contrast with Go-style slices and strings when **`&` is NOT used**:
```go
// NO `&`: reference-counted shared buffers — works just like Go!
s := "hello world"
w := s[0:5]
s = "bye"                  // ok: w retains the old "hello world" buffer
println(w)                 // prints "hello"

nums := []int{1, 2, 3}
v := nums[1:3]
nums = append(nums, 4)     // ok: v retains the old [1, 2, 3] backing array
println(v[0])              // prints 2

// WITH `&`: raw pointer *int has no refcount — borrow checker protects it!
p := &nums[0]
nums = append(nums, 5)     // compile error: cannot append to nums while p (&nums[0]) is live
println(*p)
```

**OWN-B3.** *Returning a tracked borrow.*
- Functions returning ordinary values (`string`, heap `[]T`, `User`, `Unique[T]`, `Shared[T]`) return self-contained values and carry **no borrow summary**.
- Only a function returning a raw pointer `*T`, `Option[*T]`, or a borrow-carrying struct/array returns a borrow. The compiler computes which pointer-capable parameters/receiver (such as `arena *mem.Arena` or `p *Parser`) the returned borrow may point into (stored in export data; conservative union for interface/indirect/C calls). Returning `&local` or a borrow into a local variable/array is always a compile error.

**OWN-B4.** *Aliasing at a call.*
- Two writable `*T` arguments (or a `*T` receiver and a `*T` argument) that may point to overlapping places are a compile error (`swap(&list[i], &list[j])` — use `list.Swap(i, j)` or `Split`), except when both point into the same `mem.Arena` / `mem.DropArena` graph (`OWN-B9` rule 5).
- Passing `&x` alongside an `OWN-C2` read from `x` (`u.SetName(u.Name)`) is **allowed and safe**: the compiler retains a temporary reference to `u.Name` before the call so reassigning `u.Name` inside `SetName` cannot free `n` mid-call ([PAR-1](05-parameters.md)). Passing `&x` alongside a tracked borrow into `x` (`both(&u, &u.Age)`) is a compile error ([OWN-B9](#45-borrow-checking-adr-0021)).

**OWN-B5.** *Where tracked borrows may live.* A raw `*T`, stack-array slice `arr[:]`, or borrow-carrying struct/array (`Parser{arena *mem.Arena}`, `Node{Left, Right Option[*Node]}`) may live only in stack local variables, parameters, function results (checked by `OWN-B3`), `Local[I]` interfaces, and allocations inside the same `mem.Arena` / `mem.DropArena` (freed automatically when the parent scope or arena drops/resets). Storing a tracked borrow in `NewUnique`, `NewShared`, a heap slice or map, a channel, a package global, an escaping closure, a kept `Send` interface, or passing `&local` to `go` is a compile error: use `Unique[T]`, `Shared[T]` or `Weak[T]` for heap-owned graphs (or `sync.Scope` for scoped goroutines, [CON-16](12-concurrency.md)).

**OWN-B6.** *What is not checked.* [Chapter 16](16-undefined-behavior.md) lists the residue: `unsafe` and raw memory, C and C++ raw pointers/threads, and `longjmp` / forced unwinding.

**OWN-B7.** *Go mode (`.go` files).* Uses the same memory and borrow rules; predeclared Go++ names (`Option`, `Unique`, `Shared`, `Weak`) are ordinary identifiers there unless imported.

**OWN-B8.** *Borrows and concurrency ([ADR-0022](../adr/0022-race-free-concurrency.md)).*
- A `Mutex[T]` guard borrows the mutex ([CON-15](12-concurrency.md)) and cannot escape.
- `ch <- v` and `go f(v)` require `Send` values ([CON-12 to CON-14](12-concurrency.md)); raw `*T` borrows cannot cross `go` or `chan`.
- Inside `sync.Scope`, goroutines started with `s.Go` finish before the scope exits and may borrow locals safely; disjoint writable slice parts use `Split` ([CON-16](12-concurrency.md)).

**OWN-B9.** *Exclusive writable `*T` access.*
1. **One writable `*T` at a time.** While a writable `*T` borrow of a local/unique place is live, no other `*T` borrow or read of an overlapping place on that same variable may be live.
2. **Disjointness.** Different local variables, different struct fields, different constant indexes (`&list[0]` and `&list[1]`), and the two results of `list.Split(k)` are provably disjoint. Variable indexes `list[i]` and `list[j]` overlap; use `list.Swap(i, j)` or `Split`.
3. **Reborrowing.** `q := p` on a writable `*T` reborrows `p`: `p` cannot be used while `q` is live (`q := p; use(q); use(p)` is fine because `q` is dead before `use(p)`).
4. **No raw `*T` in heap-owned `Unique`/`Shared` data.** Heap-owned structs and collections hold `Unique[T]`, `Shared[T]` or `Weak[T]`, never raw `*T` (`OWN-B5`); borrow-carrying structs live only on the stack or inside a `mem.Arena` whose lifetime bounds every node at once.
5. **`Shared[T]`, `*T` interior slice borrows, and arena graphs** ([OWN-P8](#43-pointers-and-heap-allocation-d17), [CON-19](12-concurrency.md)): `Shared[T]` holds `OWN-C1`/`OWN-C2` fields (or `Mutex`/`RWMutex`/`Atomic` for `OWN-C3` and multi-goroutine state). Taking an interior slice borrow `&ptr.Slice[i]` (through `*U` or `Shared[U]`) retains a temporary `+1` reference on `Slice.backing` (and on `Shared[U]`) for the borrow's lifetime so `b.Slice = append(...)` through another alias detaches `b.Slice` via COW (`refcount >= 2`) instead of freeing the borrowed element. `*T` pointers omit C `restrict` qualifiers when `T` may sit inside `Shared[T]` or a borrow-carrying arena struct, and C23 output compiles with `-fno-strict-aliasing`.
6. **`Unique[T]` grants exclusive writable access** through its single owner.

```go
swapNames(&list[0], &list[1])               // ok: constant indexes, disjoint
a, b := list.Split(k)                       // ok: disjoint slices
swapNames(&a[0], &b[0])
list.Swap(i, j)                             // ok: built-in safe swap

p := &x
q := p                                      // q reborrows p
use(q)
use(p)                                      // ok: q is not used later

s := NewShared(Config{})
s.Count++                                   // ok in 1 goroutine
mu := NewShared(sync.NewMutex(Config{}))    // across goroutines: wrap mutable state in Mutex
go func() { mu.With(func(c *Config) { c.Count++ }) }()
```

## 4.6 Rationale

- **Why shared reference-counted buffers + borrow engine ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md))?** Reference-counted backing buffers with compile-time borrow lending (`0` retains on reads) and last-use auto-move (`0` retains on final transfers) eliminate ~90% of reference-count operations at compile time while keeping Go's exact surface syntax, composable APIs, and deterministic, pause-free memory reclamation.
- **Why keep `Unique[T]`, `Shared[T]` and `Weak[T]`?** Heap pointer ownership stays explicit and self-documenting: `*T` never secretly heap-allocates, `Unique[T]` has zero reference-count overhead and single ownership, `Shared[T]` shares explicitly, and `Weak[T]` breaks parent/child cycles cleanly.
- **Why check `&` with the borrow engine?** Raw pointers `*T` have no reference count and can point into stack locals or the middle of structs and slices (`&nums[0]`, `&u.Age`), where reference counting cannot protect against reallocation or scope exit. Flow-based borrow checking on `*T` catches dangling pointers and aliasing bugs at compile time with zero lifetime annotations.
