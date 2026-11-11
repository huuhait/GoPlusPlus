# 09 orderedmap

A generic ordered map (sorted slices) used with `string` keys and `int` / `[]string` values. Run: `go run main.go`.

## Spec rules exercised

- [GEN-2, GEN-3, GEN-4, GEN-5](../../10-generics.md): generic `[K cmp.Ordered, V any]` code without a `Clone` bound on `OrderedMap` — `search` lends `k` (`cmp.Ordered` lists only `OWN-C1`/`OWN-C2` types), `Put(k K, v V)` transfers `k` and `v` into `m.keys` / `m.vals` on their last uses (retaining if instantiated with `OWN-C2`, moving if instantiated with `OWN-C3`), `Get` returns `Option[*V]` (instantiated only where called), and `Each[K cmp.Ordered, V Clone]` adds `[V Clone]` and `m.vals[i].Clone()` to pass values to `f(k K, v V)` safely.
- [PAR-1, PAR-5](../../05-parameters.md): pure Go parameter syntax (`Put(k K, v V)`, `counts.Put(w, 1)`).
- [OWN-R3a, OWN-R4](../../04-ownership.md): the two-place swap `m.keys[j], m.keys[j-1] = m.keys[j-1], m.keys[j]` moves elements cleanly without cloning.
- [RNG-1](../../08-slices-and-range.md): `range words` lends each string.
- [OWN-P1, OWN-P10, OWN-B3, OWN-B4](../../04-ownership.md#45-borrow-checking-adr-0021): `Get` returns `Option[*V]` borrowing `m` (used on `counts` where `V = int`; `*[]string` is never instantiated on `colors`, `OWN-P1`); after `p != nil`, `p` narrows to `*V` and ends before any subsequent `Put`.
- [CLO-1](../../11-closures.md): the callbacks passed to `Each` do not escape.
- [GEN-6 note](../../10-generics.md): no generic method here (methods of a generic type plus the generic function `Each`).

## Differences from the Go version

- `Get` returns `Option[*V]` instead of a nullable `*V` (`ADR-0018`: raw pointers in Go++ are never nil).
- `Each[K cmp.Ordered, V Clone](&m, f)` is a generic function with a `Clone` bound calling `m.vals[i].Clone()`, keeping `OrderedMap[K cmp.Ordered, V any]` usable with non-cloneable single-owner `V` (`GEN-2`, `GEN-3`).
- `Put(k K, v V)` and `counts.Put(w, 1)` are written identically to Go. Output is identical.
