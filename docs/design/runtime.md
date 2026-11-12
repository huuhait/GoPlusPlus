# The runtime

Every Go++ program is linked with a runtime ([ADR-0035](../adr/0035-runtime-in-c23.md)): the start and the end of a program, the failure paths of the checks, and later allocation, buffers, type tables and the scheduler. Its lowest layer is GNU C23 in `runtime/`; everything that can be Go++ is in `std/`. This page says what is there today.

## Sources

| File | What it holds |
|---|---|
| `runtime/rt.c` | `_gopp_rt_init`, which the generated `main` calls first, and `_gopp_rt_exit`, where every program that does not return from `main` ends |
| `runtime/panic.c` | `_gopp_panic_<kind>`: where a failed bounds, slice, nil, division or shift check of generated code ends. The program stops with status 2; the message and the trace of Go come with `P05.02` |
| `gopp_rt.h` | generated, never written by hand (below) |

The sources include no header of the C library. What they need of it they declare themselves (`exit`), so the runtime compiles for every target without a sysroot, like a unit of generated code; only a link needs the C library. Every symbol of the runtime starts with `_gopp_`.

## The header: one table for both sides

Generated code and the runtime read the same values and call the same functions. Both come from tables in the compiler, so the two sides cannot drift apart:

- **Layouts** (`gopp_target::HEADERS`): `gopp_string` (`ptr`, `len`, `backing`), `gopp_slice` (`ptr`, `len`, `cap`, `backing`) and `gopp_iface` (`itab`, `data`). `TargetLayout::header` gives the size, the alignment and the offset of each field. GIR takes the fields of a string and a slice from the table, the emitter writes them from it, and a string in a unit is `gopp_string` itself.
- **Functions** (`gopp_codegen_c::runtime::FUNCS`): every `_gopp_` function with its parameters as C declares them, and whether it returns.

`runtime::header(layout)` writes `gopp_rt.h` from the two tables: the names of the scalar types from Clang's own macros, each layout as a struct with a `_Static_assert` of its size, its alignment and the offset of every field, and a declaration of every function. The runtime includes that file. A unit of generated code starts with the same text, so the C compiler sees both users of every declaration, and a layout that C would lay out otherwise than `TargetLayout` says is a compile error in both.

## Building and keeping it

`runtime::build(driver, form, cache)` compiles the runtime with the Clang of the build and the flags of generated code (the mandatory flags, the target, the optimization level), with `-Wall -Wextra -Werror`. `gopp` carries the sources; nothing prebuilt is shipped.

| Form | What is made | For |
|---|---|---|
| `Form::Objects` | one object file for each source | an executable: `gopp build` gives them to the link |
| `Form::Shared` | `libgopp_rt.so` | the programs of the fast test runner ([test-runner.md](test-runner.md)), which is written for Linux; another object format is refused |

The objects stand in for a static archive: Clang and LLD are the only tools a build needs, and neither writes an archive.

A build is kept in `<cache>/runtime/<number>/`, where the cache is `GOPPCACHE`, or `~/.cache/gopp` (`%LOCALAPPDATA%\gopp` on Windows). The key of a build is the text of everything it is a function of: the form, the path and the version of Clang, the flags, the header and every source. The number is a hash of that text and only names the directory; the directory holds the whole key in the file `key`, and a build is found again only when that file is the key, byte for byte. A build is made in a directory of its own and then renamed, so a directory with the name is whole, and builds that start at the same time agree on one. The build cache of `P06` takes this over.

## Tests

`crates/gopp-codegen-c/src/runtime/tests.rs`: the header as a snapshot; the header compiles for `linux/amd64` and `windows/amd64` and refuses a layout of another pointer width; the objects are made for both targets; sixteen builds at once make one directory, a kept build is not made again, other flags and targets are other builds, a directory with a wrong key makes way; a program that names every function of the table links with both forms, and one that fails a check exits with status 2. `gopp-target` checks the offsets of every header; the emitter tests link their programs with the runtime.
