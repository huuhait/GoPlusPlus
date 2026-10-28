# ADR-0014: `sync.Pool` without a GC

- **Status:** accepted
- **Date:** 2026-10-27
- **Plan reference:** P13 Q2 (task P13.13); P13 D13.6; P12 D12.3, D12.6 (`@Pinned` sync types).

## Context

Go's `sync.Pool` caches temporary objects (buffers, encoder states) so they can be reused. Go's GC empties pools over time (a victim cache across two GC cycles), so idle objects don't stay in memory. Go++ has no GC, and values have owners, so the pool needs its own cleanup rule and an ownership-aware API. Std ports (`fmt`, `encoding/json`) use `sync.Pool` internally, and servers reuse large buffers.

The options were: keep a pool with automatic cleanup, drop pools entirely (allocation is cheaper without a GC), or a pool with a fixed maximum size. The owner chose, on 2026-10-27, the pool with automatic cleanup plus an optional maximum.

## Decision

- **`sync.Pool[T]` is a generic type** with an optional `New func() T` and an optional `Max int`.
  - `p.Get() T` returns an owned value: a stored one, or else `New()`. If `New` is nil, `Get` returns the zero value; a `T` without a zero value (P01 D1.4 rule 9) needs `New`, or declaring the pool is a compile error.
  - `p.Put(x T)` takes ownership (`p.Put(move(b))`). A value that is never put back is just dropped at the end of its scope, so forgetting `Put` can't leak.
- **Automatic cleanup.** A runtime goroutine visits pools every few seconds (provisional: 2 s). Objects not taken since the previous visit are dropped (their `drop` runs on that goroutine), like Go's two-cycle victim cache. A busy pool keeps its objects; an idle one shrinks to empty.
- **Optional `Max`.** `Max: 0` (the default) means no cap. With `Max: n`, the pool stores at most about `n` objects (the cap is split across the per-P caches); a `Put` over the cap drops the object instead. Cleanup still applies.
- Like Go, the fast path uses per-P caches with no lock. Like the other `sync` types, a pool is `@Pinned`: moving or copying it after first use is a compile error. Dropping a pool drops every object it holds.

## Consequences

- Go code that uses `sync.Pool` ports with small changes: a type argument instead of `any`, no type assertion, and `move` on `Put`.
- Idle programs give memory back without any setting; programs that need a hard memory bound set `Max`.
- `drop` of pooled objects can run on the cleanup goroutine, at a time the program doesn't choose. Types with side effects in `drop` (closing a file, for example) should not be pooled; the docs say so.
- The runtime (P12) gets one more background goroutine, and P13's `sync` port implements the per-P caches and the cap.

## Alternatives

- **No pool.** Rejected: large buffers still cost an allocation and clearing per use, and users would each write their own pool.
- **A fixed-size pool only** (Zig's `MemoryPool` style). Rejected as the only form: `Max` must be guessed, and up to `Max` objects stay in memory while idle. It is kept as the optional `Max`.
- **A pool with no cleanup and no cap.** Rejected: memory stays at its peak forever.
