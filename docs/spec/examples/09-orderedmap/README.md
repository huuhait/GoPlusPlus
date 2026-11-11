# 09 orderedmap

A generic ordered map (sorted slices) used with `string` keys and `int` / `[]string` values. Run: `go run main.go`.

## Spec rules exercised

- [GEN-2, GEN-3, GEN-4, GEN-5](../../10-generics.md): generic `[K cmp.Ordered, V any]` code without a `Clone` bound on `OrderedMap` — `search` only reads `k`, so it is passed without a copy (`cmp.Ordered` lists only `OWN-C1`/`OWN-C2` types, which can all be copied), `Put(k K, v V)` hands `k` to `m.keys` on its last use and stores `v` with `move(v)` (`V any` may be a type that cannot be copied, so a plain `v` would not do), `Get` returns `Option[*V]` (instantiated only where called), and `Each[K cmp.Ordered, V Clone]` adds `[V Clone]` because it passes `m.vals[i]` to `f(k K, v V)` by value, which copies it.
- [PAR-1, PAR-5](../../05-parameters.md): pure Go parameter syntax (`Put(k K, v V)`, `counts.Put(w, 1)`).
- [OWN-R3a, OWN-R4](../../04-ownership.md): the two-place swap `m.keys[j], m.keys[j-1] = m.keys[j-1], m.keys[j]` moves elements cleanly without copying.
- [RNG-1](../../08-slices-and-range.md): `range words` gives `w` as a copy of each string; a string copy shares the bytes.
- [OWN-P1, OWN-P10, OWN-B3, OWN-B4](../../04-ownership.md#45-borrow-checking-adr-0021): `Get` returns `Option[*V]` borrowing `m` (used on `counts` where `V = int`; on `colors` it would return `Option[*[]string]`, and `*[]string` is an ordinary pointer type, `OWN-P1`); after `p != nil`, `p` narrows to `*V` and ends before any subsequent `Put`.
- [CLO-1](../../11-closures.md): the callbacks passed to `Each` do not escape.
- [GEN-6 note](../../10-generics.md): no generic method here (methods of a generic type plus the generic function `Each`).

## Differences from the Go version

- `Get` returns `Option[*V]` instead of a nullable `*V` (`ADR-0018`: raw pointers in Go++ are never nil).
- `Each[K cmp.Ordered, V Clone](&m, f)` is a generic function with a `Clone` bound, because it copies `m.vals[i]` into the argument of `f`; this keeps `OrderedMap[K cmp.Ordered, V any]` usable with a single-owner `V` that cannot be copied (`GEN-2`, `GEN-3`).
- `Put(k K, v V)` and `counts.Put(w, 1)` are written identically to Go; inside `Put`, Go++ stores `move(v)` where Go stores `v`. Output is identical.
