# 03 kvparse

Parses `key = value` lines from `config.txt` into entries that are views into one buffer. Run: `go run main.go`.

## Spec rules exercised

- [OWN-V1, OWN-V2, OWN-V5](../../04-ownership.md): `view string` fields, slicing makes views, `@View` on the token struct.
- [OWN-V3](../../04-ownership.md): `string(data)` is an explicit copy; the owned `src` passes to the borrowed `string` parameter of `parse!` without a copy or a move.
- [PAR-2, PAR-9](../../05-parameters.md): the borrowed `string` parameter `s` of `trim` is re-pointed with `s = s[1:]`; `trim`, `cut` and `lookup` return views into their parameters.
- [PAR-10](../../05-parameters.md): a `view string` passes to the `string` parameter of `strconv.Atoi!`.
- [ERR-1, ERR-3, ERR-5, ERR-6](../../09-errors-and-panics.md): `parse!` returns `([]Entry, error)`; `return nil, err` fails explicitly; `parse!(src)` in `main` panics on error; the plain Go call `parse(...)` also works.
- [OWN-R4](../../04-ownership.md): `out = append(out, ...)` moves `out` implicitly.
- [OWN-R8](../../04-ownership.md), [UB-1](../../16-undefined-behavior.md): entries outlive nothing here, but the compiler does not check it: `src` must outlive `entries`.
- [TYP-3](../../03-types.md): `view string` compares with `string` by contents.

## Differences from the Go version

- The Go version uses plain `string` for every field. Substrings in Go share the buffer by GC; in Go++ they are views and the owner (`src`) must outlive them.
- Go++ uses `parse!`/`strconv.Atoi!`; Go checks `err` by hand.
- Output is identical.
