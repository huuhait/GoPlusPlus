# 5. Parameters and receivers

Base: Go spec [Function types](https://go.dev/ref/spec#Function_types), [Function declarations](https://go.dev/ref/spec#Function_declarations), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Calls](https://go.dev/ref/spec#Calls), [Passing arguments to ... parameters](https://go.dev/ref/spec#Passing_arguments_to_..._parameters), [Address operators](https://go.dev/ref/spec#Address_operators).

Delta D1.5: **owning types borrow, trivial types copy.**

## 5.1 Syntax

```
ParameterDecl = "move" identifier [ "..." ] Type
              | [ IdentifierList ] [ "..." ] Type .       /* Go form */
Receiver      = Parameters .              /* one ParameterDecl; may use "move" */
```

- `move` comes **before the parameter name**: `func (l *List) Push(move s string)`, `func (move b Builder) Finish() string`, `func Join(move parts ...string) string`. Each marked parameter has its own `move`, one name and an explicit type: `func f(move a T, move b T)`. There is no list form (`move a, b T`): Go reads it as the parameters `(move a)` and `(b T)`.
- `move` is a contextual word ([LEX-12](02-lexical.md#lex-12)): it is special only at the start of a parameter declaration followed by a name. `move` is also the built-in function `move(x)`.
- In function types the parameters may have no names. A marked parameter then uses the blank name: `func(move _ string)`. (`func(move string)` is a parameter *named* `move`, as in Go.)
- `move` is allowed on parameters, receivers and function types only. It is not allowed on results, fields or locals.
- A parameter without `move` still borrows ([PAR-1](#52-rules)), so calls look like Go: `list.Push(name)` moves `name` (it is an owned local passed to a `move` parameter), `list.Push(name.Clone())` keeps it.
- **Variadic parameters.** A variadic parameter of an owning type must be written `move ...T`: `func Join(move parts ...string) string`. A plain `...T` with an owning `T` that is not an interface is a compile error. In generic code, `func f[T any](xs ...T)` is a compile error at the declaration unless `T` is constrained to `Copy` (or the parameter is `move xs ...T`), the same as for an owning `T`. `move parts ...T` moves each argument into a fresh owned `[]T`. The caller writes `x.Clone()` to keep its value: `filepath.Join(dir.Clone(), "x")`. The interface case `...any` is in [TYP-7](03-types.md).

## 5.2 Rules

**PAR-1.** *Owning parameters borrow.* A parameter or receiver of an **owning** type borrows its argument. A borrowed parameter is a *borrowed variable*, so the read-only-at-the-top-level rule of [OWN-R3b](04-ownership.md#42-ownership-rules-d14) applies to it: no assigning the parameter or its fields, no `move(p)`, no `append`, no `&p`, no pointer-receiver call (a value-receiver call such as `p.Clone()` is fine). Data reached **through** slices, maps and pointers stays mutable, as in Go: elements of `[]T`, map entries, pointer targets. One more thing is allowed: the first write to a **nil** map creates it in the caller's variable, so the caller sees it. This holds for a map parameter and for a map field of a borrowed struct (`doc2.Index["x"] = 1`). Creating a nil map anywhere inside a borrowed variable is the one header change a borrowed variable may make ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)).

```go
func fill(xs []int, m map[string]int, d *Doc, doc2 Doc) {
    xs[0] = 1            // ok: element of a slice
    m["a"] = 2           // ok: map entry; if m is nil, this creates it in the caller's variable
    doc2.Index["x"] = 1 // ok: if doc2.Index is nil, this creates it in the caller's Doc
    d.Title = "x"        // ok: through a *Doc
    // xs = append(xs, 1)  // error: cannot append to a borrowed parameter
    // doc2.Title = "y"    // error: cannot assign a field of a borrowed parameter
}
```

**PAR-2.** *Re-pointing refs.* A borrowed `string` or `[]T` parameter is a local ref, so it can be re-pointed (`p = p[n:]`). It still cannot be appended to. (The same exception applies to other borrowed `string`/`[]T` variables, see [OWN-R3b](04-ownership.md#42-ownership-rules-d14).)

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

**PAR-5.** *`move` takes ownership.* A `move` parameter or receiver takes ownership.
- The caller's argument is moved, or the caller writes `x.Clone()`. An owned local passed to a `move` parameter moves automatically. A field, element or map value needs `move(x)` or `x.Clone()` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)).
- Inside the callee a `move` parameter is an ordinary owned local (mutable).
- The callee drops it on every path, including panics ([PAN-6](09-errors-and-panics.md)).
- A method with a `move` receiver is **not** in any method set used for interface satisfaction: the box of an interface value is shared and immutable ([TYP-5](03-types.md)), so the receiver cannot be moved out of it, and you cannot use up a value through a shared interface box. It can only be called directly on an owned value (`b.Finish()`). Methods with value or pointer receivers satisfy interfaces as in Go.
- `move` and `clone` are not function values: they can only be called ([EXP-1](06-expressions.md)).

```go
func (l *List[T]) Push(move v T)         { l.items = append(l.items, v) }
func (move b Builder) Finish() string    { return move(b.buf) } // b is consumed; a field needs move (OWN-R3a)
func Join(move parts ...string) string   { … }                  // owning variadic: `move` is required

l.Push(name)           // name moves; later use of name is an error
l.Push(name.Clone())    // name is kept
l.Push(move(u.Name))    // a field: needs move(x) or x.Clone()
var f Finisher = b      // error (with interface Finisher{ Finish() string }): Finish has a move receiver, so Builder does not satisfy Finisher
```

**PAR-6.** *`*T` mutates.* `*T` parameters and receivers mutate the target, as in Go. A call `v.M()` on an addressable variable `v` where `M` has a `*T` receiver passes `&v` automatically, as in Go.

**PAR-7.** *Shared handles work like Go.* Parameters of shared-handle types (`Shared[*T]`, `Weak[*T]`, `chan`, interfaces including `error`, funcs) can be copied, stored and returned by the callee. Each copy retains, so `return err` works as in Go. The call itself passes them without a retain. A callee that releases the caller's last reference through another path and then uses the parameter reads freed memory: [UB-2b](16-undefined-behavior.md).

**PAR-8.** *Arguments at a call.*
- A local passed to a borrowed parameter is **not** moved.
- A local passed to a `move` parameter is moved ([OWN-R3](04-ownership.md#42-ownership-rules-d14)).
- A temporary passed to a `move` parameter moves into it. A temporary passed to a borrowed parameter lives until the end of the full statement.
- A ref or a string literal can be passed to a `string` parameter, and a ref to a `[]T` parameter, at zero cost (see ABI).

**PAR-9.** *Results are owned.* A function result of owning type is a new owner. `return a` of a borrowed parameter needs `a.Clone()` or a `move` parameter ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). A result of type `ref string` or `ref []T` may point into a *borrowed* parameter (programmer's risk, [OWN-V5](04-ownership.md#44-refs-d16)). A ref into a local, or into a `move` parameter that dies at the return, is a compile error ([OWN-V7](04-ownership.md#44-refs-d16)).

**PAR-10.** *ABI (informative here, normative in P04 D4.4).*
- Trivial parameters follow the C ABI (copies).
- Borrowed owning parameters pass by hidden pointer. For a map, the pointer points at the owner's map header. This is one reason: the first write to a nil borrowed map creates the map in the caller's variable ([PAR-1](#52-rules)).
- `string` and `[]T` pass as `(ptr, len)`, so callers can pass owned values, refs and literals at zero cost.
- A `move` `[]T` parameter passes all 4 words ([SLC-4](08-slices-and-range.md)).
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
func Pick[T Clone](a, b T, first bool) T {
    if first { return a.Clone() }
    return b.Clone()
}

// A receiver that consumes:
b := NewBuilder()
s := b.Finish()                  // b moved into Finish
// b.Add("x")                    // error: b used after move
```

Compared with Go: call sites look exactly the same. Only declarations that keep or consume a value need `move` or `Clone`.

## 5.4 Rationale

- This is the C++ `const T&`, Swift and Mojo convention. Generic code such as `Index` works for owning `T`.
- The `move` mark goes on the few functions that take a value. Most functions have no mark, and calls look like Go.
- Trivial parameters stay real copies, so Go code like `for b != 0 { a, b = b, a%b }` works unchanged.
- **Rejected:** move-by-default parameters (Rust without `&`; hostile to Go-style generic APIs, and it puts a mark on most functions and reuses Go's `&` and `*` signs); copy-by-default owning parameters (hidden allocations); read-only trivial parameters (they reject ordinary Go code, and passing large ones by pointer gives wrong results when an argument aliases memory the callee writes).
