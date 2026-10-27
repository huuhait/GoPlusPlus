# 10. Generics

Base: Go spec [Type parameter declarations](https://go.dev/ref/spec#Type_parameter_declarations), [Type constraints](https://go.dev/ref/spec#Type_constraints), [Satisfying a type constraint](https://go.dev/ref/spec#Satisfying_a_type_constraint), [Instantiations](https://go.dev/ref/spec#Instantiations), [Type inference](https://go.dev/ref/spec#Type_inference), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Interface types](https://go.dev/ref/spec#Interface_types).

Delta D1.10. Go's design is kept: type parameters, constraint interfaces with type sets, inference. Additions follow.

## 10.1 Syntax

Only one grammar change: **methods may have type parameters** (generic methods). `MethodDecl` (with `TypeParameters` before the optional `!`) is defined in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18).

`trivial` and `clonable` are predeclared constraints, used like `comparable`: `[T trivial]`, `[T clonable]`.

## 10.2 Rules

**GEN-1.** *`trivial`* is a predeclared constraint satisfied by bitwise-copyable types ([OWN-C1](04-ownership.md#41-type-categories-d13)).

**GEN-2.** *`clonable`* is a predeclared constraint satisfied by the types that `clone` accepts ([OWN-R6](04-ownership.md#42-ownership-rules-d14)): trivial types, shared handles, owning types whose parts are clonable, types with `drop` that define `Clone() T`, and imported C++ types with a usable copy constructor. Both constraints can be embedded in a constraint interface: `interface{ clonable; cmp.Ordered }`. Neither can be part of a union.

> **Open (P01 review):** embedding `trivial`/`clonable` in a constraint interface and the ban on unions are this spec's readings; D1.10 only says they are predeclared constraints.

> **Open (P01 review):** the plan does not say whether `trivial` types satisfy `clonable`. This spec reads it as yes (see the Open note at [OWN-R6](04-ownership.md#42-ownership-rules-d14)).

**GEN-3.** *Copying a `T`.* A generic body may copy a value of type `T` only if `T` is `trivial`. Otherwise it may only:
- **borrow** it (a borrowed parameter, a read, a comparison);
- **move** it (an `own` parameter, `move(x)`);
- **`clone`** it (requires `T` to be `clonable`).

**GEN-4.** *Returning a borrowed value.* In a generic body, `return a` for a borrowed parameter of non-`trivial` `T` needs `clone(a)`, which requires `clonable` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Without the constraint it is a compile error.

**GEN-5.** *Monomorphization.* Every instantiation is compiled as its own function with the real types (P04). Consequences you can see:
- there is no run-time dictionary and no boxing of type arguments;
- ownership checks run on the generic body once (with the rules above), and the instance has the real category of each type argument;
- `var zero T` is a compile error if `T` may be a C++ class without a default constructor ([OWN-R9](04-ownership.md#42-ownership-rules-d14)). Constrain `T` or use `*T`.

**GEN-6.** *Generic methods are allowed.* A method may have its own type parameters, as in `func (s Set) Map[U any](f func(int) U) []U`. Each use is monomorphized like a generic function. The receiver's own type parameters (for example `func (l *List[T]) …`) work as in Go.

**GEN-7.** *Interfaces and generic methods.* Interfaces cannot declare generic methods. So a generic method never satisfies an interface method.

**GEN-8.** *Calling.* Type arguments may be explicit or inferred: `s.Map[string](f)` or `s.Map(f)`.

**GEN-9.** *Type arguments are checked against the constraints at the call.* `Swap[chan int](a, b)` with the `trivial` bound of `Swap` below is an error: a channel is a shared handle, not trivial.

**GEN-10.** *Syntax of the failable generic method.* `!` comes after the type parameters: `func (s Set) Parse[T any]!(text string) T`, called `s.Parse[int]!("5")`.

## 10.3 Examples

```go
// Works for any T, owning or not: both arguments are borrowed.
func Index[T comparable](xs []T, v T) int {
    for i := range xs {
        if xs[i] == v { return i }
    }
    return -1
}

// A copy is needed to return a borrowed value, so clonable is required.
func First[T clonable](xs []T) T {
    return clone(xs[0])        // without clone: error (OWN-R3a); without clonable: error
}

// Taking ownership: no clone, the caller moves in.
func (s *Stack[T]) Push(v own T) { s.items = append(s.items, v) }

// Trivial only: a plain copy is allowed.
func Swap[T trivial](a, b *T) { *a, *b = *b, *a }

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

Compared with Go: generic methods are new (Go forbids them). In Go, `First[T any]` would just copy `xs[0]`; in Go++ the `clonable` bound and the `clone` call say that a copy happens.

## 10.4 Rationale

- Borrowing parameters means most generic code needs no new syntax for owning types.
- `trivial` and `clonable` make hidden copies visible in the signature.
- Full monomorphization gives C++-class speed and lets each instance know its type's real destructor.
- Generic methods were chosen by the owner (Q5). They need no dictionary because every use is monomorphized. They cannot satisfy interfaces, so dynamic dispatch stays simple.
