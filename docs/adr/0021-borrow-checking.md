# ADR-0021: Borrow checking without lifetime annotations

- **Status:** accepted; amended 2026-10-28 (inferred borrow summaries replace elision; exclusive writable borrows close hidden aliasing; see Amendment)
- **Status note (2026-10-28, ADR-0023):** an upcast pointer (`*Child` to `*Parent`) is the same borrow, not a new one.
- **Status note (2026-10-29, ADR-0023):** an upcast exists only when the parent has a type table; see ADR-0023 decision 16.
- **Date:** 2026-10-28
- **Plan reference:** changes P01 D1.4 rule 8 ("no borrow checker"); spec OWN-R8, OWN-V5, OWN-V7, new section 4.5 (OWN-B1 to OWN-B9, B9 through the amendment), UB-1, UB-2, UB-2a to UB-2e, UB-5, UB-8, RNG-3, RNG-3a, CON-2, CLO-7, TYP-9, PAR-8 and chapter 16; extends ADR-0020 (the forward reference "ADR-0021 will add the escape rule"); new task P04.20; P04.09 note. Amendment: OWN-B3 (rewritten), OWN-B4, OWN-B6, new OWN-B9, OWN-C1, OWN-P*, PAR-6, PAR-7, CON-19, TYP-9, UB-1, UB-2, UB-2a, UB-2b, UB-2d, UB-2e, UB-8 and §16.4 / §16.5; P04.20, P03.11, P05.07, P12.13 and P14.01.

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

1. **A borrow-checking pass with no annotations.** It finds borrows from the code, tracks them with flow and liveness (like Rust's non-lexical lifetimes), and reports a change of the borrowed thing while the borrow is still used later. The rules are spec 4.5, OWN-B1 to OWN-B9 (B9 through the amendment).
2. **What is a borrow (OWN-B1):** a view; a borrowed `[]T` from slicing; a pointer made by `&x` of a local variable (freed at the end of its scope, or moved), or by `&` of a field or element of storage that can move or be freed; a range loop over a collection (borrows it for the whole loop); a value allocated from an arena (borrows the arena); a `@View` struct value; an interface value holding a boxed view.
3. **The rule (OWN-B2):** while a borrow is live, the borrowed thing may not be assigned (or a prefix of it), appended to, moved, cleared, have map inserts or deletes on a borrowed map, have its arena reset or freed, or have an owning element or field written. Writing a pointer-free Copy value into an element or field is allowed. The check is conservative: when unsure, error. A use after the borrow's last use does not count.
4. **Across functions (OWN-B3):** the result borrows only from the parameters and the receiver that the callee's body says it may borrow from (a per-function borrow summary, computed by the compiler; see Amendment A). Only where there is no body to read (an interface method, a func value of unknown origin, C/C++, extern or assembly) does the result borrow from every borrow-capable argument and the receiver. *(Was: "a borrow result borrows from every borrow-capable argument and the receiver (elision). A method returning a view borrows from its receiver. The compiler may be more conservative than the real code.")*
5. **The same thing passed twice (OWN-B4):** a writable argument plus a borrow of the same or an overlapping place at one call is an error. This is now one case of the general rule "one writable access at a time" (OWN-B9, Amendment B). *(Was: "Hidden aliases are not seen.")*
6. **Escapes (OWN-B5):** a borrow stored in a global, a heap container, a channel, an escaping closure, a `go` statement, or an escaping interface box is an error. This replaces the ADR-0020 forward reference.
7. **What stays undefined (chapter 16):** `unsafe` and raw memory (UB-7), C and C++ interop (UB-9, UB-10), data races (UB-3), `longjmp` and forced unwinding (UB-4, UB-12). UB-1, UB-2, UB-2a, UB-2b, UB-2d, UB-2e, UB-5 and UB-8 become compile errors, with only the unsafe and C/C++ residue left. After Amendment B there is no hidden-alias residue in normal code. *(Was: "aliasing the compiler cannot see (UB-2, narrowed; also UB-2b and UB-2c)" and "only the unsafe, C and hidden-alias residue left".)* UB-2c (a write to a borrowed `defer` argument) is a compile error in normal code too, by CLO-7 and OWN-B9.
8. **Fixes the owner's examples need:**
   - 1: `w := s[0:5].Clone()`, or use `w` before the assignment.
   - 2: `old := team.Name.Clone(); rename(&team, old)`.
   - 3: collect into a new slice and assign after the loop. Writing an element in the loop stays fine (a pointer-free Copy element always; an owning element unless a borrow of it is live).
   - 5: finish with `u` before `Reset`, or copy it out to the normal heap with an explicit copy-out function (illustrative `mem.CloneTo[User](u, mem.Heap)`; P05.03 sets the name). `u.Clone()` does not help: a clone stays in the arena (SLC-3).
9. **Where it runs (OWN-B1 to B9):** new task P04.20, on the flow graph of P04.09. `gopp check` runs it too. P04.09 keeps use after move; its item "use through a pointer taken in the same function" moves into P04.20.
10. **Go mode** (`.go` files): the same checker with the same rules (see open point 3).
11. **New error codes** are named at implementation (no numbers here) and get `gopp explain` texts (P03.14).

## Consequences

- Most use-after-free bugs in safe code become compile errors with a fix shown in the message.
- Some correct programs are rejected (the check is conservative). The usual fix is `.Clone()` or ending the use earlier.
- Check time must stay linear in function size (P04.20 has a budget test).
- Residue stays: `-asan` and `-race` remain useful.
- No lifetime names in signatures. The compiler reads the body to see what a result borrows from (Amendment A), so a signature rejects no more than its body needs. Only calls with no body to read (interface methods, func values of unknown origin, C/C++, extern or assembly) use the conservative "borrows from everything" rule. *(Was: "Results borrow from all borrow-capable arguments (elision), so some signatures reject more than they need to.")*

## Alternatives

- **Rust-style lifetime annotations.** Precise. Rejected: hard to learn, many false errors, a big change to the look of Go code.
- **Reference counting on every view.** Rejected: run-time cost on every view, and the language promises no GC-like hidden cost.
- **Keep it undefined behavior (ADR-0003).** Rejected by the owner.
- **Full aliasing XOR mutability (Rust's `&mut`).** Rejected at first: too strict for Go code, which writes elements through slices while other slices exist. Amendment B (2026-10-28) takes the part of it that closes hidden aliasing (one live writable borrow per place), and keeps the Go-style allowances listed there.

## Open points

1. SLC-6 (`s = s[i:j]`) is an assignment of `s`, so it is an error while a borrow of `s` is live and inside a range loop over `s` (RNG-3 already says so). No other change to SLC-6.
2. Decided (manager): the visible same-place case of UB-2b (`s.fail(s.err)`) is a compile error through OWN-B4. *(Was: "Only the hidden path stays UB." After Amendment B there is no hidden path in normal code.)*
3. Go mode: the plan does not say how `.go` files are ownership-checked. This ADR assumes the same ownership model and the same checker. **Deferred by owner 2026-10-28:** Go mode is not a current focus (Go++ plus C/C++ systems programming is), so P04 Q3 stays open and no new Go-mode work is planned; existing Go-mode rules and tests stay. The question was (P04 README Q3): do `.go` files get the borrow checker (they have no GC in Go++ either)?
4. Rules OWN-V5 ("views may be stored at the programmer's risk") and OWN-V7 ("a view from a call stays UB") were reworded to point to OWN-B5 and OWN-B3. The old principle "No borrow checker (D1.4 rule 8)" is superseded; ADR-0003 keeps its text as history.

## Amendment (2026-10-28)

Two owner decisions, taken the same day. They change Decision points 4, 5 and 7, the last Consequence and one Alternative above (the old wording is kept there as "was"). Spec: [04-ownership 4.5](../spec/04-ownership.md#45-borrow-checking-adr-0021) and chapter 16.

### Amendment A: lifetimes are inferred from the function body

This replaces the old default "results borrow from all borrow-capable arguments" (decision 4, called R3 or elision).

1. **Borrow summary.** For every function whose result is a borrow or holds one (a view, a borrowed slice, a pointer, a `@View` struct, a boxed view), the compiler computes from the body which parameters and receiver the result may borrow from. It is one bit per parameter, the union over all return paths.
   - Mutually recursive functions iterate to a fixpoint: start empty, repeat until nothing changes. This is linear in practice.
   - The summary is stored with the function and in export data (P03.11), so other packages use it.
2. **At a call**, the result borrows only from the arguments the summary names:

   ```go
   func first(a, b View[string]) View[string] { return a[0:1] }

   r := first(x, y)
   y = "new"   // OK: the summary says the result borrows only from a
   x = "new"   // error while r is live
   ```

3. **Conservative fallback.** "Borrows from all borrow-capable arguments and the receiver" applies only where there is no body to read:
   - calls through interface methods;
   - func values of unknown origin;
   - C and C++ functions;
   - extern or assembly functions.
4. **API stability.** If a new version of a package changes a function's borrow summary, the tooling reports it as an API change (P14.01, unchecked item: `gopp vet` and the API diff). The same summary mechanism also carries P04.09's "writes this parameter" bit. Both are per-function summaries in export data.
5. **No lifetime syntax**, not even optional.

### Amendment B: one writable access at a time, no hidden aliasing

Goal: close the hidden-aliasing hole. Example:

```go
func swapNames(a, b *Node) { old := a.Name; b.Name = "x"; println(old) }

swapNames(list[i], list[j])   // i may equal j: a and b may be the same Node
```

This is like Rust's `&mut` rule. The rule is spec OWN-B9.

1. **Exclusive writable borrow.** While a writable borrow of a place is live, no other borrow of that place or of an overlapping place may be live. Writable borrows are:
   - a `*T` made by `&` (used for writing);
   - a writable borrowed `[]T` from slicing;
   - a pointer-receiver call in progress;
   - a `Mutex` write guard.

   The other borrow may be another writable borrow or a view. This extends OWN-B2 and OWN-B4.
   - "Overlapping" is decided conservatively. Two places overlap unless the compiler can prove they are disjoint: different fields, different constant indexes, the two parts of a `Split`, different variables.
   - `list[i]` and `list[j]` with non-constant indexes overlap. Fixes: `list.Split(k)`, constant indexes, or the built-in safe `list.Swap(i, j)` (P12.13).
   - Kept from the first decision, because they do not make two live writable borrows of one place: writing a pointer-free Copy element while a *view* of other elements is live (`nums[0] = 5` while `v := nums[1:3]`), and writing elements inside a range loop. Decided by the owner: where these meet rule 1, the rule wins (see the Go-compat costs below).
2. **A writable pointer is not freely copyable.** `q := p` where `p` is a writable `*T` is a reborrow: `p` may not be used while `q` is live. It is non-lexical: `q := p; use(q); use(p)` is fine, because `q` is not used after `use(q)`. This matches Rust's reborrowing. The same applies to copying an interface value or a struct that holds a writable pointer. Read-only pointers and views copy freely.
3. **Raw `*T` values are always tracked borrows, and they live only where the checker can follow them** (owner decision 2026-10-28, option A). A raw `*T` can only be made by `&` (a borrow), from `Unique` / `Shared` (a borrow of the handle), or by `unsafe`. It is allowed only in parameters, results (by the OWN-B3 summary), local variables, and fields of `@View` structs. It is **not** allowed in normal struct fields, slice or array elements, map keys or values, channel elements, or globals (OWN-B5). So no raw pointer can hide in data, and that is why rule 1 needs no run-time check. Pointer fields use the pointer kinds, chosen by meaning:

   | Kind | Meaning | Use |
   |---|---|---|
   | `Unique[*T]` (preferred) | "I own it, only me" | children, next node |
   | `Shared[*T]` | "we own it together" | read-only, or with `mem.Cell` / `Mutex` to write |
   | `Weak[*T]` | "I point at it but don't own it" | back-pointers, caches, observers; always allowed, check `.Lock()` before use |

   A parent pointer is `Option[Weak[*Node]]`. The spec tree example (04-ownership 4.3) is rewritten this way.
4. **`Shared[*T]` is read-only, except its cell-typed parts,** like Rust's `Rc` / `Arc`, because two handles can name the same object. A plain write through a `Shared` handle (`parent.Name = "x"`) is a compile error. Parts of a cell type may be used through their own methods, which check at run time (like Rust's `Rc<Node>` with a `RefCell` field):
   - `mem.Cell[T]` for one goroutine, with a run-time borrow check that panics on overlapping access ("cell already borrowed"); it is not `Sync`;
   - `Mutex[T]` / `RWMutex[T]` / `Atomic[T]` for many goroutines (ADR-0022).

   A value taken from inside a `mem.Cell` is tied to a run-time cell guard; while it is live, a write to that cell panics. So freeing the last `Shared` through a cell while it is in use is a clear panic, not UB. A `View` stays strict: it never changes anything, not even through a cell (OWN-V1). The names are illustrative; P05.07 and P12.13 set the real ones. `Weak[*T]` upgrades to a read-only `Shared`. This simplifies ADR-0022 rule 8: CON-19 says "read-only except cell types", with or without goroutines.
5. **`Unique[*T]` gives exclusive access** by construction. A writable borrow through it is a writable borrow of the handle. Linked lists and trees use `Option[Unique[*Node]]`. Graphs with cycles use `Shared` plus `Weak` plus `mem.Cell`.
6. **Interface boxes that hold a pointer kind** (TYP-9: the binding is that pointer, its target writable): with rule 2, the binding is a reborrow of the pointer the box carries, and two interface copies carrying the same writable pointer follow rule 2.
7. **What remains undefined after A and B:** only `unsafe`, C and C++, data races in `unsafe` / C / C++, and `longjmp` / forced unwinding. Hidden aliasing (the old UB-2 residue) is gone in normal code.

### Go-compat costs of the amendment (decided, owner 2026-10-28)

- **Pointer fields must be rewritten.** Go code with `parent *Node`, `[]*Node` or `map[K]*V` fields does not compile. Use `Unique`, `Shared` or `Weak` (rule 3). Raw `*T` stays fine for parameters, results and locals.
- **`Shared` writes** (`s := new_shared(Config{}); s.Count++`) need `mem.Cell` or `Mutex` (rule 4).
- **Reborrow.** `q := p; use(p); use(q)` on a writable `*T` is an error; in Go it is legal and common (rule 2).
- **Aliased arguments.** `swapNames(list[i], list[j])` is rejected; use `Split`, constant indexes or `Swap` (rule 1).
- **Direct write versus a live writable slice.** `nums[0] = 5` with `v := nums[1:3]` is accepted (constant index outside the range, and a view is read-only). `nums[i] = 5` with a non-constant `i` while a writable borrowed `t := nums[1:3]` is live and used later is an error (overlap, rule 1).
- **`&` range variables.** `for i, &e := range xs { xs[i].f = 1; use(e) }` is an error: `e` is a writable alias of `xs[i]` (RNG-1a). Writing elements in a plain range loop stays fine.

### Open points of the amendment

1. **P03.21 (h).** Resolved with the P03.19 merge: P03.21 has item (h) (a binding of a value held by value in a box is a view; a pointer kind the box holds stays that pointer, a reborrow by OWN-B9 rule 7, its target writable as the pointer kind allows), and TYP-9 states both.
2. **RNG-4.** Check whether OWN-B9 makes the run-time "map insert during iteration" panic reachable only through `unsafe` or C; if so, narrow RNG-4 (unchecked item in P04.20).
