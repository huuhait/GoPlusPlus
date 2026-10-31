# p04-ops ledger (P04.23 operator lowering, one coder)

Decisions
- Operators are lowered as the ordinary method call the checker already built (`OperatorUse::Call`); `LowerOps.cpp` decides only operand order (`Swapped`), the lazy closure, `Negate`/`Sign`, the built-in `<=>`, and one-evaluation compound forms (`recordOps`/`LateOps`/`ReplayScope`).
- Debugger spelling: appendix A.9's readable form `main.Vec2.+(main.Vec2)` is kept, not `Vec2::operator+`. Not language-visible (no source, symbol or ABI change). Recorded in `docs/design/debug-info.md` ("Operator frames"); `tests/debug/ops.test`. Please confirm.
- `@Inline` together with `@NoInline` is a checker error (E0450, `sema/inline-marks.test`), manager decision; `inlineHintOf` keeps `@NoInline` first only as a fallback.
- Switch-case `==` records moved to `Info::CaseOperators` (the case value is both the key and the call's argument; one map recursed forever and collided with the value's own record). Merge + dump updated.
- Lazy `&&`/`||`: the right operand gets a synthetic closure frame in `Escape.cpp` (always non-escaping). A FuncLit or method value as the lazy operand is one E0439.
- Replay: an operator call sees its own statement's saved operands one level deep (`LateOps::Outer`), not a copy of every enclosing replay (that copy was quadratic in a swapped nest).
- GIR format: `Function::Inline` serialized, magic 0xB3. Verifier V079.

Moved (both-sided Done-whens written in the receiving entries): extends operators -> P04.22; defer/go/unwind -> P04.26/P12; range method -> P04.27; cross-package units -> P06.02; leak checks -> P04.09; differential test -> P04.14.

Review round 1 (9 findings, all fixed; each has a test that failed before)
- R6 blocker: an operator element at an instance was resolved by name only. Now `Checker::opAtInstance` (Operators.cpp) picks the overload whose signature is identical to the element's at the instance (the satisfiesOpElem rule), null = built-in; called from `MethodSets::resolveSelection` through `InstanceWalk::selection`, the one place. Tests: `run/ops-generic-table.test` (generated plain-vs-generic table with overload sets, `Inputs/gen-ops-table.py`), `ops-generic-overload`, `ops-generic-builtin`.
- R3 blocker: the lazy operand always got a frame closure. Now it has the Ctx of a func literal passed as parameter 0 of the operator method; `KeepTable` knows methods (not virtual/override/interface ones) and `classify` follows `x.M(arg)`. Tests: `run/ops-lazy-kept.test` (kept case runs), `gir/ops-lazy-escape.test` (kept: heap cell, not kept: no allocation).
- R1: `recordOperand(Elem)` for the compound `a[i] op= v` read (Operators.cpp). Test `run/ops-compound-method.test`.
- R2: unresolved `< <= > >=` under a `<=>` constraint go through `builtinCmp` and the sign test. Test `run/ops-generic-r2.test` (plain `nan < 1.0` stays IEEE) and the table.
- R5: the 8 raw `I.Operators.count` tests are `hasOpMethod`. Test `run/ops-generic-index.test`.
- R7: lazy FuncLit/method value and blank lazy initializer now name P04.27, `go` of an operator is in P12.04; both-sided items added. Tests `gir/ops-refused*.test`.
- R4: dead `O1:` check fixed; `scaled` other side and loop-vs-hand-written IR added (`codegen/ops-fadd.test`); the inline Done-when is changed in the TODO with the reason (LLVM's inliner takes both at opt 1+).
- R8: the nest has a call as left operand (a replay opens at every level); the control runs the quadratic algorithm (`LowerOptions::CopyEnclosingReplays`).
- R9: `gir/roundtrip.test` carries both hints and refuses a 0xB2 file.

Round 2 (reviewer, Mode 2): R1–R9 CONFIRMED-FIXED; extended generic table (20,545 rows incl. @Right and pointer receivers) 0 bad; KeepTable change only more precise. CLEAN.
