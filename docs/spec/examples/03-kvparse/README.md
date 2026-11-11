# 03 kvparse

Parses `key = value` lines from `config.txt` into entries whose `string` fields slice one shared backing buffer with zero byte copies. Run: `go run main.go`.

## Spec rules exercised

- [OWN-C2, OWN-S1](../../04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032): `Entry` holds plain `string` fields (`{ptr, len, backing}`); slicing `s[:i]` and `s[i+1:]` shares `src`'s refcounted backing buffer in $O(1)$ without copying bytes, and keeps the buffer alive for as long as any `Entry` exists.
- [PAR-1, PAR-2, PAR-9](../../05-parameters.md): `s string` in `trim` and `cut` is lent with 0 retain/release at the call boundary, re-sliced locally with `s = s[1:]`, and returned as a shared sub-string.
- [ERR-1, ERR-3, ERR-5, ERR-6](../../09-errors-and-panics.md): `parse!` returns `([]Entry, error)`; `return nil, err` fails explicitly; `parse!(src)` in `main` panics on error; the plain Go call `parse(...)` also works.
- [OWN-R4](../../04-ownership.md): `out = append(out, ...)` updates `out` in place when `refcount == 1`.

## Differences from the Go version

- Both Go and Go++ use plain `string` for every field and parameter. Substrings in Go share the buffer via GC; in Go++ they share `backing` via deterministic reference counting.
- Go++ uses `parse!`/`strconv.Atoi!`; Go checks `err` by hand.
- Output is identical.
