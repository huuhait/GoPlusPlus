# 4. Ownership

Base: Go spec [Variables](https://go.dev/ref/spec#Variables), [The zero value](https://go.dev/ref/spec#The_zero_value), [Assignability](https://go.dev/ref/spec#Assignability), [Assignment statements](https://go.dev/ref/spec#Assignment_statements), [Pointer types](https://go.dev/ref/spec#Pointer_types), [Address operators](https://go.dev/ref/spec#Address_operators), [Allocation](https://go.dev/ref/spec#Allocation), [Making slices, maps and channels](https://go.dev/ref/spec#Making_slices_maps_and_channels).

This is the core of Go++. Go has a GC, so every value can be shared. Go++ has no GC, so every value has **one owner** and is destroyed when its owner ends.

Contents: [at a glance](#at-a-glance-pointers-and-borrows) · [4.1 categories](#41-type-categories-d13) · [4.2 rules 1 to 10](#42-ownership-rules-d14) · [4.3 pointers and heap](#43-pointers-and-heap-allocation-d17) · [4.4 views](#44-views-d16) · [4.5 borrow checking](#45-borrow-checking-adr-0021) · [4.6 rationale](#46-rationale)

## At a glance: pointers and borrows

Two questions decide which one you use: **do I own it?** and **one owner or many?** A pointer, a func and an interface with methods are never nil (`error` and `any` keep Go's `nil`); "maybe nothing" is written `Option[T]` (ADR-0018). Rules are in the sections linked in the last column.

| I want to… | Write | Owns? | Rule |
|---|---|---|---|
| read a value inside a function | a normal parameter `func show(u User)`; an owning value is borrowed automatically (read-only, no copy), a trivial one such as `int` is just copied | no | [PAR-1](05-parameters.md) |
| change the caller's value | `func rename(u *User)`, called as `rename(&u)` | no | [OWN-P1](#43-pointers-and-heap-allocation-d17) |
| look at part of a string or a view | `s[i:j]` gives a `View[string]` / `View[[]T]`, with no copy | no | [OWN-V1, V2](#44-views-d16) |
| use part of a slice you may write | `xs[i:j]` gives a borrowed `[]T`: elements can be written, it cannot grow or outlive `xs` | no | [OWN-V2](#44-views-d16) |
| promise to change nothing, deeply | a `View[T]` parameter, e.g. `func show(u View[User])`; fields, elements, map entries and pointer targets are all read-only | no | [OWN-V1](#44-views-d16) |
| change every element in a loop | `for _, &u := range users { u.Age++ }` | no | [RNG-1a](08-slices-and-range.md) |
| own a heap value, alone | `Unique[*User]`, made with `new_unique(User{…})`; freed when its owner ends | yes | [OWN-P2](#43-pointers-and-heap-allocation-d17) |
| share a heap value | `Shared[*User]`, made with `new_shared(User{…})`; freed when the last copy ends | yes, together | [OWN-P8](#43-pointers-and-heap-allocation-d17) |
| point at a shared value without keeping it alive | `Weak[*User]`; read it with `if p := w.Lock(); p != nil { … }` | no | [OWN-P3, P5](#43-pointers-and-heap-allocation-d17) |
| say "maybe nothing" | `Option[*User]`, `Option[int]`, `Option[Logger]`, `Option[func()]`, …; pointers, funcs and interfaces with methods (all but `error` and `any`) are never nil; check with `o != nil`, `!o.IsNil()` or `v, ok := o.Get()`; compare two with `==` when `T` is comparable | — | [OWN-P9, P10](#43-pointers-and-heap-allocation-d17) |
| copy an owned value | `u.Clone()` (short form `clone(u)`) | — | [OWN-R6](#42-ownership-rules-d14) |
| give an owned value away | `move(u)`; `u` is left as the zero value | — | [OWN-R4](#42-ownership-rules-d14) |

```go
func show(u User)            { fmt.Println(u.Name) }   // borrowed: read-only
func rename(u *User)         { u.Name = "amy" }        // *T: changes the caller's User
first := name[0:3]                                     // View[string]: a view, no copy
for _, &u := range users     { u.Age++ }               // & range: changes each element
owner := new_unique(User{Name: "bob"})                 // Unique: freed when owner ends
cfg   := new_shared(Config{})                          // Shared: freed when the last copy ends
var who Option[*User]                                  // Option: nil until set
if who != nil { rename(who) }                          // checked: who is a *User here
back  := new_weak(cfg)                                 // Weak: doesn't keep cfg alive
if c := back.Lock(); c != nil { fmt.Println(c.Name) }  // the only way to read through a Weak
```

## 4.1 Type categories (D1.3)

Every type is in exactly one of three categories.

| Category | Types | Copy | End of life |
|---|---|---|---|
| Trivial | `bool`, numbers, raw `*T`, `unsafe.Pointer`, `View[T]` (every view, whatever `T`), arrays/structs of trivial types | implicit, bitwise | nothing |
| Owning | `string`, `[]T`, `map[K]V`, `Unique[*T]`, types with a `drop` method, arrays/structs that contain an owning type, C++ classes with a non-trivial copy, move or destructor | move-only; `x.Clone()` copies | drop glue runs |
| Shared handle | `Shared[*T]` (`Send`/`Sync` only if `T` is `Sync`, [CON-19](12-concurrency.md)), `Weak[*T]`, `chan T`, interface values (including `error`), func values, arrays/structs whose non-trivial fields are all shared handles | implicit; copying retains (atomic) | release; the last strong reference frees |

**OWN-C1.** *Trivial.* A trivial value is copied with `=`, as in Go. It has no destructor. A trivial type satisfies the `Copy` constraint ([GEN-1](10-generics.md)). Examples: `int`, `*Node`, `View[string]`, `struct{ X, Y int }`.

**OWN-C2.** *Owning.* An owning value is **move-only**. Copying needs `x.Clone()` (or its short form `clone(x)`, [OWN-R6](#42-ownership-rules-d14)). When its owner ends, drop glue runs.

**OWN-C3.** *Shared handle.* Copying a shared handle is implicit and **retains** (atomic increment). Ending a copy **releases**. The last strong release frees the thing.

**OWN-C4.** *Structural.* The category of an array or struct is computed from its elements and fields:
- any owning field makes it owning;
- otherwise, any shared-handle field makes it a shared handle;
- otherwise it is trivial.
A `drop` method makes a type owning, whatever its fields. `Option[T]` has the category of `T` (`Option[*T]` is trivial, `Option[string]` is owning, `Option[Shared[*T]]` is a shared handle).

**OWN-C5.** *Type parameters.* The category of a type parameter `T` is not known inside a generic body. It is treated as owning unless constrained by `Copy` ([GEN-1](10-generics.md)). After monomorphization each instance has its real category.

```go
type P struct{ X, Y int }          // trivial
type Doc struct{ Title string }    // owning (string)
type Ev struct{ Done chan int }    // shared handle (only a channel)
type Mix struct{ Title string; Done chan int }  // owning
```

*Why (C1 to C5):* three categories are enough to tell the compiler what a copy costs: nothing (trivial), a visible `x.Clone()` (owning), or a counter (shared handle). It is the same split as C++ trivially copyable types, move-only types and `shared_ptr`.

## 4.2 Ownership rules (D1.4)

Syntax: no change in this chapter. `move(place)` and `clone(x)` are predeclared function calls (typing in [EXP-1](06-expressions.md)); `x.Clone()` is an ordinary method call ([OWN-R6](#42-ownership-rules-d14)). The `View` type is in [3.1](03-types.md), the `move` parameter mark in [5.1](05-parameters.md), and the `&d` range form in [8.3](08-slices-and-range.md).

**OWN-R1.** *One owner.* Every value has exactly one owner: a variable, field, element or temporary. When the owner's life ends, the value is destroyed:
1. its `drop` method runs, if any;
2. then its owning fields are destroyed, in **reverse declaration order**.

```go
type Conn struct {
    fd  int
    buf Buffer            // declared second
    log Logger            // declared third
}
func (c *Conn) drop() { println("closing", c.fd) }   // runs first
// then c.log is destroyed, then c.buf (reverse declaration order)
```

*Why:* with one owner there is exactly one place that frees, so there is no double free and no leak.

**OWN-R2.** *Destruction order.*
- Locals are destroyed at the end of their block, in reverse declaration order. One exception: a block-scoped local that an executed `defer` uses lives until function exit ([PAN-3](09-errors-and-panics.md), [PAN-4](09-errors-and-panics.md)).
- Temporaries are destroyed at the end of the full statement.
- In the header of a `for … range`, `if` or `switch`, temporaries live until the end of the whole statement, body included (see [OWN-V6](#44-views-d16)).

```go
{
    a := File{}
    b := File{}
}                         // b is destroyed first, then a
n := len(read().Name)     // the temporary from read() is destroyed at the end of this statement
```

*Why:* the order is the reverse of construction, as in C++, so a later value may rely on an earlier one.

**OWN-R3.** *By-value use moves.* A by-value use of an owned **local** moves it:
- `b := a`, `x.f = a`, passing `a` to a `move` parameter, `return a`, `ch <- a`, `T{F: a}`.
- Any later use of `a` is a compile error, until `a` is assigned again.
- Passing `a` to a borrowed parameter, using it as an operand, or calling a method with a borrowed receiver is **not** a move.

```go
a := "hello"
b := a            // a moves into b
fmt.Println(b)
// fmt.Println(a) // compile error: a used after move
a = "new"         // a is assigned again: usable again
fmt.Println(len(a))
```

*Why (R3):* a by-value use that moves makes ownership visible without extra syntax, and a use-after-move error is cheaper than a hidden copy.

**OWN-R3a.** *Reading from other places.* The key of a map index on the left side is not a place read that needs ownership ([OWN-V4a](04-ownership.md#44-views-d16)). "Other places" are fields, elements, map entries, package variables, pointer targets, borrowed variables ([OWN-R3b](04-ownership.md#42-ownership-rules-d14): borrowed parameters, owning range variables, owning assertion bindings) and captured variables ([CLO-9](11-closures.md)).
- **Borrow where a borrow is enough.** Reading an owned value from such a place borrows it when used as an operand, a borrowed argument (including interface parameters), a receiver or the `range` source.
- **Ownership needed means compile error.** These need ownership: `:=`, `=`, a `move` argument, `return`, a channel send, a composite literal. The error has two fix-its:
  - `p.Clone()` (copy it);
  - `move(p)` (take it, leave the zero value).
  For a borrowed parameter the fix-its are `p.Clone()` and "make the parameter `move`", because a borrowed parameter cannot be moved out of ([PAR-1](05-parameters.md)).
- **Moving a borrowed variable.** `move(x)` of a borrowed variable is a compile error ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)). Fix-its: `x.Clone()`, or declare the range variable with `&` (`for _, &v := range xs`, `for k, &v := range m`), which may be moved from ([RNG-1a](08-slices-and-range.md)). For a map, `move(m[k])` also takes the value out ([RNG-3](08-slices-and-range.md)).
- **Maps.**
  - `move(m[k])` removes the entry and returns it. For a value type with no zero value ([R9](04-ownership.md#42-ownership-rules-d14)), its value may only be taken with `v, ok := move(m[k])`, because a missing key would give zero bytes; a bare `move(m[k])` statement just removes the entry.
  - `v, ok := clone(m[k])` and `v, ok := move(m[k])` keep the comma-ok form.
  - `m[k].f = x` stays illegal, as in Go.
- **Swap.** A statement that reads and writes the same places (`a[i], a[j] = a[j], a[i]`) is allowed and moves the elements.
- **`min` and `max`.** On owned strings they return a copy.

```go
type Doc struct{ Title string }

func (d *Doc) Rename(t string) {
    d.Title = t           // error: needs ownership; fix-its: t.Clone(), or make t a `move` parameter
    d.Title = t.Clone()   // ok: copy
}

func take(items []Doc) Doc {
    // return items[0]         // error: fix-its items[0].Clone() / move(items[0])
    return move(items[0])      // ok: items[0] now holds the zero Doc
}

m := map[string]string{"k": "v"}
v, ok := move(m["k"])          // removes "k"; ok reports it was there
```

*Why (R3a):* borrowing where a borrow is enough keeps Go-like code free of `Clone`; asking for `Clone` or `move` exactly where ownership is needed makes every copy and every take visible.

**OWN-R3b.** *Borrowed variables* (the one definition). A **borrowed variable** is a variable that borrows a value still owned by something else. The complete list:
1. borrowed owning parameters and receivers ([PAR-1](05-parameters.md));
2. range variables over **owning** elements of a slice, an array or a C++ container, written in the plain form `for _, d := range xs` ([RNG-1](08-slices-and-range.md));
3. map range variables whose key type or value type is **owning**, in the plain form (each such variable);
4. bindings of a type assertion or type switch whose case type is **owning**, including `case string:`, `case []T:`, `v.(string)` and `v.([]T)` (they do not match a stored `View[T]`; only `case View[T]:` does, [TYP-9](03-types.md)): the binding is always borrowed and read-only, and so is what it holds by value: map entries and slice elements are read-only too (a pointee reached through a pointer stays writable), because it may point into someone else's buffer ([TYP-9](03-types.md));
5. the range variable of a range-over-func loop in the plain form, when the yielded type is **owning** ([RNG-6a](08-slices-and-range.md));
6. a local made by slicing a slice or array that may be written, `t := xs[i:j]` or `var t = xs[i:j]` (a borrowed `[]T`, [OWN-V2](#44-views-d16)). Its elements can be written; it cannot grow, cannot be moved and cannot outlive `xs`.

Trivial and shared-handle range variables and bindings are **not** borrowed variables: they are copies (a shared handle is copied with a retain) and stay writable. The variables of `range move(xs)` and `range ch` are owned locals. A variable declared with `&` in a range clause (`for _, &d := range xs`) is an **alias** of the element, not a borrowed variable: it is writable and changes the collection, and for `defer` and `go` it counts as a place in the collection ([RNG-1a](08-slices-and-range.md)). Every other rule that says "borrowed variable" means exactly the list above.

A borrowed variable is **read-only at the top level**. This is the one rule for all kinds, and PAR-1 and PAR-2 refer to it. These are compile errors:
- assigning the variable itself, or assigning its own fields (the fields of the struct value it holds), and `append` to it. In the same scope, a `:=` that reuses the name `v` assigns to `v`, so it is an error; use a new name;
- taking its address (`&d`), or calling a method with a pointer receiver on it (an implicit `&d`). This covers the variable **and any part of it reached without going through a slice, map or pointer**: its fields, fields of fields, and array elements held by value (`&d.Buf`, `d.Buf.Reset()`). The line is the same "top level" line as PAR-1: going through a slice, map or pointer is allowed; staying inside the value is not;
- `move(x)` of it. A range variable can never be moved from in the plain form, because moving a key would corrupt the map and moving a value is ambiguous with `move(m[k])`. Fix-its: `&v` (the `&` form can be moved from, [RNG-1a](08-slices-and-range.md)) or `v.Clone()`.

These stay allowed: data reached **through** slices, maps and pointers, as in Go and PAR-1. Elements of a `[]T`, map entries and pointer targets are mutable (except through an assertion or type-switch binding, whose box is immutable: see below and [TYP-9](03-types.md)). For example `row[j] = 0` for `row` in `range grid`, and `d.Tags[0] = "x"` for `d` in `range docs`, write through the shared slice and change the collection, exactly as Go does (Go's copy shares the backing array). Also allowed: **re-pointing** a borrowed `string` or `[]T` variable as a local borrow (`s = s[1:]`, [PAR-2](05-parameters.md)); it still cannot be appended to. Re-pointing holds for parameters, range variables and assertion bindings alike, because it only changes the local borrow. The data-mutability rule above holds for parameters and range variables alike.

One exception: the first write to a **nil** borrowed map creates the map in the owner's variable (`m[k] = v` where `m` is nil). A borrowed map parameter or map range variable refers to the owner's map header in place, so the owner sees the new map. The same holds for a map field, or an array element held by value, inside a borrowed variable (`func f(d Doc) { d.Index["x"] = 1 }` with a nil `Index` creates the map in the caller's `Doc`). Creating a nil map anywhere inside a borrowed variable is the one header change a borrowed variable may make. Creating an empty map doesn't change what any reader sees; the entry the write adds is an ordinary map-entry write, so the read-only rule keeps its purpose. A type-switch or assertion binding is different: the map lives in the interface's shared, immutable box, so the map is read-only through it. Writes (`v[k] = x`, `v[k]++`, `delete(v, k)`, `clear(v)`) are compile errors, nil or not, and so are writes to the elements of a slice binding (`v[0] = 1`). Fixes: `m := v.Clone()`, or store a pointer in the interface ([OWN-P9](#43-pointers-and-heap-allocation-d17), [TYP-5](03-types.md), ADR-0018 amendment point 5).

Go lets you assign to a range variable, but that changes only a copy. A writable borrow here would silently change the collection, or write into an immutable box (TYP-5), so the top-level writes are rejected. This rejects some Go code: that is a listed difference. The error message suggests the `&` form first (`for _, &d := range docs`), because that is the way to change the element.

Fix-its: use the `&` form (`for _, &d := range docs { d.Title = "x" }`); write through the element (`docs[i].Title = …`); modify a copy (`c := d.Clone()`); for a map write back after cloning (`m[k] = c`); or take ownership (`d.Clone()`, `range move(xs)`, `move(m[k])`).

```go
for i, d := range docs {          // d is a borrowed variable
    d.Title = "x"                 // error: read-only (fix-it: for _, &d := range docs)
    d.Upper()                     // error: pointer-receiver call (implicit &d)
    p := &d                       // error
    q := &d.Buf                   // error: a field held by value
    d.Buf.Reset()                 // error: implicit &d.Buf
    d.Ptr.Reset()                 // ok: goes through a pointer field
    d.Tags[0] = "y"               // ok: goes through a slice
}
for _, &d := range docs { d.Title = "x" }            // ok: d is the element itself
for k, &v := range m { v.Count++ }                   // ok: writes the map value in place
for i := range docs { docs[i].Title = "x" }          // also ok: write through the element
for _, d := range docs { c := d.Clone(); c.Title = "x"; out = append(out, c) }   // fix-it: modify a copy
for k, d := range m {
    c := d.Clone(); c.Title = "x"; m[k] = c          // fix-it (maps): write back after cloning
}
for k, n := range map[string]int{"a": 1} { n++ }     // ok: an int value is not a borrowed variable
for _, row := range grid { row[0] = 0 }              // ok: writes through the shared slice, as in Go
for _, d := range docs { d.Tags[0] = "x" }           // ok: through a slice; changes docs[i].Tags[0]
for _, s := range names { s = s[1:]; use(s) }        // ok: re-pointing a borrowed string
```

**OWN-R3c.** *Moving a variable that is still captured or borrowed.* Moving a local (`b := a`, `T{F: a}`, `ch <- a`, a `move` argument, `move(a)`) while something still refers to it is a **compile error**, because the later read would see the zero value ([OWN-R5](04-ownership.md#42-ownership-rules-d14)) where Go sees the value. Counted as "still refers":
- a **closure that captured `a` and may still run**: the closure's later call counts as a use after move. For a non-escaping closure, the error is reported when the closure value is used after the move; for an escaping closure (shared cell, [CLO-3](11-closures.md)), a closure used with `go`, or a deferred function literal, the move is always an error (conservative: the compiler cannot see the later call);
- a **pending deferred call that holds `a`**. There are three kinds. (1) A call that borrows `a` as an argument: after `defer fmt.Println("finished", tag)`, `t := tag` is an error ([CLO-7](11-closures.md)). (2) A call that is given `&a`: after `defer flush(&a)`, moving `a` is an error. (3) A pointer-receiver call on `a`, which holds `&a`: `var b Buffer; defer b.Print(); out := b` would make `Print` read the zero value, where Go prints the value. In all three kinds, writes, assignments and pointer calls on `a` stay allowed ([PAN-3](09-errors-and-panics.md)).

Not counted (allowed, because the variable is not left with a zero value that the closure could read):
- a move where the **same place receives a value in the same statement**: the implicit moves of [OWN-R4](04-ownership.md#42-ownership-rules-d14) (`out = append(out, "b")`) and swaps (`x, y = y, x`);
- `return a` of a **local**, when `a` is seen only by pending deferred calls and non-escaping closures. Per [OWN-R10](04-ownership.md#42-ownership-rules-d14) the local is built in the result slot, and these run before the function has finished returning and see the result slot, so `s := f(); defer log.Println("ret", s); return s` is legal. If `a` is captured by an **escaping** closure (a shared cell, [CLO-2](11-closures.md)), `return a` still counts as a move and is a compile error, fix-it `a.Clone()`: `func mk() (string, func() string) { name := "ann"; get := func() string { return name }; return name, get }` is an error, and `return name.Clone(), get` is the fix;
- `return res` of a **named result** that a deferred literal captured: the result slot is the variable, so nothing is moved out from under the deferred closure ([CLO-3b](11-closures.md)).

Fix-its for the other moves: `a.Clone()` at the move site, or capture or pass a clone to the closure or the deferred call.

```go
name := "ann"
hello := func() { fmt.Println("hello", name) }
u := User{Name: name}        // error: name is captured by hello, which is used below
hello()
v := User{Name: name.Clone()} // ok: fix-it

add := func() { out = append(out, "c") }
out = append(out, "b")       // ok: out receives the value in the same statement (OWN-R4)
x, y = y, x                  // ok: a swap
func f() (res string) { defer func() { res += "!" }(); res = "x"; return res }   // ok: named result

tag := job.Tag
defer fmt.Println("finished", tag)   // borrows tag
t := tag                             // error: tag is borrowed by a pending deferred call
```

**OWN-R4.** *Explicit and implicit moves.*
- `move(place)` takes the value out of any place and leaves the zero value. For a Go++ type with no zero value ([R9](04-ownership.md#42-ownership-rules-d14)), `move` out of a place other than a local variable is a compile error (the place would be left as zero bytes): use `mem.Replace(&place, v)`, `delete`, `move(m[k])`, `s = s[:n]` or `range move(xs)`. A *local* here is a variable declared in the function: `:=` and `var` variables, comma-ok results of a map or channel lookup (`v, ok := clone(m[k])`, `v, ok := move(m[k])`, `v, ok := <-ch`), owning range variables (`range move(xs)`) and `move` parameters. A type-assertion or type-switch binding is not one: it borrows the interface's box (OWN-R3b), so take a copy with `u.Clone()`. Inside a closure, a captured variable is not a local ([CLO-9](11-closures.md)), so moving it is an error for such a type (the next call would read zero bytes). The rule looks at the type of the place itself: `x.items = append(x.items, v)` moves a `[]User`, which has a zero value, so it is fine. A swap, or any assignment that refills every place it empties in the same statement, is allowed: `xs[i], xs[j] = xs[j], xs[i]`, `c.A, c.B = c.B, c.A`. In such a swap, all index, map-key and pointer operands and their bounds and nil checks are evaluated before any value is moved, so the statement can't panic halfway and leave a place empty.
- The move is **implicit** when the same place is passed to a `move` parameter and also receives the result, in one statement: `p = append(p, …)`, `buf = strconv.AppendInt(buf, n, 10)`, `x.items = append(x.items, v)`.
- Anywhere else, write `append(move(p), …)` or `append(p.Clone(), …)`.
- **Map entries.** `m[k] = append(m[k], v)` takes the value **in place**: the slot holds the zero value until the result is stored back. The entry is never removed and re-inserted. If `k` is absent, the result is a normal insert (the key rule of [OWN-V4a](04-ownership.md#44-views-d16)). Inside `range m`, any `m[x] = …`, `m[x]++`, `m[x] op= …` or `m[x] = append(m[x], …)` where `x` is **not the current range key variable, or is the range key variable but was written earlier in the body** (for a trivial key type the range key is a writable copy, so after `k++` the name `m[k]` no longer names the current entry) is a visible potential insert (the compiler cannot know whether `x` exists), so it is a compile error ([RNG-3](08-slices-and-range.md), D1.21). Only `m[k]` with `k` the range key variable, not written earlier in the body, is a write to the current entry (defined). Example: `for k := range m { m[k] = 1; k++; m[k] = 2 }` accepts the first write and rejects the second. Fix-it: collect the changes and apply them after the loop. Inserts the compiler cannot see panic at run time ([RNG-4](08-slices-and-range.md)).
- Spread (`append(a, b...)`) and `copy(dst, src)` with non-trivial elements need `move` or `Clone` on the **source**: `append(a, move(b)...)`, `append(a, b.Clone()...)`, `copy(dst, src.Clone())`.
- Moves out of places happen **after** all other arguments are evaluated.

```go
x.items = append(x.items, v)       // implicit move of x.items; v moves too if owned
y := append(x.items, v)            // error: write append(x.items.Clone(), v) or append(move(x.items), v)
all = append(all, move(more)...)   // elements of more move into all
```

*Why (R4):* `p = append(p, …)` is the most common slice idiom in Go; the implicit move keeps it unchanged, while every other form that would hide a copy or a take must say `move` or `Clone`.

**OWN-R5.** *Zero values.*
- Every type has a zero value (as in Go), except the types with none of [R9](04-ownership.md#42-ownership-rules-d14) (the pointer kinds, C++ classes without a default constructor, structs with a `@Required` field or a required field of a `@Strict` struct, and structs or arrays containing any of them). A moved-from place holds the zero value; for C++ classes see R9, and a moved-from place of a `@Required` or `@Strict` struct holds the all-zero bytes, which only drop and assignment touch (a read is a use after move).
- A `drop` method must accept the zero value. `gopp vet` checks this. Drop glue skips an all-zero value, so a `drop` never sees a never-nil `@Required` field as nil ([TYP-15](03-types.md)).
- Drop glue skips a type's `drop` when the value is all zero bytes, so dropping a zero value behaves the same in every build profile.

```go
type Cache struct{ entries []string }
func (c *Cache) drop() {            // must accept the zero value
    flush(c.entries)                // flush(nil) must be harmless
}

a := Cache{entries: load()}
b := move(a)               // b owns the entries; a is now the zero Cache
// a is all zero bytes: its drop is skipped, so no double free
a = Cache{}                // legal: a is usable again
```

*Why (R5):* a moved-from place always holds zero bytes (the zero value, where the type has one), so it needs no drop flag. The same holds for extended locals ([PAN-3](09-errors-and-panics.md)): the defer record that was created when the `defer` ran is the only flag, and there are no extra drop flags. If no such `defer` ran on the path, the local is destroyed at the end of its block as usual: the existence of the defer record is what tells the block end whether to drop the local; no extra drop flag.

**OWN-R6.** *Copies are explicit: `x.Clone()`.*
- Every type that can be cloned has a method `Clone() T`. Built-in types have it too: `string`, `[]T`, `map[K]V`, `Unique[*T]`, numbers and the other types of [OWN-C1](#41-type-categories-d13). So `s2 := s.Clone()` and `u := User{Name: name.Clone()}` work for every cloneable `s` and `name`.
- `clone(x)` is a short form: it calls `x.Clone()`. `move(x)` stays a built-in function. Examples and fix-its prefer `x.Clone()`.
- A method named `Clone` with any other signature than `Clone() T` is legal Go (for example `func (c *Config) Clone() *Config` or `(*Request).Clone(ctx)`). `x.Clone()` calls that method, but the type `Config` is not cloneable: `clone(x)` and the `Clone` constraint ([GEN-2](10-generics.md)) reject it with a compile error. A `Clone` declared on `*T` itself is `*T`'s own method: `cfg.Clone()` on a `*Config` calls it, exactly as in Go. Its signature `Clone() *Config` matches `Clone() T` with `T = *Config`, so `*Config` satisfies `[T Clone]`.
- If the method set of a struct already has a `Clone` (declared, or promoted from an embedded field), the compiler provides none. The type is cloneable only if that `Clone` has the signature `Clone() T` for the outer type `T`. A promoted `Clone` returns the embedded type, so the outer type is usually not cloneable unless it declares its own. For example `type S struct{ *tls.Config }`: `s.Clone()` calls the promoted `(*tls.Config).Clone()`, as in Go, and `S` is not cloneable; `type W struct{ *http.Request }`: `w.Clone(ctx)` works as in Go.
- Clone is structural when all fields are cloneable: the compiler provides `Clone` and calls the `Clone` of each field. A `Clone` written by hand replaces it. A hand-written `Clone` with the exact cloneable signature `Clone() T` has a **value receiver**: `func (h Handle) Clone() Handle`. The receiver is borrowed as usual ([PAR-1](05-parameters.md)), so it only reads. `func (h *Handle) Clone() Handle` is a compile error, fix-it "use a value receiver". This rule covers only the signature `Clone() T`.
- Shared handles copy by retaining: `x.Clone()` on a `Shared[*T]`, a channel, an interface value or a func value is the same as a copy.
- A trivial `x` is cloneable: `x.Clone()` is a plain copy, unless its method set already has a `Clone` with another signature (for example `struct{ *tls.Config }`: trivial, but not cloneable). Numbers, bools and the other built-in trivial types are always cloneable. This lets generic code with `[T Clone]` take `int`.
- **Pointer kinds and `Clone`.** `u.Clone()` on a `Unique[*T]` allocates a new `Unique[*T]` holding `(*u).Clone()` (it needs a cloneable `T`), and `s.Clone()` on a `Shared[*T]` retains: for these two the pointer kind's own `Clone` wins over a promoted `T.Clone` (Go has no such types, so no Go code changes meaning). On a raw `*T` where `T` has a **value-receiver** `Clone() T` (which Go promotes into `*T`'s method set), `p.Clone()` is a **compile error** for ambiguity, because in Go `p.Clone()` calls that method (the one `T` or `*T` declares), not a pointer copy. (A `Clone` declared on `*T` itself is not ambiguous: it is called.) Fix-its: "write `(*p).Clone()` to copy the T, or `q := p` to copy the pointer". If `T` has no `Clone` method, `p.Clone()` copies the pointer. `[T Clone]` with `T = *Handle` (where `Handle` has a value-receiver `Clone`) is satisfied by the pointer copy, while a direct `p.Clone()` is the ambiguity error. The difference is intended: generic code sees `*Handle`'s own method set.
- **Interface types.** On an interface type that declares a `Clone()` method, `x.Clone()` dispatches to the dynamic method, as in Go. The built-in retaining `Clone` applies only to interface types that do not declare `Clone`.
- A type with a `drop` method is cloneable only if it defines `Clone() T`. This stops silent duplication of unique resources such as file descriptors. There is no `Clone` derive: a type with `drop` writes `Clone()` by hand, on purpose.
- A type that mixes owning fields with raw `*T` fields needs a hand-written `Clone()`, because an automatic clone would copy the raw pointers (for example a tree `Node` with a parent pointer).
- Imported C++ types clone through their copy constructor.

```go
type Handle struct{ fd int; path string }       // has a drop method below: not cloneable by default
func (h *Handle) drop() { sys_close(h.fd) }

func (h Handle) Clone() Handle {                 // value receiver, written by hand, on purpose
    return Handle{fd: sys_dup(h.fd), path: h.path.Clone()}
}
type Tree struct{ Kids []Tree; Up *Tree }        // mixes owning and raw fields:
// t.Clone() is an error; write Tree.Clone() so Up is fixed up instead of copied

a := h.Clone()             // calls Handle.Clone
b := someShared.Clone()    // a Shared[*T]: retains
c := clone(a)              // the short form: the same call
k := 5
n := k.Clone()             // numbers are cloneable too: a plain copy
```

*Why:* an automatic clone of a file descriptor would give two owners to one resource, and an automatic clone of a raw pointer would alias.

**OWN-R7.** *Destructors are a method named `drop`* with a pointer receiver: `func (f *File) drop()`. It cannot be called directly (like `init`).

```go
type File struct{ fd int }

func (f *File) drop() {
    if f.fd != 0 { sys_close(f.fd) }   // must accept the zero value
}
```

*Why (R7):* a method named `drop` is found by the compiler like `init`; forbidding direct calls prevents double destruction.

**OWN-R8.** *Borrow checking without annotations.* Views and pointers into storage that can move or be freed are checked by flow and liveness ([OWN-B1 to B7](#45-borrow-checking-adr-0021), ADR-0021). The compiler also catches: returning `&local`; returning a view into a local ([OWN-V7](#44-views-d16)); storing a borrow of a temporary; a `go` or a `defer` that borrows something that dies before the function returns (a borrowed parameter lives until return, so `defer fmt.Println("done", name)` is legal; a borrowed loop variable does not) ([PAN-3](09-errors-and-panics.md), [CON-2](12-concurrency.md)); use after move, including a use through a pointer or view taken before the move in the same function (`p := &x.In; take(move(x)); p.P.V`; taken elsewhere it is [UB-8](16-undefined-behavior.md)).
- What is left undefined is listed in [chapter 16](16-undefined-behavior.md): `unsafe`, C and C++ interop, aliasing the compiler cannot see, data races in `unsafe`, C and C++ code, `longjmp`.
- `-asan` catches the rest at run time.

```go
p := &local                // allowed
return &local              // compile error: caught
s := name[:2]              // view
name = "other"             // compile error: s is borrowed and used below (OWN-B2)
fmt.Println(s)
```

*Why:* a view is only dangerous when its owner changes while it is still used; flow and liveness see that without lifetime annotations.

**OWN-R9.** *C++ class types* (and other types with no zero value).
- A C++ type has a zero value only if it has a default constructor. The pointer kinds `*T`, `Unique[*T]`, `Shared[*T]` and `Weak[*T]` have none, and neither have `func` types nor interface types with methods other than `error` (they are never nil, [OWN-P9](#43-pointers-and-heap-allocation-d17)): `var l Logger` and `var f func()` are errors. `error` and the empty interface `any` keep their zero value `nil`, as do `unsafe.Pointer`, maps, slices and channels. A struct with a `@Required` field has none either ([TYP-15](03-types.md)), nor has a `@Strict` struct with a required field ([TYP-15a](03-types.md)), nor has any struct or array that contains a type with no zero value. `Option[T]` always has one: `nil`.
- Where Go needs a zero value and there is none, it is a compile error: `make([]T, n)` (a constant length `0` is allowed: `make([]T, 0, c)` creates no value), `m[k]` and `<-ch` outside a comma-ok form (see below), `clear(s)` on a slice (use `s = s[:0]`; `clear(m)` on a map is fine), a receive from a closed channel, `var zero T` in generic code, named results. A failing `!` function leaves such results unset ([ERR-3](09-errors-and-panics.md)), and a caller's `v, err := f()` can then use `v` only where `err == nil` is known ([OWN-P10](#43-pointers-and-heap-allocation-d17)).
- Every move of a C++ value, implicit or `move(x)`, runs its **move constructor**. The source keeps the C++ "moved-from" state (not zero) and is destroyed at the end of its scope.
- Only containers relocate values, with the type's relocate operation (P05 D5.5): `memcpy` only for Go++ types and trivially relocatable C++ types.
- **Lookups that can fail.** For a type with no zero value, a map index `m[k]` or a receive `<-ch` whose value type has no zero value may be used only in a comma-ok form (`v, ok := m[k]`, `v, ok := clone(m[k])`, `v, ok := move(m[k])`, `v, ok := <-ch`, `case v, ok := <-ch:` in a `select`), as the target of `m[k] = v`, in `delete(m, k)`, or as a bare `move(m[k])` statement that discards the value; every other use (`v := m[k]`, `u := clone(m[k])`, `u := move(m[k])`, `m[k].Email`, `f(m[k])`, `T{F: m[k]}`, `v := <-ch`, `case v := <-ch:`, `(<-ch).Email`) is an error, because a missing key or a closed channel would give zero bytes; the comma-ok forms (`u, ok := x.(T)`, `u, ok := m[k]`, `u, ok := clone(m[k])`, `u, ok := move(m[k])`, `u, ok := <-ch`) work, and `u` is usable only where `ok` is known to be true (flow typing, [OWN-P10](#43-pointers-and-heap-allocation-d17)): reading it where `ok` may be false is an error. A type switch and the single-value `u := x.(T)` work as in Go.

```go
var v vector.vector[int32]   // has a default constructor: zero value exists
w := move(v)                 // runs the move constructor; v is moved-from (not zero), destroyed at scope end
xs := make([]Foo, 3)         // compile error if Foo (a C++ class) has no default constructor
```

*Why:* C++ classes keep their own move and destroy rules, so interop needs no wrapper and no surprise zero values.

**OWN-R10.** *Pinned values.*
- `gopp vet` reports moving a local after its address, or a view into it, has been stored. A use through a pointer or view taken in the same function before the move is a compile error ([OWN-R8](#42-ownership-rules-d14), P04.09); one taken elsewhere is [UB-8](16-undefined-behavior.md).
- Types marked `@Pinned` (the `sync` types, including `Mutex[T]`) cannot be moved or copied after first use: a compile error, like Go vet's copylocks.
- `return local` builds the value in the caller's result slot, so it does not move (see the escaping-closure case in [OWN-R3c](04-ownership.md#42-ownership-rules-d14)). When different locals are returned on different paths, at most one is built in the result slot; returning the others moves them, and OWN-R3c then applies to them. The tie-break is fixed: a local that a pending deferred call (or a non-escaping closure) still uses gets the slot. If two returned locals both have pending users, returning the second one is an OWN-R3c error, fix-it `.Clone()`. Otherwise the choice does not change behaviour, because the others just move. Example: `a := f(); b := g(); defer log.Println(a); if c { return a }; return b`. Here `a` gets the slot, and `return b` moves `b` (fine, nothing pending uses `b`).

```go
mu := sync.NewMutex(0)     // @Pinned
g := mu.Lock()
m2 := mu                   // compile error: a Pinned value cannot be copied after first use

p := &local
ptrs = append(ptrs, p)     // address stored
other := move(local)       // gopp vet reports: local moved after its address was stored
func mk() Big { r := Big{}; return r }   // r is built in the caller's slot: never moves
```

*Why:* locks and addresses must not change place; moving them would break users that hold the old address.

### Example: the whole model on one screen

```go
type Node struct {
    Name     string          // owned
    Children []Unique[*Node]  // owned slice of owning pointers
    Parent   Option[*Node]   // raw back-pointer: does not own; nil for the root
}

func NewNode(name string, parent Option[*Node]) Unique[*Node] {
    return new_unique(Node{Name: name.Clone(), Parent: parent}) // name is borrowed: Clone it to own it
}

func (n *Node) Add(move child Unique[*Node]) {
    child.Parent = n
    n.Children = append(n.Children, child)   // child moves into the slice (R3, R4)
}

func main() {
    root := NewNode("root", nil)
    root.Add(NewNode("a", root))   // root (a Unique place) converts to *Node, then to Option[*Node]; the temporary moves into Add
    b := NewNode("b", root)
    root.Add(b)                    // b moves
    // fmt.Println(b.Name)         // compile error: b used after move
    head := root.Name[:2]          // View[string]: a borrow, no allocation
    fmt.Println(head, len(root.Children))
}                                  // root is dropped: the whole tree is freed
```

## 4.3 Pointers and heap allocation (D1.7)

Every heap allocation is spelled out. Go's `new` is removed. (The runtime's own allocations are not spelled in source: for example the defer records of deferred calls in loops, which hold extended locals, [PAN-3](09-errors-and-panics.md).)

| Type | Created with | Meaning |
|---|---|---|
| `*T` | `&x` (address of a variable) | raw, non-owning, **never nil** (Go syntax, no GC) |
| `Unique[*T]` | `new_unique(v)` | single owner; the value is dropped and freed when the pointer is destroyed; never nil |
| `Shared[*T]` | `new_shared(v)` | shared ownership, atomically refcounted; one allocation holds the counts and the value (like `make_shared`) |
| `Weak[*T]` | `new_weak(s)`, from a `Shared[*T]` | does not own, does not allocate; `w.Lock()` returns an `Option[Shared[*T]]`, `nil` if the value is gone |
| `Option[T]` | `nil`, or any `T` (converts automatically) | "maybe nothing", for any type; zero value `nil`; the only nilable form of a pointer, a func or an interface with methods (other than `error`) |

No pointer kind is ever nil, and none has a zero value ([OWN-P9](#43-pointers-and-heap-allocation-d17), ADR-0018). Owning pointers name their pointer type: `Unique[*User]`, not `Unique[User]` (that is an error with the fix-it).

**OWN-P1.** `*T` is a raw pointer. `&x` takes the address of an addressable operand (a variable, field or element), as in Go. A raw pointer is trivial and never frees anything.

**OWN-P2.** `new_unique(v)` and `new_shared(v)` take the initial value and **move it in**, and return `Unique[*T]` / `Shared[*T]`: `new_unique(Node{Name: "a"})`, or `new_unique(Node{})` for a zero value.

**OWN-P3.** `new_weak(s)` takes a `Shared[*T]` (borrowed). A weak pointer cannot own a new object, because it would be freed at once.

**OWN-P4.** `&T{…}` is a compile error with a fix-it: "allocation is explicit in Go++: use `new_unique(T{…})`". In Go it allocated silently.

**OWN-P5.** Field access and method calls **auto-dereference** `*T`, `Unique[*T]` and `Shared[*T]`. `Weak[*T]` does **not** (ADR-0017): its target can be freed at any moment, so `w.Name` or `w.Method()` is a compile error with the fix-it `if p := w.Lock(); p != nil { p.Name }`. A `Weak[*T]` has only its own method, `Lock`. `Unique[*T]` and `Shared[*T]` get the methods of both `T` and `*T`, except `Clone`, where the pointer kind's own method wins ([OWN-R6](#42-ownership-rules-d14)). On a raw `*T`, `p.Clone()` is an error if `T` has a value-receiver `Clone() T` (OWN-R6). Pointers have no `IsNil` method: they are never nil.

**OWN-P6.** `Unique[*T]` and `Shared[*T]` convert **implicitly** to `*T` when borrowed from a place, and so to `Option[*T]`; an `Option[Unique[*T]]` or `Option[Shared[*T]]` place converts to `Option[*T]` the same way. They never convert from a temporary, because the target would be freed immediately.

**OWN-P7.** `make([]T, n)`, `make(map[K]V)` and `make(chan T, n)` work as in Go (with the allocator rules of [SLC-2](08-slices-and-range.md)).

**OWN-P8.** `Unique[*T]` is owning (move-only). `Shared[*T]` and `Weak[*T]` are shared handles: copies retain the strong or weak count. `Weak[*T]` copies retain the weak count only.

**OWN-P9.** *Never nil, and `Option[T]`* (ADR-0018). `*T`, `Unique[*T]`, `Shared[*T]` and `Weak[*T]` always hold a target and have no zero value ([OWN-R9](#42-ownership-rules-d14)): `var p *User` and `p == nil` on a pointer are compile errors. The same holds for all `func` types and for every interface type with at least one method, because calling a nil one crashes like a nil pointer: `var l Logger`, `var f func()`, `l == nil` and `f == nil` are compile errors, and a struct with such a field has no zero value unless the field is an `Option`. **Two interfaces keep Go's `nil`: `error`, and the empty interface** (`any`, `interface{}`, or any interface type whose method set is empty, named or not, such as `type Thing interface{}`). So `var err error`, `if err != nil { return err }`, `var x any`, `make([]any, n)`, `m[k]` on a `map[string]any` and `x == nil` work as in Go. The `error` exception covers `error` and its aliases (`type E = error`) only: a defined `type MyErr error` and an interface that embeds `error` and adds methods (`interface{ error; Code() int }`) are interfaces with methods, so they are never nil. `unsafe.Pointer` keeps Go's `nil` too (it stands for a C `void*`, which may be NULL). "Maybe nothing" is `Option[T]`, for **any** other type `T`:
- its zero value is `nil`, and `o = nil` empties it; two `Option[T]` compare equal when both are nil or both are set and equal, if `T` is comparable (`Option[*T]` by address), so `Option[T]` can be a map key and `o == 5` means `o == Option[int](5)` (TYP-3); a `T` converts to `Option[T]` automatically (`var o Option[*User] = &bob`, `var n Option[int] = 5`);
- for the pointer kinds, funcs and interfaces with methods, `nil` uses the value's own nil bit pattern, so `Option[*T]` has the size and layout of a C pointer and `Option[Logger]` / `Option[func()]` cost nothing extra; for other types it adds a hidden "is set" flag;
- it has exactly two methods, `IsNil() bool` and `Get() (T, bool)`, and no fields: the value's fields and methods are reached only after a check (OWN-P10), so no name of `T` can clash with them;
- `error`, `any` (the empty interface), `unsafe.Pointer`, maps, slices and channels keep Go's `nil` (a nil slice is empty; a nil map is an empty map, and the first write creates it instead of panicking; a nil channel blocks as in Go, and `close` of a channel known to be nil is a compile error; ADR-0018 amendment point 5). Because they already have `nil`, `Option[T]` is a compile error when `T` is one of them or is itself an `Option` (`Option[error]`, `Option[any]`, `Option[unsafe.Pointer]`, `Option[[]int]`, `Option[Option[int]]`); in generic code this is checked per instance. `Option[Logger]` and `Option[func()]` are the "maybe" forms of an interface and a func;
- **Where a nil map is created (ADR-0018 amendment point 5).** The first write to a nil map creates it **in place** when the map is held in a *place*: a local or package variable; a field (of an owned value, or reached through a pointer); an element or map entry; an `&` range alias; a borrowed parameter or a borrowed range variable. Creating a nil map anywhere inside a borrowed variable (the variable itself, or a field or array element of it held by value) is the one header change a borrowed variable may make ([OWN-R3b](#42-ownership-rules-d14)): `func f(d Doc) { d.Index["x"] = 1 }` with a nil `Index` creates the map in the caller's `Doc`. It works because borrowed owning parameters pass by hidden pointer ([PAR-10](05-parameters.md)) and range variables refer to the element in place. Creating an empty map doesn't change what any reader sees; the entry the write adds is an ordinary map-entry write. A map reached through an interface is read-only and not a place (next bullet).
  - **Map inside a map:** `mm[k1][k2] = v` on a writable outer map. If `k1` is absent, the write **inserts** `k1` with a new inner map, then writes `k2`. If `k1` is present with a nil inner map, the inner map is created in the entry. Inserting a new key counts as an insert into `mm` for the active-iterator rule ([RNG-4](08-slices-and-range.md)).
  - **Not in a place: a compile error, never a panic.** Writing into a map **held in a temporary** (the result of a call that returns a value, an assertion, or a field or element of one) is an error, nil or not: `f()["a"] = 1`, `f()["a"]++`, `delete(f(), "a")`, `f()["k1"]["k2"] = v`, and `f().Index["a"] = 1` where `f` returns a struct by value. The owned result is dropped at the end of the statement, so the write is always lost; store it first: `m := f(); m["a"] = 1`. If `f` returns `*Doc`, `f().Index["a"] = 1` is allowed (a pointer target is a place). A map **inside an interface** is read-only, because the box is shared and immutable ([TYP-5](03-types.md)): through a type-switch binding or an assertion, reading, `len` and `range` are allowed, but `v["b"] = 2`, `v["b"]++`, `delete(v, "a")`, `clear(v)` and `x.(map[string]int)["b"] = 2` are errors. The elements of a slice in the box are read-only too (`v[0] = 1` on a `[]int` binding is an error). Inside an interface means everything the box holds **by value**, all the way down until a pointer: the map or slice itself, and the maps and slices in struct fields and array elements of the boxed value. So `v.(Doc).Index["x"] = 1`, `case Doc:` with `d.Index["x"] = 1`, and `d.Tags[0] = 1` are errors. A `*T` field in the boxed struct still gives a writable pointee. Fixes: `m := v.Clone(); m["b"] = 2`, or store a pointer in the interface (`var x any = &myMap`, then `(*p)["b"] = 2` through `case *map[string]int`; a pointer is never nil, so the write creates a nil map in place). So Go++ has no run-time nil-map panic.
  - **Concurrency:** two goroutines doing the first write to the same nil map is a data race on the map header, like any unsynchronized map write. In normal code it is a compile error: a map is `Sync` only for reading, so it cannot be shared and written ([CON-12](12-concurrency.md), [UB-11](16-undefined-behavior.md)).
- converting an `Option[T]` to `any` or `error` gives a nil interface when it is nil and the boxed `T` when it is set, so `fmt.Println(o)` prints `<nil>` or the value, and a type switch never sees an `Option`. An interface with methods is never nil, so for such an `I` the conversion is to `Option[I]`: nil when `o` is nil, the boxed `T` when it is set;
- ownership follows `T` ([OWN-R3](#42-ownership-rules-d14)): a checked `o` is used like any variable of type `T`. `v, ok := o.Get()` on an owning `T` (`Option[string]`, `Option[Unique[*T]]`) **borrows**: `v` is a read-only borrowed variable while `o` is unchanged. In the same scope, a `:=` that reuses the name `v` assigns to `v`, so it is an error; use a new name. `v, ok := move(o).Get()` takes the value and leaves `o` nil, and `clone(o).Get()` copies it. A checked owning `o` can be moved out with `move(o)`, which leaves it nil.

**OWN-P10.** *Checking an `Option`.* An `Option[T]` is usable as a `T` only where the compiler knows it is set:
- after `o != nil` or `!o.IsNil()`: in the `if` body, after an early `return`, `break` or `continue` on `o == nil` / `o.IsNil()`, and in the body of `for …; o != nil; …`; checks joined with `&&` (`a != nil && b != nil`), or an early exit on checks joined with `||` (`if a == nil || b == nil { return }`), narrow each operand; in `for n := head; n != nil; n = n.Next`, the body **and the post statement** see `n` as checked, unless the body assigns `n`;
- or through `v, ok := o.Get()`: when `T` has no zero value, `v` is usable only where `ok` is true (the comma-ok rule below).

The same flow typing checks two more kinds of value:
- `v, err := f()` for every call whose result list ends in `error`, when `v`'s type has no zero value: `v` is usable only where `err == nil` is known (after `if err != nil { return err }`, inside `if err == nil { … }`) ([ERR-3](09-errors-and-panics.md));
- the comma-ok forms `v, ok := m[k]`, `v, ok := <-ch`, `v, ok := o.Get()` and `v, ok := x.(T)` when the value type has no zero value: `v` is usable only where `ok` is known to be true (after `if !ok { return }`) ([OWN-R9](#42-ownership-rules-d14)).

Such a value can be checked only in a local variable of the function. Storing it straight into a field, an element, a package variable or a captured variable (`s.F, err = f()`), or handing it on as it is (`h(f())`, `h(o.Get())`, `return o.Get()`), is a compile error. The one exception is `return f()` into results that end in `error` at the same place, because the caller checks them in turn: `(T, error)`, or in a `!` function the declared results plus the failure ([ERR-3](09-errors-and-panics.md)).

A value stored in a never-nil `@Required` `error`, `any`, map or channel field must be known non-nil; [TYP-15](03-types.md) has the full list (a literal, `make(…)`, a local checked with `!= nil`, a value of a never-nil type converted to `error` / `any`, …).

Narrowing works on local variables and parameters. A field or element is copied into a local first (`if o := c.Owner; o != nil { … }`), because it could change between the check and the use. A variable captured by a function literal is not a local of the literal ([CLO-9](11-closures.md)): the literal can run at any time, so inside it a captured `Option` or checked result is never known set, neither from a check outside the literal nor from one inside it; copy it into a local first (`v := o; go func() { use(v) }()`, or `if v := o; v != nil { … }` inside the literal). An assignment to `o` ends the narrowing, and so does `move(o)`, which leaves `o` nil (after it, using `o` as a `T` again, including `move(o)` into a `T`, is an error until `o` is checked again). Where `o` is checked it simply is a `T`, so a copy made there is a `T` too: after `if a != nil`, `p := a` (or `var p = a`) gives `p` the type `*Node`, which is never nil, while `var p Option[*Node] = a` keeps the declared `Option`. Using an unchecked `Option` as its value is a compile error: "`o` may be nil; check `o != nil` first". Where `o` is checked, taking a reference into its value's own storage is a compile error too, even for a use within the statement (`sum(o.Arr[:])`), because the reference could be kept past `o = nil`, which leaves zero bytes under it: `&o.F`, a pointer-receiver method value `o.M` or `go` / `defer o.M()`, `o.Arr[:]` of an array held by value, `for _, &e := range o.Arr`, and the same made from a variable that borrows a part of the value (a range variable over owning array elements, a `Get` binding). Copy `o` first (`v := o`). Not affected: an immediate call `o.M()` (it borrows the receiver for the call only), storage reached through a pointer (`Option[*T]`, a pointer field), and a buffer the value owns (`o.Items[:]` of a slice, `o.Name[1:]`; a view into it after `o = nil` dangles, [UB-1](16-undefined-behavior.md), as for any owned local).

```go
func greet(u Option[*User]) {
    if u == nil { return }
    fmt.Println(u.Name)              // u is a *User after the early return
}

n := find(7)                         // Option[User]
if v, ok := n.Get(); ok { use(v) }   // comma-ok
if !n.IsNil() { use(n) }             // IsNil narrows the same way

var name Option[string] = "bob"
if s, ok := name.Get(); ok { fmt.Println(s) }   // s borrows: name keeps its string
if s, ok := move(name).Get(); ok { keep(s) }    // s owns it; name is now nil
use(n)                               // error: n may be nil; check n != nil first

var p *User                          // error: *User has no zero value (it is never nil)
p := &bob
p == nil                             // error: *User is never nil

var l Logger                         // error: Logger has no zero value (it is never nil)
var onStop Option[func()]            // an optional callback: nil until set
if onStop != nil { onStop() }        // onStop is a func() here
var err error                        // fine: error keeps Go's nil (so does any)
u, err := load()
if err != nil { return err }
fmt.Println(u.Name)                  // OK: err is nil here
```

```go
type Tree struct{ L, R Option[Unique[*Tree]]; V int }

func sum(t Option[*Tree]) int {
    if t == nil { return 0 }
    return t.V + sum(t.L) + sum(t.R)   // t is a *Tree here; t.L converts to Option[*Tree]: borrowed from a place (P6)
}

func build() Unique[*Tree] {
    return new_unique(Tree{V: 1, L: new_unique(Tree{V: 2})})   // R and the inner R are nil Options
}

s := new_shared(Config{Name: "x"})
w := new_weak(s)                 // does not keep Config alive
if p := w.Lock(); p != nil {     // Lock gives an Option[Shared[*Config]]; p is a Shared[*Config] here
    fmt.Println(p.Name)
}
bad := &Tree{V: 1}               // error: use new_unique(Tree{V: 1})
```

*Why (P1 to P10):* every allocation is spelled `new_unique` or `new_shared`, so nothing allocates behind a Go-looking `&T{}`; `*T` stays a raw, Go-looking pointer for C interop, but it is never nil, so a function that takes a `*User` always gets a `User`, and "maybe" is visible in the type as `Option[T]` (ADR-0018).

## 4.4 Views (D1.6)

Syntax: `View[X]` is a predeclared generic type ([3.1](03-types.md)). `X` may be any type except the pointer kinds (`*T`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`), `unsafe.Pointer` and `View[…]` itself. So `View[string]`, `View[[]byte]`, `View[User]`, `View[Bytes]` (a named type) and `View[map[K]V]` are fine, and `View[*T]`, `View[Unique[*T]]`, `View[unsafe.Pointer]` and `View[View[…]]` are compile errors that name the allowed forms. `View` is not a keyword; `ref` is an ordinary identifier ([LEX-12](02-lexical.md#lex-12)).

**OWN-V1.** *A `View[T]` is a read-only borrow, all the way down.* It is like Rust's `&T`. For `string` and slice types it is a `(ptr, len)` pair. A view is trivial ([OWN-C1](#41-type-categories-d13)). There is no write path through a view.
- **Nothing can be changed through a `View[T]`:** not its fields, not slice elements, not map entries, and not the target of a pointer reached through it.
- **Reading.** A value read out of a view is copied out as a plain value **only if its type is Copy and contains no pointer kind** (no `*T`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`, `unsafe.Pointer`, and no struct or array holding one). So `n := v.Count` gives an `int`, `x := vs[0]` on a `View[[]int]` gives an `int`, and a pointer-free Copy struct is copied. **Everything else read through a view is a view:** `v.Name` is a `View[string]`, `v.Tags` is a `View[[]string]`, a struct field of struct type is a `View[Inner]`, `v.Owner` (a `*User`) is a `View[User]` of the target, and a Copy struct with a pointer field is a `View[S]`.
- **Writes are compile errors** on a view or on any part of it: assignment, `++`, `op=`, `delete`, `clear`, `copy` into it, `append`, `&` of a part, calling a pointer-receiver method, and `for _, &e := range v` (it is `&` of a part, [RNG-1a](08-slices-and-range.md#83-range-loops-d121)).
- **A view never converts to a non-view type,** except by the copy-out above or by `.Clone()`, `string(v)` or `[]T(v)`. So passing a view, or a part of one that is not a copy-out value, is a compile error for any parameter whose type is not a `View`: a `[]T` parameter, a map, a `*T`, a struct containing these, and a plain `T` parameter (which [PAR-1](05-parameters.md) makes read-only only at the top level: a borrowed `User` could still write `u.Tags[0]`). A value-receiver method call on a view part and a method value are errors for the same reason. There are two exceptions. A `string` parameter has immutable bytes and so no write path ([PAR-8](05-parameters.md)). Boxing a view into an interface with no methods (`any`, `interface{}`) is allowed (`fmt.Println(w)`, `var a any = w`): the box keeps the dynamic type `View[T]`, and a type-switch or assertion binding to `View[T]` is a view, read-only all the way down ([TYP-9](03-types.md)). `View[T]` has no methods except the built-in `Clone` ([OWN-R6](04-ownership.md#42-ownership-rules-d14)), which every type has; it makes an owned copy. So a view can be boxed only into an interface with no methods (`any`, `interface{}`); boxing it into `error`, `fmt.Stringer` or any interface with methods is a compile error, and `v.String()` on a `View[User]` is an error. The box may not outlive the view's owner: that is a compile error ([OWN-B5](#45-borrow-checking-adr-0021)).
- **Methods.** `View[T]` has no methods except the built-in `Clone` ([OWN-R6](04-ownership.md#42-ownership-rules-d14)), which every type has; it makes an owned copy (see above); P04.09 may relax this with the "writes this parameter" bit.
- **Range variables.** A range variable over a view (`View[[]T]`, `View[map[K]V]`, or a view part) is itself a view, read-only all the way down, with the same copy-out rule for pointer-free Copy elements ([RNG-1](08-slices-and-range.md)). So `for _, d := range viewOfDocs { d.Tags[0] = "x" }` is an error.
- This is conservative for now. A later pass (P04.09) may relax it with a "writes this parameter" bit: a call would be allowed when the callee never writes through that parameter or receiver.

**OWN-V2.** *Slicing.* No allocation. `:=` infers the type. The result depends on what is sliced:
- Slicing a `string` or a view gives a **view**: `s[i:j]` on a `string` gives a `View[string]`, on a `View[[]T]` a `View[[]T]`.
- Slicing a slice or array that you may write (owned, or a borrowed writable one) gives a **borrowed `[]T`**. This is the same thing as a borrowed `[]T` parameter ([PAR-1](05-parameters.md), [PAR-2](05-parameters.md), [OWN-R3b](#42-ownership-rules-d14)): its elements can be written, but it cannot grow (`append` is an error), cannot be moved, and cannot outlive its owner.
- So `sort.Ints(nums[1:3])`, `r.Read(buf[n:])`, `copy(dst[2:], src)` and `s := nums[1:3]; s[0] = 9` all work.
- The one exception is re-slicing an owned slice into itself: `s = s[i:j]` shrinks it in place ([SLC-6](08-slices-and-range.md)).

**OWN-V3.** *Conversions.*
- Owned (or borrowed) to view is implicit wherever a view is expected (an argument, an assignment to a view variable, a comparison).
- View to owned is explicit and **copies**: `string(v)`, `[]T(v)`, and `v.Clone()` for any view (an owned copy; `string(v)` stays the form for a `View[string]`). `[]T(v)` needs elements that satisfy `Clone` ([GEN-2](10-generics.md)).
- A view never converts to a non-view type, except by the copy-out or by `.Clone()`, `string(v)` or `[]T(v)` (OWN-V1). A `View[[]T]` is not accepted by a `[]T` parameter. Two exceptions: a `string` parameter, and boxing into `any` or another interface with no methods (the box keeps the dynamic type `View[T]`, [OWN-V1](#44-views-d16), [TYP-9](03-types.md)).
- Unlike Go, where converting between slice types never copies, here the copy is how you get ownership.

**OWN-V4.** *Map keys.* Maps accept a view key for lookups, so `counts[w]` with `w View[string]` does not allocate. Inserting a new entry clones the key, whether it is a view key or a borrowed one (OWN-V4a).

**OWN-V4a.** *Map index on the left side.* `m[k] = v`, `m[k]++` and `m[k] op= v` **never consume** the key expression `k`. The key is borrowed for the lookup, and cloned only if a **new** entry is inserted. This holds for any key, view or not. So `for _, w := range words { counts[w]++ }` works as in Go (`w` is a borrowed range variable), and `m[s] = 1; fmt.Println(s)` is fine (`s` is not moved). The allocation (the key clone) happens only on insert, as for view keys. Inside a `range` over the same map, the rule of OWN-R4 "Map entries" applies: only `m[k]` with `k` the range key variable, not written earlier in the body, is allowed.

```go
counts := map[string]int{}
for _, w := range words { counts[w]++ }   // w borrowed: lookup borrows it; the first insert clones it
s := "k"
counts[s] = 1
fmt.Println(s)                            // ok: s was not moved
```

The key is cloned only when a new entry is added. Without this rule, `counts[w]++` over a borrowed range variable would need `counts[w.Clone()]++` and allocate on every update. The key clone on a map insert is part of the allocation list of principle 1.

**OWN-V5.** *Storing views.* Views may be stored only where the borrow checker can follow them ([OWN-B5](#45-borrow-checking-adr-0021)): a `@View` struct value, locals, parameters and results. A struct field of view type is a **compile error** (diagnostic `err_view_field_without_attr`, a new code) unless the struct is marked `@View` (for example tokens that point into a source buffer). The same holds for a field whose type contains a view: `[]View[string]`, `map[K]View[…]`, an array of views, and an embedded view.

**OWN-V6.** *Temporaries.* A view or a borrow into a temporary (`f()[1:3]`) lives only as long as the temporary: until the end of the full statement. In the header of a `for … range`, `if` or `switch` it lives until the end of the whole statement.
- **Lifetime extension.** In a `:=` or `var` declaration (`w := strings.ToUpper(s)[0:3]`, `var w = …`) the compiler keeps the temporary in a hidden local until the end of the enclosing block, like Rust and C++ temporary lifetime extension. Only `:=` and `var` declarations get this.
- Storing a view or a borrow of a temporary into a longer-lived place (a field, a global, an outer variable, a return) is still a compile error ([OWN-R8](#42-ownership-rules-d14)).

**OWN-V7.** *Returning a view into a local.* `return` of a view that points into a local variable of the same function, or into an owned value that dies at the return (an owned `move` parameter, a temporary), is a **compile error**. A view into a *borrowed* parameter or receiver is fine, because the caller still owns it ([PAR-9](05-parameters.md)). It is an error only when the returned view points into an **owned** local (or an owned value that dies at the return). The compiler checks only what it can see in the function: the owned local or `move` parameter itself, converted to a view at the `return` (implicit owned-to-view conversion), so `func f() View[string] { s := "hi"; return s }` and `func g(move s string) View[string] { return s }` are compile errors; a view made by slicing a temporary, or a temporary converted to a view at the `return` (`return load()[:2]`, or `return strings.Repeat("a", 3)` as a `View[string]`); a view made by slicing an owned local (`return s[:1]`), or by indexing or slicing an owned local's field held by value. A view-typed local or parameter that holds a view into a borrowed parameter or into the caller's data is fine (`s = s[1:]; return s` in `trim(s string) View[string]`, where `s` is a borrowed parameter, [PAR-2](05-parameters.md)). A view that comes out of a call borrows from the call's arguments ([OWN-B3](#45-borrow-checking-adr-0021)), so returning it is checked the same way. A function also cannot return a writable borrowed part of a slice (`return xs[1:3]` with a `[]T` result is an error, see [PAR-9](05-parameters.md)): to return a part without copying, the result type is `View[…]`, which is read-only.

```go
func bad() View[string] { s := "hi"; return s[:1] }   // error: view into the local s
func head(src string) View[string] { return src[:1] }   // ok: src is borrowed from the caller

@View
type Token struct {
    Text View[string]      // points into the source buffer; allowed only because of @View
    Line int
}

func first(src string) View[string] {     // result is a view into src
    i := 0
    for i < len(src) && src[i] != ' ' { i++ }
    return src[:i]
}

func main() {
    text := "alpha beta"
    w := first(text)             // view: no allocation
    owned := string(w)           // explicit copy: now independent of text
    counts := map[string]int{}
    counts[w]++                  // view key: lookup is free; the first insert clones the key
    fmt.Println(w, owned, counts)

    for _, c := range strings.ToUpper(text)[:3] { _ = c } // the temporary lives for the whole loop
    h := strings.ToUpper(text)[:3]  // ok: the temporary is kept until the end of the block (OWN-V6)
    var tok Token
    // tok.Text = strings.ToUpper(text)[:3]  // error: view of a temporary stored in a longer-lived place
    fmt.Println(h, tok.Line)

    nums := []int{3, 1, 2}
    sort.Ints(nums[1:3])         // ok: nums[1:3] is a borrowed []int, its elements can be written
    t := nums[1:3]
    t[0] = 9                     // ok: writes nums[1]
    // t = append(t, 1)          // error: a borrowed slice cannot grow
}

func total(xs View[[]int]) int {
    n := 0
    for _, x := range xs { n += x }      // ok: reading
    // xs[0] = 0                         // error: a View is read-only
    return n
}

func rename(u View[User]) {
    _ = u.Name                           // ok: u.Name is a View[string]
    // u.Name = "amy"                    // error: assignment through a View
    // u.Tags[0] = "x"                   // error: u.Tags is a View[[]string]
    // u.Owner.Age++                     // error: the target of a pointer field is read-only too
    // u.Reset()                         // error: pointer-receiver method
}
```

*Why (V1 to V7):* a view is a read-only borrow you can see in the type, so `s[i:j]` is free and a copy is always a visible `string(v)`; read-only all the way down means a `View[T]` parameter is a promise that the callee changes nothing, like Rust's `&T`; the dangling risk of a view is closed by the borrow checker ([4.5](#45-borrow-checking-adr-0021)); what remains is listed in chapter 16.

## 4.5 Borrow checking (ADR-0021)

Go++ has no lifetime annotations. The compiler finds borrows from the code and reports a **use after the borrowed thing was thrown away** as a compile error. It uses flow and liveness, like Rust's non-lexical lifetimes, but you never write a lifetime. The check runs in `gopp build` and in `gopp check` (P04.20). Data races are a separate check on the same flow graph ([CON-12 to CON-20](12-concurrency.md), ADR-0022); [OWN-B8](#45-borrow-checking-adr-0021) links the two.

**OWN-B1.** *What is a borrow.* A borrow is any of these:
- a view (`View[T]`, [OWN-V1](#44-views-d16));
- a borrowed `[]T` made by slicing ([OWN-V2](#44-views-d16));
- a pointer made by `&x` of a local variable (the local is freed at the end of its scope, or moved), or by `&` of a field or element of owned storage that can move or be freed: a slice or map element, or a field of a local that can be moved;
- a range loop over a collection: it borrows the collection for the whole loop ([RNG-3](08-slices-and-range.md));
- a value allocated from an arena (`mem.NewIn(&arena)` and the like, `mem.Arena`, `mem.DropArena`; illustrative names, P05.03 sets the real ones): it borrows the arena;
- a mutex guard (from `Lock` / `RLock`): it borrows the mutex ([CON-15](12-concurrency.md));
- a `@View` struct value: it borrows whatever its view fields borrow ([OWN-V5](#44-views-d16));
- an interface value that holds a boxed view: it carries that borrow ([TYP-9](03-types.md)).

**OWN-B2.** *The rule.* While a borrow is **live**, the borrowed thing may not be changed in a way that can free or move the memory the borrow depends on. This is a compile error. A borrow is live when it is used later on some path (flow and liveness). A use after the borrow's last use does not count.

"Changed" means:
- assigning or re-assigning the thing, or any prefix of it (`x`, when the borrow is of `x[i]` or `x.f`);
- `append` to it (it may reallocate);
- `move(x)`; `delete` and map inserts on a borrowed map (they may rehash);
- `clear`;
- an arena `Reset` or `Free`;
- writing an element or field whose type owns heap memory (that frees the old value);
- the end of its scope while the borrow escapes (already [OWN-V7](#44-views-d16) and OWN-B5).

`Split` on a borrowed slice (`head, tail := rest.Split(n); rest = tail`, [CON-16](12-concurrency.md)) consumes the receiver's borrow (a moving reborrow): the assignment to `rest` is allowed, and the two results are disjoint places.

The borrow made by a range loop (OWN-B1) allows element writes: only a live borrow of that element (such as the range variable of an owning element) blocks writing it.

Writing a pointer-free Copy value into an element or field is allowed: it frees and moves nothing. The check is conservative: when it is unsure, it reports an error.

```go
s := "hello world"
w := s[0:5]
s = "bye"                  // error: s is borrowed by w, and w is used below
println(w)
// Fix: w := s[0:5].Clone()   or use w before the assignment

nums := []int{1, 2, 3}
v := nums[1:3]
nums[0] = 5                // ok: a pointer-free Copy value frees and moves nothing
nums = append(nums, 4)     // error: append may reallocate, and v is used below
println(v[0])

names := []string{"a", "b"}
for _, name := range names {
    names = nil            // error: the loop borrows names
    println(name)
}
// Fix: collect into a new slice and assign after the loop. names[0] = "x" in the loop is fine for a trivial
// element; for a string element it is an error only while a borrow of that element is live (the range
// variable `name`).

var arena mem.Arena
u := mem.NewIn[User](&arena)   // illustrative name: P05.03 sets the real ones
arena.Reset()              // error: u borrows the arena, and u is used below
println(u.Name)
// Fix: finish with `u` before `Reset`, or copy it out to the normal heap with an explicit copy-out function (illustrative `mem.CloneTo[User](u, mem.Heap)`; P05.03 sets the name). u.Clone() does not help: a clone stays in the arena (SLC-3).
```

*Interaction with `s = s[i:j]` ([SLC-6](08-slices-and-range.md)).* That statement assigns the slice `s`, and `s` is a prefix of any borrow of `s[k]`. So it is an error while such a borrow is live, and inside a range loop over `s` ([RNG-3](08-slices-and-range.md)). When no borrow is live, SLC-6 is unchanged.

**OWN-B3.** *Across function calls, with no annotations (elision).*
- A call result that is a borrow (a view, a borrowed slice, a pointer, a `@View` struct, or a value holding one) borrows from **every** borrow-capable argument and from the receiver: views, borrowed parameters, pointers and borrowed slices passed in.
- A method that returns a view borrows from its receiver.
- The compiler may be more conservative than the real code. That is allowed, and the error message names the argument.

```go
func first(a, b View[string]) View[string] { return a }
w := first(x, y)           // w borrows from both x and y
x = "new"                  // error while w is used later, even if first only returns a
```

**OWN-B4.** *The same thing passed twice.* At a call, if one argument (or the receiver) is writable (a `*T`, a pointer receiver, a writable borrowed `[]T`, or a map the callee may write), then passing a borrow of the same place, or an overlapping place, as another argument is a compile error. Overlapping means the same variable, a part of it, or a prefix of it.

```go
func rename(t *Team, old View[string]) { t.Name = "new"; println(old) }
rename(&team, team.Name)             // error: old borrows team.Name, and t can write it
rename(&team, other.Name)            // ok: a different place
old := team.Name.Clone()
rename(&team, old)                   // ok: old is an owned copy
```

Two pointer variables that point to the same object, got from different sources, are not seen. That stays undefined ([UB-2](16-undefined-behavior.md)).

**OWN-B5.** *Escapes.* A borrow may live only where the checker can follow it: in locals, in parameters, in results (by OWN-B3), and in the fields of a `@View` struct value that follows these rules. These are compile errors:
- storing a borrow into a global, into a heap container (a slice, map or channel element, or a field of a struct that is not `@View`), or sending it on a channel;
- an escaping closure that captures a borrow ([CLO-3a](11-closures.md));
- a `go` statement whose argument or captured variable is a borrow ([CON-2](12-concurrency.md)): the goroutine may outlive the owner. The closure given to `s.Go` of a `sync.Scope` is not a `go` statement: the goroutine ends before the scope returns, so it may borrow ([CON-16](12-concurrency.md));
- boxing a borrow into an interface that escapes.

**OWN-B6.** *What is still not checked.* [Chapter 16](16-undefined-behavior.md) lists it: `unsafe` and raw memory, C and C++ interop, aliasing the compiler cannot see (OWN-B4), data races in `unsafe`, C and C++ code (in normal code they are compile errors, [CON-12 to CON-20](12-concurrency.md)), and `longjmp` or forced unwinding.

**OWN-B7.** *Go mode* (`.go` files). The checker is the same. A `.go` file uses the same ownership model, so borrowed slices, `&` of elements, range loops and calls are checked with the same rules. The names `View` and the arena API do not exist there unless imported, so those parts of OWN-B1 do not occur. (Open point: the plan does not yet say this in so many words; see ADR-0021.)

**OWN-B8.** *Borrows and concurrency (ADR-0022).* Three links to [chapter 12](12-concurrency.md):
- A `Mutex[T]` guard is a borrow of the mutex ([CON-15](12-concurrency.md)). The mutex may not be moved or dropped while the guard is live (OWN-B2), and the guard may not escape the function through a `go`, a channel or a global (OWN-B5).
- `ch <- v` moves `v` ([CON-14](12-concurrency.md), [OWN-R4](#42-ownership-rules-d14)); a borrow cannot be sent (OWN-B5), and the element type must be `Send`.
- Inside a `sync.Scope`, a goroutine may borrow locals. Two goroutines may not both get a writable borrow of the same place (like OWN-B4 for one call); `Split` gives disjoint parts ([CON-16](12-concurrency.md)).

*Why (B1 to B7):* the danger of a borrow is one thing, "the owner changed while I still look at it". Flow and liveness find it in one pass without annotations. Reading the code never needs a lifetime name, and the cost is a few false errors, which a `.Clone()` removes.

## 4.6 Rationale

- **Why ownership?** Deterministic, GC-free memory with C++-compatible object semantics, which C++ interop needs anyway, while keeping Go's zero values.
- **Why zero-on-move?** No drop flags and no partially dead variables. Moved-from state is always defined.
- **Why explicit `Clone`?** Hidden copies are hidden allocations. `x.Clone()` is a method, so it works the same for built-in types, user types and generic code (`[T Clone]`), and `clone(x)` stays as the short form.
- **Why views?** They make borrows visible in types and let `s[i:j]` stay free.
- **Rejected:** tracing GC (excluded by requirements); ARC everywhere (hidden cost, cycles); Rust-style lifetime annotations (hard to learn, many false errors; the annotation-free check of 4.5 is used instead, ADR-0021); C++-style implicit copies.
