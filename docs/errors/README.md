# Error codes (D3.11)

Every diagnostic has a stable code. The text output shows it as
`error[E0111]: undefined: x`, the JSON output as `"id":"err_undefined","code":"E0111"`.
`gopp explain E0111` prints what the error means, how to fix it, and an
example; `gopp explain -list` lists all codes.

- The table is `compiler/include/gopp/Basic/DiagnosticIDs.def`:
  `DIAG(Name, Code, Severity, Text)`. A new diagnostic takes the next free
  number; `custom` (a message given as a string by a tool) has no code.
- `docs/errors/codes.lock` records every code ever given. `ninja check-diag-codes`
  (part of `check`) fails when a row is removed, renumbered, or its number is
  taken by another name, and when a new row is missing from the lock file
  (`tools/check-diag-codes.py --def ... --lock ... --update` appends it; never
  edit a line by hand). A row that is no longer reported stays in the table.
- The explanations are `compiler/lib/Basic/DiagnosticExplain.def`. The unit
  test `Explain.EveryExampleMakesTheCompilerReportItsDiagnostic` compiles the
  example of each entry and checks that it reports that diagnostic. The few
  entries it cannot compile are listed in the test with the reason.
- Messages built from the operands (types2's wording) share one code per part of
  the checker (`err_sema_assign`, `err_sema_builtin`, `err_sema_call`,
  `err_sema_decl`, `err_sema_expr`, `err_sema_index`, `err_sema_predicate`,
  `err_sema_stmt`, `err_sema_typeset`, `err_sema_typexpr`). A message that
  needs its own code becomes its own row.
- Notes (`note_...`) have codes too but are printed without one: they belong to
  the diagnostic above them.
