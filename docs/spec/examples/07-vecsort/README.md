# 07 vecsort

Fills a C++ `std::vector<int32_t>`, sorts it with `std::sort`, searches it and reverses it. Run: `go run main.go` (the Go version uses `slices.Sort`, `slices.BinarySearch` and `slices.Reverse`).

## Spec rules exercised

- [FOR-1, FOR-3, FOR-8](../../14-foreign-surface.md): `import "<vector>"` and `import "<algorithm>"`; the packages are `vector` and `algorithm`; `<vector>` has no extension, so it is C++; `std::` namespaces are flattened.
- [OWN-C2, OWN-R1](../../04-ownership.md): `vector.vector[int32]` is an owning C++ class; its destructor runs at the end of `main`.
- [OWN-R9](../../04-ownership.md): the vector has a default constructor, so `var v vector.vector[int32]` is legal.
- [SLC-9](../../08-slices-and-range.md): `out = out[:0]` resets the buffer and keeps its capacity.
- [RNG-6](../../08-slices-and-range.md): `range` over a C++ container.
- [FOR-6](../../14-foreign-surface.md): `int32(19)` is an ordinary Go++ conversion; no C-specific types.

## Differences from the Go version

- Go sorts a slice; Go++ sorts a real `std::vector` with `std::sort`.
- Output is identical.

## Assumptions about other phases (P08)

- `for x := range v` yields the element value (`int32`), not a reference. P08 D8.6 says only that `range` works through `begin()`/`end()`.
- `v.size()` returns `uint64` (`size_t` on Linux), hence `int(v.size())`.
- `algorithm.binary_search` returns a Go++ `bool`.
