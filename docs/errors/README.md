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
- Every message has a row of its own. Rows whose feature the language no longer
  has (`err_view_*`, `err_unit_*`, `err_strict_*`, and the shared `err_sema_*`
  rows of an earlier scheme) are *retired*: nothing is reported with them, and
  their numbers are never used again. A row that this version reports nothing
  with yet is *reserved*. `gopp explain --list` marks both.
- Notes (`note_...`) have codes too but are printed without one: they belong to
  the diagnostic above them.
