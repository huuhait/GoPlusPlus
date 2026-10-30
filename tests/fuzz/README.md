# Parser and formatter fuzzing (P02.10)

One libFuzzer target, `gopp-fuzz-parse`, feeds arbitrary bytes to the lexer, the
parser, the diagnostics and `gopp fmt` (`ParseFuzzer.cpp`; the checks are in
`FuzzCheck.h`).

For every input:

1. the lexer and parser return: no crash, no hang, no ASan/UBSan report, with
   the Go++ language rules on and off;
2. the tree is sound: every node range lies inside the file and is not
   reversed, and there is a Bad node only if an error was reported;
3. the diagnostics render (text with carets and fix-its, JSON), and the text
   with all their fix-its applied parses without a crash;
4. the text cut into several files (at NUL bytes, or in halves) parses as one
   package on a thread pool (`parseFiles`); the Bad-node check uses the error
   state of each file parsed alone, and the rendered diagnostics are byte-equal
   to those of a serial run, with no error limit and with a limit of 3;
5. if the text parses without errors:
   - `gopp fmt` succeeds;
   - the output parses without errors to the same syntax tree (positions
     ignored; like gofmt, empty statements, redundant parentheses, the spelling
     of numbers and the empty result list are not part of the tree; imports not
     sorted for this comparison);
   - formatting the output again gives the same text (idempotence);
   - every comment of the input is in the output (up to white space).

Two bits of the input size pick the variant of 1-4 (rules on/off, one file or
a package), so plain source files work as seeds; 5 runs for every input. A
violation prints the reason and aborts, so libFuzzer saves the input as
`crash-<hash>`.

## Files

| Path | What |
|---|---|
| `ParseFuzzer.cpp` | the libFuzzer entry point |
| `FuzzCheck.h` | the properties; shared with the unit test |
| `corpus/` | seed corpus: the ten spec examples, the formatter goldens, valid parser tests, the go/printer test data, one file per tailored mistake (P02.07) |
| `regressions/` | one minimized file per fixed bug |
| `gopp.dict` | libFuzzer dictionary: keywords, operators, Go++ forms, the words of the tailored messages |
| `run-fuzz.sh` | `smoke`, `full`, `fork` and `custom` runs |
| `triage.py` | groups `crash-*` files by cause, minimizes, saves regressions |

The regular build replays `corpus/` and `regressions/` with every check and
every variant in the unit test `FuzzCorpus.*` (`ninja check-unit`): no fuzzer
needed to keep the bugs fixed.

## The symbol demangler (P04.17)

`gopp-fuzz-symbol` (`SymbolFuzzer.cpp`) feeds arbitrary bytes to the demangler
of `docs/spec/appendix-a-symbol-names.md`, as a symbol, as a type and behind the
`_GP` prefix. For every input it returns; text it accepts is exactly what the
mangler writes for the tree it read (canonical), prints, and reads back as the
same node; text it refuses has a reason. The seeds are in `symbol-corpus/`
(replayed by the unit test `SymbolName.TheFuzzSeedsAreValidNames`). It is built
only from `compiler/lib/Mangle/SymbolName.cpp`, so the instrumented build is
small:

```
nix develop -c cmake -S . -B build-fuzz -G Ninja -DGOPP_BUILD_FUZZERS=ON
nix develop -c ninja -C build-fuzz gopp-fuzz-symbol
mkdir -p .scratch/symfuzz && cp tests/fuzz/symbol-corpus/* .scratch/symfuzz/
timeout 100 build-fuzz/bin/gopp-fuzz-symbol -max_total_time=60 -max_len=2048 .scratch/symfuzz
```

(1.9 million runs in 60 s under ASan and UBSan found nothing at the time of P04.17.)

## Build

The fuzzer needs sanitizers and coverage instrumentation in every library, so it
has its own build directory. The option `GOPP_BUILD_FUZZERS` adds
`-fsanitize=fuzzer-no-link` and (unless you set `GOPP_USE_SANITIZER`)
`-fsanitize=address,undefined -fno-sanitize-recover=all` to everything:

```
nix develop -c cmake -S . -B build-fuzz -G Ninja -DGOPP_BUILD_FUZZERS=ON
nix develop -c ninja -C build-fuzz gopp-fuzz-parse
```

## Run

```
nix develop -c tests/fuzz/run-fuzz.sh smoke        # 16 workers x 4 min
nix develop -c tests/fuzz/run-fuzz.sh custom 8 600 # 8 workers x 10 min
```

The script copies the seeds and regressions into `build-fuzz/fuzz-work/corpus`
(the growing corpus) and uses the dictionary. Crash files land in
`build-fuzz/fuzz-work/`. It refuses more than `MAX_WORKERS` (default 32)
processes, to leave a shared machine to others.

### The 24 CPU-hour run

CPU time is processes x seconds. On a small machine:

```
nix develop -c tests/fuzz/run-fuzz.sh full                    # 24 x 1 hour
```

On a big one, libFuzzer's fork mode (one parent, N children sharing the
corpus; a crash is saved and the run goes on, so triage comes afterwards):

```
MAX_WORKERS=96 nix develop -c tests/fuzz/run-fuzz.sh fork 96 900   # 96 x 15 min
```

Afterwards: triage (below), fix every cause, and run again until a whole
24 CPU-hours finds nothing. Commit corpus growth only if it helps coverage
(`-merge=1` into `corpus/` keeps only the files that add coverage).

## Triage

```
nix develop -c tests/fuzz/triage.py --minimize     # group, shrink one per cause
nix develop -c tests/fuzz/triage.py --save fuzz    # also copy to regressions/
```

`triage.py` replays each `crash-*`, `timeout-*`, `oom-*`, `leak-*` file and
groups them by signature: the failed check, or the sanitizer error with the top
three frames in our code. Per cause:

1. Take the smallest file (`--minimize` runs libFuzzer's `-minimize_crash`).
2. Reproduce: `build-fuzz/bin/gopp-fuzz-parse FILE` (inside `nix develop -c` so
   the stack is symbolized).
3. Fix the bug. Is the *check* wrong (the property does not hold for good
   reasons, e.g. gofmt also drops control characters in comments)? Then fix
   `FuzzCheck.h` and say why in a comment there.
4. Move the file to `regressions/` with a name that says what it tests, and add
   a unit test in `tests/unit/` for the fix.
5. Run `ninja check-unit`.

## What fuzzing found

Every cause was fixed; the minimized input of each is in `regressions/` (or a
unit test) and is replayed on every `ninja check`.

| Cause | Fix | Regression |
|---|---|---|
| `import ( "a"` with the first spec on the line of `(`, or `)` on the line of the last spec, was sorted only by the second pass | `settle` in Format.cpp formats until stable; import sorting runs on the printed text too | `import-first-spec-after-lparen`, `import-spec-on-lparen-line`, `import-rparen-on-spec-line` |
| `type T[P (b*struct{})]t`, `type _[P ((+n)),]e`: go/printer (and gofmt) print `[P b * struct{}]` / `[P +n]`, which parse differently | the printer keeps the call-style parentheses of a type parameter with a constraint that is not a type element, plus the comma | `type-param-binary-constraint`, `type-param-unary-constraint` |
| `var a = ``` was counted as a rewritten import path; a control character in a `/* */` line; `\r` in raw strings | the check (FuzzCheck.h) knows what gofmt does there | `empty-raw-string-is-not-an-import-path`, `control-char-in-block-comment`, `formfeed-in-string-literal` |
| a comment between `#` (or `@`) and the macro name was dropped or moved | the printer prints `#`, the package and the name at their positions | `comment-between-hash-and-macro-name` |
| a list or type cut off at the end of the file ended one byte past the file; a node that starts at an unconsumed token ended before it began | `after()`, `setEnd()` and `mk()` clamp the ends | `parser-bracket-past-eof`, `parser-reversed-range-macro-param`, unit test `NodeRangesStayInsideTheFileWhenTheTextIsCutOff` |
| `(c int, a, move b string)`: the type of the `move` entry was lost on the way | the backward pass keeps the `move` type | `parser-move-type-not-shared-null-type` |
| a parser diagnostic on a line hid a syntax error on the same line (one error per line), so `gopp fmt` printed `BadDecl` | `gopp fmt` checks no language rule, parses with a private engine, and refuses a tree with a Bad node | `fmt-syntax-error-on-line-with-warning`, unit test `AnEngineAtItsLimitDoesNotHideErrors` |
