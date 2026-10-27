# Findings ledger: p02-frontend-core

Task: P02.01–P02.06 (source manager, diagnostics, lexer, AST, parser core, macros & attributes). 3 coders → PM merge (base c2).
Branch: `task/p02-frontend-core-merged`

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | blocker | AST tree height unbounded (nesting 1000 × chain 4096): `gopp parse --dump-ast` segfaults (rc 139) in the recursive dumper on a crafted 2.4 MB input | .scratch/crash-deep-dump.sh | FIXED | CONFIRMED-FIXED | 1→2 |
| R2 | minor | (manager, from reviewer judgement) text diagnostics echo the whole source line (multi-MB line → MB of output, twice); clip to a window around the caret; and document the 1024 AST height / ~1022-term chain cap as an implementation limit | none | FIXED | CONFIRMED-FIXED | 2→3 |

Notes (below the bar, for later tasks):
- P02.07: unterminated-string diagnostic has a zero-width range; `(a, move b int)` recovery leaves a BadExpr field.
- `gopp parse` on a file whose reported size is 0 (e.g. /proc files) reads it as empty.
- `ninja format` and `format-check` in one ninja call overlap; run `format` alone first.
