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
| `OWN` | [04 ownership](04-ownership.md) (`OWN-C` categories, `OWN-R` rules 1 to 10 of D1.4, `OWN-P` pointers, `OWN-V` refs) |
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
- Declarations: [Constant declarations](https://go.dev/ref/spec#Constant_declarations), [Iota](https://go.dev/ref/spec#Iota), [Type declarations](https://go.dev/ref/spec#Type_declarations), [Variable declarations](https://go.dev/ref/spec#Variable_declarations), [Short variable declarations](https://go.dev/ref/spec#Short_variable_declarations), [Blocks](https://go.dev/ref/spec#Blocks), [Declarations and scope](https://go.dev/ref/spec#Declarations_and_scope).
- Types and methods: [Struct types](https://go.dev/ref/spec#Struct_types) (with embedding), [Interface types](https://go.dev/ref/spec#Interface_types) (structural), [Method declarations](https://go.dev/ref/spec#Method_declarations), [Method sets](https://go.dev/ref/spec#Method_sets).
- Control flow: [If](https://go.dev/ref/spec#If_statements), [Switch](https://go.dev/ref/spec#Switch_statements), [For](https://go.dev/ref/spec#For_statements), [Select](https://go.dev/ref/spec#Select_statements), [Labeled statements](https://go.dev/ref/spec#Labeled_statements), [Goto](https://go.dev/ref/spec#Goto_statements), [Terminating statements](https://go.dev/ref/spec#Terminating_statements).
- Generics syntax: [Type parameter declarations](https://go.dev/ref/spec#Type_parameter_declarations), [Type constraints](https://go.dev/ref/spec#Type_constraints), [Instantiations](https://go.dev/ref/spec#Instantiations), [Type inference](https://go.dev/ref/spec#Type_inference).
- `init` functions: [Package initialization](https://go.dev/ref/spec#Package_initialization).
- Outside the language spec, also as in Go: build constraints (spelled `//gopp:build`, see [chapter 14](14-foreign-surface.md)) and the testing convention `TestXxx(t *testing.T)`.
- The memory model is [Go's](https://go.dev/ref/mem) (happens-before). See [chapter 12](12-concurrency.md).

## 1.4 Go vs Go++ at a glance

| Area | Go | Go++ | Chapter |
|---|---|---|---|
| Memory | tracing GC | owners, moves, destructors | 04 |
| `string`, `[]T`, `map` | shared references | owned values; `x.Clone()` copies (`clone(x)` is the short form) | 04, 08 |
| Parameters | all copy | owning types borrow; `move` takes ownership | 05 |
| `new`, `&T{}` | GC allocation | removed; `new_unique`, `new_shared`, `new_weak` | 04 |
| Errors | `val, err := f()` | same, plus `func f!()` and `call!()` | 09 |
| Closures | capture by reference | same; escaping closures use shared cells | 11 |
| Generics | stenciling | monomorphization; `Copy`, `Clone`; generic methods | 10 |
| Data races | limited damage | undefined behavior | 12, 16 |
| C interop | cgo | header imports `"<stdio.h>"` | 14 |
| Macros | `go generate` | `#Name(...)`, `@Name`, `macro` | 13 |

## 1.5 Delta index

| Decision | Topic | Chapter |
|---|---|---|
| D1.1 | Go 1.25 is the base | this chapter |
| D1.2 | lexical deltas | [02](02-lexical.md) |
| D1.3 | type categories | [04](04-ownership.md) (summary in [03](03-types.md)) |
| D1.4 | ownership rules 1 to 10 | [04](04-ownership.md) |
| D1.5 | parameters | [05](05-parameters.md) |
| D1.6 | refs | [04](04-ownership.md), [08](08-slices-and-range.md) |
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
| D1.20 | re-slicing in place | [08](08-slices-and-range.md) |
| D1.21 | range loops | [08](08-slices-and-range.md) |
| D1.22 | allocators in slices and maps | [08](08-slices-and-range.md) |

## 1.6 Key words used in this spec

- **Owner**: the variable, field, element or temporary that holds a value and destroys it.
- **Move**: give a value to a new owner. The old place becomes the zero value.
- **Borrow**: use a value without taking it. A borrow does not destroy anything.
- **Ref**: `ref string` or `ref []T`. A `(ptr, len)` borrow you can store. `ref` is a contextual keyword ([LEX-12](02-lexical.md)).
- **Shared handle**: a value whose copies share one thing through a reference count (`Shared[*T]`, channels, interfaces, funcs).
- **Trivial type**: copied bit by bit, no destructor. It satisfies the `Copy` constraint.
- **Owning type**: has a destructor (directly or through fields). Move-only.
- **Drop**: run the destructor (the `drop` method, then the fields).

## 1.7 Non-goals of v1

- Source compatibility with Go code that relies on GC.
- A borrow checker. Refs and raw pointers can dangle (see [chapter 16](16-undefined-behavior.md)).
- A user-facing `reflect` package.
- User-defined operator overloading, inheritance, exceptions as an error mechanism.

## 1.8 Samples

Ten sample programs drive this spec (D1.16). They live in [examples/](examples/). Each has a `main.gopp`, a Go counterpart `main.go` and an `expected_output.txt`.
