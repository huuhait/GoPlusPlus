# Error codes (D3.11)

Every diagnostic has a stable code. The text output shows it as
`error[E0111]: undefined: x` (rendered via `ariadne` in `crates/gopp-diag`), the
JSON output as `"id":"err_undefined","code":"E0111"`.
`gopp explain E0111` prints what the error means, how to fix it, and an
example; `gopp explain --list` lists all codes.

- `docs/errors/codes.lock` is the append-only source of truth recording every
  diagnostic as a line `E<number> <name>` (`E0001`..). `crates/gopp-diag` parses
  and validates the registry (`gopp_diag::LockRegistry`), and its unit tests
  fail when any row is malformed, duplicated, removed, or renumbered. A new
  diagnostic takes the next free number (`GOPP_UPDATE_CODES=1 cargo test -p gopp-diag`
  appends it; never edit or renumber existing rows in `docs/errors/codes.lock`).
  `custom` (a message given as a string by a tool or plugin) has no code.
- The explanations live in `crates/gopp-diag/src/explain.rs`. Every explainable
  code carries what the error means, how to fix it, and a minimal example.
- Messages built from the operands (types2's wording) share one code per part of
  the checker (`err_sema_assign`, `err_sema_builtin`, `err_sema_call`,
  `err_sema_decl`, `err_sema_expr`, `err_sema_index`, `err_sema_predicate`,
  `err_sema_stmt`, `err_sema_typeset`, `err_sema_typexpr`). A message that
  needs its own code becomes its own row.
- Notes (`note_...`) have codes too but are printed without one: they belong to
  the diagnostic above them.
