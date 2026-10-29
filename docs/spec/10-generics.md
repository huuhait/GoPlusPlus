# 10. Generics

Base: Go spec [Type parameter declarations](https://go.dev/ref/spec#Type_parameter_declarations), [Type constraints](https://go.dev/ref/spec#Type_constraints), [Satisfying a type constraint](https://go.dev/ref/spec#Satisfying_a_type_constraint), [Instantiations](https://go.dev/ref/spec#Instantiations), [Type inference](https://go.dev/ref/spec#Type_inference), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Interface types](https://go.dev/ref/spec#Interface_types).

Delta D1.10. Go's design is kept: type parameters, constraint interfaces with type sets, inference. Additions follow.

## 10.1 Syntax

Only one grammar change: **methods may have type parameters** (generic methods). `MethodDecl` (with `TypeParameters` before the optional `!`) is defined in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18).

A constraint interface may also list operator methods (`interface{ + (T) T }`, grammar in [TYP-27](03-types.md#31-type-syntax), rule [GEN-13](#102-rules)).

`Copy` and `Clone` are built-in constraints, written like Go's `comparable`: `[T Copy]`, `[T Clone]`. There is no type argument: `[T Clone[T]]` and the short `[Clone[T]]` are not used (the short form is not valid Go syntax; `go vet` reports "missing type parameter name or invalid array length").

## 10.2 Rules

**GEN-1.** *`Copy`* is a predeclared constraint satisfied by types that copy for free, like numbers: bitwise-copyable types ([OWN-C1](04-ownership.md#41-type-categories-d13)). `Copy` does not imply `Clone` (see GEN-2).

**GEN-2.** *`Clone`* is a predeclared constraint. `[T Clone]` means the method set of `T` includes a method `Clone() T` with a value receiver ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). It is satisfied by every type that can be cloned, **numbers included**: trivial types, shared handles, owning types whose parts are cloneable, types with `drop` that define `Clone() T`, and imported C++ types with a usable copy constructor. A type whose `Clone` method has another signature does not satisfy it (the method itself stays legal). The same holds for a `Clone` inherited from the parent of an `extends` type ([TYP-17](03-types.md#35-single-inheritance-adr-0023)): it makes the compiler provide none, and the child satisfies `Clone` only if the inherited or declared method has the signature `Clone() T` for the child `T` ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). A `Clone` declared on `*T` itself, with signature `Clone() *T`, makes `*T` satisfy the constraint, and `x.Clone()` calls the user method. For `T = *Handle` where `Handle` has a value-receiver `Clone`, `[T Clone]` uses the pointer copy (instantiation is the only place where a raw `*T` uses it; `clone(p)` is the ambiguity error like `p.Clone()`), while a direct `p.Clone()` is the ambiguity error: generic code sees `*Handle`'s own method set. `Unique[*T]` satisfies it only when `T` does. Pointer kinds satisfy it with their own `Clone` (a raw pointer copy, `Unique[*T]` a deep clone, `Shared[*T]` a retain), so `T = *Handle` works ([OWN-R6](04-ownership.md#42-ownership-rules-d14)).
- Both constraints can be embedded in a constraint interface like any Go interface: `interface{ Clone; cmp.Ordered }`.
- Go's own rule keeps them out of `|` unions, as it does for `comparable`.
- `Option[T]` satisfies `comparable` when `T` does (`eq[Option[int]](a, b)`); `Option[struct{ s []int }]` does not.
- A `Copy` type is also `Clone` (its `Clone()` is a plain copy), unless its method set already has a `Clone` with another signature ([OWN-R6](04-ownership.md#42-ownership-rules-d14)); then it is `Copy` but not `Clone`. Numbers, bools and the other built-in trivial types are always both. A generic body that needs both writes `[T interface{ Copy; Clone }]`. A `[T Copy]` body may not call `.Clone()` or pass `T` to a `[U Clone]` function, because GEN-5 checks the body once.

**GEN-2a.** *`View[T]` in generic code.* `View[T]` with a type parameter `T` is allowed, and `View[T]` is `Copy` and trivial for every `T` ([OWN-C1](04-ownership.md#41-type-categories-d13)). An instance where `T` is a pointer kind (`*U`, `Unique[*U]`, `Shared[*U]`, `Weak[*U]`), `unsafe.Pointer` or a `View` is a compile error, checked per instance (P04.08), as for `Option[T]` with a nilable `T` ([OWN-V1](04-ownership.md#44-views-d16)).

**GEN-3.** *Copying a `T`.* A generic body may copy a value of type `T` only if `T` is `Copy`. Otherwise it may only:
- **borrow** it (a borrowed parameter, a read, a comparison);
- **move** it (a `move` parameter, `move(x)`);
- **`Clone`** it (requires `T` to satisfy `Clone`).

**GEN-4.** *Returning a borrowed value.* In a generic body, `return a` for a borrowed parameter of non-`Copy` `T` needs `a.Clone()`, which requires `Clone` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Without the constraint it is a compile error.

**GEN-5.** *Monomorphization.* Every instantiation is compiled as its own function with the real types (P04). Consequences you can see:
- there is no run-time dictionary and no boxing of type arguments;
- ownership checks run on the generic body once (with the rules above), and the instance has the real category of each type argument;
- `var zero T` is a compile error if `T` may be a type with no zero value (a pointer kind, a func, an interface with methods other than `error`, a C++ class without a default constructor, a struct with a `@Required` field, a `@Strict` struct with a required field); `var zero Option[T]` works for every `T` that `Option` accepts (checked per instance, P04.08) ([OWN-R9](04-ownership.md#42-ownership-rules-d14)). Constrain `T` or use `Option[T]`.

**GEN-6.** *Generic methods are allowed.* A method may have its own type parameters, as in `func (s Set) Map[U any](f func(int) U) []U`. Each use is monomorphized like a generic function. The receiver's own type parameters (for example `func (l *List[T]) …`) work as in Go.

**GEN-7.** *Interfaces and generic methods.* Interfaces cannot declare generic methods. So a generic method never satisfies an interface method.

**GEN-8.** *Calling.* Type arguments may be explicit or inferred: `s.Map[string](f)` or `s.Map(f)`.

**GEN-9.** *Type arguments are checked against the constraints at the call.* `Swap[chan int](a, b)` with the `Copy` bound of `Swap` below is an error: a channel is a shared handle, not a `Copy` type.

**GEN-10.** *Syntax of the failable generic method.* `!` comes after the type parameters: `func (s Set) Parse[T any]!(text string) T`, called `s.Parse[int]!("5")`.

**GEN-11.** *`Send` and `Sync`* are predeclared constraints, written like `Copy` and `Clone`: `[T Send]`, `[T Sync]`. There is no type argument. They are computed by the compiler for every type ([CON-12](12-concurrency.md), ADR-0022) and cannot be implemented by hand. `[T Send]` means a `T` may be moved to another goroutine (for example in `go` or on a channel); `[T Sync]` means a `T` may be shared between goroutines. They can be embedded in a constraint interface like `Copy` and `Clone`: `interface{ Send; Clone }`. A generic body that moves or shares a `T` across goroutines needs the constraint; the check is once per body (GEN-5), and the instance is checked again with the real type (P04.08). An interface that embeds them (`interface{ Send }`) is itself `Send`; `error` is declared with both. Example: `func Spawn[T Send](x T, f func(T)) { go f(x) }`; `Spawn[View[string]]` is an error.

**GEN-12.** *`extends` and generics.* A generic struct may be a parent or a child (`type B[T any] extends A[T]`, [TYP-16](03-types.md#35-single-inheritance-adr-0023)); marks and checks apply per instance like every generic rule ([GEN-5](#102-rules)). A constraint "`T extends Parent`" does **not** exist in v1 (ADR-0023 open point 2): use a `*Parent` parameter or an interface.

**GEN-13.** *Operators in constraints* (ADR-0024). A constraint interface can list operator methods like any method: `interface{ + (T) T }`, `interface{ == (T) bool }`, `interface{ <=> (T) int }`, `interface{ [] (int) T }`. A conversion marked `@Convert` is not an operator element: to require one, list the ordinary method (`interface{ Float64() float64 }`).
- The body is checked once ([GEN-5](#102-rules)) with the operators the constraint lists, and with nothing else. `a + b` is valid if the constraint of `T` has `+ (T) T`. An instance calls the real method, or the built-in operator.
- **Built-in types satisfy the constraint with their built-in operators.** `int` satisfies `+ (int) int`, `- () int` and `== (int) bool`; `string` satisfies `+ (string) string`. A type-set constraint such as `~int | ~float64` gives the built-in operators as in Go.
- **Derived forms.** `+ (T) T` also gives `a += b` (and `+= (T)`, the in-place form, gives it too); `== (T) bool` gives `!=`; `<=> (T) int` gives `< <= > >=` ([EXP-19](06-expressions.md#610-operator-methods-adr-0024), [EXP-20](06-expressions.md#610-operator-methods-adr-0024)).
- **`<=>` is built in** for numbers and strings ([EXP-20](06-expressions.md#610-operator-methods-adr-0024)), so `int`, `float64` and `string` satisfy `<=> (T) int`. One constraint `interface{ <=> (T) int }` serves built-in and user types; `cmp.Ordered` stays for code that wants only built-in types. In such a body `a < b` means `(a <=> b) < 0`, so for floats NaN follows `cmp.Compare`, not IEEE `<`.
- **`comparable` and `==`.** `comparable` still means built-in equality. In a body with `[T comparable]`, `a == b` is the built-in comparison even for an instance whose type has an `==` method. Ask for the method with `interface{ == (T) bool }` instead. Map keys use built-in equality too (ADR-0024 decision 11).
- Elements are matched by exact signature: `* (float64) T` and `* (T) T` are two different elements, and a type must have both to satisfy both.
- An interface with operator elements is a constraint interface: it cannot be used as the type of a variable or parameter in v1 ([TYP-27](03-types.md#31-type-syntax)).
- Operator elements can be mixed with the others: `interface{ + (T) T; Clone }`.

## 10.3 Examples

```go
// Works for any T, owning or not: both arguments are borrowed.
func Index[T comparable](xs []T, v T) int {
    for i := range xs {
        if xs[i] == v { return i }
    }
    return -1
}

// A copy is needed to return a borrowed value, so Clone is required.
func First[T Clone](xs []T) T {
    return xs[0].Clone()        // without Clone(): error (OWN-R3a); without the Clone bound: error
}
func Dup[T Clone](x T) T { return x.Clone() }
a, b := Dup("hi"), Dup(5)       // strings and numbers both satisfy Clone

// Taking ownership: no clone, the caller moves in.
func (s *Stack[T]) Push(move v T) { s.items = append(s.items, v) }

// Copy only: a plain copy is allowed.
func Swap[T Copy](a, b *T) { *a, *b = *b, *a }

// Generic method.
type Set struct{ items []int }
func (s Set) Map[U any](f func(int) U) []U {
    out := make([]U, 0, len(s.items))
    for _, x := range s.items { out = append(out, f(x)) }
    return out
}
strs := Set{items: []int{1, 2}}.Map(func(i int) string { return strconv.Itoa(i) })

// Errors:
type Mapper interface{ Map(f func(int) string) []string }
var _ Mapper = Set{}      // error: generic method Map does not satisfy an interface method
```

```go
// Operators in constraints (GEN-13)
func Sum[T interface{ + (T) T; Clone }](xs []T) T {     // xs must not be empty
    acc := xs[0].Clone()
    for i := 1; i < len(xs); i++ { acc = acc + xs[i] }
    return acc
}
Sum([]int{1, 2, 3})                 // ok: int has the built-in +
Sum([]Vec2{{1, 2}, {3, 4}})         // ok: Vec2 has the method + (Vec2) Vec2
Sum([]string{"a", "b"})             // ok: string + string
Sum([]bool{true})                   // error: bool does not satisfy + (bool) bool
func Max[T interface{ Clone; <=> (T) int }](a, b T) T { if a > b { return a.Clone() }; return b.Clone() }   // GEN-4: a borrowed T is returned as a clone
Max(Vec2{1, 0}, Vec2{2, 0})         // ok
Max(1, 2)                           // ok: int has the built-in <=>
Max("a", "b")                       // ok: string too
```

Compared with Go: generic methods are new (Go forbids them). In Go, `First[T any]` would just copy `xs[0]`; in Go++ the `Clone` bound and the `Clone` call say that a copy happens.

## 10.4 Rationale

- Operator constraints (GEN-13) let one `Sum` work for `int`, `Vec2` and `BigInt`, with no dictionary: every instance is monomorphized and calls the real operator.
- Borrowing parameters means most generic code needs no new syntax for owning types.
- `Copy` and `Clone` make hidden copies visible in the signature. They are written like `comparable`, so they are plain Go syntax.
- Full monomorphization gives C++-class speed and lets each instance know its type's real destructor.
- Generic methods were chosen by the owner (Q5). They need no dictionary because every use is monomorphized. They cannot satisfy interfaces, so dynamic dispatch stays simple.
