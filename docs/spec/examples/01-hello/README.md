# 01 hello

The smallest program. Run: `go run main.go`.

## Spec rules exercised

- [LEX-1](../../02-lexical.md): the file is `.gopp`, UTF-8.
- The rest is unchanged Go: [Source file organization](https://go.dev/ref/spec#Source_file_organization), [Import declarations](https://go.dev/ref/spec#Import_declarations).
- [TYP-7](../../03-types.md): the string literal passes to the `...any` parameter of `fmt.Println` as a borrow. Nothing is boxed or allocated.

## Differences from the Go version

None. `main.go` and `main.gopp` are the same text. Output is identical.
