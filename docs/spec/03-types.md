# 3. Types

Base: Go spec [Types](https://go.dev/ref/spec#Types), [Properties of types and values](https://go.dev/ref/spec#Properties_of_types_and_values), [Underlying types](https://go.dev/ref/spec#Underlying_types), [Type identity](https://go.dev/ref/spec#Type_identity), [Assignability](https://go.dev/ref/spec#Assignability), [Interface types](https://go.dev/ref/spec#Interface_types), [Implementing an interface](https://go.dev/ref/spec#Implementing_an_interface), [Method sets](https://go.dev/ref/spec#Method_sets), [Slice types](https://go.dev/ref/spec#Slice_types), [Map types](https://go.dev/ref/spec#Map_types), [Channel types](https://go.dev/ref/spec#Channel_types), [Function types](https://go.dev/ref/spec#Function_types).

This chapter lists what changes in the type system: new types, type categories, interface values (D1.12) and layout (D1.17). The ownership rules themselves are in [chapter 4](04-ownership.md).

## 3.1 Type syntax

```
Type      = TypeName [ TypeArgs ] | TypeLit | "(" Type ")" .
TypeLit   = /* Go alternatives */ | RefType .
RefType   = "ref" ( "string" | SliceType ) .
```

- `move` is **not** a type. It is a mark on a parameter or receiver ([PAR-5](05-parameters.md)).
- `ref` is a contextual keyword: special only where a type is expected ([LEX-12](02-lexical.md#lex-12)).
- Predeclared generic types: `Unique[T]`, `Shared[T]`, `Weak[T]` ([OWN-P](04-ownership.md#43-pointers-and-heap-allocation-d17)).
- Types imported from C and C++ headers are plain Go++ types ([chapter 14](14-foreign-surface.md)).

**TYP-1.** `ref string` and `string` are different types. `ref []T` and `[]T` are different types. Identity follows [Type identity](https://go.dev/ref/spec#Type_identity): `ref string` is identical only to `ref string`.

**TYP-2.** *Assignability additions.* Besides Go's rules, a value `x` is assignable to type `V` if:
- `x` is a `string` and `V` is `ref string`; or `x` is a `[]T` and `V` is `ref []T` (owned to ref, borrowing, [OWN-V3](04-ownership.md#44-refs-d16));
- `x` is a `Unique[T]` or `Shared[T]` stored in a place and `V` is `*T` ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17));
- the assignment follows the ownership rules ([OWN-R3](04-ownership.md#42-ownership-rules-d14)): assigning an owning value moves it.

A ref is **not** assignable to an *owning destination*: an owned variable, field, element, result, `move` parameter or composite-literal field. Use `string(v)` or `[]T(v)` to copy. A **borrowed** `string` or `[]T` parameter is not an owning destination: it accepts a ref (or an owned value, or a literal) at zero cost ([PAR-8, PAR-10](05-parameters.md)).

```go
func parse(s string) {}          // borrowed parameter
v := name[:3]                    // ref string
parse(v)                         // ok: borrowed parameter accepts a ref
var t string = v                 // error: owned variable; write string(v)
```

**TYP-3.** *Comparison.* `==` on owning types such as `string` compares by value and borrows both operands. Slices, maps and funcs are comparable only with `nil`, as in Go. Structs and arrays are comparable when their fields are. A `ref string` compares with a `string` by contents, with both operands borrowed.

**TYP-4.** *Category summary.* The category of a type decides how it copies and ends its life. The full table and rules are in [OWN-C1 to OWN-C5](04-ownership.md#41-type-categories-d13).

## 3.2 Interfaces and runtime type information (D1.12)

Syntax: no change. Interfaces are Go's structural interfaces.

**TYP-5.** *Shared handles.* An interface value is a shared handle. Copying it shares the same dynamic value: an immutable, reference-counted box, or the pointer it holds. So `==`, type switches and copies behave as in Go.

**TYP-6.** *Boxing.* Converting an owned value into an interface **variable, field or element** moves it into a box. Storing a `*T` in an interface stores a borrow.

```go
var e any = name      // name (an owned string) moves into a box
// fmt.Println(name)  // compile error: name used after move
p := &point
var q any = p         // stores a borrow of point: no box, no move
```

**TYP-7.** *Interface parameters.* An interface parameter (including `...any`) is a shared handle like any interface value, so the callee may keep it: store it, return it or send it.
- Passing an interface value (such as `err`) passes the same box.
- Passing a non-interface owned value does **not** consume it. The callee gets a borrowed `(itab, &value)`: no box and no allocation. Only if the callee keeps it, the value is cloned into a box at that moment. So `fmt.Println(name)` does not consume `name`.
- A kept slice or map is then a copy that no longer shares elements with the caller's value. This is a listed difference from Go.
- A value that cannot be cloned (it does not satisfy `Clone`) can be passed as it is only when the callee does not keep the parameter ([CLO-3](11-closures.md)). Otherwise it must be passed as `move(x)`.

```go
func Log(v any) { saved = v }         // keeps v: escapes
n := "x"
Log(n)             // ok: n is cloned into a box inside Log. n is still usable.
f := File{...}     // has drop, no Clone()
Log(f)             // error: Log keeps its parameter and File has no Clone; write Log(move(f))
fmt.Println(f)     // ok: Println does not keep its parameter
```

**TYP-8.** *Inline values.* Trivial values up to one word (8 bytes: `int`, `float64`, raw pointers) are stored inline in the interface, with no box. Interface values stay 2 words.

**TYP-9.** *Type switches and assertions* use compile-time type descriptors. `case string:` matches a `string` **and** a `ref string`; `case []T:` matches a `[]T` and a `ref []T`. The same holds for the assertions `v.(string)` and `v.([]T)`. The binding is **always** a borrowed read-only variable (see below), whatever the dynamic type, because it may point into someone else's buffer. `case ref string:` is still allowed when the difference matters. It matches only a `ref string`, so it must come before `case string:` to be reached. A `case ref string:` placed after `case string:` is a compile error (an unreachable case, like Go's duplicate cases).

Interface `==` compares dynamic types, so a boxed `ref string` is not equal to a boxed `string` with the same bytes (`v == any("x")` is false when `v` holds a `ref string`); a type switch still matches both with `case string:`.

The box of an interface value is shared and immutable (TYP-5). So **every name-binding form** of an **owning** dynamic type binds a **borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) that borrows the boxed value:
- `s := v.(string)`, `s, ok := v.(string)` and `switch x := v.(type)` are all legal for an owning `T`. Go code like `if s, ok := v.(string); ok { use(s) }` works unchanged;
- the bound variable is read-only (the top-level rule of [OWN-R3b](04-ownership.md#42-ownership-rules-d14): it, and any part of it held by value, cannot be written, have its address taken, or get a pointer-receiver call, which would write into the immutable box) and can be used as an operand or a borrowed argument;
- to get an owned copy, write `owned := s.Clone()`, or `v.(string).Clone()` in a single-value expression. There is no comma-ok `Clone` form and no `move` fix-it, because a shared box cannot be moved from;
- where ownership is required directly, `v.(T)` used as a place gets the [OWN-R3a](04-ownership.md#42-ownership-rules-d14) error with fix-it `v.(T).Clone()`: a `move` argument, `return`, a send, a field store;
- when the asserted operand is a **temporary** interface value (`name := ctx.Value(k).(string)`), the temporary lives until the end of the binding's scope (lifetime extension, like a C++ const-reference binding). So it is not "a borrow of a temporary stored" in the sense of [OWN-R8](04-ownership.md#42-ownership-rules-d14);
- only a binding whose case type is a **Copy** type (numbers and the like) or a shared handle is a copy (retained), not a borrowed variable, and is writable. A `string` or `[]T` binding is never one of those, even when the value inside was a `ref`;
- if `v` is reassigned while the bound variable still borrows the old box, the box may be freed. A write to `v` that the compiler can see while the bound variable is still used afterwards is a compile error (like [RNG-3](08-slices-and-range.md)); writes it cannot see are UB ([UB-2d](16-undefined-behavior.md)).

```go
if s, ok := v.(string); ok {      // s borrows the box: no copy, no allocation
    fmt.Println(len(s))           // ok
    names = append(names, s.Clone())   // ownership needed: clone
}
n := len(v.(string))              // ok: borrowed operand
owned := v.(string).Clone()        // ok: a copy
return v.(string)                 // error: fix-it v.(string).Clone()
switch x := v.(type) {
case string:
    names = append(names, x)          // error: fix-it x.Clone()
    names = append(names, x.Clone())   // ok
case int:
    total += x                        // ok: trivial, copied
}
```

```go
switch v := x.(type) {
case ref string:  fmt.Println("ref", v)      // only a ref string; first, so it is reachable
case string:      fmt.Println("string", v)   // a string (a ref string was caught above)
}
```

**TYP-10.** *Field tables.* For every type converted to an interface, the type descriptor also carries a compact field table (field names, offsets, types, struct tags), emitted once per type (`linkonce_odr`). `fmt` and `encoding/json` read it, so printing and JSON work on any type, as in Go.

**TYP-11.** *No `reflect`.* There is no user-facing `reflect` package in v1.

## 3.3 Layout (D1.17, D1.22)

**TYP-12.** Struct layout is **declaration order with C alignment**. It is C-compatible, as C interop requires, so fields are never reordered automatically.

**TYP-13.** Informative sizes on a 64-bit target (the normative source is P04 D4.5 and D1.22):

| Type | Words | Content |
|---|---|---|
| `int`, `*T`, `Unique[T]` | 1 | `int` is 64-bit on 64-bit targets |
| `string` | 3 | `{ptr, len, cap}` (always the global allocator) |
| `ref string`, `ref []T` | 2 | `{ptr, len}` |
| `[]T` | 4 | `{ptr, len, cap, allocator}` |
| `map[K]V` | 1 | pointer to the runtime object; the header stores its allocator |
| interface | 2 | `{itab*, data}` |
| func value | 2 | `{code*, env*}` |

**TYP-14.** `int` is 64-bit on 64-bit targets. Integer overflow wraps ([EXP-6](06-expressions.md)).

## 3.4 Rationale

- **Shared handles for interfaces** keep Go's `err == io.EOF`, `errors.Is`, `%w` and type switches working with no GC.
- **Borrowed interface parameters** make `fmt.Println(name)` free and non-consuming. Boxing happens only when a callee really keeps the value.
- **Field tables instead of `reflect`** give `fmt` and `json` what they need and nothing more.
- **C layout** is required for C interop (P07) and costs nothing in practice.
- **Rejected:** boxing every interface argument (hidden allocation); moving into every interface parameter (breaks `fmt.Println(x)` followed by a use of `x`).
