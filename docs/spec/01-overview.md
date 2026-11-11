# 1. Overview

Go++ is Go with ownership instead of a garbage collector. This spec is written as **the Go 1.25 specification plus a list of deltas** (decision D1.1).

- Base: the [Go 1.25 specification](https://go.dev/ref/spec).
- Everything not mentioned in this spec works as in Go.
- Source files are `.gopp`.

## 1.1 How to read this spec

- Each chapter covers one area. Each delta (D1.2 to D1.22 in the plan) has four parts: syntax (EBNF), rules, examples, rationale.
- Rules have stable IDs, so other documents can cite them. Examples: `OWN-R3a`, `PAR-4`, `ERR-6`.
- EBNF uses the notation of the [Go spec](https://go.dev/ref/spec#Notation). Only rules that change are shown.
- "Compile error" means the compiler must reject the program. "Panic" means a run-time panic. "Undefined behavior" (UB) is listed in [chapter 16](16-undefined-behavior.md).
- `gopp vet` warnings are not errors. A rule says so when vet is involved.

The P01 review (P01.15) is done. During the review, places where the plan was silent were marked with an `Open (P01 review)` note. The owner decided every one of them, and the notes are removed: the decided reading is now normal rule text. A point left to a later phase is marked `> **Deferred to Pxx:** …`.

## 1.2 Rule ID prefixes

| Prefix | Chapter |
|---|---|
| `LEX` | [02 lexical](02-lexical.md) |
| `TYP` | [03 types](03-types.md) |
| `OWN` | [04 ownership](04-ownership.md) (`OWN-C` categories, `OWN-R` rules 1 to 10 of D1.4, `OWN-P` pointers, `OWN-S` slicing, `OWN-B` borrow checking) |
| `PAR` | [05 parameters](05-parameters.md) |
| `EXP` | [06 expressions](06-expressions.md) |
| `STM` | [07 statements](07-statements.md) |
| `SLC`, `RNG` | [08 slices and range](08-slices-and-range.md) |
| `ERR`, `PAN` | [09 errors and panics](09-errors-and-panics.md) |
| `GEN` | [10 generics](10-generics.md) |
| `CLO` | [11 closures](11-closures.md) |
| `CON` | [12 concurrency](12-concurrency.md) |
| `MAC` | [13 macros surface](13-macros-surface.md) |
| `FOR` | [14 foreign surface](14-foreign-surface.md) |
| `INI` | [15 program init](15-program-init.md) |
| `UB` | [16 undefined behavior](16-undefined-behavior.md) |

## 1.3 What stays as in Go

These parts of the Go spec are unchanged unless a chapter says otherwise:

- Source representation: [Source code representation](https://go.dev/ref/spec#Source_code_representation), [Comments](https://go.dev/ref/spec#Comments), [Semicolons](https://go.dev/ref/spec#Semicolons), [Identifiers](https://go.dev/ref/spec#Identifiers).
- Packages: [Source file organization](https://go.dev/ref/spec#Source_file_organization), [Package clause](https://go.dev/ref/spec#Package_clause), [Import declarations](https://go.dev/ref/spec#Import_declarations), [Exported identifiers](https://go.dev/ref/spec#Exported_identifiers) (visibility by capital letter).
- Declarations: [Constant declarations](https://go.dev/ref/spec#Constant_declarations) (extended to structs, fixed arrays, `Option`, and `@Const` calls in [6.11](06-expressions.md#611-compile-time-evaluation-and-composite-constants-adr-0033)), [Iota](https://go.dev/ref/spec#Iota), [Type declarations](https://go.dev/ref/spec#Type_declarations), [Variable declarations](https://go.dev/ref/spec#Variable_declarations), [Short variable declarations](https://go.dev/ref/spec#Short_variable_declarations), [Blocks](https://go.dev/ref/spec#Blocks), [Declarations and scope](https://go.dev/ref/spec#Declarations_and_scope).
- Types and methods: [Struct types](https://go.dev/ref/spec#Struct_types) (without embedded fields, [TYP-25](03-types.md#35-inheritance-adr-0023); the attribute `@Extends` adds inheritance), [Interface types](https://go.dev/ref/spec#Interface_types) (structural), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Method sets](https://go.dev/ref/spec#Method_sets).
- Control flow: [If](https://go.dev/ref/spec#If_statements), [Switch](https://go.dev/ref/spec#Switch_statements), [For](https://go.dev/ref/spec#For_statements), [Select](https://go.dev/ref/spec#Select_statements), [Labeled statements](https://go.dev/ref/spec#Labeled_statements), [Goto](https://go.dev/ref/spec#Goto_statements), [Terminating statements](https://go.dev/ref/spec#Terminating_statements).
- Generics syntax: [Type parameter declarations](https://go.dev/ref/spec#Type_parameter_declarations), [Type constraints](https://go.dev/ref/spec#Type_constraints), [Instantiations](https://go.dev/ref/spec#Instantiations), [Type inference](https://go.dev/ref/spec#Type_inference).
- `init` functions: [Package initialization](https://go.dev/ref/spec#Package_initialization).
- Outside the language spec, also as in Go: build constraints (spelled `//gopp:build`, see [chapter 14](14-foreign-surface.md)) and the testing convention `TestXxx(t *testing.T)`.
- The memory model is [Go's](https://go.dev/ref/mem) (happens-before). See [chapter 12](12-concurrency.md).

## 1.4 Go vs Go++ at a glance

| Area | Go | Go++ | Chapter |
|---|---|---|---|
| Memory | tracing GC | no GC: every value has one owner and is destroyed deterministically; reference counts only for `string` buffers, `Shared[T]` and `Weak[T]`, channels and function values; compile-time borrow engine | 04 |
| `string`, `[]T`, `map` | GC-backed shared headers | `string`: immutable counted bytes ($O(1)$ copy and slice). `[]T` and `map[K]V`: one owner, no reference count; `s[i:j]` is a new slice and `&s[i:j]` is a view of part of `s` | 04, 08 |
| Assignment (`t := s`, `t = s`) | a slice, a map or a struct holding one shares its elements with the source | a copy for every type: nothing done to `t` is seen through `s`. The copy is skipped when the program cannot notice it (`s` is not used again) | 04 |
| Moving | not in the language | `move(x)` hands the value of `x` over without a copy; reading `x` afterwards is a compile error (`E0451`). A type that cannot be copied is passed on with `move(x)` or reached through `&x` | 04 |
| Sharing | slices, maps and pointers share | written with a type: a pointer (`&x`; `*[]T` and `*map[K]V` are ordinary pointer types, indexed without a star: `xs[0]`), `Shared[T]`, a channel | 04 |
| Parameters | all copy; a slice or map argument shares its elements with the caller | pure Go syntax: a parameter is the function's own copy, elements included; no copy is made when the function only reads it. A function that changes the caller's value takes `*T` and the caller writes `&`: `addOne(&nums)` | 05 |
| Built-ins and library functions that change their argument | take the slice by value: `copy(dst, src)`, `sort.Ints(xs)`, `r.Read(buf)` | take a pointer: `copy(&dst, src)`, `sort.Ints(&xs)`, `r.Read(&buf)`. `append(s, x)` takes `s` by value and returns the result | 05, 08 |
| `new`, `&T{}` | GC allocation | removed; `NewUnique`, `NewShared`, `NewWeak` | 04 |
| Errors | `val, err := f()` | same, plus `func f!()` and `call!()` | 09 |
| Closures | capture by reference | same; escaping closures use shared cells | 11 |
| Generics | stenciling | monomorphization; `Copy`, `Clone`; generic methods | 10 |
| Data races | limited damage | compile error in normal code; UB only in `unsafe` and C/C++ (UB-3) | 12, 16 |
| C interop | cgo | header imports `"<stdio.h>"` | 14 |
| Macros | `go generate` | `#Name(...)`, `@Name`, `macro` | 13 |
| Constants | `bool`, numbers, `string` only; no user calls | `const` also holds structs, `[N]T` arrays and `Option`s in `.rodata`; `@Const` functions/methods run at compile time in `const` and normally at runtime | 06 |
| Reuse of a struct | embedding (promotion) | embedding removed in `.gopp`; struct inheritance with `@Extends(P)`, one per parent (no diamonds), with `@Virtual` / `@Abstract` / `@Override` and `base` / `base[P]` | 03 |

Go code changes meaning in two places without a compile error ([ADR-0041](../adr/0041-values-are-copied.md)). A function that changes a slice or map it received by value now changes its own copy (`sort.Ints(nums)` becomes `sort.Ints(&nums)`). Two variables assigned from each other (`t := s`) no longer see each other's changes.

## 1.5 Delta index

| Decision | Topic | Chapter |
|---|---|---|
| D1.1 | Go 1.25 is the base | this chapter |
| D1.2 | lexical deltas | [02](02-lexical.md) |
| D1.3 | type categories | [04](04-ownership.md) (summary in [03](03-types.md)) |
| D1.4 | ownership rules 1 to 10 | [04](04-ownership.md) |
| D1.5 | parameters | [05](05-parameters.md) |
| D1.6 | slicing | [04](04-ownership.md), [08](08-slices-and-range.md) |
| D1.7 | pointers and heap allocation | [04](04-ownership.md) |
| D1.8 | errors and `!` | [09](09-errors-and-panics.md) |
| D1.9 | panic, defer, recover | [09](09-errors-and-panics.md) |
| D1.10 | generics | [10](10-generics.md) |
| D1.11 | closures | [11](11-closures.md) |
| D1.12 | interfaces and RTTI | [03](03-types.md) |
| D1.13 | concurrency | [12](12-concurrency.md) |
| D1.14 | macro and attribute surface | [13](13-macros-surface.md) |
| D1.15 | foreign import surface | [14](14-foreign-surface.md) |
| D1.16 | ten samples | [examples/](examples/) |
| D1.17 | safety defaults and layout | [06](06-expressions.md), [16](16-undefined-behavior.md) |
| D1.18 | program init and exit | [15](15-program-init.md) |
| D1.19 | errors work like Go | [09](09-errors-and-panics.md) |
| D1.20 | slicing and `:0` reset | [08](08-slices-and-range.md) |
| D1.21 | range loops | [08](08-slices-and-range.md) |
| D1.22 | allocators in slices and maps | [08](08-slices-and-range.md) |
| ADR-0032 | shared buffers and compile-time borrow engine | [04](04-ownership.md), [05](05-parameters.md), [08](08-slices-and-range.md) |
| ADR-0033 | `@Const` functions and composite `const` declarations | [06](06-expressions.md#611-compile-time-evaluation-and-composite-constants-adr-0033), [13](13-macros-surface.md) |
| ADR-0041 | a value is a copy; `move(x)`; pointers to slices and maps (replaces the shared slice and map buffers of ADR-0032) | [04](04-ownership.md), [05](05-parameters.md), [08](08-slices-and-range.md), [12](12-concurrency.md) |
| ADR-0042 | what a `Shared[T]` holds is read-only; changing it needs a `Mutex` | [04](04-ownership.md), [12](12-concurrency.md) |
| ADR-0043 | a pointer is used like the variable it points to; `*p = v` is not written | [04](04-ownership.md), [03](03-types.md), [06](06-expressions.md) |
| ADR-0044 | nothing is borrowed from what an arena value owns | [04](04-ownership.md), [08](08-slices-and-range.md) |
| ADR-0045 | a copy is written `=`; `Clone` is the copy method of a type with cleanup, a constraint, and the copy of a whole object through a parent pointer | [04](04-ownership.md), [10](10-generics.md) |

## 1.6 Key words used in this spec

- **Owner**: a variable, field, element or temporary that holds a value and drops its resources when it dies.
- **Copy**: what assignment, argument passing, returning, sending and converting to an interface give the destination: its own value, for every type ([ADR-0041](../adr/0041-values-are-copied.md)). The copy of a `[]T` or `map[K]V` is a new buffer with a copy of every element. A copy the program cannot notice is skipped: the last use of the source, or a parameter that is only read.
- **Move**: hand a value to a new place without copying it. It is written `move(x)` for a local variable or a parameter; `x` cannot be read before reassignment (`E0451`). A non-local slot extracted with `mem.Take` or `mem.Replace` receives the zero value or replacement.
- **Borrow**: temporary access to a place (`*T` via `&x`, a view `&s[i:j]` of part of a slice or array, `range` loop, arena or lock guard) without taking ownership or touching a reference count.
- **Copyable type (`OWN-C2`)**: a value that can be copied, where a copy is more than its bytes (`string`; `[]T`, `map[K]V` and `Unique[T]` whose parts can be copied; `Shared[T]`, `Weak[T]`, channels, interfaces, funcs; a type with `drop()` that defines `Clone()`; and aggregates of them).
- **Counted handle**: a copyable value whose copies refer to the same storage through a reference count (`string`, `Shared[T]`, `Weak[T]`, channels, funcs). A `[]T` or `map[K]V` is not one: it has one owner, and its copy has its own elements. An interface value is not one either: it owns the value it holds, and its copy holds a copy of that value.
- **Trivial type (`OWN-C1`)**: copied bit by bit, no destructor or reference count. It satisfies the `Copy` constraint.
- **Type that cannot be copied (`OWN-C3`)**: a type with `drop()` and no `Clone()`, a `@Pinned` type, a C++ class without a usable copy constructor, or an aggregate containing one. A plain copy is a compile error, also on the last use; write `move(x)` to hand it over or `&x` to borrow it.
- **Drop**: run cleanup glue when an initialized value dies (releasing counted handles, freeing slice and map buffers, or running a `drop` method and dropping fields).

## 1.7 Non-goals of v1

- Source compatibility with Go code that relies on GC.
- Rust-style lifetime annotations. Go++ checks borrows by flow and liveness without annotations ([4.5](04-ownership.md#45-borrow-checking-adr-0021)); what is left undefined is in [chapter 16](16-undefined-behavior.md).
- A user-facing `reflect` package.
- Exceptions as an error mechanism, and diamond inheritance (`virtual` base classes). (Inheritance without diamonds via `@Extends`, [TYP-16](03-types.md#35-inheritance-adr-0023), [ADR-0023](../adr/0023-inheritance.md), and Rust-style named `@Operator` methods, [EXP-16](06-expressions.md#610-operator-methods-adr-0024), [ADR-0024](../adr/0024-operator-overloading.md), are in v1. Unsafe and control-flow operators stay non-overloadable: `=`, `.`, `&x`, `,`, implicit conversions, `&&`/`||`, `<-`, `*x`, `()`, `++`/`--`, [EXP-29](06-expressions.md#610-operator-methods-adr-0024).)

## 1.8 Samples

Ten sample programs drive this spec (D1.16). They live in [examples/](examples/). Each has a `main.gopp`, a Go counterpart `main.go` and an `expected_output.txt`.
