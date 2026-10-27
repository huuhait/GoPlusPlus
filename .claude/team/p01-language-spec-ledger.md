# Findings ledger: p01-language-spec

Task: P01 Language specification v0 (P01.01–P01.11, P01.13, P01.14, P01.16). Single coder, no PM merge.
Branch: `task/p01-language-spec-c1`

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | major | 03-types.md:26 TYP-2 bans view→owned assignability but PAR-8/D1.5 pass views to `string`/`[]T` params (sample 03 relies on it) | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R2 | major | 16-undefined-behavior.md:40-42 `return s[:2]` of a local claimed "caught"; D1.4 rule 8 / D1.6 say views kept past owner are not caught | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R3 | major | 09-errors-and-panics.md:142-143 `return ErrNotFound` in `func find!() int` breaks ERR-3; needs `return 0, ErrNotFound` | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R4 | minor | 08-slices-and-range.md:60-61 `buf[:2]` after `buf = buf[:0]` slices past len (panics per own Open note) | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R5 | minor | 13-macros-surface.md:10-11,23 MacroDecl/ProcMacroDecl not in TopLevelDecl; MacroInvocation in both SimpleStmt and Operand (ambiguous) | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R6 | minor | 14-foreign-surface.md:82 directive after imports contradicts Open note; `stdmap.map` keyword member unaddressed; 13-macros-surface.md:99 import after decls | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R7 | minor | 16-undefined-behavior.md:55-56 `u.SetName(u.Name)` cause contradicts STM-1 eval order | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R8 | minor | 04-ownership.md OWN-R1/R2/R6/R8/R9/R10 lack own examples (P01.03 done-when) | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R9 | minor | 08-slices-and-range.md:76 RNG-2 `range move(xs)`: remaining elements on break/return/panic unspecified, no Open note | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R10 | minor | examples/04-binarytrees/README.md:9 cites EXP-10 but links 04-ownership.md (it's in 06-expressions.md) | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R11 | minor | CON-4 (buffered value dropped with last channel handle) and range over C++ container are plan-silent with no Open note | none | FIXED | CONFIRMED-FIXED | 1→2 |
| R6 | minor | (reopened as R19) FOR-4 Open note wrongly says keyword-named members are undecided; P07 D7.3 / P08 Q1 decide trailing underscore | none | FIXED (via R19) | CONFIRMED-FIXED (via R19) | 3→4 |
| R12 | major | 11-closures.md CLO-1/CLO-3 + PAN-3: non-escaping closures can outlive captured vars: (a) deferred func literal in loop capturing loop-local; (b) closure stored in outer-scope local | .scratch/r_deferclosure, .scratch/r_outerlocal | FIXED | CONFIRMED-FIXED | 3→4 |
| R13 | major | CLO-2/CON-2/CON-3: escaping closure capturing a BORROWED variable (borrowed param, range var) keeps only the borrow; no rule/UB/Open | .scratch/r_capturedborrow | FIXED | CONFIRMED-FIXED | 3→4 |
| R14 | major | 09 PAN-3 defer-argument rule incomplete: raw pointers to dying locals; owned temporaries; shared-handle args/receivers | none | FIXED | CONFIRMED-FIXED | 3→4 |
| R15 | major | 11 CLO-5 Open note: value-receiver method value on owning type captured by reference silently diverges from Go (contradicts D1.11, not in P04 D4.10) | .scratch/r_methodvalue | FIXED | CONFIRMED-FIXED | 3→4 |
| R16 | major | 05-parameters.md:69 PAR-5 `return b.buf` illegal per OWN-R3a/STM-6; needs `move(b.buf)` | none | FIXED | CONFIRMED-FIXED | 3→4 |
| R17 | minor | 08 RNG-1 range var "borrows" for trivial elements (diverges from Go) and owning element reassignment dangles; no Open/UB entry | .scratch/r_rangetrivial | FIXED | CONFIRMED-FIXED | 3→4 |
| R18 | minor | 04 OWN-R5 lacks example+rationale; OWN-R3/R3a/R4/R7 lack rationale | none | FIXED | CONFIRMED-FIXED | 3→4 |
| R19 | minor | 14 FOR-4: state P07 D7.3 rule (keyword names get trailing underscore, `stdmap.map_[K,V]`), remove wrong Open note | none | FIXED | CONFIRMED-FIXED | 3→4 |
| R20 | minor | grammar duplicates: MacroInvocation in PrimaryExpr (06:14) and Operand (13:25); ViewType in TypeLit (03:11) and Type (04:302); 02:48 `!` "before ( or [" vs grammar | none | FIXED | CONFIRMED-FIXED | 3→4 |
| R21 | minor | samples use unnecessary `view string` params (02:20, 03:22/33/44, 10:152); 02-wc README:12 wrong claim | none | FIXED | CONFIRMED-FIXED | 3→4 |
| R22 | minor | 11-closures.md:32 CLO-3b fix-it `defer func(f File){ f.Close() }(move(f))` illegal (borrowed param, pointer-receiver Close); needs `f own File` | none | FIXED | CONFIRMED-FIXED | 4→5 |
| R23 | major | `delete(m,k)` during range allowed (RNG-3/D1.21) but k/v borrow the entry and delete drops it → common idiom reads freed memory; not in UB-2a, not defined | .scratch/r_rangedelete | FIXED | CONFIRMED-FIXED | 4→5 |
| R24 | minor | 11-closures.md:11 vs :32: CLO-1 "non-escaping only if cannot outlive captures" contradicts CLO-3b (defer not an escape); CLO-3b "not assigned after the defer" wrongly rejects Go rollback idiom and PAN-10 example | .scratch/r_deferassigned | FIXED | CONFIRMED-FIXED | 4→5 |
| R25 | minor | RNG-3 delayed drop covers only `delete`; `move(m[k])` of the current entry during range drops the key at once → borrowed range key `k` dangles; not in UB-2a | .scratch/r_rangemovemap | FIXED | CONFIRMED-FIXED | 5→6 |
| R26 | minor | RNG-3: after `move(m[k])` of current entry, what borrowed `v` reads is unspecified (alias of moved value vs zero value) | none | FIXED | CONFIRMED-FIXED | 6→7 |
| R27 | major | `go f(x)` args from places/borrowed vars: CON-1 "moved" vs OWN-R3a borrow; no case list like PAN-3; 15-program-init:41 `go background(logger)` | none | FIXED | CONFIRMED-FIXED | 8→9 |
| R28 | major | `move(k)`/`move(v)` on map range variables offered as fix-it; corrupts map key or ambiguous for values | none | FIXED | CONFIRMED-FIXED | 8→9 |
| R29 | major | ownership of type-assertion results / type-switch vars with owning dynamic type unspecified (box is shared+immutable) | none | FIXED (round 9) | CONFIRMED-FIXED | 9→10 |
| R30 | minor | OWN-R3a lists all range variables; must be "borrowed range variables (RNG-1)" (rejects sample 05 lines 55, 61 and 08:120 literally) | none | FIXED | CONFIRMED-FIXED | 8→9 |
| R31 | minor | PAN-3/CLO-7 "assigned after the defer": field writes / pointer-receiver mutation undefined → silent divergence from Go | .scratch/r_deferfield | FIXED | CONFIRMED-FIXED | 8→9 |
| R32 | minor | range-over-func Open note: body non-escaping vs CLO-4; no yield-after-exit panic or UB entry | .scratch/r_yieldkept | FIXED | CONFIRMED-FIXED | 8→9 |
| R33 | minor | RNG-4 "any insert during iteration panics" vs P05 D5.4 "panics if table must grow"; no Open note | none | FIXED | CONFIRMED-FIXED | 8→9 |
| R34 | minor | RNG-1 silent on shared-handle elements ([]func, []error, []chan, []Shared) | none | FIXED | CONFIRMED-FIXED | 8→9 |
| R35 | minor | shared-handle params passed without retain (PAR-7) → callee releasing the caller's handle is UAF; not in UB-2 / ch.16 | none | FIXED | CONFIRMED-FIXED | 8→9 |
| R36 | minor | 12-concurrency.md:16 CON-1 "or a closure" fix-it illegal for borrowed variables (CON-3/CLO-3a) | none | FIXED | CONFIRMED-FIXED | 9→10 |
| R37 | minor | 08:77 RNG-1 / 03:76 TYP-9: borrowed range / type-switch variables not stated read-only (writable would silently diverge from Go) | .scratch/r_rangevarwrite | FIXED (round 10) | CONFIRMED-FIXED | 10→11 |
| R38 | minor | 04:129-134 OWN-R3b example is itself UB-2a; RNG-3:99/UB-2a:77 call the recommended fix-it writes UB; TYP-9 "(like RNG-3)" citation wrong | none | FIXED | CONFIRMED-FIXED | 10→11 |
| R39 | major | CLO-3a:28 / CON-1:16 borrowed-variable lists omit assertion/switch bindings and map range vars (go log(s) dangles); OWN-R3b map range vars read-only vs trivial-copy note | none | FIXED | CONFIRMED-FIXED | 10→11 |
| R40 | major | OWN-R3b read-only over-bans: borrowed params lose PAR-2 re-pointing and PAR-1 element writes through slices (D1.5); sample 03 `trim` illegal; `row[j] = 0` over [][]T banned | none | FIXED | CONFIRMED-FIXED | 11→12 |
| R41 | minor | PAN-3 (09:165) offers `move(x)` for borrowed variables; must use the OWN-R3b term and exclude move like CON-1 | none | FIXED | CONFIRMED-FIXED | 11→12 |
| R42 | minor | OWN-R3b (04:135-136) does not ban `&d.F` / pointer-receiver calls on a by-value field of a borrowed variable; CLO-7 counts them as writes → silent divergence one level down | .scratch/r_fieldptrcall | FIXED | CONFIRMED-FIXED | 12→13 |
| R43 | major | moving a variable still captured by a closure (CLO-1/CLO-2) or borrowed by a pending deferred call (PAN-3) is not caught; later call silently reads zero value (CLO-7 "written" omits move) | .scratch/r_capturedmove | FIXED | CONFIRMED-FIXED | 14→15 |
| R44 | minor | range over array with owning elements: RNG-6 "as in Go" (copy) vs RNG-1/OWN-R3b live borrow; writes to later elements change what x reads | .scratch/r_rangearray | FIXED | CONFIRMED-FIXED | 14→15 |
| R45 | major | ranged slice reallocated where compiler can't see it (method append): no rule, no UB entry; range lowering (snapshot vs re-read) undecided | .scratch/r_rangehiddenappend | FIXED | CONFIRMED-FIXED | 14→15 |
| R46 | minor | 08:103 says writing current element is UB-2a; 08:93 and UB-2a:66 say defined (R38 leftover) | none | FIXED | CONFIRMED-FIXED | 14→15 |
| R47 | minor | OWN-R3c rejects same-statement moves (OWN-R4 implicit `x = append(x, …)`, swaps) and `return` of a named result captured by a deferred literal — contradicts D1.4 rule 4 and CLO-3b | .scratch/r_r3cappend | FIXED | CONFIRMED-FIXED | 15→16 |
| R48 | minor | RNG-6 (08:117) says any hidden write to an owning-element array is UB-2a, and any visible write an error; UB-2a says writes are defined, slices allow other-element writes; no ch.16 array row | none | FIXED | CONFIRMED-FIXED | 15→16 |
| R49 | minor | range over an owned string while body grows/reassigns it (`s += …`): no compile error, no UB entry; RNG-3/3a/UB-2e cover slices/maps only | .scratch/r_rangestring | FIXED | CONFIRMED-FIXED | 15→16 |

Notes for owner review (P01.15), below the merge bar:
- OWN-R3c same-statement exemption: `out = transform(out, add)` where `own` param receives `out` and `add` captures `out` — `add` would read the zero value if called mid-call (reviewer round 16).
| R50 | major | map insert key ownership (`m[k] = v`, `m[k]++`) with owning non-view key unspecified: borrowed `k` (word-count idiom) and owned local `k` (conditional move) | none | FIXED | CONFIRMED-FIXED | 17→18 |
| R51 | minor | PAN-3 bullets 4 vs 5 for `defer x.M()` (pointer receiver, function-level owned local); 09:193 comment + samples 04/10 READMEs use the reading that makes `arena.Reset()` illegal | none | FIXED | CONFIRMED-FIXED | 17→18 |
| R52 | minor | `own`-receiver methods and interface method sets unspecified (box immutable; P03 D3.1 needs an answer) | none | FIXED | CONFIRMED-FIXED | 17→18 |
| R53 | minor | `m[k] = append(m[k], v)` implicit move: remove+reinsert vs take-in-place unspecified (OWN-R4 vs OWN-R3a) | none | FIXED (round 18) | CONFIRMED-FIXED | 18→19 |
| R54 | minor | (manager, from below-bar list) `name := ctx.Value(k).(string)` binding borrows a temporary's box → OWN-R8 error on common Go idiom | none | FIXED | CONFIRMED-FIXED | 17→18 |
| R55 | minor | (manager, from below-bar list) RNG-3/UB-2e mention in-place re-slice of owned strings, which SLC-6/OWN-V2 don't allow; CON-1 "never for a borrowed variable" contradicts OWN-R3b slice-range `move(u)` | none | FIXED | CONFIRMED-FIXED | 17→18 |
| R56 | minor | after `defer x.M()` (pointer receiver), moving `x` is allowed (OWN-R3c misses `&x`) → deferred call reads zero value where Go reads the value | .scratch/r_deferptrmove | FIXED | CONFIRMED-FIXED | 18→19 |
| R57 | minor | (manager, from below-bar) trivial map key: range key `k` is a writable copy, so after `k++` `m[k]` no longer names the current entry; R53 rule must say "k not written in the body" | none | FIXED | CONFIRMED-FIXED | 19→20 |
| R58 | minor | (manager, from below-bar) examples/04-binarytrees/main.gopp:64 comment "not assigned later" gives the old reason for the defer being legal | none | FIXED | CONFIRMED-FIXED | 19→20 |
- Variadics (05-parameters.md:20 Open note): plain `...T` with owning non-interface `T` is a compile error, which rules out `filepath.Join(dir, name)` / `exec.Command(name, args...)` as written; likely the most important Open note (reviewer round 20).
- PAN-3 / CLO-7 with a field reached through a pointer (`defer log.Println(p.Name)` then `p.Name = …`): say explicitly that the write to the place counts (round 20).
- `move(x)` on an ARRAY range variable: OWN-R3b allows it only for slice range vars, RNG-6 says arrays follow "exactly the slice rules" (round 20).
- Sample 09 README: `clone(w)` described as needed; `move(w)` would also be legal (round 20).
