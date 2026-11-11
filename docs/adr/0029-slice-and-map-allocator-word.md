# ADR-0029: Slice and map backing headers carry their allocator

- **Status:** accepted (integrated with [ADR-0032](0032-shared-buffers-and-borrow-engine.md))
- **Date:** 2026-10-30
- **Plan reference:** settles P04 D4.5 and spec SLC-1 (`[]T` is 4 words); P01 D1.22; task P04.25; P05 D5.3 and P05.03; related [ADR-0026](0026-no-small-string-optimization.md) (`string` stays 3 words) and [ADR-0027](0027-mimalloc-global-allocator.md) (mimalloc is the global allocator).

## Context

Go++ has arenas (`mem.Arena`) and user allocators (`mem.Allocator`) next to the global mimalloc allocator ([ADR-0027](0027-mimalloc-global-allocator.md)). A slice or map that lives in an arena must grow and be freed by that arena, not by mimalloc.

On 2026-10-30 the owner decided: **`[]T` is 4 words**, and slice/map headers preserve their allocator.

## Decision

1. Every `[]T` value is **4 words**: `{ptr, len, cap, backing}` (32 bytes on a 64-bit target). When heap- or arena-allocated, `backing` points to the shared backing header `{refcount, alloc_cap, allocator}`; for `nil`, empty slices, or borrowed stack-array slices `arr[i:j]`, `backing` is `nil` ([ADR-0032](0032-shared-buffers-and-borrow-engine.md), SLC-1).
2. Every `map[K]V` runtime header `{refcount, allocator, …}` also stores its allocator.
3. `string` is 3 words, `{ptr, len, backing}`, and always uses the global allocator ([ADR-0026](0026-no-small-string-optimization.md), SLC-5).
4. The allocator is chosen **once, when the slice or map is created**. `make`, literals, `append` on a nil slice and conversions use the global allocator (`allocator == nil`, SLC-2). Functions in package `mem` create a slice or map in another allocator, for example `mem.MakeIn[T](&arena, len, cap)` (names set in P05.03).
5. Because `allocator` lives in the `backing` header, **sub-slices `s[i:j]` and lent `[]T` parameters preserve the allocator**: `append`, `Clone` and drop all use the slice's own allocator (SLC-3). Only a slice with `backing == nil` (`nil`, empty, or `arr[i:j]` on a value array) has no backing allocator and allocates in the global allocator on `append` or `Clone`.

```text
A (chosen): 4-word slice with backing header carrying refcount + allocator
  []T   ptr     ──► elements
        len
        cap
        backing ──► { refcount, alloc_cap, allocator (nil = global mimalloc) }   32 bytes

B (rejected): 3 words with no backing/allocator link
  []T   ptr, len, cap                                                            24 bytes
```

```go
// Normal code is identical to Go. The allocator in backing is nil (mimalloc).
xs := []int{1, 2, 3}
xs = append(xs, 4)

// An arena slice grows inside its arena, even across sub-slices and calls.
var arena mem.Arena
ys := mem.MakeIn[int](&arena, 0, 100)    // illustrative name (P05.03)
ys = append(ys, 1, 2, 3)                 // grows in the arena
zs := ys.Clone()                         // also lives in the arena
```

## Consequences

- Arenas and custom `mem.Allocator`s work seamlessly with plain `[]T`, sub-slices, `append`, `Clone` and maps, with no extra container types.
- `unsafe.Sizeof([]int{})` is 32 and `unsafe.Sizeof("")` is 24 on 64-bit targets.
- The C ABI refuses slices and maps by value (P04.11); a slice argument passed to a C pointer parameter passes `ptr` directly (FOR rules).

## Alternatives

- **B: 3 words without backing/allocator link. Rejected.** Slices allocated in an arena or custom allocator would not know how to grow or free their storage without separate arena-only container types.
