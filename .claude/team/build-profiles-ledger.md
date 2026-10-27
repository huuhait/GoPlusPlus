# Findings ledger: build-profiles

Task: add build profiles to the plan (P06/P09/P10/P11), per root STATE.md "Queued next". Single coder, no merge (docs only).
Branch: `task/build-profiles-c1`

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | major | P06 README:179 cites nonexistent "P11 D11.15" | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R2 | major | P11 D11.4 / :99 / ~:220 still say `mode static` (default) vs D11.13 "link decides" | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R3 | major | `link` as a strict target filter breaks default debug builds for static-only cdeps and musl targets | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R4 | minor | `strip true` meaning stated two ways (P06 ~137 vs ~142, P11 ~186) | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R5 | minor | P04 D4.6 lacks the `panic abort` (recover nil, exit 2) sentence D6.12 cites; root STATE / P01 STATE bookkeeping (manager at commit) | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R6 | major | P11:213 says the cdep `profile ( … )` pin sets the `link` preference vs D10.11/D11.13:210/D6.12 (pins take only opt/debuginfo; link is whole-program) — came from manager R3 wording | none | FIXED | CONFIRMED-FIXED | 2→3 |
| R7 | minor | P01 README D1.14 predeclared macro / built-in attribute lists lack `#Profile`, `#Setting`, `@If` (spec MAC-6/MAC-8 have them) | none | FIXED | CONFIRMED-FIXED | 4→5 |
| R8 | minor | root README principle 4 "semantics identical in both" vs D6.12 `panic abort` exception | none | FIXED | CONFIRMED-FIXED | 4→5 |
| R9 | major | P11 D11.4/D11.13: explicit `mode dynamic` parent + preference-only leaf in release (link static) → leaves-up makes leaf static → error; rule ambiguous | none | FIXED | CONFIRMED-FIXED | 4→5 |
| R10 | minor | (manager, from below-bar) P04 Q2 "debug mode" wording; spec PAN-7 should mention `-panic=abort` is the `panic` profile setting | none | FIXED | CONFIRMED-FIXED | 4→5 |
