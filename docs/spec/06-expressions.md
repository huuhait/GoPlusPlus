# 6. Expressions

Base: Go spec [Expressions](https://go.dev/ref/spec#Expressions), [Operands](https://go.dev/ref/spec#Operands), [Composite literals](https://go.dev/ref/spec#Composite_literals), [Function literals](https://go.dev/ref/spec#Function_literals), [Primary expressions](https://go.dev/ref/spec#Primary_expressions), [Method values](https://go.dev/ref/spec#Method_values), [Index expressions](https://go.dev/ref/spec#Index_expressions), [Slice expressions](https://go.dev/ref/spec#Slice_expressions), [Calls](https://go.dev/ref/spec#Calls), [Operators](https://go.dev/ref/spec#Operators), [Arithmetic operators](https://go.dev/ref/spec#Arithmetic_operators), [Integer overflow](https://go.dev/ref/spec#Integer_overflow), [String concatenation](https://go.dev/ref/spec#String_concatenation), [Comparison operators](https://go.dev/ref/spec#Comparison_operators), [Conversions](https://go.dev/ref/spec#Conversions), [Order of evaluation](https://go.dev/ref/spec#Order_of_evaluation), [Built-in functions](https://go.dev/ref/spec#Built-in_functions), [Min and max](https://go.dev/ref/spec#Min_and_max).

Most expressions are unchanged. This chapter lists the new built-ins, the places where ownership matters, and the safety defaults of D1.17.

## 6.1 Syntax

The expression grammar of Go is unchanged except for two additions, each defined in one place:
- the unwrapping call `PrimaryExpr "!" Arguments` (`f!(x)`, `os.Open!(p).Close!()`), in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18) ([ERR-5](09-errors-and-panics.md));
- macro invocations as an `Operand` (`#Name(args)`), in [13.1](13-macros-surface.md) ([MAC-2](13-macros-surface.md)).

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
- Between struct types, the required bit of each field must match too, as the field names and types do ([TYP-15a](03-types.md)): the effective required bits (by `@Required` or `@Strict`) must be equal, so `U(Raw{…})` is an error if `U` and `Raw` differ in the bit of any field, in either direction. A `@Strict` struct and a non-strict struct with the same fields convert into each other only if their effective required bits are equal.
- `R(a)` where the type of `a` has a method marked `@Convert` with result `R`: the user conversion [EXP-25](#610-operator-methods-adr-0024). It is explicit only.
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

**EXP-13.** *Upcast.* A `*Child` converts implicitly to a `*Parent`, or to any ancestor pointer, and to the pointer kinds: `Unique[*Child]` to `Unique[*Parent]`, and the same for `Shared` and `Weak`; `View[Child]` converts to `View[Parent]` (read-only, [OWN-V3](04-ownership.md#44-views-d16)). The conversion changes only the static type (free, [TYP-22](03-types.md#35-single-inheritance-adr-0023)) and keeps the same borrow ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)); an owning pointer moves as before. The reverse (`*Parent` to `*Child`) is never implicit. If the parent has no type table ([TYP-22](03-types.md#35-single-inheritance-adr-0023)), only the borrowed `*Child` to `*Parent` conversion exists; the `Unique`, `Shared` and `Weak` forms are errors.

**EXP-14.** *Dispatch.* A call through a parent pointer to a `@Virtual` or `@Abstract` method runs the override of the dynamic type (indirect call through the type table). A call to a final method is direct, and `base.M()` is direct ([TYP-20](03-types.md#35-single-inheritance-adr-0023)).

**EXP-15.** *Downcast and value slicing.* Downcast is a checked type assertion on a pointer, allowed only when the parent has a type table (otherwise it is an error: there is no dynamic type): `c := p.(*Card)` panics when the dynamic type is not `*Card` or a child of `Card`; `c, ok := p.(*Card)` gives `ok = false` instead. `p` may be a `*Parent` or an owning pointer kind held in a place; the result is a `*Card` that borrows from it ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17)). Value slicing is a compile error: `var a Parent = child`, `a = child`, passing a `Child` value where a `Parent` value is expected, a `Child` value in a `Parent` composite literal field or `append` to `[]Parent`. Use a pointer. Conversions between struct types ([EXP-9](#65-conversions)) do not turn a `Child` into a `Parent`.

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
| `_name` | `(v V) R` | value (no name) | `10.5_name` |

- The shape is checked at the declaration: the wrong number of parameters, a wrong result (`==` must return `bool`, `<=>` must return `int`), or the wrong receiver kind is a compile error that names the table row.
- A **value receiver** of a non-trivial type is a read-only borrow, like every value parameter ([PAR-4](05-parameters.md)). A **pointer receiver** is required where the operator changes the operand; the operand must then be addressable and writable (a variable, field or element that is not behind a read-only borrow). A pure operator with a pointer receiver is an error, because `a + b` would need `&a` and `a` may be a temporary.
- The operands of `<-` and `[]=` that the method changes are the receiver only. The other operands are borrowed or moved in like arguments of any call ([PAR-4](05-parameters.md), [OWN-R3](04-ownership.md#42-ownership-rules-d14)): the value `v` of a send or of `[]=` moves in when its type owns data.

**EXP-17.** *Overload sets and resolution.* The methods with one operator name on one type form an overload set. For an operator expression the checker finds the set of the **static type of the left operand** (or the receiver, for unary forms), then picks the method like this:
1. The right operand's type must be **identical** to the parameter type. There are no implicit conversions and no ranking.
2. An untyped constant picks the method whose parameter type is the constant's default type (`2` is `int`, `2.0` is `float64`). If none has it, the only method that can represent the constant is used. If several can, the call is ambiguous and is an error.
3. For the call `a(x, y)` and for `[]` the same rules run over all arguments.
4. For `@Convert` only the wanted result type counts (one method per target type).
5. If the left operand has no operator method of that name, the built-in meaning is used when the operand types have one; otherwise the error says that `T` has no operator `+` for `U`.

A **second meaning is an error**: an operator method may not be declared when the type already has that meaning from its underlying type. The checker rejects it at the declaration with the reason:
- an operator with the same parameter type that the built-in operator already has (`type Meters float64` with `+ (Meters) Meters`; `Meters * float64` is fine because the built-in needs two `Meters`);
- `[]` or `[]=` on a type whose underlying type is an array, slice, string, map or pointer to array;
- `()` on a func type, `<-` on a channel type, `range` on a type that ranges already (slice, array, string, map, channel, integer, func), unary `*` on a pointer kind;
- `@Convert` with a result type that a built-in conversion already gives ([EXP-9](#65-conversions)).

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
  - The in-place method is chosen only when `x` is a place of that type. In `a[i] += v` the element is not a place ([EXP-22](#610-operator-methods-adr-0024)), so it always means `a[i] = a[i] op v`.
  - **Overlapping operands are an error.** The in-place method has a writable receiver, so [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021) and [OWN-B4](04-ownership.md#45-borrow-checking-adr-0021) apply strictly: `m += m` and `m += m.sub` (an operand that overlaps `m`) are compile errors. There is no silent fallback to `m = m + m`. The hint says "write `m = m + m`".
  - The in-place method does not give the binary operator: `m + n` still needs `+`. A type with `+=` and no `+` is allowed (vet may hint).
- `x++` and `x--` are statements, as in Go. On a user type they call the `++` and `--` methods (pointer receiver, so `x` must be addressable and writable). There is **no fallback** to `x += 1`: a user type with no `++` method has no `x++`. Built-in numbers are unchanged.

**EXP-20.** *Comparisons.*
- `a == b` calls the `==` method. `a != b` is `!(a == b)`. A user `!=` method is an error.
- `a < b`, `a <= b`, `a > b`, `a >= b` mean `(a <=> b) < 0`, `<= 0`, `> 0`, `>= 0`. The method should return -1, 0 or 1; the compiler only tests the sign. A user `<`, `<=`, `>`, `>=` method is an error.
- A type with no `==` method keeps the built-in `==` if it has one (comparable structs, [Comparison operators](https://go.dev/ref/spec#Comparison_operators)). A type with an `==` method uses the method in `==`, `!=`, `switch` cases and everywhere else the source writes `==`. Map keys and `comparable` instances still use built-in equality, never the method ([GEN-13](10-generics.md#102-rules), ADR-0024 decision 11).
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

**EXP-27.** *User literal suffixes.* A number literal with a suffix, `10.5_km` ([LEX-13](02-lexical.md#new-tokens)), calls the method `_km` of the type `km`.
- The checker looks up the type `km` (the suffix without its `_`) in the **current package**. It must have a method `_km` with one parameter and one result: `func (km) _km (v float64) Meters`. The receiver has no name, is never created, and so may be any type, even one with no zero value. The call is static and inlines.
- The literal is converted to the parameter type as an untyped constant: `10.5_km` with `v float64` is fine, `10.5_km` with `v int64` is an error (not representable), `3_km` with `v float64` is fine. Several `_km` methods with different parameter types are an overload set ([EXP-17](#610-operator-methods-adr-0024)).
- The result is an ordinary call expression of the result type. It is **not a constant**: `const D = 10.5_km` is an error.
- A suffix with no such type or method is an error that names the suffix. Suffixes from other packages are not decided (ADR-0024 open point 1; proposal: import the suffix type like any name, `10_units.km`, or by a dot import).
- Applies to integer and floating-point literals (not to strings or runes).

**EXP-28.** *Precedence and associativity.* Overloading never changes the parse. Every operator keeps Go's precedence and associativity ([Operator precedence](https://go.dev/ref/spec#Operator_precedence)); the parser does not know types. The new `<=>` is a binary operator at **level 3**, with `== != < <= > >=`, left to right. So `a <=> b == 0` is `(a <=> b) == 0` and `a <=> b < 0` is `(a <=> b) < 0`, while `a < b < c` stays a type error as in Go. Send `a <- v` is a statement and has no precedence. Unary operators bind tighter than every binary one.

**EXP-29.** *Blocked operators.* Declaring any of these is a compile error with a reason, and none of them has an operator method form:
- `=` assignment (ownership and moves depend on it);
- `.` member access;
- unary `&x` address-of (the borrow checker depends on it);
- the comma;
- implicit conversions (they cause silent bugs; the only hook is the explicit `@Convert`).

Also an error, with a "write this instead" hint: `!=` (write `==`); `<`, `<=`, `>`, `>=` (write `<=>`); `:=`. A compound operator (`+=` and so on) can be declared only as the in-place form of [EXP-19](#610-operator-methods-adr-0024): pointer receiver, one parameter, no result. These built-in features have no operator form: `len`, `cap`, `append`, `copy`, `delete`, `close`, `min`, `max`, `clear`, `move`, `clone`, the type assertion `x.(T)`, slice expressions `a[i:j]`, `...`, and `go`, `defer`, `select`.

**EXP-30.** *Other rules.*
- **Not callable by name.** Operator methods and literal-suffix methods are used only through their operator or literal form. `a._km(1.0)` is an error, with a hint that gives the literal form. (`range` and the symbols cannot be selectors anyway.) A `@Convert` method is an ordinary method and can be called by name. `gopp doc` lists them under the type.
- **No own type parameters, no `!`.** An operator method cannot have type parameters of its own, and cannot be a `!` function ([ERR-1](09-errors-and-panics.md#91-functions-that-can-fail-d18)). Type parameters of the receiver type are fine. An operator that can fail returns an explicit result, or panics (like integer division by zero).
- **`extends`.** A child inherits the operator methods of its parent like all methods ([TYP-17](03-types.md#35-single-inheritance-adr-0023)). Resolution uses the exact static types, so a `Child` value is not accepted where the parameter type is `Parent` (value slicing, [EXP-15](#69-upcasts-and-downcasts-adr-0023)). A child may add an overload with another parameter type. **`==` on children:** if `Parent` defines `== (Parent)`, the child inherits it, and that hides the built-in struct equality of the child. With the exact-type rule `child == child` is then an error, with the hint "define `==` on Child, or compare the parent parts". The same holds for an inherited `<=>`. The same signature needs `@Override` and a `@Virtual` or `@Abstract` parent method, which is possible only for the operators with a pointer receiver ([TYP-18](03-types.md#35-single-inheritance-adr-0023)).
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
