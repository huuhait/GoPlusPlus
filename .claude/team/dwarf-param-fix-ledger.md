# Findings ledger: dwarf-param-fix

Task: at a function breakpoint (and on `step` into a function) at opt 0, gdb showed wrong parameter values (`one(20)` showed `a=16`), found in the P04.18 final review; a P04.12 bug on master. One Opus coder, reviewer loop.

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | major | gdb `step` into a function stopped before the parameter stores: gdb 17 trusts the line table only for a `clang ` producer, else its prologue analyser needs a frame pointer | .scratch/rv/repro-step.sh | FIXED: `"frame-pointer"="all"` at opt 0 with DWARF (as Clang -O0); producer left honest | CONFIRMED-FIXED | 2 |
| R2 | minor | only the last statement of the entry group was `Prologue`; no verifier rule | none | FIXED: whole group flagged; V077 | CONFIRMED-FIXED | 2 |

Round 2 (reviewer, Mode 2): CLEAN. `-no-debug` output byte-identical to master (1836/1836 objects).
