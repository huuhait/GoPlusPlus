# 09 orderedmap

A generic ordered map (sorted slices) used with an owning key (`string`) and an owning value (`[]string`). Run: `go run main.go`.

## Spec rules exercised

- [GEN-3, GEN-4, GEN-5](../../10-generics.md): generic code with a possibly owning `T`: it only borrows (`search`, `Each`), moves (`Put`), or returns a pointer (`Get`). It never copies a `K` or `V`, so no `clonable` bound is needed.
- [PAR-1, PAR-5](../../05-parameters.md): `k K` in `search`/`Get` is borrowed; `k own K, v own V` in `Put` take ownership.
- [OWN-R3, OWN-R4](../../04-ownership.md): `m.keys = append(m.keys, k)` moves `k` and implicitly moves `m.keys`.
- [OWN-R3a](../../04-ownership.md): the swap `m.keys[j], m.keys[j-1] = m.keys[j-1], m.keys[j]` is legal and moves; `counts.Put(clone(w), 1)` needs `clone` because `w` is a borrowed range variable.
- [RNG-1](../../08-slices-and-range.md): `range words` borrows each string.
- [OWN-P1](../../04-ownership.md): `Get` returns a raw pointer `*V` into the container.
- [CLO-1](../../11-closures.md): the callbacks passed to `Each` do not escape.
- [GEN-6 note](../../10-generics.md): no generic method here (only methods of a generic type).

## Differences from the Go version

- `Put` takes `own K, own V`; the call sites write `clone(w)`. In Go everything is a copy of a string header.
- Output is identical.
