# 08 derive

Uses `@Derive`, `#Assert`, `#Stringify` and a user-defined macro. This sample needs P09 macros; it is not compiled before M6.

## Spec rules exercised

- [MAC-1, MAC-2](../../13-macros-surface.md): `macro Swap(a expr, b expr) { ... }` is a declarative macro; `#Swap(tmp, other)`, `#Assert(...)` and `#Stringify(...)` are invocations in statement and expression position.
- [MAC-3, MAC-4, MAC-7](../../13-macros-surface.md): `@Derive(JSON, Equal)` is attached to a type declaration; names start with a capital; `JSON` and `Equal` are looked up in package `derive`.
- [MAC-6](../../13-macros-surface.md): `#Assert`, `#Stringify` and `@Derive` need no import.
- [OWN-R3, OWN-R6](../../04-ownership.md): `b := a` makes an independent deep structural copy.
- [OWN-R4](../../04-ownership.md): `b.Tags = append(b.Tags, "z")` grows `b.Tags` in place: the old value is not read again, so no copy is made.
- [ERR-5, ERR-6](../../09-errors-and-panics.md): `json.Marshal!(a)`; `Marshal` only reads its interface parameter, so `a` is passed without a copy ([TYP-7](../../03-types.md)).
- [PAR-1](../../05-parameters.md): `Equal` only reads its value receiver and its parameter, so `a` and `b` are passed without a copy.
