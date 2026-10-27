# 08 derive

Uses `@Derive`, `#Assert`, `#Stringify` and a user-defined macro. Run: `go run main.go`. This sample needs P09 macros; it is not compiled before M6.

## Spec rules exercised

- [MAC-1, MAC-2](../../13-macros-surface.md): `macro Swap(a expr, b expr) { ... }` is a declarative macro; `#Swap(tmp, other)`, `#Assert(...)` and `#Stringify(...)` are invocations in statement and expression position.
- [MAC-3, MAC-4, MAC-7](../../13-macros-surface.md): `@Derive(JSON, Equal)` is attached to a type declaration; names start with a capital; `JSON` and `Equal` are looked up in package `derive`.
- [MAC-6](../../13-macros-surface.md): `#Assert`, `#Stringify` and `@Derive` need no import.
- [OWN-R6](../../04-ownership.md): `clone(a)` is structural (string, slice, map are clonable).
- [OWN-R4](../../04-ownership.md): `b.Tags = append(b.Tags, "z")` moves `b.Tags` implicitly.
- [ERR-5, ERR-6](../../09-errors-and-panics.md): `json.Marshal!(a)`; an interface parameter of `Marshal` takes `a` as a borrow ([TYP-7](../../03-types.md)).
- [PAR-1](../../05-parameters.md): `Equal`'s value receiver and parameter are borrowed.

## Differences from the Go version

- Go has no macros. `main.go` writes `Equal` by hand, writes the assertion with `panic`, types the `#Stringify` text as a string literal, and swaps with `a, b = b, a`.
- `main.go` builds `b` with a second literal (Go++ uses `clone(a)`).
- `encoding/json` needs no derive in either language; `@Derive(JSON)` only adds a faster generated method.
- Output is identical.

## Assumptions about P09

- `#Stringify(a.Equal(b))` returns the canonical source text `a.Equal(b)`.
- A statement macro may be invoked as `#Swap(x, y)` with `expr` arguments that are assignable places.
- The derived `Equal` has the shape `func (x Config) Equal(o Config) bool` (P09 D9.8 and its sketch).
