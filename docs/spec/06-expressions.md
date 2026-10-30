# 6. Expressions

Base: Go spec [Expressions](https://go.dev/ref/spec#Expressions), [Operands](https://go.dev/ref/spec#Operands), [Composite literals](https://go.dev/ref/spec#Composite_literals), [Function literals](https://go.dev/ref/spec#Function_literals), [Primary expressions](https://go.dev/ref/spec#Primary_expressions), [Method values](https://go.dev/ref/spec#Method_values), [Index expressions](https://go.dev/ref/spec#Index_expressions), [Slice expressions](https://go.dev/ref/spec#Slice_expressions), [Calls](https://go.dev/ref/spec#Calls), [Operators](https://go.dev/ref/spec#Operators), [Arithmetic operators](https://go.dev/ref/spec#Arithmetic_operators), [Integer overflow](https://go.dev/ref/spec#Integer_overflow), [String concatenation](https://go.dev/ref/spec#String_concatenation), [Comparison operators](https://go.dev/ref/spec#Comparison_operators), [Conversions](https://go.dev/ref/spec#Conversions), [Order of evaluation](https://go.dev/ref/spec#Order_of_evaluation), [Built-in functions](https://go.dev/ref/spec#Built-in_functions), [Min and max](https://go.dev/ref/spec#Min_and_max).

Most expressions are unchanged. This chapter lists the new built-ins, the places where ownership matters, and the safety defaults of D1.17.

## 6.1 Syntax

The expression grammar of Go is unchanged except for two additions, each defined in one place:
- the unwrapping call `PrimaryExpr "!" Arguments` (`f!(x)`, `os.Open!(p).Close!()`), in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18) ([ERR-5](09-errors-and-panics.md));
- macro invocations as an `Operand` (`#Name(args)`), in [13.1](13-macros-surface.md) ([MAC-2](13-macros-surface.md)).

Unit literals add one form, a number literal followed by `[UnitExpr]` (`100.0[KM]`, [EXP-31](#612-unit-expressions-adr-0025)); the unit grammar is in [17.1](17-units.md#171-syntax).

Operator methods add no expression grammar ([6.10](#610-operator-methods-adr-0024)): `a + b`, `m[i, j]` (an index list is already valid Go syntax), `q <- v` and `<-q` are the same forms as before. The only new token is `<=>` ([LEX-13](02-lexical.md#new-tokens)).

## 6.2 New built-in functions

**EXP-1.** Predeclared functions and their typing:

| Call | Type | Meaning |
|---|---|---|
| `move(p)` | type of `p`; `p` must be a place (variable, field, element, map entry, pointer target) | take the value out, leave the zero value ([OWN-R4](04-ownership.md#42-ownership-rules-d14)); for a type with no zero value only from a local, or from a map as `v, ok := move(m[k])` (or a bare `move(m[k])` statement) |
| `clone(x)` | type of `x`; `x`'s type must satisfy `Clone` | short form of `x.Clone()`: copy ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). On a raw `*T` whose `T` has a value-receiver `Clone`, `clone(p)` is the same ambiguity error as `p.Clone()`; `[T Clone]` instantiation is the only place where `*T` uses the pointer copy |
| `new_unique(v)` | `Unique[*T]` where `v` has type `T` | allocate, move `v` in |
| `new_shared(v)` | `Shared[*T]` | allocate with counts, move `v` in |
| `new_weak(s)` | `Weak[*T]` where `s` is a `Shared[*T]` | make a weak reference |

- `move` and `clone` are not first-class values: they can only be called, not stored or passed (as `len` in Go).
- `x.Clone()` is the form used in examples. Every cloneable type has the method, built-in types too ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). `move` has no method form.
- `move(m[k])` removes the entry; `v, ok := move(m[k])` and `v, ok := clone(m[k])` are the comma-ok forms ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). For a value type with no zero value only the comma-ok forms may produce a value ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).
- `min`/`max` on views return a view (EXP-5).
- Go's `new` does not exist ([LEX-8](02-lexical.md)).

**EXP-2.** *Moving expressions.* An expression that gives ownership of a value (a call result, a conversion that copies, a composite literal, a `move(…)` or `x.Clone()`) produces an owned **temporary**. A temporary used where ownership is needed moves; otherwise it is destroyed at the end of the full statement ([OWN-R2](04-ownership.md#42-ownership-rules-d14)).

- **Parts of a temporary.** A field or element of an owned temporary is also a temporary. It moves out, and the rest of the temporary is destroyed at the end of the full statement. This holds for a struct field, an array element, an element of an owned slice or map, and a part reached through a `Unique[*T]` the temporary owns.
- **Exceptions.** Through a raw pointer, a `Shared[*T]` or a view, a part is a place in someone else's storage. To take it, write `x.Clone()` or `move(x)` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). A view into a part of a `Shared[*T]` temporary is still a view into a temporary: the handle may release the last strong reference at the end of the statement. Storing such a view longer than the statement is an error ([OWN-V6](04-ownership.md#44-views-d16)).
- **Conversions that share.** These are not new values. They are checked as their operand, so `string(p.Name)` needs `p.Name.Clone()` or `move(p.Name)`. Only conversions between owning types can share a buffer; a number or another trivial value shares nothing (a `@Pinned` value is never copied, [OWN-R10](04-ownership.md#42-ownership-rules-d14)). The cases are: the same underlying type (struct tags are ignored); a conversion to an interface; a conversion to or from a type parameter, unless every type in its type set copies.
- **Slice to array.** `[N]T(s)` with owning elements does not share storage: it duplicates the elements bitwise, which would leave two owners. So it is checked as its operand too, and needs `s.Clone()` or `move(s)`. With trivial elements it is a plain copy.
- **Conversions that copy.** `string(bytes)`, `[]byte(s)`, a view to an owned type, and a number to a number. Each gives a temporary.

**EXP-3.** *Order of evaluation.* As in Go ([Order of evaluation](https://go.dev/ref/spec#Order_of_evaluation)), with one addition: moves out of places (`move(p)`, the implicit move of `p = append(p, …)`) happen **after** all other arguments of the call are evaluated.

```go
x.items = append(x.items, len(x.items))   // len(x.items) is evaluated before x.items moves
```

## 6.3 Operators on owning values

**EXP-4.** Operands of `==`, `<`, `+` and other operators on owning types are **borrowed**. The same holds when the operator is a user method ([EXP-18](#610-operator-methods-adr-0024)). `a + b` on strings allocates a new owned temporary. `+=` grows the left operand in place.

**EXP-5.** Built-in `min` and `max` on owned strings return a copy ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). On views they return a view.

```go
s := "a" + name        // new owned string; name is only borrowed
t := min(s, name)      // a copy of the smaller one
```

## 6.4 Safety defaults (D1.17)

These follow Go.

**EXP-6.** *Integer overflow wraps.* See [Integer overflow](https://go.dev/ref/spec#Integer_overflow). `int` is 64-bit on 64-bit targets.

**EXP-7.** *Checks are on.* Bounds checks and nil checks are always on. Nil checks apply to channels, `error` and `any` (empty-interface) values. A nil map is empty and a write creates it, so map writes need no nil check. A write into a temporary map or into a map inside an interface is a compile error, so there is no run-time nil-map panic ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). Pointers, funcs and interfaces with methods are never nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)), so their uses need none. The checks can be turned off with `@NoBounds` (per function) or a per-package flag. Integer division by zero panics ([Run-time panics](https://go.dev/ref/spec#Run_time_panics)).

**EXP-8.** *Operations Go defines but LLVM does not.* Go++ gives them Go's results (P04 D4.12 lowers them). None of them is UB.

| Operation | Result |
|---|---|
| `MinInt / -1` | `MinInt` (no trap) |
| `MinInt % -1` | `0` (no trap) |
| shift by ≥ the bit width: `x << n`, `x >> n` | `0`; for `>>` of a negative signed value: `-1` |
| out-of-range float to integer conversion | saturates (the value is implementation-defined, never UB) |
| floating-point fusing (FMA) | follows Go's compiler on each target |

```go
var m int64 = math.MinInt64
fmt.Println(m / -1, m % -1)    // -9223372036854775808 0
var s uint = 70
fmt.Println(uint32(1) << s)    // 0
fmt.Println(int8(-8) >> s)     // -1
```

## 6.5 Conversions

**EXP-9.** Conversions follow [Conversions](https://go.dev/ref/spec#Conversions) with these additions:
- `string(v)` where `v` is a `View[string]`, and `[]T(v)` where `v` is a `View[[]T]`: copy into a new owned value. `[]T(v)` needs elements that satisfy `Clone` ([OWN-V3](04-ownership.md#44-views-d16)).
- `string([]byte)` and `[]byte(string)` copy, as in Go.
- Between struct types, the required bit of each field must match too, as the field names and types do ([TYP-15a](03-types.md)): each field is compared as "required" when its effective bit (by `@Required` or `@Strict`) is set, or when its type definitely has no zero value in every instance (a pointer, a func, a method interface, a struct with a required field, a C++ type without a default constructor). A field that is never nil on one side ([TYP-15](03-types.md#34-required-fields-d14-rule-11): `@Required` on `error`, `any`, a map or a channel) must be never nil on the other side too, so a value that may hold nil (the set-only field of an instance such as `SG[error]`) never reaches a never-nil field. The two sides must agree for every field, so `U(Raw{…})` is an error if `U` and `Raw` differ in this for any field, in either direction. A field whose type is a type parameter counts as required only through its bit, because an instance may have a zero value (GEN-5): `GenR[T](gen)` from `Gen[T]{A T}` to `GenR[T]{A T @Required}` is an error. A `@Strict` struct and a non-strict struct with the same fields convert into each other only if they agree for every field. (Assignment is stricter: type identity includes the attributes, [TYP-15a](03-types.md#34-required-fields-d14-rule-11).) Conversions between struct types do not turn a `Child` into a `Parent` (value slicing, [EXP-15](#69-upcasts-and-downcasts-adr-0023)).
- `R(a)` where the type of `a` has a method marked `@Convert` with result `R`: the user conversion [EXP-25](#610-operator-methods-adr-0024). It is explicit only.
- `N[U](x)` and `N(x)` where `N` is a numeric type and `U` a unit expression: give a value a unit, change its unit, or remove it ([EXP-32](#612-unit-expressions-adr-0025)).
- Owned to view is implicit, never written as a conversion.
- A `Unique[*T]` or `Shared[*T]` stored in a place converts implicitly to `*T`; from a temporary it is an error ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17)).

## 6.6 Composite literals and `&`

**EXP-10.** Composite literals work as in Go. Their fields are moved in: `T{F: a}` moves an owned local `a` ([OWN-R3](04-ownership.md#42-ownership-rules-d14)).

**EXP-11.** `&T{…}` is a compile error ([OWN-P4](04-ownership.md#43-pointers-and-heap-allocation-d17)). `&x` on a variable is allowed and gives a raw pointer. `&local` must not escape the function (compile error if returned).

## 6.7 Method values and function literals

**EXP-12.** A function literal is a closure; see [chapter 11](11-closures.md). A function literal declared with `!` is written `func!(x int) T { … }` ([ERR-2](09-errors-and-panics.md)). A method value is a closure over its receiver ([CLO-5](11-closures.md)).

## 6.8 Examples

```go
buf := make([]byte, 0, 16)
buf = append(buf, 'a')                    // implicit move (OWN-R4)
n := 7
s := strconv.Itoa(n) + "!"                // owned temporary + literal -> new string

u := new_unique(Config{Name: "x"})
c := u.Clone()                            // a new Unique holding a copy (needs a cloneable Config)
p := u                                    // u moves into p; u is unusable until assigned again
```

## 6.9 Upcasts and downcasts (ADR-0023)

**EXP-13.** *Upcast.* A `*Child` converts implicitly to a `*Parent`, or to any ancestor pointer, and to the pointer kinds: `Unique[*Child]` to `Unique[*Parent]`, and the same for `Shared` and `Weak`; `View[Child]` converts to `View[Parent]` (read-only, [OWN-V3](04-ownership.md#44-views-d16)). The conversion changes only the static type (free, [TYP-22](03-types.md#35-single-inheritance-adr-0023)) and keeps the same borrow ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)); an owning pointer moves as before, and a `Unique[*Child]` or `Shared[*Child]` held in a place lends a `*Parent`. The reverse (`*Parent` to `*Child`) is never implicit. Every upcast needs a type table in the target parent ([TYP-22](03-types.md#35-single-inheritance-adr-0023), ADR-0023 decision 16): without one there is no upcast of any kind, borrowed, `View` or owning, and the explicit forms `&c.Parent` and a parent's method expression called with a child are errors too; the hint is to add a `@Virtual` method to the parent or to use `*Child`. So a pointer of a type without a table never points at a child outside the parent's own methods and what they return (a parent method sees only the parent part, and may return it, like `my.Dup()`: a valid value of the parent). For a C++ parent the C++ vtable is the type table ([TYP-28](03-types.md#35-single-inheritance-adr-0023)); in a generic body `*T` with `T extends P` converts to `*P` by the same rule ([GEN-12](10-generics.md#102-rules)).

**EXP-14.** *Dispatch.* A call through a parent pointer to a `@Virtual` or `@Abstract` method runs the override of the dynamic type (indirect call through the type table). A call to a final method is direct, and `base.M()` is direct ([TYP-20](03-types.md#35-single-inheritance-adr-0023)). `p.Clone()` (and `clone(p)`) on a `*P`, a `View[P]` or a `Unique[*P]` of a type with a type table also goes through the table: it is a `Unique[*P]` that holds a full copy of the dynamic type, so the overrides keep working on the copy ([OWN-R6](04-ownership.md#42-ownership-rules-d14), ADR-0023 decision 16).

**EXP-15.** *Downcast and value slicing.* Downcast is a checked type assertion on a pointer, allowed only when the parent has a type table (otherwise it is an error: there is no dynamic type): `c := p.(*Card)` panics when the dynamic type is not `*Card` or a child of `Card`; `c, ok := p.(*Card)` gives `ok = false` instead. `p` may be a `*Parent` or an owning pointer kind held in a place; the result is a `*Card` that borrows from it ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17)). A type switch on such a `p` is a series of downcasts: each case is a pointer to a child (`case *Card:`), with the same table rule; the target of a downcast must be a pointer to a type that extends the parent. Value slicing is a compile error: `var a Parent = child`, `a = child`, passing a `Child` value where a `Parent` value is expected, a `Child` value in a `Parent` composite literal field or `append` to `[]Parent`. Use a pointer. Conversions between struct types ([EXP-9](#65-conversions)) do not turn a `Child` into a `Parent`.

The places that may hold only the parent part of a bigger value are decided by one rule: the hidden parent part `c.Payment` of a child, and, for a type with a type table (ADR-0023 decision 16), `*p` of any pointer kind and a value receiver (`func (a Animal) M()`), whose dynamic object may be a child. Such a place is used only to reach a field or call a method (`c.Payment.Amount`, `c.Payment.Fee()`, `(*p).Name`, `a.Speak()`) or under `&` (`&*p`; `&c.Payment` is the upcast pointer and needs a type table, EXP-13); the `Payment: Payment{…}` part of a child literal is fine. Every other use is an error: a copy (`x := c.Payment`, `x := *p`), `move(c.Payment)`, an argument, a result, a composite literal element, `ch <- *p`, `m[k] = *p`, `append(s, *p)`, an `Option` made from it, a deferred argument, an assignment as a whole (`c.Payment = v`, `*p = v`, `*q = *p`), and a clone of it as a value (`(*p).Clone()`, `a.Clone()`; clone through the pointer instead, EXP-14). For a type without a type table `*p` is a whole value of that type (outside the parent's own methods and what they return a pointer of it never points at a child, EXP-13; what they return is the parent part, a valid value of the parent).

```go
var c Unique[*Card] = new_unique(Card{Amount: 10, Number: "4242"})
var p Unique[*Payment] = c           // ok: upcast, c moves
q, ok := p.(*Card)                   // ok: checked downcast
var v Payment = *q                   // error: value slicing; use the pointer
```

## 6.10 Operator methods (ADR-0024)

Go++ lets a type define what an operator does, by a method whose name is the operator (grammar: [TYP-27](03-types.md#31-type-syntax)). The unsafe operators are blocked ([EXP-29](#610-operator-methods-adr-0024)). All safety rules apply to these methods as to every method.

**EXP-16.** *Forms.* The form of an operator method is fixed by its name and its parameter count. `R` is any result type, `U`, `V` and `I` any parameter types.

| Operator | Declared as (after the receiver) | Receiver | Used as |
|---|---|---|---|
| `+ - * / % & \| ^ &^ << >>` | `(b U) R` | value | `a + b` |
| `- + ^ !` | `() R` | value | `-a` |
| `==` | `(b U) bool` | value | `a == b`, `a != b` |
| `<=>` | `(b U) int` | value | `a <=> b`, `a < b`, `a <= b`, `a > b`, `a >= b` |
| `&&` `\|\|` | `(b U) R` or `(b func() U) R` | value | `a && b` |
| `[]` | `(i I, …) R` | value or pointer | `a[i]`, `a[i, j]` |
| `[]=` | `(i I, …, v V)` | pointer | `a[i] = v` |
| `()` | any parameters, any results | value or pointer | `a(x, y)` |
| `<-` (send) | `(v V)` | pointer | `a <- v` |
| `<-` (receive) | `() R` or `() (R, bool)` | pointer | `<-a`, `v, ok := <-a` |
| `++` `--` | `()` | pointer | `a++`, `a--` |
| `+= -= *= /= %= &= \|= ^= <<= >>= &^=` (optional) | `(b U)`, no result | pointer | `a += b` |
| `*` (unary) | `() View[T]` | value | `*a` |
| `@Convert` method (any name) | `() R` | value | `R(a)` |
| `range` | `() F`, where `F` is a function type that `range` accepts | value or pointer | `for x := range a` |

- The shape is checked at the declaration: the wrong number of parameters, a wrong result (`==` must return `bool`, `<=>` must return `int`), or the wrong receiver kind is a compile error that names the table row.
- A **value receiver** of a non-trivial type is a read-only borrow, like every value parameter ([PAR-4](05-parameters.md)). A **pointer receiver** is required where the operator changes the operand; the operand must then be addressable and writable (a variable, field or element that is not behind a read-only borrow). A pure operator with a pointer receiver is an error, because `a + b` would need `&a` and `a` may be a temporary.
- The operands of `<-` and `[]=` that the method changes are the receiver only. The other operands are borrowed or moved in like arguments of any call ([PAR-4](05-parameters.md), [OWN-R3](04-ownership.md#42-ownership-rules-d14)): the value `v` of a send or of `[]=` moves in when its type owns data.

**EXP-17.** *Overload sets and resolution.* The methods with one operator name on one type form an overload set. For an operator expression the checker finds the set of the **static type of the left operand** (or the receiver, for unary forms), then picks the method like this:
1. The right operand's type must be **identical** to the parameter type. There are no implicit conversions and no ranking.
2. An untyped constant picks the method whose parameter type is the constant's default type (`2` is `int`, `2.0` is `float64`). If none has it, the only method that can represent the constant is used. If several can, the call is ambiguous and is an error.
3. For the call `a(x, y)` and for `[]` the same rules run over all arguments.
4. For `@Convert` only the wanted result type counts (one method per target type).
5. If the left operand has no operator method of that name, the built-in meaning is used when the operand types have one; otherwise the error says that `T` has no operator `+` for `U`. When the left operand is an untyped constant and only the right operand's type has the operator (without `@Right`), the error says that the methods take that type as the left operand (`3 < x`: write a `@Right` method for the swapped order, [EXP-26](#610-operator-methods-adr-0024)).
6. A defined type over a number or string keeps its built-in operators next to methods for other right-hand types. `Meters * Meters` and `10 * Km` stay built in when `Meters` also has `* (Seconds) Speed`: a method is chosen for an untyped constant only by an exact match (the constant's default type), never by the weaker "can represent" match when the built-in operator takes the constant as a `Meters`. `a == i` with an interface operand `i` is the built-in comparison with the dynamic value, as `i == a` is. The cases of a `switch` compare with the `==` method of the tag's type as `tag == case` does.
7. An operand of a pointer type `*T`: the index `[]` and `[]=`, the call `()`, `<-` and `range` look through the pointer, like the method call `p.M()` (the method set of `*T` has the methods of both receiver kinds, and these forms have no built-in meaning on a pointer to a defined type), so `p[i]`, `p(x)`, `<-q`, `q <- v` and `for x := range t` work for a `*Matrix`, `*Queue` or `*Tree`. Every other operator keeps the pointer's own meaning: `p == q` compares the pointers, `*p` dereferences, `p + q`, `-p`, `p++` and `p += v` are errors. A constraint element is met by a pointer type by the same rule ([GEN-13](10-generics.md#102-rules)).

A **second meaning is an error**: an operator method may not be declared when the type already has that meaning from its underlying type. The checker rejects it at the declaration with the reason:
- an operator with the same parameter type that the built-in operator already has (`type Meters float64` with `+ (Meters) Meters`; `Meters * float64` is fine because the built-in needs two `Meters`);
- `[]` or `[]=` on a type whose underlying type is an array, slice, string, map or pointer to array;
- `()` on a func type, `<-` on a channel type, `range` on a type that ranges already (slice, array, string, map, channel, integer, func), unary `*` on a pointer kind;
- `@Convert` with a result type that a built-in conversion already gives ([EXP-9](#65-conversions));
- any operator method on a unit type, including a named one such as `type Distance float64[M]`: its operators are the built-in unit rules ([UNI-15](17-units.md#172-rules), [6.12](#612-unit-expressions-adr-0025)). (A generic type with a unit parameter, `Vec2[U unit]`, is a struct and may have operator methods.)

Two methods with the same operator and the same parameter types are a duplicate-method error. (Exception: for an operator that is not commutative, one method with `@Right` and one without may have the same parameter types, because they serve different orders, [EXP-26](#610-operator-methods-adr-0024).) With `@Right` ([EXP-26](#610-operator-methods-adr-0024)) or with inherited methods ([EXP-30](#610-operator-methods-adr-0024)) a clash can appear only at a use, in a package that sees both; that is an ambiguity error at the use.

**EXP-18.** *Ownership and safety.*
- The operands are **borrowed** and the result is a new owned **temporary** ([EXP-2](#62-new-built-in-functions), [EXP-4](#63-operators-on-owning-values)). `a + b` does not move `a` or `b`. Evaluation order is as in Go ([EXP-3](#62-new-built-in-functions)): left operand, right operand, then the call. The one exception is the lazy form of `&&` and `||` ([EXP-21](#610-operator-methods-adr-0024)).
- The call is checked exactly like the method call `a.op(b)`: borrow checking with borrow summaries ([OWN-B](04-ownership.md#45-borrow-checking-adr-0021)), views ([OWN-V](04-ownership.md#44-views-d16)), never-nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)), `Send` and `Sync` ([CON-12](12-concurrency.md)). A result may borrow from an operand only if its type is a view or a pointer, as for any method.
- **View operands** follow [OWN-V1](04-ownership.md#44-views-d16). A value read out of a view is copied out when its type is Copy and has no pointer kind, so `*a + b` (with `*a` a `View[Vec2]`) works for such a `Vec2`. A **non-Copy** view operand, for example a `View[BigInt]`, cannot be passed to an operator method: its value receiver is a non-view parameter, and a view never converts to one. That is a compile error, the same as for any method, until the P04.09 "writes this parameter" relaxation ([OWN-V1](04-ownership.md#44-views-d16)); then a method that never writes its receiver or parameter will accept it.
- The method body is ordinary code. It may allocate (`BigInt`, `Matrix`), may panic, and may call other operators. It cannot be a `!` function ([EXP-30](#610-operator-methods-adr-0024)).

**EXP-19.** *Compound forms, `++` and `--`.*
- `x op= y` (`+= -= *= /= %= &= |= ^= <<= >>= &^=`) has two meanings, and `x` is evaluated **once** in both (an index expression inside `x`, or a call, runs once):
  - **In-place method (optional).** If the type of `x` has a method named like the compound operator, with a pointer receiver, one parameter and **no result** (`func (m *Matrix) += (b Matrix)`), `x op= y` calls it. It changes `x` in place and allocates nothing. `x` must be addressable and writable. Overloads are picked by the parameter type ([EXP-17](#610-operator-methods-adr-0024)).
  - **Otherwise** `x op= y` means `x = x op y`. The binary method must exist and its result type must be assignable to `x`. The old value of `x` is dropped by the assignment ([OWN-R2](04-ownership.md#42-ownership-rules-d14)).
  - The in-place method is chosen only when `x` is a place of that type. In `a[i] += v` where `a` has the `[]` method the element is not a place ([EXP-22](#610-operator-methods-adr-0024)), so it always means `a[i] = a[i] op v`. The element of a built-in slice or array is a place: `s[0] += v` takes the in-place method of the element type if it has one, else it means `s[0] = s[0] op v`, exactly as `x += v` does (`s[0]++` and `s[0] <- v` look through the element the same way); a map element is no place, so it always means `m[k] = m[k] op v`.
  - **Overlapping operands are an error.** The in-place method has a writable receiver, so [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021) and [OWN-B4](04-ownership.md#45-borrow-checking-adr-0021) apply strictly: `m += m` and `m += m.sub` (an operand that overlaps `m`) are compile errors. There is no silent fallback to `m = m + m`. The hint says "write `m = m + m`".
  - The in-place method does not give the binary operator: `m + n` still needs `+`. A type with `+=` and no `+` is allowed (vet may hint).
- `x++` and `x--` are statements, as in Go. On a user type they call the `++` and `--` methods (pointer receiver, so `x` must be addressable and writable). There is **no fallback** to `x += 1`: a user type with no `++` method has no `x++`. Built-in numbers are unchanged.

**EXP-20.** *Comparisons.*
- `a == b` calls the `==` method. `a != b` is `!(a == b)`. A user `!=` method is an error.
- `a < b`, `a <= b`, `a > b`, `a >= b` mean `(a <=> b) < 0`, `<= 0`, `> 0`, `>= 0`. The method should return -1, 0 or 1; the compiler only tests the sign. A user `<`, `<=`, `>`, `>=` method is an error.
- A type with no `==` method keeps the built-in `==` if it has one (comparable structs, [Comparison operators](https://go.dev/ref/spec#Comparison_operators)). A type with an `==` method uses the method in `==`, `!=`, `switch` cases and everywhere else the source writes `==`. Map keys and `comparable` instances still use built-in equality, never the method ([GEN-13](10-generics.md#102-rules), ADR-0024 decision 11). So does `==` on a composite type that has no `==` method itself, such as `[2]V`, a struct with a `V` field or `Option[V]`: its built-in equality compares the `V` parts with the built-in equality of `V`, not with `V`'s `==` method.
- `<=>` is **built in for numbers and strings**: `a <=> b` is -1, 0 or 1, the same as `cmp.Compare(a, b)`. For floats NaN follows `cmp.Compare`: NaN is less than every other value and equal to NaN (so `<=>` on floats differs from `<` for NaN; `<` stays the IEEE comparison). The operands must have the same type. So `int`, `float64` and `string` satisfy `<=> (T) int` in constraints ([GEN-13](10-generics.md#102-rules)). The built-in `<=>` cannot be redeclared ([EXP-17](#610-operator-methods-adr-0024)).
- `gopp vet` warns when `==` and `<=>` **disagree in definition**: only one of the two is defined for a right-hand type; or they are defined for different right-hand types; or, where vet can see both bodies, they use different sets of fields (best effort). It also warns when a type with a user `==` is used as a map key.

**EXP-21.** *`&&` and `||` on user types.* They apply when the left operand has a user type with the method; with a `bool` left operand they are the built-in short-circuit operators.
- **Eager form** `(b U) R`: the right operand is evaluated before the call. There is **no short-circuit**. This is a listed difference from the built-in `&&`.
- **Lazy form** `(b func() U) R`: the compiler passes the right operand `e` as the closure `func() U { return e }`. The method decides whether and how often to call it, so `e` runs only if the method calls it. The closure follows the normal closure rules ([CLO-1](11-closures.md), [CLO-4](11-closures.md)): when the method does not keep the parameter, the closure does not escape and may use borrowed variables.
- If both forms exist for the same `U`, the lazy form is used.
- `if a && b` needs a `bool` result, as always; a user `&&` may return any type.

**EXP-22.** *Index.* `a[i]` and `a[i, j]` on a value whose type has `[]`. `m[i, j]` already parses as an index with a list ([6.1](#61-syntax)); the checker treats it as an instantiation only when `m` is a generic function or type.
- Read: `a[i, j]` calls `[](i, j)`. The result is a value (a temporary, [EXP-2](#62-new-built-in-functions)); it can be a `View[T]` or a pointer that borrows from the receiver.
- Write: `a[i, j] = v` calls `[]=(i, j, v)`; `v` is moved in. The last parameter is the value, the others are the indexes. `a` must be addressable and writable.
- `a[i] op= v` means `a[i] = a[i] op v` with `a` and the indexes evaluated once. It needs `[]`, `[]=` and the binary method of the element type.
- `a[i]` is **not a place**: `a[i] = v` is always a `[]=` call, and `a[i]++`, `&a[i]`, `a[i].f = v` and `move(a[i])` are errors (as `m[k].f = v` is for Go maps). Write `a[i] += 1` or build a new value and assign it.
- Slice expressions `a[i:j]` are not overloadable. Using `[]` on a type with built-in indexing is an error at the declaration ([EXP-17](#610-operator-methods-adr-0024)).
- Overload sets work on the index types: `[](i int)` and `[](name string)` can both exist.

**EXP-23.** *Call.* `a(x, y)` where `a` is a value of a type with `()` methods (not a func value and not a type name, so conversions `T(x)` are unchanged). Overloads are picked by the argument types ([EXP-17](#610-operator-methods-adr-0024)). `defer a(x)` and `go a(x)` work as for a method call: the receiver and the arguments are evaluated at the statement ([CON-1](12-concurrency.md)). A pointer receiver needs an addressable, writable `a`.

**EXP-24.** *Dereference.* Unary `*a` calls the `*` method. The method must return `View[T]` (read-only, borrowed from the receiver by borrow summaries, [OWN-B](04-ownership.md#45-borrow-checking-adr-0021)); a `*T` or any other result type is an error at the declaration. `*a` has **the type of the result**, `View[T]`. Writes through a user deref are not supported. A pointer-free Copy `T` is copied out of the view when read ([OWN-V1](04-ownership.md#44-views-d16)), so `*a + b` works for `Vec2`. `*a = v` and `&*a` are errors ([EXP-29](#610-operator-methods-adr-0024)). There is no automatic dereference: `a.f` never looks through a user `*` method (as for `Weak`, [OWN-P5](04-ownership.md#43-pointers-and-heap-allocation-d17)). Write `(*a).f`.

**EXP-25.** *Explicit conversion.* The attribute `@Convert` ([MAC-8](13-macros-surface.md#132-rules)) on an ordinary method turns it into a conversion: `@Convert func (b BigInt) Float64() float64` enables `float64(b)`.
- `@Convert` needs a method with no parameters and exactly one result, and a value receiver. Anything else is an error. A type may have **at most one** `@Convert` method per target type.
- The method keeps its name and is a normal method: `b.Float64()` still works.
- It is a conversion expression, so it is **explicit**. It is never used by assignment, argument passing, return, comparison or `any(b)`. `var f float64 = b` is an error that says "write `float64(b)`".
- The result type must be identical to the conversion's target. There is no chaining.
- The result is a new owned temporary ([EXP-2](#62-new-built-in-functions), "conversions that copy"); the operand is borrowed.
- `@Convert` with a result type that a built-in conversion already gives is an error ([EXP-17](#610-operator-methods-adr-0024)). `@Convert` on a function that is not a method is an error.

**EXP-26.** *`@Right`: a number on the left.* The built-in attribute `@Right` ([MAC-8](13-macros-surface.md#132-rules)) on a binary operator method makes `s op a` valid, where `a` has the receiver type and `s` has the parameter type.
- **Every binary operator method** may have `@Right` (`+ - * / % & | ^ << >> &^ == <=>`), except `&&` and `||` (swapping would break the laziness of [EXP-21](#610-operator-methods-adr-0024)).
- **Commutative operators `+ * & | ^ ==`:** the order does not matter, so the same method serves both `a op s` and `s op a`. The compiler calls `a.op(s)` for both and keeps the source evaluation order (`s`, then `a`).
- **All other operators:** the method is **written for the swapped order**. With `@Right`, `func (a Vec2) / (s float64) Vec2 { return Vec2{s / a.X, s / a.Y} }` is the body of `s / a`. The receiver `a` is the **right** operand. Such a method handles `s / a` **only**; it does not handle `a / s`. For `a / s` write a second method without `@Right`. Example: `1.0 / v` calls the `@Right` method with `s = 1.0`. The error message for a missing method says "write the method for the swapped order".
- For `<=>` under `@Right`, the body compares `s` with `a` and returns the sign of `s <=> a`.
- The parameter type must differ from the receiver type. `@Right` on anything that is not a binary operator method is an error.
- If the left operand's type already has a method for this pair, the use is ambiguous and is an error ([EXP-17](#610-operator-methods-adr-0024)).

**EXP-27.** *(Removed: no literal suffixes.)* Go++ has no user literal suffixes like `10.5_km` (owner decision 2026-10-28, ADR-0024 decision 15). The number is kept free for later rules, so the other EXP numbers and their links do not change. Units are written the Go way, as in `time.Duration`: typed constants times a number; an operator method makes it work for struct types too. Since ADR-0025, units of measure with unit math are also in the language: [EXP-31](#612-unit-expressions-adr-0025) and [chapter 17](17-units.md). The unit literal `100.0[KM]` is not a user suffix.
```go
type Meters float64
const (
    M  Meters = 1
    Km Meters = 1000
)
func (m Meters) String() string { return strconv.FormatFloat(float64(m), 'f', -1, 64) + "m" }

d := 10*Km + 500*M                  // Meters(10500); from another package: 10*units.Km
timeout := 30 * time.Second         // the same idiom in the standard library

// with operators (EXP-16): a money type with a currency constant
type Currency int32                 // a Copy type, so reading it out of a borrowed receiver is a copy
const CurUSD Currency = 1
type Money struct { Cents int64; Cur Currency }
var USD = Money{100, CurUSD}        // one dollar
@Right
func (a Money) * (n int64) Money    { return Money{a.Cents * n, a.Cur} }   // serves USD * 10 and 10 * USD
price := 10 * USD
```


**EXP-28.** *Precedence and associativity.* Overloading never changes the parse. Every operator keeps Go's precedence and associativity ([Operator precedence](https://go.dev/ref/spec#Operator_precedence)); the parser does not know types. The new `<=>` is a binary operator at **level 3**, with `== != < <= > >=`, left to right. So `a <=> b == 0` is `(a <=> b) == 0` and `a <=> b < 0` is `(a <=> b) < 0`, while `a < b < c` stays a type error as in Go. Send `a <- v` is a statement and has no precedence. Unary operators bind tighter than every binary one.

**EXP-29.** *Blocked operators.* Declaring any of these is a compile error with a reason, and none of them has an operator method form:
- `=` assignment (ownership and moves depend on it);
- `.` member access;
- unary `&x` address-of (the borrow checker depends on it);
- the comma;
- implicit conversions (they cause silent bugs; the only hook is the explicit `@Convert`).

Also an error, with a "write this instead" hint: `!=` (write `==`); `<`, `<=`, `>`, `>=` (write `<=>`); `:=`. A compound operator (`+=` and so on) can be declared only as the in-place form of [EXP-19](#610-operator-methods-adr-0024): pointer receiver, one parameter, no result. These built-in features have no operator form: `len`, `cap`, `append`, `copy`, `delete`, `close`, `min`, `max`, `clear`, `move`, `clone`, the type assertion `x.(T)`, slice expressions `a[i:j]`, `...`, and `go`, `defer`, `select`.

**EXP-30.** *Other rules.*
- **Not callable by name.** Operator methods are used only through their operator form (`range` and the symbols cannot be selectors anyway). A `@Convert` method is an ordinary method and can be called by name. `gopp doc` lists them under the type.
- **No own type parameters, no `!`.** An operator method cannot have type parameters of its own, and cannot be a `!` function ([ERR-1](09-errors-and-panics.md#91-functions-that-can-fail-d18)). Type parameters of the receiver type are fine. An operator that can fail returns an explicit result, or panics (like integer division by zero).
- **`extends`.** A child inherits the operator methods of its parent like all methods ([TYP-17](03-types.md#35-single-inheritance-adr-0023)). Resolution uses the exact static types, so a `Child` value is not accepted where the parameter type is `Parent` (value slicing, [EXP-15](#69-upcasts-and-downcasts-adr-0023)). A child may add an overload with another parameter type. **`==` on children:** if `Parent` defines `== (Parent)`, the child inherits it, and that hides the built-in struct equality of the child. With the exact-type rule `child == child` is then an error, with the hint "define `==` on Child, or compare the parent parts". The same holds for an inherited `<=>`. The same signature needs `@Override` and a `@Virtual` or `@Abstract` parent method, which is possible only for the operators with a pointer receiver ([TYP-18](03-types.md#35-single-inheritance-adr-0023)).
- **Partial places** ([EXP-15](#69-upcasts-and-downcasts-adr-0023), ADR-0023 decision 16). An operator method with a value receiver copies its receiver, so on a place that may hold only the parent part of a child (a tagged `*p`, the target of a `*P` the operator looks through, a tagged value receiver, the parent part `c.P`) the call is a taken receiver and an error, decided by the rule of a method value. A pointer-receiver operator borrows the place, as `(*p).Set(i, v)` does, so `(*p)[i] = v`, `*p += v`, `(*p)++`, `*p <- v` and `range *p` are fine with pointer-receiver methods. A variable of the child type is an exact value: an inherited operator runs on its parent part, like `Dup()`.
- **Export data.** The operator methods and the `@Right` marks are part of a type's export data, so importers get the same overload sets and errors.
- **Go mode.** In `.go` files nothing changes.

```go
type Vec2 struct{ X, Y float64 }
func (a Vec2) + (b Vec2) Vec2    { return Vec2{a.X + b.X, a.Y + b.Y} }
@Right
func (a Vec2) * (s float64) Vec2 { return Vec2{a.X * s, a.Y * s} }
func (a Vec2) - () Vec2          { return Vec2{-a.X, -a.Y} }
func (a Vec2) <=> (b Vec2) int   { return cmp.Compare(a.X*a.X+a.Y*a.Y, b.X*b.X+b.Y*b.Y) }

c := a + b                  // a.+(b)
d := -a * 2.0               // (a.-()).*(2.0)
e := 2.0 * a                // ok: @Right
c += b                      // c = c + b
if a < b { … }              // (a <=> b) < 0
f := a / 2.0                // error: Vec2 has no operator / for float64
g := a + 1                  // error: no + takes int (no implicit conversion)
```

## 6.11 Rationale

- Go semantics for overflow, shifts and division are kept so that ported code behaves the same. Some of them are UB in LLVM, so the compiler pays a small cost (a compare or a mask) to keep Go's answers.
- Moves after other arguments make `append(move(p), p2...)`-like calls predictable.
- `move` is a built-in (not a method) so that it works on every type, including C++ classes. `Clone` is a method that every cloneable type has, built-in types too, so `x.Clone()` works everywhere; `clone(x)` is the short form.
- Operator methods (ADR-0024) give numeric and container types readable code and let generic code require `+`, `==` and so on. They are plain methods with fixed names, so the borrow, view and never-nil rules need no special case. The operators that would break ownership, borrowing or clarity (`=`, `.`, `&x`, the comma, implicit conversions) have no form. Compound operators, `!=` and `< <= > >=` are made from `+`, `==` and `<=>`, so one definition cannot disagree with another. Eager `&&` is allowed but listed as a difference; the lazy `func() T` form keeps short-circuit.
- Units (ADR-0025) keep the checks at compile time. `+` converts both operands to the finer unit because nothing is lost and the result does not depend on the order; assignment never rescales, so a function signature says exactly which unit it takes. Dividing two values of one dimension gives a plain number, so `distance / limit` can be compared and printed as a ratio.

## 6.12 Unit expressions (ADR-0025)

Units of measure are in [chapter 17](17-units.md): declarations (UNI-1 to UNI-4), canonical form and identity (UNI-5), unit types (UNI-6 to UNI-8). This section is the expression side. "Unit value" means a value whose type is `T[U]` or a named type with such an underlying type. Every rule here has its error case in the examples of [17.3](17-units.md#173-examples).

**EXP-31.** *Unit literals.* A number literal (integer, floating-point or imaginary) directly followed by `[UnitExpr]` is a constant with that unit: `100.0[KM]`, `3[M/S]`, `2i[A]`.
- Indexing a number literal is never legal in Go++, so there is no clash with an index expression. The `[` must follow the literal on the same line; `gopp fmt` removes spaces before it. A rune or string literal cannot take a unit.
- The constant is **untyped in its number part**, as Go constants are: `100.0[KM]` has the default type `float64[KM]`, `3[M/S]` has `int[M/S]`, `2i[A]` has `complex128[A]`. It converts to any numeric type that can hold the number, with that exact unit: `var n int[KM] = 100.0[KM]` is fine, `var m float64[M] = 100.0[KM]` is an error (EXP-35).
- For an expression that is not a literal, use the conversion of EXP-32.

**EXP-32.** *Conversions and `.In`.*
- `N[U](x)` **gives a unit**: if `x` is a plain numeric value (any numeric kind, converted as in Go), the result is `x` read as a number of `U`: `float64[KM](v)`. If `x` is a unit value, it is a **scale conversion**: `x` must have the same dimension as `U`, and the value is multiplied by the constant factor (`float64[M](d)` for `d` in `KM` multiplies by 1000). Another dimension is an error. The numeric kind may change at the same time: `float64[KM](n)` for `n int[M]` multiplies by 1/1000 in floating point.
- `N(x)` where `N` is a plain numeric type and `x` a unit value **removes the unit**: the number is kept as it is in its own unit (`float64(100.0[KM])` is `100.0`). It is always explicit and has Go's numeric conversion rules (`int(d)` truncates).
- A conversion to a named unit type (`Distance(x)`, `type Distance float64[M]`) acts like the conversion to its underlying `float64[M]`. A conversion to a named type without a unit removes the unit.
- `x.In[U]()` on a unit value is the same as `T[U](x)` for the base type `T` of `x`: it converts to another unit of the **same dimension** and keeps the numeric kind. `In` is a built-in member of unit values (like `len`, not a method): it cannot be stored as a method value (`f := d.In[M]` is an error), is not in any method set, and on a value that has no unit it is an error. On a constant it gives a constant; an untyped constant keeps its kind, so `1999[M].In[KM]()` is the untyped integer `1[KM]` (the integer rule below) and `1999.0[M].In[KM]()` the untyped `1.999[KM]`. The unit argument is read with the unit grammar ([UNI-7](17-units.md#172-rules)): `speed.In[M/S^2]()` is legal.
- *Integer to a coarser unit.* An explicit conversion of an integer to a **coarser** unit (`int[KM](m)`, `m.In[KM]()` for `m int[M]`) is allowed and **truncates toward zero**, like Go's integer division: `int[KM](int[M](1999))` is `int[KM](1)`, and `int[KM](int[M](-1999))` is `-1`. This holds for an integer constant too (`int[KM](1999[M])` is 1), while a floating-point constant that is not whole in the new unit does not fit an integer kind, as in Go (`int[KM](1500.0[M])` is an error). An explicit conversion is the programmer's choice, as Go's float to int conversion truncates. The implicit conversions of EXP-34 stay exact-only (EXP-35). To a finer unit (`int[M](k)`) the conversion multiplies by the integer ratio. If the ratio is not an integer in either direction, the explicit conversion computes `x * p / q` with the exact ratio `p/q` in wide arithmetic and truncates toward zero.

**EXP-33.** *`*` and `/`.* The unit of the result is made by the unit math and simplified to the canonical form ([UNI-5](17-units.md#172-rules)).
- `a * b` has the unit `ua * ub`, and `a / b` has the unit `ua / ub`. The scales of the operands are **not** converted: the value is the plain product or quotient, and the type remembers the factor (`KM * M` is `KM*M`, a type with the factor 1000 and the dimension `M^2`). The numeric kinds must be the same, as in Go.
- A scalar (a value with no unit) may multiply or divide a unit value, on either side; the unit is kept: `2 * d`, `d / 2`, `d * k` for a plain `k float64`. `1.0 / t` has the unit `1/S`.
- If the result has an **empty dimension** (`M/M`, `KM/M`, `d / d`), its type is the plain numeric type, and the value is multiplied by the factor of the unit that remains: `d / 500.0[M]` for `d` in `KM` is `float64` and the value is the ratio `200`, not `0.2`; `5.0[KM] / 2.0[M]` is the plain `2500.0` (5 KM over 2 M is 2500, not 2.5). For an integer kind the factor must be an integer, else it is an error (EXP-35). **Two integer values of one dimension** (`5[KM] / 2[M]`, `k / m`) follow the rule of `+`: the coarser operand is converted to the finer unit first, then the integers divide, so `5[KM] / 2[M]` is `5000 / 2 = 2500` and `1[M] / 3[KM]` is `1 / 3000 = 0`, for a constant and for a variable alike (a ratio of the two units that is not an integer is the error of EXP-35).
- `real(c)` and `imag(c)` of a `complex128[U]` are `float64[U]` (the same for `complex64` and `float32`); `complex(a, b)` needs the same unit on both arguments and is a `complex128[U]`. None of them removes a unit (EXP-35: write `float64(...)` to strip it).
- Division by zero and overflow are as in Go for the base kind.

**EXP-34.** *`+`, `-` and comparisons.* `a + b`, `a - b` and `==`, `!=`, `<`, `<=`, `>`, `>=` need two unit values of the **same dimension**. A different dimension is an error ("cannot add KM and H").
- If the units are identical, nothing is converted. If only the scale differs, **both operands are converted to the finer unit** (the one with the smaller factor) at compile time, with a constant multiplication, and the result of `+` and `-` has that unit. It does not matter which operand is on the left: `100.0[KM] + 500.0[M]` and `500.0[M] + 100.0[KM]` are both `100500.0[M]`, of type `float64[M]`. This holds for every numeric base type. For constants the conversion is exact, before the value is rounded to the type.
- A comparison converts in the same way and gives `bool`. `<=>` (ADR-0024) works like the comparisons and gives `int`.
- The two numeric kinds must be the same. The named-type rule of Go holds (`Distance + float64[M]` is an error, UNI-8).
- Unary `-` and `+` keep the unit. **Compound assignment** `x += y`, `x -= y` (and `%=`) always keeps the unit of `x`, because the target cannot change type: `y` is converted to the unit of `x`, not to the finer unit. For floats and complex numbers this is always allowed (`x` in `KM`, `x += 500.0[M]` adds `0.5`). For integers it is allowed only when the conversion is exact: `y` has the unit of `x`, or a coarser unit by an integer ratio (`int[M] += int[KM]` multiplies by 1000). `int[KM] += int[M]` is an error with the hint "convert first: `x += int[KM](y)` truncates, or use floats".
- `min(a, b, …)` and `max(a, b, …)` take unit values of one dimension and follow this rule: all arguments are converted to the finest unit among them, and the result has that unit.

**EXP-35.** *Plain numbers, exact units and integers.*
- **A plain number and a unit value** cannot be added, subtracted or compared: `d + 1.0`, `d > 1.0` and `d < 5` are errors ("cannot add a number without a unit to a value in KM"); write `d + 1.0[KM]`, `d > 1.0[KM]`. `d++` and `d--` are errors for the same reason; use `d += 1.0[KM]`.
- **The one exception is zero.** An untyped constant whose value is `0` (`0`, `0.0`, `0i`) is accepted wherever a unit value of any unit is expected, because zero means the same in every unit: comparisons (`d > 0`, `d == 0`), assignment and initialization (`var d float64[KM] = 0`, `d = 0`), `+` and `-` (`d + 0`), call arguments (`f(0)`), `return 0`, composite fields and `const L float64[KM] = 0`. The value takes the unit it is used with; the result of `d + 0` has the type of `d`. Any other plain number is still an error, and so is a typed zero (`float64(0)`, a variable holding `0.0`): only the untyped constant counts.
- **Exact unit** is needed in assignment, initialization, call arguments, `return`, composite literal fields and elements, channel sends, map keys and index values, and the element types of `append` and `copy`. (`switch` tags and `case` values are comparisons and follow EXP-34.) "Exact" means the types are identical: same dimension **and** same factor. `var m float64[M] = d` with `d` in `KM` is an error with the fix-it `d.In[M]()`. An API boundary never rescales a value silently.
- **Unit to plain.** A `float64[KM]` value is not assignable to `float64`, and a `float64` is not assignable to `float64[KM]` (a plain constant is not either, except the untyped constant `0`). Write `float64(d)` or `float64[KM](v)`.
- **Integers must be exact.** Because `+`, `-`, `%`, `min`, `max` and comparisons convert to the finer unit, the conversion of an integer kind is a multiplication by the ratio of the two factors. It is exact when the ratio is an **integer**: `int[KM] + int[M]` and `int[M] + int[KM]` both work and give `int[M]`. A ratio that is not an integer (`int[IN] + int[CM]` with `unit IN = 2.54 CM`: IN converts to the finer CM with the ratio 127/50) is an error: "the conversion needs the ratio 127/50, which cannot be exact for an int; convert to float first (`float64[CM](n)`)". Only **implicit** conversions are exact-only. An explicit conversion (EXP-32, `int[KM](m)`) truncates toward zero, so `int[KM](m)` is accepted while `km += m` for `int[KM] km` and `m int[M]` is rejected. Integer multiplication wraps like any Go `*`; a constant that overflows is a compile error.
- **Floats** use the factor rounded once to the type (`float32` or `float64`) and one multiplication. **The factor must be a number of the kind.** A scale change whose factor, rounded to the kind, is `0` or infinite (`float32[M]` to a unit of `1e60 M`), or does not fit an integer kind (`uint8[KM]` against `uint8[M]` needs 1000; for a truncating conversion the numerator and the denominator must fit), is an error (E0435), in plain and in generic code alike. Without it the checker would accept a value that wraps or becomes `Inf` or `0`. A round trip `KM` to `M` to `KM` may differ in the last bit, as `x * 1000 / 1000` does. Constants are exact (EXP-37).

**EXP-36.** *`%`, shifts and bit operators.*
- `a % b` with two integer unit values of one dimension follows `+` (both are converted to the finer unit, the result has that unit). `a % 2` (a plain number) is an error: a remainder of "2 what".
- `a << n` and `a >> n` with a unit value `a` and a plain integer shift count `n` keep the unit of `a` (they multiply and divide by powers of two). A unit value as the shift count is an error.
- `&`, `|`, `^`, `&^` and unary `^` on a unit value are errors: bit patterns have no unit. Use `uint32(x)` to remove the unit first.
- `!` and `&&` do not apply (the operands are not `bool`).

**EXP-37.** *Constants.* An untyped constant with a unit is an exact rational (or a complex of rationals) with an exact unit. Constant arithmetic follows EXP-33 and EXP-34 exactly and without rounding: `1.0[KM] + 500.0[M]` is the constant `1500.0[M]`; `2.0[H] * 3.0[KM]` is `6.0[H*KM]`. `.In[U]()` and the conversion `float64[M](1.0[KM])` give constants. A constant is rounded only when it takes a typed type, and a conversion that cannot be represented (an `int` kind and `1.5[M]`) is an error as in Go. A `const` declaration may hold a unit constant: `const Limit = 120.0[KM/H]`, `const Limit float64[KM/H] = 120.0[KM/H]`. A typed constant with a unit cannot be given a plain number: `const L float64[KM] = 120` is an error.

```go
d := 100.0[KM]
t := 2.0[H]
speed := d / t                 // float64[KM/H]
total := d + 500.0[M]          // float64[M], 100500.0 (both operands in the finer unit)
n := 5[M]
big := 2[KM]
_ = n + big                    // ok: int[M], 2005
_ = big + n                    // ok: int[M], 2005 (the same, in either order)
_ = d > 0                      // ok: the untyped constant 0 is accepted
_ = d > 1.0                    // error: a plain number other than 0
var z float64[KM] = 0          // ok
_ = 5.0[KM] / 2.0[M]           // float64, 2500.0: the factor is kept
_ = d / 500.0[M]               // float64, 200
_ = d + t                      // error: cannot add KM and H
_ = d + 1.0                    // error: a plain number
var m float64[M] = d           // error: exact unit needed
```

