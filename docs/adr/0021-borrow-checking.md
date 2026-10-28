# ADR-0021: Borrow checking without lifetime annotations

- **Status:** accepted
- **Date:** 2026-10-28
- **Plan reference:** changes P01 D1.4 rule 8 ("no borrow checker"); spec OWN-R8, OWN-V5, OWN-V7, new section 4.5 (OWN-B1 to OWN-B7), UB-1, UB-2, UB-2a to UB-2e, UB-5, UB-8, RNG-3, RNG-3a, CON-2, CLO-7, TYP-9, PAR-8 and chapter 16; extends ADR-0020 (the forward reference "ADR-0021 will add the escape rule"); new task P04.20; P04.09 note.

## Context

ADR-0003 chose "no borrow checker" (D1.4 rule 8): views and raw pointers could dangle, as in C++, and `-asan` found the rest. Since then the language got read-only views (ADR-0020), borrowed slices, arenas and never-nil pointers. Many "dangling" cases are easy to see in the code. For example:

```go
w := s[0:5]; s = "bye"; println(w)                    // 1: view of a string that was replaced
rename(&team, team.Name)                              // 2: callee writes t.Name; old points into it
for _, name := range names { names = nil; println(name) }   // 3: loop over a slice that was cleared
var arena mem.Arena; u := mem.NewIn[User](&arena); arena.Reset(); println(u.Name)   // 5 (illustrative names, P05.03): value from an arena after Reset
```

On 2026-10-28 the owner decided that these must be compile errors, but without Rust-style lifetime annotations. Data races stay the programmer's job (mutex or channels, plus `-race` tests): UB-3 does not change. (Update 2026-10-28: ADR-0022 later made data races compile errors in normal code and narrowed UB-3.)

## Decision

1. **A borrow-checking pass with no annotations.** It finds borrows from the code, tracks them with flow and liveness (like Rust's non-lexical lifetimes), and reports a change of the borrowed thing while the borrow is still used later. The rules are spec 4.5, OWN-B1 to OWN-B7.
2. **What is a borrow (OWN-B1):** a view; a borrowed `[]T` from slicing; a pointer made by `&x` of a local variable (freed at the end of its scope, or moved), or by `&` of a field or element of storage that can move or be freed; a range loop over a collection (borrows it for the whole loop); a value allocated from an arena (borrows the arena); a `@View` struct value; an interface value holding a boxed view.
3. **The rule (OWN-B2):** while a borrow is live, the borrowed thing may not be assigned (or a prefix of it), appended to, moved, cleared, have map inserts or deletes on a borrowed map, have its arena reset or freed, or have an owning element or field written. Writing a pointer-free Copy value into an element or field is allowed. The check is conservative: when unsure, error. A use after the borrow's last use does not count.
4. **Across functions (OWN-B3):** a borrow result borrows from every borrow-capable argument and the receiver. A method returning a view borrows from its receiver. The compiler may be more conservative than the real code.
5. **The same thing passed twice (OWN-B4):** a writable argument plus a borrow of the same or an overlapping place at one call is an error. Hidden aliases are not seen.
6. **Escapes (OWN-B5):** a borrow stored in a global, a heap container, a channel, an escaping closure, a `go` statement, or an escaping interface box is an error. This replaces the ADR-0020 forward reference.
7. **What stays undefined (chapter 16):** `unsafe` and raw memory (UB-7), C and C++ interop (UB-9, UB-10), aliasing the compiler cannot see (UB-2, narrowed; also UB-2b and UB-2c), data races (UB-3), `longjmp` and forced unwinding (UB-4, UB-12). UB-1, UB-2a, UB-2d, UB-2e, UB-5 and UB-8 become compile errors, with only the unsafe, C and hidden-alias residue left.
8. **Fixes the owner's examples need:**
   - 1: `w := s[0:5].Clone()`, or use `w` before the assignment.
   - 2: `old := team.Name.Clone(); rename(&team, old)`.
   - 3: collect into a new slice and assign after the loop. Writing an element in the loop stays fine (a pointer-free Copy element always; an owning element unless a borrow of it is live).
   - 5: finish with `u` before `Reset`, or copy it out to the normal heap with an explicit copy-out function (illustrative `mem.CloneTo[User](u, mem.Heap)`; P05.03 sets the name). `u.Clone()` does not help: a clone stays in the arena (SLC-3).
9. **Where it runs:** new task P04.20, on the flow graph of P04.09. `gopp check` runs it too. P04.09 keeps use after move; its item "use through a pointer taken in the same function" moves into P04.20.
10. **Go mode** (`.go` files): the same checker with the same rules (see open point 3).
11. **New error codes** are named at implementation (no numbers here) and get `gopp explain` texts (P03.14).

## Consequences

- Most use-after-free bugs in safe code become compile errors with a fix shown in the message.
- Some correct programs are rejected (the check is conservative). The usual fix is `.Clone()` or ending the use earlier.
- Check time must stay linear in function size (P04.20 has a budget test).
- Residue stays: `-asan` and `-race` remain useful.
- No lifetime names in signatures. Results borrow from all borrow-capable arguments (elision), so some signatures reject more than they need to.

## Alternatives

- **Rust-style lifetime annotations.** Precise. Rejected: hard to learn, many false errors, a big change to the look of Go code.
- **Reference counting on every view.** Rejected: run-time cost on every view, and the language promises no GC-like hidden cost.
- **Keep it undefined behavior (ADR-0003).** Rejected by the owner.
- **Full aliasing XOR mutability (Rust's `&mut`).** Rejected: too strict for Go code, which writes elements through slices while other slices exist.

## Open points

1. SLC-6 (`s = s[i:j]`) is an assignment of `s`, so it is an error while a borrow of `s` is live and inside a range loop over `s` (RNG-3 already says so). No other change to SLC-6.
2. Decided (manager): the visible same-place case of UB-2b (`s.fail(s.err)`) is a compile error through OWN-B4. Only the hidden path stays UB.
3. Go mode: the plan does not say how `.go` files are ownership-checked. This ADR assumes the same ownership model and the same checker. It needs a deliberate decision before P04.20 is built (P04 README Q3): do `.go` files get the borrow checker (they have no GC in Go++ either)?
4. Rules OWN-V5 ("views may be stored at the programmer's risk") and OWN-V7 ("a view from a call stays UB") were reworded to point to OWN-B5 and OWN-B3. The old principle "No borrow checker (D1.4 rule 8)" is superseded; ADR-0003 keeps its text as history.
