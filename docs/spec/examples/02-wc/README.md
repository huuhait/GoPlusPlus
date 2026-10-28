# 02 wc

Counts lines, words and bytes of two files and finds the longest word. Run from this directory: `go run main.go`.

Files: `input1.txt`, `input2.txt` (inputs), `main.gopp`, `main.go`, `expected_output.txt`.

## Spec rules exercised

- [ERR-1, ERR-5, ERR-6](../../09-errors-and-panics.md): `func count!()`, `os.ReadFile!` returns the error inside `count`; `count!(name)` in `main` panics on failure.
- [OWN-C1, OWN-C2](../../04-ownership.md): `Stats` is trivial; `string` is owning.
- [OWN-V1, OWN-V2, OWN-V3](../../04-ownership.md): `text[:]` and `s[i:j]` are refs; `string(best)` and `string(data)` are explicit copies.
- [PAR-1, PAR-8](../../05-parameters.md): `name` and `s` are borrowed `string` parameters; passing the owned `text` (through the ref `rest`) to `nextWord` does not move it, and the returned refs point into it.
- [EXP-9](../../06-expressions.md): conversions.
- [PAR-9](../../05-parameters.md): `nextWord` returns refs into its borrowed parameter.
- [RNG-1](../../08-slices-and-range.md): `range inputs` borrows each element.

## Differences from the Go version

- `main.go` returns an error with `(Stats, string, error)`; `main.gopp` uses `func count!()` and `!`.
- `best` is a `ref string` in Go++ (a plain `string` in Go). It is copied with `string(best)` because `text` is destroyed when `count` returns.
- Output is identical.
