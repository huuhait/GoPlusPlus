# 10. Generics

Base: Go spec [Type parameter declarations](https://go.dev/ref/spec#Type_parameter_declarations), [Type constraints](https://go.dev/ref/spec#Type_constraints), [Satisfying a type constraint](https://go.dev/ref/spec#Satisfying_a_type_constraint), [Instantiations](https://go.dev/ref/spec#Instantiations), [Type inference](https://go.dev/ref/spec#Type_inference), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Interface types](https://go.dev/ref/spec#Interface_types).

Delta D1.10. Go's design is kept: type parameters, constraint interfaces with type sets, inference. Additions follow.

## 10.1 Syntax

Two grammar changes. The first: **methods may have type parameters** (generic methods). `MethodDecl` (with `TypeParameters` before the optional `!`) is defined in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18). The second: the **`extends` constraint form** ([GEN-12](#102-rules)):

```
TypeParamDecl = IdentifierList ( TypeConstraint | "extends" Type ) .     /* [T extends P] is short for [T interface{ extends P }] */
InterfaceElem = /* Go alternatives */ | "extends" Type .                /* GEN-12 */
```

A type parameter whose whole constraint is `unit` is a **unit parameter** ([GEN-14](#102-rules)): `func Avg[U unit](xs []float64[U]) float64[U]`.

```
TypeConstraint = /* Go alternatives */ | "unit" .                       /* GEN-14 */
```

A constraint interface may also list operator methods (`interface{ + (T) T }`, grammar in [TYP-27](03-types.md#31-type-syntax), rule [GEN-13](#102-rules)).

`Copy` and `Clone` are built-in constraints, written like Go's `comparable`: `[T Copy]`, `[T Clone]`. There is no type argument: `[T Clone[T]]` and the short `[Clone[T]]` are not used (the short form is not valid Go syntax; `go vet` reports "missing type parameter name or invalid array length").

## 10.2 Rules

**GEN-1.** *`Copy`* is a predeclared constraint satisfied by types that copy for free, like numbers: bitwise-copyable types ([OWN-C1](04-ownership.md#41-type-categories-d13)). `Copy` does not imply `Clone` (see GEN-2). A type parameter whose type set lists only Copy types (`[T int | float64]`, `[T ~int]`, `[T int | *int]`) is Copy without writing `Copy`: it is decided on the real type set, in one place, for copying, returning and passing it to a `[U Copy]` function alike. `any`, `comparable` and a constraint with only methods list no types and are not Copy. A `~T` term also holds every defined type over `T`, and one of those may have a `drop` method (or be `@Pinned`): so a parameter that is Copy only through a type set with a `~T` term carries an implicit Copy requirement, and every instantiation must be Copy, checked like any constraint where the type arguments are given or inferred, in another package too, and through another generic (the outer parameter must be Copy or carry the same requirement): `dup[T ~int]` called with `type N int` that has `drop` is "N does not satisfy Copy, which the type set of T (~int) implies: N has a drop method". Exact terms (`N | int`) are judged as written. Copy says nothing about pointers: read through a view, `[T int | *int]` is still a view ([OWN-V1](04-ownership.md#44-views-d16)).

**GEN-2.** *`Clone`* is a predeclared constraint. `[T Clone]` means the method set of `T` includes a method `Clone() T` with a value receiver ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). It is satisfied by every type that can be cloned, **numbers included**: trivial types, shared handles, owning types whose parts are cloneable, types with `drop` that define `Clone() T`, and imported C++ types with a usable copy constructor. A type whose `Clone` method has another signature does not satisfy it (the method itself stays legal). The same holds for a `Clone` inherited from the parent of an `extends` type ([TYP-17](03-types.md#35-single-inheritance-adr-0023)): it makes the compiler provide none, and the child satisfies `Clone` only if the inherited or declared method has the signature `Clone() T` for the child `T` ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). A `Clone` declared on `*T` itself, with signature `Clone() *T`, makes `*T` satisfy the constraint, and `x.Clone()` calls the user method. For `T = *Handle` where `Handle` has a value-receiver `Clone`, `[T Clone]` uses the pointer copy (instantiation is the only place where a raw `*T` uses it; `clone(p)` is the ambiguity error like `p.Clone()`), while a direct `p.Clone()` is the ambiguity error: generic code sees `*Handle`'s own method set. `Unique[*T]` satisfies it only when `T` does. Pointer kinds satisfy it with their own `Clone` (a raw pointer copy, `Unique[*T]` a deep clone, `Shared[*T]` a retain), so `T = *Handle` works ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). A type parameter whose type set lists only cloneable types (`[T ~string | ~int]`) is `Clone` without writing it, decided on the real type set as for Copy (GEN-1), with the same implicit requirement on every instantiation when a `~T` term is involved (a `~[]int` type with `drop` and no `Clone()` does not satisfy it); `any`, `comparable` and a constraint with only methods are not.
- Both constraints can be embedded in a constraint interface like any Go interface: `interface{ Clone; cmp.Ordered }`.
- Go's own rule keeps them out of `|` unions, as it does for `comparable`.
- `Option[T]` satisfies `comparable` when `T` does (`eq[Option[int]](a, b)`); `Option[struct{ s []int }]` does not.
- A `Copy` type is also `Clone` (its `Clone()` is a plain copy), unless its method set already has a `Clone` with another signature ([OWN-R6](04-ownership.md#42-ownership-rules-d14)); then it is `Copy` but not `Clone`. Numbers, bools and the other built-in trivial types are always both. A generic body that needs both writes `[T interface{ Copy; Clone }]`. A `[T Copy]` body may not call `.Clone()` or pass `T` to a `[U Clone]` function, because GEN-5 checks the body once.

**GEN-2a.** *`View[T]` in generic code.* `View[T]` with a type parameter `T` is allowed, and `View[T]` is `Copy` and trivial for every `T` ([OWN-C1](04-ownership.md#41-type-categories-d13)). An instance where `T` is a pointer kind (`*U`, `Unique[*U]`, `Shared[*U]`, `Weak[*U]`), `unsafe.Pointer` or a `View` is a compile error (E0371, the error of the written type), checked per instance with the predicate of the written type: a `View[T]` anywhere in generic code (a signature, a body, a generic type, a generic alias) is a use of `T`, decided on each type argument like the other uses of [GEN-12](#102-rules), so every route is covered: an explicit or an inferred instantiation, one in another package (the use travels in the export data), and one that passes through another generic. As for `Option[T]` with a nilable `T` ([OWN-V1](04-ownership.md#44-views-d16)).

**GEN-3.** *Copying a `T`.* A generic body may copy a value of type `T` only if `T` is `Copy`: constrained by `Copy`, or with a type set that lists only Copy types (GEN-1; with a `~T` term, every instantiation must then be Copy). Otherwise it may only:
- **borrow** it (a borrowed parameter, a read, a comparison);
- **move** it (a `move` parameter, `move(x)`);
- **`Clone`** it (requires `T` to satisfy `Clone`).

**GEN-4.** *Returning a borrowed value.* In a generic body, `return a` for a borrowed parameter of non-`Copy` `T` needs `a.Clone()`, which requires `Clone` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Without the constraint it is a compile error.

**GEN-5.** *Monomorphization.* Every instantiation is compiled as its own function with the real types (P04). Consequences you can see:
- there is no run-time dictionary and no boxing of type arguments;
- ownership checks run on the generic body once (with the rules above), and the instance has the real category of each type argument;
- `var zero T` is a compile error if `T` may be a type with no zero value (a pointer kind, a func, an interface with methods other than `error`, a C++ class without a default constructor, a struct with a `@Required` field, a `@Strict` struct with a required field); `var zero Option[T]` works for every `T` that `Option` accepts (checked per instance, P04.08) ([OWN-R9](04-ownership.md#42-ownership-rules-d14)). Constrain `T` or use `Option[T]`.
- an instantiation cycle is a compile error, as in Go (types2 `mono.go`): a generic type or function that instantiates itself, directly or through others, with a type argument built from its own type parameter (`type T[P any] struct{ s []T[[]P] }`, `func f[P any]() { f[[]P]() }`) would need instances without end. As in Go, it is reported only for a package with no other error (P03.23); a type on such a cycle is marked before any body is checked, so the checks that follow every instance a type reaches (clone, views held, the by-value cycle sweep) stop at its instances, and it stays valid for everything else, so every other error is reported as Go reports it. A type declared inside a generic function (or a method of a generic type, or a function literal in one) is implicitly parameterized by the type parameters around it, as in Go (`func f[P any]() { type L struct{ x []P }; f[L]() }` is a cycle: the instances of `L` are new for every instance of `f`); each such type, a local generic one too, is a vertex of the check, with an edge of weight 1 from every type parameter around it, so any generic call over it that comes back is reported with Go's text, positions and note order (`P instantiated as L`, `L implicitly parameterized by P`) (P03.26). An instantiation through a generic alias counts (`type A[P any] = T[[]P]` with `T` holding `A[P]`). A type that holds an instance of itself by value (`type T[P any] struct{ a Option[T[[]P]] }`) is an invalid recursive type instead ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).

**GEN-6.** *Generic methods are allowed.* A method may have its own type parameters, as in `func (s Set) Map[U any](f func(int) U) []U`. Each use is monomorphized like a generic function. The receiver's own type parameters (for example `func (l *List[T]) …`) work as in Go.

**GEN-7.** *Interfaces and generic methods.* Interfaces cannot declare generic methods. So a generic method never satisfies an interface method.

**GEN-8.** *Calling.* Type arguments may be explicit or inferred: `s.Map[string](f)` or `s.Map(f)`.

**GEN-9.** *Type arguments are checked against the constraints at the call.* `Swap[chan int](a, b)` with the `Copy` bound of `Swap` below is an error: a channel is a shared handle, not a `Copy` type.

**GEN-10.** *Syntax of the failable generic method.* `!` comes after the type parameters: `func (s Set) Parse[T any]!(text string) T`, called `s.Parse[int]!("5")`.

**GEN-11.** *`Send` and `Sync`* are predeclared constraints, written like `Copy` and `Clone`: `[T Send]`, `[T Sync]`. There is no type argument. They are computed by the compiler for every type ([CON-12](12-concurrency.md), ADR-0022) and cannot be implemented by hand. `[T Send]` means a `T` may be moved to another goroutine (for example in `go` or on a channel); `[T Sync]` means a `T` may be shared between goroutines. They can be embedded in a constraint interface like `Copy` and `Clone`: `interface{ Send; Clone }`. A generic body that moves or shares a `T` across goroutines needs the constraint; the check is once per body (GEN-5), and the instance is checked again with the real type (P04.08). An interface that embeds them (`interface{ Send }`) is itself `Send`; `error` is declared with both. Example: `func Spawn[T Send](x T, f func(T)) { go f(x) }`; `Spawn[View[string]]` is an error.

**GEN-12.** *`extends` and generics* (ADR-0023 decision 15). A generic struct may be a parent or a child (`type B[T any] extends A[T]`, [TYP-16](03-types.md#35-single-inheritance-adr-0023)); marks and checks apply per instance like every generic rule ([GEN-5](#102-rules)). The constraint `[T extends P]` means `T` is `P` or a type that extends `P`, directly or indirectly:
```go
func Feed[T extends Animal](a *T) { a.Eat(); a.Speak() }
func Both[T interface{ extends Animal; Clone }](a *T) Unique[*T] { a.Eat(); return a.Clone() }
```
- *Grammar.* In a type parameter list `T extends P` is short for `T interface{ extends P }`. `extends` is also an element of a constraint interface, next to methods, operator methods and type terms. It is contextual ([LEX-12](02-lexical.md#lex-12)). `P` is a struct type, a generic one with arguments too (`T extends Box[U]`), or a C++ class ([TYP-28](03-types.md#35-single-inheritance-adr-0023)).
- *In the body* (checked once, [GEN-5](#102-rules)): `*T` converts to `*P` (an upcast, the same borrow, [EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023)); the methods of `P` (operator methods too, [GEN-13](#102-rules)) and the fields of `P` are available on `T`. Nothing else of `T` is known. A call runs the version of the real type of the instance: `a.Speak()` in `Feed[Dog]` runs `Dog`'s `Speak`, and through a `*P` that really points to a `Dog` it dispatches dynamically as always ([EXP-14](06-expressions.md#69-upcasts-and-downcasts-adr-0023)). A write to a parent field through `*T` is allowed when the borrow rules allow it ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)). Every upcast (`*T` to `*P`, `Unique[*T]` to `Unique[*P]`, and `Shared`, `Weak`) and the downcast `p.(*T)` of a `*P` exist only when `P` has a type table; with a final-only `P` they are errors, as for a concrete child ([TYP-22](03-types.md#35-single-inheritance-adr-0023), ADR-0023 decision 16), and the fields and methods of `P` stay available on `T`. With a tagged `P`, `*a` has no value use ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023)) and `a.Clone()` is a `Unique[*T]` holding a full copy of the dynamic type ([OWN-R6](04-ownership.md#42-ownership-rules-d14)); a tagged type does not satisfy `Copy`, since generic code copies a `T` through a `*T` freely.
- *Value slicing* stays an error: a `T` value does not convert to a `P` value ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023)). `var zero T` follows [GEN-5](#102-rules): it is an error when `P` has no zero value.
- *Instantiation.* `Feed[Dog]` and `Feed[Animal]` are accepted. `Feed[int]`, `Feed[Cat]` where `Cat` does not extend `Animal`, an interface type and a pointer type as `T` are errors, with the constraint named. Elements may be combined, but `extends` with a type-set term (`~int`) has an empty type set and is an error.
- *Inference.* In `func Feed[T extends Box[U], U any](a *T)`, `U` is inferred from the argument: the ancestor of its type that is an instance of `Box` (`IntBox extends Box[int]` gives `U = int`, also through generic parents) is unified with the bound.
- *Generic code cannot cut a child down to `T`.* What the generic code does with a type parameter `T` that the plain code decides on the real type is recorded as a *use* of `T` (its kind, and the method name where it matters): a copy, a move or an assignment through `*p` (a pointer of any kind to `T`); a value clone through a pointer or a view (`(*p).Clone()`, `v.Clone()` on a `View[T]`, `clone(...)`); a value of `T` made or held (`var z T`, `T{...}`, `make([]T, n)`, an element of type `T`, and `T` held by value inside an instance, `Option[T]` or `Box[T]`, as for `Option[Shape]` in plain code; a pointer or a view of `T` holds none); and a method of `T` whose receiver is taken (a method value `x.M`, `go` / `defer x.M()`, a `move` receiver), on `x`, `(*p)` or `p`. Every instantiation decides each use on the real type argument with the rule of the plain code ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023), [TYP-19](03-types.md#35-single-inheritance-adr-0023)), so a generic body and the same code written for the type itself are rejected together: `*p` of a tagged `Animal` may be only the parent part of a `Dog`; `f := x.Val` copies the parent part with `T = Dog` when `Val` is inherited (as `g := dog.Val` does), and copies what the pointer points to with `T = *Animal`; an abstract type has no values. The uses are an implicit requirement, like Copy for a `~T` type set (GEN-1): every instantiation checks them (explicit, inferred, a function value, through another generic, a method of a generic type, in another package: the uses travel in the export data), the error (E0316, E0323 for a type) names the use, and a use passes on to a type parameter that is given for such a parameter. A direct call `x.M()` only borrows the receiver and a method expression `T.M` binds none (like `(*Animal).Val`), so both stay valid, as do pointer-receiver methods and `[T any]` swap code over untagged types.
- *Final-only parents.* If `P` has no type table, the limits of [TYP-22](03-types.md#35-single-inheritance-adr-0023) apply in the body: no upcast `*T` to `*P` (ADR-0023 decision 16), no `Unique[*T]` to `Unique[*P]` (and `Shared`, `Weak`) and no downcast, since the body is checked once for every `T`; the fields and methods of `P` stay available on `T`.
- *Not decided* (ADR-0023 open point 5): whether a C++ class derived from a C++ `P` in C++ satisfies `T extends P`; v1 default is no.

**GEN-13.** *Operators in constraints* (ADR-0024). A constraint interface can list operator methods like any method: `interface{ + (T) T }`, `interface{ == (T) bool }`, `interface{ <=> (T) int }`, `interface{ [] (int) T }`. A conversion marked `@Convert` is not an operator element: to require one, list the ordinary method (`interface{ Float64() float64 }`).
- The body is checked once ([GEN-5](#102-rules)) with the operators the constraint lists, and with nothing else. `a + b` is valid if the constraint of `T` has `+ (T) T`. An instance calls the real method, or the built-in operator.
- **Built-in types satisfy the constraint with their built-in operators.** `int` satisfies `+ (int) int`, `- () int` and `== (int) bool`; `string` satisfies `+ (string) string`. A type-set constraint such as `~int | ~float64` gives the built-in operators as in Go.
- **Derived forms.** `+ (T) T` also gives `a += b` (and `+= (T)`, the in-place form, gives it too); `== (T) bool` gives `!=`; `<=> (T) int` gives `< <= > >=` ([EXP-19](06-expressions.md#610-operator-methods-adr-0024), [EXP-20](06-expressions.md#610-operator-methods-adr-0024)).
- **`<=>` is built in** for numbers and strings ([EXP-20](06-expressions.md#610-operator-methods-adr-0024)), so `int`, `float64` and `string` satisfy `<=> (T) int`. One constraint `interface{ <=> (T) int }` serves built-in and user types; `cmp.Ordered` stays for code that wants only built-in types. In such a body `a < b` means `(a <=> b) < 0`, so for floats NaN follows `cmp.Compare`, not IEEE `<`.
- **`comparable` and `==`.** `comparable` still means built-in equality. In a body with `[T comparable]`, `a == b` is the built-in comparison even for an instance whose type has an `==` method. Ask for the method with `interface{ == (T) bool }` instead. Map keys use built-in equality too (ADR-0024 decision 11).
- Elements are matched by exact signature: `* (float64) T` and `* (T) T` are two different elements, and a type must have both to satisfy both.
- A type meets an element when its method set has the operator with that signature (a value type needs a value receiver; a `@Right` method of a non-commutative operator serves the swapped order and meets no element), or when it has the built-in operator of that form: `+ (T) T` for numbers and strings, `== (T) bool` for every comparable type (pointers too) that has no `==` method of its own ([EXP-20](06-expressions.md#610-operator-methods-adr-0024)), `<=> (T) int` for numbers and strings, `[] (int) E` for a `[]E`, `[N]E` or `string` (`E` is `byte`), `++ ()` for numbers. A pointer type `*T` gets the operator methods of `T` only for the forms that look through a pointer ([EXP-17](06-expressions.md#610-operator-methods-adr-0024) rule 7), so generic and plain code decide alike. A type parameter meets it with the elements of its own constraint, or when every term of its type set does.
- An interface with operator elements is a constraint interface: it cannot be used as the type of a variable or parameter in v1 ([TYP-27](03-types.md#31-type-syntax)).
- Operator elements can be mixed with the others: `interface{ + (T) T; Clone }`.

**GEN-14.** *Unit parameters* (ADR-0025). `[U unit]` declares a type parameter that stands for a unit. `unit` is the predeclared constraint of [LEX-7](02-lexical.md#predeclared-identifiers); it cannot be combined with other elements (`interface{ unit; Clone }` is an error). It is allowed on generic functions, generic methods and generic types.
- *Use.* `U` can appear in a unit expression in a type: `float64[U]`, `float64[U/S]`, `float64[U^2]`, `float64[M*U]`. It is not a type and not a value: `var x U` and `U(3)` are errors.
- *Argument.* The argument for a unit parameter is a unit expression ([UNI-7](17-units.md#172-rules)). In an expression with explicit arguments only `*`, `/`, parentheses and names are read (UNI-7). A unit parameter can be given another unit parameter of the caller.
- *Inference.* From a value argument the compiler unifies the unit expression of the parameter type with the canonical form of the argument type. It solves for the unit parameter when the parameter appears once as a factor with exponent 1 or -1 (`float64[U]`, `float64[U/S]`, `float64[M*U]`); the other units in the expression are known, so the answer is exact (`float64[U/H]` with an argument `float64[KM/H]` gives `U = KM`). A parameter with another exponent (`float64[U^2]`) must be given explicitly in v1. If the argument has no unit, or the dimension cannot match, the call is an error that names the parameter. Two arguments that need two different values for one `U` are an error (unit types must be identical, [UNI-5](17-units.md#172-rules)).
- *In the body* (checked once, [GEN-5](#102-rules)): `U` is a unit of unknown dimension, so the body may use the operations that work for any unit: `+`, `-`, comparisons, a scalar `*` and `/`, `.In[U]()` (identity), and assignment between values of the same type. `x * y` of two `float64[U]` has the unit `U^2`, which is a type `float64[U^2]`; a body can build it and pass it on. A conversion to a concrete unit (`float64[M](x)`) is an error in the body, because the dimension of `U` is not known. Each instance (`Avg[KM]`) is monomorphized, so the generated code is the plain-number code.
- *Constraints over numeric types.* A unit type does **not** satisfy a constraint with a type-set term such as `~float64`, `cmp.Ordered` or a `Number` made of such terms (the underlying type of `float64[M]` is `float64[M]`); this keeps `[T ~float64]` meaning "a number with no unit". It satisfies operator constraints ([GEN-13](#102-rules), [UNI-10](17-units.md#172-rules)), `comparable`, `Copy`, `Clone`, `Send` and `Sync`.
- *Generic types.* `type Vec2[U unit] struct{ X, Y float64[U] }` makes `Vec2[M]` and `Vec2[KM/H]`. This is how a user type gets a unit in v1 (UNI-6). Operator methods on such a type ([ADR-0024](../adr/0024-operator-overloading.md)) work as on any generic type: `func (a Vec2[U]) + (b Vec2[U]) Vec2[U]`.
- *Export data.* The `unit` kind of a type parameter is part of the signature in export data.

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

Unit-generic code ([GEN-14](#102-rules)):

```go
func Avg[U unit](xs []float64[U]) float64[U] {
    var s float64[U]
    for _, x := range xs { s += x }
    return s / float64(len(xs))
}
Avg([]float64[KM]{1.0[KM], 3.0[KM]})       // 2 KM: U inferred as KM
Avg[M]([]float64[M]{1.0[M]})               // explicit

type Vec2[U unit] struct{ X, Y float64[U] }
func Len2[U unit](v Vec2[U]) float64[U*U] { return v.X*v.X + v.Y*v.Y }   // float64[U^2]
var f float64[M] = Avg([]float64[KM]{1.0[KM]})   // error: float64[KM] is not float64[M]
Avg([]float64{1.0})                        // error: no unit to infer U from
func Bad[U unit]() { var x U }             // error: U is a unit, not a type
```

Compared with Go: generic methods are new (Go forbids them). In Go, `First[T any]` would just copy `xs[0]`; in Go++ the `Clone` bound and the `Clone` call say that a copy happens.

## 10.4 Rationale

- Operator constraints (GEN-13) let one `Sum` work for `int`, `Vec2` and `BigInt`, with no dictionary: every instance is monomorphized and calls the real operator.
- Unit parameters (GEN-14) are a separate kind of parameter, so `[U unit]` code is checked once and every instance is plain-number code; type-set constraints stay unitless on purpose.
- Borrowing parameters means most generic code needs no new syntax for owning types.
- `Copy` and `Clone` make hidden copies visible in the signature. They are written like `comparable`, so they are plain Go syntax.
- Full monomorphization gives C++-class speed and lets each instance know its type's real destructor.
- Generic methods were chosen by the owner (Q5). They need no dictionary because every use is monomorphized. They cannot satisfy interfaces, so dynamic dispatch stays simple.
