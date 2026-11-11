# 10. Generics

Base: Go spec [Type parameter declarations](https://go.dev/ref/spec#Type_parameter_declarations), [Type constraints](https://go.dev/ref/spec#Type_constraints), [Satisfying a type constraint](https://go.dev/ref/spec#Satisfying_a_type_constraint), [Instantiations](https://go.dev/ref/spec#Instantiations), [Type inference](https://go.dev/ref/spec#Type_inference), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Interface types](https://go.dev/ref/spec#Interface_types).

Delta D1.10. Go's design is kept: type parameters, constraint interfaces with type sets, inference. Additions follow.

## 10.1 Syntax

Two grammar changes. The first: **methods may have type parameters** (generic methods). `MethodDecl` (with `TypeParameters` before the optional `!`) is defined in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18). The second: the **`extends` constraint form** ([GEN-12](#102-rules)):

```
TypeParamDecl = IdentifierList ( TypeConstraint | "extends" Type ) .     /* [T extends P] is short for [T interface{ extends P }] */
InterfaceElem = /* Go alternatives */ | "extends" Type .                /* GEN-12 */
```

A constraint interface may also list Rust-style operator method signatures (`interface{ Add(T) T; Cmp(T) int }`, [TYP-27](03-types.md#31-type-syntax), rule [GEN-13](#102-rules)).

`Copy` and `Clone` are built-in constraints, written like Go's `comparable`: `[T Copy]`, `[T Clone]`. There is no type argument: `[T Clone[T]]` and the short `[Clone[T]]` are not used (the short form is not valid Go syntax; `go vet` reports "missing type parameter name or invalid array length").

## 10.2 Rules

**GEN-1.** *`Copy`* is a predeclared constraint satisfied by types that copy for free, like numbers: bitwise-copyable types ([OWN-C1](04-ownership.md#41-type-categories-d13)). `Copy` does not imply `Clone` (see GEN-2). A type parameter whose type set lists only Copy types (`[T int | float64]`, `[T ~int]`, `[T int | *int]`) is Copy without writing `Copy`: it is decided on the real type set, in one place, for copying, returning and passing it to a `[U Copy]` function alike. `any`, `comparable` and a constraint with only methods list no types and are not Copy. A `~T` term also holds every defined type over `T`, and one of those may have a `drop` method (or be `@Pinned`): so a parameter that is Copy only through a type set with a `~T` term carries an implicit Copy requirement, and every instantiation must be Copy, checked like any constraint where the type arguments are given or inferred, in another package too, and through another generic (the outer parameter must be Copy or carry the same requirement): `dup[T ~int]` called with `type N int` that has `drop` is "N does not satisfy Copy, which the type set of T (~int) implies: N has a drop method". Exact terms (`N | int`) are judged as written.

**GEN-2.** *`Clone`* is a predeclared constraint. `[T Clone]` means the method set of `T` includes a method `Clone() T` with a value receiver ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). It is satisfied by every type that can be cloned, **numbers included**: trivial types, shared handles, owning types whose parts are cloneable, types with `drop` that define `Clone() T`, and imported C++ types with a usable copy constructor. A type whose `Clone` method has another signature does not satisfy it (the method itself stays legal). The same holds for a `Clone` inherited from a parent of an `extends` type ([TYP-17](03-types.md#35-inheritance-adr-0023)): it makes the compiler provide none, and the child satisfies `Clone` only if the inherited or declared method has the signature `Clone() T` for the child `T` ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). A `Clone` declared on `*T` itself, with signature `Clone() *T`, makes `*T` satisfy the constraint, and `x.Clone()` calls the user method. For `T = *Handle` where `Handle` has a value-receiver `Clone`, `[T Clone]` uses the pointer copy (instantiation is the only place where a raw `*T` uses it; `clone(p)` is the ambiguity error like `p.Clone()`), while a direct `p.Clone()` is the ambiguity error: generic code sees `*Handle`'s own method set. `Unique[T]` satisfies it only when `T` does. Pointer kinds satisfy it with their own `Clone` (a raw pointer copy, `Unique[T]` a deep clone, `Shared[T]` a retain), so `T = *Handle` works ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). A type parameter whose type set lists only cloneable types (`[T ~string | ~int]`) is `Clone` without writing it, decided on the real type set as for Copy (GEN-1), with the same implicit requirement on every instantiation when a `~T` term is involved (a `~[]int` type with `drop` and no `Clone()` does not satisfy it); `any`, `comparable` and a constraint with only methods are not.
- Both constraints can be embedded in a constraint interface like any Go interface: `interface{ Clone; cmp.Ordered }`.
- Go's own rule keeps them out of `|` unions, as it does for `comparable`.
- `Option[T]` satisfies `comparable` when `T` does (`eq[Option[int]](a, b)`); `Option[struct{ s []int }]` does not.
- A `Copy` type is also `Clone` (its `Clone()` is a plain copy), unless its method set already has a `Clone` with another signature ([OWN-R6](04-ownership.md#42-ownership-rules-d14)); then it is `Copy` but not `Clone`. Numbers, bools and the other built-in trivial types are always both. A generic body that needs both writes `[T interface{ Copy; Clone }]`. A `[T Copy]` body may not call `.Clone()` or pass `T` to a `[U Clone]` function, because GEN-5 checks the body once.

**GEN-3.** *Using a `T` in generic code.* A generic body is checked once against `T`'s constraint, conservatively assuming `T` may be single-owner (`OWN-C3`) unless constrained otherwise:
- if `T` is `Copy` (constrained by `Copy`, or with a type set of `Copy` types, GEN-1), `T` values copy freely everywhere;
- if `T`'s type set lists only `OWN-C1` and `OWN-C2` types (`[T ~int | ~string]`, `[K cmp.Ordered]`), `T` carries an implicit non-`OWN-C3` requirement on any `~U` term (rejecting defined types with `drop` or `@Pinned`, just as `GEN-1` and `GEN-2` do), and values of `T` lend, share and retain like `OWN-C2` with no `Copy` or `Clone` bound;
- otherwise, a local or parameter `v` of type `T` can be **read / lent** any number of times (`v == w`, `&v`, passing `&v`, pure `@Operator` / `@Convert` calls) and **transferred once on its last use** (`return v`, `s.items = append(s.items, v)`, `m[k] = v`, passing `v` by value to another function) with **no `Clone` bound** — at monomorphization time, `OWN-C1` copies, `OWN-C2` lends/retains or auto-moves, and `OWN-C3` moves; a non-keeping `[]T` parameter (`func Index[T comparable](xs []T, v T) int`) also lends `xs` read-only for every `T` ([PAR-5](05-parameters.md));
- **duplicating** a potentially `OWN-C3` `T` (transferring a local `v` by value and still reading `v` afterwards, or copying an element `xs[0]` out of a non-local place without `mem.Take` / `mem.Replace`) requires `[T Clone]` and an explicit `x.Clone()`.

**GEN-4.** *Returning from a slice or pointer.* In a generic body, `return v` for a by-value parameter `v T` works for `[T any]` (transferring `v` on its last use). Returning an element out of a slice or pointer (`return xs[0]`, `return *p`) without `Copy` requires `xs[0].Clone()` with `[T Clone]` (or `mem.Take(&xs[0])` when `T` is known to have a zero value, [OWN-R4](04-ownership.md#42-ownership-rules-d14)).

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

**GEN-11.** *`Send` and `Sync`* are predeclared constraints, written like `Copy` and `Clone`: `[T Send]`, `[T Sync]`. There is no type argument. They are computed by the compiler for every type ([CON-12](12-concurrency.md), ADR-0022, [ADR-0032](../adr/0032-shared-buffers-and-borrow-engine.md)) and cannot be implemented by hand. `[T Send]` means a `T` may be moved to another goroutine (for example in `go` or on a channel); `[T Sync]` means a `T` may be shared between goroutines concurrently. They can be embedded in a constraint interface like `Copy` and `Clone`: `interface{ Send; Clone }`. A generic body that moves or shares a `T` across goroutines needs the constraint; the check is once per body (GEN-5), and the instance is checked again with the real type (P04.08). Plain `func(...)` types are both `Send` and `Sync` by default, and every ordinary `interface` type (`any`, `io.Reader`, etc.) is `Send` by default (an interface that embeds `Sync`, such as `error` or `interface{ Sync; Write([]byte) (int, error) }`, is both `Send` and `Sync`; `Local[T]` opts out of both for single-goroutine callbacks/interfaces, [CON-12](12-concurrency.md)). Example: `func Spawn[T Send](x T, f func(T)) { go f(x) }`; `Spawn[*int]` and `Spawn[Local[func()]]` are errors (neither `*int` nor `Local[func()]` is `Send`).

**GEN-12.** *`extends` and generics* (ADR-0023 decision 15). A generic struct may be a parent or a child (`type B[T any] extends A[T], Log[T]`, [TYP-16](03-types.md#35-inheritance-adr-0023)); marks and checks apply per instance like every generic rule ([GEN-5](#102-rules)). The constraint `[T extends P]` means `T` is `P` or a type that extends `P`, directly or indirectly:
```go
func Feed[T extends Animal](a *T) { a.Eat(); a.Speak() }
func Both[T interface{ extends Payment; extends Auditable; Clone }](a *T) Unique[T] { a.Audit(); return a.Clone() } // Payment has a type table
```
- *Grammar.* In a type parameter list `T extends P` is short for `T interface{ extends P }`. `extends` is also an element of a constraint interface (multiple `extends` elements require `T` to extend all listed parents), next to methods, operator methods and type terms. It is contextual ([LEX-12](02-lexical.md#lex-12)). `P` is a struct type, a generic one with arguments too (`T extends Box[U]`), or a C++ class ([TYP-28](03-types.md#35-inheritance-adr-0023)).
- *In the body* (checked once, [GEN-5](#102-rules)): `*T` converts to `*P` (an upcast, the same borrow, [EXP-13](06-expressions.md#69-upcasts-and-downcasts-adr-0023)); the methods of `P` (operator methods too, [GEN-13](#102-rules)) and the fields of `P` are available on `T`. Nothing else of `T` is known. A call runs the version of the real type of the instance: `a.Speak()` in `Feed[Dog]` runs `Dog`'s `Speak`, and through a `*P` that really points to a `Dog` it dispatches dynamically as always ([EXP-14](06-expressions.md#69-upcasts-and-downcasts-adr-0023)). A write to a parent field through `*T` is allowed when the borrow rules allow it ([OWN-B1](04-ownership.md#45-borrow-checking-adr-0021)). Every upcast (`*T` to `*P`, `Unique[T]` to `Unique[P]`, and `Shared`, `Weak`) and the downcast `p.(*T)` of a `*P` exist only when `P` has a type table; with a final-only `P` they are errors, as for a concrete child ([TYP-22](03-types.md#35-inheritance-adr-0023), ADR-0023 decision 16), and the fields and methods of `P` stay available on `T`. With a tagged `P`, `*a` has no value use ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023)) and `a.Clone()` is a `Unique[T]` holding a full copy of the dynamic type ([OWN-R6](04-ownership.md#42-ownership-rules-d14)); a tagged type does not satisfy `Copy`, since generic code copies a `T` through a `*T` freely.
- *Value slicing* stays an error: a `T` value does not convert to a `P` value ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023)). `var zero T` follows [GEN-5](#102-rules): it is an error when `P` has no zero value.
- *Instantiation.* `Feed[Dog]` and `Feed[Animal]` are accepted. `Feed[int]`, `Feed[Cat]` where `Cat` does not extend `Animal`, an interface type and a pointer type as `T` are errors, with the constraint named. Elements may be combined, but `extends` with a type-set term (`~int`) has an empty type set and is an error, and combining two `extends` elements whose types cannot share a child (for example a diamond or clashing final methods, TYP-16 / TYP-17) has no valid concrete instantiation.
- *Inference.* In `func Feed[T extends Box[U], U any](a *T)`, `U` is inferred from the argument: the unique ancestor of its type that is an instance of `Box` (`IntBox extends Box[int]` gives `U = int`, also through generic parents; a type cannot extend two instances of `Box`, TYP-16) is unified with the bound.
- *Generic code cannot cut a child down to `T`.* What the generic code does with a type parameter `T` that the plain code decides on the real type is recorded as a *use* of `T` (its kind, and the method name where it matters): a copy, a move or an assignment through `*p` (a pointer of any kind to `T`); a value clone through a pointer (`(*p).Clone()`, `clone(...)`); a value of `T` made or held (`var z T`, `T{...}`, `make([]T, n)`, an element of type `T`, and `T` held by value inside an instance, `Option[T]` or `Box[T]`, as for `Option[Shape]` in plain code; a pointer to `T` holds none); and a method of `T` whose receiver is taken by value (a method value `x.M`, `go` / `defer x.M()`, or a single-owner value receiver), on `x`, `(*p)` or `p`. Every instantiation decides each use on the real type argument with the rule of the plain code ([EXP-15](06-expressions.md#69-upcasts-and-downcasts-adr-0023), [TYP-19](03-types.md#35-inheritance-adr-0023)), so a generic body and the same code written for the type itself are rejected together: `*p` of a tagged `Animal` may be only the parent part of a `Dog`; `f := x.Val` copies the parent part with `T = Dog` when `Val` is inherited (as `g := dog.Val` does), and copies what the pointer points to with `T = *Animal`; an abstract type has no values. The uses are an implicit requirement, like Copy for a `~T` type set (GEN-1): every instantiation checks them (explicit, inferred, a function value, through another generic, a method of a generic type, in another package: the uses travel in the export data), the error (E0316, E0323 for a type) names the use, and a use passes on to a type parameter that is given for such a parameter. A direct call `x.M()` on a lent/pointer receiver and a method expression `T.M` bind none (like `(*Animal).Val`), so both stay valid, as do pointer-receiver methods and `[T any]` swap code over untagged types.
- *Final-only parents.* If `P` has no type table, the limits of [TYP-22](03-types.md#35-inheritance-adr-0023) apply in the body: no upcast `*T` to `*P` (ADR-0023 decision 16), no `Unique[T]` to `Unique[P]` (and `Shared`, `Weak`) and no downcast, since the body is checked once for every `T`; the fields and methods of `P` stay available on `T`.
- *Not decided* (ADR-0023 open point 5): whether a C++ class derived from a C++ `P` in C++ satisfies `T extends P`; v1 default is no.

**GEN-13.** *Operators in constraints* ([ADR-0024](../adr/0024-operator-overloading.md)). A constraint interface lists operator methods using their normal Rust-style Go method signatures: `interface{ Add(T) T }`, `interface{ Eq(T) bool }`, `interface{ Cmp(T) int }`, `interface{ Index(int) T }`. A conversion marked `@Convert` is required by listing its ordinary method (`interface{ Float64() float64 }`).
- The body is checked once ([GEN-5](#102-rules)) with the operators the constraint lists, and with nothing else: `a + b` (and `a.Add(b)`) is valid if the constraint of `T` has `Add(T) T`. An instance calls the `@Operator` method on user types, or uses the built-in operator on built-in types.
- **Built-in types satisfy matching operator signatures with their built-in operators.** `int` and `float64` satisfy `Add(T) T`, `Sub(T) T`, `Mul(T) T`, `Div(T) T`, `Neg() T`, `Eq(T) bool` and `Cmp(T) int` (plus `Rem` and bitwise signatures on integers); `string` satisfies `Add(string) string`, `Eq(string) bool` and `Cmp(string) int`; `[]E`, `[N]E` and `string` satisfy `Index(int) E` (and `[]E` / `*[N]E` satisfy `SetIndex(int, E)`). A type-set constraint such as `~int | ~float64` gives the built-in operators as in Go.
- **Derived forms.** `Add(T) T` gives both `a + b` and `a += b` (`a = a + b`), while `AddAssign(T)` gives `a += b` only (`+` still requires `Add(T) T`); `Eq(T) bool` gives `a == b` and `a != b`; `Cmp(T) int` gives `a < b`, `a <= b`, `a > b` and `a >= b` ([EXP-19](06-expressions.md#610-operator-methods-adr-0024), [EXP-20](06-expressions.md#610-operator-methods-adr-0024); for built-in floats in a `Cmp(T) int` constraint body, NaN ordering follows `cmp.Compare`).
- **`comparable` and `Eq`.** `comparable` still means built-in equality. In a body with `[T comparable]`, `a == b` is the built-in comparison even for an instance whose type has `@Operator Eq`. Ask for the operator method with `interface{ Eq(T) bool }` instead. Map keys use built-in equality too.
- Elements are matched by exact signature: `Mul(float64) T` and `Mul(T) T` are two different elements, and a type must have both to satisfy both.
- A user type meets an operator constraint element when its `@Operator` method set has that operator signature (a value type needs a value receiver; a non-commutative `@Right` method serves the swapped order and meets no element), or when it has the built-in operator of that form (`Eq(T) bool` for any comparable type with no `@Operator Eq` of its own). A pointer type `*T` gets the `@Operator` methods of `T` only for the forms that look through a pointer (`Index`, `SetIndex`, `Iter`, [EXP-17](06-expressions.md#610-operator-methods-adr-0024) rule 7), so generic and plain code decide alike.
- An interface that lists multiple overloaded signatures of the same method name (`interface{ Mul(T) T; Mul(float64) T }`) is a constraint-only interface in v1 ([TYP-27](03-types.md#31-type-syntax)).
- Operator elements can be mixed freely with other constraint elements: `interface{ Add(T) T; Clone }`.
- *Instantiation cycles on method signatures:* unlike Go (`go.dev/issue/48703`), a method's signature counts as a field does (`func (x G[T]) Next() Option[*G[*T]]` is the instantiation cycle `E0366`, while a method returning the same instance, a permutation of its parameters or another type is fine; Go mode keeps Go's rule). Only the receiver type's own parameters count (`func (l List[T]) Zip[U any](o List[U]) List[Pair[T, U]]` is legal; `func (l List[T]) Deep[U any]() { l.Deep[*U]() }` is `E0366` in the body).

## 10.3 Examples

```go
// Works for any T, owning or not: xs is lent read-only and v is only compared.
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

// Taking a value into a container: pure Go syntax, works for every T (Copy, OWN-C2, and OWN-C3)!
func (s *Stack[T]) Push(v T) { s.items = append(s.items, v) }

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
func Sum[T interface{ Add(T) T; Clone }](xs []T) T {    // xs must not be empty
    acc := xs[0].Clone()
    for i := 1; i < len(xs); i++ { acc = acc + xs[i] }
    return acc
}
Sum([]int{1, 2, 3})                 // ok: int has built-in +
Sum([]Vec2{{1, 2}, {3, 4}})         // ok: Vec2 has @Operator Add(Vec2) Vec2
Sum([]string{"a", "b"})             // ok: string + string
Sum([]bool{true})                   // error: bool does not satisfy Add(bool) bool
func Max[T interface{ Cmp(T) int }](a, b T) T { if a > b { return a }; return b }   // GEN-3: each branch returns a or b on its last use!
Max(Vec2{1, 0}, Vec2{2, 0})         // ok
Max(1, 2)                           // ok: int satisfies Cmp(int) int
Max("a", "b")                       // ok: string too
```

Compared with Go: generic methods are new (Go forbids them). In Go, `First[T any]` would just copy `xs[0]`; in Go++ the `Clone` bound and the `Clone` call say that a copy happens.

## 10.4 Rationale

- Operator constraints (GEN-13) let one `Sum` work for `int`, `Vec2` and `BigInt`, with no dictionary: every instance is monomorphized and calls the real operator.
- Lending `OWN-C2` handles and `[]T` slices means most generic code needs no new syntax for everyday or owning types.
- `Copy` and `Clone` make hidden copies visible in the signature. They are written like `comparable`, so they are plain Go syntax.
- Full monomorphization gives C++-class speed and lets each instance know its type's real destructor.
- Generic methods were chosen by the owner (Q5). They need no dictionary because every use is monomorphized. They cannot satisfy interfaces, so dynamic dispatch stays simple.
