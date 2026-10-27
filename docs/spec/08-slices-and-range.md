# 8. Slices, maps and range

Base: Go spec [Slice types](https://go.dev/ref/spec#Slice_types), [Map types](https://go.dev/ref/spec#Map_types), [Slice expressions](https://go.dev/ref/spec#Slice_expressions), [Appending to and copying slices](https://go.dev/ref/spec#Appending_and_copying_slices), [Making slices, maps and channels](https://go.dev/ref/spec#Making_slices_maps_and_channels), [Length and capacity](https://go.dev/ref/spec#Length_and_capacity), [Deletion of map elements](https://go.dev/ref/spec#Deletion_of_map_elements), [Clear](https://go.dev/ref/spec#Clear), [For statements with range clause](https://go.dev/ref/spec#For_range).

Deltas covered: D1.20 (re-slicing in place), D1.21 (range loops), D1.22 (allocators), and the temporaries rule of D1.6.

Syntax: **no change**. All rules here are about meaning.

## 8.1 Layout and allocators (D1.22)

**SLC-1.** An owned `[]T` is 4 words: pointer, length, capacity, allocator. A map header also stores its allocator. A view (`view []T`, `view string`) is 2 words.

**SLC-2.** A **nil allocator means the global allocator**, so zero values stay valid. `make`, `append`, literals and conversions use the global allocator unless told otherwise.

**SLC-3.** Functions in package `mem` create a slice or map in another allocator, such as a `mem.Arena` (P05 D5.3). Growth (`append`), `clone` and drop use the value's **own** allocator. A clone stays in the same allocator.

**SLC-4.** Views and borrowed `[]T` parameters stay `(ptr, len)`: reading and writing elements needs no allocator. An `own []T` parameter passes all 4 words.

**SLC-5.** `string` always uses the global allocator, so strings stay 3 words. The allocator word points to an allocator object whose first word is its function table, so one pointer is enough.

```go
var arena mem.Arena
xs := mem.MakeIn[int](&arena, 0, 100)   // illustrative name: P05.03 sets the real ones
xs = append(xs, 1, 2, 3)                // grows inside the arena
ys := clone(xs)                         // ys also lives in the arena
arena.Reset()                           // xs and ys now dangle (UB-5)
```

*Why:* arena- and pool-backed collections keep the same `[]T` and `map` types everywhere. *Cost:* one more word in every owned slice and map header.

## 8.2 Re-slicing in place (D1.20)

**SLC-6.** `s = s[i:j]` on the **same** owned slice (same place on both sides) shrinks it in place. It drops the elements outside `[i, j)` and keeps the capacity.

**SLC-7.** For `i > 0` the remaining elements move down (O(n)), so a slice needs no extra offset field.

**SLC-8.** Any other slicing makes a view: `t := s[i:j]` gives a `view []T`; `t = s[i:j]` where `t` is a different owned variable is a type error (use `[]T(v)` to copy).

**SLC-9.** Reset a buffer with `buf = buf[:0]`: it drops all elements and keeps the capacity.

**SLC-10.** *Append-style functions* take `dst own []T`, and the call `buf = strconv.AppendInt(buf, n, 10)` moves implicitly ([OWN-R4](04-ownership.md#42-ownership-rules-d14)).

**SLC-11.** For fast FIFO queues, std has `container/deque`. Popping from the front of a slice with `s = s[1:]` is O(n).

**SLC-12.** `cap()` of a borrowed slice (a parameter or a view) equals its length.

> **Open (P01 review):** D1.20 does not say what `s = s[:n]` does when `len(s) < n <= cap(s)` (legal in Go). Elements past `len` are not live values. This spec reads it as a run-time panic (like a slice bounds error). The same reading applies to views: `xs[i:j]` with `j > len(xs)` panics, even if `j <= cap(xs)` (a view has no capacity). Likewise the three-index form `s[i:j:k]` is not defined for views (they have no capacity); this spec reads it as allowed only on arrays and not on slices, views or strings in v0.

```go
type Queue struct{ items []string }

func (q *Queue) Pop() string {
    first := move(q.items[0])
    q.items = q.items[1:]           // in place: drops nothing live (zero value left), shifts the rest
    return first
}

buf := make([]byte, 0, 64)
buf = strconv.AppendInt(buf, 42, 10)    // moves buf in and out: no copy
head := buf[:2]                         // a view of the first 2 bytes (len is 2 after AppendInt): not a shrink
buf = buf[:0]                           // reset, capacity kept (head now points at dropped data)
```

Compared with Go: `s = s[1:]` in Go is O(1) and keeps the old array alive. In Go++ it is O(n) and frees the dropped elements. Use `container/deque` for queues.

## 8.3 Range loops (D1.21)

Base: [For statements with range clause](https://go.dev/ref/spec#For_range).

```
RangeClause = [ ExpressionList "=" | IdentifierList ":=" ] "range" Expression .   /* unchanged */
```

**RNG-1.** `for i, x := range xs` gives `x` the element in the cheapest safe way:
- **trivial elements** are copied into `x`, as in Go and as for trivial parameters ([PAR-4](05-parameters.md)). Changing the element later does not change `x`;
- **shared-handle elements** (`[]func()`, `[]error`, `[]chan T`, `[]Shared[T]`) are copied with a retain, as in Go and as D1.3 says ("implicit; copying retains"). So `handlers[0] = nil; f()` works inside the loop and `x` can be captured by an escaping closure;
- **owning elements** are **borrowed**: `x` is a borrowed variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)), read-only. To keep it, write `clone(x)` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)).

> **Open (P01 review):** D1.21 says only that `range` "borrows each element". For trivial and shared-handle elements this spec reads that as a copy (with a retain for handles), because a borrow would silently differ from Go (`ss[0] = 10` while ranging would change `x`). For shared-handle elements the range variable is a retained copy, so it cannot dangle ([UB-2a](16-undefined-behavior.md) and [CLO-3a](11-closures.md) apply to owning elements only). For owning elements, using `x` after the element was written is a compile error when the compiler can see both, and otherwise UB ([RNG-3](08-slices-and-range.md), [UB-2a](16-undefined-behavior.md)).

**RNG-2.** `for _, x := range move(xs)` **moves** the elements out one by one. `x` is an owned local per iteration. After the loop `xs` holds the zero value.

> **Open (P01 review):** the plan does not say what happens to elements not yet iterated when the loop ends early (`break`, `return`, `goto` out, panic). This spec reads it with [OWN-R1, OWN-R2](04-ownership.md#42-ownership-rules-d14): the moved-out slice is a temporary of the `for` header, owned by the statement, and each element not yet moved out is destroyed when the statement ends (on every path, including panic), in increasing index order. Elements already moved out belong to the per-iteration variable and are destroyed at the end of their iteration.

**RNG-3.** Inside the loop body, a statement that can reallocate the ranged slice, map or owned string is a **compile error** when the compiler can see it:
- appending to it, or growing an owned string with `+=` (STM-8 grows in place and can free the buffer);
- assigning it, or (slices only) re-slicing it in place (`s = s[i:j]`);
- inserting into the map: inside `range m`, any `m[x] = …`, `m[x]++`, `m[x] op= …` or `m[x] = append(m[x], …)` where `x` is not the current range key variable (or is `k` but `k` was written earlier in the body, e.g. `k++` on a trivial key) counts as a visible potential insert (the compiler cannot know whether `x` exists). Only `m[k]` with `k` the range key variable, not written earlier in the body, is a write to the current entry. Fix-it: collect the changes and apply them after the loop;
- calling `clear` on it.

For an owned string: `for _, r := range s { s += "x" }` is a compile error. Fix-its: range over `clone(s)`, or build into a new variable.

`delete` on the ranged map is allowed, and so is removing an entry with `move(m[k])` or `v, ok := move(m[k])` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Removing the entry the loop is **currently on**, by either form, takes it out of the map at once, but whatever stays owned by the map side is dropped only at the end of that iteration: the key (both forms) and the value (`delete` only). A value moved out belongs to its new owner, and the slot it left holds the zero value (OWN-R5) until the end of the iteration. So `k` stays valid, and a borrowed `v` reads the zero value after `move(m[k])` (defined, but surprising, like `move(xs[i])` below), never the moved-out buffer. After `delete(m, k)`, `v` still reads the old value. Removing any **other** entry drops it at once.

Writing the element or entry the loop is on (`xs[i] = y`, `m[k] = y`) is **defined**: the old value is dropped. The problem is using the borrowed range variable ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) **after** such a write, because it borrowed the dropped value. When the compiler can see the write and a later use of the range variable in the body, that is a compile error (the same visible-write reading as [TYP-9](03-types.md)); otherwise it is UB ([UB-2a](16-undefined-behavior.md)). Range variables of trivial or shared-handle elements are copies and are not affected.

> **Open (P01 review):** D1.21 does not mention writes to the current element. The visible-write compile error is this spec's reading, matching the visible-write rule for assertion bindings (TYP-9) and RNG-3's "when the compiler can see it".

Map range variables with an owning key or value type are borrowed variables ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)): `move(k)` and `move(v)` are compile errors; write `clone(k)` / `clone(v)`, or `move(m[k])` to take the value out. A map range variable of trivial type (for example the `int` values of `map[string]int`) is a writable copy.

> **Open (P01 review):** D1.21 and D1.4 rule 3a do not say whether a map range variable can be moved. Moving the key would corrupt the map and moving the value is ambiguous with `move(m[k])`, so this spec makes it an error.

> **Open (P01 review):** D1.21 allows `delete` during `range` but does not say when the entry's key and value are dropped, and P05 D5.4 says keys and values are dropped on delete. The common Go idiom `for k, v := range m { if expired(v) { delete(m, k); log.Println("expired", k) } }` must not read freed memory, and neither must `for k := range m { v := move(m[k]); fmt.Println(k, v) }`, so this spec delays the drop for the current entry; this changes the timing of P05 D5.4 inside a `range` only, and it also fixes what a borrowed `v` reads after `move(m[k])`: the zero value, not the moved-out bytes (the map already counts its active iterators, D1.21 / [RNG-4](08-slices-and-range.md), so it knows a `range` is active). The loop variables of other entries are not alive, so nothing else is borrowed, unless the program kept a view or pointer into a removed entry: that is UB, as in [UB-2a](16-undefined-behavior.md).

Other operations on the ranged collection: `append`, assigning the collection, inserting a key and `clear` are compile errors (above); assigning an element or entry the loop is on, or `copy` into the ranged slice, is defined, and a later use of the borrowed range variable is a compile error when the compiler can see it and otherwise [UB-2a](16-undefined-behavior.md); `move(xs[i])` leaves the zero value in the element, so a borrowed `x` reads the zero value (defined, but surprising). Re-slicing the ranged slice in place is an assignment, so it is a compile error (strings have no in-place re-slice, [OWN-V2](04-ownership.md#44-views-d16)).

**RNG-3a.** *Evaluation of the range expression.* As in Go, the range expression is evaluated **once**, at loop start. For a slice or a string, the loop takes `(ptr, len)` at that moment and walks that buffer. If the buffer is reallocated or freed during the loop where the compiler cannot see it (a hidden `+=` on the ranged string is the same; for example `for _, v := range w.queue { w.push(10) }` where `push` appends to `w.queue`), the loop reads freed memory: this is UB, even for `[]int` ([UB-2e](16-undefined-behavior.md)). `-asan` catches it.

> **Open (P01 review):** in Go the old array stays alive, so the loop is safe; in Go++ the old buffer is freed (D1.20, P05 D5.4). A run-time check like the map iterator count (RNG-4) would catch this, but it would cost a counter in every slice header or a check per iteration, so this spec rejects it. The visible cases are already compile errors (RNG-3).

**RNG-4.** Maps count their active iterators. In the cases the compiler cannot see, an insert during iteration **panics** with "map grew during iteration" only when the insert would make the table grow (P05 D5.4); an insert that fits in the current table is allowed.

> **Open (P01 review):** D1.21 and P05 D5.4 agree on the panic, but it depends on the map's capacity at that moment, so it is not deterministic across runs and sizes. Go allows such inserts in every case.

**RNG-5.** Go allows inserting into a map during `range`, so this is a listed difference from Go (P04 D4.10).

**RNG-6.** *Other range sources* work as in Go: strings (runes), arrays, integers, channels, functions. Ranging over a channel receives, and so moves each value out (`v` is an owned local per iteration, [CON-4](12-concurrency.md)). Imported C++ containers range through `begin()`/`end()` (P08 D8.6).

**Arrays.** Go ranges over a *copy* of an array. Go++ keeps that for arrays with **trivial** elements (copied up front, exactly like Go). Arrays with **owning** elements are *not* copied, because that would be a hidden allocation (principle 1): the range variable borrows the live element, so the array follows **exactly** the slice rules of RNG-3 and [UB-2a](16-undefined-behavior.md): writing an element is defined, using a borrowed range variable after its own element was written is a compile error when visible and otherwise UB, and writes to other elements are fine. This is a listed difference from Go.

> **Open (P01 review):** D1.21 and RNG-6 say nothing about the copy Go makes for arrays (repro: `.scratch/r_rangearray`, Go ranges over the old values after a write inside the loop). The split by element category is this spec's reading.

> **Open (P01 review):** `range` over a C++ container is stated only in P08 D8.6 and not in D1.21. The type of the loop variable (the element value, or a raw `*T` because `*it` returns `T&`, D8.7) and whether it borrows or copies are not specified. This spec leaves it to P08. Sample 07 assumes the element value.

> **Open (P01 review):** range-over-func (Go 1.23) is part of the Go 1.25 base. The plan does not say how ownership works for values passed to the `yield` function. This spec reads it as: yielded values of owning type are borrowed like any borrowed parameter, and the loop body is a non-escaping closure ([CLO-1](11-closures.md)) **because** Go's run-time check is kept: calling `yield` after the loop has exited panics ("range function continued iteration after whole loop exit"), as in Go. This is an explicit exception to [CLO-4](11-closures.md)'s "a call through a func value assumes the parameter is kept": the iterator function receives `yield` as a func value that the loop invalidates when it ends.

**RNG-7.** *Temporaries in the header.* The range expression is evaluated once. If it creates temporaries, they live until the end of the whole `for` statement ([OWN-R2](04-ownership.md#42-ownership-rules-d14), [STM-4](07-statements.md)).

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
- Borrowing in `range` is the cheapest default and matches Go's reading code. Consuming needs an explicit `move(xs)`, so it is visible at the loop.
- The compile errors in RNG-3 catch the common bug (appending while ranging) early. The run-time counter covers the rest at a small cost.
