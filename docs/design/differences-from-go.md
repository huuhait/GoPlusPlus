# Where a program does not do what Go does

A program that is written as in Go mostly does in Go++ what it does in Go. The places where it does not are decided, not found: each is a rule of the specification, and each is listed here with the run test that shows it (plan decision D4.10). A difference that is not in this table is a bug.

The table has the differences that the compiler implements today; it grows with the lowering.

| What | Go | Go++ | Rule | Test |
|---|---|---|---|---|
| `for i, v := range arr` over an array whose elements are not copied by their bytes (strings, slices, …) | walks a copy of the array: the loop does not see what its body writes | walks the array itself: a later element that the body wrote is read as written. An array of numbers is still copied first | [RNG-6](../spec/08-slices-and-range.md) | `tests/run/range_array.gopp` |
| A float that an integer type cannot hold, converted to it (`int8(300.0)` at run time, `int(NaN)`) | whatever the machine gives | the nearest value of the type; 0 for what is no number | [EXP-8](../spec/06-expressions.md) | `tests/run/convert.gopp` |
| `t := s`, `f(s)`, `return s`, `x.field = s` for a slice `s` | `t` and `s` are two names for the same elements | `t` has its own elements: nothing done to one is seen through the other ([ADR-0041](../adr/0041-values-are-copied.md)). `s = append(s, v)` still grows `s` where it is | [OWN-R3](../spec/04-ownership.md), [SLC-4](../spec/08-slices-and-range.md) | `tests/run/slice_values.gopp`, `tests/run/deep_copies.gopp` (values that hold slices and maps) |
| `t := s[i:j]` of a slice or an array | the same elements | a new slice with copies of them; `s = s[i:j]` keeps the elements, and `&s[i:j]` names the same ones | [SLC-6](../spec/08-slices-and-range.md), [SLC-8](../spec/08-slices-and-range.md) | `tests/run/slice_values.gopp` |
| `s[:n]` with `len(s) < n <= cap(s)` | the slice grows into its room | out of range: `slice bounds out of range [:n] with length len(s)` | [SLC-13](../spec/08-slices-and-range.md) | `tests/run/panic_slice_length.gopp` |
| `copy(dst, src)` | writes the elements that `dst` shares with its other names | the destination is written `&dst` | [ADR-0041](../adr/0041-values-are-copied.md) rule 10 | `tests/run/slice_values.gopp` |
| `t := m`, `f(m)`, `return m`, `x.field = m` for a map `m` | `t` and `m` are two names for the same map | `t` has its own entries; a function changes the map of its caller through a `*map[K]V` | [OWN-R3](../spec/04-ownership.md), [ADR-0041](../adr/0041-values-are-copied.md) | `tests/run/map_values.gopp` |
| `for _, row := range grid` over elements that are slices or maps, or hold them | `row` is the element's own slice: `row[0] = x` changes `grid` | `row` is a copy: nothing done to it is seen in `grid`; `for _, &row := range grid` names the element (a plain `row` is only read, so no copy is made) | [RNG-1](../spec/08-slices-and-range.md) | `tests/run/deep_copies.gopp` |
| `m[k] = v` for a nil map `m` | `panic: assignment to entry in nil map` | the write makes the map where it is held | [OWN-P9](../spec/04-ownership.md), [EXP-7](../spec/06-expressions.md) | `tests/run/map_values.gopp` |
| A new key in a map, or `clear` of it, while a `range` loop walks it | allowed: the loop may or may not see the key | `panic: map insert during iteration`, `panic: map clear during iteration`, on every run. Writing a key that is there and `delete` are allowed | [RNG-4](../spec/08-slices-and-range.md), [RNG-5](../spec/08-slices-and-range.md) | `tests/run/panic_map_insert.gopp`, `tests/run/panic_map_clear.gopp` |

Not different, though it may look so:

- The text of a stack trace is no part of what a program means ([UB chapter](../spec/16-undefined-behavior.md)), and the run tests compare everything before it.
- The capacity of a slice that `append` grew is the one that Go's `growslice` gives (D5.7). Go's compiler starts a slice that does not leave its function in a 32-byte buffer on the stack, which gives other capacities there.
- The order in which the operands of one expression are read is left open by Go where a call is among them; Go++ reads them from left to right ([EXP-3](../spec/06-expressions.md)).
