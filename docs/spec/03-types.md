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
- Predeclared generic types: `Unique[*T]`, `Shared[*T]`, `Weak[*T]` and `Option[T]` ([OWN-P](04-ownership.md#43-pointers-and-heap-allocation-d17)). The argument of `Unique`, `Shared` and `Weak` must be a pointer type `*T`.
- Types imported from C and C++ headers are plain Go++ types ([chapter 14](14-foreign-surface.md)).

**TYP-1.** `ref string` and `string` are different types. `ref []T` and `[]T` are different types. Identity follows [Type identity](https://go.dev/ref/spec#Type_identity): `ref string` is identical only to `ref string`.

**TYP-2.** *Assignability additions.* `nil` is assignable only to `error`, `any` (the empty interface), `unsafe.Pointer`, slices, maps, channels and `Option` types, not to a pointer kind, a func or an interface with methods, which are never nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). Besides Go's rules, a value `x` is assignable to type `V` if:
- `x` is a `string` and `V` is `ref string`; or `x` is a `[]T` and `V` is `ref []T` (owned to ref, borrowing, [OWN-V3](04-ownership.md#44-refs-d16));
- `x` is a `Unique[*T]` or `Shared[*T]` stored in a place and `V` is `*T` or `Option[*T]` ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17));
- `x` has type `T` and `V` is `Option[T]`, or `x` is `nil` and `V` is an `Option` ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)); an `Option[T]` is assignable to `T` only where it is checked ([OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17));
- the assignment follows the ownership rules ([OWN-R3](04-ownership.md#42-ownership-rules-d14)): assigning an owning value moves it.

A ref is **not** assignable to an *owning destination*: an owned variable, field, element, result, `move` parameter or composite-literal field. Use `string(v)` or `[]T(v)` to copy. A **borrowed** `string` or `[]T` parameter is not an owning destination: it accepts a ref (or an owned value, or a literal) at zero cost ([PAR-8, PAR-10](05-parameters.md)).

```go
func parse(s string) {}          // borrowed parameter
v := name[:3]                    // ref string
parse(v)                         // ok: borrowed parameter accepts a ref
var t string = v                 // error: owned variable; write string(v)
```

**TYP-3.** *Comparison.* `==` on owning types such as `string` compares by value and borrows both operands. Slices and maps are comparable only with `nil`, as in Go. Funcs are not comparable at all: a func is never nil, so `f == nil` is an error. An interface with methods (other than `error`) is never nil either, so `l == nil` is an error, while `err == nil`, `err != nil` and `x == nil` for an `x any` work as in Go ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). An `Option[T]` is comparable with `nil`, and two `Option[T]` values compare equal when both are nil, or both are set and their values are equal (`Option[*T]` compares by address); this needs a comparable `T`, so a struct with `Option` fields stays comparable and can be a map key (`var a, b Option[int]; a == b`, `map[Option[string]]int`). An `Option[T]` with a non-comparable `T` (`Option[struct{ s []int }]`) compares only with `nil`. Comparing an `Option[T]` with a plain `T` is allowed: `o == 5` means `o == Option[int](5)`. A pointer is not comparable with `nil`, because it is never nil (`p == nil` is an error). Structs and arrays are comparable when their fields are. A `ref string` compares with a `string` by contents, with both operands borrowed.

**TYP-4.** *Category summary.* The category of a type decides how it copies and ends its life. The full table and rules are in [OWN-C1 to OWN-C5](04-ownership.md#41-type-categories-d13).

## 3.2 Interfaces and runtime type information (D1.12)

Syntax: no change. Interfaces are Go's structural interfaces.

**TYP-5.** *Shared handles.* An interface value is a shared handle. Copying it shares the same dynamic value: an immutable, reference-counted box, or the pointer it holds. The box is immutable all the way down: a map or slice inside it is read-only through the interface (see the binding rules below). So `==`, type switches and copies behave as in Go.

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
- the data in the box is read-only **all the way down**: through a binding or an assertion, a map or a slice inside the interface can be read, measured (`len`) and ranged over, but not written. `v["b"] = 2`, `v["b"]++`, `delete(v, k)`, `clear(v)`, `x.(map[string]int)["b"] = 2` and `v[0] = 1` (on a `[]int` binding) are compile errors, nil or not, because the box is immutable and so is what it owns. "Inside an interface" means everything the box holds **by value**, all the way down until a pointer: the map or slice itself, and the maps and slices in struct fields and array elements of the boxed value. So `v.(Doc).Index["x"] = 1`, `case Doc:` with `d.Index["x"] = 1`, and `d.Tags[0] = 1` are errors. A `*T` field in the boxed struct still gives a writable pointee. Fixes: copy it out (`m := v.Clone(); m["b"] = 2`), or store a pointer in the interface (`var x any = &myMap`, then `(*p)["b"] = 2` through `case *map[string]int`; [OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17));
- the bound variable is read-only (the top-level rule of [OWN-R3b](04-ownership.md#42-ownership-rules-d14): it, and any part of it held by value, cannot be written, have its address taken, or get a pointer-receiver call, which would write into the immutable box) and can be used as an operand or a borrowed argument. In the same scope, a `:=` that reuses the name `v` assigns to `v`, so it is an error; use a new name;
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

**TYP-10.** *Field tables.* For every type converted to an interface, the type descriptor also carries a compact field table (field names, offsets, types, struct tags, and which fields are required, by `@Required` or by `@Strict` ([TYP-15](#34-required-fields-d14-rule-11), [TYP-15a](#34-required-fields-d14-rule-11)); the table holds the effective bit), emitted once per type (`linkonce_odr`). `fmt` and `encoding/json` read it, so printing and JSON work on any type, as in Go.

**TYP-11.** *No `reflect`.* There is no user-facing `reflect` package in v1.

## 3.3 Layout (D1.17, D1.22)

**TYP-12.** Struct layout is **declaration order with C alignment**. It is C-compatible, as C interop requires, so fields are never reordered automatically.

**TYP-13.** Informative sizes on a 64-bit target (the normative source is P04 D4.5 and D1.22):

| Type | Words | Content |
|---|---|---|
| `int`, `*T`, `Unique[*T]` | 1 | `int` is 64-bit on 64-bit targets |
| `string` | 3 | `{ptr, len, cap}` (always the global allocator) |
| `ref string`, `ref []T` | 2 | `{ptr, len}` |
| `[]T` | 4 | `{ptr, len, cap, allocator}` |
| `map[K]V` | 1 | pointer to the runtime object; the header stores its allocator |
| interface | 2 | `{itab*, data}` |
| func value | 2 | `{code*, env*}` |

**TYP-14.** `int` is 64-bit on 64-bit targets. Integer overflow wraps ([EXP-6](06-expressions.md)).

## 3.4 Required fields (D1.4 rule 11)

**TYP-15.** *`@Required` fields.* A struct field marked `@Required` (after its type, [MAC-12](13-macros-surface.md#132-rules)) must be set in every composite literal of the struct, by name or by position; leaving it out is a compile error that names the field. The attribute is allowed only on struct fields (embedded fields too). A struct with a required field has **no zero value** ([OWN-R9](04-ownership.md#42-ownership-rules-d14)), and so has any struct or array that contains such a type; for a generic type this belongs to the instance (`Box[User]`). So array literals with missing elements (`[3]User{{…}}`) and keyed slice literals with gaps (`[]User{5: {…}}`) are errors too, and so is any use of `m[k]` or `<-ch` other than a comma-ok form, a write `m[k] = v` or `delete` (`v := m[k]`, `m[k].Email`, `f(m[k])`, `case u := <-ch:`); the comma-ok forms (`u, ok := x.(User)`, `u, ok := clone(m[k])`, `u, ok := <-ch`, …) work (a single-value `clone(m[k])` or `move(m[k])` is an error), with `u` usable only where `ok` is true ([OWN-R9](04-ownership.md#42-ownership-rules-d14)). On a field of type `error`, `any` (the empty interface), a map or a channel type, `@Required` also means **never nil** (checked at compile time): a stored value must be known non-nil. This is the one list of known non-nil values (other rules link here): a literal, `make(…)`, a local checked with `!= nil` (flow typing, [OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17)), any value of a never-nil type converted to `error` or `any` (a pointer kind, a func, an interface with methods, or a concrete non-interface value, such as a `*MyErr` or a `Logger` stored into an `error` or `any` field), and a read of another never-nil `@Required` field of the same kind. `move` out of such a field and `&` of it (except as `mem.Replace`'s first argument) are errors (a required `error`, `any`, map or channel field of a `@Strict` struct is such a field too, [TYP-15a](#34-required-fields-d14-rule-11)); on a slice, an `unsafe.Pointer`, an `Option` or a type-parameter field it only means "must be set". On a pointer field (`*T`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`), a func field, or a field of an interface with methods other than `error`, it is a compile error, because those are never nil anyway ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17), ADR-0018). `move` out of any non-local place (element, field, `*p`, `&` range alias, a captured variable inside a closure) of such a struct (a `@Required` or `@Strict` one) is an error too, while swaps stay allowed ([OWN-R4](04-ownership.md#42-ownership-rules-d14)). The guarantee is about construction: a moved-from place still holds the all-zero bytes ([OWN-R5](04-ownership.md#42-ownership-rules-d14)). JSON decoding builds a fresh value: `u, err := json.Decode[User](data)`, an error naming the field if the input lacks a required one (ADR-0016).

```go
type User struct {
    ID    int    @Required
    Email string @Required
    Age   int
}

u := User{ID: 1, Email: "a@b.c"}     // OK; Age is 0
u := User{Email: "a@b.c"}            // error: missing required field ID
var v User                           // error: User has no zero value
users := make([]User, 0, 10)         // OK: no User is created
users = append(users, User{ID: 2, Email: "c@d.e"})
users := make([]User, 10)            // error: would create Users without ID
u, err := json.Decode[User](data)    // a fresh User; error if "ID" is missing in data
if w, ok := x.(User); ok { … }       // OK: w is used only where ok is true

type Car struct{ Owner Unique[*User] }       // a pointer field: never nil, no @Required needed
c := Car{Owner: new_unique(u)}       // OK
c := Car{Owner: nil}                 // error: Unique[*User] is never nil
old := mem.Replace(&c.Owner, new_unique(w))   // OK; move(c.Owner) is an error

type Server struct{ Log io.Writer }  // an interface field: never nil, must be set in every literal
type Job struct{ Done chan bool @Required }   // a channel field: @Required makes it never nil
```

**TYP-15a.** *`@Strict` structs and `@Option` fields* (ADR-0019). `@Strict` is a built-in attribute on a struct type, written on the line before `type` ([MAC-3](13-macros-surface.md#132-rules), generic struct types too). On a non-struct type, a func, a var or a field it is an error. In a `@Strict` struct every field counts as `@Required`, with all of the meaning of [TYP-15](#34-required-fields-d14-rule-11): it must be set in every literal, the struct has no zero value, this spreads to containing types, and the field is never nil for `error`, `any`, map and channel types ("must be set" only for slices, `unsafe.Pointer` and type-parameter fields). Embedded fields count like any field. Three kinds of field are **not** required: a field of type `Option[T]`, a field marked `@Option`, and a blank `_` field. Pointer, func and method-interface fields are required in every struct already, so in a `@Strict` struct they are simply required as before (no error, unlike `@Required` on them).

`@Option` is a built-in field attribute, written after the field's type ([MAC-12](13-macros-surface.md#132-rules)). It means "this field may be left out of a literal and gets its zero value". The field is a plain Go field: an `error`, map or `any` field marked `@Option` may be nil, so `Err: nil` is fine. These are compile errors:
- `@Option` in a struct that is not `@Strict` (the message says fields are optional there by default);
- `@Option` on a field whose type has no zero value (a pointer kind, a func, an interface with methods, a struct with required fields, a C++ type without a default constructor), because it can't be left out; use `Option[T]`;
- `@Option` on an `Option[T]` field (redundant);
- `@Option` on a blank `_` field (redundant: `_` is never required);
- `@Option` together with `@Required`;
- `@Required` on a field of a `@Strict` struct that `@Strict` already requires (redundant; the fix-it removes it).

`@Required` on an `Option[T]` field of a `@Strict` struct is allowed and keeps its TYP-15 meaning "must be set": the literal must name the field, and `Note: nil` is fine. `@Option` on an embedded field is allowed and means the same as on any field; if the embedded type has no zero value, it is the error above. For a type from another package the effective bits come from the serialized export data, as for `@Required`.

A `@Strict` struct with at least one required field has no zero value. If no field is required (all are `Option`, `@Option` or `_`), `@Strict` changes nothing and `gopp vet` warns about it. The required bit of each field is part of what must match in a struct conversion ([EXP-9](06-expressions.md#65-conversions)): a `@Strict` struct and a non-strict struct with the same fields don't convert into each other unless their effective required bits are equal. The effective bits are exported with the type, shown in the field table ([TYP-10](#32-interfaces-and-runtime-type-information-d112)), and used by JSON decoding (a missing required field is an error naming it; a missing `@Option` field gets its zero value; a missing `Option` field gets nil), by the macro API and by the LSP. Attributes are not Go syntax, so `.go` files are not affected.

```go
@Strict
type Payment struct {
    ID      int
    Amount  int64
    Note    Option[string]   // may be left out: nil
    Retries int @Option      // may be left out: 0
}

p := Payment{ID: 1, Amount: 500}   // OK
p := Payment{ID: 1}                // error: Amount not set
var q Payment                      // error: Payment has no zero value
type Bad struct{ N int @Option }   // error: Bad is not @Strict; fields are optional already
```

## 3.5 Rationale

- **Shared handles for interfaces** keep Go's `err == io.EOF`, `errors.Is`, `%w` and type switches working with no GC.
- **Borrowed interface parameters** make `fmt.Println(name)` free and non-consuming. Boxing happens only when a callee really keeps the value.
- **Field tables instead of `reflect`** give `fmt` and `json` what they need and nothing more.
- **C layout** is required for C interop (P07) and costs nothing in practice.
- **`@Strict` as an opt-in on the type**, not required-by-default: Go code and partial literals such as `http.Server{Addr: ":80"}` keep working, and adding a field to a library struct breaks nobody (ADR-0019).
- **`@Required` as an attribute**, not a keyword: no new grammar, and the "no zero value" rule already exists for C++ types (ADR-0016).
- **Rejected:** boxing every interface argument (hidden allocation); moving into every interface parameter (breaks `fmt.Println(x)` followed by a use of `x`).
