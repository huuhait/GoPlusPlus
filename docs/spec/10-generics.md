# 10. Generics

Base: Go spec [Type parameter declarations](https://go.dev/ref/spec#Type_parameter_declarations), [Type constraints](https://go.dev/ref/spec#Type_constraints), [Satisfying a type constraint](https://go.dev/ref/spec#Satisfying_a_type_constraint), [Instantiations](https://go.dev/ref/spec#Instantiations), [Type inference](https://go.dev/ref/spec#Type_inference), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Interface types](https://go.dev/ref/spec#Interface_types).

Delta D1.10. Go's design is kept: type parameters, constraint interfaces with type sets, inference. Additions follow.

## 10.1 Syntax

Only one grammar change: **methods may have type parameters** (generic methods). `MethodDecl` (with `TypeParameters` before the optional `!`) is defined in [9.1](09-errors-and-panics.md#91-functions-that-can-fail-d18).

`Copy` and `Clone` are built-in constraints, written like Go's `comparable`: `[T Copy]`, `[T Clone]`. There is no type argument: `[T Clone[T]]` and the short `[Clone[T]]` are not used (the short form is not valid Go syntax; `go vet` reports "missing type parameter name or invalid array length").

## 10.2 Rules

**GEN-1.** *`Copy`* is a predeclared constraint satisfied by types that copy for free, like numbers: bitwise-copyable types ([OWN-C1](04-ownership.md#41-type-categories-d13)). `Copy` does not imply `Clone` (see GEN-2).

**GEN-2.** *`Clone`* is a predeclared constraint. `[T Clone]` means the method set of `T` includes a method `Clone() T` with a value receiver ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). It is satisfied by every type that can be cloned, **numbers included**: trivial types, shared handles, owning types whose parts are cloneable, types with `drop` that define `Clone() T`, and imported C++ types with a usable copy constructor. A type whose `Clone` method has another signature does not satisfy it (the method itself stays legal). The same holds for a `Clone` promoted from an embedded field: it makes the compiler provide none, and the outer type satisfies `Clone` only if the promoted or declared method has the signature `Clone() T` for the outer `T` ([OWN-R6](04-ownership.md#42-ownership-rules-d14)). A `Clone` declared on `*T` itself, with signature `Clone() *T`, makes `*T` satisfy the constraint, and `x.Clone()` calls the user method. For `T = *Handle` where `Handle` has a value-receiver `Clone`, `[T Clone]` uses the pointer copy (instantiation is the only place where a raw `*T` uses it; `clone(p)` is the ambiguity error like `p.Clone()`), while a direct `p.Clone()` is the ambiguity error: generic code sees `*Handle`'s own method set. `Unique[*T]` satisfies it only when `T` does. Pointer kinds satisfy it with their own `Clone` (a raw pointer copy, `Unique[*T]` a deep clone, `Shared[*T]` a retain), so `T = *Handle` works ([OWN-R6](04-ownership.md#42-ownership-rules-d14)).
- Both constraints can be embedded in a constraint interface like any Go interface: `interface{ Clone; cmp.Ordered }`.
- Go's own rule keeps them out of `|` unions, as it does for `comparable`.
- `Option[T]` satisfies `comparable` when `T` does (`eq[Option[int]](a, b)`); `Option[struct{ s []int }]` does not.
- A `Copy` type is also `Clone` (its `Clone()` is a plain copy), unless its method set already has a `Clone` with another signature ([OWN-R6](04-ownership.md#42-ownership-rules-d14)); then it is `Copy` but not `Clone`. Numbers, bools and the other built-in trivial types are always both. A generic body that needs both writes `[T interface{ Copy; Clone }]`. A `[T Copy]` body may not call `.Clone()` or pass `T` to a `[U Clone]` function, because GEN-5 checks the body once.

**GEN-3.** *Copying a `T`.* A generic body may copy a value of type `T` only if `T` is `Copy`. Otherwise it may only:
- **borrow** it (a borrowed parameter, a read, a comparison);
- **move** it (a `move` parameter, `move(x)`);
- **`Clone`** it (requires `T` to satisfy `Clone`).

**GEN-4.** *Returning a borrowed value.* In a generic body, `return a` for a borrowed parameter of non-`Copy` `T` needs `a.Clone()`, which requires `Clone` ([OWN-R3a](04-ownership.md#42-ownership-rules-d14)). Without the constraint it is a compile error.

**GEN-5.** *Monomorphization.* Every instantiation is compiled as its own function with the real types (P04). Consequences you can see:
- there is no run-time dictionary and no boxing of type arguments;
- ownership checks run on the generic body once (with the rules above), and the instance has the real category of each type argument;
- `var zero T` is a compile error if `T` may be a type with no zero value (a pointer kind, a func, an interface with methods other than `error`, a C++ class without a default constructor, a struct with a `@Required` field); `var zero Option[T]` works for every `T` that `Option` accepts (checked per instance, P04.08) ([OWN-R9](04-ownership.md#42-ownership-rules-d14)). Constrain `T` or use `Option[T]`.

**GEN-6.** *Generic methods are allowed.* A method may have its own type parameters, as in `func (s Set) Map[U any](f func(int) U) []U`. Each use is monomorphized like a generic function. The receiver's own type parameters (for example `func (l *List[T]) …`) work as in Go.

**GEN-7.** *Interfaces and generic methods.* Interfaces cannot declare generic methods. So a generic method never satisfies an interface method.

**GEN-8.** *Calling.* Type arguments may be explicit or inferred: `s.Map[string](f)` or `s.Map(f)`.

**GEN-9.** *Type arguments are checked against the constraints at the call.* `Swap[chan int](a, b)` with the `Copy` bound of `Swap` below is an error: a channel is a shared handle, not a `Copy` type.

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

Compared with Go: generic methods are new (Go forbids them). In Go, `First[T any]` would just copy `xs[0]`; in Go++ the `Clone` bound and the `Clone` call say that a copy happens.

## 10.4 Rationale

- Borrowing parameters means most generic code needs no new syntax for owning types.
- `Copy` and `Clone` make hidden copies visible in the signature. They are written like `comparable`, so they are plain Go syntax.
- Full monomorphization gives C++-class speed and lets each instance know its type's real destructor.
- Generic methods were chosen by the owner (Q5). They need no dictionary because every use is monomorphized. They cannot satisfy interfaces, so dynamic dispatch stays simple.
