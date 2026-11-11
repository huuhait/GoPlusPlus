# 6. Expressions

Base: Go spec [Expressions](https://go.dev/ref/spec#Expressions), [Operands](https://go.dev/ref/spec#Operands), [Composite literals](https://go.dev/ref/spec#Composite_literals), [Function literals](https://go.dev/ref/spec#Function_literals), [Primary expressions](https://go.dev/ref/spec#Primary_expressions), [Method values](https://go.dev/ref/spec#Method_values), [Index expressions](https://go.dev/ref/spec#Index_expressions), [Slice expressions](https://go.dev/ref/spec#Slice_expressions), [Calls](https://go.dev/ref/spec#Calls), [Operators](https://go.dev/ref/spec#Operators), [Arithmetic operators](https://go.dev/ref/spec#Arithmetic_operators), [Integer overflow](https://go.dev/ref/spec#Integer_overflow), [String concatenation](https://go.dev/ref/spec#String_concatenation), [Comparison operators](https://go.dev/ref/spec#Comparison_operators), [Conversions](https://go.dev/ref/spec#Conversions), [Order of evaluation](https://go.dev/ref/spec#Order_of_evaluation), [Built-in functions](https://go.dev/ref/spec#Built-in_functions), [Min and max](https://go.dev/ref/spec#Min_and_max).

Most expressions are unchanged. This chapter lists the new built-ins, the places where ownership matters, and the safety defaults of D1.17.

## 6.1 Syntax

The expression grammar of Go is unchanged except for two additions, each defined in one place:
- the unwrapping call `PrimaryExpr "!" Arguments` (`f!(x)`, `os.Open!(p).Close!()`), in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18) ([ERR-5](09-errors-and-panics.md));
- macro invocations as an `Operand` (`#Name(args)`), in [13.1](13-macros-surface.md) ([MAC-2](13-macros-surface.md)).

Operator methods add no expression or token grammar ([6.10](#610-operator-methods-adr-0024)): `a + b` and `m[i, j]` (an index list is already valid Go syntax) use Go's existing tokens and precedence.

## 6.2 New built-in functions

**EXP-1.** Predeclared functions and their typing:

| Call | Type | Meaning |
|---|---|---|
| `move(x)` | type of `x`; `x` must be a local variable or a parameter | the value of `x`, handed over without a copy. `x` cannot be read afterwards until it is assigned again (`E0451`, [OWN-R3](04-ownership.md#42-ownership-rules-d14), [ADR-0041](../adr/0041-values-are-copied.md)) |
| `NewUnique(v)` | `Unique[T]` where `v` has type `T` | allocate and store `v`, an ordinary by-value argument ([PAR-8](05-parameters.md)) |
| `NewShared(v)` | `Shared[T]` | allocate with counts and store `v`, an ordinary by-value argument |
| `NewWeak(s)` | `Weak[T]` where `s` is a `Shared[T]` | make a weak reference |

- `move`, `NewUnique`, `NewShared` and `NewWeak` are built-in functions, not first-class values: they can only be called, not stored or passed (as `len` in Go). A local or a parameter is moved with `move(x)`; taking a value out of a field, an element or a map entry uses `mem.Take(&place)` or `mem.Replace(&place, v)` ([OWN-R4](04-ownership.md#42-ownership-rules-d14)).
- `v, ok := m[k]` is the comma-ok copy of a map entry ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). For a value type with no zero value only comma-ok map forms may produce a value ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).
- Go's `new` does not exist ([LEX-8](02-lexical.md)).

**EXP-2.** *Temporaries and transfers.* An expression that produces a fresh value (a call result, a conversion that copies, a composite literal, `mem.Take(&p)`, `mem.Replace(&p, v)`, or `move(x)`) produces an owned **temporary**. A temporary used as an operand of a transfer (`:=`, `=`, `return`, argument, literal field) is handed over: no copy is made, and no `move` is needed; otherwise it is dropped at the end of the full statement ([OWN-R2](04-ownership.md#42-ownership-rules-d14)).

- **Parts of a temporary.** A field or element of an owned temporary is also a temporary. It moves out without a copy, and the rest of the temporary is dropped at the end of the full statement. This holds for a struct field, an array element, an element of an owned slice or map, and a part reached through a `Unique[T]` the temporary owns.
- **Parts behind pointers.** Through a raw `*T`, a part lives in someone else's storage. Reading it out (`x := p.f`) copies it; to take it out without a copy, or when it cannot be copied, use `mem.Take(&p.f)` or `mem.Replace(&p.f, v)` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). (What a `Shared[T]` holds is read-only: a part of it is read or copied out, and `mem.Take` and `mem.Replace` on it are compile errors, [OWN-P8](04-ownership.md#43-pointers-and-heap-allocation-d17).) A `string` part (`p.Name`) read out of a `*T` or `Shared[T]` simply retains its buffer; a slice or map part (`p.Tags`) is copied with its elements, unless it is only read. Borrowing `&x` into a part of a temporary `Shared[T]` or `Unique[T]` cannot outlive the statement ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17), [OWN-R8](04-ownership.md#42-ownership-rules-d14)).
- **Conversions that keep the representation.** Conversions between types with the same underlying representation (`type S string` ↔ `string`, `type IDs []int` ↔ `[]int`, struct tag conversions) and conversions to an interface ([TYP-6](03-types.md#32-interfaces-and-runtime-type-information-d112)) copy the value as `=` does ([OWN-R3](04-ownership.md#42-ownership-rules-d14)): a `string` keeps the same bytes, and a slice or a map gets a new buffer. The copy is skipped when the operand is a temporary, the last use of a local, or `move(x)`. A value that cannot be copied is moved, and it cannot be converted to an interface at all ([TYP-6](03-types.md#32-interfaces-and-runtime-type-information-d112)). A `@Pinned` value is never copied or moved once it is initialized in a place ([OWN-R10](04-ownership.md#42-ownership-rules-d14)).
- **Slice to array.** `[N]T(s)` copies `N` elements out of `s`, each by the copy rule of its type ([OWN-R3](04-ownership.md#42-ownership-rules-d14)). For elements that cannot be copied it is a compile error, because a slice may hold more than `N` elements.
- **Conversions that copy.** `string(bytes)` (`[]byte` → `string`), `[]byte(s)` (`string` → `[]byte`), and numeric conversions allocate or compute a fresh independent value.

**EXP-3.** *Order of evaluation.* As in Go ([Order of evaluation](https://go.dev/ref/spec#Order_of_evaluation)): operands are evaluated left to right. There are two additions. When the copy of an argument is skipped and the place itself is handed over (`p = append(p, …)`, [OWN-R4](04-ownership.md#42-ownership-rules-d14)), that happens **after** all other arguments of the call are evaluated. `move(x)` takes effect when it is evaluated, so a later read of `x` in the same statement is a use after move (`E0451`): `f(move(x), len(x))` is an error, and `f(len(x), move(x))` is fine.

```go
x.items = append(x.items, len(x.items))   // len(x.items) is evaluated before x.items is handed to append
f(len(xs), move(xs))                      // ok: len(xs) is evaluated before xs is moved
f(move(xs), len(xs))                      // error (E0451): xs is read after it was moved
```

## 6.3 Operators on non-trivial values

**EXP-4.** Operands of `==`, `<`, `+` and other operators on non-trivial types are only **read**: no copy is made and the operand is not consumed. The same holds when the operator is a user method ([EXP-18](#610-operator-methods-adr-0024)). `a + b` on strings allocates a new string buffer. `s += suffix` reuses `s`'s heap buffer in place when the buffer is not shared (`refcount == 1` and `(s.ptr - data) + len(s) + len(suffix) <= byte_cap`, with `refcount`, `byte_cap` and `data` as defined in [SLC-5](08-slices-and-range.md); the trailing NUL byte is updated), or allocates a new buffer when shared or out of capacity.

**EXP-5.** Built-in `min` and `max` on `string` return the smaller or larger `string` handle directly (retaining its shared immutable buffer in $O(1)$ without copying bytes).

```go
s := "a" + name        // new string; name is only read
t := min(s, name)      // O(1) shared string handle, no byte copy
```

## 6.4 Safety defaults (D1.17)

These follow Go.

**EXP-6.** *Integer overflow wraps.* See [Integer overflow](https://go.dev/ref/spec#Integer_overflow). `int` is 64-bit on 64-bit targets.

**EXP-7.** *Checks are on.* Bounds checks and nil checks are always on. An index or a slice bound that is a constant, on an operand whose length is written in the program, is checked at compile time ([SLC-13](08-slices-and-range.md)). Nil checks apply to channels, `error` and `any` (empty-interface) values. A nil map is empty and a write creates it, so map writes need no nil check. A write into a temporary map, or into the map that a type assertion yields, is a compile error, so there is no run-time nil-map panic ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)). Pointers, funcs and interfaces with methods are never nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)), so their uses need none. `@NoBounds` on a function turns off the index and slice bounds checks in that function, and nothing else: nil checks, the division-by-zero check and every other check stay on. There is no package-wide switch in version 1. Integer division by zero panics ([Run-time panics](https://go.dev/ref/spec#Run_time_panics)).

**EXP-8.** *Operations Go defines where C leaves behavior undefined.* Go++ lowers them explicitly in C23 (`P04 D4.12` + `-fwrapv`) so every case produces Go's exact result and none of them is UB.

| Operation | Result |
|---|---|
| `MinInt / -1` | `MinInt` (no trap) |
| `MinInt % -1` | `0` (no trap) |
| shift by ≥ the bit width: `x << n`, `x >> n` | `0`; for `>>` of a negative signed value: `-1` |
| out-of-range float to integer conversion | saturates deterministically: `MaxInt` above range / `+Inf`, `MinInt` below range / `-Inf`, `0` for `NaN` (saturating C23 helper, never UB) |
| floating-point fusing (FMA) | follows Go's compiler on each target (`-ffp-contract=off` + explicit `__builtin_fma` on `arm64`) |

```go
var m int64 = math.MinInt64
fmt.Println(m / -1, m % -1)    // -9223372036854775808 0
var s uint = 70
fmt.Println(uint32(1) << s)    // 0
fmt.Println(int8(-8) >> s)     // -1
```

## 6.5 Conversions

**EXP-9.** Conversions follow [Conversions](https://go.dev/ref/spec#Conversions) with these additions:
- `string([]byte)` and `[]byte(string)` copy into a fresh backing buffer, as in Go, so mutating the `[]byte` never affects the `string`.
- Between struct types, the required bit of each field must match too, as the field names and types do ([TYP-15](03-types.md#34-required-fields-d14-rule-11)): each field is compared as "required" when `@Required` is set on it, or when its type definitely has no zero value in every instance (a pointer, a func, a method interface, a struct with a required field, a C++ type without a default constructor). A field that is never nil on one side ([TYP-15](03-types.md#34-required-fields-d14-rule-11): `@Required` on `error`, `any`, a map or a channel) must be never nil on the other side too, so a value that may hold nil (the set-only field of an instance such as `SG[error]`) never reaches a never-nil field. The two sides must agree for every field, so `U(Raw{…})` is an error if `U` and `Raw` differ in this for any field, in either direction. A field whose type is a type parameter counts as required only through `@Required`, because an instance may have a zero value (GEN-5): `GenR[T](gen)` from `Gen[T]{A T}` to `GenR[T]{A T @Required}` is an error. (Assignment is stricter: type identity includes `@Required`, [TYP-15](03-types.md#34-required-fields-d14-rule-11).) Conversions between struct types do not turn a `Child` into a `Parent` (value slicing, [EXP-15](#69-upcasts-and-downcasts-adr-0023)).
- `R(a)` where the type of `a` has a method marked `@Convert` with result `R`: the user conversion [EXP-25](#610-operator-methods-adr-0024). It is explicit only.
- A `Unique[T]` or `Shared[T]` stored in a place converts implicitly to `*T`; from a temporary it is an error ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17)).

## 6.6 Composite literals and `&`

**EXP-10.** Composite literals work as in Go. Each field or element gets its own copy of the value written for it (`T{F: a}` copies `a`, [OWN-R3](04-ownership.md#42-ownership-rules-d14)). The copy is skipped when the value is a temporary, the last use of a local, or `move(a)`. A value that cannot be copied ([OWN-C3](04-ownership.md#41-type-categories-d13)) is moved in (`T{F: move(a)}`, or a temporary); a plain `T{F: a}` is a compile error, also on the last use of `a`.

**EXP-11.** `&T{…}` is a compile error ([OWN-P4](04-ownership.md#43-pointers-and-heap-allocation-d17)). `&x` on a variable is allowed and gives a raw pointer; on a slice or a map variable it gives a `*[]T` or `*map[K]V`, which is indexed, sliced and ranged without `*` ([OWN-P1, OWN-P5](04-ownership.md#43-pointers-and-heap-allocation-d17)). `&s[i:j]` on a slice or an array gives a `*[]T` that covers those elements of `s` itself ([OWN-S2](04-ownership.md#44-slicing-d16-adr-0041)). `&local` must not escape the function (compile error if returned).

## 6.7 Method values and function literals

**EXP-12.** A function literal is a closure; see [chapter 11](11-closures.md). A function literal declared with `!` is written `func!(x int) T { … }` ([ERR-2](09-errors-and-panics.md)). A method value is a closure over its receiver ([CLO-5](11-closures.md)).

## 6.8 Examples

```go
buf := make([]byte, 0, 16)
buf = append(buf, 'a')                    // in place: no copy of buf is made (OWN-R4)
n := 7
s := strconv.Itoa(n) + "!"                // owned temporary + literal -> new string

u := NewUnique(Config{Name: "x"})
c := u                                    // a copy: a new Unique holding a copy of the Config (Config must be copyable)
p := move(u)                              // u moves into p; u is unusable until assigned again
```

## 6.9 Upcasts and downcasts (ADR-0023)

**EXP-13.** *Upcast.* When an ancestor `Parent` has a type table ([TYP-22](03-types.md#35-inheritance-adr-0023)), a `*Child` converts implicitly to `*Parent`, and to the owning pointer kinds: `Unique[Child]` to `Unique[Parent]`, and the same for `Shared` and `Weak`. The conversion adjusts the pointer by the constant subobject offset of `Parent` inside `Child` (`0` for a primary parent at offset `0`, `+offset(Parent)` for a secondary parent; all pointer kinds stay 1 word / 8 bytes via `offset_to_top` in the type table, [TYP-22](03-types.md#35-inheritance-adr-0023)) and keeps the same borrow ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)); an owning pointer is copied or moved like any other value, and a `Unique[Child]` or `Shared[Child]` held in a place converts to a `*Parent` that borrows from it. The reverse (`*Parent` to `*Child`) is never implicit. Every upcast needs a type table in the target parent ([TYP-22](03-types.md#35-inheritance-adr-0023), ADR-0023 decision 16): without one there is no upcast of any kind, borrowed or owning, and a parent's method expression called with a child is an error too; the hint is to add a `@Virtual` method to the parent or to use `*Child`. So a pointer of a type without a table never points at a child outside the parent's own methods and what they return (a parent method sees only that parent part, and may return it, like `my.Dup()`: a valid value of the parent). For a C++ parent (in first position) the C++ vtable is the primary type table ([TYP-28](03-types.md#35-inheritance-adr-0023)); in a generic body `*T`, where the constraint of `T` is declared with `@Extends(P)`, converts to `*P` by the same rule ([GEN-12](10-generics.md#102-rules)).

**EXP-14.** *Dispatch.* A call through a parent pointer to a `@Virtual` or `@Abstract` method runs the override of the dynamic type (indirect call through the type table, adjusting the receiver offset via a thunk on secondary bases). A call to a final method is direct, and `base.M()` / `base[Parent].M()` is direct ([TYP-20](03-types.md#35-inheritance-adr-0023)). `p.Clone()` on a `*P` or a `Unique[P]` of a type with a type table also goes through the table: it recovers the complete object via `offset_to_top` and returns a `Unique[P]` holding a full copy of the dynamic type, so the overrides keep working on the copy ([OWN-R6](04-ownership.md#42-ownership-rules-d14), ADR-0023 decision 16).

**EXP-15.** *Downcast and value slicing.* Downcast is a checked type assertion on a pointer, allowed only when the parent has a type table (otherwise it is an error: there is no dynamic type): `c := p.(*Card)` panics when the dynamic type is not `*Card` or a child of `Card`; `c, ok := p.(*Card)` gives `ok = false` instead. `p` may be a `*Parent` or an owning pointer kind held in a place; the result is a `*Card` that borrows from it ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17)). A type switch on such a `p` is a series of downcasts: each case is a pointer to a child (`case *Card:`), with the same table rule; the target of a downcast must be a pointer to a type that has the parent as an ancestor. Value slicing is a compile error: `var a Parent = child`, `a = child`, passing a `Child` value where a `Parent` value is expected, a `Child` value in a `Parent` composite literal field or `append` to `[]Parent`. Use a pointer. Conversions between struct types ([EXP-9](#65-conversions)) do not turn a `Child` into a `Parent`.

The places that may hold only the parent part of a bigger value are decided by one rule: for a type with a type table (ADR-0023 decision 16), `*p` of any pointer kind and a value receiver (`func (a Animal) M()`), whose dynamic object may be a child. Such a place is used only to reach a field or call a method (`(*p).Name`, `a.Speak()`) or under `&` (`&*p`); the `Payment: Payment{…}` part of a child literal is fine. Every other use is an error: a copy (`x := *p`), `mem.Take(p)`, an argument, a result, a composite literal element, `ch <- *p`, `m[k] = *p`, `append(s, *p)`, an `Option` made from it, a deferred argument, an assignment as a whole (`p = v` with a value `v`, `q = *p`), and the same uses written without the star (`var x Payment = p`, `append(s, p)`, [OWN-P5](04-ownership.md#43-pointers-and-heap-allocation-d17)); copy the whole object through the pointer instead (`p.Clone()`, EXP-14). For a type without a type table `*p` is a whole value of that type (outside the parent's own methods and what they return a pointer of it never points at a child, EXP-13; what they return is the parent part, a valid value of the parent).

```go
var c Unique[Card] = NewUnique(Card{Amount: 10, Number: "4242"})
var p Unique[Payment] = move(c)      // ok: upcast, c moves
q, ok := p.(*Card)                   // ok: checked downcast
var v Payment = *q                   // error: value slicing; use the pointer
```

## 6.10 Operator methods (ADR-0024)

Go++ lets a type define safe arithmetic, bitwise, comparison, indexing and iteration operators by declaring a Rust-style named method marked `@Operator` ([TYP-27](03-types.md#31-type-syntax), [MAC-8](13-macros-surface.md#132-rules)). Unsafe and control-flow operators (`=`, `.`, `&x`, `,`, implicit conversions, `&&`, `||`, `<-`, unary `*x`, `()`, `++`, `--`) are not overloadable ([EXP-29](#610-operator-methods-adr-0024)). All safety rules apply to operator methods as to every method.

**EXP-16.** *Forms.* `@Operator` is allowed only on methods with one of the fixed Rust-style identifier names and shapes below (`R` is any result type, `U`, `V` and `I` any parameter types):

| Expression | `@Operator` Method Name | Signature (after receiver) | Receiver |
|---|---|---|---|
| `a + b`, `a - b`, `a * b`, `a / b`, `a % b` | `Add`, `Sub`, `Mul`, `Div`, `Rem` | `(b U) R` | value |
| `a & b`, `a \| b`, `a ^ b`, `a &^ b`, `a << n`, `a >> n` | `BitAnd`, `BitOr`, `BitXor`, `BitClear`, `Shl`, `Shr` | `(b U) R` | value |
| `-a`, `!a`, `^a` | `Neg`, `Not`, `BitNot` | `() R` | value |
| `a += b`, `a -= b`, `a *= b`, `a /= b`, `a %= b` *(optional in-place)* | `AddAssign`, `SubAssign`, `MulAssign`, `DivAssign`, `RemAssign` | `(b U)`, no result | pointer |
| `a &= b`, `a \|= b`, `a ^= b`, `a &^= b`, `a <<= n`, `a >>= n` *(optional in-place)* | `BitAndAssign`, `BitOrAssign`, `BitXorAssign`, `BitClearAssign`, `ShlAssign`, `ShrAssign` | `(b U)`, no result | pointer |
| `a == b`, `a != b` | `Eq` | `(b U) bool` | value |
| `a < b`, `a <= b`, `a > b`, `a >= b` | `Cmp` | `(b U) int` | value |
| `a[i]`, `a[i, j]` | `Index` | `(i I, …) R` | value or pointer |
| `a[i] = v`, `a[i, j] = v` | `SetIndex` | `(i I, …, v V)`, no result | pointer |
| `for x := range a` | `Iter` | `() F`, where `F` is a function type that `range` accepts | value or pointer |
| `R(a)` *(explicit conversion)* | any method name with `@Convert` (without `@Operator`) | `() R` | value |

- Without `@Operator`, an ordinary method named `Add`, `Eq`, `Index`, etc. is a normal Go method and does **not** overload any operator. Putting `@Operator` on a non-method or on any method name not in the table above is a compile error listing the valid names.
- The shape is checked at the declaration: the wrong number of parameters, a wrong result (`Eq` must return `bool`, `Cmp` must return `int`), or the wrong receiver kind is a compile error that names the table row.
- A **value receiver** and pure operator operands (`Add`..`Shr`, `Neg`, `Not`, `BitNot`, `Eq`, `Cmp`, value-receiver `Index` and `Iter`, and `@Convert`) follow the rule of by-value parameters for every type: the operand is never consumed, and no copy is made when the method only reads it ([PAR-5, PAR-8](05-parameters.md)). A **pointer receiver** is required where the operator mutates the operand (`*Assign`, `SetIndex`); the operand must then be addressable and writable (a variable, field or element that is not behind a read-only borrow). A pure binary/unary arithmetic or comparison operator with a pointer receiver is an error, because `a + b` would need `&a` and `a` may be a temporary.
- The non-receiver operands of `SetIndex` and `*Assign` follow ordinary parameter rules ([PAR-1, PAR-4, PAR-5, PAR-8](05-parameters.md), [OWN-R3](04-ownership.md#42-ownership-rules-d14)).

**EXP-17.** *Overload sets and resolution.* The `@Operator` methods with one name on one type form an overload set (for example `@Operator Mul(Vec2) Vec2` and `@Operator @Right Mul(float64) Vec2`). Mixing `@Operator` and non-`@Operator` methods of the same name on one type is a compile error. For an operator expression the checker finds the `@Operator` set of the **static type of the left operand** (or the receiver, for unary forms), then picks the method like this:
1. The right operand's type must be **identical** to the parameter type. There are no implicit conversions and no ranking.
2. An untyped constant picks the method whose parameter type is the constant's default type (`2` is `int`, `2.0` is `float64`). If none has it, the only method that can represent the constant is used. If several can, the call is ambiguous and is an error.
3. For `a[i, j]` (`Index` / `SetIndex`) the same rules run over all index arguments.
4. For `@Convert` only the wanted result type counts (one method per target type).
5. If the left operand has no `@Operator` method for that operator, the built-in meaning is used when the operand types have one; otherwise the error says that `T` has no operator `+` (`@Operator Add`) for `U`. When the left operand is an untyped constant and only the right operand's type has the operator (without `@Right`), the error says that the methods take that type as the left operand (`3 < x`: write a `@Right` `Cmp` method for the swapped order, [EXP-26](#610-operator-methods-adr-0024)).
6. A defined type over a number or string keeps its built-in operators next to `@Operator` methods for other right-hand types. `Meters * Meters` and `10 * Km` stay built in when `Meters` also has `@Operator Mul(Seconds) Speed`: a method is chosen for an untyped constant only by an exact match (the constant's default type), never by the weaker "can represent" match when the built-in operator takes the constant as a `Meters`. `a == i` with an interface operand `i` is the built-in comparison with the dynamic value, as `i == a` is. The cases of a `switch` compare with the `@Operator Eq` method of the tag's type as `tag == case` does.
7. An operand of a pointer type `*T` is read or written as the value it points to wherever only the value fits ([OWN-P5](04-ownership.md#43-pointers-and-heap-allocation-d17)). So the operator methods of `T` are reached through the pointer, like the method call `p.M()` (the method set of `*T` has the methods of both receiver kinds): `p[i]`, `p[i] = v`, `for x := range t`, `p + q`, `-p` and `p += v` work for a `*Matrix` or `*Tree`. Two forms keep the pointer's own meaning: `p == q` on two pointers compares the pointers, and `*p` dereferences. In a constraint, only `Index`, `SetIndex` and `Iter` are met by a pointer type ([GEN-13](10-generics.md#102-rules)); the other operator elements are not.

A **second meaning is an error**: an `@Operator` method may not be declared when the type already has that meaning from its underlying type:
- an operator with the same parameter type that the built-in operator already has (`type Meters float64` with `@Operator Add(Meters) Meters`; `@Operator Mul(float64) Meters` is fine because the built-in needs two `Meters`);
- `Index` or `SetIndex` on a type whose underlying type is an array, slice, string, map or pointer to array;
- `Iter` on a type that ranges already (slice, array, string, map, channel, integer, func);
- `@Convert` with a result type that a built-in conversion already gives ([EXP-9](#65-conversions)).

Two `@Operator` methods with the same name and the same parameter types are a duplicate-method error (except for a non-commutative operator, where one method with `@Right` and one without may share the same parameter type because they serve opposite operand orders, [EXP-26](#610-operator-methods-adr-0024)).

**EXP-18.** *Ownership and safety.*
- Operands of pure operators are only **read**, whatever their type (`a + b` does not consume `a` or `b`, and makes no copy of them when the method only reads them, [PAR-5](05-parameters.md)), and the result is a fresh **temporary** ([EXP-2](#62-new-built-in-functions), [EXP-4](#63-operators-on-non-trivial-values)). Evaluation order is always Go's ([EXP-3](#62-new-built-in-functions)): left operand, right operand, then the call.
- The call is checked exactly like `a.Add(b)`: borrow checking with borrow summaries ([OWN-B1 to OWN-B10](04-ownership.md#45-borrow-checking-adr-0021)), never-nil ([OWN-P9](04-ownership.md#43-pointers-and-heap-allocation-d17)), `Send` and `Sync` ([CON-12](12-concurrency.md)). A result may borrow from an operand only if its type is a pointer or borrows an array/arena, as for any method.
- The method body is ordinary code. It may allocate (`BigInt`, `Matrix`), may panic, and may call other operators. It cannot be a `!` function ([EXP-30](#610-operator-methods-adr-0024)).

**EXP-19.** *Compound assignment (`+=`, `-=`, …).*
- `x op= y` (`+= -= *= /= %= &= |= ^= <<= >>= &^=`) has two meanings, and `x` is evaluated **once** in both:
  - **In-place method (optional).** If the type of `x` has the matching `@Operator` `*Assign` method (`AddAssign`, `SubAssign`, …) with a pointer receiver, one parameter and **no result** (`@Operator func (m *Matrix) AddAssign(b Matrix)`), `x += y` calls it in place without allocating. `x` must be addressable and writable. Overloads are picked by the parameter type ([EXP-17](#610-operator-methods-adr-0024)).
  - **Otherwise** `x op= y` means `x = x op y`. The binary `@Operator` method (`Add`, `Sub`, …) must exist and its result type must be assignable to `x`. The old value of `x` is dropped by the assignment ([OWN-R2](04-ownership.md#42-ownership-rules-d14)).
  - The in-place method is chosen only when `x` is a place of that type. In `a[i] += v` where `a` has `@Operator Index`, the element is not a place ([EXP-22](#610-operator-methods-adr-0024)), so it always means `a[i] = a[i] op v`. Built-in slice or array elements are places (`s[0] += v` calls `AddAssign` when present, else `s[0] = s[0] + v`); a map element is no place, so it always means `m[k] = m[k] op v`.
  - **Overlapping operands are an error.** The in-place method has a writable receiver, so [OWN-B9](04-ownership.md#45-borrow-checking-adr-0021) and [OWN-B4](04-ownership.md#45-borrow-checking-adr-0021) apply strictly: `m += m` and `m += m.sub` are compile errors with hint `"write m = m + m"`.
  - Defining `AddAssign` alone does not define `+` (`m + n` still requires `Add`).

**EXP-20.** *Comparisons (`Eq` and `Cmp`).*
- `a == b` calls `@Operator Eq(b)`; `a != b` is `!(a == b)`.
- `a < b`, `a <= b`, `a > b`, `a >= b` mean `a.Cmp(b) < 0`, `<= 0`, `> 0`, `>= 0` (the method returns negative, `0`, or positive `int`, matching `cmp.Compare`).
- A type with no `@Operator Eq` method keeps the built-in `==` if it has one (comparable structs, [Comparison operators](https://go.dev/ref/spec#Comparison_operators)). A type with `@Operator Eq` uses it in `==`, `!=`, `switch` cases and everywhere else the source writes `==`. Map keys and `comparable` instances still use built-in equality, never the `Eq` method ([GEN-13](10-generics.md#102-rules)). So does built-in `==` on a composite type with no `@Operator Eq` of its own (`[2]V`, a struct with a `V` field, or `Option[V]`).
- `gopp vet` warns when `Eq` and `Cmp` **disagree in definition** (only one is defined for a right-hand type, or they inspect different field sets) and when a type with `@Operator Eq` is used as a map key.

EXP-21, EXP-23 and EXP-24 are not used.

**EXP-22.** *Indexing (`Index` and `SetIndex`).* `a[i]` and `a[i, j]` on a value whose type has `@Operator Index` (`m[i, j]` parses as an index list; the checker treats it as an instantiation only when `m` is a generic function or type).
- Read: `a[i, j]` calls `a.Index(i, j)`. The result is a temporary ([EXP-2](#62-new-built-in-functions)), or a `*T` that borrows from the receiver ([OWN-B3](04-ownership.md#45-borrow-checking-adr-0021)).
- Write: `a[i, j] = v` calls `(&a).SetIndex(i, j, v)`; the last parameter is the assigned value, the preceding parameters are the indexes, and `a` must be addressable and writable.
- `a[i] op= v` means `a[i] = a[i] op v` with `a` and the indexes evaluated once (requiring `Index`, `SetIndex` and the binary operator of the element type).
- `a[i]` is **not a place**: `&a[i]` and `a[i].f = v` are compile errors (as `m[k].f = v` is for Go maps).
- Slice expressions `a[i:j]` are not overloadable.
- Overload sets work on the index types: `@Operator Index(i int)` and `@Operator Index(name string)` can both exist on one type.

**EXP-25.** *Explicit conversion (`@Convert`).* The attribute `@Convert` ([MAC-8](13-macros-surface.md#132-rules)) on an ordinary method turns it into an explicit conversion: `@Convert func (b BigInt) Float64() float64` enables `float64(b)`.
- `@Convert` requires a value-receiver method with no parameters and exactly one result, and **at most one** `@Convert` method per target type.
- The method keeps its name and stays callable as `b.Float64()`.
- It is **explicit** only: never invoked implicitly by assignment, argument passing, return, comparison or `any(b)`. `var f float64 = b` is an error with hint `"write float64(b)"`.
- The result is a new owned temporary ([EXP-2](#62-new-built-in-functions)); the operand is only read and never consumed (including values that cannot be copied, [PAR-5](05-parameters.md)).
- `@Convert` with a result type that a built-in conversion already gives ([EXP-17](#610-operator-methods-adr-0024)) or on a non-method is an error.

**EXP-26.** *`@Right`: scalar or other type on the left.* The built-in attribute `@Right` ([MAC-8](13-macros-surface.md#132-rules)) alongside `@Operator` on a binary operator method (`Add`, `Sub`, `Mul`, `Div`, `Rem`, `BitAnd`, `BitOr`, `BitXor`, `BitClear`, `Shl`, `Shr`, `Eq`, `Cmp`) makes `s op a` valid, where `a` has the receiver type and `s` has the parameter type.
- **Commutative operators (`Add`, `Mul`, `BitAnd`, `BitOr`, `BitXor`, `Eq`):** one `@Operator @Right` method serves **both** `a op s` and `s op a`, keeping Go's left-to-right operand evaluation order (`s`, then `a`).
- **Non-commutative operators (`Sub`, `Div`, `Rem`, `BitClear`, `Shl`, `Shr`, `Cmp`):** the `@Operator @Right` method is **written for the swapped order** (`s op a`, where the receiver `a` is the right operand) and serves `s op a` **only** (`@Operator @Right func (a Vec2) Div(s float64) Vec2 { return Vec2{s / a.X, s / a.Y} }` handles `1.0 / v`; write a separate `@Operator Div` without `@Right` for `v / 2.0`). For `Cmp` under `@Right`, the body returns the sign comparing `s` against `a`.
- The parameter type must differ from the receiver type. `@Right` on anything other than a binary `@Operator` method is an error.
- If the left operand's type already defines a matching `@Operator` for the pair, the expression is ambiguous and is a compile error ([EXP-17](#610-operator-methods-adr-0024)).

**EXP-27.** *No user literal suffixes.* Go++ has no user literal suffixes like `10.5_km`. Units and domain quantities use normal Go defined types (`30 * time.Second`) and `@Operator` methods (`10 * USD` via `@Operator @Right Mul`, or `(100 * Kilometer) / (2 * Hour)` via `@Operator Div`).
```go
type Currency int32
const CurUSD Currency = 1
type Money struct { Cents int64; Cur Currency }
var USD = Money{100, CurUSD}

@Operator @Right
func (a Money) Mul(n int64) Money { return Money{a.Cents * n, a.Cur} } // USD * 10 and 10 * USD
price := 10 * USD
```

**EXP-28.** *Precedence and associativity.* Overloading never changes the parse: every operator keeps Go's exact precedence and associativity ([Operator precedence](https://go.dev/ref/spec#Operator_precedence)).

**EXP-29.** *Non-overloadable operators.* None of the following can be overloaded:
- `=` assignment (ownership and moves depend on it);
- `.` member access;
- unary `&x` address-of (the borrow checker depends on it);
- `,` comma and implicit conversions (only explicit `@Convert` exists);
- `&&` and `||` (always short-circuit `bool`);
- `<-` send and receive (always a built-in Go channel);
- unary `*x` dereference (always a real pointer dereference);
- `()` call (always a real function call or type conversion);
- `++` and `--` (built-in numeric types only);
- built-in forms `len`, `cap`, `append`, `copy`, `delete`, `close`, `min`, `max`, `clear`, `move`, `x.(T)`, `a[i:j]`, `...`, `go`, `defer`, `select`.

**EXP-30.** *Other rules.*
- **Callable by name too.** Because `@Operator` methods have normal Go identifier names (`Add`, `Sub`, `Mul`, `Eq`, `Cmp`, `Index`, `SetIndex`, `Iter`), they can also be called directly as methods (`a.Add(b)`, `m.Index(r, c)`), resolving across the overload set by exact argument types (`EXP-17`; a non-commutative `@Right` method is invoked only by the swapped operator `s op a`).
- **No own type parameters, no `!`.** An `@Operator` method cannot declare type parameters of its own and cannot be a `!` function ([ERR-1](09-errors-and-panics.md#91-functions-that-can-fail-d18)). Type parameters of the receiver type are fine.
- **Inheritance.** A child inherits the `@Operator` methods of all its parents ([TYP-17](03-types.md#35-inheritance-adr-0023)). Overloads with distinct parameter types across parents combine; two parents defining the same `@Operator` name and parameter types follow TYP-17's clash rule (final or differing result/receiver is an error; `@Virtual` / `@Abstract` pointer-receiver operators require `@Override` on the child). Resolution uses exact static types, so a `Child` value is not accepted where the parameter type is `Parent` (value slicing, [EXP-15](#69-upcasts-and-downcasts-adr-0023)). **`Eq` and `Cmp` on children:** if a `Parent` defines `@Operator Eq(Parent)`, the child inherits it and hides built-in struct equality on the child, so `child == child` is an error with hint `"define @Operator Eq on Child, or compare the parent parts"` (likewise for `Cmp`).
- **Partial places** ([EXP-15](#69-upcasts-and-downcasts-adr-0023), ADR-0023 decision 16). An `@Operator` method with a value receiver takes its receiver by value (a copy, or the operand itself when the copy is skipped, `PAR-5` / `PAR-8`), so on a partial place (a tagged `*p`, the target of a `*P` the operator looks through, or a tagged value receiver) calling a value-receiver operator is an error decided by the rule of a method value. Pointer-receiver operators (`SetIndex`, `*Assign`, pointer-receiver `Index` / `Iter`) borrow the place, so `p[i] = v`, `p += v` and `range *p` work on pointer-receiver methods.
- **Export data and Go mode.** `@Operator`, `@Right` and `@Convert` marks travel in export data. In `.go` files nothing changes.

```go
type Vec2 struct{ X, Y float64 }

@Operator
func (a Vec2) Add(b Vec2) Vec2    { return Vec2{a.X + b.X, a.Y + b.Y} }
@Operator @Right
func (a Vec2) Mul(s float64) Vec2 { return Vec2{a.X * s, a.Y * s} }
@Operator
func (a Vec2) Neg() Vec2          { return Vec2{-a.X, -a.Y} }
@Operator
func (a Vec2) Cmp(b Vec2) int     { return cmp.Compare(a.X*a.X+a.Y*a.Y, b.X*b.X+b.Y*b.Y) }

c := a + b                  // a.Add(b)
d := -a * 2.0               // a.Neg().Mul(2.0)
e := 2.0 * a                // ok: @Right
c += b                      // c = c + b
if a < b { … }              // a.Cmp(b) < 0
f := a / 2.0                // error: Vec2 has no @Operator Div for float64
g := a + 1                  // error: no @Operator Add takes int (no implicit conversion)
```

## 6.11 Compile-time evaluation and composite constants (ADR-0033)

Go++ extends `const` declarations to pure composite values (structs, fixed arrays, `Option`) and lets functions and methods marked `@Const` ([MAC-8](13-macros-surface.md#132-rules), [ADR-0033](../adr/0033-const-functions-and-composite-constants.md)) execute at **both** compile time and runtime.

**EXP-31.** *Composite `const` declarations.* A `const` declaration (at package scope or inside a function) may have any **constant-capable type**:
- `bool`, numeric types (`int`..`complex128`), and `string` (untyped or typed, as in Go);
- fixed-size arrays `[N]T` whose element type `T` is constant-capable;
- structs (including untagged structs with parents and without `drop`, [TYP-22](03-types.md#35-inheritance-adr-0023)) whose fields are all constant-capable;
- `Option[T]` whose payload type `T` is constant-capable (`nil` or a constant `T`).

Types with runtime identity, a mutable heap buffer, or destructors (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`, `[]T`, `map[K]V`, `chan T`, `func(...)`, `interface`, `unsafe.Pointer`, `@Pinned`, types with `drop`, tagged types of a hierarchy, imported C++ classes) cannot be `const`.
- **Immutable and non-addressable.** Every `const` value is immutable and has no mutable address: `Origin.X = 1`, `CRCTable[0] = 1`, `&Origin` and `&CRCTable[0]` are compile errors, just as `&X` is for a scalar Go `const`.
- **Operations on composite constants.** Field access (`Origin.X`), array/string indexing (`CRCTable[i]`), `len`, `cap`, `Option.IsNil()` / `Option.Get()`, and `@Const` **value-receiver** methods and operators work directly on composite constants (pointer-receiver methods and operators require a mutable addressable variable and cannot be called directly on a `const`, though they work on local `var`s inside a `@Const` function). When all operands (`i`, arguments) are constant expressions, `Origin.X`, `CRCTable[5]`, `Origin + Vec2{1, 1}` and `Origin.Dot(b)` are themselves constant expressions.
- **Static storage.** Materialized composite constants (`CRCTable[runtimeIdx]`, passing `Origin` at runtime) live in read-only static storage (`.rodata`), deduplicated across the binary, with zero `init()` startup cost and zero data races.

**EXP-32.** *`@Const` functions and methods.* Putting `@Const` ([MAC-8](13-macros-surface.md#132-rules)) on a top-level function, a value- or pointer-receiver method, an `@Operator` / `@Convert` method, or a generic function/method marks it as compile-time capable:
- **In a constant context** (`const` initializer at package or local scope, array length `[N]T`, `@Align(N)`, `@If(cond)`, macro `lit` arguments), calling a `@Const` function or method with constant arguments evaluates it at **compile time**.
- **In ordinary runtime expressions** (`t := makeCRCTable()`, `h := hash(runtimeStr)`), a `@Const` function compiles and executes as a normal Go++ function with zero overhead. Inside a runtime function, declaring a local `const mask = buildMask(12)` forces compile-time evaluation.
- Calling a function or method **without** `@Const` inside a constant expression is a compile error (`cannot call non-@Const function … in constant expression`).
- `@Const` on a non-function/method, an `@Extern` function, a `drop` method, or a `@Virtual` / `@Abstract` / `@Override` method is a compile error.

**EXP-33.** *What a `@Const` body may use (checked once at declaration).* A `@Const` body is purity-checked when declared, regardless of whether any `const` calls it yet:
- **Allowed:** local variables (`var`, `:=`), assignments, pointers `&local` to local variables/fields/elements within the `@Const` evaluation, arithmetic/bitwise/comparison/boolean/string operations, composite literals (structs, arrays, `Option`, and **local** `[]T` slices used during computation), `if`, `switch`, `for` and `range` loops, `return`, built-ins (`len`, `cap`, `min`, `max`, `make([]T, …)` for constant-capable `T`, `append`, `copy`, `clear`, `complex`, `real`, `imag`), predeclared `Option.IsNil()` and `Option.Get()`, a copy of a value when every copy method `Clone` it calls is `@Const`, reading `const` declarations (same package or imported), calling other `@Const` functions/methods (same package or imported, including recursion; in a `@Const` generic body, calling a constraint method requires the instantiated concrete method or built-in operator to be `@Const`), `!` propagation over constant-capable value error types whose `Error() string` method is `@Const`, and `panic(…)`.
- **Disallowed (compile error at the `@Const` declaration):** reading or writing any package-level `var`, calling non-`@Const` functions/methods or `@Extern` / C / C++ header functions, dynamic interface or `func`-value calls, `NewUnique` / `NewShared` / `NewWeak`, `map`, `chan`, `go`, `select`, `Mutex` / `RWMutex` / `Atomic`, `defer` / `recover`, `unsafe`, or types with `drop`.

**EXP-34.** *Deterministic bounded evaluation.*
- Compile-time evaluation uses Go++'s exact integer and IEEE-754 semantics ([EXP-6](#64-safety-defaults-d117), [EXP-8](#64-safety-defaults-d117)) with bounded step (fuel), memory and call-depth limits: one constant expression may use at most 10,000,000 evaluation steps, 64 MiB of memory and a call depth of 1,024.
- Reaching `panic(…)`, an unhandled `!` failure, division by zero, out-of-bounds indexing/slicing, or going over the step, memory or call-depth limit during compile-time evaluation is a **compile error** at the `const` evaluation site showing the compile-time call stack. An error for a limit names the limit.
- `@Const` marks and checked bodies travel in export data (`.gopx`), so imported `@Const` functions work identically to same-package ones.

```go
@Const @Operator
func (a Vec2) Add(b Vec2) Vec2 { return Vec2{a.X + b.X, a.Y + b.Y} }

@Const
func makeCRCTable() [256]uint32 {
    var t [256]uint32
    for i := 0; i < 256; i++ {
        c := uint32(i)
        for k := 0; k < 8; k++ {
            if c&1 != 0 { c = 0xedb88320 ^ (c >> 1) } else { c >>= 1 }
        }
        t[i] = c
    }
    return t
}

const Origin   = Vec2{0, 0}
const UnitDiag = Origin + Vec2{1, 1}   // compile-time Vec2{1, 1}
const CRCTable = makeCRCTable()        // [256]uint32 in .rodata, 0 init() cost!
```

## 6.12 Rationale

- Go semantics for overflow, shifts and division are kept so that ported code behaves the same. Where C or hardware instructions would trap or leave behavior undefined (`MinInt / -1`, wide shifts, float-to-int overflow), the C23 backend emits `-fwrapv` plus explicit guards so Go's answers always hold.
- Evaluating other arguments before transferring the destination slice makes `x.items = append(x.items, len(x.items))` work naturally as in Go.
- A copy has one spelling, `=` (an assignment, an argument or a result), so nothing in the source can be mistaken for a form that shares ([ADR-0045](../adr/0045-clone-is-not-called.md)). Explicit extraction out of non-local slots uses ordinary library functions `mem.Take(&p)` and `mem.Replace(&p, v)` rather than a dedicated built-in ([ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)); moving a local is the built-in `move(x)` ([ADR-0041](../adr/0041-values-are-copied.md)).
- Rust-style named `@Operator` methods ([ADR-0024](../adr/0024-operator-overloading.md)) give numeric and container types readable math (`a + b`, `2.0 * a`, `m[r, c]`) and normal Go method/interface names (`Add`, `Eq`, `Cmp`, `Index`, `Iter`) with zero symbol-name or lexer changes, while keeping `&&`/`||` short-circuiting, `<-` strictly for channels, `*p` strictly for pointers, and `f(x)` strictly for functions.
- `@Const` functions and composite `const` declarations ([ADR-0033](../adr/0033-const-functions-and-composite-constants.md)) put lookup tables, structs and computed constants in `.rodata` with zero new keywords, zero startup cost, and purity checked once at the function declaration.

