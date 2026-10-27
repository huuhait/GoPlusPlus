# Findings ledger: p02-frontend-tools

Task: P02.07–P02.11 (error recovery & tailored messages, `gopp parse` polish, `gopp fmt`, parser fuzzing, parse throughput). 3 coders → PM merge (base c2).
Branch: `task/p02-frontend-tools-merged`

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | major | `gopp fmt` import dedupe drops a leading block comment (`/* lead */ "a"` + `"a" // dup` → `/* lead */` lost); sort/dedupe path has no comment-preservation test | .scratch/repro_import_dup_comment.sh | FIXED | CONFIRMED-FIXED | 1→2 |
| R2 | major | err_for_in fix-it on `for i in range(10) {` gives `for _, i := range range(10) {`, which does not parse | .scratch/repro_forin_range.sh | FIXED | CONFIRMED-FIXED | 1→2 |
| R3 | major | fix-its of err_fails_returns_error, err_arrow_before_result, err_decl_without_var, err_struct_decl delete comments inside the replaced range | .scratch/repro_fixit_comment_loss.sh | FIXED | CONFIRMED-FIXED | 1→2 |
| R4 | minor | err_move_type_not_shared fix-it copies a variadic `...T` onto earlier names (`a ...int, move b ...int`, invalid) | none (`gopp parse -fixit-stdout` on `func f5(a, move b ...int) {}`) | FIXED | CONFIRMED-FIXED | 1→2 |
| R5 | minor | (manager, from reviewer note) FuzzCheck.h `checkPackage` passes `HadErrors=true`, so the Bad-node check never runs in package mode, and it never compares parallel vs serial diagnostics | none | FIXED | CONFIRMED-FIXED (mutant-tested) | 1→3 |
| R6 | minor | (manager, from reviewer note) the err_fmt_unreported_syntax_error guard and the `-fixit` build-all-then-write path have no test that fails without the fix (Definition of done) | none | FIXED | CONFIRMED-FIXED | 1→3 |
| R7 | minor | (manager, from reviewer note) spec LEX-11 promises a fix-it "move it to gopp.mod"; code gives a note (a fix-it cannot move text to another file). Change the spec text to match the code | none | FIXED | CONFIRMED-FIXED (manager) | 1→2 |
| R8 | major | valid Go++ `while!(x)` / `while !(x)` (unwrapping call of a function named `while`) is read as a C-style while loop: parse error, `gopp fmt` refuses the file, `-fixit` rewrites to `for!(1)` | .scratch/repro_while_bang_call.sh | FIXED | CONFIRMED-FIXED | 3→4 |
| R9 | minor | ERR-15 missed when the last result field has several names: `func f!() (a, b error)` accepted | .scratch/repro_err15_grouped_names.sh | FIXED | CONFIRMED-FIXED | 3→4 |
| R10 | minor | `gopp parse -fixit a.gopp -` rewrites a.gopp, then fails on `-`; the stdin check must come before any write | .scratch/repro_fixit_stdin_partial_write.sh | FIXED | CONFIRMED-FIXED | 3→4 |
| R11 | major | err_angle_type_params fix-it only swaps `<>`→`[]`: `func Map<T>(x T) T` → `func Map[T](x T) T` (missing constraint, does not parse); same for `<T, U>`, `<T extends Foo>`, `<T: Foo>` | .scratch/repro_angle_fixit_invalid.sh | FIXED | CONFIRMED-FIXED | 3→4 |
| R12 | minor | (manager, from reviewer notes) docs drift: tests/fuzz/README.md item 4 and the FuzzCheck.h header don't mention the parallel-vs-serial comparison; GOROOT counts in the docs (7354 files / 44 rejected) don't match a recount (7355 / 47) | none | FIXED | CONFIRMED-FIXED (reopened in 4: README vs compare-gofmt.py; script fixed) | 3→4→5 |
| R13 | minor | (manager, from reviewer notes) gofmt parity in the CLI: `gopp fmt DIR` errors (exit 2) on a subdirectory named `x.gopp` (gofmt skips directories while walking); `-l` on stdin prints `<stdin>` (gofmt: `<standard input>`) | none | FIXED | CONFIRMED-FIXED | 3→4 |
| R14 | major | (new in R11 fix) squareTypeParams fix-it silently changes meaning: `<T, U extends Foo>` / `<T, U: Foo>` → `[T, U Foo]` gives T the bound too; `<in T>` → `[in T]` (param named `in`); `<T: int, string>` → `[T int, string any]` | .scratch/repro_angle_fixit_round4.sh | FIXED | CONFIRMED-FIXED | 4→5 |
| R15 | minor | (new in R11 fix) isConstraint accepts any words: `<const T>`, `<T: Foo Bar>`, `<T super Foo>`, `<T: map>`, `<T: func>` get fix-its whose text does not parse | .scratch/repro_angle_fixit_round4.sh (R15 lines) | FIXED | CONFIRMED-FIXED | 4→5 |

Notes (below the bar, for later tasks):
- bench "serial" is best-of-N, so lazy line tables are warm after run 1; a cold single run is ~4.1 M lines/s vs 4.5 reported (still >4x the D2.9 budget).
- Only the first error per line is reported, so `gopp parse -fixit` fixes one item per line per run and exits 1 if more remain (documented).
- `-w` / `-fixit` write in place (truncate + write), not atomically.
- The `struct S { int x; }` / `class` fix-it fixes only the header; C-style fields stay (Go reads them as fields named `int`); P03 flags them.
- `for i,` newline `x in xs`: the for-in fix-it spans two lines; it applies correctly, only the caret display looks odd.
- FuzzCheck `checkPackage` names every piece `piece.gopp`, so a diagnostic credited to the wrong file is caught only by the unit test (distinct names).
