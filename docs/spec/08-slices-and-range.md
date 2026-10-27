# 8. Slices, maps and range

Base: Go spec [Slice types](https://go.dev/ref/spec#Slice_types), [Map types](https://go.dev/ref/spec#Map_types), [Slice expressions](https://go.dev/ref/spec#Slice_expressions), [Appending to and copying slices](https://go.dev/ref/spec#Appending_and_copying_slices), [Making slices, maps and channels](https://go.dev/ref/spec#Making_slices_maps_and_channels), [Length and capacity](https://go.dev/ref/spec#Length_and_capacity), [Deletion of map elements](https://go.dev/ref/spec#Deletion_of_map_elements), [Clear](https://go.dev/ref/spec#Clear), [For statements with range clause](https://go.dev/ref/spec#For_range).

Deltas covered: D1.20 (re-slicing in place), D1.21 (range loops), D1.22 (allocators), and the temporaries rule of D1.6.

Syntax: one change, the `&` mark in the range clause ([8.3](#83-range-loops-d121)). All other rules here are about meaning.

## 8.1 Layout and allocators (D1.22)

**SLC-1.** An owned `[]T` is 4 words: pointer, length, capacity, allocator. A map header also stores its allocator. A ref (`ref []T`, `ref string`) is 2 words.

**SLC-2.** A **nil allocator means the global allocator**, so zero values stay valid. `make`, `append`, literals and conversions use the global allocator unless told otherwise.

**SLC-3.** Functions in package `mem` create a slice or map in another allocator, such as a `mem.Arena` (P05 D5.3). Growth (`append`), `Clone` and drop use the value's **own** allocator. A clone stays in the same allocator.

**SLC-4.** Refs and borrowed `[]T` parameters stay `(ptr, len)`: reading and writing elements needs no allocator. A `move` `[]T` parameter passes all 4 words.

**SLC-5.** `string` always uses the global allocator, so strings stay 3 words. The allocator word points to an allocator object whose first word is its function table, so one pointer is enough.

```go
var arena mem.Arena
xs := mem.MakeIn[int](&arena, 0, 100)   // illustrative name: P05.03 sets the real ones
xs = append(xs, 1, 2, 3)                // grows inside the arena
ys := xs.Clone()                         // ys also lives in the arena
arena.Reset()                           // xs and ys now dangle (UB-5)
```

*Why:* arena- and pool-backed collections keep the same `[]T` and `map` types everywhere. *Cost:* one more word in every owned slice and map header.

## 8.2 Re-slicing in place (D1.20)

**SLC-6.** `s = s[i:j]` on the **same** owned slice (same place on both sides) shrinks it in place. It drops the elements outside `[i, j)` and keeps the capacity.

**SLC-7.** For `i > 0` the remaining elements move down (O(n)), so a slice needs no extra offset field.

**SLC-8.** Any other slicing makes a ref: `t := s[i:j]` gives a `ref []T`; `t = s[i:j]` where `t` is a different owned variable is a type error (use `[]T(v)` to copy).

**SLC-9.** Reset a buffer with `buf = buf[:0]`: it drops all elements and keeps the capacity.

**SLC-10.** *Append-style functions* take `move dst []T`, and the call `buf = strconv.AppendInt(buf, n, 10)` moves implicitly ([OWN-R4](04-ownership.md#42-ownership-rules-d14)).

**SLC-11.** For fast FIFO queues, std has `container/deque`. Popping from the front of a slice with `s = s[1:]` is O(n).

**SLC-12.** `cap()` of a borrowed slice (a parameter or a ref) equals its length.

**SLC-13.** *Re-slicing past the length.* `s = s[:n]` with `len(s) < n <= cap(s)` (legal in Go) is a run-time **panic**, like a slice bounds error: the elements past `len` were already freed, so they are not live values. The same holds for refs: `xs[i:j]` with `j > len(xs)` panics, even if `j <= cap(xs)` (a ref has no capacity). The three-index form `s[i:j:k]` is allowed only on arrays; it is not defined for slices, refs or strings in v0.

```go
s := []string{"a", "b", "c"}
s = s[:0]
s = s[:2]            // panic: slice bounds out of range (the items were freed by s[:0])
```

```go
type Queue struct{ items []string }

func (q *Queue) Pop() string {
    first := move(q.items[0])
    q.items = q.items[1:]           // in place: drops nothing live (zero value left), shifts the rest
    return first
}

buf := make([]byte, 0, 64)
buf = strconv.AppendInt(buf, 42, 10)    // moves buf in and out: no copy
head := buf[:2]                         // a ref into the first 2 bytes (len is 2 after AppendInt): not a shrink
buf = buf[:0]                           // reset, capacity kept (head now points at dropped data)
```

Compared with Go: `s = s[1:]` in Go is O(1) and keeps the old array alive. In Go++ it is O(n) and frees the dropped elements. Use `container/deque` for queues.

## 8.3 Range loops (D1.21)

Base: [For statements with range clause](https://go.dev/ref/spec#For_range).

```
RangeClause = [ ExpressionList "=" | RangeVars ":=" ] "range" Expression .
RangeVars   = RangeVar [ "," RangeVar ] .
RangeVar    = [ "&" ] identifier .
```

`&` marks an *alias* variable ([RNG-1a](#83-range-loops-d121)). It is allowed only on the **value** variable: the second variable for slices, arrays, maps and C++ containers (`for _, &d := range xs`, `for k, &v := range m`), and the first variable when the iterator yields one pointer value (`for &d := range seq`, [RNG-6a](#83-range-loops-d121)). It is never allowed on the key or index. It is only part of `:=`, not of the `=` form.

**RNG-1.** `for i, x := range xs` gives `x` the element in the cheapest safe way:
- **trivial elements** are copied into `x`, as in Go and as for trivial parameters ([PAR-4](05-parameters.md)). Changing the element later does not change `x`. Use `&x` to change the real item;
- **shared-handle elements** (`[]func()`, `[]error`, `[]chan T`, `[]Shared[T]`) are copied with a retain, as in Go and as D1.3 says ("implicit; copying retains"). So `handlers[0] = nil; f()` works inside the loop and `x` can be captured by an escaping closure. Because the range variable is a retained copy, it cannot dangle ([UB-2a](16-undefined-behavior.md) and [CLO-3a](11-closures.md) apply to owning elements only);
- **owning elements** are **borrowed**: `x` is a borrowed variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)), read-only, like Rust's `for d in &docs`. To keep it, write `x.Clone()` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). To change it, use the `&` form (RNG-1a). The error message for a write to a plain range variable suggests `&x` first.

**RNG-1a.** *The `&` form: the element itself.* In `for _, &d := range xs`, `d` is **the element itself**, not a copy and not a read-only borrow. It works for every element category.
- Writes change the collection: `d.Title = "x"`, `d.Count++`, `d = Doc{}` (the old value is dropped), and pointer-receiver calls such as `d.Upper()`.
- The `&` form covers slices, arrays, map values (`for k, &v := range m { v.Count++ }`), imported C++ containers ([RNG-6](#83-range-loops-d121)) and iterators that yield `*T` ([RNG-6a](#83-range-loops-d121)). The key can't use `&`. Strings, integers and channels have no element to alias, so `&` there is a compile error.
- **Arrays follow [OWN-R3b](04-ownership.md#42-ownership-rules-d14).** `for _, &d := range arr` is allowed when the array is an owned local, or is reached through a pointer or a slice. It is a compile error when the array is held **by value** inside a borrowed variable (a borrowed parameter, a plain range variable, an assertion binding), because the writes would go into borrowed storage. A package-level array, or an array field held by value inside an owned local, is allowed.
- **Map aliases.** A `defer` (or any later use after the loop) that keeps the `&v` alias of `for k, &v := range m` is a compile error, because later inserts can rehash the map and move the value. Except for map aliases, a `defer` may keep an `&` alias (see below). Fix-it: call it inside the loop (`v.Flush()`), or keep a copy (`c := v.Clone(); defer c.Flush()`).
- **`&` and `defer`/`go`.** An `&` range variable is an alias of an element, so for [PAN-3](09-errors-and-panics.md) and [CLO-3b](11-closures.md) it counts as a **place** (`xs[i]`), not as an owned loop-body local. `defer f.Close()` evaluates `&xs[i]`: fine if the collection lives until the function returns, a compile error if the collection is a local of the loop body. This holds except for map aliases (above). For a slice, `defer` keeping `&xs[i]` stays legal while `xs` lives until return; a later `append` or assignment to `xs` before return that the compiler can see is a compile error, and a hidden one is [UB-1](16-undefined-behavior.md) (a ref into a slice element after `append` reallocated it). `defer show(f)` follows the borrowed-argument rule for places (the collection lives until return and `f` is not written after). A deferred literal that captures `f` follows CLO-3b.
- `move(d)` is allowed. It takes the value out and leaves the **zero value** in the element: `for _, &d := range xs { keep(move(d)) }` leaves the zero value in `xs[i]`. For a map, `for k, &v := range m { keep(move(v)) }` leaves the zero value in `m[k]` and the key stays.
- It never copies: for an array the `&` form ranges over the live array, not over a copy ([RNG-6](#83-range-loops-d121)).
- An alias variable is a place inside the collection, so the rules for other places apply ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)): reading borrows, and taking ownership needs `d.Clone()` or `move(d)`. Like a borrowed variable, it cannot be captured by an escaping closure or handed to `go` ([CLO-3a](11-closures.md), [CON-1](12-concurrency.md)).
- Growing the collection inside the loop stays a compile error ([RNG-3](#83-range-loops-d121)).
- `m[k].f = x` outside a loop stays illegal, as in Go. Inside the loop, write `for k, &v := range m { v.f = x }`.

```go
for _, &d := range docs { d.Title = "x" }             // changes docs[i].Title
for k, &v := range m { v.Count++ }                    // changes the map value in place
for _, &x := range nums { x *= 2 }                    // trivial elements: changes the real items
for _, &d := range xs { keep(move(d)) }               // xs[i] now holds the zero value
for k, &v := range m { keep(move(v)) }                // m[k] now holds the zero value; k stays
for _, d := range docs { d.Title = "x" }              // error: d is read-only. Fix-it: for _, &d := range docs
for k, &v := range m { m[k+"!"] = v.Clone() }         // error: inserts into the ranged map (RNG-3)

type Grid struct{ Cells [4]Doc }
func local()           { var a [4]Doc; for _, &d := range a { d.N++ } }        // ok: owned local array
func viaPtr(g *Grid)   { for _, &d := range g.Cells { d.N++ } }                // ok: reached through a pointer
func viaSlice(gs []Grid) { for _, &d := range gs[0].Cells { d.N++ } }          // ok: reached through a slice
func byValue(g Grid)   { for _, &d := range g.Cells { d.N++ } }                // error: array held by value in a borrowed parameter
```

*Why:* Go's `for _, d := range docs { d.Title = "x" }` silently changes a copy. A writable borrow would be a surprise in the other direction. The `&` makes "I want the real element" visible at the loop, and it reads like C++ `auto&`.

**RNG-2.** `for _, x := range move(xs)` **moves** the elements out one by one. `x` is an owned local per iteration. After the loop `xs` holds the zero value.

If the loop stops early (`break`, `return`, `goto` out, panic), the moved-out slice is a temporary of the `for` header, owned by the statement ([OWN-R1, OWN-R2](04-ownership.md#42-ownership-rules-d14)). Each element not yet moved out is freed when the statement ends, on every path including panic, in increasing index order. Elements already moved out belong to the per-iteration variable and are destroyed at the end of their iteration.

**RNG-3.** Inside the loop body, a statement that can reallocate the ranged slice, map or owned string is a **compile error** when the compiler can see it. This holds for the plain form and the `&` form:
- appending to it, or growing an owned string with `+=` (STM-8 grows in place and can free the buffer);
- assigning it, or (slices only) re-slicing it in place (`s = s[i:j]`);
- inserting into the map: inside `range m`, any `m[x] = …`, `m[x]++`, `m[x] op= …` or `m[x] = append(m[x], …)` where `x` is not the current range key variable (or is `k` but `k` was written earlier in the body, e.g. `k++` on a trivial key) counts as a visible potential insert (the compiler cannot know whether `x` exists). Only `m[k]` with `k` the range key variable, not written earlier in the body, is a write to the current entry. Fix-it: collect the changes and apply them after the loop;
- calling `clear` on it.

For an owned string: `for _, r := range s { s += "x" }` is a compile error. Fix-its: range over `s.Clone()`, or build into a new variable.

`delete` on the ranged map is allowed, and so is removing an entry with `move(m[k])` or `v, ok := move(m[k])` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Removing the entry the loop is **currently on**, by either form, takes it out of the map at once. What stays owned by the map side is freed only at the end of that iteration: the key (both forms) and the value (`delete` only). So `k` and `v` stay valid to the end of the turn. A value moved out belongs to its new owner, and the slot it left holds the zero value ([OWN-R5](04-ownership.md#42-ownership-rules-d14)) until the end of the iteration: after `move(m[k])` a borrowed `v` reads the zero value (defined, but surprising), never the moved-out buffer. After `delete(m, k)`, `v` still reads the old value. Removing any **other** entry frees it at once. The map already counts its active iterators ([RNG-4](#83-range-loops-d121)), so it knows a `range` is active. This delay changes the timing of P05 D5.4 inside a `range` only. The loop variables of other entries are not alive, so nothing else is borrowed, unless the program kept a ref or pointer into a removed entry: that is UB, as in [UB-2a](16-undefined-behavior.md).

Writing the element or entry the loop is on (`xs[i] = y`, `m[k] = y`) is **defined**: the old value is dropped. For the plain form, the problem is using the borrowed range variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) **after** such a write, because it borrowed the dropped value. When the compiler can see the write and a later use of the range variable in the body, that is a compile error (the same visible-write reading as [TYP-9](03-types.md)); otherwise it is UB ([UB-2a](16-undefined-behavior.md)). Range variables of trivial or shared-handle elements are copies and are not affected. An `&` variable is an alias of the slot, so it sees the new value and is not affected either.

Map range variables with an owning key or value type are borrowed variables in the plain form ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)). `move(k)` and `move(v)` are compile errors, because moving a key would corrupt the map and moving a value is ambiguous with `move(m[k])`. Write `v.Clone()`, or use `&v`, or `move(m[k])` to take the value out. A map range variable of trivial type (for example the `int` values of `map[string]int`) is a writable copy. The key can never be moved or aliased.

Other operations on the ranged collection: `append`, assigning the collection, inserting a key and `clear` are compile errors (above); assigning an element or entry the loop is on, or `copy` into the ranged slice, is defined, and a later use of the borrowed range variable is a compile error when the compiler can see it and otherwise [UB-2a](16-undefined-behavior.md); `move(xs[i])` leaves the zero value in the element, so a borrowed `x` reads the zero value (defined, but surprising). Re-slicing the ranged slice in place is an assignment, so it is a compile error (strings have no in-place re-slice, [OWN-V2](04-ownership.md#44-refs-d16)).

**RNG-3a.** *Evaluation of the range expression.* As in Go, the range expression is evaluated **once**, at loop start. For a slice or a string, the loop takes `(ptr, len)` at that moment and walks that buffer. If the buffer is reallocated or freed during the loop where the compiler cannot see it (a hidden `+=` on the ranged string is the same; for example `for _, v := range w.queue { w.push(10) }` where `push` appends to `w.queue`), the loop reads freed memory: this is UB, even for `[]int` ([UB-2e](16-undefined-behavior.md)). `-asan` catches it.

In Go the old array stays alive, so the same loop is safe there. In Go++ the old buffer is freed (D1.20, P05 D5.4). A run-time check like the map iterator count (RNG-4) would cost a counter in every slice header or a check per iteration, so there is none. The visible cases are already compile errors (RNG-3).

**RNG-4.** Maps count their active iterators. In the cases the compiler cannot see, inserting a **new key** into a map while any `range` over that map is running **always panics**, with "map insert during iteration". It does not depend on the table's size or capacity, so it behaves the same on every run. Writing a key that already exists is not an insert and is allowed. The visible inserts are compile errors ([RNG-3](#83-range-loops-d121)).

```go
for k := range m { addKey(m) }     // addKey inserts a new key: panics every time
```

**RNG-5.** Go allows inserting into a map during `range`, so this is a listed difference from Go (P04 D4.10).

**RNG-6.** *Other range sources.*
- Strings (runes), integers and channels work as in Go. Ranging over a channel receives, and so moves each value out (`v` is an owned local per iteration, [CON-4](12-concurrency.md)).
- **Arrays.** Go ranges over a *copy* of an array. Go++ keeps that for arrays with **trivial** elements in the plain form (copied up front, exactly like Go). Arrays with **owning** elements are *not* copied, because that would be a hidden allocation (principle 1): the range variable borrows the live element, so the array follows **exactly** the slice rules of RNG-3 and [UB-2a](16-undefined-behavior.md): writing an element is defined, using a borrowed range variable after its own element was written is a compile error when visible and otherwise UB, and writes to other elements are fine. The `&` form never copies the array. This is a listed difference from Go.
- **Imported C++ containers** (anything with `begin()`/`end()`, P08 D8.6) work like a slice for the two-variable and `&` forms. `for _, x := range vec` is a read-only borrow of each element: no C++ copy constructor runs (trivial element types are copied, as in RNG-1). `for _, &x := range vec { x += 1 }` changes the real element, like C++ `auto&`. The one-variable form `for x := range c` gives the index for indexable containers and the key for associative ones (`std::map`); for containers with neither (`std::list`) it is a compile error, so use `for _, x := range c`. For associative containers the two-variable form gives the key and the mapped value, like a Go map (`for k, v := range cppmap`; `&v` aliases the mapped value).

**RNG-6a.** *Range over a function.* Range-over-func (Go 1.23) follows the slice-loop rules.
- `for x := range seq` borrows each yielded value read-only, like a borrowed parameter: `x` is a borrowed variable when the yielded type is owning. Write `x.Clone()` to keep it.
- `for &x := range seq` changes the real item. It is allowed only when the iterator yields the real items as pointers (`iter.Seq[*Doc]`); otherwise it is a compile error. `x` is then the `Doc` the pointer points to. For `iter.Seq2`, `&` goes on the second variable, under the same condition.
- The loop body is a non-escaping closure ([CLO-1](11-closures.md)), **because** Go's run-time check is kept: calling `yield` after the loop has exited panics ("range function continued iteration after whole loop exit"), as in Go. This is an explicit exception to [CLO-4](11-closures.md)'s "a call through a func value assumes the parameter is kept": the iterator function receives `yield` as a func value that the loop invalidates when it ends.

```go
func (l *DocList) All() iter.Seq[*Doc] { … }

for &d := range docs.All() { d.Title = "x" }     // ok: the iterator yields *Doc
for d := range docs.All() { use(d) }             // d is a *Doc (a writable copy of the pointer); (*d).Clone() copies the Doc

func (l *DocList) Values() iter.Seq[Doc] { … }
for d := range docs.Values() { use(d) }          // d is a Doc: read-only; d.Clone() keeps it
```

**RNG-7.** *Temporaries in the header.* The range expression is evaluated once. If it creates temporaries, they live until the end of the whole `for` statement ([OWN-R2](04-ownership.md#42-ownership-rules-d14), [STM-4](07-statements.md)).

**RNG-8.** *No redeclaring a loop variable.* Declaring a variable with the same name as a loop variable inside that loop's body, in the body block or in any block nested in it, is a **compile error**. This covers `u := u`, `u := u.Clone()` and `var u = u`, for the variables of a range clause and of a `for` clause. The parameters of a function literal inside the loop body count too, so `go func(u User){…}(u.Clone())` is an error: rename the parameter. Old Go code that wrote `u := u` (needed before Go 1.22) must rename the copy. Fix-its never reuse the name: `mine := u.Clone()`, or `go fetch(u.Clone())`, or `range move(users)` ([CLO-3a](11-closures.md)).

```go
for _, u := range users {
    u := u.Clone()               // error: u redeclares the loop variable. Use another name
    mine := u.Clone()            // ok
}
```

```go
for _, w := range strings.Fields(text) {     // the slice returned by Fields is a temporary:
    fmt.Println(w)                           // it lives until the loop ends
}

names := []string{"a", "b"}
for _, n := range names {
    // names = append(names, n)   // compile error: reallocates the ranged slice
    // m[n] = 1  with m ranged    // compile error
}

for i, n := range move(names) {   // n is owned in each iteration
    jobs <- Job{ID: i, Text: n}   // n moves into the struct, then the channel
}
// names is now the zero value (nil)

m := map[string]int{"a": 1, "b": 2}
for k := range m { delete(m, k) } // allowed
```

## 8.4 Rationale

- In-place re-slicing needs no offset field and frees memory as soon as it is not needed. The cost, O(n) for `s[1:]`, is paid only where Go code uses a slice as a queue, and `container/deque` replaces that pattern.
- Allocators in the type keep one `[]T` type for heap and arena data. The alternative (a separate `ArenaSlice[T]`) would split every generic API in two.
- Borrowing in `range` is the cheapest default and matches Go's reading code. Changing the real element needs the `&` form, and consuming needs an explicit `move(xs)`, so both are visible at the loop.
- The compile errors in RNG-3 catch the common bug (appending while ranging) early. The run-time iterator count covers map inserts the compiler cannot see, and it panics every time, so the bug does not depend on table size.
