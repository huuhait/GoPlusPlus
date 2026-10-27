# 07 vecsort

Fills a C++ `std::vector<int32_t>`, sorts it with `std::sort`, searches it and reverses it. Run: `go run main.go` (the Go version uses `slices.Sort`, `slices.BinarySearch` and `slices.Reverse`).

## Spec rules exercised

- [FOR-1, FOR-3, FOR-8](../../14-foreign-surface.md): `import "<vector>"` and `import "<algorithm>"`; the packages are `vector` and `algorithm`; `<vector>` has no extension, so it is C++; `std::` namespaces are flattened.
- [OWN-C2, OWN-R1](../../04-ownership.md): `vector.vector[int32]` is an owning C++ class; its destructor runs at the end of `main`.
- [OWN-R9](../../04-ownership.md): the vector has a default constructor, so `var v vector.vector[int32]` is legal.
- [SLC-9](../../08-slices-and-range.md): `out = out[:0]` resets the buffer and keeps its capacity.
- [RNG-6](../../08-slices-and-range.md): `for _, x := range v` over a C++ container works like a slice: each `int32` element is a trivial value, so `x` is a copy. (For a C++ class element it would be a read-only borrow, and `for _, &x := range v` would change the real element, like `auto&`.)
- [FOR-6](../../14-foreign-surface.md): `int32(19)` is an ordinary Go++ conversion; no C-specific types.

## Differences from the Go version

- Go sorts a slice; Go++ sorts a real `std::vector` with `std::sort`.
- Output is identical.

## Assumptions about other phases (P08)

- `for _, x := range v` yields the element value (`int32`) as for a slice (RNG-6). P08 D8.6 lowers `range` to `begin()`/`end()` iterator calls.
- `v.size()` returns `uint64` (`size_t` on Linux), hence `int(v.size())`.
- `algorithm.binary_search` returns a Go++ `bool`.
