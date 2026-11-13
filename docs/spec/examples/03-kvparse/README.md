# 03 kvparse

Parses `key = value` lines from `config.txt` into entries whose `string` fields slice one shared backing buffer with zero byte copies.

## Spec rules exercised

- [OWN-C2, OWN-S1](../../04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032): `Entry` holds plain `string` fields (`{ptr, len, backing}`); slicing `s[:i]` and `s[i+1:]` shares `src`'s refcounted backing buffer in $O(1)$ without copying bytes, and keeps the buffer alive for as long as any `Entry` exists.
- [PAR-1, PAR-2, PAR-9](../../05-parameters.md): `cut` only reads `s string`, so the argument is passed as it is (0 retain/release at the call boundary); `trim` re-slices its own `s` with `s = s[1:]`; both return sub-strings that share the same bytes.
- [ERR-1, ERR-3, ERR-5, ERR-6](../../09-errors-and-panics.md): `parse!` returns `([]Entry, error)`; `return nil, err` fails explicitly; `parse!(src)` in `main` panics on error; the plain call `parse(...)` also works.
- [OWN-R4](../../04-ownership.md): `out = append(out, ...)` updates `out` in place: the old `out` is not read again, so no copy is made.
