# 4. Ownership

Base: Go spec [Variables](https://go.dev/ref/spec#Variables), [The zero value](https://go.dev/ref/spec#The_zero_value), [Assignability](https://go.dev/ref/spec#Assignability), [Assignment statements](https://go.dev/ref/spec#Assignment_statements), [Pointer types](https://go.dev/ref/spec#Pointer_types), [Address operators](https://go.dev/ref/spec#Address_operators), [Allocation](https://go.dev/ref/spec#Allocation), [Making slices, maps and channels](https://go.dev/ref/spec#Making_slices_maps_and_channels).

This is the core of Go++. Go has a GC, so every value can be shared. Go++ has no GC, so every value has **one owner** and is destroyed when its owner ends.

Contents: [4.1 categories](#41-type-categories-d13) · [4.2 rules 1 to 10](#42-ownership-rules-d14) · [4.3 pointers and heap](#43-pointers-and-heap-allocation-d17) · [4.4 views](#44-views-d16) · [4.5 rationale](#45-rationale)

## 4.1 Type categories (D1.3)

Every type is in exactly one of three categories.

| Category | Types | Copy | End of life |
|---|---|---|---|
| Trivial | `bool`, numbers, raw `*T`, `unsafe.Pointer`, `view string`, `view []T`, arrays/structs of trivial types | implicit, bitwise | nothing |
| Owning | `string`, `[]T`, `map[K]V`, `Unique[T]`, types with a `drop` method, arrays/structs that contain an owning type, C++ classes with a non-trivial copy, move or destructor | move-only; `clone(x)` copies | drop glue runs |
| Shared handle | `Shared[T]`, `Weak[T]`, `chan T`, interface values (including `error`), func values, arrays/structs whose non-trivial fields are all shared handles | implicit; copying retains (atomic) | release; the last strong reference frees |

**OWN-C1.** *Trivial.* A trivial value is copied with `=`, as in Go. It has no destructor. Examples: `int`, `*Node`, `view string`, `struct{ X, Y int }`.

**OWN-C2.** *Owning.* An owning value is **move-only**. Copying needs `clone(x)`. When its owner ends, drop glue runs.

**OWN-C3.** *Shared handle.* Copying a shared handle is implicit and **retains** (atomic increment). Ending a copy **releases**. The last strong release frees the thing.

**OWN-C4.** *Structural.* The category of an array or struct is computed from its elements and fields:
- any owning field makes it owning;
- otherwise, any shared-handle field makes it a shared handle;
- otherwise it is trivial.
A `drop` method makes a type owning, whatever its fields.

**OWN-C5.** *Type parameters.* The category of a type parameter `T` is not known inside a generic body. It is treated as owning unless constrained by `trivial` ([GEN-1](10-generics.md)). After monomorphization each instance has its real category.

```go
type P struct{ X, Y int }          // trivial
type Doc struct{ Title string }    // owning (string)
type Ev struct{ Done chan int }    // shared handle (only a channel)
type Mix struct{ Title string; Done chan int }  // owning
```

*Why (C1 to C5):* three categories are enough to tell the compiler what a copy costs: nothing (trivial), a visible `clone` (owning), or a counter (shared handle). It is the same split as C++ trivially copyable types, move-only types and `shared_ptr`.

## 4.2 Ownership rules (D1.4)

Syntax: no change. `move(place)` and `clone(x)` are ordinary predeclared function calls (typing in [EXP-1](06-expressions.md)).

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
- Locals are destroyed at the end of their block, in reverse declaration order.
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
- `b := a`, `x.f = a`, passing `a` to an `own` parameter, `return a`, `ch <- a`, `T{F: a}`.
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
- **Ownership needed means compile error.** These need ownership: `:=`, `=`, an `own` argument, `return`, a channel send, a composite literal. The error has two fix-its:
  - `clone(p)` (copy it);
  - `move(p)` (take it, leave the zero value).
  For a borrowed parameter the fix-its are `clone(p)` and "make the parameter `own`", because a borrowed parameter cannot be moved out of ([PAR-1](05-parameters.md)).
- **Moving a borrowed variable.** `move(x)` of a borrowed variable is a compile error, except for a slice range variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)). For a map range variable the fix-its are `clone(k)` / `clone(v)`, or `move(m[k])` to take the value out ([RNG-3](08-slices-and-range.md)).
- **Maps.**
  - `move(m[k])` removes the entry and returns it.
  - `v, ok := clone(m[k])` and `v, ok := move(m[k])` keep the comma-ok form.
  - `m[k].f = x` stays illegal, as in Go.
- **Swap.** A statement that reads and writes the same places (`a[i], a[j] = a[j], a[i]`) is allowed and moves the elements.
- **`min` and `max`.** On owned strings they return a copy.

```go
type Doc struct{ Title string }

func (d *Doc) Rename(t string) {
    d.Title = t           // error: needs ownership; fix-its: clone(t), or make t `own`
    d.Title = clone(t)    // ok: copy
}

func take(items []Doc) Doc {
    // return items[0]         // error: fix-its clone(items[0]) / move(items[0])
    return move(items[0])      // ok: items[0] now holds the zero Doc
}

m := map[string]string{"k": "v"}
v, ok := move(m["k"])          // removes "k"; ok reports it was there
```

*Why (R3a):* borrowing where a borrow is enough keeps Go-like code free of `clone`; asking for `clone` or `move` exactly where ownership is needed makes every copy and every take visible.

**OWN-R3b.** *Borrowed variables* (the one definition). A **borrowed variable** is a variable that borrows a value still owned by something else. The complete list:
1. borrowed owning parameters and receivers ([PAR-1](05-parameters.md));
2. range variables over **owning** elements of a slice or array ([RNG-1](08-slices-and-range.md));
3. map range variables whose key type or value type is **owning** (each such variable);
4. bindings of a type assertion or type switch whose dynamic type is **owning** ([TYP-9](03-types.md)).

Trivial and shared-handle range variables and bindings are **not** borrowed variables: they are copies (a shared handle is copied with a retain) and stay writable. The variables of `range move(xs)` and `range ch` are owned locals. Every other rule that says "borrowed variable" means exactly this list.

A borrowed variable is **read-only at the top level**. This is the one rule for all four kinds, and PAR-1 and PAR-2 refer to it. These are compile errors:
- assigning the variable itself, or assigning its own fields (the fields of the struct value it holds), and `append` to it;
- taking its address (`&d`), or calling a method with a pointer receiver on it (an implicit `&d`). This covers the variable **and any part of it reached without going through a slice, map or pointer**: its fields, fields of fields, and array elements held by value (`&d.Buf`, `d.Buf.Reset()`). The line is the same "top level" line as PAR-1: going through a slice, map or pointer is allowed; staying inside the value is not;
- `move(x)` of it, except for a slice range variable, where `move(x)` takes the element and leaves the zero value in it (a map range variable can never be moved, because moving a key would corrupt the map and moving a value is ambiguous with `move(m[k])`).

These stay allowed: data reached **through** slices, maps and pointers, as in Go and PAR-1. Elements of a `[]T`, map entries and pointer targets are mutable. For example `row[j] = 0` for `row` in `range grid`, and `d.Tags[0] = "x"` for `d` in `range docs`, write through the shared slice and change the collection, exactly as Go does (Go's copy shares the backing array). Also allowed: **re-pointing** a borrowed `string` or `[]T` variable as a local view (`s = s[1:]`, [PAR-2](05-parameters.md)); it still cannot be appended to.

> **Open (P01 review):** PAR-2 gives the re-pointing exception for borrowed parameters. This spec extends it to `string`/`[]T` range variables and assertion bindings, because re-pointing only changes the local view and matches Go. The plan is silent on those.

Fix-its: write through the element (`docs[i].Title = …`, `docs[i].Upper()`); modify a copy (`c := clone(d)`); for a map write back after cloning (`m[k] = c`); or take ownership (`clone(d)`, `range move(xs)`, `move(m[k])`).

```go
for i, d := range docs {          // d is a borrowed variable
    d.Title = "x"                 // error: read-only
    d.Upper()                     // error: pointer-receiver call (implicit &d)
    p := &d                       // error
    q := &d.Buf                   // error: a field held by value
    d.Buf.Reset()                 // error: implicit &d.Buf
    d.Ptr.Reset()                 // ok: goes through a pointer field
    d.Tags[0] = "y"               // ok: goes through a slice
}
for i := range docs { docs[i].Title = "x" }          // fix-it: write through the element
for _, d := range docs { c := clone(d); c.Title = "x"; out = append(out, c) }   // fix-it: modify a copy
for k, d := range m {
    c := clone(d); c.Title = "x"; m[k] = c           // fix-it (maps): write back after cloning
}
for k, n := range map[string]int{"a": 1} { n++ }     // ok: an int value is not a borrowed variable
for _, row := range grid { row[0] = 0 }              // ok: writes through the shared slice, as in Go
for _, d := range docs { d.Tags[0] = "x" }           // ok: through a slice; changes docs[i].Tags[0]
for _, s := range names { s = s[1:]; use(s) }        // ok: re-pointing a borrowed string
```

> **Open (P01 review):** Go lets you assign to a range variable, but that changes only a copy (the collection is unchanged; calling a pointer method on it changes only the copy too). A writable borrow here would silently change the collection, or write into an immutable box (TYP-5), so this spec rejects the top-level writes. It rejects some Go code: that is a listed difference.

**OWN-R3c.** *Moving a variable that is still captured or borrowed.* Moving a local (`b := a`, `T{F: a}`, `return a`, `ch <- a`, an `own` argument, `move(a)`) while something still refers to it is a **compile error**, because the later read would see the zero value ([OWN-R5](04-ownership.md#42-ownership-rules-d14)) where Go sees the value. Counted as "still refers":
- a **closure that captured `a` and may still run**: the closure's later call counts as a use after move. For a non-escaping closure, the error is reported when the closure value is used after the move; for an escaping closure (shared cell, [CLO-3](11-closures.md)), a closure used with `go`, or a deferred function literal, the move is always an error (conservative: the compiler cannot see the later call);
- a **pending deferred call that borrows `a`**, or a pending **pointer-receiver deferred call** on `a` (it holds `&a`: `var b Buffer; defer b.Print(); out := b` would make `Print` read the zero value, where Go prints the value; writes, assignments and pointer calls on `a` stay allowed, [PAN-3](09-errors-and-panics.md)); for a borrowing call, for example (`defer fmt.Println("finished", tag)` and then `t := tag`; [CLO-7](11-closures.md)).

Not counted (allowed, because the variable is not left with a zero value that the closure could read):
- a move where the **same place receives a value in the same statement**: the implicit moves of [OWN-R4](04-ownership.md#42-ownership-rules-d14) (`out = append(out, "b")`) and swaps (`x, y = y, x`);
- `return res` of a **named result** that a deferred literal captured: the result slot is the variable, so nothing is moved out from under the deferred closure ([CLO-3b](11-closures.md)).

Fix-its for the other moves: `clone(a)` at the move site, or capture or pass a clone to the closure or the deferred call.

```go
name := "ann"
hello := func() { fmt.Println("hello", name) }
u := User{Name: name}        // error: name is captured by hello, which is used below
hello()
v := User{Name: clone(name)} // ok: fix-it

add := func() { out = append(out, "c") }
out = append(out, "b")       // ok: out receives the value in the same statement (OWN-R4)
x, y = y, x                  // ok: a swap
func f() (res string) { defer func() { res += "!" }(); res = "x"; return res }   // ok: named result

tag := job.Tag
defer fmt.Println("finished", tag)   // borrows tag
t := tag                             // error: tag is borrowed by a pending deferred call
```

> **Open (P01 review):** D1.4 rule 8 lists use after move as caught, but not that a closure or a pending `defer` is a use. This spec counts them, so Go++ never prints `""` where Go prints the value (repro: Go prints `ann start` / `ann done`). The conservative error for escaping closures may reject code that is in fact safe.

**OWN-R4.** *Explicit and implicit moves.*
- `move(place)` takes the value out of any place and leaves the zero value.
- The move is **implicit** when the same place is passed to an `own` parameter and also receives the result, in one statement: `p = append(p, …)`, `buf = strconv.AppendInt(buf, n, 10)`, `x.items = append(x.items, v)`.
- Anywhere else, write `append(move(p), …)` or `append(clone(p), …)`.
- **Map entries.** `m[k] = append(m[k], v)` takes the value **in place**: the slot holds the zero value until the result is stored back. The entry is never removed and re-inserted. If `k` is absent, the result is a normal insert (the key rule of [OWN-V4a](04-ownership.md#44-views-d16)). Inside `range m`, any `m[x] = …`, `m[x]++`, `m[x] op= …` or `m[x] = append(m[x], …)` where `x` is **not the current range key variable, or is the range key variable but was written earlier in the body** (for a trivial key type the range key is a writable copy, so after `k++` the name `m[k]` no longer names the current entry) is a visible potential insert (the compiler cannot know whether `x` exists), so it is a compile error ([RNG-3](08-slices-and-range.md), D1.21). Only `m[k]` with `k` the range key variable, not written earlier in the body, is a write to the current entry (defined). Example: `for k := range m { m[k] = 1; k++; m[k] = 2 }` accepts the first write and rejects the second. Fix-it: collect the changes and apply them after the loop. (Open, see below.)
- Spread (`append(a, b...)`) and `copy(dst, src)` with non-`trivial` elements need `move` or `clone` on the **source**: `append(a, move(b)...)`, `append(a, clone(b)...)`, `copy(dst, clone(src))`.
- Moves out of places happen **after** all other arguments are evaluated.

```go
x.items = append(x.items, v)       // implicit move of x.items; v moves too if owned
y := append(x.items, v)            // error: write append(clone(x.items), v) or append(move(x.items), v)
all = append(all, move(more)...)   // elements of more move into all
```

> **Open (P01 review):** the plan gives the implicit move only for `p = append(p, …)` on a place; this spec extends it to a map entry `m[k] = append(m[k], v)` as above, the most common Go idiom for grouping.

*Why (R4):* `p = append(p, …)` is the most common slice idiom in Go; the implicit move keeps it unchanged, while every other form that would hide a copy or a take must say `move` or `clone`.

**OWN-R5.** *Zero values.*
- Every type has a zero value (as in Go). A moved-from place holds it (C++ classes: see R9).
- A `drop` method must accept the zero value. `gopp vet` checks this.
- Drop glue skips a type's `drop` when the value is all zero bytes, so dropping a zero value behaves the same in every build mode.

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

*Why (R5):* every type has a zero value in Go, so a moved-from place needs no drop flag: it simply holds the zero value, and dropping it is a no-op in every build mode.

**OWN-R6.** *Copies are explicit: `clone(x)`.*
- Clone is structural when all fields are clonable.
- Shared handles copy by retaining.
- A type with a `drop` method is clonable only if it defines `Clone() T`. This stops silent duplication of unique resources such as file descriptors. There is no `Clone` derive: a type with `drop` writes `Clone()` by hand, on purpose.
- A type that mixes owning fields with raw `*T` fields needs a hand-written `Clone()`, because an automatic clone would copy the raw pointers (for example a tree `Node` with a parent pointer).
- Imported C++ types clone through their copy constructor.

> **Open (P01 review):** the plan does not say whether `clone(x)` accepts trivial types. This spec reads it as yes: for a trivial `x`, `clone(x)` is a plain copy. Without this, `clonable` generics could not take `int`. Likewise, `clone(u)` of a `Unique[T]` is read as "allocate a new `T` holding `clone(*u)`" (requires clonable `T`).

```go
type Handle struct{ fd int; path string }       // has a drop method below: not clonable by default

func (h *Handle) Clone() Handle {                // written by hand, on purpose
    return Handle{fd: sys_dup(h.fd), path: clone(h.path)}
}
type Tree struct{ Kids []Tree; Up *Tree }        // mixes owning and raw fields:
// clone(t) is an error; write Tree.Clone() so Up is fixed up instead of copied

a := clone(h)             // calls Handle.Clone
b := clone(someShared)    // a Shared[T]: retains
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

**OWN-R8.** *No borrow checker.* Views and raw pointers can dangle, as in C++.
- The compiler **does** catch: returning `&local`; storing a borrow of a temporary; a `defer` or `go` that borrows something that dies first ([PAN-3](09-errors-and-panics.md), [CON-2](12-concurrency.md)); use after move.
- The cases it does **not** catch are listed in [chapter 16](16-undefined-behavior.md): views or raw pointers kept past their owner, and a borrowed parameter whose owner the callee frees through another path (`u.SetName(u.Name)`).
- `-asan` catches the rest at run time. v1 has no extra safety layer.

```go
p := &local                // allowed
return &local              // compile error: caught
s := name[:2]              // view
dropName()                 // frees name's buffer
fmt.Println(s)             // NOT caught: dangling view (UB-1). -asan finds it at run time
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
- `gopp vet` reports moving a local after its address, or a view into it, has been stored.
- Types marked `@Pinned` (the `sync` types) cannot be moved or copied after first use: a compile error, like Go vet's copylocks.
- `return local` builds the value in the caller's result slot, so it never moves.

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
    return new_unique(Node{Name: clone(name), Parent: parent}) // name is borrowed: clone it to own it
}

func (n *Node) Add(child own Unique[Node]) {
    child.Parent = n
    n.Children = append(n.Children, child)   // child moves into the slice (R3, R4)
}

func main() {
    root := NewNode("root", nil)
    root.Add(NewNode("a", root))   // the temporary moves into Add
    b := NewNode("b", root)
    root.Add(b)                    // b moves
    // fmt.Println(b.Name)         // compile error: b used after move
    head := root.Name[:2]          // view string: a borrow, no allocation
    fmt.Println(head, len(root.Children))
}                                  // root is dropped: the whole tree is freed
```

## 4.3 Pointers and heap allocation (D1.7)

Every heap allocation is spelled out. Go's `new` is removed.

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

**OWN-P5.** Field access and method calls **auto-dereference** every pointer kind. `Unique[T]` and `Shared[T]` have the methods of both `T` and `*T`.

**OWN-P6.** `Unique[T]` and `Shared[T]` convert **implicitly** to `*T` when borrowed from a place. They never convert from a temporary, because the target would be freed immediately.

**OWN-P7.** `make([]T, n)`, `make(map[K]V)` and `make(chan T, n)` work as in Go (with the allocator rules of [SLC-2](08-slices-and-range.md)).

**OWN-P8.** `Unique[T]` is owning (move-only). `Shared[T]` and `Weak[T]` are shared handles: copies retain the strong or weak count. `Weak[T]` copies retain the weak count only.

> **Open (P01 review):** the plan says `Unique[T]` is "nullable (zero value `nil`)" but does not list comparing with `nil`. This spec allows `u == nil` and `u != nil` for `Unique[T]`, `Shared[T]` and `Weak[T]`, as for any nullable pointer.

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

*Why (P1 to P8):* every allocation is spelled `new_unique` or `new_shared`, so nothing allocates behind a Go-looking `&T{}`; `*T` stays a raw pointer so Go pointer code and C pointers keep working.

## 4.4 Views (D1.6)

Syntax: `ViewType` is defined in [3.1](03-types.md). `view` is allowed in front of `string` and slice types only: `view string`, `view []byte`.

**OWN-V1.** `view string` (immutable bytes) and `view []T` (mutable elements, fixed length) are `(ptr, len)` borrows. They are trivial ([OWN-C1](#41-type-categories-d13)).

**OWN-V2.** *Slicing makes views.* `s[i:j]` on a `string` gives a `view string`. `xs[i:j]` on a slice or array gives a `view []T`. No allocation. `:=` infers the view type. The one exception is re-slicing an owned slice into itself: `s = s[i:j]` shrinks it in place ([SLC-6](08-slices-and-range.md)).

**OWN-V3.** *Conversions.*
- Owned to view is implicit wherever a view is expected (an argument, an assignment to a view variable, a comparison).
- View to owned is explicit and **copies**: `string(v)`, `[]T(v)`. `[]T(v)` needs `clonable` elements.
- Unlike Go, where converting between slice types never copies, here the copy is how you get ownership.

**OWN-V4.** *Map keys.* Maps accept a `view` key for lookups, so `counts[w]` with `w view string` does not allocate. Inserting with a view key clones the key.

**OWN-V4a.** *Map index on the left side.* `m[k] = v`, `m[k]++` and `m[k] op= v` **never consume** the key expression `k`. The key is borrowed for the lookup, and cloned only if a **new** entry is inserted. This holds for any key, view or not. So `for _, w := range words { counts[w]++ }` works as in Go (`w` is a borrowed range variable), and `m[s] = 1; fmt.Println(s)` is fine (`s` is not moved). The allocation (the key clone) happens only on insert, as for view keys. Inside a `range` over the same map, the rule of OWN-R4 "Map entries" applies: only `m[k]` with `k` the range key variable, not written earlier in the body, is allowed.

```go
counts := map[string]int{}
for _, w := range words { counts[w]++ }   // w borrowed: lookup borrows it; the first insert clones it
s := "k"
counts[s] = 1
fmt.Println(s)                            // ok: s was not moved
```

> **Open (P01 review):** D1.6 states the clone only for view keys. Extending it to every key is this spec's reading: otherwise `counts[w]++` over a borrowed range variable would need `counts[clone(w)]++` and allocate on every update. It adds "the key clone on a map insert" to the allocation list of principle 1.


**OWN-V5.** *Storing views.* Views may be stored and returned at the programmer's risk, like C++ `string_view` and `span`. `gopp vet` warns about a struct field of view type unless the struct is marked `@View` (for example tokens that point into a source buffer).

**OWN-V6.** *Temporaries.* A view of a temporary (`f()[1:3]`) lives only as long as the temporary: until the end of the full statement. In the header of a `for … range`, `if` or `switch` it lives until the end of the whole statement. Storing a borrow of a temporary in a longer-lived place is a compile error ([OWN-R8](#42-ownership-rules-d14)).

```go
@View
type Token struct {
    Text view string      // points into the source buffer; no vet warning thanks to @View
    Line int
}

func first(src string) view string {     // result is a view into src
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
    // h := strings.ToUpper(text)[:3]  // error: view of a temporary stored in h
}
```

*Why (V1 to V6):* a view is a borrow you can see in the type, so `s[i:j]` is free and a copy is always a visible `string(v)`; the dangling risk is the same as C++ `string_view` and is listed in chapter 16.

## 4.5 Rationale

- **Why ownership?** Deterministic, GC-free memory with C++-compatible object semantics, which C++ interop needs anyway, while keeping Go's zero values.
- **Why zero-on-move?** No drop flags and no partially dead variables. Moved-from state is always defined.
- **Why explicit `clone`?** Hidden copies are hidden allocations.
- **Why views?** They make borrows visible in types and let `s[i:j]` stay free.
- **Rejected:** tracing GC (excluded by requirements); ARC everywhere (hidden cost, cycles); a Rust-style borrow checker (cost in compile speed and simplicity; reconsider after v1); C++-style implicit copies.
