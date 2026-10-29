# ADR-0019: Strict structs (`@Strict`) and optional fields (`@Option`)

- **Status:** accepted
- **Status note (2026-10-28, ADR-0023):** the sentences about embedded fields (decision 2 "Embedded fields count like any field", and the `@Option` on an embedded field rule) no longer apply in `.gopp` files, because struct embedding is removed; the parent's fields of an `extends` type count in the child instead (TYP-17). The text below is kept as it was written.
- **Date:** 2026-10-28
- **Plan reference:** extends ADR-0016 (`@Required`); P01 D1.4 rule 11; spec TYP-10, TYP-15, TYP-15a, EXP-9, MAC-8, MAC-12; new task P03.20; P09.04, P09.08, P13.08, P14.06.

## Context

`@Required` (ADR-0016) marks one field at a time. For a type where almost every field matters (a payment, a config, a request), the author must write `@Required` on each field, and a field added later is optional until someone remembers the mark.

On 2026-10-28 the owner chose a second built-in attribute, `@Strict`, placed on the struct type. In a strict struct every field is required, and the few fields that may be left out are marked. `@Required` stays for the case where only one or two fields matter.

## Decision

1. **`@Strict` is a built-in attribute on a struct type** (MAC-8). It is written on the line before `type`, like other top-level attributes (MAC-3). Generic struct types are allowed. On a non-struct type, a func, a var, or a field it is an error.
2. **In a `@Strict` struct every field counts as `@Required`,** with all of its meaning (TYP-15, ADR-0016): it must be set in every composite literal; the struct has no zero value; this spreads to containing types; the field is never nil for `error`, `any`, map and channel types; for slices, `unsafe.Pointer` and type-parameter fields it only means "must be set". Embedded fields count like any field. Three kinds of field are not required:
   - a field whose type is `Option[T]`;
   - a field marked `@Option` (point 3);
   - a blank `_` field.

   Pointer, func and method-interface fields have no zero value anyway, so they are required in every struct already. In a `@Strict` struct they are simply required as before. This is not an error (writing `@Required` on them is, ADR-0018).
3. **`@Option` is a built-in field attribute,** written after the field's type (MAC-12). It means "this field may be left out of a literal and then gets its zero value". The field is a plain Go field: an `error`, map or `any` field marked `@Option` may be nil, and `Err: nil` is fine. These are errors:
   - `@Option` in a struct that is not `@Strict` (fields are optional there by default; the message says so);
   - `@Option` on a field whose type has no zero value (a pointer kind, a func, an interface with methods, a struct with required fields, a C++ type without a default constructor), because it can't be left out; the hint is to use `Option[T]`;
   - `@Option` on an `Option[T]` field (redundant);
   - `@Option` on a blank `_` field (redundant: `_` is never required);
   - `@Option` together with `@Required`.

   `@Option` on an embedded field is allowed and means the same as on any field (it may be left out and gets its zero value); if the embedded type has no zero value, it is the error above.
4. **`@Required` in a `@Strict` struct is an error only where it is redundant,** that is, on a field that `@Strict` already requires ("redundant: `@Strict` already requires it", with a fix-it that removes it). On an `Option[T]` field it is allowed and keeps its TYP-15 meaning "must be set": the literal must name the field, and `Note: nil` is fine.
5. **A `@Strict` struct with at least one required field has no zero value,** as in ADR-0016 points 3 and 4. If no field is required (all are `Option`, `@Option` or `_`), `@Strict` changes nothing, and `gopp vet` warns about it.
6. **Struct conversion.** The "required" bit of each field is part of what must match when one struct type is converted to another. A `@Strict` struct and a non-strict struct with the same fields therefore don't convert into each other, unless their effective required bits are equal (spec EXP-9, TYP-15a).
7. **The effective bits are used everywhere `@Required` is used:** type serialization (P03.11; for a type from another package the bits come from the serialized export data, as for `@Required`), the field table (TYP-10), JSON decoding (a required field missing from the input is an error naming it; a missing `@Option` field gets its zero value; a missing `Option` field gets nil), the macro API (`field.Required()` reports the effective bit; `type.Strict()` and `field.Option()` are added), and the LSP quick fix "add missing required fields".
8. **Go mode:** attributes are not Go syntax, so nothing changes in `.go` files.

```go
@Strict
type Payment struct {
    ID      int
    Amount  int64
    Note    Option[string]   // may be left out: nil
    Retries int @Option      // may be left out: 0
}

p := Payment{ID: 1, Amount: 500}   // OK
p := Payment{ID: 1}                // error: Amount not set
var q Payment                      // error: Payment has no zero value
```

## Consequences

- One mark on the type makes a new field required by default. A forgotten field in a literal is a compile error.
- The checker reuses the P03.15 machinery. The only new work is computing the effective required bit of each field, the attribute checks, and the conversion rule.
- Other tools that read "required" (JSON, macros, LSP) must read the effective bit, not only the `@Required` mark. P09.04, P09.08, P13.08 and P14.06 are extended.
- Adding a field to a `@Strict` struct breaks every literal of it. This is the point of the attribute, but the author of a library type should use it with care.
- A struct with `@Strict` and no required field has no effect; `gopp vet` says so.

## Alternatives

- **All fields required by default, with no attribute.** Rejected by the owner: it breaks Go code and partial literals such as `http.Server{Addr: ":80"}`, it removes the zero value from almost every struct, and adding a field to a library struct would break every user.
- **Keep only per-field `@Required`.** Rejected: it works, but it is more typing for the types that need it most, and a new field is optional until someone adds the mark.
- **A vet warning only.** Rejected: too easy to ignore, and `gopp build` doesn't run vet (as in ADR-0016).
