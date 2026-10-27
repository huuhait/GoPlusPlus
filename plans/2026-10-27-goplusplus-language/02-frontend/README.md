# P02 — Front-end: lexer, parser, AST, `gopp fmt`

**Goal:** parse all of Go++ v0 into an AST with precise source ranges and good diagnostics, fast. Ship `gopp fmt` early so the canonical style exists from day one.
**Depends on:** P01 (grammar) · **Unblocks:** P03, P09, P14 · **Size:** M · **Milestone:** M0

## Decisions

**D2.1 — Hand-written lexer and recursive-descent parser.** Go and Clang both work this way. It is the easiest way to handle Go's quirks (automatic semicolons, the composite-literal ambiguity in `if`/`for`/`switch` headers) with good error recovery and top speed.
*Rejected:* parser generators (worse errors, awkward with semicolon insertion); PEG (backtracking cost).

**D2.2 — AST in arenas, semantics in side tables.**
- Nodes are bump-allocated per file and are immutable after parsing. Identifiers are interned.
- Types and resolutions live in side tables owned by P03, so the same AST serves both the compiler and the LSP.

**D2.3 — Comments live in a comment map (like `go/ast`), not a full CST.** That is enough for `gopp fmt` and doc extraction. There is no CST: the LSP uses the AST plus re-lexing for token-level needs (decided, Q1).

**D2.4 — Clang-style source locations.**
- Files are mmapped into one global 32-bit offset space, so a position is 4 bytes.
- Each LSP snapshot gets its own offset space (P14 D14.3).
- Line tables are built lazily.

**D2.5 — Diagnostics engine.**
- Each diagnostic has a severity, primary and secondary ranges, notes and fix-its.
- Text output uses Clang-style carets. `-diag-format=json` serves tools.
- The default limit is 10 errors per package, as in Go.

**D2.6 — The parser doesn't interpret macros.**
- `#Name(...)` and `@Name(...)` become nodes holding a balanced token tree, which P09 interprets. Qualified names (`#pkg.Name(...)`, `@pkg.Name`) work the same way (P01 D1.14).
- `macro` definitions are parsed fully, since their bodies are ordinary Go++ syntax with fragment parameters.

**D2.7 — Header imports are ordinary import specs at this stage.** The parser accepts the `"<…>"` form. Whether any other path is a Go++ package or a C/C++ header is decided later, from gopp.mod (P01 D1.15, P10 D10.1).

**D2.8 — `gopp fmt` applies gofmt's rules, extended to the new syntax.**
- One style, no options: tabs, aligned struct fields and comments, grouped and sorted imports.
- Header imports keep their order, because C header order can matter. `gopp fmt` sorts an import group (lines not separated by a blank line) only when the group has no header import.
  - To tell headers from packages, it uses the compiler's rule (P01 D1.15, P10 D10.1) with the nearest gopp.mod, as `goimports` does.
  - Without a gopp.mod, only `"<…>"` paths and paths ending in a header extension (P01 D1.15) count as headers.
- Invariant: `parse(print(parse(src))) == parse(src)`.

**D2.9 — Budget (provisional): lex + parse ≥ 1 M lines/s per core** on the bench corpus. Files of one package are parsed in parallel.

## Design notes

- **Semicolon insertion:** Go's rule, unchanged (D1.2).
- **Disambiguation:**
  - `own` and `view` are keywords, so type parsing stays LL(1). `Unique[T]`, `Shared[T]` and `Weak[T]` are ordinary generic types.
  - In a parameter list, `own` is a mode followed by a type (`v own T`).
  - `!` right before a parameter or argument list — after a name, `]`, `)` or the keyword `func` — marks a function that can fail (`func f!(…)`, `func!(int) T`) or an unwrapping call (`f!(x)`, `parse[int]!(s)`), D1.8. In unary position it is logical not. Go never puts `!` after a name, `]`, `)` or `func`, so this is unambiguous.
- **Token trees:** `#Assert(x > 0, "msg")` stores the tokens `x > 0 , "msg"` with their bracket structure. P09 decides how each fragment is parsed.
- **Error recovery:**
  - Resynchronize on `;`, `}`, and keywords that start statements and declarations (the technique Go's parser uses).
  - Tailored messages explain Go habits that are errors in Go++. For example, "a `*T` field doesn't own: did you mean `Unique[T]`?" is reported by P03 through hooks the parser provides.

## Deliverables

- `compiler/lib/{Basic,Lex,AST,Parse,Format}`.
- `gopp parse --dump-ast|--dump-tokens` and `gopp fmt [-l] [-w] [-d]`.
- Golden tests for every grammar production.
- Ported Go scanner and parser test cases (BSD-licensed, attributed).
- A libFuzzer target and its corpus.

## Exit criteria

- Every production of the P01 grammar has a test, and all ten P01 samples parse.
- The `gopp fmt` round-trip invariant holds on the corpus, and the gofmt-derived golden tests pass.
- 24 CPU-hours of parser fuzzing produce no crash.
- The D2.9 budget is met, or an ADR records the gap and the plan to close it.

## Risks

- **Grammar churn from P01/P09** → no parser tables, so changes stay local; heavy golden testing.
- **Formatter edge cases** → reuse gofmt's test corpus where the syntax overlaps.

## Open questions

- **Q1** (decided 2026-10-27) A full CST for the LSP? No: the LSP uses the AST plus re-lexing (D2.3).
- **Q2** Macros in type position (`var x #Vec(int)`)? Owned by P09 (P09 Q3).
