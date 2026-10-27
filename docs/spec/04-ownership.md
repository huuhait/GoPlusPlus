# 4. Ownership

Base: Go spec [Variables](https://go.dev/ref/spec#Variables), [The zero value](https://go.dev/ref/spec#The_zero_value), [Assignability](https://go.dev/ref/spec#Assignability), [Assignment statements](https://go.dev/ref/spec#Assignment_statements), [Pointer types](https://go.dev/ref/spec#Pointer_types), [Address operators](https://go.dev/ref/spec#Address_operators), [Allocation](https://go.dev/ref/spec#Allocation), [Making slices, maps and channels](https://go.dev/ref/spec#Making_slices_maps_and_channels).

This is the core of Go++. Go has a GC, so every value can be shared. Go++ has no GC, so every value has **one owner** and is destroyed when its owner ends.

Contents: [4.1 categories](#41-type-categories-d13) · [4.2 rules 1 to 10](#42-ownership-rules-d14) · [4.3 pointers and heap](#43-pointers-and-heap-allocation-d17) · [4.4 refs](#44-refs-d16) · [4.5 rationale](#45-rationale)

## 4.1 Type categories (D1.3)

Every type is in exactly one of three categories.

| Category | Types | Copy | End of life |
|---|---|---|---|
| Trivial | `bool`, numbers, raw `*T`, `unsafe.Pointer`, `ref string`, `ref []T`, arrays/structs of trivial types | implicit, bitwise | nothing |
| Owning | `string`, `[]T`, `map[K]V`, `Unique[T]`, types with a `drop` method, arrays/structs that contain an owning type, C++ classes with a non-trivial copy, move or destructor | move-only; `x.Clone()` copies | drop glue runs |
| Shared handle | `Shared[T]`, `Weak[T]`, `chan T`, interface values (including `error`), func values, arrays/structs whose non-trivial fields are all shared handles | implicit; copying retains (atomic) | release; the last strong reference frees |

**OWN-C1.** *Trivial.* A trivial value is copied with `=`, as in Go. It has no destructor. A trivial type satisfies the `Copy` constraint ([GEN-1](10-generics.md)). Examples: `int`, `*Node`, `ref string`, `struct{ X, Y int }`.

**OWN-C2.** *Owning.* An owning value is **move-only**. Copying needs `x.Clone()` (or its short form `clone(x)`, [OWN-R6](#42-ownership-rules-d14)). When its owner ends, drop glue runs.

**OWN-C3.** *Shared handle.* Copying a shared handle is implicit and **retains** (atomic increment). Ending a copy **releases**. The last strong release frees the thing.

**OWN-C4.** *Structural.* The category of an array or struct is computed from its elements and fields:
- any owning field makes it owning;
- otherwise, any shared-handle field makes it a shared handle;
- otherwise it is trivial.
A `drop` method makes a type owning, whatever its fields.

**OWN-C5.** *Type parameters.* The category of a type parameter `T` is not known inside a generic body. It is treated as owning unless constrained by `Copy` ([GEN-1](10-generics.md)). After monomorphization each instance has its real category.

```go
type P struct{ X, Y int }          // trivial
type Doc struct{ Title string }    // owning (string)
type Ev struct{ Done chan int }    // shared handle (only a channel)
type Mix struct{ Title string; Done chan int }  // owning
```

*Why (C1 to C5):* three categories are enough to tell the compiler what a copy costs: nothing (trivial), a visible `x.Clone()` (owning), or a counter (shared handle). It is the same split as C++ trivially copyable types, move-only types and `shared_ptr`.

## 4.2 Ownership rules (D1.4)

Syntax: no change in this chapter. `move(place)` and `clone(x)` are predeclared function calls (typing in [EXP-1](06-expressions.md)); `x.Clone()` is an ordinary method call ([OWN-R6](#42-ownership-rules-d14)). The `ref` type is in [3.1](03-types.md), the `move` parameter mark in [5.1](05-parameters.md), and the `&d` range form in [8.3](08-slices-and-range.md).

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
- In the header of a `for … range`, `if` or `switch`, temporaries live until the end of the whole statement, body included (see [OWN-V6](#44-refs-d16)).

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

**OWN-R3a.** *Reading from other places.* The key of a map index on the left side is not a place read that needs ownership ([OWN-V4a](04-ownership.md#44-refs-d16)). "Other places" are fields, elements, map entries, package variables, pointer targets, borrowed variables ([OWN-R3b](04-ownership.md#42-ownership-rules-d14): borrowed parameters, owning range variables, owning assertion bindings) and captured variables ([CLO-9](11-closures.md)).
- **Borrow where a borrow is enough.** Reading an owned value from such a place borrows it when used as an operand, a borrowed argument (including interface parameters), a receiver or the `range` source.
- **Ownership needed means compile error.** These need ownership: `:=`, `=`, a `move` argument, `return`, a channel send, a composite literal. The error has two fix-its:
  - `p.Clone()` (copy it);
  - `move(p)` (take it, leave the zero value).
  For a borrowed parameter the fix-its are `p.Clone()` and "make the parameter `move`", because a borrowed parameter cannot be moved out of ([PAR-1](05-parameters.md)).
- **Moving a borrowed variable.** `move(x)` of a borrowed variable is a compile error ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)). Fix-its: `x.Clone()`, or declare the range variable with `&` (`for _, &v := range xs`, `for k, &v := range m`), which may be moved from ([RNG-1a](08-slices-and-range.md)). For a map, `move(m[k])` also takes the value out ([RNG-3](08-slices-and-range.md)).
- **Maps.**
  - `move(m[k])` removes the entry and returns it.
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
4. bindings of a type assertion or type switch whose case type is **owning**, including `case string:`, `case []T:`, `v.(string)` and `v.([]T)`, which also match a dynamic `ref string` / `ref []T`: the binding is always borrowed and read-only, because it may point into someone else's buffer ([TYP-9](03-types.md));
5. the range variable of a range-over-func loop in the plain form, when the yielded type is **owning** ([RNG-6a](08-slices-and-range.md)).

Trivial and shared-handle range variables and bindings are **not** borrowed variables: they are copies (a shared handle is copied with a retain) and stay writable. The variables of `range move(xs)` and `range ch` are owned locals. A variable declared with `&` in a range clause (`for _, &d := range xs`) is an **alias** of the element, not a borrowed variable: it is writable and changes the collection, and for `defer` and `go` it counts as a place in the collection ([RNG-1a](08-slices-and-range.md)). Every other rule that says "borrowed variable" means exactly the list above.

A borrowed variable is **read-only at the top level**. This is the one rule for all kinds, and PAR-1 and PAR-2 refer to it. These are compile errors:
- assigning the variable itself, or assigning its own fields (the fields of the struct value it holds), and `append` to it;
- taking its address (`&d`), or calling a method with a pointer receiver on it (an implicit `&d`). This covers the variable **and any part of it reached without going through a slice, map or pointer**: its fields, fields of fields, and array elements held by value (`&d.Buf`, `d.Buf.Reset()`). The line is the same "top level" line as PAR-1: going through a slice, map or pointer is allowed; staying inside the value is not;
- `move(x)` of it. A range variable can never be moved from in the plain form, because moving a key would corrupt the map and moving a value is ambiguous with `move(m[k])`. Fix-its: `&v` (the `&` form can be moved from, [RNG-1a](08-slices-and-range.md)) or `v.Clone()`.

These stay allowed: data reached **through** slices, maps and pointers, as in Go and PAR-1. Elements of a `[]T`, map entries and pointer targets are mutable. For example `row[j] = 0` for `row` in `range grid`, and `d.Tags[0] = "x"` for `d` in `range docs`, write through the shared slice and change the collection, exactly as Go does (Go's copy shares the backing array). Also allowed: **re-pointing** a borrowed `string` or `[]T` variable as a local ref (`s = s[1:]`, [PAR-2](05-parameters.md)); it still cannot be appended to. This holds for parameters, range variables and assertion bindings alike, because re-pointing only changes the local ref.

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
- `move(place)` takes the value out of any place and leaves the zero value.
- The move is **implicit** when the same place is passed to a `move` parameter and also receives the result, in one statement: `p = append(p, …)`, `buf = strconv.AppendInt(buf, n, 10)`, `x.items = append(x.items, v)`.
- Anywhere else, write `append(move(p), …)` or `append(p.Clone(), …)`.
- **Map entries.** `m[k] = append(m[k], v)` takes the value **in place**: the slot holds the zero value until the result is stored back. The entry is never removed and re-inserted. If `k` is absent, the result is a normal insert (the key rule of [OWN-V4a](04-ownership.md#44-refs-d16)). Inside `range m`, any `m[x] = …`, `m[x]++`, `m[x] op= …` or `m[x] = append(m[x], …)` where `x` is **not the current range key variable, or is the range key variable but was written earlier in the body** (for a trivial key type the range key is a writable copy, so after `k++` the name `m[k]` no longer names the current entry) is a visible potential insert (the compiler cannot know whether `x` exists), so it is a compile error ([RNG-3](08-slices-and-range.md), D1.21). Only `m[k]` with `k` the range key variable, not written earlier in the body, is a write to the current entry (defined). Example: `for k := range m { m[k] = 1; k++; m[k] = 2 }` accepts the first write and rejects the second. Fix-it: collect the changes and apply them after the loop. Inserts the compiler cannot see panic at run time ([RNG-4](08-slices-and-range.md)).
- Spread (`append(a, b...)`) and `copy(dst, src)` with non-trivial elements need `move` or `Clone` on the **source**: `append(a, move(b)...)`, `append(a, b.Clone()...)`, `copy(dst, src.Clone())`.
- Moves out of places happen **after** all other arguments are evaluated.

```go
x.items = append(x.items, v)       // implicit move of x.items; v moves too if owned
y := append(x.items, v)            // error: write append(x.items.Clone(), v) or append(move(x.items), v)
all = append(all, move(more)...)   // elements of more move into all
```

*Why (R4):* `p = append(p, …)` is the most common slice idiom in Go; the implicit move keeps it unchanged, while every other form that would hide a copy or a take must say `move` or `Clone`.

**OWN-R5.** *Zero values.*
- Every type has a zero value (as in Go). A moved-from place holds it (C++ classes: see R9).
- A `drop` method must accept the zero value. `gopp vet` checks this.
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

*Why (R5):* every type has a zero value in Go, so a moved-from place needs no drop flag. The same holds for extended locals ([PAN-3](09-errors-and-panics.md)): the defer record that was created when the `defer` ran is the only flag, and there are no extra drop flags. If no such `defer` ran on the path, the local is destroyed at the end of its block as usual: the existence of the defer record is what tells the block end whether to drop the local; no extra drop flag.

**OWN-R6.** *Copies are explicit: `x.Clone()`.*
- Every type that can be cloned has a method `Clone() T`. Built-in types have it too: `string`, `[]T`, `map[K]V`, `Unique[T]`, numbers and the other types of [OWN-C1](#41-type-categories-d13). So `s2 := s.Clone()` and `u := User{Name: name.Clone()}` work for every cloneable `s` and `name`.
- `clone(x)` is a short form: it calls `x.Clone()`. `move(x)` stays a built-in function. Examples and fix-its prefer `x.Clone()`.
- A method named `Clone` with any other signature than `Clone() T` is legal Go (for example `func (c *Config) Clone() *Config` or `(*Request).Clone(ctx)`). `x.Clone()` calls that method, but the type `Config` is not cloneable: `clone(x)` and the `Clone` constraint ([GEN-2](10-generics.md)) reject it with a compile error. A `Clone` declared on `*T` itself is `*T`'s own method: `cfg.Clone()` on a `*Config` calls it, exactly as in Go. Its signature `Clone() *Config` matches `Clone() T` with `T = *Config`, so `*Config` satisfies `[T Clone]`.
- If the method set of a struct already has a `Clone` (declared, or promoted from an embedded field), the compiler provides none. The type is cloneable only if that `Clone` has the signature `Clone() T` for the outer type `T`. A promoted `Clone` returns the embedded type, so the outer type is usually not cloneable unless it declares its own. For example `type S struct{ *tls.Config }`: `s.Clone()` calls the promoted `(*tls.Config).Clone()`, as in Go, and `S` is not cloneable; `type W struct{ *http.Request }`: `w.Clone(ctx)` works as in Go.
- Clone is structural when all fields are cloneable: the compiler provides `Clone` and calls the `Clone` of each field. A `Clone` written by hand replaces it. A hand-written `Clone` with the exact cloneable signature `Clone() T` has a **value receiver**: `func (h Handle) Clone() Handle`. The receiver is borrowed as usual ([PAR-1](05-parameters.md)), so it only reads. `func (h *Handle) Clone() Handle` is a compile error, fix-it "use a value receiver". This rule covers only the signature `Clone() T`.
- Shared handles copy by retaining: `x.Clone()` on a `Shared[T]`, a channel, an interface value or a func value is the same as a copy.
- A trivial `x` is cloneable: `x.Clone()` is a plain copy, unless its method set already has a `Clone` with another signature (for example `struct{ *tls.Config }`: trivial, but not cloneable). Numbers, bools and the other built-in trivial types are always cloneable. This lets generic code with `[T Clone]` take `int`.
- **Pointer kinds and `Clone`.** `u.Clone()` on a `Unique[T]` allocates a new `Unique[T]` holding `(*u).Clone()` (it needs a cloneable `T`), and `s.Clone()` on a `Shared[T]` retains: for these two the pointer kind's own `Clone` wins over a promoted `T.Clone` (Go has no such types, so no Go code changes meaning). On a raw `*T` where `T` has a **value-receiver** `Clone() T` (which Go promotes into `*T`'s method set), `p.Clone()` is a **compile error** for ambiguity, because Go would call `T`'s. (A `Clone` declared on `*T` itself is not ambiguous: it is called.) Fix-its: "write `(*p).Clone()` to copy the T, or `q := p` to copy the pointer". If `T` has no `Clone` method, `p.Clone()` copies the pointer. `[T Clone]` with `T = *Handle` (where `Handle` has a value-receiver `Clone`) is satisfied by the pointer copy, while a direct `p.Clone()` is the ambiguity error. The difference is intended: generic code sees `*Handle`'s own method set.
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
b := someShared.Clone()    // a Shared[T]: retains
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

**OWN-R8.** *No borrow checker.* Refs and raw pointers can dangle, as in C++.
- The compiler **does** catch: returning `&local`; returning a `ref` into a local ([OWN-V7](#44-refs-d16)); storing a borrow of a temporary; a `go` or a `defer` that borrows something that dies before the function returns (a borrowed parameter lives until return, so `defer fmt.Println("done", name)` is legal; a borrowed loop variable does not) ([PAN-3](09-errors-and-panics.md), [CON-2](12-concurrency.md)); use after move.
- The cases it does **not** catch are listed in [chapter 16](16-undefined-behavior.md): refs or raw pointers kept past their owner, and a borrowed parameter whose owner the callee frees through another path (`u.SetName(u.Name)`).
- `-asan` catches the rest at run time. v1 has no extra safety layer.

```go
p := &local                // allowed
return &local              // compile error: caught
s := name[:2]              // ref
dropName()                 // frees name's buffer
fmt.Println(s)             // NOT caught: dangling ref (UB-1). -asan finds it at run time
```

*Why:* a borrow checker costs compile speed and simplicity; v1 keeps Go's look and relies on a small closed list of checks plus sanitizers.

**OWN-R9.** *C++ class types.*
- A C++ type has a zero value only if it has a default constructor.
- Where Go needs a zero value and there is none, it is a compile error: `make([]T, n)`, a missing map key, a receive from a closed channel, `var zero T` in generic code, named results, a failing `!`.
- Every move of a C++ value, implicit or `move(x)`, runs its **move constructor**. The source keeps the C++ "moved-from" state (not zero) and is destroyed at the end of its scope.
- Only containers relocate values, with the type's relocate operation (P05 D5.5): `memcpy` only for Go++ types and trivially relocatable C++ types.

```go
var v vector.vector[int32]   // has a default constructor: zero value exists
w := move(v)                 // runs the move constructor; v is moved-from (not zero), destroyed at scope end
xs := make([]Foo, 3)         // compile error if Foo (a C++ class) has no default constructor
```

*Why:* C++ classes keep their own move and destroy rules, so interop needs no wrapper and no surprise zero values.

**OWN-R10.** *Pinned values.*
- `gopp vet` reports moving a local after its address, or a ref into it, has been stored.
- Types marked `@Pinned` (the `sync` types) cannot be moved or copied after first use: a compile error, like Go vet's copylocks.
- `return local` builds the value in the caller's result slot, so it does not move (see the escaping-closure case in [OWN-R3c](04-ownership.md#42-ownership-rules-d14)). When different locals are returned on different paths, at most one is built in the result slot; returning the others moves them, and OWN-R3c then applies to them. The tie-break is fixed: a local that a pending deferred call (or a non-escaping closure) still uses gets the slot. If two returned locals both have pending users, returning the second one is an OWN-R3c error, fix-it `.Clone()`. Otherwise the choice does not change behaviour, because the others just move. Example: `a := f(); b := g(); defer log.Println(a); if c { return a }; return b`. Here `a` gets the slot, and `return b` moves `b` (fine, nothing pending uses `b`).

```go
var mu sync.Mutex          // @Pinned
mu.Lock()
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
    Children []Unique[Node]  // owned slice of owning pointers
    Parent   *Node           // raw back-pointer: does not own
}

func NewNode(name string, parent *Node) Unique[Node] {
    return new_unique(Node{Name: name.Clone(), Parent: parent}) // name is borrowed: Clone it to own it
}

func (n *Node) Add(move child Unique[Node]) {
    child.Parent = n
    n.Children = append(n.Children, child)   // child moves into the slice (R3, R4)
}

func main() {
    root := NewNode("root", nil)
    root.Add(NewNode("a", root))   // the temporary moves into Add
    b := NewNode("b", root)
    root.Add(b)                    // b moves
    // fmt.Println(b.Name)         // compile error: b used after move
    head := root.Name[:2]          // ref string: a borrow, no allocation
    fmt.Println(head, len(root.Children))
}                                  // root is dropped: the whole tree is freed
```

## 4.3 Pointers and heap allocation (D1.7)

Every heap allocation is spelled out. Go's `new` is removed. (The runtime's own allocations are not spelled in source: for example the defer records of deferred calls in loops, which hold extended locals, [PAN-3](09-errors-and-panics.md).)

| Type | Created with | Meaning |
|---|---|---|
| `*T` | `&x` (address of a variable) | raw, nullable, non-owning (Go syntax, no GC) |
| `Unique[T]` | `new_unique(v)` | single owner; the value is dropped and freed when the pointer is destroyed; nullable (zero value `nil`) |
| `Shared[T]` | `new_shared(v)` | shared ownership, atomically refcounted; one allocation holds the counts and the value (like `make_shared`) |
| `Weak[T]` | `new_weak(s)`, from a `Shared[T]` | does not own, does not allocate; `w.Lock()` returns a `Shared[T]`, or `nil` if the value is gone |

**OWN-P1.** `*T` is a raw pointer. `&x` takes the address of an addressable operand (a variable, field or element), as in Go. A raw pointer is trivial and never frees anything.

**OWN-P2.** `new_unique(v)` and `new_shared(v)` take the initial value and **move it in**: `new_unique(Node{Name: "a"})`, or `new_unique(Node{})` for a zero value.

**OWN-P3.** `new_weak(s)` takes a `Shared[T]` (borrowed). A weak pointer cannot own a new object, because it would be freed at once.

**OWN-P4.** `&T{…}` is a compile error with a fix-it: "allocation is explicit in Go++: use `new_unique(T{…})`". In Go it allocated silently.

**OWN-P5.** Field access and method calls **auto-dereference** every pointer kind. `Unique[T]` and `Shared[T]` get the methods of both `T` and `*T`, except `Clone` and `IsNil`, where the pointer kind's own method wins ([OWN-R6](#42-ownership-rules-d14), OWN-P9). On a raw `*T`, `p.Clone()` is an error if `T` has a value-receiver `Clone() T` (OWN-R6).

**OWN-P6.** `Unique[T]` and `Shared[T]` convert **implicitly** to `*T` when borrowed from a place. They never convert from a temporary, because the target would be freed immediately.

**OWN-P7.** `make([]T, n)`, `make(map[K]V)` and `make(chan T, n)` work as in Go (with the allocator rules of [SLC-2](08-slices-and-range.md)).

**OWN-P8.** `Unique[T]` is owning (move-only). `Shared[T]` and `Weak[T]` are shared handles: copies retain the strong or weak count. `Weak[T]` copies retain the weak count only.

**OWN-P9.** *`IsNil()` and `nil`.* Every pointer kind has an `IsNil() bool` method: raw `*T`, `Unique[T]`, `Shared[T]` and `Weak[T]`. `p == nil` and `p != nil` keep working, as in Go.
- If `T` has its own `IsNil` method, the pointer's `IsNil` wins. Call `T`'s as `(*p).IsNil()`. `Clone` follows the same tie-break for `Unique[T]` and `Shared[T]`; for a raw `*T` with a value-receiver `T.Clone` it is an ambiguity error ([OWN-R6](#42-ownership-rules-d14)).
- For `Weak[T]`, `IsNil()` is true when there is no target: it was never set, or the value is already freed.

```go
type Tree struct{ L, R Unique[Tree]; V int }

func sum(t *Tree) int {
    if t == nil { return 0 }
    return t.V + sum(t.L) + sum(t.R)   // t.L converts to *Tree: borrowed from a place (P6)
}

func build() Unique[Tree] {
    return new_unique(Tree{V: 1, L: new_unique(Tree{V: 2})})   // R and the inner R are nil
}

s := new_shared(Config{Name: "x"})
w := new_weak(s)                 // does not keep Config alive
if p := w.Lock(); p != nil {     // p is a Shared[Config]
    fmt.Println(p.Name)
}
bad := &Tree{V: 1}               // error: use new_unique(Tree{V: 1})
```

*Why (P1 to P9):* every allocation is spelled `new_unique` or `new_shared`, so nothing allocates behind a Go-looking `&T{}`; `*T` stays a raw pointer so Go pointer code and C pointers keep working.

## 4.4 Refs (D1.6)

Syntax: `RefType` is defined in [3.1](03-types.md). `ref` is allowed in front of `string` and slice types only: `ref string`, `ref []byte`.

**OWN-V1.** `ref string` (immutable bytes) and `ref []T` (mutable elements, fixed length) are `(ptr, len)` borrows. They are trivial ([OWN-C1](#41-type-categories-d13)).

**OWN-V2.** *Slicing makes refs.* `s[i:j]` on a `string` gives a `ref string`. `xs[i:j]` on a slice or array gives a `ref []T`. No allocation. `:=` infers the ref type. The one exception is re-slicing an owned slice into itself: `s = s[i:j]` shrinks it in place ([SLC-6](08-slices-and-range.md)).

**OWN-V3.** *Conversions.*
- Owned to ref is implicit wherever a ref is expected (an argument, an assignment to a ref variable, a comparison).
- Ref to owned is explicit and **copies**: `string(v)`, `[]T(v)`. `[]T(v)` needs elements that satisfy `Clone` ([GEN-2](10-generics.md)).
- Unlike Go, where converting between slice types never copies, here the copy is how you get ownership.

**OWN-V4.** *Map keys.* Maps accept a `ref` key for lookups, so `counts[w]` with `w ref string` does not allocate. Inserting a new entry clones the key, whether it is a `ref` key or a borrowed one (OWN-V4a).

**OWN-V4a.** *Map index on the left side.* `m[k] = v`, `m[k]++` and `m[k] op= v` **never consume** the key expression `k`. The key is borrowed for the lookup, and cloned only if a **new** entry is inserted. This holds for any key, ref or not. So `for _, w := range words { counts[w]++ }` works as in Go (`w` is a borrowed range variable), and `m[s] = 1; fmt.Println(s)` is fine (`s` is not moved). The allocation (the key clone) happens only on insert, as for ref keys. Inside a `range` over the same map, the rule of OWN-R4 "Map entries" applies: only `m[k]` with `k` the range key variable, not written earlier in the body, is allowed.

```go
counts := map[string]int{}
for _, w := range words { counts[w]++ }   // w borrowed: lookup borrows it; the first insert clones it
s := "k"
counts[s] = 1
fmt.Println(s)                            // ok: s was not moved
```

The key is cloned only when a new entry is added. Without this rule, `counts[w]++` over a borrowed range variable would need `counts[w.Clone()]++` and allocate on every update. The key clone on a map insert is part of the allocation list of principle 1.

**OWN-V5.** *Storing refs.* Refs may be stored and returned at the programmer's risk, like C++ `string_view` and `span`. `gopp vet` warns about a struct field of ref type unless the struct is marked `@Ref` (for example tokens that point into a source buffer).

**OWN-V6.** *Temporaries.* A ref into a temporary (`f()[1:3]`) lives only as long as the temporary: until the end of the full statement. In the header of a `for … range`, `if` or `switch` it lives until the end of the whole statement. Storing a borrow of a temporary in a longer-lived place is a compile error ([OWN-R8](#42-ownership-rules-d14)).

**OWN-V7.** *Returning a ref into a local.* `return` of a ref that points into a local variable of the same function, or into an owned value that dies at the return (an owned `move` parameter, a temporary), is a **compile error**. A ref into a *borrowed* parameter or receiver is fine, because the caller still owns it ([PAR-9](05-parameters.md)). It is an error only when the returned ref points into an **owned** local (or an owned value that dies at the return). The compiler checks only what it can see in the function: the owned local or `move` parameter itself, converted to a ref at the `return` (implicit owned-to-ref conversion), so `func f() ref string { s := "hi"; return s }` and `func g(move s string) ref string { return s }` are compile errors; a ref made by slicing a temporary, or a temporary converted to a ref at the `return` (`return load()[:2]`, or `return strings.Repeat("a", 3)` as a `ref string`); a ref made by slicing an owned local (`return s[:1]`), or by indexing or slicing an owned local's field held by value. A ref-typed local or parameter that holds a ref into a borrowed parameter or into the caller's data is fine (`s = s[1:]; return s` in `trim(s string) ref string`, where `s` is a borrowed parameter, [PAR-2](05-parameters.md)). Less obvious cases, such as a ref that comes out of a call, stay undefined behavior and `-asan` finds them ([UB-1](16-undefined-behavior.md)).

```go
func bad() ref string { s := "hi"; return s[:1] }   // error: ref into the local s
func head(src string) ref string { return src[:1] }   // ok: src is borrowed from the caller

@Ref
type Token struct {
    Text ref string      // points into the source buffer; no vet warning thanks to @Ref
    Line int
}

func first(src string) ref string {     // result is a ref into src
    i := 0
    for i < len(src) && src[i] != ' ' { i++ }
    return src[:i]
}

func main() {
    text := "alpha beta"
    w := first(text)             // ref: no allocation
    owned := string(w)           // explicit copy: now independent of text
    counts := map[string]int{}
    counts[w]++                  // ref key: lookup is free; the first insert clones the key
    fmt.Println(w, owned, counts)

    for _, c := range strings.ToUpper(text)[:3] { _ = c } // the temporary lives for the whole loop
    // h := strings.ToUpper(text)[:3]  // error: ref into a temporary stored in h
}
```

*Why (V1 to V7):* a ref is a borrow you can see in the type, so `s[i:j]` is free and a copy is always a visible `string(v)`; the dangling risk is the same as C++ `string_view` and is listed in chapter 16.

## 4.5 Rationale

- **Why ownership?** Deterministic, GC-free memory with C++-compatible object semantics, which C++ interop needs anyway, while keeping Go's zero values.
- **Why zero-on-move?** No drop flags and no partially dead variables. Moved-from state is always defined.
- **Why explicit `Clone`?** Hidden copies are hidden allocations. `x.Clone()` is a method, so it works the same for built-in types, user types and generic code (`[T Clone]`), and `clone(x)` stays as the short form.
- **Why refs?** They make borrows visible in types and let `s[i:j]` stay free.
- **Rejected:** tracing GC (excluded by requirements); ARC everywhere (hidden cost, cycles); a Rust-style borrow checker (cost in compile speed and simplicity; reconsider after v1); C++-style implicit copies.
