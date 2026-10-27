# Findings ledger: p01-15-apply

Task: apply P01.15 owner decisions D1–D25 (01-language-spec/STATE.md) to docs/spec, plan READMEs/TODO, ADRs. Single coder, no merge.
Branch: `task/p01-15-apply-c1`

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | major | 05-parameters.md:14 list form `move a, b T` ambiguous with Go (`(move a)` + `(b T)`) and with LEX-12 | .scratch/mv | FIXED | CONFIRMED-FIXED | 1→2 |
| R2 | major | 04-ownership.md:433 OWN-V7 "the local itself when its type is a ref" can reject sample 03 `trim` returning a re-pointed borrowed param; "ref-typed field of a local" undefined | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R3 | minor | TODO.md:453 P14.06 "expansion view" wrongly renamed to "expansion ref" | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R4 | minor | TODO.md:82 "rejected loop-defer example" contradicts D17; 04-gir STATE.md:14 still "views" | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R5 | minor | 02-lexical.md:86 LEX-12 `ref` + `[` rule vs `ref [3]int` array param named ref | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R6 | minor | P01 STATE.md / root STATE.md not updated ("Not yet applied") | none | — | WITHDRAWN (manager updates at commit, per task) | 1 |
| R7 | minor | ADR-0006 Alternatives missing for D4, D6, D9, D15 | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R8 | minor | 02-lexical.md:115 §2.3 table lacks RangeClause/RangeVars/RangeVar and FieldDecl/FieldAttributes rows | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R9 | major | MAC-14 (13:83), P09/P01 READMEs: "macro args must be compile-time" rejects `#Assert(len(data) > 0, …)` and sample 08 `#Swap(tmp, other)` | .scratch/final_review.py R9 | FIXED | CONFIRMED-FIXED | 3→4 |
| R10 | major | D6 vs TYP-9/OWN-R3b: `case string:` matching a `ref string` binds a writable trivial copy vs a borrow; `v.(string)` on a ref unstated | .scratch/final_review.py R10 | FIXED | CONFIRMED-FIXED | 3→4 |
| R11 | major | hand-written `func (h *Handle) Clone()` pointer receiver: `x.Clone()` illegal on borrowed vars (PAR-1/OWN-R3b) and Handle fails `[T Clone]` | .scratch/final_review.py R11 | FIXED | CONFIRMED-FIXED | 3→4 |
| R12 | minor | P03 README:52 D3.9 diagnostic for `defer f.Close()` in a loop contradicts D17 | .scratch/final_review.py R12 | FIXED | CONFIRMED-FIXED | 3→4 |
| R13 | minor | RNG-1a `&` over arrays vs OWN-R3b (array by value inside a borrowed variable / assertion binding) | .scratch/final_review.py R13 | FIXED | CONFIRMED-FIXED | 3→4 |
| R14 | minor | PAN-3/CLO-3b: `&f` range variable with `defer` has no defined answer | .scratch/final_review.py R14 | FIXED | CONFIRMED-FIXED | 3→4 |
| R15 | minor | D24: which module's cflags/cxxstd apply to a dependency's in-module header (FOR-9/P07 vs P10) | .scratch/final_review.py R15 | FIXED (round 4) | CONFIRMED-FIXED | 4→5 |
| R16 | minor | P01 README:430 Q2 `m[k].Clone()` comma-ok form invalid; spec uses `clone(m[k])` | .scratch/final_review.py R16 | FIXED | CONFIRMED-FIXED | 3→4 |
| R17 | minor | LEX-12: `ref` in expression positions that hold types (`Dup[ref string]`, `(ref string)(x)`) ambiguous | .scratch/final_review.py R17 | FIXED | CONFIRMED-FIXED | 3→4 |
| R18 | minor | (manager, from below-bar) OWN-V7 code block defines `first` twice; MAC-3 vs 13.1 declaration list; ADR-0006 D23 "no ADR needed" though MAC-14 added; PAN-3 says when the loop variable moves into the deferred call | none | FIXED (round 4) | CONFIRMED-FIXED | 4→5 |
| R19 | major | OWN-R6 built-in `Clone` on pointer kinds vs OWN-P5 promoted `T.Clone`: `p.Clone()`/`u.Clone()`/`s.Clone()` have two meanings | .scratch/recheck_r3.py R19 | FIXED | CONFIRMED-FIXED | 4→5 |
| R20 | minor | LEX-12 bullet 1 (name positions incl. param name) vs bullet 2 (`ref string` in param list is a type) for `func f(ref string)` | .scratch/recheck_r3.py R20 | FIXED | CONFIRMED-FIXED | 4→5 |
| R21 | minor | (manager, from below-bar) ADR-0006 "Fourteen decisions" count; spec doesn't mention the loop-defer vet hint; RNG-1a array cases (package-level, field in owned local); MAC-14 decl/type/tokens kinds | none | FIXED | CONFIRMED-FIXED | 4→5 |
| R22 | major | lifetime extension (PAN-3) vs absolute block-end destruction in OWN-R2/STM-3/PAN-4; exit order, conditional defers, same var deferred twice unspecified | .scratch/recheck_r4.py R22 | FIXED | CONFIRMED-FIXED | 5→6 |
| R23 | minor | RNG-6a example: `d *Doc` from `iter.Seq[*Doc]` called read-only and `d.Clone()` to keep (contradicts OWN-R3b and R19 tie-break) | .scratch/recheck_r4.py R23 | FIXED | CONFIRMED-FIXED | 5→6 |
| R24 | minor | (manager, from below-bar) `@Pinned` exclusion reason obsolete under lifetime extension; `main` exit order for extended locals vs INI-2; P03 D3.9 vet hint under "become errors" heading | none | FIXED | CONFIRMED-FIXED | 5→6 |
| R25 | minor | OWN-R5 rationale (04:229) says a non-deferred local "holds the zero value, drop is a no-op" — contradicts PAN-3 part 3 (real destruction at block end) | .scratch/recheck_r5.py | FIXED | CONFIRMED-FIXED | 6→7 |
| R26 | minor | (manager, from below-bar) PAN-3: `defer unlock(&mu)` in a loop: `&x` written in the defer statement now extends x like any other use (stale "compiler doesn't track raw pointers" reason); pointers obtained earlier stay UB-1; UB-1 example wording for extended locals; ADR-0006 D17 missing "and" | none | FIXED | CONFIRMED-FIXED | 6→7 |
| R27 | minor | PAN-3:165 "explicit refs to dying things keep their errors" vs :168 (`&x`/refs written in the defer extend x); plus (manager, below-bar) OWN-R3c should name `defer f(&a)`; UB-1:37 grammar | .scratch/recheck_r6.py | FIXED | CONFIRMED-FIXED | 7→8 |
| R28 | major | OWN-R8/UB-1 "a defer that uses a borrowed variable" is caught → rejects `defer fmt.Println("done", name)` with a borrowed param; regression vs plan D1.4 rule 8, CON text; PAN-3 lost "borrow if it lives until return" | .scratch/final_r28.py | FIXED | CONFIRMED-FIXED | 9→10 |
| R29 | minor | P09 STATE.md:39 still has pre-R9 "macro arguments must be compile-time values" | .scratch/final_r28.py | FIXED | CONFIRMED-FIXED | 9→10 |
| R30 | minor | OWN-R6 bullets 3 vs 4: pointer-receiver `Clone` with another signature (Go std `(*tls.Config).Clone() *Config`) legal or error? | .scratch/final_r28.py | FIXED | CONFIRMED-FIXED | 9→10 |
| R31 | minor | raw `*T`: "pointer's own Clone wins" silently changes Go meaning of `p.Clone()` when T has `Clone()` (`:=` gets no hint) | .scratch/final_r28.py | FIXED | CONFIRMED-FIXED | 9→10 |
| R32 | minor | (manager, from below-bar) GEN-2 Unique needs cloneable T; OWN-R6 example missing drop; MAC-13 nested func literals; MAC-3 vs MAC-12 field attrs; P02 lookahead wording; P10 ldflags combine; interface types with Clone(); RNG-8 func-literal params; `&v` map alias used by defer vs later inserts | none | FIXED | CONFIRMED-FIXED | 9→10 |
| R33 | minor | OWN-R6:234 vs :238: `cfg.Clone()` on `*tls.Config` (method declared on `*Config`) both legal and an ambiguity error; GEN-2 same | .scratch/recheck_r9.py | FIXED | CONFIRMED-FIXED | 10→11 |
| R34 | minor | RNG-1a map-alias-in-defer error vs RNG-1a:94/PAN-3/CLO-3b "place, fine if collection lives"; fix-it `v.Clone()` wrong for pointer calls; plus (manager) slice `&d` alias kept by a defer vs later append; README:77 list; MAC-13 sentence order; spec note on `[T Clone]` vs direct `p.Clone()` | .scratch/recheck_r9.py | FIXED | CONFIRMED-FIXED | 10→11 |
| R35 | minor | 08:94 / 09:166 cite UB-2e for a hidden later append to `xs` after the loop with a pending `&xs[i]` defer; that is UB-1; PAN-3 wording calls it "fine" | .scratch/recheck_r10.py | FIXED | CONFIRMED-FIXED | 11→12 |
| R36 | minor | CLO-3b (11:32) map-alias exception only removes the second condition, so a deferred literal capturing a map `&v` is allowed vs RNG-1a:93 error | .scratch/recheck_r10.py | FIXED | CONFIRMED-FIXED | 11→12 |

Notes (below the bar, for later phases):
- P08: an `&x` alias over a C++ container kept by a `defer` can dangle if the container grows (`push_back`) after the loop; extend the slice append/reassign rule to "or grown" when P08 specifies container ranges.
| R37 | major | OWN-V7 (04:435): "checks only" list omits returning the owned local / `move` param itself as a ref (implicit conversion) → compile error vs UB | .scratch/final_r37.py | FIXED | CONFIRMED-FIXED | 13→14 |
| R38 | minor | (manager, from below-bar) clone(p) on raw *T vs OWN-R6 ambiguity; `defer show(v)` with map &v vs PAN-3; D17 codegen not in P04 plan; leftover //gopp:cflags in source; generic plain ...T; CON:32 move wording; P01 README D1.14 decl list; P08 one-var container range; OWN-R3c:171 garbled; generic `!` with T=error | none | FIXED | CONFIRMED-FIXED | 13→14 |
| R39 | minor | 12-concurrency.md:32 "borrowed variable cannot be moved, except a slice range variable in & form" vs CON-1/OWN-R3b/RNG-1a (& vars are aliases, map &v movable) | .scratch/final_r39_r40.py | FIXED | CONFIRMED-FIXED | 14→15 |
| R40 | minor | RNG-6 / P01 D1.21 "C++ containers work like a slice" vs P08 D8.6 one-variable form (key for std::map, error for std::list); two-variable form for std::map unstated; P04 README cites D1.17 instead of D1.9/ADR-0006 D17 | .scratch/final_r39_r40.py | FIXED | CONFIRMED-FIXED | 14→15 |
- P08: say whether key / mapped-value variables of an associative C++ container range are borrowed variables (like OWN-R3b item 3 for Go maps).
| R41 | minor | OWN-V7 first sentence says ref into a temporary at return is an error; "checks only" list omits temporaries | .scratch/final_r41.py | FIXED | CONFIRMED-FIXED | 16→17 |
| R42 | minor | OWN-R6/GEN-2: compiler-provided Clone vs a promoted embedded Clone (`struct{ *tls.Config }`, `struct{ *http.Request }`) undefined | .scratch/final_r42.py | FIXED | CONFIRMED-FIXED | 16→17 |
| R43 | minor | (manager, from below-bar) 13.3 duplicate `type User`; TYP-9 unreachable `case ref string` → compile error; LEX-12 bullet 3 wording; P01 README D1.9 "lives until return"; ADR-0006 plan refs; TODO P02.06/P08.06/P09.06; D6 interface equality of boxed `ref string`; OWN-R3c `return a` vs OWN-R10 | none | FIXED | CONFIRMED-FIXED | 16→17 |
| R44 | minor | R42 regression: trivial `struct{ *tls.Config }` is Copy but "not cloneable" vs OWN-R6:239 / GEN-2:20 "every Copy type is Clone" | .scratch/final_r44.py | FIXED | CONFIRMED-FIXED | 17→18 |
| R45 | minor | R43#8 regression: OWN-R3c `return a` exception also covers escaping-closure captures (shared cell can't be the result slot); plus OWN-R10 multiple return paths | .scratch/final_r45.py | FIXED | CONFIRMED-FIXED | 17→18 |
| R46 | minor | (manager, from below-bar) OWN-R10 "at most one local in the result slot" has no tie-break (which one compiles depends on compiler choice); STM-6 "never moves again" needs OWN-R10 cross-ref | none | FIXED | CONFIRMED-FIXED | 18→19 |
