# 04 binarytrees

Allocation-heavy benchmark (from the Benchmarks Game), with small sizes so the output is fast and fixed. Run: `go run main.go`.

## Spec rules exercised

- [OWN-P2, OWN-P5](../../04-ownership.md): `new_unique(Node{...})` moves the value in; `Unique[Node]` has the methods of `*Node`, so `n.Left.check()` works.
- [OWN-P8, OWN-R1, OWN-R2](../../04-ownership.md): `Unique[Node]` is owning; the block that holds `stretch` ends, and the whole tree is freed. `long` is dropped when `main` ends.
- [OWN-R3](../../04-ownership.md), [EXP-10](../../06-expressions.md): `Node{Left: bottomUp(...), ...}` moves the temporaries into the literal.
- [OWN-P9](../../04-ownership.md): `n.Left == nil` compares a `Unique` with `nil`; `n.Left.IsNil()` is the same test.
- [SLC-3](../../08-slices-and-range.md), [UB-5](../../16-undefined-behavior.md): the arena variant; `arena.Reset()` makes older nodes dangle, but none are used.
- [PAN-3](../../09-errors-and-panics.md): `defer arena.Free()` evaluates `&arena` at the `defer`, and `arena` lives to the end of `main`; later `arena.Reset()` calls are legal and seen by the deferred call.
- [OWN-C1](../../04-ownership.md): `ANode` has only raw pointers, so it is trivial, as `mem.ArenaNew[T Copy]` requires (P05 D5.3).
- [INI-6](../../15-program-init.md): at the end of `main` the deferred `Free` runs first, then `long` is dropped.

## Differences from the Go version

- `main.go` uses `&Node{}` and the GC for everything. Go++ rejects `&Node{}`.
- The Go version has no arena variant; it uses `bottomUp` in the loop.
- Output is identical.

## Assumptions about other phases

- `mem.Arena` has a usable zero value, and `Reset`/`Free` have pointer receivers (P05 D5.3 gives the names; P05.03 sets the API).
- `mem.ArenaNew` is called as `mem.ArenaNew[ANode](a)` (D5.3 signature).
- The import path of the package is written `"mem"`; P05 may place it elsewhere.
