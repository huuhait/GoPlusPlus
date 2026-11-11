# 8. Slices, maps and range

> **Being rewritten.** [ADR-0041](../adr/0041-values-are-copied.md) (2026-11-11) replaces the shared-handle model of slices and maps: a value is a copy, moving is `move(x)`, sharing is a pointer or `Shared[T]`. Rules below that speak of shared slices or maps, reference counts on their buffers, or "unshared" describe the earlier model.

Base: Go spec [Slice types](https://go.dev/ref/spec#Slice_types), [Map types](https://go.dev/ref/spec#Map_types), [Slice expressions](https://go.dev/ref/spec#Slice_expressions), [Appending to and copying slices](https://go.dev/ref/spec#Appending_and_copying_slices), [Making slices, maps and channels](https://go.dev/ref/spec#Making_slices_maps_and_channels), [Length and capacity](https://go.dev/ref/spec#Length_and_capacity), [Deletion of map elements](https://go.dev/ref/spec#Deletion_of_map_elements), [Clear](https://go.dev/ref/spec#Clear), [For statements with range clause](https://go.dev/ref/spec#For_range).

Deltas covered: D1.20 (slicing and `:0` reset), D1.21 (range loops), D1.22 (allocators), D1.6, and [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md).

Syntax: one change, the `&` mark in the range clause ([8.3](#83-range-loops-d121)). All other rules here are about meaning.

## 8.1 Layout and allocators (D1.22, ADR-0029, ADR-0032)

**SLC-1.** A `[]T` value is 4 words: `{ptr, len, cap, backing}` (32 bytes on a 64-bit target, so `unsafe.Sizeof([]int{})` is 32). When heap- or arena-allocated, `backing` points to a shared backing header `{refcount, alloc_cap, allocator}`; for `nil`, empty slices, or borrowed stack-array slices `arr[i:j]`, `backing` is `nil`. A `map[K]V` value is 1 word pointing to a runtime header `{refcount, allocator, …}` ([ADR-0029](../adr/0029-slice-and-map-allocator-word.md), [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)).

**SLC-2.** A **nil allocator in the backing or map header means the global allocator**, so zero values stay valid. `make`, `append`, literals and conversions use the global allocator unless told otherwise; the global allocator is mimalloc ([ADR-0027](../adr/0027-mimalloc-global-allocator.md)). In Go++ code an allocator is a `mem.Allocator`, an interface with methods, so it is never nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)); an allocator parameter or field that may be absent is written `Option[mem.Allocator]`, nil meaning the global allocator.

**SLC-3.** Functions in package `mem` create a slice or map in another allocator, such as a `mem.Arena` (P05 D5.3; `mem.Arena` and `mem.DropArena` free all chunks automatically when their owning scope ends). Because the allocator lives in the `backing` header, **sub-slices `s[i:j]` and lent `[]T` parameters preserve the allocator**: growth (`append`), `Clone` and drop all use the slice's own allocator. Only a slice with `backing == nil` (a nil/empty slice or a stack-array slice `arr[i:j]`) has no backing allocator, so `Clone` or `append` on it allocates in the global allocator.

**SLC-4.** A `[]T` parameter has the same 4-word `{ptr, len, cap, backing}` representation and is lent at 0 retain/release at the call boundary ([PAR-1](05-parameters.md)). Mutating elements `s[i] = v` mutates the shared backing array, as in Go; if the callee stores or returns `s`, it retains `backing`.

**SLC-4a.** A slice or a map is never passed to C by value, so C never sees the slice or map header. A slice argument for a C pointer parameter becomes a pointer to its elements ([FOR rules](14-foreign-surface.md)); the C ABI refuses a slice or a map in a C signature (P04.11).

**SLC-5.** `string` is 3 words: `{ptr, len, backing}` (`backing == nil` for static string literals and `""`; heap `backing` holds `{refcount, byte_cap}` plus a trailing NUL byte and always uses the global allocator). There is no small-string optimization: the text is never stored inside the string value ([ADR-0026](../adr/0026-no-small-string-optimization.md)).

```go
var arena mem.Arena
xs := mem.MakeIn[int](&arena, 0, 100)   // illustrative name: P05.03 sets the real ones
xs = append(xs, 1, 2, 3)                // grows inside the arena
sub := xs[1:]                           // O(1) sub-slice; still carries arena backing
ys := sub.Clone()                       // ys also lives in the arena!
arena.Reset()                           // compile error if xs, sub or ys is used later (OWN-B2, UB-5)
```

Normal code never names the allocator and is the same as Go:

```go
xs := []int{1, 2, 3}
xs = append(xs, 4)                       // grows with the global allocator
fmt.Println(unsafe.Sizeof(xs))           // 32
```

*Why:* arena- and pool-backed collections keep the same `[]T` and `map[K]V` types everywhere, and sub-slicing preserves the allocator automatically.

## 8.2 Slicing, shared buffers and `:0` reset (D1.20, ADR-0032)

**SLC-6.** *Slicing is $O(1)$ and shares backing storage.* Slicing a `string` (`w := s[i:j]`, `s = s[i:j]`) returns a `string` sharing `s`'s `backing` ([OWN-S1](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032)). Slicing a `[]T` (`t := xs[i:j]`, `xs = xs[i:j]`) returns a `[]T` with `{ptr + i, j - i, cap - i, backing}` sharing `xs`'s `backing` ([OWN-S2](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032)). When `xs` is reassigned to its own sub-slice (`xs = xs[i:j]`) and no other variable retains `backing`, no retain or release runs at all.

**SLC-7.** *Front pop `s = s[1:]` is $O(1)$.* Advancing `ptr` in `s = s[1:]` takes $O(1)$ time with no element memmove, matching Go. The shared backing allocation is freed deterministically when the last slice referencing `backing` is dropped or reassigned. (If `s` holds large resources in skipped prefix elements and stays live for a long time, overwrite or `clear` skipped prefix slots, or use `container/deque`, SLC-11.)

**SLC-8.** *Slicing arrays and single-owner slices.*
- Slicing an addressable value array `arr[i:j]` produces a `[]T` with `backing == nil` and `cap == j - i`, tracked by the borrow checker so it may be passed to non-keeping parameters (`sort.Ints(arr[:])`) with zero allocation and cannot escape or outlive `arr` ([OWN-S2](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032), [OWN-B2, OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)). Appending to it allocates a fresh heap backing buffer (`cap == len`).
- Slicing a slice whose element type `T` is single-owner (`OWN-C3`, e.g. `[]Unique[Node]`, `[]File`) produces a borrowed sub-slice (`backing == nil`, `cap == len`) tracked against the owning slice ([OWN-S2](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032)).

**SLC-9.** *Shrinking and `:0` buffer reset (`s = s[:k]`).* When `s` is uniquely referenced (`refcount == 1`) and shrunk in place (`s = s[:k]` for `0 <= k < len(s)`, including `buf = buf[:0]`), Go++ drops all initialized non-trivial elements in `k..len(s)` in place and keeps `cap(s)` for reuse. When `s` is shared (`refcount > 1`), `s = s[:0]` detaches in `s`'s own allocator (`SLC-3`) without dropping the shared backing, so other slices keep their elements intact.

**SLC-10.** *Append-style functions* use ordinary Go signatures (`func AppendInt(dst []byte, i int64, base int) []byte`). In `buf = strconv.AppendInt(buf, 42, 10)`, `buf`'s last read before reassignment transfers at 0 retain/release, so `AppendInt` sees `refcount == 1` and appends in place without copying.

**SLC-11.** For ring-buffer FIFO queues that recycle prefix capacity without reallocating, std provides `container/deque`.

**SLC-12.** *Three-index slicing and `cap`.* Three-index slicing `s[i:j:k]` (`0 <= i <= j <= len(s)` and `j <= k <= cap(s)`) sets `len = j - i` and `cap = k - i`, so an `append` to a capped sub-slice allocates a fresh backing buffer instead of overwriting elements at `k..`. Unlike Go, `j` may not pass `len(s)` (SLC-13). On an array slice `arr[i:j]` (`backing == nil`), `cap` is `j - i` (`len`) unless capped smaller with `arr[i:j:j]`.

**SLC-13.** *No slicing past the length.* The high index of a slice expression on a slice may not pass its length: `s[:n]` and `s[i:n:k]` with `n > len(s)` are **out of range**, even when `n <= cap(s)`, and for every element type. Only `len(s)` elements exist: slots past the length may have been released by a shrink or never initialized, so a slice grows with `append`, not with `s[:n]`. This differs from Go, where `s[:cap(s)]` is legal.
- It is a **compile error** when the length and the index are both written in the program: the operand is an array, or a local slice whose value on every path comes from `make` with a constant length or from a composite literal and has not been assigned or appended to since, and the index is a constant (`buf := make([]byte, 3, 10); full := buf[:10]`).
- Otherwise it is a run-time **panic** ("slice bounds out of range"), as for any index that is only known when the program runs.

```go
s := []string{"a", "b", "c"}
s = s[:0]            // drops "a", "b", "c" in place, keeps cap == 3
s = s[:2]            // panic: slice bounds out of range
```

```go
buf := make([]byte, 0, 64)
buf = strconv.AppendInt(buf, 42, 10)    // transfers buf in and out: 0 retain/release, 0 copy
head := buf[:2]                         // O(1) shared []byte (retains backing)
buf = buf[:0]                           // buf detaches because head still retains the old backing;
fmt.Println(string(head))               // safe: prints "42"!
```

Compared with Go: slicing `s[i:j]` and `s = s[1:]` are $O(1)$ and share the backing array just like Go, with deterministic freeing when the last slice handle dies; extending length past `len(s)` via `s[:n]` panics instead of exposing dropped/zeroed slots (SLC-13).

## 8.3 Range loops (D1.21)

Base: [For statements with range clause](https://go.dev/ref/spec#For_range).

```
RangeClause = [ ExpressionList "=" | RangeVars ":=" ] "range" Expression .
RangeVars   = RangeVar [ "," RangeVar ] .
RangeVar    = [ "&" ] identifier .
```

`&` marks an *alias* variable ([RNG-1a](#83-range-loops-d121)). It is allowed only on the **value** variable: the second variable for slices, arrays, maps and C++ containers (`for _, &d := range xs`, `for k, &v := range m`), and the first variable when the iterator yields one pointer value (`for &d := range seq`, [RNG-6a](#83-range-loops-d121)). It is never allowed on the key or index. It is only part of `:=`, not of the `=` form.

**RNG-1.** `for i, x := range xs` gives `x` the element in the cheapest safe way:
- **trivial (`OWN-C1`) elements** are copied into `x`, as in Go ([PAR-4](05-parameters.md)). Changing a primitive `x` does not change the slice; for a trivial struct element (`Point`), mutating `x.X = 1` on a plain range copy is rejected so Go's lost-copy bug is caught — use `&x` to change the real item;
- **shared-handle (`OWN-C2`) elements** (`[]string`, `[]Doc`, `[]func()`, `[]error`, `[]chan T`, `[]Shared[T]`, `[][]T`, `[]map[K]V`) lend `x` at 0 retain/release (retaining handles in $O(1)$ if `x` or a field of `x` is stored, returned, sent, or captured by an escaping closure, [OWN-R3b](04-ownership.md#42-ownership-rules-d14)). So `names = append(names, x)` and `out = append(out, d)` work directly in plain `for i, d := range docs` with zero byte copy, while top-level field writes `d.Title = "x"` and `&d` are compile errors whose fix-it suggests `for _, &d := range docs` ([RNG-1a](#83-range-loops-d121));
- **single-owner (`OWN-C3`) elements** (`[]File`, `[]Unique[Node]`) bind `x` as a **read-only borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)), like Rust's `for f in &files`. Mutating or moving `x` is a compile error whose fix-it suggests `for _, &f := range files` ([RNG-1a](#83-range-loops-d121)) or `f.Clone()` (when cloneable);
- **ranging over a deep read-only binding** (such as a slice or map inside an interface type switch, [TYP-9](03-types.md)): `x` is also deep read-only all the way down (except plain pointer-free `Copy` values and immutable `string`s), and `&x` is a compile error;
- **the assignment form `for _, v = range xs`** works for `OWN-C1` and `OWN-C2` elements (`var s string; for _, s = range names {}` retains/assigns each string), and is a compile error (`E0253`) when keys or elements are single-owner `OWN-C3`, because each turn would implicitly move out of `xs[i]` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)).

**RNG-1a.** *The `&` form: the element itself.* In `for _, &d := range xs`, `d` is **the element itself**, not a copy and not a read-only borrow. It works for every element category.
- Writes change the collection: `d.Title = "x"`, `d.Count++`, `d = Doc{}` (the old value is dropped), and pointer-receiver calls such as `d.Upper()`.
- **`d` is a writable borrow of the element** ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). While `d` is live (used later in the body), a write through the collection to the same element is a compile error: `for i, &e := range xs { xs[i].f = 1; use(e) }`. Write through `e` only, or use the plain form `for i := range xs { xs[i].f = 1 }`.
- The `&` form covers slices, arrays, map values (`for k, &v := range m { v.Count++ }`), imported C++ containers ([RNG-6](#83-range-loops-d121)) and iterators that yield `*T` ([RNG-6a](#83-range-loops-d121)). The key can't use `&`. Strings, integers and channels have no element to alias, so `&` there is a compile error. So is `for _, &e := range v` where `v` is a deep read-only binding ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)).
- **Arrays follow [OWN-R3b](04-ownership.md#42-ownership-rules-d14).** `for _, &d := range arr` is allowed when the array is an addressable writable variable, or is reached through a writable pointer or slice. It is a compile error when the array is held **by value** inside a read-only binding (a plain range variable or an interface assertion binding).
- **Map aliases.** A `defer` (or any later use after the loop) that keeps the `&v` alias of `for k, &v := range m` is a compile error, because later inserts can rehash the map and move the value. Except for map aliases, a `defer` may keep an `&` alias (see below). Fix-it: call it inside the loop (`v.Flush()`), or keep a copy (`c := v.Clone(); defer c.Flush()`).
- **`&` and `defer`/`go`.** An `&` range variable is an alias of an element, so for [PAN-3](09-errors-and-panics.md) and [CLO-3b](11-closures.md) it counts as a **place** (`xs[i]`), not as an owned loop-body local. `defer f.Close()` evaluates `&xs[i]`: fine if the collection lives until the function returns, a compile error if the collection is a local of the loop body. This holds except for map aliases (above). For a slice, `defer` keeping `&xs[i]` stays legal while `xs` lives until return; a later `append` or assignment to `xs` before return that the compiler can see is a compile error ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021)); one through another pointer is a compile error too ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)).
- Extracting a single-owner element uses `mem.Take(&d)` (leaving the zero value in `xs[i]` or `m[k]`, when the type has a zero value) or `mem.Replace(&d, replacement)` ([OWN-R4](04-ownership.md#42-ownership-rules-d14)).
- It never copies: for an array the `&` form ranges over the live array, not over a copy ([RNG-6](#83-range-loops-d121)).
- Like any raw borrow, `&d` cannot be captured by an escaping closure or handed to `go` ([CLO-3a](11-closures.md), [CON-1](12-concurrency.md)).
- Growing the collection inside the loop stays a compile error ([RNG-3](#83-range-loops-d121)).
- `m[k].f = x` outside a loop stays illegal, as in Go. Inside the loop, write `for k, &v := range m { v.f = x }`.

```go
for _, &d := range docs { d.Title = "x" }             // changes docs[i].Title
for k, &v := range m { v.Count++ }                    // changes the map value in place
for _, &x := range nums { x *= 2 }                    // trivial elements: changes the real items
for _, &f := range files { keep(mem.Take(&f)) }       // extracts File, leaves zero value in files[i]
for _, d := range docs { d.Title = "x" }              // error: d is read-only. Fix-it: for _, &d := range docs
for k, &v := range m { m[k+"!"] = v.Clone() }         // error: inserts into the ranged map (RNG-3)

type Grid struct{ Cells [4]Doc }
func local()             { var a [4]Doc; for _, &d := range a { d.N++ } }      // ok: local array
func viaPtr(g *Grid)     { for _, &d := range g.Cells { d.N++ } }              // ok: reached through a pointer
func viaSlice(gs []Grid) { for _, &d := range gs[0].Cells { d.N++ } }          // ok: reached through a slice
```

*Why:* Go's `for _, d := range docs { d.Title = "x" }` silently changes a copy. A writable borrow would be a surprise in the other direction. The `&` makes "I want the real element" visible at the loop, and it reads like C++ `auto&`.

**RNG-2.** *Extracting elements in a range loop.* For `OWN-C1` and `OWN-C2` elements (`[]string`, `[][]byte`, `[]Shared[T]`), plain `for i, x := range xs` already lets `x` be stored, returned or sent without copying bytes (retaining `x`'s handle, and releasing `xs` when `xs` dies). To extract single-owner (`OWN-C3`) elements out of a slice one by one, use `for _, &x := range xs { keep(mem.Take(&x)) }` (or `mem.Replace(&x, v)` when the element type has no zero value).

**RNG-3.** Inside the loop body, a statement that can reallocate or replace the ranged slice, map or string variable is a **compile error** when the compiler can see it. This holds for the plain form and the `&` form:
- appending to the ranged slice variable, or growing the ranged string variable with `+=` ([STM-8](07-statements.md) may mutate in place when `refcount == 1`);
- reassigning the ranged variable (`xs = …`, `xs = xs[i:j]`);
- inserting into the map: inside `range m`, any `m[x] = …`, `m[x]++`, `m[x] op= …` or `m[x] = append(m[x], …)` where `x` is not the current range key variable (or is `k` but `k` was written earlier in the body, e.g. `k++` on a trivial key) counts as a visible potential insert (the compiler cannot know whether `x` exists). Only `m[k]` with `k` the range key variable, not written earlier in the body, is a write to the current entry. `mm[x][y] = …` also counts as an insert into `mm` when `x` may be absent, because the write inserts `x` with a new inner map ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). Fix-it: collect the changes and apply them after the loop;
- calling `clear` on it.

For a string: `for _, r := range s { s += "x" }` is a compile error. Fix-its: build into a new variable, or copy the header first (`orig := s; for _, r := range orig { s += "x" }`).

`delete(m, k)` on the ranged map is allowed. Removing the entry the loop is **currently on** takes it out of the map at once, while freeing the map-owned key/value slot at the end of that iteration so borrowed range bindings `k` and `v` stay valid through the end of the turn; removing any **other** entry frees it at once.

Writing the element or entry the loop is on (`xs[i] = y`, `m[k] = y`) is **defined**: the old value is dropped. For a plain borrowed range variable `v` (`OWN-C3` or struct element, [OWN-R3b](04-ownership.md#42-ownership-rules-d14)), reading `v` **after** overwriting `xs[i]` or `m[k]` in the same turn is a compile error ([OWN-B2](04-ownership.md#45-borrow-checking-adr-0021), [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021), [UB-2a](16-undefined-behavior.md)). Range variables of `OWN-C1` or `OWN-C2` (`string`, `Shared[T]`) elements are independent copies/handles and are not invalidated; an `&` range alias sees the new slot value directly, while writing the same element through both `xs[i]` and a live `&e` is a compile error ([RNG-1a](#83-range-loops-d121), [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). Map keys can never be aliased with `&` or mutated in place.

**RNG-3a.** *Evaluation of the range expression.* As in Go, the range expression is evaluated **once**, at loop start. For a heap-backed slice or string (`backing != nil`), the loop holds a temporary `+1` reference on `backing` for the duration of the loop ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021) rule 5), so even if another `Shared[T]` or `*T` alias reassigns or `append`-detaches its header (`refcount >= 2`), the buffer walked by `range` stays alive and unmodified. Visible mutations to the ranged variable itself remain compile errors ([RNG-3](#83-range-loops-d121)), and calls whose receiver or argument overlaps a local/unique ranged place are compile errors ([OWN-B4](04-ownership.md#45-borrow-checking-adr-0021), [UB-2e](16-undefined-behavior.md)); only `unsafe` and C code mutating raw memory stay UB (`-asan` catches those).

**RNG-4.** Maps count their active iterators. In the cases the compiler cannot see, inserting a **new key** into or calling **`clear`** on a map while any `range` over that map is running **always panics**, with `"map insert during iteration"` or `"map clear during iteration"`. It does not depend on the table's size or capacity, so it behaves the same on every run. Writing a key that already exists is not an insert and is allowed. A write `mm[k1][k2] = v` with an absent `k1` inserts `k1`, so it panics too ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). The visible inserts and `clear` calls are compile errors ([RNG-3](#83-range-loops-d121)).

```go
for k := range m { addKey(m) }     // addKey inserts a new key: panics every time
```

**RNG-5.** Go allows inserting into a map during `range`, so this is a listed difference from Go (P04 D4.10).

**RNG-6.** *Other range sources.*
- Strings (runes), integers and channels work as in Go. Ranging over a channel receives, and so moves each value out (`v` is an owned local per iteration, [CON-4](12-concurrency.md)).
- **Arrays.** Go ranges over a *copy* of an array. Go++ keeps that for arrays with **trivial** elements in the plain form (copied up front, exactly like Go). Arrays with **owning** elements are *not* copied, because that would be a hidden allocation (principle 1): the range variable borrows the live element, so the array follows **exactly** the slice rules of RNG-3 and [UB-2a](16-undefined-behavior.md): writing an element is defined, using a borrowed range variable after its own element was written is a compile error ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)), and writes to other elements are fine. The `&` form never copies the array. This is a listed difference from Go.
- **Imported C++ containers** (anything with `begin()`/`end()`, P08 D8.6) work like a slice for the two-variable and `&` forms. `for _, x := range vec` is a read-only borrow of each element: no C++ copy constructor runs (trivial element types are copied, as in RNG-1). `for _, &x := range vec { x += 1 }` changes the real element, like C++ `auto&`. The one-variable form `for x := range c` gives the index for indexable containers and the key for associative ones (`std::map`); for containers with neither (`std::list`) it is a compile error, so use `for _, x := range c`. For associative containers the two-variable form gives the key and the mapped value, like a Go map (`for k, v := range cppmap`; `&v` aliases the mapped value).

**RNG-6a.** *Range over a function.* Range-over-func (Go 1.23) follows the slice-loop rules.
- `for x := range seq` follows `RNG-1`: `OWN-C1` copies, `OWN-C2` lends as a read-only local view (retaining handles in $O(1)$ if kept), and `OWN-C3` borrows read-only (write `x.Clone()` to keep a cloneable `OWN-C3`).
- `for &x := range seq` changes the real item. It is allowed only when the iterator yields the real items as pointers (`iter.Seq[*Doc]`); otherwise it is a compile error. `x` is then the `Doc` the pointer points to. For `iter.Seq2`, `&` goes on the second variable, under the same condition.
- The loop body is a non-escaping closure ([CLO-1](11-closures.md)), **because** Go's run-time check is kept: calling `yield` after the loop has exited panics ("range function continued iteration after whole loop exit"), as in Go. This is an explicit exception to [CLO-4](11-closures.md)'s "a call through a func value assumes the parameter is kept": the iterator function receives `yield` as a func value that the loop invalidates when it ends.

```go
func (l *DocList) All() iter.Seq[*Doc] { … }

for &d := range docs.All() { d.Title = "x" }     // ok: the iterator yields *Doc
for d := range docs.All() { use(d) }             // d is a *Doc (a writable copy of the pointer); (*d).Clone() copies the Doc

func (l *DocList) Values() iter.Seq[Doc] { … }
for d := range docs.Values() { use(d) }          // d is a read-only Doc view (OWN-C2); out = append(out, d) retains handles
```

**RNG-6b.** *Range over a user type (`@Operator Iter`)* ([ADR-0024](../adr/0024-operator-overloading.md)). A type with an `@Operator` `Iter` method (`@Operator func (t *Tree) Iter() iter.Seq[int]`) can be ranged over: `for v := range t { … }` means `for v := range t.Iter() { … }`. All of [RNG-6a](#83-range-loops-d121) applies to the returned function: how `x` is borrowed, `&x` only when the iterator yields pointers, the loop body as a non-escaping closure, and the panic when `yield` is called after the loop.
- The result type must be a function type that range-over-func accepts (`iter.Seq[V]`, `iter.Seq2[K, V]`, or `func(yield func(…) bool)`), else the declaration is an error.
- The receiver may be a value or a pointer. A pointer receiver needs an addressable, writable operand (a variable, field, element or `*p`), not a temporary.
- The receiver stays borrowed for the whole loop ([OWN-B1 to OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)), so a body that writes to it, or calls a method that does, is a compile error, the same as for a slice ([RNG-3](#83-range-loops-d121)).
- `@Operator` `Iter` on a type that already ranges (slice, array, string, map, channel, integer, func) is an error at the declaration ([EXP-17](06-expressions.md#610-operator-methods-adr-0024)). Imported C++ containers keep [RNG-6](#83-range-loops-d121).
- `t.Iter()` is also callable directly as an ordinary method.

```go
@Operator
func (t *Tree) Iter() iter.Seq[int] { return func(yield func(int) bool) { t.walk(yield) } }

for v := range t { use(v) }          // ok: v is an int
for v := range t { t.insert(v) }     // error: t is borrowed by the loop (RNG-3, OWN-B2)
type Stack []int
@Operator
func (s Stack) Iter() iter.Seq[int] { … }   // error: Stack already ranges (slice)
```

**RNG-7.** *Temporaries in the header.* The range expression is evaluated once. If it creates temporaries, they live until the end of the whole `for` statement ([OWN-R2](04-ownership.md#42-ownership-rules-d14), [STM-4](07-statements.md)).

**RNG-8.** *No redeclaring a loop variable.* Declaring a variable with the same name as a loop variable inside that loop's body, in the body block or in any block nested in it, is a **compile error**. This covers `u := u`, `u := u.Clone()` and `var u = u`, for the variables of a range clause and of a `for` clause. The parameters of a function literal inside the loop body count too, so `go func(u User){…}(u.Clone())` is an error: rename the parameter. Old Go code that wrote `u := u` (needed before Go 1.22) must rename the copy. Fix-its never reuse the name: `mine := u.Clone()`, or `go fetch(u.Clone())` ([CLO-3a](11-closures.md)).

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
for i, n := range names {
    // names = append(names, n)              // compile error: reallocates the ranged slice (RNG-3)
    jobs <- Job{ID: i, Text: n}              // ok: n retains the shared immutable string buffer!
}

m := map[string]int{"a": 1, "b": 2}
for k := range m { delete(m, k) }            // allowed
```

## 8.4 Rationale

- An `@Operator Iter` method (RNG-6b) reuses range-over-func, so user containers need no new loop protocol and get the same borrow rules as slices.
- Shared refcounted slice and string backing buffers ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)) keep Go's $O(1)$ slicing (`s[i:j]` and `s = s[1:]`) while freeing backing storage deterministically when the last handle dies, and preserving arena/custom allocators across sub-slices and calls.
- Allocators in the backing header keep one `[]T` type for heap and arena data. The alternative (a separate `ArenaSlice[T]`) would split every generic API in two.
- Plain `range` lends shared handles (`string`) at zero byte copy and binds struct / single-owner elements read-only so Go's accidental lost-copy `d.Title = "x"` bug is caught at compile time; changing the real element uses `&` (`for _, &d := range docs`).
- The compile errors in RNG-3 catch the common bug (appending while ranging) early. The run-time iterator count covers map inserts the compiler cannot see, and it panics every time, so the bug does not depend on table size.
