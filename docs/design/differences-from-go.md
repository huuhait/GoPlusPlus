# Where a program does not do what Go does

Go++ reads Go files and gives most programs the result that Go gives them. The places where it does not are decided, not found: each is a rule of the specification, and each is listed here with the run test that shows it (plan decision D4.10). A difference that is not in this table is a bug.

The table has the differences that the compiler implements today; it grows with the lowering.

| What | Go | Go++ | Rule | Test |
|---|---|---|---|---|
| `for i, v := range arr` over an array whose elements are not copied by their bytes (strings, slices, …) | walks a copy of the array: the loop does not see what its body writes | walks the array itself: a later element that the body wrote is read as written. An array of numbers is still copied first | [RNG-6](../spec/08-slices-and-range.md) | `tests/run/range_array.gopp` |
| A float that an integer type cannot hold, converted to it (`int8(300.0)` at run time, `int(NaN)`) | whatever the machine gives | the nearest value of the type; 0 for what is no number | [EXP-8](../spec/06-expressions.md) | `tests/run/convert.gopp` |
| A nil pointer that is followed | `panic: …` and a `[signal SIGSEGV …]` line | the same first line, no signal line: the pointer is checked before it is followed | [runtime.md](runtime.md) | `tests/run/panic_nil.go` |

Not different, though it may look so: the text of a stack trace is no part of what a program means ([UB chapter](../spec/16-undefined-behavior.md)), and the run tests compare everything before it.
