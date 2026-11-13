# 07 vecsort

Fills a C++ `std::vector<int32_t>`, sorts it with `std::sort`, searches it and reverses it.

## Spec rules exercised

- [FOR-1, FOR-3, FOR-8](../../14-foreign-surface.md): `import "<vector>"` and `import "<algorithm>"`; the packages are `vector` and `algorithm`; `<vector>` has no extension, so it is C++; `std::` namespaces are flattened.
- [OWN-R1](../../04-ownership.md): `main` owns the C++ `vector.vector[int32]`; its destructor runs at the end of `main`.
- [OWN-R9](../../04-ownership.md): the vector has a default constructor, so `var v vector.vector[int32]` is legal.
- [SLC-9](../../08-slices-and-range.md): `out = out[:0]` resets the buffer and keeps its capacity.
- [RNG-6](../../08-slices-and-range.md): `for _, x := range v` over a C++ container works like a slice: each `int32` element is a trivial value, so `x` is a copy. (For a non-trivial C++ class element, `for _, &x := range v` borrows each element in place, like `auto&`.)
- [FOR-6](../../14-foreign-surface.md): `int32(19)` is an ordinary Go++ conversion; no C-specific types.
