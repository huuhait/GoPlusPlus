# Findings ledger: p04-allocword

Task: P04.25 Slice and map allocator word (ADR-0029). P04 wave 2, batch B. 3 coders → PM merge (Opus).
Spec: scratchpad p04-batchB-spec.md, section B4. Branch `task/p04-allocword-merged`, base master 702047a.

Manager decisions (PM merge):
- M1 base c2: one chokepoint per direction (`gopp.mem_resize_in` / `gopp.mem_free_in`, null → the global path), borrowed `[]T` parameters as a structural `Tuple(ptr, len)` (`sliceBorrowed`, same pattern as `mapByPointer`) so no code can read a borrowed word, `move []T` and results keep 4 words, `slice_clone` / `slice_free` / `map_free` for P04.09, the flag-gated test intrinsics (`-test-intrinsics`, E0442 without the flag), the per-path counting tests (`aw_paths`, `aw_maps`, `aw_nested`, `aw_borrow`, generated `aw_append_*` for 41 element types) and its mutation checks.
- M2 port from c3: the allocator object in exactly the P05 D5.3 shape (table inline at offset 0: alloc/resize/free; state after it), documented identically in gir.md §15 and P05 D5.3; the LLJIT test against an allocator written in C++ (proves the shape outside the compiler); the null-word fast path that LLVM inlines and folds to a bare `calloc` at opt ≥ 1 (keep c3's O2 FileCheck); `alloc-word-sizes` run test on both targets and the `sizes.test` positive control.
- M3 port from c1 only what is missing after M1/M2 (e.g. `AnInPlaceShrinkKeepsTheWord`, the 3-vs-4-operand verifier cases), no second test mechanism.
- M4 test intrinsics are gated by the hidden flag, never by name alone (c3's name-only gate is rejected): a user program declaring `gopp_test_*` without the flag gets the normal E0442.
- M5 borrowed-slice passing overlaps P04.18 (B3, in parallel): this task owns `sliceBorrowed`; record in gir.md that P04.18 and P04.06/07 must reuse it; whichever of P04.25/P04.18 merges second adapts.
- M6 every run test still matches `go run`; the only golden change is the `[][]int` stand-in (32-byte element); the opt matrix and IR-flag scan stay clean; gates under load: report times with `uptime`, the manager judges ASan time and suite time on a quiet machine.

| ID | Severity | Finding | Repro | Coder | Reviewer | Round |
|---|---|---|---|---|---|---|
| R1 | minor | test make_slice intrinsic: cap*esz unchecked for overflow, len<=cap and sign unchecked | .scratch/rv1/r1-make-overflow.sh | | reviewer | 1 |
| R2 | minor | Sizeof(borrowed []T param)=32 vs D4.5/Done-when text; 32 is right, spec needs the sentence | .scratch/rv1/r2-sizeof-param.gopp | | reviewer | 1 |
| R3 | minor | gir.md claims the checker refuses Clone of a borrowed slice; it accepts it → Clone of a borrowed param goes global (ADR point 7); record + P04.09 case | .scratch/rv1/r3-clone-borrowed.sh | | reviewer | 1 |
| R4 | minor | TODO P04.09 hand-off misses the old-buffer free on append growth (word + old cap) | none | | reviewer | 1 |
| R5 | minor | resize branch of mem_resize_in is dead and untested | none (grep) | | reviewer | 1 |
| R6 | minor | no P04.12 hand-off: 4-member slice debug type; two locals named like a borrowed slice param | none | | reviewer | 1 |

Manager decisions (round 1): R3 — Clone of a borrowed []T parameter or of a part uses the global allocator (the word was dropped, ADR-0029 point 7); record in SLC-3/SLC-4 and ADR-0029 as a consequence (not a new owner decision). R5 — option (b): remove the dead resize branch (mem_resize_in takes p == null only) and move "resize through the word" to TODO P05.05 with a both-sided Done-when; untested contract code does not stay.

Round 1 coder status (all FIXED, not committed):
- R1 FIXED: `gopp_test_make_slice` now calls `gopp.make_slice(alloc, esz, len, cap)` (RuntimeStr.cpp `case RtFn::MakeSlice`, LowerTest.cpp `case TF::MakeSlice`; `make` passes null, LowerStr.cpp) so it has Go's makeslice checks and the MaxAlloc/esz limit. Test: tests/run/core-panics-allocword.test + Inputs/panics-allocword/{cap-overflow,len-over-cap,negative-len,negative-cap}; repro now PASS.
- R2 FIXED: sentence in docs/spec/08-slices-and-range.md SLC-1, P04 README D4.5, TODO P04.25 Done-when; `golden/alloc-word-sizes.out` has both 32 (param type) and 16 (part, views).
- R3 FIXED (docs, TODO case for P04.09): gir.md section 15 sentence corrected; SLC-3, SLC-4, ADR-0029 consequence; TODO P04.09 both-sided case.
- R4 FIXED (TODO P04.09): old-buffer free on growth through slice_free/mem_free_in with the word and the old cap*esz, both-sided counted.
- R5 FIXED: RuntimeStr.cpp `case RtFn::MemResizeIn` takes p == null only and panics on a non-null p; test tests/gir/allocword-intrinsics.test (ON lines for mem_resize_in); gir.md section 15 and P05 D5.3 text identical; resize through the word moved to TODO P05.05 with a both-sided Done-when.
- R6 FIXED (hand-off): gir.md section 15 "Debug info hand-off" bullet and TODO P04.12. No compiler-made flag exists on `Local` in this tree (P04.12 adds it), so the pair is not marked yet; stated in both places.

Round 2 (reviewer): R1–R6 CONFIRMED-FIXED. R7 (panics-allocword not in the opt-matrix corpus) and R8 (vacuous ON-NOT) fixed by the manager: corpus entry added (252 programs, 4 from panics-allocword); the R5 check now captures the table pointer and checks (*T).0 is read and (*T).1 never is, bounded by the function end — the reviewer's mutation script now fails the mutated dump. CLEAN after these.
