# The runtime

Every Go++ program is linked with a runtime ([ADR-0035](../adr/0035-runtime-in-c23.md)): the start and the end of a program, the failure paths of the checks, and later allocation, buffers, type tables and the scheduler. Its lowest layer is GNU C23 in `runtime/`; everything that can be Go++ is in `std/`. This page says what is there today.

## Sources

| File | What it holds |
|---|---|
| `runtime/rt.c` | `_gopp_rt_init`, which the generated `main` calls first, `_gopp_rt_exit`, where every program that does not return from `main` ends, and `_gopp_rt_funcs`, which takes the names of the functions of a unit |
| `runtime/print.c` | `_gopp_print_<kind>`: the built-in `print` and `println` |
| `runtime/panic.c` | `_gopp_panic_<kind>`: where a failed bounds, slice, nil, division or shift check of generated code ends, and `_gopp_panic_must` |
| `runtime/os_linux.c`, `runtime/os_windows.c` | what the runtime asks of the system: writing to the standard error stream and walking the stack. A target gets the one of its system |
| `runtime/internal.h` | what these files share |
| `gopp_rt.h` | generated, never written by hand (below) |

The sources include no header of the C library. What they need of it they declare themselves (`exit`, `write`), so the runtime compiles for every target without a sysroot, like a unit of generated code; only a link needs the C library. The one header they include is `<unwind.h>` of the compiler. Every symbol of the runtime starts with `_gopp_`.

## The header: one table for both sides

Generated code and the runtime read the same values and call the same functions. Both come from tables in the compiler, so the two sides cannot drift apart:

- **Layouts** (`gopp_target::HEADERS`): `gopp_string` (`ptr`, `len`, `backing`), `gopp_slice` (`ptr`, `len`, `cap`, `backing`) and `gopp_iface` (`itab`, `data`). `TargetLayout::header` gives the size, the alignment and the offset of each field. GIR takes the fields of a string and a slice from the table, the emitter writes them from it, and a string in a unit is `gopp_string` itself.
- **Functions** (`gopp_codegen_c::runtime::FUNCS`): every `_gopp_` function that generated code may call, with its parameters as C declares them, and whether it returns. A unit calls such a function by its own name, so C checks every call against the one declaration; a module that names a `_gopp_` function that is not in the table is refused by the emitter.

`runtime::header(layout)` writes `gopp_rt.h` from the two tables: the names of the scalar types from Clang's own macros, each layout as a struct with a `_Static_assert` of its size, its alignment and the offset of every field, and a declaration of every function. The runtime includes that file. A unit of generated code starts with the same text, so the C compiler sees both users of every declaration, and a layout that C would lay out otherwise than `TargetLayout` says is a compile error in both.

## Building and keeping it

`runtime::build(driver, form, cache)` compiles the runtime with the Clang of the build and the flags of generated code (the mandatory flags, the target, the optimization level), with `-Wall -Wextra -Werror`. `gopp` carries the sources; nothing prebuilt is shipped.

| Form | What is made | For |
|---|---|---|
| `Form::Objects` | one object file for each source | an executable: `gopp build` gives them to the link |
| `Form::Shared` | `libgopp_rt.so` | the programs of the fast test runner ([test-runner.md](test-runner.md)), which is written for Linux; another object format is refused |

The objects stand in for a static archive: Clang and LLD are the only tools a build needs, and neither writes an archive.

A build is kept in `<cache>/runtime/<number>/`, where the cache is `GOPPCACHE`, or `~/.cache/gopp` (`%LOCALAPPDATA%\gopp` on Windows). The key of a build is the text of everything it is a function of: the form, the path and the version of Clang, the flags, the header and every source. The number is a hash of that text and only names the directory; the directory holds the whole key in the file `key`, and a build is found again only when that file is the key, byte for byte. A build is made in a directory of its own and then renamed, so a directory with the name is whole, and builds that start at the same time agree on one. The build cache of `P06` takes this over.

## `print` and `println`

The lowering works out every argument and then gives each to the function for its kind: `_gopp_print_bool`, `_gopp_print_int` (every signed integer, widened), `_gopp_print_uint`, `_gopp_print_float` (a `float32` widened), `_gopp_print_string`, `_gopp_print_pointer`, with `_gopp_print_space` between the arguments of `println` and `_gopp_print_newline` after them. A value of another kind is `E0439` for now.

The output goes to the standard error stream, at once and unbuffered, and is byte for byte what Go's runtime writes: a float has seven digits made by the steps of Go's `printfloat` (`+1.500000e+000`, `+Inf`, `NaN`; they are not always the nearest digits, and a Go program prints the same), a pointer is `0x` and hex digits, the least `int64` prints whole.

## A failure

A failed check writes what Go writes, then the trace, and the program ends with status 2:

```
panic: runtime error: index out of range [5] with length 3

goroutine 1 [running]:
main.at()
	+0x6f
main.main()
	+0x31a
```

- **The message.** The texts are those of Go's `runtime/error.go` and `runtime/panic.go`. A check of an index or of slice bounds passes its two numbers and whether the first is signed: a negative index reads `index out of range [-1]`, an unsigned one past the length reads with its length, as in Go. Go++ checks a nil pointer itself, so there is no `[signal SIGSEGV ...]` line.
- **`_gopp_panic_must(expr, position, message)`** writes `panic: must: EXPR at POS: MESSAGE` for a failed `!` in its panic form; the caller gives the message of the error.
- **The trace.** A unit with the start of the program carries a table of its functions: the address of each and its name as `gopp demangle` reads its symbol. The generated `main` gives the table to `_gopp_rt_funcs`. On a failure the runtime walks the stack with the unwinder of the system (`_Unwind_Backtrace` on ELF, `RtlCaptureStackBackTrace` on Windows, at most 62 frames there), takes the start of the function of each frame from the unwind tables, and prints the frames whose function is in a table: the name, and how far into the function the frame is. Frames of the runtime and of C are left out.
- **What a trace does not have yet:** the file and the line of a frame (they come with the debug information, `P04.12`); a function whose code Clang put into its caller, above `-O0`; the frames of other packages, until several units are linked (`P04.31`).
- `panic(v)` and `recover` come with unwinding (`P04.15`, `P05.09`), which needs the interface value; the printers of each kind are here.

## Tests

`crates/gopp-codegen-c/src/runtime/tests.rs`: 22 failures, each with the text of Go and status 2, at `-O0` and `-O2`; 28 printed values with what Go prints; a trace that names the functions of two tables, the innermost first, and none that no table has; the header as a snapshot; the header compiles for `linux/amd64` and `windows/amd64` and refuses a layout of another pointer width; the objects are made for both targets; sixteen builds at once make one directory, a kept build is not made again, other flags and targets are other builds, a directory with a wrong key makes way; a program that names every function of the table links with both forms, and one that fails a check exits with status 2. `gopp-target` checks the offsets of every header; the emitter tests link their programs with the runtime. `crates/gopp-cli/tests/cli.rs` has eight programs with what they write and how they end (`a_program_prints_and_fails_as_go_says`), and the long test `go_prints_and_fails_as_the_table_says` builds the same programs with Go and holds Go to the same table.
