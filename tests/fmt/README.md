# gopp fmt tests (P02.09)

`gopp fmt` is a port of go/printer and text/tabwriter (Copyright 2009 The Go
Authors, BSD-style license, see NOTICE) onto the Go++ syntax tree. On Go code it
prints what gofmt prints; the Go++ forms follow the same layout rules. The Go++
rules that need only the syntax (`&T{}`, ERR-15, RNG-8, LEX-11) are not checked:
like type errors for gofmt, they do not stop formatting.

| Test | What it checks |
|---|---|
| `golden.test` | `Inputs/goprinter` (go/printer testdata), `Inputs/gofmt` (cmd/gofmt testdata), `Inputs/gopp` (Go++ forms, comments, header imports): `gopp fmt X.input` equals `X.golden`; every input and golden is also a fixed point and passes the round-trip checks |
| `roundtrip.test` | the ten spec examples, every valid parse/lex/sema test, the goldens: `parse(print(parse(src))) == parse(src)`, `fmt(fmt(x)) == fmt(x)`, no comment lost |
| `header-imports.test` | header imports keep their order; groups without one are sorted |
| `flags.test` | `-l`, `-w`, `-d`, stdin, directories, exit status 2 on syntax errors |
| `goroot.long.test` (`REQUIRES: long, go`; `ninja check-long`) | the same round-trip checks and the gofmt comparison on `$(go env GOROOT)/src`, about 7350 files (eight shards, `goroot-1.long` to `goroot-7.long`) |
| `../unit/Format/` | layout rules by example, import rules, the header rule hook, the fuzz corpus replay |

`check-corpus.py` runs the round-trip checks on a list of files or directories.
`compare-gofmt.py` compares with the real gofmt. Regenerate the goldens with
`tests/update-goldens fmt` and review the diff.

## The round-trip tree

"The same tree" means the dump of `gopp parse -dump-ast` without positions, and
without what gofmt itself does not keep:

- empty statements `;`, and the empty result list `()`;
- redundant parentheses (`func f(a (int))`, `switch (x)`), except a parenthesized
  `x.(type)` as switch tag (stripping would make a type switch);
- the spelling of numbers (`0X1` to `0x1`, `1E3` to `1e3`, `0765i` to `765i`);
- the quotes of a backquoted import path;
- carriage returns in raw strings and comments;
- the order of imports (the check runs with `-no-sort-imports`).

## Differences to gofmt (Go 1.25.14, `compare-gofmt.py --goroot`)

Of the 7354 Go files below `$GOROOT/src`, 7284 are byte-identical to gofmt
(and every output is a fixed point of `gopp fmt`; the directory `go/parser/testdata/issue42951/not_a_file.go` is not a file and is not counted). The rest:

| Class | Files | Why |
|---|---|---|
| comments | 18 | only comment lines differ: go/doc/comment's reformatting of doc comments (lists, code blocks, `//text` to `// text`, directives) and the `//go:build` / `// +build` line fixes are not ported (Go++ has `//gopp:build`) |
| fixed point | 6 | gofmt is not idempotent there (a one-line function body behind a header that was split over lines). `gopp fmt` formats until the text is stable (`settle` in Format.cpp): gofmt's second pass is the result |
| Go++ syntax | 2 | `macro` is a keyword (`cmd/asm/internal/lex/input.go`); `(View[string], ok bool)` reads as the type `View[string]` (`codehost/git.go`) |
| both reject | 44 | test data with syntax errors, and an empty file |

Other differences, found by fuzzing and in the testdata, all on invalid
programs or invisible:

- A type parameter whose constraint is not a type element (`[P (b*struct{}),]`,
  `[P (+n),]`) keeps parentheses and gets a comma, so that the text parses to the
  same tree. gofmt prints `[P b * struct{}]` or `[P +n]`, which changes the
  program or does not parse.
- A formfeed inside a string literal or comment does not count as a line, as
  in the scanner. go/printer counts it and loses a blank line.
- `switch (x.(type)) {` keeps its parentheses (an expression switch with a bad
  tag stays one); gofmt turns it into a type switch.
- Imports: a backquoted path without escapes becomes a double-quoted one, as in
  gofmt; paths with escapes and `<...>` header paths are kept.

## Go++ forms

| Form | Printed as |
|---|---|
| `ref T`, `move p T` | one blank after the word |
| `f!(x)`, `func f[T any]!(...)` | `!` directly before `(`, after the type parameters |
| `for k, &v := range x` | `&` before the aliased variable |
| `@Attr` / `@Attr(args)` before a declaration | one line each if the source has them on separate lines, else separated by one blank |
| `@Attr` on a struct field | after the type, before the tag, in the tag column |
| `macro Name(p kind, ...) { }`, `macro func` | like a function: a small body written on one line stays on one line |
| `#Name(...)`, `@Name(...)` | `#`/`@`, the package and the name at their positions (a comment between them stays); the arguments in `(...)` are copied from the source as they are, comments included: the token trees belong to the macro (P09) |

Import sorting (D2.8): a group (lines not separated by a blank line) is sorted
only if it has no header import. Without a gopp.mod a header is a `"<...>"`
path or one that ends in `.h`, `.hh`, `.hpp` or `.hxx`; a tool that knows the
gopp.mod installs the compiler's rule in `FormatOptions::IsHeader`.
