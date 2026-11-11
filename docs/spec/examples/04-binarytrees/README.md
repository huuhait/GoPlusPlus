# 04 binarytrees

Allocation-heavy benchmark (from the Benchmarks Game), with small sizes so the output is fast and fixed. Run: `go run main.go`.

## Spec rules exercised

- [OWN-P2, OWN-P5](../../04-ownership.md): `NewUnique(Node{...})` moves the value in; `Unique[Node]` has the methods of `*Node`, so `n.Left.check()` works.
- [OWN-C3, OWN-P8, OWN-R1, OWN-R2](../../04-ownership.md): `Unique[Node]` is single-owner (`OWN-C3`); the block that holds `stretch` ends, and the whole tree is freed. `long` is dropped when `main` ends.
- [OWN-R3](../../04-ownership.md), [EXP-10](../../06-expressions.md): `Node{Left: bottomUp(...), ...}` moves the temporaries into the literal.
- [OWN-P9, OWN-P10, OWN-P6](../../04-ownership.md): pointers are never nil, so a leaf's links are `Option[Unique[Node]]` (`nil` for no child). `check` borrows them as `Option[*Node]`, and after `if l == nil || r == nil { return 1 }` both are `*Node` (ADR-0018).
- [SLC-3](../../08-slices-and-range.md), [OWN-B2](../../04-ownership.md#45-borrow-checking-adr-0021), [UB-5](../../16-undefined-behavior.md): the arena variant; `mem.Arena` frees its chunks automatically on drop (`SLC-3`), while `arena.Reset()` reuses chunks across iterations and ends borrows of older nodes (checked in safe code by `OWN-B2`; `UB-5` if bypassed via `unsafe`).
- [PAN-3](../../09-errors-and-panics.md): `defer arena.Free()` evaluates `&arena` at the `defer`, and `arena` lives to the end of `main`; later `arena.Reset()` calls are legal and seen by the deferred call.
- [OWN-C1, OWN-B1, OWN-B5](../../04-ownership.md): `ANode` holds only `Option[*ANode]` (bitwise-copyable and borrow-carrying tied to `arena`), which `mem.ArenaNew[ANode]` permits without per-node destructors.
- [INI-2, INI-6](../../15-program-init.md): at the end of `main`, `main`'s deferred `Free` runs first, then `main`'s locals (`arena`, `long`) drop in reverse declaration order before any background goroutines are stopped.

## Differences from the Go version

- `main.go` uses `&Node{}` and the GC for everything. Go++ rejects `&Node{}`.
- `main.go` uses nil pointers for missing children; Go++ uses `Option` links, because its pointers are never nil.
- The Go version has no arena variant; it uses `bottomUp` in the loop.
- Output is identical.

## Assumptions about other phases

- `mem.Arena` has a usable zero value, and `Reset`/`Free` have pointer receivers (P05 D5.3 gives the names; P05.03 sets the API).
- `mem.ArenaNew` is called as `mem.ArenaNew[ANode](a)` (D5.3 signature).
- The import path of the package is written `"mem"`; P05 may place it elsewhere.
