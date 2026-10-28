# ADR-0016: Required struct fields (`@Required`)

- **Status:** accepted; extended by ADR-0019 (`@Strict` and `@Option`); parts changed by ADR-0018 (point 8 for pointer, func and interface fields; point 9 and the consequences: the checks move from P04 to the P03 flow typing, task P03.18)
- **Date:** 2026-10-27
- **Plan reference:** P01 D1.4 rules 9 and 11 (decision change to the completed P01); spec TYP-15, OWN-R9, MAC-8, MAC-12; new task P03.15; P09.08, P13.08. Decides P01 Q9.

## Context

Go has no way to force a struct field to be set: `User{Email: "a@b.c"}` silently leaves `ID` at 0, and `var u User` is always allowed. The Go idiom (private fields plus a `NewUser` constructor) doesn't stop `User{}` from other packages. Rust and Swift require every field; Kotlin and TypeScript require fields unless they are marked optional; C# 11 added `required`.

On 2026-10-27 the owner chose a built-in field attribute, `@Required`, over new syntax, a constructor convention, or a vet warning. The owner chose the **strict** form: a struct with a required field has no zero value.

## Decision

1. **`@Required` is a built-in attribute on struct fields** (MAC-8), written after the field's type like any field attribute (MAC-12): `ID int @Required`. The compiler handles it; it is not a macro.
2. **Every composite literal of the struct must set each required field,** by name or by position (a positional literal sets every field anyway). `User{Email: "x"}` is a compile error that names the missing fields. `@Required` is only allowed on struct fields, embedded fields included; anywhere else it is an error.
3. **A struct with a required field has no zero value.** It joins the C++ types without a default constructor (P01 D1.4 rule 9, OWN-R9). Every place where Go would create a zero value is a compile error:
   - `var u User` with no initializer, and `var zero T` in generic code instantiated with `User`;
   - `make([]User, n)`, unless `n` is the constant `0` (`make([]User, 0, 100)` reserves room and creates no `User`);
   - `var a [3]User`, an array literal that leaves elements out (`[3]User{{…}}`), and a keyed slice or array literal with gaps (`[]User{5: {…}}` would create elements 0 to 4);
   - named results of type `User`, a failing `!` in a function returning `User`, any receive from a `chan User` other than the comma-ok form, including `case u := <-ch:` in a `select` (point 9 below);
   - `clear(s)` on a slice of `User` (Go's `clear` keeps the length and zeroes the elements); the error suggests `s = s[:0]`, which drops them. In generic code, `clear(s)` on a `[]T` can't be instantiated with such a `T`. `clear(m)` on a map is fine: it creates no values;
   - `new_unique(User{})` and other literals, through rule 2.
4. **It spreads to containing types.** A struct or array whose field or element type has no zero value has none either. A field of type `User` inside `Team` must be set in every `Team` literal, as if it were `@Required`. For generic types it is a property of the instance: `Box[User]` has no zero value, `Box[int]` has one.
5. **Moves.** A moved-from local still holds the all-zero bytes (OWN-R5); only drop and assignment touch it, and reading it is a use after move. `move` out of any place other than a local variable is an error for a Go++ type with no zero value (a slice, array or map element, an `&` range alias, a struct field, `*p`), because the place would be left as zero bytes that later code reads as a real value; use `mem.Replace(&place, v)`, `delete(m, k)` or `move(m[k])` (which remove the entry), `s = s[:n]` (which drops elements), or `range move(xs)`. A moved local is tracked by the use-after-move check. C++ types are not affected: their moved-from state is a valid C++ object. A *local* here is a variable declared in the function: `:=` and `var` variables, comma-ok results of a map or channel lookup (`v, ok := clone(m[k])`, `v, ok := move(m[k])`, `v, ok := <-ch`), owning range variables (`range move(xs)`) and `move` parameters. A type-assertion or type-switch binding is not one: it borrows the interface's box (OWN-R3b), so take a copy with `u.Clone()`. Inside a closure, a captured variable is not a local (CLO-9), so moving it is an error for such a type (the next call would read zero bytes). The rule looks at the type of the place itself: `x.items = append(x.items, v)` moves a `[]User`, which has a zero value, so it is fine. A swap, or any assignment that refills every place it empties in the same statement, is allowed: `xs[i], xs[j] = xs[j], xs[i]`, `c.A, c.B = c.B, c.A`. In such a swap, all index, map-key and pointer operands and their bounds and nil checks are evaluated before any value is moved, so the statement can't panic halfway and leave a place empty. In a map range, after `move(m[k])` (or `v, ok := move(m[k])`) of the current entry, the borrowed range variable `v` counts as moved, so reading it later in the body is a use-after-move error. Re-slicing past the length already panics for every type (SLC-13), so it can't expose a `User` either.

   ```go
   for _, &u := range users { keep(move(u)) }   // error: users[i] would be left as zero bytes
   for _, u := range move(users) { keep(u) }    // OK: users is consumed
   old := mem.Replace(&users[0], newUser)       // OK
   users[0], users[1] = users[1], users[0]      // OK: a swap refills both places
   for k, v := range m { c, ok := move(m[k]); use(v) }   // error: v was moved with m[k]
   ```
6. **JSON decoding returns a new value; there is no `json.Unmarshal`** (owner decision). `u, err := json.Decode[User](data)` builds a fresh `User` from the input, and a stream decoder has the generic method `dec.Decode[User]()`. A required field missing from the input is an error that names it. Go's fill-a-pointer forms (`json.Unmarshal(data, &u)`, `dec.Decode(&u)`, the `Unmarshaler` interface) are removed for every type, because they need a value to exist before decoding. `@Derive(JSON)` (P09.08) generates `MarshalJSON` and a decode function that `json.Decode` uses as its fast path (P13.08).
7. This ADR also fixes the `make` rule of D1.4 rule 9 for C++ types: a constant length `0` is allowed there too.
8. **Never nil** (owner decision): a `@Required` field whose type can be nil (`*T`, `Unique[T]`, `Shared[T]`, interfaces, funcs, maps, channels) is **never nil**, checked at compile time: a value stored in it (in a literal or an assignment) must be known non-nil (`new_unique(…)`, `new_shared(…)`, `&x`, `make(…)`, a map or func literal, a non-nil `@Required` field, or a variable inside an `if p != nil` check); storing `nil` or a maybe-nil value is an error; `move` out of the field is an error (use `mem.Replace(&c.Owner, v)`, which returns the old value; `v` must be known non-nil too); `&c.Owner` is an error except as the first argument of `mem.Replace`, so nil can't be written through a pointer (`p := &c.Owner; *p = nil`); `@Required` on a `Weak[T]` field is an error (it can turn nil by itself); on a slice, and on a field whose type is a type parameter (`Val T @Required`), it only means "must be set", because a generic body can't know whether its instance is nilable. Reads of such a field need no nil check. `drop` never sees it nil: a moved-from value is all zero bytes and drop glue skips it (OWN-R5), and the field alone can't be moved out. C or C++ code that writes nil into the field is undefined behavior. (Changed by ADR-0018 point 7 and amendment point 3: pointers, funcs and interfaces with methods are never nil by type, so `@Required` on such a field is an error; the never-nil meaning stays for `error`, `any`, map and channel fields. The known-non-nil list is in spec TYP-15.)

   ```go
   type Car struct{ Owner Unique[User] @Required }
   c := Car{Owner: new_unique(bob)}                   // OK
   c := Car{Owner: move(p)}                           // error: p may be nil; check p != nil first
   if p != nil { c := Car{Owner: move(p)} }           // OK
   o := move(c.Owner)                                 // error: would leave nil; use mem.Replace
   old := mem.Replace(&c.Owner, new_unique(amy))      // OK
   ```

9. **Lookups that can fail** (owner decision; decides P01 Q9). For a type with no zero value (C++ types of rule 9 and `@Required` structs alike):
   - A map index `m[k]` or a receive `<-ch` whose value type has no zero value may be used only in a comma-ok form (`v, ok := m[k]`, `v, ok := clone(m[k])`, `v, ok := move(m[k])`, `v, ok := <-ch`, `case v, ok := <-ch:` in a `select`), as the target of `m[k] = v`, in `delete(m, k)`, or as a bare `move(m[k])` statement that discards the value; every other use (`v := m[k]`, `u := clone(m[k])`, `u := move(m[k])`, `m[k].Email`, `f(m[k])`, `T{F: m[k]}`, `v := <-ch`, `case v := <-ch:`, `(<-ch).Email`) is an error, because a missing key or a closed channel would give zero bytes;
   - the comma-ok forms (`u, ok := x.(T)`, `u, ok := m[k]`, `u, ok := clone(m[k])`, `u, ok := move(m[k])`, `u, ok := <-ch`) work, and `u` counts as moved on every path where `ok` is false, so the use-after-move check rejects reading it there;
   - `switch u := x.(type) { case T: … }` and the single-value `u := x.(T)` (which panics on a mismatch) work as in Go.

   ```go
   if u, ok := x.(User); ok { use(u) }        // OK
   u, ok := clone(m["bob"])
   if !ok { return }
   use(u)                                     // OK: only reached when ok is true
   v, ok2 := x.(User)
   use(v)                                     // error: v may not exist; check ok2 first
   ```
   The check is the P04 flow-sensitive use-after-move check (P04 D4.2), with the `!ok` branch treated as a move of `u`. When `ok` is false, `u`'s storage holds zero bytes that nothing reads. (Changed by ADR-0018 amendment point 4: the check is the P03 flow typing, task P03.18: `u` is usable only where `ok` is known to be true.)

## Consequences

- A value with a forgotten required field can't be built by accident, without any new syntax.
- The checker reuses rule 9's "no zero value" machinery; P03.15 adds the attribute, the literal check, the spreading rule and the `make(…, 0)` exception.
- Code that relies on zero values for such a struct needs `make([]T, 0, n)` plus `append`, or a literal.
- Generic code that writes `var zero T` can't be instantiated with these types (the same as for C++ types today).
- "Known non-nil" uses the same P04 flow check as comma-ok (an `if p != nil` narrows `p`). (Changed by ADR-0018 amendment point 4: both are P03 flow typing, task P03.18.) A value the compiler can't prove non-nil (a function result, a package variable such as `os.Stdout`) needs an explicit `if v != nil` first.
- Comma-ok lookups need no new analysis: P04.09's use-after-move check treats the `!ok` paths as moves. (Changed by ADR-0018 amendment point 4: P03 flow typing, task P03.18; P04.09 keeps them as GIR regression cases.)
- Ported Go code that calls `json.Unmarshal` must switch to `json.Decode[T]`. How a type supplies custom decoding (Go's `UnmarshalJSON`) is designed in P13.08.

## Alternatives

- **A keyword or mark** (`required ID int`, `ID! int`). Rejected: new grammar, and `!` already means "must".
- **Constructors only** (the Go idiom). Rejected: `User{}` and `var u User` stay legal.
- **A vet warning.** Rejected: too easy to ignore, and `gopp build` doesn't run vet.
- **Light form** (check literals only, keep the zero value). Rejected by the owner: `var u User` and `make([]User, n)` would still create values with the field unset.
- **Comma-ok as an error** (type switch only). Rejected by the owner: `if u, ok := x.(User); ok` is the common Go form, and the use-after-move check makes it safe for free.
- **`@Required` meaning only "must be written"** for nilable fields (`Owner: nil` allowed). Rejected by the owner in favor of never nil.
- **Never nil with run-time checks** at each store. Rejected: hidden panics, and moves and `Weak` would still break the promise.
- **New non-nil pointer types** (`Own[T]`, as in Rust, Swift, Kotlin). As strong, but new types and rules; `@Required` gives the same guarantee for fields with no new types.
- **`clear(s)` emptying the slice** for these types. Rejected: the same `clear` would zero an `[]int` but empty a `[]User`, so generic code such as `clear(s); s[0] = x` would compile and panic at run time.
- **Keeping `json.Unmarshal(data, &u)`.** Rejected by the owner: the caller would have to build `u` with placeholder values first, which defeats `@Required`.
