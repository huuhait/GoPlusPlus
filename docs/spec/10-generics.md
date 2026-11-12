# 10. Generics

Base: Go spec [Type parameter declarations](https://go.dev/ref/spec#Type_parameter_declarations), [Type constraints](https://go.dev/ref/spec#Type_constraints), [Satisfying a type constraint](https://go.dev/ref/spec#Satisfying_a_type_constraint), [Instantiations](https://go.dev/ref/spec#Instantiations), [Type inference](https://go.dev/ref/spec#Type_inference), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Interface types](https://go.dev/ref/spec#Interface_types).

Delta D1.10. Go's design is kept: type parameters, constraint interfaces with type sets, inference. Additions follow.

## 10.1 Syntax

One grammar change: **methods may have type parameters** (generic methods). `MethodDecl` (with `TypeParameters` before the optional `!`) is defined in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18).

A constraint that asks for a parent type adds no grammar: it is an interface declaration with the attribute `@Extends` ([GEN-12](#102-rules)).

A constraint interface may also list Rust-style operator method signatures (`interface{ Add(T) T; Cmp(T) int }`, [TYP-27](03-types.md#31-type-syntax), rule [GEN-13](#102-rules)).

`Copy` and `Clone` are built-in constraints, written like Go's `comparable`: `[T Copy]`, `[T Clone]`. There is no type argument: `[T Clone[T]]` and the short `[Clone[T]]` are not used (the short form is not valid Go syntax; `go vet` reports "missing type parameter name or invalid array length").

## 10.2 Rules

**GEN-1.** *`Copy`* is a predeclared constraint. `[T Copy]` means a `T` is copied by copying its bytes, like a number: the bitwise-copyable types ([OWN-C1](04-ownership.md#41-type-categories-d13)). Every `Copy` type is also `Clone` (GEN-2). A type parameter whose type set lists only Copy types (`[T int | float64]`, `[T ~int]`, `[T int | *int]`) is Copy without writing `Copy`: it is decided on the real type set, in one place, for copying, returning and passing it to a `[U Copy]` function alike. `any`, `comparable` and a constraint with only methods list no types and are not Copy. A `~T` term also holds every defined type over `T`, and one of those may have a `drop` method (or be `@Pinned`): so a parameter that is Copy only through a type set with a `~T` term carries an implicit Copy requirement, and every instantiation must be Copy, checked like any constraint where the type arguments are given or inferred, in another package too, and through another generic (the outer parameter must be Copy or carry the same requirement): `dup[T ~int]` called with `type N int` that has `drop` is "N does not satisfy Copy, which the type set of T (~int) implies: N has a drop method". Exact terms (`N | int`) are judged as written.

**GEN-2.** *`Clone`* is a predeclared constraint. `[T Clone]` means a `T` can be copied ([OWN-R3](04-ownership.md#42-ownership-rules-d14), [ADR-0041](../adr/0041-values-are-copied.md)): a plain `=`, `return`, argument or `append` of a `T` value copies it ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). It is satisfied by every type that can be copied, **numbers included**: trivial types, strings, slices and maps whose elements can be copied, pointer kinds and the other handles, structs and arrays whose parts can be copied, types with `drop` that define `Clone() T`, and imported C++ types with a usable copy constructor. A type with `drop` whose `Clone` method has another signature does not satisfy it (the method itself stays legal). The same holds for a `Clone` inherited from a parent ([TYP-17](03-types.md#35-inheritance-adr-0023)): it is the child's copy method only if it has the signature `Clone() T` for the child `T` ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). For `T = *Handle` where `Handle` has the copy method `Clone`, a copy of a `T` is the pointer copy (as `q := p` is in plain code), not a copy of the `Handle`. `Unique[T]` satisfies it only when `T` does. Pointer kinds satisfy it with their own copy (a raw pointer copy, `Unique[T]` a deep copy, `Shared[T]` a retain), so `T = *Handle` works ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). A type parameter whose type set lists only types that can be copied (`[T ~string | ~int]`) is `Clone` without writing it, decided on the real type set as for Copy (GEN-1), with the same implicit requirement on every instantiation when a `~T` term is involved (a `~[]int` type with `drop` and no `Clone()` does not satisfy it); `any`, `comparable` and a constraint with only methods are not.
- Both constraints can be embedded in a constraint interface like any Go interface: `interface{ Clone; cmp.Ordered }`.
- Go's own rule keeps them out of `|` unions, as it does for `comparable`.
- `Option[T]` satisfies `comparable` when `T` does (`eq[Option[int]](a, b)`); `Option[struct{ s []int }]` does not.
- A `Copy` type is also `Clone` (its copy is a copy of its bytes).

**GEN-3.** *Using a `T` in generic code.* A generic body is checked once against `T`'s constraint, conservatively assuming `T` may be a type that cannot be copied ([OWN-C3, OWN-C5](04-ownership.md#41-type-categories-d13)) unless constrained otherwise:
- if `T` is `Copy` (constrained by `Copy`, or with a type set of `Copy` types, GEN-1), `T` values copy freely everywhere;
- if `T` is `Clone` (constrained by `Clone`, or with a type set of types that can be copied such as `[T ~int | ~string]` or `[K cmp.Ordered]`, which carries the implicit requirement of `GEN-1` and `GEN-2` on any `~U` term), values of `T` are copied like any other value: a plain `=`, `return`, argument or `append` copies, and the copy is skipped where the program cannot tell ([OWN-R3](04-ownership.md#42-ownership-rules-d14));
- otherwise, a local or parameter `v` of type `T` can be **read** any number of times (`v == w`, `&v`, passing `&v`, pure `@Operator` / `@Convert` calls) and **moved once** with `move(v)` (`s.items = append(s.items, move(v))`, `m[k] = move(v)`, passing `move(v)` by value to another function) with **no `Clone` bound**: no copy is made, whatever the type argument is. `return v` and a temporary (a call result) need no `move`. A `[]T` parameter that the function only reads (`func Index[T comparable](xs []T, v T) int`) is also passed without a copy for every `T` ([PAR-5](05-parameters.md));
- **copying** such a `T` (a plain by-value use of a local `v`, also on its last use as for every value that cannot be copied ([OWN-C3](04-ownership.md#41-type-categories-d13)), or copying an element `xs[0]` out of a non-local place instead of taking it with `mem.Take` / `mem.Replace`) requires `[T Clone]` (or `Copy`).

**GEN-4.** *Returning from a slice or pointer.* In a generic body, `return v` for a by-value parameter `v T` works for `[T any]` (no copy is made and no `move` is needed). Returning an element out of a slice or pointer (`return xs[0]`, `return *p`) copies it, so it requires `[T Clone]` or `Copy`; without the bound, use `mem.Take(&xs[0])` when `T` is known to have a zero value ([OWN-R4](04-ownership.md#42-ownership-rules-d14)).

**GEN-5.** *Monomorphization.* Every instantiation is compiled as its own function with the real types (P04). Consequences you can see:
- there is no run-time dictionary and no boxing of type arguments;
- ownership checks run on the generic body once (with the rules above), and the instance has the real category (`OWN-C1`, `OWN-C2` or `OWN-C3`) of each type argument;
- `var zero T` is a compile error if `T` may be a type with no zero value (a pointer kind, a func, an interface with methods other than `error`, a C++ class without a default constructor, a struct with a `@Required` field); `var zero Option[T]` works for every `T` that `Option` accepts (checked per instance, P04.08) ([OWN-R9](04-ownership.md#42-ownership-rules-d14)). Constrain `T` or use `Option[T]`.
- an instantiation cycle is a compile error, as in Go (types2 `mono.go`): a generic type or function that instantiates itself, directly or through others, with a type argument built from its own type parameter (`type T[P any] struct{ s []T[[]P] }`, `func f[P any]() { f[[]P]() }`) would need instances without end. As in Go, it is reported only for a package with no other error (P03.23); a type on such a cycle is marked before any body is checked, so the checks that follow every instance a type reaches (clone, borrow summaries, the by-value cycle sweep) stop at its instances, and it stays valid for everything else, so every other error is reported as Go reports it. A type declared inside a generic function (or a method of a generic type, or a function literal in one) is implicitly parameterized by the type parameters around it, as in Go (`func f[P any]() { type L struct{ x []P }; f[L]() }` is a cycle: the instances of `L` are new for every instance of `f`); each such type, a local generic one too, is a vertex of the check, with an edge of weight 1 from every type parameter around it, so any generic call over it that comes back is reported with Go's text, positions and note order (`P instantiated as L`, `L implicitly parameterized by P`) (P03.26). An instantiation through a generic alias counts (`type A[P any] = T[[]P]` with `T` holding `A[P]`). A type that holds an instance of itself by value (`type T[P any] struct{ a Option[T[[]P]] }`) is an invalid recursive type instead ([OWN-R9](04-ownership.md#42-ownership-rules-d14)).
- the instances of a package are bounded, so no program expands without end (P04.08). Past a bound is one error, `cannot instantiate F: <the bound> (GEN-5)` (E0449), at the instantiation that passes it, with the instantiations that led there; no instance is made after it, and `gopp check` reports it as the build does. The bounds:
  - a type argument that an instance makes by substitution, one that generic code builds around a type parameter (`[]T`, `Pair[T, T]`; not `T` itself, passed on as it is), nests at most 48 levels and has at most 4096 parts as a tree, counted with the real types of the instance: `a type argument nests more than 48 levels`, `a type argument has more than 4096 parts as a tree`. A type written out in the source has no such bound, however deep or large;
  - a package has at most 100000 instances, or 8 per AST node of its non-instantiated declarations if that is more (independent of comments and formatting): `the package needs more than 100000 instances`;
  - the code of a package and of all its instances is at most 4000000 nodes of syntax, or 64 per AST node of its non-instantiated declarations if that is more: `the instances of the package hold more than 4000000 nodes of code`.

  A type written in the source can be far larger as a tree than as text (`type A2 = P[A1, A1]`, doubling at every level). Its name in a panic message and in its type descriptor (`%T`) is Go's up to 65536 parts as a tree; a larger one is written `main.P[...]`.

**GEN-6.** *Generic methods are allowed.* A method may have its own type parameters, as in `func (s Set) Map[U any](f func(int) U) []U`. Each use is monomorphized like a generic function. The receiver's own type parameters (for example `func (l *List[T]) …`) work as in Go.

**GEN-7.** *Interfaces and generic methods.* Interfaces cannot declare generic methods. So a generic method never satisfies an interface method.

**GEN-8.** *Calling.* Type arguments may be explicit or inferred: `s.Map[string](f)` or `s.Map(f)`.

**GEN-9.** *Type arguments are checked against the constraints at the call.* `Swap[chan int](a, b)` with the `Copy` bound of `Swap` below is an error: a channel is a shared handle, not a `Copy` type.

**GEN-10.** *Syntax of the failable generic method.* `!` comes after the type parameters: `func (s Set) Parse[T any]!(text string) T`, called `s.Parse[int]!("5")`.

**GEN-11.** *`Send` and `Sync`* are predeclared constraints, written like `Copy` and `Clone`: `[T Send]`, `[T Sync]`. There is no type argument. They are computed by the compiler for every type ([CON-12](12-concurrency.md), ADR-0022, [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)) and cannot be implemented by hand. `[T Send]` means a `T` may be given to another goroutine, as a copy or by a move (for example in `go` or on a channel); `[T Sync]` means a `T` may be shared between goroutines concurrently. They can be embedded in a constraint interface like `Copy` and `Clone`: `interface{ Send; Clone }`. A generic body that gives a `T` to another goroutine or shares it between goroutines needs the constraint; the check is once per body (GEN-5), and the instance is checked again with the real type (P04.08). Plain `func(...)` types are both `Send` and `Sync` by default, and every ordinary `interface` type (`any`, `io.Reader`, etc.) is `Send` by default (an interface that embeds `Sync`, such as `error` or `interface{ Sync; Write([]byte) (int, error) }`, is both `Send` and `Sync`; `Local[T]` opts out of both for single-goroutine callbacks/interfaces, [CON-12](12-concurrency.md)). Example: `func Spawn[T Send](x T, f func(T)) { go f(move(x)) }`; `Spawn[*int]` and `Spawn[Local[func()]]` are errors (neither `*int` nor `Local[func()]` is `Send`).

**GEN-12.** *Inheritance and generics* (ADR-0023 decision 15, [ADR-0034](../adr/0034-extends-attribute.md)). A generic struct may be a parent or a child (`@Extends(A[T]) @Extends(Log[T]) type B[T any] struct { … }`, [TYP-16](03-types.md#35-inheritance-adr-0023)); marks and checks apply per instance like every generic rule ([GEN-5](#102-rules)). An interface declared with `@Extends(P)` is a constraint: a type satisfies it when it is `P` or has `P` as an ancestor, directly or indirectly. Below, "`T` extends `P`" means that.
```go
@Extends(Animal)
type AnimalKind interface{}

func Feed[T AnimalKind](a *T) { a.Eat(); a.Speak() }

@Extends(Payment)
@Extends(Auditable)
type AuditedPayment interface {
    Clone
}

func Both[T AuditedPayment](a *T) Unique[T] { a.Audit(); return a.Clone() } // Payment has a type table
```
- *Form.* `@Extends` goes before the declaration of one interface type ([TYP-16](03-types.md#35-inheritance-adr-0023)); several `@Extends` require `T` to extend all the parents. The body of the interface may hold methods, operator methods and embedded constraints next to it. `P` is a struct type, a generic one with arguments too (`@Extends(Box[U]) type BoxKind[U any] interface{}`), or a C++ class ([TYP-28](03-types.md#35-inheritance-adr-0023)). Such an interface is a constraint only: it is not the type of a value. There is no short form inside a type parameter list or inside an interface body: the constraint has a name.
- *In the body* (checked once, [GEN-5](#102-rules)): `*T` converts to `*P` (an upcast, the same borrow, [EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023)); the methods of `P` (operator methods too, [GEN-13](#102-rules)) and the fields of `P` are available on `T`. Nothing else of `T` is known. A call runs the version of the real type of the instance: `a.Speak()` in `Feed[Dog]` runs `Dog`'s `Speak`, and through a `*P` that really points to a `Dog` it dispatches dynamically as always ([EXP-14](06-expressions.md#69-upcasts-and-downcasts-adr-0023)). A write to a parent field through `*T` is allowed when the borrow rules allow it ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)). Every upcast (`*T` to `*P`, `Unique[T]` to `Unique[P]`, and `Shared`, `Weak`) and the downcast `p.(*T)` of a `*P` exist only when `P` has a type table; with a final-only `P` they are errors, as for a concrete child ([TYP-22](03-types.md#35-inheritance-adr-0023), ADR-0023 decision 16), and the fields and methods of `P` stay available on `T`. With a tagged `P`, `*a` has no value use ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023)) and `a.Clone()` is a `Unique[T]` holding a full copy of the dynamic type ([OWN-R6](04-ownership.md#42-ownership-rules-d14)); a tagged type does not satisfy `Copy`, since generic code copies a `T` through a `*T` freely.
- *Value slicing* stays an error: a `T` value does not convert to a `P` value ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023)). `var zero T` follows [GEN-5](#102-rules): it is an error when `P` has no zero value.
- *Instantiation.* `Feed[Dog]` and `Feed[Animal]` are accepted. `Feed[int]`, `Feed[Cat]` where `Cat` does not extend `Animal`, an interface type and a pointer type as `T` are errors, with the constraint named. `@Extends` may be combined with the elements of the interface body, but with a type-set term (`~int`) the type set is empty and that is an error, and two `@Extends` whose parents cannot share a child (for example a diamond or clashing final methods, TYP-16 / TYP-17) has no valid concrete instantiation.
- *Inference.* In `func Feed[T BoxKind[U], U any](a *T)`, with `BoxKind` as above, `U` is inferred from the argument: the unique ancestor of its type that is an instance of `Box` (an `IntBox` with the parent `Box[int]` gives `U = int`, also through generic parents; a type cannot extend two instances of `Box`, TYP-16) is unified with the bound.
- *Generic code cannot cut a child down to `T`.* What the generic code does with a type parameter `T` that the plain code decides on the real type is recorded as a *use* of `T` (its kind, and the method name where it matters): a copy, a move or an assignment through a pointer (a pointer of any kind to `T`); a value of `T` made or held (`var z T`, `T{...}`, `make([]T, n)`, an element of type `T`, and `T` held by value inside an instance, `Option[T]` or `Box[T]`, as for `Option[Shape]` in plain code; a pointer to `T` holds none); and a method of `T` whose receiver is taken by value (a method value `x.M`, `go` / `defer x.M()`, or a value receiver of a type that cannot be copied), on `x`, `(*p)` or `p`. Every instantiation decides each use on the real type argument with the rule of the plain code ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023), [TYP-19](03-types.md#35-inheritance-adr-0023)), so a generic body and the same code written for the type itself are rejected together: `*p` of a tagged `Animal` may be only the parent part of a `Dog`; `f := x.Val` copies the parent part with `T = Dog` when `Val` is inherited (as `g := dog.Val` does), and copies what the pointer points to with `T = *Animal`; an abstract type has no values. The uses are an implicit requirement, like Copy for a `~T` type set (GEN-1): every instantiation checks them (explicit, inferred, a function value, through another generic, a method of a generic type, in another package: the uses travel in the export data), the error (E0316, E0323 for a type) names the use, and a use passes on to a type parameter that is given for such a parameter. A direct call `x.M()` whose receiver is only read or is a pointer receiver, and a method expression `T.M` bind none (like `(*Animal).Val`), so both stay valid, as do pointer-receiver methods and `[T any]` swap code over untagged types.
- *Final-only parents.* If `P` has no type table, the limits of [TYP-22](03-types.md#35-inheritance-adr-0023) apply in the body: no upcast `*T` to `*P` (ADR-0023 decision 16), no `Unique[T]` to `Unique[P]` (and `Shared`, `Weak`) and no downcast, since the body is checked once for every `T`; the fields and methods of `P` stay available on `T`.
- *Not decided* (ADR-0023 open point 5): whether a C++ class derived from a C++ `P` in C++ satisfies a constraint with `@Extends(P)`; v1 default is no.

**GEN-13.** *Operators in constraints* ([ADR-0024](../adr/0024-operator-overloading.md)). A constraint interface lists operator methods using their normal Rust-style Go method signatures: `interface{ Add(T) T }`, `interface{ Eq(T) bool }`, `interface{ Cmp(T) int }`, `interface{ Index(int) T }`. A conversion marked `@Convert` is required by listing its ordinary method (`interface{ Float64() float64 }`).
- The body is checked once ([GEN-5](#102-rules)) with the operators the constraint lists, and with nothing else: `a + b` (and `a.Add(b)`) is valid if the constraint of `T` has `Add(T) T`. An instance calls the `@Operator` method on user types, or uses the built-in operator on built-in types.
- **Built-in types satisfy matching operator signatures with their built-in operators.** `int` and `float64` satisfy `Add(T) T`, `Sub(T) T`, `Mul(T) T`, `Div(T) T`, `Neg() T`, `Eq(T) bool` and `Cmp(T) int` (plus `Rem` and bitwise signatures on integers); `string` satisfies `Add(string) string`, `Eq(string) bool` and `Cmp(string) int`; `[]E`, `*[]E`, `[N]E` and `string` satisfy `Index(int) E`. `*[]E` and `*[N]E` satisfy `SetIndex(int, E)`; a by-value `[]E` does not, because a write through it would change a copy. A type-set constraint such as `~int | ~float64` gives the built-in operators as in Go.
- **Derived forms.** `Add(T) T` gives both `a + b` and `a += b` (`a = a + b`), while `AddAssign(T)` gives `a += b` only (`+` still requires `Add(T) T`). An `*Assign` element is met by a type with that `@Operator` method (its receiver is a pointer, and the body changes a place of type `T`) and by a built-in type with the compound operator; `Eq(T) bool` gives `a == b` and `a != b`; `Cmp(T) int` gives `a < b`, `a <= b`, `a > b` and `a >= b` ([EXP-19](06-expressions.md#610-operator-methods-adr-0024), [EXP-20](06-expressions.md#610-operator-methods-adr-0024); for built-in floats in a `Cmp(T) int` constraint body, NaN ordering follows `cmp.Compare`).
- **`comparable` and `Eq`.** `comparable` still means built-in equality. In a body with `[T comparable]`, `a == b` is the built-in comparison even for an instance whose type has `@Operator Eq`. Ask for the operator method with `interface{ Eq(T) bool }` instead. Map keys use built-in equality too.
- Elements are matched by exact signature: `Mul(float64) T` and `Mul(T) T` are two different elements, and a type must have both to satisfy both.
- A type that has `@Operator` methods of the element's name meets the element by those methods alone. A user type meets an operator constraint element when its `@Operator` method set has that operator signature (a value type needs a value receiver; a non-commutative `@Right` method serves the swapped order and meets no element), or when it has the built-in operator of that form (`Eq(T) bool` for any comparable type with no `@Operator Eq` of its own). A pointer type `*T` gets the `@Operator` methods of `T` only for `Index`, `SetIndex` and `Iter` ([EXP-17](06-expressions.md#610-operator-methods-adr-0024) rule 7): the signatures of the other elements name `T` itself (`Add(T) T`), and a `*T` is not a `T`.
- An interface that lists multiple overloaded signatures of the same method name (`interface{ Mul(T) T; Mul(float64) T }`) is a constraint-only interface in v1 ([TYP-27](03-types.md#31-type-syntax)).
- Operator elements can be mixed freely with other constraint elements: `interface{ Add(T) T; Clone }`.
- *Instantiation cycles on method signatures:* unlike Go (`go.dev/issue/48703`), a method's signature counts as a field does (`func (x G[T]) Next() Option[*G[*T]]` is the instantiation cycle `E0366`, while a method returning the same instance, a permutation of its parameters or another type is fine; Go mode keeps Go's rule). Only the receiver type's own parameters count (`func (l List[T]) Zip[U any](o List[U]) List[Pair[T, U]]` is legal; `func (l List[T]) Deep[U any]() { l.Deep[*U]() }` is `E0366` in the body). A method with type parameters of its own is instantiated where it is called, for the type arguments of its receiver too: inside `Deep`, `var x List[*T]; x.Deep[U]()` is `E0366`.

## 10.3 Examples

```go
// Works for any T, copyable or not: xs and v are only read, so no copy is made.
func Index[T comparable](xs []T, v T) int {
    for i := range xs {
        if xs[i] == v { return i }
    }
    return -1
}

// Returning an element copies it, so T must be copyable: Clone is required.
func First[T Clone](xs []T) T {
    return xs[0]                // a copy of the element; without the Clone bound: error
}
func Dup[T Clone](x T) (T, T) { return x, x }   // two values from one: one of them is a copy
a, b := Dup("hi")               // strings and numbers both satisfy Clone
c, d := Dup(5)

// Taking a value into a container: v is moved in, so no copy is made and every T works, copyable or not.
func (s *Stack[T]) Push(v T) { s.items = append(s.items, move(v)) }

// Copy: a T is copied by copying its bytes.
func Swap[T Copy](a, b *T) { a, b = *b, *a }

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
func Sum[T interface{ Add(T) T; Clone }](xs []T) T {    // xs must not be empty
    acc := xs[0]                    // a copy of the element: needs the Clone bound
    for i := 1; i < len(xs); i++ { acc = acc + xs[i] }
    return acc
}
Sum([]int{1, 2, 3})                 // ok: int has built-in +
Sum([]Vec2{{1, 2}, {3, 4}})         // ok: Vec2 has @Operator Add(Vec2) Vec2
Sum([]string{"a", "b"})             // ok: string + string
Sum([]bool{true})                   // error: bool does not satisfy Add(bool) bool
func Max[T interface{ Cmp(T) int }](a, b T) T { if a > b { return a }; return b }   // GEN-3: returning a parameter needs no move and no Clone bound
Max(Vec2{1, 0}, Vec2{2, 0})         // ok
Max(1, 2)                           // ok: int satisfies Cmp(int) int
Max("a", "b")                       // ok: string too
```

Compared with Go: generic methods are new (Go forbids them). In Go, `First[T any]` would just copy `xs[0]`; in Go++ the copy needs the `Clone` bound, which says that a `T` can be copied.

## 10.4 Rationale

- Operator constraints (GEN-13) let one `Sum` work for `int`, `Vec2` and `BigInt`, with no dictionary: every instance is monomorphized and calls the real operator.
- Reading a value makes no copy, and `move(v)` hands a value over, so most generic code needs no bound, for everyday types and for types that cannot be copied.
- `Copy` and `Clone` say in the signature that a generic body copies a `T`. They are written like `comparable`, so they are plain Go syntax.
- Full monomorphization gives C++-class speed and lets each instance know its type's real destructor.
- Generic methods were chosen by the owner (Q5). They need no dictionary because every use is monomorphized. They cannot satisfy interfaces, so dynamic dispatch stays simple.
