# 3. Types

Base: Go spec [Types](https://go.dev/ref/spec#Types), [Properties of types and values](https://go.dev/ref/spec#Properties_of_types_and_values), [Underlying types](https://go.dev/ref/spec#Underlying_types), [Type identity](https://go.dev/ref/spec#Type_identity), [Assignability](https://go.dev/ref/spec#Assignability), [Interface types](https://go.dev/ref/spec#Interface_types), [Implementing an interface](https://go.dev/ref/spec#Implementing_an_interface), [Method sets](https://go.dev/ref/spec#Method_sets), [Slice types](https://go.dev/ref/spec#Slice_types), [Map types](https://go.dev/ref/spec#Map_types), [Channel types](https://go.dev/ref/spec#Channel_types), [Function types](https://go.dev/ref/spec#Function_types).

This chapter lists what changes in the type system: new types, type categories, interface values (D1.12) and layout (D1.17). The ownership rules themselves are in [chapter 4](04-ownership.md).

## 3.1 Type syntax

```
Type       = TypeName [ TypeArgs ] | TypeLit | "(" Type ")" .
TypeLit    = /* Go alternatives */ .

StructType = "struct" "{" { FieldDecl ";" } "}" .
FieldDecl  = IdentifierList Type [ FieldAttributes ] [ Tag ] .              /* no EmbeddedField, TYP-25 */
```

A struct type has no embedded fields in `.gopp` files ([TYP-25](#35-inheritance-adr-0023)). Inheritance adds no syntax of its own: a type names each parent with the attribute `@Extends` before its declaration ([TYP-16](#35-inheritance-adr-0023), [ADR-0034](../adr/0034-extends-attribute.md)). Interface types can still embed interfaces and constraints.

**TYP-27.** *Named operator methods (`@Operator`)* ([ADR-0024](../adr/0024-operator-overloading.md)). Operator overloading uses normal Go identifier method names (`Add`, `Sub`, `Mul`, `Div`, `Rem`, `Eq`, `Cmp`, `Index`, `SetIndex`, `Iter`, …) marked with the built-in `@Operator` attribute ([MAC-8](13-macros-surface.md#132-rules)); `MethodName` in `MethodDecl` and `MethodElem` stays a normal Go `identifier`. Full operator signatures and resolution rules are in [6.10](06-expressions.md#610-operator-methods-adr-0024).
- **Overload sets.** Several `@Operator` methods on one type may share the same operator method name when their parameter lists differ (`@Operator func (a Vec2) Mul(b Vec2) Vec2` and `@Operator @Right func (a Vec2) Mul(s float64) Vec2`, [EXP-17](06-expressions.md#610-operator-methods-adr-0024)). Ordinary methods without `@Operator` keep unique names, as in Go, and never overload operators.
- **Constraints and interfaces.** Constraint interfaces list operator methods using normal Go method syntax (`interface{ Add(T) T; Cmp(T) int }`, [GEN-13](10-generics.md#102-rules)). An interface that lists two overloaded signatures of the same method name (`interface{ Mul(T) T; Mul(float64) T }`) is a constraint-only interface in v1; an interface with unique method names (`interface{ Add(int) int }`) is a normal Go interface too.
- **Not allowed on an `@Operator` method:** type parameters of its own, and `!` ([EXP-30](06-expressions.md#610-operator-methods-adr-0024)). Type parameters of the receiver type are fine: `@Operator func (a Vec[T]) Add(b Vec[T]) Vec[T]`.
- **Go mode.** In `.go` files `@Operator` does not exist.

Other notes on type syntax:

- Predeclared generic types: `Unique[T]`, `Shared[T]`, `Weak[T]`, `Option[T]` and `Local[T]` ([OWN-P](04-ownership.md#43-pointers-and-heap-allocation-d17), [CON-12](12-concurrency.md), [LEX-7](02-lexical.md#predeclared-identifiers)). `Unique[T]`, `Shared[T]` and `Weak[T]` take the pointee value type `T` directly (writing `*T` inside `[...]` is a compile error with fix-it `Unique[T]` / `Shared[T]` / `Weak[T]`). `Local[T]` takes a `func` or `interface` type `T` (`Local[func()]`, `Local[any]`) for single-goroutine-only callbacks and interfaces.
- Pointers to slice or map types (`*[]T`, `*map[K]V`, including pointers to defined slice or map types) are ordinary pointer types. Such a pointer is used without `*` for indexing (`xs[0]`, `m[k]`), slicing, `range`, `len`, `cap`, `delete` and `clear`; the whole value is written the same way (`xs = append(xs, 4)`, [ADR-0043](../adr/0043-no-star-to-write-through-a-pointer.md)) ([OWN-P1](04-ownership.md#43-pointers-and-heap-allocation-d17), [ADR-0041](../adr/0041-values-are-copied.md)).
- Types imported from C and C++ headers are plain Go++ types ([chapter 14](14-foreign-surface.md)).

**TYP-1.** *Slicing types.* Slicing a `string` produces `string`; slicing a `[]T` or array `[N]T` produces `[]T`, a new slice with copies of those elements; `&s[i:j]` on a `[]T` or an array produces `*[]T`, a view of those elements of `s` itself ([OWN-S1, OWN-S2](04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032)).

**TYP-2.** *Assignability additions.* `nil` is assignable only to `error`, `any` (the empty interface), `unsafe.Pointer`, slices, maps, channels and `Option` types, not to a pointer kind, a func or an interface with methods, which are never nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). Besides Go's rules, a value `x` is assignable to type `V` if:
- `x` is a `Unique[T]` or `Shared[T]` stored in a place and `V` is `*T` or `Option[*T]` ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17));
- `x` has type `T` and the destination is a place of type `*T`: the assignment writes the variable the pointer points to (`n = 9`, `xs = append(xs, 4)`). And `x` has type `*T` where only a `T` fits: the value it points to is read. A `*T` assigned to a `*T` re-points, as in Go ([OWN-P5](04-ownership.md#43-pointers-and-heap-allocation-d17), [ADR-0043](../adr/0043-no-star-to-write-through-a-pointer.md));
- `x` has type `T` and `V` is `Option[T]`, or `x` is `nil` and `V` is an `Option` ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)); an `Option[T]` is assignable to `T` only where it is checked ([OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17));
- the assignment follows the ownership rules ([OWN-R3](04-ownership.md#42-ownership-rules-d14)): the destination gets its own copy of the value, for every type. Plain data is copied by its bytes; a `string`, `Shared[T]` or other counted handle is copied as the handle; a `[]T` or `map[K]V` is copied with its elements. The copy is skipped on the last use of a local and with `move(x)`. A value that cannot be copied ([OWN-C3](04-ownership.md#41-type-categories-d13)) is moved (`move(x)`, or a temporary); a plain copy of it is a compile error, also on its last use.

```go
func parse(s string) {}          // parse only reads s, so no copy is made (0 retain/release)
v := name[:3]                    // string sharing name's backing buffer in O(1)
parse(v)                         // ok
var t string = v                 // ok: shares backing buffer
```

**TYP-3.** *Comparison.* `==` on `string` and structs compares by value and borrows both operands. Slices and maps are comparable only with `nil`, as in Go. Funcs are not comparable at all: a func is never nil, so `f == nil` is an error. An interface with methods (other than `error`) is never nil either, so `l == nil` is an error, while `err == nil`, `err != nil` and `x == nil` for an `x any` work as in Go ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). An `Option[T]` is comparable with `nil`, and two `Option[T]` values compare equal when both are nil, or both are set and their values are equal (`Option[*T]` compares by address); this needs a comparable `T`, so a struct with `Option` fields stays comparable and can be a map key (`var a, b Option[int]; a == b`, `map[Option[string]]int`). An `Option[T]` with a non-comparable `T` (`Option[struct{ s []int }]`) compares only with `nil`. Comparing an `Option[T]` with a plain `T` is allowed: `o == 5` means `o == Option[int](5)`. A pointer is not comparable with `nil`, because it is never nil (`p == nil` is an error). Structs and arrays are comparable when their fields are.

**TYP-4.** *Category summary.* The category of a type decides how it copies and ends its life. The full table and rules are in [OWN-C1 to OWN-C5](04-ownership.md#41-type-categories-d13).

## 3.2 Interfaces and runtime type information (D1.12)

Syntax: no change. Interfaces are Go's structural interfaces.

**TYP-5.** *Interface values own what they hold* ([ADR-0041](../adr/0041-values-are-copied.md) rule 11). An interface value has a box of its own that holds a copy of the value put into it. The box has one owner and no reference count; a value of up to one word is stored inline ([TYP-8](#32-interfaces-and-runtime-type-information-d112)). Copying an interface value (`b := a`, storing, passing, sending) copies the value it holds, by the rule of that value's type ([OWN-R3](04-ownership.md#42-ownership-rules-d14)): a `string` or a `Shared[T]` as its handle, a `[]T` or a `map[K]V` with its elements, a `*T` as the pointer. After `b := a`, nothing done to `b` is seen through `a`. The copy is skipped when the program cannot tell, as for every value: on the last use of the source, with `move(x)`, and for a parameter that the function only reads. A method called through the interface works on the interface's own value and may change it. Only a value that can be copied can be converted to an interface type ([TYP-6](#32-interfaces-and-runtime-type-information-d112)), so an interface value can always be copied. `==` and type switches work as in Go. This includes methods with a pointer receiver: the interface owns the value, so it can pass its address, and a struct value satisfies an interface through such methods without being boxed as a pointer.

**TYP-6.** *Boxing.* Converting a value into a **kept** interface variable, field or element boxes it (ordinary interfaces are `Send` by default, so the converted value must be `Send`; `Local[I]` accepts single-goroutine non-`Send` values such as a `Shared[T]` whose `T` is not `Sync`, [CON-12](12-concurrency.md)). The box gets its own copy of the value ([OWN-R3](04-ownership.md#42-ownership-rules-d14), [ADR-0041](../adr/0041-values-are-copied.md)); the copy is skipped when the source is a temporary, on the last use of a local, or with `move(x)`:
- An `OWN-C1` trivial value up to one word is stored inline ([TYP-8](#32-interfaces-and-runtime-type-information-d112)); larger `OWN-C1` values copy into a box.
- A `string`, `Shared[T]`, `Weak[T]`, `chan T` or `func` value (alone or as a part of a struct) is copied as its handle: the box retains it (no retain on the last use of a local), so the caller's variable stays usable if read later.
- A `[]T` or `map[K]V` (or a struct, array or `Option` that holds one) is copied into the box with its elements: a new buffer with a copy of every element or entry. When the copy is skipped, the value moves into the box in $O(1)$. Later writes to the source variable never change the boxed value.
- A value that cannot be copied (a `File`, [OWN-C3](04-ownership.md#41-type-categories-d13)) cannot be converted to an interface type ([ADR-0041](../adr/0041-values-are-copied.md) rule 3): `var x any = f` is a compile error, and so is `var x any = move(f)`. The fix-its are to put the value in a `Shared[T]` first (`var x any = NewShared(move(f))`), or to pass `&f` as the interface for the length of a call ([TYP-7](#32-interfaces-and-runtime-type-information-d112)). A `Unique[T]` is copied like any other value: a new allocation holding a copy of what it points to.
- Storing a `*T` in a non-escaping local interface (`var q Local[any] = &point`) stores a tracked borrow (no heap box, subject to [OWN-B5](04-ownership.md#45-borrow-checking-adr-0021)).

```go
var a any = name      // name (string) is copied as its handle (retained); name stays usable!
fmt.Println(name)     // ok
s := []int{1, 2, 3}
var c any = s         // s is used below, so the box gets its own copy of the elements
s[0] = 99             // ok: c still holds [1, 2, 3]
f := openFile!()      // a File cannot be copied (drop, no Clone)
// var b any = f      // compile error: a File cannot be converted to an interface, also with move(f)
var b any = NewShared(move(f))   // ok: the box holds a Shared[File]
// f.Write(data)      // compile error (E0451): f used after move
p := &point
var q Local[any] = p  // stores a local borrow of point: no box, tracked by OWN-B5
```

**TYP-7.** *Interface parameters.* An interface parameter (including `...any`) is the function's own interface value, like every by-value parameter ([PAR-1](05-parameters.md)):
- Passing an interface value (such as `err`) copies it, and so copies the value it holds ([TYP-5](#32-interfaces-and-runtime-type-information-d112)). The copy is skipped when the callee only reads the parameter, on the last use of the argument, and with `move(x)`.
- Passing a non-interface value does **not** allocate a box at the call when the callee does not keep the parameter: the callee receives `(itab, &value)`. When the callee only reads the parameter, `&value` is the caller's variable itself, so `fmt.Println(name)` and `fmt.Println(slice)` neither allocate, copy nor consume `name` or `slice`. When the callee may change the parameter without keeping it, `&value` is a copy in the caller's frame.
- Passing `&x` as the interface (`fmt.Println(&file)`) lends `x` for the length of the call: nothing is boxed or copied, and the function may change `x` through it. This is how a value that cannot be copied (a `File`) is given to an interface parameter that the callee does not keep.
- If the callee keeps the parameter (stores, returns or sends it), the caller boxes the value for the call, by the rules of [TYP-6](#32-interfaces-and-runtime-type-information-d112): the box gets its own copy, and the copy is skipped when the argument is a temporary, the last use of a local, or `move(x)`. A value that cannot be copied (a `File`) cannot be converted to an interface type ([TYP-6](#32-interfaces-and-runtime-type-information-d112)): `Log(f)` and `Log(move(f))` are compile errors; put it in a `Shared[T]` first (`Log(NewShared(move(f)))`).

```go
func Log(v any) { saved = v }         // keeps v
n := "x"
Log(n)             // ok: n is boxed because Log keeps v; n stays usable
xs := []int{1, 2}
Log(xs)            // ok: the box gets its own copy of xs, because xs is used below
xs[0] = 9          // ok: saved still holds [1, 2]
f := File{...}     // cannot be copied (has drop, no Clone)
// fmt.Println(f)  // compile error: a File cannot be converted to an interface
fmt.Println(&f)    // ok: Println does not keep v, so f is only borrowed for the call
// Log(move(f))    // compile error: a File cannot be converted to an interface
Log(NewShared(move(f)))   // ok: the box holds a Shared[File]; reading f afterwards is a compile error (E0451)
```

**TYP-8.** *Inline values.* Trivial values up to one word (8 bytes: `int`, `float64`, raw pointers) are stored inline in the interface, with no box. Interface values stay 2 words.

**TYP-9.** *Type switches and assertions* use compile-time type descriptors. `x := v.(T)`, `x, ok := v.(T)` and `switch x := v.(type)` give `x` a copy of the value that `v` holds ([TYP-5](#32-interfaces-and-runtime-type-information-d112), [ADR-0041](../adr/0041-values-are-copied.md) rule 11). Asserting to a type that cannot be copied (`v.(os.File)`) is a compile error: no interface value holds one.
- `x` is an ordinary variable. It can be changed (`x[0] = 1`, `x["b"] = 2`, `delete(x, k)`, `clear(x)`, `x.Tags[0] = "a"`, `&x`, a pointer-receiver call), and changing it does not change `v`.
- The copy is made by the rule of the type ([OWN-R3](04-ownership.md#42-ownership-rules-d14)): a plain value copies its bytes; a `string`, `Shared[T]`, `Weak[T]`, `chan` or `func` is copied as its handle (so `names = append(names, s)` retains the bytes of the string); a `[]T` or `map[K]V` gets a new buffer; a `Unique[T]` gets a new allocation; a `*T` is copied as the pointer and reborrows its target while live per [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021). What a `Shared[T]` holds stays read-only through the handle: `x.Count++` on a `Shared[Config]` is a compile error, and a value that changes is in a `Mutex` (`Shared[Mutex[Config]]`, [OWN-P8](04-ownership.md#43-pointers-and-heap-allocation-d17), [ADR-0042](../adr/0042-shared-is-read-only.md)).
- The copy is skipped when the program cannot tell ([OWN-R3](04-ownership.md#42-ownership-rules-d14)): when `x` is only read, when `v` is a temporary (`cfg := ctx.Value(k).(Config)`), and on the last use of `v`. So reading fields, indexing, `len`, comparing with `nil` and ranging over `x` cost no copy and no allocation. When `v` is assigned while `x` is still used, `x` keeps the value it was given ([UB-2d](16-undefined-behavior.md)).
- To change the value that `v` itself holds, call a method through the interface ([TYP-5](#32-interfaces-and-runtime-type-information-d112)) or assign the changed copy back (`v = x`). To change one value that every holder sees, hold a `Shared[Mutex[T]]` in the interface (`var box any = NewShared(sync.NewMutex(Holder{M: myMap}))`, [OWN-P8](04-ownership.md#43-pointers-and-heap-allocation-d17)).
- This replaces the earlier deep read-only binding into the box ([ADR-0041](../adr/0041-values-are-copied.md) rule 11).

```go
if s, ok := v.(string); ok {      // a string is copied as its handle: works just like Go
    fmt.Println(len(s))
    names = append(names, s)      // ok: retains the bytes of the string
}
switch x := v.(type) {
case []int:
    fmt.Println(len(x), x[0])     // ok: x is only read, so no copy is made
case map[string]int:
    x["b"] = 2                    // ok: x is a copy of its own; v still holds the old map
    fmt.Println(len(x))
case int:
    total += x                    // ok: trivial, copied
}
```

**TYP-10.** *Field tables.* For every type converted to an interface, the type descriptor also carries a compact field table (field names, offsets, types, struct tags, and which fields are required by `@Required`, [TYP-15](#34-required-fields-d14-rule-11)), emitted once per type (one weak definition that the linker merges). For a type with parents ([TYP-16](#35-inheritance-adr-0023)) the table includes all parent chains: the parents' fields come first in `@Extends` order, then the child's own ([TYP-22](#35-inheritance-adr-0023)). `fmt` and `encoding/json` read it, so printing and JSON work on any type, as in Go.

**TYP-11.** *No `reflect`.* There is no user-facing `reflect` package in v1.

## 3.3 Layout (D1.17, D1.22)

**TYP-12.** Struct layout is **declaration order with C alignment**. It is C-compatible, as C interop requires, so fields are never reordered automatically.

**TYP-13.** Informative sizes on a 64-bit target (the normative source is P04 D4.5, D1.22, [ADR-0029](../adr/0029-slice-and-map-allocator-word.md), [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md) and [ADR-0041](../adr/0041-values-are-copied.md)):

| Type | Words | Content |
|---|---|---|
| `int`, `*T`, `Unique[T]`, `Shared[T]`, `Weak[T]` | 1 | `int` is 64-bit on 64-bit targets |
| `string` | 3 | `{ptr, len, backing}` (`backing == nil` for static literals/empty; heap `backing` holds `{refcount, byte_cap}` plus a trailing NUL byte) |
| `[]T` | 4 | `{ptr, len, cap, backing}` (`backing == nil` for a nil or empty slice and for a `&s[i:j]` view; heap `backing` holds `{alloc_cap, allocator}`, with no reference count: a slice has one owner) |
| `map[K]V` | 1 | pointer to the runtime object; the header stores `{allocator, …}`, with no reference count: a map has one owner |
| interface | 2 | `{itab*, data}` |
| func value | 2 | `{code*, env*}` |

**TYP-14.** `int` is 64-bit on 64-bit targets. The byte sizes in this spec are for 64-bit targets, and version 1 has only 64-bit targets (`linux/amd64`, `windows/amd64`, [ADR-0037](../adr/0037-v1-targets.md)). Integer overflow wraps ([EXP-6](06-expressions.md)).

## 3.4 Required fields (D1.4 rule 11)

**TYP-15.** *`@Required` fields.* A struct field marked `@Required` (after its type, [MAC-12](13-macros-surface.md#132-rules)) must be set in every composite literal of the struct, by name or by position; leaving it out is a compile error that names the field. The attribute is allowed only on struct fields (parents' fields apply in a child, [TYP-17](#35-inheritance-adr-0023)). A struct with a required field has **no zero value** ([OWN-R9](04-ownership.md#42-ownership-rules-d14)), and so has any struct or array that contains such a type; for a generic type this belongs to the instance (`Box[User]`). So array literals with missing elements (`[3]User{{…}}`) and keyed slice literals with gaps (`[]User{5: {…}}`) are errors too, and so is any single-value read of `m[k]` or `<-ch` other than a comma-ok form, a write `m[k] = v` or `delete` (`v := m[k]`, `m[k].Email`, `f(m[k])`, `case u := <-ch:`); the comma-ok forms (`u, ok := x.(User)`, `u, ok := m[k]`, `u, ok := <-ch`, …) work (`mem.Take` on it is an error), with `u` usable only where `ok` is true ([OWN-R9](04-ownership.md#42-ownership-rules-d14)). On a field of type `error`, `any` (the empty interface), a map or a channel type, `@Required` also means **never nil** (checked at compile time): a stored value must be known non-nil. This is the one list of known non-nil values (other rules link here): a literal, `make(…)`, a local checked with `!= nil` (flow typing, [OWN-P10](04-ownership.md#43-pointers-and-heap-allocation-d17)), any value of a never-nil type converted to `error` or `any` (a pointer kind, a func, an interface with methods, or a concrete non-interface value, such as a `*MyErr` or a `Logger` stored into an `error` or `any` field), and a read of another never-nil `@Required` field of the same kind. `mem.Take` on such a field and `&` of it (except as `mem.Replace`'s first argument with a known non-nil replacement) are errors; on a slice, an `unsafe.Pointer` or a type-parameter field it only means "must be set". On an `Option[T]` field (including a defined type over `Option[T]`, or a `@Required` type-parameter field instantiated with an `Option` type), `@Required` is a compile error (`E0363`): `Option[T]` means optional while `@Required` means required (fix-its: `T @Required` or `Option[T]`). On a pointer field (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`), a func field, or a field of an interface with methods other than `error`, `@Required` is also a compile error, because those are never nil anyway ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17), ADR-0018). `mem.Take` on any non-local place (element, field, `*p`, `&` range alias, a captured variable inside a closure) of a `@Required` struct is an error too, while `mem.Replace(&place, v)` and swaps stay allowed ([OWN-R4](04-ownership.md#42-ownership-rules-d14)). Type identity includes each field's `@Required` attribute as it includes struct tags (`struct{A int @Required}` and `struct{A int}` are distinct types), and conversions between struct types compare requiredness and never-nil-ness per field ([EXP-9](06-expressions.md#65-conversions)). The guarantee is about construction: a moved-from local cannot be read before reassignment (`E0451`, [OWN-R5](04-ownership.md#42-ownership-rules-d14)). JSON decoding builds a fresh value: `u, err := json.Decode[User](data)`, an error naming the field if the input lacks a required one (ADR-0016).

```go
type User struct {
    ID    int    @Required
    Email string @Required
    Note  Option[string]   // optional: nil when omitted
    Age   int              // optional: 0 when omitted
}

u := User{ID: 1, Email: "a@b.c"}     // OK; Note is nil, Age is 0
u := User{Email: "a@b.c"}            // error: missing required field ID
var v User                           // error: User has no zero value
users := make([]User, 0, 10)         // OK: no User is created
users = append(users, User{ID: 2, Email: "c@d.e"})
users := make([]User, 10)            // error: would create Users without ID
u, err := json.Decode[User](data)    // a fresh User; error if "ID" is missing in data
if w, ok := x.(User); ok { … }       // OK: w is used only where ok is true

type Car struct{ Owner Unique[User] }        // a pointer field: never nil, no @Required needed
c := Car{Owner: NewUnique(u)}        // OK
c := Car{Owner: nil}                 // error: Unique[User] is never nil
old := mem.Replace(&c.Owner, NewUnique(w))   // OK; mem.Take(&c.Owner) is an error

type Server struct{ Log io.Writer }  // an interface field: never nil, must be set in every literal
type Job struct{ Done chan bool @Required }        // a channel field: @Required makes it never nil
type Bad struct{ Note Option[string] @Required }   // error E0363: Option[T] and @Required conflict
```

## 3.5 Inheritance (ADR-0023)

Go's struct embedding is removed in `.gopp` files ([TYP-25](#35-inheritance-adr-0023)). The replacement is the attribute `@Extends`: one or more struct parents without diamonds, explicit method marks, disambiguated `base` calls, and dynamic dispatch only where a method is marked virtual. Marks are built-in attributes ([MAC-8](13-macros-surface.md#132-rules)). Pointer conversions are in [EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023).

**TYP-16.** *Syntax.* A type names each of its parents with the built-in attribute `@Extends(Parent)` before its declaration ([MAC-8](13-macros-surface.md#132-rules), [ADR-0034](../adr/0034-extends-attribute.md)):
```go
@Extends(Payment)
@Extends(Auditable)
@Extends(Timestamps)
type Card struct {
    Number string
}
```
- **One parent per attribute.** Several parents are several `@Extends` attributes. Their order, top to bottom, is the order of the parents (`P1`, `P2`, …) wherever a rule speaks of it: layout ([TYP-22](#35-inheritance-adr-0023)), drop order ([TYP-24](#35-inheritance-adr-0023)) and the C++ parent ([TYP-28](#35-inheritance-adr-0023)). `@Extends(A, B)` and `@Extends()` are errors. Other attributes (`@Abstract`, `@Derive(…)`, `@Pinned`) may stand before, between or after them.
- **Where it goes.** Before a top-level declaration of one type. When the declared type is a struct type literal (`type Card struct { … }`), the type inherits from the parent. When it is an interface type literal (`type Pet interface { … }`), the interface becomes a constraint ([GEN-12](10-generics.md#102-rules)). Anywhere else `@Extends` is an error: on an alias (`type X = Y`), on a type defined from another type (`type X Y`), on a parenthesized `type ( … )` group, and on anything that is not a type. A type declared inside a function has no parents, because attributes do not go on statements ([13.1](13-macros-surface.md)).
- **The argument** is one type. A parent must be a struct type; it may be qualified (`@Extends(audit.Trail)`) and generic. The type parameters of the declaration are in scope in the argument, although it is written before them: `@Extends(A[T]) @Extends(Log[T]) type B[T any] struct { … }`. A child of a child is fine.
- **No cycles, no duplicate parents, no diamonds.** A cycle of parents is an error, and so is naming the same parent twice, two instantiations of the same generic parent (`Box[int]` and `Box[string]`), a parent that is already an ancestor of another parent (`Payment` and `Card`), or two parents that share any common ancestor (`B` and `C` that both have the parent `Entity`, as parents of `D`). Every ancestor appears at most once in a type's hierarchy.
- `type X Y` where `Y` has parents, is a tagged type or is an abstract type is an error (it would copy the declaration, hidden parents and all): write `@Extends(Y)` before `type X struct{}`. A type with no hierarchy is defined from another as in Go.
- A parent that is not a struct type (a number, an interface type, a pointer type) is an error. At most one C++ class may be a parent, and it must be the first `@Extends` ([TYP-28](#35-inheritance-adr-0023), ADR-0023 decision 14).

**TYP-17.** *What the child gets and clash rules.* The child has all parents' fields and methods.
- A composite literal may set ancestor fields directly (`Card{Amount: 100, Actor: "alice", CreatedAt: now, Number: "4242"}`) or through any parent part (`Card{Payment: Payment{Amount: 100}, Timestamps: Timestamps{CreatedAt: now}, Number: "4242"}`). Setting the same field both ways is an error. Grandparent fields work the same way. A composite literal of a type with parents must use field names: a positional literal is an error.
- **Field clashes:** a field name of the child may not clash with any ancestor's field or method, and two parents may not declare a field (or a field and a method) of the same name.
- **Method clashes:** a child method of the same name as an ancestor's requires `@Override` (TYP-18). Across multiple parents, if two parents have a method of the same name and either is **final** or their signatures differ, it is a compile error; if all clashing parent versions are `@Virtual` or `@Abstract` with the **exact same signature**, it is a compile error unless the child declares an `@Override` method of that signature to resolve the clash (or all clashing versions are `@Abstract` and the child itself is marked `@Abstract`). One `@Override` on the child replaces all matching ancestor slots.
- `@Required` and never-nil rules of all parents' fields apply in the child ([TYP-15](#34-required-fields-d14-rule-11)): a child literal must set every parent's required fields, and if any parent has no zero value the child has no zero value ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).

**TYP-18.** *Method marks* (attributes, [MAC-8](13-macros-surface.md#132-rules)). On a parent method: no mark = **final**; `@Virtual` = may be replaced; `@Abstract` = no body, must be replaced. On a child method, `@Override` replaces the matching `@Virtual` or `@Abstract` method(s) of one or more ancestors and is **required** for that. An `@Override` method stays replaceable by a further child. Compile errors:
- a same-named method without `@Override`;
- `@Override` with nothing to override (for example a typo);
- overriding a final method;
- a signature that does not match exactly (receiver kind, parameters, results, `!`);
- `@Virtual`, `@Abstract` or `@Override` on a value receiver;
- these marks on functions that are not methods;
- a body on an `@Abstract` method.

**TYP-19.** *Abstract types.* A type with an `@Abstract` method that is not overridden on its path is abstract, and so is a type marked `@Abstract` (even when nothing is left open: the attribute states the intent). A value receiver of an abstract type is an error (`func (s Shape) M()`), and so is its method expression `Shape.M`: the receiver is a value of the type.
- It has no values, so it is never held by value. This is decided in one place on the real types: every type expression that holds a value (a variable, a struct field, an element of an array, slice, map (key or value) or channel, a parameter, a result, the type of `make`, of a type assertion or of a conversion) may not be abstract, and a type argument is decided on the substituted types: an instance that holds the argument by value is an error (`Box[Payment]` for `struct{ V T }`, `Option[Payment]`, a generic function whose parameter or result holds it), one that holds it only behind a pointer is fine (`Ptr[Payment]` for `struct{ P *T }`, `Feed[Payment]` with `func Feed[T any](p *T)`). Generic code that writes a type holding `T` by value behind a pointer or an interface (`func F[T any](p *Box[T])`, `*[4]T`, `type H[T any] struct{ p *Box[T] }`, `type A[T any] = *Box[T]`), or inside a type argument that the generic it is given to holds only behind a pointer (`Ptr[[]T]`, `Ptr[Box[T]]`, `Ptr[func(T)]` for `struct{ P *T }`), is decided as the plain `*Box[Payment]` and `Ptr[[]Payment]` are: an error for an abstract argument, on every route (a use of `T`, [GEN-12](10-generics.md#102-rules)). `Ptr[T]` and `Ptr[*T]` hold no value of `T`. A literal `Payment{…}`, `NewUnique(Payment{…})` and a conversion `Payment(x)` are errors too. `*Payment`, `Unique[Payment]`, `Shared[Payment]` and `Weak[Payment]` are fine (because `Unique`, `Shared` and `Weak` hold `Payment` behind a heap pointer, not inline by value). The one exception is a parent part inside a child's composite literal: `Card{Payment: Payment{…}}` is allowed even when `Payment` is abstract, because that part is not a standalone value.
- It is usable only through pointers to a concrete child.
- A child that leaves an `@Abstract` method unimplemented is an error unless the child is itself marked `@Abstract` (a type attribute, [MAC-8](13-macros-surface.md#132-rules)): `@Abstract @Extends(Animal) type Bird struct{}`. `@Abstract` as a type attribute on a type without `@Extends` is an error.

**TYP-20.** *`base` and `base[Parent]`.* Inside a method of a child type:
- `base.M(…)` calls the direct parent's version of `M` statically (no dynamic dispatch), for any parent method, final ones included, when **exactly one** direct parent has `M`. If two or more direct parents have `M`, unqualified `base.M(…)` is a compile error naming the parents; write `base[Parent].M(…)`.
- `base[Parent].M(…)` (and `base[Parent].f`), where `Parent` names one of the child's direct parents, as written in its `@Extends` attributes (including type arguments for a generic parent, such as `base[Box[T]].Reset()`), selects that direct parent statically.
- It is an error when `M` is `@Abstract` in the selected parent (no body).
- `base` is contextual: it has this meaning only inside methods (and function literals within methods, which capture the receiver) of a type with parents. `base.f` and `base[Parent].f` read a field of that parent part; neither `base` nor `base[Parent]` alone is a value (`x := base`, `x := base[Payment]` and `&base` are errors). Declaring a local, parameter, result or receiver named `base` there is an error with a rename hint. Elsewhere `base` is an ordinary identifier ([LEX-12](02-lexical.md#lex-12)).

**TYP-21.** *Up- and downcast.* Rules in [EXP-13 to EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023). In short: when an ancestor `P` has a type table (TYP-22), `*Child` converts implicitly to `*P` (adjusting by the constant subobject offset of `P` inside `Child`), and `Unique`, `Shared` and `Weak` keep the conversion while staying 1 word (8 bytes); without a type table on `P` there is no upcast of any kind to `P` (ADR-0023 decision 16). Downcast is a checked assertion `p.(*Card)` or a type switch, and needs the type table too. Copying a `Child` into a `Parent` value (value slicing) is an error, and so is a value use of a place that may hold a subtype: for a type with a type table, `*p` and a value receiver ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023)).

**TYP-22.** *Cost and layout.*
- Parent subobjects are laid out first in `@Extends` order (`P1`, `P2`, …), followed by the child's own fields, with standard C alignment ([TYP-12](#33-layout-d117-d122)).
- **Primary type table:** a type needs a type table when it declares or inherits a `@Virtual` or `@Abstract` method, or has a parent that has a type table. If `P1` has a type table, `P1` sits at offset `0` and its first word is shared as the child's primary type-table pointer (no extra word is added). Otherwise, when a table is needed, one primary type-table pointer word is stored at offset `0`, in front of `P1`.
- **Secondary parents:** an untagged parent (`Timestamps`, no `@Virtual` or `@Abstract` in its own hierarchy) adds **zero** table words—only its fields. A tagged secondary parent (`Auditable`, `i >= 2`) already begins with its own type-table pointer word inside its subobject, which the child initializes to the `(Child, Pi)` secondary type table.
- **`offset_to_top`:** every type table carries a signed byte offset `offset_to_top` (`0` for standalone objects and primary bases at offset `0`, `-offset(Pi)` for secondary base subobjects). Virtual overrides on secondary bases adjust the receiver back to `*Child` via a compiler-generated thunk, and `Unique[P]` drop, `Shared[P]` / `Weak[P]` retain and release (finding the refcount header before `top = (byte*)p + p.vptr.offset_to_top`), dynamic `Clone()`, and downcasts all recover the complete object start `top` in one addition, keeping `Unique[T]`, `Shared[T]` and `Weak[T]` at 1 word (8 bytes).
- Calls through a parent pointer to virtual methods are indirect. Every other call is direct.
- Untagged parents (no `@Virtual` or `@Abstract` method anywhere in that parent's own hierarchy) have zero table overhead and can be extended freely. Their limit is a compile error: there is no upcast of any kind to a parent without a type table (ADR-0023 decision 16), neither `*Child` to `*Parent` (so outside the parent's own methods and what they return a parent pointer never points at a child, and a copy through it (`*p`) is exact; a parent method that returns its receiver hands out the parent part, a valid parent value, as `my.Dup()` does), nor the owning forms `Unique`, `Shared` and `Weak`, nor a method expression of the parent called with a child; and a downcast `p.(*Child)` or a type switch is an error, because there is no dynamic type. What decides is the table of the TARGET parent: a child that has a table still has no upcast to a table-less parent (`Timestamps`). The hint names both ways out: add a `@Virtual` method to the parent, or use `*Child`. Inherited methods, method values, `base.M()`, `base[Parent].M()`, fields and interfaces work as before: inside a method of the parent, the receiver is that parent's subobject in the child. When the target parent has a type table, every upcast and the downcast are allowed.
- *Exception: a C++ parent.* When `P1` is a C++ class, its C++ vtable (inside the C++ part at offset `0`) is the primary type table; no extra word is stored in front of `P1`, and a polymorphic C++ class counts as having a table. Details are in [TYP-28](#35-inheritance-adr-0023).

**TYP-23.** *Interfaces.* A child satisfies every interface its method set satisfies. The method set includes inherited methods from all parents. A call through an interface uses the overrides.

**TYP-24.** *Safety interplay.* Each of these is also stated where the rule lives:
- ownership: drop order is the child's `drop` method (if any), then the child's own fields in reverse declaration order, then the parent subobjects in **reverse** `@Extends` order (`Pn` down to `P1`) ([OWN-R2](04-ownership.md#42-ownership-rules-d14)); moves are of the whole value ([OWN-R3](04-ownership.md#42-ownership-rules-d14));
- copies and `Clone`: a copy covers all parents' and the child's fields; an abstract type cannot be copied by value; a copy is always of the whole real value: through a pointer or a `Unique` of any parent `P` with a type table it is written `p.Clone()` and gives a `Unique[P]` holding a full copy of the dynamic type ([OWN-R6](04-ownership.md#42-ownership-rules-d14), ADR-0023 decision 16);
- borrow checking: an upcast pointer to any tagged ancestor is the same borrow ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021), ADR-0021);
- `Send` / `Sync`: a child is labeled from all its fields, all parents' fields included ([CON-12](12-concurrency.md), ADR-0022);
- `@Required`: [TYP-17](#35-inheritance-adr-0023);
- generics: an interface with `@Extends(Parent)` is a constraint, and several `@Extends` on it require all the parents ([GEN-12](10-generics.md#102-rules), ADR-0023 decision 15, ADR-0034); inside the body `*T` converts to `*Parent` (when `Parent` has a table) and the parent's methods and fields are available;
- C++ parent: [TYP-28](#35-inheritance-adr-0023).

**TYP-25.** *No struct embedding.* In a `.gopp` file an embedded field (an anonymous field of a struct type: `struct{ Animal; … }`, `struct{ *Animal }`, `struct{ sync.Mutex }`) is a compile error. The fix-it says: use `@Extends` to inherit from a struct (`@Extends(Payment)` before `type Card struct { … }`), or a named field for plain composition (`Lock Mutex[State]`, `Log Logger`). Promoted fields and methods go away with embedding: the method set of a struct never includes methods of its named fields. Interface embedding stays (`interface{ io.Reader; io.Writer }`, a constraint in a constraint interface): it composes type sets and is not inheritance.

**TYP-26.** *Go mode.* A `.go` file has no attributes, so it has no `@Extends` and no method marks; `base` is not special there, and struct embedding works as in Go.

**TYP-28.** *Extending a C++ class* (ADR-0023 decision 14; task P08.16).
```go
import "<QtWidgets>"
@Extends(QtWidgets.QPushButton)
@Extends(Timestamps)
type MyButton struct { clicks int }

@Override
func (b *MyButton) mousePressEvent(e *QtWidgets.QMouseEvent) {
    b.clicks++
    b.Touch()
    base[QtWidgets.QPushButton].mousePressEvent(e)
}
```
- A Go++ struct may have at most **one** C++ class as a parent, and it must be the **first** `@Extends` (`P1`), followed by any number of Go++ struct parents (`P2, P3, …`; not a C++ template instance, not a second C++ base, no C++ virtual inheritance in v1). All of TYP-16 to TYP-24 apply with the changes below.
- *Upcast.* When the C++ parent is polymorphic (it has a `virtual` method, so it has a C++ vtable), a borrowed `*MyButton` converts to `*QtWidgets.QPushButton` (at offset `0`), and for the owning forms (`Unique`, `Shared`, `Weak`) it also needs a virtual destructor. A non-polymorphic C++ parent follows the untagged parent rules of [TYP-22](#35-inheritance-adr-0023) (no upcast, no downcast; its fields and methods stay available on the child).
- *Marks.* `@Override` replaces a C++ `virtual` method (the C++ `virtual` counts as `@Virtual`). A non-virtual method, or one marked `final` in C++, cannot be overridden (error). A C++ pure virtual method counts as `@Abstract`: it must be overridden, or the Go++ type is marked `@Abstract`. The signature must match exactly; an override cannot have `!`.
- *`base.M()` and `base[CppBase].M()`* call the C++ class's own implementation, statically.
- *Layout.* The C++ object comes first at offset `0` (with its vtable pointer, if it has one), then any Go++ struct parents `P2, P3, …`, then the child's Go++ fields. The C++ vtable is the primary type table: no extra word is stored in front of the C++ part. The compiler generates a C++ subclass shim through Clang, so C++ code that calls the virtual method reaches the Go++ override. Adding a new `@Virtual` or `@Abstract` method (or a tagged secondary parent) directly below a non-polymorphic C++ class is an error (it would need a primary table pointer at offset `0` that the C++ part does not have).
- *Construction* calls a C++ constructor through a composite literal or a constructor call; the syntax is decided in P08.16. A C++ base with no default constructor gives the type no zero value ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).
- *Destruction.* The child's `drop` runs first, then its Go++ fields in reverse order, then Go++ parents `Pn .. P2` in reverse order, then the C++ destructor of `P1` ([OWN-R2](04-ownership.md#42-ownership-rules-d14)).
- *Safety.* On the Go++ side the type follows the Go++ rules. C++ code holding raw pointers to such an object is outside the checks (UB-9, UB-10, [chapter 16](16-undefined-behavior.md)).
- *Not in v1:* more than one C++ base, C++ virtual inheritance, a C++ class template as the parent.


```go
type Payment struct { Amount int64; ID string }

@Abstract
func (p *Payment) Pay() error

@Virtual
func (p *Payment) Fee() int64 { return 0 }

func (p *Payment) Total() int64 { return p.Amount + p.Fee() }   // final

type Auditable struct { Actor string }

@Virtual
func (a *Auditable) Audit() string { return "by " + a.Actor }

type Timestamps struct { CreatedAt int64 }                      // untagged: 0 extra words
func (t *Timestamps) Age(now int64) int64 { return now - t.CreatedAt }

@Extends(Payment)
@Extends(Auditable)
@Extends(Timestamps)
type Card struct {
    Number string
}

@Override
func (c *Card) Pay() error { return nil }
@Override
func (c *Card) Fee() int64 { return c.Amount * 3 / 100 }
@Override
func (c *Card) Audit() string { return base[Auditable].Audit() + " #" + c.Number }

@Extends(Card)
type PremiumCard struct{}

@Override
func (p *PremiumCard) Fee() int64 { return base.Fee() / 2 }      // Card's version (unambiguous)

c := Card{Amount: 100, Actor: "alice", CreatedAt: 10, Number: "4242"}          // ok: ancestor fields set directly
d := Card{Payment: Payment{Amount: 100}, Auditable: Auditable{Actor: "bob"}, Timestamps: Timestamps{CreatedAt: 10}, Number: "1"}
e := Card{Amount: 1, Payment: Payment{Amount: 2}, Actor: "x", Number: "1"}     // error: Amount set twice
var p Payment                                                                  // error: Payment is abstract
type Z struct{ Payment }                                                       // error: struct embedding is removed; use @Extends or a named field
type R interface{ io.Reader; io.Writer }                                       // ok: interface embedding stays
```

The full example, its output and the list of errors are in [ADR-0023](../adr/0023-inheritance.md).

## 3.6 Rationale

- **Interface values that own what they hold** ([ADR-0041](../adr/0041-values-are-copied.md)) keep the one rule that `b := a` is a copy. Go's `err == io.EOF`, `errors.Is`, `%w` and type switches keep working with no GC ([ERR-12](09-errors-and-panics.md)).
- **Borrowed interface parameters** make `fmt.Println(name)` free and non-consuming. Boxing happens only when a callee really keeps the value.
- **Several parents without diamonds instead of struct embedding** ([ADR-0023](../adr/0023-inheritance.md)): a struct can combine an OOP base and stateful struct mixins (`@Extends(Payment)`, `@Extends(Auditable)`, `@Extends(Timestamps)` before `type Card struct`), parent methods see child `@Override`s, diamond inheritance is rejected at compile time, untagged parents add zero words, and `offset_to_top` in type tables keeps `Unique[T]`, `Shared[T]` and `Weak[T]` at 8 bytes. Embedding would be a second, non-polymorphic way to reuse structs, so it is removed from `.gopp` files.
- **Rust-style named `@Operator` methods** ([ADR-0024](../adr/0024-operator-overloading.md)): normal Go identifier names (`Add`, `Sub`, `Mul`, `Eq`, `Cmp`, `Index`, `SetIndex`, `Iter`) keep method declarations and interface constraints clean, allow calling `a.Add(b)` directly as well as `a + b`, and avoid accidental overloading of unrelated `.Add(...)` methods unless marked `@Operator`.
- **Field tables instead of `reflect`** give `fmt` and `json` what they need and nothing more.
- **C layout** is required for C interop (P07) and costs nothing in practice.
- **`@Required` as an opt-in field attribute**, not required-by-default: Go code and partial literals such as `http.Server{Addr: ":80"}` keep working, `Option` only ever means `Option[T]`, adding an optional field to a library struct breaks nobody, and no new keyword is needed (ADR-0016).
- **Rejected:** boxing every interface argument (hidden allocation); moving into every interface parameter (breaks `fmt.Println(x)` followed by a use of `x`).
