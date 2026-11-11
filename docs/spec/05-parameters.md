# 5. Parameters and receivers

Base: Go spec [Function types](https://go.dev/ref/spec#Function_types), [Function declarations](https://go.dev/ref/spec#Function_declarations), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Calls](https://go.dev/ref/spec#Calls), [Passing arguments to ... parameters](https://go.dev/ref/spec#Passing_arguments_to_..._parameters), [Address operators](https://go.dev/ref/spec#Address_operators).

Delta D1.5 ([ADR-0041](../adr/0041-values-are-copied.md)): **100% Go parameter syntax — a by-value parameter is the function's own copy, a copy that cannot be noticed is skipped, `*T` changes the caller's value, and a value that cannot be copied is given with `move`.**

## 5.1 Syntax

No grammar change from Go. Parameter lists, receiver declarations, variadic parameters (`...T`) and function types are written **identically to Go**:

```go
func show(u User)
func rename(u *User)
func (l *List[T]) Push(v T)
func Join(parts ...string) string
```

## 5.2 Rules

**PAR-1.** *A by-value parameter is the function's own copy* ([ADR-0041](../adr/0041-values-are-copied.md) decision 4). A parameter or receiver of a non-pointer type holds its own value, for every type (`string`, `[]T`, `map[K]V`, `Shared[T]`, `Weak[T]`, `chan`, `interface`, `func`, structs, arrays, `Option`): the argument is copied into it by the rule of its type ([ADR-0041](../adr/0041-values-are-copied.md) rule 2).
- **Changes stay in the function:** nothing the function does to the parameter changes the caller's variable. `xs[0] = 1`, `m["a"] = 2`, `xs = append(xs, 1)`, `s = s[1:]` and `u.Title = "tmp"` change the function's copy only.
- **The copy is skipped when the program cannot tell** ([ADR-0041](../adr/0041-values-are-copied.md) rule 4). When the argument is a temporary or a local that is not read again (its last use), the value is handed over. When the function only reads the parameter and the argument cannot change during the call, the argument is passed as it is. In both cases no buffer is copied, and for a counted type (`string`, `Shared[T]`, `Weak[T]`, `chan`, interface and function values) nothing is retained or released at the call. A program must not depend on how many copies are made.
- **An argument that can change during the call:** if the argument is read from a place that the call could write (a slice element, map entry, `Shared[T]` field, package global, captured variable, pointer target, or a local whose `&` is also passed to the same call, as in `u.SetName(u.Name)` or `s.fail(s.err)`), the copy is made unless the compiler can show that the call does not write that place. The function never sees the change: in `u.SetName(u.Name)` the parameter keeps the old name. For a `string` this copy only retains the buffer.
- **Storing or returning the parameter:** a parameter that the function stores into a struct field, slice, map, global, channel or result is copied or handed over like any other value: handed over on its last use, copied otherwise. A view (`&arr[i:j]`, `&s[i:j]`) is a pointer, not a slice value: it is passed to a `*[]T` parameter and is checked like every borrow ([SLC-8](08-slices-and-range.md), [OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)).
- **Changing the caller's slice or map takes a pointer:** `*[]T` and `*map[K]V` are ordinary pointer types, and the caller writes `&` (`addOne(&nums)`). Through such a pointer, indexing (`xs[0]`, `m[k]`), slicing, `range`, `len`, `cap`, `delete` and `clear` are written without `*`; only the whole value is written with the star (`*xs = append(*xs, 4)`, `*xs = nil`) ([ADR-0041](../adr/0041-values-are-copied.md) rule 7, `PAR-6`).
- **A lost write is an error** ([ADR-0041](../adr/0041-values-are-copied.md) rule 9). Writing to an element or entry of a by-value slice or map parameter (an assignment, `delete`, `clear`, or an `append` assigned back to the parameter) when the parameter is not read, returned or passed on afterwards is a compile error, because the write is lost. The fix-its are a pointer parameter and returning the value.

```go
func fill(xs *[]int, m *map[string]int, d *Doc, doc2 Doc) {
    xs[0] = 1            // changes the caller's slice: indexing needs no star
    m["a"] = 2           // changes the caller's map
    d.Title = "x"        // changes the caller's Doc through *Doc
    doc2.Title = "local" // local to fill (does not change caller's Doc)
}

func sum(xs []int) int {                              // only reads xs: sum(nums) copies nothing
    n := 0
    for _, x := range xs { n += x }
    return n
}

func zero(xs []int)         { xs[0] = 0 }             // compile error: the write is lost
func zeroed(xs []int) []int { xs[0] = 0; return xs }  // ok: changes its copy and returns it

fill(&nums, &counts, &doc, other)                     // the call shows what fill can change
nums = zeroed(nums)                                   // nums is not read again before it is assigned: no copy
```

**PAR-2.** *Re-slicing parameters.* Re-slicing a `string` or `[]T` parameter (`s = s[1:]`) changes the function's own value in $O(1)$ with no allocation ([SLC-6, SLC-7](08-slices-and-range.md), [OWN-S1, OWN-S2](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032)); the caller's string or slice is unchanged.

**PAR-3.** *Taking `&param`.* Taking `&param` of a parameter borrows the callee's local parameter slot for the duration of the callee; like any `&local`, it may not be returned or escape the callee ([OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)).

**PAR-4.** *Trivial parameters copy (`OWN-C1`).* Parameters of trivial types (`int`, `bool`, floats, raw `*T`, trivial structs/arrays) are ordinary copies, as in Go.

```go
func gcd(a, b int) int {
    for b != 0 { a, b = b, a%b }
    return a
}
```

**PAR-5.** *Parameters of a type that cannot be copied (`OWN-C3`).* A by-value parameter or receiver of a type that cannot be copied (a type with a `drop` method and no `Clone`, a `@Pinned` type, a C++ class without a usable copy constructor, or a type that holds one; [ADR-0041](../adr/0041-values-are-copied.md) rule 3) **takes the value itself**:
- The argument is `move(x)` for a local or parameter `x`, or a temporary (a call result, a literal). A plain `f(x)` is a compile error whose fix-its are `move(x)` and `&x`. After `move(x)`, reading `x` before it is assigned again is `E0451`.
- The callee owns the parameter and destroys it on every exit path (including panics) unless it is moved onward (`move(p)` into a field, an element or another call) or returned.
- Exceptions (read-only use, no `move`): a hand-written `Clone() T` method (`func (h Handle) Clone() Handle`, [OWN-R6](04-ownership.md#42-ownership-rules-d14)), a pure `@Operator` method (`Add`..`Shr`, `Neg`, `Not`, `BitNot`, `Eq`, `Cmp`, or value-receiver `Index` / `Iter`, [EXP-16](06-expressions.md#610-operator-methods-adr-0024)), a `@Convert` method ([EXP-25](06-expressions.md#610-operator-methods-adr-0024)), and a `[]T` slice parameter that the function only reads (`func checkAll(files []File)`, [GEN-3](10-generics.md)) take their operands as they are, read-only, and do not consume them.
- To **use the caller's value** without taking it, declare the parameter or receiver as `*T` (`func (f *File) Read(p *[]byte) (int, error)`, `func check(n *Node) int`): `Unique[T]` places convert implicitly to `*T` ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17)), and addressable `File` variables take `&f` on method calls automatically, as in Go.
- A method with a value receiver of a type that cannot be copied cannot be called through an interface, because the call would take the value out of the box, which is shared and immutable ([TYP-5](03-types.md)); methods on `*T` or on value receivers that can be copied satisfy interfaces as in Go.
- `Unique[T]` can be copied when `T` can: the copy is a new allocation holding a copy of what it points to. Such an argument follows `PAR-1`, and on its last use it is handed over without copying.

```go
func keep(pool *Pool, f File) { pool.files = append(pool.files, move(f)) } // f is keep's own; it moves it on
func fd(f *File) int          { return f.fdPlusOne - 1 }                   // uses the caller's File

f := openLog()            // a File has drop and no Clone: it cannot be copied
fd(&f)                    // pointer: f is still ours
// keep(&pool, f)         // compile error: File cannot be copied. Fix-its: move(f), &f
keep(&pool, move(f))      // f is given to keep; reading f after this line is E0451
keep(&pool, openLog())    // a temporary needs no move
```

**PAR-6.** *`*T` mutates in place.* `*T` parameters and receivers mutate the target, as in Go. A call `v.M()` on an addressable variable `v` where `M` has a `*T` receiver passes `&v` automatically. Every `*T` parameter is a tracked borrow ([OWN-B1 to OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)): overlapping writable `*T` arguments in one call (`swap(&list[i], &list[j])`) are a compile error, and `return p` is tracked by `OWN-B3`.

**PAR-7.** *Variadic parameters (`...T`).* Variadic parameters work as in Go for every type: `func Join(parts ...string) string`, `func Println(a ...any)`, `func Collect[T any](xs ...T) []T`. Each argument goes into the variadic slice by the rules of `PAR-8`: it is copied, the copy is skipped for a temporary or a local on its last use, and a value that cannot be copied is a temporary or `move(x)`. The variadic slice is the function's own. Passing an existing slice with `f(slice...)` passes `slice` as a by-value `[]T` argument (`PAR-1`): no copy is made when the function only reads it or when this is the last use of `slice`; when `T` cannot be copied, `PAR-5` applies.

**PAR-8.** *Arguments at a call.*
- `OWN-C1` arguments copy bitwise.
- Every other argument that can be copied is copied into the parameter by the rule of its type: a `string`, `Shared[T]`, `Weak[T]`, `chan`, interface or function value copies the handle and retains it; a `[]T` or `map[K]V` gets a new buffer with a copy of every element or entry. The copy is skipped for a temporary, for a local on its last use, and when the function only reads the parameter and the argument cannot change during the call (`PAR-1`). The caller's locals remain valid after the call, except one given with `move(x)`.
- An argument that cannot be copied is given by value with `move(x)` or as a temporary (except the read-only forms in `PAR-5`); passed to `*T`, it is used in place through the pointer.
- Temporaries created in argument expressions live until the end of the full statement ([OWN-R2](04-ownership.md#42-ownership-rules-d14)).

**PAR-9.** *Results.* Function results are owned by the caller:
- `return x` gives the caller its own value. Returning a local (`return s`) hands it over: nothing is copied and no `move` is written. `return s[i:j]` on a local slice hands over that part without copying ([SLC-6](08-slices-and-range.md)). Returning from a place that lives on (`return u.Name`, `return c.items`) copies: a `string` shares its bytes and retains the buffer; a `[]T` or `map[K]V` gets a new buffer.
- Returning a local of a type that cannot be copied (`return file`) hands it to the caller; no `move` is needed ([ADR-0041](../adr/0041-values-are-copied.md) rule 3).
- Returning a raw `*T`, `Option[*T]`, or borrow-carrying struct/array borrows from the parameter(s) named in the function's pointer-borrow summary ([OWN-B3](04-ownership.md#45-borrow-checking-adr-0021)); returning `&local` or a view of a local (`&arr[i:j]`, `&s[i:j]`) is a compile error. `return arr[:]` without `&` returns a new slice with copies of the elements.

**PAR-10.** *ABI summary (informative here, normative in `docs/design/calling-convention.md`).*
- Scalars and small aggregates (up to 4 scalars, including 3-word `string` and 4-word `[]T`) pass directly in registers under `swiftcall`; larger aggregates pass by pointer to a caller/callee copy.
- When the copy of a by-value argument is skipped (`PAR-1`), the caller passes the value as it is: no buffer is copied and no count is retained or released in the caller or the prologue.
- A by-value parameter of a type that cannot be copied passes from caller to callee, which runs drop glue on exit if the value was not moved onward.

## 5.3 Examples

```go
// Generic read-only search: works for int, string, User. It only reads xs and v, so nothing is copied
func Index[T comparable](xs []T, v T) int {
    for i := range xs {
        if xs[i] == v { return i }
    }
    return -1
}

// Generic container insert: plain Go signature and call sites
func (l *List[T]) Push(v T) {
    l.items = append(l.items, move(v))   // T may be a type that cannot be copied (GEN-3)
}

names := []string{"a", "b"}
i := Index(names, "b")     // Index only reads names: no copy
var list List[string]
name := "alice"
list.Push(name)            // name is printed on the next line, so Push gets a copy (the same bytes, retained)
fmt.Println(i, name)
```

## 5.4 Rationale

- **Pure Go signatures:** Function declarations are written as in Go. A call that lets the function change the caller's slice or map shows it with `&`.
- **Fast by default:** Skipping the copy of a parameter that is only read keeps the ~80–90% of calls that only inspect their inputs free of copies and reference-count updates, and handing over a value on its last use does the same when a newly built value goes into a container.
- **Stable APIs:** Whether a function internally caches a `string` or `User` parameter is an implementation detail, never a compile-breaking signature change for callers.
