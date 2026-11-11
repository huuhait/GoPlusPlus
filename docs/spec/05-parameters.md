# 5. Parameters and receivers

Base: Go spec [Function types](https://go.dev/ref/spec#Function_types), [Function declarations](https://go.dev/ref/spec#Function_declarations), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Calls](https://go.dev/ref/spec#Calls), [Passing arguments to ... parameters](https://go.dev/ref/spec#Passing_arguments_to_..._parameters), [Address operators](https://go.dev/ref/spec#Address_operators).

Delta D1.5 ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)): **100% Go parameter syntax — trivial types copy, shared/value handles lend without retaining, `*T` mutates in place, single-owner values move.**

## 5.1 Syntax

No grammar change from Go. Parameter lists, receiver declarations, variadic parameters (`...T`) and function types are written **identically to Go**:

```go
func show(u User)
func rename(u *User)
func (l *List[T]) Push(v T)
func Join(parts ...string) string
```

## 5.2 Rules

**PAR-1.** *Shared/value handle parameters lend (`OWN-C2`).* A parameter or receiver of an `OWN-C2` type (`string`, `[]T`, `map[K]V`, `Shared[T]`, `Weak[T]`, `chan`, `interface`, `func`, or an array/struct/`Option` of them) **lends** the argument at the call boundary:
- **Zero-cost call boundary:** When passed from an unaliased local variable or temporary, the caller passes the handle with **zero retain/release operations**.
- **Storing or returning inside the callee:** If the callee stores an `OWN-C2` parameter into a struct field, slice, map, global, channel or result, the callee retains a reference at that store (or elides the retain+release pair when the caller passed a temporary or last-use local and the call is inlined/optimized; boxing a mutable container into a kept interface follows [TYP-7](03-types.md#32-interfaces-and-runtime-type-information-d112)). Stack-borrowed array slices (`arr[:]`) and borrowed `OWN-C3` sub-slices may be passed only to non-keeping slice parameters ([OWN-S2](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032)); passing them to a keeping parameter or an indirect `func([]T)` is a compile error (`use arr[:].Clone()`).
- **Aliasing safety:** If an `OWN-C2` argument is read from a place that could be overwritten during the call (a slice element, map entry, `Shared[T]` field, package global, captured closure cell, dereferenced pointer, or a local whose `&` is also passed to the same call, as in `u.SetName(u.Name)` or `s.fail(s.err)`), the caller holds a temporary retain across the call so the callee never reads freed memory.
- **Go-style shared headers (`[]T` and `map[K]V`):** As in Go, passing `xs []int` or `m map[string]int` passes the handle by value and shares the underlying elements or map entries, so `xs[0] = 1` and `m["a"] = 2` (when `m != nil`) mutate the caller's elements/entries directly, while `xs = append(xs, 1)`, `xs = xs[1:]`, or writing into a `nil` map parameter updates only the callee's local parameter unless returned (`*[]T` and `*map[K]V` do not exist; passing a known-`nil` map to a void/non-returning function emits a warning, [OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)).
- **Local mutability of value parameters:** Reassigning a parameter variable (`s = s[1:]`, `u.Title = "tmp"` on a local copy) affects only the callee's local binding, as in Go; to mutate the caller's struct or slice header in place, take `*T` (`PAR-6`).

```go
func fill(xs []int, m map[string]int, d *Doc, doc2 Doc) {
    xs[0] = 1            // ok: writes shared slice element, as in Go
    m["a"] = 2           // ok: writes shared map entry, as in Go
    d.Title = "x"        // ok: mutates caller's Doc through *Doc
    doc2.Title = "local" // local to fill (does not change caller's Doc)
}
```

**PAR-2.** *Re-slicing parameters.* Re-slicing a `string` or `[]T` parameter (`s = s[1:]`) updates the callee's local header in $O(1)$ with no allocation ([OWN-S1, OWN-S2](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032)).

**PAR-3.** *Taking `&param`.* Taking `&param` of a parameter borrows the callee's local parameter slot for the duration of the callee; like any `&local`, it may not be returned or escape the callee ([OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)).

**PAR-4.** *Trivial parameters copy (`OWN-C1`).* Parameters of trivial types (`int`, `bool`, floats, raw `*T`, trivial structs/arrays) are ordinary copies, as in Go.

```go
func gcd(a, b int) int {
    for b != 0 { a, b = b, a%b }
    return a
}
```

**PAR-5.** *Single-owner parameters move (`OWN-C3`).* A by-value parameter or receiver of a single-owner type (`Unique[T]`, a type with `drop()`, or a struct/collection containing one) **takes ownership**:
- Passing a local or temporary moves it into the callee (`E0451` if the caller reads that local again before reassigning it).
- The callee owns the parameter and destroys it on every exit path (including panics) unless moved onward (`return`, stored in a field, etc.).
- Exceptions (read-only lending): a hand-written `Clone() T` method (`func (h Handle) Clone() Handle`, [OWN-R6](04-ownership.md#42-ownership-rules-d14)), a pure `@Operator` method (`Add`..`Shr`, `Neg`, `Not`, `BitNot`, `Eq`, `Cmp`, or value-receiver `Index` / `Iter`, [EXP-16](06-expressions.md#610-operator-methods-adr-0024)), a `@Convert` method ([EXP-25](06-expressions.md#610-operator-methods-adr-0024)), and a non-keeping `[]T` slice parameter (`func checkAll(files []File)`, [GEN-3](10-generics.md)) borrow `OWN-C3` operands read-only without consuming them.
- To **borrow** a single-owner value without consuming it, declare the parameter or receiver as `*T` (`func (f *File) Read(p []byte) (int, error)`, `func check(n *Node) int`): `Unique[T]` places convert implicitly to `*T` ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17)), and addressable `File` variables take `&f` on method calls automatically, as in Go.
- A method with a consuming `OWN-C3` value receiver cannot be called through an interface (whose box is shared and immutable, [TYP-5](03-types.md)); methods on `*T` or on `OWN-C1`/`OWN-C2` value receivers satisfy interfaces as in Go.

```go
func attach(parent *Node, child Unique[Node]) { parent.Left = child } // child moves in
func check(n *Node) int                       { return n.Val }        // borrows Unique[Node] as *Node

sub := NewUnique(Node{Val: 7})
check(sub)            // borrowed as *Node: sub is still ours
attach(&root, sub)    // moved into attach: sub is now consumed
```

**PAR-6.** *`*T` mutates in place.* `*T` parameters and receivers mutate the target, as in Go. A call `v.M()` on an addressable variable `v` where `M` has a `*T` receiver passes `&v` automatically. Every `*T` parameter is a tracked borrow ([OWN-B1 to OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)): overlapping writable `*T` arguments in one call (`swap(&list[i], &list[j])`) are a compile error, and `return p` is tracked by `OWN-B3`.

**PAR-7.** *Variadic parameters (`...T`).* Variadic parameters work as in Go for every type category: `func Join(parts ...string) string`, `func Println(a ...any)`, `func Collect[T any](xs ...T) []T`. Each argument is copied (`OWN-C1`), retained/auto-moved (`OWN-C2`), or moved (`OWN-C3`) into the variadic slice. Passing an existing slice with `f(slice...)` shares the slice directly when `T` is `OWN-C1`/`OWN-C2` (and moves `slice` when `T` is `OWN-C3`).

**PAR-8.** *Arguments at a call.*
- `OWN-C1` arguments copy bitwise.
- `OWN-C2` arguments lend at zero cost (retaining only if stored by the callee, or temporarily across the call if read from an aliasable place, `PAR-1`); caller locals remain valid after the call.
- `OWN-C3` arguments passed by value move into the callee (except the read-only lending forms in `PAR-5`); passed to `*T`, they borrow.
- Temporaries created in argument expressions live until the end of the full statement ([OWN-R2](04-ownership.md#42-ownership-rules-d14)).

**PAR-9.** *Results.* Function results are owned by the caller:
- Returning an `OWN-C2` value (`return s`, `return s[i:j]`, `return u.Name`) transfers a strong reference to the caller (zero-cost move when returning a local, retain when returning a non-local place or shared parameter).
- Returning an `OWN-C3` local (`return uniqueNode`, `return file`) moves ownership to the caller.
- Returning a raw `*T`, `Option[*T]`, or borrow-carrying struct/array borrows from the parameter(s) named in the function's pointer-borrow summary ([OWN-B3](04-ownership.md#45-borrow-checking-adr-0021)); returning `&local` or a stack-array slice `arr[:]` is a compile error.

**PAR-10.** *ABI summary (informative here, normative in `docs/design/calling-convention.md`).*
- Scalars and small aggregates (up to 4 scalars, including 3-word `string` and 4-word `[]T`) pass directly in registers under `swiftcall`; larger aggregates pass by pointer to a caller/callee copy.
- Lent `OWN-C2` parameters require no retain/release in the caller or prologue; only stores to escaping destinations emit a retain.
- `OWN-C3` value parameters transfer ownership from caller to callee, which runs drop glue on exit if not moved onward.

## 5.3 Examples

```go
// Generic read-only search: works for int, string, User — 0 copies, 0 retains
func Index[T comparable](xs []T, v T) int {
    for i := range xs {
        if xs[i] == v { return i }
    }
    return -1
}

// Generic container insert: plain Go signature and call sites
func (l *List[T]) Push(v T) {
    l.items = append(l.items, v)
}

names := []string{"a", "b"}
i := Index(names, "b")     // lent: 0 retains
var list List[string]
name := "alice"
list.Push(name)            // retained because `name` is also printed on the next line
fmt.Println(i, name)
```

## 5.4 Rationale

- **Pure Go signatures:** Function declarations and call sites look identical to Go.
- **Fast by default:** Lending `OWN-C2` parameters at the call boundary avoids atomic/non-atomic reference-count churn on the ~80–90% of calls that only inspect their inputs, while last-use move elision avoids churn when handing newly built values into containers.
- **Stable APIs:** Whether a function internally caches a `string` or `User` parameter is an implementation detail, never a compile-breaking signature change for callers.
