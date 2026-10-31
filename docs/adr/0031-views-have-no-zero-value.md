# ADR-0031: A view of a single value has no zero value

- **Status:** accepted
- **Date:** 2026-10-31
- **Plan reference:** P04.08 review round 4, finding R10; spec OWN-V1 and OWN-R9 ([04-ownership.md](../spec/04-ownership.md)); related ADR-0018 (pointers are never nil, `Option[T]`) and ADR-0020 (views).

## Context

A `View[T]` of a single value (`View[int]`, `View[User]`, `View[[2]int]`, `View[map[K]V]`) is a pointer to the value it looks at. A `View[string]` or a `View[[]T]` is a `{ptr, len}` pair.

Until now every view had a zero value. For a view of a string or a slice that is the empty view. For a view of a single value it was a **null pointer**. The checker accepted `var v View[int]` and a zero `@View` struct. Reading such a view, or comparing two of them, read through the null pointer, and the program crashed. Review round 4 of P04.08 found it (R10): once a view compared as the value it looks at (OWN-V, R7), `a == b` of two zero `@View` structs was a segmentation fault where master printed `true`.

The owner was shown three options:

```go
var v View[int]
println(int(v) == 0)
```

- **A. A view of a single value has no zero value**, as a pointer kind has none (ADR-0018, OWN-R9). `var v View[int]` is a compile error. A view that may be missing is `Option[View[int]]`, whose nil is the null pointer, as for `Option[*T]`.
- **B. A zero view looks at the zero value of its type** (a hidden static zero per type). `var v View[int]` reads `0`.
- **C. A nil view that panics on read.** Every read and comparison of a view checks for null first.

## Decision

**Option A.** The owner's words: "VIEW means it is looked at something so it must come from smth".

1. A `View[T]` where `T` is neither a string nor a slice type has **no zero value**. It always looks at a value. A view of a type parameter has none unless every type its constraint allows is a string or a slice.
2. Everywhere a type with no zero value is an error, such a view is one too, with the same rule-9 machinery of the checker (OWN-R9, `Checker::hasZeroValue`):
   - `var v View[int]` (E0333, with the fix: set it from a value, `View[int](x)`, or use `Option[View[int]]`);
   - a struct or an array that holds such a view: `var w Win`, `var a [2]View[int]`, and `Win{}` without the field (E0335). This includes a `@View` struct;
   - `make([]View[int], n)` with a length that is not the constant `0` (E0333);
   - `m[k]` and `<-ch` of such a value outside a comma-ok form (E0334), and the comma-ok value used where `ok` may be false (E0350, OWN-P10);
   - `var zero T` in generic code where `T` may be one (GEN-5), and an instance of a generic type that holds one, such as `var b Box[View[int]]`;
   - named results, a receive from a closed channel and the other cases of OWN-R9.
3. `Option[View[T]]` is the "maybe" form. Its nil is the null pointer, as for `Option[*T]`. `v, ok := o.Get()` works.
4. `View[string]` and `View[[]T]` keep their zero value: the empty view. A view of a string or a slice looks at a **range** of items, and a range of zero items is valid: reading it reads nothing (`len` is 0, a loop over it runs no time), so no read can fault. A view of a single value must read that value, so it cannot be empty. The owner was shown this case explicitly and accepted it once this reason was explained.
5. Moving a view copies it and empties nothing (OWN-V1). So moving a view out of a place never leaves a zero view, and such a move is not the "would be left as zero bytes" error (E0341) of a pointer.

```go
type P struct{ N int }

@View
type Win struct{ Cur View[P] }

func f(x int, p P, m map[int]View[int]) {
	var v View[int]                 // error: View[int] has no zero value
	var w Win                       // error: field Cur of type View[P] has none
	w2 := Win{}                     // error: missing required field Cur
	s := make([]View[int], 3)       // error: make creates zero values
	r := m[1]                       // error: m[1] may not exist (comma-ok form)

	ok1 := View[int](x)             // ok: it looks at x
	ok2 := Win{Cur: View[P](p)}     // ok
	var o Option[View[int]]         // ok: nil
	o = ok1
	if u, set := o.Get(); set {
		println(int(u))             // 3 for x == 3
	}
	var sv View[string]             // ok: empty
	var sl View[[]int]              // ok: empty
	s2 := make([]View[int], 0, 3)   // ok: no zero values are made
	if r2, found := m[1]; found {
		println(int(r2))
	}
}
```

## Consequences

- No view of a single value is ever null, so reading one (`int(v)`, `v.N`, `println(v)`) and comparing two (OWN-V) never fault. No null check is emitted.
- The checker reuses its zero-value machinery: the view case of `ZeroQuery` (Ownership.cpp) answers "none" for a view of a single value. Every rule that asks `hasZeroValue` follows: the declarations, composite literals, `make`, map reads and receives, the comma-ok flow (OWN-P10), named results, generics and the per-instance checks of P04.08. The error codes are the pointer kinds' (E0333, E0334, E0335, E0350). No new code was needed.
- The `move` rule for a place of a type with no zero value (E0341) exempts a view variable, which `move` copies (OWN-V1). Without this, swapping views through pointers (P03.26) would become an error.
- `Option[View[T]]` costs nothing over a view: its nil is the null pointer.
- Programs that declared a zero view of a single value no longer compile. In the test suite, two tests are rewritten:
  - `sema/extends-generic-plain-table.test`: the `zero` row of the view column is rejected on both sides;
  - the `view-uncomparable` panic program (`tests/run/Inputs/panics/`): its type-assertion binding, a `View[K]`, is now used only where `ok` is true.

  `sema/view-repoint-move.test` is unchanged and still passes, because a view's move copies.
- Tests: `sema/rules/ADR-0031-example.test` (the forms above, both sides) and `run/view-option.test` (the accepted forms run: `Option[View[int]]` nil and set, a `@View` struct, empty string and slice views, the comma-ok read, `make` with length 0, and non-zero views comparing by value).

## Alternatives

- **B: the zero view looks at a zero value. Rejected.** It needs a hidden static zero value for every type, and it hides a mistake: a view that was never set reads as `0` instead of being an error. It is also not what "a view" means. A view looks at something that exists.
- **C: a nil view that panics on read. Rejected.** It puts a null check before every read and every comparison of a view. It also turns a mistake the compiler can see into a run-time panic. `Option[View[T]]` already gives "maybe nothing" with a check the type system enforces, as `Option[*T]` does for pointers (ADR-0018).
