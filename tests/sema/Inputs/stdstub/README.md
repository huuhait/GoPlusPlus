# Standard library stubs (TEST ONLY)

Declaration-only Go++ packages (`cmp`, `errors`, `fmt`, `mem`, `os`, `slices`,
`strconv`, `sync`) with exactly the API that `docs/spec/examples` use, so that
`tests/sema/spec-examples-stubs.test` can type-check the P01 samples before
P13 ships the standard library (and P05 the real `mem`).

- They are found only through the hidden option
  `gopp check --test-stubs tests/sema/Inputs/stdstub` (LoadOptions::StdRoot),
  which also mocks the C and C++ headers of the examples (`zlib.h`, `<vector>`,
  `<algorithm>`: `compiler/lib/Sema/ExampleMocks.cpp`) until P07 and P08
  import real headers. A normal `gopp check` never reads them.
- Signatures follow Go. A function the examples call with `!` is declared
  with `!` (`func!() T` and `func() (T, error)` are one type).
- A function without a body is a declaration; a generic one has a body that
  panics. Nothing runs them.
