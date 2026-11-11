# 02 wc

Counts lines, words and bytes of two files and finds the longest word. Run from this directory: `go run main.go`.

Files: `input1.txt`, `input2.txt` (inputs), `main.gopp`, `main.go`, `expected_output.txt`.

## Spec rules exercised

- [ERR-1, ERR-5, ERR-6](../../09-errors-and-panics.md): `func count!()`, `os.ReadFile!` returns the error inside `count`; `count!(name)` in `main` panics on failure.
- [OWN-C1, OWN-C2](../../04-ownership.md): `Stats` is trivial; `string` is immutable, and a copy of one shares the same bytes (`{ptr, len, backing}`).
- [OWN-S1](../../04-ownership.md#44-slicing-and-shared-buffers-d16-adr-0032): `s[i:j]` and `s[j:]` produce `string` values sharing `s`'s refcounted backing buffer in $O(1)$ with zero allocation.
- [PAR-1, PAR-8, PAR-9](../../05-parameters.md): `name` and `s` are `string` parameters that their functions only read, so the arguments are passed as they are (0 retain/release at the call boundary); `nextWord` returns sub-strings sharing `s`'s backing.
- [EXP-9](../../06-expressions.md): `string(data)` converts `[]byte` to `string`.
- [RNG-1](../../08-slices-and-range.md): `range inputs`: the body only reads `name`, so the string element is not copied.

## Differences from the Go version

- `main.go` returns an error with `(Stats, string, error)`; `main.gopp` uses `func count!()` and `!`.
- Everything else — `string` parameters, slicing, and returning `best` — is written identically to Go, with deterministic reference-counted buffer cleanup instead of GC.
- Output is identical.
