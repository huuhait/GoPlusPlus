# ADR-0020: `View[…]` replaces `ref`

- **Status:** accepted
- **Date:** 2026-10-28
- **Plan reference:** changes P01 D1.2 (contextual words), D1.6 (refs, now views) and ADR-0006 D5; spec LEX-2, LEX-7, LEX-12, TYP-1, TYP-2, TYP-9, OWN-R3b, OWN-V1 to OWN-V7 (section 4.4), PAR-2, PAR-8, PAR-9, SLC-8, MAC-8 and every chapter that names a ref; spec examples 02, 03, 06 and 10; new task P03.21; P02, P04, P07, P14 wording.

## Context

Go++ has a type for "a borrowed `(ptr, len)` window into a string or a slice". ADR-0003 and ADR-0006 (D5) spelled it with a contextual word in front of the type: `ref string`, `ref []T`.

On 2026-10-28 the owner asked to change it, for three reasons:

- `ref` looks like a C++ reference to any value (`T&`). It works only on `string` and slices, so the word promises more than the type gives.
- It is a style outlier. The other Go++ types that wrap a type use a predeclared generic name with the full type inside the brackets: `Option[T]`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`.
- A `ref []T` could write its elements, so "a view" was not a promise to the reader. The owner wants a type that means "nothing can change through this".
- `ref` needs special parser rules (LEX-12): it is a word that is a keyword in some places and a name in others, and a parameter named `ref` of type `string` is an error.

## Decision

1. **The borrow type is spelled `View[T]`.** The full type goes inside the brackets, as `Unique[*T]` shows the pointer. `ref string` becomes `View[string]` and `ref []T` becomes `View[[]T]`:

   | Old | New |
   |---|---|
   | `ref string` | `View[string]` |
   | `ref []T` | `View[[]T]` |
   | `ref []byte` | `View[[]byte]` |
   | (new) any other type `T` | `View[T]` |

   ```go
   func sum(xs View[[]int]) int
   func trim(s View[string]) View[string]
   w := s[0:5]              // still inferred: w is a View[string]
   ```

2. **A `View[T]` is read-only, all the way down.** This is like Rust's `&T`. Nothing can be changed through a `View[T]`: not its fields, not slice elements, not map entries, and not the target of a pointer reached through it. A value read out of a view is copied out as a plain value **only if its type is Copy and contains no pointer kind** (no `*T`, `Unique`, `Shared`, `Weak`, `unsafe.Pointer`, and no struct or array holding one): `n := v.Count` and `x := vs[0]` on a `View[[]int]` give an `int`. Everything else read through a view is a view: `v.Name` is a `View[string]`, `v.Tags` a `View[[]string]`, a struct field of struct type a `View[Inner]`, `v.Owner` (a `*User`) a `View[User]` of the target, and a Copy struct with a pointer field a `View[S]`. These are compile errors on a view or any part of it: assignment, `++`, `op=`, `delete`, `clear`, `copy` into it, `append`, `&` of a part, calling a pointer-receiver method, `for _, &e := range v` (it is `&` of a part), and passing a part to a `*T` or writable `[]T` parameter.

   **A view never converts to a non-view type,** except by the copy-out above or by `.Clone()`, `string(v)` or `[]T(v)`. So passing a view, or a part of one that is not a copy-out value, is an error for any parameter whose type is not a `View`: a `[]T`, a map, a `*T`, a struct containing these, and a plain `T` (PAR-1 makes a plain `T` parameter read-only only at the top level, so a borrowed `User` could still write `u.Tags[0]`); a value-receiver method call on a view part and a method value are errors for the same reason. There are two exceptions. A `string` parameter (immutable bytes, no write path). And boxing a view into an interface with no methods (`any`, `interface{}`) is allowed (`fmt.Println(w)`, `var a any = w`): the box keeps the dynamic type `View[T]`, and any type-switch or assertion binding to `View[T]` is a view, read-only all the way down, so nothing can be written through it. `View[T]` has no methods except the built-in `Clone` (OWN-R6), which every type has; it makes an owned copy. This matches the ban on receiver calls on a view part. So boxing it into `error`, `fmt.Stringer` or any interface with methods is an error. The box may not outlive the view's owner; that escape rule is in ADR-0021 (OWN-B5).

   **Range variables.** A range variable over a view (or a view part) is itself a view, read-only all the way down, with the same copy-out rule: `for _, d := range viewOfDocs { d.Tags[0] = "x" }` is an error. This is conservative for now: a later pass (P04.09) may relax it with a "writes this parameter" bit.

3. **`View[X]` is allowed for every type `X` except** the pointer kinds (`*T`, `Unique[*T]`, `Shared[*T]`, `Weak[*T]`), `unsafe.Pointer`, and `View[View[…]]`. Those are compile errors, and the message names the allowed forms. Named types are fine: `View[User]`, `View[Bytes]`. (This replaces the first draft of this ADR, which allowed only `string` and `[]T`.)
   - `View` is a predeclared generic type name, not a keyword. It follows the same scope rules as `Option` (LEX-7): it lives in the universe block, and a declaration with the same name in any scope, package level included, hides it (as for Go's predeclared names).
   - What `ref` was allowed or forbidden to do inside `Option` and generics stays the same for `View`.

   ```go
   func total(xs View[[]int]) int   // deep read-only
   func show(u View[User])          // u.Name = "x" and u.Tags[0] = "x" are errors
   ```

4. **Slicing.**
   - Slicing a `string` or a `View` gives a `View` (`View[string]`, `View[[]T]`).
   - Slicing a slice or array you may write (owned, or borrowed writable) gives a **borrowed `[]T`**, the same thing as a borrowed `[]T` parameter (PAR-1, PAR-2, OWN-R3b): elements can be written, it cannot grow (`append` is an error), cannot be moved, and cannot outlive its owner. So `sort.Ints(nums[1:3])`, `r.Read(buf[n:])`, `copy(dst[2:], src)` and `s := nums[1:3]; s[0] = 9` keep working.

5. **Results, fields and parameters.**
   - A function cannot return a writable borrowed part of a slice. To return a part without copying, the result type is `View[…]`. PAR-9 still applies: a result of owning type is a new owner.
   - A struct field of `View` type is a **compile error** (`err_view_field_without_attr`, a new code) unless the struct is marked `@View` (OWN-V5). It was a `gopp vet` warning in the first draft; the owner made it an error.
   - A `View[T]` parameter means "deep read-only". A plain `T` parameter is read-only only at the top level (PAR-1): a borrowed `[]int` parameter can still write elements, and a borrowed `User` can still write `u.Tags[0]`. Owned to `View` is implicit at a call.

6. **Temporary lifetime extension.** `w := strings.ToUpper(s)[0:3]` (and `var w = …`) is accepted. The compiler keeps the temporary in a hidden local until the end of the enclosing block, like Rust and C++. Only `:=` and `var` declarations get this. Storing a view or borrow of a temporary into a longer-lived place (a field, a global, an outer variable, a return) is still an error (OWN-V6, OWN-R8).

7. **Type switches.** `View[T]` and `T` are different dynamic types, for every `T`. `case T:` never matches a stored `View[T]`; only `case View[T]:` does. A boxed `View[string]` is not equal to a boxed `string`. Std code that prints values (`fmt`) handles `View[...]` cases explicitly (P13).

   **Layout.** A `View[T]` whose `T` has the underlying type `string` or a slice (`View[string]`, `View[[]T]`, `View[Bytes]`) is `(ptr, len)`. Every other `View[T]` is one pointer. All views are trivial.

   **Rules changed by this ADR:** LEX-2, LEX-7, LEX-12 (the `ref` word and the `View` name), TYP-1, TYP-2, TYP-9, TYP-13, OWN-C1, OWN-R3b (item 4 and the new item 6), OWN-V1, OWN-V2, OWN-V3, OWN-V5, OWN-V6, OWN-V7, PAR-2, PAR-8, PAR-9, PAR-10, GEN-2a (new), SLC-1, SLC-8, SLC-12, SLC-13, RNG-1, RNG-1a, CON-2, UB-1 and MAC-8. The rest are unchanged: OWN-V4 and V4a (map keys), the other PAR, GEN, UB and SLC rules. In prose the spec says "view" where it said "ref".

8. **The contextual word `ref` is removed** (LEX-2, LEX-12). `ref` is an ordinary identifier again, so Go code that uses `ref` as a name works. `move` stays contextual.

9. **The struct attribute `@Ref` (OWN-V5) is renamed `@View`.** The MAC-8 table is updated: a struct with a view field must carry `@View` (point 5). `@Ref` is an unknown attribute.

10. **Errors and printed types** use `View[...]`: in messages, hover text, `gopp doc` and the debugger pretty printers.

11. **Go mode** (`.go` files): none of the Go++ predeclared names exist there (`Option`, `Unique`, `Shared`, `Weak`, `View` and the others of LEX-7); they are ordinary names. `ref` is an ordinary name there too.

12. **No `gopp fix` migration.** There are no users yet.

13. **Grammar.** The production `RefType` is removed from the EBNF. `View[X]` is an ordinary `TypeName TypeArgs`, so the parser needs no special case. The checker builds the view type when it resolves the predeclared name `View`.

## Consequences

- One style for all wrapper types: `Option[T]`, `Unique[*T]`, `View[string]`. The parser and the lexer lose a special case (LEX-12 is shorter), and `ref` is free as a name.
- Views are safe to pass around: a callee that takes `View[T]` cannot change anything the caller owns, so code review can rely on the type. The price: read-only std functions must take `View[[]byte]`, not `[]byte`, if they should accept views (P13).
- Slicing a writable slice gives a borrowed `[]T`, which the compiler must keep from growing, moving or outliving its owner (a new item in OWN-R3b).
- A view stored in a struct needs `@View` on the struct, so the risk is visible where the struct is declared.
- Signatures are longer: `View[string]` is 12 characters, `ref string` was 10.
- `View[X]` needs a validity check for `X` in the checker (new error codes, `gopp explain` texts). The grammar cannot enforce it.
- The word "view" was the term in the first drafts (before ADR-0006 D5 renamed it to `ref`), so old notes that say "view" now match again.
- ADR-0003, ADR-0006 and ADR-0008 keep their old text as history. Their Status lines point here.
- Every test and every spec example that uses `ref` types is rewritten (task P03.21).

## Alternatives

- **Keep `ref`.** Short, and already in the spec and tests. Rejected by the owner: it is a new word, it looks like a C++ reference, and its style differs from `Option[T]` and `Unique[*T]`.
- **`view []T` (a word in front of the type).** A clearer word than `ref`, but it keeps the same problem: a contextual word with special parser rules, and a style that differs from the generic wrappers.
- **`View[int]` with only the element type** (`View[T]` is a view of a slice of `T`). Shorter, but `View[int]` reads as "a view of one int", and a view of a `[]string` versus a view of a `string` becomes unclear (`View[byte]` could mean bytes of a string or of a `[]byte`).
- **Keep `ref []T` writable (views that can write elements).** This was the first design. Rejected by the owner: "a view" should be a promise that nothing changes.
- **Rust-style `&[]T` / `&str`.** In Go, `&` already means "take the address", so `&[]T` would read as a pointer to a slice.
