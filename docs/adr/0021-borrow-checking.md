# ADR-0021: Borrow checking without lifetime annotations

- **Status:** accepted (integrated with [ADR-0032](0032-shared-buffers-and-borrow-engine.md))
- **Status note (2026-10-29, [ADR-0023](0023-inheritance.md)):** an upcast pointer (`*Child` to `*Parent`, where `Parent` has a type table) is the same borrow, not a new one.
- **Date:** 2026-10-28
- **Status note (2026-11-11, [ADR-0041](0041-values-are-copied.md)):** the reference-counted buffers of `[]T` and `map[K]V` (Context), `arr[i:j]` and a slice of a single-owner slice as tracked borrows (decision 2), and the temporary retain of a slice or map argument and of a slice inside a `Shared[T]` (decision 6) are replaced: a slice or a map is a value with one owner, `s[i:j]` and `arr[i:j]` are copies, and the tracked borrow of part of a slice is the view `&s[i:j]`, a `*[]T`; flow-based borrow checking with no lifetime annotations and the other borrow rules stand, and they now also cover `*[]T` and `*map[K]V`.
- **Plan reference:** spec OWN-R8, section 4.5 (OWN-B1 to OWN-B9), UB-1, UB-2, UB-2a to UB-2e, UB-5, UB-8, RNG-3, RNG-3a, CON-2, CLO-7, TYP-9, PAR-6 and chapter 16; task P04.20.

## Context

Because Go++ frees memory deterministically without a garbage collector, raw pointers `*T` (`&x`), stack-array slices (`arr[i:j]`), active `range` loops, `mem.Arena` allocations, and `Mutex` guards can point into memory that would dangle if reallocated or destroyed while the borrow is still live:

```go
p := &nums[0]
nums = append(nums, 5)        // reallocates nums while p points to old element
println(*p)

var arena mem.Arena
u := mem.NewIn[User](&arena)
arena.Reset()                 // frees arena memory while u is still read below
println(u.Name)
```

Meanwhile, everyday values without `&` (`string`, `[]T`, `map[K]V`, `Shared[T]`) use reference-counted shared buffers ([ADR-0032](0032-shared-buffers-and-borrow-engine.md)) and need no borrow checking at call sites. On 2026-10-28 the owner decided that tracked borrows must be checked at compile time **with zero Rust-style lifetime annotations**.

## Decision

1. **Flow- and liveness-based borrow checking (`OWN-B1` to `OWN-B9`).** The compiler tracks borrows over the GIR CFG using backward liveness (like non-lexical lifetimes) and rejects any mutation or scope exit that invalidates a live borrow.
2. **What is a tracked borrow (`OWN-B1`):**
   - a raw pointer `*T` made by `&x` (of a local, parameter, struct field or slice/array element), an `&` range alias (`for _, &e := range xs`), or borrowed from a `Unique[T]` / `Shared[T]` place (`OWN-P6`);
   - a slice made by slicing a value array (`arr[i:j]`) or a single-owner `OWN-C3` slice (`OWN-S2`, allowed for non-keeping parameters and rejected on keeping or indirect calls);
   - a **borrow-carrying struct, array or `Option`** holding `*T` or a borrowed slice (`Parser{arena *mem.Arena}`, `Node{Left, Right Option[*Node]}`);
   - an active `range` loop over a slice, array, string or map (`RNG-3`);
   - a value allocated from a `mem.Arena` / `mem.DropArena` (borrows the arena; `mem.Arena` frees all chunks automatically at scope exit);
   - a `Mutex[T]` / `RWMutex[T]` guard (`Lock` / `RLock`, `CON-15`).
3. **No invalidation while live (`OWN-B2`).** While a tracked borrow is live (read on some later path), the borrowed storage may not be reassigned, moved, cleared, grown (`append` on the same slice variable `xs` while `&xs[i]` is live), inserted into or cleared (for maps), have its arena reset/freed (bump-allocating further nodes in `mem.Arena` adds chunks without moving existing chunks and does not invalidate live arena borrows), or go out of scope (`return &local`).
4. **Body-inferred borrow summaries (`OWN-B3`).**
   - Functions returning ordinary values (`string`, heap `[]T`, `User`, `Unique[T]`, `Shared[T]`) return self-contained handles/values and carry **no borrow summary**.
   - For functions returning a raw `*T`, `Option[*T]`, or a borrow-carrying struct/array, the compiler infers from the function body which pointer-capable parameters/receiver the returned borrow may point into (iterating mutually recursive functions to a fixpoint and storing the bitmask in export data). Calls with no visible body (interface methods, indirect `func` values, C/C++, extern/asm) conservatively borrow from all pointer-capable arguments.
   - **No lifetime syntax**, not even optional.
5. **Where tracked borrows may live (`OWN-B5`).** Raw `*T`, stack-array slices `arr[i:j]`, and borrow-carrying structs/arrays may live only in stack local variables, parameters, function results (`OWN-B3`), `Local[I]` interfaces, and allocations inside the same `mem.Arena` / `mem.DropArena`. Storing a tracked borrow in `NewUnique`, `NewShared`, a heap slice or map, a channel, a package global, an escaping closure, a kept `Send` interface, or passing `&local` to plain `go` is a compile error: heap-owned graphs use `Unique[T]`, `Shared[T]` or `Weak[T]`.
6. **Exclusive writable `*T` access (`OWN-B4`, `OWN-B9`).**
   - **One writable `*T` at a time:** while a writable `*T` borrow of a place is live, no other `*T` borrow or read of an overlapping place may be live (`swapNames(&list[i], &list[j])` with variable `i, j` is a compile error; use `list.Swap(i, j)`, constant indexes `&list[0], &list[1]`, or `list.Split(k)`).
   - **Safe `OWN-C2` argument lending alongside `&x`:** passing `&u` alongside an `OWN-C2` field read `u.SetName(u.Name)` is safe and allowed because the caller retains a temporary reference to `u.Name`'s buffer across the call ([ADR-0032](0032-shared-buffers-and-borrow-engine.md), `PAR-1`).
   - **Reborrowing:** `q := p` on a writable `*T` reborrows `p`; `p` cannot be used while `q` is live (`q := p; use(q); use(p)` is accepted).
   - **Pointer kinds in data:**
     | Kind | Meaning | Access |
     |---|---|---|
     | `Unique[T]` (`NewUnique`) | single heap owner (`OWN-C3`) | exclusive writable access |
     | `Shared[T]` (`NewShared`) | shared heap owner (`OWN-C2`) | writable in 1 goroutine (`OWN-C1`/`OWN-C2` fields; interior `&ptr.Slice[i]` and `range` over heap slices retain temporary `+1` backing refs); `Send + Sync` across goroutines when `T` is synchronized (`Mutex`, `RWMutex`, `Atomic`) |
     | `Weak[T]` (`NewWeak`) | non-owning back-pointer (`OWN-C2`) | `w.Lock()` upgrades to `Option[Shared[T]]` |
7. **Assignment-form `range` (`E0253`).** `for _, v = range xs` (using `=` instead of `:=`) over single-owner (`OWN-C3`) elements is a compile error `E0253` at the range clause; trivial (`OWN-C1`) and shared-handle (`OWN-C2`) elements are accepted.
8. **Package-variable borrows across unknown calls.** A call to an unknown callee (func value, interface method, body-less declaration) while a borrow of a package variable is live and used afterwards is a compile error when that package variable can still be written (either a `Sync` global without an active `Mutex` guard, or any global during `init`). Read-only post-`init` globals (`CON-18`) remain borrowable across unknown calls.
9. **What remains undefined ([chapter 16](../spec/16-undefined-behavior.md)):** `unsafe` and raw memory (`UB-7`), C and C++ raw pointers/threads (`UB-9`, `UB-10`), data races there (`UB-3`), and `longjmp` / forced unwinding (`UB-4`, `UB-12`).

## Consequences

- Dangling `&` pointers, slice reallocation during element borrow, arena use-after-reset, and overlapping `*T` aliasing bugs are caught at compile time in linear time per function, with zero lifetime annotations.
- Everyday value code (`string`, `[]T`, `map[K]V`, `Shared[T]`) stays as friction-free as Go.

## Alternatives

- **Rust-style lifetime annotations.** Rejected: viral signature noise and steep learning curve.
- **Leave dangling `*T` pointers undefined (`ADR-0003`'s initial position).** Rejected by the owner in favor of compile-time safety.
