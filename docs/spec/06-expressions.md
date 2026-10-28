# 6. Expressions

Base: Go spec [Expressions](https://go.dev/ref/spec#Expressions), [Operands](https://go.dev/ref/spec#Operands), [Composite literals](https://go.dev/ref/spec#Composite_literals), [Function literals](https://go.dev/ref/spec#Function_literals), [Primary expressions](https://go.dev/ref/spec#Primary_expressions), [Method values](https://go.dev/ref/spec#Method_values), [Index expressions](https://go.dev/ref/spec#Index_expressions), [Slice expressions](https://go.dev/ref/spec#Slice_expressions), [Calls](https://go.dev/ref/spec#Calls), [Operators](https://go.dev/ref/spec#Operators), [Arithmetic operators](https://go.dev/ref/spec#Arithmetic_operators), [Integer overflow](https://go.dev/ref/spec#Integer_overflow), [String concatenation](https://go.dev/ref/spec#String_concatenation), [Comparison operators](https://go.dev/ref/spec#Comparison_operators), [Conversions](https://go.dev/ref/spec#Conversions), [Order of evaluation](https://go.dev/ref/spec#Order_of_evaluation), [Built-in functions](https://go.dev/ref/spec#Built-in_functions), [Min and max](https://go.dev/ref/spec#Min_and_max).

Most expressions are unchanged. This chapter lists the new built-ins, the places where ownership matters, and the safety defaults of D1.17.

## 6.1 Syntax

The expression grammar of Go is unchanged except for two additions, each defined in one place:
- the unwrapping call `PrimaryExpr "!" Arguments` (`f!(x)`, `os.Open!(p).Close!()`), in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18) ([ERR-5](09-errors-and-panics.md));
- macro invocations as an `Operand` (`#Name(args)`), in [13.1](13-macros-surface.md) ([MAC-2](13-macros-surface.md)).

## 6.2 New built-in functions

**EXP-1.** Predeclared functions and their typing:

| Call | Type | Meaning |
|---|---|---|
| `move(p)` | type of `p`; `p` must be a place (variable, field, element, map entry, pointer target) | take the value out, leave the zero value ([OWN-R4](04-ownership.md#42-ownership-rules-d14)); for a type with no zero value only from a local, or from a map as `v, ok := move(m[k])` (or a bare `move(m[k])` statement) |
| `clone(x)` | type of `x`; `x`'s type must satisfy `Clone` | short form of `x.Clone()`: copy ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). On a raw `*T` whose `T` has a value-receiver `Clone`, `clone(p)` is the same ambiguity error as `p.Clone()`; `[T Clone]` instantiation is the only place where `*T` uses the pointer copy |
| `new_unique(v)` | `Unique[T]` where `v` has type `T` | allocate, move `v` in |
| `new_shared(v)` | `Shared[T]` | allocate with counts, move `v` in |
| `new_weak(s)` | `Weak[T]` where `s` is a `Shared[T]` | make a weak reference |

- `move` and `clone` are not first-class values: they can only be called, not stored or passed (as `len` in Go).
- `x.Clone()` is the form used in examples. Every cloneable type has the method, built-in types too ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). `move` has no method form.
- `move(m[k])` removes the entry; `v, ok := move(m[k])` and `v, ok := clone(m[k])` are the comma-ok forms ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). For a value type with no zero value only the comma-ok forms may produce a value ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).
- `min`/`max` on refs return a ref (EXP-5).
- Go's `new` does not exist ([LEX-8](02-lexical.md)).

**EXP-2.** *Moving expressions.* An expression that gives ownership of a value (a call result, a conversion that copies, a composite literal, a `move(…)` or `x.Clone()`) produces an owned **temporary**. A temporary used where ownership is needed moves; otherwise it is destroyed at the end of the full statement ([OWN-R2](04-ownership.md#42-ownership-rules-d14)).

- **Parts of a temporary.** A field or element of an owned temporary is also a temporary. It moves out, and the rest of the temporary is destroyed at the end of the full statement. This holds for a struct field, an array element, an element of an owned slice or map, and a part reached through a `Unique[T]` the temporary owns.
- **Exceptions.** Through a raw pointer, a `Shared[T]` or a ref, a part is a place in someone else's storage. To take it, write `x.Clone()` or `move(x)` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). A ref into a part of a `Shared[T]` temporary is still a ref into a temporary: the handle may release the last strong reference at the end of the statement. Storing such a ref longer than the statement is an error ([OWN-V6](04-ownership.md#44-refs-d16)).
- **Conversions that share.** These are not new values. They are checked as their operand, so `string(p.Name)` needs `p.Name.Clone()` or `move(p.Name)`. Only conversions between owning types can share a buffer; a number or another trivial value shares nothing (a `@Pinned` value is never copied, [OWN-R10](04-ownership.md#42-ownership-rules-d14)). The cases are: the same underlying type (struct tags are ignored); a conversion to an interface; a conversion to or from a type parameter, unless every type in its type set copies.
- **Slice to array.** `[N]T(s)` with owning elements does not share storage: it duplicates the elements bitwise, which would leave two owners. So it is checked as its operand too, and needs `s.Clone()` or `move(s)`. With trivial elements it is a plain copy.
- **Conversions that copy.** `string(bytes)`, `[]byte(s)`, a ref to an owned type, and a number to a number. Each gives a temporary.

**EXP-3.** *Order of evaluation.* As in Go ([Order of evaluation](https://go.dev/ref/spec#Order_of_evaluation)), with one addition: moves out of places (`move(p)`, the implicit move of `p = append(p, …)`) happen **after** all other arguments of the call are evaluated.

```go
x.items = append(x.items, len(x.items))   // len(x.items) is evaluated before x.items moves
```

## 6.3 Operators on owning values

**EXP-4.** Operands of `==`, `<`, `+` and other operators on owning types are **borrowed**. `a + b` on strings allocates a new owned temporary. `+=` grows the left operand in place.

**EXP-5.** Built-in `min` and `max` on owned strings return a copy ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). On refs they return a ref.

```go
s := "a" + name        // new owned string; name is only borrowed
t := min(s, name)      // a copy of the smaller one
```

## 6.4 Safety defaults (D1.17)

These follow Go.

**EXP-6.** *Integer overflow wraps.* See [Integer overflow](https://go.dev/ref/spec#Integer_overflow). `int` is 64-bit on 64-bit targets.

**EXP-7.** *Checks are on.* Bounds checks and nil checks are always on, unless turned off with `@NoBounds` (per function) or a per-package flag. Integer division by zero panics ([Run-time panics](https://go.dev/ref/spec#Run_time_panics)).

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
- `string(v)` where `v` is a `ref string`, and `[]T(v)` where `v` is a `ref []T`: copy into a new owned value. `[]T(v)` needs elements that satisfy `Clone` ([OWN-V3](04-ownership.md#44-refs-d16)).
- `string([]byte)` and `[]byte(string)` copy, as in Go.
- Owned to ref is implicit, never written as a conversion.
- A `Unique[T]` or `Shared[T]` stored in a place converts implicitly to `*T`; from a temporary it is an error ([OWN-P6](04-ownership.md#43-pointers-and-heap-allocation-d17)).

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

## 6.9 Rationale

- Go semantics for overflow, shifts and division are kept so that ported code behaves the same. Some of them are UB in LLVM, so the compiler pays a small cost (a compare or a mask) to keep Go's answers.
- Moves after other arguments make `append(move(p), p2...)`-like calls predictable.
- `move` is a built-in (not a method) so that it works on every type, including C++ classes. `Clone` is a method that every cloneable type has, built-in types too, so `x.Clone()` works everywhere; `clone(x)` is the short form.
