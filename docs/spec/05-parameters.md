# 5. Parameters and receivers

Base: Go spec [Function types](https://go.dev/ref/spec#Function_types), [Function declarations](https://go.dev/ref/spec#Function_declarations), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Calls](https://go.dev/ref/spec#Calls), [Passing arguments to ... parameters](https://go.dev/ref/spec#Passing_arguments_to_..._parameters), [Address operators](https://go.dev/ref/spec#Address_operators).

Delta D1.5: **owning types borrow, trivial types copy.**

## 5.1 Syntax

```
ParameterDecl = [ IdentifierList ] [ "own" ] [ "..." ] Type .
Receiver      = Parameters .              /* one ParameterDecl; may use "own" */
```

- `own` comes after the name and before the type: `child own Unique[Node]`, `func (b own Builder) Finish() string`.
- `own` may be used without a name in function types: `func(own string)`.
- `own` is allowed on parameters and receivers only. It is not allowed on results, fields or locals.

> **Open (P01 review):** D1.5 names parameters and receivers only. The ban on `own` for results, fields and locals, and the unnamed form `func(own string)`, are this spec's readings.

> **Open (P01 review):** `own ...T` (variadic) is allowed by the grammar above, but the plan says nothing about variadic parameters of owning element type (except `...any`, [TYP-7](03-types.md)). This spec reads: `own ...T` moves (or `clone`s) each argument into a fresh owned `[]T`. A plain `...T` with an owning `T` that is not an interface is a compile error in v0.

## 5.2 Rules

**PAR-1.** *Owning parameters borrow.* A parameter or receiver of an **owning** type borrows its argument. A borrowed parameter is a *borrowed variable*, so the read-only-at-the-top-level rule of [OWN-R3b](04-ownership.md#42-ownership-rules-d14) applies to it: no assigning the parameter or its fields, no `move(p)`, no `append`, no `&p`, no pointer-receiver call. Data reached **through** slices, maps and pointers stays mutable, as in Go: elements of `[]T`, map entries, pointer targets.

```go
func fill(xs []int, m map[string]int, d *Doc, doc2 Doc) {
    xs[0] = 1            // ok: element of a slice
    m["a"] = 2           // ok: map entry
    d.Title = "x"        // ok: through a *Doc
    // xs = append(xs, 1)  // error: cannot append to a borrowed parameter
    // doc2.Title = "y"    // error: cannot assign a field of a borrowed parameter
}
```

**PAR-2.** *Re-pointing views.* A borrowed `string` or `[]T` parameter is a local view, so it can be re-pointed (`p = p[n:]`). It still cannot be appended to. (The same exception applies to other borrowed `string`/`[]T` variables, see [OWN-R3b](04-ownership.md#42-ownership-rules-d14).)

```go
func skipSpaces(s string) int {
    n := len(s)
    for len(s) > 0 && s[0] == ' ' { s = s[1:] }   // re-pointing: legal, no allocation
    return n - len(s)
}
```

**PAR-3.** *No `&param`.* `&param` on a borrowed owning parameter, or on any part of it held by value (`&param.Buf`, `param.Buf.Reset()`), is a compile error (the rule of [OWN-R3b](04-ownership.md#42-ownership-rules-d14)). Fix-it: take a `*T` parameter.

**PAR-4.** *Trivial parameters are copies.* Parameters of trivial types (`int`, raw pointers, trivial structs) are ordinary copies, as in Go. The callee may change its copy. The compiler may skip the copy only when nothing can write through an alias.

```go
func gcd(a, b int) int {
    for b != 0 { a, b = b, a%b }    // unchanged Go code works
    return a
}
```

**PAR-5.** *`own` takes ownership.* An `own` parameter or receiver takes ownership.
- The caller's argument is moved, or the caller writes `clone(x)`.
- Inside the callee an `own` parameter is an ordinary owned local (mutable).
- The callee drops it on every path, including panics ([PAN-6](09-errors-and-panics.md)).
- A method with an `own` receiver is **not** in any method set used for interface satisfaction: the box of an interface value is shared and immutable ([TYP-5](03-types.md)), so the receiver cannot be moved out of it. It can only be called directly on an owned value (`b.Finish()`).

> **Open (P01 review):** the plan does not say how `own` receivers meet interfaces; this spec reads it from the shared, immutable box (D1.12). Methods with value or pointer receivers satisfy interfaces as in Go.

```go
func (l *List[T]) Push(v own T)          { l.items = append(l.items, v) }
func (b own Builder) Finish() string     { return move(b.buf) } // b is consumed; a field needs move (OWN-R3a)

l.Push(name)           // name moves; later use of name is an error
l.Push(clone(name))    // name is kept
```

**PAR-6.** *`*T` mutates.* `*T` parameters and receivers mutate the target, as in Go. A call `v.M()` on an addressable variable `v` where `M` has a `*T` receiver passes `&v` automatically, as in Go.

**PAR-7.** *Shared handles work like Go.* Parameters of shared-handle types (`Shared[T]`, `Weak[T]`, `chan`, interfaces including `error`, funcs) can be copied, stored and returned by the callee. Each copy retains, so `return err` works as in Go. The call itself passes them without a retain. A callee that releases the caller's last reference through another path and then uses the parameter reads freed memory: [UB-2b](16-undefined-behavior.md).

**PAR-8.** *Arguments at a call.*
- A local passed to a borrowed parameter is **not** moved.
- A local passed to an `own` parameter is moved ([OWN-R3](04-ownership.md#42-ownership-rules-d14)).
- A temporary passed to an `own` parameter moves into it. A temporary passed to a borrowed parameter lives until the end of the full statement.
- A view or a string literal can be passed to a `string` parameter, and a view to a `[]T` parameter, at zero cost (see ABI).

**PAR-9.** *Results are owned.* A function result of owning type is a new owner. `return a` of a borrowed parameter needs `clone(a)` or an `own` parameter ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). A result of type `view string` or `view []T` may point into a parameter (programmer's risk, [OWN-V5](04-ownership.md#44-views-d16)).

**PAR-10.** *ABI (informative here, normative in P04 D4.4).*
- Trivial parameters follow the C ABI (copies).
- Borrowed owning parameters pass by hidden pointer.
- `string` and `[]T` pass as `(ptr, len)`, so callers can pass owned values, views and literals at zero cost.
- An `own []T` parameter passes all 4 words ([SLC-4](08-slices-and-range.md)).
- Go++ to Go++ calls may use the internal convention.

## 5.3 Examples

```go
// Generic code works for owning T with no moves and no clones (borrowed params):
func Index[T comparable](xs []T, v T) int {
    for i := range xs {
        if xs[i] == v { return i }
    }
    return -1
}
names := []string{"a", "b"}
i := Index(names, "b")           // names and "b" are borrowed; names is still usable

// Returning a borrowed value needs a copy:
func Pick[T clonable](a, b T, first bool) T {
    if first { return clone(a) }
    return clone(b)
}

// A receiver that consumes:
b := NewBuilder()
s := b.Finish()                  // b moved into Finish
// b.Add("x")                    // error: b used after move
```

Compared with Go: call sites look exactly the same. Only declarations that keep or consume a value need `own` or `clone`.

## 5.4 Rationale

- This is the C++ `const T&`, Swift and Mojo convention. Generic code such as `Index` works for owning `T`.
- Trivial parameters stay real copies, so Go code like `for b != 0 { a, b = b, a%b }` works unchanged.
- **Rejected:** move-by-default parameters (Rust without `&`; hostile to Go-style generic APIs); copy-by-default owning parameters (hidden allocations); read-only trivial parameters (they reject ordinary Go code, and passing large ones by pointer gives wrong results when an argument aliases memory the callee writes).
