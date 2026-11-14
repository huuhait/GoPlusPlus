# 4. Ownership

Base: Go spec [Variables](https://go.dev/ref/spec#Variables), [The zero value](https://go.dev/ref/spec#The_zero_value), [Assignability](https://go.dev/ref/spec#Assignability), [Assignment statements](https://go.dev/ref/spec#Assignment_statements), [Pointer types](https://go.dev/ref/spec#Pointer_types), [Address operators](https://go.dev/ref/spec#Address_operators), [Allocation](https://go.dev/ref/spec#Allocation), [Making slices, maps and channels](https://go.dev/ref/spec#Making_slices_maps_and_channels).

This is the core of Go++ ([ADR-0041](../adr/0041-values-are-copied.md), [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)). Go++ has **no garbage collector**:
- **A value is a copy**, for every type (`int`, `string`, `[]T`, `map[K]V`, ordinary structs): `t := s`, storing, returning and passing an argument each give the destination its own value. A copy the program cannot notice is skipped, `move(x)` hands a value over without copying, and every value is freed deterministically when its owner ends, with zero GC pauses.
- **Sharing is written with a type:** a pointer `*T` (`&x`) or `Shared[T]`. Heap pointer ownership is spelled explicitly (`Unique[T]`, `Shared[T]`, `Weak[T]`), and raw pointers `*T` (`&x`) are checked by the **borrow engine** so they never dangle or alias unsafely.

Contents: [at a glance](#at-a-glance-pointers-and-borrows) · [4.1 categories](#41-type-categories-d13) · [4.2 rules 1 to 10](#42-ownership-rules-d14) · [4.3 pointers and heap](#43-pointers-and-heap-allocation-d17) · [4.4 slicing](#44-slicing-and-shared-buffers-d16-adr-0032) · [4.5 borrow checking](#45-borrow-checking-adr-0021) · [4.6 rationale](#46-rationale)

## At a glance: pointers and borrows

Two questions decide which pointer you use: **do I own it on the heap?** and **one owner or many?** A pointer, a func and an interface with methods are never nil (`error` and `any` keep Go's `nil`); "maybe nothing" is written `Option[T]` ([ADR-0018](../adr/0018-never-nil-pointers-and-option.md)).

| I want to… | Write | Owns? | Rule |
|---|---|---|---|
| read or pass an ordinary value | a normal parameter `func show(u User)`; the function's own copy, and no copy is made when the function only reads it | yes (its copy) | [PAR-1](05-parameters.md) |
| change the caller's variable in place | `func rename(u *User)`, called as `rename(&u)` | no (borrow) | [OWN-P1](#43-pointers-and-heap-allocation-d17) |
| slice part of a string | `s[i:j]` gives a `string` sharing the immutable bytes in $O(1)$, no byte copy | shares bytes | [OWN-S1](#44-slicing-and-shared-buffers-d16-adr-0032) |
| slice part of a slice | `xs[i:j]` is a new `[]T` with copies of those elements; `&xs[i:j]` is a `*[]T` view of the elements themselves | `xs[i:j]`: yes (its copy); `&xs[i:j]`: no (borrow) | [OWN-S2](#44-slicing-and-shared-buffers-d16-adr-0032) |
| change slice elements or map entries in a function | `func fill(xs *[]int, m *map[string]int)`, called as `fill(&xs, &m)`; inside, `xs[0] = 1` and `m["a"] = 2` need no star | no (borrow) | [OWN-P1, P5](#43-pointers-and-heap-allocation-d17) |
| change every element in a loop | `for _, &u := range users { u.Age++ }` | no (borrow) | [RNG-1a](08-slices-and-range.md) |
| hand a value over without copying | `move(x)`; `x` cannot be read again until it is assigned | yes | [OWN-R3](#42-ownership-rules-d14) |
| own a heap value, alone | `Unique[User]`, made with `NewUnique(User{…})`; freed when its owner ends; a copy allocates a new one | yes (1 owner) | [OWN-P2](#43-pointers-and-heap-allocation-d17) |
| share a heap value | `Shared[User]`, made with `NewShared(User{…})`; every holder can read it, nobody changes it directly; a part that changes is in a `Mutex` / `Atomic` ([ADR-0042](../adr/0042-shared-is-read-only.md)); freed when the last reference ends | yes, together | [OWN-P8](#43-pointers-and-heap-allocation-d17) |
| point at a shared value without keeping it alive | `Weak[User]`, made with `NewWeak(s)`; read it with `if p := w.Lock(); p != nil { … }` | no | [OWN-P3, P5](#43-pointers-and-heap-allocation-d17) |
| say "maybe nothing" | `Option[*User]`, `Option[int]`, `Option[Logger]`, `Option[func()]`, …; check with `o != nil`, `!o.IsNil()` or `v, ok := o.Get()` | — | [OWN-P9, P10](#43-pointers-and-heap-allocation-d17) |
| make an independent copy | `t := u`; `*p` is the copy of what a pointer `p` points to | yes (new copy) | [OWN-R3, R6](#42-ownership-rules-d14) |
| take a value out of a place and leave zero / replacement | `mem.Take(&place)` or `mem.Replace(&place, next)` | yes | [OWN-R4](#42-ownership-rules-d14) |

```go
func show(u User)            { fmt.Println(u.Name) }   // only reads u: no copy is made
func rename(u *User)         { u.Name = "amy" }        // *T: changes the caller's User
func fill(xs *[]int)         { xs[0] = 1 }             // *[]T: changes the caller's slice; called as fill(&nums)
kept := move(nums)                                     // move: no copy; nums cannot be read until assigned again
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

Every type is in exactly one of three categories ([ADR-0041](../adr/0041-values-are-copied.md)).

| Category | Types | Assignment & passing | End of life |
|---|---|---|---|
| Trivial (`Copy`) | `bool`, numbers, raw `*T`, `unsafe.Pointer`, arrays/structs of trivial types | implicit bitwise copy | nothing |
| Copyable | `string`; `[]T`, `map[K]V` and `Unique[T]` (when `T`, `K`, `V` can be copied); `Shared[T]`, `Weak[T]`, `chan T`, interface values (including `error`), `func` values; types with a `drop` method that define `Clone() T`; arrays/structs/`Option`s whose parts can all be copied and are not all trivial | a copy by the type's own rule; no copy is made on the last use of the source or for a parameter that is only read; `move(x)` moves | destroyed with its owner: a slice or map frees its buffer, a counted handle is released (the last one frees immediately) |
| Cannot be copied | types with a `drop` method and no `Clone() T`, `@Pinned` types, C++ classes without a usable copy constructor, and arrays/structs/`[]T`/`map[K]V`/`Unique[T]`/`Option`s containing a part that cannot be copied | a plain copy is a compile error; `move(x)` moves; `*T` borrows | `drop` / destructor and field drop glue run |

**OWN-C1.** *Trivial (`Copy`).* A trivial value is copied bitwise with `=`, as in Go, and has no destructor. A trivial type satisfies the `Copy` constraint ([GEN-1](10-generics.md)). Examples: `int`, `float64`, `*Node`, `struct{ X, Y int }`. Copying a *writable* `*T` (or an interface or struct that holds one) is a reborrow, not a free alias: the source may not be used while the copy is live ([OWN-B9](#45-borrow-checking-adr-0021) rule 3).

**OWN-C2.** *Copyable.* These values can be copied, and a copy is more than a copy of their bytes ([ADR-0041](../adr/0041-values-are-copied.md)). Assignment, storing, returning and passing each copy the value ([OWN-R3](#42-ownership-rules-d14)); what a copy is depends on the type:
- **`string`** holds immutable bytes in a reference-counted backing buffer (or static storage for string literals, where retain/release is a no-op). A copy shares the bytes; they never change, so the sharing cannot be seen.
- **`[]T`** and **`map[K]V`** (when `T`, `K` and `V` can be copied) have exactly one owner and no reference count. A copy is a new buffer with a copy of each element or entry, so after `t := s` nothing done to `t` is seen through `s`. A function that changes the caller's slice or map takes a pointer (`*[]T`, `*map[K]V`, [OWN-P1](#43-pointers-and-heap-allocation-d17)).
- **Structs, arrays and `Option`s** whose parts can all be copied: each part is copied by its own rule.
- **`Unique[T]`** when `T` can be copied: a copy is a new allocation holding a copy of what it points to.
- **A type with a `drop` method that defines `Clone() T`:** a copy is a call of that method ([OWN-R6](#42-ownership-rules-d14)). A C++ class that is not trivial and has a usable copy constructor is copied by that constructor.
- **`Shared[T]`, `Weak[T]`, `chan T` and `func` values** are the types whose job is sharing: a copy is another reference-counted handle to the same thing.
- **An interface value** (including `error`) has a box of its own with one owner and no reference count: a copy is a new box holding a copy of the value, by the rule of that value's type; a value of up to one word is stored inline ([TYP-5](03-types.md), [ADR-0041](../adr/0041-values-are-copied.md) rule 11). An interface value holds only a value that can be copied ([TYP-6](03-types.md)).
- **Copies that are not made.** A copy the program cannot notice is skipped: on the last use of the source, and for a parameter that the function only reads ([OWN-R3](#42-ownership-rules-d14), [PAR-1](05-parameters.md)). When the owner of a slice or map ends, its elements are destroyed and its buffer is freed. When the last handle to a counted value is released, the value is destroyed and freed immediately.

**OWN-C3.** *Cannot be copied.* A type with a `drop` method and no `Clone() T` (such as `os.File`), a `@Pinned` type, a C++ class without a usable copy constructor, and any type that holds a part that cannot be copied have **one owner and no copy** ([ADR-0041](../adr/0041-values-are-copied.md)). A plain copy of such a value (`b := a`, `x.f = a`, `T{F: a}`, `ch <- a`, passing `a` to a by-value parameter) is a compile error, also when it is the last use of `a`; the fix-its are `move(a)` and `&a`. `move(a)` hands the value over; any later read of `a` is a compile error (`E0451`, use after move) until `a` is assigned again ([OWN-R3](#42-ownership-rules-d14)). Returning a local and using a temporary (a call result, a literal) need no `move`. Borrowing uses `*T` (`&x`, or implicit conversion of `Unique[T]` to `*T` per [OWN-P6](#43-pointers-and-heap-allocation-d17)).

**OWN-C4.** *Structural.* The category of an array, struct, slice, map, `Unique[T]` or `Option[T]` is computed from its parts:
- any field or element that cannot be copied (`OWN-C3`) makes the composite `OWN-C3`;
- otherwise, any copyable (`OWN-C2`) field or element (or being a `string`, `[]T`, `map[K]V`, `Unique`, `chan`, `interface`, `func`, `Shared` or `Weak` itself) makes it `OWN-C2`;
- otherwise it is trivial (`OWN-C1`).
A `@Pinned` attribute always makes a type `OWN-C3`. A `drop` method makes a type `OWN-C3`, unless the type defines `Clone() T`, which makes it `OWN-C2`. `Shared[T]`, `Weak[T]`, `chan T` and `func` values are `OWN-C2` whatever they refer to, and an interface value is always `OWN-C2`: it holds only a value that can be copied ([TYP-6](03-types.md)). `Option[*T]` is trivial, `Option[string]`, `Option[User]` (with `User` holding strings/slices) and `Option[Unique[User]]` are `OWN-C2`, and `Option[os.File]` is `OWN-C3`.

**OWN-C5.** *Type parameters.* Inside a generic body, an unconstrained type parameter `T` may be instantiated with any category (`OWN-C1`, `OWN-C2` or `OWN-C3`), so the body is checked conservatively as potentially `OWN-C3` unless constrained by `Copy` or `Clone` ([GEN-1, GEN-2, GEN-3](10-generics.md)). After monomorphization each instance uses the real category of its type argument.

```go
type P struct{ X, Y int }                    // trivial (OWN-C1)
type Doc struct{ Title string }              // copyable (OWN-C2): d2 := d is a copy
type Tree struct{ Sub Option[Unique[Tree]] } // copyable (OWN-C2): a copy copies the whole tree
type Log struct{ Out os.File }               // cannot be copied (OWN-C3): move(l) or &l
```

*Why (C1 to C5):* one rule covers every type: a value is a copy, so `string`, `[]T`, `map[K]V` and ordinary structs behave like `int`, and two names change one thing only when a type says so (a pointer, a `Mutex` in a `Shared[T]`, a channel). `OWN-C3` keeps strict single ownership for values that have cleanup and no way to duplicate.

## 4.2 Ownership rules (D1.4)

Syntax: no new keywords. `move(x)` is a predeclared function ([OWN-R3](#42-ownership-rules-d14)); `NewUnique`, `NewShared` and `NewWeak` allocate heap pointers ([4.3](#43-pointers-and-heap-allocation-d17)); `mem.Take` and `mem.Replace` extract values from addressable places.

**OWN-R1.** *Deterministic destruction.* Every non-trivial value is destroyed deterministically without a tracing GC:
- A value is destroyed when its owner's life ends:
  1. its `drop` method runs, if any;
  2. then its non-trivial fields are destroyed, in **reverse declaration order**.
- Destroying a `[]T` or a `map[K]V` destroys its elements or entries (for a slice, exactly its `len` elements) and frees its buffer. There is no reference count to read: the slice or map has one owner.
- Destroying an interface value destroys the value it holds and frees its box. The box has one owner and no reference count ([TYP-5](03-types.md)).
- Destroying a counted value (`string`, `Shared[T]`, `Weak[T]`, `chan T`, a `func` value) releases its reference; when the last strong reference is released, what it refers to is destroyed and its heap block is freed immediately.

```go
type Conn struct {
    fdPlusOne int
    buf       []byte          // declared second
    log       Logger          // declared third
}
func (c *Conn) drop() {
    if c.fdPlusOne != 0 { println("closing", c.fdPlusOne-1) } // runs first
}
// then c.log is destroyed, then c.buf is freed (reverse declaration order)
```

**OWN-R2.** *Destruction order.*
- Locals are destroyed (or released) at the end of their block, in reverse declaration order. One exception: a block-scoped local that an executed `defer` uses lives until function exit ([PAN-3, PAN-4](09-errors-and-panics.md)).
- Temporaries are destroyed at the end of the full statement, in reverse order of creation.
- In the header of a `for … range`, `if` or `switch`, temporaries live until the end of the whole statement, body included.
- A value of a type with parents ([TYP-16](03-types.md#35-inheritance-adr-0023)) is destroyed child first: the child's `drop` method (if any), then the child's own fields in reverse declaration order, then the parent subobjects in reverse `@Extends` order (`Pn` down to `P1`, each running its `drop`, its fields in reverse, and its own parents in reverse) ([TYP-24](03-types.md#35-inheritance-adr-0023), [ADR-0023](../adr/0023-inheritance.md)). Destroying a `Unique[Parent]` that holds a `Child` destroys the whole `Child`. Each type has its `drop` method for itself: a child declares its own next to those of its parents without `@Override`, and two parents may both have one; the same holds for the copy method `Clone() T` of a type with a `drop` method ([OWN-R6](#42-ownership-rules-d14)).
- *The `drop` method of a type with a type table uses its receiver only to reach its fields.* The fields of the child have ended when the `drop` method of a parent runs, so a method that the child replaces ([EXP-14](06-expressions.md#69-upcasts-and-downcasts-adr-0023)) must not run there. Any type with a type table may be such a parent. In its `drop` method the receiver `b` is written only as `b.field`: calling a method on it (`b.Label()`, `base.Flush()`), giving it to a function and storing it are compile errors. The methods of a field, and functions that are given fields, are called as usual. A type without a type table has no such limit.

```go
type Base struct {
    name string
    log  Log
}

@Virtual
func (b *Base) Label() string { return "base " + b.name }

func (b *Base) drop() {
    b.log.Write(b.Label())           // error: cannot use b here
    b.log.Write("base " + b.name)    // ok: the fields of b
}

@Extends(Base)
type Child struct{ extra []string }

@Override
func (c *Child) Label() string { return "child " + c.name + c.extra[0] }   // c.extra has ended when Base's drop runs
```

**OWN-R3.** *Copying, copies that are not made, and `move(x)`* ([ADR-0041](../adr/0041-values-are-copied.md)).
- **A value is a copy.** Initialization and assignment (`b := a`, `b = a`), storing into a field, an element or a map entry (`x.f = a`, `T{F: a}`, `xs = append(xs, a)`, `m[k] = a`), passing `a` to a by-value parameter or receiver, `return a`, `ch <- a`, `go f(a)` and converting `a` to an interface each give the destination its own value, for every type. After `b := a`, nothing done to `b` is seen through `a`. What a copy is depends on the type ([OWN-C1, OWN-C2](#41-type-categories-d13)).
- **A copy that cannot be noticed is not made.** A program cannot tell the difference and must not depend on how many copies are made. A user-defined `Clone` ([OWN-R6](#42-ownership-rules-d14)) is skipped the same way, so it must not have effects a program relies on. The compiler skips a copy in two cases:
  - The source is a local with **no later read** on any path (its last use): the value is handed over, with no allocation and no reference-count change.
  - A function **only reads** a by-value parameter and the argument cannot change during the call: the argument is passed as it is ([PAR-1](05-parameters.md)).
- **`move(x)`** is a predeclared function. Its operand is a local variable or a parameter. The result is the value of `x`; no copy is made. Any later read of `x` before `x` is assigned again is a compile error (`E0451`, use after move).
- **For values that cannot be copied (`OWN-C3`, such as `os.File`):**
  - A plain copy of a local (`b := a`, `x.f = a`, passing `a` to a `T` parameter, `ch <- a`, `T{F: a}`, `xs = append(xs, a)`) is a compile error (`E0487`), also on the last use of `a`. Fix-its: `move(a)` to hand the value over, or pass `&a` to a `*T` parameter to borrow.
  - Returning a local (`return a`) and using a temporary (a call result, a literal) need no `move`.
- *Types with parents.* A child and its parent parts are one value. Taking only a parent part out of a child (`var p Payment = card`) is value slicing and is a compile error ([TYP-21](03-types.md#35-inheritance-adr-0023)). Putting a `Unique[Child]` into a `Unique[Parent]` place is an upcast; the `Unique` is copied or moved like any other value ([EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023)).

```go
// OWN-C2 (string, slice, map, ordinary struct): a value is a copy
names := []string{"ann", "bob"}
backup := names          // `names` is used below -> backup is a copy with its own elements
names[0] = "amy"         // backup[0] is still "ann"
users.Add(names)         // last use of `names` -> no copy is made
total := count(backup)   // count only reads its parameter -> no copy is made
other := move(backup)    // moved: no copy
// fmt.Println(backup)   // compile error E0451: backup used after move

// OWN-C3 (File): cannot be copied
f := openLog()           // func openLog() File
// g := f                // compile error: File cannot be copied; fix-its: move(f), &f
w := Writer{Out: move(f)} // f is moved into w.Out
// f.Write(data)         // compile error E0451: f used after move
f = openLog()            // assigned again: usable
```

**OWN-R3a.** *Reading from non-local places.* "Non-local places" are struct fields, slice/array elements, map entries, package variables, pointer targets, and variables captured by closures ([CLO-9](11-closures.md)).
- For `OWN-C1` (trivial) and `OWN-C2` (copyable) types, reading a value from any place and storing it into a new owner copies it (`d.Title = other.Title`, `return items[0]`, `counts[w]++` all work as in Go). No copy is made for an operand that is only read, or for an argument to a parameter that is only read ([OWN-R3](#42-ownership-rules-d14)). A place is never emptied by a plain read: the last-use case of [OWN-R3](#42-ownership-rules-d14) and `move` need a local. To take a value out of a place without copying it, use `mem.Take` or `mem.Replace` ([OWN-R4](#42-ownership-rules-d14)).
- For `OWN-C3` (cannot be copied) types:
  - Borrowing a place (`check(&w.Out)` where `check` takes `*File`, or calling a `*T` method `w.Out.Write(data)`) borrows in place without moving.
  - Taking the value out of a non-local place with `f := w.Out` is a compile error (`E0253`): the value cannot be copied, and moving silently out of a struct field or slice element would leave an unexpected zero/empty slot behind. Fix-its: `&w.Out` (to borrow), `mem.Take(&w.Out)` (to take and leave the zero value, when the type has one), or `mem.Replace(&w.Out, next)` (for any type, including `@Required` / never-nil fields).
  - Swapping places in one parallel assignment (`xs[i], xs[j] = xs[j], xs[i]`, `c.A, c.B = c.B, c.A`) is allowed for every movable type, including `OWN-C3` types with no zero value: all index and pointer operands and their bounds checks are evaluated before any bytes move.

**OWN-R3b.** *Read-only bindings (plain range variables; assertion bindings are now copies).*
A plain `range` variable is a read-only name for an existing value. An assertion binding was one and is now a copy:
1. **Plain `range` variables over structs/arrays (`for _, d := range docs`):** for `OWN-C1` and `OWN-C2` elements, `d` is a read-only copy of the element, and the copy is not made when the body only reads `d` (top-level assignments `d.Title = "x"` and `&d` are compile errors so `d.Title = "x"` never silently mutates a discarded copy as in Go; fix-it `for _, &d := range docs { d.Title = "x" }`, [RNG-1, RNG-1a](08-slices-and-range.md)). Passing `d` or `d.Title` to functions and storing `out = append(out, d)` work as in Go. Writing to an element of a slice or map field (`d.Tags[0] = "x"`) is a write to `d` and is the same compile error, and so are `delete` and `clear` on `d` or a part of it. Assigning the whole variable (`d = Doc{}`, `x++`, `line = strings.TrimSpace(line)`) changes the loop's own copy, as in Go, and is allowed. A part reached through a raw pointer (`d.Next.Title = "x"`) is not a part of `d`. The index or key variable follows the same rules and has no `&` form. For `OWN-C3` elements, `d` is a read-only view of the element (`&d` in `for _, &d := range xs` gives writable access): assigning to `d`, taking `&d`, `move(d)` and giving `d` by value are compile errors.
2. **Type-assertion and type-switch bindings (`s, ok := v.(T)`, `switch x := v.(type)`)** are no longer read-only bindings ([ADR-0041](../adr/0041-values-are-copied.md) rule 11): the binding is an ordinary variable that holds a copy of the value, and the copy is not made when the binding is only read ([TYP-5, TYP-9](03-types.md)). Changing it does not change the interface value; box a `Shared[Mutex[T]]` to change a value every holder sees.

**OWN-R3c.** *Locals captured by closures or `defer`.* A closure uses the captured variable itself, as if through `&x` ([CLO-1, CLO-3a](11-closures.md)). Moving a local (`move(x)`) while a closure or pending `defer` still refers to it is a compile error ([CLO-7, CLO-9](11-closures.md), [PAN-3](09-errors-and-panics.md)). `defer` arguments are evaluated at the `defer` statement and are copies like any other argument ([OWN-R3](#42-ownership-rules-d14)).

**OWN-R4.** *In-place updates, `mem.Take` and `mem.Replace`.*
- `p = append(p, …)`, `p = p[i:j]`, `buf = strconv.AppendInt(buf, n, 10)` and `m[k] = append(m[k], v)` update the destination in place. They are cases of the copy that is not made ([OWN-R3](#42-ownership-rules-d14)): the old value of the destination is not read again, so it is handed over. When the slice's backing buffer has spare capacity, `append` writes directly in place with zero allocation.
- `mem.Take(&place)` returns the current value of `place` and writes the zero value of `T` into `place` (compile error if `T` has no zero value, [OWN-R9](#42-ownership-rules-d14)).
- `mem.Replace(&place, v)` stores `v` into `place` and returns the previous value; it works for every movable type, including never-nil pointers and `@Required` structs.

**OWN-R5.** *Zero values and moved-from state.*
- Every type has Go's zero value, except the types listed in [OWN-R9](#42-ownership-rules-d14) (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`, `func` types, interfaces with methods other than `error`, structs with required fields, C++ classes without a default constructor, and arrays/structs containing them).
- When a Go++ local or `mem.Take` slot is moved from, its storage is set to all-zero bytes, and drop glue is a no-op on all-zero bytes (so no extra boolean drop flags are needed on normal paths). A custom `drop()` method must treat the zero value as a no-op ([OWN-R7](#42-ownership-rules-d14)). Imported C++ classes without zero-state destruction use their C++ move constructor and destruction state tracking ([OWN-R9](#42-ownership-rules-d14)).

**OWN-R6.** *The copy of a value and the copy method* ([ADR-0045](../adr/0045-clone-is-not-called.md)).
- A copy is written as an assignment, an argument or a result: `b := a`, `f(a)`, `return a` ([OWN-R3](#42-ownership-rules-d14)). The copy of what a pointer points to is `*p`. The constraint `Clone` means "can be copied"; `Copy` means "is copied by copying its bytes" ([GEN-1](10-generics.md)).
- The copy of each type that can be copied (`OWN-C1`, `OWN-C2`): built-in scalars (the bytes), `string` (the same immutable bytes), `[]T` (a fresh backing array in the slice's allocator with a copy of each element), `map[K]V` (a fresh map in the map's allocator with a copy of each key and value), `Unique[T]` (a new `Unique[T]` holding a copy of `*u`), `Shared[T]` / `Weak[T]` / `chan` / `func` (the handle, retained), an interface value (a box of its own holding a copy of the value, [TYP-5](03-types.md)), and structs/arrays whose fields can all be copied (each field by its own rule).
- A type with a `drop` method can be copied **only** if it defines the copy method `Clone() T` by hand (with a value receiver `func (h Handle) Clone() Handle`, whose receiver is borrowed read-only without consuming `h`); `b := a` then calls that method. Without it the type cannot be copied (`OWN-C3`). This prevents accidental duplication of OS handles. A copy may be skipped ([OWN-R3](#42-ownership-rules-d14)), so a `Clone` method must not have effects a program relies on.
- *Types with parents.* The copy of a child copies all parent and child fields. For any ancestor `P` with a type table ([TYP-22](03-types.md#35-inheritance-adr-0023)), `p.Clone()` on a `*P`, a `Unique[P]` or a `Shared[P]` returns a `Unique[P]` holding a full copy of the dynamic type ([ADR-0023](../adr/0023-inheritance.md) decision 16). `*p` would give only the parent part ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023)). `p.Clone()` there is always this copy of the whole value, also when `P` has a copy method `Clone() P` of its own (that method is what `=` calls for a value of `P`, and what the copy of the whole value calls for the `P` part); it is an error when `P` cannot be copied.
- *A child of a parent that is copied through a pointer can be copied.* A `Unique[P]` may hold a value of any child of `P`, and its copy (`b := a`, or `p.Clone()`) is a copy of that whole value. So when a parent `P` with a type table can be copied, every child of it can be copied too: a child that cannot is a compile error where it is declared. For a generic parent or child the rule holds for every type argument: if `P` can be copied for some type arguments, the child can be copied for all of its own (`@Extends(Shape) type Box[T any] struct{ v T }` is an error; `Box[T Clone]` is fine). A parent that can never be copied (it holds a value with a `drop` method and no `Clone`), or one without a type table, asks nothing of its children.

```go
type File struct{ fd int }
func (f *File) drop() { /* close */ }

type Shape struct{ name string }
@Virtual
func (s *Shape) Area() int { return 0 }

@Extends(Shape)
type Sheet struct{ f File }           // error: Sheet cannot be copied, and its parent Shape can

@Extends(Shape)
type Page struct{ f Shared[File] }    // ok: the copy shares the file

func twin(s *Shape) Unique[Shape] { return s.Clone() }   // a copy of the Page, or of whatever s points into
```

**OWN-R7.** *Destructors are a method named `drop`* with a pointer receiver: `func (f *File) drop()`. It cannot be called directly (like `init`), and must accept the zero value safely. In a type with a type table it uses its receiver only to reach its fields ([OWN-R2](#42-ownership-rules-d14)).

```go
// Store fd + 1 so valid POSIX fd 0 (stdin) is non-zero and 0 means "closed / moved-from".
type File struct{ fdPlusOne int }

func (f *File) drop() {
    if f.fdPlusOne != 0 { sys_close(f.fdPlusOne - 1) }
}
```

**OWN-R8.** *Borrow checking without lifetime annotations.* Raw pointers `*T` (`&x`), slice views (`&s[i:j]`), active `range` loops, arena allocations and mutex guards are checked by flow and liveness ([OWN-B1 to OWN-B10](#45-borrow-checking-adr-0021)). Returning `&local`, resizing a slice while `&xs[i]` is live, passing overlapping `*T` arguments, or sending `&local` to `go` are compile errors. What remains unchecked (`unsafe`, C/C++ raw pointers, `longjmp`) is listed in [chapter 16](16-undefined-behavior.md).

**OWN-R9.** *Types with no zero value and C++ classes.*
- Pointer kinds (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`), `func` types, interface types with methods other than `error`, structs with `@Required` fields, C++ classes without a default constructor, and arrays/structs containing them have **no zero value** ([OWN-P9](#43-pointers-and-heap-allocation-d17)). `Option[T]` always has the zero value `nil`.
- Operations that materialize a zero value (`var x T`, `make([]T, n)` for non-zero `n`, `clear(slice)`, single-value `m[k]` or `<-ch`) are compile errors when `T` has no zero value. Use `make([]T, 0, cap)`, `s = s[:0]`, and comma-ok lookups `v, ok := m[k]` / `v, ok := <-ch` (usable where `ok` is known true, [OWN-P10](#43-pointers-and-heap-allocation-d17)).
- Moving an imported C++ class runs its C++ move constructor and destroys the moved-from source at scope end.

**OWN-R10.** *Pinned values and RVO.*
- A value of a type marked `@Pinned` (the `sync` types, including `Mutex[T]`) cannot be moved or copied once it is initialized in a place (a variable, a field, or a heap allocation): a compile error. A function may still return a freshly built one, because the result is built in the caller's place.
- `return local` constructs the local directly in the caller's result slot when possible so it never moves in memory.

### Example: the whole model on one screen

```go
type Node struct {
    Name     string                 // copyable (OWN-C2)
    Children Mutex[[]Shared[Node]]  // changes after the node is shared, so it is in a Mutex
    Parent   Option[Weak[Node]]     // back-pointer: does not keep Parent alive
}

func NewNode(name string, parent Option[Weak[Node]]) Shared[Node] {
    return NewShared(Node{Name: name, Children: sync.NewMutex([]Shared[Node]{}), Parent: parent})
}

func Add(parent Shared[Node], child Shared[Node]) {
    kids := parent.Children.Lock()                      // a shared Node is changed under its lock
    kids = append(kids, child)
}

func main() {
    root := NewNode("root", nil)
    Add(root, NewNode("a", NewWeak(root)))
    b := NewNode("b", NewWeak(root))
    Add(root, b)                                        // a copy of b (another handle) goes into Children; b is usable below
    // b.Name = "b-renamed"                             // compile error: b is shared, and Name is not in a Mutex
    if w, ok := b.Parent.Get(); ok {
        if p := w.Lock(); p != nil { fmt.Println(p.Name) }
    }
    head := root.Name[:2]                               // plain string: shares buffer, 0 byte copy
    fmt.Println(head)
}                                                       // counts hit 0: whole tree freed immediately
```

## 4.3 Pointers and heap allocation (D1.7)

A heap pointer is made with `NewUnique`, `NewShared` or `NewWeak`. Go's `new` is removed.

| Type | Created with | Meaning |
|---|---|---|
| `*T` | `&x` (address of a variable, field or element) | raw, non-owning tracked borrow, **never nil** (0 malloc, 0 counter) |
| `Unique[T]` | `NewUnique(v)` | single heap owner (`OWN-C2`, or `OWN-C3` when `T` cannot be copied); a copy allocates a new one; dropped and freed when its owner ends; never nil |
| `Shared[T]` | `NewShared(v)` | shared heap ownership (`OWN-C2`); read-only through the handle, a part that changes is in a `Mutex` / `Atomic` (OWN-P8); `Send + Sync` with an atomic refcount when `T` is `Sync`; never nil |
| `Weak[T]` | `NewWeak(s)`, from a `Shared[T]` | non-owning back-pointer (`OWN-C2`); `w.Lock()` returns `Option[Shared[T]]`, `nil` if freed |
| `Option[T]` | `nil`, or any `T` (converts automatically) | "maybe nothing", zero value `nil`; the nilable form of pointers, funcs and interfaces with methods |

**OWN-P1.** `*T` is a raw borrowed pointer. `&x` takes the address of an addressable operand (a variable, struct field or slice/array element), as in Go, and gives a `*T` that is `x`. Pointers to slice or map types (`*[]T`, `*map[K]V`, including pointers to defined slice or map types) are ordinary pointer types ([ADR-0041](../adr/0041-values-are-copied.md)): a function that changes the caller's slice or map takes one, and the caller writes `&` (`func addOne(xs *[]int) { xs[0]++ }`, called as `addOne(&nums)`; [OWN-P5](#43-pointers-and-heap-allocation-d17)). `&s[i:j]` gives a `*[]T` that is part of a slice or array ([OWN-S2](#44-slicing-and-shared-buffers-d16-adr-0032)). `*T` never allocates and never frees anything.

**OWN-P2.** `NewUnique(v)` and `NewShared(v)` take `v` by value ([OWN-R3](#42-ownership-rules-d14)), store it in a heap allocation and return `Unique[T]` or `Shared[T]`: `NewUnique(Node{Name: "a"})`.

**OWN-P3.** `NewWeak(s)` takes a `Shared[T]` and returns `Weak[T]` without keeping the target alive.

**OWN-P4.** `&T{…}` is a compile error with fix-it: "use `NewUnique(T{…})` or `NewShared(T{…})`".

**OWN-P5.** Field access and method calls **auto-dereference** `*T`, `Unique[T]` and `Shared[T]`. A pointer is used like the variable it points to, without writing `*` ([ADR-0043](../adr/0043-no-star-to-write-through-a-pointer.md)). *Reading:* a `*T` used where only a `T` fits is read as the value it points to: indexing (`xs[0]`, `m[k]`), slicing (`xs[i:j]`), `range xs`, `len`, `cap`, `delete`, `clear`, operators (`n + 1`), an argument for a `T` parameter (`append(xs, 4)`), `var m int = n`, `return n` for a `T` result. *Writing:* assigning a `T` to a `*T` writes the variable it points to: `n = 9`, `n++`, `n += 2`, `xs = append(xs, 4)`, `xs = nil`. *Where a pointer fits, it stays a pointer,* as in Go: `q := p`, an argument for a `*T` parameter, `p == q` on two pointers; and assigning a `*T` to a `*T` points it at the other variable (`n = n.next`). When the right side fits both `T` and `*T`, it re-points. `*p` is still an expression that reads the value (`m := *p`, `a, b = *b, *a`). On the left of an assignment the star may be written, as in Go: `*p = v`, `*p += v` and `*p++` write the same variable as `p = v`, `p += v` and `p++`. `Weak[T]` does **not** ([ADR-0017](../adr/0017-weak-no-auto-deref.md)): only `w.Lock()` is available on `Weak[T]`. `Unique[T]` and `Shared[T]` get the methods of `T` and `*T`.

**OWN-P6.** `Unique[T]` and `Shared[T]` stored in a place convert **implicitly** to `*T` (and `Option[*T]`); `Option[Unique[T]]` and `Option[Shared[T]]` places convert to `Option[*T]`. Converting from a temporary is an error because the temporary would die at the end of the statement. A pointer made from a `Shared[T]` is read-only, like the handle ([OWN-P8](#43-pointers-and-heap-allocation-d17)).

**OWN-P7.** `make([]T, n)`, `make(map[K]V)` and `make(chan T, n)` work as in Go (with the allocator rules of [SLC-2](08-slices-and-range.md)).

**OWN-P8.** `Unique[T]` has one owner and grants exclusive writable access. `Shared[T]` and `Weak[T]` are reference-counted handles (`OWN-C2`):
- **Read-only through the handle** ([ADR-0042](../adr/0042-shared-is-read-only.md)): several handles are the same `T`, so what a `Shared[T]` holds cannot be changed through a handle, all the way down. Reading a field or an element, `len`, `range`, copying a part out, a method with a value receiver and a method with a pointer receiver that only reads through it are allowed. Assigning a field or an element, `append`, `delete`, `clear`, `mem.Take`, `mem.Replace` and a method that writes through its receiver are compile errors (`E0492`). A pointer taken into the value (`&p.Name`) is a read-only binding ([OWN-R3b](#42-ownership-rules-d14)). The handle, or such a pointer, is given only to a function that only reads through the parameter, which the compiler finds out for each function, also through what the function calls (`E0492`). Nothing is known of a method of an interface or of a function value, so neither is given one; a function of a header says it by `const`. Of a `@Virtual` method that a child may replace, what the method of that name does counts. Any field type may sit in `T`: no handle can overwrite it.
- **Changing shared data:** the part that changes is a `Mutex[U]`, `RWMutex[U]`, `Atomic[U]` or a channel and is changed through it (`kids := p.Children.Lock(); kids = append(kids, c)`), or the whole value is one (`Shared[Mutex[Node]]`). This is the same in one goroutine and across goroutines.
- **Across goroutines:** `Shared[T]` and `Weak[T]` are `Send` and `Sync` when `T` is `Sync` ([CON-19](12-concurrency.md)), and the reference count is atomic exactly then.

**OWN-P9.** *Never nil, and `Option[T]`* ([ADR-0018](../adr/0018-never-nil-pointers-and-option.md)).
- `*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`, `func` types, and interface types with at least one method (except `error`) are **never nil** and have no zero value.
- `error`, `any` (empty interfaces), `unsafe.Pointer`, slices, maps and channels keep Go's `nil`. Writing to a nil map held in an addressable local variable, struct field, slice element or map entry (`var m map[string]int; m["a"] = 1`) creates the map in place; a `map[K]V` parameter is the function's own copy ([PAR-1](05-parameters.md)), so writing to a map parameter, nil or not, changes only the callee's parameter; the write is a compile error when the parameter is not read, returned or passed on afterwards ([ADR-0041](../adr/0041-values-are-copied.md) rule 9). To fill the caller's map, take a `*map[K]V`. Writing to a temporary map (`f()["a"] = 1`), or to the map that a type assertion yields (`v.(map[string]int)["a"] = 1`: the assertion gives a copy, [TYP-9](03-types.md)), is a compile error (`E0353` / `E0354`), so Go++ has no run-time nil-map panic.
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

<a id="44-slicing-and-shared-buffers-d16-adr-0032"></a>
## 4.4 Slicing (D1.6, ADR-0041)

Slicing a `string` shares its immutable bytes. Slicing a `[]T` or an array gives a copy of the elements (`xs[i:j]`) or a view of the elements themselves (`&xs[i:j]`):

**OWN-S1.** *String slicing.* `s[i:j]` on a `string` returns a `string` `{s.ptr + i, j - i, s.backing}` in $O(1)$ with no byte allocation. If `s.backing != nil`, retaining `s.backing` keeps the heap bytes alive for as long as the substring lives; if `s` is a static literal (`backing == nil`), no reference count is touched.

**OWN-S2.** *Slice and array slicing* ([ADR-0041](../adr/0041-values-are-copied.md)).
- `xs[i:j]` on a `[]T` or an array is a value: a new `[]T` with copies of elements `i` to `j`. Nothing done to it is seen through `xs`, and reassigning or growing `xs` does not change it. The copy is skipped when the program cannot tell ([OWN-R3](#42-ownership-rules-d14)): `xs = xs[i:j]` shortens `xs` in place and destroys the elements that are cut off at that statement, and passing `xs[i:j]` to a parameter that is only read (`w.Write(hash[:])`) passes the elements as they are, with `0` heap allocation.
- `p := &xs[i:j]` (and `&arr[i:j]` on an array) gives a `*[]T` that covers elements `i` to `j` of `xs` itself: a write through it (`p[0] = v`) changes `xs`. The view has no spare capacity (`cap == len`) and `backing == nil` ([SLC-1](08-slices-and-range.md)). It is tracked by the borrow checker as a **borrow of `xs`** ([OWN-B1](#45-borrow-checking-adr-0021)): it cannot outlive `xs`, and `xs` cannot be assigned, grown or moved while the view is in use ([OWN-B2](#45-borrow-checking-adr-0021)). Growing or replacing the view (`p = append(p, v)`) never changes `xs`. A function that changes the elements takes the view: `sort.Ints(&arr[:])`, `r.Read(&buf[n:])`.
- When the elements cannot be copied (`[]os.File`, `OWN-C3`), `sub := xs[i:j]` is a compile error like any other plain copy ([OWN-C3](#41-type-categories-d13)); write `&xs[i:j]` for a view.

**OWN-S3.** *Map keys.* Looking up or updating `m[k]` reads `k` for the lookup and makes no copy of it; `k` is copied into the map only when a new key is inserted.

<a id="45-borrow-checking-adr-0021"></a>
## 4.5 Borrow checking (ADR-0021, ADR-0032)

Go++ has no lifetime annotations. Plain value code (`T`, `string`, `[]T`, `map[K]V`) needs no borrow checking at the call site because each value is its holder's own copy ([OWN-R3](#42-ownership-rules-d14)). The borrow checker runs on **tracked borrows** (`&x` raw pointers, slice views `&s[i:j]`, borrow-carrying structs/arrays, active `range` loops, arenas, mutex guards and `sync.Scope`) using flow and liveness.

**OWN-B1.** *What is a tracked borrow.* A tracked borrow is any of:
- a raw pointer `*T` made by `&x` (of a local, parameter, struct field or slice/array element), an `&` range alias (`for _, &e := range xs`), or borrowed from a `Unique[T]` / `Shared[T]` place ([OWN-P6](#43-pointers-and-heap-allocation-d17));
- a slice view `&s[i:j]` of a slice or an array ([OWN-S2](#44-slicing-and-shared-buffers-d16-adr-0032));
- a **borrow-carrying struct, array or `Option`** whose fields/elements contain `*T` or a slice view (`Parser{arena *mem.Arena}`, `Node{Left, Right Option[*Node]}`);
- an active `range` loop over a slice, array or map ([RNG-3](08-slices-and-range.md));
- a value allocated from a `mem.Arena` / `mem.DropArena` (it borrows the arena);
- a `Mutex[T]` / `RWMutex[T]` guard (`Lock` / `RLock`, [CON-15](12-concurrency.md)).

An upcast (`*Child` to `*Parent`) is the same borrow of the same place ([EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023)).

**OWN-B2.** *No invalidation while a borrow is live.* While a tracked borrow is **live** (used on some path afterwards), the borrowed storage may not be modified in a way that can free, reallocate or relocate the memory the borrow points into:
- reassigning or moving (`move(x)`) the owner or a prefix of it while `&x` or `&x.f` is live;
- `append`, `clear` or in-place re-assignment on the same slice variable/path `xs` while `&xs[i]` or a view `&xs[i:j]` (or an active `range` over `xs`) is live — because `xs` is the one owner of its buffer, and `append` may free or reallocate the backing array that `&xs[i]` points into;
- inserting into or clearing a map while an `&v` range alias or `range` loop on that map is live;
- `arena.Reset()`, `arena.Free()`, or ending `arena`'s scope while an arena-allocated pointer, slice or borrow-carrying struct is live (bump-allocating further values via `mem.ArenaNew` or `mem.MakeIn` adds chunk blocks without moving existing chunks, so it does **not** invalidate existing arena borrows; `mem.Arena` and `mem.DropArena` free all chunks automatically when their owning scope ends);
- ending the owner's scope while the borrow escapes (`return &local`).

A pending deferred call holds what it was given until the function returns, so its borrows are live until then ([PAN-3](09-errors-and-panics.md)).

Note the contrast with plain slices and strings when **`&` is NOT used**:
```go
// NO `&`: each value is its own copy
s := "hello world"
w := s[0:5]
s = "bye"                  // ok: w retains the old "hello world" buffer (strings are counted)
println(w)                 // prints "hello"

nums := []int{1, 2, 3}
v := nums[1:3]             // v is a new slice with copies of 2 and 3
nums = append(nums, 4)     // ok: v has its own elements
println(v[0])              // prints 2

// WITH `&`: raw pointer *int points into nums — borrow checker protects it!
p := &nums[0]
nums = append(nums, 5)     // compile error: cannot append to nums while p (&nums[0]) is live
println(*p)
```

**OWN-B3.** *Returning a tracked borrow.*
- Functions returning ordinary values (`string`, `[]T`, `User`, `Unique[T]`, `Shared[T]`) return self-contained values and carry **no borrow summary**.
- Only a function returning a raw pointer `*T`, `Option[*T]`, or a borrow-carrying struct/array returns a borrow. The compiler computes which pointer-capable parameters/receiver (such as `arena *mem.Arena` or `p *Parser`) the returned borrow may point into (stored in export data; conservative union for interface/indirect/C calls). Returning `&local` or a borrow into a local variable/array is always a compile error.

**OWN-B4.** *Aliasing at a call.*
- Two writable `*T` arguments (or a `*T` receiver and a `*T` argument) that may point to overlapping places are a compile error (`swap(&list[i], &list[j])` — use `list.Swap(i, j)` or `Split`), except when both point into the same `mem.Arena` / `mem.DropArena` graph: two arena pointers may be the same value, and [OWN-B10](#45-borrow-checking-adr-0021) limits what is done through them.
- Passing `&x` alongside an `OWN-C2` read from `x` (`u.SetName(u.Name)`) is **allowed and safe**: `u` may change during the call, so the argument is copied before the call (for a `string`, the copy is a retain of its bytes) and reassigning `u.Name` inside `SetName` cannot free `n` mid-call ([OWN-R3](#42-ownership-rules-d14), [PAR-1](05-parameters.md)). Passing `&x` alongside a tracked borrow into `x` (`both(&u, &u.Age)`) is a compile error ([OWN-B9](#45-borrow-checking-adr-0021)).

**OWN-B5.** *Where tracked borrows may live.* A raw `*T`, slice view `&s[i:j]`, or borrow-carrying struct/array (`Parser{arena *mem.Arena}`, `Node{Left, Right Option[*Node]}`) may live only in stack local variables, parameters, function results (checked by `OWN-B3`), `Local[I]` interfaces, and allocations inside the same `mem.Arena` / `mem.DropArena` (freed automatically when the parent scope or arena drops/resets). Storing a tracked borrow in `NewUnique`, `NewShared`, a heap slice or map (the slice that a call makes of the arguments that it lists for a `...T` parameter is one), a channel, a package global, an escaping closure, a kept `Send` interface, or passing `&local` to `go` is a compile error: use `Unique[T]`, `Shared[T]` or `Weak[T]` for heap-owned graphs (or `sync.Scope` for scoped goroutines, [CON-16](12-concurrency.md)).

**OWN-B6.** *What is not checked.* [Chapter 16](16-undefined-behavior.md) lists the residue: `unsafe` and raw memory, C and C++ raw pointers/threads, and `longjmp` / forced unwinding.

**OWN-B8.** *Borrows and concurrency ([ADR-0022](../adr/0022-race-free-concurrency.md)).*
- A `Mutex[T]` guard borrows the mutex ([CON-15](12-concurrency.md)) and cannot escape.
- `ch <- v` and `go f(v)` require `Send` values ([CON-12 to CON-14](12-concurrency.md)); raw `*T` borrows cannot cross `go` or `chan`.
- Inside `sync.Scope`, goroutines started with `s.Go` finish before the scope exits and may borrow locals safely; disjoint writable slice parts use `Split` ([CON-16](12-concurrency.md)).

**OWN-B9.** *Exclusive writable `*T` access.*
1. **One writable `*T` at a time.** While a writable `*T` borrow of a local/unique place is live, no other `*T` borrow or read of an overlapping place on that same variable may be live.
2. **Disjointness.** Different local variables, different struct fields, different constant indexes (`&list[0]` and `&list[1]`), and the two views that `list.Split(k)` returns are provably disjoint. Variable indexes `list[i]` and `list[j]` overlap; use `list.Swap(i, j)` or `Split`.
3. **Reborrowing.** `q := p` on a writable `*T` reborrows `p`: `p` cannot be used while `q` is live (`q := p; use(q); use(p)` is fine because `q` is dead before `use(p)`).
4. **No raw `*T` in heap-owned `Unique`/`Shared` data.** Heap-owned structs and collections hold `Unique[T]`, `Shared[T]` or `Weak[T]`, never raw `*T` (`OWN-B5`); borrow-carrying structs live only on the stack or inside a `mem.Arena` whose lifetime bounds every node at once.
5. **`Shared[T]` and arena graphs** ([OWN-P8](#43-pointers-and-heap-allocation-d17), [CON-19](12-concurrency.md), [ADR-0042](../adr/0042-shared-is-read-only.md)): what a `Shared[T]` holds is read-only through the handle, so no handle can replace or free something that another handle points into; a part that changes sits in a `Mutex`, `RWMutex` or `Atomic` and is reached through a guard. Pointers into an arena stay writable and follow [OWN-B10](#45-borrow-checking-adr-0021). `*T` pointers omit C `restrict` qualifiers when `T` may sit inside `Shared[T]` or a borrow-carrying arena struct, and C23 output compiles with `-fno-strict-aliasing`.
6. **`Unique[T]` grants exclusive writable access** through its single owner.

```go
swapNames(&list[0], &list[1])               // ok: constant indexes, disjoint
a, b := list.Split(k)                       // ok: two views that do not overlap
swapNames(&a[0], &b[0])
list.Swap(i, j)                             // ok: built-in safe swap

p := &x
q := p                                      // q reborrows p
use(q)
use(p)                                      // ok: q is not used later

s := NewShared(Config{})
fmt.Println(s.Count)                        // ok: everyone can read a shared value
// s.Count++                                // compile error: a shared value is changed through a Mutex (OWN-P8)
mu := NewShared(sync.NewMutex(Config{}))    // in one goroutine or several: the part that changes is in a Mutex
go func() { mu.With(func(c *Config) { c.Count++ }) }()
```

**OWN-B10.** *Pointers into an arena* ([ADR-0044](../adr/0044-no-borrow-into-what-an-arena-value-owns.md)). Two pointers into one arena may point to the same value, and the compiler does not tell them apart ([OWN-B4](#45-borrow-checking-adr-0021)). So nothing may be kept that another such pointer could free.
- **An arena pointer** is a `*T` that comes from an arena allocation (`mem.ArenaNew`), that was read from a field, an element or an `Option` reached through a pointer (`n.Left`), that was made from an arena pointer (`q := p`, `&p.f`), or that a call returned when its borrow summary ([OWN-B3](#45-borrow-checking-adr-0021)) says the result may come from an arena or from an arena pointer.
- **No borrow into what the value owns separately.** Through an arena pointer `a`, the elements of a slice, the entries of a map, the value an interface holds, and what a `Unique[T]` or `Shared[T]` field points to cannot be borrowed. These are compile errors (`E0491`): `&a.Items[i]`, a view `&a.Items[i:j]`, `for _, &x := range a.Items`, `&a.U.f` through a `Unique` field, and a method call that takes the address of such a part (`a.Items[i].Bump()` with a pointer receiver, a method call on an interface field `a.W.Write(b)`). The fix-it is to use the index each time, or to copy the part out, change it and store it back.
- **What stays allowed.** Reading and writing by index or key (`x := a.Items[i]`, `a.Items[i] = v`, `a.Items[i].Count++`, `a.M[k] = v`), replacing or growing the whole part (`a.Items = append(a.Items, v)`), `len`, `cap`, and passing the part by value. The index is applied at the moment of the read or the write, after the other operands of the statement are evaluated, so nothing is kept across a call. A plain `for _, x := range a.Items` loops over its own copy of the slice or map when the body could change it (it makes a call or writes through a pointer), and over the slice or map itself otherwise. A pointer to a field of the value (`&a.Count`, `&a.Items`) is allowed and is an arena pointer: an arena never frees or moves one value before it frees them all ([OWN-B2](#45-borrow-checking-adr-0021)).
- **At a call.** For each pointer parameter and receiver the compiler infers, and stores in export data, whether the function borrows into what the pointee owns separately (by one of the forms above, or by passing the pointer on to such a parameter). Passing an arena pointer to such a parameter is a compile error at the call. A call that cannot see the body (through an interface or a function value) counts as borrowing through every pointer argument and receiver. Inside a function, a pointer parameter is not an arena pointer by itself; what is read through it is.
- **A type that owns nothing separately is not restricted.** When `T` has no slice, map, interface, `Unique[T]` or `Shared[T]` in its fields, directly or inside a struct, array or `Option` field, there is nothing to borrow into, and none of this applies to a `*T` (`Node{Kind Kind; L, R Option[*Node]}`). A `string` field is a counted handle and does not count.

```go
type Node struct {
    Items []Item
    Next  Option[*Node]
}

func touch(a, b *Node) {             // a and b may be the same Node
    a.Items[0] = Item{}              // ok: by index
    b.Items = append(b.Items, Item{})
    a.Items[0].Count++               // ok: reaches the element where it is now
}

n := mem.ArenaNew[Node](&arena)      // n is an arena pointer
// p := &n.Items[0]                  // compile error: use n.Items[0]
// for _, &x := range n.Items { }    // compile error: use for i := range n.Items
// n.Items[0].Bump()                 // compile error for a pointer receiver
touch(n, n)                          // ok: touch borrows nothing from Items
```

## 4.6 Rationale

- **Why copies + borrow engine ([ADR-0041](../adr/0041-values-are-copied.md))?** One rule covers every type: a value is a copy, and two names change one thing only when a type says so (a pointer, a `Mutex` in a `Shared[T]`, a channel). A slice or a map has exactly one owner, so its buffer needs no reference count and is freed deterministically, with no pauses. A copy the program cannot notice (the last use of the source, a parameter that is only read) is skipped at compile time. The cost is accepted: a copy can be large and is not visible in the source, and Go code that changes a slice or map it received by value now changes a copy.
- **Why keep `Unique[T]`, `Shared[T]` and `Weak[T]`?** Heap pointer ownership stays explicit and self-documenting: `*T` never secretly heap-allocates, `Unique[T]` has zero reference-count overhead and single ownership, `Shared[T]` shares explicitly, and `Weak[T]` breaks parent/child cycles cleanly.
- **Why check `&` with the borrow engine?** Raw pointers `*T` have no reference count and can point into stack locals or the middle of structs and slices (`&nums[0]`, `&u.Age`), where reference counting cannot protect against reallocation or scope exit. Flow-based borrow checking on `*T` catches dangling pointers and aliasing bugs at compile time with zero lifetime annotations.
