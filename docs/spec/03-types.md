# 3. Types

Base: Go spec [Types](https://go.dev/ref/spec#Types), [Properties of types and values](https://go.dev/ref/spec#Properties_of_types_and_values), [Underlying types](https://go.dev/ref/spec#Underlying_types), [Type identity](https://go.dev/ref/spec#Type_identity), [Assignability](https://go.dev/ref/spec#Assignability), [Interface types](https://go.dev/ref/spec#Interface_types), [Implementing an interface](https://go.dev/ref/spec#Implementing_an_interface), [Method sets](https://go.dev/ref/spec#Method_sets), [Slice types](https://go.dev/ref/spec#Slice_types), [Map types](https://go.dev/ref/spec#Map_types), [Channel types](https://go.dev/ref/spec#Channel_types), [Function types](https://go.dev/ref/spec#Function_types).

This chapter lists what changes in the type system: new types, type categories, interface values (D1.12) and layout (D1.17). The ownership rules themselves are in [chapter 4](04-ownership.md).

## 3.1 Type syntax

```
Type      = TypeName [ TypeArgs ] | TypeLit | "(" Type ")" .
TypeLit   = /* Go alternatives */ .

TypeSpec     = /* Go alternatives */ | ExtendsDecl .
ExtendsDecl  = identifier [ TypeParameters ] "extends" Type StructBody .     /* TYP-16 */
StructBody   = "{" { FieldDecl ";" } "}" .
FieldDecl    = IdentifierList Type [ FieldAttributes ] [ Tag ] .              /* no EmbeddedField, TYP-25 */
```

`extends` is a contextual keyword ([LEX-12](02-lexical.md#lex-12)). A struct type has no embedded fields in `.gopp` files ([TYP-25](#35-single-inheritance-adr-0023)). Interface types can still embed interfaces and constraints.

- `move` is **not** a type. It is a mark on a parameter or receiver ([PAR-5](05-parameters.md)).
- `View[X]` is an ordinary `TypeName TypeArgs`: a predeclared generic type, not a keyword and not new syntax ([LEX-7](02-lexical.md#predeclared-identifiers), [OWN-V1](04-ownership.md#44-views-d16)). It is a read-only borrow of an `X`. `X` may be any type except the pointer kinds (`*T`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`), `unsafe.Pointer` and `View[…]`: `View[string]`, `View[[]byte]`, `View[User]` and `View[Bytes]` are fine; the others are a compile error that names the allowed forms.
- Predeclared generic types: `Unique[*T]`, `Shared[*T]`, `Weak[*T]`, `Option[T]` and `View[X]` ([OWN-P](04-ownership.md#43-pointers-and-heap-allocation-d17)). The argument of `Unique`, `Shared` and `Weak` must be a pointer type `*T`.
- Types imported from C and C++ headers are plain Go++ types ([chapter 14](14-foreign-surface.md)).

**TYP-1.** `View[T]` and `T` are different types for every `T` (`View[string]` and `string`, `View[[]T]` and `[]T`). Identity follows [Type identity](https://go.dev/ref/spec#Type_identity): `View[string]` is identical only to `View[string]`.

**TYP-2.** *Assignability additions.* `nil` is assignable only to `error`, `any` (the empty interface), `unsafe.Pointer`, slices, maps, channels and `Option` types, not to a pointer kind, a func or an interface with methods, which are never nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). Besides Go's rules, a value `x` is assignable to type `V` if:
- `x` has type `T` (owned or borrowed) and `V` is `View[T]` (owned to view, borrowing, [OWN-V3](04-ownership.md#44-views-d16));
- `x` is a `Unique[*T]` or `Shared[*T]` stored in a place and `V` is `*T` or `Option[*T]` ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17));
- `x` has type `T` and `V` is `Option[T]`, or `x` is `nil` and `V` is an `Option` ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)); an `Option[T]` is assignable to `T` only where it is checked ([OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17));
- the assignment follows the ownership rules ([OWN-R3](04-ownership.md#42-ownership-rules-d14)): assigning an owning value moves it.

A view is **not** assignable to an *owning destination*: an owned variable, field, element, result, `move` parameter or composite-literal field. Use `string(v)` or `[]T(v)` to copy. A **borrowed** `string` or `[]T` parameter is not an owning destination: it accepts an owned value or a literal at zero cost, and a `View[string]` for a `string` parameter (immutable bytes), and an interface with no methods such as `any` (boxing keeps the dynamic type `View[T]`). `View[T]` has no methods except the built-in `Clone` (OWN-R6), which makes an owned copy, so it never satisfies an interface with methods (`error`, `fmt.Stringer`): `var s fmt.Stringer = view` is an error. A view, or a part of one, is **not** accepted by any other non-view parameter (a `[]T`, a map, a `*T`, a plain `T`), because that parameter could write through it ([OWN-V1](04-ownership.md#44-views-d16), [PAR-8, PAR-10](05-parameters.md)).

```go
func parse(s string) {}          // borrowed parameter
v := name[:3]                    // View[string]
parse(v)                         // ok: borrowed parameter accepts a view
var t string = v                 // error: owned variable; write string(v)
```

**TYP-3.** *Comparison.* `==` on owning types such as `string` compares by value and borrows both operands. Slices and maps are comparable only with `nil`, as in Go. Funcs are not comparable at all: a func is never nil, so `f == nil` is an error. An interface with methods (other than `error`) is never nil either, so `l == nil` is an error, while `err == nil`, `err != nil` and `x == nil` for an `x any` work as in Go ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). An `Option[T]` is comparable with `nil`, and two `Option[T]` values compare equal when both are nil, or both are set and their values are equal (`Option[*T]` compares by address); this needs a comparable `T`, so a struct with `Option` fields stays comparable and can be a map key (`var a, b Option[int]; a == b`, `map[Option[string]]int`). An `Option[T]` with a non-comparable `T` (`Option[struct{ s []int }]`) compares only with `nil`. Comparing an `Option[T]` with a plain `T` is allowed: `o == 5` means `o == Option[int](5)`. A pointer is not comparable with `nil`, because it is never nil (`p == nil` is an error). Structs and arrays are comparable when their fields are. A `View[string]` compares with a `string` by contents, with both operands borrowed.

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

**TYP-9.** *Type switches and assertions* use compile-time type descriptors. `View[T]` and `T` are **different dynamic types**, for every `T`, `string` and slices included. `case T:` (and `v.(T)`) never matches a stored `View[T]`; only `case View[T]:` (and `v.(View[T])`) does. The binding is **always** a borrowed read-only variable (see below), whatever the dynamic type, because it may point into someone else's buffer; for a stored `View[T]` it is read-only all the way down ([OWN-V1](04-ownership.md#44-views-d16)): `case View[Doc]: d.Tags[0] = x` and `x.(View[Doc]).Tags[0] = x` are errors. Boxing a view into `any` or another interface with no methods is allowed (a `View[T]` has no methods except the built-in `Clone`, so boxing it into `error`, `fmt.Stringer` or any interface with methods is an error); the box keeps the dynamic type `View[T]`, and it may not outlive the view's owner (a compile error, [OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)).

When an interface holds a pointer kind, the binding is that pointer with its target writable (a `*T` or `Unique[*T]` target; a `Shared[*T]` target is read-only, [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)). By OWN-B9 rule 3 the binding is a reborrow of the pointer the box carries, and two interface copies that carry the same writable pointer follow the same rule: the other copy may not be used while the binding is live.

Interface `==` compares dynamic types, so a boxed `View[string]` is not equal to a boxed `string` with the same bytes (`v == any("x")` is false when `v` holds a `View[string]`). A type switch tells them apart: `case string:` does not match a stored `View[string]`.

The box of an interface value is shared and immutable (TYP-5). So **every name-binding form** of an **owning** dynamic type binds a **borrowed variable** ([OWN-R3b](04-ownership.md#42-ownership-rules-d14)) that borrows the boxed value:
- `s := v.(string)`, `s, ok := v.(string)` and `switch x := v.(type)` are all legal for an owning `T`. Go code like `if s, ok := v.(string); ok { use(s) }` works unchanged;
- the data in the box is read-only **all the way down**: through a binding or an assertion, a map or a slice inside the interface can be read, measured (`len`) and ranged over, but not written. `v["b"] = 2`, `v["b"]++`, `delete(v, k)`, `clear(v)`, `x.(map[string]int)["b"] = 2` and `v[0] = 1` (on a `[]int` binding) are compile errors, nil or not, because the box is immutable and so is what it owns. "Inside an interface" means everything the box holds **by value**, all the way down until a pointer: the map or slice itself, and the maps and slices in struct fields and array elements of the boxed value. So `v.(Doc).Index["x"] = 1`, `case Doc:` with `d.Index["x"] = 1`, and `d.Tags[0] = 1` are errors. A `*T` field in the boxed struct still gives a writable pointee (until P03.21: a box binding becomes a view, so a pointee reached through a field of a boxed value is read-only, [OWN-V1](04-ownership.md#44-views-d16)). When the box holds a pointer kind itself (`var x any = &doc`, then `case *Doc:` or `x.(*Doc)`), the binding is that pointer, a reborrow of it, and its target is writable as the pointer kind allows (`x.(*Doc).F = 1`; see the pointer-kind paragraph below and [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)), now and after P03.21, because the target is not in the box: only values held by value in the box become views. Every direct way to write is covered: also `copy`, `append` through a conversion that shares the box's slice (`append([]int(v), x)`), `move` of an element, and making a pointer into a part, which is the error itself (no such pointer is ever held): `&v[i]`, a pointer-receiver call or method value on an element, an `&` range variable over a part, a slice to array pointer conversion (`(*[2]int)(v)`) and `unsafe.SliceData(v)`. An Option `Get` binding of an `Option[T]` held in the box (`d, ok := w.Get()` on a `case Option[Doc]:` binding, or on `x.(Option[Doc])`) binds the box too: `d.Tags[0] = 1` is an error. P03.19 checks these direct writes. A write through a variable or a call result that stands for a part of the box (a view `r := v[1:]`, a range variable over a part, `head(v)[0] = 1` with `func head(s View[[]int]) View[[]int]`, a reference a helper returns, stores or sends) is not checked yet: P03.21 makes these bindings views ([ADR-0020](../adr/0020-view-type.md), [OWN-V1](04-ownership.md#44-views-d16)), read-only all the way down by their type, which closes every indirect write and escape in one place (a binding of a pointer kind the box holds stays that pointer). A binding whose case type is a type parameter (`case S:` with `S ~[]int`) is read-only too. A call that hands the map or slice on as a borrowed argument (`fill(v)`, `sort.Ints(v)`) is not checked by P03.19; once the binding is a view (P03.21), passing it to a non-view parameter is an error (OWN-V1), and P04.09's "writes this parameter" bit lets a callee that only reads take it. Fixes: copy it out (`m := v.Clone(); m["b"] = 2`), or store a pointer in the interface (`var x any = &myMap`, then `(*p)["b"] = 2` through `case *map[string]int`; [OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17));
- the bound variable is read-only (the top-level rule of [OWN-R3b](04-ownership.md#42-ownership-rules-d14): it, and any part of it held by value, cannot be written, have its address taken, or get a pointer-receiver call, which would write into the immutable box) and can be used as an operand or a borrowed argument. In the same scope, a `:=` that reuses the name `v` assigns to `v`, so it is an error; use a new name;
- to get an owned copy, write `owned := s.Clone()`, or `v.(string).Clone()` in a single-value expression. There is no comma-ok `Clone` form and no `move` fix-it, because a shared box cannot be moved from;
- where ownership is required directly, `v.(T)` used as a place gets the [OWN-R3a](04-ownership.md#42-ownership-rules-d14) error with fix-it `v.(T).Clone()`: a `move` argument, `return`, a send, a field store;
- when the asserted operand is a **temporary** interface value (`name := ctx.Value(k).(string)`), the temporary lives until the end of the binding's scope (lifetime extension, like a C++ const-reference binding). So it is not "a borrow of a temporary stored" in the sense of [OWN-R8](04-ownership.md#42-ownership-rules-d14);
- only a binding whose case type is a **Copy** type (numbers and the like) or a shared handle is a copy (retained), not a borrowed variable, and is writable. A `string` or `[]T` binding is never one of those, even when the value inside was a view;
- if `v` is reassigned while the bound variable still borrows the old box, the box may be freed. A write to `v` that the compiler can see while the bound variable is still used afterwards is a compile error (like [RNG-3](08-slices-and-range.md)); a write through a pointer to `v` is a writable borrow of `v` ([OWN-B9](04-ownership.md#45-borrow-checking-adr-0021)), so it is an error too ([UB-2d](16-undefined-behavior.md)).

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
case View[string]: fmt.Println("view", v)      // only a stored View[string]
case string:       fmt.Println("string", v)    // only a string, never a stored View[string]
}
```

**TYP-10.** *Field tables.* For every type converted to an interface, the type descriptor also carries a compact field table (field names, offsets, types, struct tags, and which fields are required, by `@Required` or by `@Strict` ([TYP-15](#34-required-fields-d14-rule-11), [TYP-15a](#34-required-fields-d14-rule-11)); the table holds the effective bit), emitted once per type (`linkonce_odr`). For a type that uses `extends` the table includes the parent chain: the parent's fields come first, then the child's own ([TYP-22](#35-single-inheritance-adr-0023)). `fmt` and `encoding/json` read it, so printing and JSON work on any type, as in Go.

**TYP-11.** *No `reflect`.* There is no user-facing `reflect` package in v1.

## 3.3 Layout (D1.17, D1.22)

**TYP-12.** Struct layout is **declaration order with C alignment**. It is C-compatible, as C interop requires, so fields are never reordered automatically.

**TYP-13.** Informative sizes on a 64-bit target (the normative source is P04 D4.5 and D1.22):

| Type | Words | Content |
|---|---|---|
| `int`, `*T`, `Unique[*T]` | 1 | `int` is 64-bit on 64-bit targets |
| `string` | 3 | `{ptr, len, cap}` (always the global allocator) |
| `View[T]` where the underlying type of `T` is `string` or a slice (`View[string]`, `View[[]T]`, `View[Bytes]`, `View[S]` for `type S string`) | 2 | `{ptr, len}` |
| `View[T]` for every other `T` (map, chan, func, interface, `Option` and structs included) | 1 | `{ptr}` |
| `[]T` | 4 | `{ptr, len, cap, allocator}` |
| `map[K]V` | 1 | pointer to the runtime object; the header stores its allocator |
| interface | 2 | `{itab*, data}` |
| func value | 2 | `{code*, env*}` |

**TYP-14.** `int` is 64-bit on 64-bit targets. Integer overflow wraps ([EXP-6](06-expressions.md)).

## 3.4 Required fields (D1.4 rule 11)

**TYP-15.** *`@Required` fields.* A struct field marked `@Required` (after its type, [MAC-12](13-macros-surface.md#132-rules)) must be set in every composite literal of the struct, by name or by position; leaving it out is a compile error that names the field. The attribute is allowed only on struct fields (a parent's fields apply in a child, [TYP-17](#35-single-inheritance-adr-0023)). A struct with a required field has **no zero value** ([OWN-R9](04-ownership.md#42-ownership-rules-d14)), and so has any struct or array that contains such a type; for a generic type this belongs to the instance (`Box[User]`). So array literals with missing elements (`[3]User{{…}}`) and keyed slice literals with gaps (`[]User{5: {…}}`) are errors too, and so is any use of `m[k]` or `<-ch` other than a comma-ok form, a write `m[k] = v` or `delete` (`v := m[k]`, `m[k].Email`, `f(m[k])`, `case u := <-ch:`); the comma-ok forms (`u, ok := x.(User)`, `u, ok := clone(m[k])`, `u, ok := <-ch`, …) work (a single-value `clone(m[k])` or `move(m[k])` is an error), with `u` usable only where `ok` is true ([OWN-R9](04-ownership.md#42-ownership-rules-d14)). On a field of type `error`, `any` (the empty interface), a map or a channel type, `@Required` also means **never nil** (checked at compile time): a stored value must be known non-nil. This is the one list of known non-nil values (other rules link here): a literal, `make(…)`, a local checked with `!= nil` (flow typing, [OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17)), any value of a never-nil type converted to `error` or `any` (a pointer kind, a func, an interface with methods, or a concrete non-interface value, such as a `*MyErr` or a `Logger` stored into an `error` or `any` field), and a read of another never-nil `@Required` field of the same kind. `move` out of such a field and `&` of it (except as `mem.Replace`'s first argument) are errors (a required `error`, `any`, map or channel field of a `@Strict` struct is such a field too, [TYP-15a](#34-required-fields-d14-rule-11)); on a slice, an `unsafe.Pointer`, an `Option` or a type-parameter field it only means "must be set". On a pointer field (`*T`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`), a func field, or a field of an interface with methods other than `error`, it is a compile error, because those are never nil anyway ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17), ADR-0018). `move` out of any non-local place (element, field, `*p`, `&` range alias, a captured variable inside a closure) of such a struct (a `@Required` or `@Strict` one) is an error too, while swaps stay allowed ([OWN-R4](04-ownership.md#42-ownership-rules-d14)). The guarantee is about construction: a moved-from place still holds the all-zero bytes ([OWN-R5](04-ownership.md#42-ownership-rules-d14)). JSON decoding builds a fresh value: `u, err := json.Decode[User](data)`, an error naming the field if the input lacks a required one (ADR-0016).

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

**TYP-15a.** *`@Strict` structs and `@Option` fields* (ADR-0019). `@Strict` is a built-in attribute on a struct type, written on the line before `type` ([MAC-3](13-macros-surface.md#132-rules), generic struct types too). On a non-struct type, a func, a var or a field it is an error. In a `@Strict` struct every field counts as `@Required`, with all of the meaning of [TYP-15](#34-required-fields-d14-rule-11): it must be set in every literal, the struct has no zero value, this spreads to containing types, and the field is never nil for `error`, `any`, map and channel types ("must be set" only for slices, `unsafe.Pointer` and type-parameter fields). The parent's fields count in a child ([TYP-17](#35-single-inheritance-adr-0023)). Three kinds of field are **not** required: a field of type `Option[T]`, a field marked `@Option`, and a blank `_` field. Pointer, func and method-interface fields are required in every struct already, so in a `@Strict` struct they are simply required as before (no error, unlike `@Required` on them).

`@Option` is a built-in field attribute, written after the field's type ([MAC-12](13-macros-surface.md#132-rules)). It means "this field may be left out of a literal and gets its zero value". The field is a plain Go field: an `error`, map or `any` field marked `@Option` may be nil, so `Err: nil` is fine. These are compile errors:
- `@Option` in a struct that is not `@Strict` (the message says fields are optional there by default);
- `@Option` on a field whose type has no zero value (a pointer kind, a func, an interface with methods, a struct with required fields, a C++ type without a default constructor), because it can't be left out; use `Option[T]`;
- `@Option` on an `Option[T]` field (redundant);
- `@Option` on a blank `_` field (redundant: `_` is never required);
- `@Option` together with `@Required`;
- `@Required` on a field of a `@Strict` struct that `@Strict` already requires (redundant; the fix-it removes it).

`@Required` on an `Option[T]` field of a `@Strict` struct is allowed and keeps its TYP-15 meaning "must be set": the literal must name the field, and `Note: nil` is fine. For a type from another package the effective bits come from the serialized export data, as for `@Required`.

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

## 3.5 Single inheritance (ADR-0023)

Go's embedding is removed in `.gopp` files ([TYP-25](#35-single-inheritance-adr-0023)). The replacement is `extends`: one parent, explicit method marks, dynamic dispatch only where a method is marked virtual. Marks are built-in attributes ([MAC-8](13-macros-surface.md#132-rules)). Pointer conversions are in [EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023).

**TYP-16.** *Syntax.* `type Child extends Parent { fields }` (grammar in [3.1](#31-type-syntax)).
- The parent must be a struct type, possibly generic: `type B[T any] extends A[T] { … }`. A child of a child is fine.
- There is one parent. `extends A, B` is an error.
- `extends` is contextual ([LEX-12](02-lexical.md#lex-12)): special only after the type name (and type parameters) in a type declaration. It is not reserved.
- `extends` on a non-struct type, an interface type, a pointer type or a C++ type is an error. C++ class inheritance at the boundary is open (ADR-0023 open point 1, P08 D8.8).

**TYP-17.** *What the child gets.* The child has the parent's fields and methods.
- A composite literal may set parent fields directly (`Card{Amount: 100, Number: "4242"}`) or through the parent (`Card{Payment: Payment{…}}`). Setting the same field both ways is an error. Grandparent fields work the same way. A composite literal of an `extends` type must use field names: a positional literal is an error.
- Field and method names may not clash with the parent's (or any ancestor's), except through `@Override` (TYP-18).
- `@Required`, `@Strict` and never-nil rules of the parent's fields apply in the child ([TYP-15](#34-required-fields-d14-rule-11), [TYP-15a](#34-required-fields-d14-rule-11)): a child literal must set the parent's required fields, and a child of a type with no zero value has no zero value ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).

**TYP-18.** *Method marks* (attributes, [MAC-8](13-macros-surface.md#132-rules)). On a parent method: no mark = **final**; `@Virtual` = may be replaced; `@Abstract` = no body, must be replaced. On a child method, `@Override` replaces a `@Virtual` or `@Abstract` method of an ancestor and is **required** for that. An `@Override` method stays replaceable by a further child. Compile errors:
- a same-named method without `@Override`;
- `@Override` with nothing to override (for example a typo);
- overriding a final method;
- a signature that does not match exactly (receiver kind, parameters, results, `!`);
- `@Virtual`, `@Abstract` or `@Override` on a value receiver;
- these marks on functions that are not methods;
- a body on an `@Abstract` method.

**TYP-19.** *Abstract types.* A type with an `@Abstract` method that is not overridden on its path is abstract.
- It cannot be created directly: a literal, `var x T`, `make([]T, n)`, `new_unique(T{…})`, a struct field or array element of that type, an argument, and so on are errors. The one exception is the parent part inside a child's composite literal: `Card{Payment: Payment{…}}` is allowed even when `Payment` is abstract, because that part is not a standalone value.
- It is usable only through pointers to a concrete child.
- A child that leaves an `@Abstract` method unimplemented is an error unless the child is itself marked `@Abstract` (a type attribute, [MAC-8](13-macros-surface.md#132-rules)): `@Abstract type Bird extends Animal {}`.  `@Abstract` as a type attribute on a type that is not an `extends` type is an error.

**TYP-20.** *`base`.* Inside a method of a child type, `base.M(…)` calls the direct parent's version of `M`, statically (no dispatch), for any parent method, final ones included. It is an error when `M` is `@Abstract` in the parent (no body). `base` is contextual: it has this meaning only inside methods of an `extends` type. `base.M()` is also allowed inside a function literal within such a method: it captures the receiver and means the same as in the method body. Declaring a local, parameter or result named `base` there is an error with a rename hint. Elsewhere `base` is an ordinary identifier ([LEX-12](02-lexical.md#lex-12)).

**TYP-21.** *Up- and downcast.* Rules in [EXP-13 to EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023). In short: `*Child` converts implicitly to `*Parent` and to any ancestor pointer, and `Unique`, `Shared`, `Weak` and `View` keep the conversion; downcast is a checked assertion `p.(*Card)`; copying a `Child` into a `Parent` value (value slicing) is an error. The owning-pointer upcast and the downcast need a type table in the parent (TYP-22).

**TYP-22.** *Cost and layout.*
- A type gets a hidden type-table pointer only if it (or an ancestor) has `@Virtual` or `@Abstract` methods. It is one word, stored first.
- The parent part is the first part of the child, after that word if the child has one, so an upcast is free (at most a fixed address adjustment). Struct layout stays declaration order with C alignment ([TYP-12](#33-layout-d117-d122)): parent fields, then the child's.
- Calls through a parent pointer to virtual methods are indirect. Every other call is direct.
- Final-only hierarchies (no `@Virtual` or `@Abstract` method anywhere in the hierarchy) have zero overhead and can still be extended. Their limits are compile errors: an upcast gives only a borrowed `*Parent`; `Unique[*Child]` to `Unique[*Parent]` (and `Shared`, `Weak`) is an error, because a parent owner could not drop the right type; a downcast `p.(*Child)` is an error, because there is no dynamic type. When the parent has a type table, all three are allowed.
- A cycle in `extends` is an error.

**TYP-23.** *Interfaces.* A child satisfies every interface its method set satisfies. The method set includes inherited methods. A call through an interface uses the overrides.

**TYP-24.** *Safety interplay.* Each of these is also stated where the rule lives:
- ownership: drop order is the child's fields first, then the parent part ([OWN-R2](04-ownership.md#42-ownership-rules-d14)); moves are of the whole value ([OWN-R3](04-ownership.md#42-ownership-rules-d14));
- `Clone`: auto-`Clone` covers parent and child fields; an abstract type is not cloneable by value ([OWN-R6](04-ownership.md#42-ownership-rules-d14));
- borrow checking: an upcast pointer is the same borrow ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021), ADR-0021);
- views: `View[Child]` converts to `View[Parent]`, read-only ([OWN-V1](04-ownership.md#44-views-d16), ADR-0020);
- `Send` / `Sync`: a child is labeled from all its fields, the parent's included ([CON-12](12-concurrency.md), ADR-0022);
- `@Strict`: [TYP-17](#35-single-inheritance-adr-0023);
- generics: a constraint `T extends Parent` does not exist (ADR-0023 open point 2; [GEN-2](10-generics.md)).

**TYP-25.** *No struct embedding.* In a `.gopp` file an embedded field (an anonymous field of a struct type: `struct{ Animal; … }`, `struct{ *Animal }`, `struct{ sync.Mutex }`) is a compile error. The fix-it says: use `extends` for the one main parent, and a named field for everything else (`Lock sync.Mutex`, `Log Logger`). Promoted fields and methods go away with embedding: the method set of a struct never includes methods of its fields. Interface embedding stays (`interface{ io.Reader; io.Writer }`, a constraint in a constraint interface): it composes type sets and is not inheritance.

**TYP-26.** *Go mode.* In `.go` files `extends`, `base` and the marks are not special, and struct embedding works as in Go.

```go
type Payment struct { Amount int64; ID string }

@Abstract
func (p *Payment) Pay() error

@Virtual
func (p *Payment) Fee() int64 { return 0 }

func (p *Payment) Total() int64 { return p.Amount + p.Fee() }   // final

type Card extends Payment { Number string }

@Override
func (c *Card) Pay() error { return nil }
@Override
func (c *Card) Fee() int64 { return c.Amount * 3 / 100 }

type PremiumCard extends Card {}

@Override
func (p *PremiumCard) Fee() int64 { return base.Fee() / 2 }      // Card's version

c := Card{Amount: 100, Number: "4242"}                            // ok: parent field set directly
d := Card{Payment: Payment{Amount: 100}, Number: "1"}             // ok: through the parent
e := Card{Amount: 1, Payment: Payment{Amount: 2}}                 // error: Amount set twice
var p Payment                                                     // error: Payment is abstract
type Z struct{ Payment }                                          // error: struct embedding is removed; use extends or a named field
type L struct{ sync.Mutex }                                       // error: write Lock sync.Mutex
type R interface{ io.Reader; io.Writer }                          // ok: interface embedding stays
```

The full example, its output and the list of errors are in [ADR-0023](../adr/0023-single-inheritance.md).

## 3.6 Rationale

- **Shared handles for interfaces** keep Go's `err == io.EOF`, `errors.Is`, `%w` and type switches working with no GC.
- **Borrowed interface parameters** make `fmt.Println(name)` free and non-consuming. Boxing happens only when a callee really keeps the value.
- **`extends` and marks instead of embedding** (ADR-0023): the call from a parent method to a replaced method does what the reader expects, every override is visible, and only `@Virtual` / `@Abstract` methods cost an indirect call. Embedding would be a second, different way to reuse a struct, so it is removed from `.gopp` files.
- **Field tables instead of `reflect`** give `fmt` and `json` what they need and nothing more.
- **C layout** is required for C interop (P07) and costs nothing in practice.
- **`@Strict` as an opt-in on the type**, not required-by-default: Go code and partial literals such as `http.Server{Addr: ":80"}` keep working, and adding a field to a library struct breaks nobody (ADR-0019).
- **`@Required` as an attribute**, not a keyword: no new grammar, and the "no zero value" rule already exists for C++ types (ADR-0016).
- **Rejected:** boxing every interface argument (hidden allocation); moving into every interface parameter (breaks `fmt.Println(x)` followed by a use of `x`).
