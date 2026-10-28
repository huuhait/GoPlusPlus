# types2 conformance suite

A port of a subset of Go's type checker tests, to measure how close the Go++
checker is to `go/types` / `types2` when it runs in Go mode (`gopp check -go`,
`CheckOptions::GoCompat`: Go's built-ins, no ownership rules, no generic
methods).

- **Source:** `$(go env GOROOT)/src/internal/types/testdata/{check,fixedbugs,spec,examples}`
  (Go 1.25.14). Files are copied unchanged, with their license header, under
  `testdata/`. The Go license is in `LICENSE-GO` (BSD-3-Clause).
- **Harness:** `tests/unit/Sema/ConformanceTest.cpp` (gtest `Go/Conformance.*`,
  in-process; the whole suite runs in well under a second).
  It reads the `/* ERROR "text" */` and `/* ERRORx "regexp" */` comments. An
  annotation matches a diagnostic on the **same line** whose message contains
  the text (or matches the regexp). Column positions are not compared (the
  Go test harness compares the column too; this is a known difference).
  A file passes when every annotation matches and no other error is reported.
- **Imports:** the Go standard library packages the files import exist as
  empty packages; any other import path fails, as with Go's test importer.
  The packages whose names the files look up (`math/big`, `go/ast`, `io`, `os`,
  `context`, `database/sql`, `math`, `strings`, `sort`) have a small stub in
  `stdstub/<path>.go` (Go source with the API the files use, checked by the
  harness itself). The stubs are test-only; the real standard library is P13.
- **Expected failures:** `expected-failures.txt`, one line per file:
  - `path | xfail | reason`: a gap of the checker. A listed file that passes,
    and an unlisted file that fails, are both test failures, so the list can
    only shrink on purpose.
  - `path | exclude | reason`: the file tests what Go++ does not have by
    design. It still runs, but it is not counted. A file whose first line has
    flags (`// -lang=go1.17`) must be excluded. The flags `-gotypesalias=1`
    and `-goexperiment=aliastypeparams` select what Go++ always does, so such
    files are counted.
- **Report:** `GOPP_CONFORMANCE_REPORT=1 build/bin/GoppUnitTests --gtest_filter='Go/Conformance.*'`
  prints the pass rates of the counted files, overall and for generics files.
  Add `GOPP_CONFORMANCE_DETAIL=1` to see the first mismatches of each failing
  file. "Generics" is a file-level heuristic (file name, or type-parameter
  syntax).

## Status

| Measure (counted files only) | Result |
|---|---|
| Files passing | 285 / 305 (93.4%) |
| Annotations matched | 2648 / 2699 (98.1%) |
| Generics files passing | 157 / 167 (94.0%) |
| Generics annotations matched | 783 / 799 (98.0%) |
| Excluded files (not counted) | 34 (8 of them pass anyway) |

The P03.06 goal (at least 90% of the generics cases) and the P03 exit
criterion (at least 90% of the counted files) are met.

## Exclusions by design (the Go++ rule that makes them differ)

| What | Files | Go++ rule |
|---|---|---|
| First line has `-lang=go1.N` | 30 | Go++ has one language version. Version-gated errors such as "requires go1.18 or later" never exist. |
| `-gotypesalias=0` without `-lang` | 3 | Go++ has only the alias mode; the pre-1.23 non-alias mode does not exist. |
| `-fakeImportC` | 1 | Go++ does not use cgo. C and C++ headers are imported by path (`import "<vector>"`, D3.7, `docs/spec/14-foreign-surface.md`). |

## Known gaps (not by design)

Every `xfail` line names its reason. The families:

- **Cycle diagnostics** (4 files): some `invalid recursive type` reports
  differ in position from types2, and a generic type whose constraint and
  method both mention it is reported once (types2: twice).
- **Instantiation cycles** (3 files): the monomorphization cycle check of
  types2 (`mono.go`) is not ported.
- **Parser messages** (3 files): the Go++ parser reports some Go mistakes
  with its own P02.07 messages (`T[]`, import placement).
- **Alias spelling** (2 files): Go++ resolves an alias at once (there is no
  `Alias` type), so messages print the aliased type where types2 prints the
  alias name (`A`, not `int`).
- **Function type strings have no parameter names** (3 files): `TupleType`
  stores only types, so `func(n T, _ List[T]) T` prints as
  `func(T, List[T]) T`. Types are uniqued, so names cannot be stored there
  without making the text depend on which declaration was seen first.
- A tail of single-message differences in statements, labels and `goto`,
  expressions and the "declared at" inference note (5 files).

Closed in P03 (P03.18 batch): the size model (`Sizes.cpp` ports types2's
`gcsizes.go` for amd64, so `unsafe.Sizeof/Alignof/Offsetof` match gc and "is
too large" is reported), constants (float exponents up to 2^31 as in
`big.Float`, with 10^n in 512 bits so a huge exponent stays fast; constant
arity and the position of errors in inherited initializers), the cycle
diagnostics that came from declaration order (`var a, b = f()` shares one
declaration, a function's signature is no initialization dependency, an
interface type being declared has no methods yet, "is not a type" before a
cycle), `n:m` variable declarations, types2's `lookupError` wording, and the
std packages the files import (`stdstub/`).

## Not covered

- Go version gating, as above.
- Column checks.
- Multi-file packages of the Go testdata (the harness checks one file at a
  time; the loader unit tests cover packages).
