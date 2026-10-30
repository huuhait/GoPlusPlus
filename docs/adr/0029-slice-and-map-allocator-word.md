# ADR-0029: An owned slice and a map header carry their allocator

- **Status:** accepted
- **Date:** 2026-10-30
- **Plan reference:** settles the conflict between P04 D4.5 (`[]T` is 3 words) and spec SLC-1 (4 words); P01 D1.22; task P04.25; P05 D5.3 and P05.03; related ADR-0026 (`string` stays 3 words) and ADR-0027 (mimalloc is the global allocator).

## Context

Go++ has arenas (`mem.Arena`) and user allocators (`mem.Allocator`) next to the global mimalloc allocator (ADR-0027). A slice or map that lives in an arena must grow and be freed by that arena, not by mimalloc.

Two documents disagreed:

- Spec SLC-1 said an owned `[]T` is 4 words: pointer, length, capacity, allocator.
- P04 D4.5 said `[]T` is `{ptr, len, cap}`, 3 words. P04.04 made the checker's `unsafe.Sizeof` follow D4.5, and P04.05 lowered slices as 3 words. `docs/design/gir.md` section 15 kept this as an open question.

On 2026-10-30 the owner decided: **option A, 4 words.**

## Decision

1. An owned `[]T` is **4 words**: `{ptr, len, cap, allocator}`. It is 32 bytes on a 64-bit target.
2. A map header also stores its allocator.
3. Borrowed slices and views stay small: a borrowed `[]T` and `View[string]`, `View[[]T]` are `{ptr, len}` (2 words); every other view is `{ptr}`.
4. `string` stays 3 words, `{ptr, len, cap}`, and always uses the global allocator (ADR-0026, SLC-5).
5. The allocator is chosen **once, when the slice or map is created**. `make`, literals, `append` on a nil slice and conversions use the global allocator (the nil allocator word, SLC-2). A function of package `mem` creates a slice or map in another allocator, for example `mem.MakeIn[T](&arena, len, cap)`. The exact names belong to P05.03.
6. `append`, `Clone` and drop use the slice's **own** allocator word. A clone stays in the same allocator (SLC-3).
7. Passing an owned slice to a borrowed parameter or making a view drops the allocator word. Reading and writing elements never needs it. A `move` `[]T` parameter passes all 4 words.

```text
A (chosen): the slice knows its allocator
  []T   ptr ──► elements
        len
        cap
        allocator  (nil = global mimalloc)         32 bytes

B (rejected): like Go
  []T   ptr, len, cap                              24 bytes
```

```go
// Normal code is the same as Go. The allocator word is nil (mimalloc).
xs := []int{1, 2, 3}
xs = append(xs, 4)

// An arena slice grows inside its arena, with the same type and the same code.
var arena mem.Arena
ys := mem.MakeIn[int](&arena, 0, 100)    // illustrative name (P05.03)
ys = append(ys, 1, 2, 3)                 // grows in the arena
zs := ys.Clone()                         // also lives in the arena
```

## Consequences

- Arenas and custom `mem.Allocator`s work with plain `[]T`, `append`, `Clone` and maps. There are no extra types and no extra rules.
- Normal code is unchanged from Go.
- The cost is 8 bytes per owned slice and one word in each map header. Moving a slice copies 4 words, not 3.
- `unsafe.Sizeof([]int{})` is 32 and `unsafe.Sizeof(View[[]int])` is 16 on both targets. `string` stays 24.
- The runtime's growth and free go through the allocator word. Until P05 has arenas, P04.25 proves this with a test-only second allocator.
- The C ABI still refuses slices and maps by value (P04.11). A slice passes to C as a pointer (FOR rules), which drops the word.
- Code that reads the layout of a slice (P04.04/P04.05 lowering, the runtime helpers, debug info, `unsafe.Slice`) changes in P04.25.

## Alternatives

- **B: 3 words, like Go. Rejected.** The slice would not know its allocator. Then `append` and drop on an arena or custom-allocator slice would use the wrong allocator (a crash or a leak). To avoid that we would need separate slice types for arenas, or extra rules (for example "never append to an arena slice"). Both break the promise that `mem.Arena` works with the ordinary types.
- **Prior art.**
  - Rust `Vec<T, A>` and C++ `std::vector<T, Alloc>` carry their allocator as part of the container.
  - Zig passes the allocator by hand at every call.
  - Go has a garbage collector only, so it needs no allocator word.
