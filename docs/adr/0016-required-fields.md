# ADR-0016: Required struct fields (`@Required`)

- **Status:** accepted (integrated with [ADR-0018](0018-never-nil-pointers-and-option.md), [ADR-0023](0023-inheritance.md) and [ADR-0032](0032-shared-buffers-and-borrow-engine.md))
- **Date:** 2026-10-27
- **Plan reference:** P01 D1.4 rules 9 and 11; spec TYP-15, OWN-R9, MAC-8, MAC-12; task P03.15; P09.08, P13.08. Decides P01 Q9.

## Context

Go has no way to force a struct field to be set: `User{Email: "a@b.c"}` silently leaves `ID` at 0, and `var u User` is always allowed. The Go idiom (private fields plus a `NewUser` constructor) doesn't stop `User{}` inside the same package or capture intent in public data structs.

On 2026-10-27 the owner chose a built-in field attribute, `@Required`, over new syntax, a constructor convention, or a vet warning, using the strict rule: a struct with a required field has no zero value.

## Decision

1. **`@Required` is a built-in attribute on struct fields** (MAC-8), written after the field's type like any field attribute (MAC-12): `ID int @Required`. The compiler handles it; it is not a macro.
2. **Every composite literal of the struct must set each required field,** by name or by position (a positional literal sets every field anyway). `User{Email: "x"}` is a compile error that names the missing fields. `@Required` is only allowed on struct fields (inherited by child types under `extends`, TYP-17); anywhere else it is an error.
3. **A struct with a required field has no zero value.** It joins the types with no zero value (OWN-R9). Every place where Go would materialize a zero value is a compile error:
   - `var u User` with no initializer, and `var zero T` in generic code instantiated with `User`;
   - `make([]User, n)`, unless `n` is the constant `0` (`make([]User, 0, 100)` reserves room and creates no `User`);
   - `var a [3]User`, an array literal that leaves elements out (`[3]User{{…}}`), and a keyed slice or array literal with gaps (`[]User{5: {…}}` would create elements 0 to 4);
   - named results of type `User`, a failing `!` in a Go-style `(User, error)` function not declared with `!`, and any single-value read of `m[k]` or `<-ch` other than a comma-ok form (point 9 below);
   - `clear(s)` on a slice of `User` (Go's `clear` keeps the length and zeroes the elements; the error suggests `s = s[:0]`, which drops them). `clear(m)` on a map is fine: it creates no values;
   - `NewUnique(User{})` and other literals, through rule 2.
4. **It spreads to containing types.** A struct or array whose field or element type has no zero value has none either. A field of type `User` inside `Team` must be set in every `Team` literal, as if it were `@Required`. For generic types it is a property of the instance: `Box[User]` has no zero value, `Box[int]` has one.
5. **Extracting from places (`mem.Take`, `mem.Replace`, swaps).** A moved-from local holds all-zero bytes (OWN-R5); reading it before reassignment is a use-after-move compile error (`E0451`). `mem.Take(&place)` is a compile error for any type with no zero value (and on any non-local place of a `@Required` struct), because `place` cannot be left holding a zero value; use `mem.Replace(&place, v)`, `delete(m, k)`, `s = s[:n]`, or a parallel swap (`xs[i], xs[j] = xs[j], xs[i]`, `c.A, c.B = c.B, c.A`), where all index and pointer operands and their bounds checks are evaluated before any value moves.

   ```go
   old := mem.Replace(&users[0], newUser)       // OK
   users[0], users[1] = users[1], users[0]      // OK: swap refills both places
   ```
6. **JSON decoding returns a new value; there is no `json.Unmarshal`** (owner decision). `u, err := json.Decode[User](data)` builds a fresh `User` from the input, and a stream decoder has the generic method `dec.Decode[User]()`. A required field missing from the input is an error that names it. Go's fill-a-pointer forms (`json.Unmarshal(data, &u)`, `dec.Decode(&u)`, `Unmarshaler`) are removed so decoding never requires a pre-existing placeholder value. `@Derive(JSON)` (P09.08) generates `MarshalJSON` and a decode function that `json.Decode` uses as its fast path (P13.08).
7. **`make([]T, 0, cap)`** with constant length `0` is allowed for every type with no zero value, C++ classes included.
8. **Never nil on nilable fields** ([ADR-0018](0018-never-nil-pointers-and-option.md), TYP-15). On a field of type `error`, `any` (empty interface), `map` or `chan`, `@Required` also means **never nil**, checked at compile time:
   - a stored value must be known non-nil (a literal, `make(…)`, a local checked with `!= nil`, any value of a never-nil type converted to `error` or `any`, or a read of another never-nil `@Required` field of the same kind);
   - `mem.Take` on such a field and `&` of it (except as the first argument of `mem.Replace(&c.Ch, v)` with a known non-nil `v`) are compile errors;
   - on a slice, `unsafe.Pointer` or type-parameter field, `@Required` means "must be set in literals" only;
   - on an `Option[T]` field (including a defined type over `Option[T]`, or a `@Required` type-parameter field instantiated with an `Option` type), `@Required` is a compile error (`E0363`), because `Option[T]` ("optional") conflicts with `@Required` ("required"); fix-its: `T @Required` or `Option[T]`;
   - on a pointer field (`*T`, `Unique[T]`, `Shared[T]`, `Weak[T]`), a `func` field, or an interface with methods other than `error`, `@Required` is a compile error because those types are already never nil ([ADR-0018](0018-never-nil-pointers-and-option.md)).

   ```go
   type Job struct {
       ID   int       @Required
       Done chan bool @Required
   }
   j := Job{ID: 1, Done: make(chan bool, 1)}        // OK
   old := mem.Replace(&j.Done, make(chan bool, 1))  // OK
   ```
9. **Lookups that can fail** (decides P01 Q9). For a type with no zero value:
   - A map index `m[k]` or channel receive `<-ch` may be used only in a comma-ok form (`v, ok := m[k]`, `v, ok := clone(m[k])`, `v, ok := <-ch`, `case v, ok := <-ch:`), as the target of `m[k] = v`, or in `delete(m, k)`. Single-value reads (`v := m[k]`, `m[k].Email`, `v := <-ch`) are compile errors;
   - in comma-ok forms (`u, ok := x.(User)`, `u, ok := m[k]`, `u, ok := clone(m[k])`, `u, ok := <-ch`), flow typing (P03.18, OWN-P10) makes `u` usable only where `ok` is known true;
   - `switch u := x.(type) { case User: … }` and single-value `u := x.(User)` (which panics on mismatch) work as in Go.

   ```go
   if u, ok := x.(User); ok { use(u) }        // OK
   u, ok := m["bob"]
   if !ok { return }
   use(u)                                     // OK: only reached when ok is true
   ```

## Consequences

- A value with a forgotten required field cannot be built by accident, with zero new keywords.
- Partial literals like `http.Server{Addr: ":80"}` keep working for ordinary Go structs, while `@Required` opts individual fields into compile-time completeness checks.
- Ported Go code that calls `json.Unmarshal` switches to `json.Decode[T]`.

## Alternatives

- **A keyword or mark** (`required ID int`, `ID! int`). Rejected: new grammar, and `!` already means fallible/unwrap.
- **Constructors only** (the Go idiom). Rejected: `User{}` and `var u User` stay legal inside the package.
- **A vet warning.** Rejected: too easy to ignore.
- **Light form** (check literals only, keep the zero value). Rejected: `var u User` and `make([]User, n)` would still create invalid values.
- **Keeping `json.Unmarshal(data, &u)`.** Rejected: callers would have to construct placeholder values first, defeating `@Required`.
